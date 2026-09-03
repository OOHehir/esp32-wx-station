#include "app_ota.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "app_diag.h"
#include "app_httpd.h"
#include "app_led.h"
#include "app_net.h"
#include "app_trust.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "image_check.h"
#include "manifest.h"
#include "ota_internal.h"
#include "ota_record.h"
#include "rollback_policy.h"
#include "sdkconfig.h"
#include "update_policy.h"

static const char *TAG = "app_ota";

ESP_EVENT_DEFINE_BASE(APP_OTA_EVENT);

/* Every TLS connection this component makes goes through here, so there is one answer to
 * "what does this device trust?" and no way for a second call site to quietly differ.
 * Whichever anchor is chosen, the clock has to be set first or validity cannot be
 * checked at all -- see CONFIG_APP_OTA_REQUIRE_TIME_SYNC. */
static void set_trust_anchor(esp_http_client_config_t *cfg)
{
    cfg->cert_pem = app_trust_ca_pem();
    cfg->crt_bundle_attach = app_trust_bundle_attach();
}

#define MANIFEST_MAX 1024

typedef struct {
    bool forced;
} cmd_t;

static QueueHandle_t     s_cmds;
static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_poll_timer;
static app_ota_info_t    s_info;
static bool s_ready;

/* ------------------------------------------------------------------- accessors */

const char *app_ota_state_name(app_ota_state_t state)
{
    switch (state) {
    case APP_OTA_IDLE:        return "idle";
    case APP_OTA_SELFTEST:    return "selftest";
    case APP_OTA_CHECKING:    return "checking";
    case APP_OTA_UP_TO_DATE:  return "up_to_date";
    case APP_OTA_DOWNLOADING: return "downloading";
    case APP_OTA_READY:       return "ready";
    case APP_OTA_FAILED:      return "failed";
    }
    return "unknown";
}

void app_ota_get_info(app_ota_info_t *out)
{
    if (!out) {
        return;
    }
    if (!s_lock) {
        memset(out, 0, sizeof(*out));
        out->progress_pct = -1;
        out->last_check_s = -1;
        out->selftest_left_s = -1;
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_info;
    xSemaphoreGive(s_lock);
}

bool app_ota_busy(void)
{
    if (!s_lock) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool busy = (s_info.state == APP_OTA_DOWNLOADING || s_info.state == APP_OTA_READY);
    xSemaphoreGive(s_lock);
    return busy;
}

void ota_set_state(app_ota_state_t state, const char *fmt, ...)
{
    char msg[sizeof(s_info.message)] = "";
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_info.state = state;
    strlcpy(s_info.message, msg, sizeof(s_info.message));
    xSemaphoreGive(s_lock);

    if (msg[0]) {
        ESP_LOGI(TAG, "%s: %s", app_ota_state_name(state), msg);
    } else {
        ESP_LOGI(TAG, "%s", app_ota_state_name(state));
    }
    esp_event_post(APP_OTA_EVENT, APP_OTA_EVENT_STATE_CHANGED, &state, sizeof(state), 0);
}

void ota_set_selftest_left(int32_t seconds)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_info.selftest_left_s = seconds;
    xSemaphoreGive(s_lock);
}

/* Keep the status page's view of the quarantine in step with NVS. */
static void publish_record(void)
{
    ota_record_t rec;
    ota_record_load(&rec);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_info.failed_version, rec.failed, sizeof(s_info.failed_version));
    s_info.failed_count = rec.failed_count;
    xSemaphoreGive(s_lock);
}

/* --------------------------------------------------------------- manifest fetch */

esp_err_t ota_build_manifest_url(char *out, size_t out_len)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    int n = snprintf(out, out_len, "%s/%s/%s/manifest.json",
                     CONFIG_APP_OTA_BASE_URL, desc->project_name, CONFIG_IDF_TARGET);
    return (n > 0 && (size_t)n < out_len) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

/* Fetch the manifest into `buf`. Small, bounded, and read in one go -- a firmware server
 * that answers this with megabytes is not one to keep talking to. */
