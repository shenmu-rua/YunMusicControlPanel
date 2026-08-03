#ifndef AUTH_STORE_H
#define AUTH_STORE_H

#include <stddef.h>

int auth_store_init(const char* plugin_path);
int auth_store_save(const char* cookie);
int auth_store_load(char* cookie_out, size_t cookie_size);
int auth_store_delete(void);

#endif
