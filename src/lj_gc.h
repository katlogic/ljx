/*
** Garbage collector
** Copyright (C) 2014 Karel Tuma. See Copyright Notice in luajit.h
**
** Major portions taken verbatim or adapted from LuaJIT 2.1.
** Copyright (C) 2005-2014 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_GC_H
#define _LJ_GC_H

#include "lj_obj.h"

/* Garbage collector flags */
#define GCF_notrunning     1
#define GCF_stayontrace    2 /* not set if GCSatomic and GCScallfin */

/* Garbage collector states. Order matters. */
#define GCSpropagate	0
#define GCSatomic	1
#define GCSswpstr       2
#define GCSswpallgc	3
#define GCSswpfinobj	4
#define GCSswptobefnz	5
#define GCSswpend	6
#define GCScallfin	7
#define GCSpause	8
#define GCSinsideatomic	9

#define issweepphase(g)  \
	((g)->gc.state >= GCSswpstr && (g)->gc.state <= GCSswpend)

#define GCFINHEADSTART  1
#define GCSTEPSIZE	1024
#define GCSWEEPMAX	40
#define GCSWEEPCOST	10
#define GCSWEEPSTRCOST	10
#define GCFINALIZECOST	GCSWEEPCOST
#define STEPMULADJ      200
#define PAUSEADJ        100

/*
** macro to tell when main invariant (white objects cannot point to black
** ones) must be kept. During a collection, the sweep
** phase may break the invariant, as objects turned white may point to
** still-black objects. The invariant is restored when sweep ends and
** all objects are white again.
*/

#define keepinvariant(g)	((g)->gc.state <= GCSatomic)

/* XXX TBD */
#define iscollectable(tv) tvisgcv(tv)

/* Bitmasks for marked field of GCobj. */
/* Layout for bit use in `marked' field: */
#define LJ_GC_WHITE0	0x01
#define LJ_GC_WHITE1	0x02
#define LJ_GC_BLACK	0x04
#define LJ_GC_FINALIZED	0x08
#define LJ_GC_FIXED	0x10
#define LJ_GC_SFIXED	0x20
/* bit 7 is currently used by tests (luaL_checkmemory) */

#define LJ_GC_WHITES	(LJ_GC_WHITE0 | LJ_GC_WHITE1)
#define LJ_GC_COLORS	(LJ_GC_WHITES | LJ_GC_BLACK)

/* Macros to test and set GCobj colors. */
#define iswhite(x)	((x)->gch.marked & LJ_GC_WHITES)
#define isblack(x)	((x)->gch.marked & LJ_GC_BLACK)
#define isgray(x)	(!((x)->gch.marked & (LJ_GC_BLACK|LJ_GC_WHITES)))
#define tofinalize(x)	((x)->gch.marked & LJ_GC_FINALIZED)
#define tviswhite(x)	(tvisgcv(x) && iswhite(gcV(x)))
#define otherwhite(g)	(g->gc.currentwhite ^ LJ_GC_WHITES)
#define changewhite(x)	((x)->gch.marked ^= LJ_GC_WHITES)
#define isdead(g, v)	((v)->gch.marked & otherwhite(g) & LJ_GC_WHITES)

#define curwhite(g)	((g)->gc.currentwhite & LJ_GC_WHITES)
#define newwhite(g, x)	(obj2gco(x)->gch.marked = (uint8_t)curwhite(g))
#define makewhite(g, x) \
  ((x)->gch.marked = ((x)->gch.marked & (uint8_t)~LJ_GC_COLORS) | curwhite(g))
#define flipwhite(x)	((x)->gch.marked ^= LJ_GC_WHITES)
#define black2gray(x)	((x)->gch.marked &= (uint8_t)~LJ_GC_BLACK)
#define fixstring(s)	((s)->marked |= LJ_GC_FIXED)
#define markfinalized(x)	((x)->gch.marked |= LJ_GC_FINALIZED)

/* Collector. */
LJ_FUNC void lj_gc_setdebt(global_State *g, MDiff debt);
LJ_FUNC size_t lj_gc_separateudata(global_State *g, int all);
LJ_FUNC void lj_gc_checkfinalizer(lua_State *L, GCobj *o);
LJ_FUNC void lj_gc_finalize_udata(lua_State *L);
#if LJ_HASFFI
LJ_FUNC void lj_gc_finalize_cdata(lua_State *L);
#else
#define lj_gc_finalize_cdata(L)		UNUSED(L)
#endif
LJ_FUNC void lj_gc_freeall(lua_State *L);
LJ_FUNCA int LJ_FASTCALL lj_gc_step(lua_State *L);
LJ_FUNCA void LJ_FASTCALL lj_gc_step_fixtop(lua_State *L);
#if LJ_HASJIT
LJ_FUNC int LJ_FASTCALL lj_gc_step_jit(global_State *g, MSize steps);
#endif
LJ_FUNC void lj_gc_fullgc(lua_State *L);

