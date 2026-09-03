/* app_wx -- fetch, cache, and hold the forecast.
 *
 * The parsing lives in wx_parse.c, which is pure and tested on the host against a
 * recorded response. This file is the part that cannot be tested there: a socket, a TLS
 * handshake and an NVS blob. Keeping the split sharp is the point -- when the panel shows
 * the wrong weather, the question "is it the parser or the network" already has an
 * answer (PLAN 5b).
 */
#include "app_wx.h"

#include <stdio.h>
#include <string.h>

#include "app_trust.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "app_cfg.h"
#include "wx_location.h"
#include "wx_parse.h"

#if CONFIG_APP_WX_ENABLE

static const char *TAG = "app_wx";

#define NVS_NAMESPACE "app_wx"
#define NVS_KEY       "forecast"

/* One version byte in front of the cached struct.
 *
 * A cache is a struct written by one firmware and read by another, and app_wx_forecast_t
 * will grow a field eventually. Without this, the first boot after that OTA reads the old
 * bytes into the new layout and draws whatever they happen to mean -- a forecast that is
 * wrong rather than absent, which is the failure this project keeps designing against.
 * Bump on any change to app_wx_forecast_t.
 *
 * 2: sunrise and sunset. The first boot after this OTA discards the cached forecast and
 * shows the no-forecast screen until the next fetch, which is a minute at most and the
 * correct price for not reading v1 bytes as a v2 struct.
 *
 * 3: apparent temperature. Same price, and the same reason to pay it: feels_c sits
 * between temp_c and precip_mm, so v2 bytes read as a v3 struct would shift every later
 * field by four and put yesterday's rainfall where the sunlight belongs. */
#define CACHE_VERSION 3

typedef struct {
    uint8_t           version;
    app_wx_forecast_t forecast;
} cache_blob_t;

/* The held forecast, and the lock over it.
 *
 * app_wx_fetch() runs on whatever task calls it and app_wx_get() is called from the
 * display task, so the struct is genuinely shared. It is 200-odd bytes: copied under the
 * lock rather than handed out by pointer, so a caller cannot be halfway through reading a
 * forecast while a fetch replaces it. */
static app_wx_forecast_t s_forecast;
static bool              s_have;
static SemaphoreHandle_t s_lock;

static bool lock(void)
{
    return s_lock != NULL && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE;
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

/* --- The cache ---------------------------------------------------------------------- */

static esp_err_t cache_load(app_wx_forecast_t *out)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;   /* ESP_ERR_NVS_NOT_FOUND on a device that has never fetched */
    }

    cache_blob_t blob;
    size_t len = sizeof(blob);
    err = nvs_get_blob(h, NVS_KEY, &blob, &len);
    nvs_close(h);

    if (err != ESP_OK) {
        return err;
    }
    if (len != sizeof(blob)) {
        /* A blob of the wrong size is a blob from another firmware. Say so and discard:
         * it is the same fault the version byte catches, seen a moment earlier. */
        ESP_LOGW(TAG, "cached forecast is %u bytes, expected %u -- discarding",
                 (unsigned)len, (unsigned)sizeof(blob));
        return ESP_ERR_INVALID_SIZE;
    }
    if (blob.version != CACHE_VERSION) {
        ESP_LOGW(TAG, "cached forecast is version %u, this firmware writes %u -- discarding",
                 blob.version, CACHE_VERSION);
        return ESP_ERR_INVALID_VERSION;
    }
    if (blob.forecast.count == 0 || blob.forecast.count > APP_WX_HOURS) {
        ESP_LOGW(TAG, "cached forecast claims %u hours -- discarding", blob.forecast.count);
        return ESP_ERR_INVALID_SIZE;
    }

    *out = blob.forecast;
    return ESP_OK;
}

