# hype.efi build: lightweight clang/lld pipeline targeting
# x86_64-unknown-uefi, per plan.md §8 -- not EDK2 (that's reserved for
# the separate guest firmware pipeline, M4+).

CC      := clang
LD      := ld.lld
TARGET  := x86_64-unknown-uefi

CFLAGS  := --target=$(TARGET) -ffreestanding -fshort-wchar -mno-red-zone \
           -Wall -Wextra -g -O1 -std=c11
LDFLAGS := -flavor link -subsystem:efi_application -entry:efi_main

BUILD_DIR := build
CORE_SRCS := core/format.c core/console.c core/panic.c core/halt.c core/memmap.c
BOOT_SRCS := boot/main.c
SRCS      := $(BOOT_SRCS) $(CORE_SRCS)
OBJS      := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SRCS))
OUT       := $(BUILD_DIR)/hype.efi

OVMF_CODE := /usr/share/OVMF/OVMF_CODE.fd
OVMF_VARS := /usr/share/OVMF/OVMF_VARS.fd
ESP       := $(BUILD_DIR)/esp

.PHONY: all clean test run

all: $(OUT)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT): $(OBJS)
	$(LD) $(LDFLAGS) -out:$@ $(OBJS)

test:
	core/tests/run.sh

# Boot hype.efi under QEMU+OVMF as a removable-media ESP (M0-4).
run: $(OUT)
	@mkdir -p $(ESP)/EFI/BOOT
	cp $(OUT) $(ESP)/EFI/BOOT/BOOTX64.EFI
	cp $(OVMF_VARS) $(BUILD_DIR)/OVMF_VARS.fd
	qemu-system-x86_64 \
	  -machine q35 -m 512 -nodefaults \
	  -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	  -drive if=pflash,format=raw,file=$(BUILD_DIR)/OVMF_VARS.fd \
	  -drive format=raw,file=fat:rw:$(ESP) \
	  -serial stdio -display none -vga none

clean:
	rm -rf $(BUILD_DIR)
