#include "can_hal.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "spi.h"

#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/*
 * Project D - Gateway MCP2515 CAN HAL
 *
 * Higher layers should not know MCP2515 register addresses or SPI details.
 *
 * Verified Gateway hardware:
 *   D5 / GPIO14 -> MCP2515 SCK
 *   D6 / GPIO12 -> MCP2515 MISO / SDO
 *   D7 / GPIO13 -> MCP2515 MOSI / SDI
 *   D8 / GPIO15 -> MCP2515 CS
 *   D4 / GPIO2  -> MCP2515 INT
 *
 * MCP2515 oscillator = 10 MHz
 * CAN bitrate        = 500 kbit/s
 */

static const char *TAG = "CAN_HAL";

/* MCP2515 SPI instructions */
#define MCP_RESET       0xC0U
#define MCP_READ        0x03U
#define MCP_WRITE       0x02U
#define MCP_RTS_TX0     0x81U

/* MCP2515 control/configuration registers */
#define MCP_CANSTAT     0x0EU
#define MCP_CANCTRL     0x0FU

#define MCP_TEC         0x1CU
#define MCP_REC         0x1DU

#define MCP_CNF3        0x28U
#define MCP_CNF2        0x29U
#define MCP_CNF1        0x2AU
#define MCP_CANINTE     0x2BU
#define MCP_CANINTF     0x2CU
#define MCP_EFLG        0x2DU

/* MCP2515 TX buffer 0 */
#define MCP_TXB0CTRL    0x30U
#define MCP_TXB0SIDH    0x31U
#define MCP_TXB0SIDL    0x32U
#define MCP_TXB0DLC     0x35U
#define MCP_TXB0D0      0x36U

/* MCP2515 RX buffer 0 */
#define MCP_RXB0CTRL    0x60U
#define MCP_RXB0SIDH    0x61U
#define MCP_RXB0SIDL    0x62U
#define MCP_RXB0DLC     0x65U
#define MCP_RXB0D0      0x66U

#define MCP_RX0IF       0x01U

/* MCP2515 operating modes */
#define MCP_MODE_CONFIG 0x80U
#define MCP_MODE_NORMAL 0x00U

/* ESP8266 / FreeRTOS configuration */
#define MCP_INT_GPIO        GPIO_NUM_2
#define CAN_RX_QUEUE_LEN    16U
#define CAN_RX_TASK_STACK   2048U
#define CAN_RX_TASK_PRIO    5U

static SemaphoreHandle_t s_rx_irq_sem = NULL;
static SemaphoreHandle_t s_spi_mutex = NULL;
static QueueHandle_t s_rx_queue = NULL;

static volatile uint32_t s_rx_queue_overflow = 0;
static volatile uint32_t s_rx_queue_high_water = 0;
static volatile uint32_t s_rx_frames = 0;
static volatile uint32_t s_tx_frames = 0;

/* -------------------------------------------------------------------------- */
/* Low-level SPI helpers                                                      */
/* -------------------------------------------------------------------------- */

/*
 * Send one MCP2515 single-byte instruction.
 *
 * ESP8266 RTOS SDK v3.4 exposes command/address/MOSI/MISO as separate phases.
 * For an 8-bit command, pass the command unshifted and set bits.cmd = 8.
 */
static esp_err_t mcp_spi_command(uint8_t instruction)
{
    spi_trans_t trans;
    uint16_t cmd = instruction;

    memset(&trans, 0, sizeof(trans));

    trans.bits.val = 0;
    trans.cmd = &cmd;
    trans.bits.cmd = 8;

    return spi_trans(HSPI_HOST, &trans);
}

/*
 * Write one MCP2515 register.
 *
 * Wire format:
 *   0x02 -> register address -> data byte
 *
 * This packing matches the already-proven Saleae bring-up.
 */
static esp_err_t mcp_write_reg(uint8_t reg, uint8_t value)
{
    spi_trans_t trans;
    uint16_t cmd = MCP_WRITE;
    uint32_t addr = ((uint32_t)reg << 24);
    uint32_t data = value;

    memset(&trans, 0, sizeof(trans));

    trans.bits.val = 0;
    trans.cmd = &cmd;
    trans.addr = &addr;
    trans.mosi = &data;

    trans.bits.cmd = 8;
    trans.bits.addr = 8;
    trans.bits.mosi = 8;

    return spi_trans(HSPI_HOST, &trans);
}

