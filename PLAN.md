# esp32-wx-station — plan for a 7-colour ePaper weather display on ESP32

Status: **phases 0 & 1 done. Phase 2 is next & needs the panel.** The XIAO ESP32-C3 runs
this firmware on the LAN — associated, time synced, answering `/api/status` &
`/api/ota/check` — & §1b's heap figures are measured rather than estimated. `app_gfx`,
`app_ui` & `app_wx`'s types are in, with 228 host checks & no ESP-IDF needed to run them.
**No panel has been driven**, so everything from §3c onward is still unproven: the
register values, the chip-select behaviour, the 3v3 draw & the refresh timing.

Every figure below is either read out of a datasheet, read out of vendor driver source,
or measured here on 2026-08-28 — each is marked as such. Measurements taken on the bench
board are marked *observed*, never stated as requirements.

Goal: a XIAO ESP32-C3 driving a **Pimoroni Inky Impression 5.7"** (600 × 448, seven
colours) that wakes hourly, fetches a 12-hour forecast & redraws temperature, rainfall &
sunlight. Built as a fork of [`esp-ota`](../esp-ota), so it is updatable over the air from
the NAS without a cable for the rest of its life.

The register values in §3c are the load-bearing part & are cross-checked against two
independent vendor implementations in §3d. The "write it ourselves" decisions are checked
against the ESP Component Registry in §1d rather than assumed.

---

## 1. Decisions to lock before writing code

| Decision | Recommendation | Why |
|---|---|---|
| Base | **Fork `esp-ota`** per its `docs/start-a-new-project.md` | Wi-Fi, SoftAP commissioning, SNTP, mDNS, NVS config, signed OTA with rollback, crash dumps & the status page all arrive working. `main/` then holds two lines & the weather app is five components. See §5 |
| Target chip | **Seeed XIAO ESP32-C3 — sufficient**, with consequences | 4 MB flash, 400 KB SRAM, **no PSRAM**. Measured: this firmware builds at 1 118 208 bytes, **41 % of the slot free**. RAM is the binding constraint & it forces banded rendering (§1b, §5a). Three of its eleven GPIO are strapping pins & one of them is a trap (§3b) |
| Panel driver | **Write one.** `app_epd`, ~400 lines | Registry searched, not assumed: **`q=uc8159` returns nothing** (§1d). The nearest component targets a different controller, hard-depends on LVGL & toggles chip select in the one way this panel does not accept. The init sequence is nine registers (§3c), cross-checked against a second vendor (§3d) |
| Graphics | **Write one.** `app_gfx`: banded surfaces, clipping, primitives, fonts | LVGL has no indexed-7-colour render path, & on a board with no PSRAM the drawing model is banded anyway (§5a). Fonts converted from TTF at build time. Revisit once, at the end of phase 1 — see §1d |
| Weather source | **Open-Meteo**, `api.open-meteo.com/v1/forecast` | Free, no API key, no account, HTTPS, & the exact hourly variables wanted. Verified response for the 12-hour query is **1469 bytes** (§4). OpenWeatherMap was already in service in `led_thermometer` & is rejected here — see §1c |
| Trust anchor | **Decided: two anchors.** Private CA for the NAS, IDF root bundle enforced against Open-Meteo | The one real architectural change to the template. Verification is enforced on both; they simply answer to different anchors, because only one of the two servers is on the LAN. See §1a |
| Power | **Mains, always on. No ESP32 deep sleep** | The panel needs a 5 V rail it cannot supply itself (§3b), & OTA polling, mDNS & the status page all assume a device that is awake. *Controller* deep sleep between refreshes is separate & is taken (§3c). ESP32 deep sleep is deferred, not designed out (§8) |
| Refresh cadence | **Hourly, on the hour**, driven by SNTP time | Matches the data: Open-Meteo publishes hourly. Well inside E Ink's minimum refresh interval for this panel family |

### 1a. The trust fork — the template assumes one anchor & this project has two

`components/app_trust` in `esp-ota` is explicit about it:

> One anchor, one place, and no way for two call sites to quietly differ about it.

Both consumers take the same pair of fields:

```c
cfg->cert_pem          = app_trust_ca_pem();          /* app_ota.c:44  */
cfg->crt_bundle_attach = app_trust_bundle_attach();   /* app_ota.c:45  */
```

Exactly one of the two is non-NULL, chosen by a Kconfig `choice`. That works while every
TLS connection goes to the same NAS. This project breaks it: the OTA server is on the LAN
with a certificate from a CA that was made locally, & `api.open-meteo.com` is on the
public internet with a certificate from Let's Encrypt. No single anchor covers both.

**Decided 2026-08-28: option 1, two anchors.** Certificate verification is *enforced*
against Open-Meteo, which is the connection that leaves the house and the one where a
public CA is the only thing standing between the device and whoever answers the DNS query.
The NAS keeps the private CA it already has: it is on the local network, the CA was made
here, and that is a reasonable trust boundary for a firmware server nobody outside the LAN
can reach. Neither connection is unverified — the two just answer to different anchors.

Three ways out, in order of preference:

1. **Give `app_trust` a second, named anchor.** ← **chosen** `app_trust_public_*()` alongside the
   existing pair, selecting `MBEDTLS_CERTIFICATE_BUNDLE` whenever the weather client is
   enabled. The OTA anchor is untouched, so the decision that cannot be changed after
   devices ship stays exactly as it was, & the diff against upstream is small enough to
   carry. **Recommended.**
2. **Reverse-proxy Open-Meteo through the NAS.** One `location` block in the nginx stack
   `esp-ota` already documents, & the device then makes every TLS connection to a
   certificate its own CA issued. Genuinely attractive for a no-cloud house — it keeps the
   device's trust surface at exactly one CA, & gives free response caching for a fleet.
   The cost is that the display goes stale when the NAS is down, which is a different
   failure from the display going stale when the internet is down.
3. **Pin ISRG Root X1 alone** for the weather client. Smaller than the 64 KB bundle &
   tighter than trusting 144 roots, but it puts a third-party CA's rotation schedule on
   the critical path of a device meant to run for years untouched.

> **Verified 2026-08-28, & the reason option 1 works at all:** `api.open-meteo.com`
> presents `open-meteo.com` ← `Let's Encrypt YR2` ← **`ISRG Root YR`**. `ISRG Root YR` is
> **not** in the ESP-IDF v6.0.2 bundle. The chain validates only because the `Root YR` the
> server sends is itself cross-signed by `ISRG Root X1`, which **is** in the bundle, & in
> the `cmn` subset too. See §8 — a live dependency on somebody else's server
> configuration, not on their root store.

### 1b. The framebuffer, & why the XIAO forces the architecture

600 × 448 = 268 800 pixels. A whole frame does not fit on this board, & that is the single
fact that shapes `app_gfx`.

| Representation | Bytes |
|---|---|
| Whole frame, 1 byte per pixel, drawing-friendly | 268 800 |
| Whole frame, packed 4 bits per pixel, wire format | 134 400 |
| **One 32-row band, 1 B/px + its packed copy** | **28 800** |

**Observed on the bench board, 2026-08-28**, building & running this firmware for
`esp32c3` at ESP-IDF v6.0.2:

```
esp32-wx-station.bin binary size 0x111000 bytes. Smallest app partition is 0x1d0000
bytes. 0xbf000 bytes (41%) free.

heap_init: At 3FC98E60 len 000271A0 (156 KiB): RAM
heap_init: At 3FCC0000 len 0001C710 (113 KiB): Retention RAM
heap_init: At 3FCDC710 len 00002950 (10 KiB): Retention RAM
heap_init: At 50000024 len 00001FC4 (7 KiB): RTCRAM

app_diag:  heap    269740 free
```

