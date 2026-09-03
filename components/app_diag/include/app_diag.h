/* app_diag -- what a human needs to know about how this device is running.
 *
 * Boot reason, boot count, which slot is executing, and whether the running image is
 * still on probation. The last one matters most: PLAN §4 makes rollback the whole point
 * of the OTA design, and PENDING_VERIFY is the state in which the bootloader is about
 * to decide whether to keep this image. It must be visible, not inferred from a log.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_diag_init(void);

/* One multi-line block at boot: version, chip, slot, reset reason, heap. The first
 * thing you read when a board misbehaves. */
void app_diag_log_banner(void);

uint32_t    app_diag_boot_count(void);
uint64_t    app_diag_uptime_s(void);
const char *app_diag_reset_reason(void);
const char *app_diag_running_partition(void);
const char *app_diag_app_version(void);
const char *app_diag_build_timestamp(void);
const char *app_diag_idf_version(void);
const char *app_diag_chip_model(void);
uint32_t    app_diag_free_heap(void);
uint32_t    app_diag_min_free_heap(void);

/* True while the running image is in ESP_OTA_IMG_PENDING_VERIFY -- the bootloader will
 * revert to the previous slot on the next reset unless the self-test marks it valid. */
bool app_diag_pending_verify(void);

/* ------------------------------------------------------------------ crash dumps */

/* What a panic left behind. Its own flash partition, so it survives the reboot and the
 * next OTA -- the dump may describe a build that is no longer running.
 *
 * A dump is a copy of task stacks and can hold a Wi-Fi passphrase; nothing here uploads
 * it. Compiled out unless CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH, and safe to call anyway. */
typedef struct {
    bool     present;
    uint32_t size;            /* bytes of dump in the partition */
    char     task[24];        /* the task that crashed */
    uint32_t pc;              /* program counter at the fault */
    char     reason[80];      /* the panic reason, when the dump records one */
    char     elf_sha256[36];  /* identifies the *build* that crashed, which may not be
                               * the one now running -- a dump can outlive an update */
} app_diag_coredump_t;

/* Always writes `out`; sets present=false when there is nothing stored, the stored dump
 * fails its checksum, or the feature is compiled out. */
void app_diag_coredump_info(app_diag_coredump_t *out);

/* Read `len` bytes of the raw dump at `offset`, for handing to esp-coredump off-device.
 * Returns the number of bytes read, 0 at the end or when there is nothing stored. */
size_t app_diag_coredump_read(size_t offset, void *buf, size_t len);

/* Forget it. The partition holds exactly one dump, so the *first* crash after an erase
 * is the one kept -- erasing is what makes room for the next one to be recorded. */
esp_err_t app_diag_coredump_erase(void);

#ifdef __cplusplus
}
#endif
