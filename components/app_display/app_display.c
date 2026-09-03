#include "app_display.h"

#include <string.h>
#include <time.h>

#include "app_cfg.h"
#include "app_diag.h"
#include "app_epd.h"
#include "app_gfx.h"
#include "app_net.h"
#include "app_ui.h"
#include "app_wx.h"
#include "app_httpd.h"
#include "esp_heap_caps.h"
#include "psa/crypto.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app_display";

/* 448 rows in 32-row bands: 14 of them, exactly.
 *
 * The size is the memory decision of PLAN 1b. A whole frame is 268 800 bytes at one byte
 * per pixel and this board's low-water free heap during a TLS session is 157 860,
 * measured. One band is 19 200 bytes of surface plus 9 600 packed. */
#define BAND_ROWS   32
#define BAND_PIXELS ((size_t)APP_GFX_WIDTH * BAND_ROWS)
#define BAND_PACKED (BAND_PIXELS / 2)

/* Seconds the phase 2 colour bar stays on the glass before the next frame replaces it. */
#define BAR_HOLD_S 30

/* How long to wait for SNTP before drawing without a fresh forecast. Long enough to cover
 * the 90-second worst case PLAN 8 records on this board, short enough that a device with
 * no internet still puts something on the glass rather than sitting blank. */
#define NET_WAIT_MS (120 * 1000)

/* Stack for the display task. Sized by the TLS handshake, which dwarfs everything else
 * this task does -- see the note in app_display_start(). */
#define APP_DISPLAY_STACK 8192

#define FRAME_HASH_LEN 32
#define NVS_NAMESPACE  "app_display"
#define NVS_KEY_HASH   "framehash"
#define NVS_KEY_CLEAN  "lastclean"

/* PLAN 4c's backoff: a fetch that fails is retried at these offsets, in minutes, and
 * after the last one the loop simply waits for the next scheduled hour. Open-Meteo being
 * briefly unreachable is not an emergency -- the panel already shows a forecast, marked
 * with its age. */
static const uint16_t RETRY_MINUTES[] = { 1, 2, 5, 15 };
#define RETRY_COUNT (sizeof(RETRY_MINUTES) / sizeof(RETRY_MINUTES[0]))

static TaskHandle_t s_task;

/* Wake-ups carry why they happened.
 *
 * NOTIFY_FORCE used to be a `bool` cleared by whichever task asked for the repaint, while
 * the display task set it again in hash_store() -- so a long press landing in that window
 * was swallowed and logged as "frame unchanged", which is the reading-as-broken-hardware
 * failure the button exists to avoid. Carried in the notification instead, the frame hash
 * has exactly one writer: the task that owns the panel. */
#define NOTIFY_REFRESH  (1u << 0)
#define NOTIFY_FORCE    (1u << 1)

static uint32_t request_refresh(uint32_t bits);
static uint8_t *s_surface;
static uint8_t *s_packed;

/* The hash of what is physically on the glass, as far as this firmware knows.
 *
 * Persisted, because the whole point is to survive the reboot. Zero means "unknown",
 * which is the honest state on a device that has never drawn: the first frame then always
 * refreshes rather than gambling that the panel happens to show the right thing. */
static uint8_t s_frame_hash[FRAME_HASH_LEN];
static bool    s_frame_hash_known;

/* What the status page reports. Counters rather than a log, because the question anybody
 * actually asks of this panel is "is it still updating, and is the hash saving anything",
 * and two numbers answer it from a browser without a serial cable. */
static uint32_t s_refreshes;      /* refreshes actually performed */
static uint32_t s_skipped;        /* draws where the frame turned out unchanged */
static uint32_t s_last_ms;        /* how long the last refresh took */
static int64_t  s_last_uptime_s = -1;
static app_ui_mode_t s_last_mode;
static bool s_drawn;          /* has anything been rendered this boot at all? */

/* Render one whole frame, band by band, straight down the panel.
 *
 * app_ui_draw is called once per band with the same forecast and state, and clips
 * itself; test_layout_bands_match_whole_frame asserts on the host that fourteen bands
 * produce exactly what one 448-row surface would. */
