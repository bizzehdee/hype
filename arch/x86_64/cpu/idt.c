#include "idt.h"

void hype_idt_encode_entry(hype_idt_entry_t *entry, uint64_t handler,
                            uint16_t selector, uint8_t ist, uint8_t type_attr) {
    entry->offset_low = (uint16_t)(handler & 0xFFFFu);
    entry->selector = selector;
    entry->ist = (uint8_t)(ist & 0x07u);
    entry->type_attr = type_attr;
    entry->offset_mid = (uint16_t)((handler >> 16) & 0xFFFFu);
    entry->offset_high = (uint32_t)((handler >> 32) & 0xFFFFFFFFu);
    entry->reserved = 0;
}

void hype_idt_build(hype_idt_entry_t *table, const uint64_t *stub_table, uint16_t code_selector) {
    unsigned int i;
    for (i = 0; i < HYPE_IDT_ENTRY_COUNT; i++) {
        hype_idt_encode_entry(&table[i], stub_table[i], code_selector, 0,
                               HYPE_IDT_TYPE_INTERRUPT_GATE);
    }
}