/* GC check: drive collector forward if the GC threshold has been reached. */
#define lj_gc_check(L) \
  { if (LJ_UNLIKELY(G(L)->gc.debt > 0)) \
      lj_gc_step(L); }
#define lj_gc_check_fixtop(L) \
  { if (LJ_UNLIKELY(G(L)->gc.debt > 0)) \
      lj_gc_step_fixtop(L); }

/* Write barriers. */
LJ_FUNC void lj_gc_barrierf(global_State *g, GCobj *o, GCobj *v);
LJ_FUNCA void LJ_FASTCALL lj_gc_barrieruv(global_State *g, TValue *tv);
LJ_FUNC void lj_gc_closeuv(global_State *g, GCupval *uv);
#if LJ_HASJIT
LJ_FUNC void lj_gc_barriertrace(global_State *g, uint32_t traceno);
#endif

/* Move the GC propagation frontier back for tables (make it gray again). */
static LJ_AINLINE void lj_gc_barrierback(global_State *g, GCtab *t)
{
  GCobj *o = obj2gco(t);
  lua_assert(t->gct == ~LJ_TTAB);
  lua_assert(isblack(o) && !isdead(g, o));
  lua_assert(g->gc.state != GCScallfin && g->gc.state != GCSpause);
  black2gray(o);
  setgcrefr(t->gclist, g->gc.grayagain);
  setgcref(g->gc.grayagain, o);
}

/* Barrier for stores to table objects. TValue and GCobj variant. */
#define lj_gc_anybarriert(L, t)  \
  { if (LJ_UNLIKELY(isblack(obj2gco(t)))) lj_gc_barrierback(G(L), (t)); }
#define lj_gc_barriert(L, t, tv) \
  { if (tviswhite(tv) && isblack(obj2gco(t))) \
      lj_gc_barrierback(G(L), (t)); }
#define lj_gc_objbarriert(L, t, o)  \
  { if (iswhite(obj2gco(o)) && isblack(obj2gco(t))) \
      lj_gc_barrierback(G(L), (t)); }

/* Barrier for stores to any other object. TValue and GCobj variant. */
#define lj_gc_barrier(L, p, tv) \
  { if (tviswhite(tv) && isblack(obj2gco(p))) \
      lj_gc_barrierf(G(L), obj2gco(p), gcV(tv)); }
#define lj_gc_objbarrier(L, p, o) \
  { if (iswhite(obj2gco(o)) && isblack(obj2gco(p))) \
      lj_gc_barrierf(G(L), obj2gco(p), obj2gco(o)); }

/* Allocator. */
LJ_FUNC void *lj_mem_realloc(lua_State *L, void *p, GCSize osz, GCSize nsz);
LJ_FUNC void * LJ_FASTCALL lj_mem_newgco(lua_State *L, GCSize size);
LJ_FUNC void *lj_mem_grow(lua_State *L, void *p,
			  MSize *szp, MSize lim, MSize esz);


#define lj_mem_new(L, s)	lj_mem_realloc(L, NULL, 0, (s))

static LJ_AINLINE void lj_mem_free(global_State *g, void *p, size_t osize)
{
  g->gc.debt -= (MSize)osize;
  g->allocf(g->allocd, p, osize, 0);
}

#define lj_mem_newvec(L, n, t)	((t *)lj_mem_new(L, (GCSize)((n)*sizeof(t))))
#define lj_mem_reallocvec(L, p, on, n, t) \
  ((p) = (t *)lj_mem_realloc(L, p, (on)*sizeof(t), (GCSize)((n)*sizeof(t))))
#define lj_mem_growvec(L, p, n, m, t) \
  ((p) = (t *)lj_mem_grow(L, (p), &(n), (m), (MSize)sizeof(t)))
#define lj_mem_freevec(g, p, n, t)	lj_mem_free(g, (p), (n)*sizeof(t))

#define lj_mem_newobj(L, t)	((t *)lj_mem_newgco(L, sizeof(t)))
#define lj_mem_newt(L, s, t)	((t *)lj_mem_new(L, (s)))
#define lj_mem_freet(g, p)	lj_mem_free(g, (p), sizeof(*(p)))

/* actual number of total bytes allocated */
#define gc_gettotalbytes(g)	((g)->gc.total + (g)->gc.debt)

#endif

