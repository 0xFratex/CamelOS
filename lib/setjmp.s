# lib/setjmp.s - i386 setjmp/longjmp implementation for CamelOS
# Based on the standard i386 setjmp/longjmp ABI
#
# jmp_buf layout (6 ints = 24 bytes):
#   [0] = ebx
#   [1] = esi
#   [2] = edi
#   [3] = ebp
#   [4] = esp
#   [5] = eip (return address)

.section .text
.globl setjmp
.type setjmp, @function
setjmp:
    movl    4(%esp), %eax       # eax = jmp_buf pointer
    movl    %ebx, 0(%eax)       # save ebx
    movl    %esi, 4(%eax)       # save esi
    movl    %edi, 8(%eax)       # save edi
    movl    %ebp, 12(%eax)      # save ebp
    movl    %esp, 16(%eax)      # save esp
    # Get return address
    movl    (%esp), %ecx
    movl    %ecx, 20(%eax)      # save eip
    xorl    %eax, %eax          # return 0
    ret

.globl longjmp
.type longjmp, @function
longjmp:
    movl    4(%esp), %eax       # eax = jmp_buf pointer
    movl    8(%esp), %edx       # edx = val
    # Restore registers
    movl    0(%eax), %ebx
    movl    4(%eax), %esi
    movl    8(%eax), %edi
    movl    12(%eax), %ebp
    movl    16(%eax), %esp
    # Get saved eip
    movl    20(%eax), %ecx
    # Make sure val != 0
    testl   %edx, %edx
    jnz     1f
    incl    %edx                # if val == 0, make it 1
1:
    movl    %edx, %eax          # return val
    jmp     *%ecx               # jump to saved eip

.section .note.GNU-stack, "", @progbits
