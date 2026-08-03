/*
 * ui_win32.c - YunMusic Win32 UI (redesigned)
 * Read-only progress, compact volume popup, playlist, cover art
 */

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ui_win32.h"
#include "api_bot.h"
#include "api_netease.h"
#include "lrc_parser.h"
#include "config.h"
#include "auth_store.h"
#include "daily_policy.h"
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

typedef void (WINAPI *GdiplusDebugEventProc)(int level, char* message);
typedef struct {
    UINT32 GdiplusVersion;
    GdiplusDebugEventProc DebugEventCallback;
    BOOL SuppressBackgroundThread;
    BOOL SuppressExternalCodecs;
} GdiplusStartupInput;

#if defined(_WIN64)
_Static_assert(sizeof(GdiplusStartupInput) == 24,
    "GdiplusStartupInput must match the Windows x64 ABI");
#endif

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
#define NETEASE_AUTH_CLASS L"YunNeteaseAuth"
#define WIN_WIDTH   420
#define WIN_HEIGHT  650
#define WIN_MIN_WIDTH  380
#define WIN_MIN_HEIGHT 560
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

/* Worker-to-UI messages. Pointer payloads are owned and freed by the UI thread. */
#define WM_YUN_LYRICS_READY  (WM_APP + 1)
#define WM_YUN_PLAYLIST_ITEM (WM_APP + 2)
#define WM_YUN_DAILY_RESULT  (WM_APP + 3)
#define WM_YUN_POLL_RESULT   (WM_APP + 4)
#define WM_YUN_STATUS_TEXT   (WM_APP + 5)
#define WM_YUN_COVER_READY   (WM_APP + 6)
#define WM_YUN_SEARCH_RESULT (WM_APP + 7)
#define WM_YUN_BOT_DONE      (WM_APP + 8)
#define WM_YUN_PLAYLIST_LOAD (WM_APP + 9)
#define WM_YUN_PLAYLIST_DONE (WM_APP + 10)
#define WM_YUN_AUTH_QR_READY (WM_APP + 11)
#define WM_YUN_AUTH_RESULT   (WM_APP + 12)
#define WM_YUN_AUTH_LOGOUT   (WM_APP + 13)

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
static HWND g_hwndBtnDesktopLyric = NULL;
static HWND g_hwndLyrics = NULL;
static HWND g_hwndPlaylist = NULL;
static HWND g_hwndPlaylistLabel = NULL;
static HWND g_hwndSearchEdit = NULL;
static HWND g_hwndSearchBtn = NULL;
static HWND g_hwndSearchList = NULL;
static HWND g_hwndBtnDaily = NULL;
static HWND g_hwndBtnAccount = NULL;
static HWND g_hwndBtnDailyPlayAll = NULL;
static HWND g_hwndBtnResultBack = NULL;

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

/* GDI+ */
static GpBitmap* g_coverBitmap = NULL;
static char g_loadedCoverPath[MAX_PATH] = {0};
static volatile LONG g_cover_generation = 0;

/* Lyrics */
static LyricData g_lyrics = {0};
static int g_last_lyric_index = -1;
static char g_current_song_id[64] = {0};

/* Search */
static NeteaseSearchResult g_search_result = {0};
static NeteasePlaylistResult g_playlist_search_result = {0};
static int g_search_mode = 0;  /* Requested search mode: 0=songs, 1=playlists */
enum SearchResultSource {
    RESULT_SOURCE_NONE,
    RESULT_SOURCE_SONG_SEARCH,
    RESULT_SOURCE_PLAYLIST_SEARCH,
    RESULT_SOURCE_PLAYLIST_TRACKS,
    RESULT_SOURCE_DAILY_SONGS
};
static enum SearchResultSource g_result_source = RESULT_SOURCE_NONE;
static char g_playlist_detail_name[256] = {0};
static HWND g_hwndSearchModeBtn = NULL;
static volatile LONG g_search_generation = 0;

/* Per-Windows-user NetEase discovery session. Never sent to the Bot API. */
static NeteaseAuthSession g_auth_session = {0};
static HWND g_hwndAuth = NULL;
static HWND g_hwndAuthQr = NULL;
static HWND g_hwndAuthStatus = NULL;
static HWND g_hwndAuthRefresh = NULL;
static HWND g_hwndAuthLogout = NULL;
static GpBitmap* g_authQrBitmap = NULL;
static char g_authQrPath[MAX_PATH] = {0};
static volatile LONG g_auth_generation = 0;
static int g_auth_start_on_create = 1;

/* GDI objects */
static HFONT g_fontNormal = NULL;
static HFONT g_fontSmall = NULL;
static HFONT g_fontBold = NULL;

/* Tracked workers: plugin shutdown waits for every handle before unloading. */
typedef DWORD (WINAPI *UiWorkerProc)(LPVOID);
typedef struct {
    UiWorkerProc proc;
    LPVOID param;
} UiWorkerContext;

static SRWLOCK g_worker_lock = SRWLOCK_INIT;
static SRWLOCK g_custom_bot_lock = SRWLOCK_INIT;
static HANDLE* g_worker_handles = NULL;
static size_t g_worker_handle_count = 0;
static size_t g_worker_handle_capacity = 0;
static HANDLE g_shutdown_event = NULL;
static volatile LONG g_ui_shutting_down = 0;
static volatile LONG g_poll_in_progress = 0;
static volatile LONG g_daily_in_progress = 0;
static volatile LONG g_playlist_load_active = 0;

/* Forward declarations */
static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK VolPopupProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK NeteaseAuthProc(HWND, UINT, WPARAM, LPARAM);
static void create_controls(HWND hwnd);
static void layout_controls(HWND hwnd);
static void update_ui_from_status(const BotStatus* status);
static void format_time(double seconds, char* buf, size_t len);
static int load_cover_from_file(const char* path);
static void fetch_lyrics_and_cover(const char* song_id);
static void update_lyrics_display(int position_ms);
static void do_search(const char* keyword);
static void play_song_by_id(const char* song_id);
static int send_bot_command(const char* cmd);
static int send_playlist_bot_command(const char* cmd);
static int playlist_ui_add(const char* song_id, const wchar_t* text);
static void playlist_ui_add_placeholder(const wchar_t* text);
static const char* playlist_ui_item_id(int index);
static void playlist_ui_delete(int index);
static void playlist_ui_clear(void);
static int playlist_ui_find_id(const char* song_id);
static int gdip_init(void);
static void gdip_shutdown(void);
static void account_window_show(void);
static void account_window_show_relogin_required(void);
static void update_result_actions_visibility(void);

/* ======================== Helpers ======================== */

static int utf8_to_wchar(const char* utf8, wchar_t* wbuf, int wbuf_chars) {
    if (!utf8 || !wbuf || wbuf_chars <= 0) return -1;
    return MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wbuf, wbuf_chars);
}

static int wchar_to_utf8(const wchar_t* wstr, char* utf8, int utf8_size) {
    if (!wstr || !utf8 || utf8_size <= 0) return -1;
    return WideCharToMultiByte(CP_UTF8, 0, wstr, -1, utf8, utf8_size, NULL, NULL);
}

static int ui_is_shutting_down(void) {
    return InterlockedCompareExchange(&g_ui_shutting_down, 0, 0) != 0;
}

static int worker_wait_or_shutdown(DWORD milliseconds) {
    if (!g_shutdown_event) {
        Sleep(milliseconds);
        return ui_is_shutting_down();
    }
    return WaitForSingleObject(g_shutdown_event, milliseconds) == WAIT_OBJECT_0;
}

static DWORD WINAPI ui_worker_trampoline(LPVOID param) {
    UiWorkerContext* context = (UiWorkerContext*)param;
    UiWorkerProc proc = context->proc;
    LPVOID worker_param = context->param;
    free(context);
    return proc(worker_param);
}

/* Caller must hold g_worker_lock exclusively. */
static void reap_finished_workers_locked(void) {
    size_t write_index = 0;
    for (size_t i = 0; i < g_worker_handle_count; i++) {
        HANDLE handle = g_worker_handles[i];
        if (WaitForSingleObject(handle, 0) == WAIT_OBJECT_0) {
            CloseHandle(handle);
        } else {
            g_worker_handles[write_index++] = handle;
        }
    }
    g_worker_handle_count = write_index;
}

static int ui_start_worker(UiWorkerProc proc, LPVOID param) {
    if (!proc) return 0;

    UiWorkerContext* context = (UiWorkerContext*)malloc(sizeof(UiWorkerContext));
    if (!context) return 0;
    context->proc = proc;
    context->param = param;

    AcquireSRWLockExclusive(&g_worker_lock);
    if (ui_is_shutting_down()) {
        ReleaseSRWLockExclusive(&g_worker_lock);
        free(context);
        return 0;
    }

    reap_finished_workers_locked();
    if (g_worker_handle_count >= g_worker_handle_capacity) {
        size_t new_capacity = g_worker_handle_capacity ? g_worker_handle_capacity * 2 : 8;
        HANDLE* new_handles =
            (HANDLE*)realloc(g_worker_handles, sizeof(HANDLE) * new_capacity);
        if (!new_handles) {
            ReleaseSRWLockExclusive(&g_worker_lock);
            free(context);
            return 0;
        }
        g_worker_handles = new_handles;
        g_worker_handle_capacity = new_capacity;
    }

    HANDLE thread = CreateThread(NULL, 0, ui_worker_trampoline, context, 0, NULL);
    if (!thread) {
        ReleaseSRWLockExclusive(&g_worker_lock);
        free(context);
        return 0;
    }

    g_worker_handles[g_worker_handle_count++] = thread;
    ReleaseSRWLockExclusive(&g_worker_lock);
    return 1;
}

static int post_owned_text(HWND hwnd, UINT message, const wchar_t* text) {
    if (!hwnd || !text || ui_is_shutting_down()) return 0;
    size_t length = wcslen(text) + 1;
    wchar_t* copy = (wchar_t*)malloc(sizeof(wchar_t) * length);
    if (!copy) return 0;
    memcpy(copy, text, sizeof(wchar_t) * length);
    if (!PostMessageW(hwnd, message, 0, (LPARAM)copy)) {
        free(copy);
        return 0;
    }
    return 1;
}

static void post_status_text(HWND hwnd, const wchar_t* text) {
    post_owned_text(hwnd, WM_YUN_STATUS_TEXT, text);
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
    HGDIOBJ oldBrush = SelectObject(hdc, hBrush);
    HGDIOBJ oldPen = SelectObject(hdc, hPen);
    RoundRect(hdc, rc->left, rc->top, rc->right, rc->bottom, radius, radius);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
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

    /* Built-in codecs are sufficient for cover JPEG/PNG files. */
    GdiplusStartupInput input = {1, NULL, FALSE, TRUE};
    if (pStartup(&g_gdipToken, &input, NULL) != GDIP_ST_OK) {
        FreeLibrary(g_gdipModule); g_gdipModule = NULL; return -1;
    }

    return 0;
}

static void gdip_shutdown(void) {
    if (g_coverBitmap && pGdipDisposeImage) {
        pGdipDisposeImage((GpImage*)g_coverBitmap);
        g_coverBitmap = NULL;
    }
    if (g_loadedCoverPath[0]) {
        DeleteFileA(g_loadedCoverPath);
        g_loadedCoverPath[0] = '\0';
    }
    if (g_gdipModule) {
        PFNGdiplusShutdown pShutdown = (PFNGdiplusShutdown)GetProcAddress(g_gdipModule, "GdiplusShutdown");
        if (pShutdown) pShutdown(g_gdipToken);
        FreeLibrary(g_gdipModule); g_gdipModule = NULL;
    }
}

static void clear_cover_image(void) {
    if (g_coverBitmap && pGdipDisposeImage) {
        pGdipDisposeImage((GpImage*)g_coverBitmap);
        g_coverBitmap = NULL;
    }
    if (g_loadedCoverPath[0]) {
        DeleteFileA(g_loadedCoverPath);
        g_loadedCoverPath[0] = '\0';
    }
    if (g_hwndCover) InvalidateRect(g_hwndCover, NULL, TRUE);
}

static int load_cover_from_file(const char* path) {
    if (!pGdipCreateBitmapFromFile || !path || !path[0]) return -1;
    clear_cover_image();

    wchar_t wpath[MAX_PATH];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH))
        return -1;
    GpStatus st = pGdipCreateBitmapFromFile(wpath, &g_coverBitmap);
    if (st != GDIP_ST_OK) g_coverBitmap = NULL;
    if (g_coverBitmap) {
        strncpy(g_loadedCoverPath, path, sizeof(g_loadedCoverPath) - 1);
        g_loadedCoverPath[sizeof(g_loadedCoverPath) - 1] = '\0';
    }
    if (g_hwndCover) InvalidateRect(g_hwndCover, NULL, TRUE);
    return g_coverBitmap ? 0 : -1;
}

/* ======================== NetEase Account Window ======================== */

typedef struct {
    HWND hwnd;
    LONG generation;
} AuthLoginThreadData;

typedef struct {
    LONG generation;
    char path[MAX_PATH];
} AuthQrReady;

typedef struct {
    LONG generation;
    int rc;
    char user_id[64];
    char nickname[128];
} AuthLoginResult;

typedef struct {
    HWND hwnd;
    NeteaseAuthSession session;
} AuthLogoutThreadData;

static int auth_generation_is_current(LONG generation) {
    return InterlockedCompareExchange(&g_auth_generation, 0, 0) == generation;
}

static void clear_auth_qr(void) {
    if (g_authQrBitmap && pGdipDisposeImage) {
        pGdipDisposeImage((GpImage*)g_authQrBitmap);
        g_authQrBitmap = NULL;
    }
    if (g_authQrPath[0]) {
        DeleteFileA(g_authQrPath);
        g_authQrPath[0] = '\0';
    }
    if (g_hwndAuthQr) InvalidateRect(g_hwndAuthQr, NULL, TRUE);
}

static int save_qr_data_uri(const char* data_uri, char* path, size_t path_size,
                            LONG generation) {
    if (!data_uri || !path || path_size == 0) return -1;
    const char* comma = strchr(data_uri, ',');
    const char* encoded = comma ? comma + 1 : data_uri;
    DWORD byte_count = 0;
    if (!CryptStringToBinaryA(encoded, 0, CRYPT_STRING_BASE64,
            NULL, &byte_count, NULL, NULL) || byte_count == 0 ||
        byte_count > 1024U * 1024U) return -1;

    BYTE* bytes = (BYTE*)malloc(byte_count);
    if (!bytes) return -1;
    if (!CryptStringToBinaryA(encoded, 0, CRYPT_STRING_BASE64,
            bytes, &byte_count, NULL, NULL)) {
        free(bytes);
        return -1;
    }

    char temp_dir[MAX_PATH];
    DWORD length = GetTempPathA(MAX_PATH, temp_dir);
    int written = length > 0 && length < MAX_PATH
        ? snprintf(path, path_size, "%syunmusic_qr_%lu_%ld.png", temp_dir,
            (unsigned long)GetCurrentProcessId(), (long)generation)
        : -1;
    if (written < 0 || (size_t)written >= path_size) {
        SecureZeroMemory(bytes, byte_count);
        free(bytes);
        return -1;
    }

    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, NULL);
    DWORD file_written = 0;
    int result = file != INVALID_HANDLE_VALUE &&
        WriteFile(file, bytes, byte_count, &file_written, NULL) &&
        file_written == byte_count ? 0 : -1;
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    SecureZeroMemory(bytes, byte_count);
    free(bytes);
    if (result != 0) DeleteFileA(path);
    return result;
}

