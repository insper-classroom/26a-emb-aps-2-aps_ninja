#ifndef PINS_H
#define PINS_H

#include "hardware/i2c.h"

// ============================================================================
// HC-06 Bluetooth  -> definido em hc06.h (UART1: TX=4, RX=5, STATE=2, EN=3)
// ============================================================================

// ============================================================================
// MPU6050 IMU (I2C0) — barramento separado do OLED p/ não brigar com o
// sampling da rede neural (Edge Impulse)
// ============================================================================
#define IMU_I2C_INST     i2c0
#define IMU_I2C_SDA_PIN  16
#define IMU_I2C_SCL_PIN  17
#define IMU_I2C_ADDR     0x68

// ============================================================================
// 4 entradas digitais — pull-up interno, todas com IRQ (requisito do roteiro).
// Fisicamente o controle monta UM botão (GP18 -> GND): o botão de AÇÃO, que
// vira ESPAÇO no PC (inicia o jogo / ativa o poder). Qualquer uma das 4
// entradas dispara a mesma ação.
// ============================================================================
#define BTN_ACTION_PIN   18   // botão de ação (espaço) — o único montado
#define BTN_AUX1_PIN     19
#define BTN_AUX2_PIN     20
#define BTN_AUX3_PIN     21

// id lógico enviado ao PC (campo button_id do protocolo)
#define BTN_ID_ACTION    0

// ============================================================================
// Motor de vibração (háptico) — PWM, acionado por transistor/MOSFET + diodo
// ============================================================================
#define HAPTIC_PIN       22

// ============================================================================
// Bateria — divisor de tensão lido pelo ADC
// ============================================================================
#define BATTERY_ADC_PIN  28
#define BATTERY_ADC_CHAN 2

// ============================================================================
// LED RGB (PWM) — feedback de eventos / calibração / bateria
// ============================================================================
#define LED_PIN_R  7
#define LED_PIN_G  8
#define LED_PIN_B  9

// ============================================================================
// LED de status do Bluetooth (PWM)
// ============================================================================
#define LED_STATUS_PIN 11

// ============================================================================
// OLED SSD1306 (I2C1)
// ============================================================================
#define OLED_I2C_INST    i2c1
#define OLED_I2C_SDA_PIN 14
#define OLED_I2C_SCL_PIN 15
#define OLED_I2C_ADDR    0x3C
#define OLED_WIDTH       128
#define OLED_HEIGHT      32

#endif // PINS_H