esp_err_t ota_fetch_manifest(const char *url, char *buf, size_t buf_len)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = CONFIG_APP_OTA_HTTP_TIMEOUT_MS,
        .keep_alive_enable = false,
    };
    set_trust_anchor(&cfg);

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    int64_t len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGW(TAG, "manifest HTTP %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NOT_FOUND;
    }
    if (len >= (int64_t)buf_len) {
        ESP_LOGW(TAG, "manifest is %lld bytes, buffer is %u", (long long)len, (unsigned)buf_len);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Chunked responses report len < 0, so read until the buffer is full or the body
     * ends rather than trusting Content-Length. */
    int total = 0;
    while (total < (int)buf_len - 1) {
        int r = esp_http_client_read(client, buf + total, buf_len - 1 - total);
        if (r < 0) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        if (r == 0) {
            break;
        }
        total += r;
    }
    buf[total] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return total > 0 ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

/* ------------------------------------------------------------------- download */

static void note_progress(int written, int total)
{
    int pct = (total > 0) ? (int)((int64_t)written * 100 / total) : -1;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changed = (s_info.progress_pct != pct);
    s_info.bytes_written = (uint32_t)written;
    s_info.bytes_total = (uint32_t)(total > 0 ? total : 0);
    s_info.progress_pct = pct;
    xSemaphoreGive(s_lock);

    if (changed && pct >= 0 && pct % 10 == 0) {
        ESP_LOGI(TAG, "%d%% (%d/%d bytes)", pct, written, total);
        esp_event_post(APP_OTA_EVENT, APP_OTA_EVENT_PROGRESS, &pct, sizeof(pct), 0);
    }
}

/* Counted only for an image that arrived and was then rejected -- that repeats. Transport
 * failures do not come through here: flaky Wi-Fi must not quarantine a good version. */
static void reject_image(const char *version, const char *fmt, ...)
{
    char why[80];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, sizeof(why), fmt, ap);
    va_end(ap);

    unsigned n = ota_record_note_failure(version);
    publish_record();
    ota_set_state(APP_OTA_FAILED, "%s (attempt %u for %s)", why, n, version);
}

/* Hash the passive slot and compare it with the manifest, before esp_https_ota_finish()
 * makes the slot bootable. See image_check.h for why it happens here and not after. */
