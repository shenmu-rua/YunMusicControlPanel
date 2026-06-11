/*
 * api_netease.c - Netease Cloud Music API client using WinHTTP
 */

#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "api_netease.h"
#include "cJSON.h"

#pragma comment(lib, "winhttp.lib")

static char g_base_host[256] = {0};
static int  g_base_port = 80;
static int  g_use_https = 0;

/* URL-encode a UTF-8 string. Caller must free() the result. */
static char* url_encode(const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    /* Worst case: every byte becomes %XX (3x) */
    char* out = (char*)malloc(len * 3 + 1);
    if (!out) return NULL;
    char* p = out;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)str[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            *p++ = (char)c;
        } else {
            snprintf(p, 4, "%%%02X", c);
            p += 3;
        }
    }
    *p = '\0';
    return out;
}

static void parse_url(const char* url, char* host, int host_len, int* port, int* https) {
    const char* p = url;
    *https = 0;
    *port = 80;

    if (strncmp(p, "https://", 8) == 0) {
        *https = 1; *port = 443; p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    }

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

/* HTTP GET returning response body as malloc'd string */
static char* http_get(const wchar_t* host, int port, const wchar_t* path, int use_https) {
    HINTERNET hSession = NULL, hConnect = NULL, hRequest = NULL;
    char* result = NULL;
    char* buffer = NULL;
    DWORD totalSize = 0;

    hSession = WinHttpOpen(L"YunMusicPlugin/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) goto cleanup;

    hConnect = WinHttpConnect(hSession, host, (INTERNET_PORT)port, 0);
    if (!hConnect) goto cleanup;

    DWORD flags = use_https ? WINHTTP_FLAG_SECURE : 0;
    hRequest = WinHttpOpenRequest(hConnect, L"GET", path,
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) goto cleanup;

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) goto cleanup;
    if (!WinHttpReceiveResponse(hRequest, NULL)) goto cleanup;

    DWORD bufSize = 4096;
    buffer = (char*)malloc(bufSize);
    if (!buffer) goto cleanup;

    for (;;) {
        DWORD bytesAvailable = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &bytesAvailable)) break;
        if (bytesAvailable == 0) break;

        if (totalSize + bytesAvailable + 1 > bufSize) {
            bufSize = (totalSize + bytesAvailable + 1) * 2;
            char* newbuf = (char*)realloc(buffer, bufSize);
            if (!newbuf) break;
            buffer = newbuf;
        }

        DWORD bytesRead = 0;
        if (!WinHttpReadData(hRequest, buffer + totalSize, bytesAvailable, &bytesRead)) break;
        totalSize += bytesRead;
    }

    if (buffer && totalSize > 0) {
        buffer[totalSize] = '\0';
        result = buffer;
        buffer = NULL;
    }

cleanup:
    if (buffer) free(buffer);
    if (hRequest) WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    if (hSession) WinHttpCloseHandle(hSession);
    return result;
}

