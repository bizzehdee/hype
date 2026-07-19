#include "io_histogram.h"

void hype_io_hist_record(uint32_t *counts, unsigned nports, uint16_t port) {
    if ((unsigned)port >= nports) {
        return;
    }
    if (counts[port] != 0xFFFFFFFFu) {
        counts[port]++;
    }
}

unsigned hype_io_hist_top(const uint32_t *counts, unsigned nports, hype_io_hist_entry_t *out,
                          unsigned max_out) {
    unsigned filled = 0;
    unsigned p;

    if (max_out == 0) {
        return 0;
    }

    for (p = 0; p < nports; p++) {
        uint32_t c = counts[p];
        unsigned i, pos;
        if (c == 0) {
            continue;
        }
        /* Insertion-sort this port into the top list (descending count; ties
         * keep the lower port first, so the earlier-inserted lower port stays
         * ahead of a later equal-count higher port). Drop it if it can't beat
         * the current tail once the list is full. */
        if (filled == max_out && c <= out[filled - 1u].count) {
            continue;
        }
        pos = filled; /* default: append at the end (grows the list) */
        for (i = 0; i < filled; i++) {
            if (c > out[i].count) {
                pos = i;
                break;
            }
        }
        /* Shift entries at [pos, end) down by one to open a slot at pos. When
         * the list is already full the last entry falls off the bottom. */
        {
            unsigned last = (filled < max_out) ? filled : (max_out - 1u);
            unsigned j;
            for (j = last; j > pos; j--) {
                out[j] = out[j - 1u];
            }
        }
        out[pos].port = (uint16_t)p;
        out[pos].count = c;
        if (filled < max_out) {
            filled++;
        }
    }
    return filled;
}

uint64_t hype_io_hist_total(const uint32_t *counts, unsigned nports) {
    uint64_t total = 0;
    unsigned p;
    for (p = 0; p < nports; p++) {
        total += counts[p];
    }
    return total;
}