static DWORD WINAPI auth_login_thread_proc(LPVOID param) {
    AuthLoginThreadData* data = (AuthLoginThreadData*)param;
    HWND hwnd = data->hwnd;
    LONG generation = data->generation;
    free(data);

    AuthLoginResult* final_result =
        (AuthLoginResult*)calloc(1, sizeof(AuthLoginResult));
    if (!final_result) return 0;
    final_result->generation = generation;
    final_result->rc = -1;

    char key[128] = {0};
    char* qrimg = NULL;
    char cookie[NETEASE_COOKIE_MAX] = {0};
    if (!api_netease_auth_supported() ||
        api_netease_qr_login_key(key, sizeof(key)) != 0 ||
        api_netease_qr_login_create(key, &qrimg) != 0) goto done;

    AuthQrReady* qr = (AuthQrReady*)calloc(1, sizeof(AuthQrReady));
    if (!qr) goto done;
    qr->generation = generation;
    if (save_qr_data_uri(qrimg, qr->path, sizeof(qr->path), generation) != 0 ||
        ui_is_shutting_down() || !auth_generation_is_current(generation) ||
        !PostMessageW(hwnd, WM_YUN_AUTH_QR_READY, 0, (LPARAM)qr)) {
        if (qr->path[0]) DeleteFileA(qr->path);
        free(qr);
        goto done;
    }
    free(qrimg);
    qrimg = NULL;

    for (int attempt = 0; attempt < 60; attempt++) {
        if (worker_wait_or_shutdown(2000) ||
            !auth_generation_is_current(generation)) goto done;
        int status = api_netease_qr_login_check(key, cookie, sizeof(cookie));
        if (status == NETEASE_QR_WAITING || status == NETEASE_QR_CONFIRM)
            continue;
        if (status == NETEASE_QR_EXPIRED) {
            final_result->rc = -2;
            goto done;
        }
        if (status != NETEASE_QR_SUCCESS) {
            final_result->rc = -5;
            goto done;
        }

        NeteaseAuthSession session = {0};
        strncpy(session.cookie, cookie, sizeof(session.cookie) - 1);
        if (api_netease_login_status(&session) != 0) {
            final_result->rc = -4;
            SecureZeroMemory(&session, sizeof(session));
            goto done;
        }
        char verified_cookie[NETEASE_COOKIE_MAX] = {0};
        if (auth_store_save(session.cookie) != 0 ||
            auth_store_load(verified_cookie, sizeof(verified_cookie)) != 0 ||
            strcmp(session.cookie, verified_cookie) != 0) {
            final_result->rc = -3;
            SecureZeroMemory(verified_cookie, sizeof(verified_cookie));
            SecureZeroMemory(&session, sizeof(session));
            goto done;
        }
        SecureZeroMemory(verified_cookie, sizeof(verified_cookie));
        final_result->rc = 0;
        strncpy(final_result->user_id, session.user_id,
            sizeof(final_result->user_id) - 1);
        strncpy(final_result->nickname, session.nickname,
            sizeof(final_result->nickname) - 1);
        SecureZeroMemory(&session, sizeof(session));
        SecureZeroMemory(cookie, sizeof(cookie));
        goto done;
    }
    final_result->rc = -2;

done:
    if (qrimg) free(qrimg);
    SecureZeroMemory(cookie, sizeof(cookie));
    if (ui_is_shutting_down() || !auth_generation_is_current(generation) ||
        !PostMessageW(hwnd, WM_YUN_AUTH_RESULT, 0, (LPARAM)final_result)) {
        free(final_result);
    }
    return 0;
}

static DWORD WINAPI auth_logout_thread_proc(LPVOID param) {
    AuthLogoutThreadData* data = (AuthLogoutThreadData*)param;
    api_netease_logout(&data->session);
    SecureZeroMemory(&data->session, sizeof(data->session));
    HWND hwnd = data->hwnd;
    free(data);
    if (!ui_is_shutting_down()) PostMessageW(hwnd, WM_YUN_AUTH_LOGOUT, 0, 0);
    return 0;
}

static void account_start_login(void) {
    if (!g_hwndAuth) return;
    LONG generation = InterlockedIncrement(&g_auth_generation);
    clear_auth_qr();
    if (!api_netease_auth_supported()) {
        SetWindowTextW(g_hwndAuthStatus,
            L"网易云 API 地址无效，请检查配置。");
        EnableWindow(g_hwndAuthRefresh, TRUE);
        return;
    }
    SetWindowTextW(g_hwndAuthStatus, L"正在生成登录二维码...");
    EnableWindow(g_hwndAuthRefresh, FALSE);
    AuthLoginThreadData* data =
        (AuthLoginThreadData*)calloc(1, sizeof(AuthLoginThreadData));
    if (!data) return;
    data->hwnd = g_hwndAuth;
    data->generation = generation;
    if (!ui_start_worker(auth_login_thread_proc, data)) {
        free(data);
        EnableWindow(g_hwndAuthRefresh, TRUE);
        SetWindowTextW(g_hwndAuthStatus, L"无法启动登录任务");
    }
}

