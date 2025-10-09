/*
 * mpv notification OSD
 *
 * Copyright 2025 Attila Fidan
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/param.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include <mpv/client.h>

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>
#include <libnotify/notify.h>
#include <libswscale/swscale.h>

/* D-Bus spec maximum message length is 128 MiB */
#define MAX_IMAGE_SIZE 127 * 1024 * 1024

static const char *client_name;
static bool mpv_has_app_name;
static bool server_body_markup;

static NotifyNotification *ntf;

static char summary_store[512];
static char *summary;
static char body[4096];

static long pd_thumbnail;
static long pd_show;

enum done_action {
    /*
     * a property or event means that the notification should be opened (track
     * changed, keep-open changed, etc.)
     *
     * sends notification if considered unfocused or forced, and starts timer to
     * close it (overrides A_NTF_UPD)
     */
    A_NTF_RST = 1 << 0,
    /*
     * the notification object (category, urgency, etc.), thumbnail, or a
     * property which affects the contents of the summary or body changed and if
     * a notification is opened, it should be updated
     *
     * sends notification if considered unfocused and the timer is armed, or
     * forced
     */
    A_NTF_UPD = 1 << 1,
    /*
     * close an open notification unless it's forced (overrides A_NTF_RST and
     * A_NTF_UPD)
     */
    A_NTF_CLOSE = 1 << 2,
    /*
     * the video has changed in some way which affects the thumbnail
     *
     * queues screenshot if ntf_image_enabled and the timer is armed. after
     * receiving the result and post-processing the screenshot, that will
     * trigger an ntf upd.
     */
    A_QUEUE_SHOT = 1 << 3,
    /*
     * same as above but allows doing so even when timer is not armed. used on
     * video reconfig so that the current cover art or some frame from the video
     * is readily available to show when opening the notification, otherwise
     * there will be a brief flicker from an older cover art or the mpv icon.
     */
    A_FORCED_QUEUE_SHOT = 1 << 4,
    /*
     * some property or option has changed which affects if the notification
     * image should be enabled. screenshots aren't queued or processed when
     * the image is not enabled.
     */
    A_NTF_CHECK_IMAGE = 1 << 5,
};

/*
 * accumulated actions to process when reaching done()/idle fn after a series of
 * events
 */
static int done_actions;

static bool ntf_image_enabled;

/* mark summary/body to be rewritten at the next ntf_upd */
static bool rewrite_summary;
static bool rewrite_body;

static bool metadata_avail;
static bool mouse_hovered;

static int64_t UD_SCREENSHOT = 1001;
static bool screenshot_in_progress;

static long percent_pos_rounded;

static bool force_open;

static char *osd_str_chapter = NULL;
static char *osd_str_chapters = NULL;
static char *osd_str_edition = NULL;
static char *osd_str_editions = NULL;

static struct {
    int src_w;
    int src_stride;
    int src_h;
    int dst_w;
    int dst_stride;
    int dst_h;
    uint8_t *thumbnail;
    GdkPixbuf *pixbuf;
    SwsContext *sws;
} thumbnail_ctx;

enum opts_key {
    O_EXPIRE_TIMEOUT = 0,
    O_NTF_APP_ICON,
    O_NTF_CATEGORY,
    O_NTF_URGENCY,
    O_SEND_THUMBNAIL,
    O_SEND_PROGRESS,
    O_SEND_SUB_TEXT,
    O_THUMBNAIL_SIZE,
    O_SCREENSHOT_FLAGS,
    O_THUMBNAIL_SCALING,
    O_DISABLE_SCALING,
    O_FOCUS_MANUAL,
    O_PERFDATA,

    O_END,
};

/*
 * on init, deep copy opts_defaults into opts, apply from file onto opts, then
 * deep copy opts into opts_base. on script-opts changes, deep copy opts into a
 * local opts_previous, free opts, deep copy opts_base into opts, apply
 * script-opts onto opts, then for all options run the appropriate action if
 * there is a difference between opts_previous and opts, then free opts_previous
 */

static struct mpv_node opts_defaults[O_END] = {
    [O_EXPIRE_TIMEOUT] = {.format = MPV_FORMAT_INT64, .u.int64 = 10},
    [O_NTF_APP_ICON] = {.format = MPV_FORMAT_STRING, .u.string = "mpv"},
    [O_NTF_CATEGORY] = {.format = MPV_FORMAT_STRING, .u.string = "mpv"},
    [O_NTF_URGENCY] = {.format = MPV_FORMAT_INT64, .u.int64 = NOTIFY_URGENCY_LOW},
    [O_SEND_THUMBNAIL] = {.format = MPV_FORMAT_FLAG, .u.flag = 1},
    [O_SEND_PROGRESS] = {.format = MPV_FORMAT_FLAG, .u.flag = 1},
    [O_SEND_SUB_TEXT] = {.format = MPV_FORMAT_FLAG, .u.flag = 1},
    [O_THUMBNAIL_SIZE] = {.format = MPV_FORMAT_INT64, .u.int64 = 64},
    [O_SCREENSHOT_FLAGS] = {.format = MPV_FORMAT_STRING, .u.string = "video"},
    [O_THUMBNAIL_SCALING] = {.format = MPV_FORMAT_INT64, .u.int64 = SWS_BICUBIC },
    [O_DISABLE_SCALING] = {.format = MPV_FORMAT_FLAG, .u.flag = 0},
    [O_FOCUS_MANUAL] = {.format = MPV_FORMAT_FLAG, .u.flag = 0},
    [O_PERFDATA] = {.format = MPV_FORMAT_FLAG, .u.flag = 0},
};

static struct mpv_node opts_base[O_END];
static struct mpv_node opts[O_END];

enum metadata_key {
    M_ALBUM = 0,
    M_ALBUM_ARTIST,
    M_ARTIST,
    M_ARTIST__ESC,
    M_DATE,
    M_DATE__YEAREXT,
    M_DISC,
    M_DISCC,
    M_DISCNUMBER,
    M_DISCTOTAL,
    M_ORIGINALDATE,
    M_ORIGINALDATE__YEAREXT,
    M_ORIGINALYEAR,
    M_TITLE,
    M_TOTALDISCS,
    M_YEAR,

    M_END,
};

static char *metadata[M_END];

enum observed_prop_userdata {
    P_APP_NAME = 0,
    P_BRIGHTNESS,
    P_CHAPTER,
    P_CHAPTERS,
    P_CONTRAST,
    P_CURRENT_TRACKS__VIDEO__IMAGE,
    P_DURATION,
    P_EDITION,
    P_EDITIONS,
    P_EOF_REACHED,
    P_FOCUSED,
    P_GAMMA,
    P_HUE,
    P_IDLE_ACTIVE,
    P_IMAGE_DISPLAY_DURATION,
    P_KEEP_OPEN,
    P_LAVFI_COMPLEX,
    P_MEDIA_TITLE,
    P_METADATA,
    P_MSG_LEVEL,
    P_MOUSE_POS,
    P_MUTE,
    P_OPTIONS_SCRIPT_OPTS,
    P_LOOP_FILE,
    P_PAUSE,
    P_PAUSED_FOR_CACHE,
    P_PERCENT_POS,
    P_PLAY_DIRECTION,
    P_PLAYLIST_COUNT,
    P_PLAYLIST_POS,
    P_SATURATION,
    P_SEEKING,
    P_SPEED,
    P_SUB_TEXT,
    P_SUB_VISIBILITY,
    P_TIME_POS,
    P_USER_DATA__DETECT_IMAGE__DETECTED,
    P_VID,
    P_VOLUME,
};

struct observed_prop {
    const char *name;
    mpv_format format;
    bool string_needs_escaping;
    /* mask of actions to trigger when property changed */
    int action;
    /* only trigger the actions if the property is true */
    bool action_if_true;
    bool part_of_summary;
    bool part_of_body;
    mpv_node node;
};

