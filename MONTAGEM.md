# Guia de montagem na perfboard — Controle SUBWAY-CTRL

Guia pra soldar o controle numa **perfboard de furos isolados** (cada furo é uma
ilha separada; toda ligação é feita por você com solda/fio).

Placa: **32 × 25 furos** (32 na horizontal = colunas 1–32; 25 na vertical =
linhas 1–25). Passo 0,1" (2,54 mm).

> Este é um layout **sugerido**. Adapte se quiser — o que **não** muda é a lista
> de ligações elétricas (seção "Ligações por módulo").

---

## 1. A ideia central: cada lado da Pico tem seus sinais

Coloque cada módulo **do lado da Pico onde ficam os pinos dele**. Olha o mapa
(Pico com o USB pra cima, ocupando as linhas 3–22):

```
        LADO ESQUERDO (col ~8)              LADO DIREITO (col ~15)
 linha   pino  GPIO   sinal                 pino  GPIO   sinal
   3      1    GP0    -                       40   VBUS   (5V)
   4      2    GP1    -                       39   VSYS   -
   5      3    GND    --- GND                 38   GND    --- GND
   6      4    GP2  → HC-06 STATE             37   3V3_EN -
   7      5    GP3  → HC-06 EN/KEY            36   3V3    --- 3V3 (saída!)
   8      6    GP4  → HC-06 RXD               35   VREF   -
   9      7    GP5  → HC-06 TXD               34   GP28 → BATERIA (ADC)
  10      8    GND    --- GND                 33   AGND   --- GND
  11      9    GP6    -                       32   GP27   -
  12     10    GP7  → RGB R                   31   GP26   -
  13     11    GP8  → RGB G                   30   RUN    -
  14     12    GP9  → RGB B                   29   GP22 → MOTOR (base BC337)
  15     13    GND    --- GND                 28   GND    --- GND
  16     14    GP10   -                       27   GP21 → Botão PROFILE
  17     15    GP11 → LED STATUS              26   GP20 → Botão MACRO
  18     16    GP12   -                       25   GP19 → Botão SPECIAL
  19     17    GP13   -                       24   GP18 → Botão START
  20     18    GND    --- GND                 23   GND    --- GND
  21     19    GP14 → OLED SDA                22   GP17 → IMU SCL
  22     20    GP15 → OLED SCL                21   GP16 → IMU SDA
```

**Conclusão:**
- **Esquerda da Pico** (colunas 1–7): HC-06, RGB, LED status, OLED.
- **Direita da Pico** (colunas 16–32): botões, motor+driver, bateria, IMU.

---

## 2. Planta baixa (vista de cima)

```
   col: 1   4   7  8        15  18      24        32
 linha +---------------------------------------------+
   1   | 3V3 rail (fio rígido) ====================== |  <- trilho 3V3 (linha 1)
   2   | GND rail (fio rígido) ====================== |  <- trilho GND  (linha 2)
   3   |          [ USB ↑ ]                           |
   4   | HC-06   ┌─────────┐                BOTÕES    |
   5   | header  │         │              [S][SP]     |
   6   | (6 pin) │  PICO   │              [M][PR]     |
   7   |  V T R T│ (vert.) │                          |
   8   |  C X X S│  pinos  │   MOTOR: BC337 + diodo   |
   9   | RGB LED │  col 8  │   GP28→ divisor bateria  |
  10   |  +3res  │  e col  │                          |
  ...  |         │  15     │                          |
  17   | LED st. │         │                          |
  ...  │         │         │                          |
  21   | OLED    │         │   IMU (MPU6050)          |
  22   | (4 pin) └─────────┘   header (4 pin)         |
  23   |                                              |
  24   |                                              |
  25   +---------------------------------------------+
```

(Posições aproximadas — o importante é "esquerda x direita" da seção 1.)

---

## 3. Trilhos de alimentação (faça PRIMEIRO)

Sem trilhos prontos, você cria dois "barramentos" com **fio rígido estanhado**:

- **Trilho 3V3:** um fio reto ao longo da **linha 1**, de ponta a ponta.
- **Trilho GND:** um fio reto ao longo da **linha 2**, de ponta a ponta.

Depois, alimente os trilhos a partir da Pico:
- Pico **3V3 (pino 36, direita, linha 7)** → trilho 3V3
- Pico **GND (qualquer um: pinos 3,8,13,18,23,28,33,38)** → trilho GND

Tudo que precisa de energia tapa nesses dois trilhos. **GND único pra tudo** é
obrigatório (módulos, LEDs, motor, divisor).

> O HC-06 funciona na alimentação que você já usa hoje (3V3 ou VBUS/5V). Se for
> 5V, puxe do **VBUS (pino 40)**, não do trilho 3V3.

---

## 4. Ligações por módulo (a "bíblia" da solda)

Marque cada linha com ✓ conforme solda e testa.

### HC-06 (esquerda) — UART1, dados a 9600
- [ ] GP4 (pino 6)  → HC-06 **RXD**   (TX da Pico no RX do módulo!)
- [ ] GP5 (pino 7)  → HC-06 **TXD**
- [ ] GP2 (pino 4)  → HC-06 **STATE**
- [ ] GP3 (pino 5)  → HC-06 **EN/KEY**
- [ ] HC-06 **VCC** → trilho 3V3 (ou VBUS/5V)
- [ ] HC-06 **GND** → trilho GND

