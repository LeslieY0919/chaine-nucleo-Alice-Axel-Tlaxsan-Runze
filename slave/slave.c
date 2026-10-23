#include "slave.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (24 - 8 * i));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16
         | (uint32_t)p[2] << 8 | p[3];
}

static void say(Slave *s, const char *format, ...)
{
    char text[200];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof text, format, args);
    va_end(args);
    s->io.notice(text);
}

static void hex(char *out, const uint8_t *data, unsigned length)
{
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < length; ++i) {
        out[2 * i] = digits[data[i] >> 4];
        out[2 * i + 1] = digits[data[i] & 15];
    }
    out[2 * length] = 0;
}

static void printable(char *out, size_t size, const uint8_t *data, uint8_t length)
{
    size_t used = 0;
    for (unsigned i = 0; i < length && used + 4 < size; ++i) {
        uint8_t c = data[i];
        if (c == '"' || c == '\\') {
            out[used++] = '\\';
            out[used++] = (char)c;
        } else if (c >= 32 && c != 127) {
            out[used++] = (char)c;
        } else {
            static const char digits[] = "0123456789abcdef";
            out[used++] = '\\';
            out[used++] = 'x';
            out[used++] = digits[c >> 4];
            out[used++] = digits[c & 15];
        }
    }
    out[used] = 0;
}

static void masked(uint8_t *out, const uint8_t *clear, uint8_t length, uint32_t key)
{
    for (unsigned i = 0; i < length; ++i)
        out[i] = clear[i] ^ (uint8_t)(key >> (24 - 8 * (i % 4)));
}

static uint32_t next_id(const Slave *s)
{
    return s->id < s->nodes ? s->id + 1 : SLAVE_PC_OUTPUT_ID;
}

bool slave_is_last(const Slave *s)
{
    return s->state != SLAVE_UNCONFIGURED && s->id == s->nodes;
}

static void clear_config(Slave *s)
{
    s->state = SLAVE_UNCONFIGURED;
    s->id = s->nodes = s->session = s->key_in = s->key_out = 0;
    s->reset_relayed = s->reset_requested = false;
}

static void send_raw(Slave *s, SlaveLink link, const uint8_t *frame)
{
    if (!s->io.send(link, frame))
        s->io.notice(link == LINK_UP ? "ERREUR : emission UART vers l'amont"
                                     : "ERREUR : emission vers l'aval");
}

static void send_message(Slave *s, SlaveLink link, const ProtocolMessage_t *m,
                         uint32_t key)
{
    uint8_t frame[PROTOCOL_FRAME_SIZE];
    if (!protocol_encode(m, key, frame)) {
        s->io.notice("ERREUR : trame non encodable");
        return;
    }
    send_raw(s, link, frame);
}

static void send_command(Slave *s, SlaveLink link, CommandType_t command,
                         uint32_t source, const void *payload, uint8_t length)
{
    ProtocolMessage_t m;
    if (!protocol_message(&m, command, source, payload, length)) {
        s->io.notice("ERREUR : trame non encodable");
        return;
    }
    send_message(s, link, &m, 0);
}

static void relay(Slave *s, SlaveLink to, const ProtocolMessage_t *m)
{
    ProtocolMessage_t copy = *m;
    copy.header.id_src = s->id;
    send_message(s, to, &copy, 0);
}

static void reject(Slave *s, uint32_t source, uint8_t code, const char *text)
{
    s->io.notice(text);
    if (source && source <= SLAVE_MAX_NODES)
        send_command(s, LINK_UP, CMD_ERROR, source, &code, 1);
}

static void ignore_neighbor(Slave *s, SlaveLink link, uint32_t source,
                            uint32_t expected)
{
    say(s, "IGNORE : trame %s de l'ID %" PRIu32 ", voisin attendu ID %" PRIu32,
        link == LINK_UP ? "amont" : "aval", source, expected);
}

void slave_init(Slave *s, SlaveIO io)
{
    memset(s, 0, sizeof *s);
    s->io = io;
    s->state = SLAVE_UNCONFIGURED;
}

uint32_t slave_rx_key(const Slave *s, SlaveLink link)
{
    uint32_t key = link == LINK_UP ? s->key_in : s->key_out;
    return key ? key : 1;
}

