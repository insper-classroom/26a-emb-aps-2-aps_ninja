# 🛹 Controle Gestual para Subway Surfers

Controle estilo *Wii* que traduz **movimentos do corpo em comandos de jogo**. O jogador
segura o controle na mão e, com gestos (levantar, abaixar, virar para os lados),
controla o personagem do **Subway Surfers** sem encostar no teclado.

O reconhecimento de movimento é feito por uma **rede neural rodando localmente na
própria placa** (Edge Impulse / edge computing), e o resultado é enviado ao PC por
**Bluetooth**. O controle ainda **reage de volta** ao jogo: quando o jogador morre,
ele vibra.

> **Lab Expert aplicado:** *IA + Bluetooth*
> - **IA (Edge Impulse / RP2350):** classificação de gestos da IMU rodando *on-device*.
> - **Bluetooth (HC-06):** transporte dos eventos controle ↔ PC, com protocolo próprio.

---

## 🎮 Jogo

**Subway Surfers** — *endless runner*. O personagem corre automaticamente e o jogador
só precisa reagir: desviar para os lados, pular obstáculos e rolar por baixo deles.
É um encaixe ideal para um classificador de gestos, porque o jogo é controlado por um
conjunto **pequeno e bem distinto de movimentos**.

| Gesto do jogador | Ação no jogo | Tecla enviada ao PC |
|---|---|---|
| Parado (`idle`) | — | — |
| Levantar a mão (`up`) | Pular | `↑` |
| Abaixar a mão (`down`) | Rolar | `↓` |
| Virar o pulso à esquerda (`left`) | Trocar de pista | `←` |
| Virar o pulso à direita (`right`) | Trocar de pista | `→` |

---

## 🕹️ Ideia do controle

Formato de **bastão/controle de mão (estilo Wii-mote)**, segurado verticalmente. A IMU
fica alinhada com o eixo da mão para capturar os gestos de forma natural. Os botões
ficam acessíveis ao polegar.

