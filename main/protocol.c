// ============================================================================
// Protocolo Bluetooth do controle — implementação (ver protocol.h)
// ============================================================================

#include "protocol.h"

uint8_t proto_checksum(uint8_t type, uint8_t len, const uint8_t *payload) {
    uint8_t c = (uint8_t)(type ^ len);
    for (uint8_t i = 0; i < len; i++) c ^= payload[i];
    return c;
}

size_t proto_build(uint8_t sync, uint8_t type,
                   const uint8_t *payload, uint8_t len,
                   uint8_t *buf) {
    buf[0] = sync;
    buf[1] = type;
    buf[2] = len;
    for (uint8_t i = 0; i < len; i++) buf[3 + i] = payload[i];
    buf[3 + len] = proto_checksum(type, len, payload);
    return (size_t)(4 + len);
}
