/**
 * user_copy.c - Safe user-space memory access for CamelOS
 *
 * Implements validated copy operations between kernel and user space.
 * These are the CRITICAL security boundary functions that prevent Ring 3
 * processes from tricking the kernel into reading/writing kernel memory
 * by passing forged pointers in syscall arguments.
 *
 * Threat model:
 *   A user process (CPL 3) invokes int 0x80 / sysenter with registers
 *   set to arbitrary values.  The kernel must NOT dereference any
 *   address from these registers without first confirming it lies
 *   in user space (below KERNEL_BASE = 0xC0000000).
 *
 * Implementation notes:
 *   - All arithmetic is done via uintptr_t to avoid undefined behaviour
 *     on pointer overflow.
 *   - validate_user_ptr() checks the ENTIRE range [ptr, ptr+size), not
 *     just the start address — a classic attack is to pass a pointer
 *     near the user/kernel boundary so the range straddles into kernel
 *     memory.
 *   - The write_access parameter is reserved for future page-level
 *     permission checking (e.g., verifying that the user page is
 *     writable before a copy_to_user).  Currently it is accepted but
 *     not enforced because the kernel does not yet maintain a reverse
 *     page-flags lookup for arbitrary virtual addresses.
 */

#include "user_copy.h"
#include "../include/string.h"
#include "vmm.h"       /* KERNEL_BASE */

/* -----------------------------------------------------------------------
 * validate_user_ptr
 * ----------------------------------------------------------------------- */
int validate_user_ptr(const void* ptr, size_t size, int write_access)
{
    uintptr_t start = (uintptr_t)ptr;
    uintptr_t end;

    (void)write_access;  /* reserved for future page-level write check */

    /* 1. NULL pointer check */
    if (start == 0) return -1;

    /* 2. Overflow check: start + size must not wrap around.
     *    If it does, the range would wrap past 0 and confuse
     *    the kernel-boundary check below. */
    end = start + size;
    if (end < start) return -1;   /* unsigned overflow */

    /* 3. Kernel boundary check: the entire range must be below
     *    KERNEL_BASE.  Any address >= KERNEL_BASE is mapped to
     *    kernel code/data and must never be accessed on behalf of
     *    an untrusted user pointer. */
    if (start >= KERNEL_BASE) return -1;
    if (end   >  KERNEL_BASE) return -1;   /* range straddles boundary */

    return 0;   /* valid */
}

/* -----------------------------------------------------------------------
 * copy_from_user
 * ----------------------------------------------------------------------- */
int copy_from_user(void* kernel_dst, const void* user_src, size_t count)
{
    /* Zero-length copy is always safe */
    if (count == 0) return 0;

    /* Validate the source (user) pointer range */
    if (validate_user_ptr(user_src, count, 0) != 0) return -1;

    /* Destination is in kernel space — caller's responsibility */

    memcpy(kernel_dst, user_src, count);
    return 0;
}

/* -----------------------------------------------------------------------
 * copy_to_user
 * ----------------------------------------------------------------------- */
int copy_to_user(void* user_dst, const void* kernel_src, size_t count)
{
    /* Zero-length copy is always safe */
    if (count == 0) return 0;

    /* Validate the destination (user) pointer range — write access */
    if (validate_user_ptr(user_dst, count, 1) != 0) return -1;

    /* Source is in kernel space — caller's responsibility */

    memcpy(user_dst, kernel_src, count);
    return 0;
}

/* -----------------------------------------------------------------------
 * validate_user_string
 * ----------------------------------------------------------------------- */
int validate_user_string(const void* user_str, size_t max_len)
{
    if (user_str == NULL) return -1;

    uintptr_t start = (uintptr_t)user_str;

    /* Check that the starting address is in user space */
    if (start >= KERNEL_BASE) return -1;

    /* We cannot safely scan the string (it might fault on an
     * unmapped page), so we validate the maximum possible range.
     * The caller should pick a reasonable max_len (e.g. PATH_MAX). */
    if (validate_user_ptr(user_str, max_len, 0) != 0) return -1;

    return 0;
}
