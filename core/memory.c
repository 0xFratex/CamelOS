// core/memory.c
#include "types.h"
#include "spinlock.h"

// API
void* memset(void* ptr, int value, size_t num);
void* memcpy(void* destination, const void* source, size_t num);
void  init_heap(uint32_t start_address, uint32_t size);
void* kmalloc(size_t size);
void* kzalloc(size_t size);
void* krealloc(void* ptr, size_t new_size);
void  kfree(void* ptr);

// Monitoring
uint32_t k_get_free_mem(void);
uint32_t k_get_total_mem(void);

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

// Global heap lock — protects the free-list and bookkeeping
// from concurrent access by interrupt handlers or other CPUs.
spinlock_t g_heap_lock = SPINLOCK_INIT;

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
    extern void s_printf(const char* fmt, ...);
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

    uint32_t flags = spinlock_irqsave_acquire(&g_heap_lock);

    // Align size to 16 bytes
    if (size % 16 != 0) size += 16 - (size % 16);

    mem_block_t* curr;
    mem_block_t* best_fit;
    size_t best_size_diff;

    // Try up to 2 passes: first normal search, then after coalescing
    for (int pass = 0; pass < 2; pass++) {
        curr = heap_head;
        best_fit = 0;
        best_size_diff = 0xFFFFFFFF;

        // Find Best Fit
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

        // Found a block — proceed
        if (best_fit) break;

        // No block found — coalesce and retry once
        if (pass == 0) {
            coalesce_heap();
        }
    }

    // No suitable block found even after coalescing
    if (!best_fit) {
        spinlock_irqsave_release(&g_heap_lock, flags);
        return 0;
    }

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

    spinlock_irqsave_release(&g_heap_lock, flags);
    return (void*)((uint8_t*)curr + sizeof(mem_block_t));
}

void* kzalloc(size_t size) { return kmalloc(size); }