static struct observed_prop observed_props[] = {
    /* this only exists in my personal mpv tree, and is for distinguishing my
     * various mpv instances */
    [P_APP_NAME] = {"app-name", MPV_FORMAT_STRING,
        false, A_NTF_UPD},
    [P_BRIGHTNESS] = {"brightness", MPV_FORMAT_INT64,
        false, A_QUEUE_SHOT},
    [P_CHAPTER] = {"chapter", MPV_FORMAT_INT64,
        false, A_NTF_UPD, false, false, true},
    [P_CHAPTERS] = {"chapters", MPV_FORMAT_INT64,
        false, A_NTF_UPD, false, false, true},
    [P_CONTRAST] = {"contrast", MPV_FORMAT_INT64,
        false, A_QUEUE_SHOT},
    [P_CURRENT_TRACKS__VIDEO__IMAGE] = {"current-tracks/video/image", MPV_FORMAT_FLAG},
    [P_DURATION] = {"duration", MPV_FORMAT_INT64,
        false, A_NTF_UPD, false, false, true},
    [P_EDITION] = {"edition", MPV_FORMAT_INT64,
        false, A_NTF_UPD, false, false, true},
    [P_EDITIONS] = {"editions", MPV_FORMAT_INT64,
        false, A_NTF_UPD, false, false, true},
    [P_EOF_REACHED] = {"eof-reached", MPV_FORMAT_FLAG,
        false, A_NTF_RST, true, false, true},
    [P_FOCUSED] = {"focused", MPV_FORMAT_FLAG,
        false, A_NTF_CLOSE, true},
    [P_GAMMA] = {"gamma", MPV_FORMAT_INT64,
        false, A_QUEUE_SHOT},
    [P_HUE] = {"hue", MPV_FORMAT_INT64,
        false, A_QUEUE_SHOT},
    [P_IDLE_ACTIVE] = {"idle-active", MPV_FORMAT_FLAG,
        false, A_NTF_UPD | A_NTF_CHECK_IMAGE},
    [P_IMAGE_DISPLAY_DURATION] = {"image-display-duration", MPV_FORMAT_DOUBLE,
        false, A_NTF_UPD, false, false, true},
    [P_KEEP_OPEN] = {"keep-open", MPV_FORMAT_STRING,
        false, A_NTF_RST, false, false, true},
    [P_LAVFI_COMPLEX] = {"lavfi-complex", MPV_FORMAT_STRING,
        false, A_NTF_UPD | A_NTF_CHECK_IMAGE},
    [P_LOOP_FILE] = {"loop-file", MPV_FORMAT_STRING,
        false, A_NTF_RST, false, false, true},
    [P_MEDIA_TITLE] = {"media-title", MPV_FORMAT_STRING,
        false, A_NTF_UPD, false, true, false},
    [P_METADATA] = {"metadata", MPV_FORMAT_NODE,
        false, A_NTF_RST | A_NTF_CHECK_IMAGE, false, true, true},
    [P_MOUSE_POS] = {"mouse-pos", MPV_FORMAT_NODE},
    [P_MSG_LEVEL] = {"msg-level", MPV_FORMAT_STRING},
    [P_MUTE] = {"mute", MPV_FORMAT_FLAG,
        false, A_NTF_UPD, false, false, true},
    [P_OPTIONS_SCRIPT_OPTS] = {"options/script-opts", MPV_FORMAT_NODE},
    [P_PAUSE] = {"pause", MPV_FORMAT_FLAG,
        false, A_NTF_RST, false, false, true},
    [P_PAUSED_FOR_CACHE] = {"paused-for-cache", MPV_FORMAT_FLAG,
        false, A_NTF_UPD, false, false, true},
    [P_PERCENT_POS] = {"percent-pos", MPV_FORMAT_DOUBLE},
    [P_PLAY_DIRECTION] = {"play-direction", MPV_FORMAT_STRING,
        false, A_NTF_UPD, false, false, true},
    [P_PLAYLIST_COUNT] = {"playlist-count", MPV_FORMAT_INT64,
        false, A_NTF_UPD, false, false, true},
    [P_PLAYLIST_POS] = {"playlist-pos", MPV_FORMAT_INT64,
        false, A_NTF_UPD, false, false, true},
    [P_SATURATION] = {"saturation", MPV_FORMAT_INT64,
        false, A_QUEUE_SHOT},
    [P_SEEKING] = {"seeking", MPV_FORMAT_FLAG,
        false, A_NTF_UPD, false, false, true},
    [P_SPEED] = {"speed", MPV_FORMAT_DOUBLE,
        false, A_NTF_UPD, false, false, true},
    [P_SUB_TEXT] = {"sub-text", MPV_FORMAT_STRING,
        true, A_NTF_UPD, false, false, true},
    [P_SUB_VISIBILITY] = {"sub-visibility", MPV_FORMAT_FLAG,
        false, A_NTF_UPD, false, false, true},
    [P_TIME_POS] = {"time-pos", MPV_FORMAT_INT64,
        false, A_NTF_UPD, false, false, true},
    /* set by my detect-image.lua */
    [P_USER_DATA__DETECT_IMAGE__DETECTED] = {"user-data/detect-image/detected", MPV_FORMAT_FLAG,
        false, A_NTF_UPD, false, true},
    [P_VID] = {"vid", MPV_FORMAT_INT64,
        false, A_NTF_UPD | A_NTF_CHECK_IMAGE},
    [P_VOLUME] = {"volume", MPV_FORMAT_INT64,
        false, A_NTF_UPD, false, false, true},
};

enum log_level {
    LOG_QUIET,
    LOG_ERROR,
    LOG_VERBOSE,
    LOG_DEBUG,
};

static enum log_level cur_lvl = LOG_ERROR;

static int wakeup_pipe[2] = {-1, -1};
static int timer_fd = -1;
static bool timer_armed = false;

static mpv_handle *hmpv;

static void ntf_set_progress_bar(void);
static void ntf_set_urgency(void);
static void ntf_set_category(void);
static void ntf_set_app_name(void);
static void ntf_set_app_icon(void);
static void ntf_set_image(void);
static void ntf_uninit(void);
static void ntf_init(void);
static void thumbnail_ctx_destroy(void);

static void set_log_level(char *msg_level)
{
    if (!msg_level) {
        cur_lvl = LOG_ERROR;
        return;
    }

    char *level_value = NULL;

    char *str1, *str2, *token, *subtoken, *saveptr1, *saveptr2;
    int j;

    for (str1 = msg_level; ; str1 = NULL) {
        token = strtok_r(str1, ",", &saveptr1);
        if (!token)
            break;

        bool use_this_value = false;
        for (j = 1, str2 = token; ; j++, str2 = NULL) {
            subtoken = strtok_r(str2, "=", &saveptr2);
            if (!subtoken)
                break;

            if (j == 1) {
                if (!strcmp(subtoken, client_name) || !strcmp(subtoken, "all"))
                    use_this_value = true;
            } else if (j == 2 && use_this_value) {
                free(level_value);
                level_value = strdup(subtoken);
            }
        }
    }

    if (level_value) {
        if (!strcmp(level_value, "no"))
            cur_lvl = LOG_QUIET;
        else if (!strcmp(level_value, "v"))
            cur_lvl = LOG_VERBOSE;
        else if (!strcmp(level_value, "debug") || !strcmp(level_value, "trace"))
            cur_lvl = LOG_DEBUG;
        else
            cur_lvl = LOG_ERROR;

        free(level_value);
    } else {
        cur_lvl = LOG_ERROR;
    }
}

static const char *log_level_to_str(enum log_level level)
{
    switch (level) {
        case LOG_ERROR: return "ERROR";
        case LOG_VERBOSE: return "VERBOSE";
        case LOG_DEBUG: return "DEBUG";
        default: abort();
    }
}

static void logger(const enum log_level level, const char *fmt, ...)
{
    if (level > cur_lvl)
        return;

    char log_buf[4096];
    size_t log_buf_count = 0;

    log_buf_count += snprintf(log_buf + log_buf_count,
            sizeof(log_buf) - log_buf_count, "%s: %s: ",
            client_name, log_level_to_str(level));

    if (log_buf_count >= sizeof(log_buf))
        return;

    va_list ap;
    va_start(ap, fmt);
    log_buf_count += vsnprintf(log_buf + log_buf_count,
            sizeof(log_buf) - log_buf_count, fmt, ap);
    va_end(ap);

    mpv_command(hmpv, (const char *[]){"print-text", log_buf, NULL});
}

#define ERR(...) logger(LOG_ERROR, __VA_ARGS__)
#define VERBOSE(...) logger(LOG_VERBOSE, __VA_ARGS__)
#define DEBUG(...) logger(LOG_DEBUG, __VA_ARGS__)

static void timer_disarm(void)
{
    struct itimerspec new_value = {0};
    timerfd_settime(timer_fd, 0, &new_value, 0);
    timer_armed = false;
}

static bool strtolol(const char *str, long *lol)
{
    errno = 0;
    char *endptr;
    *lol = strtol(str, &endptr, 10);
    if (errno == ERANGE) {
        switch (*lol) {
            case LLONG_MIN:
                return false;
            case LLONG_MAX:
                return false;
            default:
                abort();
        }
    } else if (errno != 0) {
        abort();
    } else if (*endptr != '\0') {
        /* only entirely valid strings allowed */
        return false;
    }

    return true;
}

static const char *get_replace_str(char c)
{
    switch (c) {
        case '<':
            return "&lt;";
        case '>':
            return "&gt;";
        case '&':
            return "&amp;";
    }
    return NULL;
}

