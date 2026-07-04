#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MATRIX_MAX_ROOMS       768
#define MATRIX_MAX_MESSAGES    4
#define MATRIX_ACTIVE_HISTORY_MESSAGES 96
#define MATRIX_HISTORY_FETCH_LIMIT 64
#define MATRIX_ACTIVE_MEMBERS  96
#define MATRIX_HOMESERVER_LEN  160
#define MATRIX_USER_ID_LEN     140
#define MATRIX_TOKEN_LEN       512
#define MATRIX_DEVICE_ID_LEN   64
#define MATRIX_ROOM_ID_LEN     140
#define MATRIX_ROOM_NAME_LEN   96
#define MATRIX_SENDER_LEN      140
#define MATRIX_EVENT_ID_LEN    140
#define MATRIX_BODY_LEN        384
#define MATRIX_ERROR_LEN       160
#define MATRIX_MAX_MEMBERS     6
#define MATRIX_MAX_REACTIONS   4
#define MATRIX_REACTION_KEY_LEN 24
#define MATRIX_AUDIO_STATUS_LEN 48

typedef struct {
    char    key[MATRIX_REACTION_KEY_LEN];
    uint8_t count;
} matrix_reaction_t;

typedef struct {
    char    sender[MATRIX_SENDER_LEN];
    char    sender_display[MATRIX_ROOM_NAME_LEN];
    char    event_id[MATRIX_EVENT_ID_LEN];
    char    body[MATRIX_BODY_LEN];
    matrix_reaction_t reactions[MATRIX_MAX_REACTIONS];
    int64_t origin_server_ts;
    bool    is_notice;
} matrix_message_t;

typedef struct {
    char user_id[MATRIX_SENDER_LEN];
    char display_name[MATRIX_ROOM_NAME_LEN];
} matrix_member_t;

typedef struct {
    char             room_id[MATRIX_ROOM_ID_LEN];
    char             name[MATRIX_ROOM_NAME_LEN];
    bool             has_name;
    bool             encrypted;
    bool             favorite;
    matrix_member_t  members[MATRIX_MAX_MEMBERS];
    int              member_count;
    matrix_message_t messages[MATRIX_MAX_MESSAGES];
    int              message_count;
    int64_t          last_activity_ts;
    bool             unread;
    uint16_t         unread_count;
} matrix_room_t;

typedef enum {
    MATRIX_STATE_LOGGED_OUT = 0,
    MATRIX_STATE_LOGGING_IN,
    MATRIX_STATE_LOGGED_IN,
    MATRIX_STATE_SYNCING,
    MATRIX_STATE_ERROR,
} matrix_state_t;

typedef struct {
    uint32_t sync_count;
    uint32_t joined_rooms;
    uint32_t named_rooms;
    uint32_t last_sync_join_rooms;
    uint32_t last_sync_invite_rooms;
    uint32_t last_sync_leave_rooms;
    uint32_t last_sync_events;
    uint32_t last_sync_messages;
    uint32_t total_messages;
    int      last_http_status;
    int64_t  last_sync_age_ms;
} matrix_sync_stats_t;

// Must be called once before any other matrix_* function.
esp_err_t matrix_client_init(void);

// Persistence stores access tokens and cached room metadata. Enable it only
// when the user opted into saving credentials.
void      matrix_set_persistence_enabled(bool enabled);
esp_err_t matrix_restore_session(void);
void      matrix_clear_persisted_session(void);

// Attempt to discover & log in to a homeserver. `homeserver_input` may be a bare
// server name (e.g. "matrix.org") or a full URL. Blocks until finished (performs
// network I/O), intended to be called from a dedicated task, not the UI task.
esp_err_t matrix_login(
    const char *homeserver_input, const char *username, const char *password, char *err_out, size_t err_out_len
);

// Starts the background /sync long-poll task. Only valid after a successful matrix_login().
void matrix_start_sync(void);
void matrix_request_full_resync(void);

// Clears the in-memory session (best-effort server-side logout too).
void matrix_logout(void);

matrix_state_t matrix_get_state(void);

// Returns true and clears the flag if new data arrived since the last call.
bool matrix_consume_dirty(void);

// Lock must be held while reading room/message data returned below.
void matrix_lock(void);
void matrix_unlock(void);

int            matrix_room_count(void);
matrix_room_t *matrix_get_room(int index);
int            matrix_find_room_index_by_id(const char *room_id);
int            matrix_active_history_count(const char *room_id);
matrix_message_t *matrix_get_active_history_message(const char *room_id, int index);
const char       *matrix_get_audio_status(const char *event_id);

const char *matrix_get_user_id(void);
const char *matrix_get_last_error(void);
void        matrix_get_sync_stats(matrix_sync_stats_t *out_stats);

// Queues a message to be sent asynchronously from the sender task.
esp_err_t matrix_send_message(int room_index, const char *body);
esp_err_t matrix_fetch_room_history(int room_index);
esp_err_t matrix_request_audio_download(const char *room_id, const char *event_id);

#ifdef __cplusplus
}
#endif