/* HTTP GET with Cookie header, optionally capture Set-Cookie into cookie_out */
static char* http_get_with_cookie(const wchar_t* host, int port, const wchar_t* path,
                                   int use_https, const char* cookie,
                                   char* cookie_out, int cookie_out_size) {
    HINTERNET hSession = NULL, hConnect = NULL, hRequest = NULL;
    char* result = NULL;
    char* buffer = NULL;
    DWORD totalSize = 0;

    hSession = WinHttpOpen(L"YunMusicPlugin/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) goto cleanup;

    hConnect = WinHttpConnect(hSession, host, (INTERNET_PORT)port, 0);
    if (!hConnect) goto cleanup;

    DWORD flags = use_https ? WINHTTP_FLAG_SECURE : 0;
    hRequest = WinHttpOpenRequest(hConnect, L"GET", path,
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) goto cleanup;

    /* Add Cookie header if provided */
    if (cookie && cookie[0]) {
        wchar_t wcookie[1024];
        wchar_t cookie_hdr[1200];
        MultiByteToWideChar(CP_UTF8, 0, cookie, -1, wcookie, 1024);
        wsprintfW(cookie_hdr, L"Cookie: %s", wcookie);
        WinHttpAddRequestHeaders(hRequest, cookie_hdr, (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
    }

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) goto cleanup;
    if (!WinHttpReceiveResponse(hRequest, NULL)) goto cleanup;

    /* Capture Set-Cookie header */
    if (cookie_out && cookie_out_size > 0) {
        cookie_out[0] = '\0';
        DWORD hdrSize = 0;
        /* Query Set-Cookie header size */
        WinHttpQueryHeaders(hRequest,
            WINHTTP_QUERY_SET_COOKIE | WINHTTP_QUERY_FLAG_REQUEST_HEADERS,
            NULL, NULL, &hdrSize, WINHTTP_NO_HEADER_INDEX);
        if (hdrSize > 0 && hdrSize < (DWORD)cookie_out_size) {
            wchar_t* whdr = (wchar_t*)malloc(hdrSize);
            if (whdr) {
                if (WinHttpQueryHeaders(hRequest,
                    WINHTTP_QUERY_SET_COOKIE | WINHTTP_QUERY_FLAG_REQUEST_HEADERS,
                    NULL, whdr, &hdrSize, WINHTTP_NO_HEADER_INDEX)) {
                    WideCharToMultiByte(CP_UTF8, 0, whdr, -1, cookie_out, cookie_out_size, NULL, NULL);
                }
                free(whdr);
            }
        }
    }

    DWORD bufSize = 4096;
    buffer = (char*)malloc(bufSize);
    if (!buffer) goto cleanup;

    for (;;) {
        DWORD bytesAvailable = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &bytesAvailable)) break;
        if (bytesAvailable == 0) break;
        if (totalSize + bytesAvailable + 1 > bufSize) {
            bufSize = (totalSize + bytesAvailable + 1) * 2;
            char* newbuf = (char*)realloc(buffer, bufSize);
            if (!newbuf) break;
            buffer = newbuf;
        }
        DWORD bytesRead = 0;
        if (!WinHttpReadData(hRequest, buffer + totalSize, bytesAvailable, &bytesRead)) break;
        totalSize += bytesRead;
    }

    if (buffer && totalSize > 0) {
        buffer[totalSize] = '\0';
        result = buffer;
        buffer = NULL;
    }

cleanup:
    if (buffer) free(buffer);
    if (hRequest) WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    if (hSession) WinHttpCloseHandle(hSession);
    return result;
}

/* HTTP GET binary data, save to file */
static int http_get_to_file(const wchar_t* host, int port, const wchar_t* path,
                            int use_https, const char* local_path) {
    HINTERNET hSession = NULL, hConnect = NULL, hRequest = NULL;
    int ret = -1;
    FILE* fp = NULL;
    DWORD totalBytes = 0;

    hSession = WinHttpOpen(L"YunMusicPlugin/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) goto cleanup;

    hConnect = WinHttpConnect(hSession, host, (INTERNET_PORT)port, 0);
    if (!hConnect) goto cleanup;

    DWORD flags = use_https ? WINHTTP_FLAG_SECURE : 0;
    hRequest = WinHttpOpenRequest(hConnect, L"GET", path,
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) goto cleanup;

    /* Add headers that CDN servers may require */
    WinHttpAddRequestHeaders(hRequest,
        L"Referer: https://music.163.com/\r\n"
        L"Accept: image/*,*/*\r\n",
        (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) goto cleanup;
    if (!WinHttpReceiveResponse(hRequest, NULL)) goto cleanup;

    fp = fopen(local_path, "wb");
    if (!fp) goto cleanup;

    for (;;) {
        DWORD bytesAvailable = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &bytesAvailable)) break;
        if (bytesAvailable == 0) break;

        char buf[4096];
        DWORD bytesRead = 0;
        DWORD toRead = bytesAvailable > sizeof(buf) ? sizeof(buf) : bytesAvailable;
        if (!WinHttpReadData(hRequest, buf, toRead, &bytesRead)) break;
        if (fwrite(buf, 1, bytesRead, fp) != bytesRead) break;
        totalBytes += bytesRead;
    }

    /* Only succeed if we actually downloaded data */
    ret = (totalBytes > 0) ? 0 : -1;

cleanup:
    if (fp) fclose(fp);
    if (hRequest) WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    if (hSession) WinHttpCloseHandle(hSession);
    return ret;
}

