# STATUS do projeto — retomar daqui (atualizado 2026-06-10)

## ✅ Pronto e validado

- **PONTA A PONTA (2026-06-10)**: gestos E botão atravessaram a Bluetooth e chegaram no
  `subway_play.py` — dezenas de gestos (DOWN/LEFT/RIGHT/UP, 62–99%) e 9 apertos do botão
  (`BOTAO -> espaco`). O controle está funcionalmente completo.
- **Decisão de design (2026-06-10): UM botão físico** (GP18 → GND) que age como ESPAÇO
  (inicia o jogo / ativa o poder). Macro/perfil/calibração removidos do firmware; as 4
  entradas digitais seguem configuradas com IRQ (requisito do roteiro) e qualquer uma
  dispara a mesma ação. Gestos = setas (RIGHT→→, LEFT→←, UP→↑, DOWN→↓).
- **IA na Pico** (`main/imu_ei.cpp`): MPU6050 → Edge Impulse → gestos IDLE/UP/DOWN/LEFT/RIGHT
  com até 99% de confiança. Validado 3 min contínuos, heap estável (sem vazamento).
- **Robustez do IMU**: I2C com timeout (mau contato não trava), auto-reacordar se o sensor
  resetar p/ sleep, e heartbeat na serial com `atividade` (≈10 mil parado, 220–380 mil mexendo).
- **HC-06 SUBWAY-CTRL** (MAC `00:22:09:01:6A:92`, PIN 1234): AT em 38400, **dados em 9600 —
  confirmado por varredura de baud** (`HC06_BAUD_SWEEP` em main.c). Alimentar com **VBUS 5 V**.
- **Driver do PC**: `python/subway_play.py` (gestos → setas via pynput, botão → espaço,
  eventos d/c/p → vibração).

## ⏳ Falta

1. **Jogar de verdade**: rodar `subway_play.py` num terminal SEU (com acesso ao display —
   rodado pelo Claude o pynput não tem X auth) e browser em modo X11
   (`google-chrome --ozone-platform=x11 https://poki.com/...`). Antes de conectar:
   `bluetoothctl disconnect 00:22:09:01:6A:92` (HC-06 só aceita 1 conexão).
2. **Observar falsos LEFT em repouso**: no fim do teste, com o controle (talvez) parado,
   o modelo emitiu LEFT 62–93% repetidamente. Se atrapalhar no jogo: subir
   `EI_GESTURE_MIN_CONF`, checar orientação do IMU (ver memória do amigo) ou re-treinar.
3. Detecção automática de "game over" no PC (hoje é manual: tecla `d` no subway_play.py).
4. Remover os prints de debug: `[TX]` (main.c), `[IA] viva/atividade` (imu_ei.cpp), `[BTN]` (input_task).
5. Bateria + TP4056 (controle sem fio de verdade) e montagem final.

## ⚠️ Avisos que economizam horas

- **Módulo "forza3"** (`00:22:04:01:5E:1D`) é outro HC-06 do lab e é problemático — NÃO usar.
  Identificar módulo plugado: tentar conectar nos MACs conhecidos (pareados não aparecem
  como NEW no scan do bluetoothctl).
- **Replug da USB NÃO reinicia a Pico** se o Debug Probe estiver alimentando — usar BOOTSEL.
- Gravação: BOOTSEL + `cp build/main/pico_emb.uf2 /run/media/luis/RP2350/`.
- HC-06 ignora `AT+NAME`/`AT+PIN` (variante de firmware) — já está nomeado, não importa.
