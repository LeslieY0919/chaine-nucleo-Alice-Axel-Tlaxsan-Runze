#include "stm32f4xx_hal.h"
#include "slave.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define RX_SIZE 256u

typedef struct {
    volatile uint16_t head, tail;
    volatile bool overflow;
    volatile uint32_t last_tick;
    uint8_t bytes[RX_SIZE], current;
} RxQueue;

static UART_HandleTypeDef pc, up, down;
static RxQueue pc_rx, up_rx, down_rx;
static ProtocolReceiver_t up_receiver, down_receiver;
static Slave slave;

void SysTick_Handler(void) { HAL_IncTick(); }
void USART2_IRQHandler(void) { HAL_UART_IRQHandler(&pc); }
void UART5_IRQHandler(void) { HAL_UART_IRQHandler(&up); }
void USART3_IRQHandler(void) { HAL_UART_IRQHandler(&down); }

static RxQueue *queue_of(UART_HandleTypeDef *uart)
{
    return uart == &pc ? &pc_rx : uart == &up ? &up_rx : &down_rx;
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *uart)
{
    RxQueue *q = queue_of(uart);
    uint16_t next = (q->head + 1u) % RX_SIZE;
    if (next == q->tail) q->overflow = true;
    else { q->bytes[q->head] = q->current; q->head = next; }
    q->last_tick = HAL_GetTick();
    HAL_UART_Receive_IT(uart, &q->current, 1);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *uart)
{
    RxQueue *q = queue_of(uart);
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
    HAL_UART_Transmit(&pc, (uint8_t *)text, (uint16_t)strlen(text), 200);
    HAL_UART_Transmit(&pc, (uint8_t *)"\r\n", 2, 100);
}

static bool send_frame(SlaveLink link, const uint8_t *frame)
{
    if (link == LINK_DOWN && slave_is_last(&slave)) {
        static const char digits[] = "0123456789abcdef";
        char line[4 + 2 * PROTOCOL_FRAME_SIZE + 1] = "@DN ";
        for (unsigned i = 0; i < PROTOCOL_FRAME_SIZE; ++i) {
            line[4 + 2 * i] = digits[frame[i] >> 4];
            line[5 + 2 * i] = digits[frame[i] & 15];
        }
        line[4 + 2 * PROTOCOL_FRAME_SIZE] = 0;
        notice(line);
        return true;
    }
    UART_HandleTypeDef *uart = link == LINK_UP ? &up : &down;
    return HAL_UART_Transmit(uart, (uint8_t *)frame, PROTOCOL_FRAME_SIZE, 100) == HAL_OK;
}

static void dispatch(SlaveLink link, ProtocolResult_t result, const ProtocolMessage_t *m)
{
    if (result == PROTOCOL_OK) slave_receive(&slave, link, m, HAL_GetTick());
    else slave_bad_frame(&slave, link, result);
}

static void incoming(SlaveLink link, uint8_t byte)
{
    ProtocolReceiver_t *r = link == LINK_UP ? &up_receiver : &down_receiver;
    ProtocolMessage_t m;
    ProtocolResult_t result = protocol_receive(r, byte, slave_rx_key(&slave, link), &m);
    if (result == PROTOCOL_WAIT) return;
    if (link == LINK_DOWN && slave_is_last(&slave)) return;
    dispatch(link, result, &m);
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void from_pc_output(const char *text)
{
    uint8_t frame[PROTOCOL_FRAME_SIZE];
    if (!slave_is_last(&slave) || strlen(text) != 2 * PROTOCOL_FRAME_SIZE) {
        notice("REFUSE : /dn reserve au PC sortie de la derniere carte");
        return;
    }
    for (unsigned i = 0; i < sizeof frame; ++i) {
        int a = hex_digit(text[2 * i]), b = hex_digit(text[2 * i + 1]);
        if (a < 0 || b < 0) { notice("ERREUR : hexadecimal invalide"); return; }
        frame[i] = (uint8_t)(a * 16 + b);
    }
    ProtocolMessage_t m;
    dispatch(LINK_DOWN, protocol_decode(frame, slave_rx_key(&slave, LINK_DOWN), &m), &m);
}

static const char *position(void)
{
    if (slave.state == SLAVE_UNCONFIGURED) return "-";
    return slave_is_last(&slave) ? "DERNIERE" : "RELAIS";
}

static void command(char *line)
{
    if (!strcmp(line, "/help")) {
        notice("/status ; /reset (comme USER x3). Le texte se saisit sur le maitre.");
    } else if (!strcmp(line, "/status")) {
        char text[200];
        snprintf(text, sizeof text,
                 "ETAT=%s ROLE=ESCLAVE ID=%" PRIu32 "/%" PRIu32 " POSITION=%s"
                 " SESSION=%08" PRIx32 " CLE_AMONT=%08" PRIx32 " CLE_AVAL=%08" PRIx32
                 " CRC=%s MSG=%" PRIu32 " LED=%u",
                 slave_state_name(&slave), slave.id, slave.nodes, position(), slave.session,
                 slave.key_in, slave.key_out, slave.crc_error ? "ERREUR" : "OK", slave.messages,
                 HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_5) == GPIO_PIN_SET);
        notice(text);
    } else if (!strcmp(line, "/reset")) {
        slave_request_reset(&slave, HAL_GetTick());
    } else if (!strncmp(line, "/dn ", 4)) {
        from_pc_output(line + 4);
    } else if (*line) {
        notice("ERREUR : commande inconnue, voir /help");
    }
}

