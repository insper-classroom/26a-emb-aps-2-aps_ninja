#!/usr/bin/env python3
"""
Teste de feedback do controle SUBWAY-CTRL.

Conecta no HC-06 por Bluetooth SPP (RFCOMM, socket nativo do Linux — sem
PyBluez) e manda quadros PC->Controle pra disparar vibracao, LED RGB e
eventos de jogo. Serve pra validar o caminho de recepcao (bt_rx_task) e os
atuadores (haptic_task).

Protocolo (ver main/protocol.h):
  PC -> Controle: 0x55 | TYPE | LEN | PAYLOAD[LEN] | CHK
  CHK = XOR(TYPE, LEN, PAYLOAD...)

Uso:
  python3 subway_test.py [MAC] [canal]
  (default: MAC do SUBWAY-CTRL, canal RFCOMM 1)
"""
import socket
import sys
import time

MAC_DEFAULT = "00:22:09:01:6A:92"   # SUBWAY-CTRL
CHANNEL_DEFAULT = 1                  # SPP do HC-06 costuma ser canal 1

SYNC_RX        = 0x55
MSG_GAME_EVENT = 0x10
MSG_HAPTIC     = 0x11
MSG_LED        = 0x12
GE_DIED, GE_COIN, GE_POWERUP = 1, 2, 3


def build(type_, payload=()):
    payload = bytes(payload)
    chk = type_ ^ len(payload)
    for b in payload:
        chk ^= b
    return bytes([SYNC_RX, type_, len(payload)]) + payload + bytes([chk])


def main():
    mac = sys.argv[1] if len(sys.argv) > 1 else MAC_DEFAULT
    chan = int(sys.argv[2]) if len(sys.argv) > 2 else CHANNEL_DEFAULT

    print(f"Conectando em {mac} (canal RFCOMM {chan})...")
    s = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_STREAM,
                      socket.BTPROTO_RFCOMM)
    s.settimeout(15)
    s.connect((mac, chan))
    print("Conectado! Disparando feedback (olha o controle):\n")

    def send(nome, frame, espera=1.2):
        print(f"  -> {nome:32s} {frame.hex(' ')}")
        s.send(frame)
        time.sleep(espera)

    # Vibracao pura via MSG_HAPTIC: payload [intensidade(0-255), duracao/10ms]
    send("HAPTIC forte (255, ~300ms)",  build(MSG_HAPTIC, [255, 30]))
    send("HAPTIC fraco (120, ~120ms)",  build(MSG_HAPTIC, [120, 12]))

    # Eventos de jogo (cada um tem seu padrao de vibra+LED no haptic_task)
    send("COIN (tapinha curto)",        build(MSG_GAME_EVENT, [GE_COIN]))
    send("POWERUP (azul + vibra)",      build(MSG_GAME_EVENT, [GE_POWERUP]), 1.6)
    send("DIED (vermelho + vibra forte)", build(MSG_GAME_EVENT, [GE_DIED]), 1.6)

    # LED RGB direto (testa o azul tambem)
    send("LED azul",                    build(MSG_LED, [0, 0, 255]))
    send("LED verde",                   build(MSG_LED, [0, 255, 0]))
    send("LED apaga",                   build(MSG_LED, [0, 0, 0]), 0.2)

    s.close()
    print("\nPronto! Se vibrou e os LEDs mudaram, recepcao BT + atuadores OK.")


if __name__ == "__main__":
    main()
