#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include <semphr.h>

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "hardware/uart.h"

#include "hc06.h"
#include "pins.h"
#include "protocol.h"
#include "controller.h"
#include "imu_ei.h"
#include "ssd1306/ssd1306.h"

// ============================================================================
// Configuração
// ============================================================================
#define HC06_NAME          "SUBWAY-CTRL"
#define HC06_DEFAULT_PIN   "1234"
// O modulo SUBWAY-CTRL (00:22:09:01:6A:92) responde AT em 38400 mas troca os
// DADOS do SPP em 9600 (verificado: quadros chegam integros no PC a 9600).
#define HC06_DATA_BAUD     9600
// 1 = firmware de diagnostico: varre bauds transmitindo quadros identificados
// p/ descobrir o baud de dados de um modulo desconhecido. 0 = operacao normal.
// (usado p/ confirmar empiricamente os 9600 do modulo SUBWAY-CTRL)
#define HC06_BAUD_SWEEP    0

#define BTN_DEBOUNCE_MS    40

#define BATTERY_PERIOD_MS  2000
#define BATTERY_LOW_PCT    15

#define STATUS_FADE_STEP   8
#define STATUS_FADE_MS     20

// ============================================================================
// Único estado estático do programa: ponteiro de contexto para as ISRs.
// As ISRs do pico-sdk não recebem user-data, então precisam alcançar as filas
// por aqui. É o único uso de variável estática (não compartilha estado de
// aplicação — só dá às ISRs acesso às filas).
// ============================================================================
static controller_t *s_isr_ctx = NULL;

// ============================================================================
// ISRs
// ============================================================================
static void uart_rx_isr(void) {
    while (uart_is_readable(HC06_UART_ID)) {
        uint8_t ch = uart_getc(HC06_UART_ID);
        BaseType_t hpw = pdFALSE;
        xQueueSendFromISR(s_isr_ctx->q_rx, &ch, &hpw);
        portYIELD_FROM_ISR(hpw);
    }
}

static void gpio_btn_isr(uint gpio, uint32_t events) {
    btn_event_t e = {
        .gpio = (uint8_t)gpio,
        .edge = (events & GPIO_IRQ_EDGE_FALL) ? 1 : 0,  // pull-up: fall = press
    };
    BaseType_t hpw = pdFALSE;
    xQueueSendFromISR(s_isr_ctx->q_buttons, &e, &hpw);
    portYIELD_FROM_ISR(hpw);
}

// ============================================================================
// UART HC-06
// ============================================================================
static void init_uart_hc06(void) {
    uart_init(HC06_UART_ID, HC06_BAUD_RATE);
    gpio_set_function(HC06_TX_PIN, UART_FUNCSEL_NUM(HC06_UART_ID, HC06_TX_PIN));
    gpio_set_function(HC06_RX_PIN, UART_FUNCSEL_NUM(HC06_UART_ID, HC06_RX_PIN));
    uart_set_hw_flow(HC06_UART_ID, false, false);
    uart_set_format(HC06_UART_ID, 8, 1, UART_PARITY_NONE);
}

static void enable_uart_irq(bool on) {
    int UART_IRQ = (HC06_UART_ID == uart0) ? UART0_IRQ : UART1_IRQ;
    if (on) {
        uart_set_fifo_enabled(HC06_UART_ID, false);
        irq_set_exclusive_handler(UART_IRQ, uart_rx_isr);
        irq_set_enabled(UART_IRQ, true);
        uart_set_irq_enables(HC06_UART_ID, true, false);
    } else {
        uart_set_irq_enables(HC06_UART_ID, false, false);
        irq_set_enabled(UART_IRQ, false);
    }
}

