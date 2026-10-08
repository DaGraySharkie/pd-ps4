// PS4: give the C heap its own block of memory.
//
// The OpenOrbis C library (musl) takes all of its heap from anonymous mmap(). On PS4 those
// mappings come out of the application's flexible memory (~255 MiB), which is also where Piglet
// keeps textures and render targets when it is allowed to. mmap()/munmap() are therefore
// replaced here: anonymous read/write mappings are carved out of one arena, everything else is
// passed to the kernel.
//
// The arena comes from system flexible memory if the process may map it (a separate pool, which
// leaves all of the direct memory to Piglet), otherwise from direct memory.
//
// Same idea as Ps4Heap.c in alechurri/shipofharkinian-ps4; this is a separate, smaller
// implementation sized for Perfect Dark.

#include "platform.h"

#ifdef PLATFORM_PS4

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>

#include <orbis/libkernel.h>

#define HEAP_PAGE 0x4000ULL                    // 16 KiB, the PS4 page size
#define HEAP_ALIGN 0x200000ULL                 // 2 MiB, direct memory allocation alignment
#define HEAP_SYSFLEX_SIZE (512ULL << 20)       // arena size when system flexible memory works
#define HEAP_DIRECT_MAX (512ULL << 20)         // direct memory arena: try this, halve down to...
#define HEAP_DIRECT_MIN (128ULL << 20)         // ...this
#define HEAP_LARGE_PAGES 64                    // >= 1 MiB: allocate top-down
#define HEAP_MAP_FIXED 0x10

// Declared with the wrong prototype by the OpenOrbis headers.
int32_t ps4heapMapNamedSystemFlexibleMemory(void **addr, size_t len, int32_t prot, int32_t flags, const char *name)
	__asm__("sceKernelMapNamedSystemFlexibleMemory");

static uint8_t *arenaBase;
static size_t arenaPages;
static uint8_t *pageMap;  // one byte per page, 1 = used; stored in the arena's first pages
static size_t searchHint;
static size_t pagesUsed;
static size_t pagesPeak;
static int initDone;
static int arenaKind;     // 0 = none, 1 = system flexible, 2 = direct
static volatile int heapLock;

static inline void ps4heapLock(void)
{
	while (__sync_lock_test_and_set(&heapLock, 1)) {
		while (heapLock) {
			__builtin_ia32_pause();
		}
	}
}

static inline void ps4heapUnlock(void)
{
	__sync_lock_release(&heapLock);
}

static void ps4heapInit(void)
{
	initDone = 1;

	size_t size = HEAP_SYSFLEX_SIZE;
	void *addr = NULL;
	if (sceKernelReserveVirtualRange(&addr, size, 0, HEAP_PAGE) == 0 && addr) {
		if (ps4heapMapNamedSystemFlexibleMemory(&addr, size, ORBIS_KERNEL_PROT_CPU_RW, HEAP_MAP_FIXED, "pd heap") == 0) {
			arenaBase = (uint8_t *)addr;
			arenaKind = 1;
		} else {
			sceKernelMunmap(addr, size);
		}
	}

	const size_t dmemSize = sceKernelGetDirectMemorySize();
	for (size = HEAP_DIRECT_MAX; !arenaBase && size >= HEAP_DIRECT_MIN; size /= 2) {
		off_t phys = 0;
		int32_t ret = -1;
		// prefer the top of physical memory, Piglet and the system libraries allocate from the bottom
		if (dmemSize > size) {
			ret = sceKernelAllocateDirectMemory((off_t)(dmemSize - size), (off_t)dmemSize, size, HEAP_ALIGN, ORBIS_KERNEL_WB_ONION, &phys);
		}
		if (ret != 0) {
			ret = sceKernelAllocateDirectMemory(0, (off_t)dmemSize, size, HEAP_ALIGN, ORBIS_KERNEL_WB_ONION, &phys);
		}
		if (ret != 0) {
			continue;
		}
		addr = NULL;
		if (sceKernelMapDirectMemory(&addr, size, ORBIS_KERNEL_PROT_CPU_RW, 0, phys, HEAP_ALIGN) == 0) {
			arenaBase = (uint8_t *)addr;
			arenaKind = 2;
			break;
		}
		sceKernelReleaseDirectMemory(phys, size);
	}

	if (!arenaBase) {
		return;
	}
	if (arenaKind == 1) {
		size = HEAP_SYSFLEX_SIZE;
	}

	arenaPages = size / HEAP_PAGE;
	const size_t mapPages = (arenaPages + HEAP_PAGE - 1) / HEAP_PAGE;
	pageMap = arenaBase;
	memset(pageMap, 0, arenaPages);
	memset(pageMap, 1, mapPages);
	searchHint = mapPages;
	pagesUsed = pagesPeak = mapPages;
}

