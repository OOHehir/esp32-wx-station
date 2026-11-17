#include "app_epd.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app_epd";

/* UC8159 registers. Names and values from Pimoroni's inky_uc8159.py; every one that
 * Soldered's Inkplate6Color driver also sends agrees with it (PLAN 3d). */
#define UC8159_PSR   0x00   /* panel setting            */
#define UC8159_PWR   0x01   /* power setting            */
#define UC8159_POF   0x02   /* power off                */
#define UC8159_PFS   0x03   /* power off sequence       */
#define UC8159_PON   0x04   /* power on                 */
#define UC8159_DSLP  0x07   /* deep sleep               */
#define UC8159_DTM1  0x10   /* start the data phase     */
#define UC8159_DRF   0x12   /* display refresh          */
#define UC8159_PLL   0x30
#define UC8159_TSE   0x41   /* temperature sensor       */
#define UC8159_CDI   0x50   /* VCOM and data interval   */
#define UC8159_TCON  0x60   /* gate/source non-overlap  */
#define UC8159_TRES  0x61   /* resolution               */
#define UC8159_DAM   0x65   /* external flash           */
#define UC8159_PWS   0xE3

#define BORDER_WHITE 1

/* ESP-IDF's SPI master caps a single transaction at 4092 bytes without raising this.
 * Pimoroni chunk at 4096 and Soldered at 4092 for the same reason. */
#define SPI_CHUNK    4092

#if CONFIG_APP_EPD_ENABLE

static spi_device_handle_t s_spi;
static bool s_ready;
static bool s_busy_wired;   /* set by report_busy_wiring() at init */

/* Set by busy_wait() the first time BUSY is actually seen low, cleared at the start of
 * every frame. This is the only positive evidence the panel exists: s_busy_wired says a
 * pin reads high against a pull-down, which an ordinary pull-up resistor on the panel
 * board does too. Only an assert that arrives after a command proves a controller is on
 * the end of the wire and acting on what it was sent. */
static bool s_busy_observed;
static size_t s_frame_written;
static int64_t s_frame_started_us;

/* Chip select is asserted once, in app_epd_init(), and never released.
 *
 * This is not laziness. Two independent drivers for this panel refuse to let the SPI
 * peripheral manage CS: Pimoroni set spi.no_cs and hold it low across each command and
 * its data, and Soldered drive it low at init and never raise it again (PLAN 3c, 3d).
 * The obvious ESP-IDF implementation -- spics_io_num set, hardware toggling CS between
 * the command byte and the data phase -- is the one arrangement no working driver uses,
 * and it fails by leaving the panel blank with nothing on the console to say why.
 *
 * Nothing else shares this bus, so holding CS costs nothing. */
static void cs_assert(void)
{
    gpio_set_level(CONFIG_APP_EPD_CS_GPIO, 0);
}

static esp_err_t spi_write(const uint8_t *data, size_t len)
{
    while (len > 0) {
        size_t chunk = len > SPI_CHUNK ? SPI_CHUNK : len;
        spi_transaction_t t = {
            .length = chunk * 8,
            .tx_buffer = data,
        };
        esp_err_t err = spi_device_polling_transmit(s_spi, &t);
        if (err != ESP_OK) {
            return err;
        }
        data += chunk;
        len -= chunk;
    }
    return ESP_OK;
}

static esp_err_t send_command(uint8_t cmd)
{
    gpio_set_level(CONFIG_APP_EPD_DC_GPIO, 0);
    return spi_write(&cmd, 1);
}

static esp_err_t send_data(const uint8_t *data, size_t len)
{
    gpio_set_level(CONFIG_APP_EPD_DC_GPIO, 1);
    return spi_write(data, len);
}

static esp_err_t send_cmd_data(uint8_t cmd, const uint8_t *data, size_t len)
{
    esp_err_t err = send_command(cmd);
    if (err == ESP_OK && len > 0) {
        err = send_data(data, len);
    }
    return err;
}

/* Wait for the panel to release BUSY, which is active low.
 *
 * `nominal_ms` is how long the step takes on a working panel; `timeout_ms` is when to
 * give up. When BUSY is not wired the nominal delay is used blind -- Pimoroni's driver
 * does the same thing for the same reason ("assume we're not getting a signal and wait
 * the timeout period to be safe"), and it is what lets a panel with only its BUSY line
 * wrong still refresh. That turns a dead end into a test: if the glass updates on timed
 * waits, everything except BUSY is correct.
 *
 * Every real wait is timed and logged. On a newly wired panel the durations are the only
 * evidence available that anything is happening inside the glass, and they are how the
 * first attempt here was diagnosed -- a refresh that "succeeded" in 219 ms rather than
 * the 30 seconds ACeP physically takes. */
