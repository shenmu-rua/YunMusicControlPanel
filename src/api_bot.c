/*
 * api_bot.c - ts3audiobot HTTP API client using WinHTTP
 *
 * API endpoints (verified via reverse engineering):
 *   Status: GET /api/bot/use/0/(/json/merge/(/song)/(/volume)/(/repeat)/(/random))
 *   Play:   GET /api/bot/use/0/(/play)   → 204
 *   Pause:  GET /api/bot/use/0/(/pause)  → 204
 *   Next:   GET /api/bot/use/0/(/next)   → 204
 *   Prev:   GET /api/bot/use/0/(/previous) → 204
 *   Volume: GET /api/bot/use/0/(/volume/{0-100}) → 204
 *   Seek:   GET /api/bot/use/0/(/seek/{seconds}) → 204
 */

#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "api_bot.h"
#include "cJSON.h"

#pragma comment(lib, "winhttp.lib")

static char g_base_host[256] = {0};
static int  g_base_port = 80;
static int  g_use_https = 0;

/* Persistent HTTP connection (reused across requests) */
static HINTERNET g_hSession = NULL;
static HINTERNET g_hConnect = NULL;
static wchar_t   g_whost[256] = {0};
static int       g_conn_port = 0;
static int       g_conn_https = 0;
static CRITICAL_SECTION g_conn_lock;
static INIT_ONCE g_conn_lock_once = INIT_ONCE_STATIC_INIT;
static volatile LONG g_cancel_requested = 0;
static volatile LONG g_queue_poll_counter = 0;
static ULONGLONG g_retry_after_tick = 0;

#define MAX_HTTP_RESPONSE_SIZE (1024U * 1024U)
#define BOT_RETRY_DELAY_MS 2000ULL

static BOOL CALLBACK init_connection_lock(PINIT_ONCE once, PVOID param, PVOID* context) {
    (void)once;
    (void)param;
    (void)context;
    InitializeCriticalSection(&g_conn_lock);
    return TRUE;
}

static int acquire_connection_lock(void) {
    if (!InitOnceExecuteOnce(&g_conn_lock_once, init_connection_lock, NULL, NULL))
        return 0;
    EnterCriticalSection(&g_conn_lock);
    return 1;
}

static int lock_connection(void) {
    if (InterlockedCompareExchange(&g_cancel_requested, 0, 0) != 0)
        return 0;
    if (!acquire_connection_lock()) return 0;
    if (InterlockedCompareExchange(&g_cancel_requested, 0, 0) != 0) {
        LeaveCriticalSection(&g_conn_lock);
        return 0;
    }
    return 1;
}

/* Caller must hold g_conn_lock. */
static void close_connection_locked(void) {
    if (g_hConnect) { WinHttpCloseHandle(g_hConnect); g_hConnect = NULL; }
    if (g_hSession) { WinHttpCloseHandle(g_hSession); g_hSession = NULL; }
    g_whost[0] = 0;
    g_conn_port = 0;
    g_conn_https = 0;
}

