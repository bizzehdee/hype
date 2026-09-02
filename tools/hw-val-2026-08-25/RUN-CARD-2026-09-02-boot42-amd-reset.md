# Boot 42 (AMD 5950X) -- does a real controller stall recover on its own (#786)

Build: default, no `EXTRA_CFLAGS`. First commit after `d3dee9f` carrying the #781 series
(#782 injection, #783 refusal, #784 teardown, #785 reset). Config unchanged from boot 41.

## What boot 41 settled

- The input death is the controller: every register healthy (not halted, no HSE/HCE, command
  ring Running, root port 4 in U0, event ring consumed), yet a No-Op never completed and
  Command Abort left CRR set. Only a host-controller reset can recover it.
- #791 is fixed; #787's 387 s Pico drop is unchanged; #788 got worse (3.0 per 1,000).

## What changed since boot 41

- **#785.** When a controller's command ring cannot be restarted (the abort fails, or the
  controller raises HSE/HCE), hype releases everything on it, issues `HCRST`, rebuilds its
  rings, re-scans its root ports, walks its hubs and re-claims its keyboards through the same
  path an arrival uses. Bounded: 3 resets per controller per run, then it is left dead and
  says so. Under QEMU (tools/781) an injected wedge on the input controller recovered in
  1.2 s with the hub, the keyboard and the mouse back and reporting.
- **#783.** The controller carrying the log sink or the boot medium is never reset. One line
  at boot says which: `fw-1 XHCIOWN: log sink on ctrl[N], boot medium on ctrl[M]`. On this
  machine both should be ctrl[1]; the keyboards are on ctrl[2].
- The probe's No-Op is what asks for the reset on a silent stall: 30 s of silence, a No-Op
  that fails, a 5 s abort that fails, then the next input tick resets the controller.

## Before you boot

- Confirm the banner sha matches the staged build.
- Confirm `XHCIOWN: log sink on ctrl[1], boot medium on ctrl[1]` -- if it says ctrl[2], the
  keyboards' controller is protected and nothing will be reset; stop and report.
- Confirm the first `HIDTICK` line says `silence_revive=0`.

## The sequence

Unchanged from boot 41.

1. Boot, stay on the dashboard.
2. **Right-Ctrl + Right-Alt + 1** (both modifiers right-hand).
3. Wait for `localhost login:`, type **`root`** and Enter, get `localhost:~#`.
4. **BOOTSEL once** -- LED solid, `a0001` appears in the guest.
5. Leave it. The stall came at 291 s, 2450 s and 1262 s in the last three runs. About 36 s
   after the keyboards die you should see them come back: the Pico types again on its own,
   and the guest shows new `a00NN` markers. **Keep typing on the Logitech after it comes back**
   -- that is the human-keyboard half of the evidence.
6. Leave it at least 20 minutes past the first recovery if the machine can be spared: the
   bound (3 resets) and whether the stall recurs are the second thing this run measures.
7. Power off normally. Bring back `HYPE.LOG` and `RUN1A.LOG`.

## What to read

| Ticket | What to read | It says |
| --- | --- | --- |
| **#786** | `fw-1 XHCIRESET ctrl[2]: reset #1 done in N ms \| released ... \| back: ports= devices= keyboards= mouse=` | the reset ran and what came back; `keyboards=` should be 3 (Keychron, Logitech, Pico) |
| **#786** | `HIDTICK[..] ... reports=` for the Pico and the Logitech climbing again after the reset line, and new `a00NN` markers in `KBDCHARS` | the recovered controller delivers events |
| **#786** | `fw-1: boot #N` at the top and the `XHCIRESET` line itself in `HYPE.LOG` | the log on ctrl[1] survived the reset of ctrl[2] |
| **#785** | more than one `XHCIRESET ctrl[2]: reset #` line, and their spacing | whether the stall recurs, and how fast; `left dead` after the third is the bound holding |
| **#783** | `XHCIRESET ctrl[1]: REFUSED` | should NOT appear; if it does, ctrl[1] stalled and the refusal worked |
| **#781** | `CTRLSILENCE ctrl[2]: ... No-Op FAILED` immediately before the first reset | the trigger, same as boot 41 |
| **#787** | `hub slot 3 port 3 changed -- now empty` | the Pico's 387 s drop, unchanged if Pico-side; note whether the stall again follows a re-arrival |
| **#788** | doubled characters per 1,000 in `KBDCHARS` | boot 41 was 3.0, boot 40 0.35 |

If the stall does not recur, that is a run with no result for #786, not a pass. Say so.
