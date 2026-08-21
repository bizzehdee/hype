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
# Build stamp, printed at boot so any
# captured log or screen photo says which build produced it. Real-hardware
# debugging on a machine with no serial port means comparing captures that all
# begin with identical boilerplate -- without a stamp there is no way to tell a
# fresh capture from a stale one, which has repeatedly wasted whole cycles.
# "-dirty" flags uncommitted changes, so an unreproducible build is obvious.
HYPE_BUILD_ID := $(shell git describe --always --dirty --abbrev=7 2>/dev/null || echo unknown)

CFLAGS  := --target=$(TARGET) -ffreestanding -fshort-wchar -mno-red-zone \
           -Wall -Wextra -g -O1 -std=c11 -MMD -MP \
           -Werror=constant-conversion \
           -DHYPE_BUILD_ID='"$(HYPE_BUILD_ID)"' $(EXTRA_CFLAGS)
LDFLAGS := -flavor link -subsystem:efi_application -entry:efi_main

BUILD_DIR := build
CORE_SRCS := core/format.c core/console.c core/halt.c core/memmap.c \
             core/serial.c core/serial_hw.c core/font8x8.c core/gop.c core/gop_text.c core/gop_mode.c core/gop_mode_hw.c \
             core/png_write.c \
             core/fatal.c core/strutil.c core/guest_ram.c core/mp.c core/linux_boot.c \
             core/admission.c core/file_io.c core/guest_mem.c core/logbuf.c \
             core/clockfacts.c core/io_histogram.c core/log_drain.c core/log_level.c core/kboot.c core/ram_pool.c core/vm_create.c core/vm_delete.c core/l2switch.c core/qcow2_create.c core/sha256.c core/tpm2.c core/render_budget.c core/scancode_queue.c core/ticket_lock.c \
             core/host_pci.c core/host_pci_hw.c core/ahci_host.c core/ahci_host_hw.c \
             core/gpt.c core/iso_stream.c core/fat.c core/file_range.c core/fs_ops.c core/ntfs.c core/ext2_alloc.c core/jbd2.c core/ext_jalloc.c core/ext.c core/ext_write.c core/blk_image.c core/blk_qcow2.c core/nvme_host.c core/nvme_host_hw.c core/blk_backend.c core/blk_phys.c core/blk_phys_hw.c core/phys_guard.c \
             core/kbd_decode.c core/vt_screen.c core/vt_render.c core/dashboard.c core/vm_lifecycle.c core/vm_isolation.c core/input_script.c core/input_runner.c core/vm_watchdog.c core/cmdparse.c \
             core/cfg.c core/phys_confirm.c core/scancode.c core/xhci.c core/xhci_hw.c core/usb_msc.c core/usb_hid.c core/blk_usb.c \
             core/fat_write.c core/fat_write_fs.c core/fat_exfat.c core/fat_exfat_fs.c \
             core/rtc.c core/rtc_hw.c \
             core/log_sink.c core/log_split.c core/disk_inventory.c core/cpu_topology.c core/smp_pack.c core/e1000.c core/e1000_hw.c core/arp.c core/virtio_net_ring.c core/nat.c core/e1000_dev_ring.c core/guest_nic.c core/pe_ident.c core/mtrr.c core/run_state.c
