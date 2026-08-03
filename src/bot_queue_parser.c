#include "bot_queue_parser.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char* trim(char* text) {
    while (*text && isspace((unsigned char)*text)) text++;
    char* end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) *--end = '\0';
    return text;
}

static void strip_bbcode(const char* input, char* output, size_t output_size) {
    size_t used = 0;
    int in_tag = 0;
    for (const char* cursor = input; *cursor && used + 1 < output_size;
         cursor++) {
        if (*cursor == '[') {
            in_tag = 1;
            continue;
        }
        if (in_tag) {
            if (*cursor == ']') in_tag = 0;
            continue;
        }
        output[used++] = *cursor;
    }
    output[used] = '\0';
}

static void extract_song_id(const char* line, char* output, size_t output_size) {
    output[0] = '\0';
    const char* id = strstr(line, "id=");
    if (!id) return;
    id += 3;
    size_t used = 0;
    while (id[used] && id[used] != '&' && id[used] != ']' &&
           id[used] != '/' && !isspace((unsigned char)id[used]) &&
           used + 1 < output_size) {
        output[used] = id[used];
        used++;
    }
    output[used] = '\0';
}

int bot_queue_parse(const char* value, BotQueuePreview* preview) {
    if (!value || !preview) return -1;
    memset(preview, 0, sizeof(*preview));
    if (strstr(value, "播放列表为空")) {
        preview->synced = 1;
        return 0;
    }
    if (!strstr(value, "播放列表")) return -1;

    for (const char* cursor = value; (cursor = strchr(cursor, '[')) != NULL;
         cursor++) {
        int current = 0;
        int total = 0;
        if (sscanf(cursor, "[%d/%d]", &current, &total) == 2 && total >= 0) {
            preview->total = total;
        }
    }

    char* copy = (char*)malloc(strlen(value) + 1);
    if (!copy) return -1;
    strcpy(copy, value);
    char* context = NULL;
    for (char* line = strtok_s(copy, "\r\n", &context); line;
         line = strtok_s(NULL, "\r\n", &context)) {
        char* clean_line = trim(line);
        char* colon = strchr(clean_line, ':');
        if (!colon) continue;
        *colon = '\0';
        char* end = NULL;
        long number = strtol(clean_line, &end, 10);
        if (number <= 0 || !end || *trim(end) != '\0') continue;
        if (preview->count >= BOT_QUEUE_PREVIEW_MAX) continue;

        BotQueuePreviewItem* item = &preview->items[preview->count];
        char* description = trim(colon + 1);
        extract_song_id(description, item->song_id, sizeof(item->song_id));
        strip_bbcode(description, item->title, sizeof(item->title));
        char* title = trim(item->title);
        if (title != item->title)
            memmove(item->title, title, strlen(title) + 1);
        if (item->title[0]) preview->count++;
    }
    free(copy);
    if (preview->total < preview->count) preview->total = preview->count;
    preview->synced = 1;
    return 0;
}
