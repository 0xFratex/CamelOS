; i386 setjmp/longjmp implementation for CamelOS
; jmp_buf layout: ebx, esi, edi, ebp, esp, eip

global setjmp
setjmp:
    mov     eax, [esp+4]    ; get jmp_buf pointer
    mov     [eax], ebx      ; save ebx
    mov     [eax+4], esi    ; save esi
    mov     [eax+8], edi    ; save edi
    mov     [eax+12], ebp   ; save ebp
    lea     ecx, [esp+4]    ; save esp (pointing after return address)
    mov     [eax+16], ecx
    mov     ecx, [esp]      ; save return address
    mov     [eax+20], ecx
    xor     eax, eax        ; return 0
    ret

global longjmp
longjmp:
    mov     edx, [esp+4]    ; get jmp_buf pointer
    mov     eax, [esp+8]    ; get return value
    
    ; return 1 if value is 0
    test    eax, eax
    jnz     .ok
    inc     eax
.ok:

    ; Restore registers
    mov     ebx, [edx]
    mov     esi, [edx+4]
    mov     edi, [edx+8]
    mov     ebp, [edx+12]
    mov     esp, [edx+16]
    jmp     [edx+20]        ; jump to saved eip
