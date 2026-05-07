/**
 * user_copy.h - Safe user-space memory access for CamelOS
 *
 * Provides validated copy operations between kernel and user space.
 * These functions MUST be used whenever the kernel reads from or writes to
 * buffers whose addresses were supplied by a Ring 3 (user-mode) process.
 *
 * Without this validation, a malicious user process could pass a kernel-space
 * address as a syscall argument, tricking the kernel into reading or writing
 * arbitrary kernel memory (privilege escalation / information leak).
 *
 * Usage in syscall handlers:
 *   // Instead of:  memcpy(kbuf, (void*)arg2, count);
 *   // Use:         if (copy_from_user(kbuf, (void*)arg2, count)) return -1;
 *
 *   // Instead of:  memcpy((void*)arg2, kbuf, count);
 *   // Use:         if (copy_to_user((void*)arg2, kbuf, count)) return -1;
 */

#ifndef USER_COPY_H
#define USER_COPY_H

#include "../include/types.h"

/**
 * Validate that a user-space pointer range is safe to access.
 *
 * Checks:
 *   1. ptr is not NULL
 *   2. ptr + size does not wrap around (unsigned overflow)
 *   3. The entire range [ptr, ptr+size) lies below KERNEL_BASE (0xC0000000)
 *
 * @param ptr          Base address of the user buffer
 * @param size         Number of bytes to access
 * @param write_access Non-zero if the kernel intends to write to this range
 * @return             0 if valid, -1 if invalid
 */
int validate_user_ptr(const void* ptr, size_t size, int write_access);

/**
 * Safely copy data from user space into a kernel buffer.
 *
 * Validates the source pointer before performing the copy.
 *
 * @param kernel_dst  Destination in kernel memory (must already be valid)
 * @param user_src    Source in user space (will be validated)
 * @param count       Number of bytes to copy
 * @return            0 on success, -1 on validation error
 */
int copy_from_user(void* kernel_dst, const void* user_src, size_t count);

/**
 * Safely copy data from a kernel buffer into user space.
 *
 * Validates the destination pointer before performing the copy.
 *
 * @param user_dst    Destination in user space (will be validated)
 * @param kernel_src  Source in kernel memory (must already be valid)
 * @param count       Number of bytes to copy
 * @return            0 on success, -1 on validation error
 */
int copy_to_user(void* user_dst, const void* kernel_src, size_t count);

/**
 * Validate that a NUL-terminated user-space string is safe to read.
 *
 * Walks up to max_len bytes checking that every page touched
 * is in user space.  Does NOT copy the string — use this to
 * validate before copy_from_user() or before dereferencing directly
 * in cases where the length is not known ahead of time.
 *
 * @param user_str   Pointer to the user-space string
 * @param max_len    Maximum number of bytes to check (prevents infinite scan)
 * @return           0 if valid, -1 if invalid
 */
int validate_user_string(const void* user_str, size_t max_len);

#endif /* USER_COPY_H */
