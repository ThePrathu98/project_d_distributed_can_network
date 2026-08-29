#pragma once

#include <stdint.h>
#include "freertos/FreeRTOS.h"

/*
 * Project D - Gateway CAN HAL
 *
 * Purpose:
 *   Hide MCP2515/SPI/interrupt details from the application.
 *
 * Public API:
 *   can_hal_init()
 *   can_hal_send()
 *   can_hal_receive()
 *   can_hal_get_errors()
 *
 * RX architecture:
 *   MCP2515 INT -> GPIO2 ISR -> binary semaphore -> RX task
 *               -> SPI read -> software RX queue
 *
 * Important real-time rule:
 *   The ISR never performs SPI, logging, parsing, or blocking work.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define CAN_HAL_MAX_DATA_LEN 8U

typedef struct {
    uint16_t id;                        /* Standard 11-bit CAN identifier. */
    uint8_t dlc;                        /* Number of valid payload bytes: 0..8. */
    uint8_t data[CAN_HAL_MAX_DATA_LEN]; /* Classical CAN payload. */
} can_hal_frame_t;

/*
 * Controller + software health snapshot.
 *
 * eflg/tec/rec come from MCP2515 error/status registers.
 * The queue counters are important for the Day 1-3 stress test.
 */
typedef struct {
    uint8_t eflg;
    uint8_t tec;
    uint8_t rec;

    uint32_t rx_frames;
    uint32_t tx_frames;
    uint32_t rx_queue_high_water;
    uint32_t rx_queue_overflow;
} can_hal_errors_t;

/*
 * Initialize:
 *   - ESP8266 HSPI
 *   - MCP2515 @ 500 kbit/s
 *   - MCP2515 INT on D4/GPIO2
 *   - ISR/semaphore/RX task
 *   - software RX queue
 *
 * Current verified Gateway wiring:
 *   D5 / GPIO14 -> MCP2515 SCK
 *   D6 / GPIO12 -> MCP2515 MISO / SDO
 *   D7 / GPIO13 -> MCP2515 MOSI / SDI
 *   D8 / GPIO15 -> MCP2515 CS
 *   D4 / GPIO2  -> MCP2515 INT
 *
 * Returns 0 on success, negative on failure.
 */
int can_hal_init(void);

/* Send one standard 11-bit CAN frame. */
int can_hal_send(const can_hal_frame_t *frame);

/*
 * Receive one frame from the software RX queue.
 *
 * timeout_ticks:
 *   0              -> do not wait
 *   finite value   -> wait up to that many RTOS ticks
 *   portMAX_DELAY  -> wait indefinitely
 *
 * Returns:
 *   1  frame received
 *   0  timeout/no frame
 *  -1  invalid argument/internal error
 */
int can_hal_receive(can_hal_frame_t *frame, TickType_t timeout_ticks);

/*
 * Read MCP2515 error registers and software queue statistics.
 * Returns 0 on success, negative on failure.
 */
int can_hal_get_errors(can_hal_errors_t *errors);

#ifdef __cplusplus
}
#endif
