#include "master.h"
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

static void fail(Master *m, const char *reason)
{
    m->state = MASTER_WAIT;
    m->pending = MASTER_NO_COMMAND;
    m->io.notice(reason);
}

static bool send_message(Master *m, CommandType_t command,
                         const void *payload, uint8_t length)
{
    ProtocolMessage_t message;
    uint8_t frame[PROTOCOL_FRAME_SIZE];
    if (!protocol_message(&message, command, 0, payload, length)
        || !protocol_encode(&message, m->keys[0], frame)
        || !m->io.send(frame)) {
        fail(m, "ERREUR : emission UART");
        return false;
    }
    return true;
}

static void expect(Master *m, uint8_t command, uint8_t phase,
                   uint8_t origin, uint32_t now)
{
    m->pending = command;
    m->phase = phase;
    m->ack_origin = origin;
    m->sent_at = now;
}

static void send_init(Master *m, uint8_t phase, uint32_t now)
{
    uint8_t payload[16] = {phase, 0, m->nodes, 0};
    if (phase == PHASE_CONFIG) {
        payload[1] = m->target;
        put32(payload + 4, m->keys[m->target - 1]);
        put32(payload + 8, m->keys[m->target]);
    }
    put32(payload + 12, m->session);
    if (send_message(m, CMD_INIT, payload, sizeof payload))
        expect(m, CMD_INIT, phase, payload[1], now);
}

static void configure(Master *m, uint32_t now)
{
    uint32_t old_session = m->session;
    m->session = m->io.random32();
    if (!m->session || m->session == old_session)
        m->session = old_session == UINT32_MAX ? 1 : old_session + 1;
    memset(m->keys, 0, sizeof m->keys);
    for (unsigned i = 0; i < m->nodes; ++i) {
        uint32_t key = m->io.random32();
        if (!key) key = 1;
        for (unsigned j = 0; j < i;) {
            if (key == m->keys[j]) { if (++key == 0) key = 1; j = 0; }
            else ++j;
        }
        m->keys[i] = key;
    }
    m->crc_error = false;
    m->state = MASTER_CONFIG;
    m->target = 1;
    m->io.notice("INIT : configuration des cartes");
    send_init(m, PHASE_CONFIG, now);
}

void master_init(Master *m, MasterIO io)
{
    memset(m, 0, sizeof *m);
    m->io = io;
    m->nodes = 1;
    m->pending = MASTER_NO_COMMAND;
    m->state = MASTER_WAIT;
}

bool master_start(Master *m, uint8_t nodes, uint32_t now)
{
    if (!nodes || nodes > MASTER_MAX_NODES) return false;
    if (m->state != MASTER_WAIT) {
        m->io.notice("OCCUPE : utiliser /reset avant une nouvelle initialisation");
        return false;
    }
    m->nodes = nodes;
    if (m->session) master_reset(m, now);
    else configure(m, now);
    return true;
}

void master_reset(Master *m, uint32_t now)
{
    if (m->state == MASTER_RESETTING) return;
    m->state = MASTER_RESETTING;
    m->crc_error = false;
    m->io.notice("RESET : demande de reinitialisation et resynchronisation");
    if (send_message(m, CMD_RESET, NULL, 0))
        expect(m, CMD_RESET, PHASE_DATA, 0, now);
}

bool master_text(Master *m, const uint8_t *text, uint8_t length, uint32_t now)
{
    if (m->state != MASTER_READY || m->pending != MASTER_NO_COMMAND) {
        m->io.notice("REFUSE : chaine non synchronisee ou ACK en attente");
        return false;
    }
    if (!length || length > MAX_PAYLOAD_SIZE) return false;
    if (!send_message(m, CMD_MSG, text, length)) return false;
    expect(m, CMD_MSG, PHASE_DATA, 0, now);
    m->io.notice("MSG : envoye, attente de confirmation finale");
    return true;
}