/* Caller must hold g_conn_lock. */
static int ensure_connection_locked(void) {
    if (GetTickCount64() < g_retry_after_tick) return -1;
    if (g_hSession && g_hConnect &&
        g_conn_port == g_base_port && g_conn_https == g_use_https &&
        g_whost[0] != 0) {
        return 0;
    }

    close_connection_locked();

    if (g_base_host[0] == 0) return -1;

    if (!MultiByteToWideChar(CP_UTF8, 0, g_base_host, -1, g_whost, 256))
        return -1;

    g_hSession = WinHttpOpen(L"YunMusicPlugin/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!g_hSession) return -1;

    WinHttpSetTimeouts(g_hSession, 1000, 1500, 2500, 2500);
    DWORD connect_retries = 1;
    WinHttpSetOption(g_hSession, WINHTTP_OPTION_CONNECT_RETRIES,
        &connect_retries, sizeof(connect_retries));

    g_hConnect = WinHttpConnect(g_hSession, g_whost, (INTERNET_PORT)g_base_port, 0);
    if (!g_hConnect) {
        close_connection_locked();
        return -1;
    }

    g_conn_port = g_base_port;
    g_conn_https = g_use_https;
    return 0;
}

static void close_connection(void) {
    if (!acquire_connection_lock()) return;
    close_connection_locked();
    LeaveCriticalSection(&g_conn_lock);
}

/* Caller must hold g_conn_lock. */
static void mark_connection_failed_locked(void) {
    close_connection_locked();
    g_retry_after_tick = GetTickCount64() + BOT_RETRY_DELAY_MS;
}

static int request_has_success_status(HINTERNET request) {
    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    if (!WinHttpQueryHeaders(request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status_code, &status_size, WINHTTP_NO_HEADER_INDEX)) {
        return 0;
    }
    return status_code >= 200 && status_code < 300;
}

/*
 * parse_url - Parse URL into host, port, and https flag
 */
static void parse_url(const char* url, char* host, int host_len, int* port, int* https) {
    if (!host || host_len <= 0 || !port || !https) return;
    host[0] = '\0';
    if (!url) return;

    const char* p = url;
    *https = 0;
    *port = 80;

    if (strncmp(p, "https://", 8) == 0) {
        *https = 1;
        *port = 443;
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    }

    /* Extract host:port */
    const char* colon = strchr(p, ':');
    const char* slash = strchr(p, '/');

    if (colon && (!slash || colon < slash)) {
        int hlen = (int)(colon - p);
        if (hlen >= host_len) hlen = host_len - 1;
        memcpy(host, p, hlen);
        host[hlen] = '\0';
        *port = atoi(colon + 1);
    } else {
        int hlen = slash ? (int)(slash - p) : (int)strlen(p);
        if (hlen >= host_len) hlen = host_len - 1;
        memcpy(host, p, hlen);
        host[hlen] = '\0';
    }
}

/*
 * http_get - HTTP GET using the serialized persistent connection.
 * Caller must free() the result. Returns NULL on error.
 */
static char* http_get(const wchar_t* path, int required) {
    HINTERNET hRequest = NULL;
    char* result = NULL;
    char* buffer = NULL;
    DWORD totalSize = 0;
    int read_complete = 1;

    if (!path || !lock_connection()) return NULL;
    if (ensure_connection_locked() != 0) goto cleanup;

    DWORD flags = g_use_https ? WINHTTP_FLAG_SECURE : 0;
    hRequest = WinHttpOpenRequest(g_hConnect, L"GET", path,
                                  NULL, WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) goto cleanup;

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) goto cleanup;
    if (!WinHttpReceiveResponse(hRequest, NULL)) goto cleanup;
    if (!request_has_success_status(hRequest)) goto cleanup;

    DWORD bufSize = 4096;
    buffer = (char*)malloc(bufSize);
    if (!buffer) goto cleanup;

    for (;;) {
        DWORD bytesAvailable = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &bytesAvailable)) {
            read_complete = 0;
            break;
        }
        if (bytesAvailable == 0) break;
        if (bytesAvailable > MAX_HTTP_RESPONSE_SIZE - totalSize - 1) {
            read_complete = 0;
            break;
        }
        if (totalSize + bytesAvailable + 1 > bufSize) {
            bufSize = (totalSize + bytesAvailable + 1) * 2;
            if (bufSize > MAX_HTTP_RESPONSE_SIZE)
                bufSize = MAX_HTTP_RESPONSE_SIZE;
            char* newbuf = (char*)realloc(buffer, bufSize);
            if (!newbuf) {
                read_complete = 0;
                break;
            }
            buffer = newbuf;
        }
        DWORD bytesRead = 0;
        if (!WinHttpReadData(hRequest, buffer + totalSize, bytesAvailable, &bytesRead)) {
            read_complete = 0;
            break;
        }
        totalSize += bytesRead;
    }

    if (read_complete && buffer && totalSize > 0) {
        buffer[totalSize] = '\0';
        result = buffer;
        buffer = NULL;
    }

cleanup:
    if (buffer) free(buffer);
    if (hRequest) WinHttpCloseHandle(hRequest);
    if (!result && required) mark_connection_failed_locked();
    else if (!result) {
        close_connection_locked();
        g_retry_after_tick = 0;
    }
    else g_retry_after_tick = 0;
    LeaveCriticalSection(&g_conn_lock);
    return result;
}

