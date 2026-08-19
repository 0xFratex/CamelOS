#include "task.h"
#include "memory.h"
#include "string.h"
#include "scheduler.h"

task_t* current_task = 0;
task_t* task_list_head = 0;
int next_pid = 1;

void tasking_init() {
    // Create Kernel Task (PID 0)
    task_t* ktask = (task_t*)kmalloc(sizeof(task_t));
    ktask->id = 0;
    ktask->uid = 0; // Root
    ktask->state = 1;
    strcpy(ktask->name, "kernel");
    ktask->next = ktask; // Circular list
    strcpy(ktask->cwd, "/"); // Initialize CWD to root
    current_task = ktask;
    task_list_head = ktask;
}

task_t* create_task(int id, uint32_t entry_point, uint32_t stack_top) {
    task_t* new_task = (task_t*)kmalloc(sizeof(task_t));
    if (!new_task) return 0;
    
    /* Zero the entire struct so all fields (especially address_space,
     * signal_state, etc.) start as NULL/0 rather than containing
     * garbage from the heap.  Without this, the scheduler's
     * address_space switch check could dereference a garbage pointer. */
    memset(new_task, 0, sizeof(task_t));
    
    new_task->id = id;
    new_task->uid = 0; // Default to Root
    new_task->state = TASK_STATE_READY;
    new_task->is_app_bundle = 0;
    new_task->name[0] = '\0';
    
    // Setup CPU context on stack (must match irq_common_stub layout)
    // The IRQ stub pushes: pusha, ds, es, fs, gs
    // The IRQ macro pushes: err_code (0), int_no (32 for timer)
    // The CPU pushes: eip, cs, eflags (for interrupt)
    // Stack grows down, so we push in reverse order:
    uint32_t* top = (uint32_t*)stack_top;
    
    // CPU-saved state (for iret)
    *(--top) = 0x202;       // EFLAGS (interrupts enabled, IF=1)
    *(--top) = 0x08;        // CS (kernel code segment)
    *(--top) = entry_point; // EIP (entry point)
    
    // IRQ macro pushes
    *(--top) = 0;           // err_code (no error code)
    *(--top) = 32;          // int_no (timer IRQ vector - arbitrary for initial frame)
    
    // pusha layout: eax, ecx, edx, ebx, esp_placeholder, ebp, esi, edi
    *(--top) = 0;           // eax
    *(--top) = 0;           // ecx
    *(--top) = 0;           // edx
    *(--top) = 0;           // ebx
    uint32_t esp_val = (uint32_t)top; // esp (placeholder - calculated before decrement)
    *(--top) = esp_val;
    *(--top) = 0;           // ebp
    *(--top) = 0;           // esi
    *(--top) = 0;           // edi
    
    // Segment registers
    *(--top) = 0x10;        // DS (kernel data segment)
    *(--top) = 0x10;        // ES
    *(--top) = 0x10;        // FS
    *(--top) = 0x10;        // GS

    new_task->esp = (uint32_t)top;
    new_task->priority = 128;  // Default priority
    new_task->time_slice = 10;
    new_task->time_used = 0;
    new_task->sleep_until = 0;
    new_task->block_reason = 0;
    strcpy(new_task->cwd, "/"); // Initialize CWD to root
    new_task->next = 0;
    
    return new_task;
}

void create_user_task(void (*entry)(), const char* name, int uid, int is_app) {
    task_t* new_task = (task_t*)kmalloc(sizeof(task_t));
    new_task->id = next_pid++;
    new_task->uid = uid;
    new_task->state = TASK_STATE_READY;
    new_task->is_app_bundle = is_app;
    strcpy(new_task->name, name);
    
    // Allocate Stack
    uint32_t stack_size = 16384;
    uint32_t* stack = (uint32_t*)kmalloc(stack_size);
    if (!stack) {
        kfree(new_task);
        return;
    }

    // Setup CPU context on stack (must match irq_common_stub layout)
    uint32_t* top = (uint32_t*)((uint8_t*)stack + stack_size);
    
    // CPU-saved state (for iret)
    *(--top) = 0x202;           // EFLAGS (interrupts enabled)
    *(--top) = 0x08;            // CS (kernel code segment)
    *(--top) = (uint32_t)entry; // EIP (entry point)
    
    // IRQ macro pushes
    *(--top) = 0;               // err_code (no error code)
    *(--top) = 32;              // int_no (timer IRQ vector)
    
    // pusha layout: eax, ecx, edx, ebx, esp_placeholder, ebp, esi, edi
    *(--top) = 0;               // eax
    *(--top) = 0;               // ecx
    *(--top) = 0;               // edx
    *(--top) = 0;               // ebx
    uint32_t esp_val2 = (uint32_t)top; // esp (placeholder)
    *(--top) = esp_val2;
    *(--top) = 0;               // ebp
    *(--top) = 0;               // esi
    *(--top) = 0;               // edi
    
    // Segment registers
    *(--top) = 0x10;            // DS
    *(--top) = 0x10;            // ES
    *(--top) = 0x10;            // FS
    *(--top) = 0x10;            // GS
    
    new_task->esp = (uint32_t)top;
    strcpy(new_task->cwd, "/"); // Initialize CWD to root
    
    // Add to linked list
    task_t* tmp = task_list_head;
    while(tmp->next != task_list_head) tmp = tmp->next;
    tmp->next = new_task;
    new_task->next = task_list_head;
}