void api_netease_init(const char* base_url) {
    parse_url(base_url, g_base_host, sizeof(g_base_host), &g_base_port, &g_use_https);
}

int api_netease_search(const char* keyword, int limit, NeteaseSearchResult* result) {
    if (!keyword || !result) return -1;
    memset(result, 0, sizeof(NeteaseSearchResult));

    char* encoded_kw = url_encode(keyword);
    if (!encoded_kw) return -1;

    wchar_t host[256];
    wchar_t path[1024];
    char path_utf8[1024];

    MultiByteToWideChar(CP_UTF8, 0, g_base_host, -1, host, 256);
    snprintf(path_utf8, sizeof(path_utf8),
        "/search?keywords=%s&limit=%d", encoded_kw, limit);
    free(encoded_kw);

    MultiByteToWideChar(CP_UTF8, 0, path_utf8, -1, path, 1024);

    char* response = http_get(host, g_base_port, path, g_use_https);
    if (!response) return -1;

    cJSON* root = cJSON_Parse(response);
    free(response);
    if (!root) return -1;

    cJSON* data = cJSON_GetObjectItem(root, "result");
    if (!data) { cJSON_Delete(root); return -1; }

    cJSON* songs = cJSON_GetObjectItem(data, "songs");
    if (!songs || !cJSON_IsArray(songs)) { cJSON_Delete(root); return -1; }

    int count = cJSON_GetArraySize(songs);
    if (count <= 0) { cJSON_Delete(root); return 0; }

    result->songs = (NeteaseSong*)malloc(sizeof(NeteaseSong) * count);
    if (!result->songs) { cJSON_Delete(root); return -1; }
    result->count = count;

    for (int i = 0; i < count; i++) {
        cJSON* item = cJSON_GetArrayItem(songs, i);
        NeteaseSong* s = &result->songs[i];
        memset(s, 0, sizeof(NeteaseSong));

        cJSON* id = cJSON_GetObjectItem(item, "id");
        if (id) {
            if (cJSON_IsString(id))
                strncpy(s->id, id->valuestring, sizeof(s->id) - 1);
            else if (cJSON_IsNumber(id))
                snprintf(s->id, sizeof(s->id), "%.0f", id->valuedouble);
        }

        cJSON* name = cJSON_GetObjectItem(item, "name");
        if (name && cJSON_IsString(name))
            strncpy(s->name, name->valuestring, sizeof(s->name) - 1);

        cJSON* artists = cJSON_GetObjectItem(item, "artists");
        if (artists && cJSON_IsArray(artists) && cJSON_GetArraySize(artists) > 0) {
            cJSON* artist = cJSON_GetArrayItem(artists, 0);
            cJSON* artist_name = cJSON_GetObjectItem(artist, "name");
            if (artist_name && cJSON_IsString(artist_name))
                strncpy(s->artist, artist_name->valuestring, sizeof(s->artist) - 1);
        }

        cJSON* album = cJSON_GetObjectItem(item, "album");
        if (album && cJSON_IsObject(album)) {
            cJSON* album_name = cJSON_GetObjectItem(album, "name");
            if (album_name && cJSON_IsString(album_name))
                strncpy(s->album, album_name->valuestring, sizeof(s->album) - 1);
        }

        cJSON* duration = cJSON_GetObjectItem(item, "duration");
        if (duration && cJSON_IsNumber(duration))
            s->duration_ms = duration->valueint;
    }

    cJSON_Delete(root);
    return 0;
}