ARCH_SRCS := arch/x86_64/cpu/gdt.c arch/x86_64/cpu/gdt_load.c arch/x86_64/cpu/idt.c \
             arch/x86_64/cpu/idt_load.c arch/x86_64/cpu/isr_decode.c \
             arch/x86_64/cpu/paging.c arch/x86_64/cpu/paging_load.c arch/x86_64/cpu/mtrr_hw.c \
             arch/x86_64/cpu/pic.c arch/x86_64/cpu/lapic.c arch/x86_64/cpu/pit.c \
             arch/x86_64/cpu/pit_hw.c arch/x86_64/cpu/timer.c arch/x86_64/cpu/timer_isr.c \
             arch/x86_64/cpu/ps2_host.c arch/x86_64/cpu/ps2_host_hw.c \
             arch/x86_64/cpu/leader_chord.c arch/x86_64/cpu/host_input.c \
             arch/x86_64/cpu/cpu_features.c arch/x86_64/cpu/cpu_features_hw.c \
             arch/x86_64/cpu/fpu_state.c arch/x86_64/cpu/fpu_state_hw.c \
             arch/x86_64/cpu/vmm_select.c arch/x86_64/cpu/vmexit.c arch/x86_64/cpu/mmio_decode.c \
             arch/x86_64/cpu/cpuid_emulate.c arch/x86_64/cpu/msr_emulate.c \
             arch/x86_64/cpu/hyperv.c \
             arch/x86_64/cpu/ap_boot.c \
             arch/x86_64/svm/svm_bits.c arch/x86_64/svm/svm_enable_hw.c arch/x86_64/svm/svm_ops.c \
             arch/x86_64/svm/vmcb.c arch/x86_64/svm/svm_vcpu.c arch/x86_64/svm/npt.c \
             arch/x86_64/vmx/vmx_bits.c arch/x86_64/vmx/vmx_enable_hw.c arch/x86_64/vmx/vmx_ops.c \
             arch/x86_64/vmx/vmcs_hw.c arch/x86_64/vmx/ept.c
ARCH_ASM_SRCS := arch/x86_64/cpu/chkstk.S arch/x86_64/cpu/isr_stubs.S arch/x86_64/cpu/ap_trampoline.S \
             arch/x86_64/vmx/vmx_run.S arch/x86_64/svm/svm_run.S
DEVICE_SRCS := devices/pic.c devices/pit.c devices/hpet.c devices/smbios.c devices/pflash.c devices/acpi.c devices/acpi_loader.c \
               devices/fw_cfg.c devices/ahci.c devices/atapi.c devices/ramfb.c devices/pci.c devices/tpm_crb.c \
               devices/cmos.c devices/ps2_keyboard.c devices/ps2_mouse.c devices/bochs_vbe.c \
               devices/fb_blit.c devices/virtio_blk.c devices/ata_disk.c devices/e820.c \
               devices/guest_lapic.c devices/guest_uart.c devices/vt_filter.c devices/pvclock.c \
               devices/ioapic.c devices/nvme.c devices/virtio_net.c devices/e1000_dev.c
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

# #535/#534: micro-kernel guest artifacts. Freestanding ELF -> flat binary -> bzImage-shaped
# image, through the same clang/lld pipeline as hype.efi (no edk2, no extra toolchain).
#
# NOT built by `all`, deliberately: these are GUESTS, not part of the hypervisor. They change only
# when a test changes, and decoupling the two is the whole argument of #534 -- a build of hype must
# not depend on them, and rebuilding hype must not rebuild them.
#
# They DO live under $(BUILD_DIR), so `make clean` removes them: re-run `make micro` after one, or
# the harness reports MISSING rather than a verdict. Deliberate -- they are build outputs and
# belong with the others -- but it is the one thing that surprises.
MICRO_DIR   := tests/micro
MICRO_OUT   := $(BUILD_DIR)/micro
MICRO_NAMES := hello faulter ram1 cpumsr fwcfg intdeliver pausespin ps2 pflash pci ramfb virtioblk ahci atadisk bochsvbe virtionet netdns netpeer netgoal e1000dns
MICRO_IMAGES := $(patsubst %,$(MICRO_OUT)/%.bin,$(MICRO_NAMES))
MICRO_CFLAGS := --target=x86_64-unknown-elf -ffreestanding -fno-stack-protector -fno-pic \
                -mno-red-zone -mno-sse -Wall -Wextra -Werror -O2 -std=c11
MICRO_LDFLAGS := -T $(MICRO_DIR)/micro.ld -nostdlib --build-id=none

.PHONY: all clean test run run-cd run-2disk micro

micro: $(MICRO_IMAGES) $(MICRO_OUT)/suite.bin

