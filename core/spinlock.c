// core/spinlock.c
// Kernel spinlock primitives for x86 (32-bit).
// Uses the xchg instruction for atomic test-and-set and
// cli/sti for interrupt-safe variants.

#include "spinlock.h"

// Initialize a spinlock to the unlocked state
void spinlock_init(spinlock_t* lock) {
    lock->locked = 0;
    lock->interrupt_flags = 0;
}

// Acquire the spinlock — busy-wait (spin) until we get it.
// Uses the x86 xchg instruction which implicitly locks the
// bus, providing an atomic test-and-set on the locked field.
void spinlock_acquire(spinlock_t* lock) {
    // xchg exchanges the value in a register with memory atomically.
    // We keep trying to store 1 (locked) and read back the previous
    // value.  If the previous value was 0, we acquired the lock.
    // If it was 1, someone else holds it and we spin.
    while (1) {
        uint32_t prev;
        asm volatile(
            "xchg %0, %1"
            : "=r"(prev), "+m"(lock->locked)
            : "0"(1)           // we want to store 1
            : "memory"
        );
        if (prev == 0) {
            // We got the lock — it was 0, now it's 1
            break;
        }
        // Lock is contended — spin.
        // A pause hint improves performance on hyperthreaded CPUs.
        asm volatile("pause");
    }
}

// Release the spinlock.
// A write barrier ensures all prior stores are visible before
// we mark the lock free, preventing reordering that could let
// another CPU see the lock as released before our critical
// section writes are globally visible.
void spinlock_release(spinlock_t* lock) {
    // Full memory barrier before releasing
    asm volatile("" : : : "memory");
    lock->locked = 0;
}

// IRQ-safe acquire: disable interrupts, save their previous
// state, then acquire the lock.  Returns the saved interrupt
// flags so they can be restored on release.
uint32_t spinlock_irqsave_acquire(spinlock_t* lock) {
    uint32_t flags;

    // Save current interrupt state (EFLAGS.IF bit and others)
    asm volatile("pushfl; popl %0" : "=r"(flags));

    // Disable interrupts
    asm volatile("cli");

    // Acquire the lock (interrupts are now off, so we won't
    // be preempted by an interrupt handler that might try to
    // take the same lock).
    spinlock_acquire(lock);

    return flags;
}

// IRQ-safe release: release the lock, then restore the
// interrupt flags that were saved during acquire.
void spinlock_irqsave_release(spinlock_t* lock, uint32_t flags) {
    // Release the lock first
    spinlock_release(lock);

    // Restore interrupt state (this may re-enable interrupts
    // if they were enabled before the corresponding acquire)
    asm volatile("pushl %0; popfl" : : "r"(flags) : "memory");
}