int api_netease_get_lyrics(const char* song_id, char** lrc_out) {
    if (!song_id || !lrc_out) return -1;
    *lrc_out = NULL;

    wchar_t host[256];
    wchar_t path[512];
    char path_utf8[512];

    MultiByteToWideChar(CP_UTF8, 0, g_base_host, -1, host, 256);
    snprintf(path_utf8, sizeof(path_utf8), "/lyric?id=%s", song_id);
    MultiByteToWideChar(CP_UTF8, 0, path_utf8, -1, path, 512);

    char* response = http_get(host, g_base_port, path, g_use_https);
    if (!response) return -1;

    cJSON* root = cJSON_Parse(response);
    free(response);
    if (!root) return -1;

    cJSON* lrc_obj = cJSON_GetObjectItem(root, "lrc");
    if (!lrc_obj) { cJSON_Delete(root); return -1; }

    cJSON* lyric = cJSON_GetObjectItem(lrc_obj, "lyric");
    if (lyric && cJSON_IsString(lyric)) {
        const char* text = lyric->valuestring;
        size_t len = strlen(text) + 1;
        *lrc_out = (char*)malloc(len);
        if (*lrc_out) memcpy(*lrc_out, text, len);
    }

    cJSON_Delete(root);
    return *lrc_out ? 0 : -1;
}

int api_netease_get_cover_url(const char* song_id, char* url_out, int url_out_size) {
    if (!song_id || !url_out) return -1;
    url_out[0] = '\0';

    wchar_t host[256];
    wchar_t path[512];
    char path_utf8[512];

    MultiByteToWideChar(CP_UTF8, 0, g_base_host, -1, host, 256);
    snprintf(path_utf8, sizeof(path_utf8), "/song/detail?ids=%s", song_id);
    MultiByteToWideChar(CP_UTF8, 0, path_utf8, -1, path, 512);

    char* response = http_get(host, g_base_port, path, g_use_https);
    if (!response) return -1;

    cJSON* root = cJSON_Parse(response);
    free(response);
    if (!root) return -1;

    cJSON* songs = cJSON_GetObjectItem(root, "songs");
    if (songs && cJSON_IsArray(songs) && cJSON_GetArraySize(songs) > 0) {
        cJSON* song = cJSON_GetArrayItem(songs, 0);
        /* Try "al" first (Netease song/detail), then "album" (alternative format) */
        cJSON* al = cJSON_GetObjectItem(song, "al");
        if (!al) al = cJSON_GetObjectItem(song, "album");
        if (al && cJSON_IsObject(al)) {
            cJSON* picUrl = cJSON_GetObjectItem(al, "picUrl");
            if (!picUrl) picUrl = cJSON_GetObjectItem(al, "picurl");
            if (picUrl && cJSON_IsString(picUrl)) {
                strncpy(url_out, picUrl->valuestring, url_out_size - 1);
            }
        }
    }

    cJSON_Delete(root);
    return url_out[0] ? 0 : -1;
}

int api_netease_download_file(const char* url, const char* local_path) {
    if (!url || !local_path) return -1;

    /* Parse the URL to extract host, port, path */
    char host_str[256] = {0};
    char path_str[1024] = {0};
    int port = 80;
    int https = 0;

    const char* p = url;
    if (strncmp(p, "https://", 8) == 0) {
        https = 1; port = 443; p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    }

    const char* slash = strchr(p, '/');
    if (slash) {
        int hlen = (int)(slash - p);
        if (hlen >= (int)sizeof(host_str)) hlen = sizeof(host_str) - 1;
        memcpy(host_str, p, hlen);
        host_str[hlen] = '\0';
        strncpy(path_str, slash, sizeof(path_str) - 1);
    } else {
        strncpy(host_str, p, sizeof(host_str) - 1);
        strcpy(path_str, "/");
    }

    /* Check for port in host */
    const char* colon = strchr(host_str, ':');
    if (colon) {
        port = atoi(colon + 1);
        host_str[colon - host_str] = '\0';
    }

    wchar_t whost[256];
    wchar_t wpath[1024];
    MultiByteToWideChar(CP_UTF8, 0, host_str, -1, whost, 256);
    MultiByteToWideChar(CP_UTF8, 0, path_str, -1, wpath, 1024);

    return http_get_to_file(whost, port, wpath, https, local_path);
}

