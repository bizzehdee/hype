# hype.efi build: lightweight clang/lld pipeline targeting
# x86_64-unknown-uefi, per plan.md §8 -- not EDK2 (that's reserved for
# the separate guest firmware pipeline, M4+).

CC      := clang
LD      := ld.lld
TARGET  := x86_64-unknown-uefi

# -MMD -MP: emit a .d file of header prerequisites next to each .o, so editing a
# header (e.g. a struct in devices/*.h) recompiles EVERY .c that includes it, not
# just the .c whose timestamp changed. Without this, a stale .o with an outdated
# struct layout links against freshly-built ones that disagree on field offsets --
# which silently corrupted the ATAPI result struct across atapi.o/svm_vcpu.o and
# looked like a mysterious "struct size sensitivity" (GLADDER-STRUCT / #180).
CFLAGS  := --target=$(TARGET) -ffreestanding -fshort-wchar -mno-red-zone \
           -Wall -Wextra -g -O1 -std=c11 -MMD -MP
LDFLAGS := -flavor link -subsystem:efi_application -entry:efi_main

BUILD_DIR := build
CORE_SRCS := core/format.c core/console.c core/halt.c core/memmap.c \
             core/serial.c core/serial_hw.c core/font8x8.c core/gop.c core/gop_text.c \
             core/fatal.c core/strutil.c core/guest_ram.c core/mp.c core/linux_boot.c \
             core/admission.c core/file_io.c core/guest_mem.c core/logbuf.c core/nvlog.c \
             core/clockfacts.c core/io_histogram.c core/chunked_iso.c
ARCH_SRCS := arch/x86_64/cpu/gdt.c arch/x86_64/cpu/gdt_load.c arch/x86_64/cpu/idt.c \
             arch/x86_64/cpu/idt_load.c arch/x86_64/cpu/isr_decode.c \
             arch/x86_64/cpu/paging.c arch/x86_64/cpu/paging_load.c \
             arch/x86_64/cpu/pic.c arch/x86_64/cpu/lapic.c arch/x86_64/cpu/pit.c \
             arch/x86_64/cpu/pit_hw.c arch/x86_64/cpu/timer.c arch/x86_64/cpu/timer_isr.c \
             arch/x86_64/cpu/ps2_host.c arch/x86_64/cpu/ps2_host_hw.c \
             arch/x86_64/cpu/leader_chord.c \
             arch/x86_64/cpu/cpu_features.c arch/x86_64/cpu/cpu_features_hw.c \
             arch/x86_64/cpu/vmm_select.c arch/x86_64/cpu/vmexit.c arch/x86_64/cpu/mmio_decode.c \
             arch/x86_64/cpu/cpuid_emulate.c arch/x86_64/cpu/msr_emulate.c \
             arch/x86_64/cpu/ap_boot.c \
             arch/x86_64/svm/svm_bits.c arch/x86_64/svm/svm_enable_hw.c arch/x86_64/svm/svm_ops.c \
             arch/x86_64/svm/vmcb.c arch/x86_64/svm/svm_vcpu.c arch/x86_64/svm/npt.c \
             arch/x86_64/vmx/vmx_bits.c arch/x86_64/vmx/vmx_enable_hw.c arch/x86_64/vmx/vmx_ops.c \
             arch/x86_64/vmx/vmcs_hw.c arch/x86_64/vmx/ept.c
ARCH_ASM_SRCS := arch/x86_64/cpu/isr_stubs.S arch/x86_64/cpu/ap_trampoline.S
DEVICE_SRCS := devices/pic.c devices/pit.c devices/pflash.c devices/acpi.c devices/acpi_loader.c \
               devices/fw_cfg.c devices/ahci.c devices/atapi.c devices/ramfb.c devices/pci.c \
               devices/cmos.c devices/ps2_keyboard.c devices/ps2_mouse.c devices/bochs_vbe.c \
               devices/fb_blit.c devices/virtio_blk.c devices/ata_disk.c devices/e820.c \
               devices/guest_lapic.c devices/guest_uart.c devices/vt_filter.c devices/pvclock.c \
               devices/ioapic.c
BOOT_SRCS := boot/main.c
SRCS      := $(BOOT_SRCS) $(CORE_SRCS) $(ARCH_SRCS) $(DEVICE_SRCS)
OBJS      := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SRCS)) \
             $(patsubst %.S,$(BUILD_DIR)/%.o,$(ARCH_ASM_SRCS))
OUT       := $(BUILD_DIR)/hype.efi

# Fedora's edk2-ovmf path -- override on the command line (make
# OVMF_CODE=... OVMF_VARS=... run) if your distro installs these
# elsewhere (see docs/toolchain.md).
OVMF_CODE ?= /usr/share/OVMF/OVMF_CODE.fd
OVMF_VARS ?= /usr/share/OVMF/OVMF_VARS.fd
# A real, reasonably-sized ISO9660 image for ISO-1's own test -- reuses
# the same edk2-ovmf package's own UefiShell.iso (real media, not a
# synthetic blob) rather than vendoring a copy of it into this repo;
# override on the command line for a different test image.
TEST_ISO  ?= /usr/share/edk2/ovmf/UefiShell.iso
ESP       := $(BUILD_DIR)/esp

.PHONY: all clean test run

all: $(OUT)

# Pull in the per-object header-dependency files emitted by -MMD (leading '-'
# so a first build, before any .d exists, doesn't error). This is what makes a
# header edit trigger the right recompiles.
-include $(OBJS:.o=.d)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) --target=$(TARGET) -ffreestanding -mno-red-zone -c $< -o $@

$(OUT): $(OBJS)
	$(LD) $(LDFLAGS) -out:$@ $(OBJS)

test:
	core/tests/run.sh

# Boot hype.efi under QEMU+OVMF as a removable-media ESP (M0-4).
# -enable-kvm -cpu host: required from M2 onward -- our own VMX/SVM
# bring-up needs the guest CPU to actually expose real VT-x/AMD-V
# (nested virtualization), which plain TCG emulation doesn't faithfully
# provide (plan.md §10 decision #4's own stated testing strategy).
# Falls back to TCG automatically if /dev/kvm isn't available.
# -smp 2: required from M3-2 onward so there's a real second pCPU to
# exercise EFI_MP_SERVICES_PROTOCOL-based vCPU pinning against; with
# only 1 CPU the test guest still runs correctly, just on the BSP
# (the documented, non-fatal fallback -- see boot/main.c).
run: $(OUT)
	@mkdir -p $(ESP)/EFI/BOOT
	cp $(OUT) $(ESP)/EFI/BOOT/BOOTX64.EFI
	cp $(OVMF_VARS) $(BUILD_DIR)/OVMF_VARS.fd
	@mkdir -p $(ESP)/EFI/hype
	cp fw/OVMF_CODE.fd fw/OVMF_VARS.fd $(ESP)/EFI/hype/
	@mkdir -p $(ESP)/iso
	cp $(TEST_ISO) $(ESP)/iso/test.iso
	qemu-system-x86_64 \
	  -machine q35 -m 2048 -nodefaults \
	  -accel kvm -accel tcg -cpu host -smp 2 \
	  -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	  -drive if=pflash,format=raw,file=$(BUILD_DIR)/OVMF_VARS.fd \
	  -drive format=raw,file=fat:rw:$(ESP) \
	  -serial stdio -display none -vga none

clean:
	rm -rf $(BUILD_DIR)