// ============================================================================
// PWM helpers
// ============================================================================
static void pwm_init_pin(uint pin) {
    gpio_set_function(pin, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(pin);
    pwm_set_wrap(slice, 255);
    pwm_set_gpio_level(pin, 0);
    pwm_set_enabled(slice, true);
}

static inline void pwm_set_duty(uint pin, uint8_t duty) {
    pwm_set_gpio_level(pin, duty);
}

static inline void rgb_set(uint8_t r, uint8_t g, uint8_t b) {
    pwm_set_duty(LED_PIN_R, r);
    pwm_set_duty(LED_PIN_G, g);
    pwm_set_duty(LED_PIN_B, b);
}

// ============================================================================
// Botões: init com IRQ (todos no mesmo callback)
// ============================================================================
static void init_buttons(void) {
    const uint pins[] = {
        BTN_ACTION_PIN, BTN_AUX1_PIN, BTN_AUX2_PIN, BTN_AUX3_PIN,
    };
    for (size_t i = 0; i < count_of(pins); i++) {
        gpio_init(pins[i]);
        gpio_set_dir(pins[i], GPIO_IN);
        gpio_pull_up(pins[i]);
    }
    // O primeiro registra o callback; os demais só habilitam o mesmo bank IRQ.
    gpio_set_irq_enabled_with_callback(pins[0],
        GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, true, &gpio_btn_isr);
    for (size_t i = 1; i < count_of(pins); i++) {
        gpio_set_irq_enabled(pins[i], GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, true);
    }
}

// índice 0..3 da entrada (p/ debounce individual); 0xFF se não for botão
static uint8_t btn_index_from_gpio(uint8_t gpio) {
    switch (gpio) {
        case BTN_ACTION_PIN: return 0;
        case BTN_AUX1_PIN:   return 1;
        case BTN_AUX2_PIN:   return 2;
        case BTN_AUX3_PIN:   return 3;
        default:             return 0xFF;
    }
}

// ============================================================================
// Task: IMU + inferência (Edge Impulse)  —  em imu_ei.cpp (parte de IA).
// O SDK do Edge Impulse é C++, então a task vive num .cpp próprio e é
// declarada em imu_ei.h com linkage C.
// ============================================================================

// ============================================================================
// Task: entrada (botão de ação -> evento ao PC)
// O controle tem UM botão físico, que faz o papel de ESPAÇO no jogo (iniciar
// partida / ativar o poder). As 4 entradas digitais ficam configuradas com
// IRQ; qualquer uma pressionada dispara a mesma ação.
// ============================================================================
static void input_task(void *p) {
    controller_t *ctx = (controller_t *)p;

    TickType_t last_press[4] = {0, 0, 0, 0};   // debounce por entrada

    btn_event_t e;
    for (;;) {
        if (xQueueReceive(ctx->q_buttons, &e, portMAX_DELAY) != pdTRUE) continue;

        uint8_t idx = btn_index_from_gpio(e.gpio);
        if (idx == 0xFF) continue;

        // DEBUG (remover depois): observar o botão na serial USB
        printf("[BTN] gpio=%u edge=%s\n", e.gpio, e.edge ? "press" : "release");

        if (e.edge == 0) continue;   // só a borda de descida (press) interessa

        TickType_t now = xTaskGetTickCount();
        if ((now - last_press[idx]) < pdMS_TO_TICKS(BTN_DEBOUNCE_MS)) continue;
        last_press[idx] = now;

        ctrl_event_t ev = { MSG_BUTTON, BTN_ID_ACTION, 1 };
        xQueueSend(ctx->q_events, &ev, 0);
    }
}

// ============================================================================
// Task: TX Bluetooth (eventos -> protocolo -> UART)
// ============================================================================
static void bt_tx_task(void *p) {
    controller_t *ctx = (controller_t *)p;

    ctrl_event_t ev;
    uint8_t payload[PROTO_MAX_PAYLOAD];
    uint8_t frame[PROTO_MAX_FRAME];

    for (;;) {
        if (xQueueReceive(ctx->q_events, &ev, portMAX_DELAY) != pdTRUE) continue;

        uint8_t len = 0;
        switch (ev.type) {
            case MSG_GESTURE:
            case MSG_BUTTON:
                payload[0] = ev.a;
                payload[1] = ev.b;
                len = 2;
                break;
            case MSG_BATTERY:
                payload[0] = ev.a;
                len = 1;
                break;
            default:
                continue;
        }

        size_t n = proto_build(PROTO_SYNC_TX, ev.type, payload, len, frame);
        for (size_t i = 0; i < n; i++) {
            uart_putc_raw(HC06_UART_ID, frame[i]);
        }

        // DEBUG (remover depois): rastreia o despacho dos eventos nao-bateria
        if (ev.type != MSG_BATTERY)
            printf("[TX] type=0x%02X a=%u b=%u (%u bytes na UART)\n",
                   ev.type, ev.a, ev.b, (unsigned)n);
    }
}

// ============================================================================
// Task: RX Bluetooth (UART -> parser -> q_feedback)
// Máquina de estados que valida sync + checksum dos quadros PC -> Controle.
// ============================================================================
static void bt_rx_task(void *p) {
    controller_t *ctx = (controller_t *)p;

    enum { WAIT_SYNC, WAIT_TYPE, WAIT_LEN, WAIT_PAYLOAD, WAIT_CHK } st = WAIT_SYNC;
    uint8_t type = 0, len = 0, idx = 0;
    uint8_t payload[PROTO_MAX_PAYLOAD];
    uint8_t ch;

    for (;;) {
        if (xQueueReceive(ctx->q_rx, &ch, portMAX_DELAY) != pdTRUE) continue;

        switch (st) {
            case WAIT_SYNC:
                if (ch == PROTO_SYNC_RX) st = WAIT_TYPE;
                break;
            case WAIT_TYPE:
                type = ch;
                st = WAIT_LEN;
                break;
            case WAIT_LEN:
                len = ch;
                if (len > PROTO_MAX_PAYLOAD) { st = WAIT_SYNC; break; }
                idx = 0;
                st = (len == 0) ? WAIT_CHK : WAIT_PAYLOAD;
                break;
            case WAIT_PAYLOAD:
                payload[idx++] = ch;
                if (idx >= len) st = WAIT_CHK;
                break;
            case WAIT_CHK:
                if (ch == proto_checksum(type, len, payload)) {
                    fb_event_t fb = { type, 0, 0, 0 };
                    if (len > 0) fb.a = payload[0];
                    if (len > 1) fb.b = payload[1];
                    if (len > 2) fb.c = payload[2];
                    xQueueSend(ctx->q_feedback, &fb, 0);
                }
                st = WAIT_SYNC;
                break;
        }
    }
}

// ============================================================================
// Task: inicialização do HC-06.
// Roda SOB o scheduler (com a USB CDC já viva) pra que os prints de
// diagnóstico da config AT realmente apareçam na serial. Faz a config,
// liga a IRQ de RX e se autodestrói.
// ============================================================================
static void bt_init_task(void *p) {
    controller_t *ctx = (controller_t *)p;

    vTaskDelay(pdMS_TO_TICKS(2500));   // tempo p/ a USB CDC subir e você abrir o monitor

    if (!hc06_config(HC06_NAME, ctx->pin)) {
        printf("[AVISO] HC-06 nao configurado -- seguindo sem BT.\n");
    }

#if HC06_BAUD_SWEEP
    // DIAGNOSTICO (HC06_BAUD_SWEEP=1): descobre o baud de DADOS do modulo por
    // forca bruta. Varre os bauds em loop transmitindo quadros MSG_STATUS cujo
    // payload identifica o baud; o PC, conectado por SPP, ve QUAL indice chega
    // integro -> esse e o baud de dados do modulo. Nao liga a IRQ nem libera
    // o fluxo normal: e' so pra diagnostico.
    const unsigned sweep[] = {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200};
    uint8_t frame[PROTO_MAX_FRAME];
    for (;;) {
        for (uint8_t i = 0; i < count_of(sweep); i++) {
            uart_set_baudrate(HC06_UART_ID, sweep[i]);
            printf("[SWEEP] idx=%u baud=%u\n", i, sweep[i]);
            for (int n = 0; n < 10; n++) {
                uint8_t payload[1] = { i };
                size_t len = proto_build(PROTO_SYNC_TX, MSG_STATUS, payload, 1, frame);
                for (size_t k = 0; k < len; k++)
                    uart_putc_raw(HC06_UART_ID, frame[k]);
                vTaskDelay(pdMS_TO_TICKS(300));
            }
        }
    }
#else
    // A config roda no baud de AT; os dados SPP deste modulo usam 9600.
    uart_set_baudrate(HC06_UART_ID, HC06_DATA_BAUD);
    printf("UART de dados em %d baud.\n", HC06_DATA_BAUD);

    enable_uart_irq(true);             // só agora liga a IRQ de RX

    vTaskDelete(NULL);
#endif
}

// ============================================================================
// Task: háptico + feedback visual (motor de vibração + LED RGB)
// Consome a q_feedback (eventos do jogo / comandos do PC).
// ============================================================================
static void vibrate(uint8_t intensity, uint16_t ms) {
    pwm_set_duty(HAPTIC_PIN, intensity);
    vTaskDelay(pdMS_TO_TICKS(ms));
    pwm_set_duty(HAPTIC_PIN, 0);
}

static void haptic_task(void *p) {
    controller_t *ctx = (controller_t *)p;

    fb_event_t fb;
    for (;;) {
        if (xQueueReceive(ctx->q_feedback, &fb, portMAX_DELAY) != pdTRUE) continue;

        switch (fb.type) {
            case MSG_GAME_EVENT:
                if (fb.a == GE_DIED) {
                    rgb_set(255, 0, 0);
                    vibrate(255, 400);            // morreu: vibração forte
                    rgb_set(0, 0, 0);
                } else if (fb.a == GE_COIN) {
                    vibrate(120, 60);             // moeda: tapinha curto
                } else if (fb.a == GE_POWERUP) {
                    rgb_set(0, 0, 255);
                    vibrate(180, 150);
                    rgb_set(0, 0, 0);
                }
                break;
            case MSG_HAPTIC:
                vibrate(fb.a ? fb.a : 200, fb.b ? (uint16_t)fb.b * 10 : 150);
                break;
            case MSG_LED:
                rgb_set(fb.a, fb.b, fb.c);
                break;
        }
    }
}

// ============================================================================
// Task: bateria (ADC -> % -> evento + alerta)
// ============================================================================
// Abaixo desta tensão de bateria consideramos que NAO ha bateria ligada (ex.:
// alimentado só por USB, ADC flutuando) -> evita o alerta de "bateria baixa"
// floodando vermelho sem parar. Nenhuma Li-ion real opera abaixo disso.
#define BATTERY_PRESENT_V  2.5f

// Le a bateria. Retorna a % (0..100) e, por *present, se ha bateria de fato.
static uint8_t battery_read_pct(bool *present) {
    adc_select_input(BATTERY_ADC_CHAN);
    uint16_t raw = adc_read();                  // 0..4095 (12 bits)
    // Divisor /2: Vbat = 2 * Vadc. Faixa útil Li-ion ~3.3V (0%) .. 4.2V (100%).
    float vadc = (raw / 4095.0f) * 3.3f;
    float vbat = vadc * 2.0f;
    float pct  = (vbat - 3.3f) / (4.2f - 3.3f) * 100.0f;
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    if (present) *present = (vbat > BATTERY_PRESENT_V);
    return (uint8_t)pct;
}

static void battery_task(void *p) {
    controller_t *ctx = (controller_t *)p;

    for (;;) {
        bool present = false;
        uint8_t pct = battery_read_pct(&present);

        xSemaphoreTake(ctx->mutex_state, portMAX_DELAY);
        ctx->battery_pct = pct;
        xSemaphoreGive(ctx->mutex_state);

        ctrl_event_t ev = { MSG_BATTERY, pct, 0 };
        xQueueSend(ctx->q_events, &ev, 0);

        // Só alerta se houver bateria de verdade e ela estiver baixa.
        if (present && pct <= BATTERY_LOW_PCT) {
            fb_event_t fb = { MSG_LED, 255, 0, 0 };   // alerta: pisca vermelho
            xQueueSend(ctx->q_feedback, &fb, 0);
        }

        vTaskDelay(pdMS_TO_TICKS(BATTERY_PERIOD_MS));
    }
}

// ============================================================================
// Task: status (OLED + LED de conexão)
// ============================================================================
static void oled_draw(ssd1306_t *disp, const controller_t *ctx,
                      bool connected, uint8_t batt) {
    char line[32];
    ssd1306_clear(disp);
    ssd1306_draw_string(disp, 0, 0, 1, "SUBWAY CONTROL");
    snprintf(line, sizeof(line), "BT:%s", connected ? "ON" : "...");
    ssd1306_draw_string(disp, 0, 12, 1, line);
    snprintf(line, sizeof(line), "BAT:%u%% PIN:%s", batt, ctx->pin);
    ssd1306_draw_string(disp, 0, 24, 1, line);
    ssd1306_show(disp);
}

static void status_task(void *p) {
    controller_t *ctx = (controller_t *)p;

    // O OLED é tocado apenas por esta task (sem estado global).
    ssd1306_t disp;
    i2c_init(OLED_I2C_INST, 400000);
    gpio_set_function(OLED_I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(OLED_I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(OLED_I2C_SDA_PIN);
    gpio_pull_up(OLED_I2C_SCL_PIN);
    disp.external_vcc = false;
    ssd1306_init(&disp, OLED_WIDTH, OLED_HEIGHT, OLED_I2C_ADDR, OLED_I2C_INST);

    int  level = 0, step = STATUS_FADE_STEP;
    bool last_conn = false, first = true;
    uint8_t last_batt = 0xFF;

    for (;;) {
        bool connected = gpio_get(HC06_STATE_PIN);

        uint8_t batt;
        xSemaphoreTake(ctx->mutex_state, portMAX_DELAY);
        ctx->connected = connected;
        batt = ctx->battery_pct;
        xSemaphoreGive(ctx->mutex_state);

        // Redesenha o OLED só quando algo muda
        if (first || connected != last_conn || batt != last_batt) {
            oled_draw(&disp, ctx, connected, batt);
            last_conn = connected; last_batt = batt; first = false;
        }

        // LED de status: aceso fixo quando pareado, "respira" quando aguardando
        if (connected) {
            pwm_set_duty(LED_STATUS_PIN, 255);
            vTaskDelay(pdMS_TO_TICKS(150));
        } else {
            level += step;
            if (level >= 255) { level = 255; step = -STATUS_FADE_STEP; }
            if (level <= 0)   { level = 0;   step =  STATUS_FADE_STEP; }
            pwm_set_duty(LED_STATUS_PIN, (uint8_t)level);
            vTaskDelay(pdMS_TO_TICKS(STATUS_FADE_MS));
        }
    }
}

// ============================================================================
// main
// ============================================================================
int main(void) {
    stdio_init_all();
    sleep_ms(200);

    // --- Botões (4x digital, IRQ) ---
    init_buttons();

    // --- HC-06 STATE pin (entrada) ---
    gpio_init(HC06_STATE_PIN);
    gpio_set_dir(HC06_STATE_PIN, GPIO_IN);

    // --- PWM: RGB, status, háptico ---
    pwm_init_pin(LED_PIN_R);
    pwm_init_pin(LED_PIN_G);
    pwm_init_pin(LED_PIN_B);
    pwm_init_pin(LED_STATUS_PIN);
    pwm_init_pin(HAPTIC_PIN);

    // --- ADC: bateria ---
    adc_init();
    adc_gpio_init(BATTERY_ADC_PIN);

    // --- HC-06: UART + config inicial (antes do scheduler, sem IRQ) ---
    init_uart_hc06();

    // --- Contexto (sem variáveis globais) ---
    // Local de main(): como main() nunca retorna (vTaskStartScheduler),
    // a instância vive durante todo o programa. É passada às tasks por ponteiro.
    controller_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    strncpy(ctx.pin, HC06_DEFAULT_PIN, sizeof(ctx.pin) - 1);

    ctx.q_events    = xQueueCreate(64, sizeof(ctrl_event_t));
    ctx.q_rx        = xQueueCreate(256, sizeof(uint8_t));
    ctx.q_buttons   = xQueueCreate(32, sizeof(btn_event_t));
    ctx.q_feedback  = xQueueCreate(32, sizeof(fb_event_t));
    ctx.mutex_state = xSemaphoreCreateMutex();

    // Expõe o contexto às ISRs (a IRQ de RX é ligada pelo bt_init_task,
    // após a config AT do HC-06)
    s_isr_ctx = &ctx;

    // --- Tasks ---
    xTaskCreate(bt_init_task, "btinit",  1024, &ctx, 1, NULL);  // config HC-06
    xTaskCreate(imu_task,     "imu",     8192, &ctx, 3, NULL);  // [IA] DSP+NN precisam de stack
    xTaskCreate(input_task,   "input",   1024, &ctx, 2, NULL);
    xTaskCreate(bt_tx_task,   "bt_tx",   512,  &ctx, 2, NULL);
    xTaskCreate(bt_rx_task,   "bt_rx",   512,  &ctx, 2, NULL);
    xTaskCreate(haptic_task,  "haptic",  512,  &ctx, 1, NULL);
    xTaskCreate(battery_task, "battery", 512,  &ctx, 1, NULL);
    xTaskCreate(status_task,  "status",  1024, &ctx, 1, NULL);

    vTaskStartScheduler();
    for (;;);
}