void api_netease_search_free(NeteaseSearchResult* result) {
    if (result && result->songs) {
        free(result->songs);
        result->songs = NULL;
        result->count = 0;
    }
}

int api_netease_search_playlists(const char* keyword, int limit, NeteasePlaylistResult* result) {
    if (!keyword || !result) return -1;
    memset(result, 0, sizeof(NeteasePlaylistResult));

    char* encoded_kw = url_encode(keyword);
    if (!encoded_kw) return -1;

    wchar_t host[256];
    wchar_t path[1024];
    char path_utf8[1024];

    MultiByteToWideChar(CP_UTF8, 0, g_base_host, -1, host, 256);
    snprintf(path_utf8, sizeof(path_utf8),
        "/search?keywords=%s&type=1000&limit=%d", encoded_kw, limit);
    free(encoded_kw);

    MultiByteToWideChar(CP_UTF8, 0, path_utf8, -1, path, 1024);

    char* response = http_get(host, g_base_port, path, g_use_https);
    if (!response) return -1;

    cJSON* root = cJSON_Parse(response);
    free(response);
    if (!root) return -1;

    cJSON* data = cJSON_GetObjectItem(root, "result");
    if (!data) { cJSON_Delete(root); return -1; }

    cJSON* playlists = cJSON_GetObjectItem(data, "playlists");
    if (!playlists || !cJSON_IsArray(playlists)) { cJSON_Delete(root); return -1; }

    int count = cJSON_GetArraySize(playlists);
    if (count <= 0) { cJSON_Delete(root); return 0; }

    result->playlists = (NeteasePlaylist*)malloc(sizeof(NeteasePlaylist) * count);
    if (!result->playlists) { cJSON_Delete(root); return -1; }
    result->count = count;

    for (int i = 0; i < count; i++) {
        cJSON* item = cJSON_GetArrayItem(playlists, i);
        NeteasePlaylist* p = &result->playlists[i];
        memset(p, 0, sizeof(NeteasePlaylist));

        cJSON* id = cJSON_GetObjectItem(item, "id");
        if (id) {
            if (cJSON_IsString(id))
                strncpy(p->id, id->valuestring, sizeof(p->id) - 1);
            else if (cJSON_IsNumber(id))
                snprintf(p->id, sizeof(p->id), "%.0f", id->valuedouble);
        }

        cJSON* name = cJSON_GetObjectItem(item, "name");
        if (name && cJSON_IsString(name))
            strncpy(p->name, name->valuestring, sizeof(p->name) - 1);

        cJSON* creator = cJSON_GetObjectItem(item, "creator");
        if (creator && cJSON_IsObject(creator)) {
            cJSON* cname = cJSON_GetObjectItem(creator, "nickname");
            if (cname && cJSON_IsString(cname))
                strncpy(p->creator, cname->valuestring, sizeof(p->creator) - 1);
        }

        cJSON* tc = cJSON_GetObjectItem(item, "trackCount");
        if (tc && cJSON_IsNumber(tc)) p->track_count = tc->valueint;

        cJSON* pc = cJSON_GetObjectItem(item, "playCount");
        if (pc && cJSON_IsNumber(pc)) p->play_count = pc->valueint;
    }

    cJSON_Delete(root);
    return 0;
}

void api_netease_search_playlists_free(NeteasePlaylistResult* result) {
    if (result && result->playlists) {
        free(result->playlists);
        result->playlists = NULL;
        result->count = 0;
    }
}

int api_netease_get_playlist_tracks(const char* playlist_id, NeteaseSearchResult* result) {
    if (!playlist_id || !result) return -1;
    memset(result, 0, sizeof(NeteaseSearchResult));

    wchar_t host[256];
    wchar_t path[512];
    char path_utf8[512];

    MultiByteToWideChar(CP_UTF8, 0, g_base_host, -1, host, 256);
    snprintf(path_utf8, sizeof(path_utf8), "/playlist/detail?id=%s", playlist_id);
    MultiByteToWideChar(CP_UTF8, 0, path_utf8, -1, path, 512);

    char* response = http_get(host, g_base_port, path, g_use_https);
    if (!response) return -1;

    cJSON* root = cJSON_Parse(response);
    free(response);
    if (!root) return -1;

    cJSON* playlist = cJSON_GetObjectItem(root, "playlist");
    if (!playlist) { cJSON_Delete(root); return -1; }

    cJSON* tracks = cJSON_GetObjectItem(playlist, "tracks");
    if (!tracks || !cJSON_IsArray(tracks)) { cJSON_Delete(root); return -1; }

    int count = cJSON_GetArraySize(tracks);
    if (count <= 0) { cJSON_Delete(root); return 0; }

    result->songs = (NeteaseSong*)malloc(sizeof(NeteaseSong) * count);
    if (!result->songs) { cJSON_Delete(root); return -1; }
    result->count = count;

    for (int i = 0; i < count; i++) {
        cJSON* item = cJSON_GetArrayItem(tracks, i);
        NeteaseSong* s = &result->songs[i];
        memset(s, 0, sizeof(NeteaseSong));

        cJSON* id = cJSON_GetObjectItem(item, "id");
        if (id) {
            if (cJSON_IsString(id))
                strncpy(s->id, id->valuestring, sizeof(s->id) - 1);
            else if (cJSON_IsNumber(id))
                snprintf(s->id, sizeof(s->id), "%.0f", id->valuedouble);
        }

        cJSON* name = cJSON_GetObjectItem(item, "name");
        if (name && cJSON_IsString(name))
            strncpy(s->name, name->valuestring, sizeof(s->name) - 1);

        /* Artist: try "ar" (standard) then "artists" (alternative) */
        cJSON* ar = cJSON_GetObjectItem(item, "ar");
        if (!ar) ar = cJSON_GetObjectItem(item, "artists");
        if (ar && cJSON_IsArray(ar) && cJSON_GetArraySize(ar) > 0) {
            cJSON* artist = cJSON_GetArrayItem(ar, 0);
            cJSON* artist_name = cJSON_GetObjectItem(artist, "name");
            if (artist_name && cJSON_IsString(artist_name))
                strncpy(s->artist, artist_name->valuestring, sizeof(s->artist) - 1);
        }

        /* Album: try "al" then "album" */
        cJSON* al = cJSON_GetObjectItem(item, "al");
        if (!al) al = cJSON_GetObjectItem(item, "album");
        if (al && cJSON_IsObject(al)) {
            cJSON* al_name = cJSON_GetObjectItem(al, "name");
            if (al_name && cJSON_IsString(al_name))
                strncpy(s->album, al_name->valuestring, sizeof(s->album) - 1);
        }

        cJSON* dt = cJSON_GetObjectItem(item, "dt");
        if (!dt) dt = cJSON_GetObjectItem(item, "duration");
        if (dt && cJSON_IsNumber(dt)) s->duration_ms = dt->valueint;
    }

    cJSON_Delete(root);
    return 0;
}

/* ======================== QR Login + Daily Songs ======================== */

static char g_music_u[512] = {0};

void api_netease_set_cookie(const char* music_u) {
    if (music_u)
        strncpy(g_music_u, music_u, sizeof(g_music_u) - 1);
    else
        g_music_u[0] = '\0';
}

int api_netease_qr_login_key(char* key_out, int key_size) {
    if (!key_out) return -1;
    key_out[0] = '\0';

    wchar_t host[256];
    MultiByteToWideChar(CP_UTF8, 0, g_base_host, -1, host, 256);
    const wchar_t* path = L"/login/qr/key?timestamp=0";

    char* resp = http_get(host, g_base_port, path, g_use_https);
    if (!resp) return -1;

    cJSON* root = cJSON_Parse(resp);
    free(resp);
    if (!root) return -1;

    cJSON* data = cJSON_GetObjectItem(root, "data");
    if (data) {
        cJSON* key = cJSON_GetObjectItem(data, "unikey");
        if (key && cJSON_IsString(key))
            strncpy(key_out, key->valuestring, key_size - 1);
    }
    cJSON_Delete(root);
    return key_out[0] ? 0 : -1;
}

int api_netease_qr_login_create(const char* key, char* qrurl_out, int url_size) {
    if (!key || !qrurl_out) return -1;
    qrurl_out[0] = '\0';

    wchar_t host[256];
    wchar_t path[512];
    char path_utf8[512];
    MultiByteToWideChar(CP_UTF8, 0, g_base_host, -1, host, 256);
    snprintf(path_utf8, sizeof(path_utf8), "/login/qr/create?key=%s&qrimg=true", key);
    MultiByteToWideChar(CP_UTF8, 0, path_utf8, -1, path, 512);

    char* resp = http_get(host, g_base_port, path, g_use_https);
    if (!resp) return -1;

    cJSON* root = cJSON_Parse(resp);
    free(resp);
    if (!root) return -1;

    cJSON* data = cJSON_GetObjectItem(root, "data");
    if (data) {
        cJSON* url = cJSON_GetObjectItem(data, "qrurl");
        if (url && cJSON_IsString(url))
            strncpy(qrurl_out, url->valuestring, url_size - 1);
    }
    cJSON_Delete(root);
    return qrurl_out[0] ? 0 : -1;
}

int api_netease_qr_login_check(const char* key, char* cookie_out, int cookie_size) {
    if (!key) return -1;
    if (cookie_out && cookie_size > 0) cookie_out[0] = '\0';

    wchar_t host[256];
    wchar_t path[512];
    char path_utf8[512];
    MultiByteToWideChar(CP_UTF8, 0, g_base_host, -1, host, 256);
    snprintf(path_utf8, sizeof(path_utf8), "/login/qr/check?key=%s&timestamp=0", key);
    MultiByteToWideChar(CP_UTF8, 0, path_utf8, -1, path, 512);

    char* resp = http_get(host, g_base_port, path, g_use_https);
    if (!resp) return -1;

    cJSON* root = cJSON_Parse(resp);
    free(resp);
    if (!root) return -1;

    int code = -1;
    cJSON* sc = cJSON_GetObjectItem(root, "code");
    if (sc && cJSON_IsNumber(sc)) {
        int c = sc->valueint;
        if (c == 803) code = 0;       /* Login success */
        else if (c == 801) code = 1;  /* Waiting for scan */
        else if (c == 802) code = 2;  /* Scanned, waiting confirm */
        else code = -1;               /* Error/expired */
    }

    /* Extract MUSIC_U from JSON body "cookie" field */
    if (code == 0 && cookie_out && cookie_size > 0) {
        cJSON* ck = cJSON_GetObjectItem(root, "cookie");
        if (ck && cJSON_IsString(ck)) {
            /* cookie field contains full cookie string like "MUSIC_U=xxx; ..." */
            const char* mu = strstr(ck->valuestring, "MUSIC_U=");
            if (mu) {
                mu += 8;
                const char* end = strchr(mu, ';');
                int len = end ? (int)(end - mu) : (int)strlen(mu);
                if (len > 0 && len < cookie_size) {
                    memcpy(cookie_out, mu, len);
                    cookie_out[len] = '\0';
                }
            }
        }
    }

    cJSON_Delete(root);
    return code;
}

