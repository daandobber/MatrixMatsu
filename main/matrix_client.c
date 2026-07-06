#include "matrix_client.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "audio_player.h"
#include "cJSON.h"
#include "driver/sdmmc_host.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include "video_player.h"

static const char *TAG = "matrix";

#define HTTP_RESPONSE_MAX 1048576  // 1 MiB cap on any single response body
#define HTTP_TIMEOUT_MS   70000
#define SYNC_TIMEOUT_MS   30000
#define INITIAL_SYNC_TIMEOUT_MS 8000
#define MATRIX_CACHE_ROOMS 30
#define MATRIX_HISTORY_CACHE_MAGIC 0x4D584843u
#define MATRIX_HISTORY_CACHE_VERSION 2u
#define APP_REPOSITORY_SLUG "nl.daandobber.matrixmatsu"

// Keep sync responses small enough for the device: no presence/typing/read
// receipts/account-data, only the state needed for labels/encryption and only
// message-like timeline events.
#define MATRIX_SYNC_FILTER_JSON                                                                                      \
    "{\"room\":{\"state\":{\"lazy_load_members\":true,\"types\":[\"m.room.create\",\"m.room.name\","               \
    "\"m.room.canonical_alias\",\"m.room.encryption\"]},\"ephemeral\":{\"limit\":0},"                              \
    "\"account_data\":{\"types\":[\"m.tag\"]},"                                                                      \
    "\"timeline\":{\"limit\":8,\"types\":[\"m.room.message\",\"m.room.encrypted\",\"m.room.member\",\"m.reaction\"]}}," \
    "\"presence\":{\"limit\":0},"                                                                                   \
    "\"account_data\":{\"limit\":0}}"

#define MATRIX_TAGS_FILTER_JSON                                                                                      \
    "{\"room\":{\"state\":{\"limit\":0,\"types\":[]},\"ephemeral\":{\"limit\":0},"                                  \
    "\"account_data\":{\"types\":[\"m.tag\"]},\"timeline\":{\"limit\":0,\"types\":[]}},"                            \
    "\"presence\":{\"limit\":0},\"account_data\":{\"limit\":0}}"

typedef struct {
    char room_id[MATRIX_ROOM_ID_LEN];
    char body[MATRIX_BODY_LEN];
} matrix_send_request_t;

typedef struct {
    char room_id[MATRIX_ROOM_ID_LEN];
} matrix_history_request_t;

typedef struct {
    char room_id[MATRIX_ROOM_ID_LEN];
    char event_id[MATRIX_EVENT_ID_LEN];
} matrix_media_request_t;

typedef struct {
    char event_id[MATRIX_EVENT_ID_LEN];
    char text[MATRIX_AUDIO_STATUS_LEN];
} matrix_audio_status_t;

typedef struct {
    char     event_id[MATRIX_EVENT_ID_LEN];
    char     mimetype[MATRIX_MEDIA_MIMETYPE_LEN];
    char     label[MATRIX_BODY_LEN];
    uint8_t *data;
    size_t   len;
} matrix_image_cache_t;

typedef struct {
    char homeserver[MATRIX_HOMESERVER_LEN];
    char user_id[MATRIX_USER_ID_LEN];
    char access_token[MATRIX_TOKEN_LEN];
    char device_id[MATRIX_DEVICE_ID_LEN];
    char next_batch[128];
    char filter_id[64];
    char last_error[MATRIX_ERROR_LEN];
    uint32_t sync_count;
    uint32_t last_sync_join_rooms;
    uint32_t last_sync_invite_rooms;
    uint32_t last_sync_leave_rooms;
    uint32_t last_sync_events;
    uint32_t last_sync_messages;
    uint32_t total_messages;
    int      last_http_status;
    int64_t  last_sync_us;

    matrix_room_t rooms[MATRIX_MAX_ROOMS];
    int           room_count;
    char          active_history_room_id[MATRIX_ROOM_ID_LEN];
    matrix_message_t active_history[MATRIX_ACTIVE_HISTORY_MESSAGES];
    int           active_history_count;
    char          active_members_room_id[MATRIX_ROOM_ID_LEN];
    matrix_member_t active_members[MATRIX_ACTIVE_MEMBERS];
    int           active_member_count;
    matrix_audio_status_t audio_status[8];
    int           audio_status_next;
    matrix_image_cache_t image_cache;
    char          pending_invites[MATRIX_MAX_ROOMS][MATRIX_ROOM_ID_LEN];
    int           pending_invite_count;

    volatile matrix_state_t state;
    volatile bool           dirty;
    volatile bool           sync_should_run;

    SemaphoreHandle_t mutex;
    QueueHandle_t     send_queue;
} matrix_session_t;

static matrix_session_t *s_session = NULL;
static bool              s_persistence_enabled = false;
static int64_t           s_last_persist_us     = 0;
static bool              s_sd_mount_attempted  = false;
static bool              s_sd_mounted          = false;
static sdmmc_card_t     *s_sd_card             = NULL;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
} matrix_history_cache_header_t;

typedef struct {
    char    room_id[96];
    char    name[64];
    int64_t last_activity_ts;
    uint8_t has_name;
    uint8_t encrypted;
    uint8_t favorite;
} matrix_cached_room_t;

static void matrix_set_last_error(const char *message) {
    if (s_session == NULL) return;
    matrix_lock();
    snprintf(s_session->last_error, sizeof(s_session->last_error), "%s", message ? message : "Matrix error");
    s_session->dirty = true;
    matrix_unlock();
}

static void matrix_clear_last_error(void) {
    if (s_session == NULL) return;
    matrix_lock();
    if (s_session->last_error[0] != '\0') {
        s_session->last_error[0] = '\0';
        s_session->dirty         = true;
    }
    matrix_unlock();
}

static bool matrix_sd_ready(void) {
    struct stat st;
    if (stat("/sd", &st) == 0) return true;
    if (s_sd_mount_attempted) return s_sd_mounted;
    s_sd_mount_attempted = true;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files              = 6,
        .allocation_unit_size   = 16 * 1024,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot         = SDMMC_HOST_SLOT_0;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t res = esp_vfs_fat_sdmmc_mount("/sd", &host, &slot_config, &mount_config, &s_sd_card);
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "SD card not available for Matrix cache: %s", esp_err_to_name(res));
        s_sd_mounted = false;
        return false;
    }

    s_sd_mounted = true;
    return true;
}

static uint32_t matrix_room_cache_hash(const char *room_id) {
    uint32_t hash = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)room_id; p != NULL && *p != '\0'; p++) {
        hash ^= *p;
        hash *= 16777619u;
    }
    return hash;
}

static bool matrix_history_cache_path(const char *room_id, char *out, size_t out_len) {
    if (!matrix_sd_ready()) return false;
    mkdir("/sd/apps", 0775);
    mkdir("/sd/apps/" APP_REPOSITORY_SLUG, 0775);
    mkdir("/sd/apps/" APP_REPOSITORY_SLUG "/history", 0775);
    int len = snprintf(out, out_len, "/sd/apps/" APP_REPOSITORY_SLUG "/history/%08" PRIx32 ".bin", matrix_room_cache_hash(room_id));
    return len > 0 && len < (int)out_len;
}

static void matrix_load_history_cache(const char *room_id) {
    if (s_session == NULL || room_id == NULL || room_id[0] == '\0') return;

    char path[96];
    if (!matrix_history_cache_path(room_id, path, sizeof(path))) return;

    FILE *file = fopen(path, "rb");
    if (file == NULL) return;

    matrix_history_cache_header_t header = {0};
    if (fread(&header, 1, sizeof(header), file) != sizeof(header) ||
        header.magic != MATRIX_HISTORY_CACHE_MAGIC || header.version != MATRIX_HISTORY_CACHE_VERSION ||
        header.count > MATRIX_ACTIVE_HISTORY_MESSAGES) {
        fclose(file);
        return;
    }

    matrix_message_t *messages = heap_caps_calloc(header.count, sizeof(*messages), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (messages == NULL) {
        fclose(file);
        return;
    }
    size_t byte_count = sizeof(*messages) * header.count;
    size_t got        = fread(messages, 1, byte_count, file);
    fclose(file);
    if (got != byte_count) {
        free(messages);
        return;
    }

    matrix_lock();
    if (strcmp(s_session->active_history_room_id, room_id) == 0) {
        s_session->active_history_count = (int)header.count;
        memcpy(s_session->active_history, messages, byte_count);
        s_session->dirty = true;
    }
    matrix_unlock();
    free(messages);
}

static void matrix_save_history_cache(const char *room_id) {
    if (s_session == NULL || room_id == NULL || room_id[0] == '\0') return;

    char path[96];
    if (!matrix_history_cache_path(room_id, path, sizeof(path))) return;

    matrix_message_t *messages = heap_caps_calloc(
        MATRIX_ACTIVE_HISTORY_MESSAGES, sizeof(*messages), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (messages == NULL) return;

    uint32_t count = 0;
    matrix_lock();
    if (strcmp(s_session->active_history_room_id, room_id) == 0 && s_session->active_history_count > 0) {
        count = (uint32_t)s_session->active_history_count;
        if (count > MATRIX_ACTIVE_HISTORY_MESSAGES) count = MATRIX_ACTIVE_HISTORY_MESSAGES;
        memcpy(messages, s_session->active_history, sizeof(*messages) * count);
    }
    matrix_unlock();

    if (count == 0) {
        free(messages);
        return;
    }

    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        free(messages);
        return;
    }
    matrix_history_cache_header_t header = {
        .magic   = MATRIX_HISTORY_CACHE_MAGIC,
        .version = MATRIX_HISTORY_CACHE_VERSION,
        .count   = count,
    };
    fwrite(&header, 1, sizeof(header), file);
    fwrite(messages, sizeof(*messages), count, file);
    fclose(file);
    free(messages);
}

static void persist_session(bool force) {
    if (!s_persistence_enabled || s_session == NULL) return;

    int64_t now = esp_timer_get_time();
    if (!force && s_last_persist_us > 0 && now - s_last_persist_us < 60000000LL) {
        return;
    }

    char homeserver[MATRIX_HOMESERVER_LEN];
    char user_id[MATRIX_USER_ID_LEN];
    char access_token[MATRIX_TOKEN_LEN];
    char device_id[MATRIX_DEVICE_ID_LEN];
    char next_batch[128];
    char filter_id[64];
    matrix_cached_room_t *cached = heap_caps_calloc(MATRIX_CACHE_ROOMS, sizeof(*cached), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (cached == NULL) return;
    int cached_count = 0;

    matrix_lock();
    snprintf(homeserver, sizeof(homeserver), "%s", s_session->homeserver);
    snprintf(user_id, sizeof(user_id), "%s", s_session->user_id);
    snprintf(access_token, sizeof(access_token), "%s", s_session->access_token);
    snprintf(device_id, sizeof(device_id), "%s", s_session->device_id);
    snprintf(next_batch, sizeof(next_batch), "%s", s_session->next_batch);
    snprintf(filter_id, sizeof(filter_id), "%s", s_session->filter_id);
    for (int pass = 0; pass < 2 && cached_count < MATRIX_CACHE_ROOMS; pass++) {
        bool want_favorite = pass == 0;
        for (int i = 0; i < s_session->room_count && cached_count < MATRIX_CACHE_ROOMS; i++) {
            if (s_session->rooms[i].favorite != want_favorite) continue;
            memset(&cached[cached_count], 0, sizeof(cached[cached_count]));
            snprintf(cached[cached_count].room_id, sizeof(cached[cached_count].room_id), "%s", s_session->rooms[i].room_id);
            snprintf(cached[cached_count].name, sizeof(cached[cached_count].name), "%s", s_session->rooms[i].name);
            cached[cached_count].last_activity_ts = s_session->rooms[i].last_activity_ts;
            cached[cached_count].has_name         = s_session->rooms[i].has_name ? 1 : 0;
            cached[cached_count].encrypted        = s_session->rooms[i].encrypted ? 1 : 0;
            cached[cached_count].favorite         = s_session->rooms[i].favorite ? 1 : 0;
            cached_count++;
        }
    }
    matrix_unlock();

    if (homeserver[0] == '\0' || user_id[0] == '\0' || access_token[0] == '\0' || filter_id[0] == '\0') {
        free(cached);
        return;
    }

    nvs_handle_t nvs;
    if (nvs_open("mxsess", NVS_READWRITE, &nvs) != ESP_OK) {
        free(cached);
        return;
    }
    nvs_set_u8(nvs, "valid", 1);
    nvs_set_str(nvs, "hs", homeserver);
    nvs_set_str(nvs, "uid", user_id);
    nvs_set_str(nvs, "tok", access_token);
    nvs_set_str(nvs, "dev", device_id);
    nvs_set_str(nvs, "since", next_batch);
    nvs_set_str(nvs, "filter", filter_id);
    int stored_room_count = 0;
    if (cached_count > 0) {
        esp_err_t blob_res = nvs_set_blob(nvs, "rooms", cached, sizeof(cached[0]) * cached_count);
        if (blob_res == ESP_OK) {
            stored_room_count = cached_count;
        } else {
            nvs_erase_key(nvs, "rooms");
        }
    } else {
        nvs_erase_key(nvs, "rooms");
    }
    nvs_set_i32(nvs, "room_n", stored_room_count);
    nvs_commit(nvs);
    nvs_close(nvs);
    free(cached);
    s_last_persist_us = now;
}

/* ------------------------------------------------------------------------ */
/* HTTP plumbing                                                             */
/* ------------------------------------------------------------------------ */

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
    esp_err_t err;
} http_response_buffer_t;

typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
    size_t   max_bytes;
    esp_err_t err;
} http_media_buffer_t;

typedef struct {
    uint32_t events;
    uint32_t messages;
} matrix_sync_parse_counts_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    if (evt->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }
    http_response_buffer_t *resp = (http_response_buffer_t *)evt->user_data;
    if (resp == NULL || evt->data_len <= 0) {
        return ESP_OK;
    }

    size_t needed = resp->len + (size_t)evt->data_len + 1;
    if (needed > HTTP_RESPONSE_MAX) {
        ESP_LOGW(TAG, "HTTP response truncated (exceeds %d bytes)", HTTP_RESPONSE_MAX);
        resp->err = ESP_ERR_INVALID_SIZE;
        return ESP_FAIL;
    }
    if (needed > resp->cap) {
        size_t new_cap = resp->cap == 0 ? 4096 : resp->cap * 2;
        if (new_cap < needed) new_cap = needed;
        if (new_cap > HTTP_RESPONSE_MAX) new_cap = HTTP_RESPONSE_MAX;
        char *new_buf = heap_caps_realloc(resp->buf, new_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (new_buf == NULL) {
            ESP_LOGE(TAG, "Out of memory growing HTTP response buffer");
            resp->err = ESP_ERR_NO_MEM;
            return ESP_FAIL;
        }
        resp->buf = new_buf;
        resp->cap = new_cap;
    }
    memcpy(resp->buf + resp->len, evt->data, evt->data_len);
    resp->len += evt->data_len;
    resp->buf[resp->len] = '\0';
    return ESP_OK;
}

static void matrix_set_audio_status_locked(const char *event_id, const char *text) {
    if (s_session == NULL || event_id == NULL || event_id[0] == '\0') return;
    const char *status = (text != NULL && text[0] != '\0') ? text : "";

    for (int i = 0; i < (int)(sizeof(s_session->audio_status) / sizeof(s_session->audio_status[0])); i++) {
        if (strcmp(s_session->audio_status[i].event_id, event_id) == 0) {
            snprintf(s_session->audio_status[i].text, sizeof(s_session->audio_status[i].text), "%s", status);
            s_session->dirty = true;
            return;
        }
    }

    int slot = s_session->audio_status_next++ % (int)(sizeof(s_session->audio_status) / sizeof(s_session->audio_status[0]));
    snprintf(s_session->audio_status[slot].event_id, sizeof(s_session->audio_status[slot].event_id), "%s", event_id);
    snprintf(s_session->audio_status[slot].text, sizeof(s_session->audio_status[slot].text), "%s", status);
    s_session->dirty = true;
}

static void matrix_set_audio_status(const char *event_id, const char *text) {
    matrix_lock();
    matrix_set_audio_status_locked(event_id, text);
    matrix_unlock();
}

static void matrix_clear_image_cache_locked(void) {
    if (s_session == NULL) return;
    heap_caps_free(s_session->image_cache.data);
    memset(&s_session->image_cache, 0, sizeof(s_session->image_cache));
}

static void matrix_store_image_cache(
    const char *event_id, const char *mimetype, const char *label, uint8_t *data, size_t len
) {
    if (event_id == NULL || event_id[0] == '\0' || data == NULL || len == 0) return;
    matrix_lock();
    if (s_session != NULL) {
        matrix_clear_image_cache_locked();
        snprintf(s_session->image_cache.event_id, sizeof(s_session->image_cache.event_id), "%s", event_id);
        snprintf(s_session->image_cache.mimetype, sizeof(s_session->image_cache.mimetype), "%s",
                 mimetype != NULL && mimetype[0] != '\0' ? mimetype : "image/jpeg");
        snprintf(s_session->image_cache.label, sizeof(s_session->image_cache.label), "%s",
                 label != NULL && label[0] != '\0' ? label : "image");
        s_session->image_cache.data = data;
        s_session->image_cache.len  = len;
        s_session->dirty = true;
        data = NULL;
    }
    matrix_unlock();
    heap_caps_free(data);
}

static uint8_t *matrix_realloc_media_buffer(uint8_t *old_data, size_t old_len, size_t new_cap) {
    uint8_t *new_data = heap_caps_realloc(old_data, new_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (new_data != NULL) return new_data;

    new_data = heap_caps_malloc(new_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (new_data == NULL) new_data = heap_caps_malloc(new_cap, MALLOC_CAP_8BIT);
    if (new_data == NULL) return NULL;
    if (old_data != NULL && old_len > 0) memcpy(new_data, old_data, old_len);
    heap_caps_free(old_data);
    return new_data;
}

static esp_err_t http_media_event_handler(esp_http_client_event_t *evt) {
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    http_media_buffer_t *sink = (http_media_buffer_t *)evt->user_data;
    if (sink == NULL || evt->data_len <= 0) return ESP_OK;

    size_t data_len = (size_t)evt->data_len;
    if (sink->len + data_len > sink->max_bytes) {
        sink->err = ESP_ERR_INVALID_SIZE;
        return ESP_FAIL;
    }

    size_t needed = sink->len + data_len;
    if (needed > sink->cap) {
        size_t new_cap = sink->cap == 0 ? 16384 : sink->cap * 2;
        if (new_cap < needed) new_cap = needed;
        if (new_cap > sink->max_bytes) new_cap = sink->max_bytes;
        uint8_t *new_data = matrix_realloc_media_buffer(sink->data, sink->len, new_cap);
        if (new_data == NULL) {
            sink->err = ESP_ERR_NO_MEM;
            return ESP_FAIL;
        }
        sink->data = new_data;
        sink->cap  = new_cap;
    }

    memcpy(sink->data + sink->len, evt->data, data_len);
    sink->len += data_len;
    return ESP_OK;
}

// Performs a blocking HTTP request. On ESP_OK, *out_status carries the HTTP status
// code and *out_body (if non-NULL) is a heap-allocated NUL-terminated response body
// that the caller must free().
static esp_err_t matrix_http_request(
    const char *method, const char *url, const char *bearer_token, const char *json_body, char **out_body,
    int *out_status, char *err_detail, size_t err_detail_len
) {
    *out_body   = NULL;
    *out_status = 0;
    if (err_detail != NULL && err_detail_len > 0) {
        err_detail[0] = '\0';
    }

    http_response_buffer_t resp = {0};

    esp_http_client_config_t config = {
        .url               = url,
        .timeout_ms        = HTTP_TIMEOUT_MS,
        .event_handler     = http_event_handler,
        .user_data         = &resp,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    if (strcmp(method, "GET") == 0) {
        config.method = HTTP_METHOD_GET;
    } else if (strcmp(method, "POST") == 0) {
        config.method = HTTP_METHOD_POST;
    } else if (strcmp(method, "PUT") == 0) {
        config.method = HTTP_METHOD_PUT;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        if (err_detail != NULL && err_detail_len > 0) {
            snprintf(err_detail, err_detail_len, "http init failed");
        }
        return ESP_FAIL;
    }

    if (bearer_token != NULL && bearer_token[0] != '\0') {
        char auth[MATRIX_TOKEN_LEN + 32];
        snprintf(auth, sizeof(auth), "Bearer %s", bearer_token);
        esp_http_client_set_header(client, "Authorization", auth);
    }
    if (json_body != NULL) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, json_body, strlen(json_body));
    }

    esp_err_t res = esp_http_client_perform(client);
    int       sock_errno = 0;
    int       tls_code   = 0;
    int       tls_flags  = 0;
    if (res == ESP_OK) {
        *out_status = esp_http_client_get_status_code(client);
    } else {
        sock_errno = esp_http_client_get_errno(client);
        esp_http_client_get_and_clear_last_tls_error(client, &tls_code, &tls_flags);
        if (err_detail != NULL && err_detail_len > 0) {
            if (sock_errno != 0) {
                snprintf(
                    err_detail, err_detail_len, "errno=%d %s tls=%d flags=0x%x", sock_errno, strerror(sock_errno),
                    tls_code, tls_flags
                );
            } else {
                snprintf(err_detail, err_detail_len, "errno=0 tls=%d flags=0x%x", tls_code, tls_flags);
            }
        }
        ESP_LOGW(TAG, "HTTP request to %s failed: %s", url, esp_err_to_name(res));
    }
    esp_http_client_cleanup(client);

    if (res != ESP_OK) {
        if (resp.err != ESP_OK) {
            res = resp.err;
        }
        free(resp.buf);
        return res;
    }

    *out_body = resp.buf;
    return ESP_OK;
}

static esp_err_t matrix_http_download_buffer(
    const char *url, const char *bearer_token, size_t max_bytes, uint8_t **out_data, size_t *out_len, int *out_status
) {
    if (out_data != NULL) *out_data = NULL;
    if (out_len != NULL) *out_len = 0;
    if (out_status != NULL) *out_status = 0;
    if (url == NULL || out_data == NULL || out_len == NULL || max_bytes == 0) return ESP_ERR_INVALID_ARG;

    http_media_buffer_t sink = {
        .max_bytes = max_bytes,
        .err       = ESP_OK,
    };
    esp_http_client_config_t config = {
        .url               = url,
        .method            = HTTP_METHOD_GET,
        .timeout_ms        = HTTP_TIMEOUT_MS,
        .event_handler     = http_media_event_handler,
        .user_data         = &sink,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) return ESP_FAIL;

    if (bearer_token != NULL && bearer_token[0] != '\0') {
        char auth[MATRIX_TOKEN_LEN + 32];
        snprintf(auth, sizeof(auth), "Bearer %s", bearer_token);
        esp_http_client_set_header(client, "Authorization", auth);
    }

    esp_err_t res = esp_http_client_perform(client);
    int status = 0;
    if (res == ESP_OK) {
        status = esp_http_client_get_status_code(client);
        if (out_status != NULL) *out_status = status;
    }
    esp_http_client_cleanup(client);

    if (res == ESP_OK && sink.err != ESP_OK) res = sink.err;
    if (res != ESP_OK || status != 200 || sink.len == 0) {
        heap_caps_free(sink.data);
        return res == ESP_OK ? ESP_FAIL : res;
    }

    *out_data = sink.data;
    *out_len  = sink.len;
    return ESP_OK;
}

static bool matrix_url_encode(const char *in, char *out, size_t out_len) {
    static const char *hex = "0123456789ABCDEF";
    size_t             o   = 0;
    size_t             i   = 0;
    if (out_len == 0) return false;
    for (; in[i] != '\0' && o + 4 < out_len; i++) {
        unsigned char c = (unsigned char)in[i];
        bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                          c == '_' || c == '.' || c == '~';
        if (unreserved) {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[(c >> 4) & 0xF];
            out[o++] = hex[c & 0xF];
        }
    }
    out[o] = '\0';
    return in[i] == '\0';
}

#define MATRIX_IMAGE_THUMBNAIL_DIM 720

static bool matrix_mxc_encode_parts(
    const char *mxc, char *server_enc, size_t server_enc_len, char *media_enc, size_t media_enc_len
) {
    if (mxc == NULL || strncmp(mxc, "mxc://", 6) != 0) return false;
    const char *server = mxc + 6;
    const char *slash  = strchr(server, '/');
    if (slash == NULL || slash == server || slash[1] == '\0') return false;

    char server_raw[96];
    char media_raw[160];
    size_t server_len = (size_t)(slash - server);
    if (server_len >= sizeof(server_raw)) return false;
    memcpy(server_raw, server, server_len);
    server_raw[server_len] = '\0';
    snprintf(media_raw, sizeof(media_raw), "%s", slash + 1);

    return matrix_url_encode(server_raw, server_enc, server_enc_len) &&
           matrix_url_encode(media_raw, media_enc, media_enc_len);
}

static bool matrix_mxc_to_download_url(const char *homeserver, const char *mxc, char *out, size_t out_len) {
    if (homeserver == NULL || homeserver[0] == '\0') return false;
    char server_enc[96 * 3];
    char media_enc[160 * 3];
    if (!matrix_mxc_encode_parts(mxc, server_enc, sizeof(server_enc), media_enc, sizeof(media_enc))) return false;

    int len = snprintf(
        out, out_len, "%s/_matrix/client/v1/media/download/%s/%s", homeserver, server_enc, media_enc
    );
    return len > 0 && len < (int)out_len;
}

/* Server-generated thumbnails are re-encoded (baseline JPEG/PNG), which sidesteps
 * progressive JPEGs that neither the hardware nor tjpgd software decoder can read. */
static bool matrix_mxc_to_thumbnail_url(const char *homeserver, const char *mxc, char *out, size_t out_len) {
    if (homeserver == NULL || homeserver[0] == '\0') return false;
    char server_enc[96 * 3];
    char media_enc[160 * 3];
    if (!matrix_mxc_encode_parts(mxc, server_enc, sizeof(server_enc), media_enc, sizeof(media_enc))) return false;

    int len = snprintf(
        out, out_len, "%s/_matrix/client/v1/media/thumbnail/%s/%s?width=%u&height=%u&method=scale",
        homeserver, server_enc, media_enc, MATRIX_IMAGE_THUMBNAIL_DIM, MATRIX_IMAGE_THUMBNAIL_DIM
    );
    return len > 0 && len < (int)out_len;
}

/* ------------------------------------------------------------------------ */
/* Room / message bookkeeping (caller must hold the session mutex)          */
/* ------------------------------------------------------------------------ */

static matrix_room_t *find_or_create_room(const char *room_id) {
    for (int i = 0; i < s_session->room_count; i++) {
        if (strcmp(s_session->rooms[i].room_id, room_id) == 0) {
            return &s_session->rooms[i];
        }
    }
    if (s_session->room_count >= MATRIX_MAX_ROOMS) {
        return NULL;
    }
    matrix_room_t *room = &s_session->rooms[s_session->room_count++];
    memset(room, 0, sizeof(*room));
    snprintf(room->room_id, sizeof(room->room_id), "%s", room_id);
    return room;
}

static void remove_room_by_id(const char *room_id) {
    if (room_id == NULL || room_id[0] == '\0') return;
    for (int i = 0; i < s_session->room_count; i++) {
        if (strcmp(s_session->rooms[i].room_id, room_id) == 0) {
            if (i < s_session->room_count - 1) {
                memmove(
                    &s_session->rooms[i], &s_session->rooms[i + 1],
                    (s_session->room_count - i - 1) * sizeof(s_session->rooms[0])
                );
            }
            s_session->room_count--;
            memset(&s_session->rooms[s_session->room_count], 0, sizeof(s_session->rooms[0]));
            return;
        }
    }
}

static void sort_rooms_by_activity(void) {
    for (int i = 1; i < s_session->room_count; i++) {
        matrix_room_t room = s_session->rooms[i];
        int           j    = i - 1;
        while (j >= 0 && s_session->rooms[j].last_activity_ts < room.last_activity_ts) {
            s_session->rooms[j + 1] = s_session->rooms[j];
            j--;
        }
        s_session->rooms[j + 1] = room;
    }
}

static bool set_room_name_by_id(const char *room_id, const char *name) {
    if (room_id == NULL || name == NULL || name[0] == '\0') return false;
    for (int i = 0; i < s_session->room_count; i++) {
        if (strcmp(s_session->rooms[i].room_id, room_id) == 0) {
            snprintf(s_session->rooms[i].name, sizeof(s_session->rooms[i].name), "%s", name);
            s_session->rooms[i].has_name = true;
            return true;
        }
    }
    return false;
}

static void matrix_localpart(const char *mxid, char *out, size_t out_len) {
    if (out == NULL || out_len == 0) return;
    out[0] = '\0';
    if (mxid == NULL || mxid[0] == '\0') {
        snprintf(out, out_len, "?");
        return;
    }
    const char *start = mxid[0] == '@' ? mxid + 1 : mxid;
    const char *colon = strchr(start, ':');
    size_t      len   = colon ? (size_t)(colon - start) : strlen(start);
    if (len >= out_len) len = out_len - 1;
    memcpy(out, start, len);
    out[len] = '\0';
}

static const char *member_display_name(matrix_room_t *room, const char *user_id) {
    if (room == NULL || user_id == NULL) return NULL;
    if (s_session != NULL && strcmp(s_session->active_members_room_id, room->room_id) == 0) {
        for (int i = 0; i < s_session->active_member_count; i++) {
            if (strcmp(s_session->active_members[i].user_id, user_id) == 0 &&
                s_session->active_members[i].display_name[0] != '\0') {
                return s_session->active_members[i].display_name;
            }
        }
    }
    for (int i = 0; i < room->member_count; i++) {
        if (strcmp(room->members[i].user_id, user_id) == 0 && room->members[i].display_name[0] != '\0') {
            return room->members[i].display_name;
        }
    }
    return NULL;
}

static void update_room_fallback_name(matrix_room_t *room) {
    if (room == NULL || room->has_name || s_session == NULL) return;
    if (strcmp(s_session->active_members_room_id, room->room_id) == 0) {
        for (int i = 0; i < s_session->active_member_count; i++) {
            if (strcmp(s_session->active_members[i].user_id, s_session->user_id) != 0) {
                snprintf(room->name, sizeof(room->name), "%s", s_session->active_members[i].display_name);
                room->has_name = room->name[0] != '\0';
                return;
            }
        }
    }
    for (int i = 0; i < room->member_count; i++) {
        if (strcmp(room->members[i].user_id, s_session->user_id) != 0) {
            snprintf(room->name, sizeof(room->name), "%s", room->members[i].display_name);
            room->has_name = room->name[0] != '\0';
            return;
        }
    }
}

static void upsert_active_member(matrix_room_t *room, const char *user_id, const char *display_name) {
    if (s_session == NULL || room == NULL || user_id == NULL || user_id[0] == '\0') return;
    if (strcmp(s_session->active_members_room_id, room->room_id) != 0) return;

    char fallback[MATRIX_ROOM_NAME_LEN];
    matrix_localpart(user_id, fallback, sizeof(fallback));
    const char *name = (display_name != NULL && display_name[0] != '\0') ? display_name : fallback;

    for (int i = 0; i < s_session->active_member_count; i++) {
        if (strcmp(s_session->active_members[i].user_id, user_id) == 0) {
            snprintf(s_session->active_members[i].display_name, sizeof(s_session->active_members[i].display_name), "%s", name);
            goto update_messages;
        }
    }
    if (s_session->active_member_count < MATRIX_ACTIVE_MEMBERS) {
        matrix_member_t *member = &s_session->active_members[s_session->active_member_count++];
        snprintf(member->user_id, sizeof(member->user_id), "%s", user_id);
        snprintf(member->display_name, sizeof(member->display_name), "%s", name);
    }

update_messages:
    for (int i = 0; i < s_session->active_history_count; i++) {
        if (strcmp(s_session->active_history[i].sender, user_id) == 0) {
            snprintf(s_session->active_history[i].sender_display, sizeof(s_session->active_history[i].sender_display), "%s", name);
        }
    }
    update_room_fallback_name(room);
}

static void upsert_room_member(matrix_room_t *room, const char *user_id, const char *display_name) {
    if (room == NULL || user_id == NULL || user_id[0] == '\0') return;
    char fallback[MATRIX_ROOM_NAME_LEN];
    matrix_localpart(user_id, fallback, sizeof(fallback));
    const char *name = (display_name != NULL && display_name[0] != '\0') ? display_name : fallback;

    for (int i = 0; i < room->member_count; i++) {
        if (strcmp(room->members[i].user_id, user_id) == 0) {
            snprintf(room->members[i].display_name, sizeof(room->members[i].display_name), "%s", name);
            for (int j = 0; j < room->message_count; j++) {
                if (strcmp(room->messages[j].sender, user_id) == 0) {
                    snprintf(room->messages[j].sender_display, sizeof(room->messages[j].sender_display), "%s", name);
                }
            }
            update_room_fallback_name(room);
            return;
        }
    }
    if (room->member_count >= MATRIX_MAX_MEMBERS) return;
    matrix_member_t *member = &room->members[room->member_count++];
    snprintf(member->user_id, sizeof(member->user_id), "%s", user_id);
    snprintf(member->display_name, sizeof(member->display_name), "%s", name);
    for (int j = 0; j < room->message_count; j++) {
        if (strcmp(room->messages[j].sender, user_id) == 0) {
            snprintf(room->messages[j].sender_display, sizeof(room->messages[j].sender_display), "%s", name);
        }
    }
    update_room_fallback_name(room);
}

static void process_member_event(matrix_room_t *room, cJSON *ev) {
    cJSON *state_key = cJSON_GetObjectItemCaseSensitive(ev, "state_key");
    cJSON *content   = cJSON_GetObjectItemCaseSensitive(ev, "content");
    cJSON *display   = content ? cJSON_GetObjectItemCaseSensitive(content, "displayname") : NULL;
    if (cJSON_IsString(state_key)) {
        upsert_room_member(room, state_key->valuestring, cJSON_IsString(display) ? display->valuestring : NULL);
    }
}

static bool room_has_event(matrix_room_t *room, const char *event_id) {
    if (room == NULL || event_id == NULL || event_id[0] == '\0') return false;
    for (int i = 0; i < room->message_count; i++) {
        if (strcmp(room->messages[i].event_id, event_id) == 0) {
            return true;
        }
    }
    return false;
}

static bool update_optimistic_message(
    matrix_room_t *room, const char *event_id, const char *sender, const char *body, int64_t ts
) {
    if (room == NULL || event_id == NULL || event_id[0] == '\0' || sender == NULL || body == NULL) return false;
    for (int i = room->message_count - 1; i >= 0; i--) {
        matrix_message_t *msg = &room->messages[i];
        if (msg->event_id[0] == '\0' && strcmp(msg->sender, sender) == 0 && strcmp(msg->body, body) == 0) {
            snprintf(msg->event_id, sizeof(msg->event_id), "%s", event_id);
            msg->origin_server_ts = ts;
            if (ts > room->last_activity_ts) {
                room->last_activity_ts = ts;
            }
            return true;
        }
    }
    return false;
}

static bool add_reaction_to_message(matrix_message_t *msg, const char *key) {
    if (msg == NULL || key == NULL || key[0] == '\0') return false;

    int empty = -1;
    for (int i = 0; i < MATRIX_MAX_REACTIONS; i++) {
        if (msg->reactions[i].key[0] == '\0') {
            if (empty < 0) empty = i;
            continue;
        }
        if (strcmp(msg->reactions[i].key, key) == 0) {
            if (msg->reactions[i].count < 255) msg->reactions[i].count++;
            return true;
        }
    }

    if (empty < 0) return false;
    snprintf(msg->reactions[empty].key, sizeof(msg->reactions[empty].key), "%s", key);
    msg->reactions[empty].count = 1;
    return true;
}

static bool append_reaction(matrix_room_t *room, const char *event_id, const char *key) {
    if (room == NULL || event_id == NULL || event_id[0] == '\0' || key == NULL || key[0] == '\0') return false;
    bool changed = false;

    for (int i = 0; i < room->message_count; i++) {
        if (strcmp(room->messages[i].event_id, event_id) == 0) {
            changed |= add_reaction_to_message(&room->messages[i], key);
            break;
        }
    }
    if (s_session != NULL && strcmp(s_session->active_history_room_id, room->room_id) == 0) {
        for (int i = 0; i < s_session->active_history_count; i++) {
            if (strcmp(s_session->active_history[i].event_id, event_id) == 0) {
                changed |= add_reaction_to_message(&s_session->active_history[i], key);
                break;
            }
        }
    }
    return changed;
}

static bool parse_reaction_event(cJSON *ev, char *event_id_out, size_t event_id_len, char *key_out, size_t key_len) {
    if (event_id_out == NULL || key_out == NULL || event_id_len == 0 || key_len == 0) return false;
    event_id_out[0] = '\0';
    key_out[0] = '\0';

    cJSON *content = cJSON_GetObjectItemCaseSensitive(ev, "content");
    cJSON *relates = content ? cJSON_GetObjectItemCaseSensitive(content, "m.relates_to") : NULL;
    cJSON *rel_type = relates ? cJSON_GetObjectItemCaseSensitive(relates, "rel_type") : NULL;
    cJSON *event_id = relates ? cJSON_GetObjectItemCaseSensitive(relates, "event_id") : NULL;
    cJSON *key = relates ? cJSON_GetObjectItemCaseSensitive(relates, "key") : NULL;
    if (!cJSON_IsString(rel_type) || strcmp(rel_type->valuestring, "m.annotation") != 0) return false;
    if (!cJSON_IsString(event_id) || !cJSON_IsString(key)) return false;

    snprintf(event_id_out, event_id_len, "%s", event_id->valuestring);
    snprintf(key_out, key_len, "%s", key->valuestring);
    return event_id_out[0] != '\0' && key_out[0] != '\0';
}

static bool active_history_has_event(const char *event_id) {
    if (event_id == NULL || event_id[0] == '\0') return false;
    for (int i = 0; i < s_session->active_history_count; i++) {
        if (strcmp(s_session->active_history[i].event_id, event_id) == 0) {
            return true;
        }
    }
    return false;
}

static bool append_html_entity(char *out, size_t out_len, size_t *o, const char *name, size_t name_len) {
    const char *replacement = NULL;
    char        numeric[8]  = {0};

    if (name_len == 3 && strncmp(name, "amp", 3) == 0) {
        replacement = "&";
    } else if (name_len == 2 && strncmp(name, "lt", 2) == 0) {
        replacement = "<";
    } else if (name_len == 2 && strncmp(name, "gt", 2) == 0) {
        replacement = ">";
    } else if (name_len == 4 && strncmp(name, "quot", 4) == 0) {
        replacement = "\"";
    } else if (name_len == 5 && strncmp(name, "apos", 5) == 0) {
        replacement = "'";
    } else if (name_len == 3 && strncmp(name, "#39", 3) == 0) {
        replacement = "'";
    } else if (name_len > 1 && name[0] == '#') {
        int code = 0;
        if (name[1] == 'x' || name[1] == 'X') {
            for (size_t i = 2; i < name_len; i++) {
                char c = name[i];
                int  v = -1;
                if (c >= '0' && c <= '9') v = c - '0';
                if (c >= 'a' && c <= 'f') v = 10 + c - 'a';
                if (c >= 'A' && c <= 'F') v = 10 + c - 'A';
                if (v < 0) return false;
                code = (code << 4) | v;
            }
        } else {
            for (size_t i = 1; i < name_len; i++) {
                if (name[i] < '0' || name[i] > '9') return false;
                code = code * 10 + (name[i] - '0');
            }
        }
        if (code > 0 && code < 0x80) {
            numeric[0]  = (char)code;
            replacement = numeric;
        }
    }

    if (replacement == NULL) return false;
    size_t len = strlen(replacement);
    if (*o + len >= out_len) return false;
    memcpy(out + *o, replacement, len);
    *o += len;
    out[*o] = '\0';
    return true;
}

static void append_html_text_range(char *out, size_t out_len, size_t *o, const char *start, const char *end) {
    for (const char *p = start; p < end && *p != '\0' && *o + 1 < out_len; p++) {
        if (*p == '&') {
            const char *semi = p + 1;
            while (semi < end && *semi != '\0' && *semi != ';' && semi - p < 12) {
                semi++;
            }
            if (semi < end && *semi == ';' && append_html_entity(out, out_len, o, p + 1, (size_t)(semi - p - 1))) {
                p = semi;
                continue;
            }
        }
        out[(*o)++] = *p;
        out[*o]     = '\0';
    }
}

static const char *find_case_insensitive(const char *haystack, const char *needle) {
    size_t needle_len = strlen(needle);
    if (needle_len == 0) return haystack;
    for (const char *p = haystack; *p != '\0'; p++) {
        if (strncasecmp(p, needle, needle_len) == 0) return p;
    }
    return NULL;
}

static const char *skip_plain_reply_fallback(const char *body) {
    if (body == NULL || body[0] != '>') return body;

    const char *blank = strstr(body, "\n\n");
    if (blank == NULL) return body;

    const char *p = body;
    while (p < blank) {
        if (*p != '>') return body;
        const char *next = strchr(p, '\n');
        if (next == NULL || next > blank) break;
        p = next + 1;
    }
    return blank + 2;
}

static bool extract_html_attr(const char *tag_start, const char *tag_end, const char *attr, char *out, size_t out_len) {
    size_t attr_len = strlen(attr);
    for (const char *p = tag_start; p < tag_end && *p != '\0'; p++) {
        if ((p == tag_start || p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n') &&
            p + attr_len < tag_end && strncasecmp(p, attr, attr_len) == 0 && p[attr_len] == '=') {
            const char *value = p + attr_len + 1;
            if (value >= tag_end) return false;
            char quote = *value;
            if (quote != '\'' && quote != '"') return false;
            value++;
            const char *value_end = value;
            while (value_end < tag_end && *value_end != quote) {
                value_end++;
            }
            size_t o = 0;
            out[0]   = '\0';
            append_html_text_range(out, out_len, &o, value, value_end);
            return o > 0;
        }
    }
    return false;
}

static void html_to_plain_text(const char *html, char *out, size_t out_len) {
    size_t o = 0;
    if (out_len == 0) return;
    out[0] = '\0';

    for (const char *p = html; p != NULL && *p != '\0' && o + 1 < out_len;) {
        if (*p != '<') {
            const char *next = strchr(p, '<');
            if (next == NULL) next = p + strlen(p);
            append_html_text_range(out, out_len, &o, p, next);
            p = next;
            continue;
        }

        const char *tag_end = strchr(p, '>');
        if (tag_end == NULL) break;
        if (strncasecmp(p + 1, "mx-reply", 8) == 0) {
            const char *reply_end = find_case_insensitive(tag_end + 1, "</mx-reply>");
            p = reply_end != NULL ? reply_end + strlen("</mx-reply>") : tag_end + 1;
            continue;
        }
        if (strncasecmp(p + 1, "br", 2) == 0 && o + 1 < out_len) {
            out[o++] = '\n';
            out[o]   = '\0';
        } else if (strncasecmp(p + 1, "img", 3) == 0) {
            char alt[MATRIX_BODY_LEN];
            if (extract_html_attr(p + 1, tag_end, "alt", alt, sizeof(alt))) {
                size_t len = strlen(alt);
                if (o + len >= out_len) len = out_len - o - 1;
                memcpy(out + o, alt, len);
                o += len;
                out[o] = '\0';
            }
        }
        p = tag_end + 1;
    }
}

static const char *message_display_body(cJSON *content, char *scratch, size_t scratch_len) {
    cJSON *body      = content ? cJSON_GetObjectItemCaseSensitive(content, "body") : NULL;
    cJSON *formatted = content ? cJSON_GetObjectItemCaseSensitive(content, "formatted_body") : NULL;

    if (cJSON_IsString(formatted) && formatted->valuestring != NULL && strchr(formatted->valuestring, '<') != NULL) {
        html_to_plain_text(formatted->valuestring, scratch, scratch_len);
        if (scratch[0] != '\0') return scratch;
    }

    return cJSON_IsString(body) ? skip_plain_reply_fallback(body->valuestring) : NULL;
}

static void append_active_history_message(
    matrix_room_t *room, const char *event_id, const char *sender, const char *body, int64_t ts, bool notice
) {
    if (s_session == NULL || room == NULL) return;
    if (strcmp(s_session->active_history_room_id, room->room_id) != 0) return;
    if (active_history_has_event(event_id)) return;

    if (s_session->active_history_count >= MATRIX_ACTIVE_HISTORY_MESSAGES) {
        memmove(
            &s_session->active_history[0], &s_session->active_history[1],
            sizeof(matrix_message_t) * (MATRIX_ACTIVE_HISTORY_MESSAGES - 1)
        );
        s_session->active_history_count = MATRIX_ACTIVE_HISTORY_MESSAGES - 1;
    }

    char fallback_sender[MATRIX_ROOM_NAME_LEN];
    matrix_localpart(sender, fallback_sender, sizeof(fallback_sender));
    const char *display_sender = member_display_name(room, sender);

    matrix_message_t *msg = &s_session->active_history[s_session->active_history_count++];
    snprintf(msg->sender, sizeof(msg->sender), "%s", sender ? sender : "?");
    snprintf(msg->sender_display, sizeof(msg->sender_display), "%s", display_sender ? display_sender : fallback_sender);
    snprintf(msg->event_id, sizeof(msg->event_id), "%s", event_id ? event_id : "");
    snprintf(msg->body, sizeof(msg->body), "%s", body ? body : "");
    msg->origin_server_ts = ts;
    msg->is_notice        = notice;
}

static bool append_message(
    matrix_room_t *room, const char *event_id, const char *sender, const char *body, int64_t ts, bool notice
) {
    if (room_has_event(room, event_id)) {
        return false;
    }
    if (update_optimistic_message(room, event_id, sender, body, ts)) {
        return true;
    }
    if (room->message_count >= MATRIX_MAX_MESSAGES) {
        memmove(&room->messages[0], &room->messages[1], sizeof(matrix_message_t) * (MATRIX_MAX_MESSAGES - 1));
        room->message_count = MATRIX_MAX_MESSAGES - 1;
    }
    char fallback_sender[MATRIX_ROOM_NAME_LEN];
    matrix_localpart(sender, fallback_sender, sizeof(fallback_sender));
    const char *display_sender = member_display_name(room, sender);

    matrix_message_t *msg = &room->messages[room->message_count++];
    snprintf(msg->sender, sizeof(msg->sender), "%s", sender ? sender : "?");
    snprintf(msg->sender_display, sizeof(msg->sender_display), "%s", display_sender ? display_sender : fallback_sender);
    snprintf(msg->event_id, sizeof(msg->event_id), "%s", event_id ? event_id : "");
    snprintf(msg->body, sizeof(msg->body), "%s", body ? body : "");
    msg->origin_server_ts = ts;
    msg->is_notice        = notice;
    if (ts > room->last_activity_ts) {
        room->last_activity_ts = ts;
    }
    if (s_session != NULL) {
        s_session->total_messages++;
        append_active_history_message(room, event_id, sender, body, ts, notice);
    }
    return true;
}

/* ------------------------------------------------------------------------ */
/* Homeserver discovery + login                                             */
/* ------------------------------------------------------------------------ */

static void strip_trailing_slash(char *s) {
    size_t len = strlen(s);
    while (len > 0 && s[len - 1] == '/') {
        s[--len] = '\0';
    }
}

static void discover_homeserver_base_url(const char *server_input, char *out_url, size_t out_len) {
    bool has_scheme = strncmp(server_input, "http://", 7) == 0 || strncmp(server_input, "https://", 8) == 0;

    char fallback[MATRIX_HOMESERVER_LEN];
    if (has_scheme) {
        snprintf(fallback, sizeof(fallback), "%s", server_input);
    } else {
        snprintf(fallback, sizeof(fallback), "https://%s", server_input);
    }
    strip_trailing_slash(fallback);

    if (has_scheme) {
        snprintf(out_url, out_len, "%s", fallback);
        return;
    }

    char wk_url[MATRIX_HOMESERVER_LEN + 40];
    snprintf(wk_url, sizeof(wk_url), "https://%s/.well-known/matrix/client", server_input);

    char     *body   = NULL;
    int       status = 0;
    esp_err_t res    = matrix_http_request("GET", wk_url, NULL, NULL, &body, &status, NULL, 0);

    if (res == ESP_OK && status == 200 && body != NULL) {
        cJSON *root = cJSON_Parse(body);
        if (root != NULL) {
            cJSON *hs   = cJSON_GetObjectItemCaseSensitive(root, "m.homeserver");
            cJSON *base = hs ? cJSON_GetObjectItemCaseSensitive(hs, "base_url") : NULL;
            if (cJSON_IsString(base) && base->valuestring != NULL && base->valuestring[0] != '\0') {
                snprintf(out_url, out_len, "%s", base->valuestring);
                strip_trailing_slash(out_url);
                cJSON_Delete(root);
                free(body);
                return;
            }
            cJSON_Delete(root);
        }
    }
    free(body);

    snprintf(out_url, out_len, "%s", fallback);
}

static esp_err_t register_sync_filter(
    const char *homeserver, const char *user_id, const char *token, const char *filter_json, char *out_filter_id,
    size_t out_filter_id_len, char *err_out, size_t err_out_len
) {
    if (out_filter_id == NULL || out_filter_id_len == 0) return ESP_ERR_INVALID_ARG;
    out_filter_id[0] = '\0';
    if (err_out != NULL && err_out_len > 0) err_out[0] = '\0';

    char encoded_user[MATRIX_USER_ID_LEN * 3];
    if (!matrix_url_encode(user_id, encoded_user, sizeof(encoded_user))) {
        if (err_out) snprintf(err_out, err_out_len, "user_id too long");
        return ESP_ERR_INVALID_SIZE;
    }

    char url[MATRIX_HOMESERVER_LEN + sizeof(encoded_user) + 64];
    int  url_len = snprintf(url, sizeof(url), "%s/_matrix/client/v3/user/%s/filter", homeserver, encoded_user);
    if (url_len < 0 || url_len >= (int)sizeof(url)) {
        if (err_out) snprintf(err_out, err_out_len, "filter URL too long");
        return ESP_ERR_INVALID_SIZE;
    }

    char     *resp_body = NULL;
    int       status    = 0;
    char      net_detail[96];
    esp_err_t res = matrix_http_request(
        "POST", url, token, filter_json, &resp_body, &status, net_detail, sizeof(net_detail)
    );
    if (res != ESP_OK) {
        if (err_out) snprintf(err_out, err_out_len, "filter network: %s %s", esp_err_to_name(res), net_detail);
        free(resp_body);
        return res;
    }
    if (status != 200) {
        char reason[96] = "filter registration failed";
        if (resp_body != NULL) {
            cJSON *eroot = cJSON_Parse(resp_body);
            if (eroot != NULL) {
                cJSON *err = cJSON_GetObjectItemCaseSensitive(eroot, "error");
                if (cJSON_IsString(err) && err->valuestring != NULL) {
                    snprintf(reason, sizeof(reason), "%s", err->valuestring);
                }
                cJSON_Delete(eroot);
            }
        }
        if (err_out) snprintf(err_out, err_out_len, "filter HTTP %d: %s", status, reason);
        free(resp_body);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp_body);
    free(resp_body);
    if (root == NULL) {
        if (err_out) snprintf(err_out, err_out_len, "filter response was not JSON");
        return ESP_FAIL;
    }
    cJSON *filter_id = cJSON_GetObjectItemCaseSensitive(root, "filter_id");
    if (!cJSON_IsString(filter_id) || filter_id->valuestring == NULL || filter_id->valuestring[0] == '\0') {
        if (err_out) snprintf(err_out, err_out_len, "filter response missing filter_id");
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    snprintf(out_filter_id, out_filter_id_len, "%s", filter_id->valuestring);
    cJSON_Delete(root);
    return ESP_OK;
}

static void preload_joined_rooms(const char *homeserver, const char *token) {
    char url[MATRIX_HOMESERVER_LEN + 64];
    int  url_len = snprintf(url, sizeof(url), "%s/_matrix/client/v3/joined_rooms", homeserver);
    if (url_len < 0 || url_len >= (int)sizeof(url)) return;

    char *resp_body  = NULL;
    int   status     = 0;
    char  net_detail[96];
    esp_err_t res = matrix_http_request("GET", url, token, NULL, &resp_body, &status, net_detail, sizeof(net_detail));
    if (res != ESP_OK || status < 200 || status >= 300 || resp_body == NULL) {
        ESP_LOGW(TAG, "joined_rooms preload failed: %s status=%d", esp_err_to_name(res), status);
        free(resp_body);
        return;
    }

    cJSON *root = cJSON_Parse(resp_body);
    free(resp_body);
    if (root == NULL) return;

    cJSON *joined_rooms = cJSON_GetObjectItemCaseSensitive(root, "joined_rooms");
    if (cJSON_IsArray(joined_rooms)) {
        matrix_lock();
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, joined_rooms) {
            if (!cJSON_IsString(item) || item->valuestring == NULL) continue;
            if (s_session->room_count >= MATRIX_MAX_ROOMS) break;
            find_or_create_room(item->valuestring);
        }
        s_session->dirty = true;
        matrix_unlock();
    }

    cJSON_Delete(root);
}

esp_err_t matrix_login(
    const char *homeserver_input, const char *username, const char *password, char *err_out, size_t err_out_len
) {
    if (s_session == NULL) return ESP_ERR_INVALID_STATE;
    if (err_out != NULL && err_out_len > 0) err_out[0] = '\0';

    s_session->state = MATRIX_STATE_LOGGING_IN;

    char base_url[MATRIX_HOMESERVER_LEN];
    discover_homeserver_base_url(homeserver_input, base_url, sizeof(base_url));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "m.login.password");
    cJSON *identifier = cJSON_AddObjectToObject(root, "identifier");
    cJSON_AddStringToObject(identifier, "type", "m.id.user");
    cJSON_AddStringToObject(identifier, "user", username);
    cJSON_AddStringToObject(root, "password", password);
    cJSON_AddStringToObject(root, "initial_device_display_name", "Tanmatsu Chat");
    char *body_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    char url[MATRIX_HOMESERVER_LEN + 64];
    snprintf(url, sizeof(url), "%s/_matrix/client/v3/login", base_url);

    char     *resp_body = NULL;
    int       status    = 0;
    char      net_detail[96];
    esp_err_t res       = matrix_http_request("POST", url, NULL, body_str, &resp_body, &status, net_detail, sizeof(net_detail));
    free(body_str);

    if (res != ESP_OK) {
        if (err_out) snprintf(err_out, err_out_len, "Network error: %s %s", esp_err_to_name(res), net_detail);
        s_session->state = MATRIX_STATE_ERROR;
        free(resp_body);
        return ESP_FAIL;
    }

    if (status != 200) {
        char reason[128] = "login failed";
        if (resp_body != NULL) {
            cJSON *eroot = cJSON_Parse(resp_body);
            if (eroot != NULL) {
                cJSON *err = cJSON_GetObjectItemCaseSensitive(eroot, "error");
                if (cJSON_IsString(err) && err->valuestring) {
                    snprintf(reason, sizeof(reason), "%s", err->valuestring);
                }
                cJSON_Delete(eroot);
            }
        }
        if (err_out) snprintf(err_out, err_out_len, "HTTP %d: %s", status, reason);
        s_session->state = MATRIX_STATE_ERROR;
        free(resp_body);
        return ESP_FAIL;
    }

    cJSON *root2 = cJSON_Parse(resp_body);
    free(resp_body);
    if (root2 == NULL) {
        if (err_out) snprintf(err_out, err_out_len, "Invalid response from server");
        s_session->state = MATRIX_STATE_ERROR;
        return ESP_FAIL;
    }

    cJSON *tok = cJSON_GetObjectItemCaseSensitive(root2, "access_token");
    cJSON *uid = cJSON_GetObjectItemCaseSensitive(root2, "user_id");
    cJSON *dev = cJSON_GetObjectItemCaseSensitive(root2, "device_id");

    if (!cJSON_IsString(tok) || !cJSON_IsString(uid)) {
        if (err_out) snprintf(err_out, err_out_len, "Malformed login response");
        cJSON_Delete(root2);
        s_session->state = MATRIX_STATE_ERROR;
        return ESP_FAIL;
    }

    char filter_id[64];
    char filter_err[128];
    res = register_sync_filter(
        base_url, uid->valuestring, tok->valuestring, MATRIX_SYNC_FILTER_JSON, filter_id, sizeof(filter_id), filter_err,
        sizeof(filter_err)
    );
    if (res != ESP_OK) {
        if (err_out) snprintf(err_out, err_out_len, "%s", filter_err);
        cJSON_Delete(root2);
        s_session->state = MATRIX_STATE_ERROR;
        return ESP_FAIL;
    }

    matrix_lock();
    snprintf(s_session->homeserver, sizeof(s_session->homeserver), "%s", base_url);
    snprintf(s_session->access_token, sizeof(s_session->access_token), "%s", tok->valuestring);
    snprintf(s_session->user_id, sizeof(s_session->user_id), "%s", uid->valuestring);
    snprintf(s_session->filter_id, sizeof(s_session->filter_id), "%s", filter_id);
    if (cJSON_IsString(dev)) {
        snprintf(s_session->device_id, sizeof(s_session->device_id), "%s", dev->valuestring);
    }
    s_session->next_batch[0]       = '\0';
    s_session->room_count          = 0;
    s_session->pending_invite_count = 0;
    s_session->sync_count          = 0;
    s_session->last_sync_join_rooms   = 0;
    s_session->last_sync_invite_rooms = 0;
    s_session->last_sync_leave_rooms  = 0;
    s_session->last_sync_events    = 0;
    s_session->last_sync_messages = 0;
    s_session->total_messages      = 0;
    s_session->last_http_status    = 0;
    s_session->last_sync_us        = 0;
    s_session->state               = MATRIX_STATE_LOGGED_IN;
    matrix_unlock();

    preload_joined_rooms(base_url, tok->valuestring);
    persist_session(true);

    cJSON_Delete(root2);
    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Sync processing                                                           */
/* ------------------------------------------------------------------------ */

static bool room_join_is_space(cJSON *room_obj) {
    cJSON *state        = cJSON_GetObjectItemCaseSensitive(room_obj, "state");
    cJSON *state_events = state ? cJSON_GetObjectItemCaseSensitive(state, "events") : NULL;
    cJSON *ev           = NULL;
    if (!cJSON_IsArray(state_events)) return false;

    cJSON_ArrayForEach(ev, state_events) {
        cJSON *type = cJSON_GetObjectItemCaseSensitive(ev, "type");
        if (!cJSON_IsString(type) || strcmp(type->valuestring, "m.room.create") != 0) continue;
        cJSON *content   = cJSON_GetObjectItemCaseSensitive(ev, "content");
        cJSON *room_type = content ? cJSON_GetObjectItemCaseSensitive(content, "type") : NULL;
        return cJSON_IsString(room_type) && strcmp(room_type->valuestring, "m.space") == 0;
    }
    return false;
}

static bool room_obj_favorite_tag(cJSON *room_obj, bool *has_tag_out) {
    if (has_tag_out != NULL) *has_tag_out = false;
    if (room_obj == NULL) return false;
    cJSON *account_data = cJSON_GetObjectItemCaseSensitive(room_obj, "account_data");
    cJSON *events       = account_data ? cJSON_GetObjectItemCaseSensitive(account_data, "events") : NULL;
    if (!cJSON_IsArray(events)) return false;

    cJSON *ev = NULL;
    cJSON_ArrayForEach(ev, events) {
        cJSON *type = cJSON_GetObjectItemCaseSensitive(ev, "type");
        if (!cJSON_IsString(type) || strcmp(type->valuestring, "m.tag") != 0) continue;
        if (has_tag_out != NULL) *has_tag_out = true;
        cJSON *content = cJSON_GetObjectItemCaseSensitive(ev, "content");
        cJSON *tags    = content ? cJSON_GetObjectItemCaseSensitive(content, "tags") : NULL;
        cJSON *fav     = tags ? cJSON_GetObjectItemCaseSensitive(tags, "m.favourite") : NULL;
        return cJSON_IsObject(fav);
    }
    return false;
}

static void process_room_account_data(matrix_room_t *room, cJSON *room_obj) {
    if (room == NULL || room_obj == NULL) return;
    bool has_tag = false;
    bool favorite = room_obj_favorite_tag(room_obj, &has_tag);
    if (has_tag) {
        room->favorite = favorite;
    }
}

static void process_favorite_discovery_body(const char *body) {
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        ESP_LOGW(TAG, "Failed to parse favorite discovery response JSON");
        return;
    }

    cJSON *rooms = cJSON_GetObjectItemCaseSensitive(root, "rooms");
    cJSON *join  = rooms ? cJSON_GetObjectItemCaseSensitive(rooms, "join") : NULL;
    if (!cJSON_IsObject(join)) {
        cJSON_Delete(root);
        return;
    }

    matrix_lock();
    cJSON *room_obj = NULL;
    int favorite_count = 0;
    for (int i = 0; i < s_session->room_count; i++) {
        if (s_session->rooms[i].favorite) favorite_count++;
    }
    cJSON_ArrayForEach(room_obj, join) {
        if (room_obj->string == NULL) continue;
        bool has_tag = false;
        bool favorite = room_obj_favorite_tag(room_obj, &has_tag);
        if (!has_tag) continue;
        if (!favorite) {
            int existing_idx = matrix_find_room_index_by_id(room_obj->string);
            matrix_room_t *existing = matrix_get_room(existing_idx);
            if (existing != NULL && existing->favorite) {
                existing->favorite = false;
                if (favorite_count > 0) favorite_count--;
            }
            continue;
        }
        if (favorite_count >= MATRIX_CACHE_ROOMS && matrix_find_room_index_by_id(room_obj->string) < 0) {
            continue;
        }

        matrix_room_t *room = find_or_create_room(room_obj->string);
        if (room == NULL) break;
        if (!room->favorite) favorite_count++;
        room->favorite = true;
    }
    sort_rooms_by_activity();
    s_session->dirty = true;
    matrix_unlock();

    persist_session(true);
    cJSON_Delete(root);
}

static matrix_sync_parse_counts_t process_room_join(const char *room_id, cJSON *room_obj) {
    matrix_sync_parse_counts_t counts = {0};
    if (room_join_is_space(room_obj)) {
        remove_room_by_id(room_id);
        return counts;
    }

    matrix_room_t *room = find_or_create_room(room_id);
    if (room == NULL) return counts;
    process_room_account_data(room, room_obj);

    cJSON *state        = cJSON_GetObjectItemCaseSensitive(room_obj, "state");
    cJSON *state_events = state ? cJSON_GetObjectItemCaseSensitive(state, "events") : NULL;
    cJSON *ev            = NULL;
    if (cJSON_IsArray(state_events)) {
        cJSON_ArrayForEach(ev, state_events) {
            cJSON *type = cJSON_GetObjectItemCaseSensitive(ev, "type");
            if (!cJSON_IsString(type)) continue;
            cJSON *content = cJSON_GetObjectItemCaseSensitive(ev, "content");
            if (strcmp(type->valuestring, "m.room.name") == 0 && content) {
                cJSON *name = cJSON_GetObjectItemCaseSensitive(content, "name");
                if (cJSON_IsString(name) && name->valuestring[0] != '\0') {
                    snprintf(room->name, sizeof(room->name), "%s", name->valuestring);
                    room->has_name = true;
                }
            } else if (strcmp(type->valuestring, "m.room.canonical_alias") == 0 && content && !room->has_name) {
                cJSON *alias = cJSON_GetObjectItemCaseSensitive(content, "alias");
                if (cJSON_IsString(alias) && alias->valuestring[0] != '\0') {
                    snprintf(room->name, sizeof(room->name), "%s", alias->valuestring);
                }
            } else if (strcmp(type->valuestring, "m.room.encryption") == 0) {
                room->encrypted = true;
            } else if (strcmp(type->valuestring, "m.room.member") == 0) {
                process_member_event(room, ev);
            }
        }
    }

    cJSON *timeline        = cJSON_GetObjectItemCaseSensitive(room_obj, "timeline");
    cJSON *timeline_events = timeline ? cJSON_GetObjectItemCaseSensitive(timeline, "events") : NULL;
    if (cJSON_IsArray(timeline_events)) {
        cJSON_ArrayForEach(ev, timeline_events) {
            cJSON *type     = cJSON_GetObjectItemCaseSensitive(ev, "type");
            cJSON *sender   = cJSON_GetObjectItemCaseSensitive(ev, "sender");
            cJSON *ts       = cJSON_GetObjectItemCaseSensitive(ev, "origin_server_ts");
            cJSON *event_id = cJSON_GetObjectItemCaseSensitive(ev, "event_id");
            if (!cJSON_IsString(type)) continue;
            counts.events++;
            const char *sender_str = cJSON_IsString(sender) ? sender->valuestring : "?";
            const char *event_str  = cJSON_IsString(event_id) ? event_id->valuestring : "";
            int64_t     ts_val     = cJSON_IsNumber(ts) ? (int64_t)ts->valuedouble : 0;

            if (strcmp(type->valuestring, "m.room.message") == 0) {
                cJSON *content = cJSON_GetObjectItemCaseSensitive(ev, "content");
                cJSON *msgtype = content ? cJSON_GetObjectItemCaseSensitive(content, "msgtype") : NULL;
                char   body_buf[MATRIX_BODY_LEN];
                const char *display_body = message_display_body(content, body_buf, sizeof(body_buf));
                if (display_body != NULL) {
                    bool notice = cJSON_IsString(msgtype) && strcmp(msgtype->valuestring, "m.notice") == 0;
                    if (append_message(room, event_str, sender_str, display_body, ts_val, notice)) {
                        counts.messages++;
                        if (strcmp(sender_str, s_session->user_id) != 0) {
                            room->unread = true;
                            if (room->unread_count < UINT16_MAX) room->unread_count++;
                        }
                    }
                }
            } else if (strcmp(type->valuestring, "m.room.encrypted") == 0) {
                room->encrypted = true;
                if (append_message(room, event_str, sender_str, "[encrypted message]", ts_val, true)) {
                    counts.messages++;
                    if (strcmp(sender_str, s_session->user_id) != 0) {
                        room->unread = true;
                        if (room->unread_count < UINT16_MAX) room->unread_count++;
                    }
                }
            } else if (strcmp(type->valuestring, "m.reaction") == 0) {
                char reaction_event_id[MATRIX_EVENT_ID_LEN];
                char reaction_key[MATRIX_REACTION_KEY_LEN];
                if (parse_reaction_event(ev, reaction_event_id, sizeof(reaction_event_id), reaction_key, sizeof(reaction_key))) {
                    if (append_reaction(room, reaction_event_id, reaction_key)) {
                        s_session->dirty = true;
                    }
                }
            } else if (strcmp(type->valuestring, "m.room.member") == 0) {
                process_member_event(room, ev);
            }
        }
    }

    return counts;
}

static void process_sync_body(const char *body) {
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        ESP_LOGW(TAG, "Failed to parse sync response JSON");
        matrix_set_last_error("Sync response was not valid JSON");
        return;
    }

    cJSON *next_batch = cJSON_GetObjectItemCaseSensitive(root, "next_batch");

    matrix_lock();
    if (cJSON_IsString(next_batch)) {
        snprintf(s_session->next_batch, sizeof(s_session->next_batch), "%s", next_batch->valuestring);
    }

    cJSON *rooms  = cJSON_GetObjectItemCaseSensitive(root, "rooms");
    cJSON *join   = rooms ? cJSON_GetObjectItemCaseSensitive(rooms, "join") : NULL;
    cJSON *invite = rooms ? cJSON_GetObjectItemCaseSensitive(rooms, "invite") : NULL;
    cJSON *leave  = rooms ? cJSON_GetObjectItemCaseSensitive(rooms, "leave") : NULL;
    uint32_t sync_join_count   = 0;
    uint32_t sync_invite_count = 0;
    uint32_t sync_leave_count  = 0;
    matrix_sync_parse_counts_t counts = {0};
    if (join != NULL) {
        cJSON *room_obj = NULL;
        cJSON_ArrayForEach(room_obj, join) {
            matrix_sync_parse_counts_t room_counts = process_room_join(room_obj->string, room_obj);
            counts.events += room_counts.events;
            counts.messages += room_counts.messages;
            sync_join_count++;
        }
    }
    if (invite != NULL) {
        cJSON *room_obj = NULL;
        cJSON_ArrayForEach(room_obj, invite) {
            sync_invite_count++;
            if (room_obj->string != NULL && s_session->pending_invite_count < MATRIX_MAX_ROOMS) {
                bool already_pending = false;
                for (int i = 0; i < s_session->pending_invite_count; i++) {
                    if (strcmp(s_session->pending_invites[i], room_obj->string) == 0) {
                        already_pending = true;
                        break;
                    }
                }
                if (!already_pending) {
                    snprintf(
                        s_session->pending_invites[s_session->pending_invite_count],
                        sizeof(s_session->pending_invites[s_session->pending_invite_count]), "%s", room_obj->string
                    );
                    s_session->pending_invite_count++;
                }
            }
        }
    }
    if (leave != NULL) {
        cJSON *room_obj = NULL;
        cJSON_ArrayForEach(room_obj, leave) {
            sync_leave_count++;
        }
    }
    s_session->sync_count++;
    s_session->last_sync_join_rooms   = sync_join_count;
    s_session->last_sync_invite_rooms = sync_invite_count;
    s_session->last_sync_leave_rooms  = sync_leave_count;
    s_session->last_sync_events       = counts.events;
    s_session->last_sync_messages     = counts.messages;
    s_session->last_sync_us           = esp_timer_get_time();
    if (counts.messages > 0) {
        sort_rooms_by_activity();
    }
    bool force_persist = s_session->sync_count <= 1 || counts.messages > 0;
    s_session->dirty = true;
    matrix_unlock();
    persist_session(force_persist);

    cJSON_Delete(root);
}

static esp_err_t join_invited_room(
    const char *homeserver, const char *token, const char *room_id, char *err_out, size_t err_out_len
) {
    if (err_out != NULL && err_out_len > 0) err_out[0] = '\0';

    char encoded_room_id[MATRIX_ROOM_ID_LEN * 3];
    if (!matrix_url_encode(room_id, encoded_room_id, sizeof(encoded_room_id))) {
        if (err_out) snprintf(err_out, err_out_len, "invite room ID too long");
        return ESP_ERR_INVALID_SIZE;
    }

    char url[MATRIX_HOMESERVER_LEN + sizeof(encoded_room_id) + 64];
    int  url_len = snprintf(url, sizeof(url), "%s/_matrix/client/v3/join/%s", homeserver, encoded_room_id);
    if (url_len < 0 || url_len >= (int)sizeof(url)) {
        if (err_out) snprintf(err_out, err_out_len, "join URL too long");
        return ESP_ERR_INVALID_SIZE;
    }

    char     *resp_body = NULL;
    int       status    = 0;
    char      net_detail[96];
    esp_err_t res = matrix_http_request("POST", url, token, "{}", &resp_body, &status, net_detail, sizeof(net_detail));
    if (res != ESP_OK) {
        if (err_out) snprintf(err_out, err_out_len, "Auto-join network: %s %s", esp_err_to_name(res), net_detail);
        free(resp_body);
        return res;
    }
    if (status < 200 || status >= 300) {
        char reason[96] = "join failed";
        if (resp_body != NULL) {
            cJSON *eroot = cJSON_Parse(resp_body);
            if (eroot != NULL) {
                cJSON *err = cJSON_GetObjectItemCaseSensitive(eroot, "error");
                if (cJSON_IsString(err) && err->valuestring != NULL) {
                    snprintf(reason, sizeof(reason), "%s", err->valuestring);
                }
                cJSON_Delete(eroot);
            }
        }
        if (err_out) snprintf(err_out, err_out_len, "Auto-join HTTP %d: %s", status, reason);
        free(resp_body);
        return ESP_FAIL;
    }

    free(resp_body);
    return ESP_OK;
}

