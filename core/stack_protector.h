#ifndef HYPE_CORE_STACK_PROTECTOR_H
#define HYPE_CORE_STACK_PROTECTOR_H

/* #604: TSC-derived reseed, called once as the first statement in efi_main() --
 * __stack_chk_guard otherwise starts at its link-time constant, the same value on
 * every boot. */
void hype_stack_protector_init(void);

#endif /* HYPE_CORE_STACK_PROTECTOR_H */