static LRESULT CALLBACK NeteaseAuthProc(HWND hwnd, UINT msg,
                                        WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        /* CreateWindowEx sends WM_CREATE before it returns to the caller. */
        g_hwndAuth = hwnd;
        g_hwndAuthQr = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_OWNERDRAW, 58, 56, 244, 244,
            hwnd, (HMENU)IDC_AUTH_QR, g_hInstance, NULL);
        g_hwndAuthStatus = CreateWindowExW(0, L"STATIC",
            g_auth_session.cookie[0] ? L"已保存个人账号会话" : L"尚未登录",
            WS_CHILD | WS_VISIBLE | SS_CENTER, 20, 310, 320, 48,
            hwnd, (HMENU)IDC_AUTH_STATUS, g_hInstance, NULL);
        SendMessage(g_hwndAuthStatus, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
        g_hwndAuthRefresh = CreateWindowExW(0, L"BUTTON", L"登录/刷新二维码",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 20, 370, 132, 30,
            hwnd, (HMENU)IDC_AUTH_REFRESH, g_hInstance, NULL);
        g_hwndAuthLogout = CreateWindowExW(0, L"BUTTON", L"退出个人账号",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 158, 370, 96, 30,
            hwnd, (HMENU)IDC_AUTH_LOGOUT, g_hInstance, NULL);
        CreateWindowExW(0, L"BUTTON", L"关闭", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            260, 370, 80, 30, hwnd, (HMENU)IDC_AUTH_CLOSE, g_hInstance, NULL);
        SendMessage(g_hwndAuthRefresh, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);
        SendMessage(g_hwndAuthLogout, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);
        EnableWindow(g_hwndAuthLogout, g_auth_session.cookie[0] != '\0');
        if (!g_auth_session.cookie[0] && g_auth_start_on_create)
            account_start_login();
        return 0;

    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lParam;
        if (dis->CtlID == IDC_AUTH_QR) {
            FillRect(dis->hDC, &dis->rcItem, (HBRUSH)GetStockObject(WHITE_BRUSH));
            if (g_authQrBitmap && pGdipCreateFromHDC && pGdipDrawImageRectI &&
                pGdipDeleteGraphics) {
                GpGraphics* graphics = NULL;
                if (pGdipCreateFromHDC(dis->hDC, &graphics) == GDIP_ST_OK) {
                    pGdipDrawImageRectI(graphics, (GpImage*)g_authQrBitmap,
                        4, 4, 234, 234);
                    pGdipDeleteGraphics(graphics);
                }
            } else {
                SetBkMode(dis->hDC, TRANSPARENT);
                DrawTextW(dis->hDC, L"等待二维码", -1, &dis->rcItem,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            return TRUE;
        }
        break;
    }

    case WM_YUN_AUTH_QR_READY: {
        AuthQrReady* result = (AuthQrReady*)lParam;
        if (result && auth_generation_is_current(result->generation)) {
            clear_auth_qr();
            wchar_t path[MAX_PATH];
            if (MultiByteToWideChar(CP_UTF8, 0, result->path, -1,
                    path, MAX_PATH) && pGdipCreateBitmapFromFile &&
                pGdipCreateBitmapFromFile(path, &g_authQrBitmap) == GDIP_ST_OK) {
                strncpy(g_authQrPath, result->path,
                    sizeof(g_authQrPath) - 1);
                SetWindowTextW(g_hwndAuthStatus,
                    L"请使用网易云音乐扫码并在手机上确认登录");
                InvalidateRect(g_hwndAuthQr, NULL, TRUE);
            } else {
                DeleteFileA(result->path);
                SetWindowTextW(g_hwndAuthStatus, L"二维码图片加载失败");
            }
        } else if (result && result->path[0]) {
            DeleteFileA(result->path);
        }
        free(result);
        return 0;
    }

    case WM_YUN_AUTH_RESULT: {
        AuthLoginResult* result = (AuthLoginResult*)lParam;
        EnableWindow(g_hwndAuthRefresh, TRUE);
        if (!result || !auth_generation_is_current(result->generation)) {
            free(result);
            return 0;
        }
        if (result->rc == 0) {
            SecureZeroMemory(&g_auth_session, sizeof(g_auth_session));
            if (auth_store_load(g_auth_session.cookie,
                    sizeof(g_auth_session.cookie)) == 0) {
                strncpy(g_auth_session.user_id, result->user_id,
                    sizeof(g_auth_session.user_id) - 1);
                strncpy(g_auth_session.nickname, result->nickname,
                    sizeof(g_auth_session.nickname) - 1);
                g_auth_session.logged_in = 1;
                wchar_t nickname[160];
                utf8_to_wchar(g_auth_session.nickname, nickname, 160);
                wchar_t status[220];
                swprintf(status, 220, L"已登录个人账号：%s", nickname);
                SetWindowTextW(g_hwndAuthStatus, status);
                EnableWindow(g_hwndAuthLogout, TRUE);
            }
        } else if (result->rc == -2) {
            SetWindowTextW(g_hwndAuthStatus, L"二维码已过期，请刷新");
        } else if (result->rc == -3) {
            SetWindowTextW(g_hwndAuthStatus, L"登录成功，但加密保存失败");
        } else if (result->rc == -4) {
            SetWindowTextW(g_hwndAuthStatus,
                L"扫码已确认，但 API 未接受登录 Cookie");
        } else if (result->rc == -5) {
            SetWindowTextW(g_hwndAuthStatus,
                L"二维码状态接口返回异常，请刷新重试");
        } else {
            SetWindowTextW(g_hwndAuthStatus, L"登录失败，请刷新二维码重试");
        }
        free(result);
        return 0;
    }

    case WM_YUN_AUTH_LOGOUT:
        SetWindowTextW(g_hwndAuthStatus, L"已退出个人账号");
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_AUTH_REFRESH) {
            account_start_login();
            return 0;
        }
        if (LOWORD(wParam) == IDC_AUTH_LOGOUT) {
            AuthLogoutThreadData* data =
                (AuthLogoutThreadData*)calloc(1, sizeof(AuthLogoutThreadData));
            if (data) {
                data->hwnd = hwnd;
                data->session = g_auth_session;
            }
            InterlockedIncrement(&g_auth_generation);
            clear_auth_qr();
            auth_store_delete();
            SecureZeroMemory(&g_auth_session, sizeof(g_auth_session));
            EnableWindow(g_hwndAuthLogout, FALSE);
            SetWindowTextW(g_hwndAuthStatus, L"正在退出个人账号...");
            if (data && !ui_start_worker(auth_logout_thread_proc, data)) {
                SecureZeroMemory(&data->session, sizeof(data->session));
                free(data);
                PostMessageW(hwnd, WM_YUN_AUTH_LOGOUT, 0, 0);
            }
            return 0;
        }
        if (LOWORD(wParam) == IDC_AUTH_CLOSE) {
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        break;

    case WM_CLOSE:
        InterlockedIncrement(&g_auth_generation);
        clear_auth_qr();
        if (g_hwndAuthRefresh) EnableWindow(g_hwndAuthRefresh, TRUE);
        if (g_hwndAuthStatus && !g_auth_session.cookie[0])
            SetWindowTextW(g_hwndAuthStatus,
                L"登录已暂停，请点击“登录/刷新二维码”继续");
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_DESTROY:
        InterlockedIncrement(&g_auth_generation);
        clear_auth_qr();
        g_hwndAuth = NULL;
        g_hwndAuthQr = NULL;
        g_hwndAuthStatus = NULL;
        g_hwndAuthRefresh = NULL;
        g_hwndAuthLogout = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void account_window_show(void) {
    if (!g_hwndAuth) {
        WNDCLASSEXW wc = {0};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = NeteaseAuthProc;
        wc.hInstance = g_hInstance;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = NETEASE_AUTH_CLASS;
        RegisterClassExW(&wc);
        g_hwndAuth = CreateWindowExW(WS_EX_TOOLWINDOW,
            NETEASE_AUTH_CLASS, L"网易云个人账号",
            WS_POPUP | WS_CAPTION | WS_SYSMENU,
            CW_USEDEFAULT, CW_USEDEFAULT, 376, 452,
            g_hwndMain, NULL, g_hInstance, NULL);
    }
    if (g_hwndAuth) {
        ShowWindow(g_hwndAuth, SW_SHOWNORMAL);
        SetForegroundWindow(g_hwndAuth);
    }
}

static void account_window_show_relogin_required(void) {
    g_auth_start_on_create = 0;
    account_window_show();
    g_auth_start_on_create = 1;
    InterlockedIncrement(&g_auth_generation);
    clear_auth_qr();
    if (g_hwndAuthStatus)
        SetWindowTextW(g_hwndAuthStatus,
            L"个人账号登录已失效，请点击“登录/刷新二维码”");
    if (g_hwndAuthRefresh) EnableWindow(g_hwndAuthRefresh, TRUE);
    if (g_hwndAuthLogout) EnableWindow(g_hwndAuthLogout, FALSE);
}

/* ======================== Background Fetch ======================== */

typedef struct {
    char song_id[64];
    LONG generation;
    HWND hwnd;
} FetchThreadData;

typedef struct {
    LONG generation;
    int success;
    LyricData lyrics;
} LyricsFetchResult;

typedef struct {
    LONG generation;
    char path[MAX_PATH];
} CoverFetchResult;

/* Background thread: add playlist songs to bot queue one by one */
static volatile LONG g_playlist_add_cancel = 0;
static volatile LONG g_playlist_add_active = 0;
static volatile LONG g_playlist_added_count = 0;

static void playlist_added_clear(void) {
    InterlockedExchange(&g_playlist_added_count, 0);
}

static void playlist_added_push(const char* song_id) {
    (void)song_id;
    InterlockedIncrement(&g_playlist_added_count);
}

typedef struct {
    char (*song_ids)[64];
    char (*song_names)[256];
    int count;
    int daily_source;
    int play_first_if_idle;
    HWND hwnd;
} PlaylistAddThreadData;

typedef struct {
    char song_id[64];
    wchar_t text[512];
} PlaylistItemMessage;

static void post_playlist_item(HWND hwnd, const char* song_id,
                               const wchar_t* text) {
    if (!hwnd || !song_id || !text || ui_is_shutting_down()) return;
    PlaylistItemMessage* item =
        (PlaylistItemMessage*)calloc(1, sizeof(PlaylistItemMessage));
    if (!item) return;
    strncpy(item->song_id, song_id, sizeof(item->song_id) - 1);
    wcsncpy(item->text, text, 511);
    item->text[511] = L'\0';
    if (!PostMessageW(hwnd, WM_YUN_PLAYLIST_ITEM, 0, (LPARAM)item))
        free(item);
}

static void playlist_thread_data_free(PlaylistAddThreadData* data) {
    if (!data) return;
    free(data->song_ids);
    free(data->song_names);
    free(data);
}

static DWORD WINAPI playlist_add_thread_proc(LPVOID param) {
    PlaylistAddThreadData* data = (PlaylistAddThreadData*)param;
    int total = data->count;
    int added = 0;
    int failed = 0;
    int lowest_queued_index = total;
    int play_first_if_idle = data->play_first_if_idle;
    const wchar_t* failed_message =
        L"Bot 网易云/VIP 登录可能失效，请联系管理员执行 !yun login";

    playlist_added_clear();

    if (play_first_if_idle) {
        BotStatus live_status;
        if (api_bot_poll_status(&live_status) == 0)
            play_first_if_idle = live_status.song.title[0] == '\0';
    }

    /* An idle Bot does not start after yun add, so explicitly play item 1. */
    if (play_first_if_idle && total > 0 && !g_playlist_add_cancel &&
        !ui_is_shutting_down()) {
        char play_command[128];
        snprintf(play_command, sizeof(play_command), "(/yun/play/%s)",
            data->song_ids[0]);
        if (send_playlist_bot_command(play_command) == 0) {
            playlist_added_push(data->song_ids[0]);
            added = 1;
        } else if (!g_playlist_add_cancel && !ui_is_shutting_down()) {
            failed = 1;
            InterlockedExchange(&g_playlist_add_cancel, 1);
            post_status_text(data->hwnd, failed_message);
        }
    }

    /* The Bot queue is LIFO: send bottom-to-top so playback stays top-to-bottom. */
    int send_number = 0;
    for (;;) {
        int i = daily_queue_send_index(send_number, total,
            play_first_if_idle);
        if (i < 0) break;
        if (g_playlist_add_cancel || ui_is_shutting_down()) break;
        if (send_number > 0 && worker_wait_or_shutdown(500)) break;
        if (g_playlist_add_cancel || ui_is_shutting_down()) break;

        char url_cmd[512];
        if (daily_build_add_command(data->song_ids[i], url_cmd,
                sizeof(url_cmd)) != 0 ||
            send_playlist_bot_command(url_cmd) != 0) {
            if (!g_playlist_add_cancel && !ui_is_shutting_down())
                post_status_text(data->hwnd, failed_message);
            failed = 1;
            InterlockedExchange(&g_playlist_add_cancel, 1);
            break;
        }
        playlist_added_push(data->song_ids[i]);
        added++;
        lowest_queued_index = i;

        wchar_t winfo[256];
        char info[256];
        snprintf(info, sizeof(info), "已添加 %d/%d", added, total);
        utf8_to_wchar(info, winfo, 256);
        post_status_text(data->hwnd, winfo);
        send_number++;
    }

    /* Display successfully queued items in their intended playback order. */
    for (int i = lowest_queued_index; i < total; i++) {
        wchar_t item_text[512];
        char item_utf8[512];
        snprintf(item_utf8, sizeof(item_utf8), "%s %s",
            data->daily_source ? "[日推]" : "[歌单]", data->song_names[i]);
        utf8_to_wchar(item_utf8, item_text, 512);
        post_playlist_item(data->hwnd, data->song_ids[i], item_text);
    }

    if (!g_playlist_add_cancel && !ui_is_shutting_down()) {
        wchar_t winfo[256];
        char info[256];
        snprintf(info, sizeof(info), data->daily_source
            ? "每日推荐已添加：%d/%d" : "歌单前15首已添加：%d/%d",
            added, total);
        utf8_to_wchar(info, winfo, 256);
        post_status_text(data->hwnd, winfo);
    } else if (!failed && !ui_is_shutting_down()) {
        post_status_text(data->hwnd, L"已取消添加歌单");
    }

    if (!ui_is_shutting_down())
        PostMessageW(data->hwnd, WM_YUN_PLAYLIST_DONE,
            MAKEWPARAM(added, total), (LPARAM)failed);
    playlist_thread_data_free(data);
    InterlockedExchange(&g_playlist_add_active, 0);
    return 0;
}

static int make_cover_temp_path(char* path, size_t path_size, LONG generation) {
    char temp_dir[MAX_PATH];
    DWORD length = GetTempPathA(MAX_PATH, temp_dir);
    if (length == 0 || length >= MAX_PATH) return -1;

    int written = snprintf(path, path_size, "%syunmusic_cover_%lu_%ld.jpg",
        temp_dir, (unsigned long)GetCurrentProcessId(), (long)generation);
    return (written > 0 && (size_t)written < path_size) ? 0 : -1;
}

static DWORD WINAPI fetch_thread_proc(LPVOID param) {
    FetchThreadData* data = (FetchThreadData*)param;
    char song_id[64];
    LONG gen = data->generation;
    HWND hwnd = data->hwnd;
    strncpy(song_id, data->song_id, sizeof(song_id) - 1);
    song_id[sizeof(song_id) - 1] = '\0';
    free(data);

    /* Lyrics */
    LyricsFetchResult* lyrics_result =
        (LyricsFetchResult*)calloc(1, sizeof(LyricsFetchResult));
    if (!lyrics_result) return 0;
    lyrics_result->generation = gen;

    char* lrc_text = NULL;
    if (api_netease_get_lyrics(song_id, &lrc_text) == 0 && lrc_text) {
        if (lrc_parse(&lyrics_result->lyrics, lrc_text) == 0)
            lyrics_result->success = 1;
        free(lrc_text);
    }

    if (ui_is_shutting_down() ||
        !PostMessageW(hwnd, WM_YUN_LYRICS_READY, 0, (LPARAM)lyrics_result)) {
        lrc_free(&lyrics_result->lyrics);
        free(lyrics_result);
    }

    if (ui_is_shutting_down()) return 0;

    /* Cover */
    char cover_url[512] = {0};
    CoverFetchResult* cover_result =
        (CoverFetchResult*)calloc(1, sizeof(CoverFetchResult));
    if (!cover_result) return 0;
    cover_result->generation = gen;

    if (make_cover_temp_path(cover_result->path, sizeof(cover_result->path), gen) == 0 &&
        api_netease_get_cover_url(song_id, cover_url, sizeof(cover_url)) == 0 &&
        api_netease_download_file(cover_url, cover_result->path) == 0) {
        if (!ui_is_shutting_down() &&
            PostMessageW(hwnd, WM_YUN_COVER_READY, 0, (LPARAM)cover_result)) {
            return 0;
        }
    }

    if (cover_result->path[0]) DeleteFileA(cover_result->path);
    free(cover_result);
    return 0;
}

static void fetch_lyrics_and_cover(const char* song_id) {
    if (!song_id || !song_id[0]) return;
    strncpy(g_current_song_id, song_id, sizeof(g_current_song_id) - 1);

    FetchThreadData* data = (FetchThreadData*)malloc(sizeof(FetchThreadData));
    if (!data) return;
    strncpy(data->song_id, song_id, sizeof(data->song_id) - 1);
    data->song_id[sizeof(data->song_id) - 1] = '\0';
    data->generation = InterlockedIncrement(&g_cover_generation);
    data->hwnd = g_hwndMain;
    if (!ui_start_worker(fetch_thread_proc, data))
        free(data);
}

/* ======================== Daily Songs ======================== */

typedef struct {
    HWND hwnd;
    LONG generation;
    NeteaseAuthSession session;
} DailyThreadData;

typedef struct {
    LONG generation;
    int rc;
    char nickname[128];
    NeteaseSearchResult songs;
} DailyThreadResult;

static void daily_thread_result_free(DailyThreadResult* result) {
    if (!result) return;
    api_netease_search_free(&result->songs);
    free(result);
}

static int daily_generation_is_current(LONG generation) {
    return InterlockedCompareExchange(&g_search_generation, 0, 0) == generation;
}

static DWORD WINAPI daily_fetch_thread(LPVOID param) {
    DailyThreadData* data = (DailyThreadData*)param;
    HWND hwnd = data->hwnd;
    LONG generation = data->generation;
    NeteaseAuthSession session = data->session;
    SecureZeroMemory(&data->session, sizeof(data->session));
    free(data);

    DailyThreadResult* result =
        (DailyThreadResult*)calloc(1, sizeof(DailyThreadResult));
    if (!result) {
        InterlockedExchange(&g_daily_in_progress, 0);
        if (!ui_is_shutting_down())
            PostMessageW(hwnd, WM_YUN_DAILY_RESULT, (WPARAM)generation, 0);
        return 0;
    }
    result->generation = generation;
    result->rc = -3;

    if (daily_generation_is_current(generation))
        post_status_text(hwnd, L"正在加载每日推荐...");

    if (!api_netease_auth_supported()) {
        result->rc = -4;
        goto done;
    }
    if (!session.cookie[0]) {
        result->rc = -5;
        goto done;
    }
    int login_status = api_netease_login_status(&session);
    if (login_status == NETEASE_AUTH_INVALID) {
        result->rc = -5;
        goto done;
    }
    if (login_status != 0) goto done;
    strncpy(result->nickname, session.nickname,
        sizeof(result->nickname) - 1);
    int daily_status = api_netease_get_daily_songs(&session, &result->songs);
    if (daily_status == NETEASE_AUTH_INVALID) {
        result->rc = -5;
    } else if (daily_status == 0) {
        result->rc = 0;
    }

done:
    SecureZeroMemory(&session, sizeof(session));
    InterlockedExchange(&g_daily_in_progress, 0);
    if (ui_is_shutting_down() ||
        !PostMessageW(hwnd, WM_YUN_DAILY_RESULT, 0, (LPARAM)result)) {
        daily_thread_result_free(result);
    }
    return 0;
}

/* ======================== Version Check ======================== */

#ifndef YUNMUSIC_VERSION
#define YUNMUSIC_VERSION "1.2.14"
#endif

static int compare_versions(const char* left, const char* right) {
    if (!left || !right) return 0;
    if (*left == 'v' || *left == 'V') left++;
    if (*right == 'v' || *right == 'V') right++;

    for (int part = 0; part < 4; part++) {
        char* left_end = NULL;
        char* right_end = NULL;
        unsigned long left_value = strtoul(left, &left_end, 10);
        unsigned long right_value = strtoul(right, &right_end, 10);
        if (left_value < right_value) return -1;
        if (left_value > right_value) return 1;

        left = (left_end && *left_end == '.') ? left_end + 1 : "";
        right = (right_end && *right_end == '.') ? right_end + 1 : "";
    }
    return 0;
}

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
    WinHttpSetTimeouts(hS, 1000, 1500, 2500, 2500);
    DWORD connect_retries = 1;
    WinHttpSetOption(hS, WINHTTP_OPTION_CONNECT_RETRIES,
        &connect_retries, sizeof(connect_retries));
    HINTERNET hC = WinHttpConnect(hS, whost, (INTERNET_PORT)port, 0);
    if (!hC) { WinHttpCloseHandle(hS); return 0; }
    DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hR = WinHttpOpenRequest(hC, L"GET", wpath,
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hR) { WinHttpCloseHandle(hC); WinHttpCloseHandle(hS); return 0; }

    if (!WinHttpSendRequest(hR, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(hR, NULL)) {
        WinHttpCloseHandle(hR);
        WinHttpCloseHandle(hC);
        WinHttpCloseHandle(hS);
        return 0;
    }

    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    if (!WinHttpQueryHeaders(hR,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_size,
            WINHTTP_NO_HEADER_INDEX) ||
        status_code < 200 || status_code >= 300) {
        WinHttpCloseHandle(hR);
        WinHttpCloseHandle(hC);
        WinHttpCloseHandle(hS);
        return 0;
    }

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

    if (remote_ver && compare_versions(remote_ver, YUNMUSIC_VERSION) > 0) {
        /* New version available */
        char msg[512];
        if (remote_url)
            snprintf(msg, sizeof(msg), "Update available: v%s -> %s", remote_ver, remote_url);
        else
            snprintf(msg, sizeof(msg), "Update available: v%s", remote_ver);

        wchar_t wmsg[512];
        utf8_to_wchar(msg, wmsg, 512);
        post_status_text(hwnd, wmsg);
    }

    cJSON_Delete(root);
    return 0;
}

/* ======================== Bot Command Helper ======================== */

/* Caller must hold g_custom_bot_lock exclusively. */
static int send_bot_command_locked(const char* cmd) {
    return api_bot_send_command_get(cmd);
}

static int send_bot_command(const char* cmd) {
    AcquireSRWLockExclusive(&g_custom_bot_lock);
    int result = send_bot_command_locked(cmd);
    ReleaseSRWLockExclusive(&g_custom_bot_lock);
    return result;
}

static int send_playlist_bot_command(const char* cmd) {
    AcquireSRWLockExclusive(&g_custom_bot_lock);
    if (g_playlist_add_cancel || ui_is_shutting_down()) {
        ReleaseSRWLockExclusive(&g_custom_bot_lock);
        return -1;
    }
    int result = send_bot_command_locked(cmd);
    ReleaseSRWLockExclusive(&g_custom_bot_lock);
    return result;
}

enum BotAction {
    BOT_ACTION_PLAY,
    BOT_ACTION_PAUSE,
    BOT_ACTION_NEXT,
    BOT_ACTION_VOLUME,
    BOT_ACTION_CUSTOM
};

typedef struct {
    HWND hwnd;
    enum BotAction action;
    int value;
    char first_command[512];
    char second_command[512];
} BotActionThreadData;

static DWORD WINAPI bot_action_thread_proc(LPVOID param) {
    BotActionThreadData* data = (BotActionThreadData*)param;
    int result = -1;
    switch (data->action) {
    case BOT_ACTION_PLAY:
        result = api_bot_play();
        break;
    case BOT_ACTION_PAUSE:
        result = api_bot_pause();
        break;
    case BOT_ACTION_NEXT:
        result = api_bot_next();
        break;
    case BOT_ACTION_VOLUME:
        result = api_bot_set_volume(data->value);
        break;
    case BOT_ACTION_CUSTOM:
        result = 0;
        if (data->first_command[0])
            result = send_bot_command(data->first_command);
        if (result == 0 && data->second_command[0])
            result = send_bot_command(data->second_command);
        break;
    }

    HWND hwnd = data->hwnd;
    free(data);
    if (!ui_is_shutting_down())
        PostMessageW(hwnd, WM_YUN_BOT_DONE, (WPARAM)(result == 0), 0);
    return 0;
}

static int queue_bot_action(enum BotAction action, int value,
                            const char* first_command,
                            const char* second_command) {
    BotActionThreadData* data =
        (BotActionThreadData*)calloc(1, sizeof(BotActionThreadData));
    if (!data) return 0;
    data->hwnd = g_hwndMain;
    data->action = action;
    data->value = value;
    if (first_command) {
        strncpy(data->first_command, first_command,
            sizeof(data->first_command) - 1);
    }
    if (second_command) {
        strncpy(data->second_command, second_command,
            sizeof(data->second_command) - 1);
    }
    if (!ui_start_worker(bot_action_thread_proc, data)) {
        free(data);
        return 0;
    }
    return 1;
}

static void play_song_by_id(const char* song_id) {
    if (!song_id || !song_id[0]) return;

    /* Cancel an ongoing playlist import and keep the UI consistent with the bot queue. */
    InterlockedExchange(&g_playlist_add_cancel, 1);
    int clear_queue =
        InterlockedCompareExchange(&g_playlist_add_active, 0, 0) != 0 ||
        InterlockedCompareExchange(&g_playlist_added_count, 0, 0) > 0;
    if (clear_queue) {
        playlist_ui_clear();
        playlist_added_clear();
    }

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "(/yun/play/%s)", song_id);
    if (!queue_bot_action(BOT_ACTION_CUSTOM, 0,
            clear_queue ? "(/clear)" : NULL, cmd)) {
        SetWindowTextW(g_hwndLyrics, L"无法启动播放任务");
    }
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

static void update_volume_display(void) {
    int vol = (int)SendMessage(g_hwndVolSlider, TBM_GETPOS, 0, 0);
    char vol_str[16];
    snprintf(vol_str, sizeof(vol_str), "%d%%", vol);
    wchar_t wvol[16];
    utf8_to_wchar(vol_str, wvol, 16);
    SetWindowTextW(g_hwndBtnVol, wvol);
    g_last_status.volume = (float)vol;
}

static void vol_apply(void) {
    if (!g_hwndVolSlider) return;
    int vol = (int)SendMessage(g_hwndVolSlider, TBM_GETPOS, 0, 0);
    if (!queue_bot_action(BOT_ACTION_VOLUME, vol, NULL, NULL))
        SetWindowTextW(g_hwndLyrics, L"无法启动音量设置任务");
    update_volume_display();
}

static LRESULT CALLBACK VolPopupProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_HSCROLL:
    case WM_VSCROLL:
        if ((HWND)lParam == g_hwndVolSlider) {
            if (LOWORD(wParam) == TB_THUMBTRACK)
                update_volume_display();
            else
                vol_apply();
        }
        return 0;

    case WM_NOTIFY: {
        (void)lParam;
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

#define DL_WIDTH  700
#define DL_HEIGHT 112
#define DL_CONTROL_W 62
#define DL_MENU_LOCK      3101
#define DL_MENU_FONT_UP   3102
#define DL_MENU_FONT_DOWN 3103
#define DL_MENU_CENTER    3104
#define DL_MENU_HIDE      3105

static int g_dl_locked = 0;         /* 0=unlocked(draggable), 1=locked(click-through) */
static int g_dl_last_index = -1;    /* Separate index for desktop lyrics */
static int g_dl_font_size = 24;
static HFONT g_fontDlMain = NULL;
static HFONT g_fontDlSecondary = NULL;
static HFONT g_fontDlControl = NULL;

static void desktop_lyric_update_button(void) {
    if (g_hwndBtnDesktopLyric) {
        SetWindowTextW(g_hwndBtnDesktopLyric,
            g_desktop_lyric_visible ? L"隐藏歌词" : L"桌面歌词");
    }
}

static void desktop_lyric_recreate_fonts(void) {
    if (g_fontDlMain) DeleteObject(g_fontDlMain);
    if (g_fontDlSecondary) DeleteObject(g_fontDlSecondary);
    if (g_fontDlControl) DeleteObject(g_fontDlControl);
    int secondary_size = g_dl_font_size - 8;
    if (secondary_size < 12) secondary_size = 12;
    g_fontDlMain = CreateFontW(-g_dl_font_size, 0, 0, 0, FW_BOLD,
        0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
        L"Microsoft YaHei UI");
    g_fontDlSecondary = CreateFontW(-secondary_size, 0, 0, 0, FW_NORMAL,
        0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
        L"Microsoft YaHei UI");
    g_fontDlControl = CreateFontW(-12, 0, 0, 0, FW_NORMAL,
        0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
        L"Microsoft YaHei UI");
}

static void desktop_lyric_center(HWND hwnd) {
    RECT work_area;
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0)) return;
    int x = work_area.left +
        (work_area.right - work_area.left - DL_WIDTH) / 2;
    int y = work_area.bottom - DL_HEIGHT - 64;
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, DL_WIDTH, DL_HEIGHT,
        SWP_NOACTIVATE);
}

