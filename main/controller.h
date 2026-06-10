#ifndef CONTROLLER_H
#define CONTROLLER_H

// ============================================================================
// Tipos compartilhados e contexto do controle.
//
// Regra do projeto: SEM variáveis globais. Todo o estado vive neste struct,
// que é passado como parâmetro (pvParameters) para todas as tasks. A
// comunicação entre tasks é feita por filas; o estado de leitura/escrita
// compartilhada é protegido por mutex_state.
// ============================================================================

#include <stdint.h>
#include <stdbool.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"

// Evento Controle -> PC (vai na q_events, consumido por bt_tx_task)
typedef struct {
    uint8_t type;   // MSG_GESTURE | MSG_BUTTON | MSG_BATTERY
    uint8_t a;      // gesture_id | button_id | percent
    uint8_t b;      // confianca  | edge
} ctrl_event_t;

// Evento de botão cru, vindo da ISR (vai na q_buttons, consumido por input_task)
typedef struct {
    uint8_t gpio;
    uint8_t edge;   // 1 = press (borda de descida), 0 = release (borda de subida)
} btn_event_t;

// Feedback PC -> Controle (vai na q_feedback, consumido por haptic_task)
typedef struct {
    uint8_t type;   // MSG_GAME_EVENT | MSG_HAPTIC | MSG_LED
    uint8_t a, b, c;
} fb_event_t;

// Contexto compartilhado do controle
typedef struct {
    // --- filas e semáforos ---
    QueueHandle_t     q_events;     // ctrl_event_t : imu/input/battery -> bt_tx
    QueueHandle_t     q_rx;         // uint8_t      : ISR UART -> bt_rx
    QueueHandle_t     q_buttons;    // btn_event_t  : ISR botões -> input
    QueueHandle_t     q_feedback;   // fb_event_t   : bt_rx/battery -> haptic
    SemaphoreHandle_t mutex_state;

    // --- estado protegido por mutex_state ---
    bool    connected;     // HC-06 pareado
    uint8_t battery_pct;   // 0..100
    char    pin[8];        // PIN do Bluetooth
} controller_t;

#endif // CONTROLLER_H
