# A minimal project on RobCo-Core: an image, host tests and an SD card.
# The tool tests copy it outside the repository and set ROBCO_CORE.

ROBCO_CORE ?= ../../..
IMAGE      := sample
LINK_ADDR  := 0x8000
CONFIG_H   := src/robco_config.h
SRCS       := src/main.c src/greet.c

# Host tests: tests/host/test_*.c with the pure source src/greet.c.
HOST_TEST_DIR := tests/host
HOST_SRCS     := src/greet.c
HOST_INCLUDES := src
HOST_DEFINES  := SAMPLE_HOST=1

include $(ROBCO_CORE)/mk/robco.mk
include $(ROBCO_CORE)/mk/host.mk
