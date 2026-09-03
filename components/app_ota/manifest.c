#include "manifest.h"

#include <string.h>

#include "cJSON.h"

static void copy_str(const cJSON *root, const char *key, char *dst, size_t dst_len)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item) && item->valuestring) {
        strlcpy(dst, item->valuestring, dst_len);
    } else {
        dst[0] = '\0';
    }
}

esp_err_t manifest_parse(const char *json, manifest_t *out)
{
    if (!json || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    copy_str(root, "project", out->project, sizeof(out->project));
    copy_str(root, "chip",    out->chip,    sizeof(out->chip));
    copy_str(root, "version", out->version, sizeof(out->version));
    copy_str(root, "url",     out->url,     sizeof(out->url));
    copy_str(root, "sha256",  out->sha256,  sizeof(out->sha256));

    const cJSON *size = cJSON_GetObjectItemCaseSensitive(root, "size");
    if (cJSON_IsNumber(size) && size->valuedouble > 0) {
        out->size = (uint32_t)size->valuedouble;
    }
    const cJSON *mandatory = cJSON_GetObjectItemCaseSensitive(root, "mandatory");
    out->mandatory = cJSON_IsTrue(mandatory);

    cJSON_Delete(root);
    return ESP_OK;
}
