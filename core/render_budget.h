#ifndef HYPE_CORE_RENDER_BUDGET_H
#define HYPE_CORE_RENDER_BUDGET_H

/*
 * #373: adaptive row budget for a bounded terminal redraw.
 *
 * A view switch starts at the conservative eight-row floor that protected the
 * BSP from a measured six-second full redraw. Fast, productive passes double
 * the next budget. Slow passes reduce it. Passes that draw no cells provide no
 * evidence about redraw cost and therefore do not change the budget.
 */
#define HYPE_RENDER_BUDGET_MIN_ROWS 8u
#define HYPE_RENDER_BUDGET_FAST_US 2000u
#define HYPE_RENDER_BUDGET_SLOW_US 8000u

typedef struct {
    unsigned rows;
} hype_render_budget_t;

void hype_render_budget_reset(hype_render_budget_t *budget);
unsigned hype_render_budget_rows(const hype_render_budget_t *budget, unsigned screen_rows);
void hype_render_budget_record(hype_render_budget_t *budget, unsigned screen_rows,
                               unsigned drawn_cells, unsigned long long elapsed_us);

#endif /* HYPE_CORE_RENDER_BUDGET_H */