static char *strdupesc(const char *s)
{
    if (!server_body_markup)
        return strdup(s);

    /* if all characters were '&' and became '&amp;' */
    char *dst = malloc((strlen(s) * 4) + 1);
    if (!dst)
        return NULL;

    const char *in = s;
    char *out = dst;
    while (*in) {
        const char *replace = get_replace_str(*in);
        if (!replace) {
            *out++ = *in++;
            continue;
        }

        out = stpcpy(out, replace);
        in++;
    }
    *out = 0;

    return dst;
}

static bool is_date_sep(char c)
{
    return c == '-' || c == '.' || c == '/' || c == ' ';
}

/* copy only YYYY from YYYY-MM-DD for display purposes */
static char *strdupescyear(const char *s)
{
    size_t s_len = strlen(s);

    if (s_len != 10)
        return strdupesc(s);

    if (isdigit(s[0]) && isdigit(s[1]) && isdigit(s[2]) && isdigit(s[3]) &&
            is_date_sep(s[4]) && isdigit(s[5]) && isdigit(s[6]) &&
            is_date_sep(s[7]) && isdigit(s[8]) && isdigit(s[9]))
        return strndup(s, 4);
    else
        return strdupesc(s);
}

static bool str_is_set(const char *str)
{
    return str && *str != '\0';
}

static bool opt_true(enum opts_key opt)
{
    switch (opts[opt].format) {
        case MPV_FORMAT_NONE:
            return false;
        case MPV_FORMAT_STRING:
            return str_is_set(opts[opt].u.string);
        case MPV_FORMAT_FLAG:
            return opts[opt].u.flag;
        case MPV_FORMAT_INT64:
            return opts[opt].u.int64;
        default:
            return false;
    }
}

static void opts_copy(struct mpv_node *dst, struct mpv_node *src)
{
    memcpy(dst, src, sizeof(*dst) * O_END);

    for (int i = 0; i < O_END; i++) {
        if (src[i].format == MPV_FORMAT_STRING)
            dst[i].u.string = strdup(src[i].u.string);
    }
}

static void opts_destroy(struct mpv_node *o)
{
    for (int i = 0; i < O_END; i++) {
        if (o[i].format == MPV_FORMAT_STRING) {
            free(o[i].u.string);
            o[i].u.string = NULL;
        }
    }
}

static void opts_run_changed(struct mpv_node *before, struct mpv_node *after)
{
    for (int i = 0; i < O_END; i++) {
        bool changed = false;

        switch (before[i].format) {
            case MPV_FORMAT_STRING:
                /* strings could technically be null because strdup is allowed
                 * to fail when copying opts */
                if ((!before[i].u.string && after[i].u.string) ||
                        (before[i].u.string && !after[i].u.string)) {
                    changed = true;
                    break;
                }

                changed = strcmp(before[i].u.string, after[i].u.string);
                break;
            case MPV_FORMAT_FLAG:
                changed = before[i].u.flag != after[i].u.flag;
                break;
            case MPV_FORMAT_INT64:
                changed = before[i].u.int64 != after[i].u.int64;
                break;
            default:
                break;
        }

        if (changed) {
            VERBOSE("option %d changed", i);
            switch (i) {
                case O_NTF_APP_ICON:
                    ntf_set_app_icon();
                    done_actions |= A_NTF_UPD;
                    break;
                case O_NTF_CATEGORY:
                    ntf_set_category();
                    done_actions |= A_NTF_UPD;
                    break;
                case O_NTF_URGENCY:
                    ntf_set_urgency();
                    done_actions |= A_NTF_UPD;
                    break;
                case O_SEND_THUMBNAIL:
                    /*
                     * ntf_check_image doesn't queue thumbnails itself when
                     * enabling, for reasons described there. if images will be
                     * enabled then this queue shot will work
                     */
                    done_actions |= A_NTF_CHECK_IMAGE;
                    if (opt_true(O_SEND_THUMBNAIL))
                        done_actions |= A_QUEUE_SHOT;
                    break;
                case O_SEND_PROGRESS:
                    ntf_set_progress_bar();
                    done_actions |= A_NTF_UPD;
                    break;
                case O_SEND_SUB_TEXT:
                    done_actions |= A_NTF_UPD;
                    rewrite_body = true;
                    break;
                case O_THUMBNAIL_SIZE:
                    thumbnail_ctx_destroy();
                    done_actions |= A_QUEUE_SHOT;
                    break;
                case O_SCREENSHOT_FLAGS:
                    done_actions |= A_QUEUE_SHOT;
                    break;
                case O_THUMBNAIL_SCALING:
                    thumbnail_ctx_destroy();
                    done_actions |= A_QUEUE_SHOT;
                    break;
                case O_DISABLE_SCALING:
                    thumbnail_ctx_destroy();
                    done_actions |= A_QUEUE_SHOT;
                    break;
                case O_FOCUS_MANUAL:
                    done_actions |= A_NTF_RST;
                    break;
                case O_PERFDATA:
                    done_actions |= A_NTF_UPD;
                    rewrite_body = true;
                    break;
                default:
                    break;
            }
        }
    }
}

static bool set_opt_bool(struct mpv_node *o, enum opts_key opt, const char *value)
{
    if (!strcmp(value, "yes"))
        o[opt].u.flag = 1;
    else if (!strcmp(value, "no"))
        o[opt].u.flag = 0;
    else
        return false;

    return true;
}

static void set_opt_string(struct mpv_node *o, enum opts_key opt,
        const char *value)
{
    free(o[opt].u.string);
    o[opt].u.string = strdup(value);
}

/* string options are allowed to be empty strings */
static void set_opt(struct mpv_node *o, int line_count, const char *key,
        const char *value)
{
    int64_t num_value;

    char *msg_pfx;
    char msg_pfx_store[64] = {0};
    if (line_count < 0) {
        msg_pfx = "script-opts";
    } else {
        snprintf(msg_pfx_store, sizeof(msg_pfx_store), "script-opts/%s.conf:%d",
                client_name, line_count);
        msg_pfx = msg_pfx_store;
    }

    VERBOSE("%s setting option '%s' to '%s'", msg_pfx, key, value);

    if (!strcmp(key, "expire_timeout")) {
        if (!strtolol(value, &num_value) || num_value < 0)
            goto bad_number;
        o[O_EXPIRE_TIMEOUT].u.int64 = num_value;
    } else if (!strcmp(key, "ntf_app_icon")) {
        set_opt_string(o, O_NTF_APP_ICON, value);
    } else if (!strcmp(key, "ntf_category")) {
        set_opt_string(o, O_NTF_CATEGORY, value);
    } else if (!strcmp(key, "ntf_urgency")) {
        if (!strcmp(value, "low")) {
            o[O_NTF_URGENCY].u.int64 = NOTIFY_URGENCY_LOW;
        } else if (!strcmp(value, "normal")) {
            o[O_NTF_URGENCY].u.int64 = NOTIFY_URGENCY_NORMAL;
        } else if (!strcmp(value, "critical")) {
            o[O_NTF_URGENCY].u.int64 = NOTIFY_URGENCY_CRITICAL;
        } else {
            ERR("%s unknown notification urgency '%s', setting to 'low'",
                    msg_pfx, value);
            o[O_NTF_URGENCY].u.int64 = NOTIFY_URGENCY_LOW;
        }
    } else if (!strcmp(key, "send_thumbnail")) {
        if (!set_opt_bool(o, O_SEND_THUMBNAIL, value))
            goto bad_bool;
    } else if (!strcmp(key, "send_progress")) {
        if (!set_opt_bool(o, O_SEND_PROGRESS, value))
            goto bad_bool;
    } else if (!strcmp(key, "send_sub_text")) {
        if (!set_opt_bool(o, O_SEND_SUB_TEXT, value))
            goto bad_bool;
    } else if (!strcmp(key, "thumbnail_size")) {
        if (!strtolol(value, &num_value) || num_value < 1)
            goto bad_number;
        o[O_THUMBNAIL_SIZE].u.int64 = num_value;
    } else if (!strcmp(key, "screenshot_flags")) {
        set_opt_string(o, O_SCREENSHOT_FLAGS, value);
    } else if (!strcmp(key, "thumbnail_scaling")) {
        if (!strcmp(value, "fast-bilinear")) {
            o[O_THUMBNAIL_SCALING].u.int64 = SWS_FAST_BILINEAR;
        } else if (!strcmp(value, "bilinear")) {
            o[O_THUMBNAIL_SCALING].u.int64 = SWS_BILINEAR;
        } else if (!strcmp(value, "bicubic")) {
            o[O_THUMBNAIL_SCALING].u.int64 = SWS_BICUBIC;
        } else if (!strcmp(value, "lanczos")) {
            o[O_THUMBNAIL_SCALING].u.int64 = SWS_LANCZOS;
        } else {
            ERR("%s unknown thumbnail scaling option '%s', setting to 'bicubic'",
                    msg_pfx, value);
            o[O_THUMBNAIL_SCALING].u.int64 = SWS_BICUBIC;
        }
    } else if (!strcmp(key, "disable_scaling")) {
        if (!set_opt_bool(o, O_DISABLE_SCALING, value))
            goto bad_bool;
    } else if (!strcmp(key, "focus_manual")) {
        if (!set_opt_bool(o, O_FOCUS_MANUAL, value))
            goto bad_bool;
    } else if (!strcmp(key, "perfdata")) {
        if (!set_opt_bool(o, O_PERFDATA, value))
            goto bad_bool;
    } else {
        ERR("%s unknown key '%s', ignoring", msg_pfx, key);
        return;
    }

    return;

bad_number:
    ERR("%s error converting value '%s' for key '%s' into number, or number is insuitable, using default or config file value",
            msg_pfx, value, key);
    return;

bad_bool:
    ERR("%s error converting value '%s' for key '%s' into boolean, using default or config file value",
            msg_pfx, value, key);
}

