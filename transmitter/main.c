#include "stm32f4xx_hal.h"
#include "master.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define RX_SIZE 256u

typedef struct {
    volatile uint16_t head, tail;
    volatile bool overflow;
    uint8_t bytes[RX_SIZE], current;
} RxQueue;

static UART_HandleTypeDef pc, link;
static RxQueue pc_rx, link_rx;
static Master master;
static ProtocolReceiver_t receiver;
static bool test_mode;
static uint32_t last_link_byte;

void SysTick_Handler(void) { HAL_IncTick(); }
void USART2_IRQHandler(void) { HAL_UART_IRQHandler(&pc); }
void USART3_IRQHandler(void) { HAL_UART_IRQHandler(&link); }

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *uart)
{
    RxQueue *q = uart == &pc ? &pc_rx : &link_rx;
    uint16_t next = (q->head + 1u) % RX_SIZE;
    if (next == q->tail) q->overflow = true;
    else { q->bytes[q->head] = q->current; q->head = next; }
    HAL_UART_Receive_IT(uart, &q->current, 1);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *uart)
{
    RxQueue *q = uart == &pc ? &pc_rx : &link_rx;
    q->overflow = true;
    if (uart->RxState == HAL_UART_STATE_READY)
        HAL_UART_Receive_IT(uart, &q->current, 1);
}

static bool pop(RxQueue *q, uint8_t *byte)
{
    if (q->tail == q->head) return false;
    *byte = q->bytes[q->tail];
    q->tail = (q->tail + 1u) % RX_SIZE;
    return true;
}

static void notice(const char *text)
{
    HAL_UART_Transmit(&pc, (uint8_t *)text, (uint16_t)strlen(text), 100);
    HAL_UART_Transmit(&pc, (uint8_t *)"\r\n", 2, 100);
}

static bool send_frame(const uint8_t *frame)
{
    if (test_mode) {
        static const char hex[] = "0123456789abcdef";
        char line[5 + 2 * PROTOCOL_FRAME_SIZE + 1] = "@TX ";
        for (unsigned i = 0; i < PROTOCOL_FRAME_SIZE; ++i) {
            line[4 + 2 * i] = hex[frame[i] >> 4];
            line[5 + 2 * i] = hex[frame[i] & 15];
        }
        line[4 + 2 * PROTOCOL_FRAME_SIZE] = 0;
        notice(line);
        return true;
    }
    return HAL_UART_Transmit(&link, (uint8_t *)frame, PROTOCOL_FRAME_SIZE, 100) == HAL_OK;
}

static uint32_t random32(void)
{
    static uint32_t state = 0x6d2b79f5;
    state ^= DWT->CYCCNT ^ HAL_GetTick() ^ HAL_GetUIDw0() ^ HAL_GetUIDw1();
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state ? state : 1;
}

static void incoming(uint8_t byte, uint32_t now)
{
    ProtocolMessage_t message;
    last_link_byte = now;
    ProtocolResult_t result = protocol_receive(&receiver, byte, master.keys[0], &message);
    if (result == PROTOCOL_OK) master_receive(&master, &message, now);
    else if (result == PROTOCOL_BAD_CRC) master_bad_crc(&master);
    else if (result == PROTOCOL_BAD_FORMAT) notice("ERREUR : format de trame invalide");
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void command(char *line, uint32_t now)
{
    if (!strcmp(line, "/help")) {
        notice("/init N : N cartes esclaves (1..16) ; /reset ; /status");
        notice("Texte : 1..16 octets par ligne. USER : 3 appuis en 2 secondes.");
        notice("/test on|off : simulation sur PC, sans liaison physique");
    } else if (!strcmp(line, "/status")) {
        char status[128];
        snprintf(status, sizeof status, "ETAT=%s MODE=%s CRC=%s USER=%u LED=%u",
                 master_state_name(&master), test_mode ? "SIMULATION" : "USART3",
                 master.crc_error ? "ERREUR" : "OK",
                 HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_13) == GPIO_PIN_RESET,
                 HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_5) == GPIO_PIN_SET);
        notice(status);
    } else if (!strncmp(line, "/init ", 6)) {
        char *end;
        unsigned long nodes = strtoul(line + 6, &end, 10);
        if (*end || end == line + 6 || nodes < 1 || nodes > MASTER_MAX_NODES)
            notice("ERREUR : /init N, avec N entre 1 et 16");
        else master_start(&master, (uint8_t)nodes, now);
    } else if (!strcmp(line, "/reset")) {
        receiver.used = 0;
        master_reset(&master, now);
    } else if (!strcmp(line, "/test on") || !strcmp(line, "/test off")) {
        test_mode = !strcmp(line, "/test on");
        master_init(&master, (MasterIO){send_frame, notice, random32});
        receiver.used = 0;
        notice(test_mode ? "MODE SIMULATION : recepteur joue par le PC"
                         : "MODE USART3 : attente de /init N");
    } else if (!strncmp(line, "/rx ", 4) && test_mode) {
        if (strlen(line + 4) != 2 * PROTOCOL_FRAME_SIZE) {
            notice("ERREUR : /rx attend 26 octets hexadecimaux");
            return;
        }
        uint8_t frame[PROTOCOL_FRAME_SIZE];
        for (unsigned i = 0; i < sizeof frame; ++i) {
            int a = hex_digit(line[4 + 2 * i]), b = hex_digit(line[5 + 2 * i]);
            if (a < 0 || b < 0) { notice("ERREUR : hexadecimal invalide"); return; }
            frame[i] = (uint8_t)(a * 16 + b);
        }
        for (unsigned i = 0; i < sizeof frame; ++i) incoming(frame[i], now);
    } else if (line[0] == '/') notice("ERREUR : commande inconnue, voir /help");
    else if (strlen(line) > MAX_PAYLOAD_SIZE) notice("REFUSE : maximum 16 octets par ligne");
    else if (*line) master_text(&master, (uint8_t *)line, (uint8_t)strlen(line), now);
}

