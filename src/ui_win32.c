/*
 * ui_win32.c - YunMusic Win32 UI (redesigned)
 * Interactive progress bar, compact volume popup, playlist, cover art
 */

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <winhttp.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ui_win32.h"
#include "api_bot.h"
#include "api_netease.h"
#include "lrc_parser.h"
#include "config.h"
#include "resource.h"
#include "cJSON.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")

/* ======================== GDI+ ======================== */

typedef int GpStatus;
typedef void* GpGraphics;
typedef void* GpImage;
typedef void* GpBitmap;
#define GDIP_ST_OK 0

typedef struct {
    unsigned int GdiplusVersion;
    int DebugEventCallback;
    int SuppressBackgroundThread;
    int SuppressExternalCodecs;
} GdiplusStartupInput;

typedef GpStatus (WINAPI *PFNGdiplusStartup)(ULONG_PTR*, const GdiplusStartupInput*, void*);
typedef void     (WINAPI *PFNGdiplusShutdown)(ULONG_PTR);
typedef GpStatus (WINAPI *PFNGdipCreateBitmapFromFile)(const WCHAR*, GpBitmap**);
typedef GpStatus (WINAPI *PFNGdipDisposeImage)(GpImage*);
typedef GpStatus (WINAPI *PFNGdipCreateFromHDC)(HDC, GpGraphics**);
typedef GpStatus (WINAPI *PFNGdipDrawImageRectI)(GpGraphics*, GpImage*, int, int, int, int);
typedef GpStatus (WINAPI *PFNGdipDeleteGraphics)(GpGraphics*);

static HMODULE g_gdipModule = NULL;
static ULONG_PTR g_gdipToken = 0;
static PFNGdipCreateBitmapFromFile  pGdipCreateBitmapFromFile = NULL;
static PFNGdipDisposeImage          pGdipDisposeImage = NULL;
static PFNGdipCreateFromHDC         pGdipCreateFromHDC = NULL;
static PFNGdipDrawImageRectI        pGdipDrawImageRectI = NULL;
static PFNGdipDeleteGraphics        pGdipDeleteGraphics = NULL;

/* ======================== Layout Constants ======================== */

#define YUNMUSIC_CLASS   L"YunMusicWindow"
#define VOLPOPUP_CLASS   L"YunVolPopup"
#define DESKTOP_LYRIC_CLASS L"YunDesktopLyric"
#define WIN_WIDTH   400
#define WIN_HEIGHT  600
#define MARGIN      10
#define COVER_SIZE  60

/* Colors - match TS3 system theme */
#define CLR_BG       GetSysColor(COLOR_BTNFACE)
#define CLR_TEXT     GetSysColor(COLOR_WINDOWTEXT)
#define CLR_TEXT_DIM GetSysColor(COLOR_GRAYTEXT)
#define CLR_PANEL    GetSysColor(COLOR_WINDOW)

/* Context menu IDs */
#define CM_PLAY_NOW  1
#define CM_PLAY_NEXT 2

/* ======================== Controls ======================== */

static HWND g_hwndMain = NULL;
static HWND g_hwndCover = NULL;
static HWND g_hwndTitle = NULL;
static HWND g_hwndArtist = NULL;
static HWND g_hwndProgress = NULL;
static HWND g_hwndProgressText = NULL;
static HWND g_hwndBtnPlay = NULL;
static HWND g_hwndBtnNext = NULL;
static HWND g_hwndBtnVol = NULL;
static HWND g_hwndLyrics = NULL;
static HWND g_hwndPlaylist = NULL;
static HWND g_hwndSearchEdit = NULL;
static HWND g_hwndSearchBtn = NULL;
static HWND g_hwndSearchList = NULL;

/* Volume popup */
static HWND g_hwndVolPopup = NULL;
static HWND g_hwndVolSlider = NULL;
static int g_vol_popup_visible = 0;

/* Desktop lyrics */
static HWND g_hwndDesktopLyric = NULL;
static int g_desktop_lyric_visible = 0;

/* ======================== State ======================== */

static HINSTANCE g_hInstance = NULL;
static int g_is_visible = 0;
static BotStatus g_last_status;
static int g_progress_dragging = 0;

/* GDI+ */
static GpBitmap* g_coverBitmap = NULL;
static char g_coverTempPath[MAX_PATH] = {0};
static int g_cover_generation = 0;  /* Prevents race condition on rapid song changes */

/* Lyrics */
static LyricData g_lyrics = {0};
static int g_last_lyric_index = -1;
static char g_current_song_id[64] = {0};

/* Search */
static NeteaseSearchResult g_search_result = {0};
static NeteasePlaylistResult g_playlist_search_result = {0};
static int g_search_mode = 0;  /* 0=songs, 1=playlists */
static HWND g_hwndSearchModeBtn = NULL;

/* GDI objects */
static HFONT g_fontNormal = NULL;
static HFONT g_fontSmall = NULL;
static HFONT g_fontBold = NULL;

/* Forward declarations */
static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK VolPopupProc(HWND, UINT, WPARAM, LPARAM);
static void create_controls(HWND hwnd);
static void update_ui_from_status(const BotStatus* status);
static void format_time(double seconds, char* buf, size_t len);
static void load_cover_from_file(const char* path);
static void fetch_lyrics_and_cover(const char* song_id);
static void update_lyrics_display(int position_ms);
static void do_search(const char* keyword);
static void play_song_by_id(const char* song_id);
static void send_bot_command(const char* cmd);
static int gdip_init(void);
static void gdip_shutdown(void);

/* ======================== Helpers ======================== */

static int utf8_to_wchar(const char* utf8, wchar_t* wbuf, int wbuf_chars) {
    if (!utf8 || !wbuf || wbuf_chars <= 0) return -1;
    return MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wbuf, wbuf_chars);
}

static int wchar_to_utf8(const wchar_t* wstr, char* utf8, int utf8_size) {
    if (!wstr || !utf8 || utf8_size <= 0) return -1;
    return WideCharToMultiByte(CP_UTF8, 0, wstr, -1, utf8, utf8_size, NULL, NULL);
}

static void format_time(double seconds, char* buf, size_t len) {
    int total = (int)seconds;
    int mins = total / 60;
    int secs = total % 60;
    snprintf(buf, len, "%d:%02d", mins, secs);
}

/* Draw a rounded rectangle */
static void draw_rounded_rect(HDC hdc, RECT* rc, COLORREF fill, int radius) {
    HBRUSH hBrush = CreateSolidBrush(fill);
    HPEN hPen = CreatePen(PS_SOLID, 1, fill);
    SelectObject(hdc, hBrush);
    SelectObject(hdc, hPen);
    RoundRect(hdc, rc->left, rc->top, rc->right, rc->bottom, radius, radius);
    DeleteObject(hPen);
    DeleteObject(hBrush);
}

/* ======================== GDI+ ======================== */

static int gdip_init(void) {
    g_gdipModule = LoadLibraryW(L"gdiplus.dll");
    if (!g_gdipModule) return -1;

    PFNGdiplusStartup pStartup = (PFNGdiplusStartup)GetProcAddress(g_gdipModule, "GdiplusStartup");
    pGdipCreateBitmapFromFile = (PFNGdipCreateBitmapFromFile)GetProcAddress(g_gdipModule, "GdipCreateBitmapFromFile");
    pGdipDisposeImage         = (PFNGdipDisposeImage)GetProcAddress(g_gdipModule, "GdipDisposeImage");
    pGdipCreateFromHDC        = (PFNGdipCreateFromHDC)GetProcAddress(g_gdipModule, "GdipCreateFromHDC");
    pGdipDrawImageRectI       = (PFNGdipDrawImageRectI)GetProcAddress(g_gdipModule, "GdipDrawImageRectI");
    pGdipDeleteGraphics       = (PFNGdipDeleteGraphics)GetProcAddress(g_gdipModule, "GdipDeleteGraphics");

    if (!pStartup || !pGdipCreateBitmapFromFile || !pGdipDisposeImage ||
        !pGdipCreateFromHDC || !pGdipDrawImageRectI || !pGdipDeleteGraphics) {
        FreeLibrary(g_gdipModule); g_gdipModule = NULL; return -1;
    }

    GdiplusStartupInput input = {1, 0, 0, 0};
    if (pStartup(&g_gdipToken, &input, NULL) != GDIP_ST_OK) {
        FreeLibrary(g_gdipModule); g_gdipModule = NULL; return -1;
    }

    GetTempPathA(MAX_PATH, g_coverTempPath);
    strcat(g_coverTempPath, "yunmusic_cover.jpg");
    return 0;
}

static void gdip_shutdown(void) {
    if (g_coverBitmap && pGdipDisposeImage) {
        pGdipDisposeImage((GpImage*)g_coverBitmap);
        g_coverBitmap = NULL;
    }
    if (g_gdipModule) {
        PFNGdiplusShutdown pShutdown = (PFNGdiplusShutdown)GetProcAddress(g_gdipModule, "GdiplusShutdown");
        if (pShutdown) pShutdown(g_gdipToken);
        FreeLibrary(g_gdipModule); g_gdipModule = NULL;
    }
}