# #554: the suite kernel. Every member is compiled a SECOND time from the same unedited source, with
# -DMICRO_SUITE (which turns micro_halt() into a longjmp back to the dispatcher) and
# -Dmicro_main=micro_test_<name> (which renames its entry point without touching the file). That is
# what keeps a suite member and a standalone artifact the same code -- neither can drift from the
# other, because there is only one source.
#
# faulter is deliberately NOT a member: it triple-faults on purpose, which would take the suite VM
# and every test after it down. It stays a standalone artifact where that is the whole point.
MICRO_SUITE_MEMBERS := hello ram1 cpumsr fwcfg pci pflash intdeliver pausespin ps2 ramfb
# Every header a microtest may include, in ONE place: the per-test rule and the suite rule both
# use it, so a new shared header cannot be added to one and forgotten in the other. A missing
# entry does not fail the build -- it silently links a STALE object, which is the worst way for a
# dependency to be wrong.
MICRO_HDRS := $(MICRO_DIR)/micro.h $(MICRO_DIR)/micro_pci.h $(MICRO_DIR)/micro_idt.h \
              $(MICRO_DIR)/micro_fwcfg.h $(MICRO_DIR)/micro_ahci.h
MICRO_SUITE_OBJS := $(patsubst %,$(MICRO_OUT)/suite-%.o,$(MICRO_SUITE_MEMBERS))

$(MICRO_OUT)/suite-%.o: $(MICRO_DIR)/%.c $(MICRO_HDRS)
	@mkdir -p $(MICRO_OUT)
	$(CC) $(MICRO_CFLAGS) -DMICRO_SUITE -Dmicro_main=micro_test_$* -c $< -o $@

$(MICRO_OUT)/suite.elf: $(MICRO_DIR)/suite.c $(MICRO_SUITE_OBJS) $(MICRO_DIR)/crt0.S \
                        $(MICRO_DIR)/suite_jmp.S $(MICRO_DIR)/micro.ld
	@mkdir -p $(MICRO_OUT)
	$(CC) $(MICRO_CFLAGS) -DMICRO_SUITE -c $(MICRO_DIR)/suite.c -o $(MICRO_OUT)/suite-main.o
	$(CC) --target=x86_64-unknown-elf -ffreestanding -c $(MICRO_DIR)/crt0.S -o $(MICRO_OUT)/crt0.o
	$(CC) --target=x86_64-unknown-elf -ffreestanding -c $(MICRO_DIR)/suite_jmp.S \
	      -o $(MICRO_OUT)/suite_jmp.o
	$(LD) $(MICRO_LDFLAGS) -o $@ $(MICRO_OUT)/crt0.o $(MICRO_OUT)/suite_jmp.o \
	      $(MICRO_OUT)/suite-main.o $(MICRO_SUITE_OBJS)

$(MICRO_OUT)/%.elf: $(MICRO_DIR)/%.c $(MICRO_DIR)/crt0.S $(MICRO_HDRS) $(MICRO_DIR)/micro.ld
	@mkdir -p $(MICRO_OUT)
	$(CC) $(MICRO_CFLAGS) -c $< -o $(MICRO_OUT)/$*.o
	$(CC) --target=x86_64-unknown-elf -ffreestanding -c $(MICRO_DIR)/crt0.S -o $(MICRO_OUT)/crt0.o
	$(LD) $(MICRO_LDFLAGS) -o $@ $(MICRO_OUT)/crt0.o $(MICRO_OUT)/$*.o

$(MICRO_OUT)/%.flat: $(MICRO_OUT)/%.elf
	llvm-objcopy -O binary $< $@

$(MICRO_OUT)/%.bin: $(MICRO_OUT)/%.flat
	tools/micro/mkbzimage.py $< $@


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
	@tools/check-no-vex.sh $(OUT)
	@tools/check-no-preebs-fileio.sh boot/main.c

test:
	@python3 tools/check-fw1-statics.py
	@python3 tools/check-one-mmio-list.py
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