- **Projeto mecânico:** carcaça impressa em 3D no formato de bastão, com compartimento
  para a bateria e o módulo de carga, abertura para o OLED e para o LED RGB, e furos
  para os 4 botões. *(imagens na seção [Imagens](#-imagens))*
- **Scratch/proposta:** ver [Imagens](#-imagens).

---

## 🔌 Inputs e Outputs

### Entradas (sensores)
| Entrada | Hardware | Interface | Função |
|---|---|---|---|
| Movimento | **MPU6050 (IMU)** | I2C0 (GP16/GP17) | gesto do jogador → rede neural |
| 4x digital | **4 botões táteis** | GPIO IRQ (GP18–GP21) | Start/Pause, Hoverboard, Macro, Perfil |
| Tensão da bateria | **Divisor de tensão** | ADC2 (GP28) | nível de bateria |

### Saídas (atuadores)
| Saída | Hardware | Interface | Função |
|---|---|---|---|
| Conexão BT | **HC-06** | UART1 (GP4/GP5) | link com o PC |
| Status visual | **OLED SSD1306** | I2C1 (GP14/GP15) | conectado / bateria / calibração |
| Status / feedback | **LED RGB** | PWM (GP7/8/9) | estado, calibração, alerta |
| LED de conexão | **LED status** | PWM (GP11) | pareado / aguardando |
| Háptico | **Motor de vibração** | PWM (GP22) + driver | feedback ao morrer/eventos |

### Pinagem completa
| Bloco | Pinos |
|---|---|
| HC-06 (Bluetooth) | UART1: TX=GP4, RX=GP5, STATE=GP2, EN=GP3 |
| MPU6050 (IMU) | I2C0: SDA=GP16, SCL=GP17 (addr 0x68) |
| 4 botões | GP18, GP19, GP20, GP21 (pull-up interno) |
| Motor de vibração | GP22 (PWM, via transistor + diodo flyback) |
| Bateria | GP28 = ADC2 (divisor /2) |
| OLED SSD1306 | I2C1: SDA=GP14, SCL=GP15 (addr 0x3C, 128x32) |
| LED RGB | GP7 (R), GP8 (G), GP9 (B) — PWM |
| LED de status | GP11 — PWM |

---

## 📡 Protocolo utilizado

Comunicação serial sobre Bluetooth (HC-06), com quadros delimitados por byte de
sincronismo, tipo, tamanho e *checksum* (XOR), para robustez a ruído.

### Controle → PC
```
0xAA | TYPE | LEN | PAYLOAD[LEN] | CHK
CHK = XOR(TYPE, LEN, PAYLOAD...)
```
| TYPE | Mensagem | Payload |
|---|---|---|
| `0x01` | GESTURE | `gesture_id`, `confiança (%)` |
| `0x02` | BUTTON | `button_id`, `edge (1=press, 0=release)` |
| `0x03` | BATTERY | `percent` |
| `0x04` | STATUS | `flags` |

`gesture_id`: `0=idle, 1=up, 2=down, 3=left, 4=right`

### PC → Controle
```
0x55 | TYPE | LEN | PAYLOAD[LEN] | CHK
```
| TYPE | Mensagem | Payload |
|---|---|---|
| `0x10` | GAME_EVENT | `event_id` (1=morreu, 2=moeda, 3=powerup) |
| `0x11` | HAPTIC | `pattern_id` |
| `0x12` | LED | `r`, `g`, `b` |

No PC, o aplicativo recebe `GESTURE` e simula a seta correspondente; ao detectar a
morte do jogador, envia `GAME_EVENT(morreu)` e o controle responde com vibração + LED
vermelho.

---

## 🧱 Diagrama de blocos do firmware

Firmware em **FreeRTOS**, sem variáveis globais — todo o estado vive em um *struct* de
contexto passado às tasks; o acesso compartilhado é protegido por mutex.

```
                       CONTROLE — Raspberry Pi Pico 2 (FreeRTOS)
 ┌──────────────────────────────────────────────────────────────────────────┐
 │  [MPU6050]──I2C0──▶ imu_task ──(Edge Impulse run_classifier)               │
 │                        │ gesto + confiança                                 │
 │                        ▼                                                    │
 │  [4 botões]──IRQ──▶ (ISR botões) ──▶ q_buttons ──▶ input_task              │
 │                                                       │ perfil/macro        │
 │  [bateria]──ADC──▶ battery_task ───────────┐          ▼                     │
 │                                            └────▶  q_events                 │
 │                                                       │                     │
 │                                                       ▼                     │
 │                                                  bt_tx_task ──UART1──▶[HC06]~~▶ PC
 │                                                                            │
 │  [HC-06]~~▶ (ISR UART RX) ──▶ q_rx ──▶ bt_rx_task                          │
 │                                            │ decodifica                     │
 │                                            ▼                                │
 │                                       q_feedback                            │
 │                                        │        │                          │
 │                                        ▼        ▼                          │
 │                                 haptic_task   status_task                  │
 │                               [motor + RGB]  [OLED + LED status]            │
 │                                                                            │
 │  mutex_state ─ protege: conexão, perfil ativo, % bateria, dados calib.     │
 └────────────────────────────────────────────────────────────────────────────┘
```

### Tasks
| Task | Prioridade | Função |
|---|---|---|
| `imu_task` | 3 | Lê a IMU, roda a inferência (Edge Impulse) e emite o gesto |
| `input_task` | 2 | Trata botões, aplica perfil/macro/calibração e gera eventos |
| `bt_tx_task` | 2 | Empacota eventos no protocolo e transmite pela UART |
| `bt_rx_task` | 2 | Decodifica pacotes vindos do PC e gera feedback |
| `haptic_task` | 1 | Reproduz vibração e feedback visual (motor + LED RGB) |
| `status_task` | 1 | Atualiza o OLED e o LED de status da conexão |
| `battery_task` | 1 | Lê a tensão da bateria e alerta nível baixo |

### Filas
| Fila | Produtor → Consumidor |
|---|---|
| `q_events` | imu / input / battery → `bt_tx_task` |
| `q_rx` | ISR UART → `bt_rx_task` |
| `q_buttons` | ISR botões → `input_task` |
| `q_feedback` | `bt_rx_task` → haptic / status |

### Semáforos
| Objeto | Função |
|---|---|
| `mutex_state` | Protege o estado compartilhado (conexão, perfil, bateria, calibração) |

### Interrupções (ISR)
| ISR | Origem → Destino |
|---|---|
| `uart_rx_isr` | RX do HC-06 → `q_rx` |
| `gpio_btn_isr` | 4 botões (1 callback) → `q_buttons` |

---

## ✨ Conceitos extras aplicados

- **ADC + IMU em conjunto:** leitura de bateria (ADC) + gestos (IMU).
- **Controle recebe info do PC e reage:** jogador morre → controle vibra.
- **Háptico:** motor de vibração para eventos do jogo.
- **Calibração guiada (wizard):** segurar um botão zera o gyro/accel, com status por LED.
- **Gerenciamento de bateria:** leitura de tensão, indicação de nível e alerta.
- **Botão de macro:** grava e reproduz uma sequência de comandos.
- **Perfis de usuário:** mapeamentos/sensibilidade trocados por botão.

---

## 🖼️ Imagens

> *(Preencher na entrega final)*
>
> - Proposta (sketch/CAD): _link_
> - Controle real (fotos): _link_
> - Vídeo de demonstração: _link_
> - Modelo Edge Impulse: _link_

---

## 🛠️ Como compilar

Projeto baseado no **Pico SDK 2.2.0** + **FreeRTOS**.

```bash
# via extensão Raspberry Pi Pico no VS Code (recomendado), ou:
mkdir build && cd build
cmake .. && ninja
```

O binário `pico_emb.uf2` é gerado em `build/`. Grave segurando o BOOTSEL ao conectar a
Pico 2.

A interface no PC fica em `python/mouse_rgb.py` (porta serial `/dev/ttyACM0` no Linux).

---

## 🔗 Links úteis

- [Edge Impulse — Continuous Motion Recognition](https://docs.edgeimpulse.com/docs/tutorials/end-to-end-tutorials/continuous-motion-recognition)
- [Repo exemplo — data forwarding](https://github.com/insper-embarcados/edgeimpulse-dataforwarding)
- [Repo exemplo — runner (deploy)](https://github.com/insper-embarcados/edgeimpulse-runner)
