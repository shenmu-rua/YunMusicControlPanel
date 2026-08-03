#include "cookie_jar.h"

#include <string.h>

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

int main(void) {
    char jar[4096] = {0};
    CHECK(cookie_jar_merge(jar, sizeof(jar),
        "NMTID=anonymous; Max-Age=315360000; Expires=Thu, 31 Jul 2036 07:46:30 GMT; Path=/;") == 0);
    CHECK(strcmp(jar, "NMTID=anonymous") == 0);

    CHECK(cookie_jar_merge(jar, sizeof(jar),
        "MUSIC_U=personal-session; Max-Age=1296000; Path=/;; "
        "__csrf=csrf-value; Path=/; HttpOnly; SameSite=Lax") == 0);
    CHECK(cookie_jar_has(jar, "NMTID"));
    CHECK(cookie_jar_has(jar, "MUSIC_U"));
    CHECK(cookie_jar_has(jar, "__csrf"));
    CHECK(strstr(jar, "Max-Age") == NULL);
    CHECK(strstr(jar, "Path") == NULL);

    CHECK(cookie_jar_merge(jar, sizeof(jar), "NMTID=replaced; Path=/") == 0);
    CHECK(strstr(jar, "NMTID=replaced") != NULL);
    CHECK(strstr(jar, "NMTID=anonymous") == NULL);
    return 0;
}
