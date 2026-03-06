#include "task.h"
#include "memory.h"
#include "string.h"

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
    current_task = ktask;
    task_list_head = ktask;
}

task_t* create_task(int id, uint32_t entry_point, uint32_t stack_top) {
    task_t* new_task = (task_t*)kmalloc(sizeof(task_t));
    if (!new_task) return 0;
    
    new_task->id = id;
    new_task->uid = 0; // Default to Root
    new_task->state = TASK_STATE_READY;
    new_task->is_app_bundle = 0;
    new_task->name[0] = '\0';
    
    // Setup CPU context on stack (simulating an interrupt frame)
    uint32_t* top = (uint32_t*)stack_top;
    
    // Push standard registers expected by ISR (iret)
    *(--top) = 0x202; // EFLAGS (Interrupts enabled)
    *(--top) = 0x08;  // CS
    *(--top) = entry_point; // EIP
    
    // Pusha: eax, ecx, edx, ebx, original_esp, ebp, esi, edi
    *(--top) = 0; *(--top) = 0; *(--top) = 0; *(--top) = 0;
    uint32_t esp_placeholder = (uint32_t)(top + 1) + 4;  // Calculate ESP before decrement
    *(--top) = esp_placeholder; // ESP placeholder
    *(--top) = 0; *(--top) = 0; *(--top) = 0;

    // Segment selectors
    *(--top) = 0x10; // DS
    
    new_task->esp = (uint32_t)top;
    new_task->priority = 128;  // Default priority
    new_task->time_slice = 10;
    new_task->time_used = 0;
    new_task->sleep_until = 0;
    new_task->block_reason = 0;
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

    // Setup CPU context on stack (simulating an interrupt frame)
    uint32_t* top = (uint32_t*)((uint8_t*)stack + stack_size);
    
    // Push standard registers expected by ISR (iret)
    *(--top) = 0x202; // EFLAGS (Interrupts enabled)
    *(--top) = 0x08;  // CS
    *(--top) = (uint32_t)entry; // EIP
    
    // Pusha: eax, ecx, edx, ebx, original_esp, ebp, esi, edi
    *(--top) = 0; *(--top) = 0; *(--top) = 0; *(--top) = 0;
    uint32_t esp_placeholder = (uint32_t)(top + 1) + 4;  // Calculate ESP before decrement
    *(--top) = esp_placeholder; // ESP placeholder
    *(--top) = 0; *(--top) = 0; *(--top) = 0;

    // Segment selectors
    *(--top) = 0x10; // DS
    
    new_task->esp = (uint32_t)top;
    
    // Add to linked list
    task_t* tmp = task_list_head;
    while(tmp->next != task_list_head) tmp = tmp->next;
    tmp->next = new_task;
    new_task->next = task_list_head;
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