static void opts_from_file(struct mpv_node *o)
{
    mpv_node path_node = {0};
    FILE *cfg_file = NULL;
    char path_to_expand[64];
    snprintf(path_to_expand, sizeof(path_to_expand), "~~home/script-opts/%s.conf", client_name);
    const char *args[] = {"expand-path", path_to_expand, NULL};

    if (mpv_command_ret(hmpv, args, &path_node) != 0)
        goto done;

    if (path_node.format != MPV_FORMAT_STRING)
        goto done;

    const char *cfg_path = path_node.u.string;
    cfg_file = fopen(cfg_path, "r");
    if (!cfg_file)
        goto done;

    char *line = NULL;
    size_t size;
    int line_count = 0;

    while (getline(&line, &size, cfg_file) != -1) {
        char *value;

        line_count++;

        if (!str_is_set(line) || *line == '#' || !(value = strchr(line, '=')))
            continue;

        size_t line_len = strlen(line);
        if (line[line_len - 1] == '\n')
            line[line_len - 1] = '\0';

        *value = '\0';
        value++;

        set_opt(o, line_count, line, value);
   }

    free(line);

done:
    if (cfg_file)
        fclose(cfg_file);
    mpv_free_node_contents(&path_node);
}

static void opts_from_runtime(struct mpv_node *o, mpv_node *node)
{
    if (node->format != MPV_FORMAT_NODE_MAP)
        return;

    mpv_node_list *list = node->u.list;
    for (int i = 0; i < list->num; i++) {
        char *key = list->keys[i];
        mpv_node *value = &list->values[i];

        if (value->format != MPV_FORMAT_STRING)
            continue;

        char *opt_name = NULL;

        char *str, *token, *saveptr;
        int j;

        bool use_this_opt = false;
        for (j = 1, str = key; ; j++, str = NULL) {
            token = strtok_r(str, "-", &saveptr);
            if (!token)
                break;

            if (j == 1 && !strcmp(token, client_name))
                use_this_opt = true;
            else if (j == 2 && use_this_opt)
                opt_name = strdup(token);
        }

        if (opt_name && value->u.string)
            set_opt(o, -1, opt_name, value->u.string);

        free(opt_name);
    }
}

static bool op_true(enum observed_prop_userdata ud)
{
    struct observed_prop *prop = &observed_props[ud];
    switch (prop->node.format) {
        case MPV_FORMAT_NONE:
            return false;
        case MPV_FORMAT_STRING:
            return str_is_set(prop->node.u.string);
        case MPV_FORMAT_FLAG:
            return prop->node.u.flag;
        case MPV_FORMAT_INT64:
            return prop->node.u.int64;
        default:
            return false;
    }
}

static bool op_avail(enum observed_prop_userdata ud)
{
    struct observed_prop *prop = &observed_props[ud];
    return prop->node.format != MPV_FORMAT_NONE;
}

static void get_osd_str_chapter(void)
{
    free(osd_str_chapter);
    osd_str_chapter = NULL;
    mpv_free(osd_str_chapters);
    osd_str_chapters = NULL;
    char *tmp = osd_str_chapter = mpv_get_property_osd_string(hmpv, "chapter");
    if (!tmp)
        return;
    osd_str_chapter = strdupesc(tmp);
    mpv_free(tmp);
    osd_str_chapters = mpv_get_property_osd_string(hmpv, "chapters");
}

static void get_osd_str_edition(void)
{
    free(osd_str_edition);
    osd_str_edition = NULL;
    mpv_free(osd_str_editions);
    osd_str_editions = NULL;
    char *tmp = mpv_get_property_osd_string(hmpv, "edition");
    if (!tmp)
        return;
    osd_str_edition = strdupesc(tmp);
    mpv_free(tmp);
    osd_str_editions = mpv_get_property_osd_string(hmpv, "editions");
}

static void save_prop(mpv_event *event, struct observed_prop *prop)
{
    mpv_event_property *event_prop = event->data;

    if (prop->node.format == MPV_FORMAT_STRING)
        free(prop->node.u.string);

    prop->node.format = event_prop->format;

    switch (event_prop->format) {
        case MPV_FORMAT_STRING:
            prop->node.u.string = prop->string_needs_escaping ?
                strdupesc(*(char **)event_prop->data) :
                strdup(*(char **)event_prop->data);
            break;
        case MPV_FORMAT_FLAG:
            prop->node.u.flag = *(int *)event_prop->data;
            break;
        case MPV_FORMAT_INT64:
            prop->node.u.int64 = *(int64_t *)event_prop->data;
            break;
        case MPV_FORMAT_DOUBLE:
            prop->node.u.double_ = *(double *)event_prop->data;
            break;
        case MPV_FORMAT_NONE:
        case MPV_FORMAT_NODE:
        default:
            prop->node = (struct mpv_node){0};
            break;
    }
}

static bool mouse_is_hovered(mpv_event_property *event_prop)
{
    if (event_prop->format != MPV_FORMAT_NODE)
        return false;

    struct mpv_node *node = event_prop->data;

    if (node->format != MPV_FORMAT_NODE_MAP)
        return false;

    mpv_node_list *list = node->u.list;
    for (int i = 0; i < list->num; i++) {
        char *key = list->keys[i];
        mpv_node *value = &list->values[i];

        if (value->format != MPV_FORMAT_FLAG)
            continue;

        if (!strcmp(key, "hover"))
            return value->u.flag;
    }

    return false;
}

static void metadata_destroy(void)
{
    for (size_t i = 0; i < sizeof(metadata) / sizeof(metadata[0]); i++) {
        if (metadata[i]) {
            free(metadata[i]);
            metadata[i] = NULL;
        }
    }
}

static void metadata_update(mpv_event_property *event_prop)
{
    metadata_destroy();
    metadata_avail = false;

    if (event_prop->format != MPV_FORMAT_NODE)
        return;

    struct mpv_node *node = event_prop->data;

    if (node->format != MPV_FORMAT_NODE_MAP)
        return;

    metadata_avail = true;

    mpv_node_list *list = node->u.list;
    for (int i = 0; i < list->num; i++) {
        char *key = list->keys[i];
        mpv_node *value = &list->values[i];

        if (value->format != MPV_FORMAT_STRING)
            continue;

        if (!strcasecmp(key, "album")) {
            if (!metadata[M_ALBUM])
                metadata[M_ALBUM] = strdupesc(value->u.string);
        } else if (!strcasecmp(key, "album_artist")) {
            if (!metadata[M_ALBUM_ARTIST])
                metadata[M_ALBUM_ARTIST] = strdupesc(value->u.string);
        } else if (!strcasecmp(key, "artist")) {
            if (!metadata[M_ARTIST])
                metadata[M_ARTIST] = strdup(value->u.string);
            if (!metadata[M_ARTIST__ESC])
                metadata[M_ARTIST__ESC] = strdupesc(value->u.string);
        } else if (!strcasecmp(key, "date")) {
            if (!metadata[M_DATE])
                metadata[M_DATE] = strdupesc(value->u.string);
            if (!metadata[M_DATE__YEAREXT])
                metadata[M_DATE__YEAREXT] = strdupescyear(value->u.string);
        } else if (!strcasecmp(key, "disc")) {
            if (!metadata[M_DISC])
                metadata[M_DISC] = strdupesc(value->u.string);
        } else if (!strcasecmp(key, "discc")) {
            if (!metadata[M_DISCC])
                metadata[M_DISCC] = strdupesc(value->u.string);
        } else if (!strcasecmp(key, "discnumber")) {
            if (!metadata[M_DISCNUMBER])
                metadata[M_DISCNUMBER] = strdupesc(value->u.string);
        } else if (!strcasecmp(key, "disctotal")) {
            if (!metadata[M_DISCTOTAL])
                metadata[M_DISCTOTAL] = strdupesc(value->u.string);
        } else if (!strcasecmp(key, "originaldate")) {
            if (!metadata[M_ORIGINALDATE])
                metadata[M_ORIGINALDATE] = strdupesc(value->u.string);
            if (!metadata[M_ORIGINALDATE__YEAREXT])
                metadata[M_ORIGINALDATE__YEAREXT] = strdupescyear(value->u.string);
        } else if (!strcasecmp(key, "originalyear")) {
            if (!metadata[M_ORIGINALYEAR])
                metadata[M_ORIGINALYEAR] = strdupesc(value->u.string);
        } else if (!strcasecmp(key, "title")) {
            if (!metadata[M_TITLE])
                metadata[M_TITLE] = strdup(value->u.string);
        } else if (!strcasecmp(key, "totaldiscs")) {
            if (!metadata[M_TOTALDISCS])
                metadata[M_TOTALDISCS] = strdupesc(value->u.string);
        } else if (!strcasecmp(key, "year")) {
            if (!metadata[M_YEAR])
                metadata[M_YEAR] = strdupesc(value->u.string);
        }
    }
}