/*
 * Read one MCP2515 register.
 *
 * Wire format:
 *   0x03 -> register address -> returned byte
 */
static uint8_t mcp_read_reg(uint8_t reg)
{
    spi_trans_t trans;
    uint16_t cmd = MCP_READ;
    uint32_t addr = ((uint32_t)reg << 24);
    uint32_t rx = 0;

    memset(&trans, 0, sizeof(trans));

    trans.bits.val = 0;
    trans.cmd = &cmd;
    trans.addr = &addr;
    trans.miso = &rx;

    trans.bits.cmd = 8;
    trans.bits.addr = 8;
    trans.bits.miso = 8;

    if (spi_trans(HSPI_HOST, &trans) != ESP_OK) {
        return 0xFFU;
    }

    return (uint8_t)(rx & 0xFFU);
}

/* -------------------------------------------------------------------------- */
/* MCP2515 controller helpers                                                 */
/* -------------------------------------------------------------------------- */

static void mcp_reset(void)
{
    (void)mcp_spi_command(MCP_RESET);

    /*
     * Allow the controller to settle before reading CANSTAT/CANCTRL.
     */
    vTaskDelay(pdMS_TO_TICKS(20));
}

/*
 * Configure the already-proven 10 MHz MCP2515 for 500 kbit/s.
 *
 * Values proven during bring-up:
 *   CNF1 = 0x00
 *   CNF2 = 0xA1
 *   CNF3 = 0x01
 *
 * Record these exact values in docs/network_design.md.
 */
static int mcp_init_controller(void)
{
    uint8_t canstat;
    uint8_t canctrl;

    mcp_reset();

    canstat = mcp_read_reg(MCP_CANSTAT);
    canctrl = mcp_read_reg(MCP_CANCTRL);

    ESP_LOGI(TAG, "CANSTAT after reset = 0x%02X", canstat);
    ESP_LOGI(TAG, "CANCTRL after reset = 0x%02X", canctrl);

    if ((canstat & 0xE0U) != MCP_MODE_CONFIG) {
        ESP_LOGE(TAG, "MCP2515 did not enter CONFIG mode");
        return -1;
    }

    if (mcp_write_reg(MCP_CNF1, 0x00U) != ESP_OK ||
        mcp_write_reg(MCP_CNF2, 0xA1U) != ESP_OK ||
        mcp_write_reg(MCP_CNF3, 0x01U) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to program MCP2515 bit timing");
        return -1;
    }

    /*
     * RXB0CTRL = 0x60 accepts all valid CAN frames during bring-up.
     * Later we can tighten acceptance filtering after the DBC is frozen.
     */
    if (mcp_write_reg(MCP_RXB0CTRL, 0x60U) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure RX buffer 0");
        return -1;
    }

    /* Clear stale flags and enable RX buffer 0 interrupt only. */
    if (mcp_write_reg(MCP_CANINTF, 0x00U) != ESP_OK ||
        mcp_write_reg(MCP_CANINTE, MCP_RX0IF) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure MCP2515 interrupts");
        return -1;
    }

    if (mcp_write_reg(MCP_CANCTRL, MCP_MODE_NORMAL) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to request NORMAL mode");
        return -1;
    }

    vTaskDelay(pdMS_TO_TICKS(10));

    canstat = mcp_read_reg(MCP_CANSTAT);

    if ((canstat & 0xE0U) != MCP_MODE_NORMAL) {
        ESP_LOGE(TAG, "MCP2515 failed to enter NORMAL mode: CANSTAT=0x%02X",
                 canstat);
        return -1;
    }

    ESP_LOGI(TAG, "MCP2515 NORMAL mode OK, CAN=500 kbit/s");

    return 0;
}

/*
 * Read one standard 11-bit frame from RX buffer 0.
 *
 * Caller must already own s_spi_mutex.
 *
 * Return:
 *   1 = frame read
 *   0 = no RX0 frame pending
 *  -1 = error
 */
