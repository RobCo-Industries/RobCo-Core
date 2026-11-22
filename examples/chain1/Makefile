# First stage of a two-stage chain. Validates the image at 0x00200000 and enters it.
# The sd variant (CHAIN1_SD) loads it from the SD card first.

ROBCO_CORE ?= ../..
IMAGE      := chain1
LINK_ADDR  := 0x8000
SRCS       := main.c
CONFIG_H   := robco_config.h

include $(ROBCO_CORE)/mk/robco.mk
