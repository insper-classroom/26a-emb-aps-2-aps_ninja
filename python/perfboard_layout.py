#!/usr/bin/env python3
"""
Gera um diagrama da montagem do controle SUBWAY-CTRL numa perfboard 32x25
(furos isolados). Saida: perfboard_layout.png

Convencao: coluna 1..32 (x, esquerda->direita), linha 1..25 (y, cima->baixo).
"""
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, Rectangle, Circle

COLS, ROWS = 32, 25

def xy(col, row):
    """(coluna, linha) -> coordenadas do plot (linha 1 no topo)."""
    return col, (ROWS + 1 - row)

fig, ax = plt.subplots(figsize=(16, 12))

# ---- furos da perfboard ----
for c in range(1, COLS + 1):
    for r in range(1, ROWS + 1):
        x, y = xy(c, r)
        ax.add_patch(Circle((x, y), 0.10, color="#cccccc", zorder=1))

# ---- trilhos de alimentacao (linha 1 = 3V3, linha 2 = GND) ----
y31 = xy(1, 1)[1]
y_gnd = xy(1, 2)[1]
ax.plot([1, COLS], [y31, y31], color="#d11", lw=4, zorder=2)
ax.plot([1, COLS], [y_gnd, y_gnd], color="#222", lw=4, zorder=2)
ax.text(COLS + 0.3, y31, "3V3", color="#d11", va="center", fontweight="bold")
ax.text(COLS + 0.3, y_gnd, "GND", color="#222", va="center", fontweight="bold")

# ---- Pico (vertical, USB no topo). Pinos esq=col8, dir=col15, linhas 3..22 ----
PL, PR = 8, 15          # colunas dos pinos esquerda/direita
PTOP, PBOT = 3, 22      # linhas
x0, ytop = xy(PL - 0.6, PTOP - 0.8)
x1, ybot = xy(PR + 0.6, PBOT + 0.8)
ax.add_patch(FancyBboxPatch((x0, ybot), x1 - x0, ytop - ybot,
             boxstyle="round,pad=0.1", fc="#1b5e20", ec="#0b3d12",
             alpha=0.9, zorder=3))
ax.text((x0 + x1) / 2, xy(1, PTOP - 1.5)[1], "PICO 2  (USB ↑)",
        color="white", ha="center", fontweight="bold", fontsize=13, zorder=5)

# Mapa: linha -> (sinal_esq, sinal_dir). So rotulo os relevantes.
left = {3:"GP0",4:"GP1",5:"GND",6:"GP2",7:"GP3",8:"GP4",9:"GP5",10:"GND",
        11:"GP6",12:"GP7",13:"GP8",14:"GP9",15:"GND",16:"GP10",17:"GP11",
        18:"GP12",19:"GP13",20:"GND",21:"GP14",22:"GP15"}
right = {3:"VBUS",4:"VSYS",5:"GND",6:"3V3EN",7:"3V3",8:"VREF",9:"GP28",
         10:"AGND",11:"GP27",12:"GP26",13:"RUN",14:"GP22",15:"GND",
         16:"GP21",17:"GP20",18:"GP19",19:"GP18",20:"GND",21:"GP17",22:"GP16"}

def pin_xy(side, row):
    return xy(PL if side == "L" else PR, row)

for row in range(PTOP, PBOT + 1):
    for side, table, dx in (("L", left, -0.45), ("R", right, 0.45)):
        x, y = pin_xy(side, row)
        ax.add_patch(Circle((x, y), 0.13, color="#ffd54a", zorder=4))
        lbl = table[row]
        ax.text(x + dx, y, lbl, fontsize=6.5, va="center",
                ha="right" if side == "L" else "left", color="white", zorder=5)

# ---- modulos: (nome, col, row, w, h, cor) caixa centrada aprox ----
def box(name, c, r, w, h, color):
    x, y = xy(c, r)
    ax.add_patch(Rectangle((x - 0.0, y - h), w, h, fc=color, ec="#333",
                 alpha=0.85, zorder=3))
    ax.text(x + w / 2, y - h / 2, name, ha="center", va="center",
            fontsize=8, fontweight="bold", zorder=5)

