/*
 * ui_win32.h - Win32 native UI for YunMusic plugin
 */

#ifndef UI_WIN32_H
#define UI_WIN32_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ui_init - Create the main dialog window (hidden by default)
 * Returns 0 on success, -1 on error
 */
int ui_init(HINSTANCE hInstance, HWND hParent);

/*
 * ui_show - Show the main window and bring to front
 */
void ui_show(void);

/*
 * ui_hide - Hide the main window
 */
void ui_hide(void);

/*
 * ui_toggle - Toggle window visibility
 */
void ui_toggle(void);

/*
 * ui_destroy - Destroy the window and clean up
 */
void ui_destroy(void);

/*
 * ui_is_visible - Check if window is currently visible
 */
int ui_is_visible(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_WIN32_H */
