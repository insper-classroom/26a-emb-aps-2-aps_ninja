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
// UP/DOWN/LEFT/RIGHT vêm da inclinação (Fusion AHRS, estilo APS 7);
// SPECIAL vem da IA (Edge Impulse, gesto "UPDOWN") e vira espaço no PC.
#define GEST_IDLE    0
#define GEST_UP      1
#define GEST_DOWN    2
#define GEST_LEFT    3
#define GEST_RIGHT   4
#define GEST_SPECIAL 5

// game event_id (PC -> Controle)
#define GE_DIED     1
#define GE_COIN     2
#define GE_POWERUP  3

#define PROTO_MAX_PAYLOAD 8
#define PROTO_MAX_FRAME   (4 + PROTO_MAX_PAYLOAD)

// Implementações em protocol.c (header só com protótipos)
uint8_t proto_checksum(uint8_t type, uint8_t len, const uint8_t *payload);

// Monta um quadro completo em buf (>= PROTO_MAX_FRAME). Retorna o nº de bytes.
size_t proto_build(uint8_t sync, uint8_t type,
                   const uint8_t *payload, uint8_t len,
                   uint8_t *buf);

#endif // PROTOCOL_H