# #325: the same harness with a real ATAPI device attached, for the optical-drive media source.
#
# Three things differ from `run` and all three are load-bearing:
#  - the ESP is an mtools-built FAT IMAGE, not vvfat. vvfat SIGSEGVs QEMU inside its own AHCI
#    emulation while OVMF reads it -- not a hype bug, but it ends the run.
#  - the CD gets a LOWER boot priority than the ESP, or the firmware boots the disc itself and
#    hype never runs.
#  - guest RAM comes down via hype.cfg, because OVMF fragments the memory map above 2 GB and
#    hype needs its guest RAM in one contiguous run.
run-cd: $(OUT)
	rm -f $(BUILD_DIR)/esp.img
	dd if=/dev/zero of=$(BUILD_DIR)/esp.img bs=1M count=128 status=none
	mkfs.vfat -F 32 -n HYPEESP $(BUILD_DIR)/esp.img >/dev/null
	mmd -i $(BUILD_DIR)/esp.img ::/EFI ::/EFI/BOOT ::/EFI/hype ::/iso
	mcopy -i $(BUILD_DIR)/esp.img $(OUT) ::/EFI/BOOT/BOOTX64.EFI
	mcopy -i $(BUILD_DIR)/esp.img fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/
	mcopy -i $(BUILD_DIR)/esp.img $(TEST_ISO) ::/iso/test.iso
	mcopy -i $(BUILD_DIR)/esp.img tools/qemu-cd-hype.cfg ::/hype.cfg
	cp $(OVMF_VARS) $(BUILD_DIR)/OVMF_VARS.fd
	qemu-system-x86_64 \
	  -machine q35 -m 3072 -nodefaults \
	  -accel kvm -accel tcg -cpu host -smp 2 \
	  -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	  -drive if=pflash,format=raw,file=$(BUILD_DIR)/OVMF_VARS.fd \
	  -drive format=raw,file=$(BUILD_DIR)/esp.img,if=none,id=esp \
	  -device ide-hd,drive=esp,bus=ide.0,bootindex=0 \
	  -drive id=hostcd,if=none,format=raw,readonly=on,file=$(TEST_ISO) \
	  -device ide-cd,drive=hostcd,bus=ide.2,bootindex=1 \
	  -serial stdio -display none -vga none

# #329: two guest disks on different buses, to check each is presented AND usable.
#
# Slot 0 is ahci-sata and slot 1 is nvme, backed by two tagged images (tools/make-2disk-images.sh),
# so a slot bound to the wrong file or the wrong capacity shows up as the wrong tag rather than as
# a plausible-looking empty disk.
run-2disk: $(OUT)
	tools/make-2disk-images.sh $(BUILD_DIR)
	rm -f $(BUILD_DIR)/esp_2disk.img $(BUILD_DIR)/fat2d.img
	dd if=/dev/zero of=$(BUILD_DIR)/esp_2disk.img bs=1M count=196 status=none
	parted -s $(BUILD_DIR)/esp_2disk.img mklabel gpt
	parted -s $(BUILD_DIR)/esp_2disk.img mkpart ESP fat32 1MiB 193MiB
	parted -s $(BUILD_DIR)/esp_2disk.img set 1 esp on
	dd if=/dev/zero of=$(BUILD_DIR)/fat2d.img bs=1M count=192 status=none
	mkfs.vfat -F 32 -n HYPEESP $(BUILD_DIR)/fat2d.img >/dev/null
	mmd -i $(BUILD_DIR)/fat2d.img ::/EFI ::/EFI/BOOT ::/EFI/hype ::/iso ::/hype ::/hype/disks
	mcopy -i $(BUILD_DIR)/fat2d.img $(OUT) ::/EFI/BOOT/BOOTX64.EFI
	mcopy -i $(BUILD_DIR)/fat2d.img fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/
	mcopy -i $(BUILD_DIR)/fat2d.img $(TEST_ISO) ::/iso/test.iso
	mcopy -i $(BUILD_DIR)/fat2d.img $(BUILD_DIR)/diska.img $(BUILD_DIR)/diskb.img ::/hype/disks/
	mcopy -i $(BUILD_DIR)/fat2d.img tools/qemu-2disk-hype.cfg ::/hype.cfg
	dd if=$(BUILD_DIR)/fat2d.img of=$(BUILD_DIR)/esp_2disk.img bs=512 seek=2048 conv=notrunc status=none
	cp $(OVMF_VARS) $(BUILD_DIR)/OVMF_VARS.fd
	qemu-system-x86_64 \
	  -machine q35 -m 3072 -nodefaults \
	  -accel kvm -accel tcg -cpu host -smp 2 \
	  -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	  -drive if=pflash,format=raw,file=$(BUILD_DIR)/OVMF_VARS.fd \
	  -drive format=raw,file=$(BUILD_DIR)/esp_2disk.img,if=none,id=esp \
	  -device ide-hd,drive=esp,bus=ide.0,bootindex=0 \
	  -serial stdio -display none -vga none

clean:
	rm -rf $(BUILD_DIR)