static void load_cover_from_file(const char* path) {
    if (!pGdipCreateBitmapFromFile || !path || !path[0]) return;
    if (g_coverBitmap) { pGdipDisposeImage((GpImage*)g_coverBitmap); g_coverBitmap = NULL; }

    wchar_t wpath[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH);
    GpStatus st = pGdipCreateBitmapFromFile(wpath, &g_coverBitmap);
    if (st != GDIP_ST_OK) g_coverBitmap = NULL;
    if (g_hwndCover) InvalidateRect(g_hwndCover, NULL, TRUE);
}

/* ======================== Background Fetch ======================== */

typedef struct { char song_id[64]; int generation; } FetchThreadData;

/* Background thread: add playlist songs to bot queue one by one */
static volatile LONG g_playlist_add_cancel = 0;
static char (*g_playlist_added_ids)[64] = NULL;
static int g_playlist_added_count = 0;
static int g_playlist_added_cap = 0;

static void playlist_added_clear(void) {
    if (g_playlist_added_ids) { free(g_playlist_added_ids); g_playlist_added_ids = NULL; }
    g_playlist_added_count = 0;
    g_playlist_added_cap = 0;
}

static void playlist_added_push(const char* song_id) {
    if (g_playlist_added_count >= g_playlist_added_cap) {
        g_playlist_added_cap = g_playlist_added_cap ? g_playlist_added_cap * 2 : 32;
        g_playlist_added_ids = (char(*)[64])realloc(g_playlist_added_ids, 64 * g_playlist_added_cap);
    }
    strncpy(g_playlist_added_ids[g_playlist_added_count], song_id, 63);
    g_playlist_added_ids[g_playlist_added_count][63] = '\0';
    g_playlist_added_count++;
}

typedef struct {
    char (*song_ids)[64];
    char (*song_names)[256];
    int count;
} PlaylistAddThreadData;

static DWORD WINAPI playlist_add_thread_proc(LPVOID param) {
    PlaylistAddThreadData* data = (PlaylistAddThreadData*)param;
    int total = data->count;

    playlist_added_clear();

    /* Play first song immediately */
    if (total > 0 && !g_playlist_add_cancel) {
        playlist_added_push(data->song_ids[0]);
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "(/yun/play/%s)", data->song_ids[0]);
        send_bot_command(cmd);
        if (g_hwndPlaylist) {
            wchar_t witem[512];
            char item_utf8[512];
            snprintf(item_utf8, sizeof(item_utf8), "[歌单] %s", data->song_names[0]);
            utf8_to_wchar(item_utf8, witem, 512);
            SendMessageW(g_hwndPlaylist, LB_ADDSTRING, 0, (LPARAM)witem);
        }
    }

    /* Add remaining songs with delay */
    int added = 1;
    for (int i = 1; i < total; i++) {
        if (g_playlist_add_cancel) break;
        Sleep(500);
        if (g_playlist_add_cancel) break;

        playlist_added_push(data->song_ids[i]);
        char url_cmd[512];
        snprintf(url_cmd, sizeof(url_cmd),
            "(/yun/add/https%%3A%%2F%%2Fmusic.163.com%%2F%%23%%2Fsong%%3Fid%%3D%s)",
            data->song_ids[i]);
        send_bot_command(url_cmd);
        added++;

        if (g_hwndPlaylist) {
            wchar_t witem[512];
            char item_utf8[512];
            snprintf(item_utf8, sizeof(item_utf8), "[歌单] %s", data->song_names[i]);
            utf8_to_wchar(item_utf8, witem, 512);
            SendMessageW(g_hwndPlaylist, LB_ADDSTRING, 0, (LPARAM)witem);
        }

        if (g_hwndLyrics) {
            wchar_t winfo[256];
            char info[256];
            snprintf(info, sizeof(info), "Added %d/%d songs...", added, total);
            utf8_to_wchar(info, winfo, 256);
            PostMessageW(g_hwndLyrics, WM_SETTEXT, 0, (LPARAM)winfo);
        }
    }

    if (!g_playlist_add_cancel && g_hwndLyrics) {
        wchar_t winfo[256];
        char info[256];
        snprintf(info, sizeof(info), "Playlist added: %d songs", added);
        utf8_to_wchar(info, winfo, 256);
        PostMessageW(g_hwndLyrics, WM_SETTEXT, 0, (LPARAM)winfo);
    }

    g_playlist_add_cancel = 0;
    free(data->song_ids);
    free(data->song_names);
    free(data);
    return 0;
}

static DWORD WINAPI fetch_thread_proc(LPVOID param) {
    FetchThreadData* data = (FetchThreadData*)param;
    char song_id[64];
    int gen = data->generation;
    strncpy(song_id, data->song_id, sizeof(song_id) - 1);
    song_id[sizeof(song_id) - 1] = '\0';
    free(data);

    /* Lyrics */
    char* lrc_text = NULL;
    if (api_netease_get_lyrics(song_id, &lrc_text) == 0 && lrc_text) {
        LyricData new_lyrics = {0};
        if (lrc_parse(&new_lyrics, lrc_text) == 0) {
            lrc_free(&g_lyrics);
            g_lyrics = new_lyrics;
            g_last_lyric_index = -1;
        }
        free(lrc_text);
    }

    /* Cover */
    char cover_url[512] = {0};
    if (api_netease_get_cover_url(song_id, cover_url, sizeof(cover_url)) == 0) {
        if (api_netease_download_file(cover_url, g_coverTempPath) == 0) {
            if (g_hwndMain) PostMessageA(g_hwndMain, WM_APP + 1, (WPARAM)gen, 0);
        }
    }
    return 0;
}

static void fetch_lyrics_and_cover(const char* song_id) {
    if (!song_id || !song_id[0]) return;
    strncpy(g_current_song_id, song_id, sizeof(g_current_song_id) - 1);

    FetchThreadData* data = (FetchThreadData*)malloc(sizeof(FetchThreadData));
    if (!data) return;
    strncpy(data->song_id, song_id, sizeof(data->song_id) - 1);
    data->generation = ++g_cover_generation;
    HANDLE hThread = CreateThread(NULL, 0, fetch_thread_proc, data, 0, NULL);
    if (hThread) CloseHandle(hThread);
}

/* ======================== Daily Songs ======================== */

static int try_fetch_daily(HWND hwnd) {
    YunConfig* cfg = config_get();
    if (cfg->music_u[0])
        api_netease_set_cookie(cfg->music_u);

    NeteaseSearchResult* result = (NeteaseSearchResult*)malloc(sizeof(NeteaseSearchResult));
    if (result && api_netease_get_daily_songs(result) == 0 && result->count > 0) {
        PostMessageA(hwnd, WM_APP + 3, 0, (LPARAM)result);
        return 1;
    }
    free(result);
    return 0;
}

static DWORD WINAPI daily_fetch_thread(LPVOID param) {
    HWND hwnd = (HWND)param;
    YunConfig* cfg = config_get();

    if (g_hwndLyrics)
        PostMessageA(g_hwndLyrics, WM_SETTEXT, 0, (LPARAM)L"Loading daily songs...");

    /* Always try fetching first (server session or saved cookie) */
    if (try_fetch_daily(hwnd))
        return 0;

    /* Failed - need QR login */
    if (g_hwndLyrics)
        PostMessageA(g_hwndLyrics, WM_SETTEXT, 0,
            (LPARAM)L"Scan QR code in browser to login...");

    char qr_key[128] = {0};
    if (api_netease_qr_login_key(qr_key, sizeof(qr_key)) != 0) {
        PostMessageA(hwnd, WM_APP + 3, (WPARAM)-1, 0);
        return 0;
    }

    char qr_url[512] = {0};
    if (api_netease_qr_login_create(qr_key, qr_url, sizeof(qr_url)) != 0) {
        PostMessageA(hwnd, WM_APP + 3, (WPARAM)-1, 0);
        return 0;
    }

    ShellExecuteA(NULL, "open", qr_url, NULL, NULL, SW_SHOWNORMAL);

    /* Poll login status */
    char music_u[512] = {0};
    int logged_in = 0;
    for (int i = 0; i < 60; i++) {
        Sleep(2000);
        int check = api_netease_qr_login_check(qr_key, music_u, sizeof(music_u));
        if (check == 0) {
            logged_in = 1;
            break;
        }
        if (check == -1) break;
    }

    if (!logged_in) {
        PostMessageA(hwnd, WM_APP + 3, (WPARAM)-2, 0);
        return 0;
    }

    /* Save cookie if extracted */
    if (music_u[0]) {
        strncpy(cfg->music_u, music_u, sizeof(cfg->music_u) - 1);
        config_save();
        api_netease_set_cookie(music_u);
    }

    /* Wait for server session to stabilize */
    Sleep(2000);

    /* Try fetching after login - first without cookie (server session), then with cookie */
    if (try_fetch_daily(hwnd))
        return 0;

    PostMessageA(hwnd, WM_APP + 3, (WPARAM)-3, 0);
    return 0;
}

/* ======================== Version Check ======================== */

#define CURRENT_VERSION "1.2.0"