static void desktop_lyric_set_font_size(HWND hwnd, int font_size) {
    if (font_size < 18) font_size = 18;
    if (font_size > 36) font_size = 36;
    if (font_size == g_dl_font_size) return;
    g_dl_font_size = font_size;
    desktop_lyric_recreate_fonts();
    InvalidateRect(hwnd, NULL, FALSE);
}

static void desktop_lyric_show_menu(HWND hwnd, int x, int y) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, DL_MENU_LOCK,
        g_dl_locked ? L"解除锁定" : L"锁定并允许鼠标穿透");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, DL_MENU_FONT_UP, L"增大字号");
    AppendMenuW(menu, MF_STRING, DL_MENU_FONT_DOWN, L"减小字号");
    AppendMenuW(menu, MF_STRING, DL_MENU_CENTER, L"恢复默认位置");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, DL_MENU_HIDE, L"隐藏桌面歌词");

    int command = TrackPopupMenu(menu,
        TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
        x, y, 0, hwnd, NULL);
    DestroyMenu(menu);
    switch (command) {
    case DL_MENU_LOCK:
        g_dl_locked = !g_dl_locked;
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    case DL_MENU_FONT_UP:
        desktop_lyric_set_font_size(hwnd, g_dl_font_size + 2);
        break;
    case DL_MENU_FONT_DOWN:
        desktop_lyric_set_font_size(hwnd, g_dl_font_size - 2);
        break;
    case DL_MENU_CENTER:
        desktop_lyric_center(hwnd);
        break;
    case DL_MENU_HIDE:
        ShowWindow(hwnd, SW_HIDE);
        g_desktop_lyric_visible = 0;
        desktop_lyric_update_button();
        break;
    }
}

