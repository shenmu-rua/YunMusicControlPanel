/*
 * plugin.c - TeamSpeak 3 Plugin entry point for YunMusic
 *
 * Based on official TS3 Plugin SDK (API Version 26)
 * https://github.com/teamspeak/ts3client-pluginsdk
 */

#if defined(WIN32) || defined(__WIN32__) || defined(_WIN32)
#pragma warning(disable : 4100) /* Disable Unreferenced parameter warning */
#include <Windows.h>
#endif

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Export decorations for MSVC (GCC exports all symbols by default with -shared) */
#ifdef _MSC_VER
#define TS3_EXPORT __declspec(dllexport)
#else
#define TS3_EXPORT __attribute__((visibility("default")))
#endif

#include "teamspeak/public_definitions.h"
#include "teamspeak/public_errors.h"
#include "teamspeak/public_errors_rare.h"
#include "teamspeak/public_rare_definitions.h"
#include "ts3_functions.h"
#include "plugin_definitions.h"

#include "config.h"
#include "auth_store.h"
#include "api_bot.h"
#include "ui_win32.h"

#ifdef _WIN32
#define _strcpy(dest, destSize, src) strcpy_s(dest, destSize, src)
#else
#define _strcpy(dest, destSize, src) \
    { strncpy(dest, src, destSize - 1); (dest)[destSize - 1] = '\0'; }
#endif

#define PLUGIN_API_VERSION 26
#define PATH_BUFSIZE 512

#ifndef YUNMUSIC_VERSION
#define YUNMUSIC_VERSION "1.2.14"
#endif

static struct TS3Functions ts3Functions;
static char* pluginID = NULL;
static HMODULE g_hModule = NULL;
static volatile LONG g_ui_init_state = 0;

/* Menu IDs */
enum { MENU_ID_MUSIC_PANEL = 1 };

static HWND find_ts3_owner_window(void)
{
    HWND candidate = GetActiveWindow();
    if (!candidate) candidate = GetForegroundWindow();
    if (!candidate) return NULL;

    candidate = GetAncestor(candidate, GA_ROOT);
    DWORD process_id = 0;
    GetWindowThreadProcessId(candidate, &process_id);
    return process_id == GetCurrentProcessId() ? candidate : NULL;
}

/*
 * Keep TS3 startup lightweight. Creating the hidden Win32 windows starts
 * timers and network workers, so defer that work until the user actually
 * opens the music panel. This also isolates plugin startup from other TS3
 * plugins that hook top-level windows or raw-input device enumeration.
 */
static int ensure_ui_initialized(void)
{
    LONG state = InterlockedCompareExchange(&g_ui_init_state, 0, 0);
    if (state == 2) return 1;
    if (InterlockedCompareExchange(&g_ui_init_state, 1, 0) != 0) return 0;

    if (ui_init(g_hModule, find_ts3_owner_window()) == 0) {
        InterlockedExchange(&g_ui_init_state, 2);
        return 1;
    }

    InterlockedExchange(&g_ui_init_state, 0);
    ts3Functions.logMessage("YunMusic: Failed to create UI", LogLevel_ERROR,
        "YunMusic", 0);
    return 0;
}

#ifdef _WIN32
static int wcharToUtf8(const wchar_t* str, char** result)
{
    if (!str || !result) return -1;
    int outlen = WideCharToMultiByte(CP_UTF8, 0, str, -1, 0, 0, 0, 0);
    if (outlen <= 0) return -1;
    *result    = (char*)malloc(outlen);
    if (!*result) return -1;
    if (WideCharToMultiByte(CP_UTF8, 0, str, -1, *result, outlen, 0, 0) == 0) {
        free(*result);
        *result = NULL;
        return -1;
    }
    return 0;
}
#endif

/*********************************** Required functions ************************************/

TS3_EXPORT const char* ts3plugin_name()
{
#ifdef _WIN32
    static char* result = NULL;
    if (!result) {
        const wchar_t* name = L"YunMusic";
        if (wcharToUtf8(name, &result) == -1) {
            result = "YunMusic";
        }
    }
    return result;
#else
    return "YunMusic";
#endif
}