static void on_property_change(mpv_event *event)
{
    mpv_event_property *event_prop = event->data;
    struct observed_prop *prop = &observed_props[event->reply_userdata];

    save_prop(event, prop);

    if (!prop->action_if_true || op_true(event->reply_userdata))
        done_actions |= prop->action;

    if (prop->part_of_summary)
        rewrite_summary = true;
    if (prop->part_of_body)
        rewrite_body = true;

    switch (event->reply_userdata) {
        case P_APP_NAME:
            ntf_set_app_name();
            break;
        case P_CHAPTER:
        case P_CHAPTERS:
            get_osd_str_chapter();
            break;
        case P_EDITION:
        case P_EDITIONS:
            get_osd_str_edition();
            break;
        case P_IDLE_ACTIVE:
            ntf_set_progress_bar();
            rewrite_body = true;
            break;
        case P_METADATA:
            metadata_update(event_prop);
            break;
        case P_MOUSE_POS: {
            bool old_mouse_hovered = mouse_hovered;
            mouse_hovered = mouse_is_hovered(event_prop);
            if (!old_mouse_hovered && mouse_hovered)
                done_actions |= A_NTF_CLOSE;
            break;
        }
        case P_MSG_LEVEL:
            set_log_level(observed_props[P_MSG_LEVEL].node.u.string);
            break;
        case P_OPTIONS_SCRIPT_OPTS: {
            struct mpv_node opts_previous[O_END] = {0};
            opts_copy(opts_previous, opts);
            opts_destroy(opts);
            opts_copy(opts, opts_base);
            if (event_prop->format == MPV_FORMAT_NODE)
                opts_from_runtime(opts, event_prop->data);
            opts_run_changed(opts_previous, opts);
            opts_destroy(opts_previous);
            break;
        }
        case P_PERCENT_POS: {
            /*
             * avoid constantly queueing screenshots for cover art. that means
             * we have to make sure we otherwise detect if the image has changed
             * (e.g. equalizer options), which probably won't be perfect
             */
            if (!op_true(P_CURRENT_TRACKS__VIDEO__IMAGE))
                done_actions |= A_QUEUE_SHOT;

            long old_rounded = percent_pos_rounded;
            if (op_avail(P_PERCENT_POS) && isnormal(observed_props[P_PERCENT_POS].node.u.double_))
                percent_pos_rounded = lround(observed_props[P_PERCENT_POS].node.u.double_);
            else
                percent_pos_rounded = 0;

            if (old_rounded != percent_pos_rounded) {
                ntf_set_progress_bar();
                done_actions |= A_NTF_UPD;
                rewrite_body = true;
            }
            break;
        }
        case P_PLAYLIST_COUNT:
        case P_PLAYLIST_POS:
            ntf_set_progress_bar();
            break;
        case P_USER_DATA__DETECT_IMAGE__DETECTED:
            ntf_set_progress_bar();
            break;
        default:
            break;
    }

    DEBUG("property changed, %s.", prop->name);
}

static void thumbnail_ctx_destroy(void)
{
    if (thumbnail_ctx.thumbnail)
        free(thumbnail_ctx.thumbnail);
    if (thumbnail_ctx.pixbuf)
        g_object_unref(thumbnail_ctx.pixbuf);
    if (thumbnail_ctx.sws)
        sws_freeContext(thumbnail_ctx.sws);

    memset(&thumbnail_ctx, 0, sizeof(thumbnail_ctx));
    if (ntf)
        ntf_set_image();

    VERBOSE("destroyed thumbnail context");
}

static void thumbnail_ctx_maybe_new(double src_w, double src_h,
        double src_stride)
{
    if (src_w == thumbnail_ctx.src_w && src_h == thumbnail_ctx.src_h &&
            (!opt_true(O_DISABLE_SCALING) ||
             src_stride == thumbnail_ctx.src_stride)) {
        /* we can keep the same ctx if only src stride changes with sws, but
         * remember to update it before processing */
        thumbnail_ctx.src_stride = src_stride;
        return;
    }

    thumbnail_ctx_destroy();

    thumbnail_ctx.src_w = src_w;
    thumbnail_ctx.src_stride = src_stride;
    thumbnail_ctx.src_h = src_h;
    if (opt_true(O_DISABLE_SCALING)) {
        thumbnail_ctx.dst_w = src_w;
        thumbnail_ctx.dst_stride = src_stride;
        thumbnail_ctx.dst_h = src_h;
    } else {
        double scaled_size = (double)opts[O_THUMBNAIL_SIZE].u.int64;
        double ratio = fmin(scaled_size / src_w, scaled_size / src_h);
        thumbnail_ctx.dst_w = MAX(1, (int)(src_w * ratio));
        thumbnail_ctx.dst_stride = thumbnail_ctx.dst_w * 4;
        thumbnail_ctx.dst_h = MAX(1, (int)(src_h * ratio));
        thumbnail_ctx.sws = sws_getContext(src_w, src_h, AV_PIX_FMT_RGBA,
                thumbnail_ctx.dst_w, thumbnail_ctx.dst_h, AV_PIX_FMT_RGBA,
                opts[O_THUMBNAIL_SCALING].u.int64, NULL, NULL, NULL);
        if (!thumbnail_ctx.sws) {
            thumbnail_ctx_destroy();
            return;
        }
    }

    int64_t alloc_size = thumbnail_ctx.dst_stride * thumbnail_ctx.dst_h;

    if (alloc_size > MAX_IMAGE_SIZE) {
        ERR("thumbnail output resolution is too large, disabling thumbnails");
        thumbnail_ctx_destroy();
        return;
    }

    thumbnail_ctx.thumbnail = malloc(alloc_size);
    if (!thumbnail_ctx.thumbnail) {
        thumbnail_ctx_destroy();
        return;
    }
    thumbnail_ctx.pixbuf = gdk_pixbuf_new_from_data(thumbnail_ctx.thumbnail,
            GDK_COLORSPACE_RGB, true, 8, thumbnail_ctx.dst_w,
            thumbnail_ctx.dst_h, thumbnail_ctx.dst_stride, NULL, NULL);
    if (!thumbnail_ctx.pixbuf) {
        thumbnail_ctx_destroy();
        return;
    }

    /* this function is only called while ntf_image_enabled is true */
    ntf_set_image();
    VERBOSE("configured thumbnail context");
}

static void thumbnail_ctx_process(void *data)
{
    if (!thumbnail_ctx.thumbnail)
        return;

    struct timespec tp[2] = {0};
    if (opt_true(O_PERFDATA))
        clock_gettime(CLOCK_MONOTONIC, &tp[0]);

    if (thumbnail_ctx.sws) {
        const uint8_t *const src_slice[1] = {data};
        const int src_stride[1] = {thumbnail_ctx.src_stride};
        uint8_t *const dst[1] = {thumbnail_ctx.thumbnail};
        const int dst_stride[1] = {thumbnail_ctx.dst_stride};
        sws_scale(thumbnail_ctx.sws, src_slice, src_stride, 0,
                thumbnail_ctx.src_h, dst, dst_stride);
    } else {
        memcpy(thumbnail_ctx.thumbnail, data,
                thumbnail_ctx.dst_stride * thumbnail_ctx.dst_h);
    }

    if (opt_true(O_PERFDATA)) {
        clock_gettime(CLOCK_MONOTONIC, &tp[1]);
        long in_ns[2];
        in_ns[0] = tp[0].tv_sec * 1e9 + tp[0].tv_nsec;
        in_ns[1] = tp[1].tv_sec * 1e9 + tp[1].tv_nsec;
        pd_thumbnail = (in_ns[1] - in_ns[0]) / 1e3;
        rewrite_body = true;
    }

    done_actions |= A_NTF_UPD;
}