Where:

- **Flash is comfortable.** 1 118 208 bytes into an 1 810 432-byte slot, 41 % free, before
  a line of display code. `partitions_4mb.csv` gives two slots & still leaves SPIFFS &
  coredump partitions
- **269 740 bytes of heap at the boot banner** — but that figure is printed *before* the
  Wi-Fi driver starts, & the log shows Wi-Fi initialising immediately after
- **The number that decides the architecture is 156 KiB.** That is the largest *contiguous*
  pool; the other 113 KiB is retention RAM in a separate region. A single 134 400-byte
  packed frame is 131 KiB & must come from one pool, leaving 25 KiB of main RAM for
  Wi-Fi, lwIP, mbedTLS & every task stack. It is not viable, & no amount of tuning makes
  it viable

**Decision: render in bands, on every target.** 32 rows is 600 × 32 = 19 200 bytes of
1-byte-per-pixel surface plus 9 600 bytes packed — 28 800 bytes, & 448 / 32 = 14 bands
exactly. §5a makes this the only drawing model rather than a fallback, because a seam only
exercised on one chip is a seam that rots.

> **Note:** an earlier draft recommended PSRAM & a whole-frame buffer on an ESP32-S3. The
> XIAO ESP32-C3 has **no PSRAM** — confirmed by `esptool flash-id`, which reports
> "Embedded Flash 4MB (XMC)" & no PSRAM. This is the better outcome: banding is the only
> model that runs unchanged on every Espressif part, which is the chip-agnosticism
> `esp-ota` asks for in its own PLAN §1a.

**Measured 2026-08-28, with the board on the LAN.** `GET /api/status` while associated,
with mDNS, SNTP & the HTTP server all up, then `POST /api/ota/check` to force a TLS
attempt:

| Moment | `heap_free` | `heap_min` |
|---|---|---|
| Boot banner, before Wi-Fi starts | 269 872 | — |
| Associated; mDNS, SNTP & httpd up | 196 156 | 195 940 |
| **Low-water during a TLS attempt** | 195 576 | **157 860** |

Where:

- **The network stack costs about 73.7 KB** — 269 872 down to 196 156
- **A TLS session costs a further ~37.7 KB**, & that is the figure that matters: 157 860
  bytes is the worst case the display code must survive. The attempt failed at connect
  (`ESP_ERR_HTTP_CONNECT`, the NAS was not running), so mbedTLS had already allocated its
  buffers; a successful fetch adds little to the peak
- **This settles §1b as a measurement rather than an argument.** A 134 400-byte packed
  frame against a 157 860-byte low-water mark leaves 23 KB for everything else, & that is
  before asking whether 131 KB is available *contiguously* in a 156 KiB pool. One 32-row
  band is 28 800 bytes, or 18 % of the same budget

### 1c. `led_thermometer` — assessment

[`led_thermometer`](../led_thermometer) is the nearest prior art in this tree: an ESP32-C3
that fetches a forecast & shows minimum, current & maximum temperature on a 50-LED WS2812
strip. It works & it is in service. It is nonetheless **not** the base for this project, &
almost none of it transfers.

| Component | Verdict | Why |
|---|---|---|
| `http_get` | **Do not reuse** | Plain HTTP on port 80 over raw POSIX sockets, no TLS at all. See the four faults below |
| `light_driver` | Not applicable | WS2812 over RMT. `esp-ota`'s `app_led` already covers addressable LEDs, with pin & type in `sdkconfig.defaults.<target>` where they belong |
| `mqtt_handle` | Superseded | The stock IDF example with hard-coded topics (`/topic/qos1`, `cmnd/stairs/power`). `app_mqtt` has retained status, a last will & fleet commands |
| `pytest_http_request.py` | Dead file | Espressif's example test, unmodified. It asserts on `http_request.bin` & a connection to `example.com`; neither exists in this project |

`http_get` is the one that looks reusable & is not. It is a lightly edited copy of the IDF
`http_request` example — its own header comment says so — & four things rule it out:

- **No TLS.** `WEB_PORT "80"` & a hand-rolled `GET ... HTTP/1.0`. Adding TLS is not an
  edit to this file; it is `esp_http_client` instead of this file, which is what §5's
  `app_wx` already specifies
- **The parser cannot survive this project's response.** It runs `sscanf` for
  `"temp_min":%d` across a 64-byte `recv_buf` that is zeroed & refilled on every `read()`,
  so **any token straddling a read boundary is silently missed**. Survivable when a missed
  minimum leaves the previous one on a strip; not survivable for 13 hours × 7 variables,
  where a hole is a wrong chart rather than a missing one
- **`%d` on a float field.** OpenWeatherMap sends `"temp":9.34`; `%d` yields `9`, & −0.6
  truncates to 0. Right for one LED per degree, wrong for a temperature line
- **It fights `app_net`.** `http_get_task()` calls `esp_wifi_connect()` in a loop
  (`http_get.c:77`). `esp-ota` gives Wi-Fi a state machine that owns the radio; a second
  caller poking it is a merge conflict at run time rather than at build time

> **One latent panic, worth fixing in `led_thermometer` itself regardless of this
> project:** `ESP_ERROR_CHECK(close(s))` at `http_get.c:118`, `:130` & `:167`. `close()`
> returns −1 on failure, not an `esp_err_t`, & `ESP_ERROR_CHECK(-1)` aborts the device. A
> socket that fails to close therefore reboots the thermometer instead of logging.

**Licence.** `led_thermometer` is Apache 2.0, & `light_driver.[ch]` additionally carry
Espressif's `LicenseRef-Included`, whose clause 4 forbids reverse engineering. `esp-ota`'s
README records its own licence as not yet chosen. Copying files across drags those headers
along & settles a question deliberately left open.

**What does transfer, & it is worth having:**

- **OpenWeatherMap, rejected with a reason rather than by default.** The free tier's 5-day
  forecast is on **3-hour** steps, & `led_thermometer` gets twelve hours only by `cnt=4`
  × 3 h. This display promises *hourly* resolution over twelve hours, which that endpoint
  cannot supply at any `cnt`. OWM also needs an API key, a secret to provision & rotate.
  Open-Meteo needs neither
- **`sensitive_data.h` is the pattern to stop using.** `led_thermometer` keeps its API key
  in a gitignored header, so the key is compiled into the image — & an OTA-updated fleet
  would carry it in every published binary on the NAS. `esp-ota` already solved this
  twice: `sdkconfig.local` for build-time site settings, & `app_cfg` for NVS-backed
  runtime configuration
- **The minimum / current / maximum framing** is a good idea & is kept: §6 annotates the
  temperature panel with both extremes rather than only plotting the line
- **The location.** `common.h` sets `OPENWEATHERMAP_LOCATION "Dublin,IE"`, which probably
  answers §9's third open question

### 1d. Existing components — survey

The "write one" decisions above are only defensible if nothing already does the job, so
the ESP Component Registry was searched rather than assumed. **`q=uc8159` returns zero
results.** Nothing in the registry drives this controller. The candidates that came
closest:

| Component | Licence | Verdict |
|---|---|---|
| `tuanpmt/esp_epaper` 2.0.0 | MIT | **Closest by far.** Right architecture, wrong controller & three blockers — below |
| `solderedelectronics/inkplate` 1.2.0 | **GPL-3.0-only** | Drives this exact panel & controller. Unusable as a dependency, invaluable as a reference — see §3d |
| `cleishm/idfxx_epaper` 1.0.0 | Apache-2.0 | Clean, modular, host-tested — & 1 bpp mono or 2 bpp grey only. No indexed colour. Also C++23 |
| `crosser/esp-lcd-panel-uc8179`, `cleishm/idfxx_epaper_uc8179` | Apache-2.0 | **UC81*79*, not 8159.** Different chip: 800 × 480 black & white |
| `espressif/esp_lcd_ssd1681`, `nicolaielectronics/ssd1619`, `kk-kk-k/gdey0154d67` | mixed | Other controllers, all monochrome |
| `solderedelectronics/inkplate_lvgl` 1.1.1 | GPL-3.0-only | Same copyleft problem |
| `vroland/epdiy` 2.1.3 | LGPL-3.0-or-later | Parallel-interface panels. Wrong bus entirely |
| CalEPD / `cale-idf` | — | **Not a registry component at all**, GitHub-only C++ |

**`tuanpmt/esp_epaper` deserves the detail**, because it is the one that could have
replaced both `app_epd` & `app_gfx`:

- Its controllers are a six-function vtable plus one registry line, & `ADDING_PANELS.md`
  documents how to add one. A UC8159 controller would be ~150 lines. That part is
  genuinely attractive
- **Its "ACeP" controller is not this panel.** `acep_6color.c` is Spectra 6 / GDEP073E01 —
  six colours, no orange, & an init opening `CMDH 0xAA` from a different controller family
  altogether. There is no UC8159 path & no 600 × 448 panel registered
- **Its SPI layer raises CS between a command & its data** (`epaper_spi.c:95` then `:100`).
  §3c & §3d establish that no working driver for this panel does that
- **LVGL 9 is a hard dependency**, `public: true` in its `idf_component.yml` &
  `PRIV_REQUIRES lvgl` in its `CMakeLists.txt`. Not a switch
- **Its palette is numbered differently.** `EPD_COLOR_7COLOR` & `EPD_PIXEL_ORANGE` exist,
  but as yellow 2, red 3, orange 4, blue 5, green 6 — against the panel's native green 2,
  blue 3, red 4, yellow 5, orange 6 that §3d confirms from two vendors

**Decision: unchanged, & now on evidence rather than assumption.** Adopting `esp_epaper`
means an upstream PR, an LVGL dependency & a palette remap to get a driver that still has
to be written; writing it means one file with no dependencies.

> **Revisited & closed, end of phase 1.** Fonts came to one ~140-line Python tool &
> **5 141 bytes** of generated tables across three faces (2 181 + 1 906 + 1 054). Two bugs
> on the way, both found by rendering a string & looking at it: Pillow stores a mode `"1"`
> image one byte per pixel, so the obvious bit-unpacking smears glyphs sideways; & Pillow
> measures bearings from the top of the line box where the device draws from the baseline.
> Neither is the sort of difficulty that justifies LVGL 9, an upstream pull request & a
> palette remap. **`app_gfx` stands. This is not reopened again.**

**Licence note.** The Inkplate component is GPL-3.0-only & `epdiy` is LGPL-3.0. Linking
either into this firmware would settle the licence question for the whole project by
accident. Reading Soldered's source to cross-check register values (§3d) is not linking &
carries no such obligation; copying its code would.

---

## 2. Buy list & what is actually being connected

| Item | Note |
|---|---|
| Inky Impression 5.7" (PIM534) | 600 × 448, seven colours, ~30 s refresh. **Pimoroni have discontinued this in favour of the 7.3" (PIM773)** — confirm stock before committing. §8 covers what changes if it becomes the 7.3" |
| **Seeed XIAO ESP32-C3** | To hand & verified: ESP32-C3 QFN32 rev v0.4, embedded 4 MB flash (XMC), single core 160 MHz, no PSRAM |
| 40-pin male header or Pi-header breakout | The Inky's connector is a **female socket** meant to swallow a Pi's header. Jumper wires into a socket are miserable; a breakout is worth the two pounds |
| 5 V supply, 2 A, into the XIAO's USB-C | The XIAO's 5V pin is USB VBUS, so one supply feeds both boards. See the warnings in §3b |

---

## 3. The panel

### 3a. What it is

Read from the product page & from `pimoroni/inky`'s `inky/inky_uc8159.py`:

- 600 × 448, 0.1915 mm dot pitch, 114.9 × 85.8 mm active area
- E Ink Gallery Palette 4000, ACeP. Seven colours plus an eighth "clean" code
- Controller **UC8159**, 4-wire SPI, **3 MHz** in Pimoroni's driver
- ~30 s to refresh. There is no partial update: ACeP is full-frame only
- Four tactile buttons & an I²C HAT-ID EEPROM, neither needed by an ESP32

Colour indices, which `app_gfx` uses verbatim so a rendering can be compared against
Pimoroni's Python output:

```
0 BLACK   1 WHITE   2 GREEN   3 BLUE   4 RED   5 YELLOW   6 ORANGE   7 CLEAN
```

### 3b. Wiring the XIAO ESP32-C3

> **Warning — power before pins.** pinout.xyz records that the Inky Impression "Needs 5v
> and 3v3 power" and uses 12 GPIO. A Pimoroni forum thread warns that shorting the 5 V or
> 3v3 rails "will blow the PMIC". Get the two rails right, and do not hot-plug the ribbon.
>
> *Measured in phase 2, and it does not match the assumption this section was written on.*
> A refresh on the bench board draws **18 mA peak from 3v3 & 0.01 mA peak from 5 V**, so
> whatever the ±15 V rails are made from, on this board they are made from 3v3. The
> earlier claim here — that the panel's ±15 V come from a PMIC fed from 5 V — is
> unsupported by measurement & has been removed rather than repeated. What 5 V is for on
> this board is now an open question (§9); it is left connected, because a rail drawing
> nothing costs nothing & the forum warning about shorting it still applies.

> **Warning — do not put BUSY on D9 or D0.** ESP32-C3 strapping pins are **GPIO2, GPIO8 &
> GPIO9** (ESP-IDF v6.0.2, `docs/en/api-reference/peripherals/gpio/esp32c3.inc`). On the
> XIAO those are D0, D8 & D9. GPIO9 is the boot-mode pin: held low at reset, the chip
> enters download mode & **the firmware never runs**. BUSY is driven by the panel, is low
> while the panel is busy, and the panel is busy immediately after power-up — so BUSY on
> D9 gives a board that boots only sometimes, with nothing on the console to say why.

> **Resolved — the 3v3 draw is negligible.** Seeed rate the XIAO's 3v3 output at 500 mA,
> shared with an ESP32-C3 that peaks near 350 mA transmitting, and the Inky's draw was
> unpublished. *Measured in phase 2:* **18 mA peak during a refresh, 11.6 µA with the
> controller asleep.** That is 3.6 % of the rail against a budget the radio does not come
> close to exhausting, so the separate regulator this section used to hold in reserve is
> not needed & only ground is shared by choice, not by necessity.
>
> The sleep figure also settles the power model of §3a: a panel updated hourly spends 59
> minutes in 60 drawing microamps, so the cost of the design is the refresh, not the wait.

The Inky's signal assignments, by Raspberry Pi header position (from pinout.xyz &
confirmed against the constants in `inky_uc8159.py`):

