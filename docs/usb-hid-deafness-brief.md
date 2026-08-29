# USB HID interrupt-IN endpoints go permanently silent on real hardware

**Status:** root cause identified and fixed in software; awaiting a hardware run to confirm.
**Date:** 2026-08-29. **Tree:** `hype` @ `86fd3a8` (analysis), fix on top.

> **Update after expert review.** Two independent xHCI reviewers read this brief. Both
> converged on the hypothesis in §4 — a lossy completion path, not a controller fault — and
> both ranked it above the TT and hardware theories. The fix is described in §10, and §11
> records which of the reviewers' specific claims did not survive checking against the
> source, so nobody re-litigates them.

---

## 1. The symptom in one paragraph

`hype` is a bare-metal UEFI type-1 hypervisor. After `ExitBootServices` it owns the
xHCI controllers directly and drives USB HID itself, by polling. On the reporter's
AMD 5950X desktop, a USB keyboard works for somewhere between zero and thirty seconds
and then stops delivering input **permanently** for the rest of the boot. Unplugging
and re-plugging the keyboard, or the hub it sits behind, does not recover it. The same
keyboard, on the same ports, on the same machine, works correctly under Linux, FreeBSD
and Windows.

The failure is **silent**. hype's own counters show the endpoint being polled
thousands of times, zero reports arriving, and **zero errors** — no `STALL`, no
`Babble`, no `USB Transaction Error`, no halted endpoint.

```
fw-1 DIAG: HID[0/2] 3434:0da4 polls=5779 reports=0 errors=0 (slot4 ep=0x81)
fw-1 DIAG: HID[1/2] 046d:c547 polls=5779 reports=15 errors=0 (slot5 ep=0x82)
```

Those two lines are from the end of one real boot (boot 18). Both HID endpoints are on
the same controller. One never produced a single report. The other produced 15 and then
stopped — its count was already 15 at poll 2364 and unchanged at poll 5779.

---

## 2. The machine and its USB topology

Two xHCI controllers, 12 USB devices, 4 external hub devices plus root hubs. hype's own
enumeration, from the same boot:

| # | ctrl | root port | route string | slot | speed | VID:PID | class | owner |
|---|---|---|---|---|---|---|---|---|
| 0 | 1 | 4 | 0x00000 | 1 | SS | `152d:1561` JMicron SATA bridge | 08/06/50 | hype (boot + log medium) |
| 1 | 1 | 6 | 0x00000 | 2 | HS | `05e3:0608` Genesys hub | 09/00/01 | free |
| 2 | 1 | 6 | 0x00004 | 3 | FS | `1e71:2007` NZXT | 03/00/00 | free |
| 3 | 1 | 12 | 0x00000 | 4 | FS | `1462:7c91` MSI Mystic Light | 03/00/00 | free |
| 4 | 2 | 1 | 0x00000 | 0 | HS | `1bcf:2284` UGREEN camera | ef/02/01 | free |
| 5 | 2 | 3 | 0x00000 | 1 | HS | `0bda:5411` Realtek hub | 09/00/02 | free |
| 6 | 2 | 4 | 0x00000 | 2 | HS | `05e3:0610` Genesys hub | 09/00/01 | free |
| 7 | 2 | 4 | 0x00001 | 3 | HS | `eba4:6579` UGREEN camera 4K | ef/02/01 | free |
| **8** | **2** | **4** | **0x00002** | **4** | **FS** | **`3434:0da4` Keychron K10 v2** | **03/01/01** | **hype** |
| **9** | **2** | **4** | **0x00004** | **5** | **FS** | **`046d:c547` Logitech receiver** | **03/01/02** | **hype** |
| 10 | 2 | 7 | 0x00000 | 6 | SS | `0bda:0411` Realtek hub | 09/00/03 | free |
| 11 | 2 | 8 | 0x00000 | 7 | SS | `05e3:0626` Genesys hub | 09/00/03 | free |

The two devices that fail are both **Full Speed devices behind a High Speed hub**
(route `0x00002` and `0x00004` off root port 4 of controller 2), so both need a
Transaction Translator. hype does program `TT Hub Slot ID` and `TT Port Number` in the
Slot Context for these; that path exists and was checked.

### The keyboard

`3434:0da4` Keychron K10 v2, Full Speed, **three HID interfaces**:

| iface | class/sub/proto | endpoints |
|---|---|---|
| 0 | 3 / 1 / 1 — **boot keyboard** | `0x81` IN, interrupt, mps 8, bInterval 1 |
| 1 | 3 / 0 / 0 | `0x82` IN mps 32, `0x02` OUT mps 32, bInterval 1 |
| 2 | 3 / 0 / 0 | `0x83` IN mps 32, bInterval 1 |

hype claims interface 0 only: `SET_CONFIGURATION(1)`, `SET_PROTOCOL(boot)` on
interface 0, `Configure Endpoint` for DCI 3, then polls `0x81` for 8-byte boot reports.

### The Logitech receiver

`046d:c547`, Full Speed, composite: a keyboard endpoint (`0x82`) **and** a mouse
endpoint (`0x81`) on one slot — two DCIs on slot 5.

---

## 3. How hype drives these endpoints

This is the part most likely to contain the bug, so it is described in full.

### No interrupts

hype does **not** enable xHCI interrupts and has no ISR. It polls. There is one
Interrupter (IR0), one Event Ring Segment, and `ERSTSZ = 1`.

### The event ring is 16 TRBs

```c
#define RING_TRBS 16u                          /* core/xhci_hw.c:53 */
uint8_t evt_ring[XPAGE] __attribute__((aligned(XPAGE)));   /* 4 KiB allocated */
put_le32(hw->erst + 8, RING_TRBS);             /* but segment size = 16 TRBs */
```

The backing allocation is a 4 KiB page (256 TRBs) but the segment size written to the
ERST, and the wrap modulus used by the dequeue cursor, are both 16. Every transfer ring
is also 16 TRBs with a Link TRB in the last slot.

`ERDP` is written (with EHB set) after every event consumed.

### The poll loop

The BSP runs a dispatch loop and calls `fw_1_host_input_poll()` from it. That function
is rate-limited to **125 Hz** by a TSC gate. Each tick it does two things:

1. `usb_hid_drain()` — for each claimed HID endpoint, one call to hype's interrupt-IN
   poll (`int_in_poll`), described below.
2. `fw_1_usb_hotplug_poll()` — for each controller, `hype_xhci_pump_events(xc, 16)`,
   then a root-port and hub-port change sweep.

On controller 2 hype polls **7 interrupt-IN endpoints**: 3 HID (keyboard `0x81`,
receiver `0x82`, receiver `0x81`) and 4 hub status-change endpoints.

### The interrupt-IN poll

Per endpoint, per tick, `int_in_poll()`:

1. Tops the endpoint's outstanding queue back up to `HYPE_INT_IN_DEPTH = 4` TRBs
   (each with its own 64-byte report buffer), ringing the doorbell for each.
2. Checks a single-slot "handed to me" flag (`have_completion` + `comp_cc`).
3. Checks an 8-entry "parked" table for a completion matching its **head** TRB.
4. Otherwise **peeks exactly one** event ring entry (`XHCI_POLL_PEEK = 1` — one read
   of the cycle bit; it does not spin).
5. If that event is for a different (slot, DCI), it is routed — see below — and the
   poll returns idle.

### Foreign-event routing — the suspected loss channel

Because one shared event ring serves every endpoint, a poll for endpoint A routinely
dequeues an event for endpoint B. `route_foreign_event()` handles it:

```c
other = iin_hw_for(c, oslot, odci, 0);
if (other && int_in_head_matches(other, hype_xhci_event_trb_ptr(evt))) {
    other->comp_cc = hype_xhci_event_cc(evt);   /* single slot, not a queue */
    other->have_completion = 1;
    other->handed++;
    return;
}
if (other) other->to_park++;
hype_xhci_parked_put(&hw->parked, oslot, odci, trb, cc, residue);
```

