#ifndef DAILY_POLICY_H
#define DAILY_POLICY_H

#include <stddef.h>

/* Daily recommendations are appended to the shared Bot queue in this limit. */
int daily_queue_limit(int song_count);

/* Map serial send number to source index for a LIFO Bot queue. */
int daily_queue_send_index(int send_number, int song_count,
                           int first_song_is_playing);

/* Build the existing Bot queue command without exposing the user's cookie. */
int daily_build_add_command(const char* song_id, char* buffer,
                            size_t buffer_size);

#endif
