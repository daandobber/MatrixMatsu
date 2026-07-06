#include <stdio.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "bsp/power.h"
#include "custom_certificates.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_types.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/task.h"
#include "hal/lcd_types.h"
#include "audio_player.h"
#include "image_viewer.h"
#include "matrix_client.h"
#include "video_player.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"
#include "pax_types.h"
#include "portmacro.h"
#include "sdmmc_cmd.h"
#include "wifi_connection.h"
#include "wifi_remote.h"

static char const TAG[] = "main";

// Terminal color palette (ARGB8888), selected at runtime.
static pax_col_t g_col_bg        = 0xFF000000;
static pax_col_t g_col_fg        = 0xFFC8C8C8;
static pax_col_t g_col_dim       = 0xFF707070;
static pax_col_t g_col_accent    = 0xFF33FF66;
static pax_col_t g_col_error     = 0xFFFF5555;
static pax_col_t g_col_mine      = 0xFF55CCFF;
static pax_col_t g_col_select_bg = 0xFF103318;
static float     g_font_size     = 16.0f;

#define BLACK          g_col_bg
#define TERM_FG        g_col_fg
#define TERM_DIM       g_col_dim
#define TERM_GREEN     g_col_accent
#define TERM_RED       g_col_error
#define TERM_CYAN      g_col_mine
#define TERM_SELECT_BG g_col_select_bg
#define FONT_SIZE      g_font_size
#define TITLE_FONT_SIZE (g_font_size + 4.0f)

#define WRAP_LINE_LEN        96
#define MAX_WRAP_LINES_TOTAL 400
#define EMOJI_EXT_MARKER     31
#define EMOJI_FLAG_PAIR_MARKER 116

// Global display / input state
static size_t                     display_h_res       = 0;
static size_t                     display_v_res       = 0;
static bsp_display_color_format_t display_color_format = 0;
static bsp_display_endianness_t   display_data_endian  = 0;
static pax_buf_t                  fb                   = {0};
static QueueHandle_t              input_event_queue    = NULL;
static float                      g_char_w             = 9.0f;
static float                      g_line_h             = 20.0f;

// Scratch buffers used only by render_chat(), allocated once from PSRAM.
static char      (*g_all_lines)[WRAP_LINE_LEN] = NULL;
static pax_col_t *g_line_colors                = NULL;
static pax_col_t *g_line_name_colors           = NULL;
static uint8_t   *g_line_name_lens             = NULL;
static int16_t   *g_line_message_indices       = NULL;

typedef enum {
    APP_SCREEN_LOGIN = 0,
    APP_SCREEN_ROOM_LIST,
    APP_SCREEN_CHAT,
    APP_SCREEN_IMAGE_VIEWER,
    APP_SCREEN_VIDEO_PLAYER,
    APP_SCREEN_EMOJI_PICKER,
    APP_SCREEN_MENU,
    APP_SCREEN_SETTINGS,
} app_screen_t;

typedef enum {
    LOGIN_FIELD_HOMESERVER = 0,
    LOGIN_FIELD_USERNAME,
    LOGIN_FIELD_PASSWORD,
    LOGIN_FIELD_COUNT,
} login_field_t;

static app_screen_t screen = APP_SCREEN_LOGIN;
static app_screen_t menu_return_screen = APP_SCREEN_LOGIN;

// Login screen state
static char          login_homeserver[128] = "matrix.org";
static char          login_username[128]   = "";
static char          login_password[128]   = "";
static login_field_t login_focus           = LOGIN_FIELD_HOMESERVER;
static char          login_error[160]      = "";
static volatile bool login_in_progress     = false;
static bool          remember_account      = true;
static bool          remember_password     = false;

// Room list state
static int room_selected = 0;
static int room_scroll   = 0;
static bool room_list_show_all = false;
static bool debug_status = false;

// Chat state
static int  chat_room_index     = -1;
static char chat_room_id[MATRIX_ROOM_ID_LEN] = "";
static char compose_buffer[400] = "";
static int  chat_scroll_offset  = 0;
static int  chat_selected_message = -1;
static int  emoji_selected      = 0;
static int  emoji_scroll        = 0;
static pax_buf_t viewed_image = {0};
static bool viewed_image_valid = false;
static char viewed_image_label[MATRIX_BODY_LEN] = "";
static char viewed_image_error[96] = "";
static char pending_image_event_id[MATRIX_EVENT_ID_LEN] = "";
static volatile bool image_decode_busy = false;
static volatile bool image_decode_ready = false;
static char active_video_event_id[MATRIX_EVENT_ID_LEN] = "";
static bool video_screen_cleared = false;

typedef struct {
    uint8_t *data;
    size_t   data_len;
    char     mimetype[MATRIX_MEDIA_MIMETYPE_LEN];
    char     label[MATRIX_BODY_LEN];
} image_decode_request_t;

// Menu/settings state
static int menu_selected     = 0;
static int settings_selected = 0;
static int theme_index       = 0;
static int font_size_index   = 1;
static char audio_volume_status[MATRIX_AUDIO_STATUS_LEN] = "";

/* -------------------------------------------------------------------------- */
/* Forward declarations                                                        */
/* -------------------------------------------------------------------------- */

static void blit(void);
static void measure_font(void);
static void draw_box(float x, float y, float w, float h, pax_col_t color, const char *title);
static const char *matrix_state_text(matrix_state_t state);
static void draw_matrix_status(float x, float y);
static int  wrap_text(const char *text, int max_cols, char lines[][WRAP_LINE_LEN], int max_lines);
static void load_saved_account(void);
static void save_account_settings(void);
static void clear_saved_account(void);
static void apply_theme(void);
static const char *theme_name(void);

static char  *login_field_buffer(login_field_t field);
static size_t login_field_capacity(login_field_t field);
static void   start_login(void);

static bool handle_global_hotkeys(bsp_input_event_t *event);
static bool handle_input_login(bsp_input_event_t *event);
static bool handle_input_room_list(bsp_input_event_t *event);
static bool handle_input_chat(bsp_input_event_t *event);
static bool handle_input_image_viewer(bsp_input_event_t *event);
static bool handle_input_video_player(bsp_input_event_t *event);
static bool handle_input_emoji_picker(bsp_input_event_t *event);
static bool handle_input_menu(bsp_input_event_t *event);
static bool handle_input_settings(bsp_input_event_t *event);
static void send_current_message(void);

static void render(void);
static void render_login(void);
static void render_room_list(void);
static void render_chat_frame(bool do_blit);
static void render_chat(void);
static void render_chat_input_only(void);
static void render_image_viewer(void);
static void render_video_player(void);
static void render_emoji_picker(void);
static void render_menu(void);
static void render_settings(void);

/* -------------------------------------------------------------------------- */
/* Small helpers                                                               */
/* -------------------------------------------------------------------------- */

static void blit(void) {
    esp_err_t res = bsp_display_blit(0, 0, display_h_res, display_v_res, pax_buf_get_pixels(&fb));
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to blit to display: %d", res);
    }
}

static void measure_font(void) {
    pax_vec2f size = pax_text_size(pax_font_sky_mono, FONT_SIZE, "M");
    if (size.x > 0) g_char_w = size.x;
    if (size.y > 0) {
        float text_line_h  = size.y + 6.0f;
        float emoji_line_h = FONT_SIZE * 1.70f;
        g_line_h = text_line_h > emoji_line_h ? text_line_h : emoji_line_h;
    }
}

static const char *theme_name(void) {
    switch (theme_index) {
        case 1: return "amber";
        case 2: return "blue";
        case 3: return "magenta";
        case 4: return "cyan";
        case 5: return "mono";
        case 6: return "solar";
        case 7: return "matrix";
        case 8: return "rose";
        case 9: return "violet";
        case 10: return "lime";
        case 11: return "ice";
        case 12: return "warning";
        case 13: return "mint";
        case 14: return "white";
        case 15: return "red";
        default: return "green";
    }
}

static void apply_theme(void) {
    if (theme_index < 0) theme_index = 0;
    if (theme_index > 15) theme_index = 0;
    switch (theme_index) {
        case 1:
            g_col_bg        = 0xFF050403;
            g_col_fg        = 0xFFE8D6B0;
            g_col_dim       = 0xFF8A7860;
            g_col_accent    = 0xFFFFB000;
            g_col_error     = 0xFFFF5555;
            g_col_mine      = 0xFFFFD06A;
            g_col_select_bg = 0xFF332400;
            break;
        case 2:
            g_col_bg        = 0xFF02070D;
            g_col_fg        = 0xFFC8D8E8;
            g_col_dim       = 0xFF6C7A88;
            g_col_accent    = 0xFF55CCFF;
            g_col_error     = 0xFFFF6060;
            g_col_mine      = 0xFF33FFCC;
            g_col_select_bg = 0xFF06283A;
            break;
        case 3:
            g_col_bg = 0xFF08030A; g_col_fg = 0xFFE8C8E8; g_col_dim = 0xFF806080; g_col_accent = 0xFFFF55CC; g_col_error = 0xFFFF5555; g_col_mine = 0xFFFF99DD; g_col_select_bg = 0xFF331026;
            break;
        case 4:
            g_col_bg = 0xFF001010; g_col_fg = 0xFFC8E8E8; g_col_dim = 0xFF608080; g_col_accent = 0xFF33FFEE; g_col_error = 0xFFFF6655; g_col_mine = 0xFF99FFF0; g_col_select_bg = 0xFF003333;
            break;
        case 5:
            g_col_bg = 0xFF000000; g_col_fg = 0xFFE0E0E0; g_col_dim = 0xFF808080; g_col_accent = 0xFFFFFFFF; g_col_error = 0xFFFF5555; g_col_mine = 0xFFFFFFFF; g_col_select_bg = 0xFF303030;
            break;
        case 6:
            g_col_bg = 0xFF001B22; g_col_fg = 0xFFEEE8D5; g_col_dim = 0xFF839496; g_col_accent = 0xFFB58900; g_col_error = 0xFFDC322F; g_col_mine = 0xFF2AA198; g_col_select_bg = 0xFF073642;
            break;
        case 7:
            g_col_bg = 0xFF000800; g_col_fg = 0xFFB8FFB8; g_col_dim = 0xFF408040; g_col_accent = 0xFF00FF41; g_col_error = 0xFFFF4040; g_col_mine = 0xFF80FF80; g_col_select_bg = 0xFF003010;
            break;
        case 8:
            g_col_bg = 0xFF120608; g_col_fg = 0xFFF2CCD2; g_col_dim = 0xFF9A6A70; g_col_accent = 0xFFFF6F91; g_col_error = 0xFFFF4040; g_col_mine = 0xFFFFB0C0; g_col_select_bg = 0xFF3A121A;
            break;
        case 9:
            g_col_bg = 0xFF090414; g_col_fg = 0xFFD8CCF2; g_col_dim = 0xFF7A6A9A; g_col_accent = 0xFFB388FF; g_col_error = 0xFFFF6060; g_col_mine = 0xFFD0B0FF; g_col_select_bg = 0xFF221040;
            break;
        case 10:
            g_col_bg = 0xFF071000; g_col_fg = 0xFFD8F0C8; g_col_dim = 0xFF709060; g_col_accent = 0xFFA3FF12; g_col_error = 0xFFFF5555; g_col_mine = 0xFFD0FF66; g_col_select_bg = 0xFF223300;
            break;
        case 11:
            g_col_bg = 0xFF031018; g_col_fg = 0xFFD8F2FF; g_col_dim = 0xFF6A8794; g_col_accent = 0xFF91E5FF; g_col_error = 0xFFFF6464; g_col_mine = 0xFFC0F4FF; g_col_select_bg = 0xFF102C38;
            break;
        case 12:
            g_col_bg = 0xFF100800; g_col_fg = 0xFFFFE0B0; g_col_dim = 0xFFAA8050; g_col_accent = 0xFFFF7A00; g_col_error = 0xFFFF3030; g_col_mine = 0xFFFFBF4D; g_col_select_bg = 0xFF3A1B00;
            break;
        case 13:
            g_col_bg = 0xFF03110D; g_col_fg = 0xFFCFF2E6; g_col_dim = 0xFF679080; g_col_accent = 0xFF5CFFB0; g_col_error = 0xFFFF6060; g_col_mine = 0xFFA0FFD2; g_col_select_bg = 0xFF0B3024;
            break;
        case 14:
            g_col_bg = 0xFFF4F4F4; g_col_fg = 0xFF101010; g_col_dim = 0xFF666666; g_col_accent = 0xFF0066CC; g_col_error = 0xFFCC0000; g_col_mine = 0xFF005A8A; g_col_select_bg = 0xFFD8E8FF;
            break;
        case 15:
            g_col_bg = 0xFF100000; g_col_fg = 0xFFFFD0D0; g_col_dim = 0xFF996060; g_col_accent = 0xFFFF4040; g_col_error = 0xFFFFA000; g_col_mine = 0xFFFF8080; g_col_select_bg = 0xFF3A0000;
            break;
        default:
            g_col_bg        = 0xFF000000;
            g_col_fg        = 0xFFC8C8C8;
            g_col_dim       = 0xFF707070;
            g_col_accent    = 0xFF33FF66;
            g_col_error     = 0xFFFF5555;
            g_col_mine      = 0xFF55CCFF;
            g_col_select_bg = 0xFF103318;
            break;
    }

    static const float sizes[] = {14.0f, 16.0f, 18.0f, 20.0f};
    if (font_size_index < 0) font_size_index = 0;
    if (font_size_index >= (int)(sizeof(sizes) / sizeof(sizes[0]))) font_size_index = 1;
    g_font_size = sizes[font_size_index];
    measure_font();
}

static void draw_box(float x, float y, float w, float h, pax_col_t color, const char *title) {
    pax_outline_rect(&fb, color, x, y, w, h);
    if (title != NULL && title[0] != '\0') {
        char labeled[80];
        snprintf(labeled, sizeof(labeled), " %s ", title);
        float label_w = g_char_w * (float)strlen(labeled);
        pax_simple_rect(&fb, BLACK, x + 10, y - g_line_h / 2, label_w, g_line_h);
        pax_draw_text(&fb, color, pax_font_sky_mono, FONT_SIZE, x + 10, y - g_line_h / 2, labeled);
    }
}

static const char *matrix_state_text(matrix_state_t state) {
    switch (state) {
        case MATRIX_STATE_LOGGED_OUT: return "logged out";
        case MATRIX_STATE_LOGGING_IN: return "logging in";
        case MATRIX_STATE_LOGGED_IN: return "online";
        case MATRIX_STATE_SYNCING: return "sync wait";
        case MATRIX_STATE_ERROR: return "error";
        default: return "unknown";
    }
}

static void draw_matrix_status(float x, float y) {
    const char *err = matrix_get_last_error();
    if (err != NULL && err[0] != '\0') {
        char line[192];
        snprintf(line, sizeof(line), "M err: %s", err);
        pax_draw_text(&fb, TERM_RED, pax_font_sky_mono, FONT_SIZE, x, y, line);
    } else {
        matrix_sync_stats_t stats;
        matrix_get_sync_stats(&stats);
        int age_s = stats.last_sync_age_ms >= 0 ? (int)(stats.last_sync_age_ms / 1000) : -1;
        char line[160];
        snprintf(
            line, sizeof(line), "M: %s http=%d sync=%" PRIu32 " rooms=%" PRIu32 " j=%" PRIu32 " i=%" PRIu32
                                " l=%" PRIu32 " ev=%" PRIu32 " msg=%" PRIu32 " age=%ds",
            matrix_state_text(matrix_get_state()), stats.last_http_status, stats.sync_count, stats.joined_rooms,
            stats.last_sync_join_rooms, stats.last_sync_invite_rooms, stats.last_sync_leave_rooms,
            stats.last_sync_events, stats.last_sync_messages, age_s
        );
        pax_draw_text(&fb, TERM_DIM, pax_font_sky_mono, FONT_SIZE, x, y, line);
    }
}

static pax_col_t sender_name_color(const char *sender, bool mine, bool notice) {
    if (notice) return TERM_DIM;
    if (mine) return TERM_CYAN;

    static const pax_col_t palette[] = {
        0xFFFFB000, 0xFF55CCFF, 0xFFFF6F91, 0xFFA3FF12,
        0xFFB388FF, 0xFF5CFFB0, 0xFFFF7A00, 0xFF91E5FF,
        0xFFFFD43B, 0xFFFF99DD, 0xFF33FFCC, 0xFFFF8080,
    };

    uint32_t hash = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)sender; p != NULL && *p != '\0'; p++) {
        hash ^= *p;
        hash *= 16777619u;
    }
    return palette[hash % (sizeof(palette) / sizeof(palette[0]))];
}

// Greedy word wrap into caller-provided fixed-width line buffers.
static int wrap_text(const char *text, int max_cols, char lines[][WRAP_LINE_LEN], int max_lines) {
    if (max_cols < 1) max_cols = 1;
    if (max_cols > WRAP_LINE_LEN - 1) max_cols = WRAP_LINE_LEN - 1;

    int         line_count = 0;
    const char *p          = text;

    while (*p != '\0' && line_count < max_lines) {
        const char *line_start = p;
        const char *last_space = NULL;
        const char *cursor     = p;
        int         col        = 0;

        while (*cursor != '\0' && *cursor != '\n' && col < max_cols) {
            if (*cursor == ' ') {
                last_space = cursor;
            }
            if ((unsigned char)*cursor == EMOJI_EXT_MARKER && cursor[1] != '\0') {
                if (col + 1 > max_cols) break;
                if ((unsigned char)cursor[1] == EMOJI_FLAG_PAIR_MARKER && cursor[2] != '\0' && cursor[3] != '\0') {
                    cursor += 4;
                } else {
                    cursor += 2;
                }
            } else {
                cursor++;
            }
            col++;
        }

        int seg_len;
        if (*cursor == '\0' || *cursor == '\n') {
            seg_len = (int)(cursor - line_start);
            memcpy(lines[line_count], line_start, seg_len);
            lines[line_count][seg_len] = '\0';
            line_count++;
            p = (*cursor == '\n') ? cursor + 1 : cursor;
        } else if (last_space != NULL) {
            seg_len = (int)(last_space - line_start);
            memcpy(lines[line_count], line_start, seg_len);
            lines[line_count][seg_len] = '\0';
            line_count++;
            p = last_space + 1;
        } else {
            seg_len = (int)(cursor - line_start);
            memcpy(lines[line_count], line_start, seg_len);
            lines[line_count][seg_len] = '\0';
            line_count++;
            p = cursor;
        }
    }

    if (*p != '\0' && line_count == max_lines && max_lines > 0) {
        char *last = lines[max_lines - 1];
        int   keep = max_cols - 3;
        if (keep < 0) keep = 0;
        if ((int)strlen(last) > keep) last[keep] = '\0';
        strcat(last, "...");
    }

    return line_count;
}

