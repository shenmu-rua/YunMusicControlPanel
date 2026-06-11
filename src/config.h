/*
 * config.h - INI configuration file reader/writer
 */

#ifndef CONFIG_H
#define CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Configuration structure */
typedef struct {
    char bot_api_url[256];      /* ts3audiobot API base URL */
    char netease_api_url[256];  /* Netease Cloud Music API URL */
    int  poll_interval_ms;      /* Status polling interval (default: 2000) */
    int  window_x;              /* Window position X */
    int  window_y;              /* Window position Y */
    int  always_on_top;         /* Keep window on top */
    char music_u[512];          /* Netease MUSIC_U cookie for login */
    char update_url[256];       /* Version check URL (GitHub API or custom JSON) */
} YunConfig;

/*
 * config_init - Initialize config, load from INI file
 * Returns 0 on success, -1 on error (file not found, uses defaults)
 */
int config_init(const char* plugin_path);

/*
 * config_get - Get the global config pointer
 */
YunConfig* config_get(void);

/*
 * config_save - Save current config to INI file
 */
int config_save(void);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_H */
