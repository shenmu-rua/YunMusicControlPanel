/*
 * lrc_parser.c - LRC lyric format parser
 * Parses standard LRC format: [mm:ss.xx]text
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lrc_parser.h"

#define INITIAL_CAPACITY 64

/*
 * parse_timestamp - Parse [mm:ss.xx] timestamp to milliseconds
 * Returns pointer past the closing bracket, or NULL on failure
 */
static const char* parse_timestamp(const char* p, int* out_ms) {
    if (*p != '[') return NULL;
    p++;

    int minutes = 0;
    while (*p >= '0' && *p <= '9') {
        minutes = minutes * 10 + (*p - '0');
        p++;
    }

    if (*p != ':') return NULL;
    p++;

    int seconds = 0;
    while (*p >= '0' && *p <= '9') {
        seconds = seconds * 10 + (*p - '0');
        p++;
    }

    int centiseconds = 0;
    if (*p == '.' || *p == ':') {
        p++;
        int digits = 0;
        while (*p >= '0' && *p <= '9' && digits < 3) {
            centiseconds = centiseconds * 10 + (*p - '0');
            p++;
            digits++;
        }
        /* Normalize to milliseconds (e.g. .34 -> 340ms, .345 -> 345ms) */
        while (digits < 3) {
            centiseconds *= 10;
            digits++;
        }
    }

    if (*p != ']') return NULL;
    p++;

    *out_ms = minutes * 60000 + seconds * 1000 + centiseconds;
    return p;
}

int lrc_parse(LyricData* data, const char* lrc_text) {
    if (!data || !lrc_text) return -1;

    data->lines = (LyricLine*)malloc(sizeof(LyricLine) * INITIAL_CAPACITY);
    if (!data->lines) return -1;
    data->count = 0;
    data->capacity = INITIAL_CAPACITY;

    const char* p = lrc_text;

    while (*p) {
        /* Skip whitespace */
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;

        /* Try to parse timestamp */
        int ts_ms = 0;
        const char* text_start = parse_timestamp(p, &ts_ms);
        if (text_start) {
            /* Extract text until newline */
            const char* line_end = text_start;
            while (*line_end && *line_end != '\n' && *line_end != '\r')
                line_end++;

            /* Trim trailing whitespace */
            const char* text_end = line_end;
            while (text_end > text_start && (*(text_end - 1) == ' ' || *(text_end - 1) == '\t'))
                text_end--;

            int text_len = (int)(text_end - text_start);
            if (text_len > 0) {
                /* Grow array if needed */
                if (data->count >= data->capacity) {
                    data->capacity *= 2;
                    LyricLine* new_lines = (LyricLine*)realloc(data->lines,
                        sizeof(LyricLine) * data->capacity);
                    if (!new_lines) break;
                    data->lines = new_lines;
                }

                data->lines[data->count].timestamp_ms = ts_ms;
                strncpy(data->lines[data->count].text, text_start, text_len);
                data->lines[data->count].text[text_len] = '\0';
                data->count++;
            }

            p = line_end;
        } else {
            /* Skip non-timestamp line (metadata like [ti:], [ar:], etc.) */
            while (*p && *p != '\n') p++;
        }

        /* Skip newline */
        while (*p == '\n' || *p == '\r') p++;
    }

    return 0;
}

int lrc_find_line(const LyricData* data, int position_ms) {
    if (!data || data->count == 0) return -1;

    /* Binary search for the current line */
    int lo = 0, hi = data->count - 1;
    int result = -1;

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (data->lines[mid].timestamp_ms <= position_ms) {
            result = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }

    return result;
}

void lrc_free(LyricData* data) {
    if (data && data->lines) {
        free(data->lines);
        data->lines = NULL;
        data->count = 0;
        data->capacity = 0;
    }
}