/*
 * http_get_no_response - HTTP GET that expects 204 (no content)
 * Returns 0 on success, -1 on error
 */
static int http_get_no_response(const wchar_t* path) {
    HINTERNET hRequest = NULL;
    int ret = -1;

    if (!path || !lock_connection()) return -1;
    if (ensure_connection_locked() != 0) goto cleanup;

    DWORD flags = g_use_https ? WINHTTP_FLAG_SECURE : 0;
    hRequest = WinHttpOpenRequest(g_hConnect, L"GET", path,
                                  NULL, WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) goto cleanup;

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) goto cleanup;
    if (!WinHttpReceiveResponse(hRequest, NULL)) goto cleanup;
    if (request_has_success_status(hRequest)) ret = 0;

cleanup:
    if (hRequest) WinHttpCloseHandle(hRequest);
    if (ret != 0) mark_connection_failed_locked();
    else g_retry_after_tick = 0;
    LeaveCriticalSection(&g_conn_lock);
    return ret;
}

void api_bot_init(const char* base_url) {
    InterlockedExchange(&g_cancel_requested, 0);
    InterlockedExchange(&g_queue_poll_counter, 0);
    if (!acquire_connection_lock()) return;
    close_connection_locked();
    g_retry_after_tick = 0;
    parse_url(base_url, g_base_host, sizeof(g_base_host), &g_base_port, &g_use_https);
    LeaveCriticalSection(&g_conn_lock);
}

void api_bot_cancel_pending(void) {
    InterlockedExchange(&g_cancel_requested, 1);
}

void api_bot_cleanup(void) {
    api_bot_cancel_pending();
    close_connection();
}

int api_bot_poll_status(BotStatus* status) {
    if (!status) return -1;
    memset(status, 0, sizeof(BotStatus));

    LONG queue_poll = InterlockedIncrement(&g_queue_poll_counter) - 1;
    int include_queue = queue_poll % 3 == 0;
    const wchar_t* status_path =
        L"/api/bot/use/0/(/json/merge/(/song)/(/volume)/(/repeat)/(/random))";

    char* response = http_get(status_path, 1);
    if (!response) return -1;

    /* Parse JSON array: [songObj, volumeFloat, repeatStr, randomBool] */
    cJSON* root = cJSON_Parse(response);
    free(response);
    if (!root || !cJSON_IsArray(root)) {
        if (root) cJSON_Delete(root);
        return -1;
    }

    /* Element 0: Song info object */
    cJSON* song_item = cJSON_GetArrayItem(root, 0);
    if (song_item && cJSON_IsObject(song_item)) {
        cJSON* title = cJSON_GetObjectItem(song_item, "Title");
        cJSON* link = cJSON_GetObjectItem(song_item, "Link");
        cJSON* pos = cJSON_GetObjectItem(song_item, "Position");
        cJSON* len = cJSON_GetObjectItem(song_item, "Length");
        cJSON* paused = cJSON_GetObjectItem(song_item, "Paused");
        cJSON* atype = cJSON_GetObjectItem(song_item, "AudioType");

        if (title && cJSON_IsString(title))
            strncpy(status->song.title, title->valuestring, sizeof(status->song.title) - 1);
        if (link && cJSON_IsString(link))
            strncpy(status->song.link, link->valuestring, sizeof(status->song.link) - 1);
        if (pos && cJSON_IsNumber(pos))
            status->song.position = pos->valuedouble;
        if (len && cJSON_IsNumber(len))
            status->song.length = len->valuedouble;
        if (paused && cJSON_IsBool(paused))
            status->song.paused = cJSON_IsTrue(paused) ? 1 : 0;
        if (atype && cJSON_IsString(atype))
            strncpy(status->song.audio_type, atype->valuestring, sizeof(status->song.audio_type) - 1);

        /* Extract song ID from link (e.g. "https://music.163.com/#/song?id=208940") */
        const char* id_start = strstr(status->song.link, "id=");
        if (id_start) {
            id_start += 3;
            const char* id_end = strchr(id_start, '&');
            if (!id_end) id_end = id_start + strlen(id_start);
            int id_len = (int)(id_end - id_start);
            if (id_len > 0 && id_len < (int)sizeof(status->song.song_id)) {
                memcpy(status->song.song_id, id_start, id_len);
                status->song.song_id[id_len] = '\0';
            }
        }
    }

    /* Element 1: Volume (float) */
    cJSON* vol_item = cJSON_GetArrayItem(root, 1);
    if (vol_item && cJSON_IsNumber(vol_item))
        status->volume = (float)vol_item->valuedouble;

    /* Element 2: Repeat (string) */
    cJSON* repeat_item = cJSON_GetArrayItem(root, 2);
    if (repeat_item && cJSON_IsString(repeat_item))
        strncpy(status->repeat, repeat_item->valuestring, sizeof(status->repeat) - 1);
    else if (repeat_item && cJSON_IsNumber(repeat_item))
        snprintf(status->repeat, sizeof(status->repeat), "%.0f",
            repeat_item->valuedouble);

    /* Element 3: Random (bool) */
    cJSON* random_item = cJSON_GetArrayItem(root, 3);
    if (random_item && cJSON_IsBool(random_item))
        status->random = cJSON_IsTrue(random_item) ? 1 : 0;

    cJSON_Delete(root);

    /* Queue sync is best-effort and must never make normal status polling fail. */
    if (include_queue) {
        char* queue_response =
            http_get(L"/api/bot/use/0/(/yun/list)", 0);
        if (queue_response) {
            cJSON* queue_root = cJSON_Parse(queue_response);
            free(queue_response);
            cJSON* value = queue_root
                ? cJSON_GetObjectItem(queue_root, "Value") : NULL;
            if (value && cJSON_IsString(value))
                bot_queue_parse(value->valuestring, &status->queue);
            if (queue_root) cJSON_Delete(queue_root);
        }
    }
    return 0;
}