static void adopt(Slave *s, const uint8_t *p)
{
    clear_config(s);
    s->state = SLAVE_CONFIGURED;
    s->id = p[1];
    s->nodes = p[2];
    s->key_in = get32(p + 4);
    s->key_out = get32(p + 8);
    s->session = get32(p + 12);
    s->crc_error = false;
    s->messages = 0;
    say(s, "CONFIG : carte ID %" PRIu32 " sur %" PRIu32 " esclaves, role %s",
        s->id, s->nodes, slave_is_last(s) ? "DERNIERE (sortie PC en clair)" : "RELAIS");
    say(s, "CLES : amont %08" PRIx32 ", aval %08" PRIx32 ", session %08" PRIx32,
        s->key_in, s->key_out, s->session);
}

static void handle_init(Slave *s, const ProtocolMessage_t *m)
{
    const uint8_t *p = m->payload;
    uint32_t source = m->header.id_src;
    uint8_t phase = p[0], target = p[1], nodes = p[2];
    uint32_t key_in = get32(p + 4), key_out = get32(p + 8), session = get32(p + 12);
    bool configured = s->state != SLAVE_UNCONFIGURED;
    uint32_t error_id = configured ? s->id : source + 1;

    if (p[3] || !nodes || nodes > SLAVE_MAX_NODES || !session
        || phase < SLAVE_PHASE_CONFIG || phase > SLAVE_PHASE_COMMIT) {
        reject(s, error_id, ERROR_COMMAND, "ERREUR : INIT invalide");
        return;
    }
    bool fresh = !configured || session != s->session;

    if (phase == SLAVE_PHASE_CONFIG) {
        if (!target || target > nodes) {
            reject(s, error_id, ERROR_COMMAND, "ERREUR : INIT CONFIG, carte cible invalide");
            return;
        }
        if (fresh || target == s->id) {
            if (source != (uint32_t)target - 1) {
                ignore_neighbor(s, LINK_UP, source, (uint32_t)target - 1);
                return;
            }
            if (!key_in || (target == nodes) != (key_out == 0)) {
                reject(s, target, ERROR_COMMAND, "ERREUR : INIT CONFIG, cles invalides");
                return;
            }
            adopt(s, p);
            uint8_t ack[7] = {CMD_INIT, SLAVE_PHASE_CONFIG, target};
            put32(ack + 3, session);
            send_command(s, LINK_UP, CMD_ACK, s->id, ack, sizeof ack);
            return;
        }
        if (source != s->id - 1) {
            ignore_neighbor(s, LINK_UP, source, s->id - 1);
            return;
        }
        if (nodes != s->nodes || target < s->id || slave_is_last(s)) {
            reject(s, s->id, ERROR_STATE, "ERREUR : CONFIG incoherente avec cette session");
            return;
        }
        say(s, "CONFIG carte %u : relayee vers l'aval", target);
        relay(s, LINK_DOWN, m);
        return;
    }

    if (fresh) {
        reject(s, error_id, ERROR_STATE,
               "ERREUR : PROBE/COMMIT recu sans configuration de cette session");
        return;
    }
    if (source != s->id - 1) {
        ignore_neighbor(s, LINK_UP, source, s->id - 1);
        return;
    }
    if (target || key_in || key_out || nodes != s->nodes) {
        reject(s, s->id, ERROR_COMMAND, "ERREUR : PROBE/COMMIT invalide");
        return;
    }
    s->io.notice(phase == SLAVE_PHASE_PROBE
                 ? "PROBE : verification transmise vers l'aval"
                 : "COMMIT : synchronisation transmise vers l'aval");
    relay(s, LINK_DOWN, m);
}

