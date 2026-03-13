[bits 16]
[org 0x7c00]

; === Camel OS MBR (Bootloader Fix) ===
; Ensures EAX is set to 0x2BADB002 before jumping to kernel
; matching GRUB behavior for consistent kernel entry.

KERNEL_ADDR     equ 0x100000    
SECTORS_TOTAL   equ 16384       
BUFFER_ADDR     equ 0x8000      
MBOOT_INFO_ADDR equ 0x0600      
VESA_INFO_ADDR  equ 0x0800      

start:
    jmp 0:init_segments

init_segments:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    mov [saved_drive], dl

    ; Enable A20
    in al, 0x92
    or al, 2
    out 0x92, al
    sti

    ; --- VESA VBE SETUP ---
    mov ax, 0x4F02
    mov bx, 0x4144 ; 1024x768x32
    int 0x10
    cmp ax, 0x004F
    je .vbe_success

    ; Fallback
    mov ax, 0x4F02
    mov bx, 0x4118 ; 1024x768x24
    int 0x10

.vbe_success:
    ; Get Mode Info
    mov ax, 0x4F01
    mov cx, bx
    and cx, 0x3FFF
    mov di, VESA_INFO_ADDR
    int 0x10

    ; Fill Fake Multiboot Header
    cld
    mov di, MBOOT_INFO_ADDR
    xor ax, ax
    mov cx, 30
    rep stosd

    ; Set Flags
    mov dword [MBOOT_INFO_ADDR + 0], 0x00001800 ; VBE | Framebuffer

    ; Copy Framebuffer Info
    mov eax, [VESA_INFO_ADDR + 40] 
    mov [MBOOT_INFO_ADDR + 88], eax 
    
    xor eax, eax
    mov ax, [VESA_INFO_ADDR + 16]
    mov [MBOOT_INFO_ADDR + 96], eax ; Pitch

    xor eax, eax
    mov ax, [VESA_INFO_ADDR + 18]
    mov [MBOOT_INFO_ADDR + 100], eax ; Width

    xor eax, eax
    mov ax, [VESA_INFO_ADDR + 20]
    mov [MBOOT_INFO_ADDR + 104], eax ; Height

    xor eax, eax
    mov al, [VESA_INFO_ADDR + 25]
    mov [MBOOT_INFO_ADDR + 108], al ; BPP

    jmp .load_kernel

.load_kernel:
    ; Unreal Mode
    cli
    push ds
    push es
    lgdt [gdt_desc]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp $+2
    mov bx, 0x10
    mov ds, bx
    mov es, bx
    mov eax, cr0
    and al, 0xFE
    mov cr0, eax
    pop es
    pop ds
    sti

    ; Read Kernel
    mov edi, KERNEL_ADDR
    mov dword [curr_lba], 1
    mov dword [sectors_left], SECTORS_TOTAL

.read_loop:
    cmp dword [sectors_left], 0
    jle .boot_kernel

    mov word [chunk_size], 64
    mov si, dap
    mov word [dap.count], 64
    mov word [dap.offset], BUFFER_ADDR
    mov word [dap.segment], 0
    mov eax, dword [curr_lba]
    mov dword [dap.lba_low], eax

    mov ah, 0x42
    mov dl, [saved_drive]
    int 0x13
    
    ; Copy to High Mem
    push ds
    push es
    xor ax, ax
    mov ds, ax
    mov esi, BUFFER_ADDR
    mov ecx, 32768
    db 0x67 ; 32-bit addr prefix
    rep movsb
    pop es
    pop ds

    add dword [curr_lba], 64
    sub dword [sectors_left], 64
    jmp .read_loop

.boot_kernel:
    cli
    lgdt [gdt_desc]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp 0x08:pm_entry

; GDT
gdt:
    dq 0
    dw 0xFFFF, 0, 0x9A00, 0x00CF
    dw 0xFFFF, 0, 0x9200, 0x00CF
gdt_end:
gdt_desc:
    dw gdt_end - gdt - 1
    dd gdt

align 4
dap: db 0x10, 0
.count:   dw 0
.offset:  dw 0
.segment: dw 0
.lba_low:  dd 0
.lba_high: dd 0

[bits 32]
pm_entry:
    mov ax, 0x10
    mov ds, ax
    mov ss, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov esp, 0x90000
    
    ; --- FIX: Set Multiboot Magic & Info ---
    mov eax, 0x2BADB002      ; Magic
    mov ebx, MBOOT_INFO_ADDR ; Info Ptr
    
    jmp 0x100000

saved_drive: db 0
chunk_size:  dw 0
curr_lba:    dd 0
sectors_left: dd 0

times 510-($-$$) db 0
dw 0xaa55