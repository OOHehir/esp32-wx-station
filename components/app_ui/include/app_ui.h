/* app_ui -- the layout. Forecast in, drawing calls out.
 *
 * No I/O, no clock, no network: everything the layout depends on arrives in
 * app_ui_state_t, so the whole component runs under gcc on the build machine and a
 * rendering is a pure function of its inputs (PLAN 5b). That is what lets the layout be
 * iterated against a file instead of against a 30-second panel refresh.
 *
 * Nothing here mentions bands. app_ui_draw is handed a surface and draws PLAN 6's whole
 * layout in absolute panel coordinates; app_gfx clips. app_display calls it once per
 * band, the host tests call it once over a full-height surface, and the two must agree.
 */
#pragma once

#include "app_gfx.h"
#include "app_wx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Which of PLAN 4c's five situations the panel is showing. The forecast alone cannot say:
 * "no network" and "network but the fetch failed" want different screens and hold the
 * same stale forecast. */
typedef enum {
    APP_UI_OK,             /* fresh forecast */
    APP_UI_STALE,          /* forecast usable, marked with its age */
    APP_UI_STALE_OLD,      /* forecast old enough to warn about prominently */
    APP_UI_NO_FORECAST,    /* nothing cached yet */
    APP_UI_UNPROVISIONED,  /* no credentials; show the setup AP full-screen */
} app_ui_mode_t;

/* Everything outside the forecast that the layout needs. Passed in rather than fetched
 * so that a test can place the device in any state without a network stack. */
typedef struct {
    app_ui_mode_t mode;
    time_t        now;            /* UTC; the header clock and staleness both use it */
    /* "Dublin". Nothing draws it since the header gave the top of the panel to the
     * temperature and the feels-like under it: the device is in one place, and whoever is
     * reading it is standing in that place. Kept because app_display already holds it and
     * the portal can change it, so a screen that does need to name a location has it. */
    const char   *place;
    const char   *version;        /* "v0.1.0" */
    const char   *net_state;      /* app_net_state_name(), for the no-forecast screen */
    const char   *ip;             /* "" when not connected */
    const char   *ap_ssid;        /* setup AP, only read when UNPROVISIONED */
    const char   *ap_pass;
} app_ui_state_t;

/* Seconds before a forecast stops being "fresh" and the footer starts saying how old it
 * is, and before it is old enough to say so loudly. PLAN 4c's three hours. */
#define APP_UI_STALE_AFTER_S     (3 * 3600)

/* Which of PLAN 4c's five screens the current situation calls for.
 *
 * A pure decision with no ESP-IDF in it, because the table it implements is the kind of
 * logic that is silent when wrong: a device showing a fresh render of a nine-hour-old
 * forecast looks exactly like a device working correctly. Tested on the host.
 *
 * `age_s` is what app_wx_age_s() returns -- negative when there has never been a
 * forecast. `have_forecast` is separate from it because a forecast fetched before the
 * clock was set has an age that is meaningless rather than absent.
 */
app_ui_mode_t app_ui_choose_mode(bool provisioned, bool have_forecast, int64_t age_s);

/* Draw the whole layout. `forecast` may be NULL when the mode does not need one. */
void app_ui_draw(const app_gfx_surface_t *surface,
                 const app_wx_forecast_t *forecast,
                 const app_ui_state_t *state);

#ifdef __cplusplus
}
#endif