static void handle_msg(Slave *s, const ProtocolMessage_t *m, uint32_t now)
{
    uint32_t source = m->header.id_src;
    uint8_t length = m->header.length;
    if (s->state == SLAVE_UNCONFIGURED) {
        reject(s, source + 1, ERROR_STATE, "ERREUR : MSG recu par une carte non configuree");
        return;
    }
    if (source != s->id - 1) {
        ignore_neighbor(s, LINK_UP, source, s->id - 1);
        return;
    }
    if (s->state != SLAVE_READY) {
        reject(s, s->id, ERROR_STATE, "ERREUR : MSG recu avant la synchronisation");
        return;
    }
    ++s->messages;
    s->last_activity = now;

    uint8_t cipher[MAX_PAYLOAD_SIZE] = {0};
    char in_hex[2 * MAX_PAYLOAD_SIZE + 1], text[4 * MAX_PAYLOAD_SIZE + 1];
    masked(cipher, m->payload, length, s->key_in);
    hex(in_hex, cipher, length);
    printable(text, sizeof text, m->payload, length);

    if (slave_is_last(s)) {
        uint8_t frame[PROTOCOL_FRAME_SIZE];
        say(s, "DECODE : recu %s -> clair \"%s\" (%u octets) -> PC sortie",
            in_hex, text, length);
        slave_encode_clear_msg(s->id, m->payload, length, frame);
        send_raw(s, LINK_DOWN, frame);
        return;
    }
    char out_hex[2 * MAX_PAYLOAD_SIZE + 1];
    masked(cipher, m->payload, length, s->key_out);
    hex(out_hex, cipher, length);
    say(s, "RELAIS : recu %s -> clair \"%s\" -> reemis %s", in_hex, text, out_hex);
    ProtocolMessage_t copy = *m;
    copy.header.id_src = s->id;
    send_message(s, LINK_DOWN, &copy, s->key_out);
}

static void from_up(Slave *s, const ProtocolMessage_t *m, uint32_t now)
{
    uint32_t source = m->header.id_src;
    const uint8_t *p = m->payload;
    bool configured = s->state != SLAVE_UNCONFIGURED;

    switch (m->header.command_type) {
    case CMD_INIT:
        handle_init(s, m);
        return;
    case CMD_MSG:
        handle_msg(s, m, now);
        return;
    default:
        break;
    }
    if (!configured) {
        s->io.notice("IGNORE : trame amont, carte non configuree");
        return;
    }
    if (source != s->id - 1) {
        ignore_neighbor(s, LINK_UP, source, s->id - 1);
        return;
    }
    switch (m->header.command_type) {
    case CMD_RESET:
        s->reset_relayed = true;
        s->io.notice("RESET : transmis vers l'aval, attente de la confirmation");
        relay(s, LINK_DOWN, m);
        return;
    case CMD_ACK:
        if (m->header.length == 7 && p[0] == CMD_RESET && get32(p + 3) == s->session) {
            if (s->reset_requested) {
                s->reset_requested = false;
                s->io.notice("RESET local : demande confirmee par le maitre");
            } else if (!slave_is_last(s)) {
                s->io.notice("ACK RESET du maitre : relaye vers l'aval");
                relay(s, LINK_DOWN, m);
            } else {
                s->io.notice("IGNORE : ACK RESET sans demande locale");
            }
            return;
        }
        s->io.notice("IGNORE : ACK inattendu venant de l'amont");
        return;
    case CMD_ERROR:
        say(s, "ERREUR signalee par l'amont : code %u", p[0]);
        if (p[0] == ERROR_CRC) s->crc_error = true;
        return;
    default:
        return;
    }
}

static void from_down(Slave *s, const ProtocolMessage_t *m)
{
    uint32_t source = m->header.id_src;
    const uint8_t *p = m->payload;
    if (s->state == SLAVE_UNCONFIGURED) {
        s->io.notice("IGNORE : trame aval, carte non configuree");
        return;
    }
    if (source != next_id(s)) {
        ignore_neighbor(s, LINK_DOWN, source, next_id(s));
        return;
    }
    switch (m->header.command_type) {
    case CMD_ACK: {
        if (m->header.length != 7) {
            s->io.notice("IGNORE : ACK local sans session");
            return;
        }
        if (get32(p + 3) != s->session) {
            s->io.notice("IGNORE : ACK d'une autre session");
            return;
        }
        bool config = p[0] == CMD_INIT && p[1] == SLAVE_PHASE_CONFIG;
        bool origin_ok = config ? p[2] > s->id && p[2] <= s->nodes : p[2] == 0;
        bool phase_ok = p[0] == CMD_INIT ? p[1] >= SLAVE_PHASE_CONFIG && p[1] <= SLAVE_PHASE_COMMIT
                                         : p[1] == SLAVE_PHASE_DATA;
        if (!origin_ok || !phase_ok) {
            s->io.notice("IGNORE : ACK avec phase ou origine invalide");
            return;
        }
        relay(s, LINK_UP, m);
        s->crc_error = false;
        if (config) {
            say(s, "ACK CONFIG de la carte %u : relaye vers le maitre", p[2]);
        } else if (p[0] == CMD_INIT && p[1] == SLAVE_PHASE_PROBE) {
            s->io.notice("ACK PROBE du PC sortie : relaye vers le maitre");
        } else if (p[0] == CMD_INIT) {
            s->state = SLAVE_READY;
            s->io.notice("PRET : COMMIT confirme par le PC sortie, carte operationnelle");
        } else if (p[0] == CMD_MSG) {
            s->io.notice("ACK MSG : livraison confirmee, relaye vers le maitre");
        } else {
            clear_config(s);
            s->io.notice("RESET confirme par le PC sortie : configuration effacee");
        }
        return;
    }
    case CMD_RESET:
        say(s, "RESET demande par l'ID %" PRIu32 " : relaye vers le maitre", source);
        relay(s, LINK_UP, m);
        return;
    case CMD_ERROR:
        say(s, "ERREUR code %u signalee par l'aval : relayee vers le maitre", p[0]);
        relay(s, LINK_UP, m);
        return;
    default:
        s->io.notice("IGNORE : commande inattendue venant de l'aval");
        return;
    }
}