static esp_err_t cache_store(const app_wx_forecast_t *f)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    cache_blob_t blob = { .version = CACHE_VERSION, .forecast = *f };
    err = nvs_set_blob(h, NVS_KEY, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* --- The fetch ---------------------------------------------------------------------- */

/* The query of PLAN 4a. Built at each fetch rather than as one string constant, because
 * the coordinates are now settable from the portal and a macro would freeze the ones this
 * firmware was compiled with. */
#define WX_URL_MAX 320

#define NVS_KEY_LAT   "wx_lat"
#define NVS_KEY_LON   "wx_lon"
#define NVS_KEY_PLACE "wx_place"

void app_wx_get_location(app_wx_location_t *out)
{
    if (out == NULL) {
        return;
    }
    /* Compiled-in first, then overwritten by anything stored. A stored value that fails
     * to load for any reason therefore leaves a working location rather than an empty
     * one, which is the difference between a stale forecast and no forecast at all. */
    snprintf(out->lat, sizeof(out->lat), "%s", CONFIG_APP_WX_LATITUDE);
    snprintf(out->lon, sizeof(out->lon), "%s", CONFIG_APP_WX_LONGITUDE);
    snprintf(out->place, sizeof(out->place), "%s", CONFIG_APP_WX_PLACE);

    /* Each read is sized to the field it fills, so a stored value too long for its
     * destination is refused by app_cfg_get_str rather than silently truncated into
     * something that still parses -- a clipped longitude is a valid number for a
     * different place. */
    char lat[sizeof(out->lat)], lon[sizeof(out->lon)], place[sizeof(out->place)];

    if (app_cfg_get_str(NVS_KEY_LAT, lat, sizeof(lat)) == ESP_OK &&
        wx_coord_valid(lat, -90.0, 90.0)) {
        memcpy(out->lat, lat, sizeof(lat));
    }
    if (app_cfg_get_str(NVS_KEY_LON, lon, sizeof(lon)) == ESP_OK &&
        wx_coord_valid(lon, -180.0, 180.0)) {
        memcpy(out->lon, lon, sizeof(lon));
    }
    if (app_cfg_get_str(NVS_KEY_PLACE, place, sizeof(place)) == ESP_OK &&
        wx_place_valid(place, sizeof(place) - 1)) {
        memcpy(out->place, place, sizeof(place));
    }
}

esp_err_t app_wx_set_location(const char *lat, const char *lon, const char *place)
{
    /* Every field checked before any is written. Storing as it goes would leave a device
     * fetching for one place and labelling the panel with another after a rejected
     * third field, which looks exactly like a correct display of the wrong weather. */
    if (!wx_coord_valid(lat, -90.0, 90.0) ||
        !wx_coord_valid(lon, -180.0, 180.0) ||
        !wx_place_valid(place, 31)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = app_cfg_set_str(NVS_KEY_LAT, lat);
    if (err == ESP_OK) { err = app_cfg_set_str(NVS_KEY_LON, lon); }
    if (err == ESP_OK) { err = app_cfg_set_str(NVS_KEY_PLACE, place); }
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "location set to %s, %s (%s); takes effect at the next fetch",
             lat, lon, place);
    return ESP_OK;
}

static void build_url(char *out, size_t len)
{
    app_wx_location_t loc;
    app_wx_get_location(&loc);
    snprintf(out, len,
        "https://api.open-meteo.com/v1/forecast"
        "?latitude=%s&longitude=%s"
        "&hourly=temperature_2m,apparent_temperature,precipitation"
        ",precipitation_probability,shortwave_radiation,cloud_cover,is_day,weather_code"
        "&daily=sunrise,sunset"
        "&forecast_hours=13&forecast_days=2&timezone=auto",
        loc.lat, loc.lon);
}

#define BODY_MAX 4096

static esp_err_t http_get(char *buf, size_t buf_len, size_t *out_len)
{
    /* Verification is not optional on this connection: it is the one that leaves the LAN,
     * where a public CA is all that stands between the device and whoever answers the DNS
     * query (PLAN 1a). A build without the bundle fails here rather than connecting
     * without checking anything. */
    app_trust_bundle_attach_fn attach = app_trust_public_bundle_attach();
    if (attach == NULL) {
        ESP_LOGE(TAG, "no public trust anchor compiled in; refusing to fetch unverified");
        return ESP_ERR_INVALID_STATE;
    }

    char url[WX_URL_MAX];
    build_url(url, sizeof(url));

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = CONFIG_APP_WX_HTTP_TIMEOUT_MS,
        .keep_alive_enable = false,
        .crt_bundle_attach = attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
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
        ESP_LOGW(TAG, "HTTP %d from Open-Meteo", status);
        err = ESP_ERR_NOT_FOUND;
        goto out;
    }
    if (len >= (int64_t)buf_len) {
        ESP_LOGW(TAG, "response is %lld bytes, buffer is %u",
                 (long long)len, (unsigned)buf_len);
        err = ESP_ERR_INVALID_SIZE;
        goto out;
    }

    /* Read until the body ends rather than trusting Content-Length: a chunked response
     * reports len < 0, and Open-Meteo does not promise not to use one. */
    size_t total = 0;
    while (total < buf_len - 1) {
        int r = esp_http_client_read(client, buf + total, (int)(buf_len - 1 - total));
        if (r < 0) {
            err = ESP_FAIL;
            goto out;
        }
        if (r == 0) {
            break;
        }
        total += (size_t)r;
    }
    buf[total] = '\0';
    *out_len = total;
    err = total > 0 ? ESP_OK : ESP_ERR_INVALID_RESPONSE;

out:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

esp_err_t app_wx_fetch(void)
{
    char *body = malloc(BODY_MAX);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t len = 0;
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = http_get(body, BODY_MAX, &len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "fetch failed: %s (the cached forecast is untouched)",
                 esp_err_to_name(err));
        free(body);
        return err;
    }

    app_wx_forecast_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    wx_parse_verdict_t v = wx_parse(body, len, &fresh);
    free(body);

    if (v != WX_PARSE_OK) {
        /* Naming the verdict matters here. "Parse failed" sends somebody to read JSON by
         * hand; "RAGGED" says the series disagreed and points at the response. */
        ESP_LOGW(TAG, "%u bytes parsed as %s (the cached forecast is untouched)",
                 (unsigned)len, wx_parse_verdict_name(v));
        return ESP_ERR_INVALID_RESPONSE;
    }

    fresh.fetched_at = time(NULL);

    if (!lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    s_forecast = fresh;
    s_have = true;
    unlock();

    esp_err_t cache_err = cache_store(&fresh);
    if (cache_err != ESP_OK) {
        /* Not fatal: the forecast is in RAM and the panel can draw it. What is lost is
         * the next boot, which will show nothing instead of this. Worth a warning rather
         * than a failure the caller has to handle. */
        ESP_LOGW(TAG, "forecast fetched but not cached: %s", esp_err_to_name(cache_err));
    }

    ESP_LOGI(TAG, "%u hours from %s in %lld ms, %s", fresh.count, app_trust_public_name(),
             (esp_timer_get_time() - t0) / 1000,
             cache_err == ESP_OK ? "cached" : "not cached");
    return ESP_OK;
}

/* --- Lifecycle and accessors -------------------------------------------------------- */

esp_err_t app_wx_start(void)
{
    if (s_lock == NULL && (s_lock = xSemaphoreCreateMutex()) == NULL) {
        return ESP_ERR_NO_MEM;
    }

    app_wx_forecast_t cached;
    memset(&cached, 0, sizeof(cached));
    esp_err_t err = cache_load(&cached);

    if (err == ESP_OK) {
        if (lock()) {
            s_forecast = cached;
            s_have = true;
            unlock();
        }
        ESP_LOGI(TAG, "cached forecast restored: %u hours, fetched %lld s ago",
                 cached.count, (long long)(time(NULL) - cached.fetched_at));
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no cached forecast; the panel shows its no-forecast screen until "
                      "the first fetch");
    } else {
        ESP_LOGW(TAG, "cached forecast unusable: %s", esp_err_to_name(err));
    }

    char url[WX_URL_MAX];
    build_url(url, sizeof(url));
    ESP_LOGI(TAG, "ready; %s verified against %s", url, app_trust_public_name());
    return ESP_OK;
}