### OLED SSD1306 (esquerda) — I2C1, addr 0x3C
- [ ] GP14 (pino 19) → OLED **SDA**
- [ ] GP15 (pino 20) → OLED **SCL**
- [ ] OLED **VCC** → trilho 3V3
- [ ] OLED **GND** → trilho GND

### LED RGB (esquerda) — catodo comum, cada cor com resistor 220–330Ω
- [ ] GP7 (pino 10) →[R]→ anodo **R**
- [ ] GP8 (pino 11) →[R]→ anodo **G**
- [ ] GP9 (pino 12) →[R]→ anodo **B**
- [ ] catodo comum → trilho GND

### LED de status (esquerda)
- [ ] GP11 (pino 15) →[R 220–330Ω]→ anodo do LED
- [ ] catodo → trilho GND

### Botões (direita) — pull-up interno, cada um pro GND
- [ ] GP18 (pino 24) → Botão START → trilho GND
- [ ] GP19 (pino 25) → Botão SPECIAL → trilho GND
- [ ] GP20 (pino 26) → Botão MACRO → trilho GND
- [ ] GP21 (pino 27) → Botão PROFILE → trilho GND

### Motor de vibração (direita) — driver BC337 (E-B-C) + diodo
- [ ] GP22 (pino 29) →[1kΩ]→ **Base** do BC337
- [ ] **Collector** → um fio do motor
- [ ] outro fio do motor → trilho 3V3
- [ ] **Emitter** → trilho GND
- [ ] **Diodo** em paralelo com o motor: faixa (catodo) pro lado do 3V3

### Bateria (direita) — divisor de tensão no ADC
- [ ] topo do divisor → +bateria ; base → GND
- [ ] meio do divisor → GP28 (pino 34)

### IMU MPU6050 (direita) — I2C0, addr 0x68  [parte do parceiro de IA]
- [ ] GP16 (pino 21) → IMU **SDA**
- [ ] GP17 (pino 22) → IMU **SCL**
- [ ] IMU **VCC** → trilho 3V3 ; **GND** → trilho GND

---

## 5. Ordem de montagem (importante pra não se enroscar)

1. **Headers fêmea** da Pico (2 barras de 20), HC-06, OLED, IMU. Encaixe os
   módulos só no final — solde os headers vazios primeiro.
2. **Trilhos 3V3 e GND** (seção 3) + os fios da Pico até eles.
3. **Resistores, LEDs e botões** (componentes passivos).
4. **Driver do motor** (BC337 + diodo + 1kΩ).
5. **Fios de sinal** por último (GP* → cada módulo).
6. Encaixe os módulos e teste **um de cada vez** (veja seção 7).

---

## 6. Como fazer uma ligação na perfboard pura

Cada furo é isolado, então você conecta dois pontos por **baixo da placa**:
- **Furos vizinhos:** uma gota de solda já faz a ponte (ou um pedacinho de perna
  de resistor cortada).
- **Pontos distantes:** passe um **fio** (rígido fino ou perna de componente)
  por baixo, soldando nas duas pontas. Fios que cruzam outros → use fio com capa.
- **Trilhos longos:** fio rígido estanhado reto, soldando em cada furo que toca.

Dica: solde a ponta de **um** lado, posicione, depois solde a outra. Não tente
segurar tudo de uma vez.

---

## 7. Teste incremental (já validamos cada um em protoboard)

Não solde tudo e teste no fim — teste **a cada módulo**:

1. **Só a Pico** alimentada → grava firmware, vê se boota (LED da placa / serial).
2. **OLED** → deve mostrar `SUBWAY CONTROL / BT:... / BAT:..% PIN:1234`.
3. **HC-06** → aparece como `SUBWAY-CTRL` no Bluetooth, pareia com PIN 1234.
4. **Botões** → cada aperto vira `[BTN]` na serial (se mantiver o debug) ou troca
   perfil/macro.
5. **LED status** → respira sem pareado, fixo quando pareado.
6. **RGB** → segura PROFILE 1s = amarelo.
7. **Motor + RGB feedback** → use `python/subway_test.py` (manda vibração/cor por
   Bluetooth).

---

## Pinagem de referência rápida

| GPIO | Pino | Função |
|------|------|--------|
| GP2  | 4  | HC-06 STATE |
| GP3  | 5  | HC-06 EN/KEY |
| GP4  | 6  | HC-06 RXD (TX Pico) |
| GP5  | 7  | HC-06 TXD (RX Pico) |
| GP7  | 10 | RGB R |
| GP8  | 11 | RGB G |
| GP9  | 12 | RGB B |
| GP11 | 15 | LED status |
| GP14 | 19 | OLED SDA |
| GP15 | 20 | OLED SCL |
| GP16 | 21 | IMU SDA |
| GP17 | 22 | IMU SCL |
| GP18 | 24 | Botão Start |
| GP19 | 25 | Botão Special |
| GP20 | 26 | Botão Macro |
| GP21 | 27 | Botão Profile |
| GP22 | 29 | Motor (base BC337) |
| GP28 | 34 | Bateria (ADC) |
| 3V3  | 36 | Alimentação 3V3 |
| VBUS | 40 | 5V (USB) |
