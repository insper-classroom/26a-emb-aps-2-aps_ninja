// ============================================================================
// Task: IMU (MPU6050) — movimento por INCLINAÇÃO + especial por IA
//
// MOVIMENTO (up/down/left/right): SEM IA. Mesma técnica da APS 7 (lab do
// mouse): Fusion AHRS funde giroscópio+acelerômetro em ângulos de Euler
// (roll/pitch). Inclinar o controle além de TILT_ON_DEG dispara a seta; é
// preciso voltar abaixo de TILT_OFF_DEG (histerese) para poder disparar de
// novo. Reação por amostra (~22 ms), sem esperar janela de inferência.
//
// ESPECIAL (espaço no jogo): a IA (Edge Impulse) fica dedicada a UM gesto,
// "UPDOWN" (sobe-e-desce). Quando o classificador reconhece esse label com
// confiança >= EI_SPECIAL_MIN_CONF, envia GEST_SPECIAL ao PC. Enquanto o
// modelo novo (com o label UPDOWN) não for exportado para ei-model/, o
// classificador roda mas nunca dispara — o resto do controle funciona normal.
//
// Fluxo: amostra accel+gyro no ritmo do modelo (EI_CLASSIFIER_INTERVAL_MS).
// A cada amostra: atualiza o AHRS e a máquina de inclinação (-> MSG_GESTURE
// imediato na q_events). A cada janela cheia (50% de overlap): run_classifier()
// -> se label == "UPDOWN", publica GEST_SPECIAL (com cooldown p/ não disparar
// duas vezes na mesma execução do gesto).
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
#include "Fusion.h"

extern "C" {
#include "pins.h"
#include "protocol.h"
#include "controller.h"
#include "mpu6050.h"
#include "imu_ei.h"
}

// ---------------------------------------------------------------------------
// Movimento por inclinação (igual à filosofia da APS 7: zona morta + ângulo)
// ---------------------------------------------------------------------------
#define TILT_ON_DEG     20.0f   // inclinou além disso -> dispara a seta
#define TILT_OFF_DEG    10.0f   // voltou aquém disso  -> rearma (histerese)
#define WARMUP_SAMPLES  200     // amostras p/ calibrar o bias do gyro (APS 7)

// ---------------------------------------------------------------------------
// Especial por IA
// ---------------------------------------------------------------------------
#define EI_SPECIAL_LABEL     "updown"  // label do modelo novo (case-insensitive)
#define EI_SPECIAL_MIN_CONF  70        // confiança mínima (%)
#define EI_SPECIAL_COOLDOWN_MS 1500    // janelas com 50% overlap veem o mesmo
                                       // gesto 2x; o cooldown evita 2 espaços

// Portão de atividade: com o controle PARADO o modelo alucina "updown" com
// alta confiança (o idle do treino tinha ruído de mão; imóvel na mesa fica
// fora da distribuição). Só roda a inferência se a janela tiver movimento:
// atividade = soma de |delta| entre amostras consecutivas (unidades cruas do
// MPU). Em repouso fica na casa de dezenas de milhares; um gesto de verdade
// passa de centenas de milhares. Ajustar com o print [IA] atividade=...
#define EI_ACTIVITY_MIN  100000.0f

// Guarda de inclinação: o modelo só conhece idle/updown/waving, então uma
// INCLINAÇÃO (gesto de seta) dentro da janela também sai "updown" com alta
// confiança. Como o sobe-e-desce de verdade não passa de TILT_ON_DEG (medido
// na bancada), qualquer inclinação forte recente invalida o especial pela
// duração de uma janela do modelo.
#define EI_SPECIAL_TILT_GUARD_MS 2000

// ============================================================================
// MPU6050 (I2C0 — pinos em pins.h)
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

// Burst de 14 bytes a partir de ACCEL_XOUT_H: accel[3] + temp + gyro[3].
static bool mpu6050_read_accel_gyro(int16_t accel[3], int16_t gyro[3]) {
    uint8_t buffer[14];
    if (!mpu6050_read_reg(MPUREG_ACCEL_XOUT_H, buffer, 14)) return false;
    for (int i = 0; i < 3; i++)
        accel[i] = (int16_t)((buffer[i * 2] << 8) | buffer[i * 2 + 1]);
    for (int i = 0; i < 3; i++)
        gyro[i] = (int16_t)((buffer[8 + i * 2] << 8) | buffer[8 + i * 2 + 1]);
    return true;
}

