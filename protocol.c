#include "protocol.h"
#include <string.h>

static void write_u32(uint8_t *data, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        data[i] = (uint8_t)(value >> (24 - 8 * i));
}

static uint32_t read_u32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16)
         | ((uint32_t)data[2] << 8) | data[3];
}

static void mask_payload(uint8_t *payload, uint8_t length, uint32_t key)
{
    for (unsigned i = 0; i < length; ++i)
        payload[i] ^= (uint8_t)(key >> (24 - 8 * (i % 4)));
}

static bool valid_message(const ProtocolMessage_t *message)
{
    if (message->header.start_byte != ICD_START_BYTE
        || message->header.reserve != 0
        || message->header.length > MAX_PAYLOAD_SIZE)
        return false;

    switch (message->header.command_type) {
    case CMD_INIT:
        return message->header.length == 16;
    case CMD_RESET:
        return message->header.length == 0;
    case CMD_ACK:
        return (message->header.length == 1 || message->header.length == 7)
            && (message->payload[0] == CMD_INIT
                || message->payload[0] == CMD_RESET
                || message->payload[0] == CMD_MSG);
    case CMD_MSG:
        return true;
    case CMD_ERROR:
        return message->header.length == 1
            && message->payload[0] >= ERROR_CRC
            && message->payload[0] <= ERROR_NEIGHBOR;
    default:
        return false;
    }
}

uint16_t protocol_crc16(const uint8_t *data, size_t size)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < size; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    return crc;
}

bool protocol_message(ProtocolMessage_t *message, CommandType_t command,
                      uint32_t source, const void *payload, uint8_t length)
{
    if ((unsigned)command > CMD_ERROR || length > MAX_PAYLOAD_SIZE
        || (length && payload == NULL))
        return false;

    ProtocolMessage_t result = {0};
    result.header.start_byte = ICD_START_BYTE;
    result.header.command_type = (uint8_t)command;
    result.header.length = length;
    result.header.id_src = source;
    if (length)
        memcpy(result.payload, payload, length);
    if (!valid_message(&result))
        return false;
    *message = result;
    return true;
}

bool protocol_encode(const ProtocolMessage_t *message, uint32_t key,
                     uint8_t frame[PROTOCOL_FRAME_SIZE])
{
    if (!valid_message(message) || (message->header.command_type == CMD_MSG && !key))
        return false;

    memset(frame, 0, PROTOCOL_FRAME_SIZE);
    frame[0] = ICD_START_BYTE;
    frame[1] = message->header.command_type;
    frame[2] = message->header.length;
    write_u32(frame + 4, message->header.id_src);
    memcpy(frame + 8, message->payload, message->header.length);

    if (frame[1] == CMD_MSG)
        mask_payload(frame + 8, frame[2], key);
    uint16_t crc = protocol_crc16(frame, 24);
    frame[24] = (uint8_t)(crc >> 8);
    frame[25] = (uint8_t)crc;
    return true;
}

ProtocolResult_t protocol_decode(const uint8_t frame[PROTOCOL_FRAME_SIZE],
                                uint32_t key, ProtocolMessage_t *message)
{
    if (frame[0] != ICD_START_BYTE || frame[2] > MAX_PAYLOAD_SIZE
        || frame[3] != 0 || frame[1] > CMD_ERROR
        || (frame[1] == CMD_MSG && !key))
        return PROTOCOL_BAD_FORMAT;

    uint16_t received = (uint16_t)(((uint16_t)frame[24] << 8) | frame[25]);
    if (protocol_crc16(frame, 24) != received)
        return PROTOCOL_BAD_CRC;

    uint8_t clear[PROTOCOL_FRAME_SIZE];
    memcpy(clear, frame, sizeof clear);
    if (clear[1] == CMD_MSG)
        mask_payload(clear + 8, clear[2], key);

    for (unsigned i = clear[2]; i < MAX_PAYLOAD_SIZE; ++i)
        if (clear[8 + i] != 0)
            return PROTOCOL_BAD_FORMAT;

    ProtocolMessage_t result = {0};
    result.header.start_byte = clear[0];
    result.header.command_type = clear[1];
    result.header.length = clear[2];
    result.header.id_src = read_u32(clear + 4);
    memcpy(result.payload, clear + 8, MAX_PAYLOAD_SIZE);
    result.crc = received;
    if (!valid_message(&result))
        return PROTOCOL_BAD_FORMAT;
    *message = result;
    return PROTOCOL_OK;
}

ProtocolResult_t protocol_receive(ProtocolReceiver_t *receiver, uint8_t byte,
                                 uint32_t key, ProtocolMessage_t *message)
{
    if (!receiver->used && byte != ICD_START_BYTE)
        return PROTOCOL_WAIT;

    receiver->bytes[receiver->used++] = byte;
    if (receiver->used < PROTOCOL_FRAME_SIZE)
        return PROTOCOL_WAIT;

    ProtocolResult_t result = protocol_decode(receiver->bytes, key, message);
    receiver->used = 0;
    if (result != PROTOCOL_OK) {
        for (unsigned i = 1; i < PROTOCOL_FRAME_SIZE; ++i) {
            if (receiver->bytes[i] == ICD_START_BYTE) {
                receiver->used = (uint8_t)(PROTOCOL_FRAME_SIZE - i);
                memmove(receiver->bytes, receiver->bytes + i, receiver->used);
                break;
            }
        }
    }
    return result;
}
