# Guest-writable surface audit

This document lists every interface a guest VM can write to: MMIO windows,
PIO ports, DMA/descriptor structures, and guest-shared pages. For each
interface it names the §6j bounds-check that validates guest-supplied data
before host code acts on it, and the unit test that proves the check. Where
no test exists, it links the GitHub issue that tracks the gap. Where the
check itself is missing or wrong, it links the GitHub issue that tracks the
defect.

Scope: `bizzehdee/hype#610`. Built by reading `devices/*.c`, `core/*.c`, and
the guest-exit dispatch in `arch/x86_64/svm/svm_vcpu.c` /
`arch/x86_64/vmx/vmcs_hw.c`.

## The §6j validation pattern

`core/guest_mem.h` defines hype's one primitive for translating a
guest-physical address into a host pointer:

- `hype_gpa_map_t` holds a VM's guest-physical-to-host layout as a small set
  of contiguous regions (guest RAM, the firmware-flash window, and so on).
- `hype_gpa_to_host(map, gpa, len)` translates `[gpa, gpa+len)` and returns a
  host address only if the whole range fits inside one mapped region.
  Zero-length, overflowing, out-of-range, and region-straddling ranges all
  return 0.
- `hype_gpa_range_valid(map, gpa, len)` is the same check without a
  translation, for callers that only need to validate.

Every device model that turns a guest-supplied physical address into a host
pointer must route it through one of these two functions, or an equivalent
explicit bound (an array-index check, a state-machine guard, or a fixed-width
register mask) when the surface is a port I/O register rather than a
memory address. `core/guest_mem.c` is tested by `core/tests/test_guest_mem.c`.

## Maintenance rule

**A new guest device adds its row to this document in the same change that
adds the device.** This mirrors decision 51 in `plan.md` (the IO-APIC pin
allocation rule): a new guest-writable surface is a change to a shared
invariant this document exists to track, not something to add later. A pull
request that adds a device model without a corresponding row here is
incomplete.

## 1. Block and storage DMA structures

### virtio-blk

| Guest-writable field | Check | What it verifies | Test |
|---|---|---|---|
| Avail/used ring base (queue driver/device address) | `arch/x86_64/svm/svm_vcpu.c: process_virtio_blk_queue()` (`:4705`), via `guest_dma_xlate()` -> `hype_gpa_to_host()` | Ring address + size fits the VM's mapped GPA range | `core/tests/test_virtio_blk.c: test_chain_via_real_gpa_map_translates_and_succeeds` |
| Descriptor fetch (index, `next` chain link) | `virtq_validate_chain()` (`svm_vcpu.c:4638`) | Chain length capped at `queue_size`, rejecting an infinite `next` cycle | `test_virtio_blk.c: test_chain_cycle_is_rejected` |
| Descriptor data segment (`addr`, `len`) | `guest_dma_xlate()` in the same queue-processing path | Segment address + length fits the VM's mapped GPA range | `test_virtio_blk.c: test_chain_data_segment_outside_mapped_region_is_rejected` |
| Malformed/zero-length chain shapes | `virtq_validate_chain()` | Rejects a zero-segment or otherwise malformed chain without touching backend state | `test_virtio_blk.c: test_chain_malformed_shapes_change_nothing`, `test_chain_zero_queue_size_rejected` |
| GET_ID response buffer | length clamped to `HYPE_VIRTIO_BLK_ID_BYTES` before translation | A short guest buffer cannot be overrun | `test_virtio_blk.c: test_get_id_short_buffer_is_not_overrun` |
| Config-space register writes (queue size, driver features) | `devices/virtio_blk.c: hype_virtio_blk_common_cfg_write()` | Values clamped to project-supported ranges (queue size, feature bits) | `test_virtio_blk.c: test_queue_size_write_is_clamped_to_project_max`, `test_driver_feature_write_accumulates_across_both_halves` |
| Final sector-range check (all paths) | `core/blk_backend.c: hype_blk_range_in_bounds()`, reached via `hype_blk_backend_read/write/writev()` | `lba + count` does not overflow and stays within `total_sectors` | `core/tests/test_blk_backend.c: test_writev_validates_whole_list_before_any_byte` |

`devices/virtio_blk.c` itself holds only register decode/config-space logic;
the DMA/GPA boundary lives in the shared, vendor-neutral
`process_virtio_blk_queue()`, called identically from both the SVM and VMX
exit paths.

### AHCI (SATA)

