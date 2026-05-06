/**
 * clipboard.h - Clipboard System for CamelOS
 *
 * Provides a system-wide clipboard for copy/cut/paste operations
 * between applications. Supports text and file reference clipboard
 * types, following the macOS pasteboard model.
 *
 * The clipboard is a single global instance (like macOS) where the
 * most recent copy/cut overwrites the previous content. Applications
 * can store text data or file references for inter-app data transfer.
 */

#ifndef CLIPBOARD_H
#define CLIPBOARD_H

#include "../include/types.h"

/* ======================================================================== */
/*  Clipboard Types                                                         */
/* ======================================================================== */

#define CLIPBOARD_EMPTY      0   /* No content in clipboard */
#define CLIPBOARD_TEXT       1   /* Text data (UTF-8 string) */
#define CLIPBOARD_FILE_COPY  2   /* File reference (copy operation) */
#define CLIPBOARD_FILE_CUT   3   /* File reference (cut operation) */

/* Maximum sizes */
#define CLIPBOARD_MAX_TEXT   4096  /* Max text content in bytes */
#define CLIPBOARD_MAX_PATH   256   /* Max file path length */

/* ======================================================================== */
/*  Clipboard State (global, single-instance)                               */
/* ======================================================================== */

typedef struct {
    int       type;                    /* CLIPBOARD_TEXT, CLIPBOARD_FILE_COPY, etc. */
    char      text[CLIPBOARD_MAX_TEXT]; /* Text content (null-terminated) */
    uint32_t  text_len;                /* Length of text content (not including NUL) */
    char      file_path[CLIPBOARD_MAX_PATH]; /* Source file path for file operations */
    int       owner_pid;               /* PID of the process that owns the clipboard */
    uint32_t  timestamp;               /* When the clipboard was last modified */
} clipboard_t;

/* ======================================================================== */
/*  Public API                                                              */
/* ======================================================================== */

/**
 * clipboard_init - Initialize the clipboard system.
 *
 * Clears clipboard state. Called once during boot.
 */
void clipboard_init(void);

/**
 * clipboard_copy_text - Copy text to the clipboard.
 *
 * @text:     Null-terminated UTF-8 string to copy
 * @owner_pid: PID of the copying process (0 for kernel)
 *
 * Returns: 0 on success, -1 if text is too long
 */
int clipboard_copy_text(const char* text, int owner_pid);

/**
 * clipboard_copy_file - Copy a file reference to the clipboard.
 *
 * @path:      Absolute path of the file/directory
 * @is_cut:    0 for copy, 1 for cut (move on paste)
 * @owner_pid: PID of the copying process
 *
 * Returns: 0 on success, -1 on failure
 */
int clipboard_copy_file(const char* path, int is_cut, int owner_pid);

/**
 * clipboard_paste_text - Get text from the clipboard.
 *
 * @buf:       Destination buffer
 * @buf_size:  Size of destination buffer
 *
 * Returns: Number of bytes copied (not including NUL), or -1 if no text
 */
int clipboard_paste_text(char* buf, uint32_t buf_size);

/**
 * clipboard_get_type - Get the current clipboard content type.
 *
 * Returns: CLIPBOARD_EMPTY, CLIPBOARD_TEXT, CLIPBOARD_FILE_COPY, or CLIPBOARD_FILE_CUT
 */
int clipboard_get_type(void);

/**
 * clipboard_get_file_path - Get the file path from a file clipboard entry.
 *
 * @buf:       Destination buffer
 * @buf_size:  Size of destination buffer
 *
 * Returns: 0 on success, -1 if clipboard doesn't contain a file reference
 */
int clipboard_get_file_path(char* buf, uint32_t buf_size);

/**
 * clipboard_is_cut - Check if the file clipboard entry is a cut operation.
 *
 * Returns: 1 if cut, 0 if copy or not a file reference
 */
int clipboard_is_cut(void);

/**
 * clipboard_clear - Clear the clipboard contents.
 */
void clipboard_clear(void);

/**
 * clipboard_has_content - Check if the clipboard has any content.
 *
 * Returns: 1 if clipboard has content, 0 if empty
 */
int clipboard_has_content(void);

/* Legacy compatibility - these globals are still referenced by some code */
extern char clipboard_path[128];
extern int  clipboard_active;
extern int  clipboard_op;

#endif /* CLIPBOARD_H */
