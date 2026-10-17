/*
 * The app's heap. The console's C library heap is far too small for video work (a 4K
 * decoder could not allocate a single frame from it), so malloc and its relatives are
 * defined here on top of the console's direct memory.
 *
 * Layout: ordinary requests are carved from 64 MiB regions with boundary tags and
 * size-binned free lists; a request of 8 MiB or more gets a mapping of its own, returned
 * to the system when freed. One lock guards everything. That is enough for the current
 * users (decoders pool their frames, so allocation is not on the hot path); a
 * thread-caching allocator can replace it behind the same functions if profiling asks.
 *
 * Pointers that did not come from this heap (memory a console module allocated with its
 * own C library) are recognised and left alone by free() and realloc().
 */

#include <errno.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

int64_t sceKernelGetDirectMemorySize(void);
int sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t length,
                                  size_t alignment, int memory_type, int64_t *physical);
int sceKernelMapDirectMemory(void **address, size_t length, int protection, int flags,
                             int64_t physical, size_t alignment);
int sceKernelReleaseDirectMemory(int64_t physical, size_t length);
int sceKernelUsleep(unsigned int microseconds);

#define MEMORY_TYPE 12 /* ordinary read/write memory; the type other homebrew maps here */
#define GRANULE ((size_t)0x4000)
#define REGION_BYTES ((size_t)64 << 20)
#define LARGE_REQUEST ((size_t)8 << 20)
#define MAX_REGIONS 256
#define BIN_COUNT 40

#define FLAG_USED ((size_t)1)
#define FLAG_ALIAS ((size_t)2) /* header of an aligned pointer inside a bigger block */
#define FLAG_LARGE ((size_t)4)
#define FLAG_MASK ((size_t)15)

/* Sits immediately before every pointer this heap hands out. */
struct header
{
    size_t size_flags; /* block size including this header, plus flags */
    size_t previous;   /* size of the block before this one (0: first in its region);
                          for an alias, the distance back to the real pointer;
                          for a large block, its capacity */
};

struct links
{
    struct header *next;
    struct header *previous;
};

/* Start of a large mapping; the block's header follows at offset 48, its memory at 64. */
struct large
{
    struct large *next;
    struct large *previous;
    int64_t physical;
    size_t mapped;
    size_t reserved[2];
};

struct region
{
    uintptr_t start;
    uintptr_t end;
};

static atomic_flag heap_lock = ATOMIC_FLAG_INIT;
static struct header *bins[BIN_COUNT];
static struct region regions[MAX_REGIONS];
static unsigned region_count;
static struct large *large_blocks;
static size_t bytes_in_use;
static size_t bytes_peak;
static size_t bytes_mapped;

static void lock(void)
{
    unsigned spins = 0;
    while (atomic_flag_test_and_set_explicit(&heap_lock, memory_order_acquire))
    {
        if (++spins < 64)
            __builtin_ia32_pause();
        else
            sceKernelUsleep(50);
    }
}

static void unlock(void)
{
    atomic_flag_clear_explicit(&heap_lock, memory_order_release);
}

static size_t block_size(const struct header *block)
{
    return block->size_flags & ~FLAG_MASK;
}

static struct header *next_block(struct header *block)
{
    return (struct header *)((char *)block + block_size(block));
}

static struct links *links_of(struct header *block)
{
    return (struct links *)(block + 1);
}

static unsigned bin_of(size_t size)
{
    unsigned bin = 63u - (unsigned)__builtin_clzll(size); /* floor(log2(size)), size >= 32 */
    bin -= 5;
    return bin < BIN_COUNT ? bin : BIN_COUNT - 1;
}

static void bin_insert(struct header *block)
{
    unsigned bin = bin_of(block_size(block));
    struct links *links = links_of(block);
    links->previous = NULL;
    links->next = bins[bin];
    if (bins[bin] != NULL)
        links_of(bins[bin])->previous = block;
    bins[bin] = block;
}

static void bin_remove(struct header *block)
{
    struct links *links = links_of(block);
    if (links->previous != NULL)
        links_of(links->previous)->next = links->next;
    else
        bins[bin_of(block_size(block))] = links->next;
    if (links->next != NULL)
        links_of(links->next)->previous = links->previous;
}

static void *map_direct(size_t length, int64_t *physical)
{
    void *address = NULL;
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), length, GRANULE,
                                      MEMORY_TYPE, physical) != 0)
        return NULL;
    if (sceKernelMapDirectMemory(&address, length, PROT_READ | PROT_WRITE, 0, *physical,
                                 GRANULE) != 0 ||
        address == NULL)
    {
        sceKernelReleaseDirectMemory(*physical, length);
        return NULL;
    }
    bytes_mapped += length;
    return address;
}

