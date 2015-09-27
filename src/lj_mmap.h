/*
 * OS-dependent mmap allocator used by lj_alloc.
 * This file covers the simple case, ie 32bit mmap
 */

#ifndef _LJ_MMAP_H
#define _LJ_MMAP_H
/* All must be multiples of kilobyte. */
#define DEFAULT_GRANULARITY	((size_t)128U * (size_t)1024U)
#define DEFAULT_MMAP_THRESHOLD	DEFAULT_GRANULARITY
#define STACK_RESERVE           DEFAULT_GRANULARITY*64

#define MAX_SIZE_T		(~(size_t)0)
#define MFAIL			((void *)(MAX_SIZE_T))
#define CMFAIL			((char *)(MFAIL)) /* defined for convenience */

#define page_align(S)\
  - (((S) + (LJ_PAGESIZE - SIZE_T_ONE)) & ~(LJ_PAGESIZE - SIZE_T_ONE))

#define CALL_MREMAP_NOMOVE	0

/* granularity-align a size */
#define granularity_align(S)\
  (((S) + (DEFAULT_GRANULARITY - SIZE_T_ONE))\
   & ~(DEFAULT_GRANULARITY - SIZE_T_ONE))

#if LJ_TARGET_WINDOWS
#include <windows.h>
/* Undocumented, but hey, that's what we all love so much about Windows. */
typedef long (*mmap_info)(HANDLE handle, void **addr, ULONG zbits,
		       size_t *size, ULONG alloctype, ULONG prot);
#else
/* ------------------------------ Unix ----------------------------- */
typedef struct mmap_info *mmap_info;
#include <errno.h>
#include <sys/mman.h>
#define MMAP_PROT		(PROT_READ|PROT_WRITE)
#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS		MAP_ANON
#endif
#define MMAP_FLAGS		(MAP_PRIVATE|MAP_ANONYMOUS)
#define DIRECT_MMAP CALL_MMAP
#endif
#if !LJ_TARGET_WINDOWS && (!LJ_64 || LJ_GC64)
#define mmap_align(S)		page_align(S)
#define INIT_MMAP()		(NULL)
static LJ_AINLINE void *CALL_MMAP(mmap_info bm,size_t size)
{
  int olderr = errno;
  void *ptr = mmap(NULL, size, MMAP_PROT, MMAP_FLAGS, -1, 0);
  errno = olderr;
  return ptr;
}
static LJ_AINLINE int CALL_MUNMAP(mmap_info bm,void *ptr, size_t size)
{
  int olderr = errno;
  int ret = munmap(ptr, size);
  errno = olderr;
  return ret;
}
#if LJ_TARGET_LINUX /* 32bit linux */
#define CALL_MREMAP_MV	MREMAP_MAYMOVE
static LJ_AINLINE void *CALL_MREMAP(mmap_info bm,void *ptr, size_t osz,
		size_t nsz, int flags)
{
  int olderr = errno;
  ptr = mremap(ptr, osz, nsz, flags);
  errno = olderr;
  return ptr;
}
#else
#define CALL_MREMAP(bm,addr, osz, nsz, mv) ((void)osz, MFAIL)
#endif

/* ----------------------------- Win32 ----------------------------- */
#elif LJ_TARGET_WINDOWS && !LJ_64
/* Win32 MMAP via VirtualAlloc */
static void *CALL_MMAP(mmap_info bm, size_t size)
{
  DWORD olderr = GetLastError();
  void *ptr = VirtualAlloc(0, size, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
  SetLastError(olderr);
  return ptr ? ptr : MFAIL;
}

/* For direct MMAP, use MEM_TOP_DOWN to minimize interference */
static void *DIRECT_MMAP(mmap_info bm, size_t size)
{
  DWORD olderr = GetLastError();
  void *ptr = VirtualAlloc(0, size, MEM_RESERVE|MEM_COMMIT|MEM_TOP_DOWN,
			   PAGE_READWRITE);
  SetLastError(olderr);
  return ptr ? ptr : MFAIL;
}

/* This function supports releasing coalesed segments */
static int CALL_MUNMAP(mmap_info bm, void *ptr, size_t size)
{
  DWORD olderr = GetLastError();
  MEMORY_BASIC_INFORMATION minfo;
  char *cptr = (char *)ptr;
  while (size) {
    if (VirtualQuery(cptr, &minfo, sizeof(minfo)) == 0)
      return -1;
    if (minfo.BaseAddress != cptr || minfo.AllocationBase != cptr ||
	minfo.State != MEM_COMMIT || minfo.RegionSize > size)
      return -1;
    if (VirtualFree(cptr, 0, MEM_RELEASE) == 0)
      return -1;
    cptr += minfo.RegionSize;
    size -= minfo.RegionSize;
  }
  SetLastError(olderr);
  return 0;
}

#else /* 64bit unix or windows are complex, implemented in lj_mmap.c */
#define CALL_MREMAP_MV	0
mmap_info INIT_MMAP(void);
void *CALL_MMAP(mmap_info bm, size_t size);
int CALL_MUNMAP(mmap_info bm, void *ptr, size_t size);
#if LJ_TARGET_LINUX
void *CALL_MREMAP(mmap_info bm, void *ptr, size_t osz, size_t nsz, int move);
#else
#define CALL_MREMAP(bm,addr, osz, nsz, mv) ((void)osz, MFAIL)
#endif
#if LJ_TARGET_WINDOWS
void *DIRECT_MMAP(mmap_info bm, size_t size);
#else
#define DIRECT_MMAP CALL_MMAP
#endif

#endif /* 64bit unix/win */

#ifndef mmap_align
#define mmap_align(S)	granularity_align(S)
#endif

#endif /* guard */


