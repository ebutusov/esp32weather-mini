#include "config.hpp"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_spiffs.h"

namespace ws {

static const char* TAG = "config";

static constexpr const char* kBasePath = "/spiffs";
static constexpr const char* kPartitionLabel = "storage";
static constexpr const char* kConfigPath = "/spiffs/config.json";
static constexpr long kMaxConfigSize = 2048;

// Reads the whole file into a heap buffer (NUL-terminated). Caller frees.
static esp_err_t readFile(char** out, size_t* out_len, ErrorInfo& err) {
    FILE* f = fopen(kConfigPath, "rb");
    if (!f) {
        if (errno == ENOENT) {
            err.set("config.json missing",
                    "No %s on the storage partition. Copy config.example.json "
                    "to spiffs/config.json and run idf.py flash.", kConfigPath);
            return ESP_ERR_NOT_FOUND;
        }
        err.set("config.json unreadable", "fopen failed, errno %d", errno);
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_OK;
    char* buf = nullptr;
    long size = -1;
    if (fseek(f, 0, SEEK_END) == 0) {
        size = ftell(f);
    }
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        err.set("config.json unreadable", "cannot determine file size");
        ret = ESP_FAIL;
    } else if (size == 0 || size > kMaxConfigSize) {
        err.set("config.json invalid", "file size %ld bytes (allowed 1..%ld)",
                size, kMaxConfigSize);
        ret = ESP_ERR_INVALID_SIZE;
    } else if (!(buf = static_cast<char*>(malloc(size + 1)))) {
        err.set("Out of memory", "reading config.json");
        ret = ESP_ERR_NO_MEM;
    } else if (fread(buf, 1, size, f) != static_cast<size_t>(size)) {
        err.set("config.json unreadable", "short read");
        ret = ESP_FAIL;
    }
    fclose(f);

    if (ret != ESP_OK) {
        free(buf);
        return ret;
    }
    buf[size] = '\0';
    *out = buf;
    *out_len = static_cast<size_t>(size);
    return ESP_OK;
}

// Copies a required string member into dst (size dst_size incl. NUL).
static esp_err_t getString(const cJSON* root, const char* key, char* dst,
                           size_t dst_size, bool allow_empty, ErrorInfo& err) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(item) || item->valuestring == nullptr) {
        err.set("config.json invalid", "\"%s\" is missing or not a string", key);
        return ESP_ERR_INVALID_ARG;
    }
    const size_t len = strlen(item->valuestring);
    if (len >= dst_size) {
        err.set("config.json invalid", "\"%s\" is too long (max %u chars)", key,
                static_cast<unsigned>(dst_size - 1));
        return ESP_ERR_INVALID_SIZE;
    }
    if (len == 0 && !allow_empty) {
        err.set("config.json invalid", "\"%s\" is empty", key);
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(dst, item->valuestring, len + 1);
    return ESP_OK;
}

static esp_err_t parseConfig(const char* json, size_t len, Config& out, ErrorInfo& err) {
    const char* parse_end = nullptr;
    cJSON* root = cJSON_ParseWithLengthOpts(json, len, &parse_end, false);
    if (!root) {
        // Only report the offset: the text around it may contain the password.
        const long offset = parse_end ? static_cast<long>(parse_end - json) : -1;
        err.set("config.json invalid", "JSON parse error at offset %ld", offset);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ESP_OK;
    if (!cJSON_IsObject(root)) {
        err.set("config.json invalid", "top level is not a JSON object");
        ret = ESP_ERR_INVALID_ARG;
    }
    if (ret == ESP_OK) {
        ret = getString(root, "wifi_ssid", out.wifi_ssid, sizeof(out.wifi_ssid), false, err);
    }
    if (ret == ESP_OK) {
        // Empty password = open network
        ret = getString(root, "wifi_password", out.wifi_password,
                        sizeof(out.wifi_password), true, err);
    }
    if (ret == ESP_OK) {
        ret = getString(root, "server_url", out.server_url, sizeof(out.server_url), false, err);
    }
    if (ret == ESP_OK) {
        const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, "sleep_seconds");
        if (!cJSON_IsNumber(item)) {
            err.set("config.json invalid", "\"sleep_seconds\" is missing or not a number");
            ret = ESP_ERR_INVALID_ARG;
        } else {
            const double v = item->valuedouble;
            // > 0 per PROTOCOL.md; must also be a whole number that fits uint32_t.
            if (!(v >= 1.0) || v > 4294967295.0 || std::floor(v) != v) {
                err.set("config.json invalid",
                        "\"sleep_seconds\" must be a whole number > 0");
                ret = ESP_ERR_INVALID_ARG;
            } else {
                out.sleep_seconds = static_cast<uint32_t>(v);
            }
        }
    }
    cJSON_Delete(root);
    return ret;
}

esp_err_t loadConfig(Config& out, ErrorInfo& err) {
    memset(&out, 0, sizeof(out));

    esp_vfs_spiffs_conf_t conf = {};
    conf.base_path = kBasePath;
    conf.partition_label = kPartitionLabel;
    conf.max_files = 2;
    // Never format: an unflashed partition should be reported, not wiped.
    conf.format_if_mount_failed = false;
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        err.set("SPIFFS mount failed",
                "%s. Flash the storage partition (idf.py flash).", esp_err_to_name(ret));
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    char* json = nullptr;
    size_t len = 0;
    ret = readFile(&json, &len, err);
    esp_vfs_spiffs_unregister(kPartitionLabel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "%s: %s", err.cause, err.detail);
        return ret;
    }

    ret = parseConfig(json, len, out, err);
    // The buffer holds the WiFi password: wipe it before freeing.
    memset(json, 0, len);
    free(json);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "%s: %s", err.cause, err.detail);
        return ret;
    }

    ESP_LOGI(TAG, "ssid \"%s\", url %s, sleep %u s", out.wifi_ssid, out.server_url,
             static_cast<unsigned>(out.sleep_seconds));
    return ESP_OK;
}

}  // namespace ws