| Signal | BCM | Pi header pin | XIAO pin | GPIO |
|---|---|---|---|---|
| 3v3 | — | 1 or 17 | 3V3 | — |
| 5 V | — | 2 or 4 | 5V | — |
| GND | — | 6, 9, 14, 20, 25, 30, 34, 39 | GND | — |
| SCLK | 11 | 23 | D8 | **8** ⚠ strapping |
| MOSI | 10 | 19 | D10 | **10** |
| Chip select | 8 | 24 | D1 | **3** |
| Data/Command | 22 | 15 | D2 | **4** |
| Reset | 27 | 13 | D3 | **5** |
| Busy | 17 | 11 | D4 | **6** (input) |
| Button A | 5 | 29 | D5 | **7** (input, pull-up) |
| Button B | 6 | 31 | D6 | **21** (input, pull-up) |
| Button C | 16 | 36 | D7 | **20** (input, pull-up) |
| Button D | 24 | 18 | — | not connected — see below |
| MISO | 9 | 21 | — | not connected |
| EEPROM SDA/SCL | 0, 1 | 27, 28 | — | not connected |

Where:

- **MISO is deliberately unconnected, & on this board that is load-bearing rather than
  tidy.** The UC8159 is written to & never read, so D9/GPIO9 — the boot-mode strapping pin
  — is left floating. Nothing the panel does can then stop the board booting
- **D8/GPIO8 carries SCLK & is a strapping pin.** It must be high at reset. SCLK is an
  output the ESP32 drives & the panel's SCLK is a high-impedance input, so the line should
  float rather than be pulled low. **If boot becomes intermittent, fit a 10 kΩ pull-up to
  3v3 on D8 before suspecting anything else**
- **D6 & D7 are free because the console is not on them.** *Observed:* the XIAO enumerates
  as `303a:1001 Espressif USB JTAG/serial debug unit`, so the ESP32-C3's USB Serial/JTAG
  on GPIO18/19 goes to the USB-C socket & GPIO20/21 are ordinary GPIO. This needs
  `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`, which the template's C3 defaults do not set —
  see §5's inherited-code table
- **Three buttons, not four.** The remaining pin is D0/GPIO2, also a strapping pin that
  must be high at reset. A button to ground is high when released, so it works — until
  somebody holds it while plugging the board in, which then does not boot. Leave button D
  unwired; §6 does not need it
- **The buttons short to ground**, so the ESP32 side needs internal pull-ups. Phase 5 work

Pins belong in `components/app_epd/Kconfig` with per-target defaults, following the rule
`esp-ota` applies to the status LED: a pin number is build configuration, never
application code. Nothing above appears in a `.c` file.

### 3c. The protocol, as Pimoroni's driver actually does it

Reset, then nine registers, all read from `inky_uc8159.py`:

| Register | Command | Bytes | Meaning |
|---|---|---|---|
| `TRES` | `0x61` | `02 58 01 C0` | Resolution, two 16-bit big-endian values: 600, 448 |
| `PSR` | `0x00` | `EF 08` | `(0b11 << 6) \| 0b101111`. `0b11` selects 600 × 448; `0x08` selects 7-colour |
| `PWR` | `0x01` | `37 00 23 23` | Internal DC-DC for source, gate & low-voltage source |
| `PLL` | `0x30` | `3C` | 50 Hz |
| `TSE` | `0x41` | `00` | Temperature sensor, colour mode |
| `CDI` | `0x50` | `(border << 5) \| 0x17` | VCOM & data interval; the border colour lives here |
| `TCON` | `0x60` | `22` | Gate/source non-overlap |
| `DAM` | `0x65` | `00` | External flash disabled |
| `PWS` | `0xE3` | `AA` | Undocumented, required |
| `PFS` | `0x03` | `00` | Power-off sequence, one frame |

Then, per refresh:

```
DTM1 (0x10) + 134 400 bytes    framebuffer, two pixels per byte, high nibble first
PON  (0x04)                    busy-wait 0.2 s
DRF  (0x12)                    busy-wait 32 s      <-- the ~30 s refresh
POF  (0x02)                    busy-wait 0.2 s
```

Packing, from `show()`: `(even_pixel << 4) | odd_pixel`, i.e. the left-hand pixel of each
pair occupies the high nibble.

Three traps that will produce silent misbehaviour if missed:

> **Measured 2026-08-29, & the 10 MHz question is settled.** The 134 400-byte transfer
> takes **375 ms at 3 MHz & 118 ms at 10 MHz** on the bench board, wired with a dozen
> jumper wires — the worst case a ribbon will ever be — & both refresh correctly. The
> estimates above (360 ms & 110 ms) were close enough to trust. 10 MHz is now the default.
> Keep it in proportion: a quarter of a second saved against 26 seconds of physics, so the
> upload is not why this panel is slow & tuning it further would be effort spent on 1 % of
> the cost.

- **Chip select must not be toggled per transaction — confirmed by two independent
  implementations.** Pimoroni set `spi.no_cs = True` & hold CS low across the whole
  command *and* its data. Soldered go further: `Inkplate6Color.cpp:61` drives CS low once
  at init & **never raises it again**, with `spics_io_num = -1` & only DC moving
  thereafter. In ESP-IDF that means `spics_io_num = -1` & a manual GPIO. Letting the
  hardware toggle CS between the two phases is the obvious implementation & is what **no**
  working driver for this panel does — see §1d, where it is also the reason an otherwise
  attractive component is unusable here
- **Busy is active-low & Pimoroni wait on a *rising* edge.** Their driver also warns that
  a busy pin found already high means no signal is arriving from the panel, & falls back
  to sleeping the full timeout. Reproduce that fallback; it is the difference between a
  miswired BUSY line reporting itself and a driver that hangs
- **`max_transfer_sz` must be raised.** ESP-IDF's SPI master defaults to 4092 bytes per
  transaction. Set it to 32 768 in the bus config & chunk the framebuffer; Pimoroni chunk
  at 4096 & Soldered at 4092, for the same reason

Two things Pimoroni's driver does not do, both taken from Soldered's & both worth having:

- **Deep sleep.** `DSLP` (`0x07`) with `0xA5` puts the controller down between refreshes.
  On a panel updated once an hour that is 59 minutes in every 60 spent asleep
- **10 MHz.** Soldered clock the same controller at `SPI_MASTER_FREQ_10M` against
  Pimoroni's 3 MHz, cutting the 134 400-byte transfer from ~360 ms to ~110 ms. **Start at
  3 MHz**, which is what the Inky's own ribbon & PCB are known good for, & treat 10 MHz as
  a phase-5 optimisation with the panel in front of you

### 3d. Cross-validation of §3c against a second vendor

Soldered Electronics' `Inkplate6Color` drives a 600 × 448 seven-colour ACeP panel with the
same controller, in C++, with no knowledge of Pimoroni's Python. Where the two agree, the
value is almost certainly right; where they differ, it is panel tuning rather than
protocol. **This is the strongest evidence in this document, & it cost one download.**

| Item | Pimoroni | Soldered | |
|---|---|---|---|
| Resolution `TRES 0x61` | `02 58 01 C0` | `02 58 01 C0` | **agree** |
| Panel setting `PSR 0x00` | `EF 08` | `EF 08` | **agree** |
| Power `PWR 0x01` byte 0 | `37` | `37` | **agree** |
| VCOM/interval `CDI 0x50` | `37` (border = white) | `37` | **agree** |
| `PWS 0xE3` | `AA` | `AA` | **agree** |
| Refresh order | `PON` → `DRF` → `POF` | `0x04` → `0x12` → `0x02` | **agree** |
| Framebuffer | `w × h / 2` = 134 400 B, 4bpp | `E_INK_WIDTH * E_INK_HEIGHT / 2` | **agree** |
| Colour indices | black 0 … orange 6 | black 0 … orange 6 | **agree** |
| Chip select | held across cmd + data | held low permanently | **agree: never hardware CS** |
| `PWR 0x01` bytes 2–3 | `23 23` | `05 05` | differ — VDH/VDL tuning |
| `TCON 0x60` | `22` | `20` | differ — gate/source overlap |
| Booster `BTST 0x06` | not sent | `C7 C7 1D` | Soldered only |
| `PLL 0x30` | `3C` | not sent | Pimoroni only |
| Clock | 3 MHz | 10 MHz | see above |

