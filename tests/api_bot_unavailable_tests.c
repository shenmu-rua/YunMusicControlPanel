#include <windows.h>
#include <stdio.h>

#include "api_bot.h"

int main(void) {
    BotStatus status;
    ULONGLONG started;
    ULONGLONG elapsed;

    /* The reserved .test domain must not resolve to a live Bot service. */
    api_bot_init("http://unavailable.example.test:1");
    started = GetTickCount64();
    if (api_bot_poll_status(&status) == 0) {
        fprintf(stderr, "unexpected Bot response on closed test port\n");
        api_bot_cleanup();
        return 1;
    }
    elapsed = GetTickCount64() - started;
    if (elapsed > 4000) {
        fprintf(stderr, "unavailable Bot request took too long: %llu ms\n",
            (unsigned long long)elapsed);
        api_bot_cleanup();
        return 1;
    }

    /* Shutdown cancellation must make queued/new requests fail immediately. */
    api_bot_cancel_pending();
    started = GetTickCount64();
    if (api_bot_poll_status(&status) == 0) {
        fprintf(stderr, "request succeeded after cancellation\n");
        api_bot_cleanup();
        return 1;
    }
    elapsed = GetTickCount64() - started;
    if (elapsed > 100) {
        fprintf(stderr, "cancelled Bot request was not rejected immediately: %llu ms\n",
            (unsigned long long)elapsed);
        api_bot_cleanup();
        return 1;
    }

    api_bot_cleanup();
    printf("api_bot_unavailable_tests: OK\n");
    return 0;
}