#define EMOJI_GENERIC     1
#define EMOJI_SMILE       2
#define EMOJI_GRIN        3
#define EMOJI_JOY         4
#define EMOJI_HEART_EYES  5
#define EMOJI_COOL        6
#define EMOJI_CRY         7
#define EMOJI_THINKING    8
#define EMOJI_THUMBS_UP   9
#define EMOJI_THUMBS_DOWN 10
#define EMOJI_WAVE        11
#define EMOJI_PRAY        12
#define EMOJI_HEART       13
#define EMOJI_FIRE        14
#define EMOJI_PARTY       15
#define EMOJI_ROCKET      16
#define EMOJI_STAR        17
#define EMOJI_CHECK       18
#define EMOJI_CROSS       19
#define EMOJI_HUNDRED     20
#define EMOJI_EYES        21
#define EMOJI_ANGRY       22
#define EMOJI_SKULL       23
#define EMOJI_SUN         24
#define EMOJI_MOON        25
#define EMOJI_BOLT        26
#define EMOJI_GIFT        27
#define EMOJI_MUSIC       28
#define EMOJI_PIZZA       29
#define EMOJI_COFFEE      30
#define EMOJI_EXT         EMOJI_EXT_MARKER
#define EMOJI_WINK        32
#define EMOJI_BLUSH       33
#define EMOJI_KISS        34
#define EMOJI_HEART_HANDS 35
#define EMOJI_STRONG      36
#define EMOJI_CLAP        37
#define EMOJI_SOB         38
#define EMOJI_ROFL        39
#define EMOJI_MELTING     40
#define EMOJI_PLEADING    41
#define EMOJI_NEUTRAL     42
#define EMOJI_UNAMUSED    43
#define EMOJI_SWEAT_SMILE 44
#define EMOJI_RELIEVED    45
#define EMOJI_SLEEPING    46
#define EMOJI_FLUSHED     47
#define EMOJI_SCREAM      48
#define EMOJI_MIND_BLOWN  49
#define EMOJI_SMIRK       50
#define EMOJI_ZANY        51
#define EMOJI_PARTY_FACE  52
#define EMOJI_HUGGING     53
#define EMOJI_SHUSHING    54
#define EMOJI_FACEPALM    55
#define EMOJI_SHRUG       56
#define EMOJI_OK_HAND     57
#define EMOJI_RAISED_HANDS 58
#define EMOJI_POINT_RIGHT 59
#define EMOJI_POINT_LEFT  60
#define EMOJI_POINT_UP    61
#define EMOJI_POINT_DOWN  62
#define EMOJI_HANDSHAKE   63
#define EMOJI_ORANGE_HEART 64
#define EMOJI_YELLOW_HEART 65
#define EMOJI_GREEN_HEART 66
#define EMOJI_BLUE_HEART  67
#define EMOJI_PURPLE_HEART 68
#define EMOJI_BLACK_HEART 69
#define EMOJI_BROKEN_HEART 70
#define EMOJI_SPARKLES    71
#define EMOJI_POOP        72
#define EMOJI_BOOM        73
#define EMOJI_DROPS       74
#define EMOJI_ZZZ         75
#define EMOJI_DASH        76
#define EMOJI_MONKEY_SEE  77
#define EMOJI_CAT_SMILE   78
#define EMOJI_DOG         79
#define EMOJI_CAT         80
#define EMOJI_BEER        81
#define EMOJI_WINE        82
#define EMOJI_BURGER      83
#define EMOJI_FRIES       84
#define EMOJI_CAKE        85
#define EMOJI_SOCCER      86
#define EMOJI_GAME        87
#define EMOJI_PHONE       88
#define EMOJI_LAPTOP      89
#define EMOJI_BULB        90
#define EMOJI_MONEY       91
#define EMOJI_GEM         92
#define EMOJI_WARNING     93
#define EMOJI_QUESTION    94
#define EMOJI_EXCLAMATION 95
#define EMOJI_CALENDAR    96
#define EMOJI_CLOCK       97
#define EMOJI_HOME        98
#define EMOJI_CAR         99
#define EMOJI_TRAIN       100
#define EMOJI_AIRPLANE    101
#define EMOJI_GLOBE       102
#define EMOJI_FLAG_NL     103
#define EMOJI_RAINBOW     104
#define EMOJI_SNOWFLAKE   105
#define EMOJI_UMBRELLA    106
#define EMOJI_CLOUD       107
#define EMOJI_LOCK        108
#define EMOJI_KEY         109
#define EMOJI_GEAR        110
#define EMOJI_MAGNIFY     111
#define EMOJI_BELL        112
#define EMOJI_PIN         113
#define EMOJI_FLAG_GENERIC 114
#define EMOJI_FLAG_EU      115
#define EMOJI_FLAG_PAIR    EMOJI_FLAG_PAIR_MARKER

typedef struct {
    uint8_t     marker;
    const char *utf8;
    const char *label;
} emoji_choice_t;

static const emoji_choice_t EMOJI_CHOICES[] = {
    {EMOJI_FLAG_NL, "\xF0\x9F\x87\xB3\xF0\x9F\x87\xB1", "nl flag"},
    {EMOJI_SMILE, "\xF0\x9F\x99\x82", "smile"},
    {EMOJI_GRIN, "\xF0\x9F\x98\x80", "grin"},
    {EMOJI_JOY, "\xF0\x9F\x98\x82", "joy"},
    {EMOJI_HEART_EYES, "\xF0\x9F\x98\x8D", "love"},
    {EMOJI_COOL, "\xF0\x9F\x98\x8E", "cool"},
    {EMOJI_CRY, "\xF0\x9F\x98\xA2", "cry"},
    {EMOJI_THINKING, "\xF0\x9F\xA4\x94", "think"},
    {EMOJI_ANGRY, "\xF0\x9F\x98\xA1", "angry"},
    {EMOJI_SKULL, "\xF0\x9F\x92\x80", "skull"},
    {EMOJI_EYES, "\xF0\x9F\x91\x80", "eyes"},
    {EMOJI_THUMBS_UP, "\xF0\x9F\x91\x8D", "thumb up"},
    {EMOJI_THUMBS_DOWN, "\xF0\x9F\x91\x8E", "thumb down"},
    {EMOJI_WAVE, "\xF0\x9F\x91\x8B", "wave"},
    {EMOJI_PRAY, "\xF0\x9F\x99\x8F", "pray"},
    {EMOJI_HEART, "\xE2\x9D\xA4\xEF\xB8\x8F", "heart"},
    {EMOJI_FIRE, "\xF0\x9F\x94\xA5", "fire"},
    {EMOJI_PARTY, "\xF0\x9F\x8E\x89", "party"},
    {EMOJI_ROCKET, "\xF0\x9F\x9A\x80", "rocket"},
    {EMOJI_STAR, "\xE2\xAD\x90", "star"},
    {EMOJI_CHECK, "\xE2\x9C\x85", "check"},
    {EMOJI_CROSS, "\xE2\x9D\x8C", "cross"},
    {EMOJI_HUNDRED, "\xF0\x9F\x92\xAF", "hundred"},
    {EMOJI_SUN, "\xE2\x98\x80\xEF\xB8\x8F", "sun"},
    {EMOJI_MOON, "\xF0\x9F\x8C\x99", "moon"},
    {EMOJI_BOLT, "\xE2\x9A\xA1", "bolt"},
    {EMOJI_GIFT, "\xF0\x9F\x8E\x81", "gift"},
    {EMOJI_MUSIC, "\xF0\x9F\x8E\xB5", "music"},
    {EMOJI_PIZZA, "\xF0\x9F\x8D\x95", "pizza"},
    {EMOJI_COFFEE, "\xE2\x98\x95", "coffee"},
    {EMOJI_SMILE, "\xF0\x9F\x98\x89", "wink"},
    {EMOJI_SMILE, "\xF0\x9F\x98\x8A", "blush"},
    {EMOJI_HEART_EYES, "\xF0\x9F\x98\x98", "kiss"},
    {EMOJI_THUMBS_UP, "\xF0\x9F\x92\xAA", "strong"},
    {EMOJI_WAVE, "\xF0\x9F\x91\x8F", "clap"},
    {EMOJI_WINK, "\xF0\x9F\x98\x89", "wink"},
    {EMOJI_BLUSH, "\xF0\x9F\x98\x8A", "blush"},
    {EMOJI_KISS, "\xF0\x9F\x98\x98", "kiss"},
    {EMOJI_HEART_HANDS, "\xF0\x9F\xAB\xB6", "heart hands"},
    {EMOJI_STRONG, "\xF0\x9F\x92\xAA", "strong"},
    {EMOJI_CLAP, "\xF0\x9F\x91\x8F", "clap"},
    {EMOJI_SOB, "\xF0\x9F\x98\xAD", "sob"},
    {EMOJI_ROFL, "\xF0\x9F\xA4\xA3", "rofl"},
    {EMOJI_MELTING, "\xF0\x9F\xAB\xA0", "melting"},
    {EMOJI_PLEADING, "\xF0\x9F\xA5\xBA", "plead"},
    {EMOJI_NEUTRAL, "\xF0\x9F\x98\x90", "neutral"},
    {EMOJI_UNAMUSED, "\xF0\x9F\x98\x92", "unamused"},
    {EMOJI_SWEAT_SMILE, "\xF0\x9F\x98\x85", "sweat smile"},
    {EMOJI_RELIEVED, "\xF0\x9F\x98\x8C", "relieved"},
    {EMOJI_SLEEPING, "\xF0\x9F\x98\xB4", "sleep"},
    {EMOJI_FLUSHED, "\xF0\x9F\x98\xB3", "flushed"},
    {EMOJI_SCREAM, "\xF0\x9F\x98\xB1", "scream"},
    {EMOJI_MIND_BLOWN, "\xF0\x9F\xA4\xAF", "mind blown"},
    {EMOJI_SMIRK, "\xF0\x9F\x98\x8F", "smirk"},
    {EMOJI_ZANY, "\xF0\x9F\xA4\xAA", "zany"},
    {EMOJI_PARTY_FACE, "\xF0\x9F\xA5\xB3", "party face"},
    {EMOJI_HUGGING, "\xF0\x9F\xA4\x97", "hug"},
    {EMOJI_SHUSHING, "\xF0\x9F\xA4\xAB", "shush"},
    {EMOJI_FACEPALM, "\xF0\x9F\xA4\xA6", "facepalm"},
    {EMOJI_SHRUG, "\xF0\x9F\xA4\xB7", "shrug"},
    {EMOJI_OK_HAND, "\xF0\x9F\x91\x8C", "ok"},
    {EMOJI_RAISED_HANDS, "\xF0\x9F\x99\x8C", "raised"},
    {EMOJI_POINT_RIGHT, "\xF0\x9F\x91\x89", "right"},
    {EMOJI_POINT_LEFT, "\xF0\x9F\x91\x88", "left"},
    {EMOJI_POINT_UP, "\xE2\x98\x9D", "up"},
    {EMOJI_POINT_DOWN, "\xF0\x9F\x91\x87", "down"},
    {EMOJI_HANDSHAKE, "\xF0\x9F\xA4\x9D", "handshake"},
    {EMOJI_ORANGE_HEART, "\xF0\x9F\xA7\xA1", "orange heart"},
    {EMOJI_YELLOW_HEART, "\xF0\x9F\x92\x9B", "yellow heart"},
    {EMOJI_GREEN_HEART, "\xF0\x9F\x92\x9A", "green heart"},
    {EMOJI_BLUE_HEART, "\xF0\x9F\x92\x99", "blue heart"},
    {EMOJI_PURPLE_HEART, "\xF0\x9F\x92\x9C", "purple heart"},
    {EMOJI_BLACK_HEART, "\xF0\x9F\x96\xA4", "black heart"},
    {EMOJI_BROKEN_HEART, "\xF0\x9F\x92\x94", "broken heart"},
    {EMOJI_SPARKLES, "\xE2\x9C\xA8", "sparkles"},
    {EMOJI_POOP, "\xF0\x9F\x92\xA9", "poop"},
    {EMOJI_BOOM, "\xF0\x9F\x92\xA5", "boom"},
    {EMOJI_DROPS, "\xF0\x9F\x92\xA6", "drops"},
    {EMOJI_ZZZ, "\xF0\x9F\x92\xA4", "zzz"},
    {EMOJI_DASH, "\xF0\x9F\x92\xA8", "dash"},
    {EMOJI_MONKEY_SEE, "\xF0\x9F\x99\x88", "monkey"},
    {EMOJI_CAT_SMILE, "\xF0\x9F\x98\xBA", "cat smile"},
    {EMOJI_DOG, "\xF0\x9F\x90\xB6", "dog"},
    {EMOJI_CAT, "\xF0\x9F\x90\xB1", "cat"},
    {EMOJI_BEER, "\xF0\x9F\x8D\xBA", "beer"},
    {EMOJI_WINE, "\xF0\x9F\x8D\xB7", "wine"},
    {EMOJI_BURGER, "\xF0\x9F\x8D\x94", "burger"},
    {EMOJI_FRIES, "\xF0\x9F\x8D\x9F", "fries"},
    {EMOJI_CAKE, "\xF0\x9F\x8E\x82", "cake"},
    {EMOJI_SOCCER, "\xE2\x9A\xBD", "soccer"},
    {EMOJI_GAME, "\xF0\x9F\x8E\xAE", "game"},
    {EMOJI_PHONE, "\xF0\x9F\x93\xB1", "phone"},
    {EMOJI_LAPTOP, "\xF0\x9F\x92\xBB", "laptop"},
    {EMOJI_BULB, "\xF0\x9F\x92\xA1", "bulb"},
    {EMOJI_MONEY, "\xF0\x9F\x92\xB0", "money"},
    {EMOJI_GEM, "\xF0\x9F\x92\x8E", "gem"},
    {EMOJI_WARNING, "\xE2\x9A\xA0", "warning"},
    {EMOJI_QUESTION, "\xE2\x9D\x93", "question"},
    {EMOJI_EXCLAMATION, "\xE2\x9D\x97", "bang"},
    {EMOJI_CALENDAR, "\xF0\x9F\x93\x85", "calendar"},
    {EMOJI_CLOCK, "\xF0\x9F\x95\x92", "clock"},
    {EMOJI_HOME, "\xF0\x9F\x8F\xA0", "home"},
    {EMOJI_CAR, "\xF0\x9F\x9A\x97", "car"},
    {EMOJI_TRAIN, "\xF0\x9F\x9A\x86", "train"},
    {EMOJI_AIRPLANE, "\xE2\x9C\x88", "plane"},
    {EMOJI_GLOBE, "\xF0\x9F\x8C\x8D", "globe"},
    {EMOJI_FLAG_NL, "\xF0\x9F\x87\xB3\xF0\x9F\x87\xB1", "nl flag"},
    {EMOJI_RAINBOW, "\xF0\x9F\x8C\x88", "rainbow"},
    {EMOJI_SNOWFLAKE, "\xE2\x9D\x84", "snow"},
    {EMOJI_UMBRELLA, "\xE2\x98\x94", "rain"},
    {EMOJI_CLOUD, "\xE2\x98\x81", "cloud"},
    {EMOJI_LOCK, "\xF0\x9F\x94\x92", "lock"},
    {EMOJI_KEY, "\xF0\x9F\x94\x91", "key"},
    {EMOJI_GEAR, "\xE2\x9A\x99", "gear"},
    {EMOJI_MAGNIFY, "\xF0\x9F\x94\x8D", "search"},
    {EMOJI_BELL, "\xF0\x9F\x94\x94", "bell"},
    {EMOJI_PIN, "\xF0\x9F\x93\x8C", "pin"},
};

static const int EMOJI_CHOICE_COUNT = sizeof(EMOJI_CHOICES) / sizeof(EMOJI_CHOICES[0]);
#define EMOJI_PICKER_COLS 6
#define EMOJI_ASSET_SIZE  32
#define EMOJI_ASSET_CACHE 8
#define EMOJI_NAMED_ASSET_CACHE 6
#define APP_REPOSITORY_SLUG "nl.daandobber.matrixmatsu"
#define EMOJI_PACK_MAGIC 0x314B5045u
#define EMOJI_PACK_ENTRY_NAME_LEN 32

typedef struct {
    uint8_t   marker;
    bool      loaded;
    bool      failed;
    uint32_t  last_used;
    void     *pixels;
    pax_buf_t image;
} emoji_asset_t;

typedef struct {
    char      name[24];
    bool      loaded;
    bool      failed;
    uint32_t  last_used;
    void     *pixels;
    pax_buf_t image;
} emoji_named_asset_t;

static emoji_asset_t g_emoji_assets[EMOJI_ASSET_CACHE];
static emoji_named_asset_t g_named_emoji_assets[EMOJI_NAMED_ASSET_CACHE];
static bool          g_sd_mount_attempted = false;
static bool          g_sd_mounted         = false;
static sdmmc_card_t *g_sd_card            = NULL;
static uint32_t      g_emoji_asset_tick   = 0;

static uint32_t utf8_next_codepoint(const char *s, size_t *i) {
    const unsigned char *p = (const unsigned char *)&s[*i];
    if (p[0] < 0x80) {
        (*i)++;
        return p[0];
    }
    if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        uint32_t cp = ((uint32_t)(p[0] & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F);
        *i += 2;
        return cp;
    }
    if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        uint32_t cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (uint32_t)(p[2] & 0x3F);
        *i += 3;
        return cp;
    }
    if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        uint32_t cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
                      ((uint32_t)(p[2] & 0x3F) << 6) | (uint32_t)(p[3] & 0x3F);
        *i += 4;
        return cp;
    }
    (*i)++;
    return '?';
}

static uint8_t emoji_marker(uint32_t cp) {
    switch (cp) {
        case 0x1F600: return EMOJI_GRIN;
        case 0x1F601:
        case 0x1F606:
        case 0x1F602: return EMOJI_JOY;
        case 0x1F603:
        case 0x1F604:
        case 0x1F970:
        case 0x1F642:
        case 0x263A: return EMOJI_SMILE;
        case 0x1F60D: return EMOJI_HEART_EYES;
        case 0x1F60E: return EMOJI_COOL;
        case 0x1F641:
        case 0x2639:
        case 0x1F622: return EMOJI_CRY;
        case 0x1F914: return EMOJI_THINKING;
        case 0x1F44D: return EMOJI_THUMBS_UP;
        case 0x1F44E: return EMOJI_THUMBS_DOWN;
        case 0x1F44B: return EMOJI_WAVE;
        case 0x1F64F: return EMOJI_PRAY;
        case 0x2764: return EMOJI_HEART;
        case 0x1F525: return EMOJI_FIRE;
        case 0x1F389: return EMOJI_PARTY;
        case 0x1F680: return EMOJI_ROCKET;
        case 0x2B50: return EMOJI_STAR;
        case 0x2705: return EMOJI_CHECK;
        case 0x274C: return EMOJI_CROSS;
        case 0x1F4AF: return EMOJI_HUNDRED;
        case 0x1F440: return EMOJI_EYES;
        case 0x1F621:
        case 0x1F620:
        return EMOJI_ANGRY;
        case 0x1F480: return EMOJI_SKULL;
        case 0x2600: return EMOJI_SUN;
        case 0x1F319: return EMOJI_MOON;
        case 0x26A1: return EMOJI_BOLT;
        case 0x1F381: return EMOJI_GIFT;
        case 0x1F3B5: return EMOJI_MUSIC;
        case 0x1F355: return EMOJI_PIZZA;
        case 0x2615: return EMOJI_COFFEE;
        case 0x1F609: return EMOJI_WINK;
        case 0x1F60A: return EMOJI_BLUSH;
        case 0x1F618: return EMOJI_KISS;
        case 0x1FAF6: return EMOJI_HEART_HANDS;
        case 0x1F4AA: return EMOJI_STRONG;
        case 0x1F44F: return EMOJI_CLAP;
        case 0x1F62D: return EMOJI_SOB;
        case 0x1F923: return EMOJI_ROFL;
        case 0x1FAE0: return EMOJI_MELTING;
        case 0x1F97A: return EMOJI_PLEADING;
        case 0x1F610: return EMOJI_NEUTRAL;
        case 0x1F612: return EMOJI_UNAMUSED;
        case 0x1F605: return EMOJI_SWEAT_SMILE;
        case 0x1F60C: return EMOJI_RELIEVED;
        case 0x1F634: return EMOJI_SLEEPING;
        case 0x1F633: return EMOJI_FLUSHED;
        case 0x1F631: return EMOJI_SCREAM;
        case 0x1F92F: return EMOJI_MIND_BLOWN;
        case 0x1F60F: return EMOJI_SMIRK;
        case 0x1F92A: return EMOJI_ZANY;
        case 0x1F973: return EMOJI_PARTY_FACE;
        case 0x1F917: return EMOJI_HUGGING;
        case 0x1F92B: return EMOJI_SHUSHING;
        case 0x1F926: return EMOJI_FACEPALM;
        case 0x1F937: return EMOJI_SHRUG;
        case 0x1F44C: return EMOJI_OK_HAND;
        case 0x1F64C: return EMOJI_RAISED_HANDS;
        case 0x1F449: return EMOJI_POINT_RIGHT;
        case 0x1F448: return EMOJI_POINT_LEFT;
        case 0x261D: return EMOJI_POINT_UP;
        case 0x1F447: return EMOJI_POINT_DOWN;
        case 0x1F91D: return EMOJI_HANDSHAKE;
        case 0x1F9E1: return EMOJI_ORANGE_HEART;
        case 0x1F49B: return EMOJI_YELLOW_HEART;
        case 0x1F49A: return EMOJI_GREEN_HEART;
        case 0x1F499: return EMOJI_BLUE_HEART;
        case 0x1F49C: return EMOJI_PURPLE_HEART;
        case 0x1F5A4: return EMOJI_BLACK_HEART;
        case 0x1F494: return EMOJI_BROKEN_HEART;
        case 0x2728: return EMOJI_SPARKLES;
        case 0x1F4A9: return EMOJI_POOP;
        case 0x1F4A5: return EMOJI_BOOM;
        case 0x1F4A6: return EMOJI_DROPS;
        case 0x1F4A4: return EMOJI_ZZZ;
        case 0x1F4A8: return EMOJI_DASH;
        case 0x1F648: return EMOJI_MONKEY_SEE;
        case 0x1F63A: return EMOJI_CAT_SMILE;
        case 0x1F436: return EMOJI_DOG;
        case 0x1F431: return EMOJI_CAT;
        case 0x1F37A: return EMOJI_BEER;
        case 0x1F377: return EMOJI_WINE;
        case 0x1F354: return EMOJI_BURGER;
        case 0x1F35F: return EMOJI_FRIES;
        case 0x1F382: return EMOJI_CAKE;
        case 0x26BD: return EMOJI_SOCCER;
        case 0x1F3AE: return EMOJI_GAME;
        case 0x1F4F1: return EMOJI_PHONE;
        case 0x1F4BB: return EMOJI_LAPTOP;
        case 0x1F4A1: return EMOJI_BULB;
        case 0x1F4B0: return EMOJI_MONEY;
        case 0x1F48E: return EMOJI_GEM;
        case 0x26A0: return EMOJI_WARNING;
        case 0x2753: return EMOJI_QUESTION;
        case 0x2757: return EMOJI_EXCLAMATION;
        case 0x1F4C5: return EMOJI_CALENDAR;
        case 0x1F552: return EMOJI_CLOCK;
        case 0x1F3E0: return EMOJI_HOME;
        case 0x1F697: return EMOJI_CAR;
        case 0x1F686: return EMOJI_TRAIN;
        case 0x2708: return EMOJI_AIRPLANE;
        case 0x1F30D: return EMOJI_GLOBE;
        case 0x1F308: return EMOJI_RAINBOW;
        case 0x2744: return EMOJI_SNOWFLAKE;
        case 0x2614: return EMOJI_UMBRELLA;
        case 0x2601: return EMOJI_CLOUD;
        case 0x1F512: return EMOJI_LOCK;
        case 0x1F511: return EMOJI_KEY;
        case 0x2699: return EMOJI_GEAR;
        case 0x1F50D: return EMOJI_MAGNIFY;
        case 0x1F514: return EMOJI_BELL;
        case 0x1F4CC: return EMOJI_PIN;
        default: return 0;
    }
}