void slave_receive(Slave *s, SlaveLink link, const ProtocolMessage_t *message,
                   uint32_t now)
{
    if (link == LINK_UP) from_up(s, message, now);
    else from_down(s, message);
}

void slave_bad_frame(Slave *s, SlaveLink link, ProtocolResult_t result)
{
    const char *side = link == LINK_UP ? "amont" : "aval";
    if (result != PROTOCOL_BAD_CRC) {
        say(s, "ERREUR : format de trame invalide (%s)", side);
        return;
    }
    if (!s->crc_error) say(s, "ERREUR CRC : trame %s rejetee", side);
    s->crc_error = true;
    if (s->state != SLAVE_UNCONFIGURED) {
        uint8_t code = ERROR_CRC;
        send_command(s, LINK_UP, CMD_ERROR, s->id, &code, 1);
    }
}

void slave_request_reset(Slave *s, uint32_t now)
{
    if (s->state == SLAVE_UNCONFIGURED) {
        s->io.notice("RESET local ignore : carte non configuree, utiliser /reset sur le maitre");
        return;
    }
    if (s->reset_requested) return;
    s->reset_requested = true;
    s->reset_requested_at = now;
    s->io.notice("RESET local : demande envoyee au maitre");
    send_command(s, LINK_UP, CMD_RESET, s->id, NULL, 0);
}

void slave_tick(Slave *s, uint32_t now, bool pressed)
{
    if (pressed != s->button_raw) {
        s->button_raw = pressed;
        s->button_changed = now;
    }
    if (s->presses && (uint32_t)(now - s->first_press) > 2000) s->presses = 0;
    if (s->button_stable != s->button_raw
        && (uint32_t)(now - s->button_changed) >= 30) {
        s->button_stable = s->button_raw;
        if (s->button_stable) {
            if (!s->presses) s->first_press = now;
            if (++s->presses == 3) {
                s->presses = 0;
                slave_request_reset(s, now);
            }
        }
    }
    if (s->reset_requested
        && (uint32_t)(now - s->reset_requested_at) >= SLAVE_RESET_WAIT_MS) {
        s->reset_requested = false;
        s->io.notice("RESET local : pas de confirmation du maitre");
    }
}

bool slave_led(const Slave *s, uint32_t now)
{
    if (s->crc_error) return (now / 100u) % 2u == 0;
    if (s->state == SLAVE_READY)
        return !s->messages || (uint32_t)(now - s->last_activity) >= 120u;
    uint32_t half_period = s->state == SLAVE_UNCONFIGURED ? 1000u : 250u;
    return (now / half_period) % 2u == 0;
}

const char *slave_state_name(const Slave *s)
{
    static const char *names[] = {"NON_CONFIGUREE", "CONFIGUREE", "PRET"};
    return names[s->state];
}

void slave_encode_clear_msg(uint32_t source, const uint8_t *text,
                            uint8_t length, uint8_t frame[PROTOCOL_FRAME_SIZE])
{
    if (length > MAX_PAYLOAD_SIZE) length = MAX_PAYLOAD_SIZE;
    memset(frame, 0, PROTOCOL_FRAME_SIZE);
    frame[0] = ICD_START_BYTE;
    frame[1] = CMD_MSG;
    frame[2] = length;
    put32(frame + 4, source);
    if (length) memcpy(frame + 8, text, length);
    uint16_t crc = protocol_crc16(frame, 24);
    frame[24] = (uint8_t)(crc >> 8);
    frame[25] = (uint8_t)crc;
}
