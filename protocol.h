#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include "protocol_icd.h"

#define PROTOCOL_FRAME_SIZE 26u
#define PROTOCOL_ACK_TIMEOUT_MS 100u

typedef enum {
    PROTOCOL_WAIT = 0,
    PROTOCOL_OK = 1,
    PROTOCOL_BAD_FORMAT = -1,
    PROTOCOL_BAD_CRC = -2
} ProtocolResult_t;

typedef enum {
    ERROR_CRC = 1,
    ERROR_COMMAND = 2,
    ERROR_LENGTH = 3,
    ERROR_STATE = 4,
    ERROR_NEIGHBOR = 5
} ProtocolError_t;

typedef struct {
    uint8_t bytes[PROTOCOL_FRAME_SIZE];
    uint8_t used;
} ProtocolReceiver_t;

uint16_t protocol_crc16(const uint8_t *data, size_t size);

bool protocol_message(ProtocolMessage_t *message, CommandType_t command,
                      uint32_t source, const void *payload, uint8_t length);
bool protocol_encode(const ProtocolMessage_t *message, uint32_t key,
                     uint8_t frame[PROTOCOL_FRAME_SIZE]);
ProtocolResult_t protocol_decode(const uint8_t frame[PROTOCOL_FRAME_SIZE],
                                uint32_t key, ProtocolMessage_t *message);

ProtocolResult_t protocol_receive(ProtocolReceiver_t *receiver, uint8_t byte,
                                 uint32_t key, ProtocolMessage_t *message);

#endif