// ============================================================================
// Inclinação -> gesto (com histerese). Retorna o gesto disparado nesta
// amostra (ou GEST_IDLE). *armed controla o rearme: só dispara de novo depois
// que o controle voltar para perto do neutro.
//
// Mapeamento (ajustar sinal/eixo conforme a montagem física — conferir com o
// print de diagnóstico [MOV] roll/pitch):
//   pitch < -TILT  -> UP      pitch > +TILT -> DOWN
//   roll  > +TILT  -> RIGHT   roll  < -TILT -> LEFT
// ============================================================================
static uint8_t tilt_detect(float roll, float pitch, bool *armed) {
    float aroll = fabsf(roll), apitch = fabsf(pitch);

    if (!*armed) {
        if (aroll < TILT_OFF_DEG && apitch < TILT_OFF_DEG) *armed = true;
        return GEST_IDLE;
    }
    if (aroll < TILT_ON_DEG && apitch < TILT_ON_DEG) return GEST_IDLE;

    *armed = false;
    if (apitch >= aroll)                  // eixo dominante decide
        return (pitch < 0) ? GEST_UP : GEST_DOWN;
    return (roll > 0) ? GEST_RIGHT : GEST_LEFT;
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
        printf("[IMU] MPU6050 WHO_AM_I=0x%02X %s\n", who,
               who == 0x68 ? "(OK)" : "(INESPERADO!)");
    else
        printf("[IMU] ERRO: MPU6050 nao respondeu no I2C0 (GP16/GP17) -- "
               "confira a fiacao\n");

    // Janela de features do modelo (amostras x 3 eixos).
    // HALF é a metade em floats, alinhada ao frame de 3 eixos.
    constexpr size_t N    = EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE;
    constexpr size_t HALF = (N / 2 / EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME)
                            * EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME;

    float buffer[N] = { 0 };
    size_t fill_from = 0;                  // 1ª janela amostra inteira

    const float sample_period_s = EI_CLASSIFIER_INTERVAL_MS / 1000.0f;
    const TickType_t period =
        pdMS_TO_TICKS((uint32_t)(EI_CLASSIFIER_INTERVAL_MS + 0.5f));

    // --- Fusion AHRS (APS 7): bias do gyro calibrado nas primeiras amostras ---
    FusionAhrs ahrs;
    FusionAhrsInitialise(&ahrs);
    float gyro_bias[3] = { 0, 0, 0 };
    int   warmup = 0;
    bool  tilt_armed = true;
    uint8_t last_sent = GEST_IDLE;         // último MSG_GESTURE enviado ao PC
    TickType_t last_special = 0;
    TickType_t last_tilt = 0;              // última inclinação >= TILT_ON_DEG

    TickType_t wake = xTaskGetTickCount();

    for (;;) {
        // Queda momentânea de energia reseta o MPU6050 p/ SLEEP: ele continua
        // respondendo ACK no I2C, mas com dados congelados. Confere a cada
        // janela e reacorda se preciso.
        uint8_t pwr = 0;
        if (mpu6050_read_reg(MPUREG_PWR_MGMT_1, &pwr, 1) && (pwr & 0x40)) {
            printf("[IMU] MPU6050 resetou (dormindo) -- reacordando...\n");
            mpu6050_init();
            vTaskDelay(pdMS_TO_TICKS(50));
            fill_from = 0;
            wake = xTaskGetTickCount();
            continue;
        }

        // --- amostra no ritmo do modelo; cada amostra alimenta AHRS + janela ---
        bool i2c_ok = true;
        for (size_t ix = fill_from; ix < N;
             ix += EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME) {
            int16_t accel[3], gyro[3];
            if (!mpu6050_read_accel_gyro(accel, gyro)) { i2c_ok = false; break; }

            // janela da IA (accel cru, como no treino)
            buffer[ix + 0] = (float)accel[0];
            buffer[ix + 1] = (float)accel[1];
            buffer[ix + 2] = (float)accel[2];

            // --- APS 7: gyro em °/s (±250 dps -> /131), accel em g (/16384) ---
            FusionVector gv, av;
            gv.axis.x = gyro[0] / 131.0f - gyro_bias[0];
            gv.axis.y = gyro[1] / 131.0f - gyro_bias[1];
            gv.axis.z = gyro[2] / 131.0f - gyro_bias[2];
            av.axis.x = accel[0] / 16384.0f;
            av.axis.y = accel[1] / 16384.0f;
            av.axis.z = accel[2] / 16384.0f;

            if (warmup < WARMUP_SAMPLES) {
                gyro_bias[0] += gyro[0] / 131.0f;
                gyro_bias[1] += gyro[1] / 131.0f;
                gyro_bias[2] += gyro[2] / 131.0f;
                if (++warmup == WARMUP_SAMPLES) {
                    gyro_bias[0] /= WARMUP_SAMPLES;
                    gyro_bias[1] /= WARMUP_SAMPLES;
                    gyro_bias[2] /= WARMUP_SAMPLES;
                    printf("[MOV] gyro calibrado -- movimento por inclinacao "
                           "ativo (ON=%.0f OFF=%.0f graus)\n",
                           (double)TILT_ON_DEG, (double)TILT_OFF_DEG);
                }
            } else {
                FusionAhrsUpdateNoMagnetometer(&ahrs, gv, av, sample_period_s);
                FusionEuler e =
                    FusionQuaternionToEuler(FusionAhrsGetQuaternion(&ahrs));

                // inclinação forte recente -> bloqueia o especial (ver guarda)
                if (fabsf(e.angle.roll)  >= TILT_ON_DEG ||
                    fabsf(e.angle.pitch) >= TILT_ON_DEG)
                    last_tilt = xTaskGetTickCount();

                uint8_t g = tilt_detect(e.angle.roll, e.angle.pitch,
                                        &tilt_armed);
                if (g != GEST_IDLE) {
                    static const char *names[] =
                        { "IDLE", "UP", "DOWN", "LEFT", "RIGHT" };
                    printf("[MOV] %s (roll=%.0f pitch=%.0f)\n",
                           names[g], (double)e.angle.roll,
                           (double)e.angle.pitch);
                    ctrl_event_t ev = { MSG_GESTURE, g, 100 };
                    xQueueSend(ctx->q_events, &ev, 0);
                    last_sent = g;
                } else if (tilt_armed && last_sent != GEST_IDLE) {
                    // voltou ao neutro: avisa o PC (espelha o fluxo antigo)
                    ctrl_event_t ev = { MSG_GESTURE, GEST_IDLE, 100 };
                    xQueueSend(ctx->q_events, &ev, 0);
                    last_sent = GEST_IDLE;
                }
            }

            vTaskDelayUntil(&wake, period);
        }

        // Falha de I2C (fio mau contato etc.): reinicializa o barramento,
        // descarta a janela e tenta de novo — a task nunca trava.
        if (!i2c_ok) {
            printf("[IMU] ERRO: leitura do MPU6050 falhou -- reinicializando "
                   "I2C...\n");
            vTaskDelay(pdMS_TO_TICKS(200));
            mpu6050_init();
            fill_from = 0;
            wake = xTaskGetTickCount();
            continue;
        }

        // --- inferência: só o gesto ESPECIAL (label "UPDOWN") interessa ---
        // Atividade da janela (p/ o portão e p/ diagnóstico)
        float atividade = 0;
        for (size_t ix = EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME; ix < N; ix++)
            atividade += fabsf(buffer[ix] -
                               buffer[ix - EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME]);

        // Heartbeat de diagnóstico: prova que a task está viva e mostra o heap.
        static uint32_t janelas = 0;
        if ((janelas++ % 10) == 0)
            printf("[IA] viva: janela #%lu, heap=%u, atividade=%.0f\n",
                   (unsigned long)janelas, (unsigned)xPortGetFreeHeapSize(),
                   (double)atividade);

        // Portão: janela sem movimento não passa pelo classificador.
        if (atividade < EI_ACTIVITY_MIN) {
            memmove(buffer, buffer + HALF, (N - HALF) * sizeof(float));
            fill_from = N - HALF;
            wake = xTaskGetTickCount();
            continue;
        }

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
        // DEBUG: só imprime quando há movimento (passou no portão), p/ ajustar
        // EI_ACTIVITY_MIN e EI_SPECIAL_MIN_CONF na bancada.
        printf("[IA] janela: %s conf=%u%% atividade=%.0f\n",
               result.classification[best].label, conf, (double)atividade);
        TickType_t now = xTaskGetTickCount();
        if (strcasecmp(result.classification[best].label,
                       EI_SPECIAL_LABEL) == 0 &&
            conf >= EI_SPECIAL_MIN_CONF &&
            (now - last_special) >= pdMS_TO_TICKS(EI_SPECIAL_COOLDOWN_MS)) {
            if ((now - last_tilt) < pdMS_TO_TICKS(EI_SPECIAL_TILT_GUARD_MS)) {
                printf("[IA] updown suprimido (inclinacao recente na janela)\n");
            } else {
                last_special = now;
                printf("[IA] ESPECIAL (UPDOWN) conf=%u%%\n", conf);
                ctrl_event_t ev = { MSG_GESTURE, GEST_SPECIAL, conf };
                xQueueSend(ctx->q_events, &ev, 0);
            }
        }

        // --- desliza a janela: guarda a metade recente, amostra o resto ---
        memmove(buffer, buffer + HALF, (N - HALF) * sizeof(float));
        fill_from = N - HALF;
        wake = xTaskGetTickCount();        // o run_classifier gastou ticks
    }
}