static void stop(void)
{
    __disable_irq();
    while (1) {}
}

static void hardware_init(void)
{
    HAL_Init();
    RCC_OscInitTypeDef osc = {0};
    osc.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    osc.HSIState = RCC_HSI_ON;
    osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    osc.PLL.PLLState = RCC_PLL_NONE;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) stop();
    RCC_ClkInitTypeDef clock = {0};
    clock.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK
                    | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clock.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
    clock.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clock.APB1CLKDivider = RCC_HCLK_DIV1;
    clock.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&clock, FLASH_LATENCY_0) != HAL_OK) stop();

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_USART2_CLK_ENABLE();
    __HAL_RCC_USART3_CLK_ENABLE();
    GPIO_InitTypeDef pins = {0};
    pins.Pin = GPIO_PIN_2 | GPIO_PIN_3;
    pins.Mode = GPIO_MODE_AF_PP;
    pins.Pull = GPIO_PULLUP;
    pins.Speed = GPIO_SPEED_FREQ_HIGH;
    pins.Alternate = GPIO_AF7_USART2;
    HAL_GPIO_Init(GPIOA, &pins);
    pins.Pin = GPIO_PIN_10 | GPIO_PIN_11;
    pins.Alternate = GPIO_AF7_USART3;
    HAL_GPIO_Init(GPIOC, &pins);

    pins.Pin = GPIO_PIN_5;
    pins.Mode = GPIO_MODE_OUTPUT_PP;
    pins.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &pins);
    pins.Pin = GPIO_PIN_13;
    pins.Mode = GPIO_MODE_INPUT;
    pins.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOC, &pins);

    pc.Instance = USART2;
    pc.Init.BaudRate = 115200;
    pc.Init.WordLength = UART_WORDLENGTH_8B;
    pc.Init.StopBits = UART_STOPBITS_1;
    pc.Init.Parity = UART_PARITY_NONE;
    pc.Init.Mode = UART_MODE_TX_RX;
    pc.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    pc.Init.OverSampling = UART_OVERSAMPLING_16;
    link = pc;
    link.Instance = USART3;
    if (HAL_UART_Init(&pc) != HAL_OK || HAL_UART_Init(&link) != HAL_OK) stop();
    HAL_NVIC_SetPriority(USART2_IRQn, 2, 0);
    HAL_NVIC_SetPriority(USART3_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(USART2_IRQn);
    HAL_NVIC_EnableIRQ(USART3_IRQn);
    if (HAL_UART_Receive_IT(&pc, &pc_rx.current, 1) != HAL_OK
        || HAL_UART_Receive_IT(&link, &link_rx.current, 1) != HAL_OK) stop();
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

int main(void)
{
    hardware_init();
    master_init(&master, (MasterIO){send_frame, notice, random32});
    notice("MAITRE F446RE : /help ; en attente de /init N");
    char line[128];
    unsigned used = 0;
    bool discard = false;
    while (1) {
        uint32_t now = HAL_GetTick();
        bool was_pressed = master.button_stable;
        master_tick(&master, now, HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_13) == GPIO_PIN_RESET);
        if (master.button_stable && !was_pressed && master.presses) {
            char text[96];
            snprintf(text, sizeof text, "USER : carte MAITRE, etat %s, appui %u/3 (3 en 2 s = RESET)",
                     master_state_name(&master), master.presses);
            notice(text);
        }
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, master_led(&master, now) ? GPIO_PIN_SET : GPIO_PIN_RESET);
        if (pc_rx.overflow || link_rx.overflow) {
            __disable_irq();
            pc_rx.tail = pc_rx.head;
            link_rx.tail = link_rx.head;
            pc_rx.overflow = link_rx.overflow = false;
            __enable_irq();
            used = 0;
            discard = true;
            receiver.used = 0;
            master_link_error(&master);
        }
        if (receiver.used && (uint32_t)(now - last_link_byte) > 50) {
            receiver.used = 0;
            notice("ERREUR : trame UART incomplete");
        }
        uint8_t byte;
        for (unsigned i = 0; i < RX_SIZE && pop(&link_rx, &byte); ++i)
            if (!test_mode) incoming(byte, HAL_GetTick());
        for (unsigned i = 0; i < RX_SIZE && pop(&pc_rx, &byte); ++i) {
            if (byte == '\r' || byte == '\n') {
                line[used] = 0;
                if (!discard && used) command(line, HAL_GetTick());
                else if (discard) notice("REFUSE : ligne invalide ou trop longue");
                used = 0;
                discard = false;
            } else if (byte == 8 || byte == 127) {
                if (used) --used;
            } else if (byte < 32 || used == sizeof line - 1) discard = true;
            else if (!discard) line[used++] = (char)byte;
        }
    }
}
