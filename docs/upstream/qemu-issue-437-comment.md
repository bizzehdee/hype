# Upstream: comment to add to QEMU issue #437

**Target:** https://gitlab.com/qemu-project/qemu/-/issues/437
("[AHCI] crash when running a GNU/Hurd guest", open since 2021-03-02)

**Do not open a new issue, and do not open a pull request.** QEMU takes patches by email to
`qemu-devel@nongnu.org`, and in this case there is nothing to write — the fix already exists
as commit `d9f78431d8eb` and first ships in **11.1.0**. What is missing is confirmation and,
possibly, a stable backport. Paste the text below as a comment on #437.

---

## Same crash on a plain ide-hd DMA read, with an OVMF guest; likely already fixed by `d9f78431d8eb`

Reproduced on `qemu-10.2.2-1.fc44` (Fedora 44, x86_64, KVM) with an **edk2/OVMF** guest instead
of GNU/Hurd, so the trigger is not Hurd-specific.

I believe `d9f78431d8eb` ("hw/ide/ahci: cancel in-flight buffered reads on command engine
restart", 2026-06-19) already fixes this — it addresses exactly the race described here,
`PxCMD.ST` 1 -> 0 -> 1 clearing `AHCIDevice.cur_cmd` while a read is still outstanding. I have
not been able to verify that directly: the fix is not in any 10.2.x release and Fedora 44 has no
newer QEMU, so this report is "still broken on 10.2.2, and here is an easy reproducer", not a
claim that master is broken.

Two things may still be worth acting on.

### 1. The crash I see is on the plain hard-disk DMA path, not the ATAPI paths the fix names

`d9f78431d8eb`'s message names the ATAPI `ide_buffered_readv()` paths:

```
PIO: cd_read_sector_cb() -> ide_atapi_cmd_reply_end() ->
     ide_transfer_start_norecurse() -> ahci_pio_transfer()
DMA: ide_atapi_cmd_read_dma_cb() -> ahci_dma_rw_buf() -> ahci_populate_sglist()
```

Mine is neither, and there is no CD in the configuration at all — a single `-drive format=raw`
hard disk:

```
Program terminated with signal SIGSEGV, Segmentation fault.
#0  ahci_commit_buf ()
#1  ide_dma_cb ()
#2  dma_blk_cb ()
#3  blk_aio_read_entry ()
#4  coroutine_trampoline ()
```

Faulting instruction and registers:

```
=> 0x...<ahci_commit_buf+11>:   add    %esi,0x4(%rax)
rax  0x0                      <- ad->cur_cmd is NULL
rsi  0x200                    <- tx_bytes = 512, a one-sector READ DMA
si_addr (SIGSEGV address) 0x4 <- offsetof(AHCICmdHdr, status)
```

i.e. the read-modify-write the compiler fuses from

```c
tx_bytes += le32_to_cpu(ad->cur_cmd->status);
ad->cur_cmd->status = cpu_to_le32(tx_bytes);
```

Since `ide_cancel_dma_sync()` tears down `s->bus->dma->aiocb`, which is what the hard-disk DMA
path uses, the fix plausibly covers this too. Worth a second pair of eyes, because if it does
then this is simply a second, much easier reproducer for the same fix — and if it does not, note
that `ahci_map_clb_address()` still does `ad->cur_cmd = NULL` with no cancel on the *re-map*
side, which is the other half of the 1 -> 0 -> 1 sequence.

### 2. The 10.2.x series looks like a backport candidate

Checking `hw/ide/ahci.c` at each tag for `ide_cancel_dma_sync`:

| tag | fix present |
|---|---|
| v9.2.0, v10.0.0, v10.1.0, v10.2.0, v10.2.2, v10.2.3, v10.2.4, v11.0.0 | no |
| v11.1.0-rc0 .. rc3, v11.1.0, master | **yes** |

So 10.2.4 does not carry it either. Distributions still on 10.2.x crash on an ordinary UEFI
boot from an AHCI disk: on this host, **95 `qemu-system-x86_64` SIGSEGV core dumps** accumulated
over five weeks of routine use before anyone looked at the crash rather than at the guest.

### Reproducer — no GNU/Hurd image, no CD, fails within about 6 seconds

The payload is irrelevant beyond being identifiable, so a good boot is distinguishable from a
bad one. Ours is a 2,560-byte UEFI application that initialises COM1 and prints one line.

1. Build a small raw disk and format the **whole image** as FAT32 (no partition table — edk2's
   FAT driver binds a whole-disk FAT):

   ```
   truncate -s 48M disk.img
   mformat -i disk.img -F -T $((48*1024*1024/512)) ::
   mmd    -i disk.img ::/EFI ::/EFI/BOOT
   mcopy  -i disk.img hello.efi ::/EFI/BOOT/BOOTX64.EFI
   ```

2. Boot on q35 with a **fresh varstore copy per run** (carrying one over changes the boot path
   and muddies the rate):

   ```
   cp /usr/share/edk2/ovmf/OVMF_VARS.fd vars.fd
   qemu-system-x86_64 -machine q35 -m 2048 -nodefaults -accel kvm -cpu host -smp 4 \
     -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
     -drive if=pflash,format=raw,file=vars.fd \
     -drive format=raw,file=disk.img \
     -serial file:serial.log -display none -vga std
   ```

   The bare `-drive` lands on q35's ICH9 AHCI at 00:1f.2, which is the path under test.

3. Loop it and check the exit status: a crash is SIGSEGV (shell status 139) plus a core dump.

### Rates and controls

Same image, same firmware, fresh varstore per run:

| conditions | crashes / boots |
|---|---|
| as above | 4 / 30 |
| plus `-debugcon file:... -global isa-debugcon.iobase=0x402` | 15 / 30 |

The debug console adds roughly 250 KB of port-0x402 writes per boot and about triples the rate,
so it is the quickest way to reproduce. (Worth noting because an earlier round of our own
testing recorded the opposite; the amplification is reliable now that we know what to count.)

Two controls that narrow it:

- **virtio-blk instead of AHCI: 0 failures in 24 boots** — same image, same firmware, same QEMU
  binary, arms interleaved within one session (the AHCI arm failed 1/24 in that batch).
  AHCI-specific.
- **A blank 48 MB image with no filesystem: 0 crashes in 12 boots.** Probing LBA 0 is not
  enough; it needs the real boot-time read sequence.

Happy to run experiments on this host — the reproducer is about three minutes per 30 boots — and
happy to test a 10.2.x backport if one is proposed.