static void stop(void)
{
    __disable_irq();
    while (1) {}
}

static void uart_init(UART_HandleTypeDef *uart, USART_TypeDef *instance)
{
    uart->Instance = instance;
    uart->Init.BaudRate = 115200;
    uart->Init.WordLength = UART_WORDLENGTH_8B;
    uart->Init.StopBits = UART_STOPBITS_1;
    uart->Init.Parity = UART_PARITY_NONE;
    uart->Init.Mode = UART_MODE_TX_RX;
    uart->Init.HwFlowCtl = UART_HWCONTROL_NONE;
    uart->Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(uart) != HAL_OK) stop();
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
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_USART2_CLK_ENABLE();
    __HAL_RCC_USART3_CLK_ENABLE();
    __HAL_RCC_UART5_CLK_ENABLE();
    GPIO_InitTypeDef pins = {0};
    pins.Mode = GPIO_MODE_AF_PP;
    pins.Pull = GPIO_PULLUP;
    pins.Speed = GPIO_SPEED_FREQ_HIGH;
    pins.Pin = GPIO_PIN_2 | GPIO_PIN_3;
    pins.Alternate = GPIO_AF7_USART2;
    HAL_GPIO_Init(GPIOA, &pins);
    pins.Pin = GPIO_PIN_10 | GPIO_PIN_11;
    pins.Alternate = GPIO_AF7_USART3;
    HAL_GPIO_Init(GPIOC, &pins);
    pins.Pin = GPIO_PIN_12;
    pins.Alternate = GPIO_AF8_UART5;
    HAL_GPIO_Init(GPIOC, &pins);
    pins.Pin = GPIO_PIN_2;
    HAL_GPIO_Init(GPIOD, &pins);

    pins.Pin = GPIO_PIN_5;
    pins.Mode = GPIO_MODE_OUTPUT_PP;
    pins.Pull = GPIO_NOPULL;
    pins.Alternate = 0;
    HAL_GPIO_Init(GPIOA, &pins);
    pins.Pin = GPIO_PIN_13;
    pins.Mode = GPIO_MODE_INPUT;
    pins.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOC, &pins);

    uart_init(&pc, USART2);
    uart_init(&up, UART5);
    uart_init(&down, USART3);
    HAL_NVIC_SetPriority(USART2_IRQn, 2, 0);
    HAL_NVIC_SetPriority(UART5_IRQn, 1, 0);
    HAL_NVIC_SetPriority(USART3_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(USART2_IRQn);
    HAL_NVIC_EnableIRQ(UART5_IRQn);
    HAL_NVIC_EnableIRQ(USART3_IRQn);
    if (HAL_UART_Receive_IT(&pc, &pc_rx.current, 1) != HAL_OK
        || HAL_UART_Receive_IT(&up, &up_rx.current, 1) != HAL_OK
        || HAL_UART_Receive_IT(&down, &down_rx.current, 1) != HAL_OK) stop();
}

static void check_link(RxQueue *q, ProtocolReceiver_t *r, uint32_t now)
{
    if (q->overflow) {
        __disable_irq();
        q->tail = q->head;
        q->overflow = false;
        __enable_irq();
        r->used = 0;
        notice("ERREUR UART : octets perdus");
    }
    if (r->used && q->tail == q->head && (uint32_t)(now - q->last_tick) > 50) {
        r->used = 0;
        notice("ERREUR : trame UART incomplete");
    }
}

int main(void)
{
    hardware_init();
    slave_init(&slave, (SlaveIO){send_frame, notice});
    notice("ESCLAVE F446RE : attente de /init N sur le maitre ; /help");
    char line[128];
    unsigned used = 0;
    bool discard = false;
    while (1) {
        uint32_t now = HAL_GetTick();
        bool was_pressed = slave.button_stable;
        slave_tick(&slave, now, HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_13) == GPIO_PIN_RESET);
        if (slave.button_stable && !was_pressed && slave.presses) {
            char text[120];
            snprintf(text, sizeof text,
                     "USER : carte ESCLAVE ID %" PRIu32 "/%" PRIu32 " %s, etat %s, appui %u/3",
                     slave.id, slave.nodes, position(), slave_state_name(&slave), slave.presses);
            notice(text);
        }
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, slave_led(&slave, now) ? GPIO_PIN_SET : GPIO_PIN_RESET);
        check_link(&up_rx, &up_receiver, now);
        check_link(&down_rx, &down_receiver, now);
        if (pc_rx.overflow) {
            __disable_irq();
            pc_rx.tail = pc_rx.head;
            pc_rx.overflow = false;
            __enable_irq();
            used = 0;
            discard = true;
        }
        uint8_t byte;
        for (unsigned i = 0; i < RX_SIZE && pop(&up_rx, &byte); ++i) incoming(LINK_UP, byte);
        for (unsigned i = 0; i < RX_SIZE && pop(&down_rx, &byte); ++i) incoming(LINK_DOWN, byte);
        for (unsigned i = 0; i < RX_SIZE && pop(&pc_rx, &byte); ++i) {
            if (byte == '\r' || byte == '\n') {
                line[used] = 0;
                if (!discard && used) command(line);
                used = 0;
                discard = false;
            } else if (byte == 8 || byte == 127) {
                if (used) --used;
            } else if (byte < 32 || used == sizeof line - 1) discard = true;
            else if (!discard) line[used++] = (char)byte;
        }
    }
}
