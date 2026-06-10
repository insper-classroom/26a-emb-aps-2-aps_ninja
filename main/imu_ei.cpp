// ============================================================================
// Task: IMU (MPU6050) + inferência de gestos com Edge Impulse  —  [parte de IA]
//
// O modelo foi treinado no Edge Impulse Studio com janelas de 1 s de
// acelerômetro (3 eixos, 45 Hz, valores crus do MPU6050) e classifica:
// IDLE / UP / DOWN / LEFT / RIGHT. O pipeline (spectral analysis + rede
// quantizada int8) roda inteiro aqui na Pico — edge computing, sem PC.
//
// Fluxo: amostra a janela no MESMO ritmo do treino (EI_CLASSIFIER_INTERVAL_MS)
// -> run_classifier() -> mapeia o label vencedor p/ os IDs do protocolo ->
// publica ctrl_event_t{MSG_GESTURE} na q_events quando o gesto muda
// (bt_tx_task empacota e manda ao PC).
//
// A janela desliza com 50% de overlap: em vez de esperar 1 s inteiro entre
// decisões, mantém a metade mais recente e amostra só a outra metade
// (~0,5 s por decisão — importa num jogo de reflexo como o Subway Surfers).
// ============================================================================

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "pico/stdlib.h"
#include "hardware/i2c.h"

#include <math.h>
#include <string.h>
#include <strings.h>

#include "edge-impulse-sdk/classifier/ei_run_classifier.h"

extern "C" {
#include "pins.h"
#include "protocol.h"
#include "controller.h"
#include "mpu6050.h"
#include "imu_ei.h"
}

// Confiança mínima (%) p/ aceitar um gesto != idle; abaixo disso trata como
// idle (evita movimentos espúrios virarem comando no jogo).
#define EI_GESTURE_MIN_CONF 60

// ============================================================================
// MPU6050 (I2C0 — pinos em pins.h; mesmos do treino do modelo)
//
// Todas as transações usam TIMEOUT: numa protoboard um contato ruim no SDA/SCL
// faz o i2c_*_blocking travar PRA SEMPRE e a task morre em silêncio (o resto
// do firmware segue vivo). Com timeout, detectamos, reinicializamos e seguimos.
// ============================================================================
#define IMU_I2C_TIMEOUT_US 2000