**Use Pimoroni's values as primary.** They were written for this exact board; Soldered's
were written for a different PCB carrying the same controller, & the four divergences are
all in the analogue tuning registers where the PCB is what differs.

> The colour indices agreeing matters more than it looks. `app_gfx` uses
> `0 = black … 6 = orange` as its native palette, so a rendering can be diffed against
> output from Pimoroni's Python on a host. This order is **not** universal — §1d records a
> component that numbers the same seven colours differently.

Soldered allocate their framebuffer with `MALLOC_CAP_SPIRAM`, which is exactly the option
§1b does not have on this board.

### 3e. Panel care

- E Ink advise against refreshing an ACeP panel more often than roughly every three
  minutes. Hourly is far inside that; a **Refresh now** button is not, so rate-limit any
  manual trigger to one refresh per three minutes & say so in the UI rather than silently
  dropping the request
- Run a **clean cycle weekly** — a full flush through the colour codes — to keep the panel
  healthy. Pimoroni's `CLEAN` colour code exists for this
- **The image survives power loss.** So do not refresh on boot merely because the device
  rebooted: store a hash of the last rendered frame in NVS & skip the refresh when the
  newly rendered frame hashes the same. A reboot loop that repaints the panel every time
  is 30 s of panel wear per cycle

---

## 4. The data

### 4a. The request

```
https://api.open-meteo.com/v1/forecast
  ?latitude=53.3574&longitude=-6.4486
  &hourly=temperature_2m,apparent_temperature,precipitation,precipitation_probability,shortwave_radiation,cloud_cover,is_day,weather_code
  &daily=sunrise,sunset
  &forecast_hours=13&forecast_days=2
  &timezone=auto
```

Where:

- `forecast_hours=13` — **verified 2026-08-28: the array starts at the current hour**, not
  at local midnight. Requesting 13 gives "now" plus the twelve hours the display promises,
  with no client-side hunting for an offset. This is the single detail that keeps the
  parser simple
- `daily=sunrise,sunset` with `forecast_days=2` — **verified 2026-09-02: the two do not
  fight.** `forecast_hours` still governs the hourly block, which came back as thirteen
  hours starting at the hour in progress, while `daily` came back as two days. Two rather
  than one because the thirteen hours cross midnight, so the day drawn is looked up by
  date rather than assumed to be `daily[0]`. Sunrise & sunset arrive as local wall-clock
  strings of the same shape as the hourly timestamps, & are stored as UTC the same way.
  A `null` is a day with no such event — that is what June looks like inside the Arctic
  circle — & reads as zero rather than as a broken response
- `timezone=auto` returns `utc_offset_seconds` & `timezone_abbreviation`, so hour labels
  can be drawn in local time without a TZ database on the device
- `shortwave_radiation` (W/m²) is the sunlight series — global horizontal irradiance,
  rather than a derived index, & it goes to zero at night on its own. `cloud_cover` &
  `is_day` are carried for shading & the icon
- `precipitation` is rain plus showers plus snow in mm; `precipitation_probability` is the
  chance of exceeding 0.1 mm
- `apparent_temperature` (°C) is the "feels like" number — Open-Meteo's own combination of
  wind chill, relative humidity & solar radiation. Asked for rather than derived on the
  device: computing it would mean fetching humidity & wind as well, & then reproducing a
  formula the server already applies. It is drawn under the current temperature (§6)
- No API key. Commercial use needs one & a `customer-` URL prefix; this is not that

**Measured, not estimated:** that exact URL returned **1469 bytes** for Dublin on
2026-09-03 — 1130 before `daily` was added, 1320 before `apparent_temperature` — &
`hourly.time[0]` was the hour then in progress. A 4 KB response buffer is generous. Reject
anything over 8 KB rather than growing a buffer to fit whatever arrives.

### 4b. The parse

`cJSON` is already a dependency of `app_ota`, so it costs nothing new. Parse into a fixed
struct — no allocation past cJSON's own, & a hard cap of 13 entries:

```c
typedef struct {
    time_t  t;              /* start of the hour, UTC          */
    float   temp_c;
    float   precip_mm;
    uint8_t precip_pct;
    int16_t solar_wm2;
    uint8_t cloud_pct;
    uint8_t weather_code;
    bool    is_day;
} app_wx_hour_t;

typedef struct {
    app_wx_hour_t hour[13];
    uint8_t       count;
    int32_t       utc_offset_s;
    time_t        fetched_at;   /* 0 == never                  */
    char          tz_abbr[8];
} app_wx_forecast_t;
```

Cache the whole struct in NVS on every successful fetch. On boot the display then shows
the last good forecast, visibly marked stale, instead of a blank panel for up to an hour.

### 4c. Failure behaviour

The display is passive furniture; it must never show a lie & must never show nothing.

| Condition | What the panel shows |
|---|---|
| Fetch succeeded | Normal render, footer carries the fetch time |
| Fetch failed, cache fresh (< 3 h) | Normal render, small "stale HH:MM" marker in the footer |
| Fetch failed, cache old (≥ 3 h) | Normal render, greyed, prominent stale banner |

Both stale rows depend on a detail that is easy to miss & was got wrong once: `hour[0]` is
the hour in progress, so a cached forecast cannot simply be redrawn an hour later — its
leftmost column would be labelled with an hour that has passed, which is a lie rather than
a stale truth. `app_wx_rebase()` drops the hours that have gone, so the panel keeps only
what it can still stand over & shortens as it ages. Without it these two rows are
unreachable & the panel falls to "Waiting for first forecast" instead.
| No cache at all | "Waiting for first forecast", plus the network state from `app_net_state_name()` |
| Not commissioned | The SoftAP SSID & passphrase from `app_cfg_ap_passphrase()`, full-screen |

That last row earns its keep: a device with no network has a 600 × 448 screen to tell you
so, which is far better than the serial log the template currently uses for it. *Observed
on the bench board:* the template already logs exactly this —
`provisioning AP up: SSID "wx-station-<mac6>" pass "…" http://192.168.4.1/` — so the
strings needed are already available at the point the panel would show them.

Retry with backoff on failure — 1, 2, 5, 15 minutes, then wait for the next hour. Do
**not** repaint the panel on every retry; repaint only when the rendered frame changes
(§3e).

---

## 5. Firmware architecture

Fork `esp-ota` per its `docs/start-a-new-project.md`:

```bash
cd /home/repo/esp-ota
git archive HEAD | tar -x -C ../esp32-wx-station
cd ../esp32-wx-station && git init
```

Where:

- `git archive` copies tracked files only, which leaves behind the certificates, keys &
  build output that must not be inherited
- **`git init` before building anything.** The version comes from `git describe`; a
  non-repository falls back to `0.0.0-nogit`, & since update policy compares versions for
  equality, every device would then report itself up to date for ever

> **Warning — the extraction overwrites same-named files in the destination.** `esp-ota`
> ships its own `PLAN.md`, `README.md` & `docs/`, so a plan written in the destination
> directory before the fork is silently replaced. It happened here. Fork into an empty
> directory, or move existing work aside first.

Then the three renames that doc names — `project()` in `CMakeLists.txt`,
`APP_PROJECT_NAME_SLUG` in `main/Kconfig.projbuild`, `APP_CFG_AP_PASS_SALT` in
`components/app_cfg/Kconfig` — & this project's own signing key & CA.