/* Render every band and hash the packed bytes, touching no hardware.
 *
 * The panel keeps its image without power, so a device that reboots already shows the
 * right thing and repainting it costs 26 seconds of panel wear for no visible change
 * (PLAN 3e). Hashing a dry run is what makes that skippable: rendering fourteen bands is
 * a few milliseconds of CPU against a refresh measured at 26 319 ms, so the cheap pass
 * always runs and the expensive one runs only when the picture is actually different.
 *
 * Hashing the packed bytes rather than the surface is deliberate -- packed is what the
 * panel receives, so two surfaces that pack identically are the same picture by the only
 * definition that matters here. */
static esp_err_t hash_frame(const app_wx_forecast_t *forecast, const app_ui_state_t *state,
                            uint8_t out[FRAME_HASH_LEN])
{
    /* PSA rather than mbedtls_sha256_*: mbedTLS 4 moved sha256.h under private/, and a
     * component that reaches into another's private headers breaks on somebody else's
     * schedule. psa_crypto_init() is idempotent and cheap. */
    if (psa_crypto_init() != PSA_SUCCESS) {
        return ESP_FAIL;
    }

    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        return ESP_FAIL;
    }

    for (uint16_t y0 = 0; y0 < APP_GFX_HEIGHT; y0 += BAND_ROWS) {
        uint16_t left = (uint16_t)(APP_GFX_HEIGHT - y0);
        uint16_t rows = BAND_ROWS < left ? BAND_ROWS : left;

        app_gfx_surface_t band = { .pixels = s_surface, .y0 = y0, .rows = rows };
        app_ui_draw(&band, forecast, state);

        size_t n = app_gfx_pack(&band, s_packed, BAND_PACKED);
        if (n == 0 || psa_hash_update(&op, s_packed, n) != PSA_SUCCESS) {
            psa_hash_abort(&op);
            return ESP_FAIL;
        }
    }

    size_t written = 0;
    if (psa_hash_finish(&op, out, FRAME_HASH_LEN, &written) != PSA_SUCCESS ||
        written != FRAME_HASH_LEN) {
        psa_hash_abort(&op);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t push_frame(const app_wx_forecast_t *forecast, const app_ui_state_t *state)
{
    esp_err_t err = app_epd_begin_frame();
    if (err != ESP_OK) {
        return err;
    }

    for (uint16_t y0 = 0; y0 < APP_GFX_HEIGHT; y0 += BAND_ROWS) {
        /* Clamp the last band to what is left. A surface running past the final row is
         * rejected outright by app_gfx, so an unclamped band would silently draw
         * nothing -- 448 happens to divide by 32, but a future panel size will not, and
         * the host tests cover 30- and 100-row bands for exactly this reason. */
        uint16_t left = (uint16_t)(APP_GFX_HEIGHT - y0);
        uint16_t rows = BAND_ROWS < left ? BAND_ROWS : left;

        app_gfx_surface_t band = { .pixels = s_surface, .y0 = y0, .rows = rows };
        app_ui_draw(&band, forecast, state);

        size_t n = app_gfx_pack(&band, s_packed, BAND_PACKED);
        if (n == 0) {
            ESP_LOGE(TAG, "packing band at y=%u produced nothing", (unsigned)y0);
            return ESP_FAIL;
        }
        if ((err = app_epd_write_band(s_packed, n)) != ESP_OK) {
            return err;
        }
    }

    return app_epd_end_frame();
}

/* What the panel should say right now. PLAN 4c's table, decided by app_ui_choose_mode()
 * so the rule is one host-tested function rather than a chain of ifs here. */
/* Held rather than built per call: app_ui_state_t carries `const char *`, so every string
 * it points at has to outlive the draw it is passed to. `ip` and `ap_ssid` are arrays
 * inside app_net_info_t, so that struct is caught by the same rule as the location. */
static app_wx_location_t s_location;
static app_net_info_t    s_net;

static app_ui_state_t current_state(bool have_forecast)
{
    app_net_get_info(&s_net);
    app_wx_get_location(&s_location);

    app_ui_state_t st = {
        /* The state, not `ap_active`: app_net raises the AP on the fallback path too, for
         * a device that has credentials and cannot currently reach the network. Reading
         * that as "unprovisioned" puts the full-screen setup instructions on the panel
         * every time the router reboots. */
        .mode      = app_ui_choose_mode(app_net_state() != APP_NET_STATE_UNPROVISIONED,
                                        have_forecast, app_wx_age_s()),
        .now       = time(NULL),
        .place     = s_location.place,
        .version   = app_diag_app_version(),
        .net_state = app_net_state_name(app_net_state()),
        .ip        = s_net.ip,
        .ap_ssid   = s_net.ap_ssid,
        .ap_pass   = app_cfg_ap_passphrase(),
    };
    return st;
}

/* --- The frame hash, across reboots ------------------------------------------------- */

static void hash_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;   /* never drawn; s_frame_hash_known stays false */
    }
    size_t len = sizeof(s_frame_hash);
    if (nvs_get_blob(h, NVS_KEY_HASH, s_frame_hash, &len) == ESP_OK &&
        len == sizeof(s_frame_hash)) {
        s_frame_hash_known = true;
    }
    nvs_close(h);
}

