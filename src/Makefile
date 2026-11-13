# OpenDMR bring-up (Baofeng DM-32UV, SoC HR_C7000 / CK803S), Baofeng bootloader layout (app at 0x0300C000).
#
#   make     -> build/opendmr.bin, written over the cable by the running firmware:
#               python ../tools/debug/fw_update.py
#               (or build/opendmr_BFUV32.bin in the stock update mode, PTT+SK1, with the stock
#               firmware loader)
#   make clean
#
# The toolchain (not in the repository, README "Building") in toolchain/MinGW/csky-elfabiv2-tools/bin, on the PATH or with CROSS=.

CROSS   ?= csky-elfabiv2-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump
SIZE    := $(CROSS)size

BUILD   := build
TARGET  := $(BUILD)/opendmr

CPU     := -mcpu=ck803s
# Our AMBE+2 vocoder lives in the research tree (vocoder/c/ambe) and is built from there
VOCODER := ../vocoder/c/ambe
# P25 (p25-core, the p25/ submodule): its air interface and IMBE frame layer, built from there
P25CORE := ../p25/c
P25_SRCS := $(P25CORE)/p25/p25_fec.c $(P25CORE)/p25/p25_frame.c $(P25CORE)/imbe/imbe_frame.c \
            $(P25CORE)/imbe/imbe_parms.c $(P25CORE)/imbe/imbe_dec.c $(P25CORE)/imbe/imbe_tables.c             $(P25CORE)/imbe/imbe_enc.c $(P25CORE)/imbe/imbe_ana.c $(P25CORE)/imbe/imbe_ana_tables.c
INCDIRS := $(VOCODER) $(P25CORE)/p25 $(P25CORE)/imbe config hal drivers dmr graphics graphics/fonts ui ui/text ui/widgets services app freertos/include freertos/portable/CK803
CFLAGS  := $(CPU) $(addprefix -I,$(INCDIRS)) -O2 -Wall -ffunction-sections -fdata-sections -MMD -MP
# Debug: every received AMBE frame to the data flash (services/rx_capture.h, read with tools/debug/rx_capture.py):
# make clean && make RX_CAPTURE=1. Off by default (9 KB of RAM, writes below the window)
ifeq ($(RX_CAPTURE),1)
CFLAGS  += -DRX_CAPTURE
endif
ASFLAGS := $(CPU) $(addprefix -I,$(INCDIRS)) -MMD -MP
LDFLAGS := $(CPU) -nostartfiles -Wl,--gc-sections -Wl,--undefined=vocoder_bench -Wl,--undefined=serial_update_marker \
           -Wl,--undefined=p25_phy_capture -Wl,--undefined=p25_phy_stop -Wl,--undefined=p25_phy_tx_test -Wl,--undefined=imbe_enc_test -Wl,-Map,$(TARGET).map -T ckcpu.ld
LIBS    := -lm -lc -lgcc

# Layers, see docs/architecture.md
SRCDIRS := startup freertos freertos/portable/CK803 hal drivers dmr graphics graphics/fonts ui ui/text ui/widgets services app
C_SRCS  := $(foreach d,$(SRCDIRS),$(wildcard $(d)/*.c))
S_SRCS  := $(foreach d,$(SRCDIRS),$(wildcard $(d)/*.S))
OBJS    := $(addprefix $(BUILD)/,$(C_SRCS:.c=.o) $(S_SRCS:.S=.o))            $(patsubst $(VOCODER)/%.c,$(BUILD)/vocoder/%.o,$(wildcard $(VOCODER)/*.c)) \
           $(patsubst $(P25CORE)/%.c,$(BUILD)/p25/%.o,$(P25_SRCS))

all: $(TARGET)_BFUV32.bin $(TARGET).elf.asm
	$(SIZE) $(TARGET).elf

$(TARGET).elf: $(OBJS) ckcpu.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@

# 256-byte header as the OpenDM32 releases: "BFUV32-V2", 0xFF padding, "END" at 0xFD (the loader skips it)
$(TARGET)_BFUV32.bin: $(TARGET).bin
	(printf 'BFUV32-V2'; head -c 244 /dev/zero | tr '\000' '\377'; printf 'END'; cat $<) > $@

$(TARGET).elf.asm: $(TARGET).elf
	$(OBJDUMP) -d $< > $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/vocoder/%.o: $(VOCODER)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DAMBE_PROFILE '-DAMBE_RAM_DATA=__attribute__((section(".vocoder_rodata")))' -c -o $@ $<

# the IMBE tables of the hot loops into the P25 overlay (ckcpu.ld .p25_overlay)
$(BUILD)/p25/%.o: $(P25CORE)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) '-DIMBE_RAM_DATA=__attribute__((section(".p25_rodata")))' -c -o $@ $<

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c -o $@ $<

# The firmware information shows the git version and the build time: built every time
GITVERSION := $(shell git rev-parse --short HEAD 2>/dev/null)
$(BUILD)/app/firmware_info.o $(BUILD)/services/cable_link.o: CFLAGS += $(if $(GITVERSION),-DGITVERSION=$(GITVERSION))
$(BUILD)/app/firmware_info.o: FORCE

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)

.PHONY: all clean FORCE
FORCE:
