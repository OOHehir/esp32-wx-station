/* app_display -- the glue: when to redraw, what state to draw, and the band loop.
 *
 * This is the project's own bring-up, kept out of app_core so the template stays
 * mergeable, and out of main/ so it can be reached from a test (PLAN 5).
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the display task and returns. The task owns the panel from then on. */
esp_err_t app_display_start(void);

/* Ask for a repaint at the next opportunity. Safe from any task.
 *
 * Asks; does not draw. A refresh is 26 seconds, so no caller should ever be holding a
 * broker link or an HTTP connection open across one. Repeated requests collapse into one
 * redraw, and the frame hash then makes that free if nothing has changed.
 *
 * Returns 0 when the request was queued, or the seconds still to wait when it was
 * refused. E Ink advise against refreshing an ACeP panel more often than roughly every
 * three minutes; the hourly tick is far inside that and a button is not (PLAN 3e). The
 * caller is expected to report the number rather than swallow it -- a button that
 * silently does nothing reads as broken hardware, which is the more expensive
 * misunderstanding. */
uint32_t app_display_request_refresh(void);

/* Repaint even if the frame hash says nothing changed.
 *
 * The hash records what this firmware believes is on the glass, and the two can genuinely
 * diverge: a refresh interrupted by a power cut, or a panel cleaned by hand. There is
 * otherwise no way for somebody looking at a wrong picture to say so. Subject to the same
 * rate limit as an ordinary manual refresh, and returns the same value.
 *
 * A refused request is refused entirely -- it leaves the hash alone and has no effect on
 * the next scheduled tick, so asking again after the gap is the whole recovery. This is
 * the price of the request travelling with the wake-up rather than in a flag two tasks
 * write to; the flag was racy, and a forced repaint that quietly did nothing was the
 * failure it produced. */
uint32_t app_display_force_repaint(void);

/* Seconds a manual refresh is held off after one is performed. */
#define APP_DISPLAY_MANUAL_MIN_GAP_S (3 * 60)

#ifdef __cplusplus
}
#endif