/* Adds one region to the heap as a single free block followed by an end marker. */
static int add_region(void)
{
    if (region_count == MAX_REGIONS)
        return -1;
    int64_t physical;
    char *base = map_direct(REGION_BYTES, &physical);
    if (base == NULL)
        return -1;
    regions[region_count].start = (uintptr_t)base;
    regions[region_count].end = (uintptr_t)base + REGION_BYTES;
    ++region_count;

    struct header *block = (struct header *)base;
    block->size_flags = REGION_BYTES - sizeof(struct header);
    block->previous = 0;
    struct header *end = next_block(block);
    end->size_flags = sizeof(struct header) | FLAG_USED;
    end->previous = block_size(block);
    bin_insert(block);
    return 0;
}

static struct header *take_fitting(size_t need)
{
    for (unsigned bin = bin_of(need); bin < BIN_COUNT; ++bin)
        for (struct header *block = bins[bin]; block != NULL; block = links_of(block)->next)
            if (block_size(block) >= need)
            {
                bin_remove(block);
                return block;
            }
    return NULL;
}

static void *allocate_small(size_t need)
{
    struct header *block = take_fitting(need);
    if (block == NULL)
    {
        if (add_region() != 0)
            return NULL;
        block = take_fitting(need);
        if (block == NULL)
            return NULL;
    }
    size_t size = block_size(block);
    if (size - need >= sizeof(struct header) + sizeof(struct links))
    {
        struct header *rest = (struct header *)((char *)block + need);
        rest->size_flags = size - need;
        rest->previous = need;
        next_block(rest)->previous = size - need;
        bin_insert(rest);
        size = need;
    }
    block->size_flags = size | FLAG_USED;
    bytes_in_use += size;
    if (bytes_in_use > bytes_peak)
        bytes_peak = bytes_in_use;
    return block + 1;
}

static void release_small(struct header *block)
{
    size_t size = block_size(block);
    bytes_in_use -= size;
    struct header *after = next_block(block);
    if (!(after->size_flags & FLAG_USED))
    {
        bin_remove(after);
        size += block_size(after);
    }
    if (block->previous != 0)
    {
        struct header *before = (struct header *)((char *)block - block->previous);
        if (!(before->size_flags & FLAG_USED))
        {
            bin_remove(before);
            size += block_size(before);
            block = before;
        }
    }
    block->size_flags = size;
    next_block(block)->previous = size;
    bin_insert(block);
}

static void *allocate_large(size_t bytes)
{
    size_t mapped = (bytes + 64 + GRANULE - 1) & ~(GRANULE - 1);
    int64_t physical;
    struct large *entry = map_direct(mapped, &physical);
    if (entry == NULL)
        return NULL;
    entry->physical = physical;
    entry->mapped = mapped;
    entry->previous = NULL;
    entry->next = large_blocks;
    if (large_blocks != NULL)
        large_blocks->previous = entry;
    large_blocks = entry;

    struct header *block = (struct header *)((char *)entry + 48);
    block->size_flags = FLAG_LARGE | FLAG_USED;
    block->previous = mapped - 64;
    bytes_in_use += mapped;
    if (bytes_in_use > bytes_peak)
        bytes_peak = bytes_in_use;
    return block + 1;
}

static void release_large(struct header *block)
{
    struct large *entry = (struct large *)((char *)block - 48);
    if (entry->previous != NULL)
        entry->previous->next = entry->next;
    else
        large_blocks = entry->next;
    if (entry->next != NULL)
        entry->next->previous = entry->previous;
    int64_t physical = entry->physical;
    size_t mapped = entry->mapped;
    bytes_in_use -= mapped;
    bytes_mapped -= mapped;
    munmap(entry, mapped);
    sceKernelReleaseDirectMemory(physical, mapped);
}

/* Whether `pointer` lies in memory this heap owns. Call with the lock held. */
static int owned(const void *pointer)
{
    uintptr_t address = (uintptr_t)pointer;
    for (unsigned index = 0; index < region_count; ++index)
        if (address >= regions[index].start && address < regions[index].end)
            return 1;
    for (const struct large *entry = large_blocks; entry != NULL; entry = entry->next)
        if (address > (uintptr_t)entry && address < (uintptr_t)entry + entry->mapped)
            return 1;
    return 0;
}

/* The header of the real block behind `pointer`, following an alias; `*base` receives the
 * pointer that block was handed out as. */
static struct header *resolve(void *pointer, void **base)
{
    struct header *block = (struct header *)pointer - 1;
    if (block->size_flags & FLAG_ALIAS)
    {
        pointer = (char *)pointer - block->previous;
        block = (struct header *)pointer - 1;
    }
    *base = pointer;
    return block;
}

