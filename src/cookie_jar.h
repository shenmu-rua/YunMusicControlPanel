#ifndef COOKIE_JAR_H
#define COOKIE_JAR_H

#include <stddef.h>

/* Merge Set-Cookie style text into a normalized Cookie request value. */
int cookie_jar_merge(char* jar, size_t jar_size, const char* set_cookie_text);
int cookie_jar_has(const char* jar, const char* cookie_name);

#endif
