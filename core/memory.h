#include "../include/types.h"
#ifndef MEMORY_H
#define MEMORY_H





// Standard block operations
void* memset(void* ptr, int value, size_t num);
void* memcpy(void* destination, const void* source, size_t num);
void* memmove(void* dest, const void* src, size_t n);
int   memcmp(const void* ptr1, const void* ptr2, size_t num);

// Heap Manager (KHeap)
void  init_heap(uint32_t start_address, uint32_t size);
void* kmalloc(size_t size);     // Allocate memory
void* kzalloc(size_t size);     // Allocate and zero-out
void* krealloc(void* ptr, size_t new_size); // Reallocate memory
void  kfree(void* ptr);         // Free memory (Simple stub for now)

// Heap watermark functions for shell
unsigned int k_get_heap_mark();
void k_rewind_heap(unsigned int mark);

// Paging Extensions
void* kmalloc_a(size_t size); // Page aligned
void* kmalloc_ap(size_t size, uint32_t* phys); // Page aligned + physical addr

#endif
