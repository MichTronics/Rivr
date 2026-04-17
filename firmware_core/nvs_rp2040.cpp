/**
 * @file  firmware_core/nvs_rp2040.cpp
 * @brief RP2040 LittleFS-backed implementation of the ESP-IDF NVS subset.
 */

#if defined(RIVR_PLATFORM_RP2040)

#include <Arduino.h>
#include <LittleFS.h>
#include <stdio.h>
#include <string.h>

extern "C" {
#include "nvs.h"
#include "nvs_flash.h"
}

#define TAG "NVS"
#define NVS_ROOT_DIR "/nvs"
#define NVS_MAX_HANDLES 4
#define NVS_NAME_MAX 31

typedef struct {
    bool in_use;
    bool readonly;
    char ns[NVS_NAME_MAX + 1];
} rp2040_nvs_handle_t;

static bool s_fs_ready = false;
static rp2040_nvs_handle_t s_handles[NVS_MAX_HANDLES];

static bool nvs_name_valid(const char *name)
{
    if (!name || name[0] == '\0') {
        return false;
    }
    for (const char *p = name; *p != '\0'; ++p) {
        const char c = *p;
        const bool ok = ((c >= 'a' && c <= 'z') ||
                         (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') ||
                         c == '_' || c == '-');
        if (!ok) {
            return false;
        }
    }
    return true;
}

static bool ensure_fs_ready(void)
{
    if (s_fs_ready) {
        return true;
    }

    s_fs_ready = LittleFS.begin();
    if (!s_fs_ready) {
        ESP_LOGE(TAG, "LittleFS mount failed");
        return false;
    }
    if (!LittleFS.exists(NVS_ROOT_DIR) && !LittleFS.mkdir(NVS_ROOT_DIR)) {
        ESP_LOGE(TAG, "LittleFS mkdir failed for %s", NVS_ROOT_DIR);
        s_fs_ready = false;
        return false;
    }
    return true;
}

static rp2040_nvs_handle_t *lookup_handle(nvs_handle_t h)
{
    const int idx = (int)h - 1;
    if (idx < 0 || idx >= NVS_MAX_HANDLES) {
        return NULL;
    }
    if (!s_handles[idx].in_use) {
        return NULL;
    }
    return &s_handles[idx];
}

static bool ensure_namespace_dir(const char *ns)
{
    char path[sizeof(NVS_ROOT_DIR) + 1 + NVS_NAME_MAX + 1];
    snprintf(path, sizeof(path), "%s/%s", NVS_ROOT_DIR, ns);
    if (LittleFS.exists(path)) {
        return true;
    }
    return LittleFS.mkdir(path);
}

static bool build_path(const rp2040_nvs_handle_t *handle, const char *key, char *out, size_t out_len)
{
    if (!handle || !key || !out || out_len == 0u || !nvs_name_valid(key)) {
        return false;
    }

    const int rc = snprintf(out, out_len, "%s/%s/%s", NVS_ROOT_DIR, handle->ns, key);
    return (rc > 0) && ((size_t)rc < out_len);
}

static esp_err_t read_file_bytes(const char *path, void *out, size_t *len)
{
    if (!path || !len || !ensure_fs_ready()) {
        return ESP_FAIL;
    }

    File f = LittleFS.open(path, "r");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }

    const size_t file_size = f.size();
    if (!out) {
        *len = file_size;
        f.close();
        return ESP_OK;
    }
    if (*len < file_size) {
        *len = file_size;
        f.close();
        return ESP_FAIL;
    }

    const size_t n = f.read((uint8_t *)out, file_size);
    f.close();
    if (n != file_size) {
        return ESP_FAIL;
    }

    *len = file_size;
    return ESP_OK;
}

static esp_err_t write_file_bytes(const char *path, const void *data, size_t len)
{
    if (!path || (!data && len != 0u) || !ensure_fs_ready()) {
        return ESP_FAIL;
    }

    File f = LittleFS.open(path, "w");
    if (!f) {
        return ESP_FAIL;
    }

    const size_t n = (len == 0u) ? 0u : f.write((const uint8_t *)data, len);
    f.flush();
    f.close();
    if (n != len) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

extern "C" esp_err_t nvs_flash_init(void)
{
    return ensure_fs_ready() ? ESP_OK : ESP_FAIL;
}

extern "C" esp_err_t nvs_flash_erase(void)
{
    if (s_fs_ready) {
        LittleFS.end();
        s_fs_ready = false;
    }
    memset(s_handles, 0, sizeof(s_handles));
    if (!LittleFS.format()) {
        ESP_LOGE(TAG, "LittleFS format failed");
        return ESP_FAIL;
    }
    return nvs_flash_init();
}

extern "C" esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    if (!out || !nvs_name_valid(ns) || !ensure_fs_ready()) {
        return ESP_FAIL;
    }

    if (mode == NVS_READWRITE) {
        if (!ensure_namespace_dir(ns)) {
            return ESP_FAIL;
        }
    } else {
        char path[sizeof(NVS_ROOT_DIR) + 1 + NVS_NAME_MAX + 1];
        snprintf(path, sizeof(path), "%s/%s", NVS_ROOT_DIR, ns);
        if (!LittleFS.exists(path)) {
            return ESP_ERR_NOT_FOUND;
        }
    }

    for (int i = 0; i < NVS_MAX_HANDLES; i++) {
        if (!s_handles[i].in_use) {
            s_handles[i].in_use = true;
            s_handles[i].readonly = (mode == NVS_READONLY);
            strncpy(s_handles[i].ns, ns, sizeof(s_handles[i].ns) - 1u);
            s_handles[i].ns[sizeof(s_handles[i].ns) - 1u] = '\0';
            *out = (nvs_handle_t)(i + 1);
            return ESP_OK;
        }
    }

    return ESP_FAIL;
}