### `main/` holds no logic

```c
void app_main(void)
{
    ESP_ERROR_CHECK(app_core_start());
    ESP_ERROR_CHECK(app_display_start());
}
```

`app_core_start()` is the template's bring-up. `app_display_start()` is this project's.
Everything else is a component, including the scheduling policy — which is why
`app_display` exists rather than the hourly loop living in `main/`. Two reasons:

- **`app_core` stays mergeable.** It is template code, & the moment project glue is added
  to it every upstream fix becomes a conflict
- **Policy is the part most worth testing**, & a task buried in `main/` cannot be linked
  into a host test

### The components

| Component | Owns | Knows about | I/O |
|---|---|---|---|
| `app_gfx` | Pixel surfaces, clipping, primitives, fonts, 4bpp packing | nothing | **none** |
| `app_ui` | The §6 layout. Forecast → drawing calls | `app_gfx`, `app_wx` types | **none** |
| `app_wx` | Open-Meteo URL, fetch, parse, NVS cache, staleness | `app_trust`, `app_cfg` | HTTPS, NVS |
| `app_epd` | UC8159: SPI, GPIO, reset, init, band upload, refresh, sleep, clean | nothing above it | SPI, GPIO |
| `app_display` | The hourly tick, retry backoff, repaint decision, band loop | all four | orchestration |

The dependency graph is a line, not a web: `app_display` → {`app_wx`, `app_ui`, `app_epd`}
→ `app_gfx`. Nothing points back up. `app_gfx` & `app_ui` do no I/O at all, which is what
makes §5b possible.

### 5a. Banded rendering, & why it is not a compromise

§1b rules out a whole frame in RAM. The replacement is immediate-mode drawing replayed per
band:

```c
/* A window onto `rows` rows of the panel, starting at `y0`. Every primitive clips to
 * it, so the same drawing code renders one band or a whole panel unchanged. */
typedef struct {
    uint8_t *pixels;      /* rows * APP_GFX_WIDTH, one colour index per pixel */
    uint16_t y0;
    uint16_t rows;
} app_gfx_surface_t;
```

Callers draw in **absolute panel coordinates** throughout; the surface subtracts its own
`y0`. That is what keeps `app_ui`'s layout arithmetic free of any mention of bands.

`app_display` loops fourteen times: clear the surface, draw, pack to 4bpp, hand the bytes
to `app_epd`. The panel takes its framebuffer as one long data phase with chip select held
low (§3c), so fourteen SPI writes are indistinguishable from one.

Three things this buys, none of them consolation prizes:

- **28 800 bytes instead of 268 800** (§1b), so it runs on the XIAO with room for TLS
- **The same code path on every chip.** A whole-frame fast path only a PSRAM part
  exercised would rot unseen — exactly the divergence `esp-ota`'s own PLAN §1a exists to
  prevent
- **The host test renders a full-height surface** through the identical code, so §6's
  layout is iterated against a file in milliseconds

The cost is that drawing runs fourteen times per refresh instead of once. Against a
30-second panel refresh that is not a cost.

> **One consequence, already load-bearing in the code:** `app_gfx_line()` walks the whole
> Bresenham line even where it lies outside the surface, discarding pixels it cannot
> write. Clipping the endpoints instead would change which pixels the algorithm chooses,
> so the band above would disagree with the band below along the shared edge. This is not
> hypothetical — see §5b.

### 5b. Tests — the five that are worth writing

`esp-ota` already ships the harness: `test/host/` compiled by a plain `Makefile`,
`tools/run_tests.sh` to run it, no ESP-IDF & no hardware.

| Test | Claim | Status |
|---|---|---|
| `test_band_seam_matches_whole_frame` | `app_gfx`'s primitives are seam-safe on a synthetic scene | **Written & proven to fail correctly** |
| `test_layout_bands_match_whole_frame` | The layout that actually ships is seam-safe, in all four modes | **Written & proven to fail correctly.** Also at 30- & 100-row bands, & one row at a time |
| `test_packs_left_pixel_into_high_nibble` | `(even << 4) \| odd`, per §3c | Written |
| `test_text_baseline_sits_where_asked` | A glyph lands on the baseline it was given | Written |
| `test_measures_a_degree_sign_as_one_glyph` | UTF-8 is decoded, not walked byte by byte | Written |
| `test_temp_range_of_an_empty_forecast_is_refused` | An empty forecast reports no range & leaves the outputs untouched | Written |
| `test_rejects_a_surface_off_the_panel` | An invalid surface draws nothing rather than corrupting the heap | Written |
| `test_drawing_outside_the_band_touches_nothing` | Primitives entirely above or below a band leave it untouched | Written |
| `test_parses_forecast_fixture` & friends | A recorded 1452-byte response parses to known values; truncated, missing-field & wrong-length responses are rejected rather than half-parsed | Phase 3 |
| `test_rebase_shifts_onto_the_current_hour` | An aged forecast is shifted until `hour[0].t` is the hour in progress, & refused only once nothing true is left — which is what makes §4c's stale rows reachable at all | Phase 3 |
| `test_identical_forecast_does_not_repaint` | Same forecast in, no refresh out | Phase 4 |

**The seam test was confirmed to fail before being trusted**, per the house rule that a
test which has never failed proves nothing. Against a version of `app_gfx_line()` that
snapped its endpoints into the band, it reported:

```
  FAIL  test_app_gfx.c:55  banded render differs from whole-frame render
        first difference at x=590 y=0: whole=1 banded=5
```

That is the yellow diagonal of the test scene, one pixel in from the top-right corner —
the precise failure mode §5a describes. The test also prints the first differing
coordinate, because "not equal" over 268 800 bytes is not a bug report.

Where:

- **Fixtures are recorded, not fetched.** The 1452-byte response of §4a goes in
  `test/host/fixtures/` as a literal. A test that reaches the network is not deterministic.
  `tools/record_forecast.sh` asks for §4a's variables over an explicitly pinned window —
  20:00 local to 08:00 the next morning — rather than the relative `forecast_hours=13`,
  so that a fixture re-recorded one morning still crosses local midnight & still holds the
  night hours the parser tests lean on
- **Nothing mocks SPI to test `app_epd`.** Mocking the boundary would test the mock;
  `app_epd`'s claims are about a physical panel & belong on the bench, where phase 2 puts
  them
- **`app_ui`'s output is asserted against itself**, banded against whole-frame, rather
  than against a stored digest. A digest would have to be regenerated on every deliberate
  layout change & would then assert nothing anybody had looked at
- **`make -C test/host preview`** renders every mode to a PPM. Not a test, & it earns its
  place regardless: the night stipple burying the traces, a clipped units label & a
  missing superscript were all invisible to the tests & obvious on sight

### Changes to inherited code

| File | Change | Status |
|---|---|---|
| `components/app_led/Kconfig` | Ungate `APP_LED_GPIO` & `APP_LED_TASK_STACK` | **Done.** `APP_LED_TYPE_NONE` is `app_led`'s own default & did not compile; both symbols were gated on `!APP_LED_TYPE_NONE` while `app_led.c` referenced them unconditionally. Upstreamable as-is |
| `sdkconfig.defaults.esp32c3` | Console on USB Serial/JTAG; no status LED | **Done.** The template's C3 defaults describe a DevKitM-1. See §3b & the commit |
| `components/app_trust` | A second, public anchor | §1a. The one architectural change |
| `components/app_httpd` | A generic panel/action hook beside `app_httpd_set_ota_hooks()` | So **Refresh now** & the forecast reach the status page without `app_httpd` learning what weather is |
| `components/app_mqtt` | One command: force a refresh | Nearly free; `app_mqtt` already dispatches fleet commands |

