/*
 * Win32 Dialog Resource IDs for YunMusic Plugin
 */

#ifndef RESOURCE_H
#define RESOURCE_H

/* Main dialog */
#define IDD_MAIN_DIALOG         1000

/* Controls - Song info */
#define IDC_STATIC_TITLE        1001
#define IDC_STATIC_ARTIST       1002
#define IDC_STATIC_PROGRESS     1003
#define IDC_STATIC_COVER        1004
#define IDC_PROGRESS_BAR        1005

/* Controls - Playback */
#define IDC_BTN_PREV            1010
#define IDC_BTN_PLAY_PAUSE      1011
#define IDC_BTN_NEXT            1012

/* Controls - Volume */
#define IDC_SLIDER_VOLUME       1020
#define IDC_STATIC_VOLUME       1021
#define IDC_BTN_VOLUME          1023

/* Controls - Progress */
#define IDC_SLIDER_PROGRESS     1022

/* Controls - Lyrics */
#define IDC_LYRICS_AREA         1030

/* Controls - Search */
#define IDC_EDIT_SEARCH         1040
#define IDC_BTN_SEARCH          1041
#define IDC_LIST_SEARCH         1042
#define IDC_LIST_PLAYLIST       1043
#define IDC_BTN_SEARCH_MODE     1044
#define IDC_BTN_DESKTOP_LYRIC   1045
#define IDC_BTN_DAILY           1046

/* Timer IDs */
#define TIMER_POLL_STATUS       2000
#define TIMER_LYRIC_SYNC        2001
#define TIMER_PROGRESS_SMOOTH   2002

#endif /* RESOURCE_H */