static LRESULT CALLBACK DesktopLyricProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HGDIOBJ oldBitmap = SelectObject(memDC, memBmp);

        HBRUSH panelBrush = CreateSolidBrush(RGB(26, 26, 28));
        FillRect(memDC, &rc, panelBrush);
        DeleteObject(panelBrush);

        SetBkMode(memDC, TRANSPARENT);

        RECT rcLock = {0, 0, DL_CONTROL_W, rc.bottom};
        HBRUSH lockBg = CreateSolidBrush(
            g_dl_locked ? RGB(74, 54, 34) : RGB(42, 42, 46));
        FillRect(memDC, &rcLock, lockBg);
        DeleteObject(lockBg);

        HFONT oldFont = (HFONT)SelectObject(memDC, g_fontDlControl);
        SetTextColor(memDC,
            g_dl_locked ? RGB(255, 190, 105) : RGB(185, 185, 190));
        RECT rcLockText = {0, 22, DL_CONTROL_W, rc.bottom - 8};
        DrawTextW(memDC,
            g_dl_locked ? L"已锁定\n点击解锁\n右键菜单" : L"拖动\n点击锁定\n右键菜单",
            -1, &rcLockText, DT_CENTER | DT_WORDBREAK);
        SelectObject(memDC, oldFont);

        /* Lyrics text area */
        int text_left = DL_CONTROL_W + 12;
        int text_right = rc.right - 12;

        if (g_lyrics.count > 0) {
            int pos_ms = (int)(g_last_status.song.position * 1000);
            int idx = lrc_find_line(&g_lyrics, pos_ms);
            if (idx < 0) idx = 0;

            if (idx > 0) {
                oldFont = (HFONT)SelectObject(memDC, g_fontDlSecondary);
                SetTextColor(memDC, RGB(155, 155, 162));
                RECT r = {text_left, 4, text_right, 30};
                wchar_t w[256];
                utf8_to_wchar(g_lyrics.lines[idx - 1].text, w, 256);
                DrawTextW(memDC, w, -1, &r, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                SelectObject(memDC, oldFont);
            }

            {
                oldFont = (HFONT)SelectObject(memDC, g_fontDlMain);
                SetTextColor(memDC, RGB(248, 248, 250));
                RECT r = {text_left, 30, text_right, 78};
                wchar_t w[256];
                utf8_to_wchar(g_lyrics.lines[idx].text, w, 256);
                DrawTextW(memDC, w, -1, &r, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                SelectObject(memDC, oldFont);
            }

            if (idx < g_lyrics.count - 1) {
                oldFont = (HFONT)SelectObject(memDC, g_fontDlSecondary);
                SetTextColor(memDC, RGB(155, 155, 162));
                RECT r = {text_left, 80, text_right, 108};
                wchar_t w[256];
                utf8_to_wchar(g_lyrics.lines[idx + 1].text, w, 256);
                DrawTextW(memDC, w, -1, &r, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                SelectObject(memDC, oldFont);
            }
        } else {
            oldFont = (HFONT)SelectObject(memDC, g_fontDlMain);
            SetTextColor(memDC, RGB(205, 205, 210));
            RECT r = {text_left, 20, text_right, 70};
            wchar_t title[256] = L"等待播放";
            if (g_last_status.song.title[0])
                utf8_to_wchar(g_last_status.song.title, title, 256);
            DrawTextW(memDC, title, -1, &r,
                DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            SelectObject(memDC, oldFont);
            oldFont = (HFONT)SelectObject(memDC, g_fontDlSecondary);
            SetTextColor(memDC, RGB(135, 135, 142));
            RECT hint = {text_left, 68, text_right, 100};
            DrawTextW(memDC,
                g_last_status.song.title[0] ? L"正在获取歌词…" : L"播放音乐后将在这里显示歌词",
                -1, &hint, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            SelectObject(memDC, oldFont);
        }

        BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBitmap);
        DeleteObject(memBmp);
        DeleteDC(memDC);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_NCHITTEST: {
        POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd, &pt);
        /* Lock icon area is always interactive */
        if (pt.x < DL_CONTROL_W) return HTCLIENT;
        /* When locked, rest of window is click-through */
        if (g_dl_locked) return HTTRANSPARENT;
        return HTCAPTION; /* Draggable when unlocked */
    }

    case WM_LBUTTONUP: {
        POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (pt.x < DL_CONTROL_W) {
            /* Toggle lock state */
            g_dl_locked = !g_dl_locked;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_MOUSEWHEEL:
        if (!g_dl_locked) {
            desktop_lyric_set_font_size(hwnd,
                g_dl_font_size + (GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? 1 : -1));
        }
        return 0;

    case WM_CONTEXTMENU: {
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);
        if (x == -1 && y == -1) {
            RECT window_rect;
            GetWindowRect(hwnd, &window_rect);
            x = window_rect.left + DL_CONTROL_W;
            y = window_rect.top + DL_HEIGHT / 2;
        }
        desktop_lyric_show_menu(hwnd, x, y);
        return 0;
    }

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

    YunConfig* cfg = config_get();
    g_dl_locked = cfg->desktop_lyric_locked;
    g_dl_font_size = cfg->desktop_lyric_font_size;
    desktop_lyric_recreate_fonts();

    int x = cfg->desktop_lyric_x;
    int y = cfg->desktop_lyric_y;
    if (x < 0 || y < 0) {
        RECT work_area = {0, 0,
            GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
        x = work_area.left + (work_area.right - work_area.left - DL_WIDTH) / 2;
        y = work_area.bottom - DL_HEIGHT - 64;
    }

    g_hwndDesktopLyric = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE,
        DESKTOP_LYRIC_CLASS, L"",
        WS_POPUP,
        x, y, DL_WIDTH, DL_HEIGHT,
        hParent, NULL, g_hInstance, NULL);

    if (g_hwndDesktopLyric) {
        SetLayeredWindowAttributes(g_hwndDesktopLyric, 0, 232, LWA_ALPHA);
        HRGN region = CreateRoundRectRgn(0, 0, DL_WIDTH + 1, DL_HEIGHT + 1,
            18, 18);
        if (!SetWindowRgn(g_hwndDesktopLyric, region, TRUE))
            DeleteObject(region);
        RECT lyric_rect = {x, y, x + DL_WIDTH, y + DL_HEIGHT};
        if (!MonitorFromRect(&lyric_rect, MONITOR_DEFAULTTONULL))
            desktop_lyric_center(g_hwndDesktopLyric);
    }
}

static void desktop_lyric_toggle(void) {
    /* Do not add another top-level window until desktop lyrics are requested. */
    if (!g_hwndDesktopLyric) desktop_lyric_create(NULL);
    if (!g_hwndDesktopLyric) return;
    if (g_desktop_lyric_visible) {
        ShowWindow(g_hwndDesktopLyric, SW_HIDE);
        g_desktop_lyric_visible = 0;
    } else {
        g_dl_last_index = -1; /* Force repaint */
        SetWindowPos(g_hwndDesktopLyric, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        InvalidateRect(g_hwndDesktopLyric, NULL, FALSE);
        g_desktop_lyric_visible = 1;
    }
    desktop_lyric_update_button();
}

static void desktop_lyric_update(int position_ms) {
    if (!g_hwndDesktopLyric || !g_desktop_lyric_visible) return;
    int idx = -1;
    if (g_lyrics.count > 0) {
        idx = lrc_find_line(&g_lyrics, position_ms);
        if (idx < 0) idx = 0;
    }
    if (idx == g_dl_last_index) return;
    g_dl_last_index = idx;
    InvalidateRect(g_hwndDesktopLyric, NULL, FALSE);
}

static void desktop_lyric_destroy(void) {
    if (g_hwndDesktopLyric) {
        DestroyWindow(g_hwndDesktopLyric);
        g_hwndDesktopLyric = NULL;
    }
    if (g_fontDlMain) { DeleteObject(g_fontDlMain); g_fontDlMain = NULL; }
    if (g_fontDlSecondary) { DeleteObject(g_fontDlSecondary); g_fontDlSecondary = NULL; }
    if (g_fontDlControl) { DeleteObject(g_fontDlControl); g_fontDlControl = NULL; }
    UnregisterClassW(DESKTOP_LYRIC_CLASS, g_hInstance);
}

/* ======================== Search Edit Subclass (Enter key) ======================== */

static WNDPROC g_origSearchEditProc = NULL;
static WNDPROC g_origSearchListProc = NULL;

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
    if (msg == WM_KEYDOWN && wParam == VK_DOWN && g_hwndSearchList) {
        if (SendMessage(g_hwndSearchList, LB_GETCOUNT, 0, 0) > 0) {
            SetFocus(g_hwndSearchList);
            if (SendMessage(g_hwndSearchList, LB_GETCURSEL, 0, 0) == LB_ERR)
                SendMessage(g_hwndSearchList, LB_SETCURSEL, 0, 0);
        }
        return 0;
    }
    if (msg == WM_KEYDOWN && wParam == VK_ESCAPE) {
        SetWindowTextW(hwnd, L"");
        return 0;
    }
    return CallWindowProcW(g_origSearchEditProc, hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK SearchListSubclassProc(HWND hwnd, UINT msg,
        WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
        SendMessageW(GetParent(hwnd), WM_COMMAND,
            MAKEWPARAM(IDC_LIST_SEARCH, LBN_DBLCLK), (LPARAM)hwnd);
        return 0;
    }
    if (msg == WM_KEYDOWN && (wParam == VK_APPS ||
            (wParam == VK_F10 && (GetKeyState(VK_SHIFT) & 0x8000)))) {
        SendMessageW(GetParent(hwnd), WM_CONTEXTMENU,
            (WPARAM)hwnd, MAKELPARAM(-1, -1));
        return 0;
    }
    if (msg == WM_KEYDOWN && wParam == VK_ESCAPE && g_hwndSearchEdit) {
        SetFocus(g_hwndSearchEdit);
        return 0;
    }
    return CallWindowProcW(g_origSearchListProc, hwnd, msg, wParam, lParam);
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

    /* Progress is display-only: YunBot seek currently interrupts playback. */
    int prog_y = MARGIN + COVER_SIZE + 8;
    g_hwndProgress = CreateWindowExW(0, PROGRESS_CLASSW, L"",
        WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
        MARGIN, prog_y, content_w, 14,
        hwnd, (HMENU)IDC_PROGRESS_BAR, g_hInstance, NULL);
    SendMessage(g_hwndProgress, PBM_SETRANGE32, 0, 1000);

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

    g_hwndBtnPlay = CreateWindowExW(0, L"BUTTON", L"播放",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_FLAT,
        btn_x, y, btn_w, btn_h,
        hwnd, (HMENU)IDC_BTN_PLAY_PAUSE, g_hInstance, NULL);
    SendMessage(g_hwndBtnPlay, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);

    g_hwndBtnNext = CreateWindowExW(0, L"BUTTON", L"下一首",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_FLAT,
        btn_x + btn_w + btn_gap, y, btn_w, btn_h,
        hwnd, (HMENU)IDC_BTN_NEXT, g_hInstance, NULL);
    SendMessage(g_hwndBtnNext, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);

    /* Volume button (small, right side) */
    /* Desktop lyric toggle button (left of volume) */
    g_hwndBtnDesktopLyric = CreateWindowExW(0, L"BUTTON", L"桌面歌词",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_FLAT,
        MARGIN + content_w - 84, y, 40, btn_h,
        hwnd, (HMENU)IDC_BTN_DESKTOP_LYRIC, g_hInstance, NULL);
    SendMessage(g_hwndBtnDesktopLyric, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);

    g_hwndBtnVol = CreateWindowExW(0, L"BUTTON", L"75%",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_FLAT,
        MARGIN + content_w - 40, y, 40, btn_h,
        hwnd, (HMENU)IDC_BTN_VOLUME, g_hInstance, NULL);
    SendMessage(g_hwndBtnVol, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);

    y += btn_h + 8;

    /* Lyrics area (multiline, dark themed) */
    g_hwndLyrics = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"等待播放...",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
        ES_CENTER | ES_AUTOVSCROLL,
        MARGIN, y, content_w, 100,
        hwnd, (HMENU)IDC_LYRICS_AREA, g_hInstance, NULL);
    SendMessage(g_hwndLyrics, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
    y += 106;

    /* Playlist label */
    g_hwndPlaylistLabel = CreateWindowExW(0, L"STATIC", L"播放队列",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        MARGIN, y, 80, 14,
        hwnd, NULL, g_hInstance, NULL);
    SendMessage(g_hwndPlaylistLabel, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);
    y += 16;

    /* Playlist listbox */
    g_hwndPlaylist = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
        LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        MARGIN, y, content_w, 120,
        hwnd, (HMENU)IDC_LIST_PLAYLIST, g_hInstance, NULL);
    SendMessage(g_hwndPlaylist, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);
    y += 126;

    /* Search row: [edit] [account] [daily] [mode] [search] */
    g_hwndBtnAccount = CreateWindowExW(0, L"BUTTON", L"账号",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_FLAT,
        MARGIN + content_w - 198, y, 58, 22,
        hwnd, (HMENU)IDC_BTN_ACCOUNT, g_hInstance, NULL);
    SendMessage(g_hwndBtnAccount, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);

    g_hwndBtnDaily = CreateWindowExW(0, L"BUTTON", L"日推",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_FLAT,
        MARGIN + content_w - 136, y, 38, 22,
        hwnd, (HMENU)IDC_BTN_DAILY, g_hInstance, NULL);
    SendMessage(g_hwndBtnDaily, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);

    g_hwndSearchModeBtn = CreateWindowExW(0, L"BUTTON", L"单曲",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_FLAT,
        MARGIN + content_w - 94, y, 42, 22,
        hwnd, (HMENU)IDC_BTN_SEARCH_MODE, g_hInstance, NULL);
    SendMessage(g_hwndSearchModeBtn, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);

    g_hwndSearchBtn = CreateWindowExW(0, L"BUTTON", L"搜索",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON | BS_FLAT,
        MARGIN + content_w - 48, y, 48, 22,
        hwnd, (HMENU)IDC_BTN_SEARCH, g_hInstance, NULL);
    SendMessage(g_hwndSearchBtn, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);

    g_hwndSearchEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        MARGIN, y, content_w - 142, 22,
        hwnd, (HMENU)IDC_EDIT_SEARCH, g_hInstance, NULL);
    SendMessage(g_hwndSearchEdit, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
    /* Subclass to handle Enter key */
    g_origSearchEditProc = (WNDPROC)SetWindowLongPtrW(g_hwndSearchEdit, GWLP_WNDPROC, (LONG_PTR)SearchEditSubclassProc);
    SendMessageW(g_hwndSearchEdit, EM_SETCUEBANNER, TRUE,
        (LPARAM)L"搜索歌曲或歌单，按 Enter");
    SendMessage(g_hwndSearchBtn, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);
    y += 28;

    g_hwndBtnDailyPlayAll = CreateWindowExW(0, L"BUTTON",
        L"播放全部（前15首）",
        WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON | BS_FLAT,
        MARGIN, y, content_w, 26,
        hwnd, (HMENU)IDC_BTN_DAILY_PLAY_ALL, g_hInstance, NULL);
    SendMessage(g_hwndBtnDailyPlayAll, WM_SETFONT,
        (WPARAM)g_fontSmall, TRUE);
    g_hwndBtnResultBack = CreateWindowExW(0, L"BUTTON", L"返回歌单",
        WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON | BS_FLAT,
        MARGIN, y, 76, 26,
        hwnd, (HMENU)IDC_BTN_RESULT_BACK, g_hInstance, NULL);
    SendMessage(g_hwndBtnResultBack, WM_SETFONT,
        (WPARAM)g_fontSmall, TRUE);

    /* Search results */
    g_hwndSearchList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
        LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        MARGIN, y, content_w, 80,
        hwnd, (HMENU)IDC_LIST_SEARCH, g_hInstance, NULL);
    SendMessage(g_hwndSearchList, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);
    g_origSearchListProc = (WNDPROC)SetWindowLongPtrW(
        g_hwndSearchList, GWLP_WNDPROC, (LONG_PTR)SearchListSubclassProc);
    layout_controls(hwnd);
}

static void layout_controls(HWND hwnd) {
    if (!hwnd || !g_hwndCover) return;

    RECT rc;
    GetClientRect(hwnd, &rc);
    int client_w = rc.right - rc.left;
    int client_h = rc.bottom - rc.top;
    int content_w = client_w - MARGIN * 2;
    if (content_w < 320 || client_h < 500) return;

    int progress_y = MARGIN + COVER_SIZE + 8;
    int controls_y = progress_y + 32;
    int controls_h = 30;
    int lyrics_y = controls_y + controls_h + 8;
    int lyrics_h = client_h / 6;
    if (lyrics_h < 88) lyrics_h = 88;
    if (lyrics_h > 112) lyrics_h = 112;
    int queue_label_y = lyrics_y + lyrics_h + 8;

    int search_results_h = client_h / 5;
    if (search_results_h < 76) search_results_h = 76;
    if (search_results_h > 130) search_results_h = 130;
    int search_row_y = client_h - MARGIN - search_results_h - 28;
    int playlist_y = queue_label_y + 18;
    int playlist_h = search_row_y - playlist_y - 8;
    if (playlist_h < 80) playlist_h = 80;

    HDWP layout = BeginDeferWindowPos(21);
#define YUN_DEFER(control, x, y, width, height) \
    do { if (layout && (control)) layout = DeferWindowPos(layout, (control), NULL, \
        (x), (y), (width), (height), SWP_NOZORDER | SWP_NOACTIVATE); } while (0)

    YUN_DEFER(g_hwndCover, MARGIN, MARGIN, COVER_SIZE, COVER_SIZE);
    YUN_DEFER(g_hwndTitle, MARGIN + COVER_SIZE + 10, MARGIN,
        content_w - COVER_SIZE - 10, 22);
    YUN_DEFER(g_hwndArtist, MARGIN + COVER_SIZE + 10, MARGIN + 26,
        content_w - COVER_SIZE - 10, 18);
    YUN_DEFER(g_hwndProgress, MARGIN, progress_y, content_w, 12);
    YUN_DEFER(g_hwndProgressText, MARGIN, progress_y + 13, content_w, 16);

    int play_w = 74, next_w = 68, lyric_w = 84, volume_w = 54, gap = 6;
    int controls_w = play_w + next_w + lyric_w + volume_w + gap * 3;
    int controls_x = MARGIN + (content_w - controls_w) / 2;
    YUN_DEFER(g_hwndBtnPlay, controls_x, controls_y, play_w, controls_h);
    controls_x += play_w + gap;
    YUN_DEFER(g_hwndBtnNext, controls_x, controls_y, next_w, controls_h);
    controls_x += next_w + gap;
    YUN_DEFER(g_hwndBtnDesktopLyric, controls_x, controls_y, lyric_w, controls_h);
    controls_x += lyric_w + gap;
    YUN_DEFER(g_hwndBtnVol, controls_x, controls_y, volume_w, controls_h);

    YUN_DEFER(g_hwndLyrics, MARGIN, lyrics_y, content_w, lyrics_h);
    YUN_DEFER(g_hwndPlaylistLabel, MARGIN, queue_label_y, content_w, 16);
    YUN_DEFER(g_hwndPlaylist, MARGIN, playlist_y, content_w, playlist_h);

    int account_w = 58, daily_w = 44, mode_w = 50, search_w = 54;
    int search_gap = 4;
    int edit_w = content_w - account_w - daily_w - mode_w - search_w -
        search_gap * 4;
    if (edit_w < 120) edit_w = 120;
    int search_x = MARGIN;
    YUN_DEFER(g_hwndSearchEdit, search_x, search_row_y, edit_w, 24);
    search_x += edit_w + search_gap;
    YUN_DEFER(g_hwndBtnAccount, search_x, search_row_y, account_w, 24);
    search_x += account_w + search_gap;
    YUN_DEFER(g_hwndBtnDaily, search_x, search_row_y, daily_w, 24);
    search_x += daily_w + search_gap;
    YUN_DEFER(g_hwndSearchModeBtn, search_x, search_row_y, mode_w, 24);
    search_x += mode_w + search_gap;
    YUN_DEFER(g_hwndSearchBtn, search_x, search_row_y, search_w, 24);
    int result_actions_visible = g_hwndBtnDailyPlayAll &&
        IsWindowVisible(g_hwndBtnDailyPlayAll);
    int back_visible = g_hwndBtnResultBack &&
        IsWindowVisible(g_hwndBtnResultBack);
    int result_y = search_row_y + (result_actions_visible ? 58 : 28);
    int action_x = MARGIN;
    if (back_visible) {
        YUN_DEFER(g_hwndBtnResultBack, action_x, search_row_y + 28, 76, 26);
        action_x += 80;
    }
    YUN_DEFER(g_hwndBtnDailyPlayAll, action_x, search_row_y + 28,
        content_w - (action_x - MARGIN), 26);
    YUN_DEFER(g_hwndSearchList, MARGIN, result_y,
        content_w, client_h - MARGIN - result_y);

    if (layout) EndDeferWindowPos(layout);
#undef YUN_DEFER
}

static void update_result_actions_visibility(void) {
    if (!g_hwndBtnDailyPlayAll) return;
    int visible = (g_result_source == RESULT_SOURCE_DAILY_SONGS ||
        g_result_source == RESULT_SOURCE_PLAYLIST_TRACKS) &&
        g_search_result.count > 0;
    ShowWindow(g_hwndBtnDailyPlayAll, visible ? SW_SHOW : SW_HIDE);
    int show_back = visible &&
        g_result_source == RESULT_SOURCE_PLAYLIST_TRACKS;
    ShowWindow(g_hwndBtnResultBack, show_back ? SW_SHOW : SW_HIDE);
    if (visible)
        SetWindowTextW(g_hwndBtnDailyPlayAll,
            g_result_source == RESULT_SOURCE_DAILY_SONGS
                ? L"播放全部（前15首）" : L"播放前15首");
    if (visible)
        EnableWindow(g_hwndBtnDailyPlayAll,
            InterlockedCompareExchange(&g_playlist_add_active, 0, 0) == 0);
    layout_controls(g_hwndMain);
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

typedef struct {
    HWND hwnd;
    LONG generation;
    int mode;
    char keyword[256];
} SearchThreadData;

typedef struct {
    LONG generation;
    int mode;
    int rc;
    NeteaseSearchResult songs;
    NeteasePlaylistResult playlists;
} SearchThreadResult;

static void search_thread_result_free(SearchThreadResult* result) {
    if (!result) return;
    api_netease_search_free(&result->songs);
    api_netease_search_playlists_free(&result->playlists);
    free(result);
}

static void set_search_busy(int busy) {
    if (g_hwndSearchBtn) EnableWindow(g_hwndSearchBtn, !busy);
    if (g_hwndSearchModeBtn) EnableWindow(g_hwndSearchModeBtn, !busy);
    if (g_hwndSearchEdit) EnableWindow(g_hwndSearchEdit, !busy);
}

static DWORD WINAPI search_thread_proc(LPVOID param) {
    SearchThreadData* data = (SearchThreadData*)param;
    SearchThreadResult* result =
        (SearchThreadResult*)calloc(1, sizeof(SearchThreadResult));
    if (!result) {
        if (!ui_is_shutting_down())
            PostMessageW(data->hwnd, WM_YUN_SEARCH_RESULT,
                (WPARAM)data->generation, 0);
        free(data);
        return 0;
    }
    result->generation = data->generation;
    result->mode = data->mode;
    if (data->mode == 0)
        result->rc = api_netease_search(data->keyword, 20, &result->songs);
    else
        result->rc =
            api_netease_search_playlists(data->keyword, 20, &result->playlists);

    HWND hwnd = data->hwnd;
    free(data);
    if (ui_is_shutting_down() ||
        !PostMessageW(hwnd, WM_YUN_SEARCH_RESULT, 0, (LPARAM)result)) {
        search_thread_result_free(result);
    }
    return 0;
}

static void do_search(const char* keyword) {
    if (!keyword || !keyword[0]) return;

    SearchThreadData* data =
        (SearchThreadData*)calloc(1, sizeof(SearchThreadData));
    if (!data) {
        SetWindowTextW(g_hwndLyrics, L"无法启动搜索任务");
        return;
    }

    data->hwnd = g_hwndMain;
    data->generation = InterlockedIncrement(&g_search_generation);
    data->mode = g_search_mode;
    strncpy(data->keyword, keyword, sizeof(data->keyword) - 1);

    SendMessage(g_hwndSearchList, LB_RESETCONTENT, 0, 0);
    api_netease_search_free(&g_search_result);
    api_netease_search_playlists_free(&g_playlist_search_result);
    g_result_source = RESULT_SOURCE_NONE;
    update_result_actions_visibility();
    SendMessageW(g_hwndSearchList, LB_ADDSTRING, 0, (LPARAM)L"正在搜索...");
    set_search_busy(1);

    if (!ui_start_worker(search_thread_proc, data)) {
        free(data);
        set_search_busy(0);
        SendMessage(g_hwndSearchList, LB_RESETCONTENT, 0, 0);
        SetWindowTextW(g_hwndLyrics, L"无法启动搜索任务");
    }
}

static void show_search_context_menu(HWND hwnd, int x, int y) {
    int sel = (int)SendMessage(g_hwndSearchList, LB_GETCURSEL, 0, 0);
    if (g_result_source == RESULT_SOURCE_PLAYLIST_SEARCH) {
        if (sel < 0 || sel >= g_playlist_search_result.count) return;
    } else {
        if (sel < 0 || sel >= g_search_result.count) return;
    }

    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;
    if (g_result_source == RESULT_SOURCE_PLAYLIST_SEARCH) {
        AppendMenuW(hMenu, MF_STRING, CM_PLAY_NOW, L"查看歌单内容");
    } else {
        AppendMenuW(hMenu, MF_STRING, CM_PLAY_NOW, L"立即播放");
        AppendMenuW(hMenu, MF_STRING, CM_PLAY_NEXT, L"下一首播放");
    }
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, x, y, 0, hwnd, NULL);
    DestroyMenu(hMenu);
}

/* ======================== Playlist ======================== */

static const char* playlist_ui_item_id(int index) {
    if (!g_hwndPlaylist || index < 0) return NULL;
    LRESULT data = SendMessage(g_hwndPlaylist, LB_GETITEMDATA, index, 0);
    return data == LB_ERR ? NULL : (const char*)data;
}

static int playlist_ui_add(const char* song_id, const wchar_t* text) {
    if (!g_hwndPlaylist || !song_id || !song_id[0] || !text) return -1;
    int existing = playlist_ui_find_id(song_id);
    if (existing >= 0) return existing;
    char* stored_id = (char*)calloc(64, 1);
    if (!stored_id) return -1;
    strncpy(stored_id, song_id, 63);
    LRESULT index = SendMessageW(g_hwndPlaylist, LB_ADDSTRING, 0,
        (LPARAM)text);
    if (index == LB_ERR || index == LB_ERRSPACE ||
        SendMessage(g_hwndPlaylist, LB_SETITEMDATA, index,
            (LPARAM)stored_id) == LB_ERR) {
        if (index >= 0) SendMessage(g_hwndPlaylist, LB_DELETESTRING, index, 0);
        free(stored_id);
        return -1;
    }
    return (int)index;
}

static void playlist_ui_add_placeholder(const wchar_t* text) {
    if (g_hwndPlaylist && text)
        SendMessageW(g_hwndPlaylist, LB_ADDSTRING, 0, (LPARAM)text);
}

static void playlist_ui_delete(int index) {
    char* stored_id = (char*)playlist_ui_item_id(index);
    if (stored_id) free(stored_id);
    SendMessage(g_hwndPlaylist, LB_DELETESTRING, index, 0);
}

static void playlist_ui_clear(void) {
    if (!g_hwndPlaylist) return;
    int count = (int)SendMessage(g_hwndPlaylist, LB_GETCOUNT, 0, 0);
    for (int i = 0; i < count; i++) {
        char* stored_id = (char*)playlist_ui_item_id(i);
        if (stored_id) free(stored_id);
    }
    SendMessage(g_hwndPlaylist, LB_RESETCONTENT, 0, 0);
}

static int playlist_ui_find_id(const char* song_id) {
    if (!song_id || !song_id[0]) return -1;
    int count = (int)SendMessage(g_hwndPlaylist, LB_GETCOUNT, 0, 0);
    for (int i = 0; i < count; i++) {
        const char* item_id = playlist_ui_item_id(i);
        if (item_id && strcmp(item_id, song_id) == 0) return i;
    }
    return -1;
}

static int start_first_15_enqueue(HWND hwnd,
                                  const NeteaseSearchResult* recommendations,
                                  int daily_source) {
    int count = recommendations ? daily_queue_limit(recommendations->count) : 0;
    if (!recommendations || !recommendations->songs || count <= 0) return 0;
    if (InterlockedCompareExchange(&g_playlist_add_active, 1, 0) != 0) {
        SetWindowTextW(g_hwndLyrics, L"已有批量添加任务正在执行，请稍候");
        return 0;
    }

    PlaylistAddThreadData* data =
        (PlaylistAddThreadData*)calloc(1, sizeof(PlaylistAddThreadData));
    if (!data) goto fail;
    data->song_ids = (char(*)[64])calloc((size_t)count, sizeof(char[64]));
    data->song_names =
        (char(*)[256])calloc((size_t)count, sizeof(char[256]));
    if (!data->song_ids || !data->song_names) {
        playlist_thread_data_free(data);
        goto fail_without_data;
    }

    data->count = count;
    data->daily_source = daily_source;
    data->play_first_if_idle = g_last_status.song.title[0] == '\0';
    data->hwnd = hwnd;
    for (int i = 0; i < count; i++) {
        strncpy(data->song_ids[i], recommendations->songs[i].id,
            sizeof(data->song_ids[i]) - 1);
        snprintf(data->song_names[i], sizeof(data->song_names[i]), "%s - %s",
            recommendations->songs[i].name,
            recommendations->songs[i].artist);
    }

    InterlockedExchange(&g_playlist_add_cancel, 0);
    EnableWindow(g_hwndBtnDailyPlayAll, FALSE);
    wchar_t start_status[96];
    swprintf(start_status, 96, daily_source
        ? L"正在添加每日推荐：0/%d" : L"正在添加歌单前15首：0/%d", count);
    SetWindowTextW(g_hwndLyrics, start_status);
    if (!ui_start_worker(playlist_add_thread_proc, data)) {
        EnableWindow(g_hwndBtnDailyPlayAll, TRUE);
        playlist_thread_data_free(data);
        goto fail_without_data;
    }
    return 1;

fail:
    free(data);
fail_without_data:
    InterlockedExchange(&g_playlist_add_active, 0);
    SetWindowTextW(g_hwndLyrics, L"无法启动每日推荐批量添加任务");
    return 0;
}

typedef struct {
    HWND hwnd;
    LONG generation;
    char playlist_id[64];
    char playlist_name[256];
} PlaylistLoadThreadData;

typedef struct {
    LONG generation;
    int rc;
    char playlist_name[256];
    NeteaseSearchResult tracks;
} PlaylistLoadResult;

static void playlist_load_result_free(PlaylistLoadResult* result) {
    if (!result) return;
    api_netease_search_free(&result->tracks);
    free(result);
}

static DWORD WINAPI playlist_load_thread_proc(LPVOID param) {
    PlaylistLoadThreadData* data = (PlaylistLoadThreadData*)param;
    PlaylistLoadResult* result =
        (PlaylistLoadResult*)calloc(1, sizeof(PlaylistLoadResult));
    if (!result) {
        if (ui_is_shutting_down() ||
            !PostMessageW(data->hwnd, WM_YUN_PLAYLIST_LOAD, (WPARAM)-1, 0)) {
            InterlockedExchange(&g_playlist_load_active, 0);
        }
        free(data);
        return 0;
    }

    strncpy(result->playlist_name, data->playlist_name,
        sizeof(result->playlist_name) - 1);
    result->generation = data->generation;
    result->rc =
        api_netease_get_playlist_tracks(data->playlist_id, &result->tracks);
    HWND hwnd = data->hwnd;
    free(data);

    if (ui_is_shutting_down() ||
        !PostMessageW(hwnd, WM_YUN_PLAYLIST_LOAD, 0, (LPARAM)result)) {
        playlist_load_result_free(result);
        InterlockedExchange(&g_playlist_load_active, 0);
    }
    return 0;
}

static void request_playlist_add(HWND hwnd, const NeteasePlaylist* playlist) {
    if (!playlist || !playlist->id[0]) return;
    if (InterlockedCompareExchange(&g_playlist_load_active, 1, 0) != 0) {
        SetWindowTextW(g_hwndLyrics, L"正在加载其他歌单，请稍候");
        return;
    }

    PlaylistLoadThreadData* data =
        (PlaylistLoadThreadData*)calloc(1, sizeof(PlaylistLoadThreadData));
    if (!data) goto fail;
    data->hwnd = hwnd;
    data->generation = InterlockedIncrement(&g_search_generation);
    strncpy(data->playlist_id, playlist->id, sizeof(data->playlist_id) - 1);
    strncpy(data->playlist_name, playlist->name,
        sizeof(data->playlist_name) - 1);

    SetWindowTextW(g_hwndLyrics, L"正在加载歌单详情...");
    if (!ui_start_worker(playlist_load_thread_proc, data)) {
        free(data);
        goto fail;
    }
    return;

fail:
    InterlockedExchange(&g_playlist_load_active, 0);
    SetWindowTextW(g_hwndLyrics, L"无法启动歌单加载任务");
}

static void playlist_highlight_current(const char* song_id,
                                       const char* song_title) {
    (void)song_title;
    if (!song_id || !song_id[0] || !g_hwndPlaylist) return;
    int index = playlist_ui_find_id(song_id);
    if (index >= 0) playlist_ui_delete(index);
}

static void playlist_sync_from_status(const BotStatus* status) {
    if (!status || !status->queue.synced || !g_hwndPlaylist) return;
    playlist_ui_clear();
    for (int i = 0; i < status->queue.count; i++) {
        const BotQueuePreviewItem* item = &status->queue.items[i];
        wchar_t title[512];
        char display[384];
        snprintf(display, sizeof(display), "%d. %s", i + 1, item->title);
        utf8_to_wchar(display, title, 512);
        if (item->song_id[0]) playlist_ui_add(item->song_id, title);
        else playlist_ui_add_placeholder(title);
    }
    if (status->queue.total > status->queue.count) {
        wchar_t remainder[128];
        swprintf(remainder, 128, L"… 其余 %d 首",
            status->queue.total - status->queue.count);
        playlist_ui_add_placeholder(remainder);
    }
    wchar_t label[128];
    swprintf(label, 128, L"播放队列（Bot：%d 首）", status->queue.total);
    SetWindowTextW(g_hwndPlaylistLabel, label);
}

/* ======================== UI Update ======================== */

static void update_ui_from_status(const BotStatus* status) {
    if (!status) return;
    int has_song = status->song.title[0] != '\0';

    /* Play/pause button */
    SetWindowTextW(g_hwndBtnPlay,
        !has_song || status->song.paused ? L"播放" : L"暂停");

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

        int progress = (int)(status->song.position / status->song.length * 1000);
        if (progress < 0) progress = 0;
        if (progress > 1000) progress = 1000;
        SendMessage(g_hwndProgress, PBM_SETPOS, progress, 0);
    } else {
        SendMessage(g_hwndProgress, PBM_SETPOS, 0, 0);
        SetWindowTextA(g_hwndProgressText, "0:00 / 0:00");
    }

    /* Volume button */
    char vol_str[16];
    int volume = (int)status->volume;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    snprintf(vol_str, sizeof(vol_str), "%d%%", volume);
    wchar_t wvol[16];
    utf8_to_wchar(vol_str, wvol, 16);
    SetWindowTextW(g_hwndBtnVol, wvol);

    /* Song change → fetch lyrics + cover */
    if (status->song.song_id[0] &&
        strcmp(status->song.song_id, g_current_song_id) != 0) {
        lrc_free(&g_lyrics);
        g_last_lyric_index = -1;
        g_dl_last_index = -1;
        if (g_hwndDesktopLyric)
            InvalidateRect(g_hwndDesktopLyric, NULL, FALSE);
        SetWindowTextW(g_hwndLyrics, L"加载歌词中...");
        clear_cover_image();

        /* Remove previous song from playlist */
        if (g_current_song_id[0]) {
            int old_index = playlist_ui_find_id(g_current_song_id);
            if (old_index >= 0) playlist_ui_delete(old_index);
        }

        /* Highlight in playlist (or add if not present) */
        if (strlen(status->song.title) > 0) {
            playlist_highlight_current(status->song.song_id,
                status->song.title);
        }

        fetch_lyrics_and_cover(status->song.song_id);
    }

    /* Song ended (no next song) → clear lyrics, playlist, progress */
    if (!status->song.song_id[0] && g_current_song_id[0]) {
        lrc_free(&g_lyrics);
        g_last_lyric_index = -1;
        g_dl_last_index = -1;
        if (g_hwndDesktopLyric)
            InvalidateRect(g_hwndDesktopLyric, NULL, FALSE);
        int ended_index = playlist_ui_find_id(g_current_song_id);
        if (ended_index >= 0) playlist_ui_delete(ended_index);
        g_current_song_id[0] = '\0';
        InterlockedIncrement(&g_cover_generation);
        clear_cover_image();
        SetWindowTextW(g_hwndLyrics, L"等待播放...");
        /* Reset progress bar */
        SendMessage(g_hwndProgress, PBM_SETPOS, 0, 0);
        SetWindowTextA(g_hwndProgressText, "0:00 / 0:00");
        SetWindowTextW(g_hwndBtnPlay, L"播放");
    }

    /* Lyrics sync */
    if (g_lyrics.count > 0) {
        int pos_ms = (int)(status->song.position * 1000);
        update_lyrics_display(pos_ms);
    }

    playlist_sync_from_status(status);

    g_last_status = *status;
}

/* ======================== Background Poll ======================== */

static DWORD WINAPI poll_thread_proc(LPVOID param) {
    HWND hwnd = (HWND)param;
    BotStatus* status = (BotStatus*)malloc(sizeof(BotStatus));
    int success = status && api_bot_poll_status(status) == 0;
    if (!success) {
        free(status);
        status = NULL;
    }

    if (ui_is_shutting_down() ||
        !PostMessageW(hwnd, WM_YUN_POLL_RESULT, (WPARAM)success, (LPARAM)status)) {
        free(status);
        InterlockedExchange(&g_poll_in_progress, 0);
    }
    return 0;
}

/* ======================== WndProc ======================== */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        create_controls(hwnd);
        if (config_get()->music_u[0])
            SetWindowTextW(g_hwndLyrics,
                L"旧登录信息加密迁移失败，已保留原值；请检查插件目录写入权限");
        SetTimer(hwnd, TIMER_POLL_STATUS, config_get()->poll_interval_ms, NULL);
        SetTimer(hwnd, TIMER_PROGRESS_SMOOTH, 1000, NULL);
        SetTimer(hwnd, TIMER_LYRIC_SYNC, 100, NULL);
        PostMessageW(hwnd, WM_TIMER, TIMER_POLL_STATUS, 0);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* minmax = (MINMAXINFO*)lParam;
        minmax->ptMinTrackSize.x = WIN_MIN_WIDTH;
        minmax->ptMinTrackSize.y = WIN_MIN_HEIGHT;
        return 0;
    }

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            layout_controls(hwnd);
            hide_vol_popup();
        }
        return 0;

    case WM_YUN_LYRICS_READY: {
        LyricsFetchResult* result = (LyricsFetchResult*)lParam;
        LONG current_generation =
            InterlockedCompareExchange(&g_cover_generation, 0, 0);
        if (result && result->generation == current_generation) {
            lrc_free(&g_lyrics);
            g_last_lyric_index = -1;
            g_dl_last_index = -1;
            if (result->success) {
                g_lyrics = result->lyrics;
                memset(&result->lyrics, 0, sizeof(result->lyrics));
                if (g_lyrics.count > 0) {
                    update_lyrics_display((int)(g_last_status.song.position * 1000));
                    desktop_lyric_update((int)(g_last_status.song.position * 1000));
                } else {
                    SetWindowTextW(g_hwndLyrics, L"暂无歌词");
                }
            } else {
                SetWindowTextW(g_hwndLyrics, L"歌词加载失败");
            }
        }
        if (result) {
            lrc_free(&result->lyrics);
            free(result);
        }
        return 0;
    }

    case WM_YUN_COVER_READY: {
        CoverFetchResult* result = (CoverFetchResult*)lParam;
        LONG current_generation =
            InterlockedCompareExchange(&g_cover_generation, 0, 0);
        if (result) {
            int loaded = result->generation == current_generation &&
                load_cover_from_file(result->path) == 0;
            if (!loaded) DeleteFileA(result->path);
            free(result);
        }
        return 0;
    }

    case WM_YUN_PLAYLIST_ITEM: {
        PlaylistItemMessage* item = (PlaylistItemMessage*)lParam;
        if (item) playlist_ui_add(item->song_id, item->text);
        free(item);
        return 0;
    }

    case WM_YUN_STATUS_TEXT: {
        wchar_t* text = (wchar_t*)lParam;
        if (text && g_hwndLyrics)
            SetWindowTextW(g_hwndLyrics, text);
        free(text);
        return 0;
    }

    case WM_YUN_SEARCH_RESULT: {
        SearchThreadResult* result = (SearchThreadResult*)lParam;
        LONG generation = result ? result->generation : (LONG)wParam;
        LONG current_generation =
            InterlockedCompareExchange(&g_search_generation, 0, 0);
        if (generation != current_generation) {
            search_thread_result_free(result);
            return 0;
        }

        set_search_busy(0);
        SendMessage(g_hwndSearchList, LB_RESETCONTENT, 0, 0);
        if (!result || result->rc != 0) {
            SendMessageW(g_hwndSearchList, LB_ADDSTRING, 0,
                (LPARAM)L"搜索失败，请检查网易云 API");
            SetWindowTextW(g_hwndLyrics, L"搜索失败，请稍后重试");
            search_thread_result_free(result);
            return 0;
        }

        api_netease_search_free(&g_search_result);
        api_netease_search_playlists_free(&g_playlist_search_result);
        if (result->mode == 0) {
            g_result_source = RESULT_SOURCE_SONG_SEARCH;
            g_search_result = result->songs;
            memset(&result->songs, 0, sizeof(result->songs));
            for (int i = 0; i < g_search_result.count; i++) {
                NeteaseSong* song = &g_search_result.songs[i];
                char display[512];
                int duration_seconds = song->duration_ms / 1000;
                snprintf(display, sizeof(display), "%s - %s (%d:%02d)",
                    song->name, song->artist,
                    duration_seconds / 60, duration_seconds % 60);
                wchar_t wide_display[512];
                utf8_to_wchar(display, wide_display, 512);
                SendMessageW(g_hwndSearchList, LB_ADDSTRING, 0,
                    (LPARAM)wide_display);
            }
            if (g_search_result.count == 0) {
                SendMessageW(g_hwndSearchList, LB_ADDSTRING, 0,
                    (LPARAM)L"未找到结果");
            }
        } else {
            g_result_source = RESULT_SOURCE_PLAYLIST_SEARCH;
            g_playlist_search_result = result->playlists;
            memset(&result->playlists, 0, sizeof(result->playlists));
            for (int i = 0; i < g_playlist_search_result.count; i++) {
                NeteasePlaylist* playlist =
                    &g_playlist_search_result.playlists[i];
                char display[512];
                if (playlist->play_count >= 10000) {
                    snprintf(display, sizeof(display),
                        "%s (%d首, %.1f万播放)", playlist->name,
                        playlist->track_count, playlist->play_count / 10000.0);
                } else {
                    snprintf(display, sizeof(display),
                        "%s (%d首, %d播放)", playlist->name,
                        playlist->track_count, playlist->play_count);
                }
                wchar_t wide_display[512];
                utf8_to_wchar(display, wide_display, 512);
                SendMessageW(g_hwndSearchList, LB_ADDSTRING, 0,
                    (LPARAM)wide_display);
            }
            if (g_playlist_search_result.count == 0) {
                SendMessageW(g_hwndSearchList, LB_ADDSTRING, 0,
                    (LPARAM)L"未找到歌单");
            }
        }

        update_result_actions_visibility();
        search_thread_result_free(result);
        return 0;
    }

    case WM_YUN_BOT_DONE:
        if (!wParam)
            SetWindowTextW(g_hwndLyrics,
                L"Bot 网易云/VIP 登录可能失效，请联系管理员执行 !yun login");
        SetTimer(hwnd, TIMER_POLL_STATUS, 250, NULL);
        return 0;

    case WM_YUN_PLAYLIST_DONE:
        if (g_hwndBtnDailyPlayAll &&
            (g_result_source == RESULT_SOURCE_DAILY_SONGS ||
             g_result_source == RESULT_SOURCE_PLAYLIST_TRACKS))
            EnableWindow(g_hwndBtnDailyPlayAll, TRUE);
        return 0;

    case WM_YUN_DAILY_RESULT: {
        DailyThreadResult* result = (DailyThreadResult*)lParam;
        if (g_hwndBtnDaily) EnableWindow(g_hwndBtnDaily, TRUE);
        LONG current_generation =
            InterlockedCompareExchange(&g_search_generation, 0, 0);
        LONG generation = result ? result->generation : (LONG)wParam;
        if (generation != current_generation) {
            daily_thread_result_free(result);
            return 0;
        }

        if (!result) {
            SetWindowTextW(g_hwndLyrics, L"获取每日推荐失败");
        } else if (result->rc == -4) {
            SetWindowTextW(g_hwndLyrics,
                L"网易云 API 地址无效，请检查配置");
            account_window_show();
        } else if (result->rc == -5) {
            auth_store_delete();
            SecureZeroMemory(&g_auth_session, sizeof(g_auth_session));
            SetWindowTextW(g_hwndLyrics, L"个人账号登录已失效，请重新扫码");
            account_window_show_relogin_required();
        } else if (result->rc != 0) {
            SetWindowTextW(g_hwndLyrics, L"获取每日推荐失败，请稍后重试");
        } else if (result->songs.count > 0) {
            api_netease_search_free(&g_search_result);
            api_netease_search_playlists_free(&g_playlist_search_result);
            g_search_result = result->songs;
            memset(&result->songs, 0, sizeof(result->songs));
            g_result_source = RESULT_SOURCE_DAILY_SONGS;
            g_search_mode = 0;
            SetWindowTextW(g_hwndSearchModeBtn, L"单曲");
            SendMessage(g_hwndSearchList, LB_RESETCONTENT, 0, 0);
            for (int i = 0; i < g_search_result.count; i++) {
                NeteaseSong* s = &g_search_result.songs[i];
                char display[512];
                int dur_sec = s->duration_ms / 1000;
                snprintf(display, sizeof(display), "%s - %s (%d:%02d)",
                    s->name, s->artist, dur_sec / 60, dur_sec % 60);
                wchar_t wdisplay[512];
                utf8_to_wchar(display, wdisplay, 512);
                SendMessageW(g_hwndSearchList, LB_ADDSTRING, 0,
                    (LPARAM)wdisplay);
            }
            strncpy(g_auth_session.nickname, result->nickname,
                sizeof(g_auth_session.nickname) - 1);
            g_auth_session.logged_in = 1;
            wchar_t nickname[128] = L"个人账号";
            if (result->nickname[0])
                utf8_to_wchar(result->nickname, nickname, 128);
            SYSTEMTIME now;
            GetLocalTime(&now);
            wchar_t status[256];
            swprintf(status, 256, L"%s 的每日推荐 · 更新于 %02d:%02d",
                nickname, now.wHour, now.wMinute);
            SetWindowTextW(g_hwndLyrics, status);
            update_result_actions_visibility();
        } else {
            SetWindowTextW(g_hwndLyrics, L"今日暂无推荐歌曲");
        }
        daily_thread_result_free(result);
        return 0;
    }

    case WM_YUN_PLAYLIST_LOAD: {
        PlaylistLoadResult* result = (PlaylistLoadResult*)lParam;
        InterlockedExchange(&g_playlist_load_active, 0);
        LONG current_generation =
            InterlockedCompareExchange(&g_search_generation, 0, 0);
        if (result && result->generation != current_generation) {
            playlist_load_result_free(result);
            return 0;
        }
        if (!result || result->rc != 0 || result->tracks.count <= 0) {
            SetWindowTextW(g_hwndLyrics, L"歌单加载失败或歌单为空");
        } else {
            api_netease_search_free(&g_search_result);
            g_search_result = result->tracks;
            memset(&result->tracks, 0, sizeof(result->tracks));
            g_result_source = RESULT_SOURCE_PLAYLIST_TRACKS;
            strncpy(g_playlist_detail_name, result->playlist_name,
                sizeof(g_playlist_detail_name) - 1);
            g_playlist_detail_name[sizeof(g_playlist_detail_name) - 1] = '\0';
            SendMessage(g_hwndSearchList, LB_RESETCONTENT, 0, 0);
            for (int i = 0; i < g_search_result.count; i++) {
                NeteaseSong* song = &g_search_result.songs[i];
                char display[512];
                int seconds = song->duration_ms / 1000;
                snprintf(display, sizeof(display), "%02d. %s - %s (%d:%02d)",
                    i + 1, song->name, song->artist,
                    seconds / 60, seconds % 60);
                wchar_t wide_display[512];
                utf8_to_wchar(display, wide_display, 512);
                SendMessageW(g_hwndSearchList, LB_ADDSTRING, 0,
                    (LPARAM)wide_display);
            }
            wchar_t playlist_name[256];
            utf8_to_wchar(g_playlist_detail_name, playlist_name, 256);
            wchar_t status[320];
            swprintf(status, 320, L"歌单：%s（%d 首）",
                playlist_name, g_search_result.count);
            SetWindowTextW(g_hwndLyrics, status);
            update_result_actions_visibility();
        }
        playlist_load_result_free(result);
        return 0;
    }

    case WM_YUN_POLL_RESULT: {
        /* Poll result from background thread */
        BotStatus* pStatus = (BotStatus*)lParam;
        InterlockedExchange(&g_poll_in_progress, 0);
        if (wParam && pStatus) {
            update_ui_from_status(pStatus);
        } else {
            g_last_status.song.paused = 1;
            SetWindowTextW(g_hwndArtist, L"Bot 连接失败，正在重试");
            SetWindowTextW(g_hwndBtnPlay, L"播放");
        }
        free(pStatus);
        /* Restore normal polling interval */
        SetTimer(hwnd, TIMER_POLL_STATUS, config_get()->poll_interval_ms, NULL);
        return 0;
    }

    case WM_TIMER:
        if (LOWORD(wParam) == TIMER_POLL_STATUS) {
            /* Keep at most one status request in flight. */
            if (InterlockedCompareExchange(&g_poll_in_progress, 1, 0) == 0) {
                if (!ui_start_worker(poll_thread_proc, (LPVOID)hwnd))
                    InterlockedExchange(&g_poll_in_progress, 0);
            }
        } else if (LOWORD(wParam) == TIMER_PROGRESS_SMOOTH) {
            if (!g_last_status.song.paused && g_last_status.song.length > 0) {
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
            if (!queue_bot_action(
                    !g_last_status.song.title[0] || g_last_status.song.paused
                        ? BOT_ACTION_PLAY : BOT_ACTION_PAUSE,
                    0, NULL, NULL)) {
                SetWindowTextW(g_hwndLyrics, L"无法启动播放控制任务");
            }
            return 0;

        case IDC_BTN_NEXT:
            if (!queue_bot_action(BOT_ACTION_NEXT, 0, NULL, NULL))
                SetWindowTextW(g_hwndLyrics, L"无法启动切歌任务");
            return 0;

        case IDC_BTN_VOLUME:
            if (g_vol_popup_visible) hide_vol_popup();
            else show_vol_popup();
            return 0;

        case IDC_BTN_DESKTOP_LYRIC:
            desktop_lyric_toggle();
            return 0;

        case IDC_BTN_ACCOUNT:
            account_window_show();
            return 0;

        case IDC_BTN_DAILY: {
            if (!api_netease_auth_supported()) {
                SetWindowTextW(g_hwndLyrics,
                    L"网易云 API 地址无效，请检查配置");
                account_window_show();
                return 0;
            }
            if (!g_auth_session.cookie[0]) {
                SetWindowTextW(g_hwndLyrics, L"请先登录网易云个人账号");
                account_window_show();
                return 0;
            }
            if (InterlockedCompareExchange(&g_daily_in_progress, 1, 0) == 0) {
                DailyThreadData* data =
                    (DailyThreadData*)calloc(1, sizeof(DailyThreadData));
                if (!data) {
                    InterlockedExchange(&g_daily_in_progress, 0);
                    SetWindowTextW(g_hwndLyrics, L"无法启动每日推荐任务");
                    return 0;
                }
                data->hwnd = hwnd;
                data->generation = InterlockedIncrement(&g_search_generation);
                data->session = g_auth_session;
                set_search_busy(0);
                g_result_source = RESULT_SOURCE_NONE;
                update_result_actions_visibility();
                EnableWindow(g_hwndBtnDaily, FALSE);
                if (!ui_start_worker(daily_fetch_thread, data)) {
                    SecureZeroMemory(&data->session, sizeof(data->session));
                    free(data);
                    InterlockedExchange(&g_daily_in_progress, 0);
                    EnableWindow(g_hwndBtnDaily, TRUE);
                    SetWindowTextW(g_hwndLyrics, L"无法启动每日推荐任务");
                }
            }
            return 0;
        }

        case IDC_BTN_DAILY_PLAY_ALL:
            if (g_result_source == RESULT_SOURCE_DAILY_SONGS ||
                g_result_source == RESULT_SOURCE_PLAYLIST_TRACKS)
                start_first_15_enqueue(hwnd, &g_search_result,
                    g_result_source == RESULT_SOURCE_DAILY_SONGS);
            return 0;

        case IDC_BTN_RESULT_BACK:
            if (g_result_source == RESULT_SOURCE_PLAYLIST_TRACKS) {
                api_netease_search_free(&g_search_result);
                g_result_source = RESULT_SOURCE_PLAYLIST_SEARCH;
                g_search_mode = 1;
                SetWindowTextW(g_hwndSearchModeBtn, L"歌单");
                SendMessage(g_hwndSearchList, LB_RESETCONTENT, 0, 0);
                for (int i = 0; i < g_playlist_search_result.count; i++) {
                    NeteasePlaylist* playlist =
                        &g_playlist_search_result.playlists[i];
                    char display[512];
                    snprintf(display, sizeof(display), "%s (%d首)",
                        playlist->name, playlist->track_count);
                    wchar_t wide_display[512];
                    utf8_to_wchar(display, wide_display, 512);
                    SendMessageW(g_hwndSearchList, LB_ADDSTRING, 0,
                        (LPARAM)wide_display);
                }
                SetWindowTextW(g_hwndLyrics, L"已返回歌单搜索结果");
                update_result_actions_visibility();
            }
            return 0;

        case IDC_BTN_SEARCH_MODE:
            g_search_mode = !g_search_mode;
            SetWindowTextW(g_hwndSearchModeBtn, g_search_mode ? L"歌单" : L"单曲");
            SendMessage(g_hwndSearchList, LB_RESETCONTENT, 0, 0);
            api_netease_search_free(&g_search_result);
            api_netease_search_playlists_free(&g_playlist_search_result);
            g_result_source = RESULT_SOURCE_NONE;
            update_result_actions_visibility();
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
                if (g_result_source == RESULT_SOURCE_PLAYLIST_SEARCH &&
                    sel >= 0 && sel < g_playlist_search_result.count) {
                    /* Double-click playlist: open its track detail page. */
                    NeteasePlaylist* pl = &g_playlist_search_result.playlists[sel];
                    request_playlist_add(hwnd, pl);
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
            if (g_result_source == RESULT_SOURCE_PLAYLIST_SEARCH &&
                sel >= 0 && sel < g_playlist_search_result.count) {
                NeteasePlaylist* pl = &g_playlist_search_result.playlists[sel];
                request_playlist_add(hwnd, pl);
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
                if (daily_build_add_command(song->id, cmd, sizeof(cmd)) != 0 ||
                    !queue_bot_action(BOT_ACTION_CUSTOM, 0, cmd, NULL)) {
                    SetWindowTextW(g_hwndLyrics, L"无法启动加歌任务");
                    return 0;
                }
                /* Add to playlist display */
                char display[512];
                snprintf(display, sizeof(display), "[队列] %s - %s", song->name, song->artist);
                wchar_t wdisplay[512];
                utf8_to_wchar(display, wdisplay, 512);
                playlist_ui_add(song->id, wdisplay);
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
            int menu_x = GET_X_LPARAM(lParam);
            int menu_y = GET_Y_LPARAM(lParam);
            if (menu_x == -1 && menu_y == -1) {
                int selected = (int)SendMessage(
                    g_hwndSearchList, LB_GETCURSEL, 0, 0);
                RECT item_rect;
                if (selected < 0 || SendMessage(g_hwndSearchList,
                        LB_GETITEMRECT, selected, (LPARAM)&item_rect) == LB_ERR) {
                    return 0;
                }
                POINT menu_point = {item_rect.left + 12, item_rect.bottom};
                ClientToScreen(g_hwndSearchList, &menu_point);
                menu_x = menu_point.x;
                menu_y = menu_point.y;
            } else {
                POINT pt = {menu_x, menu_y};
                ScreenToClient(g_hwndSearchList, &pt);
                LRESULT hit = SendMessage(g_hwndSearchList,
                    LB_ITEMFROMPOINT, 0, MAKELPARAM(pt.x, pt.y));
                if (HIWORD(hit) != 0) return 0;
                SendMessage(g_hwndSearchList, LB_SETCURSEL, LOWORD(hit), 0);
            }
            show_search_context_menu(hwnd, menu_x, menu_y);
            return 0;
        }
        break;
    }


    case WM_CLOSE:
        InterlockedExchange(&g_playlist_add_cancel, 1);
        InterlockedIncrement(&g_search_generation);
        set_search_busy(0);
        if (g_hwndBtnDaily) EnableWindow(g_hwndBtnDaily, TRUE);
        if (g_hwndAuth) SendMessageW(g_hwndAuth, WM_CLOSE, 0, 0);
        ShowWindow(hwnd, SW_HIDE);
        g_is_visible = 0;
        hide_vol_popup();
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_POLL_STATUS);
        KillTimer(hwnd, TIMER_PROGRESS_SMOOTH);
        KillTimer(hwnd, TIMER_LYRIC_SYNC);
        KillTimer(hwnd, 2010);
        g_hwndMain = NULL;
        g_is_visible = 0;
        return 0;

    } /* end switch */

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* ======================== Public API ======================== */