Both the `app_httpd` & `app_trust` changes are small & generally useful. Write them as if
they will go back to `esp-ota`, because they should.

### Commits

`/home/repo/agent/examples/commit-message.example.md` governs: imperative subject naming
the change rather than the file, ≤72 characters, and a body only where the reasoning would
not survive without it. Component-scoped subjects match the history `esp-ota` already has:

```
app_gfx: draw onto a band, not a frame
app_led: build with no LED configured
```

---

## 6. The layout

600 × 448, all seven colours, arithmetic that closes exactly:

```
y   0 ..  79   Header    current temperature centred with "Feels like" beneath it;
                         sunrise/sunset stacked at the left edge; High/Low & the
                         stale marker at the right
y  80 .. 199   Temperature   line + fill, °C, min/max annotated
y 200 .. 319   Rainfall      bars, mm, probability as a lighter overlay
y 320 .. 419   Sunlight      bars, W/m², night hours shaded
y 420 .. 447   Footer        version, IP, "Last updated at HH:MM D/M/YYYY"

x   0 ..  47   Gutter, axis labels
x  48 .. 587   Plot, 12 columns of 45 px
x 588 .. 599   Right margin
```

Colour assignment, chosen so each series is distinguishable in the panel's real, muted
palette rather than in the saturated one:

| Element | Colour |
|---|---|
| Background, plot fill | white |
| Text, axes, hour labels | black |
| Temperature line | red |
| Rainfall bars | blue |
| Rain probability overlay | green |
| Sunlight bars | yellow, orange for the peak hour |
| Night shading | black at low density |

The place name is **not** on the panel. It was, in the middle of the header, & it was
spending the most legible row on the display to tell somebody standing in their own hall
where they are. The apparent temperature took the space: it is the number that changes
what you put on before going out, & the air temperature alone does not say it.

Fonts: convert a TTF to 1bpp C arrays at build time with a `tools/mkfont.py`, at three
sizes — ~48 px for the current temperature, ~16 px for labels, ~12 px for the footer. A
proportional 16 px ASCII set is on the order of 3 KB, nothing against 41 % of a free slot.
Icons for the WMO weather codes as 48 × 48 indexed bitmaps.

The `is_day` series shades the night hours across all three panels at once, which is what
makes a sunlight chart that is zero for eight hours read as night rather than as missing
data.

---

## 7. Phases

Each phase ends at something demonstrable. Do not start the next until the current one is
demonstrated.

0. **Fork & rename** ✅ *(verified on the bench board)* — `esp-ota` forked, three names
   changed, own CA & signing key generated, builds for `esp32c3`, flashes to the XIAO,
   boots, raises its provisioning AP & registers its OTA hooks. Two inherited-code faults
   found & fixed on the way; see §5's table. *Nothing new written. This proves the
   inherited half works before any of it is blamed for a fault in the new half.*
1. **`app_gfx` & `app_ui` on the host** ✅ — surfaces, clipping, primitives, three
   generated faces, §6's layout & all four of §4c's screens, 228 host checks, no ESP-IDF
   & no panel. Both seam tests were confirmed failing against deliberately broken code
   before being trusted. The §1d decision point is closed in favour of `app_gfx`.
   *Remaining, & deferred to where it is actually needed:* WMO weather icons, which
   phase 4 can add once it is clear which codes Dublin actually produces.
2. **`app_epd` & first pixels** ✅ *(verified on the bench board)* — nine wires per §3b,
   chip select low & left there, Pimoroni's register values unaltered. Seven colour bands
   on the glass in the order `app_gfx` names them, so the palette is confirmed end to end
   & not merely rendered. Refresh measured at 26 319 ms, repeatable to the millisecond.
   Draw measured & recorded in §3b: 18 mA peak from 3v3, 11.6 µA asleep — the regulator
   that section held in reserve is not needed, & the 5 V assumption it was written on is
   refuted (§9.5). Four faults in the driver's own reporting found & fixed on the way; a
   refresh nothing observed no longer returns `ESP_OK`. *The milestone that proves the
   idea.* **The bring-up failure that preceded it was never identified — see §8.**
3. **`app_wx`** ✅ *(verified on the bench board)* — second trust anchor per §1a, fetch,
   parse, NVS cache. The parser was written against the recorded fixture on the host
   first, so a failure on the board is a network failure & never a parsing one; 81 host
   checks, seven of them confirmed failing against deliberately broken code before being
   trusted. *Measured:* **13 hours in 1 504 ms** over TLS verified against the IDF root
   bundle, response 1 130 bytes, & the cache survives a reboot — a device that has fetched
   once restores its forecast in 424 ms of boot, before the panel draws. `app_ui_choose_mode()`
   makes §4c's table one host-tested function rather than a chain of ifs in the display
   task. *Remaining, & deferred to phase 4:* the hourly tick & §4c's retry backoff; phase 3
   fetches once at bring-up to demonstrate itself.
4. **The whole loop** — built & verified on the bench; **the week-long soak is the part
   still outstanding.** `app_display` now ticks on the wall clock, fetches, renders, and
   refreshes only when the frame actually changed. *Measured:* the dry render-and-hash
   pass costs **620 ms** against a refresh of 26 319 ms, so the cheap pass always runs &
   the expensive one earns its place. A reboot with unchanged data now performs **zero
   refreshes** — two draw attempts, both hashing identical. §4c's backoff was verified by
   pointing the client at an unreachable host: retries at 1, 2 & 5 minutes, the cached
   forecast untouched through all of them, & **four draw attempts producing no refresh at
   all**, which is the rule about not repainting on every retry. *Remaining:* run it for a
   week & watch what the panel does at 3 a.m. when the Wi-Fi drops — **deferred by
   decision on 2026-08-29, not completed.** Phase 5 was started without it. The soak is
   the only thing that exercises the failure this design cannot rehearse on a bench: a
   Wi-Fi drop at 3 a.m., a DHCP lease change, an SNTP step, & a panel left to itself for
   168 hours. Until it is run, "it works" means "it worked for the length of a bench
   session".
5. **Fit & finish** — built & verified on the bench. **Button A is now wired & confirmed
   working on hardware (2026-09-02); B & C are still untested for want of wires.** The status-page panel carries the two counters worth having
   (refreshes against skipped, which together say whether the frame hash is earning its
   keep); the MQTT `refresh`/`redraw` command & `POST /api/refresh` both ask rather than
   draw; the clean cycle runs weekly off the wall clock; & the location is settable from
   the setup page, validated as the untrusted input it is — a coordinate goes into an
   HTTPS query, so `53.3&hourly=evil` is refused rather than parsed as far as it goes.
   Manual refreshes are rate-limited to §3e's three minutes & the refusal is reported,
   not swallowed.

   Of §3c's two Soldered optimisations, **10 MHz is measured & adopted** (375 ms → 118 ms,
   §3c) & **controller deep sleep was already in place** — `app_epd_sleep()` has run after
   every refresh since phase 2, & §3b's 11.6 µA is the measurement of it. Nothing to do
   but say so.

   *Remaining:* the three buttons of §3b are written & compile, & nothing has ever pressed
   one. The pins configure & sixty seconds of unwired pins produce no spurious presses,
   which is all an unwired board can demonstrate.
6. **Deferred, only if wanted.** ESP32 deep sleep (§8), a 48-hour view on a button, an
   indoor sensor.

---

## 8. Risks & known gotchas