// ============================================================================
// Task 7: Create a Ring 3 (user-mode) task
// ============================================================================

task_t* task_create_user(const char* name, void* entry_point, void* stack_top) {
    task_t* new_task = (task_t*)kmalloc(sizeof(task_t));
    if (!new_task) return 0;
    
    memset(new_task, 0, sizeof(task_t));
    new_task->id = next_pid++;
    new_task->uid = 1000;  // Default non-root user
    new_task->state = TASK_STATE_READY;
    new_task->is_app_bundle = 0;
    if (name) {
        strncpy(new_task->name, name, 31);
        new_task->name[31] = '\0';
    }
    
    // Allocate a kernel stack (16KB) for this task.
    // The kernel stack is used when the task traps to Ring 0.
    uint32_t kstack_size = 16384;
    uint8_t* kstack = (uint8_t*)kmalloc(kstack_size);
    if (!kstack) {
        kfree(new_task);
        return 0;
    }
    
    // Setup CPU context on the kernel stack.
    // Layout must match irq_common_stub / the iret frame.
    // For Ring 3, we set CS and segment registers to user-mode selectors.
    uint32_t* top = (uint32_t*)(kstack + kstack_size);

    // iret to Ring 3 pops EIP, CS, EFLAGS, UserESP, SS (privilege change).
    // Build order matches registers_t in isr.h: SS/UserESP are pushed
    // FIRST (highest address), before the EFLAGS/CS/EIP iret frame.
    *(--top) = 0x20 | 3;           // SS = user data segment (0x20) + RPL 3
    *(--top) = (uint32_t)stack_top; // UserESP (top of user stack)
    *(--top) = 0x202;              // EFLAGS (interrupts enabled, IOPL=0)
    *(--top) = 0x18 | 3;           // CS = user code segment (0x18) + RPL 3
    *(--top) = (uint32_t)entry_point; // EIP (entry point in user space)

    // IRQ macro pushes
    *(--top) = 0;                  // err_code (no error code)
    *(--top) = 32;                 // int_no (timer IRQ vector)

    // pusha layout: eax, ecx, edx, ebx, esp_placeholder, ebp, esi, edi
    *(--top) = 0;                  // eax
    *(--top) = 0;                  // ecx
    *(--top) = 0;                  // edx
    *(--top) = 0;                  // ebx
    *(--top) = 0;                  // esp (pusha placeholder)
    *(--top) = 0;                  // ebp
    *(--top) = 0;                  // esi
    *(--top) = 0;                  // edi

    // Segment registers (user data segment with RPL 3)
    *(--top) = 0x20 | 3;           // DS = user data segment (0x20) + RPL 3
    *(--top) = 0x20 | 3;           // ES
    *(--top) = 0x20 | 3;           // FS
    *(--top) = 0x20 | 3;           // GS

    new_task->esp = (uint32_t)top;
    new_task->kernel_stack = kstack;
    new_task->kernel_stack_top = (uint32_t)(kstack + kstack_size);
    new_task->priority = 128;
    new_task->time_slice = 10;
    new_task->time_used = 0;
    new_task->sleep_until = 0;
    new_task->block_reason = 0;
    strcpy(new_task->cwd, "/"); // Initialize CWD to root
    new_task->next = 0;
    
    // Add to linked list
    if (task_list_head) {
        task_t* tmp = task_list_head;
        while(tmp->next && tmp->next != task_list_head) tmp = tmp->next;
        tmp->next = new_task;
        new_task->next = task_list_head;
    } else {
        new_task->next = new_task;
        task_list_head = new_task;
    }
    
    return new_task;
}

// Simple Round Robin Scheduler
void switch_task(registers_t* regs) {
    if (!current_task) return;

    // Save old ESP
    current_task->esp = (uint32_t)regs; // Regs pointer *is* the stack top after pushes

    // Pick next
    current_task = current_task->next;

    // We can't actually change stack here purely in C without return.
    // In ISR handler (Assembly), we should update the stack pointer.
    // But since `regs` is passed by value in some implementations or pointer in others, 
    // we need to perform the stack switch physically.
    
    // HACK for this architecture: 
    // We modify the stack structure `regs` points to? No.
    // We need to return the NEW stack pointer to the ASM ISR stub.
    // The ISR in assembly must: "mov esp, eax" where eax is return of this func.
}

int get_current_uid() { return current_task ? current_task->uid : 0; }

void set_current_uid(int uid) {
    if (current_task) current_task->uid = uid;
}
