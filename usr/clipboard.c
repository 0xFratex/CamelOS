#include "clipboard.h"

char clipboard_path[128] = {0};
int clipboard_active = 0;
int clipboard_op = 0; // 0=Copy, 1=Cut