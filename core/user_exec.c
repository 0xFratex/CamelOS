// core/user_exec.c - Launch a raw binary as a true Ring 3 user process.
//
// This is the minimal "self-contained process" spine: map the image into a
// fresh user address space (USER_CODE_START), map a user stack, build a
// correct Ring 3 iret frame (EIP/CS/EFLAGS/UserESP/SS), and hand the task to
// the scheduler.  The image runs at CPL 3 and reaches the kernel only through
// int 0x80 syscalls.
#include "../include/types.h"
#include "memory.h"
#include "string.h"
#include "vmm.h"
#include "task.h"
#include "scheduler.h"
#include "../hal/drivers/serial.h"

extern void scheduler_add_task(task_t* task, uint8_t priority);
extern int next_pid;

int user_exec_raw(const char* name, const void* code, uint32_t size) {
    /* 1. Fresh user address space */
    address_space_t* as = vmm_create_address_space();
    if (!as) { s_printf("[USER] exec: no address space\n"); return -1; }

    /* 2. Copy the image into a kernel buffer and map it at USER_CODE_START.
     *    The kernel is identity-mapped, so virt == phys for kmalloc'd memory. */
    uint32_t code_pages = (size + 0xFFF) / 0x1000;
    uint8_t* code_buf = (uint8_t*)kmalloc_a(code_pages * 0x1000);
    if (!code_buf) { vmm_destroy_address_space(as); return -1; }
    memset(code_buf, 0, code_pages * 0x1000);
    memcpy(code_buf, code, size);

    for (uint32_t i = 0; i < code_pages; i++) {
        uint32_t va = USER_CODE_START + i * 0x1000;
        uint32_t pa = (uint32_t)(code_buf + i * 0x1000);
        if (vmm_map_page(as, va, pa,
                         VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER) != 0) {
            s_printf("[USER] exec: code map failed\n");
            vmm_destroy_address_space(as);
            return -1;
        }
    }

    /* 3. Map a user stack just below USER_STACK_TOP (grows down). */
    uint32_t stack_top = USER_STACK_TOP;
    const uint32_t stack_pages = 8; /* 32 KB */
    for (uint32_t i = 0; i < stack_pages; i++) {
        uint32_t va = stack_top - (i + 1) * 0x1000;
        uint32_t frame = pmm_alloc_frame();
        if (!frame) { vmm_destroy_address_space(as); return -1; }
        if (vmm_map_page(as, va, frame,
                         VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER) != 0) {
            vmm_destroy_address_space(as);
            return -1;
        }
    }

    /* 4. Allocate the task and its own kernel stack. */
    task_t* t = (task_t*)kmalloc(sizeof(task_t));
    if (!t) { vmm_destroy_address_space(as); return -1; }
    memset(t, 0, sizeof(task_t));
    t->id = next_pid++;
    t->uid = 1000;
    t->state = TASK_STATE_READY;
    t->address_space = as;
    strncpy(t->name, name ? name : "user", 31);
    t->name[31] = '\0';
    strcpy(t->cwd, "/");

    uint32_t kstack_size = 16384;
    uint8_t* kstack = (uint8_t*)kmalloc(kstack_size);
    if (!kstack) { vmm_destroy_address_space(as); kfree(t); return -1; }
    t->kernel_stack = kstack;
    t->kernel_stack_top = (uint32_t)kstack + kstack_size;

    /* 5. Build the Ring 3 iret frame.  iret to Ring 3 pops
     *    EIP, CS, EFLAGS, UserESP, SS (privilege change). */
    uint32_t* top = (uint32_t*)(kstack + kstack_size);
    *(--top) = 0x20 | 3;              /* SS  = user data segment */
    *(--top) = stack_top;             /* UserESP */
    *(--top) = 0x202;                 /* EFLAGS (IF=1, IOPL=0) */
    *(--top) = 0x18 | 3;              /* CS  = user code segment */
    *(--top) = USER_CODE_START;       /* EIP */
    *(--top) = 0;                     /* err_code */
    *(--top) = 32;                    /* int_no (timer vector) */
    /* pusha */
    *(--top) = 0; /* eax */ *(--top) = 0; /* ecx */
    *(--top) = 0; /* edx */ *(--top) = 0; /* ebx */
    *(--top) = 0; /* esp */ *(--top) = 0; /* ebp */
    *(--top) = 0; /* esi */ *(--top) = 0; /* edi */
    /* segments */
    *(--top) = 0x20 | 3; /* ds */ *(--top) = 0x20 | 3; /* es */
    *(--top) = 0x20 | 3; /* fs */ *(--top) = 0x20 | 3; /* gs */
    t->esp = (uint32_t)top;

    /* 6. Schedule the task (priority 128, default). */
    scheduler_add_task(t, 128);
    s_printf("[USER] exec: launched '%s' at 0x%x\n", t->name, USER_CODE_START);
    return 0;
}
