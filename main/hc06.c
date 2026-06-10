#include "hc06.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"

#define HC06_TIMEOUT_RESPOSTA_US 1200
#define HC06_ESPERA_AT_MS        500
#define HC06_ESPERA_CMD_MS       1500
#define HC06_TENTATIVAS_POR_BAUD 1

static void hc06_limpar_rx(void) {
    while (uart_is_readable_within_us(HC06_UART_ID, HC06_TIMEOUT_RESPOSTA_US))
        uart_getc(HC06_UART_ID);
}

static int hc06_ler_resposta(char *buf, size_t len) {
    int i = 0;
    while (uart_is_readable_within_us(HC06_UART_ID, HC06_TIMEOUT_RESPOSTA_US) && i < (int)(len - 1)) {
        buf[i++] = uart_getc(HC06_UART_ID);
    }
    buf[i] = '\0';
    return i;
}

bool hc06_check_connection() {
    char str[64];
    uart_puts(HC06_UART_ID, "AT");
    sleep_ms(HC06_ESPERA_AT_MS);
    int n = hc06_ler_resposta(str, sizeof(str));

    // DIAGNOSTICO: mostra exatamente o que (se algo) o HC-06 respondeu
    printf("  [AT] %d byte(s): \"", n);
    for (int i = 0; i < n; i++) putchar((str[i] >= 32 && str[i] < 127) ? str[i] : '.');
    printf("\"  hex:");
    for (int i = 0; i < n; i++) printf(" %02X", (uint8_t)str[i]);
    printf("\n");

    return strstr(str, "OK") != NULL;
}

// Envia um comando AT, espera a resposta e diz se ela contem "OK". Imprime a
// resposta crua p/ diagnostico. Limpa o RX antes p/ nao misturar respostas.
static bool hc06_cmd_ok(const char *cmd, const char *rotulo) {
    char str[64];
    hc06_limpar_rx();
    uart_puts(HC06_UART_ID, cmd);
    sleep_ms(HC06_ESPERA_CMD_MS);
    int n = hc06_ler_resposta(str, sizeof(str));
    printf("  [%s] %d byte(s): \"", rotulo, n);
    for (int i = 0; i < n; i++) putchar((str[i] >= 32 && str[i] < 127) ? str[i] : '.');
    printf("\"\n");
    return strstr(str, "OK") != NULL;
}

// HC-06 tem duas familias de firmware com sintaxes diferentes:
//   ANTIGO (linvor):  AT+NAME<nome>      -> "OKsetname"   (sem CR/LF)
//   NOVO:             AT+NAME=<nome>\r\n -> "OK"
// Tenta a antiga e, se falhar, a nova. Aceita qualquer resposta com "OK".
bool hc06_set_name(char name[]) {
    char cmd[40];
    snprintf(cmd, sizeof(cmd), "AT+NAME%s", name);
    if (hc06_cmd_ok(cmd, "NAME-velho")) return true;
    snprintf(cmd, sizeof(cmd), "AT+NAME=%s\r\n", name);
    return hc06_cmd_ok(cmd, "NAME-novo");
}

// PIN tambem muda entre as familias:
//   ANTIGO:  AT+PIN<pin>          -> "OKsetPIN"
//   NOVO:    AT+PSWD=<pin>\r\n    (alguns exigem aspas) -> "OK"
bool hc06_set_pin(char pin[]) {
    char cmd[40];
    snprintf(cmd, sizeof(cmd), "AT+PIN%s", pin);
    if (hc06_cmd_ok(cmd, "PIN-velho")) return true;
    snprintf(cmd, sizeof(cmd), "AT+PSWD=%s\r\n", pin);
    if (hc06_cmd_ok(cmd, "PSWD-novo")) return true;
    snprintf(cmd, sizeof(cmd), "AT+PSWD=\"%s\"\r\n", pin);
    return hc06_cmd_ok(cmd, "PSWD-aspas");
}

bool hc06_set_baud_115200() {
    char str[64];
    uart_puts(HC06_UART_ID, "AT+BAUD8");
    sleep_ms(HC06_ESPERA_AT_MS);
    hc06_ler_resposta(str, sizeof(str));
    return strstr(str, "OK115200") != NULL;
}

bool hc06_set_at_mode(int on) {
    gpio_init(HC06_ENABLE_PIN);
    gpio_set_dir(HC06_ENABLE_PIN, GPIO_OUT);
    gpio_put(HC06_ENABLE_PIN, on);
    return true;
}

static bool hc06_tentar_baud(uint baud) {
    printf("Tentando baud = %u...\n", baud);
    uart_set_baudrate(HC06_UART_ID, baud);
    sleep_ms(HC06_ESPERA_AT_MS);
    for (int i = 0; i < HC06_TENTATIVAS_POR_BAUD; i++) {
        if (hc06_check_connection()) return true;
        printf("baud = %u nao respondendo\n", baud);
        sleep_ms(HC06_ESPERA_AT_MS);
    }
    return false;
}

bool hc06_config(char name[], char pin[]) {
    hc06_set_at_mode(1);

    // Varre os bauds mais comuns ate o HC-06 responder. O baud que responder
    // vira o baud de operacao: o HC-06 usa o MESMO baud p/ os comandos AT e
    // p/ os dados SPP, e a UART ja fica setada nesse baud por hc06_tentar_baud.
    const unsigned bauds[] = {9600, 38400, 115200, 57600, 19200, 4800};
    unsigned baud_ok = 0;
    for (unsigned i = 0; i < sizeof(bauds) / sizeof(bauds[0]); i++) {
        if (hc06_tentar_baud(bauds[i])) { baud_ok = bauds[i]; break; }
    }

    if (baud_ok == 0) {
        printf("ERRO: HC-06 nao respondeu em NENHUM baud. Seguindo sem Bluetooth.\n");
        hc06_set_at_mode(0);
        return false;   // nao trava o boot: o resto do firmware continua
    }

    printf("Conectado em %u baud!\n\n", baud_ok);

    // NAO forcamos 115200: operamos no baud detectado (eventos do controle sao
    // pequenos, 38400 sobra). Forcar 115200 era a causa do loop infinito que
    // prendia o modulo em modo AT e o deixava invisivel no scan.

    // Nome e PIN: tenta algumas vezes, mas NAO trava se falhar. O essencial e
    // sempre chegar no hc06_set_at_mode(0) p/ o modulo ficar descobrivel.
    printf("Configurando nome...\n");
    bool name_ok = false;
    for (int i = 0; i < 2 && !(name_ok = hc06_set_name(name)); i++) {
        printf("set name falhou (tentativa %d)\n", i + 1);
        sleep_ms(HC06_ESPERA_AT_MS);
    }
    printf(name_ok ? "Nome OK\n\n" : "Nome NAO setado (segue assim mesmo)\n\n");

    printf("Configurando PIN...\n");
    bool pin_ok = false;
    for (int i = 0; i < 2 && !(pin_ok = hc06_set_pin(pin)); i++) {
        printf("set pin falhou (tentativa %d)\n", i + 1);
        sleep_ms(HC06_ESPERA_AT_MS);
    }
    printf(pin_ok ? "PIN OK\n\n" : "PIN NAO setado (segue assim mesmo)\n\n");

    printf("HC-06 configurado (baud=%u). Saindo do modo AT.\n", baud_ok);
    hc06_set_at_mode(0);
    return true;
}
