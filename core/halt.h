#ifndef HYPE_HALT_H
#define HYPE_HALT_H

/*
 * Halts the CPU forever via `hlt` in a loop. Never returns.
 *
 * Exempt from unit testing per AGENTS.md: this only makes sense with a
 * real CPU privilege transition (`hlt` traps outside ring 0) and, being
 * noreturn, calling it would simply hang a test process rather than
 * exercise any decision logic. Kept to the minimum -- one instruction in
 * a loop -- with all actual panic decision logic (what to print, when to
 * call this) living in the testable hype_panic_message() in panic.c.
 */
__attribute__((noreturn)) void hype_halt_forever(void);

#endif /* HYPE_HALT_H */
