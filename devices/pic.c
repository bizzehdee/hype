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

int hype_pic_emu_acknowledge_highest_priority(hype_pic_emu_chip_t *chip, uint8_t *out_vector) {
    uint8_t pending = chip->irr & (uint8_t)~chip->imr;
    int i;

    for (i = 0; i < 8; i++) {
        if (pending & (uint8_t)(1u << i)) {
            chip->irr &= (uint8_t) ~(1u << i);
            chip->isr |= (uint8_t)(1u << i);
            *out_vector = (uint8_t)(chip->irq_offset + i);
            return 1;
        }
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
        if (i == 2 && s_pending != 0) {
            /* Cascade: the winning line is the slave's highest-priority
             * pending IRQ. Put it in service on the slave and mark the
             * cascade (master IR2) in service too. */
            int j;
            for (j = 0; j < 8; j++) {
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

/* 8259 priority as a single comparable level (lower = higher priority): a
 * master line i maps to i*10, so the slave's own lines (which cascade in via
 * master IR2) slot between master IR1 and IR3 as 20+j. HYPE_PIC_PRIO_NONE is
 * a sentinel above every real level. */
#define HYPE_PIC_PRIO_NONE 1000
static int pic_master_prio(int i) { return i * 10; }
static int pic_slave_prio(int j) { return 20 + j; }

static int pic_highest_pending_prio(const hype_pic_emu_t *pic) {
    uint8_t m = (uint8_t)(pic->master.irr & (uint8_t)~pic->master.imr);
    uint8_t s = (uint8_t)(pic->slave.irr & (uint8_t)~pic->slave.imr);
    int best = HYPE_PIC_PRIO_NONE;
    int i;
    for (i = 0; i < 8; i++) {
        if (i == 2) {
            continue; /* cascade slot -- resolved via the slave below */
        }
        if (m & (uint8_t)(1u << i)) {
            int p = pic_master_prio(i);
            if (p < best) {
                best = p;
            }
        }
    }
    /* The slave only reaches the CPU when master IR2 (the cascade) is
     * unmasked. Its highest-priority pending line wins among the slave set. */
    if (s != 0 && (pic->master.imr & (uint8_t)(1u << 2)) == 0) {
        for (i = 0; i < 8; i++) {
            if (s & (uint8_t)(1u << i)) {
                int p = pic_slave_prio(i);
                if (p < best) {
                    best = p;
                }
                break;
            }
        }
    }
    return best;
}

static int pic_highest_inservice_prio(const hype_pic_emu_t *pic) {
    uint8_t m = pic->master.isr;
    uint8_t s = pic->slave.isr;
    int best = HYPE_PIC_PRIO_NONE;
    int i;
    for (i = 0; i < 8; i++) {
        if (i == 2) {
            continue; /* the cascade IR2 in-service is accounted via the slave */
        }
        if (m & (uint8_t)(1u << i)) {
            int p = pic_master_prio(i);
            if (p < best) {
                best = p;
            }
        }
    }
    for (i = 0; i < 8; i++) {
        if (s & (uint8_t)(1u << i)) {
            int p = pic_slave_prio(i);
            if (p < best) {
                best = p;
            }
            break;
        }
    }
    /* A master IR2 in service with no slave bit set is a plain master-IR2
     * line (rare, but keep the accounting exact). */
    if ((m & (uint8_t)(1u << 2)) && s == 0) {
        int p = pic_master_prio(2);
        if (p < best) {
            best = p;
        }
    }
    return best;
}

int hype_pic_emu_has_deliverable(const hype_pic_emu_t *pic) {
    int pend = pic_highest_pending_prio(pic);
    if (pend >= HYPE_PIC_PRIO_NONE) {
        return 0; /* nothing pending/unmasked/reachable */
    }
    /* Deliverable iff strictly higher priority than whatever is in service
     * (nothing in service -> in-service level is NONE -> always deliverable). */
    return pend < pic_highest_inservice_prio(pic);
}
