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
    
    extern isr_handler
    call isr_handler
    
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8
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
; ISR 0x80 for RTL8169
global isr128
isr128:
    push byte 0
    push byte 128
    jmp isr_common_stub

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