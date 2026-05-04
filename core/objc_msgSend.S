# core/objc_msgSend.S - Objective-C message dispatch (i386 cdecl, ELF)
# Converted from NASM to GAS (AT&T) syntax for use with 'as' assembler

.section .text

.extern objc_lookupMethod
.extern sel_registerName

.globl objc_msgSend
objc_msgSend:
    pushl   %ebp
    movl    %esp, %ebp
    pushl   %esi
    pushl   %edi

    movl    8(%ebp), %esi       # esi = self
    movl    12(%ebp), %edi      # edi = op (selector)

    testl   %esi, %esi
    jz      .nil_return

    movl    (%esi), %eax        # eax = self->isa (class pointer)

    pushl   %edi                # selector
    pushl   %eax                # class
    call    objc_lookupMethod
    addl    $8, %esp            # clean up cdecl args

    testl   %eax, %eax
    jz      .method_not_found

    popl    %edi
    popl    %esi
    popl    %ebp
    jmp     *%eax               # tail call to IMP

.nil_return:
    xorl    %eax, %eax
    popl    %edi
    popl    %esi
    popl    %ebp
    ret

.method_not_found:
    xorl    %eax, %eax
    popl    %edi
    popl    %esi
    popl    %ebp
    ret

.globl objc_msgSendSuper
objc_msgSendSuper:
    pushl   %ebp
    movl    %esp, %ebp
    pushl   %esi
    pushl   %edi

    movl    8(%ebp), %esi       # esi = super struct pointer
    movl    12(%ebp), %edi      # edi = op (selector)

    movl    4(%esi), %eax       # eax = super->class (superclass)

    pushl   %edi                # selector
    pushl   %eax                # superclass
    call    objc_lookupMethod
    addl    $8, %esp

    testl   %eax, %eax
    jz      .super_method_not_found

    movl    (%esi), %ecx        # ecx = super->receiver
    movl    %ecx, 8(%ebp)       # patch the self argument on stack

    popl    %edi
    popl    %esi
    popl    %ebp
    jmp     *%eax               # tail call to IMP

.super_method_not_found:
    xorl    %eax, %eax
    popl    %edi
    popl    %esi
    popl    %ebp
    ret

.section .note.GNU-stack, "", @progbits
