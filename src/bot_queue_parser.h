#ifndef BOT_QUEUE_PARSER_H
#define BOT_QUEUE_PARSER_H

#define BOT_QUEUE_PREVIEW_MAX 3

typedef struct {
    char song_id[64];
    char title[256];
} BotQueuePreviewItem;

typedef struct {
    int synced;
    int count;
    int total;
    BotQueuePreviewItem items[BOT_QUEUE_PREVIEW_MAX];
} BotQueuePreview;

/* Parse the human-readable Value returned by the Bot's `yun list` command. */
int bot_queue_parse(const char* value, BotQueuePreview* preview);

#endif
