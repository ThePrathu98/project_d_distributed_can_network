#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "can_hal.h"

/*
 * Project D - Gateway ECU application
 *
 * This file intentionally contains NO MCP2515 register access and NO direct
 * SPI transactions. All hardware-specific CAN work belongs in can_hal.
 *
 * Current migration test:
 *
 *   Gateway -> PCAN:
 *     CAN ID 0x100
 *     DLC    8
 *     DATA   01 02 03 04 05 06 07 08
 *     Period ~500 ms
 *
 *   PCAN -> Gateway:
 *     CAN ID 0x101
 *     DLC    8
 *     DATA   AA BB CC DD 11 22 33 44
 *
 * RX path inside the HAL:
 *   MCP2515 INT -> GPIO ISR -> semaphore -> RX worker -> SPI -> RX queue
 *
 * Application path here:
 *   gateway_rx_task() -> can_hal_receive() -> print received frame
 */

static const char *TAG = "GATEWAY";

static void log_can_frame(const char *prefix, const can_hal_frame_t *frame)
{
    uint8_t i;

    if (prefix == NULL || frame == NULL) {
        return;
    }

    printf("%s ID=0x%03X DLC=%u DATA=",
           prefix,
           (unsigned int)frame->id,
           (unsigned int)frame->dlc);

    for (i = 0; i < frame->dlc; ++i) {
        printf("%02X", frame->data[i]);

        if ((i + 1U) < frame->dlc) {
            printf(" ");
        }
    }

    printf("\n");
}

static void gateway_rx_task(void *arg)
{
    can_hal_frame_t frame;

    (void)arg;

    ESP_LOGI(TAG, "Gateway RX application task started");

    while (1) {
        int ret = can_hal_receive(&frame, portMAX_DELAY);

        if (ret == 1) {
            log_can_frame("RX", &frame);
        } else if (ret < 0) {
            ESP_LOGE(TAG, "can_hal_receive() failed");
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

void app_main(void)
{
    const can_hal_frame_t tx_frame = {
        .id = 0x100U,
        .dlc = 8U,
        .data = {0x01U, 0x02U, 0x03U, 0x04U,
                 0x05U, 0x06U, 0x07U, 0x08U}
    };

    uint32_t tx_count = 0U;

    printf("\n");
    printf("========================================\n");
    printf("Project D Gateway - CAN HAL migration test\n");
    printf("========================================\n");

    if (can_hal_init() != 0) {
        ESP_LOGE(TAG, "CAN HAL initialization failed");
        return;
    }

    ESP_LOGI(TAG, "CAN HAL initialization PASS");

    if (xTaskCreate(gateway_rx_task,
                    "gateway_rx",
                    2048,
                    NULL,
                    4,
                    NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Gateway RX task");
        return;
    }

    ESP_LOGI(TAG,
             "TX test: ID=0x100 DLC=8 DATA=01 02 03 04 05 06 07 08 every ~500 ms");
    ESP_LOGI(TAG,
             "RX test: manually send PCAN ID=0x101 DATA=AA BB CC DD 11 22 33 44");

    while (1) {
        can_hal_errors_t health;

        if (can_hal_send(&tx_frame) == 0) {
            tx_count++;
            log_can_frame("TX", &tx_frame);
        } else {
            ESP_LOGE(TAG, "CAN TX failed");
        }

        if ((tx_count % 10U) == 0U) {
            memset(&health, 0, sizeof(health));

            if (can_hal_get_errors(&health) == 0) {
                ESP_LOGI(TAG,
                         "CAN HEALTH EFLG=0x%02X TEC=%u REC=%u "
                         "TX=%u RX=%u RX_Q_HIGH=%u RX_Q_OVF=%u",
                         health.eflg,
                         (unsigned int)health.tec,
                         (unsigned int)health.rec,
                         (unsigned int)health.tx_frames,
                         (unsigned int)health.rx_frames,
                         (unsigned int)health.rx_queue_high_water,
                         (unsigned int)health.rx_queue_overflow);
            } else {
                ESP_LOGE(TAG, "can_hal_get_errors() failed");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}