TS3_EXPORT const char* ts3plugin_version()
{
    return YUNMUSIC_VERSION;
}

TS3_EXPORT int ts3plugin_apiVersion()
{
    return PLUGIN_API_VERSION;
}

TS3_EXPORT const char* ts3plugin_author()
{
    return "shenmu";
}

TS3_EXPORT const char* ts3plugin_description()
{
    return "Music control panel for ts3audiobot with lyrics, cover art, and search.";
}

/* Set TeamSpeak 3 callback functions */
TS3_EXPORT void ts3plugin_setFunctionPointers(const struct TS3Functions funcs)
{
    ts3Functions = funcs;
}

/*
 * Custom code called right after loading the plugin. Returns 0 on success, 1 on failure.
 */
TS3_EXPORT int ts3plugin_init()
{
    char pluginPath[PATH_BUFSIZE];

    /* Get plugin path */
    ts3Functions.getPluginPath(pluginPath, PATH_BUFSIZE, pluginID);

    /* Remove trailing separator */
    size_t len = strlen(pluginPath);
    if (len > 0 && (pluginPath[len - 1] == '\\' || pluginPath[len - 1] == '/')) {
        pluginPath[len - 1] = '\0';
    }

    /* Initialize config */
    if (config_init(pluginPath) != 0) {
        ts3Functions.logMessage("YunMusic: Failed to load config", LogLevel_WARNING, "YunMusic", 0);
    }

    /* Initialize bot API client */
    YunConfig* cfg = config_get();
    int auth_ready = auth_store_init(pluginPath) == 0;
    if (auth_ready && cfg->music_u[0]) {
        char migrated_cookie[640];
        char verified_cookie[640];
        snprintf(migrated_cookie, sizeof(migrated_cookie),
            "MUSIC_U=%s", cfg->music_u);
        if (auth_store_save(migrated_cookie) == 0 &&
            auth_store_load(verified_cookie, sizeof(verified_cookie)) == 0 &&
            strcmp(migrated_cookie, verified_cookie) == 0) {
            config_clear_legacy_music_u();
        } else {
            ts3Functions.logMessage(
                "YunMusic: legacy NetEase session migration failed; plaintext value retained",
                LogLevel_WARNING, "YunMusic", 0);
        }
        SecureZeroMemory(migrated_cookie, sizeof(migrated_cookie));
        SecureZeroMemory(verified_cookie, sizeof(verified_cookie));
    } else if (!auth_ready && cfg->music_u[0]) {
        ts3Functions.logMessage(
            "YunMusic: auth storage unavailable; legacy plaintext value retained",
            LogLevel_WARNING, "YunMusic", 0);
    }
    api_bot_init(cfg->bot_api_url);

    /* UI windows, timers, and workers are initialized on first use. */
    InterlockedExchange(&g_ui_init_state, 0);
    ts3Functions.logMessage("YunMusic: Plugin initialized successfully (UI deferred)",
        LogLevel_INFO, "YunMusic", 0);
    return 0;
}

/* Custom code called right before the plugin is unloaded */
TS3_EXPORT void ts3plugin_shutdown()
{
    /* Stop queued Bot requests before waiting for UI workers to exit. */
    api_bot_cancel_pending();
    if (InterlockedExchange(&g_ui_init_state, 0) == 2)
        ui_destroy();
    api_bot_cleanup();

    if (pluginID) {
        free(pluginID);
        pluginID = NULL;
    }
}

/****************************** Optional functions ********************************/

TS3_EXPORT void ts3plugin_registerPluginID(const char* id)
{
    if (!id) return;
    const size_t sz = strlen(id) + 1;
    char* newID = (char*)malloc(sz);
    if (!newID) return;
    _strcpy(newID, sz, id);
    free(pluginID);
    pluginID = newID;
}

/* Required to release the memory for parameter "data" allocated in ts3plugin_infoData and ts3plugin_initMenus */
TS3_EXPORT void ts3plugin_freeMemory(void* data)
{
    free(data);
}