static bool is_emoji_marker(unsigned char c) {
    return c >= EMOJI_GENERIC && c <= EMOJI_EXT;
}

static bool append_emoji_marker(char *out, size_t out_len, size_t *o, uint8_t marker) {
    if (marker <= EMOJI_COFFEE) {
        if (*o + 1 >= out_len) return false;
        out[(*o)++] = (char)marker;
        out[*o]     = '\0';
        return true;
    }
    if (*o + 2 >= out_len) return false;
    out[(*o)++] = (char)EMOJI_EXT;
    out[(*o)++] = (char)marker;
    out[*o]     = '\0';
    return true;
}

static bool append_flag_pair_marker(char *out, size_t out_len, size_t *o, char first, char second) {
    if (*o + 4 >= out_len) return false;
    out[(*o)++] = (char)EMOJI_EXT;
    out[(*o)++] = (char)EMOJI_FLAG_PAIR;
    out[(*o)++] = first;
    out[(*o)++] = second;
    out[*o]     = '\0';
    return true;
}

static bool try_decode_utf16_surrogate_pair(const char *in, size_t *i, uint32_t high, uint32_t *out_cp) {
    if (high < 0xD800 || high > 0xDBFF) return false;
    size_t   next_i = *i;
    uint32_t low    = utf8_next_codepoint(in, &next_i);
    if (low < 0xDC00 || low > 0xDFFF) return false;

    *out_cp = 0x10000 + ((high - 0xD800) << 10) + (low - 0xDC00);
    *i      = next_i;
    return true;
}

static int hex_digit_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

static bool parse_literal_json_u(const char *in, size_t *i, uint32_t *out_cp) {
    if (in[*i] != '\\' || in[*i + 1] != 'u') return false;
    uint32_t first = 0;
    for (int n = 0; n < 4; n++) {
        int v = hex_digit_value(in[*i + 2 + n]);
        if (v < 0) return false;
        first = (first << 4) | (uint32_t)v;
    }
    *i += 6;

    if (first >= 0xD800 && first <= 0xDBFF && in[*i] == '\\' && in[*i + 1] == 'u') {
        uint32_t second = 0;
        for (int n = 0; n < 4; n++) {
            int v = hex_digit_value(in[*i + 2 + n]);
            if (v < 0) break;
            second = (second << 4) | (uint32_t)v;
        }
        if (second >= 0xDC00 && second <= 0xDFFF) {
            *i += 6;
            *out_cp = 0x10000 + ((first - 0xD800) << 10) + (second - 0xDC00);
            return true;
        }
    }

    *out_cp = first;
    return true;
}

static bool read_next_text_codepoint(const char *in, size_t *i, uint32_t *out_cp) {
    if (parse_literal_json_u(in, i, out_cp)) return true;
    *out_cp = utf8_next_codepoint(in, i);
    uint32_t decoded_cp;
    if (try_decode_utf16_surrogate_pair(in, i, *out_cp, &decoded_cp)) {
        *out_cp = decoded_cp;
    }
    return true;
}

static void skip_emoji_joiners(const char *in, size_t *i) {
    while (in[*i] != '\0') {
        size_t   next_i = *i;
        uint32_t cp;
        read_next_text_codepoint(in, &next_i, &cp);
        if (cp != 0xFE0F && cp != 0x200D) break;
        *i = next_i;
    }
}

static bool try_parse_tag_flag(const char *in, size_t *i, uint8_t *out_marker) {
    size_t cursor = *i;
    char   tags[8];
    int    count = 0;

    skip_emoji_joiners(in, &cursor);

    while (in[cursor] != '\0' && count < (int)sizeof(tags) - 1) {
        size_t   next_cursor = cursor;
        uint32_t cp;
        read_next_text_codepoint(in, &next_cursor, &cp);

        if (cp == 0xE007F) {
            cursor = next_cursor;
            break;
        }
        if (cp == 0xFE0F || cp == 0x200D) {
            cursor = next_cursor;
            continue;
        }
        if (cp < 0xE0061 || cp > 0xE007A) break;
        tags[count++] = (char)('a' + (cp - 0xE0061));
        cursor        = next_cursor;
    }
    tags[count] = '\0';
    if (count == 0) return false;

    if (strcmp(tags, "nl") == 0 || strcmp(tags, "nld") == 0 || strncmp(tags, "nl", 2) == 0) {
        *out_marker = EMOJI_FLAG_NL;
    } else if (strcmp(tags, "eu") == 0) {
        *out_marker = EMOJI_FLAG_EU;
    } else {
        *out_marker = EMOJI_FLAG_GENERIC;
    }
    *i = cursor;
    return true;
}

static void text_with_emoji_markers(const char *in, char *out, size_t out_len) {
    size_t i = 0, o = 0;
    if (out_len == 0) return;
    out[0] = '\0';
    while (in != NULL && in[i] != '\0' && o + 1 < out_len) {
        static const unsigned char nl_flag_utf8[] = {0xF0, 0x9F, 0x87, 0xB3, 0xF0, 0x9F, 0x87, 0xB1, 0x00};
        if (memcmp((const unsigned char *)&in[i], nl_flag_utf8, sizeof(nl_flag_utf8) - 1) == 0) {
            if (!append_emoji_marker(out, out_len, &o, EMOJI_FLAG_NL)) break;
            i += sizeof(nl_flag_utf8) - 1;
            continue;
        }
        static const unsigned char eu_flag_utf8[] = {0xF0, 0x9F, 0x87, 0xAA, 0xF0, 0x9F, 0x87, 0xBA, 0x00};
        if (memcmp((const unsigned char *)&in[i], eu_flag_utf8, sizeof(eu_flag_utf8) - 1) == 0) {
            if (!append_emoji_marker(out, out_len, &o, EMOJI_FLAG_EU)) break;
            i += sizeof(eu_flag_utf8) - 1;
            continue;
        }
        static const unsigned char nl_flag_surrogate_utf8[] = {
            0xED, 0xA0, 0xBC, 0xED, 0xB7, 0xB3, 0xED, 0xA0, 0xBC, 0xED, 0xB7, 0xB1, 0x00
        };
        if (memcmp((const unsigned char *)&in[i], nl_flag_surrogate_utf8, sizeof(nl_flag_surrogate_utf8) - 1) == 0) {
            if (!append_emoji_marker(out, out_len, &o, EMOJI_FLAG_NL)) break;
            i += sizeof(nl_flag_surrogate_utf8) - 1;
            continue;
        }

        size_t   start = i;
        uint32_t cp;
        read_next_text_codepoint(in, &i, &cp);

        if (cp == 0x1F3F4) {
            uint8_t tag_marker;
            if (try_parse_tag_flag(in, &i, &tag_marker)) {
                if (!append_emoji_marker(out, out_len, &o, tag_marker)) break;
                continue;
            }
        }

        if (cp >= 0x1F1E6 && cp <= 0x1F1FF) {
            size_t   next_i = i;
            skip_emoji_joiners(in, &next_i);
            uint32_t next_cp;
            read_next_text_codepoint(in, &next_i, &next_cp);
            if (next_cp >= 0x1F1E6 && next_cp <= 0x1F1FF) {
                char first  = (char)('A' + (cp - 0x1F1E6));
                char second = (char)('A' + (next_cp - 0x1F1E6));
                if (!append_flag_pair_marker(out, out_len, &o, first, second)) break;
                i = next_i;
                continue;
            }
            if (!append_emoji_marker(out, out_len, &o, EMOJI_FLAG_GENERIC)) break;
            continue;
        }

        uint8_t  marker = emoji_marker(cp);
        if (cp == 0x1F1F3) {
            size_t   next_i = i;
            uint32_t next_cp = utf8_next_codepoint(in, &next_i);
            uint32_t decoded_next_cp;
            if (try_decode_utf16_surrogate_pair(in, &next_i, next_cp, &decoded_next_cp)) {
                next_cp = decoded_next_cp;
            }
            if (next_cp == 0x1F1F1) {
                marker = EMOJI_FLAG_NL;
                i      = next_i;
            }
        }
        if (marker != 0) {
            if (!append_emoji_marker(out, out_len, &o, marker)) break;
        } else if (cp == 0xFE0F || cp == 0x200D || (cp >= 0x1F3FB && cp <= 0x1F3FF)) {
            continue;
        } else if (cp >= 0x1F000) {
            if (debug_status) {
                char code[16];
                snprintf(code, sizeof(code), "[U+%04" PRIX32 "]", cp);
                size_t len = strlen(code);
                if (o + len + 1 >= out_len) break;
                memcpy(out + o, code, len + 1);
                o += len;
            } else if (!append_emoji_marker(out, out_len, &o, EMOJI_GENERIC)) {
                break;
            }
        } else {
            size_t len = i - start;
            if (o + len + 1 >= out_len) break;
            memcpy(out + o, in + start, len);
            o += len;
            out[o] = '\0';
        }
    }
}

static bool ensure_sd_mounted(void) {
    if (g_sd_mount_attempted) return g_sd_mounted;
    g_sd_mount_attempted = true;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files              = 4,
        .allocation_unit_size   = 16 * 1024,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot         = SDMMC_HOST_SLOT_0;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t res = esp_vfs_fat_sdmmc_mount("/sd", &host, &slot_config, &mount_config, &g_sd_card);
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "SD card not mounted for emoji assets: %s", esp_err_to_name(res));
        g_sd_mounted = false;
        return false;
    }

    ESP_LOGI(TAG, "SD card mounted for emoji assets");
    g_sd_mounted = true;
    return true;
}

typedef struct {
    uint8_t     marker;
    const char *name;
} emoji_asset_name_t;

static const emoji_asset_name_t EMOJI_ASSET_NAMES[] = {
    {EMOJI_SMILE, "smile"}, {EMOJI_GRIN, "grin"}, {EMOJI_JOY, "joy"},
    {EMOJI_HEART_EYES, "heart_eyes"}, {EMOJI_COOL, "cool"}, {EMOJI_CRY, "cry"},
    {EMOJI_THINKING, "thinking"}, {EMOJI_THUMBS_UP, "thumbs_up"}, {EMOJI_THUMBS_DOWN, "thumbs_down"},
    {EMOJI_WAVE, "wave"}, {EMOJI_PRAY, "pray"}, {EMOJI_HEART, "heart"},
    {EMOJI_FIRE, "fire"}, {EMOJI_PARTY, "party"}, {EMOJI_ROCKET, "rocket"},
    {EMOJI_STAR, "star"}, {EMOJI_CHECK, "check"}, {EMOJI_CROSS, "cross"},
    {EMOJI_HUNDRED, "hundred"}, {EMOJI_EYES, "eyes"}, {EMOJI_ANGRY, "angry"},
    {EMOJI_SKULL, "skull"}, {EMOJI_SUN, "sun"}, {EMOJI_MOON, "moon"},
    {EMOJI_BOLT, "bolt"}, {EMOJI_GIFT, "gift"}, {EMOJI_MUSIC, "music"},
    {EMOJI_PIZZA, "pizza"}, {EMOJI_COFFEE, "coffee"}, {EMOJI_WINK, "wink"},
    {EMOJI_BLUSH, "blush"}, {EMOJI_KISS, "kiss"}, {EMOJI_HEART_HANDS, "heart_hands"},
    {EMOJI_STRONG, "strong"}, {EMOJI_CLAP, "clap"}, {EMOJI_SOB, "sob"},
    {EMOJI_ROFL, "rofl"}, {EMOJI_MELTING, "melting"}, {EMOJI_PLEADING, "pleading"},
    {EMOJI_NEUTRAL, "neutral"}, {EMOJI_UNAMUSED, "unamused"}, {EMOJI_SWEAT_SMILE, "sweat_smile"},
    {EMOJI_RELIEVED, "relieved"}, {EMOJI_SLEEPING, "sleeping"}, {EMOJI_FLUSHED, "flushed"},
    {EMOJI_SCREAM, "scream"}, {EMOJI_MIND_BLOWN, "mind_blown"}, {EMOJI_SMIRK, "smirk"},
    {EMOJI_ZANY, "zany"}, {EMOJI_PARTY_FACE, "party_face"}, {EMOJI_HUGGING, "hugging"},
    {EMOJI_SHUSHING, "shushing"}, {EMOJI_FACEPALM, "facepalm"}, {EMOJI_SHRUG, "shrug"},
    {EMOJI_OK_HAND, "ok_hand"}, {EMOJI_RAISED_HANDS, "raised_hands"}, {EMOJI_POINT_RIGHT, "point_right"},
    {EMOJI_POINT_LEFT, "point_left"}, {EMOJI_POINT_UP, "point_up"}, {EMOJI_POINT_DOWN, "point_down"},
    {EMOJI_HANDSHAKE, "handshake"}, {EMOJI_ORANGE_HEART, "orange_heart"}, {EMOJI_YELLOW_HEART, "yellow_heart"},
    {EMOJI_GREEN_HEART, "green_heart"}, {EMOJI_BLUE_HEART, "blue_heart"}, {EMOJI_PURPLE_HEART, "purple_heart"},
    {EMOJI_BLACK_HEART, "black_heart"}, {EMOJI_BROKEN_HEART, "broken_heart"}, {EMOJI_SPARKLES, "sparkles"},
    {EMOJI_POOP, "poop"}, {EMOJI_BOOM, "boom"}, {EMOJI_DROPS, "drops"},
    {EMOJI_ZZZ, "zzz"}, {EMOJI_DASH, "dash"}, {EMOJI_MONKEY_SEE, "monkey_see"},
    {EMOJI_CAT_SMILE, "cat_smile"}, {EMOJI_DOG, "dog"}, {EMOJI_CAT, "cat"},
    {EMOJI_BEER, "beer"}, {EMOJI_WINE, "wine"}, {EMOJI_BURGER, "burger"},
    {EMOJI_FRIES, "fries"}, {EMOJI_CAKE, "cake"}, {EMOJI_SOCCER, "soccer"},
    {EMOJI_GAME, "game"}, {EMOJI_PHONE, "phone"}, {EMOJI_LAPTOP, "laptop"},
    {EMOJI_BULB, "bulb"}, {EMOJI_MONEY, "money"}, {EMOJI_GEM, "gem"},
    {EMOJI_WARNING, "warning"}, {EMOJI_QUESTION, "question"}, {EMOJI_EXCLAMATION, "exclamation"},
    {EMOJI_CALENDAR, "calendar"}, {EMOJI_CLOCK, "clock"}, {EMOJI_HOME, "home"},
    {EMOJI_CAR, "car"}, {EMOJI_TRAIN, "train"}, {EMOJI_AIRPLANE, "airplane"},
    {EMOJI_GLOBE, "globe"}, {EMOJI_FLAG_NL, "flag_nl"}, {EMOJI_RAINBOW, "rainbow"},
    {EMOJI_SNOWFLAKE, "snowflake"}, {EMOJI_UMBRELLA, "umbrella"}, {EMOJI_CLOUD, "cloud"},
    {EMOJI_LOCK, "lock"}, {EMOJI_KEY, "key"}, {EMOJI_GEAR, "gear"},
    {EMOJI_MAGNIFY, "magnify"}, {EMOJI_BELL, "bell"}, {EMOJI_PIN, "pin"},
};

static const char *emoji_asset_name(uint8_t marker) {
    for (int i = 0; i < (int)(sizeof(EMOJI_ASSET_NAMES) / sizeof(EMOJI_ASSET_NAMES[0])); i++) {
        if (EMOJI_ASSET_NAMES[i].marker == marker) return EMOJI_ASSET_NAMES[i].name;
    }
    return "generic";
}

static bool load_emoji_asset_file(emoji_asset_t *asset, const char *path, pax_buf_type_t type, size_t bytes_per_pixel) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;

    const size_t byte_count = EMOJI_ASSET_SIZE * EMOJI_ASSET_SIZE * bytes_per_pixel;
    void        *pixels     = heap_caps_malloc(byte_count, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pixels == NULL) {
        fclose(file);
        return false;
    }

    size_t got = fread(pixels, 1, byte_count, file);
    fclose(file);
    if (got != byte_count) {
        free(pixels);
        return false;
    }

    if (asset->loaded) {
        pax_buf_destroy(&asset->image);
    }
    free(asset->pixels);
    memset(&asset->image, 0, sizeof(asset->image));
    if (!pax_buf_init(&asset->image, pixels, EMOJI_ASSET_SIZE, EMOJI_ASSET_SIZE, type)) {
        free(pixels);
        asset->pixels = NULL;
        return false;
    }
    asset->pixels = pixels;
    asset->loaded = true;
    asset->failed = false;
    return true;
}

