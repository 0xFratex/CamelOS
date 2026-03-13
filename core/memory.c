// core/memory.c
#ifndef MEMORY_H
#define MEMORY_H

// Basic types for freestanding environment
typedef unsigned char  uint8_t;
typedef unsigned int   uint32_t;
typedef unsigned long  size_t;

#ifndef NULL
#define NULL ((void*)0)
#endif

// API
void* memset(void* ptr, int value, size_t num);
void* memcpy(void* destination, const void* source, size_t num);
void  init_heap(uint32_t start_address, uint32_t size);
void* kmalloc(size_t size);
void* kzalloc(size_t size);
void* krealloc(void* ptr, size_t new_size); //
void  kfree(void* ptr);

// Monitoring
uint32_t k_get_free_mem(void);
uint32_t k_get_total_mem(void);

#endif 

// --- Implementation ---

// Utils
void* memset(void* ptr, int value, size_t num) {
    unsigned char* p = (unsigned char*)ptr;
    while (num--) *p++ = (unsigned char)value;
    return ptr;
}

void* memcpy(void* destination, const void* source, size_t num) {
    unsigned char* d = (unsigned char*)destination;
    const unsigned char* s = (const unsigned char*)source;
    while (num--) *d++ = *s++;
    return destination;
}

// --- Heap Allocator (Enhanced) ---

#define MEM_MAGIC       0xDEADBEEF
#define GUARD_MAGIC     0xCAFEBABE //

typedef struct mem_block {
    size_t size;            // Size of user data requested
    size_t actual_size;     // Total size including alignment/padding
    int free;
    struct mem_block* next;
    uint32_t magic;         // Header corruption check
} mem_block_t;

// Trailer to detect buffer overflows
typedef struct {
    uint32_t guard; 
} mem_guard_t;

// Ensure the header size is 16-byte aligned
#define ALIGN_16(x) (((x) + 15) & ~15)
#define BLOCK_META_SIZE ALIGN_16(sizeof(mem_block_t) + sizeof(mem_guard_t))

static mem_block_t* heap_head = 0;
static uint32_t total_mem_size = 0;
static uint32_t used_mem_size = 0;

void init_heap(uint32_t start_address, uint32_t size) {
    // 16-byte alignment
    if (start_address % 16 != 0) start_address += 16 - (start_address % 16);

    heap_head = (mem_block_t*)start_address;
    // Calculate usable size subtracting metadata
    heap_head->size = size - BLOCK_META_SIZE; 
    heap_head->actual_size = size - sizeof(mem_block_t); 
    heap_head->free = 1;
    heap_head->next = 0;
    heap_head->magic = MEM_MAGIC;

    total_mem_size = size;
    used_mem_size = 0;
    
    // Logging (assumes s_printf exists)
    extern void s_printf(const char*);
    s_printf("[MEM] Enhanced Heap Initialized (Guard Bytes Enabled)\n");
}

void coalesce_heap() {
    mem_block_t* curr = heap_head;
    while (curr && curr->next) {
        if (curr->free && curr->next->free) {
            // Merge: Add next block's total size (header + data + guard)
            size_t total_next_space = sizeof(mem_block_t) + curr->next->actual_size;
            curr->actual_size += total_next_space;
            // Update user size capability (optional, usually set on alloc)
            curr->size += total_next_space; 
            
            curr->next = curr->next->next;
        } else {
            curr = curr->next;
        }
    }
}

void* kmalloc(size_t size) {
    if (size == 0) return 0;

    // Align size to 16 bytes
    if (size % 16 != 0) size += 16 - (size % 16);

    mem_block_t* curr = heap_head;
    mem_block_t* best_fit = 0;
    size_t best_size_diff = 0xFFFFFFFF;

    // Pass 1: Find Best Fit
    while (curr) {
        if (curr->free && curr->actual_size >= (size + sizeof(mem_guard_t))) {
            size_t diff = curr->actual_size - (size + sizeof(mem_guard_t));

            // Exact match optimization
            if (diff == 0) {
                best_fit = curr;
                break;
            }

            if (diff < best_size_diff) {
                best_fit = curr;
                best_size_diff = diff;
            }
        }
        curr = curr->next;
    }

    // No suitable block found
    if (!best_fit) return 0;

    // Allocation Logic on best_fit
    curr = best_fit;

    // Split block if large enough (Threshold: Metadata + 32 bytes usable)
    if (curr->actual_size > size + sizeof(mem_guard_t) + BLOCK_META_SIZE + 32) {

        size_t split_offset = sizeof(mem_block_t) + size + sizeof(mem_guard_t);
        mem_block_t* next_block = (mem_block_t*)((uint8_t*)curr + split_offset);

        next_block->magic = MEM_MAGIC;
        next_block->free = 1;
        // Remaining space calculation
        next_block->actual_size = curr->actual_size - split_offset;
        next_block->size = next_block->actual_size - sizeof(mem_guard_t);
        next_block->next = curr->next;

        curr->actual_size = size + sizeof(mem_guard_t);
        curr->next = next_block;
    }

    curr->free = 0;
    curr->size = size;
    used_mem_size += curr->actual_size;

    // Set Guard Byte
    mem_guard_t* guard = (mem_guard_t*)((uint8_t*)curr + sizeof(mem_block_t) + size);
    guard->guard = GUARD_MAGIC;

    // Zero memory for security
    memset((void*)((uint8_t*)curr + sizeof(mem_block_t)), 0, size);

    return (void*)((uint8_t*)curr + sizeof(mem_block_t));
}

