#ifndef SLAVE_H
#define SLAVE_H

#include "protocol.h"

#define SLAVE_MAX_NODES 16u
#define SLAVE_RESET_WAIT_MS 2000u
#define SLAVE_PC_OUTPUT_ID 0u

enum { SLAVE_PHASE_DATA = 0, SLAVE_PHASE_CONFIG = 1, SLAVE_PHASE_PROBE = 2,
       SLAVE_PHASE_COMMIT = 3 };

typedef enum { SLAVE_UNCONFIGURED, SLAVE_CONFIGURED, SLAVE_READY } SlaveState;

typedef enum { LINK_UP = 0, LINK_DOWN = 1 } SlaveLink;

typedef struct {
    bool (*send)(SlaveLink link, const uint8_t *frame);
    void (*notice)(const char *text);
} SlaveIO;

typedef struct {
    SlaveIO io;
    SlaveState state;
    uint32_t id, nodes, session, key_in, key_out;
    bool crc_error;
    bool reset_relayed, reset_requested;
    uint32_t reset_requested_at;
    uint32_t messages, last_activity;
    bool button_raw, button_stable;
    uint8_t presses;
    uint32_t button_changed, first_press;
} Slave;

void slave_init(Slave *s, SlaveIO io);
uint32_t slave_rx_key(const Slave *s, SlaveLink link);
void slave_receive(Slave *s, SlaveLink link, const ProtocolMessage_t *message,
                   uint32_t now);
void slave_bad_frame(Slave *s, SlaveLink link, ProtocolResult_t result);
void slave_request_reset(Slave *s, uint32_t now);
void slave_tick(Slave *s, uint32_t now, bool pressed);
bool slave_led(const Slave *s, uint32_t now);
bool slave_is_last(const Slave *s);
const char *slave_state_name(const Slave *s);

void slave_encode_clear_msg(uint32_t source, const uint8_t *text,
                            uint8_t length, uint8_t frame[PROTOCOL_FRAME_SIZE]);

#endif
