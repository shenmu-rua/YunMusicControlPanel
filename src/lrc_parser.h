/*
 * lrc_parser.h - LRC lyric format parser
 */

#ifndef LRC_PARSER_H
#define LRC_PARSER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Single lyric line */
typedef struct {
    int timestamp_ms;   /* Timestamp in milliseconds */
    char text[256];     /* Lyric text */
} LyricLine;

/* Lyric collection */
typedef struct {
    LyricLine* lines;   /* Array of lyric lines */
    int        count;   /* Number of lines */
    int        capacity;/* Allocated capacity */
} LyricData;

/*
 * lrc_parse - Parse LRC format text into LyricData
 * lrc_text: raw LRC string (e.g. "[00:12.34]Hello\n[00:16.78]World")
 * Returns 0 on success, -1 on error
 */
int lrc_parse(LyricData* data, const char* lrc_text);

/*
 * lrc_find_line - Find the current lyric line index for a given position
 * data: parsed lyric data
 * position_ms: current playback position in milliseconds
 * Returns index of current line, or -1 if no match
 */
int lrc_find_line(const LyricData* data, int position_ms);

/*
 * lrc_free - Free resources used by LyricData
 */
void lrc_free(LyricData* data);

#ifdef __cplusplus
}
#endif

#endif /* LRC_PARSER_H */
