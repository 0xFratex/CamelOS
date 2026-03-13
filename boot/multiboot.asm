; boot/multiboot.asm
; USED BY: Installer (ISO)
[BITS 32]

; --- Multiboot Header ---
MULTIBOOT_PAGE_ALIGN    equ  1 << 0
MULTIBOOT_MEMORY_INFO   equ  1 << 1
MULTIBOOT_VIDEO_MODE    equ  1 << 2  ; <--- ENABLED

MULTIBOOT_HEADER_MAGIC  equ  0x1BADB002
MULTIBOOT_HEADER_FLAGS  equ  MULTIBOOT_PAGE_ALIGN | MULTIBOOT_MEMORY_INFO | MULTIBOOT_VIDEO_MODE
MULTIBOOT_CHECKSUM      equ -(MULTIBOOT_HEADER_MAGIC + MULTIBOOT_HEADER_FLAGS)

section .multiboot
align 4
    dd MULTIBOOT_HEADER_MAGIC
    dd MULTIBOOT_HEADER_FLAGS
    dd MULTIBOOT_CHECKSUM
    dd 0, 0, 0, 0, 0
    ; Video Request: 1024x768 x 32bpp
    dd 0, 1024, 768, 32

section .text
global _start
extern main
extern _bss_start
extern _bss_end

_start:
    ; 1. Disable Interrupts
    cli

    ; 2. Save Multiboot Registers IMMEDIATELY
    mov esi, eax  ; Save Magic
    mov edi, ebx  ; Save Info

    ; 3. Setup Stack
    mov esp, stack_top
    and esp, 0xFFFFFFF0
    mov ebp, esp

    ; 4. Zero BSS
    ; Save info ptr to EDX to preserve across memset
    mov edx, edi  

    mov edi, _bss_start
    mov ecx, _bss_end
    sub ecx, _bss_start
    xor eax, eax
    rep stosb

    ; 5. Restore Arguments for C call
    push edx      ; Arg2: Info Pointer
    push esi      ; Arg1: Magic Number

    ; 6. Call C Entry
    call main

    ; 7. Safety Hang
.hang:
    cli
    hlt
    jmp .hang

section .bss
align 16
stack_bottom:
    resb 32768 ; 32KB Stack
stack_top: