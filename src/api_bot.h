/*
 * api_bot.h - ts3audiobot HTTP API client
 */

#ifndef API_BOT_H
#define API_BOT_H

#include "bot_queue_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Song information structure */
typedef struct {
    char    title[256];       /* Song title */
    char    link[512];        /* Song link URL */
    double  position;         /* Current position in seconds */
    double  length;           /* Total length in seconds */
    int     paused;           /* 1 if paused, 0 if playing */
    char    audio_type[64];   /* Audio type (e.g. "media") */
    char    song_id[64];      /* Extracted song ID (from link) */
} SongInfo;

/* Bot status structure */
typedef struct {
    SongInfo song;
    float    volume;          /* Volume 0-100 */
    char     repeat[32];      /* Repeat mode */
    int      random;          /* Random mode */
    BotQueuePreview queue;    /* Authoritative preview from `yun list` */
} BotStatus;

/*
 * api_bot_init - Initialize the bot API client
 * base_url: e.g. "http://your-server:58913"
 */
void api_bot_init(const char* base_url);

/*
 * api_bot_cancel_pending - Reject queued requests during plugin shutdown.
 * The currently running synchronous request remains bounded by short timeouts.
 */
void api_bot_cancel_pending(void);

/*
 * api_bot_cleanup - Close persistent HTTP connection.
 * Call on plugin shutdown.
 */
void api_bot_cleanup(void);

/*
 * api_bot_poll_status - Poll current bot status (song, volume, repeat, random)
 * Returns 0 on success, -1 on error
 */
int api_bot_poll_status(BotStatus* status);

/*
 * api_bot_play - Resume playback
 * Returns 0 on success, -1 on error
 */
int api_bot_play(void);

/*
 * api_bot_pause - Pause playback
 * Returns 0 on success, -1 on error
 */
int api_bot_pause(void);

/*
 * api_bot_next - Skip to next song
 * Returns 0 on success, -1 on error
 */
int api_bot_next(void);

/*
 * api_bot_prev - Go to previous song
 * Returns 0 on success, -1 on error
 */
int api_bot_prev(void);

/*
 * api_bot_set_volume - Set volume (0-100)
 * Returns 0 on success, -1 on error
 */
int api_bot_set_volume(int volume);

/*
 * api_bot_send_command_get - Send a raw ts3audiobot GET command.
 * command: e.g. "(/yun/play/123)"
 */
int api_bot_send_command_get(const char* command);

/*
 * api_bot_seek - Seek to position in seconds
 * Note: May have bugs in YunBot
 * Returns 0 on success, -1 on error
 */
int api_bot_seek(double seconds);

/*
 * api_bot_send_command_post - Send a command via POST request
 * command: the command text (e.g. "yun play https://music.163.com/#/song?id=123")
 * Returns 0 on success, -1 on error
 */
int api_bot_send_command_post(const char* command);

#ifdef __cplusplus
}
#endif

#endif /* API_BOT_H */