static bool load_emoji_asset_pack_entry(emoji_asset_t *asset, const char *pack_path, const char *name) {
    FILE *file = fopen(pack_path, "rb");
    if (file == NULL) return false;

    uint32_t magic = 0;
    uint32_t count = 0;
    if (fread(&magic, 1, sizeof(magic), file) != sizeof(magic) ||
        fread(&count, 1, sizeof(count), file) != sizeof(count) ||
        magic != EMOJI_PACK_MAGIC || count > 512) {
        fclose(file);
        return false;
    }

    const size_t byte_count = EMOJI_ASSET_SIZE * EMOJI_ASSET_SIZE * 4;
    for (uint32_t i = 0; i < count; i++) {
        char entry_name[EMOJI_PACK_ENTRY_NAME_LEN];
        if (fread(entry_name, 1, sizeof(entry_name), file) != sizeof(entry_name)) break;
        entry_name[sizeof(entry_name) - 1] = '\0';
        if (strcmp(entry_name, name) != 0) {
            fseek(file, (long)byte_count, SEEK_CUR);
            continue;
        }

        void *pixels = heap_caps_malloc(byte_count, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (pixels == NULL) {
            fclose(file);
            return false;
        }
        size_t got = fread(pixels, 1, byte_count, file);
        fclose(file);
        if (got != byte_count) {
            free(pixels);
            return false;
        }

        if (asset->loaded) {
            pax_buf_destroy(&asset->image);
        }
        free(asset->pixels);
        memset(&asset->image, 0, sizeof(asset->image));
        if (!pax_buf_init(&asset->image, pixels, EMOJI_ASSET_SIZE, EMOJI_ASSET_SIZE, PAX_BUF_32_8888ARGB)) {
            free(pixels);
            asset->pixels = NULL;
            return false;
        }
        asset->pixels = pixels;
        asset->loaded = true;
        asset->failed = false;
        return true;
    }

    fclose(file);
    return false;
}

static emoji_asset_t *get_emoji_asset(uint8_t marker) {
    for (int i = 0; i < EMOJI_ASSET_CACHE; i++) {
        if (g_emoji_assets[i].marker == marker) {
            g_emoji_assets[i].last_used = ++g_emoji_asset_tick;
            return &g_emoji_assets[i];
        }
    }

    int slot = -1;
    for (int i = 0; i < EMOJI_ASSET_CACHE; i++) {
        if (g_emoji_assets[i].marker == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        uint32_t oldest = UINT32_MAX;
        for (int i = 0; i < EMOJI_ASSET_CACHE; i++) {
            if (g_emoji_assets[i].last_used < oldest) {
                oldest = g_emoji_assets[i].last_used;
                slot   = i;
            }
        }
    }

    emoji_asset_t *asset = &g_emoji_assets[slot];
    if (asset->loaded) {
        pax_buf_destroy(&asset->image);
    }
    free(asset->pixels);
    memset(asset, 0, sizeof(*asset));
    asset->marker    = marker;
    asset->last_used = ++g_emoji_asset_tick;
    return asset;
}

static bool load_emoji_asset_from_dir(emoji_asset_t *asset, uint8_t marker, const char *emoji_dir) {
    char path[96];
    snprintf(path, sizeof(path), "%s/%s.argb8888", emoji_dir, emoji_asset_name(marker));
    if (load_emoji_asset_file(asset, path, PAX_BUF_32_8888ARGB, 4)) return true;
    snprintf(path, sizeof(path), "%s/%02u.argb8888", emoji_dir, marker);
    if (load_emoji_asset_file(asset, path, PAX_BUF_32_8888ARGB, 4)) return true;
    snprintf(path, sizeof(path), "%s/%s.rgb565", emoji_dir, emoji_asset_name(marker));
    if (load_emoji_asset_file(asset, path, PAX_BUF_16_565RGB, 2)) return true;
    snprintf(path, sizeof(path), "%s/%02u.rgb565", emoji_dir, marker);
    return load_emoji_asset_file(asset, path, PAX_BUF_16_565RGB, 2);
}

static bool try_draw_sd_emoji(uint8_t marker, float x, float y, float s) {
    emoji_asset_t *asset = get_emoji_asset(marker);
    if (asset == NULL || asset->failed) return false;
    if (!asset->loaded) {
        const char *name = emoji_asset_name(marker);
        if (!load_emoji_asset_pack_entry(asset, "/sd/apps/" APP_REPOSITORY_SLUG "/emoji.pak", name) &&
            !load_emoji_asset_pack_entry(asset, "/sd/matrixmatsu/emoji.pak", name) &&
            !load_emoji_asset_from_dir(asset, marker, "/sd/apps/" APP_REPOSITORY_SLUG) &&
            !load_emoji_asset_from_dir(asset, marker, "/sd/apps/" APP_REPOSITORY_SLUG "/emoji") &&
            !load_emoji_asset_from_dir(asset, marker, "/sd/matrixmatsu/emoji")) {
            if (!ensure_sd_mounted() ||
                (!load_emoji_asset_pack_entry(asset, "/sd/apps/" APP_REPOSITORY_SLUG "/emoji.pak", name) &&
                 !load_emoji_asset_pack_entry(asset, "/sd/matrixmatsu/emoji.pak", name) &&
                 !load_emoji_asset_from_dir(asset, marker, "/sd/apps/" APP_REPOSITORY_SLUG) &&
                 !load_emoji_asset_from_dir(asset, marker, "/sd/apps/" APP_REPOSITORY_SLUG "/emoji") &&
                 !load_emoji_asset_from_dir(asset, marker, "/sd/matrixmatsu/emoji"))) {
                asset->failed = true;
                return false;
            }
        }
    }

    pax_draw_image_sized(&fb, &asset->image, x, y, s, s);
    return true;
}

static bool load_named_emoji_asset_file(emoji_named_asset_t *asset, const char *path, pax_buf_type_t type, size_t bytes_per_pixel) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;

    const size_t byte_count = EMOJI_ASSET_SIZE * EMOJI_ASSET_SIZE * bytes_per_pixel;
    void        *pixels     = heap_caps_malloc(byte_count, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pixels == NULL) {
        fclose(file);
        return false;
    }

    size_t got = fread(pixels, 1, byte_count, file);
    fclose(file);
    if (got != byte_count) {
        free(pixels);
        return false;
    }

    if (asset->loaded) {
        pax_buf_destroy(&asset->image);
    }
    free(asset->pixels);
    memset(&asset->image, 0, sizeof(asset->image));
    if (!pax_buf_init(&asset->image, pixels, EMOJI_ASSET_SIZE, EMOJI_ASSET_SIZE, type)) {
        free(pixels);
        asset->pixels = NULL;
        return false;
    }
    asset->pixels = pixels;
    asset->loaded = true;
    asset->failed = false;
    return true;
}

static bool load_named_emoji_asset_pack_entry(emoji_named_asset_t *asset, const char *pack_path, const char *name) {
    FILE *file = fopen(pack_path, "rb");
    if (file == NULL) return false;

    uint32_t magic = 0;
    uint32_t count = 0;
    if (fread(&magic, 1, sizeof(magic), file) != sizeof(magic) ||
        fread(&count, 1, sizeof(count), file) != sizeof(count) ||
        magic != EMOJI_PACK_MAGIC || count > 512) {
        fclose(file);
        return false;
    }

    const size_t byte_count = EMOJI_ASSET_SIZE * EMOJI_ASSET_SIZE * 4;
    for (uint32_t i = 0; i < count; i++) {
        char entry_name[EMOJI_PACK_ENTRY_NAME_LEN];
        if (fread(entry_name, 1, sizeof(entry_name), file) != sizeof(entry_name)) break;
        entry_name[sizeof(entry_name) - 1] = '\0';
        if (strcmp(entry_name, name) != 0) {
            fseek(file, (long)byte_count, SEEK_CUR);
            continue;
        }

        void *pixels = heap_caps_malloc(byte_count, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (pixels == NULL) {
            fclose(file);
            return false;
        }
        size_t got = fread(pixels, 1, byte_count, file);
        fclose(file);
        if (got != byte_count) {
            free(pixels);
            return false;
        }

        if (asset->loaded) {
            pax_buf_destroy(&asset->image);
        }
        free(asset->pixels);
        memset(&asset->image, 0, sizeof(asset->image));
        if (!pax_buf_init(&asset->image, pixels, EMOJI_ASSET_SIZE, EMOJI_ASSET_SIZE, PAX_BUF_32_8888ARGB)) {
            free(pixels);
            asset->pixels = NULL;
            return false;
        }
        asset->pixels = pixels;
        asset->loaded = true;
        asset->failed = false;
        return true;
    }

    fclose(file);
    return false;
}

static emoji_named_asset_t *get_named_emoji_asset(const char *name) {
    for (int i = 0; i < EMOJI_NAMED_ASSET_CACHE; i++) {
        if (strcmp(g_named_emoji_assets[i].name, name) == 0) {
            g_named_emoji_assets[i].last_used = ++g_emoji_asset_tick;
            return &g_named_emoji_assets[i];
        }
    }

    int slot = -1;
    for (int i = 0; i < EMOJI_NAMED_ASSET_CACHE; i++) {
        if (g_named_emoji_assets[i].name[0] == '\0') {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        uint32_t oldest = UINT32_MAX;
        for (int i = 0; i < EMOJI_NAMED_ASSET_CACHE; i++) {
            if (g_named_emoji_assets[i].last_used < oldest) {
                oldest = g_named_emoji_assets[i].last_used;
                slot   = i;
            }
        }
    }

    emoji_named_asset_t *asset = &g_named_emoji_assets[slot];
    if (asset->loaded) {
        pax_buf_destroy(&asset->image);
    }
    free(asset->pixels);
    memset(asset, 0, sizeof(*asset));
    snprintf(asset->name, sizeof(asset->name), "%s", name);
    asset->last_used = ++g_emoji_asset_tick;
    return asset;
}

static bool load_named_emoji_asset_from_dir(emoji_named_asset_t *asset, const char *name, const char *emoji_dir) {
    char path[112];
    snprintf(path, sizeof(path), "%s/%s.argb8888", emoji_dir, name);
    if (load_named_emoji_asset_file(asset, path, PAX_BUF_32_8888ARGB, 4)) return true;
    snprintf(path, sizeof(path), "%s/%s.rgb565", emoji_dir, name);
    return load_named_emoji_asset_file(asset, path, PAX_BUF_16_565RGB, 2);
}

static bool try_draw_sd_named_emoji(const char *name, float x, float y, float s) {
    emoji_named_asset_t *asset = get_named_emoji_asset(name);
    if (asset == NULL || asset->failed) return false;
    if (!asset->loaded) {
        if (!load_named_emoji_asset_pack_entry(asset, "/sd/apps/" APP_REPOSITORY_SLUG "/emoji.pak", name) &&
            !load_named_emoji_asset_pack_entry(asset, "/sd/matrixmatsu/emoji.pak", name) &&
            !load_named_emoji_asset_from_dir(asset, name, "/sd/apps/" APP_REPOSITORY_SLUG) &&
            !load_named_emoji_asset_from_dir(asset, name, "/sd/apps/" APP_REPOSITORY_SLUG "/emoji") &&
            !load_named_emoji_asset_from_dir(asset, name, "/sd/matrixmatsu/emoji")) {
            if (!ensure_sd_mounted() ||
                (!load_named_emoji_asset_pack_entry(asset, "/sd/apps/" APP_REPOSITORY_SLUG "/emoji.pak", name) &&
                 !load_named_emoji_asset_pack_entry(asset, "/sd/matrixmatsu/emoji.pak", name) &&
                 !load_named_emoji_asset_from_dir(asset, name, "/sd/apps/" APP_REPOSITORY_SLUG) &&
                 !load_named_emoji_asset_from_dir(asset, name, "/sd/apps/" APP_REPOSITORY_SLUG "/emoji") &&
                 !load_named_emoji_asset_from_dir(asset, name, "/sd/matrixmatsu/emoji"))) {
                asset->failed = true;
                return false;
            }
        }
    }

    pax_draw_image_sized(&fb, &asset->image, x, y, s, s);
    return true;
}

static void draw_emoji_face(float x, float y, float s, uint8_t marker) {
    pax_col_t yellow = 0xFFFFD43B;
    pax_col_t dark   = 0xFF332200;
    pax_col_t blue   = 0xFF55B7FF;
    pax_col_t red    = 0xFFFF4D6D;

    float cx = x + s * 0.5f;
    float cy = y + s * 0.5f;
    pax_draw_circle(&fb, yellow, cx, cy, s * 0.48f);
    pax_outline_circle(&fb, 0xFFB88700, cx, cy, s * 0.48f);

    if (marker == EMOJI_HEART_EYES) {
        pax_draw_circle(&fb, red, x + s * 0.34f, y + s * 0.34f, s * 0.08f);
        pax_draw_circle(&fb, red, x + s * 0.43f, y + s * 0.34f, s * 0.08f);
        pax_draw_tri(&fb, red, x + s * 0.28f, y + s * 0.38f, x + s * 0.49f, y + s * 0.38f, x + s * 0.39f, y + s * 0.52f);
        pax_draw_circle(&fb, red, x + s * 0.62f, y + s * 0.34f, s * 0.08f);
        pax_draw_circle(&fb, red, x + s * 0.71f, y + s * 0.34f, s * 0.08f);
        pax_draw_tri(&fb, red, x + s * 0.56f, y + s * 0.38f, x + s * 0.77f, y + s * 0.38f, x + s * 0.67f, y + s * 0.52f);
    } else if (marker == EMOJI_COOL) {
        pax_draw_round_rect(&fb, dark, x + s * 0.20f, y + s * 0.30f, s * 0.24f, s * 0.16f, s * 0.04f);
        pax_draw_round_rect(&fb, dark, x + s * 0.56f, y + s * 0.30f, s * 0.24f, s * 0.16f, s * 0.04f);
        pax_draw_line(&fb, dark, x + s * 0.44f, y + s * 0.38f, x + s * 0.56f, y + s * 0.38f);
    } else {
        pax_draw_circle(&fb, dark, x + s * 0.34f, y + s * 0.36f, s * 0.06f);
        pax_draw_circle(&fb, dark, x + s * 0.66f, y + s * 0.36f, s * 0.06f);
    }

    if (marker == EMOJI_JOY || marker == EMOJI_CRY) {
        pax_draw_circle(&fb, blue, x + s * 0.22f, y + s * 0.55f, s * 0.07f);
        pax_draw_circle(&fb, blue, x + s * 0.78f, y + s * 0.55f, s * 0.07f);
    }
    if (marker == EMOJI_THINKING) {
        pax_draw_thick_line(&fb, dark, x + s * 0.52f, y + s * 0.68f, x + s * 0.78f, y + s * 0.55f, s * 0.10f);
        pax_draw_circle(&fb, yellow, x + s * 0.76f, y + s * 0.56f, s * 0.09f);
    }

    if (marker == EMOJI_CRY) {
        pax_draw_line(&fb, dark, x + s * 0.36f, y + s * 0.68f, x + s * 0.64f, y + s * 0.68f);
    } else {
        pax_draw_thick_line(&fb, dark, x + s * 0.32f, y + s * 0.65f, x + s * 0.68f, y + s * 0.65f, s * 0.06f);
        if (marker == EMOJI_GRIN || marker == EMOJI_JOY || marker == EMOJI_HEART_EYES) {
            pax_draw_rect(&fb, 0xFFFFFFFF, x + s * 0.36f, y + s * 0.58f, s * 0.28f, s * 0.08f);
        }
    }
}

static void draw_nl_flag(float x, float y, float s) {
    float flag_w = s * 1.18f;
    if (flag_w > s + 4.0f) flag_w = s + 4.0f;
    float flag_h = s * 0.76f;
    float fx     = x + (s - flag_w) * 0.5f;
    float fy     = y + (s - flag_h) * 0.5f;
    float stripe = flag_h / 3.0f;

    pax_draw_round_rect(&fb, 0xFFFFFFFF, fx, fy, flag_w, flag_h, s * 0.07f);
    pax_draw_rect(&fb, 0xFFAE1C28, fx, fy, flag_w, stripe);
    pax_draw_rect(&fb, 0xFFFFFFFF, fx, fy + stripe, flag_w, stripe);
    pax_draw_rect(&fb, 0xFF21468B, fx, fy + stripe * 2.0f, flag_w, flag_h - stripe * 2.0f);
    pax_outline_rect(&fb, 0xFF202020, fx, fy, flag_w, flag_h);
}

static void draw_generic_flag(float x, float y, float s) {
    float flag_w = s * 1.18f;
    if (flag_w > s + 4.0f) flag_w = s + 4.0f;
    float flag_h = s * 0.76f;
    float fx     = x + (s - flag_w) * 0.5f;
    float fy     = y + (s - flag_h) * 0.5f;

    pax_draw_round_rect(&fb, 0xFFFFFFFF, fx, fy, flag_w, flag_h, s * 0.07f);
    pax_draw_rect(&fb, 0xFF7C7C7C, fx, fy, flag_w, flag_h * 0.5f);
    pax_draw_rect(&fb, 0xFFE7E7E7, fx, fy + flag_h * 0.5f, flag_w, flag_h * 0.5f);
    pax_outline_rect(&fb, 0xFF202020, fx, fy, flag_w, flag_h);
}

static void draw_eu_flag(float x, float y, float s) {
    float flag_w = s * 1.18f;
    if (flag_w > s + 4.0f) flag_w = s + 4.0f;
    float flag_h = s * 0.76f;
    float fx     = x + (s - flag_w) * 0.5f;
    float fy     = y + (s - flag_h) * 0.5f;

    pax_draw_round_rect(&fb, 0xFF003399, fx, fy, flag_w, flag_h, s * 0.07f);
    for (int i = 0; i < 12; i++) {
        float a  = (float)i * 0.523599f;
        float cx = fx + flag_w * (0.5f + cosf(a) * 0.27f);
        float cy = fy + flag_h * (0.5f + sinf(a) * 0.27f);
        pax_draw_circle(&fb, 0xFFFFCC00, cx, cy, s * 0.025f);
    }
    pax_outline_rect(&fb, 0xFF202020, fx, fy, flag_w, flag_h);
}

static void draw_flag_pair(char first, char second, float x, float y, float s) {
    if (first >= 'A' && first <= 'Z' && second >= 'A' && second <= 'Z') {
        char asset_name[8];
        snprintf(asset_name, sizeof(asset_name), "flag_%c%c", (char)(first + ('a' - 'A')), (char)(second + ('a' - 'A')));
        if (try_draw_sd_named_emoji(asset_name, x, y, s)) {
            return;
        }
    }

    if (first == 'N' && second == 'L') {
        draw_nl_flag(x, y, s);
        return;
    }
    if (first == 'E' && second == 'U') {
        draw_eu_flag(x, y, s);
        return;
    }

    draw_generic_flag(x, y, s);
    char label[3] = {first, second, '\0'};
    pax_draw_text(&fb, 0xFF101010, pax_font_sky_mono, s * 0.28f, x + s * 0.23f, y + s * 0.36f, label);
}

static void draw_emoji_icon(uint8_t marker, float x, float y, float s) {
    if (marker == EMOJI_FLAG_NL) {
        draw_nl_flag(x, y, s);
        return;
    }
    if (marker == EMOJI_FLAG_EU) {
        draw_eu_flag(x, y, s);
        return;
    }
    if (marker == EMOJI_FLAG_GENERIC) {
        draw_generic_flag(x, y, s);
        return;
    }

    if (try_draw_sd_emoji(marker, x, y, s)) return;

    pax_col_t dark = 0xFF2A1A00;
    switch (marker) {
        case EMOJI_GENERIC:
        case EMOJI_SMILE:
        case EMOJI_GRIN:
        case EMOJI_JOY:
        case EMOJI_HEART_EYES:
        case EMOJI_COOL:
        case EMOJI_CRY:
        case EMOJI_THINKING:
            draw_emoji_face(x, y, s, marker);
            break;
        case EMOJI_HEART:
            pax_draw_circle(&fb, 0xFFFF3B5C, x + s * 0.35f, y + s * 0.36f, s * 0.17f);
            pax_draw_circle(&fb, 0xFFFF3B5C, x + s * 0.65f, y + s * 0.36f, s * 0.17f);
            pax_draw_tri(&fb, 0xFFFF3B5C, x + s * 0.18f, y + s * 0.43f, x + s * 0.82f, y + s * 0.43f, x + s * 0.50f, y + s * 0.86f);
            break;
        case EMOJI_FIRE:
            pax_draw_tri(&fb, 0xFFFF6B00, x + s * 0.22f, y + s * 0.85f, x + s * 0.50f, y + s * 0.10f, x + s * 0.78f, y + s * 0.85f);
            pax_draw_circle(&fb, 0xFFFF6B00, x + s * 0.50f, y + s * 0.62f, s * 0.25f);
            pax_draw_tri(&fb, 0xFFFFD43B, x + s * 0.36f, y + s * 0.82f, x + s * 0.55f, y + s * 0.38f, x + s * 0.68f, y + s * 0.82f);
            break;
        case EMOJI_THUMBS_UP:
        case EMOJI_THUMBS_DOWN: {
            if (marker == EMOJI_THUMBS_UP) {
                pax_draw_round_rect(&fb, 0xFFFFC27A, x + s * 0.40f, y + s * 0.20f, s * 0.18f, s * 0.38f, s * 0.05f);
                pax_draw_round_rect(&fb, 0xFFFFC27A, x + s * 0.22f, y + s * 0.48f, s * 0.50f, s * 0.25f, s * 0.07f);
                pax_draw_rect(&fb, 0xFF4DA3FF, x + s * 0.16f, y + s * 0.50f, s * 0.12f, s * 0.28f);
            } else {
                pax_draw_round_rect(&fb, 0xFFFFC27A, x + s * 0.40f, y + s * 0.42f, s * 0.18f, s * 0.38f, s * 0.05f);
                pax_draw_round_rect(&fb, 0xFFFFC27A, x + s * 0.22f, y + s * 0.27f, s * 0.50f, s * 0.25f, s * 0.07f);
                pax_draw_rect(&fb, 0xFF4DA3FF, x + s * 0.16f, y + s * 0.22f, s * 0.12f, s * 0.28f);
            }
            break;
        }
        case EMOJI_WAVE:
            pax_draw_circle(&fb, 0xFFFFC27A, x + s * 0.50f, y + s * 0.56f, s * 0.20f);
            for (int i = 0; i < 4; i++) {
                pax_draw_thick_line(&fb, 0xFFFFC27A, x + s * (0.34f + i * 0.09f), y + s * 0.46f, x + s * (0.28f + i * 0.09f), y + s * 0.18f, s * 0.07f);
            }
            pax_draw_line(&fb, dark, x + s * 0.75f, y + s * 0.18f, x + s * 0.90f, y + s * 0.08f);
            pax_draw_line(&fb, dark, x + s * 0.78f, y + s * 0.34f, x + s * 0.96f, y + s * 0.34f);
            break;
        case EMOJI_PRAY:
            pax_draw_thick_line(&fb, 0xFFFFC27A, x + s * 0.42f, y + s * 0.18f, x + s * 0.32f, y + s * 0.74f, s * 0.13f);
            pax_draw_thick_line(&fb, 0xFFFFC27A, x + s * 0.58f, y + s * 0.18f, x + s * 0.68f, y + s * 0.74f, s * 0.13f);
            pax_draw_rect(&fb, 0xFF5B8CFF, x + s * 0.28f, y + s * 0.72f, s * 0.44f, s * 0.14f);
            break;
        case EMOJI_PARTY:
            pax_draw_tri(&fb, 0xFFFFD43B, x + s * 0.28f, y + s * 0.82f, x + s * 0.52f, y + s * 0.16f, x + s * 0.78f, y + s * 0.82f);
            pax_draw_line(&fb, 0xFFFF4D6D, x + s * 0.36f, y + s * 0.58f, x + s * 0.70f, y + s * 0.46f);
            pax_draw_line(&fb, 0xFF33D6A6, x + s * 0.42f, y + s * 0.40f, x + s * 0.62f, y + s * 0.34f);
            pax_draw_circle(&fb, 0xFFFF4D6D, x + s * 0.80f, y + s * 0.18f, s * 0.05f);
            pax_draw_circle(&fb, 0xFF55B7FF, x + s * 0.18f, y + s * 0.28f, s * 0.05f);
            break;
        case EMOJI_ROCKET:
            pax_draw_round_rect(&fb, 0xFFDCE9FF, x + s * 0.38f, y + s * 0.16f, s * 0.24f, s * 0.52f, s * 0.12f);
            pax_draw_tri(&fb, 0xFFFF4D6D, x + s * 0.38f, y + s * 0.22f, x + s * 0.62f, y + s * 0.22f, x + s * 0.50f, y + s * 0.02f);
            pax_draw_circle(&fb, 0xFF55B7FF, x + s * 0.50f, y + s * 0.36f, s * 0.07f);
            pax_draw_tri(&fb, 0xFFFF6B00, x + s * 0.42f, y + s * 0.68f, x + s * 0.58f, y + s * 0.68f, x + s * 0.50f, y + s * 0.96f);
            break;
        case EMOJI_STAR:
            pax_draw_tri(&fb, 0xFFFFD43B, x + s * 0.50f, y + s * 0.08f, x + s * 0.62f, y + s * 0.42f, x + s * 0.98f, y + s * 0.42f);
            pax_draw_tri(&fb, 0xFFFFD43B, x + s * 0.98f, y + s * 0.42f, x + s * 0.68f, y + s * 0.60f, x + s * 0.82f, y + s * 0.92f);
            pax_draw_tri(&fb, 0xFFFFD43B, x + s * 0.82f, y + s * 0.92f, x + s * 0.50f, y + s * 0.70f, x + s * 0.18f, y + s * 0.92f);
            pax_draw_tri(&fb, 0xFFFFD43B, x + s * 0.18f, y + s * 0.92f, x + s * 0.32f, y + s * 0.60f, x + s * 0.02f, y + s * 0.42f);
            pax_draw_tri(&fb, 0xFFFFD43B, x + s * 0.02f, y + s * 0.42f, x + s * 0.38f, y + s * 0.42f, x + s * 0.50f, y + s * 0.08f);
            break;
        case EMOJI_CHECK:
            pax_draw_circle(&fb, 0xFF35D07F, x + s * 0.50f, y + s * 0.50f, s * 0.46f);
            pax_draw_thick_line(&fb, 0xFFFFFFFF, x + s * 0.25f, y + s * 0.52f, x + s * 0.43f, y + s * 0.70f, s * 0.08f);
            pax_draw_thick_line(&fb, 0xFFFFFFFF, x + s * 0.43f, y + s * 0.70f, x + s * 0.76f, y + s * 0.30f, s * 0.08f);
            break;
        case EMOJI_CROSS:
            pax_draw_circle(&fb, 0xFFFF4D5E, x + s * 0.50f, y + s * 0.50f, s * 0.46f);
            pax_draw_thick_line(&fb, 0xFFFFFFFF, x + s * 0.30f, y + s * 0.30f, x + s * 0.70f, y + s * 0.70f, s * 0.08f);
            pax_draw_thick_line(&fb, 0xFFFFFFFF, x + s * 0.70f, y + s * 0.30f, x + s * 0.30f, y + s * 0.70f, s * 0.08f);
            break;
        case EMOJI_HUNDRED:
            pax_draw_text(&fb, 0xFFFF4050, pax_font_sky_mono, s * 0.55f, x + s * 0.02f, y + s * 0.22f, "100");
            pax_draw_thick_line(&fb, 0xFFFF4050, x + s * 0.12f, y + s * 0.82f, x + s * 0.88f, y + s * 0.72f, s * 0.06f);
            break;
        case EMOJI_EYES:
            pax_draw_circle(&fb, 0xFFFFFFFF, x + s * 0.34f, y + s * 0.48f, s * 0.20f);
            pax_draw_circle(&fb, 0xFFFFFFFF, x + s * 0.66f, y + s * 0.48f, s * 0.20f);
            pax_draw_circle(&fb, 0xFF202030, x + s * 0.38f, y + s * 0.48f, s * 0.09f);
            pax_draw_circle(&fb, 0xFF202030, x + s * 0.62f, y + s * 0.48f, s * 0.09f);
            break;
        case EMOJI_ANGRY:
            pax_draw_circle(&fb, 0xFFFF6B3B, x + s * 0.50f, y + s * 0.50f, s * 0.46f);
            pax_draw_line(&fb, dark, x + s * 0.24f, y + s * 0.30f, x + s * 0.43f, y + s * 0.40f);
            pax_draw_line(&fb, dark, x + s * 0.76f, y + s * 0.30f, x + s * 0.57f, y + s * 0.40f);
            pax_draw_circle(&fb, dark, x + s * 0.35f, y + s * 0.45f, s * 0.06f);
            pax_draw_circle(&fb, dark, x + s * 0.65f, y + s * 0.45f, s * 0.06f);
            pax_draw_thick_line(&fb, dark, x + s * 0.34f, y + s * 0.72f, x + s * 0.66f, y + s * 0.66f, s * 0.06f);
            break;
        case EMOJI_SKULL:
            pax_draw_circle(&fb, 0xFFEDEDED, x + s * 0.50f, y + s * 0.42f, s * 0.34f);
            pax_draw_round_rect(&fb, 0xFFEDEDED, x + s * 0.30f, y + s * 0.52f, s * 0.40f, s * 0.30f, s * 0.06f);
            pax_draw_circle(&fb, dark, x + s * 0.38f, y + s * 0.42f, s * 0.08f);
            pax_draw_circle(&fb, dark, x + s * 0.62f, y + s * 0.42f, s * 0.08f);
            pax_draw_tri(&fb, dark, x + s * 0.50f, y + s * 0.50f, x + s * 0.43f, y + s * 0.62f, x + s * 0.57f, y + s * 0.62f);
            pax_draw_line(&fb, dark, x + s * 0.38f, y + s * 0.75f, x + s * 0.62f, y + s * 0.75f);
            break;
        case EMOJI_SUN:
            pax_draw_circle(&fb, 0xFFFFD43B, x + s * 0.50f, y + s * 0.50f, s * 0.24f);
            for (int i = 0; i < 8; i++) {
                float a = (float)i * 0.785398f;
                float sx = x + s * (0.50f + cosf(a) * 0.34f);
                float sy = y + s * (0.50f + sinf(a) * 0.34f);
                float ex = x + s * (0.50f + cosf(a) * 0.46f);
                float ey = y + s * (0.50f + sinf(a) * 0.46f);
                pax_draw_thick_line(&fb, 0xFFFFD43B, sx, sy, ex, ey, s * 0.05f);
            }
            break;
        case EMOJI_MOON:
            pax_draw_circle(&fb, 0xFFECE7B8, x + s * 0.52f, y + s * 0.48f, s * 0.36f);
            pax_draw_circle(&fb, BLACK, x + s * 0.66f, y + s * 0.38f, s * 0.34f);
            break;
        case EMOJI_BOLT:
            pax_draw_tri(&fb, 0xFFFFD43B, x + s * 0.58f, y + s * 0.04f, x + s * 0.24f, y + s * 0.54f, x + s * 0.52f, y + s * 0.50f);
            pax_draw_tri(&fb, 0xFFFFD43B, x + s * 0.42f, y + s * 0.96f, x + s * 0.78f, y + s * 0.42f, x + s * 0.48f, y + s * 0.48f);
            break;
        case EMOJI_GIFT:
            pax_draw_rect(&fb, 0xFFFF4D6D, x + s * 0.18f, y + s * 0.42f, s * 0.64f, s * 0.40f);
            pax_draw_rect(&fb, 0xFFFFD43B, x + s * 0.46f, y + s * 0.34f, s * 0.08f, s * 0.48f);
            pax_draw_rect(&fb, 0xFFFFD43B, x + s * 0.16f, y + s * 0.48f, s * 0.68f, s * 0.08f);
            pax_draw_circle(&fb, 0xFFFFD43B, x + s * 0.38f, y + s * 0.30f, s * 0.11f);
            pax_draw_circle(&fb, 0xFFFFD43B, x + s * 0.62f, y + s * 0.30f, s * 0.11f);
            break;
        case EMOJI_MUSIC:
            pax_draw_circle(&fb, 0xFF8B7CFF, x + s * 0.32f, y + s * 0.76f, s * 0.12f);
            pax_draw_circle(&fb, 0xFF8B7CFF, x + s * 0.68f, y + s * 0.66f, s * 0.12f);
            pax_draw_thick_line(&fb, 0xFF8B7CFF, x + s * 0.42f, y + s * 0.22f, x + s * 0.42f, y + s * 0.76f, s * 0.06f);
            pax_draw_thick_line(&fb, 0xFF8B7CFF, x + s * 0.78f, y + s * 0.14f, x + s * 0.78f, y + s * 0.66f, s * 0.06f);
            pax_draw_thick_line(&fb, 0xFF8B7CFF, x + s * 0.42f, y + s * 0.22f, x + s * 0.78f, y + s * 0.14f, s * 0.06f);
            break;
        case EMOJI_PIZZA:
            pax_draw_tri(&fb, 0xFFFFC45C, x + s * 0.18f, y + s * 0.18f, x + s * 0.86f, y + s * 0.30f, x + s * 0.36f, y + s * 0.88f);
            pax_draw_thick_line(&fb, 0xFFB76A28, x + s * 0.18f, y + s * 0.18f, x + s * 0.86f, y + s * 0.30f, s * 0.07f);
            pax_draw_circle(&fb, 0xFFFF4D4D, x + s * 0.46f, y + s * 0.42f, s * 0.06f);
            pax_draw_circle(&fb, 0xFFFF4D4D, x + s * 0.62f, y + s * 0.34f, s * 0.05f);
            pax_draw_circle(&fb, 0xFFFF4D4D, x + s * 0.42f, y + s * 0.62f, s * 0.05f);
            break;
        case EMOJI_COFFEE:
            pax_draw_round_rect(&fb, 0xFFFFFFFF, x + s * 0.24f, y + s * 0.44f, s * 0.46f, s * 0.28f, s * 0.06f);
            pax_outline_circle(&fb, 0xFFFFFFFF, x + s * 0.72f, y + s * 0.54f, s * 0.12f);
            pax_draw_rect(&fb, 0xFF7A4A24, x + s * 0.30f, y + s * 0.48f, s * 0.34f, s * 0.10f);
            pax_draw_line(&fb, 0xFFFFFFFF, x + s * 0.36f, y + s * 0.32f, x + s * 0.32f, y + s * 0.18f);
            pax_draw_line(&fb, 0xFFFFFFFF, x + s * 0.50f, y + s * 0.32f, x + s * 0.46f, y + s * 0.18f);
            break;
        default:
            draw_emoji_face(x, y, s, EMOJI_GENERIC);
            break;
    }
}

static void draw_text_with_emoji(pax_col_t color, float x, float y, const char *text) {
    char  segment[WRAP_LINE_LEN];
    int   seg_len = 0;
    float cursor  = x;
    float icon_s  = FONT_SIZE * 1.55f;
    if (icon_s > g_line_h - 1.0f) icon_s = g_line_h - 1.0f;
    if (icon_s < 14.0f) icon_s = 14.0f;

    for (size_t i = 0; text != NULL && text[i] != '\0'; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (is_emoji_marker(ch)) {
            uint8_t marker = ch;
            char flag_first = '\0';
            char flag_second = '\0';
            if (ch == EMOJI_EXT) {
                if (text[i + 1] == '\0') break;
                marker = (uint8_t)text[++i];
                if (marker == EMOJI_FLAG_PAIR) {
                    if (text[i + 1] == '\0' || text[i + 2] == '\0') break;
                    flag_first  = text[++i];
                    flag_second = text[++i];
                }
            }
            if (seg_len > 0) {
                segment[seg_len] = '\0';
                pax_draw_text(&fb, color, pax_font_sky_mono, FONT_SIZE, cursor, y, segment);
                cursor += pax_text_size(pax_font_sky_mono, FONT_SIZE, segment).x;
                seg_len = 0;
            }
            if (marker == EMOJI_FLAG_PAIR) {
                draw_flag_pair(flag_first, flag_second, cursor, y + (g_line_h - icon_s) * 0.28f, icon_s);
            } else {
                draw_emoji_icon(marker, cursor, y + (g_line_h - icon_s) * 0.28f, icon_s);
            }
            cursor += icon_s + 3.0f;
        } else {
            if (seg_len < (int)sizeof(segment) - 1) {
                segment[seg_len++] = (char)ch;
            }
        }
    }
    if (seg_len > 0) {
        segment[seg_len] = '\0';
        pax_draw_text(&fb, color, pax_font_sky_mono, FONT_SIZE, cursor, y, segment);
    }
}

static void draw_compose_box(float x, float y, float w, float h, bool encrypted) {
    if (encrypted) {
        draw_box(x, y, w, h, TERM_RED, "input disabled");
        pax_draw_text(
            &fb, TERM_RED, pax_font_sky_mono, FONT_SIZE, x + 12, y + g_line_h * 0.75f,
            "Encrypted rooms are not supported for sending in this client."
        );
        return;
    }

    draw_box(x, y, w, h, TERM_GREEN, "message");
    char display_compose[sizeof(compose_buffer) + 32];
    char line[sizeof(compose_buffer) + 40];
    char wrapped[6][WRAP_LINE_LEN];
    text_with_emoji_markers(compose_buffer, display_compose, sizeof(display_compose));
    snprintf(line, sizeof(line), "> ");
    strncat(line, display_compose, sizeof(line) - strlen(line) - 2);
    strncat(line, "_", sizeof(line) - strlen(line) - 1);

    int max_cols = (int)((w - 24) / g_char_w);
    if (max_cols < 8) max_cols = 8;
    int lines = wrap_text(line, max_cols, wrapped, 6);
    int visible = (int)((h - g_line_h * 0.8f) / g_line_h);
    if (visible < 1) visible = 1;
    if (visible > 6) visible = 6;
    int start = lines > visible ? lines - visible : 0;
    float ty = y + g_line_h * 0.55f;
    for (int i = start; i < lines; i++) {
        draw_text_with_emoji(TERM_FG, x + 12, ty, wrapped[i]);
        ty += g_line_h;
    }
}

static void truncate_utf8_last(char *text) {
    size_t len = strlen(text);
    if (len == 0) return;
    len--;
    while (len > 0 && (((unsigned char)text[len] & 0xC0) == 0x80)) {
        len--;
    }
    text[len] = '\0';
}

static void append_keyboard_text(char *text, size_t cap, bsp_input_event_t *event) {
    const char *piece = event->args_keyboard.utf8;
    char        ascii_piece[2];
    if (piece == NULL || piece[0] == '\0') {
        unsigned char ascii = (unsigned char)event->args_keyboard.ascii;
        if (ascii < 0x20 || ascii == 0x7F) return;
        ascii_piece[0] = (char)ascii;
        ascii_piece[1] = '\0';
        piece          = ascii_piece;
    }

    size_t len       = strlen(text);
    size_t piece_len = strlen(piece);
    if (piece_len == 0 || len + piece_len >= cap) return;
    memcpy(text + len, piece, piece_len + 1);
}

static void append_text_piece(char *text, size_t cap, const char *piece) {
    size_t len       = strlen(text);
    size_t piece_len = piece ? strlen(piece) : 0;
    if (piece_len == 0 || len + piece_len >= cap) return;
    memcpy(text + len, piece, piece_len + 1);
}

/* -------------------------------------------------------------------------- */
/* Preferences                                                                 */
/* -------------------------------------------------------------------------- */

static void load_saved_account(void) {
    nvs_handle_t nvs;
    esp_err_t    res = nvs_open("matrix_ui", NVS_READONLY, &nvs);
    if (res != ESP_OK) return;

    uint8_t remember = 1;
    if (nvs_get_u8(nvs, "remember", &remember) == ESP_OK) {
        remember_account = remember != 0;
    }
    uint8_t remember_pw = 0;
    if (nvs_get_u8(nvs, "remember_pw", &remember_pw) == ESP_OK) {
        remember_password = remember_pw != 0;
    }
    int32_t stored_theme = 0;
    if (nvs_get_i32(nvs, "theme", &stored_theme) == ESP_OK) {
        theme_index = (int)stored_theme;
    }
    int32_t stored_font = 1;
    if (nvs_get_i32(nvs, "font_size", &stored_font) == ESP_OK) {
        font_size_index = (int)stored_font;
    }
    uint8_t show_all = 0;
    if (nvs_get_u8(nvs, "show_all", &show_all) == ESP_OK) {
        room_list_show_all = show_all != 0;
    }
    uint8_t debug = 0;
    if (nvs_get_u8(nvs, "debug_status", &debug) == ESP_OK) {
        debug_status = debug != 0;
    }

    if (remember_account) {
        size_t len = sizeof(login_homeserver);
        nvs_get_str(nvs, "homeserver", login_homeserver, &len);
        len = sizeof(login_username);
        nvs_get_str(nvs, "username", login_username, &len);
        if (remember_password) {
            len = sizeof(login_password);
            nvs_get_str(nvs, "password", login_password, &len);
        }
    }

    nvs_close(nvs);
    apply_theme();
}

static void save_account_settings(void) {
    nvs_handle_t nvs;
    esp_err_t    res = nvs_open("matrix_ui", NVS_READWRITE, &nvs);
    if (res != ESP_OK) return;

    nvs_set_u8(nvs, "remember", remember_account ? 1 : 0);
    nvs_set_u8(nvs, "remember_pw", remember_password ? 1 : 0);
    nvs_set_u8(nvs, "show_all", room_list_show_all ? 1 : 0);
    nvs_set_u8(nvs, "debug_status", debug_status ? 1 : 0);
    nvs_set_i32(nvs, "theme", theme_index);
    nvs_set_i32(nvs, "font_size", font_size_index);
    if (remember_account) {
        nvs_set_str(nvs, "homeserver", login_homeserver);
        nvs_set_str(nvs, "username", login_username);
        if (remember_password) {
            nvs_set_str(nvs, "password", login_password);
        } else {
            nvs_erase_key(nvs, "password");
        }
    } else {
        nvs_erase_key(nvs, "homeserver");
        nvs_erase_key(nvs, "username");
        nvs_erase_key(nvs, "password");
    }
    nvs_commit(nvs);
    nvs_close(nvs);
}

static void clear_saved_account(void) {
    nvs_handle_t nvs;
    esp_err_t    res = nvs_open("matrix_ui", NVS_READWRITE, &nvs);
    if (res == ESP_OK) {
        nvs_erase_key(nvs, "homeserver");
        nvs_erase_key(nvs, "username");
        nvs_erase_key(nvs, "password");
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    snprintf(login_homeserver, sizeof(login_homeserver), "matrix.org");
    login_username[0] = '\0';
    login_password[0] = '\0';
    matrix_clear_persisted_session();
}

/* -------------------------------------------------------------------------- */
/* Login screen                                                                */
/* -------------------------------------------------------------------------- */

static char *login_field_buffer(login_field_t field) {
    switch (field) {
        case LOGIN_FIELD_USERNAME: return login_username;
        case LOGIN_FIELD_PASSWORD: return login_password;
        case LOGIN_FIELD_HOMESERVER:
        default: return login_homeserver;
    }
}

static size_t login_field_capacity(login_field_t field) {
    switch (field) {
        case LOGIN_FIELD_USERNAME: return sizeof(login_username);
        case LOGIN_FIELD_PASSWORD: return sizeof(login_password);
        case LOGIN_FIELD_HOMESERVER:
        default: return sizeof(login_homeserver);
    }
}

typedef struct {
    char homeserver[sizeof(login_homeserver)];
    char username[sizeof(login_username)];
    char password[sizeof(login_password)];
} login_task_args_t;

static void login_task(void *arg) {
    login_task_args_t *args = (login_task_args_t *)arg;
    char                err[sizeof(login_error)] = "";

    matrix_set_persistence_enabled(remember_password);
    esp_err_t res = matrix_login(args->homeserver, args->username, args->password, err, sizeof(err));
    free(args);

    if (res == ESP_OK) {
        save_account_settings();
        matrix_start_sync();
        room_selected = 0;
        room_scroll   = 0;
        screen        = APP_SCREEN_ROOM_LIST;
    } else {
        snprintf(login_error, sizeof(login_error), "%s", err);
    }
    login_in_progress = false;
    vTaskDelete(NULL);
}

static void start_login(void) {
    if (login_in_progress) return;
    if (login_homeserver[0] == '\0' || login_username[0] == '\0' || login_password[0] == '\0') {
        snprintf(login_error, sizeof(login_error), "Please fill in all fields");
        return;
    }

    login_error[0]    = '\0';
    login_in_progress = true;

    login_task_args_t *args = calloc(1, sizeof(login_task_args_t));
    if (args == NULL) {
        login_in_progress = false;
        return;
    }
    snprintf(args->homeserver, sizeof(args->homeserver), "%s", login_homeserver);
    snprintf(args->username, sizeof(args->username), "%s", login_username);
    snprintf(args->password, sizeof(args->password), "%s", login_password);
    xTaskCreate(login_task, "matrix_login", 8192, args, 5, NULL);
}

static bool handle_input_login(bsp_input_event_t *event) {
    if (login_in_progress) return false;

    if (event->type == INPUT_EVENT_TYPE_NAVIGATION && event->args_navigation.state) {
        switch (event->args_navigation.key) {
            case BSP_INPUT_NAVIGATION_KEY_TAB:
            case BSP_INPUT_NAVIGATION_KEY_DOWN:
                login_focus = (login_focus + 1) % LOGIN_FIELD_COUNT;
                return true;
            case BSP_INPUT_NAVIGATION_KEY_UP:
                login_focus = (login_focus + LOGIN_FIELD_COUNT - 1) % LOGIN_FIELD_COUNT;
                return true;
            case BSP_INPUT_NAVIGATION_KEY_BACKSPACE: {
                char  *buf = login_field_buffer(login_focus);
                truncate_utf8_last(buf);
                return true;
            }
            case BSP_INPUT_NAVIGATION_KEY_RETURN:
                start_login();
                return true;
            default:
                break;
        }
        return false;
    }

    if (event->type == INPUT_EVENT_TYPE_KEYBOARD) {
        char ascii = event->args_keyboard.ascii;
        if (ascii == '\b' || ascii == '\t') {
            return false;  // handled via navigation events instead
        }
        if (ascii == '\r' || ascii == '\n') {
            start_login();
            return true;
        }
        if ((unsigned char)ascii >= 0x20 || (event->args_keyboard.utf8 != NULL && event->args_keyboard.utf8[0] != '\0')) {
            char  *buf = login_field_buffer(login_focus);
            size_t cap = login_field_capacity(login_focus);
            append_keyboard_text(buf, cap, event);
            return true;
        }
    }
    return false;
}

static void render_login(void) {
    pax_background(&fb, BLACK);
    float w = pax_buf_get_widthf(&fb);
    float h = pax_buf_get_heightf(&fb);

    pax_draw_text(&fb, TERM_GREEN, pax_font_sky_mono, TITLE_FONT_SIZE, 16, 16, "TANMATSU MATRIX CLIENT");

    float box_x = 16, box_y = 16 + g_line_h * 2.5f;
    float box_w = w - 32;
    float box_h = g_line_h * 6;
    draw_box(box_x, box_y, box_w, box_h, TERM_GREEN, "login");

    static const char *labels[LOGIN_FIELD_COUNT] = {"Homeserver:", "Username:  ", "Password:  "};
    float               fx                       = box_x + 24;
    float               fy                       = box_y + g_line_h;

    for (int i = 0; i < LOGIN_FIELD_COUNT; i++) {
        pax_col_t   color  = (login_focus == i) ? TERM_GREEN : TERM_FG;
        const char *cursor = (login_focus == i) ? "_" : "";
        char        line[192];
        if (i == LOGIN_FIELD_PASSWORD) {
            char   masked[sizeof(login_password)];
            size_t len = strlen(login_password);
            if (len >= sizeof(masked)) len = sizeof(masked) - 1;
            memset(masked, '*', len);
            masked[len] = '\0';
            snprintf(line, sizeof(line), "%s %s%s", labels[i], masked, cursor);
        } else {
            snprintf(line, sizeof(line), "%s %s%s", labels[i], login_field_buffer((login_field_t)i), cursor);
        }
        pax_draw_text(&fb, color, pax_font_sky_mono, FONT_SIZE, fx, fy, line);
        fy += g_line_h * 1.6f;
    }

    if (login_in_progress) {
        pax_draw_text(&fb, TERM_GREEN, pax_font_sky_mono, FONT_SIZE, fx, fy, "Logging in...");
    } else if (login_error[0] != '\0') {
        pax_draw_text(&fb, TERM_RED, pax_font_sky_mono, FONT_SIZE, fx, fy, login_error);
    }

    bool wifi_ok = wifi_connection_is_connected();
    char status[64];
    snprintf(status, sizeof(status), "WiFi: %s", wifi_ok ? "connected" : "not connected");
    pax_draw_text(&fb, wifi_ok ? TERM_DIM : TERM_RED, pax_font_sky_mono, FONT_SIZE, 16, h - g_line_h * 2, status);
    pax_draw_text(
        &fb, TERM_DIM, pax_font_sky_mono, FONT_SIZE, 16, h - g_line_h,
        "TAB/UP/DOWN switch field   ENTER log in   ESC menu"
    );

    blit();
}

/* -------------------------------------------------------------------------- */
/* Room list screen                                                            */
/* -------------------------------------------------------------------------- */

static bool room_visible(matrix_room_t *room) {
    return room != NULL && (room_list_show_all || room->favorite);
}

static int room_visible_count_locked(void) {
    int visible = 0;
    int total   = matrix_room_count();
    for (int i = 0; i < total; i++) {
        if (room_visible(matrix_get_room(i))) visible++;
    }
    return visible;
}

static int room_visible_to_actual_index_locked(int visible_index) {
    int visible = 0;
    int total   = matrix_room_count();
    for (int i = 0; i < total; i++) {
        if (!room_visible(matrix_get_room(i))) continue;
        if (visible == visible_index) return i;
        visible++;
    }
    return -1;
}

static bool handle_input_room_list(bsp_input_event_t *event) {
    if (event->type != INPUT_EVENT_TYPE_NAVIGATION || !event->args_navigation.state) return false;

    matrix_lock();
    int count = room_visible_count_locked();
    matrix_unlock();
    switch (event->args_navigation.key) {
        case BSP_INPUT_NAVIGATION_KEY_UP:
            if (room_selected > 0) room_selected--;
            return true;
        case BSP_INPUT_NAVIGATION_KEY_DOWN:
            if (room_selected < count - 1) room_selected++;
            return true;
        case BSP_INPUT_NAVIGATION_KEY_RETURN:
            if (count > 0) {
                chat_room_id[0]    = '\0';
                compose_buffer[0]  = '\0';
                chat_scroll_offset = 0;
                chat_selected_message = -1;
                matrix_lock();
                int actual_index = room_visible_to_actual_index_locked(room_selected);
                chat_room_index = actual_index;
                matrix_room_t *room = matrix_get_room(actual_index);
                bool fetch_history = false;
                if (room != NULL) {
                    snprintf(chat_room_id, sizeof(chat_room_id), "%s", room->room_id);
                    room->unread = false;
                    room->unread_count = 0;
                    fetch_history = room->favorite;
                }
                matrix_unlock();
                if (actual_index >= 0 && fetch_history) {
                    matrix_fetch_room_history(actual_index);
                }
                screen = APP_SCREEN_CHAT;
            }
            return true;
        default:
            return false;
    }
}

static void render_room_list(void) {
    pax_background(&fb, BLACK);
    float w = pax_buf_get_widthf(&fb);
    float h = pax_buf_get_heightf(&fb);

    char header[192];
    snprintf(header, sizeof(header), "%s - %s", room_list_show_all ? "All rooms" : "Favorites", matrix_get_user_id());
    pax_draw_text(&fb, TERM_GREEN, pax_font_sky_mono, TITLE_FONT_SIZE, 16, 12, header);

    float list_y = 12 + g_line_h * 2.5f;
    float list_h = h - list_y - (debug_status ? g_line_h * 2 : g_line_h * 0.5f);
    draw_box(16, list_y, w - 32, list_h, TERM_GREEN, NULL);

    matrix_lock();
    int total_count = matrix_room_count();
    int count = room_visible_count_locked();
    if (room_selected >= count) room_selected = count > 0 ? count - 1 : 0;
    if (room_selected < 0) room_selected = 0;

    if (count == 0) {
        const char *empty_text = room_list_show_all || total_count == 0
                                     ? "(no rooms yet, waiting for sync...)"
                                     : "(no favorites yet, enable all rooms in Settings)";
        pax_draw_text(
            &fb, TERM_DIM, pax_font_sky_mono, FONT_SIZE, 32, list_y + g_line_h, empty_text
        );
    } else {
        int visible_rows = (int)((list_h - g_line_h) / g_line_h);
        if (visible_rows < 1) visible_rows = 1;
        if (room_selected < room_scroll) room_scroll = room_selected;
        if (room_selected >= room_scroll + visible_rows) room_scroll = room_selected - visible_rows + 1;

        for (int row = 0; row < visible_rows; row++) {
            int visible_idx = room_scroll + row;
            if (visible_idx >= count) break;
            int idx = room_visible_to_actual_index_locked(visible_idx);
            matrix_room_t *room = matrix_get_room(idx);
            if (room == NULL) continue;

            float     ry       = list_y + g_line_h * (row + 1);
            bool      selected = (visible_idx == room_selected);
            pax_col_t color    = selected ? TERM_GREEN : TERM_FG;
            if (selected) {
                pax_simple_rect(&fb, TERM_SELECT_BG, 24, ry - 2, w - 64, g_line_h);
            }
            const char *name   = (room->has_name && room->name[0] != '\0') ? room->name : room->room_id;
            const char *prefix = selected ? "> " : (room->unread ? "* " : "  ");
            char        display_name[128];
            char        line[160];
            char        unread_suffix[16] = "";
            text_with_emoji_markers(name, display_name, sizeof(display_name));
            if (room->unread_count > 0) {
                snprintf(unread_suffix, sizeof(unread_suffix), " (%u)", (unsigned int)room->unread_count);
            }
            snprintf(line, sizeof(line), "%s%s%s %s%s", prefix, room->favorite ? "[*]" : "   ", room->encrypted ? "[E]" : "   ", display_name, unread_suffix);
            draw_text_with_emoji(color, 32, ry, line);
        }
    }
    matrix_unlock();

    if (debug_status) {
        matrix_sync_stats_t stats;
        matrix_get_sync_stats(&stats);
        char progress[96];
        snprintf(progress, sizeof(progress), "rooms %" PRIu32 "  names %" PRIu32 "/%" PRIu32 "  sync %" PRIu32,
                 stats.joined_rooms, stats.named_rooms, stats.joined_rooms, stats.sync_count);
        float progress_w = pax_text_size(pax_font_sky_mono, FONT_SIZE, progress).x;
        float progress_x = w - progress_w - 16;
        if (progress_x < 16) progress_x = 16;
        pax_draw_text(&fb, TERM_DIM, pax_font_sky_mono, FONT_SIZE, progress_x, 12, progress);
        draw_matrix_status(16, h - g_line_h * 2);
    }

    blit();
}

/* -------------------------------------------------------------------------- */
/* Chat screen                                                                 */
/* -------------------------------------------------------------------------- */

static int chat_message_count_locked(matrix_room_t *room) {
    int history_count = matrix_active_history_count(chat_room_id);
    return history_count > 0 ? history_count : (room != NULL ? room->message_count : 0);
}

static matrix_message_t *chat_message_at_locked(matrix_room_t *room, int index) {
    int history_count = matrix_active_history_count(chat_room_id);
    return history_count > 0 ? matrix_get_active_history_message(chat_room_id, index)
                             : (room != NULL && index >= 0 && index < room->message_count ? &room->messages[index] : NULL);
}

static bool get_selected_event_id(char *event_id, size_t event_id_len) {
    if (event_id == NULL || event_id_len == 0) return false;
    event_id[0] = '\0';
    if (chat_room_id[0] == '\0' || chat_selected_message < 0) return false;

    matrix_lock();
    int room_index = matrix_find_room_index_by_id(chat_room_id);
    matrix_room_t *room = matrix_get_room(room_index);
    matrix_message_t *msg = chat_message_at_locked(room, chat_selected_message);
    if (msg != NULL && msg->event_id[0] != '\0') {
        snprintf(event_id, event_id_len, "%s", msg->event_id);
    }
    matrix_unlock();
    return event_id[0] != '\0';
}

static void image_decode_task(void *arg) {
    image_decode_request_t *req = (image_decode_request_t *)arg;
    if (req == NULL) {
        image_decode_busy = false;
        image_decode_ready = true;
        vTaskDelete(NULL);
        return;
    }

    pax_buf_t decoded = {0};
    char error[sizeof(viewed_image_error)] = "";
    esp_err_t res = image_viewer_decode(req->data, req->data_len, req->mimetype, &decoded, error, sizeof(error));
    heap_caps_free(req->data);

    if (res == ESP_OK) {
        viewed_image = decoded;
        viewed_image_valid = true;
        viewed_image_error[0] = '\0';
    } else {
        viewed_image_valid = false;
        memset(&viewed_image, 0, sizeof(viewed_image));
        snprintf(viewed_image_error, sizeof(viewed_image_error), "%.80s", error[0] != '\0' ? error : esp_err_to_name(res));
    }
    snprintf(viewed_image_label, sizeof(viewed_image_label), "%.383s", req->label[0] != '\0' ? req->label : "image");
    free(req);
    image_decode_busy = false;
    image_decode_ready = true;
    vTaskDelete(NULL);
}

static bool open_cached_image(const char *event_id) {
    uint8_t *data = NULL;
    size_t data_len = 0;
    char mimetype[MATRIX_MEDIA_MIMETYPE_LEN] = "";
    char label[MATRIX_BODY_LEN] = "";
    if (matrix_copy_cached_image(event_id, &data, &data_len, mimetype, sizeof(mimetype), label, sizeof(label)) != ESP_OK) {
        return false;
    }

    if (image_decode_busy) {
        heap_caps_free(data);
        return true;
    }
    if (viewed_image_valid) {
        image_viewer_destroy(&viewed_image);
        viewed_image_valid = false;
    }
    snprintf(viewed_image_label, sizeof(viewed_image_label), "%s", label[0] != '\0' ? label : "image");
    snprintf(viewed_image_error, sizeof(viewed_image_error), "decoding image");
    image_decode_request_t *req = calloc(1, sizeof(*req));
    if (req == NULL) {
        heap_caps_free(data);
        snprintf(viewed_image_error, sizeof(viewed_image_error), "image no mem");
        screen = APP_SCREEN_IMAGE_VIEWER;
        return true;
    }
    req->data = data;
    req->data_len = data_len;
    snprintf(req->mimetype, sizeof(req->mimetype), "%s", mimetype);
    snprintf(req->label, sizeof(req->label), "%s", label[0] != '\0' ? label : "image");
    pending_image_event_id[0] = '\0';
    image_decode_busy = true;
    image_decode_ready = false;
    screen = APP_SCREEN_IMAGE_VIEWER;
    BaseType_t ok = xTaskCreate(image_decode_task, "image_decode", 32768, req, 4, NULL);
    if (ok != pdPASS) {
        image_decode_busy = false;
        image_decode_ready = true;
        heap_caps_free(req->data);
        free(req);
        snprintf(viewed_image_error, sizeof(viewed_image_error), "image task failed");
    }
    return true;
}

static void request_selected_media(void) {
    char event_id[MATRIX_EVENT_ID_LEN] = "";
    if (!get_selected_event_id(event_id, sizeof(event_id))) return;

    if (open_cached_image(event_id)) return;
    snprintf(pending_image_event_id, sizeof(pending_image_event_id), "%s", event_id);
    if (matrix_request_media_download(chat_room_id, event_id) != ESP_OK) {
        pending_image_event_id[0] = '\0';
    }
}

static void maybe_open_pending_image(void) {
    if (screen != APP_SCREEN_CHAT || pending_image_event_id[0] == '\0') return;
    const char *status = matrix_get_audio_status(pending_image_event_id);
    if (status != NULL && strcmp(status, "image ready") == 0) {
        open_cached_image(pending_image_event_id);
    } else if (status != NULL && (strcmp(status, "not media") == 0 || strstr(status, "failed") != NULL)) {
        pending_image_event_id[0] = '\0';
    }
}

/* Video playback runs synchronously inside matrix_client's background download
 * task (see media_download_task), blitting frames straight to the display. The
 * UI only needs to know when to get out of the way (switch screens) and when
 * to come back, which it learns by polling the same audio_status text used for
 * images/audio. */
static void maybe_start_pending_video(void) {
    if (screen != APP_SCREEN_CHAT || pending_image_event_id[0] == '\0') return;
    const char *status = matrix_get_audio_status(pending_image_event_id);
    if (status == NULL) return;
    if (strcmp(status, "video: playing") == 0) {
        snprintf(active_video_event_id, sizeof(active_video_event_id), "%s", pending_image_event_id);
        pending_image_event_id[0] = '\0';
        video_screen_cleared = false;
        screen = APP_SCREEN_VIDEO_PLAYER;
    } else if (strcmp(status, "not media") == 0 || strstr(status, "failed") != NULL) {
        pending_image_event_id[0] = '\0';
    }
}

static void maybe_finish_active_video(void) {
    if (screen != APP_SCREEN_VIDEO_PLAYER || active_video_event_id[0] == '\0') return;
    const char *status = matrix_get_audio_status(active_video_event_id);
    if (status != NULL && strcmp(status, "video: playing") != 0) {
        active_video_event_id[0] = '\0';
        screen = APP_SCREEN_CHAT;
    }
}

static void send_current_message(void) {
    if (chat_room_id[0] == '\0' || compose_buffer[0] == '\0') return;

    bool encrypted = false;
    int  room_index = -1;
    matrix_lock();
    room_index = matrix_find_room_index_by_id(chat_room_id);
    matrix_room_t *room = matrix_get_room(room_index);
    encrypted            = room ? room->encrypted : false;
    chat_room_index      = room_index;
    matrix_unlock();

    if (room_index < 0 || encrypted) return;

    matrix_send_message(room_index, compose_buffer);
    compose_buffer[0]  = '\0';
    chat_scroll_offset = 0;
    chat_selected_message = -1;
}

static bool handle_input_chat(bsp_input_event_t *event) {
    if (event->type == INPUT_EVENT_TYPE_NAVIGATION && event->args_navigation.state) {
        switch (event->args_navigation.key) {
            case BSP_INPUT_NAVIGATION_KEY_ESC:
                screen = APP_SCREEN_ROOM_LIST;
                return true;
            case BSP_INPUT_NAVIGATION_KEY_BACKSPACE: {
                truncate_utf8_last(compose_buffer);
                render_chat_input_only();
                return false;
            }
            case BSP_INPUT_NAVIGATION_KEY_UP: {
                matrix_lock();
                int room_index = matrix_find_room_index_by_id(chat_room_id);
                matrix_room_t *room = matrix_get_room(room_index);
                int count = chat_message_count_locked(room);
                if (count > 0) {
                    if (chat_selected_message < 0 || chat_selected_message >= count) chat_selected_message = count - 1;
                    else if (chat_selected_message > 0) chat_selected_message--;
                }
                matrix_unlock();
                return true;
            }
            case BSP_INPUT_NAVIGATION_KEY_DOWN: {
                matrix_lock();
                int room_index = matrix_find_room_index_by_id(chat_room_id);
                matrix_room_t *room = matrix_get_room(room_index);
                int count = chat_message_count_locked(room);
                if (count > 0) {
                    if (chat_selected_message < 0 || chat_selected_message >= count) chat_selected_message = count - 1;
                    else if (chat_selected_message < count - 1) chat_selected_message++;
                }
                matrix_unlock();
                return true;
            }
            case BSP_INPUT_NAVIGATION_KEY_PGUP:
                matrix_lock();
                {
                    int room_index = matrix_find_room_index_by_id(chat_room_id);
                    matrix_room_t *room = matrix_get_room(room_index);
                    int count = chat_message_count_locked(room);
                    if (count > 0) {
                        if (chat_selected_message < 0 || chat_selected_message >= count) chat_selected_message = count - 1;
                        chat_selected_message -= 5;
                        if (chat_selected_message < 0) chat_selected_message = 0;
                    }
                }
                matrix_unlock();
                return true;
            case BSP_INPUT_NAVIGATION_KEY_PGDN:
                matrix_lock();
                {
                    int room_index = matrix_find_room_index_by_id(chat_room_id);
                    matrix_room_t *room = matrix_get_room(room_index);
                    int count = chat_message_count_locked(room);
                    if (count > 0) {
                        if (chat_selected_message < 0 || chat_selected_message >= count) chat_selected_message = count - 1;
                        else {
                            chat_selected_message += 5;
                            if (chat_selected_message >= count) chat_selected_message = count - 1;
                        }
                    }
                }
                matrix_unlock();
                return true;
            case BSP_INPUT_NAVIGATION_KEY_RETURN:
                if (compose_buffer[0] != '\0') send_current_message();
                else request_selected_media();
                return true;
            default:
                return false;
        }
    }

    if (event->type == INPUT_EVENT_TYPE_KEYBOARD) {
        char ascii = event->args_keyboard.ascii;
        if (ascii == '\b' || ascii == '\t') {
            return false;
        }
        if (ascii == '\r' || ascii == '\n') {
            if (compose_buffer[0] != '\0') send_current_message();
            else request_selected_media();
            return true;
        }
        if ((unsigned char)ascii >= 0x20 || (event->args_keyboard.utf8 != NULL && event->args_keyboard.utf8[0] != '\0')) {
            append_keyboard_text(compose_buffer, sizeof(compose_buffer), event);
            render_chat_input_only();
            return false;
        }
    }
    return false;
}

static void render_chat_frame(bool do_blit) {
    pax_background(&fb, BLACK);
    float w = pax_buf_get_widthf(&fb);
    float h = pax_buf_get_heightf(&fb);

    float messages_y = debug_status ? (12 + g_line_h * 2.5f) : (12 + g_line_h * 1.25f);
    float input_h    = g_line_h * 4.2f;
    float gap        = 8.0f;
    float bottom_pad = 10.0f;
    float messages_h = h - messages_y - input_h - gap - bottom_pad;

    int max_cols = (int)((w - 64) / g_char_w);
    if (max_cols < 10) max_cols = 10;

    char title[192]     = "";
    bool room_encrypted = false;
    int  total_lines    = 0;
    int  selected_line_start = -1;
    int  selected_line_end   = -1;

    matrix_lock();
    int room_index = matrix_find_room_index_by_id(chat_room_id);
    matrix_room_t *room = matrix_get_room(room_index);
    if (room == NULL) {
        matrix_unlock();
        screen = APP_SCREEN_ROOM_LIST;
        return;
    }
    chat_room_index = room_index;
    room_encrypted = room->encrypted;
    snprintf(
        title, sizeof(title), "%s%s", room->encrypted ? "[encrypted] " : "",
        (room->has_name && room->name[0] != '\0') ? room->name : room->room_id
    );

    if (g_all_lines != NULL && g_line_colors != NULL && g_line_name_colors != NULL && g_line_name_lens != NULL &&
        g_line_message_indices != NULL) {
        int history_count = matrix_active_history_count(chat_room_id);
        int message_count = history_count > 0 ? history_count : room->message_count;
        if (message_count > 0 && (chat_selected_message < 0 || chat_selected_message >= message_count)) {
            chat_selected_message = message_count - 1;
        }
        for (int i = 0; i < message_count && total_lines < MAX_WRAP_LINES_TOTAL - 8; i++) {
            matrix_message_t *msg = history_count > 0 ? matrix_get_active_history_message(chat_room_id, i)
                                                      : &room->messages[i];
            if (msg == NULL) continue;
            char               prefixed[MATRIX_SENDER_LEN + MATRIX_BODY_LEN + 8];
            char               prefix[MATRIX_SENDER_LEN + 4];
            char               body_with_reactions[MATRIX_BODY_LEN + 96];
            char               display_body[MATRIX_BODY_LEN + 160];
            snprintf(body_with_reactions, sizeof(body_with_reactions), "%s", msg->body);
            const char *audio_status = matrix_get_audio_status(msg->event_id);
            if (audio_status != NULL && audio_status[0] != '\0') {
                char suffix[MATRIX_AUDIO_STATUS_LEN + 8];
                snprintf(suffix, sizeof(suffix), "  [%s]", audio_status);
                strncat(body_with_reactions, suffix, sizeof(body_with_reactions) - strlen(body_with_reactions) - 1);
            }
            for (int r = 0; r < MATRIX_MAX_REACTIONS; r++) {
                if (msg->reactions[r].key[0] == '\0' || msg->reactions[r].count == 0) continue;
                char reaction[40];
                if (msg->reactions[r].count > 1) {
                    snprintf(reaction, sizeof(reaction), "  %s %u", msg->reactions[r].key, (unsigned int)msg->reactions[r].count);
                } else {
                    snprintf(reaction, sizeof(reaction), "  %s", msg->reactions[r].key);
                }
                strncat(body_with_reactions, reaction, sizeof(body_with_reactions) - strlen(body_with_reactions) - 1);
            }
            text_with_emoji_markers(body_with_reactions, display_body, sizeof(display_body));
            snprintf(prefix, sizeof(prefix), "%.40s: ", msg->sender_display[0] != '\0' ? msg->sender_display : msg->sender);
            snprintf(prefixed, sizeof(prefixed), "%s", prefix);
            strncat(prefixed, display_body, sizeof(prefixed) - strlen(prefixed) - 1);

            char      wrapped[8][WRAP_LINE_LEN];
            int       n     = wrap_text(prefixed, max_cols, wrapped, 8);
            bool      mine  = strcmp(msg->sender, matrix_get_user_id()) == 0;
            pax_col_t color = msg->is_notice ? TERM_DIM : (mine ? TERM_CYAN : TERM_FG);
            pax_col_t name_color = sender_name_color(msg->sender, mine, msg->is_notice);
            size_t    prefix_len = strlen(prefix);
            int       first_line = total_lines;

            for (int j = 0; j < n && total_lines < MAX_WRAP_LINES_TOTAL; j++) {
                strncpy(g_all_lines[total_lines], wrapped[j], WRAP_LINE_LEN - 1);
                g_all_lines[total_lines][WRAP_LINE_LEN - 1] = '\0';
                g_line_colors[total_lines] = color;
                g_line_name_colors[total_lines] = name_color;
                g_line_name_lens[total_lines] = (j == 0 && prefix_len < strlen(wrapped[j])) ? (uint8_t)prefix_len : 0;
                g_line_message_indices[total_lines] = (int16_t)i;
                total_lines++;
            }
            if (i == chat_selected_message && first_line < total_lines) {
                selected_line_start = first_line;
                selected_line_end   = total_lines - 1;
            }
        }
    }
    matrix_unlock();

    int visible_rows = (int)((messages_h - g_line_h * 1.25f) / g_line_h);
    if (visible_rows < 1) visible_rows = 1;
    int max_scroll = (total_lines > visible_rows) ? (total_lines - visible_rows) : 0;
    if (selected_line_start >= 0 && selected_line_end >= 0) {
        int visible_start = total_lines - chat_scroll_offset - visible_rows;
        int visible_end   = total_lines - chat_scroll_offset - 1;
        if (visible_start < 0) visible_start = 0;
        if (selected_line_start < visible_start) {
            chat_scroll_offset = total_lines - visible_rows - selected_line_start;
        } else if (selected_line_end > visible_end) {
            chat_scroll_offset = total_lines - selected_line_end - 1;
        }
    }
    if (chat_scroll_offset > max_scroll) chat_scroll_offset = max_scroll;
    if (chat_scroll_offset < 0) chat_scroll_offset = 0;

    int end_line   = total_lines - chat_scroll_offset;
    int start_line = end_line - visible_rows;
    if (start_line < 0) start_line = 0;

    if (debug_status) {
        draw_matrix_status(16, 12 + g_line_h);
    }
    draw_box(16, messages_y, w - 32, messages_h, TERM_GREEN, title);
    if (g_all_lines != NULL && g_line_colors != NULL && g_line_name_colors != NULL && g_line_name_lens != NULL &&
        g_line_message_indices != NULL) {
        float ty = messages_y + g_line_h;
        for (int i = start_line; i < end_line; i++) {
            bool selected = chat_selected_message >= 0 && g_line_message_indices[i] == chat_selected_message;
            if (selected) {
                pax_simple_rect(&fb, TERM_SELECT_BG, 24, ty - 2, w - 48, g_line_h);
                pax_outline_rect(&fb, TERM_GREEN, 24, ty - 2, w - 48, g_line_h);
            }
            if (g_line_name_lens[i] > 0) {
                char prefix[WRAP_LINE_LEN];
                size_t prefix_len = g_line_name_lens[i];
                if (prefix_len >= sizeof(prefix)) prefix_len = sizeof(prefix) - 1;
                memcpy(prefix, g_all_lines[i], prefix_len);
                prefix[prefix_len] = '\0';
                pax_draw_text(&fb, g_line_name_colors[i], pax_font_sky_mono, FONT_SIZE, 28, ty, prefix);
                float prefix_w = pax_text_size(pax_font_sky_mono, FONT_SIZE, prefix).x;
                draw_text_with_emoji(g_line_colors[i], 28 + prefix_w, ty, g_all_lines[i] + prefix_len);
            } else {
                draw_text_with_emoji(g_line_colors[i], 28, ty, g_all_lines[i]);
            }
            ty += g_line_h;
        }
    }

    float input_y = messages_y + messages_h + gap;
    draw_compose_box(16, input_y, w - 32, input_h, room_encrypted);

    if (do_blit) blit();
}

static void render_chat(void) {
    render_chat_frame(true);
}

static void render_chat_input_only(void) {
    float w = pax_buf_get_widthf(&fb);
    float h = pax_buf_get_heightf(&fb);

    float messages_y = 12 + g_line_h * 2.5f;
    float input_h    = g_line_h * 4.2f;
    float gap        = 8.0f;
    float bottom_pad = 10.0f;
    float messages_h = h - messages_y - input_h - gap - bottom_pad;
    float input_y    = messages_y + messages_h + gap;

    bool room_encrypted = false;
    matrix_lock();
    int room_index = matrix_find_room_index_by_id(chat_room_id);
    matrix_room_t *room = matrix_get_room(room_index);
    room_encrypted = room ? room->encrypted : false;
    chat_room_index = room_index;
    matrix_unlock();

    pax_simple_rect(&fb, BLACK, 0, input_y - g_line_h, w, h - input_y + g_line_h);
    draw_compose_box(16, input_y, w - 32, input_h, room_encrypted);
    blit();
}

/* -------------------------------------------------------------------------- */
/* Emoji picker                                                                */
/* -------------------------------------------------------------------------- */

static bool handle_input_emoji_picker(bsp_input_event_t *event) {
    if (event->type != INPUT_EVENT_TYPE_NAVIGATION || !event->args_navigation.state) return false;

    const int cols = EMOJI_PICKER_COLS;
    switch (event->args_navigation.key) {
        case BSP_INPUT_NAVIGATION_KEY_UP:
            if (emoji_selected >= cols) emoji_selected -= cols;
            return true;
        case BSP_INPUT_NAVIGATION_KEY_DOWN:
            if (emoji_selected + cols < EMOJI_CHOICE_COUNT) emoji_selected += cols;
            else emoji_selected = EMOJI_CHOICE_COUNT - 1;
            return true;
        case BSP_INPUT_NAVIGATION_KEY_LEFT:
            if (emoji_selected > 0) emoji_selected--;
            return true;
        case BSP_INPUT_NAVIGATION_KEY_RIGHT:
            if (emoji_selected < EMOJI_CHOICE_COUNT - 1) emoji_selected++;
            return true;
        case BSP_INPUT_NAVIGATION_KEY_ESC:
        case BSP_INPUT_NAVIGATION_KEY_F3:
            screen = APP_SCREEN_CHAT;
            return true;
        case BSP_INPUT_NAVIGATION_KEY_RETURN:
            if (emoji_selected >= 0 && emoji_selected < EMOJI_CHOICE_COUNT) {
                append_text_piece(compose_buffer, sizeof(compose_buffer), EMOJI_CHOICES[emoji_selected].utf8);
            }
            screen = APP_SCREEN_CHAT;
            return true;
        default:
            return false;
    }
}

static void render_emoji_picker(void) {
    render_chat_frame(false);

    float w = pax_buf_get_widthf(&fb);
    float h = pax_buf_get_heightf(&fb);

    float panel_w = w * 0.64f;
    if (panel_w < 260) panel_w = w - 32;
    float panel_h = h - g_line_h * 2.1f;
    float panel_x = w - panel_w - 16;
    float panel_y = g_line_h * 1.05f;

    pax_simple_rect(&fb, BLACK, panel_x - 6, panel_y - 6, panel_w + 12, panel_h + 12);
    draw_box(panel_x, panel_y, panel_w, panel_h, TERM_GREEN, NULL);

    int cols = EMOJI_PICKER_COLS;
    int rows = (int)((panel_h - 14.0f) / (g_line_h * 1.35f));
    if (rows < 1) rows = 1;
    int visible = rows * cols;
    if (emoji_selected < emoji_scroll) emoji_scroll = (emoji_selected / cols) * cols;
    if (emoji_selected >= emoji_scroll + visible) emoji_scroll = ((emoji_selected / cols) - rows + 1) * cols;
    if (emoji_scroll < 0) emoji_scroll = 0;

    float cell_w = (panel_w - 24.0f) / cols;
    float cell_h = g_line_h * 1.35f;
    float icon_s = FONT_SIZE * 1.62f;
    if (icon_s > cell_w - 10.0f) icon_s = cell_w - 10.0f;
    if (icon_s > cell_h - 6.0f) icon_s = cell_h - 6.0f;

    for (int i = 0; i < visible; i++) {
        int idx = emoji_scroll + i;
        if (idx >= EMOJI_CHOICE_COUNT) break;

        int   col = i % cols;
        int   row = i / cols;
        float x   = panel_x + 12 + col * cell_w + (cell_w - icon_s) * 0.5f;
        float y   = panel_y + 8 + row * cell_h + (cell_h - icon_s) * 0.5f;

        if (idx == emoji_selected) {
            pax_simple_rect(&fb, TERM_SELECT_BG, x - 5, y - 5, icon_s + 10, icon_s + 10);
            pax_outline_rect(&fb, TERM_GREEN, x - 5, y - 5, icon_s + 10, icon_s + 10);
        }

        draw_emoji_icon(EMOJI_CHOICES[idx].marker, x, y, icon_s);
    }
    blit();
}

/* -------------------------------------------------------------------------- */
/* Menu / settings                                                             */
/* -------------------------------------------------------------------------- */

static bool handle_input_menu(bsp_input_event_t *event) {
    if (event->type != INPUT_EVENT_TYPE_NAVIGATION || !event->args_navigation.state) return false;

    const int item_count = 5;
    switch (event->args_navigation.key) {
        case BSP_INPUT_NAVIGATION_KEY_UP:
            menu_selected = (menu_selected + item_count - 1) % item_count;
            return true;
        case BSP_INPUT_NAVIGATION_KEY_DOWN:
            menu_selected = (menu_selected + 1) % item_count;
            return true;
        case BSP_INPUT_NAVIGATION_KEY_ESC:
            screen = menu_return_screen;
            return true;
        case BSP_INPUT_NAVIGATION_KEY_RETURN:
            switch (menu_selected) {
                case 0:
                    screen = menu_return_screen;
                    return true;
                case 1:
                    screen = matrix_get_state() == MATRIX_STATE_LOGGED_OUT ? APP_SCREEN_LOGIN : APP_SCREEN_ROOM_LIST;
                    return true;
                case 2:
                    screen = APP_SCREEN_SETTINGS;
                    return true;
                case 3:
                    matrix_logout();
                    if (!remember_password) login_password[0] = '\0';
                    login_error[0]    = '\0';
                    screen            = APP_SCREEN_LOGIN;
                    return true;
                case 4:
                    bsp_device_restart_to_launcher();
                    return true;
                default:
                    return false;
            }
        default:
            return false;
    }
}

static bool handle_input_settings(bsp_input_event_t *event) {
    if (event->type != INPUT_EVENT_TYPE_NAVIGATION || !event->args_navigation.state) return false;

    const int item_count = 8;
    switch (event->args_navigation.key) {
        case BSP_INPUT_NAVIGATION_KEY_UP:
            settings_selected = (settings_selected + item_count - 1) % item_count;
            return true;
        case BSP_INPUT_NAVIGATION_KEY_DOWN:
            settings_selected = (settings_selected + 1) % item_count;
            return true;
        case BSP_INPUT_NAVIGATION_KEY_ESC:
            screen = APP_SCREEN_MENU;
            return true;
        case BSP_INPUT_NAVIGATION_KEY_RETURN:
            if (settings_selected == 0) {
                remember_account = !remember_account;
                if (!remember_account) remember_password = false;
                matrix_set_persistence_enabled(remember_password);
                save_account_settings();
                return true;
            }
            if (settings_selected == 1) {
                remember_password = !remember_password;
                if (remember_password) remember_account = true;
                matrix_set_persistence_enabled(remember_password);
                save_account_settings();
                return true;
            }
            if (settings_selected == 2) {
                theme_index = (theme_index + 1) % 16;
                apply_theme();
                save_account_settings();
                return true;
            }
            if (settings_selected == 3) {
                font_size_index = (font_size_index + 1) % 4;
                apply_theme();
                save_account_settings();
                return true;
            }
            if (settings_selected == 4) {
                room_list_show_all = !room_list_show_all;
                if (room_list_show_all) {
                    matrix_request_full_resync();
                }
                save_account_settings();
                return true;
            }
            if (settings_selected == 5) {
                debug_status = !debug_status;
                save_account_settings();
                return true;
            }
            if (settings_selected == 6) {
                clear_saved_account();
                save_account_settings();
                return true;
            }
            screen = APP_SCREEN_MENU;
            return true;
        default:
            return false;
    }
}

static void render_menu(void) {
    pax_background(&fb, BLACK);
    float w = pax_buf_get_widthf(&fb);
    float h = pax_buf_get_heightf(&fb);

    pax_draw_text(&fb, TERM_GREEN, pax_font_sky_mono, TITLE_FONT_SIZE, 16, 12, "Menu");
    float box_y = 12 + g_line_h * 2.5f;
    draw_box(16, box_y, w - 32, h - box_y - g_line_h, TERM_GREEN, NULL);

    const char *items[] = {"Resume", "Rooms", "Settings", "Log out", "Exit to launcher"};
    for (int i = 0; i < 5; i++) {
        float y = box_y + g_line_h * (i + 1);
        if (i == menu_selected) {
            pax_simple_rect(&fb, TERM_SELECT_BG, 24, y - 2, w - 64, g_line_h);
        }
        char line[96];
        snprintf(line, sizeof(line), "%s%s", i == menu_selected ? "> " : "  ", items[i]);
        pax_draw_text(&fb, i == menu_selected ? TERM_GREEN : TERM_FG, pax_font_sky_mono, FONT_SIZE, 32, y, line);
    }

    blit();
}

static void render_settings(void) {
    pax_background(&fb, BLACK);
    float w = pax_buf_get_widthf(&fb);
    float h = pax_buf_get_heightf(&fb);

    pax_draw_text(&fb, TERM_GREEN, pax_font_sky_mono, TITLE_FONT_SIZE, 16, 12, "Settings");
    float box_y = 12 + g_line_h * 2.5f;
    draw_box(16, box_y, w - 32, h - box_y - g_line_h, TERM_GREEN, NULL);

    char remember_line[96];
    char password_line[96];
    char all_rooms_line[96];
    char debug_line[96];
    char theme_line[96];
    char size_line[96];
    snprintf(remember_line, sizeof(remember_line), "Remember account: %s", remember_account ? "on" : "off");
    snprintf(password_line, sizeof(password_line), "Remember password: %s", remember_password ? "on" : "off");
    snprintf(all_rooms_line, sizeof(all_rooms_line), "Show all rooms: %s", room_list_show_all ? "on" : "off");
    snprintf(debug_line, sizeof(debug_line), "Debug status: %s", debug_status ? "on" : "off");
    snprintf(theme_line, sizeof(theme_line), "Theme: %s", theme_name());
    snprintf(size_line, sizeof(size_line), "Text size: %.0f", g_font_size);
    const char *items[] = {
        remember_line, password_line, theme_line, size_line, all_rooms_line, debug_line, "Clear saved account", "Back"
    };
    const int item_count = 8;
    for (int i = 0; i < item_count; i++) {
        float y = box_y + g_line_h * (i + 1);
        if (i == settings_selected) {
            pax_simple_rect(&fb, TERM_SELECT_BG, 24, y - 2, w - 64, g_line_h);
        }
        char line[128];
        snprintf(line, sizeof(line), "%s%s", i == settings_selected ? "> " : "  ", items[i]);
        pax_draw_text(&fb, i == settings_selected ? TERM_GREEN : TERM_FG, pax_font_sky_mono, FONT_SIZE, 32, y, line);
    }

    char saved_server[96];
    char saved_user[96];
    snprintf(saved_server, sizeof(saved_server), "Server: %.80s", login_homeserver);
    snprintf(saved_user, sizeof(saved_user), "User: %.82s", login_username[0] ? login_username : "(none)");
    pax_draw_text(&fb, TERM_DIM, pax_font_sky_mono, FONT_SIZE, 32, box_y + g_line_h * 9, saved_server);
    pax_draw_text(&fb, TERM_DIM, pax_font_sky_mono, FONT_SIZE, 32, box_y + g_line_h * 10, saved_user);

    blit();
}

/* -------------------------------------------------------------------------- */
/* Image viewer                                                               */
/* -------------------------------------------------------------------------- */

static bool handle_input_image_viewer(bsp_input_event_t *event) {
    if (event->type == INPUT_EVENT_TYPE_NAVIGATION && event->args_navigation.state) {
        switch (event->args_navigation.key) {
            case BSP_INPUT_NAVIGATION_KEY_ESC:
            case BSP_INPUT_NAVIGATION_KEY_RETURN:
            case BSP_INPUT_NAVIGATION_KEY_BACKSPACE:
                screen = APP_SCREEN_CHAT;
                return true;
            default:
                return false;
        }
    }
    if (event->type == INPUT_EVENT_TYPE_KEYBOARD) {
        char ascii = event->args_keyboard.ascii;
        if (ascii == '\r' || ascii == '\n' || ascii == '\b') {
            screen = APP_SCREEN_CHAT;
            return true;
        }
    }
    return false;
}

static bool handle_input_video_player(bsp_input_event_t *event) {
    bool cancel = false;
    if (event->type == INPUT_EVENT_TYPE_NAVIGATION && event->args_navigation.state) {
        switch (event->args_navigation.key) {
            case BSP_INPUT_NAVIGATION_KEY_ESC:
            case BSP_INPUT_NAVIGATION_KEY_RETURN:
            case BSP_INPUT_NAVIGATION_KEY_BACKSPACE:
                cancel = true;
                break;
            default: break;
        }
    } else if (event->type == INPUT_EVENT_TYPE_KEYBOARD) {
        char ascii = event->args_keyboard.ascii;
        if (ascii == '\r' || ascii == '\n' || ascii == '\b') cancel = true;
    }
    if (!cancel) return false;
    video_player_request_stop();
    active_video_event_id[0] = '\0';
    screen = APP_SCREEN_CHAT;
    return true;
}

static void render_image_viewer(void) {
    pax_background(&fb, BLACK);
    float w = pax_buf_get_widthf(&fb);
    float h = pax_buf_get_heightf(&fb);
    float footer_h = g_line_h + 16.0f;

    if (viewed_image_valid && viewed_image.buf != NULL && viewed_image.width > 0 && viewed_image.height > 0) {
        float max_w = w;
        float max_h = h - footer_h;
        float scale_x = max_w / (float)viewed_image.width;
        float scale_y = max_h / (float)viewed_image.height;
        float scale = scale_x < scale_y ? scale_x : scale_y;
        if (scale > 1.0f) scale = 1.0f;
        float draw_w = (float)viewed_image.width * scale;
        float draw_h = (float)viewed_image.height * scale;
        float x = (w - draw_w) / 2.0f;
        float y = (max_h - draw_h) / 2.0f;
        pax_draw_image_sized(&fb, &viewed_image, x, y, draw_w, draw_h);
    } else {
        const char *msg = viewed_image_error[0] != '\0' ? viewed_image_error : "image not loaded";
        pax_vec2f size = pax_text_size(pax_font_sky_mono, TITLE_FONT_SIZE, msg);
        pax_draw_text(&fb, TERM_RED, pax_font_sky_mono, TITLE_FONT_SIZE, (w - size.x) / 2.0f, (h - size.y) / 2.0f, msg);
    }

    pax_simple_rect(&fb, BLACK, 0, h - footer_h, w, footer_h);
    pax_outline_rect(&fb, TERM_DIM, 0, h - footer_h, w, footer_h);
    char footer[160];
    snprintf(footer, sizeof(footer), "%.140s", viewed_image_label[0] != '\0' ? viewed_image_label : "image");
    pax_draw_text(&fb, TERM_DIM, pax_font_sky_mono, FONT_SIZE, 12, h - footer_h + 8, footer);
    blit();
}

/* Frames are blitted directly to the display by the background task that
 * decodes them (see video_player_play_buffer), bypassing the pax framebuffer
 * entirely. This only clears the screen once, right before playback starts;
 * after that it must not touch the display, or it would wipe out frames the
 * background task just drew. */
static void render_video_player(void) {
    if (video_screen_cleared) return;
    pax_background(&fb, BLACK);
    blit();
    video_screen_cleared = true;
}

/* -------------------------------------------------------------------------- */
/* Dispatch                                                                    */
/* -------------------------------------------------------------------------- */

static bool handle_global_hotkeys(bsp_input_event_t *event) {
    if (event->type != INPUT_EVENT_TYPE_NAVIGATION || !event->args_navigation.state) return false;
    switch (event->args_navigation.key) {
        case BSP_INPUT_NAVIGATION_KEY_VOLUME_UP: {
            int vol = audio_player_get_volume_percent();
            vol += 5;
            if (vol > 100) vol = 100;
            audio_player_set_volume_percent(vol);
            snprintf(audio_volume_status, sizeof(audio_volume_status), "volume %d", vol);
            return true;
        }
        case BSP_INPUT_NAVIGATION_KEY_VOLUME_DOWN: {
            int vol = audio_player_get_volume_percent();
            vol -= 5;
            if (vol < 0) vol = 0;
            audio_player_set_volume_percent(vol);
            snprintf(audio_volume_status, sizeof(audio_volume_status), "volume %d", vol);
            return true;
        }
        case BSP_INPUT_NAVIGATION_KEY_ESC:
            if (screen == APP_SCREEN_IMAGE_VIEWER) {
                return false;
            }
            if (screen == APP_SCREEN_VIDEO_PLAYER) {
                return false;
            }
            if (screen == APP_SCREEN_EMOJI_PICKER) {
                return false;
            }
            if (screen == APP_SCREEN_CHAT && !login_in_progress) {
                screen = APP_SCREEN_ROOM_LIST;
                return true;
            }
            if (screen != APP_SCREEN_MENU && screen != APP_SCREEN_SETTINGS && !login_in_progress) {
                menu_return_screen = screen;
                menu_selected      = 0;
                screen             = APP_SCREEN_MENU;
                return true;
            }
            return false;
        case BSP_INPUT_NAVIGATION_KEY_F1:
            bsp_device_restart_to_launcher();
            return true;
        case BSP_INPUT_NAVIGATION_KEY_F2:
            bsp_input_set_backlight_brightness(0);
            return true;
        case BSP_INPUT_NAVIGATION_KEY_F3:
            if (screen == APP_SCREEN_CHAT && !login_in_progress) {
                emoji_selected = 0;
                emoji_scroll   = 0;
                screen         = APP_SCREEN_EMOJI_PICKER;
                return true;
            }
            if (screen == APP_SCREEN_EMOJI_PICKER) {
                return false;
            }
            bsp_input_set_backlight_brightness(100);
            return true;
        case BSP_INPUT_NAVIGATION_KEY_F4:
            return false;
        default:
            return false;
    }
}

static void render(void) {
    switch (screen) {
        case APP_SCREEN_LOGIN: render_login(); break;
        case APP_SCREEN_ROOM_LIST: render_room_list(); break;
        case APP_SCREEN_CHAT: render_chat(); break;
        case APP_SCREEN_IMAGE_VIEWER: render_image_viewer(); break;
        case APP_SCREEN_VIDEO_PLAYER: render_video_player(); break;
        case APP_SCREEN_EMOJI_PICKER: render_emoji_picker(); break;
        case APP_SCREEN_MENU: render_menu(); break;
        case APP_SCREEN_SETTINGS: render_settings(); break;
    }
}

/* -------------------------------------------------------------------------- */
/* Entry point                                                                 */
/* -------------------------------------------------------------------------- */

void app_main(void) {
    gpio_install_isr_service(0);

    esp_err_t res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES || res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        res = nvs_flash_erase();
        if (res != ESP_OK) {
            ESP_LOGE(TAG, "Failed to erase NVS flash: %d", res);
            return;
        }
        res = nvs_flash_init();
    }
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS flash: %d", res);
        return;
    }

    const bsp_configuration_t bsp_configuration = {
        .display =
            {
                .requested_color_format = BSP_DISPLAY_COLOR_FORMAT_24_888RGB,
                .num_fbs                = 1,
            },
    };
    res = bsp_device_initialize(&bsp_configuration);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize BSP: %d", res);
        return;
    }

    res = bsp_display_get_parameters(&display_h_res, &display_v_res, &display_color_format, &display_data_endian);
    if (res == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGE(TAG, "This board has no display support, the chat UI cannot run");
        return;
    }
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get display parameters: %d", res);
        return;
    }

    pax_buf_type_t format = PAX_BUF_24_888RGB;
    switch (display_color_format) {
        case BSP_DISPLAY_COLOR_FORMAT_16_565RGB: format = PAX_BUF_16_565RGB; break;
        case BSP_DISPLAY_COLOR_FORMAT_24_888RGB: format = PAX_BUF_24_888RGB; break;
        case BSP_DISPLAY_COLOR_FORMAT_32_8888ARGB: format = PAX_BUF_32_8888ARGB; break;
        default:
            ESP_LOGW(
                TAG, "BSP requests color format not explicitly handled (%u), defaulting to 24-bit RGB",
                display_color_format
            );
            break;
    }

    bsp_display_rotation_t display_rotation = bsp_display_get_default_rotation();
    pax_orientation_t      orientation      = PAX_O_UPRIGHT;
    switch (display_rotation) {
        case BSP_DISPLAY_ROTATION_90: orientation = PAX_O_ROT_CCW; break;
        case BSP_DISPLAY_ROTATION_180: orientation = PAX_O_ROT_HALF; break;
        case BSP_DISPLAY_ROTATION_270: orientation = PAX_O_ROT_CW; break;
        case BSP_DISPLAY_ROTATION_0:
        default: orientation = PAX_O_UPRIGHT; break;
    }

    pax_buf_init(&fb, NULL, display_h_res, display_v_res, format);
    pax_buf_reversed(&fb, display_data_endian == BSP_DISPLAY_ENDIAN_BIG);
    pax_buf_set_orientation(&fb, orientation);

    ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));

    measure_font();

    pax_background(&fb, BLACK);
    pax_draw_text(&fb, TERM_GREEN, pax_font_sky_mono, FONT_SIZE, 16, 16, "Connecting to WiFi...");
    blit();

    if (wifi_remote_initialize() == ESP_OK) {
        wifi_connection_init_stack();
        wifi_connect_try_all();
    } else {
        bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_OFF);
        ESP_LOGE(TAG, "WiFi radio not responding, WiFi not available");
    }

    load_saved_account();
    ESP_ERROR_CHECK(matrix_client_init());
    matrix_set_persistence_enabled(remember_password);
    bool restored_session = false;
    if (remember_password && matrix_restore_session() == ESP_OK) {
        restored_session = true;
        matrix_start_sync();
    }

    g_all_lines   = heap_caps_calloc(MAX_WRAP_LINES_TOTAL, sizeof(*g_all_lines), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    g_line_colors = heap_caps_calloc(MAX_WRAP_LINES_TOTAL, sizeof(*g_line_colors), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    g_line_name_colors = heap_caps_calloc(MAX_WRAP_LINES_TOTAL, sizeof(*g_line_name_colors), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    g_line_name_lens   = heap_caps_calloc(MAX_WRAP_LINES_TOTAL, sizeof(*g_line_name_lens), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    g_line_message_indices = heap_caps_calloc(MAX_WRAP_LINES_TOTAL, sizeof(*g_line_message_indices), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (g_all_lines == NULL || g_line_colors == NULL || g_line_name_colors == NULL || g_line_name_lens == NULL ||
        g_line_message_indices == NULL) {
        ESP_LOGE(TAG, "Failed to allocate chat render buffers");
    }

    screen = restored_session ? APP_SCREEN_ROOM_LIST : APP_SCREEN_LOGIN;
    render();

    bool was_logging_in = false;
    while (1) {
        bsp_input_event_t event;
        bool               need_redraw = false;

        if (xQueueReceive(input_event_queue, &event, pdMS_TO_TICKS(200)) == pdTRUE) {
            if (handle_global_hotkeys(&event)) {
                need_redraw = true;
            } else {
                switch (screen) {
                    case APP_SCREEN_LOGIN: need_redraw = handle_input_login(&event); break;
                    case APP_SCREEN_ROOM_LIST: need_redraw = handle_input_room_list(&event); break;
                    case APP_SCREEN_CHAT: need_redraw = handle_input_chat(&event); break;
                    case APP_SCREEN_IMAGE_VIEWER: need_redraw = handle_input_image_viewer(&event); break;
                    case APP_SCREEN_VIDEO_PLAYER: need_redraw = handle_input_video_player(&event); break;
                    case APP_SCREEN_EMOJI_PICKER: need_redraw = handle_input_emoji_picker(&event); break;
                    case APP_SCREEN_MENU: need_redraw = handle_input_menu(&event); break;
                    case APP_SCREEN_SETTINGS: need_redraw = handle_input_settings(&event); break;
                }
            }
        }

        if (was_logging_in && !login_in_progress) {
            need_redraw = true;
        }
        was_logging_in = login_in_progress;

        if (matrix_consume_dirty()) {
            maybe_open_pending_image();
            maybe_start_pending_video();
            maybe_finish_active_video();
            need_redraw = true;
        }

        if (image_decode_ready) {
            image_decode_ready = false;
            need_redraw = true;
        }

        if (need_redraw) {
            render();
        }
    }
}