int api_netease_get_daily_songs(NeteaseSearchResult* result) {
    if (!result) return -1;
    memset(result, 0, sizeof(NeteaseSearchResult));

    wchar_t host[256];
    MultiByteToWideChar(CP_UTF8, 0, g_base_host, -1, host, 256);
    const wchar_t* path = L"/recommend/songs";

    char* resp;
    if (g_music_u[0]) {
        char cookie[600];
        snprintf(cookie, sizeof(cookie), "MUSIC_U=%s", g_music_u);
        /* Debug log */
        {
            char logpath[MAX_PATH] = {0};
            GetTempPathA(MAX_PATH, logpath);
            strcat(logpath, "yunmusic_debug.log");
            FILE* f = fopen(logpath, "a");
            if (f) { fprintf(f, "[daily] using cookie: %.60s...\n", cookie); fclose(f); }
        }
        resp = http_get_with_cookie(host, g_base_port, path, g_use_https, cookie, NULL, 0);
    } else {
        /* Debug log */
        {
            char logpath[MAX_PATH] = {0};
            GetTempPathA(MAX_PATH, logpath);
            strcat(logpath, "yunmusic_debug.log");
            FILE* f = fopen(logpath, "a");
            if (f) { fprintf(f, "[daily] NO cookie, using server session\n"); fclose(f); }
        }
        resp = http_get(host, g_base_port, path, g_use_https);
    }

    /* Debug log response */
    if (resp) {
        char logpath[MAX_PATH] = {0};
        GetTempPathA(MAX_PATH, logpath);
        strcat(logpath, "yunmusic_debug.log");
        FILE* f = fopen(logpath, "a");
        if (f) { fprintf(f, "[daily] response: %.200s\n", resp); fclose(f); }
    }
    if (!resp) return -1;

    cJSON* root = cJSON_Parse(resp);
    free(resp);
    if (!root) return -1;

    cJSON* data = cJSON_GetObjectItem(root, "data");
    if (!data) { cJSON_Delete(root); return -1; }

    cJSON* songs = cJSON_GetObjectItem(data, "dailySongs");
    if (!songs || !cJSON_IsArray(songs)) { cJSON_Delete(root); return -1; }

    int count = cJSON_GetArraySize(songs);
    if (count <= 0) { cJSON_Delete(root); return 0; }

    result->songs = (NeteaseSong*)malloc(sizeof(NeteaseSong) * count);
    if (!result->songs) { cJSON_Delete(root); return -1; }
    result->count = count;

    for (int i = 0; i < count; i++) {
        cJSON* item = cJSON_GetArrayItem(songs, i);
        NeteaseSong* s = &result->songs[i];
        memset(s, 0, sizeof(NeteaseSong));

        cJSON* id = cJSON_GetObjectItem(item, "id");
        if (id) {
            if (cJSON_IsString(id))
                strncpy(s->id, id->valuestring, sizeof(s->id) - 1);
            else if (cJSON_IsNumber(id))
                snprintf(s->id, sizeof(s->id), "%.0f", id->valuedouble);
        }

        cJSON* name = cJSON_GetObjectItem(item, "name");
        if (name && cJSON_IsString(name))
            strncpy(s->name, name->valuestring, sizeof(s->name) - 1);

        cJSON* ar = cJSON_GetObjectItem(item, "ar");
        if (!ar) ar = cJSON_GetObjectItem(item, "artists");
        if (ar && cJSON_IsArray(ar) && cJSON_GetArraySize(ar) > 0) {
            cJSON* artist = cJSON_GetArrayItem(ar, 0);
            cJSON* aname = cJSON_GetObjectItem(artist, "name");
            if (aname && cJSON_IsString(aname))
                strncpy(s->artist, aname->valuestring, sizeof(s->artist) - 1);
        }

        cJSON* dt = cJSON_GetObjectItem(item, "dt");
        if (!dt) dt = cJSON_GetObjectItem(item, "duration");
        if (dt && cJSON_IsNumber(dt)) s->duration_ms = dt->valueint;
    }

    cJSON_Delete(root);
    return 0;
}