int ui_init(HINSTANCE hInstance, HWND hParent) {
    g_hInstance = hInstance;
    InterlockedExchange(&g_ui_shutting_down, 0);
    InterlockedExchange(&g_poll_in_progress, 0);
    InterlockedExchange(&g_daily_in_progress, 0);
    InterlockedExchange(&g_playlist_add_cancel, 0);
    InterlockedExchange(&g_playlist_add_active, 0);
    InterlockedExchange(&g_playlist_load_active, 0);
    playlist_added_clear();
    g_result_source = RESULT_SOURCE_NONE;
    SecureZeroMemory(&g_auth_session, sizeof(g_auth_session));

    g_shutdown_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_shutdown_event) return -1;

    gdip_init();
    api_netease_init(config_get()->netease_api_url);
    auth_store_load(g_auth_session.cookie, sizeof(g_auth_session.cookie));

    /* Create fonts */
    g_fontNormal = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    g_fontSmall = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    g_fontBold = CreateFontW(-16, 0, 0, 0, FW_BOLD, 0, 0, 0,
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

    /*
     * Keep the panel owned by TS3 and out of the independent app-window list.
     * The legacy gamepad plugin enumerates and subclasses process top-level
     * windows; an unowned WS_EX_APPWINDOW can be mistaken for its raw-input
     * target and make that plugin call std::terminate().
     */
    DWORD main_ex_style = WS_EX_TOOLWINDOW;
    if (cfg->always_on_top) main_ex_style |= WS_EX_TOPMOST;
    g_hwndMain = CreateWindowExW(
        main_ex_style,
        YUNMUSIC_CLASS, L"YunMusic",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME |
        WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
        x, y, cfg->window_width, cfg->window_height,
        hParent, NULL, hInstance, NULL);

    if (!g_hwndMain) {
        UnregisterClassW(YUNMUSIC_CLASS, g_hInstance);
        if (g_fontNormal) { DeleteObject(g_fontNormal); g_fontNormal = NULL; }
        if (g_fontSmall) { DeleteObject(g_fontSmall); g_fontSmall = NULL; }
        if (g_fontBold) { DeleteObject(g_fontBold); g_fontBold = NULL; }
        CloseHandle(g_shutdown_event);
        g_shutdown_event = NULL;
        gdip_shutdown();
        return -1;
    }

    /* Check for updates in background */
    ui_start_worker(version_check_thread, (LPVOID)g_hwndMain);

    return 0;
}

