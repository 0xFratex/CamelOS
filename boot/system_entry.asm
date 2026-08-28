; boot/system_entry.asm
[BITS 32]

; Define a special section that the Linker Script puts FIRST
section .text.entry
global _start
extern kernel_main
extern _bss_start
extern _bss_end

_start:
    ; 1. Disable Interrupts
    cli
    
    ; 2. Set up stack immediately (32KB)
    mov esp, stack_top
    and esp, 0xFFFFFFF0 ; Force 16-byte alignment for ABI compliance
    mov ebp, esp

    ; 3. Reset EFLAGS
    push 0
    popfd

    ; 4. Zero out BSS (Uninitialized variables)
    mov edi, _bss_start
    mov ecx, _bss_end
    sub ecx, _bss_start
    xor eax, eax
    rep stosb

    ; 5. Pass Boot Info (If any) and Jump to C
    ; For MBR boot, ebx is 0. For GRUB, ebx is Multiboot info.
    push ebx
    call kernel_main

    ; 6. Safety Hang
.hang:
    cli
    hlt
    jmp .hang

; --- Multiboot Header (For ISO/GRUB only) ---
MULTIBOOT_PAGE_ALIGN    equ  1 << 0
MULTIBOOT_MEMORY_INFO   equ  1 << 1
MULTIBOOT_VIDEO_MODE    equ  1 << 2
MULTIBOOT_HEADER_MAGIC  equ  0x1BADB002
MULTIBOOT_HEADER_FLAGS  equ  MULTIBOOT_PAGE_ALIGN | MULTIBOOT_MEMORY_INFO | MULTIBOOT_VIDEO_MODE
MULTIBOOT_CHECKSUM      equ -(MULTIBOOT_HEADER_MAGIC + MULTIBOOT_HEADER_FLAGS)

align 4
section .multiboot
    dd MULTIBOOT_HEADER_MAGIC
    dd MULTIBOOT_HEADER_FLAGS
    dd MULTIBOOT_CHECKSUM
    dd 0, 0, 0, 0, 0
    dd 0, 1024, 768, 32 ; Request 1024x768x32

section .text
; ... (Keep your existing ISR/IRQ stubs below unchanged) ...
; ... Copy the isr_common_stub, irq_common_stub and tables from previous file ...

; === Interrupt Service Routines (ISRs) ===
global isr_stub_table
global irq_stub_table

; --- Common ISR Handler ---
isr_common_stub:
    pusha           ; Pushes edi,esi,ebp,esp,ebx,edx,ecx,eax
    push ds
    push es
    push fs
    push gs
    
    mov ax, 0x10    ; Load Kernel Data Segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    cld             ; SECURITY: clear DF (Ring 3 can set it; string ops
                    ; in the kernel would otherwise run backwards)
    
    extern isr_handler
    call isr_handler
    
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8      ; Cleans up pushed error code and ISR number
    iret

; --- Common IRQ Handler ---
; This handler is used for hardware interrupts (IRQ 32-47).
; After calling isr_handler, it checks if the scheduler requests
; a context switch (sched_context_switch_needed != 0) and if so,
; switches to the new task's stack (sched_new_esp) before restoring
; registers and returning via iret.
;
; Task 7: Now handles both Ring 0 and Ring 3 returns.
; When iret pops CS with RPL 3, the CPU automatically switches
; to the user stack (from TSS ESP0 -> user ESP). The SS selector
; is pushed/poppped by iret when returning to a different privilege.
irq_common_stub:
    pusha
    push ds
    push es
    push fs
    push gs
    
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    cld             ; SECURITY: clear DF before C code runs
    
    extern isr_handler
    call isr_handler
    
    ; --- Preemptive Context Switch ---
    ; Check if the scheduler requested a context switch.
    ; The scheduler sets sched_context_switch_needed = 1 and
    ; sched_new_esp = new task's stack pointer.
    extern sched_context_switch_needed
    extern sched_new_esp
    
    mov eax, [sched_context_switch_needed]
    test eax, eax
    jz .no_switch
    
    ; Context switch requested: switch to the new task's stack.
    ; The new ESP points to a saved register frame that matches
    ; the layout we pushed above (gs, fs, es, ds, pusha frame).
    mov esp, [sched_new_esp]
    
    ; Clear the switch flag for next time
    mov dword [sched_context_switch_needed], 0
    
