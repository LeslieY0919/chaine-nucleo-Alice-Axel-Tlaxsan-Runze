#ifndef MASTER_H
#define MASTER_H

#include "protocol.h"

#define MASTER_MAX_NODES 16u
#define MASTER_TIMEOUT_MS 2000u
#define MASTER_NO_COMMAND 0xffu

enum { PHASE_DATA = 0, PHASE_CONFIG = 1, PHASE_PROBE = 2, PHASE_COMMIT = 3 };
typedef enum { MASTER_WAIT, MASTER_CONFIG, MASTER_PROBE, MASTER_COMMIT,
               MASTER_READY, MASTER_RESETTING } MasterState;

typedef struct {
    bool (*send)(const uint8_t *frame);
    void (*notice)(const char *text);
    uint32_t (*random32)(void);
} MasterIO;

typedef struct {
    MasterIO io;
    MasterState state;
    uint32_t session, keys[MASTER_MAX_NODES + 1], sent_at;
    uint8_t nodes, target, pending, phase, ack_origin;
    bool crc_error;
    bool button_raw, button_stable;
    uint8_t presses;
    uint32_t button_changed, first_press;
} Master;

void master_init(Master *m, MasterIO io);
bool master_start(Master *m, uint8_t nodes, uint32_t now);
void master_reset(Master *m, uint32_t now);
bool master_text(Master *m, const uint8_t *text, uint8_t length, uint32_t now);
void master_receive(Master *m, const ProtocolMessage_t *message, uint32_t now);
void master_bad_crc(Master *m);
void master_link_error(Master *m);
void master_tick(Master *m, uint32_t now, bool pressed);
bool master_led(const Master *m, uint32_t now);
const char *master_state_name(const Master *m);

#endif