static esp_err_t verify_written_image(const manifest_t *m, int image_len)
{
    if (m->size != 0 && (uint32_t)image_len != m->size) {
        reject_image(m->version, "manifest says %u bytes, received %d",
                     (unsigned)m->size, image_len);
        return ESP_ERR_INVALID_SIZE;
    }

    const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
    if (!slot) {
        ota_set_state(APP_OTA_FAILED, "no passive slot to verify");
        return ESP_FAIL;
    }

    char got[SHA_HEX_LEN + 1];
    esp_err_t err = image_check_sha256(slot, (size_t)image_len, m->sha256, got, sizeof(got));

    if (err == ESP_ERR_NOT_FOUND) {
        /* The manifest carried no usable hash. publish.sh always writes one. */
#if CONFIG_APP_OTA_REQUIRE_SHA256
        reject_image(m->version, "manifest has no sha256");
        return ESP_ERR_NOT_FOUND;
#else
        ESP_LOGW(TAG, "manifest has no sha256 -- image not hash-checked");
        return ESP_OK;
#endif
    }
    if (err == ESP_ERR_INVALID_CRC) {
        ESP_LOGE(TAG, "sha256 wanted %s", m->sha256);
        ESP_LOGE(TAG, "sha256 got    %s", got);
        reject_image(m->version, "sha256 mismatch: %.12s... not %.12s...", got, m->sha256);
        return err;
    }
    if (err != ESP_OK) {
        /* Could not read or hash the slot at all: a device-side fault, not a bad
         * publish, so it is not held against the version. */
        ota_set_state(APP_OTA_FAILED, "cannot verify image: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "sha256 %s -- matches the manifest", got);
    return ESP_OK;
}

static esp_err_t download(const manifest_t *m)
{
    const esp_app_desc_t *running = esp_app_get_description();

    esp_http_client_config_t http_cfg = {
        .url = m->url,
        .timeout_ms = CONFIG_APP_OTA_HTTP_TIMEOUT_MS,
        .keep_alive_enable = true,
    };
    set_trust_anchor(&http_cfg);
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK || !handle) {
        ota_set_state(APP_OTA_FAILED, "cannot start download: %s", esp_err_to_name(err));
        return err;
    }

    /* The header check is the reason for using the advanced API. Nothing has been
     * committed to the passive slot yet, so an image for the wrong project can still be
     * refused for free -- and flashing one is a brick. */
    esp_app_desc_t incoming;
    err = esp_https_ota_get_img_desc(handle, &incoming);
    if (err != ESP_OK) {
        ota_set_state(APP_OTA_FAILED, "no image header: %s", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        return err;
    }

    if (strncmp(incoming.project_name, running->project_name,
                sizeof(incoming.project_name)) != 0) {
        reject_image(m->version, "image is project \"%.*s\", not \"%s\"",
                     (int)sizeof(incoming.project_name), incoming.project_name,
                     running->project_name);
        esp_https_ota_abort(handle);
        return ESP_ERR_INVALID_VERSION;
    }

    /* The manifest said one thing; the image itself says another. Trust neither over the
     * other -- stop, because one of the two is wrong and continuing means installing
     * something nobody described. */
    if (strncmp(incoming.version, m->version, sizeof(incoming.version)) != 0) {
        reject_image(m->version, "manifest says %s, image says %.*s",
                     m->version, (int)sizeof(incoming.version), incoming.version);
        esp_https_ota_abort(handle);
        return ESP_ERR_INVALID_VERSION;
    }

    ota_set_state(APP_OTA_DOWNLOADING, "%s -> %s", running->version, m->version);
    app_led_set_status(APP_LED_STATUS_UPDATING);

    int total = esp_https_ota_get_image_size(handle);
    while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        note_progress(esp_https_ota_get_image_len_read(handle), total);
    }

    if (err != ESP_OK) {
        ota_set_state(APP_OTA_FAILED, "download failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        return err;
    }
    if (!esp_https_ota_is_complete_data_received(handle)) {
        /* A truncated image that was never noticed is exactly how a device ends up
         * booting into a half-written slot. */
        ota_set_state(APP_OTA_FAILED, "connection ended before the image did");
        esp_https_ota_abort(handle);
        return ESP_ERR_INVALID_SIZE;
    }

    int written = esp_https_ota_get_image_len_read(handle);
    note_progress(written, total);

    /* Everything is in flash but nothing is bootable yet -- the last moment at which
     * walking away costs nothing. */
    err = verify_written_image(m, written);
    if (err != ESP_OK) {
        esp_https_ota_abort(handle);
        return err;
    }

    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        /* finish() validates the written image and checks its signature when signing is
         * enabled; do not call abort() after it. A signature failure lands here. */
        reject_image(m->version, "image rejected: %s", esp_err_to_name(err));
        return err;
    }

    ota_set_state(APP_OTA_READY, "%s installed, restarting", m->version);
    return ESP_OK;
}

/* ------------------------------------------------------------------ the check */

static void do_check(bool forced)
{
    char url[256];
    char *body = NULL;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_info.offered_version[0] = '\0';
    s_info.progress_pct = -1;
    s_info.bytes_written = 0;
    s_info.bytes_total = 0;
    xSemaphoreGive(s_lock);

    ota_set_state(APP_OTA_CHECKING, NULL);

    if (app_net_state() != APP_NET_STATE_CONNECTED) {
        ota_set_state(APP_OTA_FAILED, "no network");
        goto done;
    }
#if CONFIG_APP_OTA_REQUIRE_TIME_SYNC
    /* Wait rather than fail outright. SNTP has been seen to take anywhere from 7 s to
     * 90 s after the lease, so a check triggered shortly after boot would otherwise
     * report a failure that fixes itself a minute later -- which teaches people to
     * distrust the button. Refusing after the wait still turns an unhelpful TLS error
     * into an accurate one. */
    if (!app_net_time_synced()) {
        ESP_LOGI(TAG, "waiting up to %ds for the clock", CONFIG_APP_OTA_TIME_SYNC_WAIT_S);
        ota_set_state(APP_OTA_CHECKING, "waiting for the clock to be set");
        if (!app_net_wait_time_sync(CONFIG_APP_OTA_TIME_SYNC_WAIT_S * 1000)) {
            ota_set_state(APP_OTA_FAILED,
                          "clock still not set after %ds -- certificate validity cannot be checked",
                          CONFIG_APP_OTA_TIME_SYNC_WAIT_S);
            goto done;
        }
    }
#endif

    if (ota_build_manifest_url(url, sizeof(url)) != ESP_OK) {
        ota_set_state(APP_OTA_FAILED, "manifest url too long");
        goto done;
    }

    body = malloc(MANIFEST_MAX);
    if (!body) {
        ota_set_state(APP_OTA_FAILED, "out of memory");
        goto done;
    }

    ESP_LOGI(TAG, "fetching %s", url);
    esp_err_t err = ota_fetch_manifest(url, body, MANIFEST_MAX);
    if (err != ESP_OK) {
        ota_set_state(APP_OTA_FAILED, "manifest unavailable: %s", esp_err_to_name(err));
        goto done;
    }

    manifest_t m;
    if (manifest_parse(body, &m) != ESP_OK) {
        ota_set_state(APP_OTA_FAILED, "manifest is not valid JSON");
        goto done;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_info.offered_version, m.version, sizeof(s_info.offered_version));
    xSemaphoreGive(s_lock);

    ota_record_t rec;
    ota_record_load(&rec);

    const esp_app_desc_t *running = esp_app_get_description();
    const update_offer_t offer = {
        .project = m.project, .chip = m.chip, .version = m.version, .url = m.url,
    };
    const update_device_t dev = {
        .project = running->project_name,
        .chip = CONFIG_IDF_TARGET,
        .version = running->version,
        .failed_version = rec.failed,
        .failed_count = rec.failed_count,
        .max_failures = CONFIG_APP_OTA_MAX_FAILURES,
        .forced = forced,
    };

    char reason[112];
    update_verdict_t verdict = update_decide(&offer, &dev, reason, sizeof(reason));

    if (verdict == UPDATE_UP_TO_DATE) {
        ota_set_state(APP_OTA_UP_TO_DATE, "%s", reason);
        goto done;
    }
    if (verdict != UPDATE_APPLY) {
        ota_set_state(APP_OTA_FAILED, "%s (%s)", reason, update_verdict_name(verdict));
        goto done;
    }

    if (download(&m) == ESP_OK) {
        free(body);
        /* Written *before* the restart, so a new image that panics during start-up --
         * or a power cut during its first boot -- is still attributable next time. */
        ota_record_set_attempted(m.version);
        ESP_LOGW(TAG, "restarting into the new image in %d ms", CONFIG_APP_OTA_RESTART_DELAY_MS);
        /* The delay lets the web UI poll /api/status once more and show that it worked,
         * rather than the page just dying mid-request (PLAN §4a). */
        vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_OTA_RESTART_DELAY_MS));
        esp_restart();
    }
    /* On failure the LED goes back to reporting the network, not the update. */
    app_led_set_status(app_net_state() == APP_NET_STATE_CONNECTED
                       ? APP_LED_STATUS_CONNECTED : APP_LED_STATUS_CONNECTING);

