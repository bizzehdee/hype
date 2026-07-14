#include "pic.h"

#define HYPE_PIC_MASTER_COMMAND 0x20u
#define HYPE_PIC_MASTER_DATA 0x21u
#define HYPE_PIC_SLAVE_COMMAND 0xA0u
#define HYPE_PIC_SLAVE_DATA 0xA1u

static void chip_reset(hype_pic_emu_chip_t *chip) {
    chip->imr = 0xFFu; /* fully masked at power-on, matching real hardware */
    chip->irq_offset = 0;
    chip->init_state = 0;
    chip->expect_icw4 = 0;
    chip->is_cascade = 0;
    chip->read_isr_select = 0;
    chip->irr = 0;
    chip->isr = 0;
}

void hype_pic_emu_reset(hype_pic_emu_t *pic) {
    chip_reset(&pic->master);
    chip_reset(&pic->slave);
}

static void chip_write_command(hype_pic_emu_chip_t *chip, uint8_t value) {
    if (value & 0x10u) {
        /* ICW1: begin a fresh initialization sequence. */
        chip->expect_icw4 = value & 0x01u;
        chip->is_cascade = (value & 0x02u) == 0; /* SNGL=0 means cascade mode (ICW3 follows) */
        chip->init_state = 1;
        chip->imr = 0;
        chip->irr = 0;
        chip->isr = 0;
        return;
    }

    if (value & 0x08u) {
        /* OCW3. */
        if (value & 0x02u) { /* RR (read register) bit set */
            chip->read_isr_select = value & 0x01u;
        }
        return;
    }

    /* OCW2 (EOI and rotate commands -- only EOI matters for this stub). */
    if (value & 0x20u) {
        if (value & 0x40u) {
            /* Specific EOI: bits 2:0 name the IRQ to clear. */
            uint8_t irq = value & 0x07u;
            chip->isr &= (uint8_t) ~(1u << irq);
        } else {
            /* Non-specific EOI: clear the highest-priority (lowest-numbered) set ISR bit. */
            int i;
            for (i = 0; i < 8; i++) {
                if (chip->isr & (1u << i)) {
                    chip->isr &= (uint8_t) ~(1u << i);
                    break;
                }
            }
        }
    }
}

static void chip_write_data(hype_pic_emu_chip_t *chip, uint8_t value) {
    switch (chip->init_state) {
        case 1: /* ICW2: vector offset. */
            chip->irq_offset = value;
            chip->init_state = chip->is_cascade ? 2 : (chip->expect_icw4 ? 3 : 0);
            return;
        case 2: /* ICW3: cascade wiring -- content unused by this stub. */
            chip->init_state = chip->expect_icw4 ? 3 : 0;
            return;
        case 3: /* ICW4: mode bits -- content unused by this stub. */
            chip->init_state = 0;
            return;
        default: /* OCW1: mask register. */
            chip->imr = value;
            return;
    }
}

int hype_pic_emu_io_write(hype_pic_emu_t *pic, uint16_t port, uint8_t value) {
    switch (port) {
        case HYPE_PIC_MASTER_COMMAND:
            chip_write_command(&pic->master, value);
            return 0;
        case HYPE_PIC_MASTER_DATA:
            chip_write_data(&pic->master, value);
            return 0;
        case HYPE_PIC_SLAVE_COMMAND:
            chip_write_command(&pic->slave, value);
            return 0;
        case HYPE_PIC_SLAVE_DATA:
            chip_write_data(&pic->slave, value);
            return 0;
        default:
            return -1;
    }
}

static uint8_t chip_read_command(const hype_pic_emu_chip_t *chip) {
    return chip->read_isr_select ? chip->isr : chip->irr;
}

int hype_pic_emu_io_read(hype_pic_emu_t *pic, uint16_t port, uint8_t *out_value) {
    switch (port) {
        case HYPE_PIC_MASTER_COMMAND:
            *out_value = chip_read_command(&pic->master);
            return 0;
        case HYPE_PIC_MASTER_DATA:
            *out_value = pic->master.imr;
            return 0;
        case HYPE_PIC_SLAVE_COMMAND:
            *out_value = chip_read_command(&pic->slave);
            return 0;
        case HYPE_PIC_SLAVE_DATA:
            *out_value = pic->slave.imr;
            return 0;
        default:
            return -1;
    }
}

void hype_pic_emu_raise_irq(hype_pic_emu_chip_t *chip, uint8_t irq) {
    if (irq > 7u) {
        return;
    }
    chip->irr |= (uint8_t)(1u << irq);
}
