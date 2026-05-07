#ifndef SPINLOCK_H
#define SPINLOCK_H

#include "../include/types.h"

// Kernel spinlock primitive using x86 atomic test-and-set.
// Protects shared data structures from concurrent access,
// including access from interrupt handlers.
//
// Usage:
//   spinlock_t my_lock = SPINLOCK_INIT;
//   // Or: spinlock_t my_lock; spinlock_init(&my_lock);
//
//   // For code that may run in interrupt context or share
//   // data with interrupt handlers:
//   uint32_t flags = spinlock_irqsave_acquire(&my_lock);
//   ... critical section ...
//   spinlock_irqsave_release(&my_lock, flags);
//
//   // For code that never shares data with interrupt handlers:
//   spinlock_acquire(&my_lock);
//   ... critical section ...
//   spinlock_release(&my_lock);

typedef struct {
    volatile uint32_t locked;        // 0 = free, 1 = held
    uint32_t interrupt_flags;        // Saved interrupt state for irqsave variants
} spinlock_t;

#define SPINLOCK_INIT { 0, 0 }

// Initialize a spinlock to the unlocked state
void spinlock_init(spinlock_t* lock);

// Acquire the spinlock (busy-wait until available).
// Does NOT disable interrupts — use only when interrupt
// safety is not required.
void spinlock_acquire(spinlock_t* lock);

// Release the spinlock.
void spinlock_release(spinlock_t* lock);

// IRQ-safe acquire: disables interrupts before acquiring,
// returns the previous interrupt state for later restoration.
uint32_t spinlock_irqsave_acquire(spinlock_t* lock);

// IRQ-safe release: releases the lock then restores the
// saved interrupt flags.
void spinlock_irqsave_release(spinlock_t* lock, uint32_t flags);

#endif // SPINLOCK_H