done:
    free(body);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_info.last_check_s = (int64_t)app_diag_uptime_s();
    xSemaphoreGive(s_lock);
}

/* ------------------------------------------------------------------- probation */

#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
/* The running image is in PENDING_VERIFY: the bootloader will put the previous one back
 * on the next reset unless this confirms it. Runs once, on the OTA task, before that
 * task starts serving checks -- an update cannot be installed on top of an unconfirmed
 * image anyway (esp_ota_set_boot_partition refuses with
 * ESP_ERR_OTA_ROLLBACK_INVALID_STATE), so there is nothing useful to do in parallel. */
static void run_probation(void)
{
    const esp_app_desc_t *running = esp_app_get_description();

    if (ota_selftest_run()) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        ota_set_selftest_left(-1);
        if (err != ESP_OK) {
            /* Refusing to confirm leaves the image on probation, which means the next
             * reset reverts it. Say so rather than reporting success. */
            ota_set_state(APP_OTA_FAILED, "could not confirm this image: %s",
                          esp_err_to_name(err));
            return;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_info.probation = false;
        xSemaphoreGive(s_lock);

        ota_record_clear_attempted();
        ota_record_clear_failure(running->version);
        publish_record();
        ota_set_state(APP_OTA_IDLE, "%s passed its self-test and is confirmed",
                      running->version);
        return;
    }

    /* With nothing valid in the other slot there is nowhere to go, and a suspect image
     * that runs beats a reboot into nothing. Not a failed update -- a device with one. */
    if (!esp_ota_check_rollback_is_possible()) {
        ESP_LOGE(TAG, "self-test failed, but there is no other valid image to fall back "
                      "to -- confirming this one rather than rebooting into nothing");
        esp_ota_mark_app_valid_cancel_rollback();
        /* Kept, so it is not a failed update -- it is a device with one image. Clearing
         * the attempt stops the next boot counting a failure for something we chose. */
        ota_record_clear_attempted();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_info.probation = false;
        xSemaphoreGive(s_lock);
        ota_set_selftest_left(-1);
        ota_set_state(APP_OTA_FAILED,
                      "self-test failed, but there is nothing to roll back to");
        return;
    }

    /* The failure is counted by the *next* boot, through ROLLBACK_BOOT_REVERTED: one
     * accounting point for both a rejected self-test and a panic, and the message then
     * lands on the device that came back rather than the one about to disappear. */
    ESP_LOGE(TAG, "self-test failed -- rolling back %s", running->version);
    ota_set_state(APP_OTA_FAILED, "self-test failed -- rolling back %s", running->version);
    app_led_set_status(APP_LED_STATUS_ERROR);
    vTaskDelay(pdMS_TO_TICKS(1000));

    esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
    /* Should not be reachable: the check above said a rollback was possible. If it
     * happens anyway, do not leave the device sitting on an image nothing will confirm. */
    ESP_LOGE(TAG, "rollback refused after all (%s) -- confirming this image instead",
             esp_err_to_name(err));
    esp_ota_mark_app_valid_cancel_rollback();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_info.probation = false;
    xSemaphoreGive(s_lock);
    ota_set_selftest_left(-1);
}
#endif