void kfree(void* ptr) {
    if (!ptr) return;

    uint32_t flags = spinlock_irqsave_acquire(&g_heap_lock);

    mem_block_t* block = (mem_block_t*)((uint8_t*)ptr - sizeof(mem_block_t));

    // 1. Header Corruption Check
    if (block->magic != MEM_MAGIC) {
        extern void s_printf(const char* fmt, ...);
        s_printf("[MEM] CRITICAL: Header corruption detected in kfree!\n");
        spinlock_irqsave_release(&g_heap_lock, flags);
        return; 
    }

    // 2. Guard Byte Check
    mem_guard_t* guard = (mem_guard_t*)((uint8_t*)ptr + block->size);
    if (guard->guard != GUARD_MAGIC) {
        extern void s_printf(const char* fmt, ...);
        s_printf("[MEM] CRITICAL: Buffer Overflow detected (Guard corrupted)!\n");
        // In a real OS, this might panic the specific process
    }

    if (!block->free) {
        block->free = 1;
        used_mem_size -= block->actual_size;
        coalesce_heap();
    }

    spinlock_irqsave_release(&g_heap_lock, flags);
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
        size_t next_actual = block->next->actual_size;
        size_t available = current_capacity + sizeof(mem_block_t) + next_actual;
        if (available >= new_size) {
            // Merge and claim — save next->next BEFORE unlinking to avoid use-after-free
            mem_block_t* next_next = block->next->next;
            block->actual_size += (sizeof(mem_block_t) + next_actual);
            block->next = next_next;

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
//
// Layout when splitting is needed:
//   [leading free block] [aligned block header] [aligned data ...] [trailing free block]
//
// BUG FIX: The previous version had two critical bugs:
//   1. The lead block's guard was placed at `aligned_ptr`, overlapping with
//      the aligned block's data.  Writing page table entries overwrote the
//      guard, causing "Buffer Overflow detected" in kfree() and corrupting
//      the heap free list.
//   2. When leading_size < sizeof(mem_block_t) (20 bytes), the aligned
//      block header at `aligned_ptr - 20` fell inside the original block's
//      own header, corrupting its magic/next pointers.
//
// The fix: only create a leading free block when there is enough space
// for a proper header + guard BETWEEN the original data start and the
// aligned block header.  Otherwise, fold the leading bytes into the
// aligned block's allocation (wasting at most ~4080 bytes, which is
// acceptable for rare page-table allocations).
void* kmalloc_ap(size_t size, uint32_t* phys) {
    // Align requested size to 16 bytes
    if (size % 16 != 0) size += 16 - (size % 16);

    // We need size bytes aligned at a 4096 boundary, plus room for a
    // block header + guard before the aligned region, so request size+4096.
    size_t actual_req = size + 4096;
    uint32_t ptr = (uint32_t)kmalloc(actual_req);

    if (ptr == 0) return 0;

    // Compute the 4096-aligned address within this allocation
    uint32_t aligned_ptr = ptr;
    if (aligned_ptr % 4096 != 0) {
        aligned_ptr += 4096 - (aligned_ptr % 4096);
    }

    // If already perfectly aligned, no splitting needed
    if (aligned_ptr == ptr) {
        if (phys) {
            *phys = aligned_ptr;
        }
        return (void*)aligned_ptr;
    }

    // Misaligned allocation — need to split.
    //
    // The aligned block header must be at (aligned_ptr - sizeof(mem_block_t)).
    // We need to ensure this doesn't overlap with the original block header
    // or the leading free block's data/guard.
    //
    // Layout:
    //   orig_header  lead_data  lead_guard  aligned_header  aligned_data  aligned_guard  [trail]
    //   |<-- lead region -->|               |<-- aligned block -->|

    mem_block_t* orig_block = (mem_block_t*)(ptr - sizeof(mem_block_t));
    if (orig_block->magic != MEM_MAGIC) {
        // Safety: if header is corrupt, just return aligned pointer without splitting
        if (phys) *phys = aligned_ptr;
        return (void*)aligned_ptr;
    }

    size_t orig_actual = orig_block->actual_size;
    mem_block_t* orig_next = orig_block->next;

    // The aligned block header starts here:
    uint32_t aligned_header_addr = aligned_ptr - sizeof(mem_block_t);

    // Space between the original data start (ptr) and the aligned block header.
    // This is where the leading free block's data + guard must fit.
    size_t leading_space = aligned_header_addr - ptr;

    // --- Aligned allocated block (always created) ---
    mem_block_t* aligned_block = (mem_block_t*)aligned_header_addr;
    aligned_block->size = size;
    aligned_block->free = 0;
    aligned_block->magic = MEM_MAGIC;

    // --- Trailing free block (if enough space) ---
    // Calculate total bytes from aligned_block header to end of original allocation
    uint32_t orig_end = (uint32_t)orig_block + sizeof(mem_block_t) + orig_actual;
    size_t aligned_block_total = (uint32_t)orig_end - aligned_header_addr;

    // Space after aligned data for a trailing free block
    size_t trailing_space = aligned_block_total - sizeof(mem_block_t) - size;

    if (trailing_space > BLOCK_META_SIZE + 32) {
        // Enough space for a trailing free block
        mem_block_t* trail = (mem_block_t*)(aligned_ptr + size + sizeof(mem_guard_t));
        trail->size = trailing_space - BLOCK_META_SIZE;
        trail->actual_size = trailing_space - sizeof(mem_block_t);
        trail->free = 1;
        trail->magic = MEM_MAGIC;
        trail->next = orig_next;
        mem_guard_t* trail_guard = (mem_guard_t*)((uint8_t*)trail + sizeof(mem_block_t) + trail->size);
        trail_guard->guard = GUARD_MAGIC;

        aligned_block->actual_size = size + sizeof(mem_guard_t);
        aligned_block->next = trail;
    } else {
        // Not enough trailing space — fold it into the aligned block
        aligned_block->actual_size = aligned_block_total - sizeof(mem_block_t);
        aligned_block->next = orig_next;
    }

    // Guard at end of aligned data
    mem_guard_t* aligned_guard = (mem_guard_t*)(aligned_ptr + size);
    aligned_guard->guard = GUARD_MAGIC;

    // --- Leading free block ---
    // Only create a leading free block if there's enough space for a
    // proper block with header + data + guard that doesn't overlap with
    // the aligned block header.
    //
    // The lead block's guard must be placed at (ptr + lead->size),
    // which must be BEFORE aligned_header_addr.
    // We need: lead->size + sizeof(mem_guard_t) <= leading_space
    // And:  lead->size >= 16 (minimum useful data)
    // So:   leading_space >= 16 + sizeof(mem_guard_t) = 20

    if (leading_space >= 16 + sizeof(mem_guard_t)) {
        // Create a proper leading free block
        orig_block->size = leading_space - sizeof(mem_guard_t);
        orig_block->actual_size = leading_space;
        orig_block->free = 1;
        orig_block->next = aligned_block;
        // Guard at end of leading block's data (BEFORE aligned block header)
        mem_guard_t* lead_guard = (mem_guard_t*)(ptr + orig_block->size);
        lead_guard->guard = GUARD_MAGIC;

        // Adjust used_mem_size: the leading block is now free
        used_mem_size -= orig_block->actual_size;
    } else {
        // Not enough leading space for a proper free block.
        // Fold the leading bytes into the aligned block by extending
        // its actual_size back to cover from the original header.
        // The original block header is abandoned (invalidated) since
        // kfree(aligned_ptr) will find the aligned_block header, not orig_block.
        //
        // This wastes at most ~4080 bytes (one page), which is acceptable
        // for page table allocations that happen rarely during boot.
        aligned_block->actual_size = (uint32_t)orig_end - aligned_header_addr - sizeof(mem_block_t);
        aligned_block->next = orig_next;

        // Invalidate the original block header so no one tries to free it
        orig_block->magic = 0;
        orig_block->free = 0;
        orig_block->next = aligned_block;

        // Update heap_head if it was pointing at orig_block
        if (heap_head == orig_block) {
            heap_head = aligned_block;
        } else {
            // Find the block that points to orig_block and update its next pointer
            mem_block_t* prev = heap_head;
            while (prev && prev->next != orig_block) {
                prev = prev->next;
            }
            if (prev) {
                prev->next = aligned_block;
            }
        }

        // Move the aligned guard to the real end
        mem_guard_t* g = (mem_guard_t*)((uint8_t*)aligned_block + sizeof(mem_block_t) + aligned_block->actual_size - sizeof(mem_guard_t));
        g->guard = GUARD_MAGIC;
    }

    if (phys) {
        *phys = aligned_ptr;
    }

    return (void*)aligned_ptr;
}

// Allocate aligned to 4KB (page size)
void* kmalloc_a(size_t size) {
    return kmalloc_ap(size, 0);
}

/* Returns 1 if ptr points into a live (allocated, not freed) heap block.
 * Use this to validate pointers before memcpy/strcpy in the resource
 * loader so a wild pointer trips a log line instead of a page fault. */
int kheap_contains(const void* ptr) {
    if (!ptr) return 0;
    const mem_block_t* b = (const mem_block_t*)((const uint8_t*)ptr - sizeof(mem_block_t));
    if (b->magic != MEM_MAGIC) return 0;   /* not a heap block */
    if (b->free)                 return 0;   /* use-after-free  */
    if ((const uint8_t*)ptr <  (const uint8_t*)b + sizeof(mem_block_t)) return 0;
    if ((const uint8_t*)ptr >= (const uint8_t*)b + sizeof(mem_block_t) + b->size) return 0;
    return 1;
}