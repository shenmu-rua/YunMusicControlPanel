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

#define DEFAULT_API_HOST "localhost"
#define DEFAULT_API_SCHEME "http"
#define DEFAULT_BOT_API_PORT 58913
#define DEFAULT_NETEASE_API_PORT 3000

int config_apply_network(void) {
    int bot_len;
    int netease_len;

    if (!g_config.api_host[0] ||
        (strcmp(g_config.api_scheme, "http") != 0 &&
         strcmp(g_config.api_scheme, "https") != 0) ||
        g_config.bot_api_port < 1 || g_config.bot_api_port > 65535 ||
        g_config.netease_api_port < 1 || g_config.netease_api_port > 65535) {
        return -1;
    }

    bot_len = snprintf(g_config.bot_api_url, sizeof(g_config.bot_api_url),
        "%s://%s:%d", g_config.api_scheme, g_config.api_host,
        g_config.bot_api_port);
    netease_len = snprintf(g_config.netease_api_url,
        sizeof(g_config.netease_api_url), "%s://%s:%d",
        g_config.api_scheme, g_config.api_host, g_config.netease_api_port);
    return bot_len > 0 && bot_len < (int)sizeof(g_config.bot_api_url) &&
           netease_len > 0 &&
           netease_len < (int)sizeof(g_config.netease_api_url) ? 0 : -1;
}

int config_init(const char* plugin_path) {
    if (!plugin_path || !plugin_path[0]) return -1;

    /* Build INI file path: plugins/yunmusic.ini */
    if (snprintf(g_ini_path, sizeof(g_ini_path), "%s\\yunmusic.ini", plugin_path)
        >= (int)sizeof(g_ini_path)) {
        g_ini_path[0] = '\0';
        return -1;
    }
    DWORD attributes = GetFileAttributesA(g_ini_path);
    int config_exists =
        attributes != INVALID_FILE_ATTRIBUTES &&
        !(attributes & FILE_ATTRIBUTE_DIRECTORY);

    /* Set defaults */
    memset(&g_config, 0, sizeof(g_config));
    strcpy(g_config.api_host, DEFAULT_API_HOST);
    strcpy(g_config.api_scheme, DEFAULT_API_SCHEME);
    g_config.bot_api_port = DEFAULT_BOT_API_PORT;
    g_config.netease_api_port = DEFAULT_NETEASE_API_PORT;
    g_config.poll_interval_ms = 2000;
    g_config.window_x = -1;
    g_config.window_y = -1;
    g_config.window_width = 420;
    g_config.window_height = 650;
    g_config.always_on_top = 0;
    g_config.desktop_lyric_x = -1;
    g_config.desktop_lyric_y = -1;
    g_config.desktop_lyric_locked = 0;
    g_config.desktop_lyric_font_size = 24;

    /* Read from INI file */
    char buf[512];

    GetPrivateProfileStringA("network", "host", g_config.api_host, buf, sizeof(buf), g_ini_path);
    strncpy(g_config.api_host, buf, sizeof(g_config.api_host) - 1);
    g_config.api_host[sizeof(g_config.api_host) - 1] = '\0';
    GetPrivateProfileStringA("network", "scheme", g_config.api_scheme, buf, sizeof(buf), g_ini_path);
    strncpy(g_config.api_scheme, buf, sizeof(g_config.api_scheme) - 1);
    g_config.api_scheme[sizeof(g_config.api_scheme) - 1] = '\0';
    g_config.bot_api_port = GetPrivateProfileIntA("network", "bot_port", DEFAULT_BOT_API_PORT, g_ini_path);
    g_config.netease_api_port = GetPrivateProfileIntA("network", "netease_port", DEFAULT_NETEASE_API_PORT, g_ini_path);
    if (config_apply_network() != 0) {
        strcpy(g_config.api_host, DEFAULT_API_HOST);
        strcpy(g_config.api_scheme, DEFAULT_API_SCHEME);
        g_config.bot_api_port = DEFAULT_BOT_API_PORT;
        g_config.netease_api_port = DEFAULT_NETEASE_API_PORT;
        config_apply_network();
    }
    GetPrivateProfileStringA("netease", "music_u", "", buf, sizeof(buf), g_ini_path);
    strncpy(g_config.music_u, buf, sizeof(g_config.music_u) - 1);
    g_config.music_u[sizeof(g_config.music_u) - 1] = '\0';

    GetPrivateProfileStringA("update", "url", "", buf, sizeof(buf), g_ini_path);
    strncpy(g_config.update_url, buf, sizeof(g_config.update_url) - 1);
    g_config.update_url[sizeof(g_config.update_url) - 1] = '\0';

    g_config.poll_interval_ms = GetPrivateProfileIntA("ui", "poll_interval_ms", g_config.poll_interval_ms, g_ini_path);
    if (g_config.poll_interval_ms < 500) g_config.poll_interval_ms = 500;
    if (g_config.poll_interval_ms > 60000) g_config.poll_interval_ms = 60000;
    g_config.window_x = GetPrivateProfileIntA("ui", "window_x", g_config.window_x, g_ini_path);
    g_config.window_y = GetPrivateProfileIntA("ui", "window_y", g_config.window_y, g_ini_path);
    g_config.window_width = GetPrivateProfileIntA("ui", "window_width", g_config.window_width, g_ini_path);
    g_config.window_height = GetPrivateProfileIntA("ui", "window_height", g_config.window_height, g_ini_path);
    if (g_config.window_width < 380) g_config.window_width = 380;
    if (g_config.window_width > 1600) g_config.window_width = 1600;
    if (g_config.window_height < 560) g_config.window_height = 560;
    if (g_config.window_height > 1200) g_config.window_height = 1200;
    g_config.always_on_top =
        GetPrivateProfileIntA("ui", "always_on_top", g_config.always_on_top, g_ini_path) ? 1 : 0;
    g_config.desktop_lyric_x = GetPrivateProfileIntA("ui", "desktop_lyric_x", g_config.desktop_lyric_x, g_ini_path);
    g_config.desktop_lyric_y = GetPrivateProfileIntA("ui", "desktop_lyric_y", g_config.desktop_lyric_y, g_ini_path);
    g_config.desktop_lyric_locked =
        GetPrivateProfileIntA("ui", "desktop_lyric_locked", g_config.desktop_lyric_locked, g_ini_path) ? 1 : 0;
    g_config.desktop_lyric_font_size = GetPrivateProfileIntA("ui", "desktop_lyric_font_size", g_config.desktop_lyric_font_size, g_ini_path);
    if (g_config.desktop_lyric_font_size < 18) g_config.desktop_lyric_font_size = 18;
    if (g_config.desktop_lyric_font_size > 36) g_config.desktop_lyric_font_size = 36;

    return config_exists ? 0 : -1;
}