/* What the previous boot amounted to. Runs before the OTA task, so the first thing a
 * status page shows is already correct. */
static bool account_for_last_boot(void)
{
    const esp_app_desc_t *running = esp_app_get_description();
    bool pending = app_diag_pending_verify();

    ota_record_t rec;
    ota_record_load(&rec);

    rollback_boot_t what = rollback_on_boot(rec.attempted, running->version, pending);
    ESP_LOGI(TAG, "boot accounting: %s (running %s, attempted \"%s\")",
             rollback_boot_name(what), running->version, rec.attempted);

    switch (what) {
    case ROLLBACK_BOOT_REVERTED: {
        unsigned n = ota_record_note_failure(rec.attempted);
        ESP_LOGW(TAG, "%s was installed but %s is running -- it did not survive "
                      "(failure #%u)", rec.attempted, running->version, n);
        ota_set_state(APP_OTA_FAILED, "%s was rolled back (failure #%u)", rec.attempted, n);
        /* Deliberately no LED change: app_net is already driving it and the next state
         * transition would overwrite this within seconds, which reads as a glitch rather
         * than as information. The status page carries the message instead. */
        break;
    }
    case ROLLBACK_BOOT_CONFIRMED:
        /* Reached this boot as a plain, valid image: the self-test already passed on an
         * earlier one. Tidy the record away and lift any quarantine it earned. */
        ota_record_clear_attempted();
        ota_record_clear_failure(running->version);
        break;
    case ROLLBACK_BOOT_PROBATION:
        ESP_LOGW(TAG, "%s is on probation -- it will be reverted on the next reset "
                      "unless the self-test confirms it", running->version);
        break;
    case ROLLBACK_BOOT_NORMAL:
        break;
    }

    publish_record();
    return what == ROLLBACK_BOOT_PROBATION;
}

/* -------------------------------------------------------------------- plumbing */

static void ota_task(void *arg)
{
    (void)arg;

#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    bool probation;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    probation = s_info.probation;
    xSemaphoreGive(s_lock);
    if (probation) {
        run_probation();
    }
#endif

    cmd_t cmd;
    for (;;) {
        if (xQueueReceive(s_cmds, &cmd, portMAX_DELAY) == pdTRUE) {
            do_check(cmd.forced);
        }
    }
}

#if CONFIG_APP_OTA_POLL_ENABLE
static void schedule_poll(void);

static void poll_fired(void *arg)
{
    (void)arg;
    app_ota_check_now(false);
    schedule_poll();
}

static void schedule_poll(void)
{
    /* Jittered, so a fleet that all booted after the same power cut does not arrive at
     * the server in the same second. */
    uint32_t base = CONFIG_APP_OTA_POLL_INTERVAL_S;
    uint32_t spread = base * CONFIG_APP_OTA_POLL_JITTER_PCT / 100;
    uint32_t delay = base - spread / 2 + (spread ? esp_random() % spread : 0);
    esp_timer_stop(s_poll_timer);
    esp_timer_start_once(s_poll_timer, (uint64_t)delay * 1000000ULL);
    ESP_LOGI(TAG, "next automatic check in %us", (unsigned)delay);
}
#endif