static size_t capacity_of(const struct header *block)
{
    if (block->size_flags & FLAG_LARGE)
        return block->previous;
    return block_size(block) - sizeof(struct header);
}

void *malloc(size_t bytes)
{
    if (bytes > (size_t)1 << 40)
    {
        errno = ENOMEM;
        return NULL;
    }
    size_t need = (bytes + sizeof(struct header) + 15) & ~(size_t)15;
    if (need < sizeof(struct header) + sizeof(struct links))
        need = sizeof(struct header) + sizeof(struct links);
    lock();
    void *result = need >= LARGE_REQUEST ? allocate_large(bytes) : allocate_small(need);
    unlock();
    if (result == NULL)
        errno = ENOMEM;
    return result;
}

void free(void *pointer)
{
    if (pointer == NULL)
        return;
    lock();
    if (owned(pointer))
    {
        void *base;
        struct header *block = resolve(pointer, &base);
        if (block->size_flags & FLAG_LARGE)
            release_large(block);
        else
            release_small(block);
    }
    unlock();
}

size_t malloc_usable_size(void *pointer)
{
    if (pointer == NULL)
        return 0;
    size_t result = 0;
    lock();
    if (owned(pointer))
    {
        void *base;
        struct header *block = resolve(pointer, &base);
        result = capacity_of(block) - (size_t)((char *)pointer - (char *)base);
    }
    unlock();
    return result;
}

void *calloc(size_t count, size_t size)
{
    size_t bytes;
    if (__builtin_mul_overflow(count, size, &bytes))
    {
        errno = ENOMEM;
        return NULL;
    }
    void *result = malloc(bytes);
    if (result != NULL)
        memset(result, 0, bytes);
    return result;
}

void *realloc(void *pointer, size_t bytes)
{
    if (pointer == NULL)
        return malloc(bytes);
    if (bytes == 0)
    {
        free(pointer);
        return NULL;
    }
    lock();
    int ours = owned(pointer);
    unlock();
    if (!ours)
    {
        /* Memory from a console module: its size is unknown, so it cannot be moved. */
        errno = ENOMEM;
        return NULL;
    }
    size_t capacity = malloc_usable_size(pointer);
    if (bytes <= capacity && (capacity < LARGE_REQUEST || bytes >= capacity / 2))
        return pointer;
    void *result = malloc(bytes);
    if (result == NULL)
        return NULL;
    memcpy(result, pointer, bytes < capacity ? bytes : capacity);
    free(pointer);
    return result;
}

void *memalign(size_t alignment, size_t bytes)
{
    if (alignment <= 16)
        return malloc(bytes);
    if ((alignment & (alignment - 1)) != 0 || bytes > (size_t)1 << 40)
    {
        errno = EINVAL;
        return NULL;
    }
    char *raw = malloc(bytes + alignment + sizeof(struct header));
    if (raw == NULL)
        return NULL;
    /* Leave room for an alias header between the real pointer and the aligned one. */
    uintptr_t aligned =
        ((uintptr_t)raw + sizeof(struct header) + alignment - 1) & ~(uintptr_t)(alignment - 1);
    struct header *alias = (struct header *)aligned - 1;
    alias->size_flags = FLAG_ALIAS | FLAG_USED;
    alias->previous = aligned - (uintptr_t)raw;
    return (void *)aligned;
}

int posix_memalign(void **result, size_t alignment, size_t bytes)
{
    if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0)
        return EINVAL;
    void *pointer = memalign(alignment, bytes);
    if (pointer == NULL)
        return ENOMEM;
    *result = pointer;
    return 0;
}

void *aligned_alloc(size_t alignment, size_t bytes)
{
    return memalign(alignment, bytes);
}

char *strdup(const char *text)
{
    size_t length = strlen(text) + 1;
    char *copy = malloc(length);
    if (copy != NULL)
        memcpy(copy, text, length);
    return copy;
}

char *strndup(const char *text, size_t limit)
{
    size_t length = 0;
    while (length < limit && text[length] != 0)
        ++length;
    char *copy = malloc(length + 1);
    if (copy != NULL)
    {
        memcpy(copy, text, length);
        copy[length] = 0;
    }
    return copy;
}

/* Bytes handed out now, the most ever handed out, and bytes mapped from the system. */
void app_heap_stats(size_t *in_use, size_t *peak, size_t *mapped)
{
    lock();
    *in_use = bytes_in_use;
    *peak = bytes_peak;
    *mapped = bytes_mapped;
    unlock();
}
