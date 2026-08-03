#include "cookie_jar.h"

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

static int is_attribute(const char* name) {
    static const char* attributes[] = {
        "domain", "path", "expires", "max-age", "samesite",
        "secure", "httponly", "priority", "partitioned"
    };
    for (size_t i = 0; i < sizeof(attributes) / sizeof(attributes[0]); i++) {
        if (_stricmp(name, attributes[i]) == 0) return 1;
    }
    return 0;
}

static int append_pair(char* output, size_t output_size,
                       const char* name, const char* value) {
    size_t used = strlen(output);
    int written = snprintf(output + used, output_size - used, "%s%s=%s",
        used ? "; " : "", name, value);
    return written > 0 && (size_t)written < output_size - used ? 0 : -1;
}

static int set_pair(char* jar, size_t jar_size,
                    const char* name, const char* value) {
    char* copy = (char*)malloc(jar_size);
    char* rebuilt = (char*)calloc(jar_size, 1);
    if (!copy || !rebuilt) {
        free(copy);
        free(rebuilt);
        return -1;
    }
    strncpy(copy, jar, jar_size - 1);
    copy[jar_size - 1] = '\0';

    char* context = NULL;
    for (char* token = strtok_s(copy, ";", &context); token;
         token = strtok_s(NULL, ";", &context)) {
        char* pair = trim(token);
        char* equals = strchr(pair, '=');
        if (!equals) continue;
        *equals = '\0';
        char* old_name = trim(pair);
        char* old_value = trim(equals + 1);
        if (!old_name[0] || _stricmp(old_name, name) == 0) continue;
        if (append_pair(rebuilt, jar_size, old_name, old_value) != 0) {
            free(copy);
            free(rebuilt);
            return -1;
        }
    }
    if (append_pair(rebuilt, jar_size, name, value) != 0) {
        free(copy);
        free(rebuilt);
        return -1;
    }
    memcpy(jar, rebuilt, strlen(rebuilt) + 1);
    free(copy);
    free(rebuilt);
    return 0;
}

int cookie_jar_merge(char* jar, size_t jar_size, const char* set_cookie_text) {
    if (!jar || jar_size == 0 || !set_cookie_text) return -1;
    char* copy = (char*)malloc(strlen(set_cookie_text) + 1);
    if (!copy) return -1;
    strcpy(copy, set_cookie_text);

    int merged = 0;
    char* context = NULL;
    for (char* token = strtok_s(copy, ";", &context); token;
         token = strtok_s(NULL, ";", &context)) {
        char* pair = trim(token);
        char* equals = strchr(pair, '=');
        if (!equals) continue;
        *equals = '\0';
        char* name = trim(pair);
        char* value = trim(equals + 1);
        if (!name[0] || !value[0] || is_attribute(name)) continue;
        if (set_pair(jar, jar_size, name, value) != 0) {
            free(copy);
            return -1;
        }
        merged = 1;
    }
    free(copy);
    return merged ? 0 : -1;
}

int cookie_jar_has(const char* jar, const char* cookie_name) {
    if (!jar || !cookie_name || !cookie_name[0]) return 0;
    size_t name_length = strlen(cookie_name);
    const char* cursor = jar;
    while (*cursor) {
        while (*cursor == ';' || isspace((unsigned char)*cursor)) cursor++;
        if (_strnicmp(cursor, cookie_name, name_length) == 0 &&
            cursor[name_length] == '=') return 1;
        const char* next = strchr(cursor, ';');
        if (!next) break;
        cursor = next + 1;
    }
    return 0;
}
