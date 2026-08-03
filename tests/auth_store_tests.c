#include "auth_store.h"

#include <windows.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

int main(void) {
    char temp_path[MAX_PATH];
    char test_dir[MAX_PATH];
    DWORD length = GetTempPathA(MAX_PATH, temp_path);
    CHECK(length > 0 && length < MAX_PATH);
    wsprintfA(test_dir, "%syunmusic_auth_test_%lu",
        temp_path, (unsigned long)GetCurrentProcessId());
    CreateDirectoryA(test_dir, NULL);

    CHECK(auth_store_init(test_dir) == 0);
    auth_store_delete();

    const char* cookie = "MUSIC_U=test-secret; __csrf=test-csrf";
    CHECK(auth_store_save(cookie) == 0);
    char loaded[4096] = {0};
    CHECK(auth_store_load(loaded, sizeof(loaded)) == 0);
    CHECK(strcmp(cookie, loaded) == 0);

    CHECK(auth_store_delete() == 0);
    CHECK(auth_store_load(loaded, sizeof(loaded)) != 0);

    char auth_path[MAX_PATH];
    wsprintfA(auth_path, "%s\\yunmusic.auth", test_dir);
    HANDLE damaged = CreateFileA(auth_path, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(damaged != INVALID_HANDLE_VALUE);
    DWORD written = 0;
    const char invalid[] = "not-an-auth-file";
    CHECK(WriteFile(damaged, invalid, sizeof(invalid), &written, NULL));
    CloseHandle(damaged);
    memset(loaded, 'x', sizeof(loaded));
    CHECK(auth_store_load(loaded, sizeof(loaded)) != 0);
    CHECK(loaded[0] == '\0');
    CHECK(auth_store_delete() == 0);

    RemoveDirectoryA(test_dir);
    SecureZeroMemory(loaded, sizeof(loaded));
    return 0;
}
