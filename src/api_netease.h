/*
 * api_netease.h - Netease Cloud Music API client
 */

#ifndef API_NETEASE_H
#define API_NETEASE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Search result item */
typedef struct {
    char id[64];
    char name[256];
    char artist[256];
    char album[256];
    int  duration_ms;
} NeteaseSong;

/* Search results */
typedef struct {
    NeteaseSong* songs;
    int          count;
} NeteaseSearchResult;

/* Playlist search result item */
typedef struct {
    char id[64];
    char name[256];
    char creator[128];
    int  track_count;
    int  play_count;
} NeteasePlaylist;

/* Playlist search results */
typedef struct {
    NeteasePlaylist* playlists;
    int              count;
} NeteasePlaylistResult;

#define NETEASE_COOKIE_MAX 4096
#define NETEASE_QR_SUCCESS 0
#define NETEASE_QR_WAITING 1
#define NETEASE_QR_CONFIRM 2
#define NETEASE_QR_ERROR  (-1)
#define NETEASE_QR_EXPIRED (-2)
#define NETEASE_AUTH_ERROR (-1)
#define NETEASE_AUTH_INVALID (-2)

typedef struct {
    char cookie[NETEASE_COOKIE_MAX];
    char user_id[64];
    char nickname[128];
    int  logged_in;
} NeteaseAuthSession;

/*
 * api_netease_init - Initialize the Netease API client
 * base_url: e.g. "http://your-server:3000"
 */
void api_netease_init(const char* base_url);

/*
 * api_netease_search - Search for songs
 * keyword: search keyword
 * limit: max results (default 20)
 * Returns 0 on success, -1 on error. Caller must free result->songs.
 */
int api_netease_search(const char* keyword, int limit, NeteaseSearchResult* result);

/*
 * api_netease_get_lyrics - Get LRC lyrics for a song
 * song_id: Netease song ID
 * lrc_out: output buffer for LRC text (caller must free)
 * Returns 0 on success, -1 on error
 */
int api_netease_get_lyrics(const char* song_id, char** lrc_out);

/*
 * api_netease_get_cover_url - Get cover image URL for a song
 * song_id: Netease song ID
 * url_out: output buffer for URL string
 * url_out_size: buffer size
 * Returns 0 on success, -1 on error
 */
int api_netease_get_cover_url(const char* song_id, char* url_out, int url_out_size);

/*
 * api_netease_download_file - Download a file from URL to local path
 * url: full URL to download
 * local_path: local file path to save to
 * Returns 0 on success, -1 on error
 */
int api_netease_download_file(const char* url, const char* local_path);

/*
 * api_netease_search_free - Free search result memory
 */
void api_netease_search_free(NeteaseSearchResult* result);

/*
 * api_netease_search_playlists - Search for playlists
 * keyword: search keyword
 * limit: max results (default 20)
 * Returns 0 on success, -1 on error. Caller must free result->playlists.
 */
int api_netease_search_playlists(const char* keyword, int limit, NeteasePlaylistResult* result);

/*
 * api_netease_search_playlists_free - Free playlist search result memory
 */
void api_netease_search_playlists_free(NeteasePlaylistResult* result);

/*
 * api_netease_get_playlist_tracks - Get songs in a playlist
 * playlist_id: Netease playlist ID
 * result: output search result with songs
 * Returns 0 on success, -1 on error. Caller must free result->songs.
 */
int api_netease_get_playlist_tracks(const char* playlist_id, NeteaseSearchResult* result);

/* Authenticated requests support both HTTP and HTTPS API deployments. */
int api_netease_auth_supported(void);

/*
 * api_netease_qr_login_key - Get a QR login key
 */
int api_netease_qr_login_key(char* key_out, int key_size);

/*
 * api_netease_qr_login_create - Create QR code URL for login
 */
int api_netease_qr_login_create(const char* key, char** qrimg_out);

/*
 * api_netease_qr_login_check - Check QR login status
 * Returns: 0=success(cookie filled), 1=waiting, 2=scanned, -1=error
 */
int api_netease_qr_login_check(const char* key, char* cookie_out, int cookie_size);

/* Validate the session; returns NETEASE_AUTH_INVALID for an expired login. */
int api_netease_login_status(NeteaseAuthSession* session);

/* Best-effort server logout for an existing session. */
int api_netease_logout(const NeteaseAuthSession* session);

/*
 * api_netease_get_daily_songs - Get daily recommendations (requires MUSIC_U cookie)
 */
int api_netease_get_daily_songs(const NeteaseAuthSession* session,
                                NeteaseSearchResult* result);

#ifdef __cplusplus
}
#endif

#endif /* API_NETEASE_H */