# Esquerda
box("HC-06", 1.5, 6, 4.5, 4.5, "#64b5f6")
box("RGB\n+3 res", 1.5, 12, 4.5, 3.0, "#ba68c8")
box("LED\nstatus", 1.5, 17.5, 4.5, 2.0, "#fff176")
box("OLED", 1.0, 22, 5.0, 3.5, "#4dd0e1")
# Direita
box("Bateria\n(divisor)", 17, 10, 4.0, 2.5, "#aed581")
box("MOTOR\nBC337+diodo", 17, 15.5, 6.0, 3.5, "#ff8a65")
box("BOTOES\nST SP MA PR", 24, 19.5, 7.0, 4.0, "#90a4ae")
box("IMU\nMPU6050", 24, 24, 6.0, 3.0, "#f06292")

# ---- fios: (pino_lado, linha) -> (col_destino, row_destino), cor ----
def wire(side, row, dc, dr, color):
    x0, y0 = pin_xy(side, row)
    x1, y1 = xy(dc, dr)
    ax.plot([x0, x1], [y0, y1], color=color, lw=1.6, alpha=0.8, zorder=2)

W = {
    "hc06": "#1565c0", "rgb": "#8e24aa", "status": "#f9a825",
    "oled": "#00838f", "batt": "#558b2f", "motor": "#d84315",
    "btn": "#455a64", "imu": "#c2185b",
}
# HC-06 (esq)
for row, c, r in [(6,4,7),(7,4,8),(8,4,9),(9,4,10)]:
    wire("L", row, c, r, W["hc06"])
# RGB (esq) GP7/8/9
for row in (12, 13, 14):
    wire("L", row, 4, 13, W["rgb"])
# LED status GP11
wire("L", 17, 4, 17.5, W["status"])
# OLED GP14/15
for row in (21, 22):
    wire("L", row, 3.5, 21, W["oled"])
# Bateria GP28 (dir)
wire("R", 9, 19, 9.5, W["batt"])
# Motor GP22 (dir)
wire("R", 14, 20, 14, W["motor"])
# Botoes GP18/19/20/21 (dir)
for row in (16, 17, 18, 19):
    wire("R", row, 27, 20, W["btn"])
# IMU GP16/17 (dir)
for row in (21, 22):
    wire("R", row, 27, 22.5, W["imu"])

# ---- legenda ----
from matplotlib.lines import Line2D
leg = [
    Line2D([0],[0], color=W["hc06"], lw=3, label="HC-06 (GP2-5)"),
    Line2D([0],[0], color=W["oled"], lw=3, label="OLED (GP14/15)"),
    Line2D([0],[0], color=W["rgb"], lw=3, label="RGB (GP7/8/9)"),
    Line2D([0],[0], color=W["status"], lw=3, label="LED status (GP11)"),
    Line2D([0],[0], color=W["btn"], lw=3, label="Botoes (GP18-21)"),
    Line2D([0],[0], color=W["motor"], lw=3, label="Motor (GP22)"),
    Line2D([0],[0], color=W["batt"], lw=3, label="Bateria (GP28)"),
    Line2D([0],[0], color=W["imu"], lw=3, label="IMU (GP16/17)"),
    Line2D([0],[0], color="#d11", lw=3, label="Trilho 3V3 (linha 1)"),
    Line2D([0],[0], color="#222", lw=3, label="Trilho GND (linha 2)"),
]
ax.legend(handles=leg, loc="lower left", bbox_to_anchor=(0.0, -0.16),
          ncol=3, fontsize=9, frameon=True)

# ---- eixos / grade de referencia ----
ax.set_xlim(0, COLS + 3)
ax.set_ylim(-0.5, ROWS + 1.5)
ax.set_xticks(range(1, COLS + 1))
ax.set_yticks([xy(1, r)[1] for r in range(1, ROWS + 1)])
ax.set_yticklabels(range(1, ROWS + 1))
ax.set_xlabel("coluna")
ax.set_ylabel("linha")
ax.set_aspect("equal")
ax.set_title("Controle SUBWAY-CTRL  -  layout perfboard 32x25  (fios = ligacoes a fazer)",
             fontsize=13, fontweight="bold")
ax.grid(True, color="#eee", lw=0.5)

plt.tight_layout()
out = __file__.rsplit("/", 1)[0] + "/perfboard_layout.png"
plt.savefig(out, dpi=110, bbox_inches="tight")
print("salvo em", out)
