# Makefile - armclang (Arm Compiler 6) command-line build of the E2CF gateway
#
# Same compiler/linker as the Keil MDK project (e2cf_mcxe31.uvprojx), so a CI
# box without Keil can build and link the exact image. Usage:
#   make AC6=/opt/ArmCompilerforEmbedded6.24   (default below)
#
# SPDX-License-Identifier: BSD-3-Clause

AC6 ?= /opt/ArmCompilerforEmbedded6.24
CC := $(AC6)/bin/armclang
LD := $(AC6)/bin/armlink
FROMELF := $(AC6)/bin/fromelf

BUILD := build
TARGET := $(BUILD)/e2cf_mcxe31

CPUFLAGS := --target=arm-arm-none-eabi -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard

DEFINES := -DDEBUG -DFLEXCAN_WAIT_TIMEOUT=1000 -DSDK_DEBUGCONSOLE=1 \
           -DMCUX_META_BUILD -DMCUXPRESSO_SDK -DCPU_MCXE31BMPB -DMCXE31B_SERIES \
           -DBOOT_HEADER_ENABLE=1

INCLUDES := -Iboard -Isource -ICMSIS -ICMSIS/m-profile -Iboot_header \
            -Icomponent/uart -Icomponent/phy -Icomponent/phy/device/phylan8741 \
            -Icomponent/silicon_id -Icomponent/silicon_id/socs \
            -Icomponent/silicon_id/socs/mcxe31x \
            -Idevice -Idevice/periph1 -Idrivers \
            -Iutilities -Iutilities/debug_console_lite -Iutilities/str

CFLAGS := $(CPUFLAGS) $(DEFINES) $(INCLUDES) -std=gnu99 -O1 -g \
          -include source/mcux_config.h -include source/mcuxsdk_version.h \
          -ffunction-sections -fdata-sections -fno-common -fshort-enums -fshort-wchar \
          -Wall -Wno-padded -Wno-missing-field-initializers

SCATTER := MCXE31B/mdk/MCXE31B_flash.scf
LDFLAGS := --cpu=Cortex-M7.fp.dp --scatter=$(SCATTER) \
           --map --xref --callgraph --symbols --info sizes --info totals \
           --strict --summary_stderr --info summarysizes \
           --diag_suppress=6314 --remove --entry=Reset_Handler \
           --predefine="-DBOOT_HEADER_ENABLE=1"

SRCS := \
    board/board.c board/clock_config.c board/hardware_init.c board/pin_mux.c \
    boot_header/boot_header.c \
    component/phy/device/phylan8741/fsl_phylan8741.c \
    component/silicon_id/fsl_silicon_id.c \
    component/silicon_id/socs/mcxe31x/fsl_silicon_id_soc.c \
    component/uart/fsl_adapter_lpuart.c \
    device/system_MCXE31B.c \
    drivers/fsl_cache.c drivers/fsl_clock.c drivers/fsl_common.c \
    drivers/fsl_common_arm.c drivers/fsl_enet_qos.c drivers/fsl_flexcan.c \
    drivers/fsl_lpuart.c drivers/fsl_siul2.c \
    startup/startup_MCXE31B.c \
    utilities/debug_console_lite/fsl_debug_console.c \
    utilities/fsl_assert.c utilities/str/fsl_str.c \
    source/can_hw.c source/dbg_log.c source/e2cf_core.c source/eth_raw.c \
    source/gw_prof.c source/gw_time.c source/main.c

OBJS := $(addprefix $(BUILD)/,$(SRCS:.c=.o))

# SDK startup file uses GCC-only "#pragma GCC optimize" that armclang ignores
$(BUILD)/startup/startup_MCXE31B.o: CFLAGS += -Wno-unknown-pragmas

.PHONY: all clean
all: $(TARGET).axf $(TARGET).bin

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET).axf: $(OBJS) $(SCATTER)
	$(LD) $(LDFLAGS) $(OBJS) -o $@ --list=$(TARGET).map

$(TARGET).bin: $(TARGET).axf
	$(FROMELF) --bin --output=$@ $<

clean:
	rm -rf $(BUILD)