static int mcp_read_rx0_locked(can_hal_frame_t *frame)
{
    uint8_t intf;
    uint8_t sidh;
    uint8_t sidl;
    uint8_t dlc;
    uint8_t i;

    if (frame == NULL) {
        return -1;
    }

    intf = mcp_read_reg(MCP_CANINTF);

    if ((intf & MCP_RX0IF) == 0U) {
        return 0;
    }

    sidh = mcp_read_reg(MCP_RXB0SIDH);
    sidl = mcp_read_reg(MCP_RXB0SIDL);
    dlc = (uint8_t)(mcp_read_reg(MCP_RXB0DLC) & 0x0FU);

    /*
     * Standard 11-bit identifier reconstruction:
     *   SIDH      = ID[10:3]
     *   SIDL[7:5] = ID[2:0]
     */
    frame->id = (uint16_t)(((uint16_t)sidh << 3) |
                           ((uint16_t)sidl >> 5));

    if (dlc > CAN_HAL_MAX_DATA_LEN) {
        dlc = CAN_HAL_MAX_DATA_LEN;
    }

    frame->dlc = dlc;
    memset(frame->data, 0, sizeof(frame->data));

    for (i = 0; i < dlc; ++i) {
        frame->data[i] = mcp_read_reg((uint8_t)(MCP_RXB0D0 + i));
    }

    /*
     * Clear RX0IF only after the complete frame is copied.
     * This allows MCP2515 INT to return HIGH when no interrupt remains.
     */
    if (mcp_write_reg(MCP_CANINTF,
                      (uint8_t)(intf & (uint8_t)~MCP_RX0IF)) != ESP_OK) {
        return -1;
    }

    return 1;
}

