#!/usr/bin/env python3
"""
SUBWAY-CTRL -> Subway Surfers: o "driver" do controle no PC.

Conecta no HC-06 por Bluetooth SPP (RFCOMM, socket nativo do Linux), lê os
quadros Controle->PC e converte em teclas do jogo:

  GESTO  up/down/left/right  ->  setas (inclinacao do controle, sem IA)
  GESTO  especial (UPDOWN)   ->  espaco (gesto sobe-e-desce, via IA)
  BOTAO  de acao (unico)     ->  espaco (inicia o jogo / ativa o poder)
  BATERIA                    ->  mostrada no terminal

E manda feedback PC->Controle pelo mesmo socket (haptico + LED):

  tecla 'd' + Enter  ->  GAME_EVENT died    (vibra forte + LED vermelho)
  tecla 'c' + Enter  ->  GAME_EVENT coin    (tapinha)
  tecla 'p' + Enter  ->  GAME_EVENT powerup (azul + vibra)
  tecla 'q' + Enter  ->  sair

(Deteccao automatica de "game over" na tela fica como melhoria; por enquanto
o evento e disparado manualmente pelas teclas acima.)

Protocolo (ver main/protocol.h):
  Controle -> PC: 0xAA | TYPE | LEN | PAYLOAD[LEN] | CHK
  PC -> Controle: 0x55 | TYPE | LEN | PAYLOAD[LEN] | CHK
  CHK = XOR(TYPE, LEN, PAYLOAD...)

Uso no LINUX (socket RFCOMM nativo):
  pip install pynput
  python3 subway_play.py [MAC] [canal]

  Dica (Wayland): a injecao de teclas usa XTEST (X11). Se o jogo estiver num
  browser Wayland-nativo, rode o browser com --ozone-platform=x11 e jogue no
  https://poki.com/ . Antes de conectar: bluetoothctl disconnect <MAC>
  (o HC-06 so aceita 1 conexao por vez).

Uso no WINDOWS (porta COM via pyserial):
  pip install pynput pyserial
  1. Pareie o SUBWAY-CTRL em Configuracoes > Bluetooth (PIN 1234).
  2. Em "Mais configuracoes de Bluetooth" > aba "Portas COM", anote a porta
     de SAIDA (Outgoing), ex.: COM5.
  3. python subway_play.py COM5
  (rodado sem argumento no Windows, o script lista as portas COM e sai)
"""
import os
import socket
import sys
import threading
import time

MAC_DEFAULT = "00:22:09:01:6A:92"   # SUBWAY-CTRL
CHANNEL_DEFAULT = 1                  # SPP do HC-06 costuma ser canal 1

# --- protocolo (espelha main/protocol.h) ---
SYNC_TX        = 0xAA   # Controle -> PC
SYNC_RX        = 0x55   # PC -> Controle
MSG_GESTURE    = 0x01
MSG_BUTTON     = 0x02
MSG_BATTERY    = 0x03
MSG_STATUS     = 0x04
MSG_GAME_EVENT = 0x10
MSG_HAPTIC     = 0x11
MSG_LED        = 0x12

# 1-4: movimento por inclinacao (Fusion/APS7, sem IA) -> setas
# 5:   gesto UPDOWN reconhecido pela IA -> espaco (especial/start)
GESTOS = {0: None, 1: "up", 2: "down", 3: "left", 4: "right", 5: "space"}
GE_DIED, GE_COIN, GE_POWERUP = 1, 2, 3

MAX_PAYLOAD = 8


def checksum(type_, payload):
    chk = type_ ^ len(payload)
    for b in payload:
        chk ^= b
    return chk


def build(type_, payload=()):
    payload = bytes(payload)
    return bytes([SYNC_RX, type_, len(payload)]) + payload + \
        bytes([checksum(type_, payload)])


# ---------------------------------------------------------------------------
# Transporte: socket RFCOMM nativo (Linux) ou porta COM do HC-06 (Windows).
# Os dois expoem recv(n) / send(data) / close(), entao o resto do script
# nao precisa saber em qual sistema esta rodando.
# ---------------------------------------------------------------------------
class RfcommLink:
    def __init__(self, mac, channel):
        self.sock = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_STREAM,
                                  socket.BTPROTO_RFCOMM)
        self.sock.settimeout(15)
        self.sock.connect((mac, channel))
        self.sock.settimeout(None)

    def recv(self, n):
        return self.sock.recv(n)

    def send(self, data):
        self.sock.send(data)

    def close(self):
        self.sock.close()


class SerialLink:
    def __init__(self, port):
        import serial  # pip install pyserial
        # O baud aqui e ignorado (porta COM Bluetooth e virtual), mas 9600
        # espelha o baud real do HC-06.
        self.ser = serial.Serial(port, 9600, timeout=None)

    def recv(self, n):
        data = self.ser.read(1)              # bloqueia ate chegar 1 byte
        extra = self.ser.in_waiting
        if extra:
            data += self.ser.read(min(extra, n - 1))
        return data

    def send(self, data):
        self.ser.write(data)

    def close(self):
        self.ser.close()


