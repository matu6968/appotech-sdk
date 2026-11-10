PREFIX ?= $(CURDIR)/toolchain/install
CROSS ?= $(PREFIX)/bin/or1k-appotech-elf-
CC := $(CROSS)gcc
AR := $(CROSS)ar
OBJDUMP := $(CROSS)objdump
SIZE := $(CROSS)size
BOARD ?= action-vision
BOARDS := action-vision silvergear-3222882
ifeq ($(filter $(BOARD),$(BOARDS)),)
$(error BOARD must be one of: $(BOARDS))
endif
# USB stack: legacy = src/usb.c (polled vendor-class debug device); tinyusb = TinyUSB with src/tusb/dcd_apx.c (see src/tusb/).
# TinyUSB is the default for both boards. silvergear-3222882: cold-booted from SPI on real hardware (2026-11-08, production and trace images).
# action-vision: made the default on 2026-11-10 after extensive RAM-launch testing (debug-link soak, mass storage, UVC, UAC, SD, hot-pull; see
# research/action-vision-compat.md); NO TinyUSB image has been cold-booted from SPI on the Action yet, so that first install is the untested step
# (the PHY bring-up is the legacy sequence copied, and rom-rescue recovers a camera that does not enumerate). USB_STACK=legacy still builds the old driver.
# The stack that is not the board's default builds in its own directory (suffix -legacy / -tinyusb) so the two never share objects.
DEFAULT_USB_STACK := tinyusb
USB_STACK ?= $(DEFAULT_USB_STACK)
ifeq ($(filter $(USB_STACK),legacy tinyusb),)
$(error USB_STACK must be legacy or tinyusb)
endif
# USB modes beyond the always-present debug link are opt-in per example: an example lists the ones it needs in examples/<name>/features.mk
# (e.g. APX_USB_MSC ?= 1), and an image carries none of the code of a mode it does not use. Mass storage (TinyUSB MSC class +
# src/tusb/msc_disk.c) costs about 9 KB of extra RAM, which printer-camera-demo on the Silvergear does not have (it overflowed EXTRA_RAM by
# 4.9 KB with it). The command line still wins: make EXAMPLE=... APX_USB_MSC=1.
-include examples/$(EXAMPLE)/features.mk
APX_USB_MSC ?= 0
ifeq ($(filter $(APX_USB_MSC),0 1),)
$(error APX_USB_MSC must be 0 or 1)
endif
# UVC (TinyUSB video class, bulk streaming, src/tusb/uvc.c): the camera composite device (UVC + mass storage). Needs APX_USB_MSC=1 as well.
APX_USB_UVC ?= 0
ifeq ($(filter $(APX_USB_UVC),0 1),)
$(error APX_USB_UVC must be 0 or 1)
endif
ifeq ($(APX_USB_UVC),1)
override APX_USB_MSC := 1
endif
# UAC (src/tusb/uac.c): a USB audio class 1 microphone in the camera composite device. Needs APX_USB_UVC=1.
APX_USB_UAC ?= 0
ifeq ($(filter $(APX_USB_UAC),0 1),)
$(error APX_USB_UAC must be 0 or 1)
endif
ifeq ($(APX_USB_UAC),1)
override APX_USB_UVC := 1
override APX_USB_MSC := 1
endif
BUILD ?= $(if $(filter action-vision,$(BOARD)),build,build/$(BOARD))$(if $(filter-out $(DEFAULT_USB_STACK),$(USB_STACK)),-$(USB_STACK))
BOARD_INPUTS := $(sort $(wildcard boards/$(BOARD)/*))
DISPLAY_SOURCE := $(if $(filter silvergear-3222882,$(BOARD)),src/display_st7789.c,src/display.c)
OPT ?= -Os
LTO ?= 1
HEAP_SIZE ?= 16384
USB_VID ?= 0x1908
USB_PID ?= 0x3319
FORCE_FULL_SPEED ?= 0
CPU_MHZ ?= 144
# 1 = hardware l.mul/l.div behind __mulsi3/__udivsi3/__divsi3/__umodsi3/__modsi3 (src/hwmuldiv.S); 0 = libgcc's software routines
HW_MULDIV ?= 1
ifeq ($(filter $(HW_MULDIV),0 1),)
$(error HW_MULDIV must be 0 or 1)
endif
# 1 = cold-boot flight recorder (last SPI sector + UART PA7); see include/appotech/trace.h
APX_SPI_TRACE ?= 0
# With tracing, keep the SDK watchdog running (default stops it so a stall stays a stall)
APX_TRACE_KEEP_WDT ?= 0
# 32.768 kHz ticks to wait at the start of a USB-launched SDK, before touching clocks or USB (0 = off)
APX_LAUNCH_SETTLE_TICKS ?= 0
APX_LAUNCH_READS ?= 0
APX_NO_CACHE_ENABLE ?= 0
APX_LAUNCH_WDT_STOP ?= 0
# Diagnostic activity of a USB-launched run in trace builds: 3 = flash log + UART, 1 = flash only, 2 = UART only, 0 = none
APX_LAUNCH_TRACE_MODE ?= 3
APX_LAUNCH_READ_LIST ?= 0xd800
# 1 = compile LVGL with -finstrument-functions; src/lvgl_port.c then keeps the last 30 entered functions in SRAM 0x3200 (previous run at 0x3300)
LVGL_INSTRUMENT ?= 0
# With APX_SPI_TRACE=1: 1 = after importing the previous run's SRAM ring, an SPI boot records into the ring only (no flash/UART traffic)
APX_SPI_TRACE_RING_ONLY ?= 0
# Diagnostic builds: reset (and log it) when the host has not configured the USB device after this many ticks (0 = off)
APX_USB_ENUM_TIMEOUT_TICKS ?= 0
ifeq ($(filter $(APX_SPI_TRACE),0 1),)
$(error APX_SPI_TRACE must be 0 or 1)
endif
ifeq ($(filter $(CPU_MHZ),96 144),)
$(error CPU_MHZ must be 96 or 144)
endif
EXAMPLES := $(shell ls -d examples/*/ | sed 's|examples/\(.*\)/|\1|')
EXAMPLE ?= usb-debug
ifeq ($(filter $(EXAMPLE),$(EXAMPLES)),)
$(error EXAMPLE must be one of: $(EXAMPLES))
endif
LVGL_ENABLED := $(filter lvgl-demo,$(EXAMPLE))
# The LVGL image has nothing but the small peripheral drivers in the extra-RAM bank, and a segment up there costs the SPI image
# the whole zero gap below it (~620 KB of flash). Its linker script keeps those drivers next to the main code instead.
LINKER_SCRIPT := $(if $(LVGL_ENABLED),$(BUILD)/ram-compact.ld,linker/ram.ld)
LVGL_DEMO ?= benchmark
# LVGL performance monitor (FPS/CPU/refresh in the benchmark's scene label): on for the benchmark, off for widgets.
LVGL_PERF ?= $(if $(filter benchmark,$(LVGL_DEMO)),1,0)
LVGL_DEMOS := benchmark widgets
ifneq ($(LVGL_ENABLED),)
ifeq ($(filter $(LVGL_DEMO),$(LVGL_DEMOS)),)
$(error LVGL_DEMO must be one of: $(LVGL_DEMOS))
endif
endif
LVGL_SOURCES := $(if $(LVGL_ENABLED),$(sort $(shell find third_party/lvgl/src -type f -name '*.c')))
LVGL_DEMO_SOURCES := $(if $(LVGL_ENABLED),third_party/lvgl/demos/lv_demos.c $(shell find third_party/lvgl/demos/widgets third_party/lvgl/demos/benchmark -type f -name '*.c' | sort))
LVGL_OBJECTS := $(patsubst third_party/lvgl/%.c,$(BUILD)/lvgl/%.o,$(LVGL_SOURCES) $(LVGL_DEMO_SOURCES))
LVGL_BUILD_INPUTS := $(if $(LVGL_ENABLED),$(LVGL_SOURCES) $(LVGL_DEMO_SOURCES) $(shell find third_party/lvgl/include -type f -name '*.h') third_party/lvgl/lvgl.h include/lv_conf.h include/appotech/lvgl_port.h src/lvgl_port.c linker/ram.ld)
MAIN := examples/$(EXAMPLE)/main.c
MAIN_INPUTS := $(MAIN)
LIBC := $(PREFIX)/or1k-appotech-elf/lib/libc.a
LIBGCC := $(shell $(CC) -print-libgcc-file-name)
BUILD_ID := $(shell { cat src/*.c src/*.S src/*.inc src/*.h $(wildcard src/tusb/*) third_party/fatfs/ff.c third_party/fatfs/ffunicode.c third_party/fatfs/ff.h third_party/fatfs/ffconf.h third_party/fatfs/diskio.h include/appotech/*.h $(BOARD_INPUTS) linker/ram.ld $(MAIN_INPUTS) third_party/stb/stb_image.h tools/prepare_stb.py $(LVGL_BUILD_INPUTS) $(LIBC) $(LIBGCC) $(wildcard toolchain/sources.lock.json); printf '%s\n' '$(BOARD)' '$(USB_STACK)' '$(APX_USB_MSC)' '$(APX_USB_UVC)' '$(APX_USB_UAC)' '$(EXAMPLE_DEFS)' '$(OPT)' '$(LTO)' '$(HEAP_SIZE)' '$(USB_VID)' '$(USB_PID)' '$(FORCE_FULL_SPEED)' '$(CPU_MHZ)' '$(HW_MULDIV)' '$(APX_SPI_TRACE)' '$(APX_TRACE_KEEP_WDT)' '$(APX_LAUNCH_SETTLE_TICKS)' '$(APX_LAUNCH_READS)' '$(APX_NO_CACHE_ENABLE)' '$(APX_LAUNCH_WDT_STOP)' '$(APX_LAUNCH_READ_LIST)' '$(APX_LAUNCH_TRACE_MODE)' '$(APX_USB_ENUM_TIMEOUT_TICKS)' '$(APX_SPI_TRACE_RING_ONLY)' '$(LVGL_INSTRUMENT)' '$(EXAMPLE)' '$(if $(LVGL_ENABLED),$(LVGL_DEMO) perf$(LVGL_PERF))'; } | sha256sum | cut -c1-16)
CFLAGS := $(OPT) -g -mappotech -fno-delayed-branch -fno-pic -ffunction-sections -fdata-sections -fno-common -Wall -Wextra -Werror -Iinclude -Iboards/$(BOARD) -Ithird_party/fatfs -DAPX_BUILD_ID='"$(BUILD_ID)"' -DAPX_USB_VID=$(USB_VID) -DAPX_USB_PID=$(USB_PID) -DAPX_USB_FORCE_FULL_SPEED=$(FORCE_FULL_SPEED) -DAPX_CPU_MHZ=$(CPU_MHZ) -DAPX_SPI_TRACE=$(APX_SPI_TRACE) -DAPX_TRACE_KEEP_WDT=$(APX_TRACE_KEEP_WDT) -DAPX_LAUNCH_SETTLE_TICKS=$(APX_LAUNCH_SETTLE_TICKS) -DAPX_LAUNCH_TRACE_MODE=$(APX_LAUNCH_TRACE_MODE) -DAPX_NO_CACHE_ENABLE=$(APX_NO_CACHE_ENABLE) -DAPX_USB_ENUM_TIMEOUT_TICKS=$(APX_USB_ENUM_TIMEOUT_TICKS) -DAPX_SPI_TRACE_RING_ONLY=$(APX_SPI_TRACE_RING_ONLY) -DAPX_LVGL_INSTRUMENT=$(LVGL_INSTRUMENT) -DAPX_LAUNCH_READS=$(APX_LAUNCH_READS) -DAPX_LAUNCH_WDT_STOP=$(APX_LAUNCH_WDT_STOP) '-DAPX_LAUNCH_READ_LIST=$(APX_LAUNCH_READ_LIST)'
ifneq ($(LVGL_ENABLED),)
CFLAGS += -Ithird_party/lvgl -Ithird_party/lvgl/include -DLV_CONF_INCLUDE_SIMPLE -DAPX_LVGL_DEMO='"$(LVGL_DEMO)"' -DAPX_LVGL_PERF=$(LVGL_PERF)
endif
# Extra preprocessor options for one example, e.g. make EXAMPLE=usb-modes EXAMPLE_DEFS=-DAPX_MODES_SD=1
EXAMPLE_DEFS ?=
CFLAGS += $(EXAMPLE_DEFS)
ifeq ($(USB_STACK),tinyusb)
# src/tusb holds tusb_config.h; TinyUSB's own headers are -isystem so only our code is held to -Werror
CFLAGS += -Isrc/tusb -isystem third_party/tinyusb/src -DAPX_USB_TINYUSB=1 -DAPX_USB_MSC=$(APX_USB_MSC) -DAPX_USB_UVC=$(APX_USB_UVC) -DAPX_USB_UAC=$(APX_USB_UAC)
endif
ifeq ($(LTO),1)
CFLAGS += -flto -fuse-linker-plugin
AR := $(CROSS)gcc-ar
NO_LTO_CFLAGS := $(filter-out -flto -fuse-linker-plugin,$(CFLAGS))
LTO_PLACED_OBJECTS := $(addprefix $(BUILD)/,image.o uart.o adc.o battery.o spi.o printer.o printer_render.o photo_prepare.o camera.o camera_files.o audio.o audio_files.o microphone.o microphone_files.o jpeg.o jpeg_files.o ffunicode.o) $(LVGL_OBJECTS)
# Keep the libc stream and heap hooks visible across LTO/plugin archive resolution.
LTO_LDFLAGS := -Wl,-u,stdout,-u,sbrk
endif
OBJECTS := $(BUILD)/runtime.o $(BUILD)/trace.o $(BUILD)/boot_marks.o $(BUILD)/exception.o $(BUILD)/spi_boot.o $(BUILD)/spi_overlay.o $(BUILD)/clock.o $(BUILD)/clock_pair.o $(BUILD)/usb.o $(BUILD)/debug.o $(BUILD)/display.o $(BUILD)/sd.o $(BUILD)/storage.o $(BUILD)/storage_stdio.o $(BUILD)/fatfs_diskio.o $(BUILD)/image.o $(BUILD)/uart.o $(BUILD)/adc.o $(BUILD)/battery.o $(BUILD)/spi.o $(BUILD)/gpio.o $(BUILD)/buttons.o $(BUILD)/printer.o $(BUILD)/watchdog.o $(BUILD)/power.o $(BUILD)/jpeg.o $(BUILD)/jpeg_files.o $(BUILD)/audio.o $(BUILD)/audio_files.o $(BUILD)/microphone.o $(BUILD)/microphone_files.o $(BUILD)/camera.o $(BUILD)/camera_files.o
ifeq ($(USB_STACK),tinyusb)
TINYUSB := third_party/tinyusb/src
# The TinyUSB glue, stack and class drivers form one unit. msc_sd.o (the SD card as the disk) stays a separate object so that it, and with it the
# SD driver, is linked only into images that call apx_usb_msc_use_sd().
TUSB_UNIT := $(BUILD)/tusb/dcd_apx.o $(BUILD)/tusb/usb_tusb.o $(BUILD)/tusb/msc_disk.o $(BUILD)/tusb/tusb.o $(BUILD)/tusb/tusb_fifo.o $(BUILD)/tusb/usbd.o
TUSB_SEPARATE :=
ifeq ($(APX_USB_MSC),1)
TUSB_UNIT += $(BUILD)/tusb/msc_device.o
TUSB_SEPARATE += $(BUILD)/tusb/msc_sd.o
endif
ifeq ($(APX_USB_UVC),1)
TUSB_UNIT += $(BUILD)/tusb/video_device.o $(BUILD)/tusb/uvc.o
endif
ifeq ($(APX_USB_UAC),1)
TUSB_UNIT += $(BUILD)/tusb/uac.o
endif
# linker/ram.ld places this code in extra RAM by object file name, which LTO would erase. So with LTO the unit is compiled as LTO objects and then
# partial-linked (-r -flinker-output=nolto-rel, which makes gcc run code generation) into one ordinary object, usb_all.o, whose function sections keep
# their names: LTO across the TinyUSB files, placement by name. (It saves about 5.5 KB over building them without LTO.) msc_sd.o is built without LTO.
# TUSB_PARTIAL_LTO=0 builds the unit as separate non-LTO objects placed by name instead (diagnostic: compare speed and size).
TUSB_PARTIAL_LTO ?= 1
ifeq ($(LTO)$(TUSB_PARTIAL_LTO),11)
TUSB_OBJECTS := $(BUILD)/usb_all.o $(TUSB_SEPARATE)
LTO_PLACED_OBJECTS += $(TUSB_SEPARATE)
else
TUSB_OBJECTS := $(TUSB_UNIT) $(TUSB_SEPARATE)
ifeq ($(LTO),1)
LTO_PLACED_OBJECTS += $(TUSB_OBJECTS)
endif
endif
OBJECTS := $(filter-out $(BUILD)/usb.o,$(OBJECTS)) $(TUSB_OBJECTS)
endif
FATFS_OBJECTS := $(BUILD)/ff.o $(BUILD)/ffunicode.o
ifeq ($(BOARD),silvergear-3222882)
OBJECTS += $(BUILD)/printer_reference.o $(BUILD)/printer_render.o $(BUILD)/photo_prepare.o
endif
ifeq ($(HW_MULDIV),1)
OBJECTS += $(BUILD)/hwmuldiv.o
# -u: pull the member from libappotech.a even when the only references are libcalls the LTO plugin creates late
LTO_LDFLAGS += -Wl,-u,__mulsi3,-u,__udivsi3,-u,__divsi3,-u,__umodsi3,-u,__modsi3
endif
ifneq ($(LVGL_ENABLED),)
OBJECTS += $(BUILD)/lvgl_port.o
LVGL_LIB := $(BUILD)/liblvgl.a
else
LVGL_LIB :=
endif
.PHONY: all clean FORCE
all: $(BUILD)/$(EXAMPLE).manifest.json $(BUILD)/libfatfs.a
$(BUILD):
	mkdir -p $@
FORCE:
$(BUILD)/config.stamp: FORCE | $(BUILD)
	python3 tools/build_config.py $@ --opt=$(OPT) --heap=$(HEAP_SIZE) --lto=$(LTO) --id=$(BUILD_ID)
ifneq ($(LTO_PLACED_OBJECTS),)
$(LTO_PLACED_OBJECTS): CFLAGS := $(NO_LTO_CFLAGS)
endif
$(BUILD)/stb/stb_image_sdk.h: third_party/stb/stb_image.h tools/prepare_stb.py | $(BUILD)
	python3 tools/prepare_stb.py $@
$(BUILD)/image.o: src/image.c $(BUILD)/stb/stb_image_sdk.h tools/check_image_stack.py $(BUILD)/config.stamp | $(BUILD)
	$(CC) $(filter-out -flto -fuse-linker-plugin,$(CFLAGS)) -I$(BUILD)/stb -Wno-unused-function -fstack-usage -MMD -MP -c $< -o $@
	python3 tools/check_image_stack.py $(BUILD)/image.su
$(BUILD)/%.o: src/%.c $(BUILD)/config.stamp | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/usb_all.o: $(TUSB_UNIT)
	$(CC) $(CFLAGS) -r -nostdlib -nostartfiles -flinker-output=nolto-rel -o $@ $^
$(BUILD)/tusb/%.o: src/tusb/%.c $(BUILD)/config.stamp | $(BUILD)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@
# TinyUSB's own sources are third-party: warnings stay visible but do not fail the build
$(BUILD)/tusb/tusb.o: $(TINYUSB)/tusb.c $(BUILD)/config.stamp | $(BUILD)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wno-error -MMD -MP -c $< -o $@
$(BUILD)/tusb/tusb_fifo.o: $(TINYUSB)/common/tusb_fifo.c $(BUILD)/config.stamp | $(BUILD)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wno-error -MMD -MP -c $< -o $@
$(BUILD)/tusb/usbd.o: $(TINYUSB)/device/usbd.c $(BUILD)/config.stamp | $(BUILD)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wno-error -MMD -MP -c $< -o $@
$(BUILD)/tusb/msc_device.o: $(TINYUSB)/class/msc/msc_device.c $(BUILD)/config.stamp | $(BUILD)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wno-error -MMD -MP -c $< -o $@
$(BUILD)/tusb/video_device.o: $(TINYUSB)/class/video/video_device.c $(BUILD)/config.stamp | $(BUILD)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wno-error -MMD -MP -c $< -o $@
$(BUILD)/display.o: $(DISPLAY_SOURCE) $(BOARD_INPUTS) $(BUILD)/config.stamp | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/%.o: src/%.S $(BUILD)/config.stamp | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/lvgl_port.o: src/lvgl_port.c $(BUILD)/config.stamp | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/lvgl/%.o: third_party/lvgl/%.c $(BUILD)/config.stamp
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(if $(filter 1,$(LVGL_INSTRUMENT)),-finstrument-functions) -MMD -MP -c $< -o $@
$(BUILD)/ram-compact.ld: linker/ram.ld | $(BUILD)
	# Also drop the TinyUSB code/tables (multi-line blocks): a file-backed segment in the extra bank would put the zero gap back into the SPI image.
	sed -e '/^ \.peripheral_code /d' -e '/^ \.peripheral_tables /d' -e '/^ \.usb_code /,/^ } > EXTRA_RAM/d' -e '/^ \.usb_tables /,/^ } > EXTRA_RAM/d' -e 's/^SECTIONS {/SECTIONS { __peripheral_code_end = ORIGIN(EXTRA_RAM);__usb_code_end = ORIGIN(EXTRA_RAM);/' $< > $@
$(BUILD)/liblvgl.a: $(LVGL_OBJECTS)
	$(AR) rcs $@ $^
$(BUILD)/ff.o: third_party/fatfs/ff.c $(BUILD)/config.stamp | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/ffunicode.o: third_party/fatfs/ffunicode.c $(BUILD)/config.stamp | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/libfatfs.a: $(FATFS_OBJECTS)
	$(AR) rcs $@ $^
$(BUILD)/startup.o: src/startup.S $(BUILD)/config.stamp | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/main.o: $(MAIN) $(BUILD)/config.stamp | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/libappotech.a: $(OBJECTS)
	rm -f $@ # ar rcs updates in place: a stale usb.o from an earlier USB_STACK would stay in the archive and could win the link
	$(AR) rcs $@ $^
$(BUILD)/$(EXAMPLE).elf: $(BUILD)/startup.o $(BUILD)/main.o $(BUILD)/libappotech.a $(BUILD)/libfatfs.a $(LVGL_LIB) $(LINKER_SCRIPT) $(LIBC) $(LIBGCC)
	$(CC) $(CFLAGS) $(LTO_LDFLAGS) -nostartfiles -T$(LINKER_SCRIPT) -Wl,-e,_start,--gc-sections,--build-id=none,--defsym=APX_HEAP_SIZE=$(HEAP_SIZE),-Map,$(BUILD)/$(EXAMPLE).map -o $@ $(filter %.o,$^) -Wl,--start-group $(BUILD)/libappotech.a $(BUILD)/libfatfs.a $(LVGL_LIB) -Wl,--end-group
	$(OBJDUMP) -d $@ > $(BUILD)/$(EXAMPLE).disasm
	$(SIZE) $@
$(BUILD)/$(EXAMPLE).manifest.json: $(BUILD)/$(EXAMPLE).elf tools/manifest.py tools/check_clock_elf.py
	python3 tools/check_clock_elf.py $< --output $(BUILD)/$(EXAMPLE).clock-check.json
	python3 tools/manifest.py $< --prefix "$(PREFIX)" --build-id $(BUILD_ID) --vid $(USB_VID) --pid $(USB_PID) --opt=$(OPT) --lto=$(LTO) --heap $(HEAP_SIZE) --full-speed $(FORCE_FULL_SPEED) --cpu-mhz $(CPU_MHZ) --board $(BOARD)
clean:
	rm -f $(BUILD)/*.o $(BUILD)/*.a $(BUILD)/*.d $(BUILD)/$(EXAMPLE).* $(BUILD)/config.stamp
	rm -rf $(BUILD)/lvgl
-include $(OBJECTS:.o=.d) $(TUSB_UNIT:.o=.d) $(FATFS_OBJECTS:.o=.d) $(LVGL_OBJECTS:.o=.d) $(BUILD)/main.d
