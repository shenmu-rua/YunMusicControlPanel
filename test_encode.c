#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#pragma comment(lib, "winhttp.lib")

static char* url_encode(const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char* out = (char*)malloc(len * 3 + 1);
    if (!out) return NULL;
    char* p = out;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)str[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            *p++ = (char)c;
        } else {
            snprintf(p, 4, "%%%02X", c);
            p += 3;
        }
    }
    *p = '\0';
    return out;
}

int main(void) {
    /* Simulate: user types "周杰伦" in edit box */
    wchar_t wkeyword[] = {0x5468, 0x6770, 0x4F26, 0}; /* 周杰伦 */

    char utf8[256] = {0};
    WideCharToMultiByte(CP_UTF8, 0, wkeyword, -1, utf8, sizeof(utf8), NULL, NULL);

    printf("Step 1 - UTF-8 hex:");
    for (int i = 0; utf8[i]; i++) printf(" %02x", (unsigned char)utf8[i]);
    printf("\n");

    char* encoded = url_encode(utf8);
    printf("Step 2 - URL encoded: %s\n", encoded);
    printf("Expected:            %%E5%%91%%A8%%E6%%9D%%B0%%E4%%BC%%A6\n");

    /* Build the full path */
    char path_utf8[1024];
    snprintf(path_utf8, sizeof(path_utf8), "/search?keywords=%s&limit=5", encoded);
    printf("Step 3 - Full path: %s\n", path_utf8);

    /* Convert to wchar for WinHTTP */
    wchar_t wpath[1024];
    MultiByteToWideChar(CP_UTF8, 0, path_utf8, -1, wpath, 1024);

    /* Make the actual HTTP request */
    HINTERNET hSession = WinHttpOpen(L"Test/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET hConnect = WinHttpConnect(hSession, L"localhost", 3000, 0);
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", wpath,
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);

    printf("Step 4 - Sending request...\n");

    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, NULL)) {
        char buf[4096] = {0};
        DWORD bytesRead = 0;
        WinHttpReadData(hRequest, buf, sizeof(buf) - 1, &bytesRead);
        buf[bytesRead] = '\0';

        /* Find first song name */
        char* name_ptr = strstr(buf, "\"name\":\"");
        if (name_ptr) {
            name_ptr += 8;
            char* end = strchr(name_ptr, '"');
            if (end) {
                *end = '\0';
                printf("Step 5 - First song name (UTF-8): %s\n", name_ptr);

                /* Convert to wchar for display */
                wchar_t wname[256];
                MultiByteToWideChar(CP_UTF8, 0, name_ptr, -1, wname, 256);
                wprintf(L"Step 6 - Display: %s\n", wname);
            }
        } else {
            printf("No results found. Response: %.200s\n", buf);
        }
    } else {
        printf("HTTP request failed!\n");
    }

    if (hRequest) WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    if (hSession) WinHttpCloseHandle(hSession);
    free(encoded);

    return 0;
}
