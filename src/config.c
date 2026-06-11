/*
 * config.c - INI configuration file reader/writer
 * Uses Win32 GetPrivateProfileString / WritePrivateProfileString
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "config.h"

static YunConfig g_config;
static char g_ini_path[MAX_PATH] = {0};

int config_init(const char* plugin_path) {
    /* Build INI file path: plugins/yunmusic.ini */
    snprintf(g_ini_path, sizeof(g_ini_path), "%s\\yunmusic.ini", plugin_path);

    /* Set defaults */
    strcpy(g_config.bot_api_url, "http://localhost:58913");
    strcpy(g_config.netease_api_url, "http://localhost:3000");
    g_config.poll_interval_ms = 2000;
    g_config.window_x = -1;
    g_config.window_y = -1;
    g_config.always_on_top = 0;

    /* Read from INI file */
    char buf[512];

    GetPrivateProfileStringA("server", "api_url", g_config.bot_api_url, buf, sizeof(buf), g_ini_path);
    strncpy(g_config.bot_api_url, buf, sizeof(g_config.bot_api_url) - 1);

    GetPrivateProfileStringA("netease", "api_url", g_config.netease_api_url, buf, sizeof(buf), g_ini_path);
    strncpy(g_config.netease_api_url, buf, sizeof(g_config.netease_api_url) - 1);

    GetPrivateProfileStringA("netease", "music_u", "", buf, sizeof(buf), g_ini_path);
    strncpy(g_config.music_u, buf, sizeof(g_config.music_u) - 1);

    GetPrivateProfileStringA("update", "url", "", buf, sizeof(buf), g_ini_path);
    strncpy(g_config.update_url, buf, sizeof(g_config.update_url) - 1);

    g_config.poll_interval_ms = GetPrivateProfileIntA("ui", "poll_interval_ms", g_config.poll_interval_ms, g_ini_path);
    g_config.window_x = GetPrivateProfileIntA("ui", "window_x", g_config.window_x, g_ini_path);
    g_config.window_y = GetPrivateProfileIntA("ui", "window_y", g_config.window_y, g_ini_path);
    g_config.always_on_top = GetPrivateProfileIntA("ui", "always_on_top", g_config.always_on_top, g_ini_path);

    return 0;
}

YunConfig* config_get(void) {
    return &g_config;
}

int config_save(void) {
    char buf[32];

    WritePrivateProfileStringA("server", "api_url", g_config.bot_api_url, g_ini_path);
    WritePrivateProfileStringA("netease", "api_url", g_config.netease_api_url, g_ini_path);
    WritePrivateProfileStringA("netease", "music_u", g_config.music_u, g_ini_path);
    WritePrivateProfileStringA("update", "url", g_config.update_url, g_ini_path);

    snprintf(buf, sizeof(buf), "%d", g_config.poll_interval_ms);
    WritePrivateProfileStringA("ui", "poll_interval_ms", buf, g_ini_path);

    snprintf(buf, sizeof(buf), "%d", g_config.window_x);
    WritePrivateProfileStringA("ui", "window_x", buf, g_ini_path);

    snprintf(buf, sizeof(buf), "%d", g_config.window_y);
    WritePrivateProfileStringA("ui", "window_y", buf, g_ini_path);

    snprintf(buf, sizeof(buf), "%d", g_config.always_on_top);
    WritePrivateProfileStringA("ui", "always_on_top", buf, g_ini_path);

    return 0;
}
