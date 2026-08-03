#include <windows.h>
#include <wincrypt.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "auth_store.h"

#define AUTH_MAGIC "YMA1"
#define AUTH_VERSION 1U
#define AUTH_MAX_BLOB (64U * 1024U)

typedef struct {
    char magic[4];
    uint32_t version;
    uint32_t blob_size;
} AuthFileHeader;

static char g_auth_path[MAX_PATH] = {0};

int auth_store_init(const char* plugin_path) {
    if (!plugin_path || !plugin_path[0]) return -1;
    int written = snprintf(g_auth_path, sizeof(g_auth_path),
        "%s\\yunmusic.auth", plugin_path);
    if (written < 0 || (size_t)written >= sizeof(g_auth_path)) {
        g_auth_path[0] = '\0';
        return -1;
    }
    return 0;
}

static int write_all(HANDLE file, const void* data, DWORD size) {
    const BYTE* cursor = (const BYTE*)data;
    while (size > 0) {
        DWORD written = 0;
        if (!WriteFile(file, cursor, size, &written, NULL) || written == 0)
            return -1;
        cursor += written;
        size -= written;
    }
    return 0;
}

int auth_store_save(const char* cookie) {
    if (!g_auth_path[0] || !cookie || !cookie[0]) return -1;

    DATA_BLOB plain = {(DWORD)(strlen(cookie) + 1), (BYTE*)cookie};
    DATA_BLOB encrypted = {0};
    if (!CryptProtectData(&plain, L"YunMusic NetEase session", NULL, NULL,
            NULL, CRYPTPROTECT_UI_FORBIDDEN, &encrypted)) {
        return -1;
    }
    if (encrypted.cbData == 0 || encrypted.cbData > AUTH_MAX_BLOB) {
        LocalFree(encrypted.pbData);
        return -1;
    }

    char temp_path[MAX_PATH + 8];
    int temp_written = snprintf(temp_path, sizeof(temp_path), "%s.tmp", g_auth_path);
    if (temp_written < 0 || (size_t)temp_written >= sizeof(temp_path)) {
        LocalFree(encrypted.pbData);
        return -1;
    }

    AuthFileHeader header = {{'Y','M','A','1'}, AUTH_VERSION, encrypted.cbData};
    HANDLE file = CreateFileA(temp_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
        FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, NULL);
    int result = -1;
    if (file != INVALID_HANDLE_VALUE) {
        if (write_all(file, &header, sizeof(header)) == 0 &&
            write_all(file, encrypted.pbData, encrypted.cbData) == 0 &&
            FlushFileBuffers(file)) {
            result = 0;
        }
        CloseHandle(file);
    }
    SecureZeroMemory(encrypted.pbData, encrypted.cbData);
    LocalFree(encrypted.pbData);

    if (result == 0 && MoveFileExA(temp_path, g_auth_path,
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return 0;
    }
    DeleteFileA(temp_path);
    return -1;
}

int auth_store_load(char* cookie_out, size_t cookie_size) {
    if (!cookie_out || cookie_size == 0) return -1;
    cookie_out[0] = '\0';
    if (!g_auth_path[0]) return -1;

    HANDLE file = CreateFileA(g_auth_path, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return -1;

    AuthFileHeader header;
    DWORD read = 0;
    int result = -1;
    BYTE* encrypted = NULL;
    if (!ReadFile(file, &header, sizeof(header), &read, NULL) ||
        read != sizeof(header) || memcmp(header.magic, AUTH_MAGIC, 4) != 0 ||
        header.version != AUTH_VERSION || header.blob_size == 0 ||
        header.blob_size > AUTH_MAX_BLOB) {
        goto cleanup;
    }

    encrypted = (BYTE*)LocalAlloc(LMEM_FIXED, header.blob_size);
    if (!encrypted) goto cleanup;
    if (!ReadFile(file, encrypted, header.blob_size, &read, NULL) ||
        read != header.blob_size) goto cleanup;

    DATA_BLOB protected_blob = {header.blob_size, encrypted};
    DATA_BLOB plain = {0};
    if (!CryptUnprotectData(&protected_blob, NULL, NULL, NULL, NULL,
            CRYPTPROTECT_UI_FORBIDDEN, &plain)) goto cleanup;
    if (plain.cbData > 0 && plain.cbData <= cookie_size &&
        plain.pbData[plain.cbData - 1] == '\0') {
        memcpy(cookie_out, plain.pbData, plain.cbData);
        result = cookie_out[0] ? 0 : -1;
    }
    SecureZeroMemory(plain.pbData, plain.cbData);
    LocalFree(plain.pbData);

cleanup:
    if (encrypted) {
        SecureZeroMemory(encrypted, header.blob_size);
        LocalFree(encrypted);
    }
    CloseHandle(file);
    if (result != 0) cookie_out[0] = '\0';
    return result;
}

int auth_store_delete(void) {
    if (!g_auth_path[0]) return -1;
    if (DeleteFileA(g_auth_path) || GetLastError() == ERROR_FILE_NOT_FOUND)
        return 0;
    return -1;
}
