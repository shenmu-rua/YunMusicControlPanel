#include "daily_policy.h"

#include <string.h>

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

int main(void) {
    CHECK(daily_queue_limit(-1) == 0);
    CHECK(daily_queue_limit(0) == 0);
    CHECK(daily_queue_limit(6) == 6);
    CHECK(daily_queue_limit(15) == 15);
    CHECK(daily_queue_limit(30) == 15);

    CHECK(daily_queue_send_index(0, 15, 0) == 14);
    CHECK(daily_queue_send_index(14, 15, 0) == 0);
    CHECK(daily_queue_send_index(15, 15, 0) == -1);
    CHECK(daily_queue_send_index(0, 15, 1) == 14);
    CHECK(daily_queue_send_index(13, 15, 1) == 1);
    CHECK(daily_queue_send_index(14, 15, 1) == -1);

    char command[256];
    CHECK(daily_build_add_command("12345", command, sizeof(command)) == 0);
    CHECK(strcmp(command,
        "(/yun/add/https%3A%2F%2Fmusic.163.com%2F%23%2Fsong%3Fid%3D12345)") == 0);
    CHECK(strstr(command, "/yun/play/") == NULL);
    CHECK(strstr(command, "clear") == NULL);
    CHECK(daily_build_add_command("", command, sizeof(command)) != 0);
    CHECK(daily_build_add_command("12345", command, 4) != 0);
    return 0;
}