static DWORD WINAPI version_check_thread(LPVOID param) {
    HWND hwnd = (HWND)param;
    YunConfig* cfg = config_get();

    if (!cfg->update_url[0]) return 0;

    /* Parse URL */
    char host[256] = {0};
    int port = 443;
    int https = 1;
    const char* p = cfg->update_url;

    if (strncmp(p, "https://", 8) == 0) { p += 8; https = 1; port = 443; }
    else if (strncmp(p, "http://", 7) == 0) { p += 7; https = 0; port = 80; }
    else return 0;

    const char* slash = strchr(p, '/');
    const char* colon = strchr(p, ':');
    if (colon && (!slash || colon < slash)) {
        int hlen = (int)(colon - p);
        if (hlen >= 255) hlen = 254;
        memcpy(host, p, hlen);
        port = atoi(colon + 1);
    } else {
        int hlen = slash ? (int)(slash - p) : (int)strlen(p);
        if (hlen >= 255) hlen = 254;
        memcpy(host, p, hlen);
    }

    const char* path = slash ? slash : "/";

    wchar_t whost[256];
    wchar_t wpath[1024];
    char path_utf8[1024];
    MultiByteToWideChar(CP_UTF8, 0, host, -1, whost, 256);
    snprintf(path_utf8, sizeof(path_utf8), "%s", path);
    MultiByteToWideChar(CP_UTF8, 0, path_utf8, -1, wpath, 1024);

    /* Fetch version info */
    HINTERNET hS = WinHttpOpen(L"YunMusicPlugin/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hS) return 0;
    HINTERNET hC = WinHttpConnect(hS, whost, (INTERNET_PORT)port, 0);
    if (!hC) { WinHttpCloseHandle(hS); return 0; }
    DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hR = WinHttpOpenRequest(hC, L"GET", wpath,
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hR) { WinHttpCloseHandle(hC); WinHttpCloseHandle(hS); return 0; }

    WinHttpSendRequest(hR, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    WinHttpReceiveResponse(hR, NULL);

    /* Read response */
    char buf[4096] = {0};
    DWORD total = 0;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hR, &avail) || avail == 0) break;
        DWORD toRead = avail > sizeof(buf) - total - 1 ? sizeof(buf) - total - 1 : avail;
        DWORD bytesRead = 0;
        if (!WinHttpReadData(hR, buf + total, toRead, &bytesRead)) break;
        total += bytesRead;
        if (total >= sizeof(buf) - 1) break;
    }
    buf[total] = '\0';

    WinHttpCloseHandle(hR);
    WinHttpCloseHandle(hC);
    WinHttpCloseHandle(hS);

    if (!total) return 0;

    /* Parse JSON */
    cJSON* root = cJSON_Parse(buf);
    if (!root) return 0;

    /* Try GitHub format: {"tag_name":"v1.2.0", "html_url":"...", "body":"..."} */
    /* Try custom format: {"version":"1.2.0", "url":"...", "changelog":"..."} */
    const char* remote_ver = NULL;
    const char* remote_url = NULL;

    cJSON* tag = cJSON_GetObjectItem(root, "tag_name");
    if (tag && cJSON_IsString(tag)) {
        remote_ver = tag->valuestring;
        if (remote_ver[0] == 'v') remote_ver++; /* skip 'v' prefix */
    }
    if (!remote_ver) {
        cJSON* ver = cJSON_GetObjectItem(root, "version");
        if (ver && cJSON_IsString(ver)) remote_ver = ver->valuestring;
    }

    cJSON* url = cJSON_GetObjectItem(root, "html_url");
    if (!url) url = cJSON_GetObjectItem(root, "url");
    if (url && cJSON_IsString(url)) remote_url = url->valuestring;

    if (remote_ver && strcmp(remote_ver, CURRENT_VERSION) != 0) {
        /* New version available */
        char msg[512];
        if (remote_url)
            snprintf(msg, sizeof(msg), "Update available: v%s -> %s", remote_ver, remote_url);
        else
            snprintf(msg, sizeof(msg), "Update available: v%s", remote_ver);

        wchar_t wmsg[512];
        utf8_to_wchar(msg, wmsg, 512);
        if (g_hwndLyrics)
            PostMessageW(g_hwndLyrics, WM_SETTEXT, 0, (LPARAM)wmsg);
    }

    cJSON_Delete(root);
    return 0;
}

/* ======================== Bot Command Helper ======================== */

/* Parse bot URL into host/port/https */
static void parse_bot_url(char* host, int host_len, int* port, int* https) {
    YunConfig* cfg = config_get();
    const char* u = cfg->bot_api_url;
    *https = 0; *port = 80;
    if (strncmp(u, "https://", 8) == 0) { *https = 1; *port = 443; u += 8; }
    else if (strncmp(u, "http://", 7) == 0) { u += 7; }
    const char* colon = strchr(u, ':');
    const char* slash = strchr(u, '/');
    if (colon && (!slash || colon < slash)) {
        int hlen = (int)(colon - u);
        if (hlen >= host_len) hlen = host_len - 1;
        memcpy(host, u, hlen); host[hlen] = '\0';
        *port = atoi(colon + 1);
    } else {
        int hlen = slash ? (int)(slash - u) : (int)strlen(u);
        if (hlen >= host_len) hlen = host_len - 1;
        memcpy(host, u, hlen); host[hlen] = '\0';
    }
}