static void hash_store(const uint8_t hash[FRAME_HASH_LEN])
{
    memcpy(s_frame_hash, hash, FRAME_HASH_LEN);
    s_frame_hash_known = true;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_blob(h, NVS_KEY_HASH, hash, FRAME_HASH_LEN) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

/* --- The weekly clean cycle (PLAN 3e) ----------------------------------------------- */

static time_t clean_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return 0;
    }
    int64_t v = 0;
    if (nvs_get_i64(h, NVS_KEY_CLEAN, &v) != ESP_OK) {
        v = 0;
    }
    nvs_close(h);
    return (time_t)v;
}

static void clean_store(time_t when)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_i64(h, NVS_KEY_CLEAN, (int64_t)when) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

/* Run a clean cycle if one is due.
 *
 * Wall clock rather than uptime, kept in NVS: a device that reboots daily would otherwise
 * never reach a week of uptime and never clean at all. A device with no clock yet does
 * nothing, because "now" of zero would make everything look overdue on every boot.
 */
static void clean_if_due(void)
{
    /* A runtime `if` on the interval, not `#if`.
     *
     * An int Kconfig that is out of scope is not defined at all, and `#if UNDEFINED > 0`
     * is quietly false rather than an error -- so a symbol that fails to reach sdkconfig
     * deletes the feature with nothing to say it did. That happened during this change:
     * the clean cycle compiled, linked, ran and did nothing, and the only way to see it
     * was that the log line never appeared. Named as a constant expression instead, a
     * missing symbol is a compile error, and the compiler folds the branch away exactly
     * as the preprocessor would have. */
    const int64_t interval = (int64_t)CONFIG_APP_DISPLAY_CLEAN_DAYS * 86400;
    if (interval <= 0) {
        return;
    }

    time_t now = time(NULL);
    if (now <= 0) {
        return;
    }

    time_t last = clean_load();
    if (last == 0) {
        /* First run with a clock: start the interval rather than cleaning immediately. A
         * brand-new panel does not need flushing, and cleaning on first boot would put
         * two minutes of flashing in front of somebody who has just plugged it in. */
        clean_store(now);
        return;
    }
    if ((int64_t)now - (int64_t)last < interval) {
        return;
    }

    ESP_LOGI(TAG, "clean cycle due (%lld days since the last one)",
             (long long)(((int64_t)now - (int64_t)last) / 86400));
    esp_err_t err = app_epd_clean();
    ESP_LOGI(TAG, "clean returned %s", esp_err_to_name(err));
    app_epd_sleep();

    if (err == ESP_OK) {
        clean_store(now);
        /* The glass now holds the clean cycle's output, not the last frame. Forgetting
         * the hash is what makes the next draw repaint rather than decide it has nothing
         * to do and leave the panel flushed. */
        s_frame_hash_known = false;
    }
}

/* Render, and refresh only if the picture changed.
 *
 * The hash is stored *after* a successful refresh, never before. Storing it first would
 * mean a refresh that failed halfway left the device believing the glass shows something
 * it does not, and the next hour would skip the repaint that would have fixed it. */
static esp_err_t draw_if_changed(const app_wx_forecast_t *forecast,
                                 const app_ui_state_t *state, const char *why)
{
    uint8_t hash[FRAME_HASH_LEN];
    esp_err_t err = hash_frame(forecast, state, hash);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rendering for the hash failed: %s", esp_err_to_name(err));
        return err;
    }

    s_last_mode = state->mode;
    s_drawn = true;

    if (s_frame_hash_known && memcmp(hash, s_frame_hash, FRAME_HASH_LEN) == 0) {
        s_skipped++;
        ESP_LOGI(TAG, "%s: frame unchanged, not refreshing", why);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "%s: frame changed, refreshing", why);
    int64_t t0 = esp_timer_get_time();
    err = push_frame(forecast, state);
    if (err == ESP_OK) {
        hash_store(hash);
        s_refreshes++;
        s_last_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
        s_last_uptime_s = esp_timer_get_time() / 1000000;
    } else {
        ESP_LOGW(TAG, "refresh failed: %s", esp_err_to_name(err));
    }
    app_epd_sleep();
    return err;
}

