; Camel OS Context Switch Routine
; 
; This function performs a context switch between two tasks.
; It saves all registers of the current task and restores
; all registers of the next task.
;
; void context_switch(uint32_t* old_esp_ptr, uint32_t new_esp)
;
; Parameters:
;   old_esp_ptr - Pointer to where to save the current ESP
;   new_esp     - The new stack pointer to switch to
;
; The context (registers) is saved on the stack in the same
; format as the ISR handler, so we can return from interrupt
; to the new task seamlessly.

global context_switch

section .text

context_switch:
    ; Prologue
    push ebp
    mov ebp, esp
    
    ; Save the current ESP to old_esp_ptr
    ; [ebp+8] = old_esp_ptr
    ; [ebp+12] = new_esp
    
    ; Save all general purpose registers
    push eax
    push ebx
    push ecx
    push edx
    push esi
    push edi
    push ebp            ; This is the ebp we pushed at entry
    
    ; Save segment registers (for completeness, though kernel uses flat segments)
    mov ax, ds
    push eax
    mov ax, es
    push eax
    mov ax, fs
    push eax
    mov ax, gs
    push eax
    
    ; Save current ESP (stack pointer after all pushes)
    mov eax, [ebp+8]    ; Get old_esp_ptr
    mov [eax], esp      ; Save current ESP to *old_esp_ptr
    
    ; Now switch to the new stack
    mov esp, [ebp+12]   ; Load new_esp into ESP
    
    ; Restore segment registers
    pop eax
    mov gs, ax
    pop eax
    mov fs, ax
    pop eax
    mov es, ax
    pop eax
    mov ds, ax
    
    ; Restore general purpose registers
    pop ebp
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    pop eax
    
    ; Epilogue - restore ebp and return
    pop ebp
    ret

; Alternative entry point for interrupt-based context switching
; This is called from the timer ISR to perform the actual switch
; 
; uint32_t do_context_switch(uint32_t current_esp, uint32_t next_esp)
;
; Returns: the new ESP to use
global do_context_switch

do_context_switch:
    ; [esp+4] = current_esp (saved ESP of current task)
    ; [esp+8] = next_esp (ESP of next task)
    
    ; Simply return the next_esp - the caller will use it
    ; The registers are already saved on the current stack
    ; by the ISR handler
    
    mov eax, [esp+8]    ; Return next_esp
    ret