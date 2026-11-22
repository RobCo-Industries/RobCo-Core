# Every component in one image, the way a kernel uses them. The build/menu
# variant adds the SD card, a configuration file, input, audio and the clock.

ROBCO_CORE ?= ../..
IMAGE      := demo
LINK_ADDR  := 0x8000
SRCS       := main.c menu.c
CONFIG_H   := robco_config.h

include $(ROBCO_CORE)/mk/robco.mk
