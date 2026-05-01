; core/objc_msgSend.asm - Objective-C message dispatch (i386 cdecl)
;
; This implements objc_msgSend and objc_msgSendSuper in pure assembly
; so that variadic arguments are properly forwarded to the method IMP.
;
; i386 cdecl calling convention:
;   All args pushed on stack right-to-left
;   Return value in EAX
;   Caller cleans up stack (cdecl)
;
; Stack layout on entry to objc_msgSend:
;   [esp+0]  = return address
;   [esp+4]  = self (id)
;   [esp+8]  = op (SEL)
;   [esp+12] = first arg (if any)
;   [esp+16] = second arg (if any)
;   ...

section .text

; External C functions we call
extern _objc_lookupMethod
extern _sel_registerName

; -------------------------------------------------------
; id objc_msgSend(id self, SEL op, ...)
; -------------------------------------------------------
global _objc_msgSend
_objc_msgSend:
    ; Prologue
    push    ebp
    mov     ebp, esp
    push    esi
    push    edi
    
    ; self = [ebp+8], op = [ebp+12]
    mov     esi, [ebp+8]        ; esi = self
    mov     edi, [ebp+12]       ; edi = op (selector)
    
    ; Check for nil self (return 0 on nil)
    test    esi, esi
    jz      .nil_return
    
    ; Get isa (class pointer) from self->isa
    mov     eax, [esi]          ; eax = self->isa (class pointer)
    
    ; Call objc_lookupMethod(class, selector) to find the IMP
    push    edi                 ; selector
    push    eax                 ; class
    call    _objc_lookupMethod
    add     esp, 8              ; clean up cdecl args
    
    ; EAX now contains the method IMP (function pointer)
    test    eax, eax
    jz      .method_not_found
    
    ; Tail-call the IMP, passing through all original args
    ; Restore saved regs first
    pop     edi
    pop     esi
    pop     ebp
    
    ; Jump to IMP. The original args (self, op, ...) are still on the stack
    ; because we only modified ebp/esi/edi (callee-saved).
    ; The IMP will see: [esp+4]=self, [esp+8]=op, [esp+12]=arg1, ...
    jmp     eax
    
.nil_return:
    xor     eax, eax            ; return 0 for nil self
    pop     edi
    pop     esi
    pop     ebp
    ret

.method_not_found:
    ; Method not found - return 0
    ; In a full runtime this would call forwardInvocation: or doesNotRecognizeSelector:
    xor     eax, eax
    pop     edi
    pop     esi
    pop     ebp
    ret

; -------------------------------------------------------
; id objc_msgSendSuper(struct objc_super* super, SEL op, ...)
; -------------------------------------------------------
global _objc_msgSendSuper
_objc_msgSendSuper:
    ; Prologue
    push    ebp
    mov     ebp, esp
    push    esi
    push    edi
    
    ; super = [ebp+8], op = [ebp+12]
    mov     esi, [ebp+8]        ; esi = super struct pointer
    mov     edi, [ebp+12]       ; edi = op (selector)
    
    ; Get the superclass from the super struct
    ; struct objc_super { id receiver; Class class; }
    mov     eax, [esi+4]        ; eax = super->class (superclass to start lookup from)
    
    ; Call objc_lookupMethod(superclass, selector)
    push    edi                 ; selector
    push    eax                 ; superclass
    call    _objc_lookupMethod
    add     esp, 8
    
    ; EAX = method IMP
    test    eax, eax
    jz      .super_method_not_found
    
    ; Tail-call the IMP, but replace self with super->receiver
    ; We need to patch [ebp+8] (self arg) to be super->receiver
    mov     ecx, [esi]          ; ecx = super->receiver
    mov     [ebp+8], ecx        ; patch the self argument on stack
    
    pop     edi
    pop     esi
    pop     ebp
    
    jmp     eax                 ; tail call to IMP

.super_method_not_found:
    xor     eax, eax
    pop     edi
    pop     esi
    pop     ebp
    ret
