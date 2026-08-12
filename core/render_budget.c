#include "render_budget.h"

void hype_render_budget_reset(hype_render_budget_t *budget) {
    budget->rows = HYPE_RENDER_BUDGET_MIN_ROWS;
}

unsigned hype_render_budget_rows(const hype_render_budget_t *budget, unsigned screen_rows) {
    unsigned rows = budget->rows;

    if (rows < HYPE_RENDER_BUDGET_MIN_ROWS) {
        rows = HYPE_RENDER_BUDGET_MIN_ROWS;
    }
    if (screen_rows != 0u && rows > screen_rows) {
        rows = screen_rows;
    }
    return rows;
}

void hype_render_budget_record(hype_render_budget_t *budget, unsigned screen_rows,
                               unsigned drawn_cells, unsigned long long elapsed_us) {
    unsigned rows;

    if (drawn_cells == 0u || screen_rows == 0u) {
        return;
    }

    rows = hype_render_budget_rows(budget, screen_rows);
    if (elapsed_us <= HYPE_RENDER_BUDGET_FAST_US) {
        if (rows < screen_rows) {
            rows = (rows > screen_rows / 2u) ? screen_rows : rows * 2u;
        }
    } else if (elapsed_us >= HYPE_RENDER_BUDGET_SLOW_US) {
        rows = (rows / 2u < HYPE_RENDER_BUDGET_MIN_ROWS)
                   ? HYPE_RENDER_BUDGET_MIN_ROWS
                   : rows / 2u;
    }
    budget->rows = rows;
}