void master_receive(Master *m, const ProtocolMessage_t *message, uint32_t now)
{
    if (!m->session || message->header.id_src != 1) {
        m->io.notice("IGNORE : voisin non configure ou identifiant inattendu");
        return;
    }
    uint8_t command = message->header.command_type;
    if (command == CMD_ERROR) {
        m->crc_error = message->payload[0] == ERROR_CRC;
        fail(m, "ERREUR signalee par la chaine : /reset requis");
        return;
    }
    if (command == CMD_RESET) {
        uint8_t ack[7] = {CMD_RESET, PHASE_DATA, 0};
        put32(ack + 3, m->session);
        if (send_message(m, CMD_ACK, ack, sizeof ack)) master_reset(m, now);
        return;
    }
    const uint8_t *p = message->payload;
    if (command != CMD_ACK || message->header.length != 7
        || m->pending == MASTER_NO_COMMAND || p[0] != m->pending
        || p[1] != m->phase || p[2] != m->ack_origin
        || get32(p + 3) != m->session) {
        m->io.notice("IGNORE : ACK inattendu, local ou ancienne session");
        return;
    }
    m->pending = MASTER_NO_COMMAND;
    m->crc_error = false;
    switch (m->state) {
    case MASTER_CONFIG:
        if (m->target < m->nodes) ++m->target;
        else m->state = MASTER_PROBE;
        send_init(m, m->state == MASTER_CONFIG ? PHASE_CONFIG : PHASE_PROBE, now);
        break;
    case MASTER_PROBE:
        m->state = MASTER_COMMIT;
        send_init(m, PHASE_COMMIT, now);
        break;
    case MASTER_COMMIT:
        m->state = MASTER_READY;
        m->io.notice("PRET : chaine synchronisee");
        break;
    case MASTER_RESETTING:
        configure(m, now);
        break;
    case MASTER_READY:
        m->io.notice("ACK : message recu par le PC de sortie");
        break;
    default:
        break;
    }
}

void master_bad_crc(Master *m)
{
    if (!m->crc_error) m->io.notice("ERREUR CRC : trame rejetee");
    m->crc_error = true;
    if (m->session) {
        uint8_t error = ERROR_CRC;
        send_message(m, CMD_ERROR, &error, 1);
    }
}

void master_link_error(Master *m)
{
    fail(m, "ERREUR UART : reception perdue, /reset requis");
}

void master_tick(Master *m, uint32_t now, bool pressed)
{
    if (pressed != m->button_raw) {
        m->button_raw = pressed;
        m->button_changed = now;
    }
    if (m->presses && (uint32_t)(now - m->first_press) > 2000) m->presses = 0;
    if (m->button_stable != m->button_raw
        && (uint32_t)(now - m->button_changed) >= 30) {
        m->button_stable = m->button_raw;
        if (m->button_stable) {
            if (!m->presses) m->first_press = now;
            if (++m->presses == 3) {
                m->presses = 0;
                master_reset(m, now);
            }
        }
    }
    if (m->pending != MASTER_NO_COMMAND
        && (uint32_t)(now - m->sent_at) >= MASTER_TIMEOUT_MS) {
        if (m->state == MASTER_RESETTING) {
            m->io.notice("RESET sans ACK : tentative de reconfiguration");
            configure(m, now);
        } else {
            fail(m, "TIMEOUT : aucune confirmation, chaine non synchronisee");
        }
    }
}

bool master_led(const Master *m, uint32_t now)
{
    if (m->crc_error) return (now / 100u) % 2u == 0;
    if (m->state == MASTER_READY) return true;
    uint32_t half_period = m->state == MASTER_WAIT ? 1000u : 250u;
    return (now / half_period) % 2u == 0;
}

const char *master_state_name(const Master *m)
{
    static const char *names[] = {"ATTENTE", "CONFIGURATION", "VERIFICATION_PC_SORTIE",
                                 "SYNCHRONISATION", "PRET", "REINITIALISATION"};
    return names[m->state];
}