static esp_err_t busy_wait(const char *what, uint32_t nominal_ms, uint32_t timeout_ms)
{
    if (!s_busy_wired) {
        vTaskDelay(pdMS_TO_TICKS(nominal_ms));
        return ESP_OK;
    }

    int64_t start = esp_timer_get_time();

    /* Wait for the assert by polling, not by sampling twice 20 ms apart.
     *
     * The panel is not instantaneous, but neither is every assert long. The earlier
     * version looked once, slept 20 ms and looked again, which sees a BUSY that is still
     * low 20 ms later and misses entirely one that asserts and clears inside the gap --
     * reporting "never went low" for a panel that answered.
     *
     * The window is bounded by the clock, not by a tick count: CONFIG_FREERTOS_HZ is 100
     * here, so a tick is 10 ms and pdMS_TO_TICKS(1) is 0. A loop of vTaskDelay(1) does
     * not wait a millisecond per pass, it yields and returns immediately, so counting
     * passes measures nothing. */
    if (gpio_get_level(CONFIG_APP_EPD_BUSY_GPIO) != 0) {
        const int64_t settle_us = 50 * 1000;
        int64_t settle_start = esp_timer_get_time();
        bool asserted = false;

        while (esp_timer_get_time() - settle_start < settle_us) {
            if (gpio_get_level(CONFIG_APP_EPD_BUSY_GPIO) == 0) {
                asserted = true;
                break;
            }
            vTaskDelay(0);   /* yield without sleeping a whole tick */
        }
        if (!asserted) {
            ESP_LOGW(TAG, "%s: busy never went low in %d ms; waiting %" PRIu32 " ms blind",
                     what, (int)(settle_us / 1000), nominal_ms);
            vTaskDelay(pdMS_TO_TICKS(nominal_ms));
            return ESP_OK;
        }
    }

    while (gpio_get_level(CONFIG_APP_EPD_BUSY_GPIO) == 0) {
        if ((esp_timer_get_time() - start) / 1000 > timeout_ms) {
            ESP_LOGE(TAG, "%s: timed out after %" PRIu32 " ms with busy still low",
                     what, timeout_ms);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    s_busy_observed = true;
    ESP_LOGI(TAG, "%s: %lld ms", what, (esp_timer_get_time() - start) / 1000);
    return ESP_OK;
}

/* Tell a disconnected BUSY pin from an idle panel.
 *
 * With an internal pull-up, a floating pin reads high -- exactly what an idle panel
 * reads. The two are indistinguishable until you pull the other way: a pin that follows
 * the internal pull in both directions has nothing on the end of it, while one held high
 * against a pull-down is being driven by the panel.
 *
 * Worth doing at every boot rather than once with a meter, because "busy never went low"
 * is otherwise the same log line for a miswired panel, an unpowered panel and a panel
 * that genuinely finished quickly. */
static void report_busy_wiring(void)
{
    gpio_set_pull_mode(CONFIG_APP_EPD_BUSY_GPIO, GPIO_PULLDOWN_ONLY);
    vTaskDelay(pdMS_TO_TICKS(20));
    int pulled_down = gpio_get_level(CONFIG_APP_EPD_BUSY_GPIO);

    gpio_set_pull_mode(CONFIG_APP_EPD_BUSY_GPIO, GPIO_PULLUP_ONLY);
    vTaskDelay(pdMS_TO_TICKS(20));
    int pulled_up = gpio_get_level(CONFIG_APP_EPD_BUSY_GPIO);

    /* Leave it floating: the panel drives this line, and an internal pull fighting it is
     * a milliamp of nothing useful. */
    gpio_set_pull_mode(CONFIG_APP_EPD_BUSY_GPIO, GPIO_FLOATING);

    if (pulled_down == 0 && pulled_up == 1) {
        s_busy_wired = false;
        ESP_LOGE(TAG, "BUSY (GPIO%d) follows the internal pull both ways -- nothing is "
                      "driving it. The panel is unwired, on a different pin, or unpowered.",
                 CONFIG_APP_EPD_BUSY_GPIO);
        ESP_LOGW(TAG, "falling back to timed waits: the panel will be driven blind, and "
                      "a refresh cannot be confirmed");
    } else if (pulled_down == 1) {
        s_busy_wired = true;
        ESP_LOGI(TAG, "BUSY (GPIO%d) held high against a pull-down: panel present and idle",
                 CONFIG_APP_EPD_BUSY_GPIO);
    } else {
        s_busy_wired = true;
        ESP_LOGI(TAG, "BUSY (GPIO%d) held low against a pull-up: panel present and busy",
                 CONFIG_APP_EPD_BUSY_GPIO);
    }
}

/* Assert RST and release it, without the settle delay.
 *
 * Pimoroni wait 100 ms after the release before talking to the panel and so do we, but
 * that wait belongs to the caller now. This panel answers a reset with a BUSY dip about a
 * millisecond wide, so sleeping 100 ms here meant the wake wait that follows always
 * arrived after the answer and always reported "busy never went low" -- a warning on
 * every frame, on a panel that had in fact replied. */
static void reset_panel(void)
{
    gpio_set_level(CONFIG_APP_EPD_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(CONFIG_APP_EPD_RST_GPIO, 1);
}

/* Does anything on the BUSY line answer a hardware reset?
 *
 * This is the one question report_busy_wiring() cannot settle. It finds BUSY high against
 * an internal pull-down and concludes "panel present and idle", but a pull-up resistor on
 * the panel board gives that reading with no controller behind it at all -- so does a wire
 * that has found 3v3. Every other panel signal is a high-impedance input, so BUSY is the
 * only line that can talk back, and a static level is not it talking.
 *
 * A reset is the cheapest thing that makes it talk. The UC8159 holds BUSY low while RST is
 * asserted and through the start-up that follows, so a controller that is powered and
 * connected cannot stay high across this. One that never moves is not there: the level is
 * a rail, and every "busy never went low" afterwards is that same rail, not a fast panel.
 *
 * Sampling is a poll rather than an interrupt because the assert is tens of milliseconds
 * wide on this controller and this runs once at bring-up. */
esp_err_t app_epd_probe_reset(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "--- reset response probe ---");

    int before = gpio_get_level(CONFIG_APP_EPD_BUSY_GPIO);
    bool went_low = (before == 0);
    int64_t first_low_us = 0;
    int64_t last_low_us = 0;
    int64_t start = esp_timer_get_time();

    /* Held low for the same 100 ms reset_panel() uses, then watched for 400 ms more --
     * comfortably past the UC8159's start-up, without waiting out a whole refresh. */
    gpio_set_level(CONFIG_APP_EPD_RST_GPIO, 0);
    bool released = false;
    int level_in_reset = -1;

    while (esp_timer_get_time() - start < 500 * 1000) {
        int64_t elapsed = (esp_timer_get_time() - start) / 1000;

        if (level_in_reset < 0 && elapsed >= 50) {
            level_in_reset = gpio_get_level(CONFIG_APP_EPD_BUSY_GPIO);
        }
        if (!released && elapsed >= 100) {
            gpio_set_level(CONFIG_APP_EPD_RST_GPIO, 1);
            released = true;
        }
        int level = gpio_get_level(CONFIG_APP_EPD_BUSY_GPIO);
        if (level == 0) {
            if (!went_low) {
                went_low = true;
                first_low_us = esp_timer_get_time() - start;
            }
            last_low_us = esp_timer_get_time() - start;
        }
        vTaskDelay(0);   /* yield; pdMS_TO_TICKS(1) is 0 at 100 Hz, so this is the honest
                          * spelling of the same thing. The loop is bounded above by the
                          * clock, so sampling is as dense as the scheduler allows. */
    }

    if (went_low) {
        int64_t held_ms = (last_low_us - first_low_us) / 1000;

        ESP_LOGI(TAG, "  BUSY low at %lld ms, held %lld ms; level while RST was asserted: %d",
                 first_low_us / 1000, held_ms, level_in_reset);

        /* A dip of about a millisecond, on the RST release edge, is what this panel
         * actually does -- measured, on a board whose refreshes were later confirmed
         * good. BUSY reading high throughout the assert is equally normal for it.
         *
         * An earlier version of this probe called a 1 ms answer "too brief for a
         * controller start-up" and blamed coupling from the neighbouring wire. That was
         * wrong, and the bench proved it: this reading was byte-identical on the run
         * where every refresh failed and on the run where every refresh worked. It
         * cannot tell those apart, because it never depended on what was broken -- which
         * is exactly why the wording below promises only what a reset can show. */
        ESP_LOGI(TAG, "  the controller answered a reset: it is powered, and RST and "
                      "BUSY reach it");
        ESP_LOGW(TAG, "  this proves nothing about the command path. SCLK, MOSI, CS and DC "
                      "all drive high-impedance inputs, so a disconnected one reads exactly "
                      "like a correct one here. The waits on PON and DRF below are the only "
                      "test of those four");
        ESP_LOGI(TAG, "--- probe passed ---");
        return ESP_OK;
    }

    ESP_LOGE(TAG, "  BUSY stayed high for the whole 500 ms reset, RST asserted included: "
                  "nothing answered");
    ESP_LOGE(TAG, "  the steady high on GPIO%d is a rail or a pull-up, not a controller. "
                  "Check the panel's own power first, then that BUSY reaches it",
             CONFIG_APP_EPD_BUSY_GPIO);
    ESP_LOGE(TAG, "--- probe failed ---");
    return ESP_ERR_INVALID_RESPONSE;
}

/* The nine registers of PLAN 3c, in Pimoroni's order and with Pimoroni's values. */
static esp_err_t panel_init_sequence(void)
{
    esp_err_t err;

    static const uint8_t tres[] = {
        (APP_EPD_WIDTH >> 8) & 0xFF, APP_EPD_WIDTH & 0xFF,
        (APP_EPD_HEIGHT >> 8) & 0xFF, APP_EPD_HEIGHT & 0xFF,
    };
    /* 0b11 in the top two bits selects 600x448; 0x08 selects the seven-colour mode.
     * Together: 0xEF, 0x08 -- the same two bytes Soldered send. */
    static const uint8_t psr[]  = { (0x03 << 6) | 0x2F, 0x08 };
    static const uint8_t pwr[]  = { 0x37, 0x00, 0x23, 0x23 };
    static const uint8_t pll[]  = { 0x3C };
    static const uint8_t tse[]  = { 0x00 };
    static const uint8_t cdi[]  = { (BORDER_WHITE << 5) | 0x17 };   /* 0x37 */
    static const uint8_t tcon[] = { 0x22 };
    static const uint8_t dam[]  = { 0x00 };
    static const uint8_t pws[]  = { 0xAA };
    static const uint8_t pfs[]  = { 0x00 };

    if ((err = send_cmd_data(UC8159_TRES, tres, sizeof(tres))) != ESP_OK) { return err; }
    if ((err = send_cmd_data(UC8159_PSR,  psr,  sizeof(psr)))  != ESP_OK) { return err; }
    if ((err = send_cmd_data(UC8159_PWR,  pwr,  sizeof(pwr)))  != ESP_OK) { return err; }
    if ((err = send_cmd_data(UC8159_PLL,  pll,  sizeof(pll)))  != ESP_OK) { return err; }
    if ((err = send_cmd_data(UC8159_TSE,  tse,  sizeof(tse)))  != ESP_OK) { return err; }
    if ((err = send_cmd_data(UC8159_CDI,  cdi,  sizeof(cdi)))  != ESP_OK) { return err; }
    if ((err = send_cmd_data(UC8159_TCON, tcon, sizeof(tcon))) != ESP_OK) { return err; }
    if ((err = send_cmd_data(UC8159_DAM,  dam,  sizeof(dam)))  != ESP_OK) { return err; }
    if ((err = send_cmd_data(UC8159_PWS,  pws,  sizeof(pws)))  != ESP_OK) { return err; }
    if ((err = send_cmd_data(UC8159_PFS,  pfs,  sizeof(pfs)))  != ESP_OK) { return err; }
    return ESP_OK;
}

/* --- Bring-up self-test ------------------------------------------------------------ */

struct pin_desc {
    const char *name;
    int gpio;
};

static const struct pin_desc PANEL_PINS[] = {
    { "SCLK", CONFIG_APP_EPD_SCLK_GPIO },
    { "MOSI", CONFIG_APP_EPD_MOSI_GPIO },
    { "CS  ", CONFIG_APP_EPD_CS_GPIO   },
    { "DC  ", CONFIG_APP_EPD_DC_GPIO   },
    { "RST ", CONFIG_APP_EPD_RST_GPIO  },
    { "BUSY", CONFIG_APP_EPD_BUSY_GPIO },
};
#define PANEL_PIN_COUNT (sizeof(PANEL_PINS) / sizeof(PANEL_PINS[0]))

static void float_all_pins(void)
{
    for (size_t i = 0; i < PANEL_PIN_COUNT; i++) {
        gpio_config_t c = {
            .pin_bit_mask = 1ULL << PANEL_PINS[i].gpio,
            .mode = GPIO_MODE_INPUT,
        };
        gpio_config(&c);
        gpio_set_pull_mode(PANEL_PINS[i].gpio, GPIO_FLOATING);
    }
}

static int read_with_pull(int gpio, gpio_pull_mode_t pull)
{
    gpio_set_pull_mode(gpio, pull);
    vTaskDelay(pdMS_TO_TICKS(5));
    return gpio_get_level(gpio);
}

esp_err_t app_epd_selftest(void)
{
    int anomalies = 0;
    bool stuck[PANEL_PIN_COUNT] = { false };
    float_all_pins();

    ESP_LOGI(TAG, "--- panel pin self-test ---");

    /* Held high or low by something external? A pin that will not follow an internal
     * pull is shorted to a rail, or to a neighbouring wire that is. */
    for (size_t i = 0; i < PANEL_PIN_COUNT; i++) {
        int gpio = PANEL_PINS[i].gpio;
        int up = read_with_pull(gpio, GPIO_PULLUP_ONLY);
        int down = read_with_pull(gpio, GPIO_PULLDOWN_ONLY);
        gpio_set_pull_mode(gpio, GPIO_FLOATING);

        if (up == 1 && down == 0) {
            ESP_LOGI(TAG, "  %s GPIO%-2d  floating (a high-impedance panel input reads "
                          "the same as an unconnected wire)", PANEL_PINS[i].name, gpio);
        } else if (up == 1 && down == 1) {
            /* On BUSY this is the healthy reading, not a fault: the panel drives that
             * line and holds it high whenever it is idle, which is exactly when this
             * runs. Counting it made a fully working board open every boot with "1
             * anomaly found; fix it before anything else". Still excluded from the
             * pairwise test below -- a driven pin reads as shorted to everything.
             *
             * Anywhere else, a pin that will not follow an internal pull-down has found
             * a rail. */
            bool is_busy = (gpio == CONFIG_APP_EPD_BUSY_GPIO);
            if (is_busy) {
                ESP_LOGI(TAG, "  %s GPIO%-2d  held high -- an idle panel drives this, so "
                              "this is what a connected one looks like",
                         PANEL_PINS[i].name, gpio);
            } else {
                ESP_LOGW(TAG, "  %s GPIO%-2d  HELD HIGH -- shorted to 3v3, or driven",
                         PANEL_PINS[i].name, gpio);
                anomalies++;
            }
            stuck[i] = true;
        } else {
            ESP_LOGW(TAG, "  %s GPIO%-2d  HELD LOW -- shorted to ground",
                     PANEL_PINS[i].name, gpio);
            stuck[i] = true;
            anomalies++;
        }
    }

    /* Shorts between panel wires. Drive one low, pull the rest up, and see which follow.
     * This is the fault a dozen jumper wires actually produce.
     *
     * Pins already known stuck are excluded from BOTH sides. A pin shorted to ground
     * reads low no matter what is driven, so including it reports a short against every
     * other pin on the panel -- which is what the first run of this test did: one wire on
     * a ground pin was reported as five separate shorts, and the real fault had to be
     * picked out of the noise. One fault should read as one line. */
    for (size_t i = 0; i < PANEL_PIN_COUNT; i++) {
        if (stuck[i]) {
            continue;
        }
        gpio_config_t out = {
            .pin_bit_mask = 1ULL << PANEL_PINS[i].gpio,
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&out);
        gpio_set_level(PANEL_PINS[i].gpio, 0);

        for (size_t j = 0; j < PANEL_PIN_COUNT; j++) {
            if (i == j || stuck[j]) {
                continue;
            }
            if (read_with_pull(PANEL_PINS[j].gpio, GPIO_PULLUP_ONLY) == 0) {
                ESP_LOGE(TAG, "  SHORT: %s (GPIO%d) and %s (GPIO%d) are connected",
                         PANEL_PINS[i].name, PANEL_PINS[i].gpio,
                         PANEL_PINS[j].name, PANEL_PINS[j].gpio);
                anomalies++;
            }
            gpio_set_pull_mode(PANEL_PINS[j].gpio, GPIO_FLOATING);
        }
        float_all_pins();
    }

    if (anomalies == 0) {
        ESP_LOGI(TAG, "--- no shorts and no stuck pins. This does NOT prove the wires "
                      "reach the panel; only BUSY can show that ---");
    } else if (anomalies == 1) {
        ESP_LOGE(TAG, "--- 1 anomaly found; fix it before anything else ---");
    } else {
        ESP_LOGE(TAG, "--- %d anomalies found; fix these before anything else ---",
                 anomalies);
    }
    return anomalies == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t app_epd_init(void)
{
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << CONFIG_APP_EPD_CS_GPIO)
                      | (1ULL << CONFIG_APP_EPD_DC_GPIO)
                      | (1ULL << CONFIG_APP_EPD_RST_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&out));

    gpio_config_t in = {
        .pin_bit_mask = 1ULL << CONFIG_APP_EPD_BUSY_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in));

    gpio_set_level(CONFIG_APP_EPD_RST_GPIO, 1);
    gpio_set_level(CONFIG_APP_EPD_DC_GPIO, 0);

    spi_bus_config_t bus = {
        .mosi_io_num = CONFIG_APP_EPD_MOSI_GPIO,
        .miso_io_num = -1,          /* the UC8159 is never read; see PLAN 3b on why this
                                     * matters more than tidiness on an ESP32-C3 */
        .sclk_io_num = CONFIG_APP_EPD_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SPI_CHUNK,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    /* Weakest drive (~5 mA) for slower edges. On jumper wires a fast edge
     * can ring past the threshold twice, and with CS held low the panel counts the extra
     * edge as a bit, shifting the rest of the frame. Set after spi_bus_initialize(),
     * which configures these pins. */
    ESP_ERROR_CHECK(gpio_set_drive_capability(CONFIG_APP_EPD_SCLK_GPIO, GPIO_DRIVE_CAP_0));
    ESP_ERROR_CHECK(gpio_set_drive_capability(CONFIG_APP_EPD_MOSI_GPIO, GPIO_DRIVE_CAP_0));

    spi_device_interface_config_t dev = {
        .clock_speed_hz = CONFIG_APP_EPD_SPI_HZ,
        .mode = 0,
        .spics_io_num = -1,         /* CS is ours, not the peripheral's -- see cs_assert */
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &s_spi));

    cs_assert();
    s_ready = true;

    ESP_LOGI(TAG, "UC8159 on SPI2: sclk=%d mosi=%d cs=%d dc=%d rst=%d busy=%d @ %d Hz",
             CONFIG_APP_EPD_SCLK_GPIO, CONFIG_APP_EPD_MOSI_GPIO, CONFIG_APP_EPD_CS_GPIO,
             CONFIG_APP_EPD_DC_GPIO, CONFIG_APP_EPD_RST_GPIO, CONFIG_APP_EPD_BUSY_GPIO,
             CONFIG_APP_EPD_SPI_HZ);
    report_busy_wiring();
    return ESP_OK;
}