/* Helper for simple GET commands (play, pause, next, prev) */
static int send_command(const char* cmd) {
    return api_bot_send_command_get(cmd);
}

int api_bot_send_command_get(const char* command) {
    if (!command || !command[0]) return -1;
    wchar_t path[512];
    char path_utf8[512];

    int written = snprintf(path_utf8, sizeof(path_utf8),
        "/api/bot/use/0/%s", command);
    if (written < 0 || (size_t)written >= sizeof(path_utf8)) return -1;
    if (!MultiByteToWideChar(CP_UTF8, 0, path_utf8, -1, path, 512))
        return -1;

    return http_get_no_response(path);
}

int api_bot_play(void) {
    return send_command("(/play)");
}

int api_bot_pause(void) {
    return send_command("(/pause)");
}

int api_bot_next(void) {
    return send_command("(/yun/next)");
}

int api_bot_prev(void) {
    return send_command("(/yun/previous)");
}

int api_bot_set_volume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "(/volume/%d)", volume);
    return send_command(cmd);
}

int api_bot_seek(double seconds) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "(/seek/%.1f)", seconds);
    return send_command(cmd);
}

/*
 * api_bot_send_command_post - Send a command via POST request
 * command: the command text (e.g. "yun play https://music.163.com/#/song?id=123")
 * Returns 0 on success, -1 on error
 */
int api_bot_send_command_post(const char* command) {
    if (!command) return -1;

    const wchar_t* path = L"/api/bot/use/0/";
    HINTERNET hRequest = NULL;
    int ret = -1;

    if (!lock_connection()) return -1;
    if (ensure_connection_locked() != 0) goto cleanup;
    DWORD flags = g_use_https ? WINHTTP_FLAG_SECURE : 0;
    hRequest = WinHttpOpenRequest(g_hConnect, L"POST", path,
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) goto cleanup;

    /* Set Content-Type header */
    WinHttpAddRequestHeaders(hRequest, L"Content-Type: text/plain; charset=utf-8",
        (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);

    DWORD cmd_len = (DWORD)strlen(command);

    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           (void*)command, cmd_len, cmd_len, 0)) {
        if (WinHttpReceiveResponse(hRequest, NULL)) {
            if (request_has_success_status(hRequest)) ret = 0;
        }
    }

cleanup:
    if (hRequest) WinHttpCloseHandle(hRequest);
    if (ret != 0) mark_connection_failed_locked();
    else g_retry_after_tick = 0;
    LeaveCriticalSection(&g_conn_lock);
    return ret;
}