bool app_wx_get(app_wx_forecast_t *out)
{
    if (out == NULL || !lock()) {
        return false;
    }
    bool have = s_have;
    if (have) {
        *out = s_forecast;
    }
    unlock();
    return have;
}

int64_t app_wx_age_s(void)
{
    if (!lock()) {
        return -1;
    }
    int64_t age = -1;
    if (s_have && s_forecast.fetched_at != 0) {
        age = (int64_t)time(NULL) - (int64_t)s_forecast.fetched_at;
    }
    unlock();
    return age;
}

#else /* !CONFIG_APP_WX_ENABLE */

esp_err_t app_wx_start(void) { return ESP_OK; }
esp_err_t app_wx_fetch(void) { return ESP_ERR_NOT_SUPPORTED; }
bool app_wx_get(app_wx_forecast_t *out) { (void)out; return false; }
int64_t app_wx_age_s(void) { return -1; }

/* The place name still exists in this build -- app_display draws it, which is why
 * APP_WX_PLACE lives outside the APP_WX_ENABLE guard. The coordinates do not: they are
 * only meaningful to a fetch, and there is none here, so they read empty rather than
 * inventing a location this firmware could not use. */
void app_wx_get_location(app_wx_location_t *out)
{
    if (out == NULL) {
        return;
    }
    out->lat[0] = '\0';
    out->lon[0] = '\0';
    snprintf(out->place, sizeof(out->place), "%s", CONFIG_APP_WX_PLACE);
}

esp_err_t app_wx_set_location(const char *lat, const char *lon, const char *place)
{
    (void)lat; (void)lon; (void)place;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
