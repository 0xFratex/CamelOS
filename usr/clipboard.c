/**
 * clipboard.c - Clipboard System Implementation for CamelOS
 *
 * Implements a system-wide clipboard (pasteboard) supporting text copy/paste
 * and file reference copy/cut operations. Follows the macOS model where
 * the clipboard is a single global instance — the most recent copy/cut
 * overwrites previous content.
 *
 * Text clipboard stores up to 4KB of UTF-8 text for inter-app transfer.
 * File clipboard stores a path reference for file manager operations
 * (copy/cut files between directories).
 */

#include "clipboard.h"
#include "memory.h"
#include "string.h"
#include "../hal/drivers/serial.h"
#include "../hal/cpu/timer.h"

/* ======================================================================== */
/*  Global Clipboard Instance                                                */
/* ======================================================================== */

static clipboard_t g_clipboard;

/* Legacy compatibility globals — some code still references these */
char clipboard_path[128] = {0};
int  clipboard_active = 0;
int  clipboard_op = 0; /* 0=Copy, 1=Cut */

/* ======================================================================== */
/*  Initialization                                                          */
/* ======================================================================== */

void clipboard_init(void)
{
    memset(&g_clipboard, 0, sizeof(g_clipboard));
    g_clipboard.type = CLIPBOARD_EMPTY;
    g_clipboard.owner_pid = -1;

    /* Clear legacy globals */
    memset(clipboard_path, 0, sizeof(clipboard_path));
    clipboard_active = 0;
    clipboard_op = 0;

    s_printf("[CLIPBOARD] Clipboard system initialized\n");
}

/* ======================================================================== */
/*  Internal: Sync legacy globals                                            */
/* ======================================================================== */

static void clipboard_sync_legacy(void)
{
    /* Update legacy globals so existing code that checks clipboard_path
     * and clipboard_active still works. */
    clipboard_active = (g_clipboard.type != CLIPBOARD_EMPTY) ? 1 : 0;

    if (g_clipboard.type == CLIPBOARD_FILE_COPY ||
        g_clipboard.type == CLIPBOARD_FILE_CUT) {
        strncpy(clipboard_path, g_clipboard.file_path, sizeof(clipboard_path) - 1);
        clipboard_path[sizeof(clipboard_path) - 1] = '\0';
        clipboard_op = (g_clipboard.type == CLIPBOARD_FILE_CUT) ? 1 : 0;
    } else {
        memset(clipboard_path, 0, sizeof(clipboard_path));
        clipboard_op = 0;
    }
}

/* ======================================================================== */
/*  Copy Operations                                                         */
/* ======================================================================== */

int clipboard_copy_text(const char* text, int owner_pid)
{
    if (!text) return -1;

    uint32_t len = strlen(text);
    if (len >= CLIPBOARD_MAX_TEXT) {
        /* Truncate to fit */
        len = CLIPBOARD_MAX_TEXT - 1;
    }

    memset(&g_clipboard, 0, sizeof(g_clipboard));
    g_clipboard.type = CLIPBOARD_TEXT;
    g_clipboard.owner_pid = owner_pid;
    g_clipboard.text_len = len;
    g_clipboard.timestamp = timer_get_ticks();

    memcpy(g_clipboard.text, text, len);
    g_clipboard.text[len] = '\0';

    clipboard_sync_legacy();

    s_printf("[CLIPBOARD] Text copied (%d bytes)\n", len);
    return 0;
}

int clipboard_copy_file(const char* path, int is_cut, int owner_pid)
{
    if (!path) return -1;

    uint32_t len = strlen(path);
    if (len >= CLIPBOARD_MAX_PATH) return -1;

    memset(&g_clipboard, 0, sizeof(g_clipboard));
    g_clipboard.type = is_cut ? CLIPBOARD_FILE_CUT : CLIPBOARD_FILE_COPY;
    g_clipboard.owner_pid = owner_pid;
    g_clipboard.timestamp = timer_get_ticks();

    strncpy(g_clipboard.file_path, path, CLIPBOARD_MAX_PATH - 1);
    g_clipboard.file_path[CLIPBOARD_MAX_PATH - 1] = '\0';

    clipboard_sync_legacy();

    s_printf("[CLIPBOARD] File %s: %s\n", is_cut ? "cut" : "copy", path);
    return 0;
}

/* ======================================================================== */
/*  Paste / Query Operations                                                */
/* ======================================================================== */

int clipboard_paste_text(char* buf, uint32_t buf_size)
{
    if (!buf || buf_size == 0) return -1;

    if (g_clipboard.type != CLIPBOARD_TEXT) return -1;

    uint32_t copy_len = g_clipboard.text_len;
    if (copy_len >= buf_size) {
        copy_len = buf_size - 1;
    }

    memcpy(buf, g_clipboard.text, copy_len);
    buf[copy_len] = '\0';

    return (int)copy_len;
}

int clipboard_get_type(void)
{
    return g_clipboard.type;
}

int clipboard_get_file_path(char* buf, uint32_t buf_size)
{
    if (!buf || buf_size == 0) return -1;

    if (g_clipboard.type != CLIPBOARD_FILE_COPY &&
        g_clipboard.type != CLIPBOARD_FILE_CUT) {
        return -1;
    }

    strncpy(buf, g_clipboard.file_path, buf_size - 1);
    buf[buf_size - 1] = '\0';

    return 0;
}

int clipboard_is_cut(void)
{
    return (g_clipboard.type == CLIPBOARD_FILE_CUT) ? 1 : 0;
}

void clipboard_clear(void)
{
    memset(&g_clipboard, 0, sizeof(g_clipboard));
    g_clipboard.type = CLIPBOARD_EMPTY;
    g_clipboard.owner_pid = -1;

    clipboard_sync_legacy();

    s_printf("[CLIPBOARD] Cleared\n");
}

int clipboard_has_content(void)
{
    return (g_clipboard.type != CLIPBOARD_EMPTY) ? 1 : 0;
}
