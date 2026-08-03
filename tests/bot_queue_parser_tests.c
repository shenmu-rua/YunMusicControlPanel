#include "bot_queue_parser.h"

#include <string.h>

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

int main(void) {
    BotQueuePreview preview;
    CHECK(bot_queue_parse("播放列表为空", &preview) == 0);
    CHECK(preview.synced == 1 && preview.count == 0 && preview.total == 0);

    const char* list =
        "\n当前正在播放：[URL=https://music.163.com/#/song?id=100]当前歌曲[/URL]\n"
        "当前播放模式：顺序播放\n播放列表 [1/8]\n"
        "1: [URL=https://music.163.com/#/song?id=101]第一首[/URL] - 歌手甲\n"
        "2: [URL=https://music.163.com/#/song?id=102]第二首[/URL] - 歌手乙\n"
        "3: [URL=https://music.163.com/#/song?id=103]第三首[/URL] - 歌手丙\n";
    CHECK(bot_queue_parse(list, &preview) == 0);
    CHECK(preview.synced == 1);
    CHECK(preview.count == 3);
    CHECK(preview.total == 8);
    CHECK(strcmp(preview.items[0].song_id, "101") == 0);
    CHECK(strcmp(preview.items[0].title, "第一首 - 歌手甲") == 0);
    CHECK(strcmp(preview.items[2].song_id, "103") == 0);
    CHECK(bot_queue_parse("unexpected response", &preview) != 0);
    return 0;
}