static void send_bot_command(const char* cmd) {
    char bhost[256] = {0};
    int bport, bhttps;
    parse_bot_url(bhost, sizeof(bhost), &bport, &bhttps);

    wchar_t whost[256], wpath[512];
    char path_utf8[512];
    MultiByteToWideChar(CP_UTF8, 0, bhost, -1, whost, 256);
    snprintf(path_utf8, sizeof(path_utf8), "/api/bot/use/0/%s", cmd);
    MultiByteToWideChar(CP_UTF8, 0, path_utf8, -1, wpath, 512);

    HINTERNET hS = WinHttpOpen(L"YunMusicPlugin/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hS) return;
    HINTERNET hC = WinHttpConnect(hS, whost, (INTERNET_PORT)bport, 0);
    if (!hC) { WinHttpCloseHandle(hS); return; }
    DWORD flags = bhttps ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hR = WinHttpOpenRequest(hC, L"GET", wpath,
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (hR) {
        WinHttpSendRequest(hR, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        WinHttpReceiveResponse(hR, NULL);
        WinHttpCloseHandle(hR);
    }
    WinHttpCloseHandle(hC);
    WinHttpCloseHandle(hS);
}

static void play_song_by_id(const char* song_id) {
    if (!song_id || !song_id[0]) return;

    /* Cancel ongoing playlist add and clear auto-added songs from bot queue */
    if (g_playlist_added_count > 0) {
        g_playlist_add_cancel = 1;
        /* Send clear to bot, then re-add manually-added songs that are in the playlist display */
        /* First: collect all current playlist titles */
        int list_count = (int)SendMessage(g_hwndPlaylist, LB_GETCOUNT, 0, 0);
        wchar_t** keep_titles = NULL;
        int keep_count = 0;
        if (list_count > 0) {
            keep_titles = (wchar_t**)malloc(sizeof(wchar_t*) * list_count);
            for (int i = 0; i < list_count; i++) {
                wchar_t item[512];
                SendMessageW(g_hwndPlaylist, LB_GETTEXT, i, (LPARAM)item);
                /* Skip auto-added entries (marked with [歌单]) */
                if (wcsncmp(item, L"[歌单] ", 4) == 0) continue;
                keep_titles[keep_count] = _wcsdup(item);
                keep_count++;
            }
        }
        /* Clear bot queue */
        send_bot_command("(/clear)");
        /* Rebuild playlist display */
        SendMessage(g_hwndPlaylist, LB_RESETCONTENT, 0, 0);
        for (int i = 0; i < keep_count; i++) {
            SendMessageW(g_hwndPlaylist, LB_ADDSTRING, 0, (LPARAM)keep_titles[i]);
            free(keep_titles[i]);
        }
        if (keep_titles) free(keep_titles);
        playlist_added_clear();
    }

    g_playlist_add_cancel = 1;
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "(/yun/play/%s)", song_id);
    send_bot_command(cmd);
}

/* ======================== Volume Popup ======================== */

static void show_vol_popup(void) {
    if (!g_hwndVolPopup) {
        WNDCLASSEXW wc = {0};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = VolPopupProc;
        wc.hInstance = g_hInstance;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = VOLPOPUP_CLASS;
        RegisterClassExW(&wc);

        g_hwndVolPopup = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            VOLPOPUP_CLASS, L"",
            WS_POPUP | WS_BORDER,
            0, 0, 40, 160,
            g_hwndMain, NULL, g_hInstance, NULL);

        g_hwndVolSlider = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | TBS_VERT | TBS_TOOLTIPS,
            4, 4, 32, 152,
            g_hwndVolPopup, (HMENU)IDC_SLIDER_VOLUME, g_hInstance, NULL);
        SendMessage(g_hwndVolSlider, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
    }

    /* Position below the volume button */
    RECT rc;
    GetWindowRect(g_hwndBtnVol, &rc);
    int vol = (int)g_last_status.volume;
    SendMessage(g_hwndVolSlider, TBM_SETPOS, TRUE, vol);
    SetWindowPos(g_hwndVolPopup, HWND_TOPMOST,
        rc.left, rc.bottom + 2, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
    ShowWindow(g_hwndVolPopup, SW_SHOW);
    g_vol_popup_visible = 1;
}

static void hide_vol_popup(void) {
    if (g_hwndVolPopup) ShowWindow(g_hwndVolPopup, SW_HIDE);
    g_vol_popup_visible = 0;
}

static void vol_apply(void) {
    int vol = (int)SendMessage(g_hwndVolSlider, TBM_GETPOS, 0, 0);
    api_bot_set_volume(vol);
    char vol_str[16];
    snprintf(vol_str, sizeof(vol_str), "%d%%", vol);
    wchar_t wvol[16];
    utf8_to_wchar(vol_str, wvol, 16);
    SetWindowTextW(g_hwndBtnVol, wvol);
}

static LRESULT CALLBACK VolPopupProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_HSCROLL:
        if ((HWND)lParam == g_hwndVolSlider)
            vol_apply();
        return 0;

    case WM_NOTIFY: {
        NMHDR* nm = (NMHDR*)lParam;
        if (nm->hwndFrom == g_hwndVolSlider && nm->code == NM_RELEASEDCAPTURE)
            vol_apply();
        return 0;
    }

    case WM_LBUTTONUP:
        vol_apply();
        return 0;

    case WM_KILLFOCUS: {
        HWND hFocus = GetFocus();
        if (hFocus && (hFocus == hwnd || IsChild(hwnd, hFocus)))
            return 0;
        vol_apply(); /* Apply final value before hiding */
        hide_vol_popup();
        return 0;
    }

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) {
            HWND hNew = (HWND)lParam;
            if (hNew && (hNew == hwnd || IsChild(hwnd, hNew)))
                return 0;
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* ======================== Desktop Lyrics Window ======================== */

#define DL_WIDTH  600
#define DL_HEIGHT 100
#define DL_LOCK_W 28

static int g_dl_locked = 0;         /* 0=unlocked(draggable), 1=locked(click-through) */
static int g_dl_last_index = -1;    /* Separate index for desktop lyrics */

static LRESULT CALLBACK DesktopLyricProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        SelectObject(memDC, memBmp);

        /* Fill with transparent color key (black = transparent via layered window) */
        HBRUSH keyBrush = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(memDC, &rc, keyBrush);
        DeleteObject(keyBrush);

        SetBkMode(memDC, TRANSPARENT);

        /* Lock icon background (slightly lighter so it's visible) */
        RECT rcLock = {0, 0, DL_LOCK_W, rc.bottom};
        HBRUSH lockBg = CreateSolidBrush(RGB(40, 40, 40));
        FillRect(memDC, &rcLock, lockBg);
        DeleteObject(lockBg);

        /* Lock symbol */
        HFONT fontLock = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI Symbol");
        HFONT oldFont = (HFONT)SelectObject(memDC, fontLock);
        SetTextColor(memDC, g_dl_locked ? RGB(255, 180, 80) : RGB(160, 160, 160));
        DrawTextA(memDC, g_dl_locked ? "O" : "o", -1, &rcLock, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        SelectObject(memDC, oldFont);
        DeleteObject(fontLock);

        /* Lyrics text area */
        int text_left = DL_LOCK_W + 4;

        if (g_lyrics.count > 0) {
            int pos_ms = (int)(g_last_status.song.position * 1000);
            int idx = lrc_find_line(&g_lyrics, pos_ms);
            if (idx < 0) idx = 0;

            if (idx > 0) {
                HFONT fs = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                    DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
                oldFont = (HFONT)SelectObject(memDC, fs);
                SetTextColor(memDC, RGB(160, 160, 160));
                RECT r = {text_left, 2, rc.right - 4, 24};
                wchar_t w[256];
                utf8_to_wchar(g_lyrics.lines[idx - 1].text, w, 256);
                DrawTextW(memDC, w, -1, &r, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                SelectObject(memDC, oldFont);
                DeleteObject(fs);
            }

            {
                HFONT fb = CreateFontW(-20, 0, 0, 0, FW_BOLD, 0, 0, 0,
                    DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
                oldFont = (HFONT)SelectObject(memDC, fb);
                SetTextColor(memDC, RGB(255, 255, 255));
                RECT r = {text_left, 26, rc.right - 4, 62};
                wchar_t w[256];
                utf8_to_wchar(g_lyrics.lines[idx].text, w, 256);
                DrawTextW(memDC, w, -1, &r, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                SelectObject(memDC, oldFont);
                DeleteObject(fb);
            }

            if (idx < g_lyrics.count - 1) {
                HFONT fs = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                    DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
                oldFont = (HFONT)SelectObject(memDC, fs);
                SetTextColor(memDC, RGB(160, 160, 160));
                RECT r = {text_left, 66, rc.right - 4, 90};
                wchar_t w[256];
                utf8_to_wchar(g_lyrics.lines[idx + 1].text, w, 256);
                DrawTextW(memDC, w, -1, &r, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                SelectObject(memDC, oldFont);
                DeleteObject(fs);
            }
        } else {
            HFONT fs = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
            oldFont = (HFONT)SelectObject(memDC, fs);
            SetTextColor(memDC, RGB(160, 160, 160));
            RECT r = {text_left, 0, rc.right, rc.bottom};
            DrawTextA(memDC, "No lyrics", -1, &r, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            SelectObject(memDC, oldFont);
            DeleteObject(fs);
        }

        BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);
        DeleteObject(memBmp);
        DeleteDC(memDC);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_NCHITTEST: {
        POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd, &pt);
        /* Lock icon area is always interactive */
        if (pt.x < DL_LOCK_W) return HTCLIENT;
        /* When locked, rest of window is click-through */
        if (g_dl_locked) return HTTRANSPARENT;
        return HTCAPTION; /* Draggable when unlocked */
    }

    case WM_LBUTTONUP: {
        POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (pt.x < DL_LOCK_W) {
            /* Toggle lock state */
            g_dl_locked = !g_dl_locked;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_RBUTTONUP:
        ShowWindow(hwnd, SW_HIDE);
        g_desktop_lyric_visible = 0;
        return 0;

    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void desktop_lyric_create(HWND hParent) {
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = DesktopLyricProc;
    wc.hInstance = g_hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = DESKTOP_LYRIC_CLASS;
    RegisterClassExW(&wc);

    int sx = GetSystemMetrics(SM_CXSCREEN);
    int sy = GetSystemMetrics(SM_CYSCREEN);
    int x = (sx - DL_WIDTH) / 2;
    int y = sy - DL_HEIGHT - 80;

    g_hwndDesktopLyric = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        DESKTOP_LYRIC_CLASS, L"",
        WS_POPUP,
        x, y, DL_WIDTH, DL_HEIGHT,
        hParent, NULL, g_hInstance, NULL);

    /* Set black as transparent color key */
    if (g_hwndDesktopLyric)
        SetLayeredWindowAttributes(g_hwndDesktopLyric, RGB(0, 0, 0), 0, LWA_COLORKEY);
}

static void desktop_lyric_toggle(void) {
    if (!g_hwndDesktopLyric) return;
    if (g_desktop_lyric_visible) {
        ShowWindow(g_hwndDesktopLyric, SW_HIDE);
        g_desktop_lyric_visible = 0;
    } else {
        g_dl_last_index = -1; /* Force repaint */
        ShowWindow(g_hwndDesktopLyric, SW_SHOW);
        InvalidateRect(g_hwndDesktopLyric, NULL, FALSE);
        g_desktop_lyric_visible = 1;
    }
}

static void desktop_lyric_update(int position_ms) {
    if (!g_hwndDesktopLyric || !g_desktop_lyric_visible) return;
    if (g_lyrics.count == 0) return;
    int idx = lrc_find_line(&g_lyrics, position_ms);
    if (idx < 0) idx = 0;
    if (idx == g_dl_last_index) return;
    g_dl_last_index = idx;
    InvalidateRect(g_hwndDesktopLyric, NULL, FALSE);
}

static void desktop_lyric_destroy(void) {
    if (g_hwndDesktopLyric) {
        DestroyWindow(g_hwndDesktopLyric);
        g_hwndDesktopLyric = NULL;
    }
    UnregisterClassW(DESKTOP_LYRIC_CLASS, g_hInstance);
}

/* ======================== Progress Bar Subclass ======================== */

static WNDPROC g_origProgressProc = NULL;

static LRESULT CALLBACK ProgressSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_LBUTTONDOWN: {
        g_progress_dragging = 1;
        SetCapture(hwnd);
        int x = GET_X_LPARAM(lParam);
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (g_last_status.song.length > 0) {
            double ratio = (double)x / (rc.right - rc.left);
            if (ratio < 0) ratio = 0;
            if (ratio > 1) ratio = 1;
            double seek_sec = ratio * g_last_status.song.length;
            int pos = (int)(ratio * 1000);
            SendMessage(hwnd, PBM_SETPOS, pos, 0);
            g_last_status.song.position = seek_sec;
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (g_progress_dragging) {
            int x = GET_X_LPARAM(lParam);
            RECT rc;
            GetClientRect(hwnd, &rc);
            if (g_last_status.song.length > 0) {
                double ratio = (double)x / (rc.right - rc.left);
                if (ratio < 0) ratio = 0;
                if (ratio > 1) ratio = 1;
                int pos = (int)(ratio * 1000);
                SendMessage(hwnd, PBM_SETPOS, pos, 0);
            }
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_progress_dragging) {
            g_progress_dragging = 0;
            ReleaseCapture();
            int x = GET_X_LPARAM(lParam);
            RECT rc;
            GetClientRect(hwnd, &rc);
            if (g_last_status.song.length > 0) {
                double ratio = (double)x / (rc.right - rc.left);
                if (ratio < 0) ratio = 0;
                if (ratio > 1) ratio = 1;
                double seek_sec = ratio * g_last_status.song.length;
                api_bot_seek(seek_sec);
                g_last_status.song.position = seek_sec;
            }
        }
        return 0;
    }
    }
    return CallWindowProcW(g_origProgressProc, hwnd, msg, wParam, lParam);
}

/* ======================== Search Edit Subclass (Enter key) ======================== */

static WNDPROC g_origSearchEditProc = NULL;

static LRESULT CALLBACK SearchEditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (wParam == VK_RETURN) {
        if (msg == WM_KEYDOWN) {
            wchar_t wkeyword[256] = {0};
            GetWindowTextW(hwnd, wkeyword, 256);
            if (wkeyword[0]) {
                char keyword[256] = {0};
                wchar_to_utf8(wkeyword, keyword, sizeof(keyword));
                if (keyword[0]) do_search(keyword);
            }
        }
        /* Suppress both WM_KEYDOWN and WM_CHAR for Enter to prevent system beep */
        return 0;
    }
    return CallWindowProcW(g_origSearchEditProc, hwnd, msg, wParam, lParam);
}

/* ======================== Controls ======================== */

static void create_controls(HWND hwnd) {
    RECT client_rc;
    GetClientRect(hwnd, &client_rc);
    int content_w = client_rc.right - MARGIN * 2;
    int y = MARGIN;

    /* Cover art (owner-drawn) */
    g_hwndCover = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
        MARGIN, MARGIN, COVER_SIZE, COVER_SIZE,
        hwnd, (HMENU)IDC_STATIC_COVER, g_hInstance, NULL);

    /* Title */
    g_hwndTitle = CreateWindowExW(0, L"STATIC", L"YunMusic",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
        MARGIN + COVER_SIZE + 8, y, content_w - COVER_SIZE - 8, 18,
        hwnd, (HMENU)IDC_STATIC_TITLE, g_hInstance, NULL);
    SendMessage(g_hwndTitle, WM_SETFONT, (WPARAM)g_fontBold, TRUE);
    y += 20;

    /* Artist */
    g_hwndArtist = CreateWindowExW(0, L"STATIC", L"未连接",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
        MARGIN + COVER_SIZE + 8, y, content_w - COVER_SIZE - 8, 16,
        hwnd, (HMENU)IDC_STATIC_ARTIST, g_hInstance, NULL);
    SendMessage(g_hwndArtist, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);
    y += 20;

    /* Progress bar (interactive - subclassed) */
    int prog_y = MARGIN + COVER_SIZE + 8;
    g_hwndProgress = CreateWindowExW(0, PROGRESS_CLASSW, L"",
        WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
        MARGIN, prog_y, content_w, 14,
        hwnd, (HMENU)IDC_PROGRESS_BAR, g_hInstance, NULL);
    SendMessage(g_hwndProgress, PBM_SETRANGE32, 0, 1000);

    /* Subclass the progress bar for mouse interaction */
    g_origProgressProc = (WNDPROC)SetWindowLongPtrW(g_hwndProgress, GWLP_WNDPROC, (LONG_PTR)ProgressSubclassProc);

    /* Progress time text */
    g_hwndProgressText = CreateWindowExW(0, L"STATIC", L"0:00 / 0:00",
        WS_CHILD | WS_VISIBLE | SS_RIGHT,
        MARGIN, prog_y + 14, content_w, 14,
        hwnd, (HMENU)IDC_STATIC_PROGRESS, g_hInstance, NULL);
    SendMessage(g_hwndProgressText, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);
    y = prog_y + 32;

    /* Playback buttons row */
    int btn_w = 56;
    int btn_h = 28;
    int btn_gap = 8;
    int total_btn_w = btn_w * 2 + btn_gap;
    int btn_x = (WIN_WIDTH - total_btn_w) / 2;

    g_hwndBtnPlay = CreateWindowExW(0, L"BUTTON", L">",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT,
        btn_x, y, btn_w, btn_h,
        hwnd, (HMENU)IDC_BTN_PLAY_PAUSE, g_hInstance, NULL);
    SendMessage(g_hwndBtnPlay, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);

    g_hwndBtnNext = CreateWindowExW(0, L"BUTTON", L">|",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT,
        btn_x + btn_w + btn_gap, y, btn_w, btn_h,
        hwnd, (HMENU)IDC_BTN_NEXT, g_hInstance, NULL);
    SendMessage(g_hwndBtnNext, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);

    /* Volume button (small, right side) */
    /* Desktop lyric toggle button (left of volume) */
    HWND hBtnLyric = CreateWindowExW(0, L"BUTTON", L"LRC",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT,
        MARGIN + content_w - 84, y, 40, btn_h,
        hwnd, (HMENU)IDC_BTN_DESKTOP_LYRIC, g_hInstance, NULL);
    SendMessage(hBtnLyric, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);

    g_hwndBtnVol = CreateWindowExW(0, L"BUTTON", L"75%",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT,
        MARGIN + content_w - 40, y, 40, btn_h,
        hwnd, (HMENU)IDC_BTN_VOLUME, g_hInstance, NULL);
    SendMessage(g_hwndBtnVol, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);

    y += btn_h + 8;

    /* Lyrics area (multiline, dark themed) */
    g_hwndLyrics = CreateWindowExW(0, L"EDIT", L"等待播放...",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_READONLY | ES_CENTER,
        MARGIN, y, content_w, 100,
        hwnd, (HMENU)IDC_LYRICS_AREA, g_hInstance, NULL);
    SendMessage(g_hwndLyrics, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
    y += 106;

    /* Playlist label */
    HWND hPlaylistLabel = CreateWindowExW(0, L"STATIC", L"播放队列",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        MARGIN, y, 80, 14,
        hwnd, NULL, g_hInstance, NULL);
    SendMessage(hPlaylistLabel, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);
    y += 16;

    /* Playlist listbox */
    g_hwndPlaylist = CreateWindowExW(0, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY,
        MARGIN, y, content_w, 120,
        hwnd, (HMENU)IDC_LIST_PLAYLIST, g_hInstance, NULL);
    SendMessage(g_hwndPlaylist, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);
    y += 126;

    /* Search row: [edit] [daily] [mode] [search] */
    HWND hBtnDaily = CreateWindowExW(0, L"BUTTON", L"日推",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT,
        MARGIN + content_w - 136, y, 38, 22,
        hwnd, (HMENU)IDC_BTN_DAILY, g_hInstance, NULL);
    SendMessage(hBtnDaily, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);

    g_hwndSearchModeBtn = CreateWindowExW(0, L"BUTTON", L"单曲",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT,
        MARGIN + content_w - 94, y, 42, 22,
        hwnd, (HMENU)IDC_BTN_SEARCH_MODE, g_hInstance, NULL);
    SendMessage(g_hwndSearchModeBtn, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);

    g_hwndSearchBtn = CreateWindowExW(0, L"BUTTON", L"搜索",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT,
        MARGIN + content_w - 48, y, 48, 22,
        hwnd, (HMENU)IDC_BTN_SEARCH, g_hInstance, NULL);
    SendMessage(g_hwndSearchBtn, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);

    g_hwndSearchEdit = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
        MARGIN, y, content_w - 142, 22,
        hwnd, (HMENU)IDC_EDIT_SEARCH, g_hInstance, NULL);
    SendMessage(g_hwndSearchEdit, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
    /* Subclass to handle Enter key */
    g_origSearchEditProc = (WNDPROC)SetWindowLongPtrW(g_hwndSearchEdit, GWLP_WNDPROC, (LONG_PTR)SearchEditSubclassProc);
    SendMessage(g_hwndSearchBtn, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);
    y += 28;

    /* Search results */
    g_hwndSearchList = CreateWindowExW(0, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY,
        MARGIN, y, content_w, 80,
        hwnd, (HMENU)IDC_LIST_SEARCH, g_hInstance, NULL);
    SendMessage(g_hwndSearchList, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);
}

/* ======================== Lyrics Display ======================== */

static void update_lyrics_display(int position_ms) {
    if (g_lyrics.count == 0) return;
    int idx = lrc_find_line(&g_lyrics, position_ms);
    if (idx < 0) idx = 0;
    if (idx == g_last_lyric_index) return;
    g_last_lyric_index = idx;

    wchar_t wtext[2048] = {0};
    int start = idx - 2;
    if (start < 0) start = 0;
    int end = start + 5;
    if (end > g_lyrics.count) end = g_lyrics.count;

    for (int i = start; i < end; i++) {
        if (i == idx) wcscat(wtext, L">> ");
        else wcscat(wtext, L"   ");
        wchar_t wline[256];
        utf8_to_wchar(g_lyrics.lines[i].text, wline, 256);
        wcscat(wtext, wline);
        if (i < end - 1) wcscat(wtext, L"\r\n");
    }
    SetWindowTextW(g_hwndLyrics, wtext);
}

/* ======================== Search ======================== */

static void do_search(const char* keyword) {
    if (!keyword || !keyword[0]) return;
    SendMessage(g_hwndSearchList, LB_RESETCONTENT, 0, 0);
    api_netease_search_free(&g_search_result);
    api_netease_search_playlists_free(&g_playlist_search_result);

    if (g_search_mode == 0) {
        /* Song search */
        int rc = api_netease_search(keyword, 20, &g_search_result);
        if (rc != 0) {
            wchar_t wkw[256];
            utf8_to_wchar(keyword, wkw, 256);
            wchar_t dbg[512];
            wsprintfW(dbg, L"[ERR rc=%d] %s", rc, wkw);
            SendMessageW(g_hwndSearchList, LB_ADDSTRING, 0, (LPARAM)dbg);
            return;
        }
        for (int i = 0; i < g_search_result.count; i++) {
            NeteaseSong* s = &g_search_result.songs[i];
            char display[512];
            int dur_sec = s->duration_ms / 1000;
            snprintf(display, sizeof(display), "%s - %s (%d:%02d)",
                s->name, s->artist, dur_sec / 60, dur_sec % 60);
            wchar_t wdisplay[512];
            utf8_to_wchar(display, wdisplay, 512);
            SendMessageW(g_hwndSearchList, LB_ADDSTRING, 0, (LPARAM)wdisplay);
        }
        if (g_search_result.count == 0)
            SetWindowTextW(g_hwndLyrics, L"未找到结果");
    } else {
        /* Playlist search */
        int rc = api_netease_search_playlists(keyword, 20, &g_playlist_search_result);
        if (rc != 0) {
            wchar_t wkw[256];
            utf8_to_wchar(keyword, wkw, 256);
            wchar_t dbg[512];
            wsprintfW(dbg, L"[ERR rc=%d] %s", rc, wkw);
            SendMessageW(g_hwndSearchList, LB_ADDSTRING, 0, (LPARAM)dbg);
            return;
        }
        for (int i = 0; i < g_playlist_search_result.count; i++) {
            NeteasePlaylist* p = &g_playlist_search_result.playlists[i];
            char display[512];
            /* Format play count: >10000 show as X.X万 */
            if (p->play_count >= 10000)
                snprintf(display, sizeof(display), "%s (%d首, %.1f万播放)",
                    p->name, p->track_count, p->play_count / 10000.0);
            else
                snprintf(display, sizeof(display), "%s (%d首, %d播放)",
                    p->name, p->track_count, p->play_count);
            wchar_t wdisplay[512];
            utf8_to_wchar(display, wdisplay, 512);
            SendMessageW(g_hwndSearchList, LB_ADDSTRING, 0, (LPARAM)wdisplay);
        }
        if (g_playlist_search_result.count == 0)
            SetWindowTextW(g_hwndLyrics, L"未找到歌单");
    }
}

static void show_search_context_menu(HWND hwnd, int x, int y) {
    int sel = (int)SendMessage(g_hwndSearchList, LB_GETCURSEL, 0, 0);
    HMENU hMenu = CreatePopupMenu();
    if (g_search_mode == 1) {
        if (sel < 0 || sel >= g_playlist_search_result.count) return;
        AppendMenuW(hMenu, MF_STRING, CM_PLAY_NOW, L"播放歌单");
    } else {
        if (sel < 0 || sel >= g_search_result.count) return;
        AppendMenuW(hMenu, MF_STRING, CM_PLAY_NOW, L"立即播放");
        AppendMenuW(hMenu, MF_STRING, CM_PLAY_NEXT, L"下一首播放");
    }
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, x, y, 0, hwnd, NULL);
    DestroyMenu(hMenu);
}

/* ======================== Playlist ======================== */

static void playlist_add(const char* song_name) {
    if (!song_name || !song_name[0]) return;
    wchar_t wname[256];
    utf8_to_wchar(song_name, wname, 256);
    SendMessageW(g_hwndPlaylist, LB_ADDSTRING, 0, (LPARAM)wname);
}

static void playlist_highlight_current(const char* song_title) {
    if (!song_title || !song_title[0] || !g_hwndPlaylist) return;

    int count = (int)SendMessage(g_hwndPlaylist, LB_GETCOUNT, 0, 0);
    wchar_t wtitle[256];
    utf8_to_wchar(song_title, wtitle, 256);

    for (int i = 0; i < count; i++) {
        wchar_t item[512];
        SendMessageW(g_hwndPlaylist, LB_GETTEXT, i, (LPARAM)item);
        if (wcsstr(item, wtitle) != NULL) {
            SendMessage(g_hwndPlaylist, LB_SETCURSEL, i, 0);
            return;
        }
    }

    /* Not found in playlist, add it and select */
    SendMessageW(g_hwndPlaylist, LB_ADDSTRING, 0, (LPARAM)wtitle);
    int new_count = (int)SendMessage(g_hwndPlaylist, LB_GETCOUNT, 0, 0);
    SendMessage(g_hwndPlaylist, LB_SETCURSEL, new_count - 1, 0);
}

/* ======================== UI Update ======================== */

static void update_ui_from_status(const BotStatus* status) {
    if (!status) return;

    /* Play/pause button */
    SetWindowTextW(g_hwndBtnPlay, status->song.paused ? L">" : L"||");

    /* Title */
    if (strlen(status->song.title) > 0) {
        wchar_t wtitle[256];
        utf8_to_wchar(status->song.title, wtitle, 256);
        SetWindowTextW(g_hwndTitle, wtitle);
    } else {
        SetWindowTextW(g_hwndTitle, L"未播放");
    }

    /* Artist (extract from title if separate field not available) */
    if (strlen(status->song.title) > 0) {
        /* Title format is usually "Song Name - Artist" */
        const char* dash = strstr(status->song.title, " - ");
        if (dash) {
            wchar_t wartist[256];
            utf8_to_wchar(dash + 3, wartist, 256);
            SetWindowTextW(g_hwndArtist, wartist);
        } else {
            SetWindowTextW(g_hwndArtist, L"");
        }
    } else {
        SetWindowTextW(g_hwndArtist, L"");
    }

    /* Progress */
    if (status->song.length > 0) {
        char time_str[64], pos_str[32], len_str[32];
        format_time(status->song.position, pos_str, sizeof(pos_str));
        format_time(status->song.length, len_str, sizeof(len_str));
        snprintf(time_str, sizeof(time_str), "%s / %s", pos_str, len_str);
        SetWindowTextA(g_hwndProgressText, time_str);

        if (!g_progress_dragging) {
            int progress = (int)(status->song.position / status->song.length * 1000);
            SendMessage(g_hwndProgress, PBM_SETPOS, progress, 0);
        }
    }

    /* Volume button */
    char vol_str[16];
    snprintf(vol_str, sizeof(vol_str), "%d%%", (int)status->volume);
    wchar_t wvol[16];
    utf8_to_wchar(vol_str, wvol, 16);
    SetWindowTextW(g_hwndBtnVol, wvol);

    /* Song change → fetch lyrics + cover */
    if (status->song.song_id[0] &&
        strcmp(status->song.song_id, g_current_song_id) != 0) {
        lrc_free(&g_lyrics);
        g_last_lyric_index = -1;
        SetWindowTextW(g_hwndLyrics, L"加载歌词中...");

        /* Remove previous song from playlist */
        if (g_current_song_id[0] && g_last_status.song.title[0]) {
            int count = (int)SendMessage(g_hwndPlaylist, LB_GETCOUNT, 0, 0);
            wchar_t wold_title[256];
            utf8_to_wchar(g_last_status.song.title, wold_title, 256);
            for (int i = 0; i < count; i++) {
                wchar_t item[512];
                SendMessageW(g_hwndPlaylist, LB_GETTEXT, i, (LPARAM)item);
                if (wcsstr(item, wold_title) != NULL) {
                    SendMessage(g_hwndPlaylist, LB_DELETESTRING, i, 0);
                    break;
                }
            }
        }

        /* Highlight in playlist (or add if not present) */
        if (strlen(status->song.title) > 0) {
            playlist_highlight_current(status->song.title);
        }

        fetch_lyrics_and_cover(status->song.song_id);
    }

    /* Song ended (no next song) → clear lyrics, playlist, progress */
    if (!status->song.song_id[0] && g_current_song_id[0]) {
        lrc_free(&g_lyrics);
        g_last_lyric_index = -1;
        g_current_song_id[0] = '\0';
        SetWindowTextW(g_hwndLyrics, L"等待播放...");
        /* Reset progress bar */
        SendMessage(g_hwndProgress, PBM_SETPOS, 0, 0);
        SetWindowTextA(g_hwndProgressText, "0:00 / 0:00");
        SetWindowTextW(g_hwndBtnPlay, L">");
        /* Remove ended song from playlist */
        if (g_last_status.song.title[0]) {
            int count = (int)SendMessage(g_hwndPlaylist, LB_GETCOUNT, 0, 0);
            wchar_t wold_title[256];
            utf8_to_wchar(g_last_status.song.title, wold_title, 256);
            for (int i = 0; i < count; i++) {
                wchar_t item[512];
                SendMessageW(g_hwndPlaylist, LB_GETTEXT, i, (LPARAM)item);
                if (wcsstr(item, wold_title) != NULL) {
                    SendMessage(g_hwndPlaylist, LB_DELETESTRING, i, 0);
                    break;
                }
            }
        }
    }

    /* Lyrics sync */
    if (g_lyrics.count > 0) {
        int pos_ms = (int)(status->song.position * 1000);
        update_lyrics_display(pos_ms);
    }

    g_last_status = *status;
}

/* ======================== Background Poll ======================== */

static DWORD WINAPI poll_thread_proc(LPVOID param) {
    HWND hwnd = (HWND)param;
    BotStatus* status = (BotStatus*)malloc(sizeof(BotStatus));
    if (status && api_bot_poll_status(status) == 0) {
        PostMessageA(hwnd, WM_APP + 4, 0, (LPARAM)status);
    } else {
        free(status);
    }
    return 0;
}

/* ======================== WndProc ======================== */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        create_controls(hwnd);
        SetTimer(hwnd, TIMER_POLL_STATUS, config_get()->poll_interval_ms, NULL);
        SetTimer(hwnd, TIMER_PROGRESS_SMOOTH, 1000, NULL);
        SetTimer(hwnd, TIMER_LYRIC_SYNC, 100, NULL);
        return 0;

    case WM_APP + 1:
        /* Only load cover if this is the latest request (prevents race on rapid song changes) */
        if ((int)wParam == g_cover_generation)
            load_cover_from_file(g_coverTempPath);
        return 0;

    case WM_APP + 3: {
        /* Daily songs result */
        if (lParam == 0 && wParam == 0) {
            SetWindowTextW(g_hwndLyrics, L"Login failed or no daily songs");
        } else if (wParam == (WPARAM)-1) {
            SetWindowTextW(g_hwndLyrics, L"Failed to get QR code");
        } else if (wParam == (WPARAM)-2) {
            SetWindowTextW(g_hwndLyrics, L"QR code expired, please try again");
        } else if (wParam == (WPARAM)-3) {
            SetWindowTextW(g_hwndLyrics, L"Failed to get daily songs");
        } else {
            NeteaseSearchResult* result = (NeteaseSearchResult*)lParam;
            if (result && result->count > 0) {
                /* Copy to global search result for interaction */
                api_netease_search_free(&g_search_result);
                g_search_result = *result;
                g_search_mode = 0;
                /* Display in search list */
                SendMessage(g_hwndSearchList, LB_RESETCONTENT, 0, 0);
                for (int i = 0; i < result->count; i++) {
                    NeteaseSong* s = &result->songs[i];
                    char display[512];
                    int dur_sec = s->duration_ms / 1000;
                    snprintf(display, sizeof(display), "%s - %s (%d:%02d)",
                        s->name, s->artist, dur_sec / 60, dur_sec % 60);
                    wchar_t wdisplay[512];
                    utf8_to_wchar(display, wdisplay, 512);
                    SendMessageW(g_hwndSearchList, LB_ADDSTRING, 0, (LPARAM)wdisplay);
                }
                /* Auto-play first song */
                play_song_by_id(g_search_result.songs[0].id);
                SetTimer(hwnd, TIMER_POLL_STATUS, 800, NULL);
                SetWindowTextW(g_hwndLyrics, L"Playing daily recommendations");
            }
            free(result);
        }
        return 0;
    }

    case WM_APP + 4: {
        /* Poll result from background thread */
        BotStatus* pStatus = (BotStatus*)lParam;
        if (pStatus) {
            update_ui_from_status(pStatus);
            free(pStatus);
        }
        /* Restore normal polling interval */
        SetTimer(hwnd, TIMER_POLL_STATUS, config_get()->poll_interval_ms, NULL);
        return 0;
    }

    case WM_TIMER:
        if (LOWORD(wParam) == TIMER_POLL_STATUS) {
            /* Start background poll to avoid blocking UI thread */
            HWND hwnd_copy = hwnd;
            HANDLE hThread = CreateThread(NULL, 0, poll_thread_proc, (LPVOID)hwnd_copy, 0, NULL);
            if (hThread) CloseHandle(hThread);
        } else if (LOWORD(wParam) == TIMER_PROGRESS_SMOOTH) {
            if (!g_last_status.song.paused && g_last_status.song.length > 0 && !g_progress_dragging) {
                g_last_status.song.position += 1.0;
                if (g_last_status.song.position > g_last_status.song.length)
                    g_last_status.song.position = g_last_status.song.length;
                char time_str[64], pos_str[32], len_str[32];
                format_time(g_last_status.song.position, pos_str, sizeof(pos_str));
                format_time(g_last_status.song.length, len_str, sizeof(len_str));
                snprintf(time_str, sizeof(time_str), "%s / %s", pos_str, len_str);
                SetWindowTextA(g_hwndProgressText, time_str);
                int progress = (int)(g_last_status.song.position / g_last_status.song.length * 1000);
                SendMessage(g_hwndProgress, PBM_SETPOS, progress, 0);
            }
        } else if (LOWORD(wParam) == TIMER_LYRIC_SYNC) {
            if (g_lyrics.count > 0 && !g_last_status.song.paused) {
                int pos_ms = (int)(g_last_status.song.position * 1000);
                update_lyrics_display(pos_ms);
                desktop_lyric_update(pos_ms);
            }
        } else if (LOWORD(wParam) == 2010) {
            /* Auto-clear temporary message */
            KillTimer(hwnd, 2010);
            if (g_lyrics.count > 0) {
                int pos_ms = (int)(g_last_status.song.position * 1000);
                update_lyrics_display(pos_ms);
            } else {
                SetWindowTextW(g_hwndLyrics, L"等待播放...");
            }
        }
        return 0;

    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lParam;
        if (dis->CtlID == IDC_STATIC_COVER) {
            HDC hdc = dis->hDC;
            RECT rc = dis->rcItem;
            HBRUSH hBg = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
            FillRect(hdc, &rc, hBg);
            DeleteObject(hBg);
            if (g_coverBitmap && pGdipCreateFromHDC && pGdipDrawImageRectI && pGdipDeleteGraphics) {
                GpGraphics* gfx = NULL;
                if (pGdipCreateFromHDC(hdc, &gfx) == GDIP_ST_OK) {
                    pGdipDrawImageRectI(gfx, (GpImage*)g_coverBitmap,
                        rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top);
                    pGdipDeleteGraphics(gfx);
                }
            } else {
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
                DrawTextA(hdc, "Music", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            return 1;
        }
        break;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_BTN_PLAY_PAUSE:
            if (g_last_status.song.paused) api_bot_play();
            else api_bot_pause();
            /* Delayed refresh: give bot time to process */
            SetTimer(hwnd, TIMER_POLL_STATUS, 500, NULL);
            return 0;

        case IDC_BTN_NEXT:
            api_bot_next();
            /* Delayed refresh: give bot time to switch song */
            SetTimer(hwnd, TIMER_POLL_STATUS, 500, NULL);
            return 0;

        case IDC_BTN_VOLUME:
            if (g_vol_popup_visible) hide_vol_popup();
            else show_vol_popup();
            return 0;

        case IDC_BTN_DESKTOP_LYRIC:
            desktop_lyric_toggle();
            return 0;

        case IDC_BTN_DAILY: {
            HANDLE hThread = CreateThread(NULL, 0, daily_fetch_thread, (LPVOID)hwnd, 0, NULL);
            if (hThread) CloseHandle(hThread);
            return 0;
        }

        case IDC_BTN_SEARCH_MODE:
            g_search_mode = !g_search_mode;
            SetWindowTextW(g_hwndSearchModeBtn, g_search_mode ? L"歌单" : L"单曲");
            return 0;

        case IDC_BTN_SEARCH: {
            wchar_t wkeyword[256] = {0};
            GetWindowTextW(g_hwndSearchEdit, wkeyword, 256);
            if (wkeyword[0]) {
                char keyword[256] = {0};
                wchar_to_utf8(wkeyword, keyword, sizeof(keyword));
                if (keyword[0]) do_search(keyword);
            }
            return 0;
        }

        case IDC_LIST_SEARCH:
            if (HIWORD(wParam) == LBN_DBLCLK) {
                int sel = (int)SendMessage(g_hwndSearchList, LB_GETCURSEL, 0, 0);
                if (g_search_mode == 1 && sel >= 0 && sel < g_playlist_search_result.count) {
                    /* Double-click playlist: fetch tracks and add all to queue */
                    NeteasePlaylist* pl = &g_playlist_search_result.playlists[sel];
                    NeteaseSearchResult tracks = {0};
                    if (api_netease_get_playlist_tracks(pl->id, &tracks) == 0 && tracks.count > 0) {
                        PlaylistAddThreadData* tdata = (PlaylistAddThreadData*)malloc(sizeof(PlaylistAddThreadData));
                        if (tdata) {
                            tdata->song_ids = (char(*)[64])malloc(sizeof(char[64]) * tracks.count);
                            tdata->song_names = (char(*)[256])malloc(sizeof(char[256]) * tracks.count);
                            tdata->count = tracks.count;
                            for (int j = 0; j < tracks.count; j++) {
                                strncpy(tdata->song_ids[j], tracks.songs[j].id, 63);
                                snprintf(tdata->song_names[j], 256, "%s - %s",
                                    tracks.songs[j].name, tracks.songs[j].artist);
                            }
                            g_playlist_add_cancel = 0;
                            HANDLE hThread = CreateThread(NULL, 0, playlist_add_thread_proc, tdata, 0, NULL);
                            if (hThread) CloseHandle(hThread);
                            SetTimer(hwnd, TIMER_POLL_STATUS, 800, NULL);
                            wchar_t winfo[256];
                            char info[256];
                            snprintf(info, sizeof(info), "Adding playlist: %s (%d songs)...", pl->name, tracks.count);
                            utf8_to_wchar(info, winfo, 256);
                            SetWindowTextW(g_hwndLyrics, winfo);
                        }
                    }
                    api_netease_search_free(&tracks);
                    return 0;
                }
                if (sel >= 0 && sel < g_search_result.count) {
                    play_song_by_id(g_search_result.songs[sel].id);
                    /* Delayed refresh: give bot time to load and play the song */
                    SetTimer(hwnd, TIMER_POLL_STATUS, 800, NULL);
                }
            }
            return 0;

        case IDC_LIST_PLAYLIST:
            if (HIWORD(wParam) == LBN_DBLCLK) {
                /* Double-click playlist item - could implement jump-to-song here */
            }
            return 0;

        case CM_PLAY_NOW: {
            int sel = (int)SendMessage(g_hwndSearchList, LB_GETCURSEL, 0, 0);
            if (g_search_mode == 1 && sel >= 0 && sel < g_playlist_search_result.count) {
                NeteasePlaylist* pl = &g_playlist_search_result.playlists[sel];
                NeteaseSearchResult tracks = {0};
                if (api_netease_get_playlist_tracks(pl->id, &tracks) == 0 && tracks.count > 0) {
                    PlaylistAddThreadData* tdata = (PlaylistAddThreadData*)malloc(sizeof(PlaylistAddThreadData));
                    if (tdata) {
                        tdata->song_ids = (char(*)[64])malloc(sizeof(char[64]) * tracks.count);
                        tdata->song_names = (char(*)[256])malloc(sizeof(char[256]) * tracks.count);
                        tdata->count = tracks.count;
                        for (int j = 0; j < tracks.count; j++) {
                            strncpy(tdata->song_ids[j], tracks.songs[j].id, 63);
                            snprintf(tdata->song_names[j], 256, "%s - %s",
                                tracks.songs[j].name, tracks.songs[j].artist);
                        }
                        g_playlist_add_cancel = 0;
                        HANDLE hThread = CreateThread(NULL, 0, playlist_add_thread_proc, tdata, 0, NULL);
                        if (hThread) CloseHandle(hThread);
                        SetTimer(hwnd, TIMER_POLL_STATUS, 800, NULL);
                    }
                }
                api_netease_search_free(&tracks);
            } else if (sel >= 0 && sel < g_search_result.count) {
                play_song_by_id(g_search_result.songs[sel].id);
                SetTimer(hwnd, TIMER_POLL_STATUS, 800, NULL);
            }
            return 0;
        }

        case CM_PLAY_NEXT: {
            int sel = (int)SendMessage(g_hwndSearchList, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < g_search_result.count) {
                NeteaseSong* song = &g_search_result.songs[sel];
                /* Use GET with URL-encoded path (POST format doesn't work) */
                char cmd[512];
                snprintf(cmd, sizeof(cmd),
                    "(/yun/add/https%%3A%%2F%%2Fmusic.163.com%%2F%%23%%2Fsong%%3Fid%%3D%s)",
                    song->id);
                send_bot_command(cmd);
                /* Add to playlist display */
                char display[512];
                snprintf(display, sizeof(display), "[队列] %s - %s", song->name, song->artist);
                wchar_t wdisplay[512];
                utf8_to_wchar(display, wdisplay, 512);
                SendMessageW(g_hwndPlaylist, LB_ADDSTRING, 0, (LPARAM)wdisplay);
                SetWindowTextW(g_hwndLyrics, L"已添加到播放队列");
                /* Auto-clear message after 3 seconds */
                SetTimer(hwnd, 2010, 3000, NULL);
            }
            return 0;
        }
        break;
        } /* end switch (LOWORD(wParam)) */

    case WM_CONTEXTMENU: {
        HWND hTarget = (HWND)wParam;
        if (hTarget == g_hwndSearchList) {
            /* Select the item under the cursor before showing menu */
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(g_hwndSearchList, &pt);
            LRESULT hit = SendMessage(g_hwndSearchList, LB_ITEMFROMPOINT, 0, MAKELPARAM(pt.x, pt.y));
            if (HIWORD(hit) == 0) {
                SendMessage(g_hwndSearchList, LB_SETCURSEL, LOWORD(hit), 0);
            }
            show_search_context_menu(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        }
        break;
    }


    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        g_is_visible = 0;
        hide_vol_popup();
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_POLL_STATUS);
        KillTimer(hwnd, TIMER_PROGRESS_SMOOTH);
        KillTimer(hwnd, TIMER_LYRIC_SYNC);
        g_hwndMain = NULL;
        g_is_visible = 0;
        return 0;

    } /* end switch */

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* ======================== Public API ======================== */

int ui_init(HINSTANCE hInstance, HWND hParent) {
    g_hInstance = hInstance;
    gdip_init();
    api_netease_init(config_get()->netease_api_url);
    api_netease_set_cookie(config_get()->music_u);

    /* Create fonts */
    g_fontNormal = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    g_fontSmall = CreateFontW(-11, 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    g_fontBold = CreateFontW(-14, 0, 0, 0, FW_BOLD, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");

    INITCOMMONCONTROLSEX icex = {sizeof(icex), ICC_BAR_CLASSES | ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icex);

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = YUNMUSIC_CLASS;
    wc.hIcon = LoadIcon(g_hInstance, MAKEINTRESOURCE(1));
    RegisterClassExW(&wc);

    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    YunConfig* cfg = config_get();
    if (cfg->window_x >= 0) x = cfg->window_x;
    if (cfg->window_y >= 0) y = cfg->window_y;

    g_hwndMain = CreateWindowExW(
        WS_EX_APPWINDOW,
        YUNMUSIC_CLASS, L"YunMusic",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        x, y, WIN_WIDTH, WIN_HEIGHT,
        hParent, NULL, hInstance, NULL);

    /* Create desktop lyrics window (hidden by default) */
    desktop_lyric_create(NULL);

    /* Check for updates in background */
    {
        HANDLE hThread = CreateThread(NULL, 0, version_check_thread, (LPVOID)g_hwndMain, 0, NULL);
        if (hThread) CloseHandle(hThread);
    }

    return g_hwndMain ? 0 : -1;
}

void ui_show(void) {
    if (g_hwndMain) {
        /* Check if window is on a visible screen, reset position if off-screen */
        RECT rc;
        GetWindowRect(g_hwndMain, &rc);
        HMONITOR mon = MonitorFromRect(&rc, MONITOR_DEFAULTTONULL);
        if (!mon) {
            /* Window is off-screen, center it on primary monitor */
            int sx = GetSystemMetrics(SM_CXSCREEN);
            int sy = GetSystemMetrics(SM_CYSCREEN);
            SetWindowPos(g_hwndMain, NULL,
                (sx - WIN_WIDTH) / 2, (sy - WIN_HEIGHT) / 2,
                0, 0, SWP_NOSIZE | SWP_NOZORDER);
        }
        ShowWindow(g_hwndMain, SW_RESTORE);
        UpdateWindow(g_hwndMain);
        SetForegroundWindow(g_hwndMain);
        BringWindowToTop(g_hwndMain);
        g_is_visible = 1;
    }
}

void ui_hide(void) {
    if (g_hwndMain) { ShowWindow(g_hwndMain, SW_HIDE); g_is_visible = 0; }
}

void ui_toggle(void) {
    if (g_is_visible) ui_hide(); else ui_show();
}

void ui_destroy(void) {
    if (g_hwndMain) {
        RECT rc;
        if (GetWindowRect(g_hwndMain, &rc)) {
            YunConfig* cfg = config_get();
            cfg->window_x = rc.left;
            cfg->window_y = rc.top;
            config_save();
        }
        DestroyWindow(g_hwndMain);
        g_hwndMain = NULL;
    }
    desktop_lyric_destroy();
    UnregisterClassW(YUNMUSIC_CLASS, g_hInstance);
    lrc_free(&g_lyrics);
    api_netease_search_free(&g_search_result);
    api_netease_search_playlists_free(&g_playlist_search_result);
    gdip_shutdown();
    if (g_coverTempPath[0]) DeleteFileA(g_coverTempPath);
    if (g_fontNormal) DeleteObject(g_fontNormal);
    if (g_fontSmall) DeleteObject(g_fontSmall);
    if (g_fontBold) DeleteObject(g_fontBold);
}

int ui_is_visible(void) {
    return g_is_visible;
}
