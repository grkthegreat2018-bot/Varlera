/* filepick.h - OS-native file/folder pickers (Win32 common dialogs;
 * zenity fallback on Linux). All return 1 when a path was chosen. */
#ifndef FILEPICK_H
#define FILEPICK_H

int pick_save_path(char *out, int n, const char *title,
                   const char *filter, const char *ext); /* overwrite prompt */
int pick_open_path(char *out, int n, const char *title,
                   const char *filter);                 /* must exist */
int pick_dir(char *out, int n);                         /* folder picker */

#endif
