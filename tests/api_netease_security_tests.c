#include "api_netease.h"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

int main(void) {
    api_netease_init("https://music-api.example.test");
    CHECK(api_netease_auth_supported() == 1);

    api_netease_init("https://music-api.example.test");
    CHECK(api_netease_auth_supported() == 1);
    api_netease_init("");
    CHECK(api_netease_auth_supported() == 0);
    return 0;
}