# ---------------------------------------------------------------------------
# Teclado: pynput injeta as setas no jogo
# ---------------------------------------------------------------------------
try:
    from pynput.keyboard import Controller, Key
    _kb = Controller()
    _KEYS = {"up": Key.up, "down": Key.down, "left": Key.left,
             "right": Key.right, "space": Key.space}

    def press(name):
        key = _KEYS[name]
        _kb.press(key)
        _kb.release(key)
except Exception as e:  # sem display / sem pynput: roda "as cegas" p/ debug
    print(f"[AVISO] teclado indisponivel ({e}) -- so mostrando eventos")

    def press(name):
        pass


# ---------------------------------------------------------------------------
# RX: maquina de estados do protocolo (igual a bt_rx_task do firmware)
# ---------------------------------------------------------------------------
def handle_frame(type_, payload):
    if type_ == MSG_GESTURE and len(payload) >= 2:
        gesto, conf = GESTOS.get(payload[0]), payload[1]
        if gesto == "space":
            press(gesto)
            print(f"  ESPECIAL UPDOWN ({conf}%) -> espaco")
        elif gesto:
            press(gesto)
            print(f"  GESTO {gesto.upper():5s} ({conf}%) -> seta")
        else:
            print(f"  GESTO idle ({conf}%)")
    elif type_ == MSG_BUTTON and len(payload) >= 2:
        edge = payload[1]
        if edge == 1:                        # botao unico de acao
            press("space")
            print("  BOTAO -> espaco (start/poder)")
    elif type_ == MSG_BATTERY and len(payload) >= 1:
        print(f"  BATERIA {payload[0]}%")
    elif type_ == MSG_STATUS and len(payload) >= 1:
        print(f"  STATUS flags=0x{payload[0]:02X}")


def rx_loop(link):
    WAIT_SYNC, GET_TYPE, GET_LEN, GET_PAYLOAD, GET_CHK = range(5)
    st, type_, length, payload = WAIT_SYNC, 0, 0, bytearray()

    while True:
        data = link.recv(64)
        if not data:
            print("Conexao encerrada pelo controle.")
            return
        for b in data:
            if st == WAIT_SYNC:
                st = GET_TYPE if b == SYNC_TX else WAIT_SYNC
            elif st == GET_TYPE:
                type_, st = b, GET_LEN
            elif st == GET_LEN:
                length, payload = b, bytearray()
                st = WAIT_SYNC if b > MAX_PAYLOAD else (GET_CHK if b == 0
                                                        else GET_PAYLOAD)
            elif st == GET_PAYLOAD:
                payload.append(b)
                if len(payload) >= length:
                    st = GET_CHK
            elif st == GET_CHK:
                if b == checksum(type_, payload):
                    handle_frame(type_, bytes(payload))
                st = WAIT_SYNC


# ---------------------------------------------------------------------------
# TX: eventos de jogo manuais pelo terminal (d/c/p) ate ter deteccao na tela
# ---------------------------------------------------------------------------
def tx_loop(link):
    eventos = {"d": ("DIED", GE_DIED), "c": ("COIN", GE_COIN),
               "p": ("POWERUP", GE_POWERUP)}
    print("Comandos: d=died  c=coin  p=powerup  q=sair  (+ Enter)")
    for line in sys.stdin:
        cmd = line.strip().lower()
        if cmd == "q":
            link.close()
            return
        if cmd in eventos:
            nome, ev = eventos[cmd]
            link.send(build(MSG_GAME_EVENT, [ev]))
            print(f"  -> GAME_EVENT {nome}")


def main():
    target = sys.argv[1] if len(sys.argv) > 1 else None

    if target and target.upper().startswith("COM"):
        # Windows: porta COM de SAIDA do HC-06 pareado
        print(f"Conectando na porta {target}...")
        link = SerialLink(target)
    elif os.name == "nt":
        # Windows sem argumento: lista as portas pra pessoa achar a certa
        print("No Windows, passe a porta COM de SAIDA do SUBWAY-CTRL:")
        print("  python subway_play.py COM5\n")
        try:
            from serial.tools import list_ports
            print("Portas COM encontradas:")
            for p in list_ports.comports():
                print(f"  {p.device}  {p.description}")
        except ImportError:
            print("(instale as dependencias:  pip install pynput pyserial)")
        return
    else:
        # Linux: socket RFCOMM nativo
        mac = target or MAC_DEFAULT
        chan = int(sys.argv[2]) if len(sys.argv) > 2 else CHANNEL_DEFAULT
        print(f"Conectando em {mac} (canal RFCOMM {chan})...")
        link = RfcommLink(mac, chan)

    print("Conectado! Abra o Subway Surfers e deixe a janela do jogo em foco.\n")

    t = threading.Thread(target=tx_loop, args=(link,), daemon=True)
    t.start()

    try:
        rx_loop(link)
    except OSError:
        pass  # conexao fechada pelo 'q'
    print("Tchau!")


if __name__ == "__main__":
    main()
