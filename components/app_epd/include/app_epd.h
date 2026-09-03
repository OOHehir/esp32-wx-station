/* app_epd -- the UC8159 panel: SPI, reset, init, framebuffer upload, refresh.
 *
 * Written rather than taken from a component because nothing in the ESP Component
 * Registry drives this controller (PLAN 1d). The register values come from Pimoroni's
 * driver and are cross-checked against Soldered's independent implementation (PLAN 3d).
 *
 * A frame is uploaded in bands, because the whole 134 400-byte framebuffer does not fit
 * in this board's RAM alongside Wi-Fi and TLS (PLAN 1b). The panel does not know or care:
 * it takes its framebuffer as one long data phase, and several SPI writes with chip
 * select held low are indistinguishable from one.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the SPI bus and the control pins. Does not touch the panel. */
esp_err_t app_epd_init(void);

/* True when this build drives a panel at all. */
bool app_epd_present(void);

/* Start a frame: wake the controller, run the nine-register init, and open the data
 * phase. Every app_epd_write_band() call after this appends to the panel's RAM. */
esp_err_t app_epd_begin_frame(void);

/* Append packed 4bpp pixels, in top-to-bottom order. The total across one frame must
 * come to APP_EPD_FRAME_BYTES; short-changing it leaves the panel showing the tail of
 * whatever was there before. */
esp_err_t app_epd_write_band(const uint8_t *packed, size_t len);

/* Close the data phase and refresh the panel. Blocks for about 30 seconds -- ACeP has no
 * partial update, so there is nothing shorter to do. */
esp_err_t app_epd_end_frame(void);

/* Put the controller to sleep. The image stays on the glass without power. Called after
 * every refresh: on a panel updated hourly this is 59 minutes in 60 asleep. */
esp_err_t app_epd_sleep(void);

/* Flush the panel through its colour codes. E Ink recommend this periodically to keep an
 * ACeP panel healthy; PLAN 3e schedules it weekly. Takes about 30 seconds per pass. */
esp_err_t app_epd_clean(void);

/* Fill the panel with one colour index. */
esp_err_t app_epd_fill(uint8_t colour);

/* Seven vertical bands, one per colour, straight through the driver.
 *
 * Phase 2's milestone. A single-colour fill proves the panel refreshes; only a bar also
 * proves that a colour index in the framebuffer reaches the pigment app_gfx expects, and
 * a palette in the wrong order is invisible until something shows all seven at once. */
esp_err_t app_epd_colour_bar(void);

/* Probe the six panel pins and report what is electrically on the end of them.
 *
 * Bring-up only, and it cannot prove a wire reaches the panel: every signal except BUSY
 * drives a high-impedance input, which is indistinguishable from nothing at all. What it
 * does catch is the faults that actually happen with a dozen jumper wires -- two pins
 * shorted together, a wire on the wrong castellation, a signal tied to ground or 3v3 --
 * and whether anything is driving BUSY.
 *
 * Logs a line per pin. Returns ESP_OK if nothing anomalous was found, which is not the
 * same as the wiring being right. */
esp_err_t app_epd_selftest(void);

/* Pulse RST and watch BUSY for an answer.
 *
 * The self-test above reports a level; this reports a response, which is the only thing
 * that distinguishes a live controller from a pull-up resistor reading high. Takes half a
 * second and needs app_epd_init() to have run. ESP_ERR_INVALID_RESPONSE means nothing on
 * the BUSY line reacted to a reset, and no refresh after it can be believed. */
esp_err_t app_epd_probe_reset(void);

#define APP_EPD_WIDTH        600
#define APP_EPD_HEIGHT       448
#define APP_EPD_FRAME_BYTES  ((size_t)APP_EPD_WIDTH * APP_EPD_HEIGHT / 2)

#ifdef __cplusplus
}
#endif