.no_switch:
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8

    ; Task 7: Check if returning to Ring 3.
    ; Look at the CS value on the stack (pushed by CPU before iret).
    ; If CS & 3 == 3, we're returning to user mode, and we need to
    ; also pop SS and ESP that the CPU pushed on privilege change.
    ; For Ring 0 returns, iret only pops EIP, CS, EFLAGS.
    ;
    ; IMPORTANT: For interrupts from Ring 3, the CPU pushes
    ; SS and ESP on the stack BEFORE the iret frame. So the
    ; stack layout when coming from Ring 3 is:
    ;   ..., EIP, CS, EFLAGS, User_ESP, User_SS
    ; And iret pops all 5 values when CS indicates Ring 3.
    ; Since our IRQ stubs don't push SS/ESP (we're already in
    ; kernel mode when the handler runs), this is handled
    ; automatically by iret as long as the context frame
    ; was set up correctly by the scheduler.
    iret

; --- Exceptions (0-31) ---
%macro ISR_NOERRCODE 1
global isr%1
isr%1:
    push byte 0
    push byte %1
    jmp isr_common_stub
%endmacro

%macro ISR_ERRCODE 1
global isr%1
isr%1:
    push byte %1
    jmp isr_common_stub
%endmacro

ISR_NOERRCODE 0
ISR_NOERRCODE 1
ISR_NOERRCODE 2
ISR_NOERRCODE 3
ISR_NOERRCODE 4
ISR_NOERRCODE 5
ISR_NOERRCODE 6
ISR_NOERRCODE 7
ISR_ERRCODE   8
ISR_NOERRCODE 9
ISR_ERRCODE   10
ISR_ERRCODE   11
ISR_ERRCODE   12
ISR_ERRCODE   13
ISR_ERRCODE   14
ISR_NOERRCODE 15
ISR_NOERRCODE 16
ISR_ERRCODE   17
ISR_NOERRCODE 18
ISR_NOERRCODE 19
ISR_NOERRCODE 20
ISR_NOERRCODE 21
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_NOERRCODE 29
ISR_NOERRCODE 30
ISR_NOERRCODE 31

; --- Hardware Interrupts (IRQs 0-15 mapped to 32-47) ---
%macro IRQ 2
global irq%1
irq%1:
    push byte 0
    push byte %2
    jmp irq_common_stub
%endmacro

IRQ 0,  32
IRQ 1,  33
IRQ 2,  34
IRQ 3,  35
IRQ 4,  36
IRQ 5,  37
IRQ 6,  38
IRQ 7,  39
IRQ 8,  40
IRQ 9,  41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

; Add at the end of ISR stubs
; ISR 0x81 for RTL8169 (moved from 0x80, which is now used for syscalls)
global isr129
isr129:
    push byte 0
    push byte 129
    jmp isr_common_stub