esp_err_t app_ota_check_now(bool forced)
{
    if (!s_cmds) {
        return ESP_ERR_INVALID_STATE;
    }
    if (app_ota_busy()) {
        return ESP_ERR_INVALID_STATE;
    }
    /* ESP-IDF refuses to move the boot partition while the running image is unconfirmed.
     * Refuse here, where the reason can be given, not a megabyte later. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool probation = s_info.probation;
    xSemaphoreGive(s_lock);
    if (probation) {
        return ESP_ERR_NOT_FINISHED;
    }

    cmd_t cmd = { .forced = forced };
    return xQueueSend(s_cmds, &cmd, 0) == pdTRUE ? ESP_OK : ESP_ERR_INVALID_STATE;
}

/* Escape a string that may carry server-supplied text into the status page. */
static void escape_into(const char *src, char *dst, size_t dst_len)
{
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 2 < dst_len; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') {
            dst[o++] = '\\';
        }
        dst[o++] = (c >= 0x20) ? c : ' ';
    }
    dst[o] = '\0';
}

/* What app_httpd renders into the "ota" object of /api/status. */
static int ota_status_json(char *buf, size_t len)
{
    app_ota_info_t info;
    app_ota_get_info(&info);

    char msg[sizeof(info.message) * 2];
    char failed[sizeof(info.failed_version) * 2];
    escape_into(info.message, msg, sizeof(msg));
    escape_into(info.failed_version, failed, sizeof(failed));

    return snprintf(buf, len,
                    ",\"state\":\"%s\",\"running\":\"%s\",\"offered\":\"%s\","
                    "\"progress\":%d,\"written\":%u,\"total\":%u,"
                    "\"message\":\"%s\",\"last_check_s\":%lld,"
                    "\"probation\":%s,\"selftest_left_s\":%d,"
                    "\"failed_version\":\"%s\",\"failed_count\":%u,\"max_failures\":%d",
                    app_ota_state_name(info.state), info.running_version,
                    info.offered_version, info.progress_pct,
                    (unsigned)info.bytes_written, (unsigned)info.bytes_total,
                    msg, (long long)info.last_check_s,
                    info.probation ? "true" : "false", (int)info.selftest_left_s,
                    failed, (unsigned)info.failed_count, CONFIG_APP_OTA_MAX_FAILURES);
}

static const app_httpd_ota_hooks_t HOOKS = {
    .check = app_ota_check_now,
    .busy = app_ota_busy,
    .status_json = ota_status_json,
};

esp_err_t app_ota_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    s_cmds = xQueueCreate(2, sizeof(cmd_t));
    if (!s_lock || !s_cmds) {
        return ESP_ERR_NO_MEM;
    }

    const esp_app_desc_t *desc = esp_app_get_description();
    s_info.state = APP_OTA_IDLE;
    s_info.progress_pct = -1;
    s_info.last_check_s = -1;
    s_info.selftest_left_s = -1;
    strlcpy(s_info.running_version, desc->version, sizeof(s_info.running_version));

    s_info.probation = account_for_last_boot();
#if !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    /* Nothing will ever confirm the image, so do not claim it is on probation: without
     * CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE the bootloader never sets PENDING_VERIFY and
     * a new image is live the moment it boots. */
    s_info.probation = false;
#endif

    if (xTaskCreate(ota_task, "app_ota", CONFIG_APP_OTA_TASK_STACK, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

#if CONFIG_APP_OTA_POLL_ENABLE
    const esp_timer_create_args_t args = { .callback = poll_fired, .name = "ota_poll" };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_poll_timer));
    schedule_poll();
#endif

    /* This is what lights up "Check for updates" in the web UI. */
    app_httpd_set_ota_hooks(&HOOKS);

    s_ready = true;
    /* #if, not a ternary: an unselected Kconfig bool is not defined at all, so naming it
     * in an expression does not compile. */
    ESP_LOGI(TAG, "ready; server %s (trusting %s)",
             CONFIG_APP_OTA_BASE_URL, app_trust_name());
    return ESP_OK;
}
