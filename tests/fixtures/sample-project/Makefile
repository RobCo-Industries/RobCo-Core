# A minimal project on RobCo-Core: an image, host tests and an SD card.
# The tool tests copy it outside the repository and set ROBCO_CORE.

ROBCO_CORE ?= ../../..
IMAGE      := sample
LINK_ADDR  := 0x8000
CONFIG_H   := src/robco_config.h
BUILD      ?= build
SRCS       := src/main.c src/greet.c

# Host tests: tests/host/test_*.c with the pure source src/greet.c.
HOST_TEST_DIR := tests/host
HOST_SRCS     := src/greet.c
HOST_INCLUDES := src
HOST_DEFINES  := SAMPLE_HOST=1

# `make sdimage`: the image and a data file, then a stamp from the hook.
# Set before the include: the fragment reads them as prerequisites.
SDIMAGE_MIB   := 8
SDIMAGE_FILES := $(BUILD)/$(IMAGE).img:SAMPLE.IMG data/hello.txt:DATA/HELLO.TXT

include $(ROBCO_CORE)/mk/robco.mk
include $(ROBCO_CORE)/mk/host.mk

# Runs after the files above are copied, before the card is packed.
sdimage-stage::
	mkdir -p $(SDIMAGE_STAGE)/DATA
	printf 'staged by %s\n' $(IMAGE) > $(SDIMAGE_STAGE)/DATA/STAMP.TXT