static bool ntf_update_server_caps(void)
{
    server_body_markup = false;

    GList *server_cap_list;
    if (!(server_cap_list = notify_get_server_caps()))
        return false;

    for (GList *l = server_cap_list; l; l = l->next) {
        if (!strcmp(l->data, "body-markup")) {
            server_body_markup = true;
            g_free(l->data);
            break;
        }

        g_free(l->data);
    }

    VERBOSE("server supports markup? %d", server_body_markup);
    g_list_free(server_cap_list);
    return true;
}

/*
 * enable or disable notification image support based on some criteria:
 * - disable if idling
 * - disable if there is no video track selected, unless a lavfi-complex is
 *   enabled or we're in the middle of switching tracks
 * - disable if send_thumbnail=no
 *
 * we have to turn it on/off ourselves instead of relying on an empty/error
 * screenshot to determine this, because when going from a video to no
 * video selected/idle player, screenshots actually still work and return the
 * image from the last displayed frame from the last played video.
 *
 * we don't immediately queue a screenshot after enabling images because that
 * could yield a screenshot of the last frame of a previous video. if switching
 * video tracks or exiting idle mode, video reconfig should happen soon which
 * will queue a correct screnshot when the new video is ready. if instead
 * send_thumbnail was changed, that handler will have queued a screenshot.
 */
static void ntf_check_image(void)
{
    bool switching_track = !op_true(P_IDLE_ACTIVE) && !op_avail(P_VID) &&
        !metadata_avail;

    if (op_true(P_IDLE_ACTIVE) || !opt_true(O_SEND_THUMBNAIL) ||
            (!op_avail(P_VID) && !op_true(P_LAVFI_COMPLEX) && !switching_track)) {
        if (ntf_image_enabled) {
            VERBOSE("notification image disabled");
            ntf_image_enabled = false;
            thumbnail_ctx_destroy();
            done_actions |= A_NTF_UPD;
        }
        return;
    }

    if (!ntf_image_enabled) {
        VERBOSE("notification image enabled");
        ntf_image_enabled = true;
    }
}

static void ntf_set_progress_bar(void)
{
    if (!ntf)
        return;

    if (op_true(P_IDLE_ACTIVE) || !opt_true(O_SEND_PROGRESS)) {
        notify_notification_set_hint(ntf, "value", NULL);
        return;
    }

    if (op_true(P_USER_DATA__DETECT_IMAGE__DETECTED)) {
        if (op_avail(P_PLAYLIST_POS) && observed_props[P_PLAYLIST_COUNT].node.u.int64 > 1) {
            double gallery_percent =
                (observed_props[P_PLAYLIST_POS].node.u.int64 + 1) / (double)observed_props[P_PLAYLIST_COUNT].node.u.int64;
            long gallery_percent_round = lround(gallery_percent * 100);
            GVariant *v = g_variant_new("i", gallery_percent_round);
            notify_notification_set_hint(ntf, "value", v);
        } else {
            notify_notification_set_hint(ntf, "value", NULL);
        }
    } else {
        GVariant *v = g_variant_new("i", percent_pos_rounded);
        notify_notification_set_hint(ntf, "value", v);
    }
}

static void ntf_set_urgency(void)
{
    if (!ntf)
        return;

    notify_notification_set_urgency(ntf, opts[O_NTF_URGENCY].u.int64);
}

static void ntf_set_category(void)
{
    if (!ntf)
        return;

    if (opt_true(O_NTF_CATEGORY)) {
        notify_notification_set_category(ntf, opts[O_NTF_CATEGORY].u.string);
    } else {
        /* notify_notification_set_category doesn't unset if you pass NULL */
        notify_notification_set_hint(ntf, "category", NULL);
    }
}

static void ntf_set_app_name(void)
{
    if (!notify_is_initted())
        return;

    if (op_true(P_APP_NAME))
        notify_set_app_name(observed_props[P_APP_NAME].node.u.string);
    else
        notify_set_app_name("mpv");
}

static void ntf_set_app_icon(void)
{
    if (!notify_is_initted())
        return;

    if (opt_true(O_NTF_APP_ICON))
        notify_set_app_icon(opts[O_NTF_APP_ICON].u.string);
    else
        notify_set_app_icon(NULL);
}

static void ntf_close(void)
{
    if (!ntf)
        return;

    DEBUG("notification close");
    GError *gerr = NULL;
    if (!notify_notification_close(ntf, &gerr)) {
        ERR("failed to close notification: %s", gerr->message);
        g_error_free(gerr);
    }
}

static void ntf_set_image(void)
{
    if (!ntf)
        return;

    notify_notification_set_image_from_pixbuf(ntf, thumbnail_ctx.pixbuf);
}

static void ntf_uninit(void)
{
    ntf_close();
    if (ntf) {
        g_object_unref(ntf);
        ntf = NULL;
    }
    if (notify_is_initted())
        notify_uninit();
}

static void ntf_init(void)
{
    if (!notify_init("mpv")) {
        ERR("notify_init() failed");
        return;
    }

    if (!ntf_update_server_caps()) {
        ERR("failed to get server caps");
        ntf_uninit();
        return;
    }

    ntf_set_app_name();
    ntf_set_app_icon();

    if (!(ntf = notify_notification_new(summary, body, NULL))) {
        ERR("failed to create notification");
        ntf_uninit();
        return;
    }
    notify_notification_set_timeout(ntf, NOTIFY_EXPIRES_NEVER);

    ntf_set_progress_bar();
    ntf_set_category();
    ntf_set_urgency();
    ntf_set_image();
}

static void write_summary(void)
{
    DEBUG("writing summary");
    if (metadata[M_ARTIST] && metadata[M_TITLE]) {
        snprintf(summary_store, sizeof(summary_store), "%s - %s",
                metadata[M_ARTIST], metadata[M_TITLE]);
        summary = summary_store;
    } else if (op_true(P_MEDIA_TITLE)) {
        summary = observed_props[P_MEDIA_TITLE].node.u.string;
    } else {
        summary = "No file";
    }
}

static void seconds_to_hhmmss(int64_t total_sec, char *buf, size_t size)
{
    int hours = total_sec / 3600;
    int minutes = (total_sec % 3600) / 60;
    int seconds = total_sec % 60;
    snprintf(buf, size, "%02d:%02d:%02d", hours, minutes, seconds);
}

#define APPEND(...) { \
    body_count += snprintf(body + body_count, sizeof(body) - body_count, __VA_ARGS__); \
    if (body_count >= sizeof(body)) { return; } \
}