static void *ps4heapAlloc(size_t len)
{
	const size_t pages = (len + HEAP_PAGE - 1) / HEAP_PAGE;
	size_t first = (size_t)-1;

	ps4heapLock();

	if (!initDone) {
		ps4heapInit();
	}

	if (arenaBase && pages && pages <= arenaPages) {
		if (pages >= HEAP_LARGE_PAGES) {
			// big blocks from the top so they don't fragment the small ones
			size_t run = 0;
			for (size_t i = arenaPages; i-- > 0;) {
				run = pageMap[i] ? 0 : run + 1;
				if (run == pages) {
					first = i;
					break;
				}
			}
		} else {
			// next fit from the bottom, wrapping around once
			for (int pass = 0; pass < 2 && first == (size_t)-1; ++pass) {
				const size_t start = pass ? 0 : searchHint;
				const size_t end = pass ? searchHint + pages : arenaPages;
				size_t run = 0;
				for (size_t i = start; i < end && i < arenaPages; ++i) {
					run = pageMap[i] ? 0 : run + 1;
					if (run == pages) {
						first = i + 1 - pages;
						searchHint = i + 1;
						break;
					}
				}
			}
		}

		if (first != (size_t)-1) {
			memset(pageMap + first, 1, pages);
			pagesUsed += pages;
			if (pagesUsed > pagesPeak) {
				pagesPeak = pagesUsed;
			}
		}
	}

	ps4heapUnlock();

	if (first == (size_t)-1) {
		return NULL;
	}

	// anonymous mappings must read back as zeroes (calloc relies on it)
	uint8_t *ret = arenaBase + first * HEAP_PAGE;
	memset(ret, 0, pages * HEAP_PAGE);
	return ret;
}

static int ps4heapOwns(const void *addr)
{
	return arenaBase && (const uint8_t *)addr >= arenaBase && (const uint8_t *)addr < arenaBase + arenaPages * HEAP_PAGE;
}

static void ps4heapFree(void *addr, size_t len)
{
	const size_t first = (size_t)((uint8_t *)addr - arenaBase) / HEAP_PAGE;
	size_t pages = (len + HEAP_PAGE - 1) / HEAP_PAGE;

	ps4heapLock();
	if (first + pages > arenaPages) {
		pages = arenaPages - first;
	}
	for (size_t i = first; i < first + pages; ++i) {
		if (pageMap[i]) {
			pageMap[i] = 0;
			--pagesUsed;
		}
	}
	if (first < searchHint) {
		searchHint = first;
	}
	ps4heapUnlock();
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
	if (!addr && fd == -1 && (flags & MAP_ANON) && !(flags & MAP_FIXED) && prot == (PROT_READ | PROT_WRITE)) {
		void *ret = ps4heapAlloc(len);
		if (ret) {
			return ret;
		}
	}

	void *mapped = NULL;
	if (sceKernelMmap(addr, len, prot, flags, fd, off, &mapped) < 0) {
		errno = ENOMEM;
		return MAP_FAILED;
	}
	return mapped;
}

int munmap(void *addr, size_t len)
{
	if (ps4heapOwns(addr)) {
		ps4heapFree(addr, len);
		return 0;
	}
	if (sceKernelMunmap(addr, len) < 0) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

int ps4HeapGetKind(void)
{
	return arenaKind;
}

void ps4HeapGetStats(size_t *arena, size_t *inuse, size_t *peak)
{
	*arena = arenaPages * HEAP_PAGE;
	*inuse = pagesUsed * HEAP_PAGE;
	*peak = pagesPeak * HEAP_PAGE;
}

#endif // PLATFORM_PS4