static void mpu6050_init(void) {
    i2c_init(IMU_I2C_INST, 400 * 1000);
    gpio_set_function(IMU_I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(IMU_I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(IMU_I2C_SDA_PIN);
    gpio_pull_up(IMU_I2C_SCL_PIN);

    uint8_t buf[] = { MPUREG_PWR_MGMT_1, 0x00 };   // acorda do sleep
    i2c_write_timeout_us(IMU_I2C_INST, IMU_I2C_ADDR, buf, 2, false,
                         IMU_I2C_TIMEOUT_US);
}

// Lê um registrador; retorna false se o MPU não respondeu (NACK/timeout).
static bool mpu6050_read_reg(uint8_t reg, uint8_t *dst, size_t n) {
    if (i2c_write_timeout_us(IMU_I2C_INST, IMU_I2C_ADDR, &reg, 1, true,
                             IMU_I2C_TIMEOUT_US) != 1)
        return false;
    return i2c_read_timeout_us(IMU_I2C_INST, IMU_I2C_ADDR, dst, n, false,
                               IMU_I2C_TIMEOUT_US) == (int)n;
}

static bool mpu6050_read_accel(int16_t accel[3]) {
    uint8_t buffer[6];
    if (!mpu6050_read_reg(MPUREG_ACCEL_XOUT_H, buffer, 6)) return false;
    for (int i = 0; i < 3; i++)
        accel[i] = (int16_t)((buffer[i * 2] << 8) | buffer[i * 2 + 1]);
    return true;
}

// ============================================================================
// Label do modelo -> gesture_id do protocolo.
// Comparação case-insensitive: o Studio exporta os labels como o dataset foi
// nomeado (neste modelo, MAIÚSCULAS: "DOWN", "IDLE", "LEFT", "RIGHT", "UP").
// ============================================================================
static uint8_t gesture_from_label(const char *label) {
    if (strcasecmp(label, "up")    == 0) return GEST_UP;
    if (strcasecmp(label, "down")  == 0) return GEST_DOWN;
    if (strcasecmp(label, "left")  == 0) return GEST_LEFT;
    if (strcasecmp(label, "right") == 0) return GEST_RIGHT;
    return GEST_IDLE;
}

// ============================================================================
// Task
// ============================================================================
extern "C" void imu_task(void *p) {
    controller_t *ctx = (controller_t *)p;

    mpu6050_init();

    // Espera a USB CDC subir e confirma o sensor no barramento (diagnóstico
    // de fiação: WHO_AM_I deve responder 0x68).
    vTaskDelay(pdMS_TO_TICKS(3000));
    uint8_t who = 0;
    if (mpu6050_read_reg(MPUREG_WHOAMI, &who, 1))
        printf("[IA] MPU6050 WHO_AM_I=0x%02X %s\n", who,
               who == 0x68 ? "(OK)" : "(INESPERADO!)");
    else
        printf("[IA] ERRO: MPU6050 nao respondeu no I2C0 (GP16/GP17) -- "
               "confira a fiacao\n");

    // Janela de features do modelo (45 amostras x 3 eixos = 135 floats).
    // HALF é a metade em floats, alinhada ao frame de 3 eixos.
    constexpr size_t N    = EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE;
    constexpr size_t HALF = (N / 2 / EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME)
                            * EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME;

    float buffer[N] = { 0 };
    size_t fill_from = 0;                  // 1ª janela amostra inteira
    uint8_t last_gesture = GEST_IDLE;

    const TickType_t period =
        pdMS_TO_TICKS((uint32_t)(EI_CLASSIFIER_INTERVAL_MS + 0.5f));

    TickType_t wake = xTaskGetTickCount();

    for (;;) {
        // Queda momentânea de energia reseta o MPU6050 p/ SLEEP: ele continua
        // respondendo ACK no I2C, mas com dados congelados (tudo viraria IDLE
        // em silêncio). Confere a cada janela e reacorda se preciso.
        uint8_t pwr = 0;
        if (mpu6050_read_reg(MPUREG_PWR_MGMT_1, &pwr, 1) && (pwr & 0x40)) {
            printf("[IA] MPU6050 resetou (dormindo) -- reacordando...\n");
            mpu6050_init();
            vTaskDelay(pdMS_TO_TICKS(50));
            fill_from = 0;
            wake = xTaskGetTickCount();
            continue;
        }

        // --- amostra no ritmo do treino (45 Hz) ---
        bool i2c_ok = true;
        for (size_t ix = fill_from; ix < N;
             ix += EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME) {
            int16_t accel[3];
            if (!mpu6050_read_accel(accel)) { i2c_ok = false; break; }
            buffer[ix + 0] = (float)accel[0];
            buffer[ix + 1] = (float)accel[1];
            buffer[ix + 2] = (float)accel[2];
            vTaskDelayUntil(&wake, period);
        }

        // Falha de I2C (fio mau contato etc.): reinicializa o barramento,
        // descarta a janela e tenta de novo — a task nunca trava.
        if (!i2c_ok) {
            printf("[IA] ERRO: leitura do MPU6050 falhou -- reinicializando "
                   "I2C...\n");
            vTaskDelay(pdMS_TO_TICKS(200));
            mpu6050_init();
            fill_from = 0;
            wake = xTaskGetTickCount();
            continue;
        }

        // --- inferência ---
        // Heartbeat de diagnóstico: prova que a task está viva, mostra o heap
        // e a ATIVIDADE do sensor (soma de |delta| da janela). Atividade ~0 com
        // o controle em movimento = sensor congelado/fora de órbita.
        float atividade = 0;
        for (size_t ix = EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME; ix < N; ix++)
            atividade += fabsf(buffer[ix] - buffer[ix - EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME]);
        static uint32_t janelas = 0;
        if ((janelas++ % 10) == 0)
            printf("[IA] viva: janela #%lu, heap=%u, atividade=%.0f, ult=[%.0f %.0f %.0f]\n",
                   (unsigned long)janelas, (unsigned)xPortGetFreeHeapSize(),
                   (double)atividade,
                   (double)buffer[N - 3], (double)buffer[N - 2], (double)buffer[N - 1]);

        signal_t signal;
        if (numpy::signal_from_buffer(buffer, N, &signal) != 0) {
            printf("[IA] ERRO: signal_from_buffer falhou\n");
            continue;
        }

        ei_impulse_result_t result = { 0 };
        EI_IMPULSE_ERROR err = run_classifier(&signal, &result, false);
        if (err != EI_IMPULSE_OK) {
            printf("[IA] ERRO: run_classifier=%d, heap livre=%u\n",
                   (int)err, (unsigned)xPortGetFreeHeapSize());
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        size_t best = 0;
        for (size_t ix = 1; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
            if (result.classification[ix].value >
                result.classification[best].value)
                best = ix;
        }

        uint8_t conf = (uint8_t)(result.classification[best].value * 100.0f);
        uint8_t gesture = gesture_from_label(result.classification[best].label);
        if (gesture != GEST_IDLE && conf < EI_GESTURE_MIN_CONF)
            gesture = GEST_IDLE;

        // --- só envia mudança (não inunda o link serial do HC-06) ---
        if (gesture != last_gesture) {
            last_gesture = gesture;
            // DEBUG (remover depois): observar a inferência na serial USB.
            // Mostra a decisão ENVIADA; se a confiança ficou abaixo do corte,
            // o label vencedor aparece entre parênteses só p/ diagnóstico.
            static const char *names[] = { "IDLE", "UP", "DOWN", "LEFT", "RIGHT" };
            if (gesture == GEST_IDLE &&
                gesture_from_label(result.classification[best].label) != GEST_IDLE) {
                printf("[IA] gesto=IDLE (descartou %s, conf=%u%% < %u%%)\n",
                       result.classification[best].label, conf, EI_GESTURE_MIN_CONF);
            } else {
                printf("[IA] gesto=%s conf=%u%%\n", names[gesture], conf);
            }
            ctrl_event_t ev = { MSG_GESTURE, gesture, conf };
            xQueueSend(ctx->q_events, &ev, 0);
        }

        // --- desliza a janela: guarda a metade recente, amostra o resto ---
        memmove(buffer, buffer + HALF, (N - HALF) * sizeof(float));
        fill_from = N - HALF;
        wake = xTaskGetTickCount();        // o run_classifier gastou ticks
    }
}