/* What to draw right now: the held forecast, shifted onto the current hour.
 *
 * A forecast whose first hour has passed must not be drawn as it stands -- its leftmost
 * column would carry a label for an hour that has already happened. Refusing it outright
 * was the first answer, and it quietly cost PLAN 4c's stale rows: with a 13-hour query,
 * an age of an hour or more always failed that test, so `have_forecast` was false & the
 * panel fell to "Waiting for first forecast" rather than the stale render the table
 * promises. Shifting keeps every hour still true & lets the age decide the marker. */
static bool current_forecast(app_wx_forecast_t *out)
{
    if (!app_wx_get(out)) {
        return false;
    }
    return app_wx_rebase(out, time(NULL));
}

/* Names for app_ui_mode_t, so the log says which of PLAN 4c's screens was drawn rather
 * than a number nobody can read against the table. */
static const char *mode_name(app_ui_mode_t m)
{
    switch (m) {
    case APP_UI_OK:            return "ok";
    case APP_UI_STALE:         return "stale";
    case APP_UI_STALE_OLD:     return "stale-old";
    case APP_UI_NO_FORECAST:   return "no-forecast";
    case APP_UI_UNPROVISIONED: return "unprovisioned";
    }
    return "?";
}

static void draw_now(const char *why)
{
    app_wx_forecast_t wx;
    bool have = current_forecast(&wx);
    app_ui_state_t st = current_state(have);

    /* What went into the frame, logged before the hash decides anything. A frame that
     * refreshes when it should not is always one of these inputs moving, and without them
     * the only evidence is "changed" against a picture nobody can diff. */
    ESP_LOGI(TAG, "%s: mode=%s ip=%s age=%llds", why, mode_name(st.mode),
             st.ip != NULL && st.ip[0] != '\0' ? st.ip : "offline",
             (long long)app_wx_age_s());

    draw_if_changed(have ? &wx : NULL, &st, why);
}

/* Seconds until the top of the next scheduled interval, so redraws land on the hour
 * rather than drifting by however long a refresh took. */
static uint32_t seconds_to_next_tick(void)
{
    const uint32_t period = (uint32_t)CONFIG_APP_DISPLAY_TICK_MINUTES * 60u;
    if (period == 0) {
        return 3600;
    }
    time_t now = time(NULL);
    if (now <= 0) {
        return period;   /* no clock yet; just wait a full period */
    }
    uint32_t past = (uint32_t)(now % period);
    return period - past;
}