static void auto_join_pending_invites(void) {
    char homeserver_copy[MATRIX_HOMESERVER_LEN];
    char token_copy[MATRIX_TOKEN_LEN];
    char invite_id[MATRIX_ROOM_ID_LEN];

    matrix_lock();
    snprintf(homeserver_copy, sizeof(homeserver_copy), "%s", s_session->homeserver);
    snprintf(token_copy, sizeof(token_copy), "%s", s_session->access_token);
    while (s_session->pending_invite_count > 0) {
        snprintf(invite_id, sizeof(invite_id), "%s", s_session->pending_invites[0]);
        if (s_session->pending_invite_count > 1) {
            memmove(
                s_session->pending_invites[0], s_session->pending_invites[1],
                (s_session->pending_invite_count - 1) * sizeof(s_session->pending_invites[0])
            );
        }
        s_session->pending_invite_count--;
        matrix_unlock();

        char err[MATRIX_ERROR_LEN];
        esp_err_t res = join_invited_room(homeserver_copy, token_copy, invite_id, err, sizeof(err));
        if (res != ESP_OK) {
            matrix_set_last_error(err);
        } else {
            matrix_clear_last_error();
        }

        matrix_lock();
    }
    matrix_unlock();
}

static bool fetch_room_state_string(
    const char *homeserver, const char *token, const char *room_id, const char *event_type, const char *field,
    char *out, size_t out_len
) {
    char encoded_room_id[MATRIX_ROOM_ID_LEN * 3];
    char encoded_event_type[64];
    if (!matrix_url_encode(room_id, encoded_room_id, sizeof(encoded_room_id))) return false;
    if (!matrix_url_encode(event_type, encoded_event_type, sizeof(encoded_event_type))) return false;

    char url[MATRIX_HOMESERVER_LEN + sizeof(encoded_room_id) + sizeof(encoded_event_type) + 96];
    int  url_len = snprintf(
        url, sizeof(url), "%s/_matrix/client/v3/rooms/%s/state/%s", homeserver, encoded_room_id, encoded_event_type
    );
    if (url_len < 0 || url_len >= (int)sizeof(url)) return false;

    char *resp_body = NULL;
    int   status    = 0;
    esp_err_t res = matrix_http_request("GET", url, token, NULL, &resp_body, &status, NULL, 0);
    if (res != ESP_OK || status < 200 || status >= 300 || resp_body == NULL) {
        free(resp_body);
        return false;
    }

    bool   ok   = false;
    cJSON *root = cJSON_Parse(resp_body);
    if (root != NULL) {
        cJSON *value = cJSON_GetObjectItemCaseSensitive(root, field);
        if (cJSON_IsString(value) && value->valuestring != NULL && value->valuestring[0] != '\0') {
            snprintf(out, out_len, "%s", value->valuestring);
            ok = true;
        }
        cJSON_Delete(root);
    }
    free(resp_body);
    return ok;
}

