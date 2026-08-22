# Guest-writable surface — enumeration and §6j audit

Status: **audit, #610**. Answers one question: for every MMIO window, PIO
port, DMA/descriptor structure, and guest-shared page a guest can write to,
where does hype enforce §10 decision 19's hard rule (§6j) — "every
guest-supplied address/length used by device emulation must be bounds-checked
against that VM's own memory/backing store before the host acts on it" — and
what proves the check works?

This is a documentation artifact, not a fix pass. Every finding below that
isn't already covered by a test is a linked defect ticket, not an inline
change (#610's explicit non-goal). #602 (fuzzing the same surfaces) and #613
(auditing storage drivers against each other for *divergent* checks) are
complementary efforts; this document is the enumeration both build on.

## How to read this

Two validation primitives recur throughout, both from `core/guest_mem.h`:

- **VALID-1** — `hype_gpa_to_host(map, gpa, len)` / `hype_gpa_range_valid(map,
  gpa, len)`: translates a guest-physical `[gpa, gpa+len)` range to a host
  pointer *only* if it lies wholly inside one of the VM's own mapped regions;
  any out-of-range, straddling, zero-length, or overflowing request returns 0
  (reject). This is the primitive every GPA-carrying device check in this
  document ultimately calls. `arch/x86_64/svm/svm_vcpu.c`'s thin wrapper
  `guest_dma_xlate()` and `devices/xhci_dev.c`'s `gmem_read`/`gmem_write`
  add nothing but a `dma_map == 0` passthrough for identity-mapped
  microtests.
- **The centralized backend bounds check** — `core/blk_backend.c`'s
  `hype_blk_range_in_bounds()`, called from every
  `hype_blk_backend_read/write/writev()`, is the *single* place a
  guest-supplied LBA/sector-count range is checked against a disk backend's
  real capacity, so no front-end (virtio-blk, AHCI, NVMe, USB-MSC) can forget
  it. `devices/ata_disk.c`'s `hype_ata_disk_range_in_bounds()` and
  `devices/atapi.c`'s READ(10)/READ(12) bounds checks are the AHCI/ATAPI
  front-end's own pre-check against the same backend, one layer up.

Each row below cites the check as **file:function**, states what it verifies,
and cites either a unit test (`core/tests/test_*.c`) or a filed defect
ticket. "PASS" means a directed test already exercises the rejection branch;
"GAP → #N" means it doesn't, and a ticket is filed.

## 1. Block/storage DMA and descriptor structures

| Surface | Guest-writable structure | Check | Test / Ticket |
|---|---|---|---|
| virtio-blk queue | Descriptor chain (addr/len/flags/next), avail/used ring bases and indices | `arch/x86_64/svm/svm_vcpu.c:virtq_fetch_desc`/`virtq_validate_chain`/`process_virtio_blk_queue` (VALID-3): every descriptor index bounded by `queue_size`, every address translated via `guest_dma_xlate`→VALID-1, chain walk capped at `queue_size` hops to reject cycles | PASS — `core/tests/test_virtio_blk.c` (`test_chain_cycle_is_rejected`, `test_chain_zero_queue_size_rejected`, `tq_run()` malformed-chain harness) |
| virtio-blk config space | `common_cfg`/`device_cfg` register writes (queue_size, queue_desc/driver/device, feature bits) | `devices/virtio_blk.c:hype_virtio_blk_common_cfg_write` — fixed offset switch, width-checked, queue_size clamped to project max | PASS — `core/tests/test_virtio_blk.c` (`test_out_of_range_and_wrong_width_are_rejected`, `test_queue_size_write_is_clamped_to_project_max`) |
| virtio-net queue | Same shape as virtio-blk, TX + RX rings | `core/virtio_net_ring.c:read_desc`/`map_rings`/`hype_virtio_net_drain_tx`/`hype_virtio_net_deliver_rx` (VALID-3 via local `xlate()` wrapping `hype_gpa_to_host`) | PASS — `core/tests/test_virtio_net_ring.c` (`test_a_looping_chain_terminates`, `test_tx_descriptor_outside_the_map_is_refused_not_read`, `test_a_descriptor_table_outside_the_map_is_refused`, `test_rx_looping_chain_terminates`) |
| AHCI command-list header | 32-byte slot entry at `PxCLB`/`PxCLBU` + slot offset | `arch/x86_64/svm/svm_vcpu.c:process_ahci_command_slot` (VALID-3): `guest_dma_xlate(dma_map, cmd_list_phys, 32u)`, rejects on 0 | **GAP → [#663](https://github.com/bizzehdee/hype/issues/663)** — check present, no unit test drives an out-of-range command-list GPA |
| AHCI command table + PRDT | `cmd_table_phys` + `prdtl`-many 16-byte PRD entries, each PRD's `data_phys` | Same function, same VALID-3 pattern; command-table length computed in 64-bit (`0x80 + prdtl*16`) so a malicious `prdtl` can't wrap the bounds check before it runs | **GAP → [#663](https://github.com/bizzehdee/hype/issues/663)** (same finding) |
| AHCI received-FIS area | `PxFB`/`PxFBU` | `arch/x86_64/svm/svm_vcpu.c:complete_ahci_soft_reset` — `guest_dma_xlate(dma_map, rx_fis_phys, 0x40+20)` | **GAP → [#663](https://github.com/bizzehdee/hype/issues/663)** (same finding) |
| ATAPI packet (12-byte CDB) content | CDB bytes embedded in the command table (bounds-checked above); opcode/LBA/transfer-length fields inside it | `devices/atapi.c:hype_atapi_execute_cdb` + per-opcode LBA-range checks | PASS — `core/tests/test_atapi.c` (`test_read10_out_of_range`, `test_read10_count_spans_past_end`, `test_read12_out_of_range`, `test_unrecognized_opcode_rejected`) |
| Plain ATA (AHCI non-ATAPI) LBA/sector-count | Register H2D FIS command fields | `devices/ata_disk.c:hype_ata_disk_range_in_bounds`, `hype_ata_prd_sector_range` | PASS — `core/tests/test_ata_disk.c` (`test_range_in_bounds`, `test_prd_sector_range`) |
| Centralized disk-backend LBA bound (all front-ends) | Final LBA/sector-count handed to a `hype_blk_backend_t` | `core/blk_backend.c:hype_blk_range_in_bounds`, called from `hype_blk_backend_read/write/writev` — the canonical "one place to forget the check" model (plan.md §10 decision #17 rationale) | PASS — `core/tests/test_blk_backend.c` (`test_bounds_gate_rejects_oob`, `test_writev_validates_whole_list_before_any_byte`, `test_writev_32bit_total_overflow_refused`) |
| NVMe doorbell / queue base | SQ/CQ tail doorbell writes, `sq_base`/`cq_base` admin-queue setup | `devices/nvme.c:hype_nvme_doorbell_decode` (misalignment refused, not rounded), `hype_nvme_process_sq` (refuses qid ≥ `HYPE_NVME_MAX_QUEUES`, refuses before CC.EN) | PASS — `core/tests/test_nvme.c` (`test_doorbell_decode_refuses_rather_than_clamps`, `test_out_of_range_doorbell_value_is_refused`) |
| NVMe PRP list resolution | `prp1`/`prp2` and chained PRP-list pages | `devices/nvme.c:hype_nvme_prp_init`/`hype_nvme_prp_next` — pure logic, alignment and zero-length checked, no reader means no traversal | PASS — `core/tests/test_nvme.c` (`test_prp_refuses_malformed_descriptors`, misaligned-PRP2 and no-reader cases) |
| NVMe guest-memory callback (the actual GPA→host translation SQE/PRP fetches use) | Every PRP/queue-base/data-pointer the guest supplies, as bytes | `boot/main.c:nvme_guest_read`/`nvme_guest_write` — single injected `hype_nvme_ctx_t.gread/.gwrite` callback pair, both call `hype_gpa_to_host` | **GAP → [#669](https://github.com/bizzehdee/hype/issues/669)** — check present, but `boot/main.c` isn't linked into `core/tests/`, so this specific call site has zero unit-test reach |
| USB-MSC (BOT/SCSI over the guest's xHCI ring) | CBW/CSW + data stage | Not a separate GPA check: `devices/usb_msc.c`'s `msc_control` only ever sees a host buffer already resolved by the xHCI ring layer below it (§2) | PASS by construction — protected by the xHCI ring check; protocol correctness covered by `core/tests/test_usb_msc_dev.c` |

## 2. Guest xHCI rings

| Surface | Guest-writable structure | Check | Test / Ticket |
|---|---|---|---|
| Command ring, transfer rings, event ring, device-context base array (DCBAA) | Ring-base GPAs (via MMIO/`CRCR`/`DCBAAP`) and every TRB's data-buffer pointer | `devices/xhci_dev.c:gmem_read`/`gmem_write`/`gread32`/`gread64` (VALID-1 wrappers), used throughout `process_command_ring`, `process_transfer_ring`, `event_ring_latch`/`event_post` — file's own top comment states the "hard reject, never a raw deref" rule explicitly | **GAP → [#665](https://github.com/bizzehdee/hype/issues/665)** — check present and exercised in-range by `core/tests/test_xhci_dev.c`'s bring-up/event-wrap tests, but no case programs a ring/DCBAA pointer *outside* the test's `hype_gpa_map_t` |

## 3. PIO register models

All of these dispatch on a small fixed port/offset set (switch or bounded
if-chain) and store into fixed-width struct fields — no guest value is ever
used as an unguarded array index or turned into a pointer, **except** CMOS's
NVRAM index, called out below because it's exactly the pattern that would be
dangerous if unguarded.

| Surface | Guest-writable field | Check | Test |
|---|---|---|---|
| PIC (8259, master+slave) | ICW/OCW command+data bytes, per-chip, fixed 4 ports | `devices/pic.c:hype_pic_emu_io_write/io_read` — fixed port switch, unrecognized ports rejected | PASS — `core/tests/test_pic_emu.c` (`test_unrecognized_port_rejected`, `test_raise_irq_ignores_out_of_range`) |
| PIT (8253/8254, guest-facing channels) | Mode/data bytes per channel, port 0x61 gate | `devices/pit.c:hype_pit_emu_io_write/io_read` — fixed port switch | PASS — `core/tests/test_pit_emu.c` (`test_unrecognized_port_rejected`) — distinct from hype's own host-side PIT reprogramming (`devices/pit.h`'s HOST-facing half), which is not guest-writable and out of this audit's scope |
| PS/2 keyboard + mouse (8042) | Data/command ports 0x60/0x64, controller commands | `devices/ps2_keyboard.c:hype_ps2_kbd_io_write`, `devices/ps2_mouse.c:hype_ps2_mouse_write_command` — fixed command bytes, ring buffers with head/tail wrap (never an unguarded index) | PASS — `core/tests/test_ps2_keyboard.c` (`test_unrecognized_port_rejected`, `test_try_enqueue_refuses_when_full_instead_of_dropping`), `core/tests/test_ps2_mouse.c` |
| CMOS/RTC NVRAM | Index port (0x70) selects a byte in a fixed register array; data port (0x71) reads/writes it | `devices/cmos.c:hype_cmos_index_write` — `cmos->index = value & HYPE_CMOS_INDEX_MASK` masks the guest-supplied index *before* it is ever used to subscript `cmos->registers[]` | PASS — `core/tests/test_cmos.c` (`test_index_out_of_bounds_wraps_within_register_file`, `test_index_write_masks_nmi_disable_bit`) |
| Guest UART (16550) | 8 registers at a fixed port block, TX ring | `devices/guest_uart.c:uart_write_reg` — `offset & 0x7u`, TX ring uses head/tail modulo `HYPE_GUEST_UART_TX_RING` | PASS — `core/tests/test_guest_uart.c` — distinct from `core/serial.c`/`serial_hw.c`, hype's own **host** serial port, which is not guest-writable |
| ACPI PM (PM1a_CNT/EVT, PM timer) | `SLP_TYP`/`SLP_EN` bits at I/O 0x604, event/timer register reads at 0x600/0x608 | No guest-memory access at any point — `boot/main.c:vmm_handle_pm1_cnt_ioio` only tests bits of the written value itself, never dereferences a guest address, so §6j's address/length rule doesn't apply here | N/A (no VALID check needed); table-generation correctness (FADT/RSDP field values) covered by `core/tests/test_acpi.c` |

## 4. fw_cfg, framebuffer, flash, TPM

| Surface | Guest-writable field | Check | Test / Ticket |
|---|---|---|---|
| fw_cfg classic PIO (selector 0x510, data 0x511) | Selector key; per-byte reads only (no writable classic-port file in this build) | `devices/fw_cfg.c:lookup_item`/`hype_fw_cfg_read_byte` — every access bounded by the target item's own recorded `size` | PASS — `core/tests/test_fw_cfg.c` (`test_read_byte_unrecognized_key_returns_zero`) |
| fw_cfg DMA access struct (port 0x518 + address-high/low at 0x514) | The 16-byte control block's own GPA | `arch/x86_64/svm/svm_vcpu.c:hype_svm_vcpu_handle_fw_cfg_ioio` (mirrored in `vmcs_hw.c`) — `guest_dma_xlate(dma_map, access_phys, 16)` before reading the control block | **GAP → [#667](https://github.com/bizzehdee/hype/issues/667)** |
| fw_cfg DMA data buffer | `op.address`/`op.length` decoded from the control block | Same function — a second, independent `guest_dma_xlate(dma_map, op.address, op.length)`; a rejected range reports a DMA error to the guest rather than touching memory | **GAP → [#667](https://github.com/bizzehdee/hype/issues/667)** (same finding); pure `hype_fw_cfg_dma_execute` logic against an already-resolved buffer is PASS-tested in `core/tests/test_fw_cfg.c` |
| ramfb surface descriptor | `address`/`fourcc`/`flags`/`width`/`height`/`stride`, written via the fw_cfg `etc/ramfb` writable file | `devices/ramfb.c:hype_ramfb_frame_size` validates every field (format, stride ≥ width×4, nonzero dims) and computes the exact byte length; `boot/main.c:fw_1_ramfb_surface` then calls `hype_gpa_to_host(&vm->dma_map, cfg->address, frame_size)` before blitting | Field validation PASS — `core/tests/test_ramfb.c` (`test_ramfb.c`'s address/stride/format/flags rejection cases). GPA-translation call site: **GAP → [#669](https://github.com/bizzehdee/hype/issues/669)** (same root cause as the NVMe callback: `boot/main.c` isn't linked into `core/tests/`) |
| pflash (CFI flash window: firmware code + varstore) | Command-sequence bytes, program/erase offsets, buffered-write payload | `devices/pflash.c:hype_pflash_read/write` — every mode transition checks `offset < pf->size` and `in_range()`; buffered-write path bounded by `HYPE_PFLASH_MAX_BUFFER_WRITE`-sized `buffer_data[]` with `buffer_pos`/`buffer_remaining` tracked in lockstep | PASS — `core/tests/test_pflash.c` |
| TPM CRB (command/response buffer + control registers) | Register writes at fixed offsets; command/response bytes at a fixed in-window data offset (hype keeps the CRB buffer *inside* the MMIO window itself — no separate guest-memory GPA is ever accepted) | `devices/tpm_crb.c:hype_tpm_crb_read/write` — `offset >= HYPE_TPM_CRB_SIZE`, `offset + size > HYPE_TPM_CRB_SIZE` checked on every access before any indexing | PASS — `core/tests/test_tpm2.c` (`test_crb_round_trip` oob-read/write cases, `test_crb_registers`) |

## 5. Interrupt controllers and timers (MMIO)

| Surface | Guest-writable register | Check | Test |
|---|---|---|---|
| LAPIC MMIO page | ICR/LVT/SVR/TPR/EOI/timer registers | `devices/guest_lapic.c:hype_guest_lapic_write` — fixed offset switch, `size != 4` rejected outright; ICR write never dereferences guest memory (IPI target is a field value, routed to the VM layer, not a pointer) | PASS — `core/tests/test_guest_lapic.c` (`test_non_dword_access_rejected`, `test_all_registers_roundtrip`, extensive ICR/self-IPI coverage) |
| IO-APIC MMIO | `IOREGSEL`/`IOWIN`, redirection-table entries | `devices/ioapic.c:hype_ioapic_mmio_write` — `ioregsel & 0xFFu`, RTE index range-checked (`index >= REDIR_BASE && index < REDIR_BASE + NUM_RTES*2`), read-only Delivery-Status/Remote-IRR bits preserved against guest overwrite | PASS — `core/tests/test_ioapic.c` (`test_bad_offset_rejected`, `test_masked_and_nonfixed_and_oob`, `test_guest_cannot_write_remote_irr`) |
| HPET MMIO | Config/counter/per-timer config+comparator registers | `devices/hpet.c:hype_hpet_write` — `base & ~7u` alignment, per-timer `idx < HYPE_HPET_NUM_TIMERS` bound via `timer_index_for` | PASS — `core/tests/test_hpet.c` (`test_unimplemented_offsets_read_zero`, `test_32bit_halves_address_the_right_word`) |

## 6. Guest-shared clock pages (host writes INTO a guest-chosen GPA)

This direction is the reverse of everything above: the guest supplies a GPA
via an MSR write, and the **host** periodically writes its own clock data
into that page. The check is still §6j's rule — the guest-supplied GPA must
be validated before the host writes through it — just applied to a write
instead of a read.

| Surface | Guest-writable field | Check | Test / Ticket |
|---|---|---|---|
| kvmclock per-vCPU system-time page | `MSR_KVM_SYSTEM_TIME`/`_OLD` value = page GPA \| enable bit | `arch/x86_64/svm/svm_vcpu.c:hype_svm_pvclock_arm_system_time` (VMX mirror `vmx_pvclock_arm_system_time` in `vmcs_hw.c`) — `hype_gpa_to_host(pvclock_map, gpa, sizeof(time_info))`, returns early (no write) on 0 | **GAP → [#667](https://github.com/bizzehdee/hype/issues/667)** |
| kvmclock wall-clock page | `MSR_KVM_WALL_CLOCK` = page GPA | `arch/x86_64/svm/svm_vcpu.c:hype_svm_pvclock_arm_wall_clock` (VMX mirror `vmx_pvclock_arm_wall_clock`) — same pattern | **GAP → [#667](https://github.com/bizzehdee/hype/issues/667)** (same finding); pure write-and-versioning logic (`devices/pvclock.c:hype_pvclock_write_time_info/write_wall_clock`) is PASS-tested in `core/tests/test_pvclock.c` |
| Hyper-V hypercall page | `HV_X64_MSR_HYPERCALL` = page GPA \| enable \| locked | `arch/x86_64/cpu/hyperv.c:hype_hv_hypercall_page_write` — `hype_gpa_to_host(map, gpa, HYPE_HV_HYPERCALL_PAGE_SIZE)`, `-1` (no state change) on failure; locked-MSR and missing-Guest-OS-ID cases also guarded | PASS — `core/tests/test_hyperv.c` (`test_invalid_full_page_is_rejected_without_changes`: short page, NULL map, NULL output all covered) |
| Hyper-V reference-TSC page | `HV_X64_MSR_REFERENCE_TSC` = page GPA \| enable | `arch/x86_64/cpu/hyperv.c:hype_hv_reference_tsc_write` — same `hype_gpa_to_host` pattern, page zeroed only after a successful translate | **GAP → [#670](https://github.com/bizzehdee/hype/issues/670)** — `core/tests/test_hyperv.c` covers only the sibling `hype_hv_hypercall_page_write`; nothing in the file references `reference_tsc` at all, not even a happy-path case |

## Summary

- **Surfaces enumerated: 33** (rows across the six tables above; each row is
  one distinct guest-writable field/structure, even where several rows share
  one underlying check function, e.g. the three AHCI DMA structures).
- **Check + test already proven: 21.**
- **Check present, test missing → defect ticket filed: 11 rows**, covering 5
  tickets (each ticket bundles the rows that share one root cause — the same
  untested function, or the same "lives in an unlinked file" structural gap):
  - [#663](https://github.com/bizzehdee/hype/issues/663) — AHCI command-list/command-table+PRDT/received-FIS (3 rows)
  - [#665](https://github.com/bizzehdee/hype/issues/665) — xHCI rings/DCBAA (1 row)
  - [#667](https://github.com/bizzehdee/hype/issues/667) — fw_cfg DMA access-struct+data-buffer, kvmclock system-time+wall-clock MSR writes (4 rows)
  - [#669](https://github.com/bizzehdee/hype/issues/669) — NVMe guest-memory callback, ramfb GPA translation (2 rows)
  - [#670](https://github.com/bizzehdee/hype/issues/670) — Hyper-V reference-TSC page write (1 row) — the one row in this table that
    turned out, on a second independent pass, to be miscited as PASS against
    its sibling's test file rather than its own; corrected here
- **No check needed (no guest-memory access at all): 1** — ACPI PM1a_CNT.

Every gap above is a *missing regression test* for a check that already
looks correct by inspection, not a known-broken bounds check. #602's
fuzz harness, once built, should be pointed at exactly this table's rows —
particularly the five GAP tickets, since a directed test and a fuzz corpus
answer different questions (a fuzz run finds a shape nobody thought of; a
directed test proves the one shape we already know matters is actually
rejected).

## Maintenance rule

**A new guest-writable interface does not get to skip this table.** Whoever
adds a new MMIO window, PIO port, DMA/descriptor structure, or guest-shared
page adds its row here — check (file:function) and test-or-ticket — in the
same change that adds the interface, exactly as decision 51 requires a new
shared IO-APIC line to extend that line's pending-OR computation in the same
change that adds the device. An enumeration that drifts out of sync with the
code is worse than no enumeration: it tells the next reader a surface is
covered when nobody has looked at it since.
