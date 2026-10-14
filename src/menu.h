#ifndef MENU_H
#define MENU_H

/* main menu: scans worlds_dir for *.bw / *.bwx, lists them newest-first;
   row click loads. Buttons: RESUME / NEW / IMPORT / QUIT. */
void menu_enter(void);              /* A.app = APP_MENU + rescan          */
void menu_press(double x, double y);
void menu_key(int key);
void menu_wheel(double y);
void menu_draw(void);

#endif