; --- Syscall Handler (int 0x80) ---
; Custom syscall entry point that preserves user registers properly
; Syscall convention:
;   EAX = syscall number
;   EBX = arg1, ECX = arg2, EDX = arg3, ESI = arg4, EDI = arg5
;   Return: EAX = result
;
; Task 7: Updated to handle syscalls from both Ring 0 and Ring 3.
; When coming from Ring 3, the CPU pushes SS and ESP before the
; iret frame. Our pusha saves the kernel ESP, not the user ESP,
; which is correct for the kernel's register save/restore.
global syscall_entry
syscall_entry:
    ; Save all registers
    pusha           ; Saves eax,ecx,edx,ebx,old_esp,ebp,esi,edi
    push ds
    push es
    push fs
    push gs
    
    ; Load kernel data segment
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    cld             ; SECURITY: clear DF (syscall args can come from Ring 3)
    
    ; Push pointer to saved register state (first arg to C handler)
    push esp
    
    ; Call C syscall handler
    extern syscall_handler
    call syscall_handler
    
    ; Restore the result into the saved eax slot on stack
    ; After pusha, the stack layout is: edi,esi,ebp,esp,ebx,edx,ecx,eax
    ; We need to write EAX result into the saved eax position
    ; esp+20 = saved eax (after pusha + 4 segs + arg)
    ; Actually: pusha(32) + ds,es,fs,gs(16) + push esp(4) = 52 bytes above
    ; saved_regs.eax is at esp+52-4+28 = let's just use the stack directly
    ; After call returns, add esp,4 to remove the arg
    add esp, 4      ; Remove the pushed esp argument
    
    pop gs
    pop fs
    pop es
    pop ds
    popa            ; Restores all regs including EAX with return value
    iret

; ============================================================================
; Task 7: SYSENTER entry point for fast system calls from Ring 3
; ============================================================================
; When a Ring 3 program executes SYSENTER:
;   - CPU loads CS from IA32_SYSENTER_CS MSR (0x08)
;   - CPU loads SS from IA32_SYSENTER_CS + 8 (0x10)
;   - CPU loads ESP from IA32_SYSENTER_ESP MSR
;   - CPU loads EIP from IA32_SYSENTER_EIP MSR
;   - CPU does NOT save the return address or user ESP!
;   - The user program must save these in registers before SYSENTER.
;
; Convention:
;   EAX = syscall number
;   EBX = return address (for sysexit)
;   ECX = user ESP (for sysexit)
;   EDX = arg1, ESI = arg2, EDI = arg3, EBP = arg4
;   Return via SYSEXIT: EIP = EBX, ESP = ECX
;
global sysenter_entry
sysenter_entry:
    ; We are now in Ring 0 with kernel stack.
    ; Save all registers (matches syscall_entry layout for compatibility)
    pusha
    push ds
    push es
    push fs
    push gs
    
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    cld             ; SECURITY: clear DF before C code runs
    
    ; Push pointer to saved register state
    push esp
    
    extern syscall_handler
    call syscall_handler
    
    add esp, 4      ; Remove the pushed argument
    
    pop gs
    pop fs
    pop es
    pop ds
    popa
    
    ; Return to Ring 3 via SYSEXIT
    ; SYSEXIT loads:
    ;   CS = IA32_SYSENTER_CS + 16 (0x18 = user code)
    ;   SS = IA32_SYSENTER_CS + 24 (0x20 = user data)
    ;   EIP = EDX (we need to set EDX = return address)
    ;   ESP = ECX (we need to set ECX = user ESP)
    ;
    ; However, our pusha/popad clobbered ECX and EDX.
    ; The user's return address was in EBX and user ESP was in ECX.
    ; After popa, ECX and EDX have their original values from the
    ; syscall entry, so:
    ;   ECX = user ESP (saved by user before sysenter)
    ;   We need EDX = return address (was in EBX before pusha)
    ; After popa, EBX has its original value (return address).
    mov edx, ebx    ; EDX = return address for sysexit
    ; ECX already has user ESP from the original register state
    sti             ; Re-enable interrupts before returning to user mode
    sysexit

section .data
global isr_stub_table
isr_stub_table:
    dd isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7, isr8, isr9
    dd isr10, isr11, isr12, isr13, isr14, isr15, isr16, isr17, isr18, isr19
    dd isr20, isr21, isr22, isr23, isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31

global irq_stub_table
irq_stub_table:
    dd irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7
    dd irq8, irq9, irq10, irq11, irq12, irq13, irq14, irq15

section .bss
align 16
stack_bottom:
    resb 32768 ; Increase Kernel Stack to 32KB for deep recursion/heavy ISRs
stack_top:

section .note.GNU-stack noalloc noexec nowrite progbits