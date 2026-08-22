#include "pic.h"

#define HYPE_PIC_MASTER_COMMAND 0x20u
#define HYPE_PIC_MASTER_DATA 0x21u
#define HYPE_PIC_SLAVE_COMMAND 0xA0u
#define HYPE_PIC_SLAVE_DATA 0xA1u

static void chip_reset(hype_pic_emu_chip_t *chip) {
    chip->imr = 0xFFu; /* fully masked at power-on, matching real hardware */
    chip->imr_write_count = 0;
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
            if (chip->imr_write_count < 8u) {
                chip->imr_writes[chip->imr_write_count] = value;
            }
            chip->imr_write_count++; /* counts past the kept window on purpose */
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

/*
 * #364: the highest-priority line currently IN SERVICE on `chip`, or 8 when none is.
 *
 * On an 8259 in fully-nested mode (its reset default, and what every PC OS uses) priority runs
 * from IR0 down to IR7, and a pending line only reaches the CPU if it is HIGHER priority than
 * everything already in service. A line of equal or lower priority waits for the EOI.
 */
static int highest_in_service(const hype_pic_emu_chip_t *chip) {
    int i;
    for (i = 0; i < 8; i++) {
        if ((chip->isr & (uint8_t)(1u << i)) != 0u) {
            return i;
        }
    }
    return 8;
}

int hype_pic_emu_acknowledge_highest_priority(hype_pic_emu_chip_t *chip, uint8_t *out_vector) {
    uint8_t pending = chip->irr & (uint8_t)~chip->imr;
    int isr = highest_in_service(chip);
    int i;

    for (i = 0; i < 8; i++) {
        if ((pending & (uint8_t)(1u << i)) == 0u) {
            continue;
        }
        /* #654: this single-chip entry point never got the #364 fully-nested-priority gate its
         * sibling hype_pic_emu_acknowledge() has -- see that function's own comment for the full
         * story. Without it, a line already in service does not block a lower-priority pending
         * line from being acknowledged too, which is not 8259 behaviour and can starve the
         * higher-priority line exactly as #364 originally found. */
        if (i >= isr) {
            return 0; /* every remaining line is lower priority still */
        }
        chip->irr &= (uint8_t) ~(1u << i);
        chip->isr |= (uint8_t)(1u << i);
        *out_vector = (uint8_t)(chip->irq_offset + i);
        return 1;
    }
    return 0;
}

void hype_pic_emu_raise_global_irq(hype_pic_emu_t *pic, uint8_t global_irq) {
    if (global_irq < 8u) {
        pic->master.irr |= (uint8_t)(1u << global_irq);
    } else if (global_irq < 16u) {
        pic->slave.irr |= (uint8_t)(1u << (global_irq - 8u));
    }
}

int hype_pic_emu_acknowledge(hype_pic_emu_t *pic, uint8_t *out_vector) {
    uint8_t m_pending = pic->master.irr & (uint8_t)~pic->master.imr;
    uint8_t s_pending = pic->slave.irr & (uint8_t)~pic->slave.imr;
    uint8_t effective;
    int m_isr = highest_in_service(&pic->master);
    int i;

    /* The slave's INT drives master IR2 (the cascade line), gated by
     * master IR2's own mask -- a slave IRQ can only reach the CPU when
     * master IR2 is unmasked. */
    effective = m_pending;
    if (s_pending != 0 && (pic->master.imr & (uint8_t)(1u << 2)) == 0) {
        effective |= (uint8_t)(1u << 2);
    }

    for (i = 0; i < 8; i++) {
        if ((effective & (uint8_t)(1u << i)) == 0) {
            continue;
        }
        /*
         * #364: FULLY-NESTED PRIORITY. Only a line strictly higher-priority than what is already
         * in service may be acknowledged.
         *
         * hype's caller used to gate delivery on "both ISRs completely clear", which blocks EVERY
         * line while ANY is in service -- so an IRQ1 left in service starved IRQ0, the HIGHEST
         * priority line on the chip, forever. FreeBSD on VMX selected the i8254 as its event timer
         * and then waited for a 100 Hz tick that could never arrive: mISR=0x2 with mIMR=0xff, so
         * nothing would ever EOI the stuck line either. That is not 8259 behaviour -- IR0 preempts
         * IR1 -- and modelling it as a single-in-service chip turned a lost injection into a dead
         * clock.
         */
        if (i >= m_isr) {
            return 0; /* every remaining line is lower priority still */
        }
        if (i == 2 && s_pending != 0) {
            /* Cascade: the winning line is the slave's highest-priority
             * pending IRQ. Put it in service on the slave and mark the
             * cascade (master IR2) in service too. */
            int j;
            int s_isr = highest_in_service(&pic->slave);
            for (j = 0; j < 8; j++) {
                if (j >= s_isr) {
                    break; /* #364: the slave nests by its own priority, same rule */
                }
                if (s_pending & (uint8_t)(1u << j)) {
                    pic->slave.irr &= (uint8_t) ~(1u << j);
                    pic->slave.isr |= (uint8_t)(1u << j);
                    pic->master.isr |= (uint8_t)(1u << 2);
                    *out_vector = (uint8_t)(pic->slave.irq_offset + j);
                    return 1;
                }
            }
        }
        /* A master-line IRQ (IR2 with no slave pending is treated as a
         * plain master line). */
        pic->master.irr &= (uint8_t) ~(1u << i);
        pic->master.isr |= (uint8_t)(1u << i);
        *out_vector = (uint8_t)(pic->master.irq_offset + i);
        return 1;
    }
    return 0;
}
