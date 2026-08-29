# Project D Gateway application component - ESP8266 RTOS SDK v3.4
#
# gateway_main.c uses the public CAN HAL API, so declare can_hal as a
# required component. This adds can_hal/include to this component's include
# path and ensures the HAL is linked into the Gateway firmware.

set(COMPONENT_SRCS "gateway_main.c")
set(COMPONENT_ADD_INCLUDEDIRS ".")
set(COMPONENT_REQUIRES can_hal pthread)

register_component()