static void display_task(void *arg)
{
    (void)arg;

    /* Before anything is driven: say what is electrically on the panel pins. On a newly
     * wired board this is the only evidence available, and it costs a second. */
    app_epd_selftest();

    if (app_epd_init() != ESP_OK || !app_epd_present()) {
        ESP_LOGW(TAG, "no panel; display task exiting");
        vTaskDelete(NULL);
        return;
    }

    /* And now whether anything answers. The self-test reads levels, which cannot tell a
     * powered controller from a pull-up resistor; this pulses reset and waits for BUSY to
     * react. If it does not, every refresh below runs blind and reports nothing. */
    app_epd_probe_reset();

    hash_load();
    ESP_LOGI(TAG, "heap before the first frame: %u; last frame %s",
             (unsigned)esp_get_free_heap_size(),
             s_frame_hash_known ? "known" : "unknown, so the first draw always refreshes");

#if CONFIG_APP_DISPLAY_BRINGUP_BAR
    /* The phase 2 colour bar, off by default now that it has done its job.
     *
     * It exercises reset, the nine-register init, the data phase and the refresh with no
     * rendering in the way, and shows whether the seven colour indices land on the
     * pigments app_gfx names -- worth having on a newly wired panel, and worth 26 seconds
     * of wear on every boot only while that is the question being asked. */
    ESP_LOGI(TAG, "bring-up: seven-colour bar");
    ESP_LOGI(TAG, "colour bar returned %s", esp_err_to_name(app_epd_colour_bar()));
    ESP_LOGI(TAG, "holding it for %d s -- band order should read black, white, green, "
                  "blue, red, yellow, orange, left to right", BAR_HOLD_S);
    vTaskDelay(pdMS_TO_TICKS(BAR_HOLD_S * 1000));
    s_frame_hash_known = false;   /* the bar is now on the glass, not whatever was */
#endif

    /* Draw at boot only when waiting would leave the panel showing nothing useful.
     *
     * The panel keeps its image, so a device that rebooted with a forecast in NVS is
     * already displaying something recent, and the fetch a few seconds from now will draw
     * the current one. Drawing both costs two refreshes -- 52 seconds of wear -- to show
     * an intermediate picture for half a minute. Observed doing exactly that before this
     * check existed.
     *
     * The two cases that cannot wait are a device with no credentials, which must put the
     * setup network on the glass rather than behind a serial cable, and one with no
     * forecast at all, where the panel may be showing anything or nothing. */
    {
        app_wx_forecast_t boot_wx;
        app_net_info_t boot_net;
        app_net_get_info(&boot_net);

        if (boot_net.ap_active || !current_forecast(&boot_wx)) {
            draw_now("boot");
        } else {
            ESP_LOGI(TAG, "boot: holding the panel's current image until the first fetch");
        }
    }

    /* The wait is on the clock, not on the link. TLS fails outright on a device whose
     * clock is unset, and PLAN 8 records SNTP on this board taking anywhere from 7 to 90
     * seconds -- so a fetch triggered by "connected" alone fails on whichever boots SNTP
     * happens to be slow. */
    if (!app_net_wait_time_sync(NET_WAIT_MS)) {
        ESP_LOGW(TAG, "no clock after %d s; running on whatever is cached",
                 NET_WAIT_MS / 1000);
    }

    uint32_t woke_for = 0;

    for (;;) {
        /* Applied here, on the task that owns the hash, rather than by the caller. */
        if (woke_for & NOTIFY_FORCE) {
            ESP_LOGI(TAG, "forced repaint: forgetting the frame hash");
            s_frame_hash_known = false;
        }

        /* One fetch, then PLAN 4c's backoff if it failed: 1, 2, 5, 15 minutes, and after
         * that simply wait for the next scheduled tick. The panel is redrawn between
         * retries only when the frame actually changed -- a forecast crossing three hours
         * old changes the screen, a failed retry on its own does not, and repainting on
         * every retry is exactly the wear PLAN 3e warns about. */
        esp_err_t fetch_err = app_wx_fetch();
        ESP_LOGI(TAG, "fetch returned %s; stack low-water %u of %u bytes",
                 esp_err_to_name(fetch_err),
                 (unsigned)uxTaskGetStackHighWaterMark(NULL), APP_DISPLAY_STACK);

        for (size_t i = 0; fetch_err != ESP_OK && i < RETRY_COUNT; i++) {
            draw_now("between retries");
            ESP_LOGW(TAG, "retry %u of %u in %u min", (unsigned)(i + 1),
                     (unsigned)RETRY_COUNT, (unsigned)RETRY_MINUTES[i]);
            vTaskDelay(pdMS_TO_TICKS((uint32_t)RETRY_MINUTES[i] * 60u * 1000u));
            fetch_err = app_wx_fetch();
            ESP_LOGI(TAG, "retry %u returned %s", (unsigned)(i + 1),
                     esp_err_to_name(fetch_err));
        }

        clean_if_due();
        draw_now(fetch_err == ESP_OK ? "fresh forecast" : "no fresh forecast");
        ESP_LOGI(TAG, "heap: %u", (unsigned)esp_get_free_heap_size());

        /* Sleep to the next tick, but wake early for a requested refresh -- the MQTT
         * command and the status page both call app_display_request_refresh(). */
        uint32_t wait_s = seconds_to_next_tick();
        ESP_LOGI(TAG, "next tick in %u s", (unsigned)wait_s);
        woke_for = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &woke_for,
                            pdMS_TO_TICKS(wait_s * 1000u)) == pdTRUE) {
            ESP_LOGI(TAG, "refresh requested%s",
                     (woke_for & NOTIFY_FORCE) ? " (forced)" : "");
        }
    }
}

uint32_t app_display_force_repaint(void)
{
    return request_refresh(NOTIFY_REFRESH | NOTIFY_FORCE);
}