/* Plugin requests to be always automatically loaded */
TS3_EXPORT int ts3plugin_requestAutoload()
{
    return 1; /* 1 = request autoloaded */
}

/*
 * Initialize plugin menus.
 * This function is called after ts3plugin_init and ts3plugin_registerPluginID.
 */
TS3_EXPORT void ts3plugin_initMenus(struct PluginMenuItem*** menuItems, char** menuIcon)
{
    if (!menuItems || !menuIcon) return;
    *menuItems = NULL;
    *menuIcon = NULL;

    /* Create menu: one global item in the Plugins menu */
    const size_t sz = 2;
    size_t       n  = 0;
    *menuItems      = (struct PluginMenuItem**)malloc(sizeof(struct PluginMenuItem*) * sz);
    if (!*menuItems) return;

    struct PluginMenuItem* item = (struct PluginMenuItem*)malloc(sizeof(struct PluginMenuItem));
    if (!item) {
        free(*menuItems);
        *menuItems = NULL;
        return;
    }
    item->type = PLUGIN_MENU_TYPE_GLOBAL;
    item->id   = MENU_ID_MUSIC_PANEL;
    _strcpy(item->text, PLUGIN_MENU_BUFSZ, "Music Panel");
    _strcpy(item->icon, PLUGIN_MENU_BUFSZ, "");
    (*menuItems)[n++] = item;

    (*menuItems)[n++] = NULL;
    assert(n == sz);

    /* Set plugin icon (PNG file next to DLL) */
    {
        char iconPath[PATH_BUFSIZE] = {0};
        char dllPath[PATH_BUFSIZE] = {0};
        GetModuleFileNameA(g_hModule, dllPath, PATH_BUFSIZE);
        char* lastSlash = strrchr(dllPath, '\\');
        if (lastSlash) {
            *(lastSlash + 1) = '\0';
            snprintf(iconPath, sizeof(iconPath), "%sYunMusicPlugin.png", dllPath);
        }
        size_t iconLen = strlen(iconPath) + 1;
        *menuIcon = (char*)malloc(iconLen);
        if (*menuIcon)
            _strcpy(*menuIcon, iconLen, iconPath);
    }

    /* Allocate a plugin ID string if not yet done */
    if (!pluginID) {
        const char* id = "yunmusic-plugin";
        const size_t idlen = strlen(id) + 1;
        pluginID = (char*)malloc(idlen * sizeof(char));
        _strcpy(pluginID, idlen, id);
    }
}

/*
 * Called when a plugin menu item is triggered.
 */
TS3_EXPORT void ts3plugin_onMenuItemEvent(uint64 serverConnectionHandlerID, enum PluginMenuType type, int menuItemID, uint64 selectedItemID)
{
    if (type == PLUGIN_MENU_TYPE_GLOBAL && menuItemID == MENU_ID_MUSIC_PANEL) {
        if (ensure_ui_initialized()) ui_toggle();
    }
}

/*
 * Plugin command keyword. Return NULL or "" if not used.
 */
TS3_EXPORT const char* ts3plugin_commandKeyword()
{
    return "yunmusic";
}

/*
 * Process plugin command. Return 0 if handled, 1 if not handled.
 */
TS3_EXPORT int ts3plugin_processCommand(uint64 serverConnectionHandlerID, const char* command)
{
    if (!command) return 1;
    if (strcmp(command, "toggle") == 0) {
        if (ensure_ui_initialized()) ui_toggle();
        return 0;
    }
    if (strcmp(command, "show") == 0) {
        if (ensure_ui_initialized()) ui_show();
        return 0;
    }
    if (strcmp(command, "hide") == 0) {
        if (InterlockedCompareExchange(&g_ui_init_state, 0, 0) == 2)
            ui_hide();
        return 0;
    }
    return 1;
}

/* ======================== DLL Entry Point ======================== */

#ifdef _WIN32
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        g_hModule = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
        break;
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
#endif