- **The chip-select behaviour in §3c is the likeliest cause of a blank panel.** The obvious
  ESP-IDF implementation is the wrong one, and a blank panel gives no clue which of a
  dozen things is at fault. No longer a guess: two independent vendor drivers both refuse
  hardware CS (§3d), and the one registry component that toggles it per transaction (§1d)
  drives different silicon.
- **A Kconfig symbol used outside the feature that defines it breaks the build nobody
  runs.** Hit three times in this tree: `app_epd`'s pins (used by the self-test, which is
  compiled in both branches), `APP_WX_PLACE` (drawn whether or not a forecast can be
  fetched) & the tick interval (the display's clock, not the weather client's). Each time
  the symptom was identical — the default build fine, the feature-off build failing to
  compile on a symbol that had silently vanished. **The rule that would have prevented all
  three:** a symbol belongs to the component that *reads* it, not to the feature it
  happens to describe, & anything read from outside an `if` must be declared outside it.
  `tools/build_matrix.sh` is what catches this, & it is worth running before a commit that
  adds a `config` inside a guard.
- **A TLS handshake needs about 12 KB of stack & does not say so.** *Measured:* the first
  live fetch died in mbedTLS with a stack protection fault reported against `app_display`,
  whose 4 KB stack was ample for everything it did before. The fault names the calling
  task, not the library, which sends you reading the wrong code. `app_display` now runs on
  8 192 bytes with a **measured 3 504 free at the worst point**, & `app_wx_fetch()` says so
  in its own header. Anything else that grows a TLS call — a second broker, a webhook —
  inherits the same trap.
- **The phase 2 bring-up fault was never identified, & it can return.** The panel ran
  through the whole sequence answering nothing on BUSY, then began working after the
  wiring was touched. It was diagnosed at the time as a missing 5 V rail; the measurement
  in §3b refutes that, because 5 V draws 0.01 mA during a refresh & a rail supplying
  nothing cannot have been withholding anything. Most likely it was a loose or wrong
  jumper on one of the six signals, none of which the self-test can see — every one but
  BUSY drives a high-impedance input, so a disconnected wire and a correct wire read
  identically. **The tell, if it comes back:** `refresh unconfirmed` in the log, with the
  reset probe still passing. That combination means the controller is alive & the command
  path is not, which is where to look first rather than at the rails.
- **`ISRG Root YR` is not in the ESP-IDF bundle.** The chain validates today only because
  Open-Meteo's server sends a cross-signed copy of it (§1a). If they stop, every device on
  the bundle anchor fails TLS on the same afternoon and the panel goes stale everywhere at
  once. The tell is a handshake failure against a host a browser is happy with.
- **Clock skew breaks TLS, & `esp-ota` records that SNTP timing varies wildly** — 7 s on
  one boot and about 90 s on the next, on the same network. The first fetch must wait on
  `app_net_wait_time_sync()`, not on a timer.
- **Measured, & the margin is comfortable: 157 860 bytes at the TLS low-water mark**
  (§1b). What is still unmeasured is the largest *contiguous* free block, which is what a
  single allocation actually needs; total free is split across a 156 KiB pool and 113 KiB
  of retention RAM. `heap_caps_get_largest_free_block()` belongs on the status page before
  `app_display` allocates anything.
- **Association is not reliable on the first attempt.** *Observed:* every boot fails once
  with `disconnected (reason 2)` and joins on the retry about five seconds later, and a
  `reason 34` disconnect and rejoin was seen mid-session at RSSI −45. The template's
  backoff absorbs it. Anything gated on the network being up at a fixed time after boot
  will be wrong roughly every time.
- **The 3v3 current draw is unknown & unmeasured**, & the XIAO's regulator is rated 500 mA
  shared with a radio that peaks near 350 mA. See §3b. Do not leave a board running
  unattended before phase 2 has measured it.
- **A strapping pin will eat a day if BUSY moves.** §3b. GPIO9 low at reset is download
  mode, and the panel holds BUSY low exactly when the board is coming up.
- **The 5.7" appears to be discontinued.** Pimoroni now sell the 7.3" (PIM773). If the
  5.7" cannot be sourced: the resolution becomes 800 × 480 and the 7.3" is **not** UC8159
  — treat it as a different driver, not a resolution constant. `APP_GFX_WIDTH` and
  `APP_GFX_HEIGHT` are the only two numbers §6 depends on.
- **`forecast_hours` starting at the current hour is verified but undocumented as such.**
  If it changed to midnight-aligned, the display would silently show the wrong twelve
  hours — which looks like a plausible forecast. Assert that `hour[0].t` is within an hour
  of `time(NULL)`. A weather display quietly twelve hours out is worse than one that says
  it has no data.
- **Do not repaint on boot.** §3e. A crash loop with a repaint in it is 30 s of panel wear
  per iteration and will show as banding long before anyone reads the log.
- **esptool's stub cannot read every sector on this board.** *Observed:* a full-flash read
  fails reproducibly at `0x7F000` with `A fatal error occurred: Packet content transfer
  stopped`, while both neighbouring sectors and the last sector of flash read fine.
  `--no-stub` reads it correctly, so the flash is sound. If a backup or verify stalls in
  that region, add `--no-stub` rather than suspecting the chip.
- **`app_httpd` runs out of URI handler slots on this build.** *Observed at boot:*
  `httpd_register_uri_handler: no slots left for registering handler` followed by
  `app_httpd: register probe /*: ESP_ERR_HTTPD_HANDLERS_FULL`. `app_httpd.c:48` sets
  `max_uri_handlers = 20` and the captive-portal catch-all is the twenty-first, so it is
  the handler that loses. The provisioning portal may therefore not redirect a phone
  automatically. **§5 adds two more handlers**, so this must be raised before, not after.
  Every other route survives — `/api/ota/check` was exercised over the LAN and worked.

---

## 9. Open questions

1. **Answered: option 1, two anchors** (§1a). Enforced verification against Open-Meteo
   via the IDF root bundle; the NAS keeps the private CA, which is reasonable for a
   firmware server reachable only from the LAN. Phase 3 implements it.
2. **Answered: the XIAO ESP32-C3.** §1b, §3b & §5a are built around it & phase 0 is
   verified on it. What remains is a measurement, not a question — free heap after
   association (§8).
3. **Answered: Dublin city centre** — latitude 53.3331, longitude −6.2489, confirmed
   against Open-Meteo's own geocoding endpoint rather than typed from memory (`Dublin`,
   Leinster, IE, population 1 024 027; the same query also returns a Dublin in Georgia & one
   in California, which is the mistake that endpoint exists to prevent). Compiled in for now;
   phase 5 makes it configurable from the provisioning portal. Open-Meteo takes decimal
   degrees rather than a place name, so there is no city lookup to inherit at run time.
4. **Answered: the board is on the LAN**, addressed by DHCP & reachable over mDNS at
   `wx-station-<mac6>.local`, SNTP synced, credentials in NVS via `tools/provision_nvs.sh`.
   §1b's heap figures are measured. Credentials live in `wifi.json`, which is gitignored.
5. **What is the 5 V rail for?** Phase 2 measured a refresh drawing 18 mA from 3v3 & 0.01
   mA from 5 V, so the panel does its work on 3v3 & pinout.xyz's "needs 5v and 3v3" is not
   describing the refresh. It may be the button LEDs, the EEPROM, a Pi-context requirement,
   or nothing this project uses. **Not urgent, but it matters for one reason:** the fault
   that stopped the panel talking during phase 2 bring-up was diagnosed as a missing 5 V
   rail, & that diagnosis is now unsupported — a rail drawing 0.01 mA cannot have been
   what stopped a refresh. The real cause of that failure is therefore unidentified, & an
   unidentified fault can return. §8 carries the consequence.