static void room_metadata_task(void *arg) {
    (void)arg;
    int cursor = 0;

    while (s_session != NULL && s_session->sync_should_run) {
        char homeserver_copy[MATRIX_HOMESERVER_LEN];
        char token_copy[MATRIX_TOKEN_LEN];
        char room_id[MATRIX_ROOM_ID_LEN] = "";

        matrix_lock();
        snprintf(homeserver_copy, sizeof(homeserver_copy), "%s", s_session->homeserver);
        snprintf(token_copy, sizeof(token_copy), "%s", s_session->access_token);
        int count = s_session->room_count;
        if (count > 0) {
            if (cursor >= count) cursor = 0;
            for (int checked = 0; checked < count; checked++) {
                int idx = (cursor + checked) % count;
                if (!s_session->rooms[idx].has_name) {
                    snprintf(room_id, sizeof(room_id), "%s", s_session->rooms[idx].room_id);
                    cursor = idx + 1;
                    break;
                }
            }
        }
        matrix_unlock();

        if (homeserver_copy[0] == '\0' || token_copy[0] == '\0') {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (room_id[0] == '\0') {
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        char name[MATRIX_ROOM_NAME_LEN] = "";
        bool got_name = fetch_room_state_string(
            homeserver_copy, token_copy, room_id, "m.room.name", "name", name, sizeof(name)
        );
        if (!got_name) {
            got_name = fetch_room_state_string(
                homeserver_copy, token_copy, room_id, "m.room.canonical_alias", "alias", name, sizeof(name)
            );
        }

        if (got_name) {
            matrix_lock();
            if (set_room_name_by_id(room_id, name)) {
                s_session->dirty = true;
            }
            matrix_unlock();
            persist_session(false);
        }

        vTaskDelay(pdMS_TO_TICKS(150));
    }

    vTaskDelete(NULL);
}

static void sync_task(void *arg) {
    (void)arg;
    char url[MATRIX_HOMESERVER_LEN + 512];

    while (s_session->sync_should_run) {
        char homeserver_copy[MATRIX_HOMESERVER_LEN];
        char token_copy[MATRIX_TOKEN_LEN];
        char since_copy[128];
        char filter_id_copy[64];
        matrix_lock();
        snprintf(homeserver_copy, sizeof(homeserver_copy), "%s", s_session->homeserver);
        snprintf(token_copy, sizeof(token_copy), "%s", s_session->access_token);
        snprintf(since_copy, sizeof(since_copy), "%s", s_session->next_batch);
        snprintf(filter_id_copy, sizeof(filter_id_copy), "%s", s_session->filter_id);
        matrix_unlock();

        if (filter_id_copy[0] == '\0') {
            matrix_set_last_error("Sync filter is not registered");
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        int url_len = 0;
        if (since_copy[0] != '\0') {
            char encoded_since[sizeof(since_copy) * 3];
            if (!matrix_url_encode(since_copy, encoded_since, sizeof(encoded_since))) {
                matrix_set_last_error("Sync since token too long");
                vTaskDelay(pdMS_TO_TICKS(3000));
                continue;
            }
            char encoded_filter_id[sizeof(filter_id_copy) * 3];
            if (!matrix_url_encode(filter_id_copy, encoded_filter_id, sizeof(encoded_filter_id))) {
                matrix_set_last_error("Sync filter ID too long");
                vTaskDelay(pdMS_TO_TICKS(3000));
                continue;
            }
            url_len = snprintf(
                url, sizeof(url), "%s/_matrix/client/v3/sync?filter=%s&timeout=%d&since=%s", homeserver_copy,
                encoded_filter_id, SYNC_TIMEOUT_MS, encoded_since
            );
        } else {
            char encoded_filter_id[sizeof(filter_id_copy) * 3];
            if (!matrix_url_encode(filter_id_copy, encoded_filter_id, sizeof(encoded_filter_id))) {
                matrix_set_last_error("Sync filter ID too long");
                vTaskDelay(pdMS_TO_TICKS(3000));
                continue;
            }
            url_len = snprintf(
                url, sizeof(url), "%s/_matrix/client/v3/sync?filter=%s&timeout=%d", homeserver_copy,
                encoded_filter_id, INITIAL_SYNC_TIMEOUT_MS
            );
        }
        if (url_len < 0 || url_len >= (int)sizeof(url)) {
            matrix_set_last_error("Sync URL too long");
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        matrix_lock();
        s_session->state = MATRIX_STATE_SYNCING;
        matrix_unlock();
        char     *resp_body = NULL;
        int       status    = 0;
        char      net_detail[96];
        esp_err_t res       = matrix_http_request(
            "GET", url, token_copy, NULL, &resp_body, &status, net_detail, sizeof(net_detail)
        );
        matrix_lock();
        s_session->last_http_status = status;
        matrix_unlock();

        if (!s_session->sync_should_run) {
            free(resp_body);
            break;
        }

        if (res == ESP_OK && status == 200 && resp_body != NULL) {
            process_sync_body(resp_body);
            auto_join_pending_invites();
            matrix_clear_last_error();
            matrix_lock();
            s_session->state = MATRIX_STATE_LOGGED_IN;
            matrix_unlock();
        } else if (res == ESP_OK && status == 401) {
            matrix_lock();
            snprintf(s_session->last_error, sizeof(s_session->last_error), "Session expired, please log in again");
            s_session->dirty = true;
            matrix_unlock();
            s_session->state           = MATRIX_STATE_ERROR;
            s_session->sync_should_run = false;
            free(resp_body);
            break;
        } else {
            ESP_LOGW(TAG, "Sync request failed (res=%s status=%d), retrying", esp_err_to_name(res), status);
            char reason[MATRIX_ERROR_LEN] = "";
            if (res == ESP_OK) {
                snprintf(reason, sizeof(reason), "Sync HTTP %d", status);
                if (resp_body != NULL) {
                    cJSON *eroot = cJSON_Parse(resp_body);
                    if (eroot != NULL) {
                        cJSON *err = cJSON_GetObjectItemCaseSensitive(eroot, "error");
                        if (cJSON_IsString(err) && err->valuestring != NULL) {
                            snprintf(reason, sizeof(reason), "Sync HTTP %d: %s", status, err->valuestring);
                        }
                        cJSON_Delete(eroot);
                    }
                }
            } else {
                snprintf(reason, sizeof(reason), "Sync network: %s %s", esp_err_to_name(res), net_detail);
            }
            matrix_set_last_error(reason);
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
        free(resp_body);
    }

    vTaskDelete(NULL);
}

static void sender_task(void *arg) {
    (void)arg;
    matrix_send_request_t req;
    uint32_t               txn_counter = 0;

    while (true) {
        if (xQueueReceive(s_session->send_queue, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        char homeserver_copy[MATRIX_HOMESERVER_LEN];
        char token_copy[MATRIX_TOKEN_LEN];
        matrix_lock();
        snprintf(homeserver_copy, sizeof(homeserver_copy), "%s", s_session->homeserver);
        snprintf(token_copy, sizeof(token_copy), "%s", s_session->access_token);
        matrix_unlock();

        if (homeserver_copy[0] == '\0' || token_copy[0] == '\0') {
            continue;  // logged out, drop
        }

        char encoded_room_id[MATRIX_ROOM_ID_LEN * 3];
        matrix_url_encode(req.room_id, encoded_room_id, sizeof(encoded_room_id));

        txn_counter++;
        char txn_id[48];
        snprintf(txn_id, sizeof(txn_id), "tm%" PRId64 "_%" PRIu32, esp_timer_get_time(), txn_counter);

        char url[MATRIX_HOMESERVER_LEN + sizeof(encoded_room_id) + 128];
        snprintf(
            url, sizeof(url), "%s/_matrix/client/v3/rooms/%s/send/m.room.message/%s", homeserver_copy,
            encoded_room_id, txn_id
        );

        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "msgtype", "m.text");
        cJSON_AddStringToObject(root, "body", req.body);
        char *body_str = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);

        char     *resp_body = NULL;
        int       status    = 0;
        esp_err_t res = matrix_http_request("PUT", url, token_copy, body_str, &resp_body, &status, NULL, 0);
        free(body_str);
        if (res != ESP_OK || status < 200 || status >= 300) {
            ESP_LOGW(TAG, "Failed to send message (res=%s status=%d)", esp_err_to_name(res), status);
        }
        free(resp_body);
    }
}

static void favorite_discovery_task(void *arg) {
    (void)arg;
    char homeserver_copy[MATRIX_HOMESERVER_LEN];
    char user_copy[MATRIX_USER_ID_LEN];
    char token_copy[MATRIX_TOKEN_LEN];

    matrix_lock();
    snprintf(homeserver_copy, sizeof(homeserver_copy), "%s", s_session->homeserver);
    snprintf(user_copy, sizeof(user_copy), "%s", s_session->user_id);
    snprintf(token_copy, sizeof(token_copy), "%s", s_session->access_token);
    matrix_unlock();

    if (homeserver_copy[0] == '\0' || user_copy[0] == '\0' || token_copy[0] == '\0') {
        vTaskDelete(NULL);
        return;
    }

    char filter_id[64];
    char filter_err[128];
    if (register_sync_filter(
            homeserver_copy, user_copy, token_copy, MATRIX_TAGS_FILTER_JSON, filter_id, sizeof(filter_id), filter_err,
            sizeof(filter_err)
        ) != ESP_OK) {
        ESP_LOGW(TAG, "favorite filter registration failed: %s", filter_err);
        vTaskDelete(NULL);
        return;
    }

    char encoded_filter_id[sizeof(filter_id) * 3];
    if (!matrix_url_encode(filter_id, encoded_filter_id, sizeof(encoded_filter_id))) {
        vTaskDelete(NULL);
        return;
    }

    char url[MATRIX_HOMESERVER_LEN + sizeof(encoded_filter_id) + 80];
    int url_len = snprintf(
        url, sizeof(url), "%s/_matrix/client/v3/sync?filter=%s&timeout=0", homeserver_copy, encoded_filter_id
    );
    if (url_len < 0 || url_len >= (int)sizeof(url)) {
        vTaskDelete(NULL);
        return;
    }

    char *resp_body = NULL;
    int status = 0;
    char net_detail[96];
    esp_err_t res = matrix_http_request(
        "GET", url, token_copy, NULL, &resp_body, &status, net_detail, sizeof(net_detail)
    );
    if (res == ESP_OK && status == 200 && resp_body != NULL) {
        process_favorite_discovery_body(resp_body);
    } else {
        ESP_LOGW(TAG, "favorite discovery failed: %s status=%d %s", esp_err_to_name(res), status, net_detail);
    }
    free(resp_body);
    vTaskDelete(NULL);
}

static void process_history_body(const char *room_id, const char *body) {
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        ESP_LOGW(TAG, "Failed to parse room history response JSON");
        return;
    }

    cJSON *chunk = cJSON_GetObjectItemCaseSensitive(root, "chunk");
    if (!cJSON_IsArray(chunk)) {
        cJSON_Delete(root);
        return;
    }

    int count = cJSON_GetArraySize(chunk);
    matrix_lock();
    matrix_room_t *room = find_or_create_room(room_id);
    if (room != NULL) {
        for (int i = count - 1; i >= 0; i--) {
            cJSON *ev = cJSON_GetArrayItem(chunk, i);
            cJSON *type     = cJSON_GetObjectItemCaseSensitive(ev, "type");
            cJSON *sender   = cJSON_GetObjectItemCaseSensitive(ev, "sender");
            cJSON *ts       = cJSON_GetObjectItemCaseSensitive(ev, "origin_server_ts");
            cJSON *event_id = cJSON_GetObjectItemCaseSensitive(ev, "event_id");
            if (!cJSON_IsString(type)) continue;

            const char *sender_str = cJSON_IsString(sender) ? sender->valuestring : "?";
            const char *event_str  = cJSON_IsString(event_id) ? event_id->valuestring : "";
            int64_t     ts_val     = cJSON_IsNumber(ts) ? (int64_t)ts->valuedouble : 0;

            if (strcmp(type->valuestring, "m.room.message") == 0) {
                cJSON *content = cJSON_GetObjectItemCaseSensitive(ev, "content");
                cJSON *msgtype = content ? cJSON_GetObjectItemCaseSensitive(content, "msgtype") : NULL;
                char   body_buf[MATRIX_BODY_LEN];
                const char *display_body = message_display_body(content, body_buf, sizeof(body_buf));
                if (display_body != NULL) {
                    bool notice = cJSON_IsString(msgtype) && strcmp(msgtype->valuestring, "m.notice") == 0;
                    append_message(room, event_str, sender_str, display_body, ts_val, notice);
                }
            } else if (strcmp(type->valuestring, "m.room.encrypted") == 0) {
                room->encrypted = true;
                append_message(room, event_str, sender_str, "[encrypted message]", ts_val, true);
            } else if (strcmp(type->valuestring, "m.reaction") == 0) {
                char reaction_event_id[MATRIX_EVENT_ID_LEN];
                char reaction_key[MATRIX_REACTION_KEY_LEN];
                if (parse_reaction_event(ev, reaction_event_id, sizeof(reaction_event_id), reaction_key, sizeof(reaction_key))) {
                    append_reaction(room, reaction_event_id, reaction_key);
                }
            }
        }
        sort_rooms_by_activity();
        s_session->dirty = true;
    }
    matrix_unlock();

    persist_session(true);
    matrix_save_history_cache(room_id);
    cJSON_Delete(root);
}

static void process_joined_members_body(const char *room_id, const char *body) {
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        ESP_LOGW(TAG, "Failed to parse joined_members response JSON");
        return;
    }

    cJSON *joined = cJSON_GetObjectItemCaseSensitive(root, "joined");
    if (!cJSON_IsObject(joined)) {
        cJSON_Delete(root);
        return;
    }

    matrix_lock();
    matrix_room_t *room = find_or_create_room(room_id);
    if (room != NULL) {
        cJSON *member = NULL;
        cJSON_ArrayForEach(member, joined) {
            if (member->string == NULL) continue;
            cJSON *display = cJSON_GetObjectItemCaseSensitive(member, "display_name");
            upsert_active_member(room, member->string, cJSON_IsString(display) ? display->valuestring : NULL);
            upsert_room_member(room, member->string, cJSON_IsString(display) ? display->valuestring : NULL);
        }
        s_session->dirty = true;
    }
    matrix_unlock();

    persist_session(true);
    cJSON_Delete(root);
}

typedef struct {
    char event_id[MATRIX_EVENT_ID_LEN];
    uint32_t duration_ms;
} audio_progress_ctx_t;

static void format_audio_progress(char *out, size_t out_len, uint32_t elapsed_ms, uint32_t duration_ms) {
    unsigned int es = (unsigned int)(elapsed_ms / 1000);
    if (duration_ms > 0) {
        unsigned int ds = (unsigned int)(duration_ms / 1000);
        snprintf(out, out_len, "%u:%02u / %u:%02u", es / 60, es % 60, ds / 60, ds % 60);
    } else {
        snprintf(out, out_len, "%u:%02u", es / 60, es % 60);
    }
}

static void audio_progress_callback(uint32_t elapsed_ms, void *ctx_ptr) {
    audio_progress_ctx_t *ctx = (audio_progress_ctx_t *)ctx_ptr;
    char text[MATRIX_AUDIO_STATUS_LEN];
    format_audio_progress(text, sizeof(text), elapsed_ms, ctx->duration_ms);
    matrix_set_audio_status(ctx->event_id, text);
}

static void media_download_task(void *arg) {
    matrix_media_request_t req = {0};
    if (arg != NULL) {
        memcpy(&req, arg, sizeof(req));
        free(arg);
    }
    if (req.room_id[0] == '\0' || req.event_id[0] == '\0') {
        vTaskDelete(NULL);
        return;
    }
    matrix_set_audio_status(req.event_id, "downloading");

    char homeserver_copy[MATRIX_HOMESERVER_LEN] = "";
    char token_copy[MATRIX_TOKEN_LEN] = "";
    matrix_lock();
    if (s_session != NULL) {
        snprintf(homeserver_copy, sizeof(homeserver_copy), "%s", s_session->homeserver);
        snprintf(token_copy, sizeof(token_copy), "%s", s_session->access_token);
    }
    matrix_unlock();

    if (homeserver_copy[0] == '\0' || token_copy[0] == '\0') {
        matrix_set_audio_status(req.event_id, "no session");
        matrix_set_last_error("audio: no session");
        vTaskDelete(NULL);
        return;
    }

    char room_enc[MATRIX_ROOM_ID_LEN * 3];
    char event_enc[MATRIX_EVENT_ID_LEN * 3];
    if (!matrix_url_encode(req.room_id, room_enc, sizeof(room_enc)) ||
        !matrix_url_encode(req.event_id, event_enc, sizeof(event_enc))) {
        matrix_set_audio_status(req.event_id, "bad event id");
        matrix_set_last_error("audio: bad event id");
        vTaskDelete(NULL);
        return;
    }

    char event_url[MATRIX_HOMESERVER_LEN + MATRIX_ROOM_ID_LEN * 3 + MATRIX_EVENT_ID_LEN * 3 + 64];
    snprintf(event_url, sizeof(event_url), "%s/_matrix/client/v3/rooms/%s/event/%s", homeserver_copy, room_enc, event_enc);

    char *event_body = NULL;
    int status = 0;
    char detail[96];
    esp_err_t res = matrix_http_request("GET", event_url, token_copy, NULL, &event_body, &status, detail, sizeof(detail));
    if (res != ESP_OK || status != 200 || event_body == NULL) {
        matrix_set_audio_status(req.event_id, "event fetch failed");
        matrix_set_last_error("audio: event fetch failed");
        free(event_body);
        vTaskDelete(NULL);
        return;
    }

    cJSON *root = cJSON_Parse(event_body);
    free(event_body);
    cJSON *content = root ? cJSON_GetObjectItemCaseSensitive(root, "content") : NULL;
    cJSON *msgtype = content ? cJSON_GetObjectItemCaseSensitive(content, "msgtype") : NULL;
    cJSON *mxc     = content ? cJSON_GetObjectItemCaseSensitive(content, "url") : NULL;
    cJSON *body    = content ? cJSON_GetObjectItemCaseSensitive(content, "body") : NULL;
    cJSON *info    = content ? cJSON_GetObjectItemCaseSensitive(content, "info") : NULL;
    cJSON *mime    = info ? cJSON_GetObjectItemCaseSensitive(info, "mimetype") : NULL;
    cJSON *size    = info ? cJSON_GetObjectItemCaseSensitive(info, "size") : NULL;
    cJSON *duration = info ? cJSON_GetObjectItemCaseSensitive(info, "duration") : NULL;
    uint32_t duration_ms = (cJSON_IsNumber(duration) && duration->valuedouble > 0) ? (uint32_t)duration->valuedouble : 0;

    bool is_audio = cJSON_IsString(msgtype) && strcmp(msgtype->valuestring, "m.audio") == 0;
    bool is_image = cJSON_IsString(msgtype) && strcmp(msgtype->valuestring, "m.image") == 0;
    bool is_video = cJSON_IsString(msgtype) && strcmp(msgtype->valuestring, "m.video") == 0;
    if ((!is_audio && !is_image && !is_video) || !cJSON_IsString(mxc) || strncmp(mxc->valuestring, "mxc://", 6) != 0) {
        cJSON_Delete(root);
        matrix_set_audio_status(req.event_id, "not media");
        matrix_set_last_error("media: not audio/image/video");
        vTaskDelete(NULL);
        return;
    }

    char media_url[MATRIX_HOMESERVER_LEN + 700];
    if (!matrix_mxc_to_download_url(homeserver_copy, mxc->valuestring, media_url, sizeof(media_url))) {
        cJSON_Delete(root);
        matrix_set_audio_status(req.event_id, "bad mxc url");
        matrix_set_last_error("audio: bad mxc url");
        vTaskDelete(NULL);
        return;
    }

    const char *mimetype =
        cJSON_IsString(mime) ? mime->valuestring : (is_image ? "image/jpeg" : (is_video ? "video/mp4" : "audio/ogg"));
    const char *label = cJSON_IsString(body) ? body->valuestring : (is_image ? "image" : (is_video ? "video" : "audio"));
    size_t max_bytes = is_image ? (4 * 1024 * 1024) : (is_video ? (24 * 1024 * 1024) : (2 * 1024 * 1024));
    if (cJSON_IsNumber(size) && size->valuedouble > 0 && size->valuedouble < (double)max_bytes) {
        max_bytes = (size_t)size->valuedouble + 4096;
    }

    uint8_t *media_data = NULL;
    size_t media_len = 0;
    status = 0;
    if (is_image) {
        char thumbnail_url[MATRIX_HOMESERVER_LEN + 700];
        if (matrix_mxc_to_thumbnail_url(homeserver_copy, mxc->valuestring, thumbnail_url, sizeof(thumbnail_url))) {
            res = matrix_http_download_buffer(thumbnail_url, token_copy, max_bytes, &media_data, &media_len, &status);
        } else {
            res = ESP_FAIL;
        }
    }
    if (!is_image || res != ESP_OK) {
        res = matrix_http_download_buffer(media_url, token_copy, max_bytes, &media_data, &media_len, &status);
    }
    if (res == ESP_OK) {
        if (is_audio) {
            matrix_set_audio_status(req.event_id, "downloaded");
            audio_progress_ctx_t progress_ctx;
            snprintf(progress_ctx.event_id, sizeof(progress_ctx.event_id), "%s", req.event_id);
            progress_ctx.duration_ms = duration_ms;
            audio_player_set_progress_callback(audio_progress_callback, &progress_ctx);
            res = audio_player_play_buffer(media_data, media_len, mimetype, label, true);
            audio_player_set_progress_callback(NULL, NULL);
            if (res != ESP_OK) {
                matrix_set_audio_status(req.event_id, audio_player_last_error());
                matrix_set_last_error(audio_player_last_error());
            } else {
                matrix_set_audio_status(req.event_id, "played");
            }
            heap_caps_free(media_data);
            media_data = NULL;
        } else if (is_video) {
            matrix_set_audio_status(req.event_id, "video: playing");
            res = video_player_play_buffer(media_data, media_len, label);
            if (res != ESP_OK) {
                matrix_set_audio_status(req.event_id, video_player_last_error());
                matrix_set_last_error(video_player_last_error());
            } else {
                matrix_set_audio_status(req.event_id, "video: done");
            }
            heap_caps_free(media_data);
            media_data = NULL;
        } else {
            matrix_store_image_cache(req.event_id, mimetype, label, media_data, media_len);
            media_data = NULL;
            matrix_set_audio_status(req.event_id, "image ready");
        }
    } else {
        matrix_set_audio_status(req.event_id, "download failed");
        matrix_set_last_error("media: download failed");
    }
    heap_caps_free(media_data);
    cJSON_Delete(root);
    vTaskDelete(NULL);
}

static void history_task(void *arg) {
    matrix_history_request_t req = {0};
    if (arg != NULL) {
        memcpy(&req, arg, sizeof(req));
        free(arg);
    }
    if (req.room_id[0] == '\0') {
        vTaskDelete(NULL);
        return;
    }

    char homeserver_copy[MATRIX_HOMESERVER_LEN];
    char token_copy[MATRIX_TOKEN_LEN];
    matrix_lock();
    snprintf(homeserver_copy, sizeof(homeserver_copy), "%s", s_session->homeserver);
    snprintf(token_copy, sizeof(token_copy), "%s", s_session->access_token);
    matrix_unlock();

    if (homeserver_copy[0] == '\0' || token_copy[0] == '\0') {
        vTaskDelete(NULL);
        return;
    }

    char encoded_room_id[MATRIX_ROOM_ID_LEN * 3];
    if (!matrix_url_encode(req.room_id, encoded_room_id, sizeof(encoded_room_id))) {
        vTaskDelete(NULL);
        return;
    }

    char members_url[MATRIX_HOMESERVER_LEN + sizeof(encoded_room_id) + 64];
    int members_url_len = snprintf(
        members_url, sizeof(members_url), "%s/_matrix/client/v3/rooms/%s/joined_members",
        homeserver_copy, encoded_room_id
    );
    if (members_url_len > 0 && members_url_len < (int)sizeof(members_url)) {
        char *members_body = NULL;
        int members_status = 0;
        char members_detail[96];
        esp_err_t members_res = matrix_http_request(
            "GET", members_url, token_copy, NULL, &members_body, &members_status, members_detail, sizeof(members_detail)
        );
        if (members_res == ESP_OK && members_status == 200 && members_body != NULL) {
            process_joined_members_body(req.room_id, members_body);
        } else {
            ESP_LOGW(
                TAG, "joined_members failed: %s status=%d %s", esp_err_to_name(members_res), members_status,
                members_detail
            );
        }
        free(members_body);
    }

    char url[MATRIX_HOMESERVER_LEN + sizeof(encoded_room_id) + 96];
    int url_len = snprintf(
        url, sizeof(url), "%s/_matrix/client/v3/rooms/%s/messages?dir=b&limit=%d",
        homeserver_copy, encoded_room_id, MATRIX_HISTORY_FETCH_LIMIT
    );
    if (url_len < 0 || url_len >= (int)sizeof(url)) {
        vTaskDelete(NULL);
        return;
    }

    char *resp_body = NULL;
    int status = 0;
    char net_detail[96];
    esp_err_t res = matrix_http_request(
        "GET", url, token_copy, NULL, &resp_body, &status, net_detail, sizeof(net_detail)
    );
    if (res == ESP_OK && status == 200 && resp_body != NULL) {
        process_history_body(req.room_id, resp_body);
    } else {
        ESP_LOGW(TAG, "room history failed: %s status=%d %s", esp_err_to_name(res), status, net_detail);
    }
    free(resp_body);
    vTaskDelete(NULL);
}

void matrix_start_sync(void) {
    if (s_session == NULL || s_session->sync_should_run) return;
    s_session->sync_should_run = true;
    xTaskCreate(favorite_discovery_task, "matrix_favs", 12288, NULL, 4, NULL);
    xTaskCreate(sync_task, "matrix_sync", 16384, NULL, 5, NULL);
    xTaskCreate(room_metadata_task, "matrix_names", 8192, NULL, 4, NULL);
    xTaskCreate(sender_task, "matrix_send", 8192, NULL, 5, NULL);
}

void matrix_request_full_resync(void) {
    if (s_session == NULL) return;
    matrix_lock();
    s_session->next_batch[0] = '\0';
    s_session->dirty = true;
    matrix_unlock();
}

/* ------------------------------------------------------------------------ */
/* Public getters / lifecycle                                                */
/* ------------------------------------------------------------------------ */

esp_err_t matrix_client_init(void) {
    if (s_session != NULL) return ESP_OK;

    s_session = heap_caps_calloc(1, sizeof(matrix_session_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_session == NULL) {
        ESP_LOGE(TAG, "Failed to allocate matrix session in PSRAM");
        return ESP_ERR_NO_MEM;
    }
    s_session->mutex      = xSemaphoreCreateMutex();
    s_session->send_queue = xQueueCreate(8, sizeof(matrix_send_request_t));
    if (s_session->mutex == NULL || s_session->send_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_session->state = MATRIX_STATE_LOGGED_OUT;

    return ESP_OK;
}

void matrix_set_persistence_enabled(bool enabled) {
    s_persistence_enabled = enabled;
    if (!enabled) {
        matrix_clear_persisted_session();
    }
}

void matrix_clear_persisted_session(void) {
    nvs_handle_t nvs;
    if (nvs_open("mxsess", NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_erase_all(nvs);
    nvs_commit(nvs);
    nvs_close(nvs);
    s_last_persist_us = 0;
}

esp_err_t matrix_restore_session(void) {
    if (s_session == NULL || !s_persistence_enabled) return ESP_ERR_INVALID_STATE;

    nvs_handle_t nvs;
    if (nvs_open("mxsess", NVS_READONLY, &nvs) != ESP_OK) return ESP_ERR_NOT_FOUND;

    uint8_t valid = 0;
    if (nvs_get_u8(nvs, "valid", &valid) != ESP_OK || valid == 0) {
        nvs_close(nvs);
        return ESP_ERR_NOT_FOUND;
    }

    char homeserver[MATRIX_HOMESERVER_LEN] = "";
    char user_id[MATRIX_USER_ID_LEN]       = "";
    char access_token[MATRIX_TOKEN_LEN]    = "";
    char device_id[MATRIX_DEVICE_ID_LEN]   = "";
    char next_batch[128]                   = "";
    char filter_id[64]                     = "";
    size_t len;
    matrix_cached_room_t *cached = NULL;

    len = sizeof(homeserver);
    if (nvs_get_str(nvs, "hs", homeserver, &len) != ESP_OK) goto fail;
    len = sizeof(user_id);
    if (nvs_get_str(nvs, "uid", user_id, &len) != ESP_OK) goto fail;
    len = sizeof(access_token);
    if (nvs_get_str(nvs, "tok", access_token, &len) != ESP_OK) goto fail;
    len = sizeof(device_id);
    nvs_get_str(nvs, "dev", device_id, &len);
    len = sizeof(next_batch);
    nvs_get_str(nvs, "since", next_batch, &len);
    len = sizeof(filter_id);
    if (nvs_get_str(nvs, "filter", filter_id, &len) != ESP_OK) goto fail;

    if (homeserver[0] == '\0' || user_id[0] == '\0' || access_token[0] == '\0' || filter_id[0] == '\0') goto fail;

    cached = heap_caps_calloc(MATRIX_CACHE_ROOMS, sizeof(*cached), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (cached == NULL) goto fail;
    int32_t room_n = 0;
    nvs_get_i32(nvs, "room_n", &room_n);
    if (room_n < 0) room_n = 0;
    if (room_n > MATRIX_CACHE_ROOMS) room_n = MATRIX_CACHE_ROOMS;
    size_t blob_len = sizeof(cached[0]) * (size_t)room_n;
    if (room_n > 0 && nvs_get_blob(nvs, "rooms", cached, &blob_len) != ESP_OK) {
        room_n = 0;
    }
    nvs_close(nvs);

    matrix_lock();
    snprintf(s_session->homeserver, sizeof(s_session->homeserver), "%s", homeserver);
    snprintf(s_session->user_id, sizeof(s_session->user_id), "%s", user_id);
    snprintf(s_session->access_token, sizeof(s_session->access_token), "%s", access_token);
    snprintf(s_session->device_id, sizeof(s_session->device_id), "%s", device_id);
    snprintf(s_session->next_batch, sizeof(s_session->next_batch), "%s", next_batch);
    snprintf(s_session->filter_id, sizeof(s_session->filter_id), "%s", filter_id);
    s_session->room_count           = 0;
    s_session->pending_invite_count = 0;
    s_session->sync_count           = 0;
    s_session->last_sync_join_rooms = 0;
    s_session->last_sync_invite_rooms = 0;
    s_session->last_sync_leave_rooms  = 0;
    s_session->last_sync_events     = 0;
    s_session->last_sync_messages   = 0;
    s_session->total_messages       = 0;
    s_session->last_http_status     = 0;
    s_session->last_sync_us         = 0;
    for (int i = 0; i < room_n && s_session->room_count < MATRIX_MAX_ROOMS; i++) {
        if (cached[i].room_id[0] == '\0') continue;
        matrix_room_t *room = find_or_create_room(cached[i].room_id);
        if (room == NULL) break;
        snprintf(room->name, sizeof(room->name), "%s", cached[i].name);
        room->has_name         = cached[i].has_name != 0 && room->name[0] != '\0';
        room->encrypted        = cached[i].encrypted != 0;
        room->favorite         = cached[i].favorite != 0;
        room->last_activity_ts = cached[i].last_activity_ts;
    }
    sort_rooms_by_activity();
    s_session->state = MATRIX_STATE_LOGGED_IN;
    s_session->dirty = true;
    matrix_unlock();

    char filter_id_new[64];
    char filter_err[128];
    if (register_sync_filter(
            homeserver, user_id, access_token, MATRIX_SYNC_FILTER_JSON, filter_id_new, sizeof(filter_id_new),
            filter_err, sizeof(filter_err)
        ) == ESP_OK) {
        matrix_lock();
        snprintf(s_session->filter_id, sizeof(s_session->filter_id), "%s", filter_id_new);
        matrix_unlock();
    }

    free(cached);
    s_last_persist_us = esp_timer_get_time();
    return ESP_OK;

fail:
    nvs_close(nvs);
    if (cached != NULL) free(cached);
    return ESP_ERR_NOT_FOUND;
}

void matrix_logout(void) {
    if (s_session == NULL) return;
    s_session->sync_should_run = false;
    matrix_clear_persisted_session();

    char homeserver_copy[MATRIX_HOMESERVER_LEN];
    char token_copy[MATRIX_TOKEN_LEN];
    matrix_lock();
    snprintf(homeserver_copy, sizeof(homeserver_copy), "%s", s_session->homeserver);
    snprintf(token_copy, sizeof(token_copy), "%s", s_session->access_token);
    matrix_unlock();

    if (homeserver_copy[0] != '\0' && token_copy[0] != '\0') {
        char url[MATRIX_HOMESERVER_LEN + 64];
        snprintf(url, sizeof(url), "%s/_matrix/client/v3/logout", homeserver_copy);
        char *resp_body = NULL;
        int   status    = 0;
        matrix_http_request("POST", url, token_copy, "{}", &resp_body, &status, NULL, 0);
        free(resp_body);
    }

    matrix_lock();
    memset(s_session->homeserver, 0, sizeof(s_session->homeserver));
    memset(s_session->access_token, 0, sizeof(s_session->access_token));
    memset(s_session->user_id, 0, sizeof(s_session->user_id));
    memset(s_session->device_id, 0, sizeof(s_session->device_id));
    memset(s_session->next_batch, 0, sizeof(s_session->next_batch));
    memset(s_session->filter_id, 0, sizeof(s_session->filter_id));
    matrix_clear_image_cache_locked();
    s_session->room_count          = 0;
    s_session->pending_invite_count = 0;
    s_session->sync_count          = 0;
    s_session->last_sync_join_rooms   = 0;
    s_session->last_sync_invite_rooms = 0;
    s_session->last_sync_leave_rooms  = 0;
    s_session->last_sync_events    = 0;
    s_session->last_sync_messages  = 0;
    s_session->total_messages      = 0;
    s_session->last_http_status    = 0;
    s_session->last_sync_us        = 0;
    s_session->state               = MATRIX_STATE_LOGGED_OUT;
    matrix_unlock();
}

matrix_state_t matrix_get_state(void) {
    return s_session ? s_session->state : MATRIX_STATE_LOGGED_OUT;
}

bool matrix_consume_dirty(void) {
    if (s_session == NULL) return false;
    bool was_dirty   = s_session->dirty;
    s_session->dirty = false;
    return was_dirty;
}

void matrix_lock(void) {
    if (s_session) xSemaphoreTake(s_session->mutex, portMAX_DELAY);
}

void matrix_unlock(void) {
    if (s_session) xSemaphoreGive(s_session->mutex);
}

int matrix_room_count(void) {
    return s_session ? s_session->room_count : 0;
}

matrix_room_t *matrix_get_room(int index) {
    if (s_session == NULL || index < 0 || index >= s_session->room_count) return NULL;
    return &s_session->rooms[index];
}

int matrix_find_room_index_by_id(const char *room_id) {
    if (s_session == NULL || room_id == NULL || room_id[0] == '\0') return -1;
    for (int i = 0; i < s_session->room_count; i++) {
        if (strcmp(s_session->rooms[i].room_id, room_id) == 0) {
            return i;
        }
    }
    return -1;
}

int matrix_active_history_count(const char *room_id) {
    if (s_session == NULL || room_id == NULL || strcmp(s_session->active_history_room_id, room_id) != 0) return 0;
    return s_session->active_history_count;
}

matrix_message_t *matrix_get_active_history_message(const char *room_id, int index) {
    if (s_session == NULL || room_id == NULL || strcmp(s_session->active_history_room_id, room_id) != 0) return NULL;
    if (index < 0 || index >= s_session->active_history_count) return NULL;
    return &s_session->active_history[index];
}

const char *matrix_get_audio_status(const char *event_id) {
    if (s_session == NULL || event_id == NULL || event_id[0] == '\0') return NULL;
    for (int i = 0; i < (int)(sizeof(s_session->audio_status) / sizeof(s_session->audio_status[0])); i++) {
        if (strcmp(s_session->audio_status[i].event_id, event_id) == 0) {
            return s_session->audio_status[i].text[0] != '\0' ? s_session->audio_status[i].text : NULL;
        }
    }
    return NULL;
}

esp_err_t matrix_copy_cached_image(
    const char *event_id, uint8_t **out_data, size_t *out_len, char *mimetype_out, size_t mimetype_out_len,
    char *label_out, size_t label_out_len
) {
    if (out_data != NULL) *out_data = NULL;
    if (out_len != NULL) *out_len = 0;
    if (event_id == NULL || out_data == NULL || out_len == NULL) return ESP_ERR_INVALID_ARG;
    matrix_lock();
    if (s_session == NULL || strcmp(s_session->image_cache.event_id, event_id) != 0 || s_session->image_cache.data == NULL ||
        s_session->image_cache.len == 0) {
        matrix_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t *copy = heap_caps_malloc(s_session->image_cache.len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (copy == NULL) copy = heap_caps_malloc(s_session->image_cache.len, MALLOC_CAP_8BIT);
    if (copy == NULL) {
        matrix_unlock();
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, s_session->image_cache.data, s_session->image_cache.len);
    *out_data = copy;
    *out_len = s_session->image_cache.len;
    if (mimetype_out != NULL && mimetype_out_len > 0) {
        snprintf(mimetype_out, mimetype_out_len, "%s", s_session->image_cache.mimetype);
    }
    if (label_out != NULL && label_out_len > 0) {
        snprintf(label_out, label_out_len, "%s", s_session->image_cache.label);
    }
    matrix_unlock();
    return ESP_OK;
}

const char *matrix_get_user_id(void) {
    return s_session ? s_session->user_id : "";
}

const char *matrix_get_last_error(void) {
    return s_session ? s_session->last_error : "";
}

void matrix_get_sync_stats(matrix_sync_stats_t *out_stats) {
    if (out_stats == NULL) return;
    memset(out_stats, 0, sizeof(*out_stats));
    out_stats->last_sync_age_ms = -1;
    if (s_session == NULL) return;

    matrix_lock();
    uint32_t named_rooms = 0;
    for (int i = 0; i < s_session->room_count; i++) {
        if (s_session->rooms[i].has_name) named_rooms++;
    }
    out_stats->sync_count         = s_session->sync_count;
    out_stats->joined_rooms            = s_session->room_count;
    out_stats->named_rooms             = named_rooms;
    out_stats->last_sync_join_rooms    = s_session->last_sync_join_rooms;
    out_stats->last_sync_invite_rooms  = s_session->last_sync_invite_rooms;
    out_stats->last_sync_leave_rooms   = s_session->last_sync_leave_rooms;
    out_stats->last_sync_events        = s_session->last_sync_events;
    out_stats->last_sync_messages      = s_session->last_sync_messages;
    out_stats->total_messages          = s_session->total_messages;
    out_stats->last_http_status        = s_session->last_http_status;
    if (s_session->last_sync_us > 0) {
        out_stats->last_sync_age_ms = (esp_timer_get_time() - s_session->last_sync_us) / 1000;
    }
    matrix_unlock();
}

esp_err_t matrix_send_message(int room_index, const char *body) {
    if (s_session == NULL || s_session->send_queue == NULL) return ESP_ERR_INVALID_STATE;
    if (body == NULL || body[0] == '\0') return ESP_ERR_INVALID_ARG;

    matrix_send_request_t req = {0};

    matrix_lock();
    if (room_index < 0 || room_index >= s_session->room_count) {
        matrix_unlock();
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(req.room_id, sizeof(req.room_id), "%s", s_session->rooms[room_index].room_id);
    snprintf(req.body, sizeof(req.body), "%s", body);
    int64_t optimistic_ts = 1;
    for (int i = 0; i < s_session->room_count; i++) {
        if (s_session->rooms[i].last_activity_ts >= optimistic_ts) {
            optimistic_ts = s_session->rooms[i].last_activity_ts + 1;
        }
    }
    // Optimistically show the message right away instead of waiting for it to come
    // back through the next /sync response.
    append_message(&s_session->rooms[room_index], NULL, s_session->user_id, body, optimistic_ts, false);
    sort_rooms_by_activity();
    s_session->dirty = true;
    matrix_unlock();

    if (xQueueSend(s_session->send_queue, &req, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t matrix_fetch_room_history(int room_index) {
    if (s_session == NULL) return ESP_ERR_INVALID_STATE;

    matrix_history_request_t *req = heap_caps_calloc(1, sizeof(*req), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (req == NULL) return ESP_ERR_NO_MEM;

    matrix_lock();
    if (room_index < 0 || room_index >= s_session->room_count) {
        matrix_unlock();
        free(req);
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(req->room_id, sizeof(req->room_id), "%s", s_session->rooms[room_index].room_id);
    snprintf(s_session->active_history_room_id, sizeof(s_session->active_history_room_id), "%s", req->room_id);
    snprintf(s_session->active_members_room_id, sizeof(s_session->active_members_room_id), "%s", req->room_id);
    s_session->active_history_count = 0;
    s_session->active_member_count = 0;
    for (int i = 0; i < s_session->rooms[room_index].message_count; i++) {
        s_session->active_history[s_session->active_history_count++] = s_session->rooms[room_index].messages[i];
        if (s_session->active_history_count >= MATRIX_ACTIVE_HISTORY_MESSAGES) break;
    }
    s_session->dirty = true;
    matrix_unlock();

    matrix_load_history_cache(req->room_id);

    BaseType_t ok = xTaskCreate(history_task, "matrix_history", 12288, req, 4, NULL);
    if (ok != pdPASS) {
        free(req);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t matrix_request_media_download(const char *room_id, const char *event_id) {
    if (s_session == NULL || room_id == NULL || room_id[0] == '\0' || event_id == NULL || event_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    matrix_media_request_t *req = heap_caps_calloc(1, sizeof(*req), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (req == NULL) return ESP_ERR_NO_MEM;
    snprintf(req->room_id, sizeof(req->room_id), "%s", room_id);
    snprintf(req->event_id, sizeof(req->event_id), "%s", event_id);
    matrix_set_audio_status(event_id, "queued");

    /* 48KB: OpenH264's decode call chain needs a lot more stack than the old
     * tinyh264 decoder did (confirmed via a standalone probe needing well
     * under 64KB but comfortably over the previous 12KB here). */
    BaseType_t ok = xTaskCreate(media_download_task, "matrix_media", 49152, req, 4, NULL);
    if (ok != pdPASS) {
        matrix_set_audio_status(event_id, "task failed");
        free(req);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t matrix_request_audio_download(const char *room_id, const char *event_id) {
    return matrix_request_media_download(room_id, event_id);
}
