#include "app_diag.h"

#include <stdlib.h>
#include <string.h>

#include "app_cfg.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_core_dump.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"

static const char *TAG = "app_diag";

static uint32_t s_boot_count;
static char s_chip[24];
static const esp_app_desc_t *s_desc;
static const esp_partition_t *s_running;

static const char *chip_model_name(esp_chip_model_t model)
{
    /* A name for the log, not a branch in behaviour -- nothing in the template changes
     * with the answer (PLAN §1a). */
    switch (model) {
    case CHIP_ESP32:   return "ESP32";
    case CHIP_ESP32S2: return "ESP32-S2";
    case CHIP_ESP32S3: return "ESP32-S3";
    case CHIP_ESP32C3: return "ESP32-C3";
    case CHIP_ESP32C2: return "ESP32-C2";
    case CHIP_ESP32C6: return "ESP32-C6";
    case CHIP_ESP32H2: return "ESP32-H2";
    default:           return CONFIG_IDF_TARGET;
    }
}

esp_err_t app_diag_init(void)
{
    s_desc = esp_app_get_description();
    s_running = esp_ota_get_running_partition();

    esp_chip_info_t info;
    esp_chip_info(&info);
    snprintf(s_chip, sizeof(s_chip), "%s rev%d.%d", chip_model_name(info.model),
             info.revision / 100, info.revision % 100);

    /* Boot count is a cheap way to spot a device stuck in a reset loop from the status
     * page, without a serial cable attached. */
    if (app_cfg_get_u32(APP_CFG_KEY_BOOT_COUNT, &s_boot_count) != ESP_OK) {
        s_boot_count = 0;
    }
    s_boot_count++;
    app_cfg_set_u32(APP_CFG_KEY_BOOT_COUNT, s_boot_count);

    return ESP_OK;
}

const char *app_diag_reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_EXT:      return "external";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:  return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_DEEPSLEEP:return "deep sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO:     return "sdio";
    default:               return "unknown";
    }
}

const char *app_diag_running_partition(void)
{
    return s_running ? s_running->label : "?";
}

const char *app_diag_app_version(void)
{
    return s_desc ? s_desc->version : "?";
}

const char *app_diag_build_timestamp(void)
{
    static char buf[32];
    if (!s_desc) {
        return "?";
    }
    snprintf(buf, sizeof(buf), "%s %s", s_desc->date, s_desc->time);
    return buf;
}

const char *app_diag_idf_version(void) { return IDF_VER; }
const char *app_diag_chip_model(void)  { return s_chip; }
uint32_t app_diag_boot_count(void)     { return s_boot_count; }
uint64_t app_diag_uptime_s(void)       { return (uint64_t)(esp_timer_get_time() / 1000000LL); }
uint32_t app_diag_free_heap(void)      { return (uint32_t)esp_get_free_heap_size(); }
uint32_t app_diag_min_free_heap(void)  { return (uint32_t)esp_get_minimum_free_heap_size(); }

bool app_diag_pending_verify(void)
{
    esp_ota_img_states_t state;
    if (!s_running || esp_ota_get_state_partition(s_running, &state) != ESP_OK) {
        return false;
    }
    return state == ESP_OTA_IMG_PENDING_VERIFY;
}

/* ------------------------------------------------------------------ crash dumps */

void app_diag_coredump_info(app_diag_coredump_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    /* image_check() verifies the checksum. A dump that fails it is worse than no dump:
     * it would be reported as evidence and then not load in the debugger. */
    if (esp_core_dump_image_check() != ESP_OK) {
        return;
    }
    size_t addr = 0, size = 0;
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK || size == 0) {
        return;
    }
    out->present = true;
    out->size = (uint32_t)size;

    /* The summary needs the ELF format (see sdkconfig.defaults). Without it there is
     * still a dump to download, just nothing to say about it on the device -- so a
     * failure here is not a failure to have a dump. */
    esp_core_dump_summary_t *sum = calloc(1, sizeof(*sum));
    if (sum && esp_core_dump_get_summary(sum) == ESP_OK) {
        strlcpy(out->task, sum->exc_task, sizeof(out->task));
        out->pc = sum->exc_pc;
        strlcpy(out->elf_sha256, (const char *)sum->app_elf_sha256, sizeof(out->elf_sha256));
    }
    free(sum);

    if (esp_core_dump_get_panic_reason(out->reason, sizeof(out->reason)) != ESP_OK) {
        out->reason[0] = '\0';
    }
#endif
}

size_t app_diag_coredump_read(size_t offset, void *buf, size_t len)
{
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    if (!buf || len == 0) {
        return 0;
    }
    size_t addr = 0, size = 0;
    if (esp_core_dump_image_check() != ESP_OK ||
        esp_core_dump_image_get(&addr, &size) != ESP_OK || offset >= size) {
        return 0;
    }
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (!part) {
        return 0;
    }
    size_t n = (len < size - offset) ? len : size - offset;
    return esp_partition_read(part, offset, buf, n) == ESP_OK ? n : 0;
#else
    (void)offset; (void)buf; (void)len;
    return 0;
#endif
}

esp_err_t app_diag_coredump_erase(void)
{
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    esp_err_t err = esp_core_dump_image_erase();
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "crash dump erased");
    }
    return err;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void app_diag_log_banner(void)
{
    ESP_LOGI(TAG, "----------------------------------------------");
    ESP_LOGI(TAG, " %s  %s", app_cfg_device_name(), app_diag_app_version());
    ESP_LOGI(TAG, " built   %s  (IDF %s)", app_diag_build_timestamp(), app_diag_idf_version());
    ESP_LOGI(TAG, " chip    %s", app_diag_chip_model());
    ESP_LOGI(TAG, " slot    %s%s", app_diag_running_partition(),
             app_diag_pending_verify() ? "  [PENDING_VERIFY -- on probation]" : "");
    ESP_LOGI(TAG, " boot    #%u, reset reason: %s",
             (unsigned)app_diag_boot_count(), app_diag_reset_reason());
    ESP_LOGI(TAG, " heap    %u free", (unsigned)app_diag_free_heap());

    /* A stored dump is the single most useful thing the banner can tell you, and it is
     * easy to miss: the crash it describes may be from a build that is no longer even
     * running, so nothing else on this boot hints that it exists. */
    app_diag_coredump_t cd;
    app_diag_coredump_info(&cd);
    if (cd.present) {
        ESP_LOGW(TAG, " CRASH   %u-byte dump stored: task \"%s\" at pc 0x%08x",
                 (unsigned)cd.size, cd.task[0] ? cd.task : "?", (unsigned)cd.pc);
        if (cd.reason[0]) {
            ESP_LOGW(TAG, "         %s", cd.reason);
        }
    }
    ESP_LOGI(TAG, "----------------------------------------------");
}
