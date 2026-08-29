Gateway CAN HAL - first migration step

Place these files here:

firmware/gateway_ecu/components/can_hal/

Expected structure:

can_hal/
├── CMakeLists.txt
├── component.mk
├── can_hal.c
└── include/
    └── can_hal.h

Public API:
- can_hal_init()
- can_hal_send()
- can_hal_receive()
- can_hal_get_errors()

RX path:
MCP2515 INT -> GPIO2 ISR -> semaphore -> RX worker -> SPI -> FreeRTOS queue

Do NOT modify project_d_can_bringup now. Keep it as the proven hardware
reference until the Gateway HAL migration is re-verified with PCAN + Saleae.
