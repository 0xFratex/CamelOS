#ifndef APP_SWITCHER_H
#define APP_SWITCHER_H

void app_switcher_handle_key(int key_code, int ctrl_down, int shift_down);
void app_switcher_release();
void app_switcher_render(int screen_w, int screen_h);
int app_switcher_is_active();

#endif