uint32_t app_display_request_refresh(void)
{
    return request_refresh(NOTIFY_REFRESH);
}

static uint32_t request_refresh(uint32_t bits)
{
    if (s_task == NULL) {
        return 0;   /* no task to ask; nothing to rate-limit either */
    }

    /* Gated on the last refresh actually performed, not on the last request. A request
     * that the frame hash turns into no refresh costs the panel nothing, and refusing it
     * would be a rate limit on a thing that never happened. */
    if (s_last_uptime_s >= 0) {
        int64_t since = (esp_timer_get_time() / 1000000) - s_last_uptime_s;
        if (since < APP_DISPLAY_MANUAL_MIN_GAP_S) {
            uint32_t wait = (uint32_t)(APP_DISPLAY_MANUAL_MIN_GAP_S - since);
            ESP_LOGW(TAG, "refresh refused: %u s until the panel may be refreshed again",
                     (unsigned)wait);
            return wait;
        }
    }

    xTaskNotify(s_task, bits, eSetBits);
    return 0;
}

/* --- The status page's panel section (PLAN 5's fit and finish) ---------------------- */

/* Reported rather than inferred. "refreshes" and "skipped" together say whether the frame
 * hash is earning its place: on a working device skipped climbs and refreshes does not,
 * and a panel that repaints every hour with nothing changing is a bug this makes visible
 * from a browser instead of a serial cable. */
static int panel_status_json(char *buf, size_t len)
{
    /* Read fresh rather than reusing the copy current_state() holds. That one is the
     * location of the last frame *drawn*, which is the right thing for a `const char *`
     * that has to outlive a draw and the wrong thing for a status page -- it reported the
     * old place until the next redraw, making a location change look like it had failed. */
    app_wx_location_t loc;
    app_wx_get_location(&loc);

    return snprintf(buf, len,
        "\"present\":%s,\"mode\":\"%s\",\"refreshes\":%u,\"skipped\":%u,"
        "\"last_refresh_ms\":%u,\"last_refresh_uptime_s\":%lld,"
        "\"place\":\"%s\",\"latitude\":\"%s\",\"longitude\":\"%s\"",
        app_epd_present() ? "true" : "false",
        /* Not a default dressed as a fact. Before the first draw there is no mode, and
         * reporting the enum's zero value said "no-forecast" on a device that had simply
         * not got there yet -- which reads as a fault rather than a boot in progress. */
        s_drawn ? mode_name(s_last_mode) : "not drawn yet",
        (unsigned)s_refreshes, (unsigned)s_skipped, (unsigned)s_last_ms,
        (long long)s_last_uptime_s, loc.place, loc.lat, loc.lon);
}

static const app_httpd_panel_hooks_t PANEL_HOOKS = {
    .refresh      = app_display_request_refresh,
    .status_json  = panel_status_json,
    .set_location = app_wx_set_location,
};
esp_err_t app_display_start(void)
{
    /* Both buffers for the life of the device, allocated before Wi-Fi has fragmented the
     * heap. 28 800 bytes against a measured 157 860-byte low-water mark (PLAN 1b). */
    s_surface = heap_caps_malloc(BAND_PIXELS, MALLOC_CAP_8BIT);
    s_packed  = heap_caps_malloc(BAND_PACKED, MALLOC_CAP_8BIT);
    if (s_surface == NULL || s_packed == NULL) {
        ESP_LOGE(TAG, "no room for a %u-byte band buffer", (unsigned)BAND_PIXELS);
        free(s_surface);
        free(s_packed);
        return ESP_ERR_NO_MEM;
    }

    /* A refresh blocks for about 30 seconds inside app_epd, so this cannot share a task
     * with anything that has to stay responsive.
     *
     * The stack is sized for the TLS handshake in app_wx_fetch(), not for the drawing.
     * At 4096 -- ample for rendering -- the first fetch died in mbedTLS with a stack
     * protection fault, which reads as a crash in app_display and is nothing of the sort.
     * The high-water mark is logged below so this number stays measured. */
    if (xTaskCreate(display_task, "app_display", APP_DISPLAY_STACK, NULL, 4, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    /* After the task exists, so a refresh request arriving immediately has somewhere to
     * go rather than being dropped by app_display_request_refresh()'s NULL check. */
    app_httpd_set_panel_hooks(&PANEL_HOOKS);
    return ESP_OK;
}
