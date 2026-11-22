# Framebuffer console example, driven by one-letter UART commands.
#
# The built-in font is Fixedsys Excelsior. The spleen variant in VARIANTS
# builds with Spleen instead. FONT_C=/path/to/font8x16.c builds with a
# table from tools/font2c.py in place of either. See docs/FONTS.md.

ROBCO_CORE ?= ../..
IMAGE      := console
LINK_ADDR  := 0x8000
SRCS       := main.c
CONFIG_H   := robco_config.h
BUILD      ?= build

# The fragment compiles sources relative to this directory, so the table
# is copied into the build directory first.
ifneq ($(strip $(FONT_C)),)
FONT_COPY := $(BUILD)/font/font8x16_user.c
SRCS      += $(FONT_COPY)
endif

include $(ROBCO_CORE)/mk/robco.mk

ifneq ($(strip $(FONT_C)),)
# Checked on every build so a different FONT_C with the same BUILD is seen.
$(FONT_COPY): FORCE
	@mkdir -p $(dir $@)
	@cmp -s $(FONT_C) $@ || cp $(FONT_C) $@

.PHONY: FORCE
FORCE:
endif