/* Caller must own s_spi_mutex. */
static int mcp_send_locked(const can_hal_frame_t *frame)
{
    uint8_t i;

    if (frame == NULL ||
        frame->dlc > CAN_HAL_MAX_DATA_LEN ||
        frame->id > 0x7FFU) {
        return -1;
    }

    if (mcp_write_reg(MCP_TXB0CTRL, 0x00U) != ESP_OK ||
        mcp_write_reg(MCP_TXB0SIDH, (uint8_t)(frame->id >> 3)) != ESP_OK ||
        mcp_write_reg(MCP_TXB0SIDL,
                      (uint8_t)((frame->id & 0x07U) << 5)) != ESP_OK ||
        mcp_write_reg(MCP_TXB0DLC, frame->dlc) != ESP_OK) {
        return -1;
    }

    for (i = 0; i < frame->dlc; ++i) {
        if (mcp_write_reg((uint8_t)(MCP_TXB0D0 + i),
                          frame->data[i]) != ESP_OK) {
            return -1;
        }
    }

    if (mcp_spi_command(MCP_RTS_TX0) != ESP_OK) {
        return -1;
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
/* Interrupt -> semaphore -> task -> queue                                    */
/* -------------------------------------------------------------------------- */

/*
 * MCP2515 INT ISR.
 *
 * Keep this ISR tiny:
 *   - no SPI
 *   - no logging
 *   - no CAN parsing
 *   - no blocking calls
 *
 * It only wakes the RX worker task.
 */
static void IRAM_ATTR mcp_int_isr(void *arg)
{
    BaseType_t higher_priority_task_woken = pdFALSE;

    (void)arg;

    if (s_rx_irq_sem != NULL) {
        xSemaphoreGiveFromISR(s_rx_irq_sem, &higher_priority_task_woken);
    }

    if (higher_priority_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/*
 * Hardware-facing RX worker.
 *
 * It wakes because of MCP2515 INT, performs the slower SPI work in task
 * context, then puts completed frames into a FreeRTOS queue.
 */
static void can_rx_worker_task(void *arg)
{
    can_hal_frame_t frame;

    (void)arg;

    ESP_LOGI(TAG, "RX worker waiting for MCP2515 INT");

    while (1) {
        if (xSemaphoreTake(s_rx_irq_sem, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /*
         * Serialize complete MCP2515 register sequences against TX/error
         * operations. This prevents two tasks from using HSPI concurrently.
         */
        if (xSemaphoreTake(s_spi_mutex, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        while (mcp_read_rx0_locked(&frame) > 0) {
            UBaseType_t queued;

            if (xQueueSend(s_rx_queue, &frame, 0) != pdTRUE) {
                /*
                 * Do not block the hardware-drain task.
                 * Count the overflow so the 10-minute stress test can detect it.
                 */
                s_rx_queue_overflow++;
            } else {
                s_rx_frames++;

                queued = uxQueueMessagesWaiting(s_rx_queue);

                if ((uint32_t)queued > s_rx_queue_high_water) {
                    s_rx_queue_high_water = (uint32_t)queued;
                }
            }
        }

        xSemaphoreGive(s_spi_mutex);
    }
}

static int int_gpio_init(void)
{
    gpio_config_t io_conf;
    esp_err_t ret;

    memset(&io_conf, 0, sizeof(io_conf));

    io_conf.pin_bit_mask = (1ULL << MCP_INT_GPIO);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;

    /* MCP2515 INT is active-low: trigger on HIGH -> LOW. */
    io_conf.intr_type = GPIO_INTR_NEGEDGE;

    if (gpio_config(&io_conf) != ESP_OK) {
        ESP_LOGE(TAG, "GPIO2 INT configuration failed");
        return -1;
    }

    ret = gpio_install_isr_service(0);

    /*
     * ESP_ERR_INVALID_STATE is acceptable if another component already
     * installed the ISR service.
     */
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %d", ret);
        return -1;
    }

    if (gpio_isr_handler_add(MCP_INT_GPIO, mcp_int_isr, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register MCP2515 GPIO2 ISR");
        return -1;
    }

    ESP_LOGI(TAG, "MCP2515 INT configured on D4/GPIO2");

    return 0;
}

/* -------------------------------------------------------------------------- */
/* Public CAN HAL API                                                         */
/* -------------------------------------------------------------------------- */

int can_hal_init(void)
{
    spi_config_t spi_config = {
        .interface = {
            .val = SPI_DEFAULT_INTERFACE
        },
        .intr_enable = {
            .val = SPI_MASTER_DEFAULT_INTR_ENABLE
        },
        .event_cb = NULL,
        .mode = SPI_MASTER_MODE,
        .clk_div = SPI_2MHz_DIV,
    };

    /*
     * Create RTOS objects before enabling the GPIO interrupt so the ISR can
     * never run with an uninitialized semaphore/queue.
     */
    s_rx_irq_sem = xSemaphoreCreateBinary();
    s_spi_mutex = xSemaphoreCreateMutex();
    s_rx_queue = xQueueCreate(CAN_RX_QUEUE_LEN, sizeof(can_hal_frame_t));

    if (s_rx_irq_sem == NULL || s_spi_mutex == NULL || s_rx_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create CAN HAL RTOS objects");
        return -1;
    }

    if (spi_init(HSPI_HOST, &spi_config) != ESP_OK) {
        ESP_LOGE(TAG, "ESP8266 HSPI initialization failed");
        return -1;
    }

    if (int_gpio_init() != 0) {
        return -1;
    }

    /*
     * No worker task is running yet, so controller initialization can use SPI
     * directly before the mutex needs to arbitrate access.
     */
    if (mcp_init_controller() != 0) {
        return -1;
    }

    if (xTaskCreate(can_rx_worker_task,
                    "can_rx_worker",
                    CAN_RX_TASK_STACK,
                    NULL,
                    CAN_RX_TASK_PRIO,
                    NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create CAN RX worker task");
        return -1;
    }

    ESP_LOGI(TAG, "CAN HAL initialized");

    return 0;
}

int can_hal_send(const can_hal_frame_t *frame)
{
    int ret;

    if (frame == NULL || s_spi_mutex == NULL) {
        return -1;
    }

    if (xSemaphoreTake(s_spi_mutex, portMAX_DELAY) != pdTRUE) {
        return -1;
    }

    ret = mcp_send_locked(frame);

    xSemaphoreGive(s_spi_mutex);

    if (ret == 0) {
        s_tx_frames++;
    }

    return ret;
}

int can_hal_receive(can_hal_frame_t *frame, TickType_t timeout_ticks)
{
    if (frame == NULL || s_rx_queue == NULL) {
        return -1;
    }

    if (xQueueReceive(s_rx_queue, frame, timeout_ticks) == pdTRUE) {
        return 1;
    }

    return 0;
}

int can_hal_get_errors(can_hal_errors_t *errors)
{
    if (errors == NULL || s_spi_mutex == NULL) {
        return -1;
    }

    if (xSemaphoreTake(s_spi_mutex, portMAX_DELAY) != pdTRUE) {
        return -1;
    }

    errors->eflg = mcp_read_reg(MCP_EFLG);
    errors->tec = mcp_read_reg(MCP_TEC);
    errors->rec = mcp_read_reg(MCP_REC);

    xSemaphoreGive(s_spi_mutex);

    errors->rx_frames = s_rx_frames;
    errors->tx_frames = s_tx_frames;
    errors->rx_queue_high_water = s_rx_queue_high_water;
    errors->rx_queue_overflow = s_rx_queue_overflow;

    return 0;
}
