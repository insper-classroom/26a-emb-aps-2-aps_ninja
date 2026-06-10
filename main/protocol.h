#ifndef PROTOCOL_H
#define PROTOCOL_H

// ============================================================================
// Protocolo Bluetooth do controle (HC-06)
//
//   Controle -> PC : 0xAA | TYPE | LEN | PAYLOAD[LEN] | CHK
//   PC -> Controle : 0x55 | TYPE | LEN | PAYLOAD[LEN] | CHK
//
//   CHK = XOR(TYPE, LEN, PAYLOAD...)
// ============================================================================

#include <stdint.h>
#include <stddef.h>

// Bytes de sincronismo (direção)
#define PROTO_SYNC_TX  0xAA   // Controle -> PC
#define PROTO_SYNC_RX  0x55   // PC -> Controle

// Mensagens Controle -> PC
#define MSG_GESTURE    0x01   // payload: gesture_id, confianca(%)
#define MSG_BUTTON     0x02   // payload: button_id, edge
#define MSG_BATTERY    0x03   // payload: percent
#define MSG_STATUS     0x04   // payload: flags

// Mensagens PC -> Controle
#define MSG_GAME_EVENT 0x10   // payload: event_id
#define MSG_HAPTIC     0x11   // payload: pattern_id
#define MSG_LED        0x12   // payload: r, g, b

// gesture_id
#define GEST_IDLE   0
#define GEST_UP     1
#define GEST_DOWN   2
#define GEST_LEFT   3
#define GEST_RIGHT  4

// game event_id (PC -> Controle)
#define GE_DIED     1
#define GE_COIN     2
#define GE_POWERUP  3

#define PROTO_MAX_PAYLOAD 8
#define PROTO_MAX_FRAME   (4 + PROTO_MAX_PAYLOAD)

static inline uint8_t proto_checksum(uint8_t type, uint8_t len, const uint8_t *payload) {
    uint8_t c = (uint8_t)(type ^ len);
    for (uint8_t i = 0; i < len; i++) c ^= payload[i];
    return c;
}

// Monta um quadro completo em buf (>= PROTO_MAX_FRAME). Retorna o nº de bytes.
static inline size_t proto_build(uint8_t sync, uint8_t type,
                                 const uint8_t *payload, uint8_t len,
                                 uint8_t *buf) {
    buf[0] = sync;
    buf[1] = type;
    buf[2] = len;
    for (uint8_t i = 0; i < len; i++) buf[3 + i] = payload[i];
    buf[3 + len] = proto_checksum(type, len, payload);
    return (size_t)(4 + len);
}

#endif // PROTOCOL_H
