# ESP8266 RTOS SDK v3.4 component definition.
#
# Keep component.mk too because this SDK generation supports the legacy
# component build flow as well.

set(COMPONENT_SRCS "can_hal.c")
set(COMPONENT_ADD_INCLUDEDIRS "include")

register_component()