extern "C" esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len)
{
    rp2040_nvs_handle_t *handle = lookup_handle(h);
    char path[96];
    if (!handle || !len || !build_path(handle, key, path, sizeof(path))) {
        return ESP_FAIL;
    }

    esp_err_t err = read_file_bytes(path, out, len);
    if (err != ESP_OK) {
        return err;
    }
    if (!out || *len == 0u) {
        return ESP_OK;
    }
    if (out[*len - 1u] != '\0') {
        return ESP_FAIL;
    }
    return ESP_OK;
}

extern "C" esp_err_t nvs_get_u16(nvs_handle_t h, const char *key, uint16_t *out)
{
    size_t len = sizeof(*out);
    rp2040_nvs_handle_t *handle = lookup_handle(h);
    char path[96];
    if (!handle || !out || !build_path(handle, key, path, sizeof(path))) {
        return ESP_FAIL;
    }
    esp_err_t err = read_file_bytes(path, out, &len);
    return (err == ESP_OK && len == sizeof(*out)) ? ESP_OK : ESP_FAIL;
}

extern "C" esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *value)
{
    rp2040_nvs_handle_t *handle = lookup_handle(h);
    char path[96];
    if (!handle || handle->readonly || !value || !build_path(handle, key, path, sizeof(path))) {
        return ESP_FAIL;
    }
    return write_file_bytes(path, value, strlen(value) + 1u);
}

extern "C" esp_err_t nvs_set_u16(nvs_handle_t h, const char *key, uint16_t value)
{
    rp2040_nvs_handle_t *handle = lookup_handle(h);
    char path[96];
    if (!handle || handle->readonly || !build_path(handle, key, path, sizeof(path))) {
        return ESP_FAIL;
    }
    return write_file_bytes(path, &value, sizeof(value));
}

extern "C" esp_err_t nvs_set_u32(nvs_handle_t h, const char *key, uint32_t value)
{
    rp2040_nvs_handle_t *handle = lookup_handle(h);
    char path[96];
    if (!handle || handle->readonly || !build_path(handle, key, path, sizeof(path))) {
        return ESP_FAIL;
    }
    return write_file_bytes(path, &value, sizeof(value));
}

extern "C" esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *out)
{
    size_t len = sizeof(*out);
    rp2040_nvs_handle_t *handle = lookup_handle(h);
    char path[96];
    if (!handle || !out || !build_path(handle, key, path, sizeof(path))) {
        return ESP_FAIL;
    }
    esp_err_t err = read_file_bytes(path, out, &len);
    return (err == ESP_OK && len == sizeof(*out)) ? ESP_OK : ESP_FAIL;
}

extern "C" esp_err_t nvs_set_i32(nvs_handle_t h, const char *key, int32_t value)
{
    rp2040_nvs_handle_t *handle = lookup_handle(h);
    char path[96];
    if (!handle || handle->readonly || !build_path(handle, key, path, sizeof(path))) {
        return ESP_FAIL;
    }
    return write_file_bytes(path, &value, sizeof(value));
}

extern "C" esp_err_t nvs_get_i32(nvs_handle_t h, const char *key, int32_t *out)
{
    size_t len = sizeof(*out);
    rp2040_nvs_handle_t *handle = lookup_handle(h);
    char path[96];
    if (!handle || !out || !build_path(handle, key, path, sizeof(path))) {
        return ESP_FAIL;
    }
    esp_err_t err = read_file_bytes(path, out, &len);
    return (err == ESP_OK && len == sizeof(*out)) ? ESP_OK : ESP_FAIL;
}

extern "C" esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    rp2040_nvs_handle_t *handle = lookup_handle(h);
    char path[96];
    if (!handle || handle->readonly || !build_path(handle, key, path, sizeof(path))) {
        return ESP_FAIL;
    }
    if (!LittleFS.exists(path)) {
        return ESP_ERR_NOT_FOUND;
    }
    return LittleFS.remove(path) ? ESP_OK : ESP_FAIL;
}

extern "C" esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{
    rp2040_nvs_handle_t *handle = lookup_handle(h);
    char path[96];
    if (!handle || !len || !build_path(handle, key, path, sizeof(path))) {
        return ESP_FAIL;
    }
    return read_file_bytes(path, out, len);
}

extern "C" esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *value, size_t len)
{
    rp2040_nvs_handle_t *handle = lookup_handle(h);
    char path[96];
    if (!handle || handle->readonly || !build_path(handle, key, path, sizeof(path))) {
        return ESP_FAIL;
    }
    return write_file_bytes(path, value, len);
}

extern "C" esp_err_t nvs_commit(nvs_handle_t h)
{
    return lookup_handle(h) ? ESP_OK : ESP_FAIL;
}

extern "C" void nvs_close(nvs_handle_t h)
{
    rp2040_nvs_handle_t *handle = lookup_handle(h);
    if (!handle) {
        return;
    }
    memset(handle, 0, sizeof(*handle));
}

#endif /* RIVR_PLATFORM_RP2040 */