static void write_body(void)
{
    DEBUG("writing body");

    size_t body_count = 0;

    /* L1: playback indicators and progress */

    if (op_avail(P_PLAYLIST_POS) &&
            observed_props[P_PLAYLIST_COUNT].node.u.int64 > 1) {
        APPEND("(%02" PRId64 "/%02" PRId64 ") ",
                observed_props[P_PLAYLIST_POS].node.u.int64 + 1,
                observed_props[P_PLAYLIST_COUNT].node.u.int64);
    }

    if (op_true(P_PAUSED_FOR_CACHE) || op_true(P_SEEKING)) {
        APPEND("⏲");
    } else if (op_true(P_PAUSE)) {
        APPEND("⏸");
    } else if (op_true(P_PLAY_DIRECTION) &&
            !strcmp(observed_props[P_PLAY_DIRECTION].node.u.string, "backward")) {
        APPEND("◀");
    } else {
        APPEND("▶");
    }

    if (!op_true(P_IDLE_ACTIVE) && !op_true(P_USER_DATA__DETECT_IMAGE__DETECTED) &&
            op_avail(P_TIME_POS)) {
        char com_time_time[16];
        seconds_to_hhmmss(observed_props[P_TIME_POS].node.u.int64,
                com_time_time, sizeof(com_time_time));

        if (op_avail(P_DURATION)) {
            char com_time_duration[16];
            seconds_to_hhmmss(observed_props[P_DURATION].node.u.int64,
                    com_time_duration, sizeof(com_time_duration));

            APPEND(" %s / %s (%d%%)", com_time_time, com_time_duration,
                    (int)percent_pos_rounded);
        } else {
            APPEND(" %s (%d%%)", com_time_time, (int)percent_pos_rounded);
        }

        if (op_avail(P_LOOP_FILE) &&
                strcmp(observed_props[P_LOOP_FILE].node.u.string, "no")) {
            APPEND(" 🔁");
        }
    }

    if (op_avail(P_SPEED) && observed_props[P_SPEED].node.u.double_ != 1) {
        APPEND(" (%.2fx)", observed_props[P_SPEED].node.u.double_);
    }

    if (op_true(P_MUTE)) {
        APPEND(" 🔇");
    }

    if (op_avail(P_VOLUME) && observed_props[P_VOLUME].node.u.int64 != 100) {
        APPEND(" (🔊 %" PRId64 "%%)", observed_props[P_VOLUME].node.u.int64);
    }

    if (!op_avail(P_IMAGE_DISPLAY_DURATION) ||
            !isnormal(observed_props[P_IMAGE_DISPLAY_DURATION].node.u.double_)) {
        if (!op_true(P_USER_DATA__DETECT_IMAGE__DETECTED) && op_true(P_KEEP_OPEN) &&
                strcmp(observed_props[P_KEEP_OPEN].node.u.string, "always")) {
            APPEND(" (auto)");
        }
    } else if (op_true(P_USER_DATA__DETECT_IMAGE__DETECTED)) {
        APPEND(" (ss: %.0fs)", observed_props[P_IMAGE_DISPLAY_DURATION].node.u.double_);
    }

    /* TODO: don't use OSD formatting, compose it ourself */

    /* L2: chapter title and position, if there is more than one chapter */

    if (osd_str_chapter && osd_str_chapters)
        APPEND("\nChapter: %s / %s", osd_str_chapter, osd_str_chapters);

    /* L3: edition title and position, if there is more than one edition */

    if (osd_str_edition && osd_str_editions)
        APPEND("\nEdition: %s / %s", osd_str_edition, osd_str_editions);

    /* L4: release information, if available */

    const char *album_artist_txt = metadata[M_ALBUM_ARTIST];
    if (!album_artist_txt) album_artist_txt = metadata[M_ARTIST__ESC];

    if (metadata[M_ALBUM]) {
        const char *date_txt = metadata[M_ORIGINALYEAR];
        if (!date_txt) date_txt = metadata[M_ORIGINALDATE__YEAREXT];
        if (!date_txt) date_txt = metadata[M_YEAR];
        if (!date_txt) date_txt = metadata[M_DATE__YEAREXT];

        APPEND("\n");
        if (album_artist_txt)
            APPEND("%s - ", album_artist_txt);
        APPEND("%s", metadata[M_ALBUM]);
        if (date_txt)
            APPEND(" (%s)", date_txt);
    } else {
        const char *date_txt = metadata[M_ORIGINALYEAR];
        if (!date_txt) date_txt = metadata[M_ORIGINALDATE];
        if (!date_txt) date_txt = metadata[M_YEAR];
        if (!date_txt) date_txt = metadata[M_DATE];
        if (date_txt) APPEND("\nDate: %s", date_txt);
    }

    /* L5: disc position, for multi-disc releases */

    const char *totaldiscs_txt = metadata[M_TOTALDISCS];
    if (!totaldiscs_txt) totaldiscs_txt = metadata[M_DISCTOTAL];
    if (!totaldiscs_txt) totaldiscs_txt = metadata[M_DISCC];
    const char *disc_txt = metadata[M_DISC];
    if (!disc_txt) disc_txt = metadata[M_DISCNUMBER];

    if (disc_txt && totaldiscs_txt &&
            strcmp(totaldiscs_txt, "0") &&
            strcmp(totaldiscs_txt, "1"))
        APPEND("\nDisc: %s / %s", disc_txt, totaldiscs_txt);

    /* L6: an additional message */

    const char *body_exttxt = NULL;

    if (op_true(P_EOF_REACHED) && op_avail(P_PLAYLIST_POS) &&
            observed_props[P_PLAYLIST_COUNT].node.u.int64 > 0 &&
            observed_props[P_PLAYLIST_POS].node.u.int64 >= 0) {
        if (observed_props[P_PLAYLIST_COUNT].node.u.int64 > 1 &&
                observed_props[P_PLAYLIST_POS].node.u.int64 + 1 == observed_props[P_PLAYLIST_COUNT].node.u.int64)
            body_exttxt = "end of playlist";
        else
            body_exttxt = "EOF";
    }

    if (body_exttxt) {
        if (server_body_markup) {
            APPEND("\n<b>%s</b>", body_exttxt);
        } else {
            APPEND("\n%s", body_exttxt);
        }
    }

    /* L7: perfdata */

    if (opt_true(O_PERFDATA)) {
        APPEND("\nThumbnail postprocess timing (last µs): %ld", pd_thumbnail);
        APPEND("\nPrevious ntf show rtt (µs): %ld", pd_show);
    }

    /* L8: current subtitle/lyric text, if any */

    if (opt_true(O_SEND_SUB_TEXT) && op_true(P_SUB_TEXT) && op_true(P_SUB_VISIBILITY))
        APPEND("\n%s", observed_props[P_SUB_TEXT].node.u.string);
}

/*
 * if the notification server is restarted while mpv is running, show/close will
 * start failing with 'ServiceUnknown: The name is not activatable'. the only
 * way to reset libnotify's global dbus proxy is to uninit and reinit the whole
 * library.
*/
static void ntf_reinit(void)
{
    ntf_uninit();
    ntf_init();
    if (ntf) {
        /*
         * unobserve and reobserve all properties if server_body_markup changed
         * so that affected properties get escaping added/removed, but also
         * generally to retry showing the notification (this will also reset the
         * timer, but that's ok)
         */
        for (size_t i = 0; i < sizeof(observed_props) / sizeof(observed_props[0]); i++) {
            if (!mpv_has_app_name && i == P_APP_NAME)
                continue;

            if (mpv_unobserve_property(hmpv, i) < 0)
                ERR("failed to unobserve property: %s", observed_props[i].name);

            if (mpv_observe_property(hmpv, i, observed_props[i].name, observed_props[i].format) != 0)
                ERR("failed to observe property: %s", observed_props[i].name);
        }
    }
}

static void ntf_upd(void)
{
    if (!ntf) {
        ntf_reinit();
        return;
    }

    if (rewrite_summary)
        write_summary();
    if (rewrite_body)
        write_body();

    DEBUG("sending notification");
    if (rewrite_summary || rewrite_body)
        notify_notification_update(ntf, summary, body, NULL);

    rewrite_summary = false;
    rewrite_body = false;

    GError *gerr = NULL;

    struct timespec tp[2] = {0};
    if (opt_true(O_PERFDATA))
        clock_gettime(CLOCK_MONOTONIC, &tp[0]);

    if (!notify_notification_show(ntf, &gerr)) {
        ERR("failed to show notification: %s", gerr->message);
        g_error_free(gerr);
        ntf_reinit();
    }

    if (opt_true(O_PERFDATA)) {
        clock_gettime(CLOCK_MONOTONIC, &tp[1]);
        long in_ns[2];
        in_ns[0] = tp[0].tv_sec * 1e9 + tp[0].tv_nsec;
        in_ns[1] = tp[1].tv_sec * 1e9 + tp[1].tv_nsec;
        pd_show = (in_ns[1] - in_ns[0]) / 1e3;
        rewrite_body = true;
    }
}

/*
 * screenshots shouldn't usually happen while the expire timer isn't armed, but
 * we allow it to be forced when a video reconfig happens so that we have a
 * screenshot of the current file's cover art (or first frame of a video) ready
 * so that opening a notification doesn't briefly flicker with an image from a
 * different album or mpv icon.
 */
static void queue_screenshot(bool force)
{
    if (!ntf_image_enabled || (!timer_armed && (!force && !force_open)))
        return;

    if (screenshot_in_progress) {
#if 0
        VERBOSE("aborting current screenshot command");
        mpv_abort_async_command(hmpv, UD_SCREENSHOT);
#endif
        screenshot_in_progress = false;
    }

    const char *screenshot_args[] = {
        "screenshot-raw", opts[O_SCREENSHOT_FLAGS].u.string, "rgba", NULL
    };

    int mpv_err;
    if (!(mpv_err = mpv_command_async(hmpv, UD_SCREENSHOT, screenshot_args))) {
        screenshot_in_progress = true;
        DEBUG("queued screenshot");
    } else {
        ERR("failed to queue screenshot: %d", mpv_err);
    }
}

static void ntf_rst(void)
{
    DEBUG("notification reset");
    bool timer_was_armed = timer_armed;
    timer_disarm();
    struct itimerspec new_value = {.it_value.tv_sec = opts[O_EXPIRE_TIMEOUT].u.int64};
    timerfd_settime(timer_fd, 0, &new_value, 0);
    timer_armed = true;
    if (!timer_was_armed)
        queue_screenshot(false);
    ntf_upd();
}

static bool player_considered_focused(void)
{
    return (op_true(P_FOCUSED) || mouse_hovered || opt_true(O_FOCUS_MANUAL));
}

