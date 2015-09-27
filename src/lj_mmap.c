/*
 * OS-dependent mmap allocator used by lj_alloc.
 * In particular, it's job is to keep allocations below 32bits on 64bit archs.
 */

#define lj_mmap_c
#define LUA_CORE

/* To get the mremap prototype. Must be defined before any system includes. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "lj_def.h"
#include "lj_arch.h"
#include "lj_mmap.h"

/* ------------------------------ Unix ----------------------------- */
#if !LJ_TARGET_WINDOWS && (LJ_64 && !LJ_GC64)
/*
 * 64 bit mode with 32 bit pointers needs special support for allocating
 * memory in the lower 4GB. We accomplish this cross-platform by keeping
 * two bitmaps of DEFAULT_GRANULARITY chunks.
 *
 * First bitmap tracks chunks we've succesfuly allocated or probes which failed
 * because areas were occupied by something else. Second bitmap tracks just
 * those failed areas.
 *
 * When first bitmap becomes completely full, therefore no area is left
 * to probe, we clear all bits in there according to the second bitmap - 
 * so that the failed probes are retried again. If this still fails in the
 * same MMAP call, we give up.
 */

#define BM_CHUNKS ((size_t)4*1024*1024/(DEFAULT_GRANULARITY/1024))
#define BM_WORDS (BM_CHUNKS/32)
#define BM_FULL  (~(uint32_t)0)

#define BM_CHECK(n,i) ((bm->n[i/32])&(1<<(i%32)))
#define BM_CLEAR(n,i) ((bm->n[i/32])&=(~(1<<(i%32))))
#define BM_SET_(n,i,j) ((bm->n[i])|=(1<<(j)))
#define BM_SET(n,i) BM_SET_(n,(i)/32,(i)%32)


typedef struct mmap_info {
  uint32_t used[BM_WORDS];
  uint32_t failed[BM_WORDS];
} *mmap_info;

mmap_info INIT_MMAP(void)
{
  mmap_info bm = malloc(sizeof(*bm));
  memset(bm, 0, sizeof(*bm));
  return bm;
}


void *CALL_MMAP(mmap_info bm, size_t size)
{
	char dummy;
	void *stackp = &dummy; /* TBD: UB */
	int i,j;
	void *ptr;
	int retried = 0;
	lua_assert(size % DEFAULT_GRANULARITY == 0);
retry:;
	for (i = 0; i < BM_WORDS; i++) {
		uint32_t w = bm->used[i];
		if (w == BM_FULL)
			continue;
		for (j = 0; j < 32; j++) {
			if (!(w&(1<<(j)))) {
				void *want = (void*)(((uintptr_t)i*32+j)*DEFAULT_GRANULARITY);
				BM_SET_(used, i, j);
				ptr = mmap(want, size,
						MMAP_PROT, MMAP_FLAGS, -1, 0);
				if ((!(ptr < stackp && ptr + STACK_RESERVE > stackp)) &&
						(ptr == want))
					return ptr;
				if (ptr != MAP_FAILED)
					munmap(ptr, size);
				BM_SET_(failed, i, j);
			}
		}
	}
	if (retried)
		return MAP_FAILED;
	/* Everything failed, clear probes and retry. */
	for (i = 0; i < BM_WORDS; i++) {
		bm->used[i] ^= bm->failed[i];
		bm->failed[i] = 0;
	}
	/* One more time */
	retried = 1;
	goto retry;
	return NULL;
}

int CALL_MUNMAP(mmap_info bm, void *ptr, size_t size)
{
  int i, olderr = errno;
	uintptr_t idx = ((uintptr_t)ptr);
	lua_assert(size % DEFAULT_GRANULARITY == 0);
	lua_assert(idx % DEFAULT_GRANULARITY == 0);
	idx /= DEFAULT_GRANULARITY;
	lua_assert(BM_CHECK(used,idx));
	for (i = 0; i < (size/DEFAULT_GRANULARITY); i++)
		BM_CLEAR(used,idx+i);
  int ret = munmap(ptr, size);
  errno = olderr;
  return ret;
}

#if LJ_TARGET_LINUX
void *CALL_MREMAP(mmap_info bm, void *ptr, size_t osz, size_t nsz, int move)
{
	void *nptr;
  int i, olderr = errno;
	uintptr_t idx = ((uintptr_t)ptr);
	lua_assert(osz % DEFAULT_GRANULARITY == 0);
	lua_assert(nsz % DEFAULT_GRANULARITY == 0);
	lua_assert(idx % DEFAULT_GRANULARITY == 0);
	idx /= DEFAULT_GRANULARITY;
  nptr = mremap(ptr, osz, nsz, 0);
	if (nptr == ptr) {
		for (i = 0; i < (osz/DEFAULT_GRANULARITY); i++)
			BM_CLEAR(used, idx+i);
		for (i = 0; i < (nsz/DEFAULT_GRANULARITY); i++)
			BM_SET(used, idx+i);
	} else lua_assert(nptr == MAP_FAILED);
  errno = olderr;
  return ptr;
}
#endif /* mremap on linux */



#elif LJ_64 && !LJ_GC64
/* ----------------------------- Win64 ----------------------------- */
#define WIN32_LEAN_AND_MEAN
/* Number of top bits of the lower 32 bits of an address that must be zero.
 * Apparently 0 gives us full 64 bit addresses and 1 gives us the lower 2GB.
 *
 * TBD: Port bitmap stuff into here.
 */
#define NTAVM_ZEROBITS		1

mmap_info INIT_MMAP(void)
{
  return (mmap_info)GetProcAddress(GetModuleHandleA("ntdll.dll"),
				 "NtAllocateVirtualMemory");
}

/* Win64 32 bit MMAP via NtAllocateVirtualMemory. */
void *CALL_MMAP(mmap_info bm, size_t size)
{
  DWORD olderr = GetLastError();
  void *ptr = NULL;
  long st = bm(INVALID_HANDLE_VALUE, &ptr, NTAVM_ZEROBITS, &size,
		  MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
  SetLastError(olderr);
  return st == 0 ? ptr : MFAIL;
}

/* For direct MMAP, use MEM_TOP_DOWN to minimize interference */
void *DIRECT_MMAP(mmap_info bm, size_t size)
{
  DWORD olderr = GetLastError();
  void *ptr = NULL;
  long st = bm(INVALID_HANDLE_VALUE, &ptr, NTAVM_ZEROBITS, &size,
		  MEM_RESERVE|MEM_COMMIT|MEM_TOP_DOWN, PAGE_READWRITE);
  SetLastError(olderr);
  return st == 0 ? ptr : MFAIL;
}

#endif /* end of windows 64 */
