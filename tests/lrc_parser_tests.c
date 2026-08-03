#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lrc_parser.h"

static void test_basic_parse_and_lookup(void) {
    const char* input =
        "[00:01.25]first line\n"
        "[00:03.500]second line\r\n";
    LyricData data = {0};

    assert(lrc_parse(&data, input) == 0);
    assert(data.count == 2);
    assert(data.lines[0].timestamp_ms == 1250);
    assert(data.lines[1].timestamp_ms == 3500);
    assert(strcmp(data.lines[0].text, "first line") == 0);
    assert(lrc_find_line(&data, 1249) == -1);
    assert(lrc_find_line(&data, 1250) == 0);
    assert(lrc_find_line(&data, 4000) == 1);

    lrc_free(&data);
}

static void test_long_line_is_safely_truncated(void) {
    enum { LONG_TEXT_LENGTH = 600 };
    const char* prefix = "[00:00.00]";
    size_t input_size = strlen(prefix) + LONG_TEXT_LENGTH + 2;
    char* input = (char*)malloc(input_size);
    LyricData data = {0};

    assert(input != NULL);
    memcpy(input, prefix, strlen(prefix));
    memset(input + strlen(prefix), 'x', LONG_TEXT_LENGTH);
    input[strlen(prefix) + LONG_TEXT_LENGTH] = '\n';
    input[strlen(prefix) + LONG_TEXT_LENGTH + 1] = '\0';

    assert(lrc_parse(&data, input) == 0);
    assert(data.count == 1);
    assert(strlen(data.lines[0].text) == sizeof(data.lines[0].text) - 1);
    assert(data.lines[0].text[sizeof(data.lines[0].text) - 1] == '\0');

    lrc_free(&data);
    free(input);
}

int main(void) {
    test_basic_parse_and_lookup();
    test_long_line_is_safely_truncated();
    puts("lrc_parser_tests: OK");
    return 0;
}
