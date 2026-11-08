# One position-independent flasher image for both memory pools. It is linked at the SRAM base and again at the DDR
# base, and the build fails unless the two binaries are byte-identical: that is the proof it can be uploaded to
# either (apxtool picks the pool at run time). The SRAM link is the shipped build/flasher/flasher.bin.
SDK := ..
CROSS := $(SDK)/toolchain/install/bin/or1k-appotech-elf-
OUT := $(SDK)/build/flasher
SRAM_BASE := 0x2000
DDR_BASE := 0x02180000
FLAGS := -Os -g -mappotech -fno-delayed-branch -ffreestanding -fno-builtin -fno-pic -fno-jump-tables -ffunction-sections -fdata-sections -fstack-usage -Wall -Wextra -Werror
LINK := -nostdlib -Tflasher.ld -Wl,--gc-sections,--build-id=none
all: $(OUT)/flasher.bin
silvergear-stage: $(OUT)/silvergear-stage.bin
silvergear-restore: $(OUT)/silvergear-restore.bin
silvergear-full-restore: $(OUT)/silvergear-full-restore.bin
$(OUT)/silvergear-full-restore.o: flasher.c | $(OUT)
	$(CROSS)gcc $(FLAGS) -DSILVERGEAR_BOOT_RESTORE -DSILVERGEAR_FULL_RESTORE -c $< -o $@
$(OUT)/silvergear-full-restore.elf: $(OUT)/entry.o $(OUT)/silvergear-full-restore.o flasher.ld
	$(CROSS)gcc $(FLAGS) $(LINK) -Wl,--defsym=FLASHER_BASE=$(SRAM_BASE) $(OUT)/entry.o $(OUT)/silvergear-full-restore.o -o $@
$(OUT)/silvergear-full-restore-ddr.elf: $(OUT)/entry.o $(OUT)/silvergear-full-restore.o flasher.ld
	$(CROSS)gcc $(FLAGS) $(LINK) -Wl,--defsym=FLASHER_BASE=$(DDR_BASE) $(OUT)/entry.o $(OUT)/silvergear-full-restore.o -o $@
$(OUT)/silvergear-full-restore.bin: $(OUT)/silvergear-full-restore.elf $(OUT)/silvergear-full-restore-ddr.elf
	$(CROSS)objcopy -O binary $(OUT)/silvergear-full-restore.elf $@
	$(CROSS)objcopy -O binary $(OUT)/silvergear-full-restore-ddr.elf $(OUT)/silvergear-full-restore-ddr.bin
	cmp $@ $(OUT)/silvergear-full-restore-ddr.bin
silvergear-bringup: $(OUT)/silvergear-bringup.bin
$(OUT)/silvergear-bringup.bin: flasher.S silvergear_bringup.c silvergear_bringup.ld ../tools/build_silvergear_bringup.py
	python3 ../tools/build_silvergear_bringup.py
$(OUT)/silvergear-restore.o: flasher.c | $(OUT)
	$(CROSS)gcc $(FLAGS) -DSILVERGEAR_BOOT_RESTORE -c $< -o $@
$(OUT)/silvergear-restore.elf: $(OUT)/entry.o $(OUT)/silvergear-restore.o flasher.ld
	$(CROSS)gcc $(FLAGS) $(LINK) -Wl,--defsym=FLASHER_BASE=$(SRAM_BASE) $(OUT)/entry.o $(OUT)/silvergear-restore.o -o $@
$(OUT)/silvergear-restore-ddr.elf: $(OUT)/entry.o $(OUT)/silvergear-restore.o flasher.ld
	$(CROSS)gcc $(FLAGS) $(LINK) -Wl,--defsym=FLASHER_BASE=$(DDR_BASE) $(OUT)/entry.o $(OUT)/silvergear-restore.o -o $@
$(OUT)/silvergear-restore.bin: $(OUT)/silvergear-restore.elf $(OUT)/silvergear-restore-ddr.elf
	$(CROSS)objcopy -O binary $(OUT)/silvergear-restore.elf $@
	$(CROSS)objcopy -O binary $(OUT)/silvergear-restore-ddr.elf $(OUT)/silvergear-restore-ddr.bin
	cmp $@ $(OUT)/silvergear-restore-ddr.bin
$(OUT)/silvergear-stage.elf: silvergear_stage.S silvergear_stage.ld | $(OUT)
	$(CROSS)gcc $(FLAGS) -nostdlib -Tsilvergear_stage.ld -Wl,--build-id=none silvergear_stage.S -o $@
	$(CROSS)objdump -d $@ > $(OUT)/silvergear-stage.disasm
$(OUT)/silvergear-stage.bin: $(OUT)/silvergear-stage.elf
	$(CROSS)objcopy -O binary $< $@
$(OUT):
	mkdir -p $@
$(OUT)/flasher.o: flasher.c | $(OUT)
	$(CROSS)gcc $(FLAGS) -c $< -o $@
$(OUT)/entry.o: flasher.S | $(OUT)
	$(CROSS)gcc $(FLAGS) -c $< -o $@
$(OUT)/flasher.elf: $(OUT)/entry.o $(OUT)/flasher.o flasher.ld
	$(CROSS)gcc $(FLAGS) $(LINK) -Wl,--defsym=FLASHER_BASE=$(SRAM_BASE),-Map,$(OUT)/flasher.map $(OUT)/entry.o $(OUT)/flasher.o -o $@
	$(CROSS)objdump -d $@ > $(OUT)/flasher.disasm
	$(CROSS)size $@
$(OUT)/flasher-ddr.elf: $(OUT)/entry.o $(OUT)/flasher.o flasher.ld
	$(CROSS)gcc $(FLAGS) $(LINK) -Wl,--defsym=FLASHER_BASE=$(DDR_BASE) $(OUT)/entry.o $(OUT)/flasher.o -o $@
$(OUT)/flasher.bin: $(OUT)/flasher.elf $(OUT)/flasher-ddr.elf
	$(CROSS)objcopy -O binary $(OUT)/flasher.elf $@
	$(CROSS)objcopy -O binary $(OUT)/flasher-ddr.elf $(OUT)/flasher-ddr.bin
	cmp $@ $(OUT)/flasher-ddr.bin || { echo "flasher is not position independent"; rm -f $@; exit 1; }
	@sort -t: -k2 -n $(OUT)/flasher.su | tail -3
clean:
	rm -rf $(OUT)