YunConfig* config_get(void) {
    return &g_config;
}

int config_save(void) {
    char buf[32];
    int ok = 1;

    if (!g_ini_path[0]) return -1;

    if (config_apply_network() != 0) return -1;
    ok &= WritePrivateProfileStringA("network", "host", g_config.api_host, g_ini_path);
    ok &= WritePrivateProfileStringA("network", "scheme", g_config.api_scheme, g_ini_path);
    snprintf(buf, sizeof(buf), "%d", g_config.bot_api_port);
    ok &= WritePrivateProfileStringA("network", "bot_port", buf, g_ini_path);
    snprintf(buf, sizeof(buf), "%d", g_config.netease_api_port);
    ok &= WritePrivateProfileStringA("network", "netease_port", buf, g_ini_path);
    ok &= WritePrivateProfileStringA("server", "api_url", NULL, g_ini_path);
    ok &= WritePrivateProfileStringA("netease", "api_url", NULL, g_ini_path);
    if (g_config.music_u[0])
        ok &= WritePrivateProfileStringA("netease", "music_u", g_config.music_u, g_ini_path);
    else
        ok &= WritePrivateProfileStringA("netease", "music_u", NULL, g_ini_path);
    ok &= WritePrivateProfileStringA("update", "url", g_config.update_url, g_ini_path);

    snprintf(buf, sizeof(buf), "%d", g_config.poll_interval_ms);
    ok &= WritePrivateProfileStringA("ui", "poll_interval_ms", buf, g_ini_path);

    snprintf(buf, sizeof(buf), "%d", g_config.window_x);
    ok &= WritePrivateProfileStringA("ui", "window_x", buf, g_ini_path);

    snprintf(buf, sizeof(buf), "%d", g_config.window_y);
    ok &= WritePrivateProfileStringA("ui", "window_y", buf, g_ini_path);

    snprintf(buf, sizeof(buf), "%d", g_config.window_width);
    ok &= WritePrivateProfileStringA("ui", "window_width", buf, g_ini_path);

    snprintf(buf, sizeof(buf), "%d", g_config.window_height);
    ok &= WritePrivateProfileStringA("ui", "window_height", buf, g_ini_path);

    snprintf(buf, sizeof(buf), "%d", g_config.always_on_top);
    ok &= WritePrivateProfileStringA("ui", "always_on_top", buf, g_ini_path);

    snprintf(buf, sizeof(buf), "%d", g_config.desktop_lyric_x);
    ok &= WritePrivateProfileStringA("ui", "desktop_lyric_x", buf, g_ini_path);

    snprintf(buf, sizeof(buf), "%d", g_config.desktop_lyric_y);
    ok &= WritePrivateProfileStringA("ui", "desktop_lyric_y", buf, g_ini_path);

    snprintf(buf, sizeof(buf), "%d", g_config.desktop_lyric_locked);
    ok &= WritePrivateProfileStringA("ui", "desktop_lyric_locked", buf, g_ini_path);

    snprintf(buf, sizeof(buf), "%d", g_config.desktop_lyric_font_size);
    ok &= WritePrivateProfileStringA("ui", "desktop_lyric_font_size", buf, g_ini_path);

    return ok ? 0 : -1;
}

int config_clear_legacy_music_u(void) {
    if (!g_ini_path[0]) return -1;
    if (!WritePrivateProfileStringA("netease", "music_u", NULL, g_ini_path))
        return -1;
    SecureZeroMemory(g_config.music_u, sizeof(g_config.music_u));
    return 0;
}