void ui_show(void) {
    if (g_hwndMain) {
        /* Check if window is on a visible screen, reset position if off-screen */
        RECT rc;
        GetWindowRect(g_hwndMain, &rc);
        HMONITOR mon = MonitorFromRect(&rc, MONITOR_DEFAULTTONULL);
        if (!mon) {
            /* Window is off-screen, center it on primary monitor */
            RECT work_area = {0, 0,
                GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
            int width = rc.right - rc.left;
            int height = rc.bottom - rc.top;
            SetWindowPos(g_hwndMain, NULL,
                work_area.left + (work_area.right - work_area.left - width) / 2,
                work_area.top + (work_area.bottom - work_area.top - height) / 2,
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
    if (g_hwndMain) {
        InterlockedExchange(&g_playlist_add_cancel, 1);
        InterlockedIncrement(&g_search_generation);
        set_search_busy(0);
        if (g_hwndBtnDaily) EnableWindow(g_hwndBtnDaily, TRUE);
        if (g_hwndAuth) SendMessageW(g_hwndAuth, WM_CLOSE, 0, 0);
        hide_vol_popup();
        ShowWindow(g_hwndMain, SW_HIDE);
        g_is_visible = 0;
    }
}

void ui_toggle(void) {
    if (g_is_visible) ui_hide(); else ui_show();
}

static void discard_worker_message(const MSG* message) {
    if (!message) return;
    switch (message->message) {
    case WM_YUN_LYRICS_READY: {
        LyricsFetchResult* result = (LyricsFetchResult*)message->lParam;
        if (result) {
            lrc_free(&result->lyrics);
            free(result);
        }
        break;
    }
    case WM_YUN_COVER_READY: {
        CoverFetchResult* result = (CoverFetchResult*)message->lParam;
        if (result) {
            DeleteFileA(result->path);
            free(result);
        }
        break;
    }
    case WM_YUN_DAILY_RESULT: {
        daily_thread_result_free((DailyThreadResult*)message->lParam);
        break;
    }
    case WM_YUN_POLL_RESULT:
        free((void*)message->lParam);
        break;
    case WM_YUN_PLAYLIST_ITEM:
    case WM_YUN_STATUS_TEXT:
        free((void*)message->lParam);
        break;
    case WM_YUN_SEARCH_RESULT:
        search_thread_result_free((SearchThreadResult*)message->lParam);
        break;
    case WM_YUN_PLAYLIST_LOAD:
        playlist_load_result_free((PlaylistLoadResult*)message->lParam);
        break;
    case WM_YUN_AUTH_QR_READY: {
        AuthQrReady* result = (AuthQrReady*)message->lParam;
        if (result) {
            if (result->path[0]) DeleteFileA(result->path);
            free(result);
        }
        break;
    }
    case WM_YUN_AUTH_RESULT:
        free((void*)message->lParam);
        break;
    }
}

static void drain_worker_messages(HWND hwnd) {
    if (!hwnd) return;
    MSG message;
    while (PeekMessageW(&message, hwnd, WM_YUN_LYRICS_READY,
            WM_YUN_AUTH_LOGOUT, PM_REMOVE)) {
        discard_worker_message(&message);
    }
}

void ui_destroy(void) {
    HWND main_window = g_hwndMain;
    if (main_window) {
        KillTimer(main_window, TIMER_POLL_STATUS);
        KillTimer(main_window, TIMER_PROGRESS_SMOOTH);
        KillTimer(main_window, TIMER_LYRIC_SYNC);
        KillTimer(main_window, 2010);
    }

    AcquireSRWLockExclusive(&g_worker_lock);
    InterlockedExchange(&g_ui_shutting_down, 1);
    InterlockedExchange(&g_playlist_add_cancel, 1);
    InterlockedExchange(&g_playlist_load_active, 0);
    InterlockedIncrement(&g_cover_generation);
    if (g_shutdown_event) SetEvent(g_shutdown_event);

    /* Best-effort cancellation for workers blocked in synchronous network I/O. */
    for (size_t i = 0; i < g_worker_handle_count; i++)
        CancelSynchronousIo(g_worker_handles[i]);

    for (size_t i = 0; i < g_worker_handle_count; i++) {
        WaitForSingleObject(g_worker_handles[i], INFINITE);
        CloseHandle(g_worker_handles[i]);
    }
    free(g_worker_handles);
    g_worker_handles = NULL;
    g_worker_handle_count = 0;
    g_worker_handle_capacity = 0;
    ReleaseSRWLockExclusive(&g_worker_lock);

    drain_worker_messages(main_window);
    drain_worker_messages(g_hwndAuth);

    if (g_hwndAuth) {
        DestroyWindow(g_hwndAuth);
        g_hwndAuth = NULL;
    }

    if (main_window) {
        RECT rc;
        if (GetWindowRect(main_window, &rc)) {
            YunConfig* cfg = config_get();
            cfg->window_x = rc.left;
            cfg->window_y = rc.top;
            cfg->window_width = rc.right - rc.left;
            cfg->window_height = rc.bottom - rc.top;
            if (g_hwndDesktopLyric) {
                RECT lyric_rect;
                if (GetWindowRect(g_hwndDesktopLyric, &lyric_rect)) {
                    cfg->desktop_lyric_x = lyric_rect.left;
                    cfg->desktop_lyric_y = lyric_rect.top;
                }
                cfg->desktop_lyric_locked = g_dl_locked;
                cfg->desktop_lyric_font_size = g_dl_font_size;
            }
            config_save();
        }
        if (g_hwndVolPopup) {
            DestroyWindow(g_hwndVolPopup);
            g_hwndVolPopup = NULL;
            g_hwndVolSlider = NULL;
            g_vol_popup_visible = 0;
        }
        playlist_ui_clear();
        DestroyWindow(main_window);
        g_hwndMain = NULL;
        g_hwndPlaylist = NULL;
    }
    desktop_lyric_destroy();
    UnregisterClassW(VOLPOPUP_CLASS, g_hInstance);
    UnregisterClassW(NETEASE_AUTH_CLASS, g_hInstance);
    UnregisterClassW(YUNMUSIC_CLASS, g_hInstance);
    lrc_free(&g_lyrics);
    api_netease_search_free(&g_search_result);
    api_netease_search_playlists_free(&g_playlist_search_result);
    SecureZeroMemory(&g_auth_session, sizeof(g_auth_session));
    gdip_shutdown();
    if (g_fontNormal) { DeleteObject(g_fontNormal); g_fontNormal = NULL; }
    if (g_fontSmall) { DeleteObject(g_fontSmall); g_fontSmall = NULL; }
    if (g_fontBold) { DeleteObject(g_fontBold); g_fontBold = NULL; }
    if (g_shutdown_event) {
        CloseHandle(g_shutdown_event);
        g_shutdown_event = NULL;
    }
}

int ui_is_visible(void) {
    return g_is_visible;
}
