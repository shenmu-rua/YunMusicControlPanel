#include "daily_policy.h"

#include <stdio.h>

int daily_queue_limit(int song_count) {
    if (song_count <= 0) return 0;
    return song_count > 15 ? 15 : song_count;
}

int daily_queue_send_index(int send_number, int song_count,
                           int first_song_is_playing) {
    if (send_number < 0 || song_count <= 0) return -1;
    int index = song_count - 1 - send_number;
    int final_index = first_song_is_playing ? 1 : 0;
    return index >= final_index ? index : -1;
}

int daily_build_add_command(const char* song_id, char* buffer,
                            size_t buffer_size) {
    if (!song_id || !song_id[0] || !buffer || buffer_size == 0) return -1;
    int written = snprintf(buffer, buffer_size,
        "(/yun/add/https%%3A%%2F%%2Fmusic.163.com%%2F%%23%%2Fsong%%3Fid%%3D%s)",
        song_id);
    return written > 0 && (size_t)written < buffer_size ? 0 : -1;
}
