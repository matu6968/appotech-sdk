PREFIX ?= $(CURDIR)/toolchain/install
CROSS ?= $(PREFIX)/bin/or1k-appotech-elf-
CC := $(CROSS)gcc
AR := $(CROSS)ar
OBJDUMP := $(CROSS)objdump
SIZE := $(CROSS)size
BUILD := build
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
LIBC := $(PREFIX)/or1k-appotech-elf/lib/libc.a
LIBGCC := $(shell $(CC) -print-libgcc-file-name)
BUILD_ID := $(shell { cat src/*.c src/*.S src/*.inc third_party/fatfs/ff.c third_party/fatfs/ffunicode.c third_party/fatfs/ff.h third_party/fatfs/ffconf.h third_party/fatfs/diskio.h include/appotech/*.h boards/action-vision/board.h linker/ram.ld $(MAIN) third_party/stb/stb_image.h tools/prepare_stb.py $(LVGL_BUILD_INPUTS) $(LIBC) $(LIBGCC) $(wildcard toolchain/sources.lock.json); printf '%s\n' '$(OPT)' '$(LTO)' '$(HEAP_SIZE)' '$(USB_VID)' '$(USB_PID)' '$(FORCE_FULL_SPEED)' '$(CPU_MHZ)' '$(HW_MULDIV)' '$(APX_SPI_TRACE)' '$(APX_TRACE_KEEP_WDT)' '$(APX_LAUNCH_SETTLE_TICKS)' '$(APX_LAUNCH_READS)' '$(APX_NO_CACHE_ENABLE)' '$(APX_LAUNCH_WDT_STOP)' '$(APX_LAUNCH_READ_LIST)' '$(APX_LAUNCH_TRACE_MODE)' '$(APX_USB_ENUM_TIMEOUT_TICKS)' '$(APX_SPI_TRACE_RING_ONLY)' '$(LVGL_INSTRUMENT)' '$(EXAMPLE)' '$(if $(LVGL_ENABLED),$(LVGL_DEMO))'; } | sha256sum | cut -c1-16)
CFLAGS := $(OPT) -g -mappotech -fno-delayed-branch -fno-pic -ffunction-sections -fdata-sections -fno-common -Wall -Wextra -Werror -Iinclude -Iboards/action-vision -Ithird_party/fatfs -DAPX_BUILD_ID='"$(BUILD_ID)"' -DAPX_USB_VID=$(USB_VID) -DAPX_USB_PID=$(USB_PID) -DAPX_USB_FORCE_FULL_SPEED=$(FORCE_FULL_SPEED) -DAPX_CPU_MHZ=$(CPU_MHZ) -DAPX_SPI_TRACE=$(APX_SPI_TRACE) -DAPX_TRACE_KEEP_WDT=$(APX_TRACE_KEEP_WDT) -DAPX_LAUNCH_SETTLE_TICKS=$(APX_LAUNCH_SETTLE_TICKS) -DAPX_LAUNCH_TRACE_MODE=$(APX_LAUNCH_TRACE_MODE) -DAPX_NO_CACHE_ENABLE=$(APX_NO_CACHE_ENABLE) -DAPX_USB_ENUM_TIMEOUT_TICKS=$(APX_USB_ENUM_TIMEOUT_TICKS) -DAPX_SPI_TRACE_RING_ONLY=$(APX_SPI_TRACE_RING_ONLY) -DAPX_LVGL_INSTRUMENT=$(LVGL_INSTRUMENT) -DAPX_LAUNCH_READS=$(APX_LAUNCH_READS) -DAPX_LAUNCH_WDT_STOP=$(APX_LAUNCH_WDT_STOP) '-DAPX_LAUNCH_READ_LIST=$(APX_LAUNCH_READ_LIST)'
ifneq ($(LVGL_ENABLED),)
CFLAGS += -Ithird_party/lvgl -Ithird_party/lvgl/include -DLV_CONF_INCLUDE_SIMPLE -DAPX_LVGL_DEMO='"$(LVGL_DEMO)"'
endif
ifeq ($(LTO),1)
CFLAGS += -flto -fuse-linker-plugin
AR := $(CROSS)gcc-ar
NO_LTO_CFLAGS := $(filter-out -flto -fuse-linker-plugin,$(CFLAGS))
LTO_PLACED_OBJECTS := $(addprefix $(BUILD)/,image.o uart.o adc.o battery.o spi.o camera.o camera_files.o audio.o audio_files.o microphone.o microphone_files.o jpeg.o jpeg_files.o ffunicode.o) $(LVGL_OBJECTS)
# Keep the libc stream and heap hooks visible across LTO/plugin archive resolution.
LTO_LDFLAGS := -Wl,-u,stdout,-u,sbrk
endif
OBJECTS := $(BUILD)/runtime.o $(BUILD)/trace.o $(BUILD)/boot_marks.o $(BUILD)/exception.o $(BUILD)/spi_boot.o $(BUILD)/spi_overlay.o $(BUILD)/clock.o $(BUILD)/clock_pair.o $(BUILD)/usb.o $(BUILD)/debug.o $(BUILD)/display.o $(BUILD)/sd.o $(BUILD)/storage.o $(BUILD)/storage_stdio.o $(BUILD)/fatfs_diskio.o $(BUILD)/image.o $(BUILD)/uart.o $(BUILD)/adc.o $(BUILD)/battery.o $(BUILD)/spi.o $(BUILD)/gpio.o $(BUILD)/buttons.o $(BUILD)/watchdog.o $(BUILD)/power.o $(BUILD)/jpeg.o $(BUILD)/jpeg_files.o $(BUILD)/audio.o $(BUILD)/audio_files.o $(BUILD)/microphone.o $(BUILD)/microphone_files.o $(BUILD)/camera.o $(BUILD)/camera_files.o
FATFS_OBJECTS := $(BUILD)/ff.o $(BUILD)/ffunicode.o
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
$(BUILD)/%.o: src/%.S $(BUILD)/config.stamp | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/lvgl_port.o: src/lvgl_port.c $(BUILD)/config.stamp | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/lvgl/%.o: third_party/lvgl/%.c $(BUILD)/config.stamp
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(if $(filter 1,$(LVGL_INSTRUMENT)),-finstrument-functions) -MMD -MP -c $< -o $@
$(BUILD)/ram-compact.ld: linker/ram.ld | $(BUILD)
	sed -e '/^ \.peripheral_code /d' -e '/^ \.peripheral_tables /d' -e 's/^SECTIONS {/SECTIONS { __peripheral_code_end = ORIGIN(EXTRA_RAM);/' $< > $@
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
	$(AR) rcs $@ $^
$(BUILD)/$(EXAMPLE).elf: $(BUILD)/startup.o $(BUILD)/main.o $(BUILD)/libappotech.a $(BUILD)/libfatfs.a $(LVGL_LIB) $(LINKER_SCRIPT) $(LIBC) $(LIBGCC)
	$(CC) $(CFLAGS) $(LTO_LDFLAGS) -nostartfiles -T$(LINKER_SCRIPT) -Wl,-e,_start,--gc-sections,--build-id=none,--defsym=APX_HEAP_SIZE=$(HEAP_SIZE),-Map,$(BUILD)/$(EXAMPLE).map -o $@ $(filter %.o,$^) -Wl,--start-group $(BUILD)/libappotech.a $(BUILD)/libfatfs.a $(LVGL_LIB) -Wl,--end-group
	$(OBJDUMP) -d $@ > $(BUILD)/$(EXAMPLE).disasm
	$(SIZE) $@
$(BUILD)/$(EXAMPLE).manifest.json: $(BUILD)/$(EXAMPLE).elf tools/manifest.py tools/check_clock_elf.py
	python3 tools/check_clock_elf.py $< --output $(BUILD)/$(EXAMPLE).clock-check.json
	python3 tools/manifest.py $< --prefix "$(PREFIX)" --build-id $(BUILD_ID) --vid $(USB_VID) --pid $(USB_PID) --opt=$(OPT) --lto=$(LTO) --heap $(HEAP_SIZE) --full-speed $(FORCE_FULL_SPEED) --cpu-mhz $(CPU_MHZ)
clean:
	rm -f $(BUILD)/*.o $(BUILD)/*.a $(BUILD)/*.d $(BUILD)/$(EXAMPLE).* $(BUILD)/config.stamp
	rm -rf $(BUILD)/lvgl
-include $(OBJECTS:.o=.d) $(FATFS_OBJECTS:.o=.d) $(LVGL_OBJECTS:.o=.d) $(BUILD)/main.d