`int_in_head_matches()` compares only against `pending_trb[0]` — the oldest outstanding
transfer — because an interrupt ring completes in order. That strictness was added
deliberately (#766) after a bug where a stale event was claimed as the current transfer,
`armed` cleared, and the enqueue pointer ran ahead of the controller's dequeue pointer.

`hype_xhci_parked_put()` is an **8-entry table with silent round-robin eviction**:

```c
/* Full: evict round-robin. */
i = p->next % HYPE_XHCI_PARKED_MAX;   /* HYPE_XHCI_PARKED_MAX == 8 */
```

**An evicted interrupt-IN completion is unrecoverable.** hype clears `armed` only on a
claimed completion, so the transfer stays counted as outstanding forever, the endpoint
is never re-armed past it, and it goes deaf for the rest of the boot. hype's own comment
says so. There is **no counter for eviction**.

---

## 4. Leading hypothesis (unproven)

Three facts line up:

1. `have_completion` is a **single slot**, not a queue, while up to **4** transfers are
   outstanding per endpoint.
2. The parked table is **8 entries** and evicts silently, while **7 interrupt-IN
   endpoints** on controller 2 share it.
3. `hype_xhci_pump_events(xc, 16)` drains up to 16 events per controller per tick and
   pushes every non-head-matching one into that same 8-entry table.

So: if two completions for the same endpoint are routed between two polls of that
endpoint — which is exactly what a burst drain produces — the first sets
`have_completion` and the second cannot match the head (the head is not retired until
the owning endpoint next polls). The second goes to the parked table. With 7 endpoints
feeding an 8-entry evicting table, an entry that is not claimed within a tick or two is
overwritten. One eviction of a HID completion = that keyboard is deaf for the boot.

This predicts the exact observed signature: intermittent onset, correlated with bus
activity, permanent, `errors=0`, and no recovery on re-plug (because hype cannot see
the unplug either — the hub's status-change endpoint is one of the seven).

It also predicts that the fix intended to make the system *more* robust made it *worse*:
raising the outstanding depth from 1 to 4 (#775) increases the number of completions in
flight per endpoint, and therefore the number that can land off-head.

**This is a hypothesis. It has not been confirmed on hardware.** The counters that would
confirm or refute it (`handed`, `deliv`, `own`, `topark`) were added to the diagnostic
line after the last hardware run, so they have never been read on the failing machine.
There is still no counter at all for the eviction itself, which is the actual loss.

---

## 5. What has been ruled out, with evidence

| Ruled out | Evidence |
|---|---|
| Endpoint halted / stalled | `errors=0` across every run; no error completion code ever logged for these endpoints |
| Wrong interface or protocol claimed | The same code path works on other boots and on other machines; `SET_PROTOCOL(boot)` on interface 0 succeeds and is logged |
| Missing TT setup for FS-behind-HS | `TT Hub Slot ID` / `TT Port Number` are programmed from the parent hub's slot and port |
| Wrong interval encoding | `hype_xhci_interval_encode()` is speed-aware (FS/LS interrupt encoded as log2 frames + 3, HS as `bInterval - 1`) |
| Context Entries too low | Fixed (#755): all three Slot Context writers now use `max(existing, dci)`, which matters for the composite receiver |
| ERDP never advanced | ERDP is written with EHB after every consumed event |
| Transfer events being dropped outright | Fixed (#761): every foreign transfer event is now routed or parked, never discarded |

**Not** ruled out, and not currently measurable from hype:

- Whether the controller stopped posting events at all (no `USBSTS.HSE` check, no
  handling of a Host Controller Event / `Event Ring Full Error` TRB).
- Whether hype's enqueue pointer has run ahead of the controller's dequeue pointer.
  The TR Dequeue Pointer in the Output Endpoint Context is only architecturally valid
  on a **Stopped or Halted** endpoint (xHCI 4.12.2); on a Running one this controller
  returns the ring base. A diagnostic built on reading it produced five false positives
  across two boots, on endpoints that were working, and was withdrawn.

---

## 6. Fixes already shipped that did not resolve it

Each was a real defect, found and fixed, and none stopped the deafness:

| # | Defect |
|---|---|
| 755 | Slot Context `Context Entries` lowered by a later Configure Endpoint, unconfiguring a sibling endpoint on a composite device |
| 757 | Stale port-change bits from enumeration swept as spurious departures |
| 759 | Idle poll spun 19,531 times per endpoint per tick; at 8 endpoints this throttled the 125 Hz tick to single-digit Hz |
| 761 | Foreign transfer events were dropped outright |
| 762 | Only `C_PORT_CONNECTION` cleared on a hub port, so other change bits latched the hub's status endpoint on forever |
| 763 | A device that returns cc=4 to `Address Device` was retried forever |
| 765 | Hub and endpoint pools too small for 12 devices |
| 766 | A completion could be claimed by a transfer it did not name; measured on hardware as four endpoints with the controller's dequeue at the ring base and hype ten TRBs ahead |
| 768 | HID claim ran against the wrong controller index |
| 769 | A controller with no claimed interrupt-IN endpoint was never drained, so its root ports could never hot-plug |
| 770/771 | Duplicate endpoint claims; a hub port hype had given up on kept re-triggering |
| 775 | Only one transfer outstanding per endpoint, so a single lost completion killed the endpoint permanently |

---

## 7. QEMU

A rig reproducing this topology exists (`tools/767/run-767-qemu.sh`: two controllers,
four hub devices, a nested hub, boot medium on one controller and all HIDs on the
other, with two added QEMU device models — a composite keyboard+mouse and a device that
refuses `Address Device`). It reproduces several of the fixed defects. **It does not
reproduce this one.** A QEMU run that appeared to reproduce it turned out to be the rig
failing to deliver `sendkey` to the emulated keyboard; the routing counters read
`handed=0 deliv=0 own=0 topark=0`, meaning no completion was ever generated rather than
lost.

Known genuine QEMU/hardware divergences are catalogued in
[`docs/qemu-vs-hardware.md`](qemu-vs-hardware.md). The relevant ones: QEMU keeps the TR
Dequeue Pointer live in a Running endpoint context and real hardware need not; QEMU
reuses slot ids with no fault where this controller fails every transfer on a recycled
slot id with cc=4; QEMU delivers events far more promptly and in order.

---

## 8. Questions for the expert

1. On a Running, non-halted interrupt-IN endpoint with valid TRBs enqueued and the
   doorbell rung, what causes a real xHCI controller to stop generating Transfer Events
   with no error indication? What should software check to distinguish "the controller
   stopped" from "software lost the event"?

2. Is a **16-TRB event ring with a single Interrupter, drained by polling at 125 Hz**,
   defensible for a controller with 7 active interrupt-IN endpoints at `bInterval` 1?
   What is the failure mode when it overflows, and how does software detect it?

3. Is the head-only TRB match (`pending_trb[0]`) correct for an interrupt-IN endpoint
   with 4 transfers outstanding, given events can be delivered late and out of order?
   Should hype match against any outstanding TRB and retire up to it instead?

4. Once an endpoint is suspected deaf, what is the correct recovery? The intended one is
   `Stop Endpoint` (which makes the TR Dequeue Pointer valid to read), then
   `Set TR Dequeue Pointer`, then re-arm. Is that sufficient, and what are the ordering
   and cycle-state requirements?

5. Is there anything about **Full Speed HID behind a High Speed hub on AMD's 5950X
   xHCI** — split transactions, TT bandwidth, the `MTT` bit, TT Think Time — that
   commonly goes wrong in a hand-written driver and presents as silent NAK-forever?

---

## 9. Where the code is

| Thing | Location |
|---|---|
| Interrupt-IN poll | `core/xhci_hw.c` — `int_in_poll()`, ~line 1930 |
| Arm / fill / retire | `core/xhci_hw.c:1849-1900`, `int_in_head_matches()` :818, `int_in_retire_head()` :823 |
| Foreign-event routing | `core/xhci_hw.c:633` `route_foreign_event()` |
| Parked table | `core/xhci.c:286` `hype_xhci_parked_put()`, size at `core/xhci.h:841` |
| Event ring init and ERDP | `core/xhci_hw.c:2894-2910`, `next_event_budget()` :478 |
| Slot/endpoint contexts | `core/xhci.c:129` `hype_xhci_slot_ctx()`, :633 `hype_xhci_interval_encode()` |
| 125 Hz input tick | `boot/main.c:8397` `fw_1_host_input_poll()` |
| Hot-plug sweep + pump | `boot/main.c:25025` `fw_1_usb_hotplug_poll()` |
| HID diagnostic line | `boot/main.c:16453` |
| Hardware logs | `tools/hw-val-2026-08-25/logs/boot-{8..19}/HYPE.LOG` |
| Host `lsusb -v` | `/home/darren/lsusbv.txt` |

Logs contain invalid UTF-8; read them with `LC_ALL=C grep -a`.


---

## 10. The fix

Landed together, because the first is meaningless without the second.

### 10.1 Retire where the event is routed, into a per-endpoint lossless queue

This is the actual defect. hype retired the head at **poll** time, so between routing TRB0's
completion and that endpoint's next poll the head still read TRB0 — and TRB1's completion,
arriving in the same drain pass, could not match it. It went to the shared 8-entry parked
table, which evicts round-robin under seven endpoints, and an evicted interrupt-IN
completion is unrecoverable because `armed` clears only on a claim.

`int_in_deliver()` now retires at the moment a completion is routed, wherever it was
dequeued, and queues the completion code and report buffer on a per-endpoint FIFO of
`HYPE_INT_IN_DEPTH` entries — exactly enough, since at most that many transfers can be
outstanding. `have_completion`/`comp_cc` (a queue of depth one) and the interrupt-IN use of
the parked table are both gone. Matching is against the whole outstanding set rather than
the head alone, which keeps the strict TRB attribution of #766 while surviving a completion
observed after the one behind it; an in-order match at index *k* retires 0..*k* and delivers
their reports rather than discarding them.

### 10.2 Rings sized to the page that was already allocated

`RING_TRBS` 16 → 256. Every ring already lived in its own 4 KiB page; only 16 TRBs of it
were used. On the **event** ring this removes the overflow risk outright: the worst case
between drains is `HYPE_INT_IN_DEPTH × interrupt-IN endpoints` = 28 on this desktop, because
an endpoint with its queue full cannot produce another event until hype retires one. 28
against 16 slots was a real hazard; against 256 it is not, which is why the 125 Hz drain
cadence is left alone. On the **transfer** rings it lengthens physical TRB-address reuse
from every 15 submissions to every 255, so stale and current completions alias far less.

### 10.3 The instrumentation that was missing

- `parked.evictions` — the fatal event had no counter at all. Unit-tested.
- Per-endpoint `lost` (a completion naming a TRB not outstanding — stale, correctly refused)
  and `skipped` (a transfer retired because a later one completed). Both should stay zero.
- **Host Controller Event** detection at the one point events leave the ring, naming
  `Event Ring Full Error` (CC 21) explicitly. A non-zero count is the controller saying it
  stopped, which no counter could previously distinguish from software losing completions.

The HID diagnostic line now carries `lost`, `skipped`, `hcevt`, `ringfull` and `evict`
alongside the routing counters.

### 10.4 Deliberately not done yet

Automatic `Stop Endpoint` → `Set TR Dequeue Pointer` → re-arm recovery. Both reviewers
described it correctly, and one added the caveat that matters: if the Event Ring is full the
controller has stopped processing the Command Ring, so the recovery command itself will not
execute. Recovery must therefore come after the event-ring state is diagnosed, not before —
and the counters in §10.3 are what will say whether it is needed at all.

TT and split-transaction scheduling are untouched. Both reviewers ranked them last, and the
signature (15 clean reports, then nothing, `errors=0`) does not match a split failure, which
presents as transaction or split completion codes.

---

## 11. Reviewer claims that did not survive checking

Recorded so they are not re-litigated. All five were checked against the source.

| Claim | Verdict |
|---|---|
| “7 endpoints at `bInterval` 1 generate 7 events/ms, so 56 land in an 8 ms window” | **Wrong.** An interrupt IN endpoint with no data NAKs, and a NAK posts no Transfer Event — an idle keyboard produces nothing. The real bound is queue depth, not poll rate: `DEPTH × endpoints` = 28. Still exceeds 16, so the conclusion (ring undersized) holds; the arithmetic does not. |
| “`Interval` must be `log2(bInterval) + 3` for Full Speed; programming 0 or 1 breaks AMD split scheduling” | **Already correct.** `hype_xhci_interval_encode()` (`core/xhci.c:633`) does exactly this for FS/LS, clamped to 3..10. `bInterval` 1 encodes as 3. |
| “The Genesys hubs report Multi-TT; hype sets `MTT = 0`, so AMD fails to allocate split schedules” | **Wrong on this topology.** The keyboard's parent is `05e3:0610`, enumerated by hype as class `09/00/01` — `bDeviceProtocol` 1, **single TT**. The MTT-capable hub (`0bda:5411`, protocol 02) carries neither failing device. A hub only operates MTT if the host selects alternate interface setting 1, which hype never does, so `MTT = 0` is the correct programming. |
| “`TT Hub Slot ID` must point at the HS parent, not an intermediate hub” | **Already correct.** `core/xhci_hw.c:1567` and `:3205` select the closest HS hub when `hype_xhci_tt_required()`, and otherwise inherit the parent's TT. |
| Suggested `int_in_match_and_retire()` | **Not usable as written.** It decrements `pending_count` and `armed` as separate quantities; in hype `armed` *is* the count. It also retires without delivering the report data — for a HID keyboard, whose boot report is a state snapshot rather than an event log, silently dropping intermediate reports loses keystrokes. |

One correction to the second review as well: it attributes the head-match failure to software
observing completions out of sequence. The precise cause is narrower — retirement was
deferred to poll time, so the head was stale by construction. Retiring at routing time fixes
it without needing any out-of-order tolerance; matching the whole outstanding set is kept as
cheap insurance, not as the mechanism.