bool app_epd_present(void)
{
    return s_ready;
}

esp_err_t app_epd_begin_frame(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    reset_panel();

    esp_err_t err = busy_wait("wake", 1000, 3000);
    if (err != ESP_OK) {
        return err;
    }
    /* The settle reset_panel() used to do. Kept at Pimoroni's 100 ms, just moved to after
     * the wake response has been given a chance to be seen. */
    vTaskDelay(pdMS_TO_TICKS(100));
    if ((err = panel_init_sequence()) != ESP_OK) {
        return err;
    }

    s_frame_written = 0;
    s_frame_started_us = esp_timer_get_time();
    return send_command(UC8159_DTM1);
}

esp_err_t app_epd_write_band(const uint8_t *packed, size_t len)
{
    if (!s_ready || packed == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_frame_written + len > APP_EPD_FRAME_BYTES) {
        ESP_LOGE(TAG, "band overruns the frame: %u + %u > %u",
                 (unsigned)s_frame_written, (unsigned)len, (unsigned)APP_EPD_FRAME_BYTES);
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = send_data(packed, len);
    if (err == ESP_OK) {
        s_frame_written += len;
    }
    return err;
}

esp_err_t app_epd_end_frame(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_frame_written != APP_EPD_FRAME_BYTES) {
        /* Refusing is kinder than refreshing. A short frame leaves the tail of the
         * previous image on the glass, which looks like a rendering bug thirty seconds
         * later rather than a caller that miscounted. */
        ESP_LOGE(TAG, "frame is %u bytes, expected %u -- not refreshing",
                 (unsigned)s_frame_written, (unsigned)APP_EPD_FRAME_BYTES);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Cleared here rather than in begin_frame(): only a response to PON, DRF or POF
     * shows the panel acting on this frame. */
    s_busy_observed = false;

    /* How long the framebuffer took to reach the panel, separately from the refresh that
     * follows it. The two are unrelated costs -- the upload is the SPI clock, the refresh
     * is the physics -- and only the first one is worth tuning (PLAN 3c). */
    int64_t upload_ms = (esp_timer_get_time() - s_frame_started_us) / 1000;
    ESP_LOGI(TAG, "frame uploaded: %u bytes in %lld ms at %d Hz",
             (unsigned)s_frame_written, upload_ms, CONFIG_APP_EPD_SPI_HZ);

    esp_err_t err;
    if ((err = send_command(UC8159_PON)) != ESP_OK) { return err; }
    if ((err = busy_wait("power on", 200, 3000)) != ESP_OK) { return err; }

    if ((err = send_command(UC8159_DRF)) != ESP_OK) { return err; }
    if ((err = busy_wait("refresh", 32000, 45000)) != ESP_OK) { return err; }

    if ((err = send_command(UC8159_POF)) != ESP_OK) { return err; }
    if ((err = busy_wait("power off", 200, 3000)) != ESP_OK) { return err; }

    /* The panel was driven, but if BUSY never asserted then nothing observed it doing
     * anything. Say so rather than returning ESP_OK: the first run of this driver logged
     * "fill returned ESP_OK" after a 219 ms refresh, which is not a success, it is a
     * report of one.
     *
     * The test is what happened during this frame, not what report_busy_wiring() guessed
     * at init. A panel board with a pull-up on BUSY reads "present and idle" whether or
     * not a controller is behind it, so gating on s_busy_wired let exactly the same false
     * success back through -- four blind waits and an ESP_OK. */
    if (!s_busy_observed) {
        ESP_LOGE(TAG, "refresh unconfirmed: BUSY never asserted at any point in this "
                      "frame, so nothing witnessed the panel act on it");
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t app_epd_sleep(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    static const uint8_t arg = 0xA5;
    return send_cmd_data(UC8159_DSLP, &arg, 1);
}

esp_err_t app_epd_fill(uint8_t colour)
{
    /* One row at a time: 300 bytes on the stack rather than 134 400 on the heap, and the
     * panel cannot tell the difference between one write and 448 of them. */
    uint8_t row[APP_EPD_WIDTH / 2];
    uint8_t packed = (uint8_t)(((colour & 0x07) << 4) | (colour & 0x07));
    memset(row, packed, sizeof(row));

    esp_err_t err = app_epd_begin_frame();
    if (err != ESP_OK) {
        return err;
    }
    for (int y = 0; y < APP_EPD_HEIGHT; y++) {
        if ((err = app_epd_write_band(row, sizeof(row))) != ESP_OK) {
            return err;
        }
    }
    return app_epd_end_frame();
}

/* Seven vertical bands, one per ACeP colour, full height.
 *
 * This is phase 2's milestone and it is a colour test, not a picture: it goes through the
 * driver alone, with no app_gfx surface and no app_ui layout in the way. A panel that
 * shows seven correct bands has proved the reset, the nine registers, the data phase, the
 * refresh and -- the part nothing else checks -- that a colour index in the framebuffer
 * lands on the pigment app_gfx thinks it does. If the bands come out in the wrong order,
 * the fault is the palette, not the rendering.
 *
 * Every row is identical because the bands are vertical, so one packed row is built once
 * and written 448 times. */
esp_err_t app_epd_colour_bar(void)
{
    uint8_t row[APP_EPD_WIDTH / 2];

    for (size_t b = 0; b < sizeof(row); b++) {
        /* Two pixels per byte, left-hand one in the high nibble (PLAN 3c). Integer
         * division puts the band edges at 600/7 = 85.7 px, so the bands differ by a pixel
         * in width, which no eye reads on 600. */
        uint8_t left  = (uint8_t)((2 * b) * 7 / APP_EPD_WIDTH);
        uint8_t right = (uint8_t)((2 * b + 1) * 7 / APP_EPD_WIDTH);
        row[b] = (uint8_t)(((left & 0x07) << 4) | (right & 0x07));
    }

    esp_err_t err = app_epd_begin_frame();
    if (err != ESP_OK) {
        return err;
    }
    for (int y = 0; y < APP_EPD_HEIGHT; y++) {
        if ((err = app_epd_write_band(row, sizeof(row))) != ESP_OK) {
            return err;
        }
    }
    return app_epd_end_frame();
}

esp_err_t app_epd_clean(void)
{
    /* Black, then white. Two full passes is what Pimoroni's own cleaning example does,
     * and it is what lifts colour that has settled into the particles. */
    esp_err_t err = app_epd_fill(0);
    if (err != ESP_OK) {
        return err;
    }
    return app_epd_fill(1);
}

#else /* !CONFIG_APP_EPD_ENABLE */

/* --- Bring-up self-test ------------------------------------------------------------ */

struct pin_desc {
    const char *name;
    int gpio;
};

static const struct pin_desc PANEL_PINS[] = {
    { "SCLK", CONFIG_APP_EPD_SCLK_GPIO },
    { "MOSI", CONFIG_APP_EPD_MOSI_GPIO },
    { "CS  ", CONFIG_APP_EPD_CS_GPIO   },
    { "DC  ", CONFIG_APP_EPD_DC_GPIO   },
    { "RST ", CONFIG_APP_EPD_RST_GPIO  },
    { "BUSY", CONFIG_APP_EPD_BUSY_GPIO },
};
#define PANEL_PIN_COUNT (sizeof(PANEL_PINS) / sizeof(PANEL_PINS[0]))

static void float_all_pins(void)
{
    for (size_t i = 0; i < PANEL_PIN_COUNT; i++) {
        gpio_config_t c = {
            .pin_bit_mask = 1ULL << PANEL_PINS[i].gpio,
            .mode = GPIO_MODE_INPUT,
        };
        gpio_config(&c);
        gpio_set_pull_mode(PANEL_PINS[i].gpio, GPIO_FLOATING);
    }
}

static int read_with_pull(int gpio, gpio_pull_mode_t pull)
{
    gpio_set_pull_mode(gpio, pull);
    vTaskDelay(pdMS_TO_TICKS(5));
    return gpio_get_level(gpio);
}

esp_err_t app_epd_selftest(void)
{
    int anomalies = 0;
    bool stuck[PANEL_PIN_COUNT] = { false };
    float_all_pins();

    ESP_LOGI(TAG, "--- panel pin self-test ---");

    /* Held high or low by something external? A pin that will not follow an internal
     * pull is shorted to a rail, or to a neighbouring wire that is. */
    for (size_t i = 0; i < PANEL_PIN_COUNT; i++) {
        int gpio = PANEL_PINS[i].gpio;
        int up = read_with_pull(gpio, GPIO_PULLUP_ONLY);
        int down = read_with_pull(gpio, GPIO_PULLDOWN_ONLY);
        gpio_set_pull_mode(gpio, GPIO_FLOATING);

        if (up == 1 && down == 0) {
            ESP_LOGI(TAG, "  %s GPIO%-2d  floating (a high-impedance panel input reads "
                          "the same as an unconnected wire)", PANEL_PINS[i].name, gpio);
        } else if (up == 1 && down == 1) {
            /* On BUSY this is the healthy reading, not a fault: the panel drives that
             * line and holds it high whenever it is idle, which is exactly when this
             * runs. Counting it made a fully working board open every boot with "1
             * anomaly found; fix it before anything else". Still excluded from the
             * pairwise test below -- a driven pin reads as shorted to everything.
             *
             * Anywhere else, a pin that will not follow an internal pull-down has found
             * a rail. */
            bool is_busy = (gpio == CONFIG_APP_EPD_BUSY_GPIO);
            if (is_busy) {
                ESP_LOGI(TAG, "  %s GPIO%-2d  held high -- an idle panel drives this, so "
                              "this is what a connected one looks like",
                         PANEL_PINS[i].name, gpio);
            } else {
                ESP_LOGW(TAG, "  %s GPIO%-2d  HELD HIGH -- shorted to 3v3, or driven",
                         PANEL_PINS[i].name, gpio);
                anomalies++;
            }
            stuck[i] = true;
        } else {
            ESP_LOGW(TAG, "  %s GPIO%-2d  HELD LOW -- shorted to ground",
                     PANEL_PINS[i].name, gpio);
            stuck[i] = true;
            anomalies++;
        }
    }

    /* Shorts between panel wires. Drive one low, pull the rest up, and see which follow.
     * This is the fault a dozen jumper wires actually produce.
     *
     * Pins already known stuck are excluded from BOTH sides. A pin shorted to ground
     * reads low no matter what is driven, so including it reports a short against every
     * other pin on the panel -- which is what the first run of this test did: one wire on
     * a ground pin was reported as five separate shorts, and the real fault had to be
     * picked out of the noise. One fault should read as one line. */
    for (size_t i = 0; i < PANEL_PIN_COUNT; i++) {
        if (stuck[i]) {
            continue;
        }
        gpio_config_t out = {
            .pin_bit_mask = 1ULL << PANEL_PINS[i].gpio,
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&out);
        gpio_set_level(PANEL_PINS[i].gpio, 0);

        for (size_t j = 0; j < PANEL_PIN_COUNT; j++) {
            if (i == j || stuck[j]) {
                continue;
            }
            if (read_with_pull(PANEL_PINS[j].gpio, GPIO_PULLUP_ONLY) == 0) {
                ESP_LOGE(TAG, "  SHORT: %s (GPIO%d) and %s (GPIO%d) are connected",
                         PANEL_PINS[i].name, PANEL_PINS[i].gpio,
                         PANEL_PINS[j].name, PANEL_PINS[j].gpio);
                anomalies++;
            }
            gpio_set_pull_mode(PANEL_PINS[j].gpio, GPIO_FLOATING);
        }
        float_all_pins();
    }

    if (anomalies == 0) {
        ESP_LOGI(TAG, "--- no shorts and no stuck pins. This does NOT prove the wires "
                      "reach the panel; only BUSY can show that ---");
    } else if (anomalies == 1) {
        ESP_LOGE(TAG, "--- 1 anomaly found; fix it before anything else ---");
    } else {
        ESP_LOGE(TAG, "--- %d anomalies found; fix these before anything else ---",
                 anomalies);
    }
    return anomalies == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t app_epd_init(void) { return ESP_OK; }
bool      app_epd_present(void) { return false; }
esp_err_t app_epd_begin_frame(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t app_epd_write_band(const uint8_t *p, size_t n) { (void)p; (void)n; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t app_epd_end_frame(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t app_epd_sleep(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t app_epd_clean(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t app_epd_fill(uint8_t c) { (void)c; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t app_epd_probe_reset(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t app_epd_colour_bar(void) { return ESP_ERR_NOT_SUPPORTED; }

#endif