static void done(void)
{
    if (done_actions & A_NTF_CHECK_IMAGE)
        ntf_check_image();

    if (done_actions & A_FORCED_QUEUE_SHOT)
        queue_screenshot(true);
    else if (done_actions & A_QUEUE_SHOT)
        queue_screenshot(false);

    if (done_actions & A_NTF_CLOSE && !force_open) {
        timer_disarm();
        ntf_close();
        goto finished;
    }

    /*
     * when metadata is unavailable and the player isn't idle, the track is
     * switching. just wait until metadata is ready, because otherwise the
     * summary text will flicker to show "No file" and the filename when
     * switching tracks. this is the same reason I prefer composing the
     * --osd-msg3 and --title text in lua rather than using property expansion.
     *
     * also maybe check that time-pos is ready?
     */
    if ((!player_considered_focused() || force_open) && ((metadata_avail && op_avail(P_TIME_POS)) ||
                    op_true(P_IDLE_ACTIVE))) {
        if (done_actions & A_NTF_RST)
            ntf_rst();
        else if (done_actions & A_NTF_UPD && (timer_armed || force_open))
            ntf_upd();
    }

finished:
    done_actions = 0;
    DEBUG("back to sleep ~");
}

static void on_done_screenshot(mpv_event *event)
{
    if (!ntf_image_enabled)
        return;

    DEBUG("post-processing screenshot");

    screenshot_in_progress = false;

    mpv_event_command *event_command = event->data;
    mpv_node command_node = event_command->result;

    mpv_byte_array *i_ba = NULL;
    int64_t i_w = 0;
    int64_t i_h = 0;
    int64_t i_stride = 0;

    if (command_node.format != MPV_FORMAT_NODE_MAP) {
        VERBOSE("screenshot command didn't return a map node");
        return;
    }

    mpv_node_list *list = command_node.u.list;
    for (int i = 0; i < list->num; i++) {
        char *key = list->keys[i];
        mpv_node *value = &list->values[i];

        if (!strcmp(key, "data"))
            i_ba = value->u.ba;
        else if (!strcmp(key, "w"))
            i_w = value->u.int64;
        else if (!strcmp(key, "h"))
            i_h = value->u.int64;
        else if (!strcmp(key, "stride"))
            i_stride = value->u.int64;
    }

    if (!i_ba || !i_w || !i_h || !i_stride) {
        ERR("screenshot command returned bad parameters");
        return;
    }

    thumbnail_ctx_maybe_new(i_w, i_h, i_stride);
    thumbnail_ctx_process(i_ba->data);
}

static void on_client_message(mpv_event *event)
{
    mpv_event_client_message *event_cm = event->data;

    if (event_cm->num_args < 1)
        return;

    if (!strcmp(event_cm->args[0], "close")) {
        done_actions |= A_NTF_CLOSE;
        force_open = false;
    } else if (!strcmp(event_cm->args[0], "open")) {
        done_actions |= A_NTF_RST;
        force_open = true;
    } else if (!strcmp(event_cm->args[0], "reload-config")) {
        struct mpv_node opts_previous[O_END] = {0};
        struct mpv_node script_opts_node = {0};
        opts_copy(opts_previous, opts);
        opts_destroy(opts);
        opts_copy(opts, opts_defaults);
        opts_from_file(opts);
        opts_destroy(opts_base);
        opts_copy(opts_base, opts);
        /* because we don't support saving node formats */
        if (mpv_get_property(hmpv, "options/script-opts", MPV_FORMAT_NODE,
                    &script_opts_node) == 0)
            opts_from_runtime(opts, &script_opts_node);
        opts_run_changed(opts_previous, opts);
        opts_destroy(opts_previous);
        mpv_free_node_contents(&script_opts_node);
    }
}

static int dispatch_mpv_events(void)
{
    char drain[4096];
    (void)!read(wakeup_pipe[0], drain, sizeof(drain));

    while (true) {
        mpv_event *event = mpv_wait_event(hmpv, 0);
        switch (event->event_id) {
            case MPV_EVENT_NONE:
                return 0;
            case MPV_EVENT_SHUTDOWN:
                return -1;
            case MPV_EVENT_VIDEO_RECONFIG:
                /*
                 * queueing a screenshot when receiving new file metadata
                 * usually yields a screenshot of the previous file. when a
                 * video reconfig happens it should be ready, though.
                 */
                DEBUG("video reconfig");
                done_actions |= A_FORCED_QUEUE_SHOT;
                break;
            case MPV_EVENT_SEEK:
                DEBUG("seeked");
                done_actions |= A_NTF_RST;
                break;
            case MPV_EVENT_COMMAND_REPLY:
                on_done_screenshot(event);
                break;
            case MPV_EVENT_CLIENT_MESSAGE:
                on_client_message(event);
                break;
            case MPV_EVENT_PROPERTY_CHANGE:
                on_property_change(event);
                break;
            default:
                break;
        }
    }
}

static void wakeup_mpv_events(__attribute__((unused)) void *d)
{
    (void)!write(wakeup_pipe[1], &(char){0}, 1);
}

static void check_prop_support(void)
{
    mpv_has_app_name = false;

    mpv_node property_list_node = {0};
    if (mpv_get_property(hmpv, "property-list", MPV_FORMAT_NODE,
                &property_list_node) != 0)
        return;

    if (property_list_node.format != MPV_FORMAT_NODE_ARRAY)
        return;

    mpv_node_list *list = property_list_node.u.list;
    for (int i = 0; i < list->num; i++) {
        mpv_node *value = &list->values[i];

        if (value->format != MPV_FORMAT_STRING)
            continue;

        if (!strcmp(value->u.string, observed_props[P_APP_NAME].name)) {
            mpv_free_node_contents(&property_list_node);
            mpv_has_app_name = true;
            return;
        }
    }

    mpv_free_node_contents(&property_list_node);
}

int mpv_open_cplugin(mpv_handle *mpv)
{
    int rc = -1;
    hmpv = mpv;
    client_name = mpv_client_name(hmpv);

    char *msg_level_str = mpv_get_property_string(hmpv, "msg-level");
    set_log_level(msg_level_str);
    mpv_free(msg_level_str);

    if (pipe2(wakeup_pipe, O_CLOEXEC | O_NONBLOCK) == -1) {
        ERR("pipe2() failed: %m");
        goto done;
    }

    if ((timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK)) == -1) {
        ERR("timerfd_create() failed: %m");
        goto done;
    }

    opts_copy(opts, opts_defaults);

    write_summary();
    write_body();

    ntf_init();

    opts_from_file(opts);
    opts_run_changed(opts_defaults, opts);
    done_actions = 0;
    opts_copy(opts_base, opts);

    check_prop_support();
    for (size_t i = 0; i < sizeof(observed_props) / sizeof(observed_props[0]); i++) {
        if (!mpv_has_app_name && i == P_APP_NAME)
            continue;

        if (mpv_observe_property(hmpv, i, observed_props[i].name, observed_props[i].format) != 0)
            ERR("failed to observe property: %s", observed_props[i].name);
    }

    mpv_set_wakeup_callback(hmpv, wakeup_mpv_events, NULL);

    struct pollfd pfd[2] = {
        {.fd = wakeup_pipe[0],  .events = POLLIN},
        {.fd = timer_fd,        .events = POLLIN},
    };

    while (true) {
        if (poll(pfd, 2, -1) == -1) {
            ERR("poll() failed: %m");
            break;
        }

        if (pfd[0].revents & POLLIN) {
            if (dispatch_mpv_events() == -1) {
                rc = 0;
                break;
            }
        } else if (pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            ERR("error or hangup on wakeup pipe read fd");
            break;
        }

        if (pfd[1].revents & POLLIN) {
            char drain[4096];
            (void)!read(timer_fd, drain, sizeof(drain));
            DEBUG("expire timer expired");
            done_actions |= A_NTF_CLOSE;
        } else if (pfd[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            ERR("error or hangup on timerfd");
            break;
        }

        done();
    }

done:
    thumbnail_ctx_destroy();

    ntf_uninit();

    free(osd_str_chapter);
    mpv_free(osd_str_chapters);
    free(osd_str_edition);
    mpv_free(osd_str_editions);

    metadata_destroy();

    for (size_t i = 0; i < sizeof(observed_props) / sizeof(observed_props[0]); i++) {
        if (observed_props[i].node.format == MPV_FORMAT_STRING)
            free(observed_props[i].node.u.string);
    }

    opts_destroy(opts);
    opts_destroy(opts_base);

    if (timer_fd != -1)
        close(timer_fd);

    for (int i = 0; i < 2; i++) {
        if (wakeup_pipe[i] != -1)
            close(wakeup_pipe[i]);
    }

    return rc;
}