| Guest-writable field | Check | What it verifies | Test |
|---|---|---|---|
| Command-list base (`PxCLB`/`PxCLBU`), ATAPI port | `process_ahci_command_slot()` (`svm_vcpu.c:2444`), via `guest_dma_xlate()` | Command-list GPA fits the VM's mapped range; a rejected translation aborts the command | `core/tests/test_ahci_dma.c: test_out_of_range_command_list_refused` (added closing **#663**/**#661**) |
| Command-list base (`PxCLB`/`PxCLBU`), disk port | `process_ahci_ata_command_slot()` (`svm_vcpu.c:3265`) | A rejected translation now aborts the command instead of being dereferenced — fixed as **#672** (was a guest-triggerable NULL dereference, the ATAPI sibling above was never affected) | `core/tests/test_ahci_dma.c` (same suite as **#663** above; **#672** is the fix commit `8efc71d`) |
| Command-table + PRDT base (`hdr.cmd_table_phys`), both ports | Same functions, second `guest_dma_xlate()` call | Command-table GPA + `0x80 + prdtl*16` bytes fits the mapped range | `test_ahci_dma.c: test_out_of_range_command_table_refused` (closed **#663**/**#661**) |
| Each PRD entry's data pointer | Same functions' PRDT loop (`svm_vcpu.c:2649`, `ahci_backend_rw_prdt()` at `:3161`) | Data-buffer GPA + byte count fits the mapped range before read/write | `test_ahci_dma.c: test_out_of_range_prd_data_pointer_refused` (closed **#663**/**#661**) |
| Received-FIS area (`PxFB`/`PxFBU`) | Same functions | FIS-area GPA fits the mapped range | Covered by the same `test_ahci_dma.c` suite (closed **#663**/**#661**) |
| Disk-port LBA/sector-count (backend-attached path) | `hype_blk_backend_read/write()` -> `hype_blk_range_in_bounds()` | `lba + count <= total_sectors` | `core/tests/test_blk_backend.c: test_bounds_gate_rejects_oob` |
| Disk-port LBA/sector-count (RAM-media fallback, no backend attached) | `devices/ata_disk.c: hype_ata_disk_range_in_bounds()` (`:150`) | Same bound, against the RAM-media capacity | `core/tests/test_ata_disk.c: test_range_in_bounds` |

Both `process_ahci_command_slot()` and `process_ahci_ata_command_slot()` are
shared verbatim between the SVM and VMX exit paths and are pure,
pointer-parameter functions with no privileged instructions — they are
directly host-callable from a unit test, but `arch/x86_64/svm/svm_vcpu.c` is
blanket coverage-exempt in `core/tests/run.sh`, which is why this whole
boundary (including the #672 defect) went unmeasured. That structural
problem is tracked separately as **#659**.

### ATAPI

| Guest-writable field | Check | What it verifies | Test |
|---|---|---|---|
| READ(10)/READ(12) LBA + transfer count | `devices/atapi.c: handle_read()` (`:141`) | `lba >= total_sectors \|\| count > total_sectors - lba` rejected as `CHECK_CONDITION`/`ILLEGAL_REQUEST` | `core/tests/test_atapi.c: test_read10_out_of_range`, `test_read12_out_of_range` |
| Destination buffer for the transfer | Handled by the AHCI PRDT loop above (ATAPI is always hosted on an AHCI port) | Same GPA bound as the AHCI command-table/PRDT check | See AHCI row above |
| READ(12) byte-length arithmetic | `handle_read()`'s `count * HYPE_ATAPI_SECTOR_SIZE` | 32-bit multiply can overflow for a large enough backing ISO — was a reported-length correctness bug, not an out-of-bounds read | Fixed as **#656** (found by the AUDIT-2 storage-model review, **#613**) — the multiply is now computed in 64 bits and refused if it would overflow the reported length |

ATAPI is read-only optical media, never backed by a `hype_blk_backend_t`, so
it reimplements the LBA-bounds check rather than routing through
`hype_blk_range_in_bounds()`. This is by design (**#613**'s finding), not a
gap.

### NVMe

| Guest-writable field | Check | What it verifies | Test |
|---|---|---|---|
| Submission/completion queue base addresses | `nvme_guest_read()`/`nvme_guest_write()` (`boot/main.c`), delegating to `hype_gpa_read()`/`hype_gpa_write()` (`core/guest_mem.c`, extracted by **#669**) | Queue-base GPA + access length fits the VM's mapped range | `core/tests/test_guest_mem.c` (closed **#669**: the translate-then-copy shape every injected callback used is now a shared, directly host-tested primitive rather than living unreachably inside `boot/main.c`) |
| SQE fetch / CQE post | `devices/nvme.c: hype_nvme_process_sq()` (`:659`), via injected `gread`/`gwrite` callbacks | Same GPA bound, applied per queue entry | `core/tests/test_nvme.c: test_sqe_decode`, `test_processor_consumes_each_entry_exactly_once` — against *fake* callbacks only; the real `boot/main.c` wiring is the #669 gap |
| PRP list continuation entries | `devices/nvme.c: hype_nvme_prp_next()` (`:396`) | Page-alignment enforced on every continuation PRP | `test_nvme.c: test_prp_refuses_malformed_descriptors`, `test_prp_list_chains_at_the_last_slot` |
| IO data payload | `hype_blk_backend_read/write()` (`devices/nvme.c:547,551`) | `lba + count <= total_sectors`, same centralized gate as virtio-blk | `test_nvme.c: test_io_bounds_and_error_paths` |
| Doorbell register writes | `devices/nvme.c` doorbell decode | Value refused (not clamped) when it names a queue slot outside the declared queue size | `test_nvme.c: test_doorbell_decode_refuses_rather_than_clamps`, `test_out_of_range_doorbell_value_is_refused`, `test_doorbell_is_bounded_by_the_declared_size` |

`devices/nvme.c` is deliberately pure: every guest-memory touch goes through
injected function pointers, and the real implementation
(`nvme_guest_read`/`nvme_guest_write` in `boot/main.c`) is the single point
documented in that file as "where a guest-supplied queue base or PRP address
becomes a host access." `core/nvme_host.c`/`core/nvme_host_hw.c` are hype
acting as an NVMe *driver* against a real physical controller — not guest
input, out of scope here.

### Centralized dispatcher (`blk_backend`)

| Guest-writable field | Check (`core/blk_backend.c`) | What it verifies | Test |
|---|---|---|---|
| `lba`, `count` on any single read/write | `hype_blk_range_in_bounds()` | `count != 0`, `lba + count` does not overflow 64 bits, and `lba + count <= total_sectors` | `core/tests/test_blk_backend.c: test_range_in_bounds` |
| `lba`, `count` dispatch (read/write) | `hype_blk_backend_read()`/`hype_blk_backend_write()` | Null-guards the backend/callback, then applies the bound above before calling into the backend | `test_blk_backend.c: test_dispatch_null_guards`, `test_bounds_gate_rejects_oob` |
| Vectored write segment list | `hype_blk_backend_writev()` | Every segment validated (64-bit sum, no `>0xFFFFFFFF` total) against `total_sectors` **before any byte moves** | `test_blk_backend.c: test_writev_validates_whole_list_before_any_byte`, `test_writev_32bit_total_overflow_refused` |

virtio-blk, the AHCI disk port (when a backend is attached), NVMe, and
USB-MSC (`devices/usb_msc.c:312`) all route through this single choke point.
ATAPI does not — it is never backed by a `hype_blk_backend_t` (see above).

### Backend implementations

`core/blk_image.c`, `core/blk_qcow2.c`, `core/blk_phys.c`, and
`core/blk_usb.c` implement the `hype_blk_backend_t` read/write callbacks.
None re-validate `lba`/`count` — they run only on ranges the dispatcher above
already validated (`core/blk_backend.c:168`, `core/blk_phys.c:5`, both
documented explicitly). Tests: `core/tests/test_blk_image.c: test_bounds`,
`test_blk_qcow2.c`, `test_blk_phys.c: test_bounds_gate`. `blk_phys.c` and
`blk_usb.c` are host-side adapters to real physical/USB storage, exercising
hype's own host xHCI/AHCI/NVMe drivers — not a guest-writable surface.

## 2. Network descriptor rings

### virtio-net

| Guest-writable field | Check | What it verifies | Test |
|---|---|---|---|
| Descriptor-table base | `core/virtio_net_ring.c: read_desc()` -> `xlate()` -> `hype_gpa_to_host()` | Per-descriptor slot (16 bytes) fits the mapped range | `core/tests/test_virtio_net_ring.c: test_a_descriptor_table_outside_the_map_is_refused` |
| Avail/used ring bases | `map_rings()` | Each ring translated as one whole range, so a partial/mid-walk read never happens | `test_virtio_net_ring.c: test_untranslatable_rings_consume_nothing` |
| Descriptor payload buffer (`addr`, `len`) | Inline `xlate()` call in `hype_virtio_net_drain_tx()`/`deliver_rx()` | Buffer GPA + length fits the mapped range before the buffer is touched | `test_virtio_net_ring.c: test_tx_descriptor_outside_the_map_is_refused_not_read`, `test_rx_through_a_real_map_and_an_unmapped_buffer` |
| Descriptor `next` chain link / head index | Bounds check against `qsz` (a ring-relative index, not a GPA) plus a bounded walk | A malformed chain cannot loop forever or read past the ring | `test_virtio_net_ring.c: test_tx_next_link_outside_the_ring`, `test_rx_looping_chain_terminates` |
| Config-space writes (queue desc/driver/device halves, MAC) | `devices/virtio_net.c: hype_virtio_net_common_cfg_write()` | Access width and offset bounds enforced; values are stored raw and validated at consumption time (above) | `core/tests/test_virtio_net.c: test_access_widths_and_bounds_are_enforced` |

This is the most thoroughly negative-tested surface in the codebase — every
GPA field has an explicit `*_outside_the_map_is_refused`-style test. It is
the reference pattern the xHCI gap below is measured against.

## 3. USB device models

### Guest xHCI controller

Every guest-memory touch in `devices/xhci_dev.c` goes through local helpers
(`gmem_read`/`gmem_write`/`gread32`/`gread64`/`gwrite32`), all wrapping
`hype_gpa_to_host()`.

| Guest-writable field | Check | What it verifies | Test |
|---|---|---|---|
| CRCR (command-ring base) | `process_command_ring()` (`xhci_dev.c:313`) | Each TRB read via `gread32`, bounded ring walk | `core/tests/test_xhci_dev.c: test_command_ring_base_outside_map_is_inert` (closed **#665**) |
| ERSTBA / event-ring segment table | `event_ring_latch()` (`:134`) | Base/size translated and rejected if zero | `test_event_ring_wrap` plus the out-of-map cases below (closed **#665**) |
| DCBAAP + per-slot DCBAA entry | `cmd_address_device()` (`:211`) | Entry GPA translated; input-context pointer also checked for alignment | `test_xhci_dev.c: test_address_device_dcbaap_outside_map_is_rejected`, `test_address_device_dcbaa_entry_outside_map_is_rejected` (closed **#665**) |
| Input Context (slot/EP0/EP-N) | `cmd_address_device()`/`cmd_configure_endpoint()` (`:231-288`) | Each sub-structure translated before read | `test_xhci_dev.c: test_address_device_input_context_outside_map_is_rejected` (closed **#665**) |
| Output Device Context writes | Same functions | Write target translated before write | Covered by the same `test_xhci_dev.c` suite (closed **#665**) |
| Transfer-ring TRB data-buffer address + length | `process_transfer_ring()` (`:452,513,536`) | Buffer GPA + `xfer_len` fits the mapped range before the transfer runs | Closed **#662** — see `test_xhci_dev.c`'s transfer-ring out-of-map cases |

The checks are real, present by inspection, and now each has a
`test_virtio_net_ring.c`-style negative test (a CRCR/ERSTBA/DCBAAP/context/
TRB-buffer pointer deliberately placed outside the map) — **#662** and
**#665** are both closed. `core/xhci.c`/`core/xhci_hw.c` are hype's own
**host**-side driver for a real controller, not guest-facing — out of scope.

### USB mass storage (USB-MSC)

| Guest-writable field | Check | What it verifies | Test |
|---|---|---|---|
| CBW (`dCBWDataTransferLength`, direction, CDB, tag) | `devices/usb_msc.c: msc_bulk_out()` (`:337`) | Signature + length sanity, CDB length clamped to 16 | `core/tests/test_usb_msc.c: test_cbw`, `test_cdbs` |
| SCSI READ(10)/WRITE(10) LBA + block count | `usb_msc.c: scsi_dispatch()` (`:312`) -> `hype_blk_range_in_bounds()` | Same centralized LBA/count gate as virtio-blk/AHCI/NVMe | `core/tests/test_usb_msc_dev.c: test_bad_lba_fails` |
| Data-OUT/Data-IN buffer capacity | Bounded by the xHCI transfer-ring buffer length (already validated above) | `devices/usb_msc.c` never calls `hype_gpa_to_host()` itself — it only sees host buffers the xHCI layer already translated | `core/tests/test_usb_msc_dev.c: test_end_to_end_via_xhci`, `test_read_write_roundtrip` |

### USB HID

`core/usb_hid.c` has no guest-writable surface: it parses a real physical
device's descriptors and translates its HID reports into PS/2 scancodes for
the guest — data flows host-device to guest only. `core/tests/test_usb_hid.c`
covers descriptor parsing and scancode mapping; there is no guest-write path
to test.

### USB passthrough

`devices/usb_passthru.c` forwards a guest's control/bulk transfers to a real
host USB device (`pt_control`/`pt_bulk_out`/`pt_bulk_in`). Like USB-MSC, it
never calls `hype_gpa_to_host()` itself — the buffers it receives are already
host pointers, translated once upstream by `devices/xhci_dev.c`'s transfer-ring
handling (see above). It relies on that funnel rather than adding its own
check. Test: `core/tests/test_usb_passthru.c`.

## 4. PIO register models

For a fixed-width hardware register (an 8259 mask byte, a 16550 scratch
register, and so on), the full byte range is a legal value on real hardware,
so "the check" is either an array-index bound on the guest-selected
register/channel, or the fact that the port-decode switch itself only
recognizes the ports the device owns — anything else is rejected or ignored,
matching real chipset behavior.

### PIC (8259, `devices/pic.c`)

Ports 0x20/0x21 (master command/data), 0xA0/0xA1 (slave command/data).

| Guest-writable field | Check | What it verifies | Test |
|---|---|---|---|
| Port selection | `hype_pic_emu_io_write()`/`_read()` switch on `port` | Only the four owned ports are handled; anything else returns -1 | `core/tests/test_pic_emu.c` |
| ICW/OCW command bytes, IMR | No bound needed — every byte value (0-255) is a legal ICW/OCW/IMR value on real hardware | n/a | `test_pic_emu.c` (init sequencing, mask, EOI cases) |

### PIT (8254, `devices/pit.c`)

Ports 0x40/0x41/0x42 (channel 0/1/2 data), 0x43 (mode/command).

| Guest-writable field | Check | What it verifies | Test |
|---|---|---|---|
| Port selection | `hype_pit_emu_io_write()`/`_read()` switch on `port` | Only the four owned ports are handled | `core/tests/test_pit_emu.c: test_unrecognized_port_rejected` |
| Channel select (command byte bits 6:7) | `write_command()`: `channel_sel == 3` (read-back) is refused, else indexes a fixed 3-element array | Channel index is always 0-2 by construction of the 2-bit field | `test_pit_emu.c: test_channel1_is_independently_addressable`, `test_channels_are_independent` |
| Reload/count value | No bound needed — any 16-bit count is legal | n/a | `test_pit_emu.c` (lobyte/hibyte access-mode cases) |
| Port 0x61 (NMI/speaker gate) | `hype_pit_emu_port61_write()`: `value & HYPE_PIT_PORT61_WRITABLE` | Only the writable bits are stored; read-only status bits (refresh toggle, CH2 OUT) are computed, not stored | `core/tests/test_pit.c` |

### PS/2 keyboard and mouse (`devices/ps2_keyboard.c`, `devices/ps2_mouse.c`)

Ports 0x60 (data), 0x64 (status/command).

| Guest-writable field | Check | What it verifies | Test |
|---|---|---|---|
| Output-FIFO index (kbd) | `push_output()`: `out_count >= HYPE_PS2_KBD_FIFO_SIZE` guard before indexing `out_fifo[]` | Ring never overruns; a full FIFO drops the byte instead | `core/tests/test_ps2_keyboard.c` |
| Mouse queue index | `enqueue_byte()`: `count >= HYPE_PS2_MOUSE_QUEUE_SIZE` guard | Same pattern for the mouse's own ring | `core/tests/test_ps2_mouse.c` |
| Controller command byte (0x64) | `hype_ps2_kbd_io_write()` switch on `value` | Unrecognized commands are silently ignored, matching real controller tolerance | `core/tests/test_ps2_host.c` |
| Device command byte (0x60, keyboard/mouse-targeted) | `hype_ps2_kbd_io_write()`/`hype_ps2_mouse_write_command()` | Unrecognized commands ACKed generically; no array index derived from the value | `test_ps2_keyboard.c`, `test_ps2_mouse.c` |

`core/kbd_decode.c`, `core/scancode.c`, `core/scancode_queue.c` sit behind
this device on the host-input side (real keyboard to guest scancode
translation) and carry no guest-write surface of their own. Tests:
`core/tests/test_kbd_decode.c`, `test_scancode.c`, `test_scancode_queue.c`.

### CMOS/RTC (`devices/cmos.c`, `core/rtc.c`)

Port 0x70 (index, write-only), port 0x71 (data).

| Guest-writable field | Check | What it verifies | Test |
|---|---|---|---|
| Register index (port 0x70) | `hype_cmos_index_write()`: `value & HYPE_CMOS_INDEX_MASK` (0x7F) | Index always fits the 128-byte `registers[]` array, whatever byte the guest writes | `core/tests/test_cmos.c: test_index_out_of_bounds_wraps_within_register_file` |
| Register data (port 0x71) | `hype_cmos_data_write()` | Read-only bits preserved per register (status A's UIP held clear, status C not writable at all, status D's VRT preserved); everything else stored as-is | `test_cmos.c: test_register_a_uip_is_held_clear`, `test_register_c_is_read_only`, `test_register_d_vrt_survives_a_guest_write` |

`core/rtc.c`/`core/rtc_hw.c` provide the calendar-arithmetic primitive
`hype_cmos_advance_to()` calls; they take no guest input directly. Test:
`core/tests/test_rtc.c`.

### UART / COM1 (`devices/guest_uart.c`)

Ports 0x3F8-0x3FF (8 registers).

| Guest-writable field | Check | What it verifies | Test |
|---|---|---|---|
| Register offset | `hype_guest_uart_read()`/`_write()`: `offset & 0x7u` | Offset always fits the fixed 8-register switch; the port decode upstream already confines `offset` to COM1's own 8-port span, so this mask is defense-in-depth | `core/tests/test_guest_uart.c` (one test per register offset) |
| THR/RBR byte (TX/RX rings) | Ring-full guard (`next != u->tx_head` / RX ring check) before indexing `tx[]`/`rx[]` | A full ring drops the byte instead of overrunning the array | `test_guest_uart.c: test_rx_ring_full_rejects` |

### ACPI power-management registers (`devices/acpi.c`)

Port 0x604 (PM1a_CNT/PM1a_EVT, 16-bit), port 0xCF9 (reset control).

| Guest-writable field | Check | What it verifies | Test |
|---|---|---|---|
| PM1a_CNT (SLP_EN and the rest of the 16-bit register) | `hype_svm_vcpu_handle_pm1_cnt_ioio()` / `hype_vmx_vcpu_handle_pm1_cnt_ioio()` (`svm_vcpu.c:512`, mirrored in `vmcs_hw.c`) | SLP_EN (bit 13) is extracted and masked off before the rest of the 16-bit value is stored; no array index or pointer derived from the value | N/A to §6j — closed as **#668**: neither handler ever dereferences a guest-supplied address/length, so the "bounds-check" question this audit asks doesn't apply. A plain correctness test for SLP_EN readback would be a reasonable nice-to-have, just not a §6j finding. |
| 0xCF9 reset-control register | `hype_svm_vcpu_handle_reset_ctl_ioio()` / VMX equivalent | Bit 2 (RST_CPU) triggers a platform reset request; no other bits are trusted for anything beyond that flag | N/A to §6j, same reasoning as the row above (**#668**) |

Both handlers live in the same coverage-exempt files as the AHCI functions
above (**#659**); the underlying logic is a simple fixed-width register mask
with no guest-address to bounds-check at all, which is why **#668** closed
as out-of-scope rather than as a fixed gap.

## 5. Firmware-interface surfaces

### fw_cfg (`devices/fw_cfg.c`)

| Guest-writable field | Check | What it verifies | Test |
|---|---|---|---|
| Writable-file internal offset (`etc/ramfb` is the only registered writable file) | `hype_fw_cfg_dma_execute()`: `if (dst_off < size)` | Bytes past the registered buffer's own size are dropped, not written | `core/tests/test_fw_cfg.c: test_writable_file_write_past_end_is_dropped` |
| Only registered items are writable | `lookup_writable_item()` (checks `write_data != 0`) | A guest cannot write to a read-only fw_cfg file | `test_fw_cfg.c: test_dma_execute_write_rejected` |
| DMA access-struct address (port 0x518) and the data-buffer GPA it names | `hype_fw_cfg_dma_op_run()` (`devices/fw_cfg.c`, extracted by **#667** from the SVM/VMX port-0x518 handlers, which both now call it instead of duplicating it) | Both GPAs fit the VM's mapped range before either is dereferenced | `core/tests/test_fw_cfg.c: test_dma_op_run_out_of_range_access_struct_refused`, `test_dma_op_run_out_of_range_data_buffer_reports_dma_error` (closed **#664**, **#667**) |

### ramfb (`devices/ramfb.c`)

| Guest-writable field | Check | What it verifies | Test |
|---|---|---|---|
| Surface descriptor fields (fourcc, width, height, stride) | `hype_ramfb_frame_size()` | Internal consistency: known format, non-zero dimensions, `stride >= width * 4` | `core/tests/test_ramfb.c: test_frame_size_validation` |
| Surface `address` + computed frame size, as a guest-physical range | `boot/main.c: fw_1_ramfb_surface()` -> `hype_gpa_to_host(&vm->dma_map, cfg->address, frame_size)` | Framebuffer GPA + size fits the VM's mapped range before hype blits to/from it | Closed as **#666**: deliberately NOT extracted, because this call site is a direct, no-separable-logic call to `hype_gpa_to_host()` — the underlying primitive is already exhaustively tested (`core/tests/test_guest_mem.c`), and extraction here would add a wrapper with no new coverage, unlike NVMe's case below where real translate-then-copy logic existed to extract. **#669** (below) is the sibling ticket that DID find real logic worth extracting, in NVMe's read/write path. |
| Pixel blit | `core/fb_blit.c: hype_fb_blit_copy()` | Operates only on already-validated host pointers; clips to `min(src,dst)` dimensions | `core/tests/test_fb_blit.c` |

### pflash (`devices/pflash.c`)

| Guest-writable field | Check | What it verifies | Test |
|---|---|---|---|
| WRITE_BYTE offset | `in_range()`, called from `hype_pflash_write()`'s `WRITE_BYTE_PENDING` case | Offset fits the flash window | `core/tests/test_pflash.c: test_write_byte_out_of_range_sets_program_error`, `test_write_and_read_out_of_range_offset_rejected` |
| BLOCK_ERASE confirm offset + erase loop | Confirm-offset match, plus an inline `(block_start+i) < pf->size` guard per erased byte | Erase cannot run past the flash window | `test_pflash.c: test_block_erase_wrong_confirm_offset_rejected`, `test_block_erase_sets_erased_bytes_to_0xff` |
| WRITE_TO_BUFFER count | `buffer_remaining` is a `uint8_t` (max 255), and `buffer_data[]` is a fixed 512-byte array | Count cannot exceed the buffer's own capacity by construction; zero count explicitly rejected | `test_pflash.c: test_buffered_write_zero_count_rejected` |
| Buffered-write commit offset | `in_range(pf, pf->buffer_offset, pf->buffer_pos)` in the `BUFFER_CONFIRM_PENDING` case | Commit cannot write past `pf->size` | `test_pflash.c` covers the round-trip and non-sequential-offset cases; a commit that overruns the end of `pf->size` has no dedicated test (minor gap, no ticket filed — the same `in_range()` primitive is proven by the WRITE_BYTE tests above) |

### TPM CRB (`devices/tpm_crb.c`, `core/tpm2.c`)

| Guest-writable field | Check | What it verifies | Test |
|---|---|---|---|
| MMIO offset/size into the CRB register+data page | `hype_tpm_crb_write()`/`_read()`: `offset >= HYPE_TPM_CRB_SIZE \|\| size == 0 \|\| size > 8 \|\| offset+size > HYPE_TPM_CRB_SIZE` | Every access fits the CRB page | `core/tests/test_tpm2.c: test_crb_round_trip`, `test_crb_registers` |
| Guest-supplied command length (read from the data buffer on CTRL_START) | `crb_execute()`: clamps `len` to 10 if it is out of `[10, HYPE_TPM_CRB_DATA_SIZE]`/`HYPE_TPM2_MAX_CMD` | A malformed length cannot drive an oversized read | `test_crb_registers` covers `len < 10`; the upper-bound clamp is proven one layer down (next row), not directly at the CRB layer |
| Command length vs. actual buffer size | `core/tpm2.c: hype_tpm2_execute()`: `size != cmd_len \|\| size > HYPE_TPM2_MAX_CMD` refused | Second, independent check on the same invariant | `test_tpm2.c: test_malformed` |
| Response length vs. CRB data buffer | `crb_execute()`: `rlen` capped to `HYPE_TPM_CRB_DATA_SIZE` | `hype_tpm2_execute()`'s own `HYPE_TPM2_MAX_RSP` (1024) is smaller than the CRB data buffer, so this line is a second-line defense; not independently exercised (minor gap, no ticket filed — defense-in-depth, primary bound proven above) |

## 6. Interrupt controller and timer MMIO

### LAPIC (`devices/guest_lapic.c`)

| Guest-writable field | Check | What it verifies | Test |
|---|---|---|---|
| ISR-block reads (8 dwords at 16-byte spacing) | `hype_guest_lapic_read()`: explicit range + 16-byte-alignment check before indexing `isr[]` | An unaligned or out-of-range offset falls through to the benign default, never indexes past `isr[7]` | `core/tests/test_guest_lapic.c: test_isr_range_ignores_unaligned_offsets` |
| Named register offsets (ID, TPR, SVR, LVT entries, ICR, timer registers) | Fixed `switch (offset)` in `hype_guest_lapic_read()`/`_write()` | Any offset not in the switch reads/writes as a benign default | `test_guest_lapic.c: test_all_registers_roundtrip` |
| Access width | Both functions: `if (size != 4u) return -1` | Only 4-byte accesses are accepted, matching the architectural register model | `test_guest_lapic.c: test_non_dword_access_rejected` |
| ICR-written vector / self-IPI bit index | `vector >> 5` into `self_ipi_pending[]`/`isr[]` (8-entry arrays) | Safe by construction: `vector` is a `uint8_t` (0-255), so `vector >> 5` is always 0-7 | `test_guest_lapic.c: test_isr_bit_lands_in_the_right_dword_across_the_whole_range` |

`core/avic.c` (AMD AVIC) accelerates delivery into the same virtual-APIC page
this model represents; it does not add a new guest-writable structure. Test:
`core/tests/test_avic.c`.

### IO-APIC (`devices/ioapic.c`)

| Guest-writable field | Check | What it verifies | Test |
|---|---|---|---|
| IOREGSEL (register select) | `hype_ioapic_mmio_write()`: `value & 0xFFu` | Selector always fits the defined index space | `core/tests/test_ioapic.c: test_ioregsel_latches` |
| Redirection-table index (derived from IOREGSEL) | Range check `index >= HYPE_IOAPIC_INDEX_REDIR_BASE && index < ... + HYPE_IOAPIC_NUM_RTES*2` before indexing `rte[]` | Index always fits the 24-entry redirection table (decision 51: all 24 pins allocated) | `test_ioapic.c: test_bad_offset_rejected`, `test_masked_and_nonfixed_and_oob` |
| RTE low-dword write | Read-only bits (Delivery-Status, Remote-IRR) preserved, not guest-settable | A guest cannot fake an in-service or pending state | `test_ioapic.c: test_guest_cannot_write_remote_irr` |
| GSI argument to `hype_ioapic_raise()`/`_deassert()`/`hype_ioapic_rte_dest()` | Each function: `gsi >= HYPE_IOAPIC_NUM_RTES` guard | Internal callers (device IRQ lines) cannot index past the table either | `test_ioapic.c` (assertion-adjacent cases) |

### HPET (`devices/hpet.c`)

| Guest-writable field | Check | What it verifies | Test |
|---|---|---|---|
| Register offset (config, int-status, counter, per-timer blocks) | `hype_hpet_write()`/`_read()`: masked to 8-byte-aligned `base`, then matched against known register ranges | Unimplemented offsets read as zero / writes are ignored | `core/tests/test_hpet.c: test_unimplemented_offsets_read_zero` |
| Timer index (derived from offset within the per-timer block) | `timer_index_for()` + `idx < HYPE_HPET_NUM_TIMERS` guard before indexing `timers[]` | Index always fits the modelled timer count | `test_hpet.c: test_32bit_halves_address_the_right_word` |
| Config register writable bits | `hype_hpet_write()`: masked to `HYPE_HPET_CONFIG_ENABLE \| HYPE_HPET_CONFIG_LEGACY_ROUTE` | Reserved bits cannot be set by the guest | `test_hpet.c: test_capability_bits_survive_a_guest_write` |
| Per-timer config writable bits | Capability bits (periodic-capable, size, route-capability) re-asserted as read-only after every write | A guest cannot claim a routing capability the timer does not have | `test_hpet.c: test_route_capability_is_reported_and_read_only` |

## 7. Guest-shared clock pages

These are the highest-risk surface in this document: a guest picks an
arbitrary guest-physical address via an MSR write, and hype computes and
writes time data into it on the guest's behalf. A missed bounds check here is
a guest-directed write primitive, not merely a crash.

### kvmclock / pvclock (`devices/pvclock.c`)

| Guest-writable field | Check | What it verifies | Test |
|---|---|---|---|
| MSR_KVM_SYSTEM_TIME GPA | `hype_pvclock_arm_system_time()` (`devices/pvclock.c`, extracted by **#667** from both backends' private helpers) | GPA + `sizeof(hype_pvclock_vcpu_time_info)` fits the VM's mapped range before the time-info page is written | `core/tests/test_pvclock.c: test_arm_system_time_out_of_range_gpa_refused` (closed **#671**, **#667**) |
| MSR_KVM_WALL_CLOCK GPA | `hype_pvclock_arm_wall_clock()` (same extraction) | Same bound, for the wall-clock page | `test_pvclock.c: test_arm_wall_clock_out_of_range_gpa_refused` (closed **#671**, **#667**) |
| Time-info/wall-clock page write itself (pure function) | `devices/pvclock.c: hype_pvclock_write_time_info()`/`_write_wall_clock()` | Version-counter memory-barrier discipline (odd = update in progress, even = complete) so a concurrent guest read never sees a torn record | `core/tests/test_pvclock.c: test_write_time_info_version_and_fields`, `test_write_wall_clock` |

### Hyper-V hypercall page (`arch/x86_64/cpu/hyperv.c`)

| Guest-writable field | Check | What it verifies | Test |
|---|---|---|---|
| Hypercall-page-enable MSR (GPA it names) | `hype_hv_hypercall_page_write()` (`:19`), via `hype_gpa_to_host()` | GPA + `HYPE_HV_HYPERCALL_PAGE_SIZE` fits the mapped range; a rejected translation leaves the page untouched and the MSR value unchanged | `core/tests/test_hyperv.c: test_invalid_full_page_is_rejected_without_changes` |
| Locked/disabled/missing-OS-ID states | Same function | A locked MSR ignores further writes; a disabled or OS-ID-less write clears the enable bit without touching the page | `test_hyperv.c: test_disabled_and_missing_os_id_do_not_touch_page`, `test_locked_and_identity_clear` |

### Hyper-V reference-TSC page (`arch/x86_64/cpu/hyperv.c`)

| Guest-writable field | Check | What it verifies | Test |
|---|---|---|---|
| Reference-TSC MSR (GPA it names) | `hype_hv_reference_tsc_write()` (`:63`), via `hype_gpa_to_host()` | GPA + 4096 bytes fits the mapped range before the page is zeroed and rewritten | `core/tests/test_hyperv.c` (closed **#670**: mirrors the existing hypercall-page tests in the same file) |

`hype_hv_reference_tsc_write()` is called only from `arch/x86_64/svm/svm_vcpu.c`
(`:2166`); no equivalent call exists in `arch/x86_64/vmx/vmcs_hw.c`, so a
guest writing `HV_X64_MSR_REFERENCE_TSC` on the Intel/VMX backend gets no
reference-TSC page at all, only on AMD/SVM. This is a functional parity gap
distinct from the test-coverage gap above; see **#670**'s comments for
follow-up.

## 8. Out of scope

- **SMBIOS** (`core/smbios.c`) — host-only table construction; a guest never
  writes anything here.
- **ACPI table loader / e820** (`devices/acpi_loader.c`, `devices/e820.c`) —
  host-side table builders serialized for the guest to read via fw_cfg; no
  guest-supplied address or length is consumed by either file.
- **Host-side drivers** (`core/ahci_host.c`, `core/nvme_host.c`,
  `core/blk_phys.c`, `core/blk_usb.c`, `core/xhci.c`/`core/xhci_hw.c`) — hype
  acting as a driver against real physical hardware on the host's behalf, not
  guest input.
- **Operator-side surfaces** (`hype.cfg`, the dashboard/terminal) — covered
  by `core/tests/test_cfg.c` and out of this audit's scope per #610's
  non-goals; they are operator input, not guest input.

## Coverage gaps and defects found by this audit

**Status as of this update: every gap and defect this audit originally found
has since been closed with a real fix or test** (verified against each
ticket's actual closing commit, not just its board status, on 2026-08-23 —
see the per-row citations above). Kept as history rather than deleted: the
next audit pass should be able to see what this one found and how it was
resolved, not just a clean present-tense table.

- **Device/interface families documented: 27** (the `###` headings in
  sections 1-7; four more families are listed in §8 as confirmed out of
  scope).
- **Guest-writable fields/structures enumerated (table rows): 87.**
- **Check and test both proven, as of this update: all 85 §6j-applicable
  rows** (87 total rows minus the 2 ACPI PM1a_CNT/reset-control rows, which
  are not §6j findings at all; see below).
- **Check missing entirely (a real defect): 1 row, found and fixed as
  #672.**

Testing gaps this audit found, all now closed with a real test added:

- **#661**, **#663** — AHCI/ATAPI command-list/command-table/PRDT GPA checks
  → `core/tests/test_ahci_dma.c`
- **#662**, **#665** — guest xHCI ring/context/TRB-buffer GPA checks
  → `core/tests/test_xhci_dev.c`
- **#664**, **#667** — fw_cfg DMA GPA checks → new `hype_fw_cfg_dma_op_run()`
  extraction + `core/tests/test_fw_cfg.c`
- **#669** — NVMe `boot/main.c` GPA checks → new `hype_gpa_read()`/
  `hype_gpa_write()` extraction (`core/guest_mem.c`) + `test_guest_mem.c`
- **#666** — ramfb surface GPA check → closed as "nothing to extract": the
  call site has no separable logic beyond a direct `hype_gpa_to_host()` call,
  already covered by `test_guest_mem.c`
- **#670** — Hyper-V reference-TSC page GPA check → `test_hyperv.c`
- **#671** — kvmclock system-time/wall-clock GPA checks → new
  `hype_pvclock_arm_system_time()`/`_arm_wall_clock()` extraction
  (`devices/pvclock.c`) + `test_pvclock.c`
- **#668** — ACPI PM1a_CNT/reset-control register handlers → closed as
  **not a §6j finding at all**: neither handler ever dereferences a
  guest-supplied address, so this audit's own template doesn't apply to
  them (see the row itself for the closing rationale)

Structural root cause of the above (still open — a process fix, not a
per-surface one):

- **#659** — `svm_vcpu.c`/`vmcs_hw.c` are blanket coverage-exempt, hiding
  pure, host-testable device logic inside two files totaling ~20% of
  `core/`+`arch/`'s source. Every extraction above (#664/#667/#669) worked
  around this one file at a time; #659 is the ticket to fix the pattern
  itself so the next device doesn't need its own extraction ticket.

Defects found (a check was missing or wrong, not merely untested) — both fixed:

- **#672** — `process_ahci_ata_command_slot()` dereferenced the command-list
  GPA translation with no null check (guest-triggerable NULL dereference) —
  fixed in commit `8efc71d`
- **#656** — ATAPI READ(12) byte-length arithmetic could overflow in 32 bits
  (correctness bug, not a memory-isolation escape; found by **#613**) — fixed
  in commit `c6a1442`

Related audits, not superseded by this document:

- **#613** — guest-facing storage device model bounds-check consistency
  (AHCI/ATAPI/ata_disk/virtio-blk)
- **#616** — pure/`_hw` split consistency across every backend pair
- **#602** — fuzzing the same guest-facing surfaces this document enumerates