void* kzalloc(size_t size) { return kmalloc(size); }

void kfree(void* ptr) {
    if (!ptr) return;

    mem_block_t* block = (mem_block_t*)((uint8_t*)ptr - sizeof(mem_block_t));

    // 1. Header Corruption Check
    if (block->magic != MEM_MAGIC) {
        extern void s_printf(const char*);
        s_printf("[MEM] CRITICAL: Header corruption detected in kfree!\n");
        return; 
    }

    // 2. Guard Byte Check
    mem_guard_t* guard = (mem_guard_t*)((uint8_t*)ptr + block->size);
    if (guard->guard != GUARD_MAGIC) {
        extern void s_printf(const char*);
        s_printf("[MEM] CRITICAL: Buffer Overflow detected (Guard corrupted)!\n");
        // In a real OS, this might panic the specific process
    }

    if (!block->free) {
        block->free = 1;
        used_mem_size -= block->actual_size;
        coalesce_heap();
    }
}

//
void* krealloc(void* ptr, size_t new_size) {
    if (!ptr) return kmalloc(new_size);
    if (new_size == 0) { kfree(ptr); return 0; }

    mem_block_t* block = (mem_block_t*)((uint8_t*)ptr - sizeof(mem_block_t));
    if (block->magic != MEM_MAGIC) return 0;

    // 1. Align new size
    if (new_size % 16 != 0) new_size += 16 - (new_size % 16);

    // 2. Check if we can just expand in place
    // Available space in this block = actual_size - guard
    size_t current_capacity = block->actual_size - sizeof(mem_guard_t);
    
    if (new_size <= current_capacity) {
        // Shrinking or same size: Just update size and move guard
        block->size = new_size;
        mem_guard_t* guard = (mem_guard_t*)((uint8_t*)ptr + new_size);
        guard->guard = GUARD_MAGIC;
        return ptr;
    }

    // 3. Try to merge with next block if it is free
    if (block->next && block->next->free) {
        size_t available = current_capacity + sizeof(mem_block_t) + block->next->actual_size;
        if (available >= new_size) {
            // Merge and claim
            block->next = block->next->next; // Unlink next
            block->actual_size += (sizeof(mem_block_t) + block->next->actual_size);
            
            // Now set new boundaries
            block->size = new_size;
            mem_guard_t* guard = (mem_guard_t*)((uint8_t*)ptr + new_size);
            guard->guard = GUARD_MAGIC;
            return ptr;
        }
    }

    // 4. Fallback: Malloc + Copy + Free
    void* new_ptr = kmalloc(new_size);
    if (!new_ptr) return 0;
    
    memcpy(new_ptr, ptr, block->size);
    kfree(ptr);
    
    return new_ptr;
}

uint32_t k_get_free_mem() { return total_mem_size - used_mem_size; }
uint32_t k_get_total_mem() { return total_mem_size; }

// Heap watermark functions for shell
static uint32_t heap_watermark = 0;

unsigned int k_get_heap_mark() {
    // Store current heap usage as watermark
    heap_watermark = used_mem_size;
    return heap_watermark;
}

void k_rewind_heap(unsigned int mark) {
    // Rewind heap to the watermark by freeing allocations made after the mark
    // This is a simplified approach - in a real system we'd need to track individual allocations
    if (mark < used_mem_size) {
        // We can't easily rewind individual allocations, so we'll just reset the heap
        // This is a simplified approach for the shell's use case
        used_mem_size = mark;

        // Reset heap to initial state
        if (heap_head) {
            heap_head->free = 1;
            heap_head->size = total_mem_size - BLOCK_META_SIZE;
            heap_head->actual_size = total_mem_size - sizeof(mem_block_t);
            heap_head->next = 0;
        }
    }
}

// --- Added for Paging Support ---

// Allocate aligned to 4KB and return physical address
// Since we don't have VM separation yet, Phy = Virt
void* kmalloc_ap(size_t size, uint32_t* phys) {
    // Simple alignment hack: alloc size + 4096, find alignment point
    // This wastes memory but works without rewriting the heap block allocator entirely

    // Only 16-byte alignment is supported natively by init_heap structure.
    // We cheat by allocating extra.

    size_t actual_req = size + 4096;
    uint32_t ptr = (uint32_t)kmalloc(actual_req);

    if (ptr == 0) return 0;

    uint32_t aligned_ptr = ptr;
    if (aligned_ptr % 4096 != 0) {
        aligned_ptr += 4096 - (aligned_ptr % 4096);
    }

    if (phys) {
        *phys = aligned_ptr; // Identity map assumption for now
    }

    return (void*)aligned_ptr;
}

// Allocate aligned to 4KB (page size)
void* kmalloc_a(size_t size) {
    return kmalloc_ap(size, 0);
}
