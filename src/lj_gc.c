/*
** Garbage collector
** Copyright (C) 2014 Karel Tuma. See Copyright Notice in luajit.h
** 
** Major portions taken verbatim or adapted from LuaJIT 2.1.0-alpha.
** Copyright (C) 2005-2014 Mike Pall. See Copyright Notice in luajit.h
**
** Major portions taken verbatim or adapted from the Lua 5.3.0-work2 interpreter.
** Copyright (C) 1994-2014 Lua.org, PUC-Rio. See Copyright Notice in lua.h
*/

#define lj_gc_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_func.h"
#include "lj_udata.h"
#include "lj_meta.h"
#include "lj_state.h"
#include "lj_frame.h"
#if LJ_HASFFI
#include "lj_ctype.h"
#include "lj_cdata.h"
#endif
#include "lj_trace.h"
#include "lj_vm.h"

/* -- Macros ---------------------------------------------------------- */

/* Macros to set GCobj colors and flags. */
#define white2gray(x)		((x)->gch.marked &= (uint8_t)~LJ_GC_WHITES)
#define gray2black(x)		((x)->gch.marked |= LJ_GC_BLACK)

/* Link table 't' into a list pointed by 'dst'. */
#define linktable(t, dst) { \
  setgcrefr(t->gclist, dst); \
  setgcref(dst, obj2gco(t)); }

/* Mark a TValue (if needed). */
#define gc_marktv(g, tv) \
  { lua_assert(!tvisgcv(tv) || (~itype(tv) == gcval(tv)->gch.gct)); \
    if (tviswhite(tv)) gc_mark(g, gcV(tv)); }

/* Mark a GCobj (if needed). */
#define gc_markobj(g, o) \
  { if (iswhite(obj2gco(o))) gc_mark(g, obj2gco(o)); }

/* Mark a string object. */
#define gc_mark_str(s)		((s)->marked &= (uint8_t)~LJ_GC_WHITES)

/* -- Write barriers ------------------------------------------------------ */

/* Move the GC propagation frontier forward. */
static void gc_mark(global_State *g, GCobj *o);
void lj_gc_barrierf(global_State *g, GCobj *o, GCobj *v)
{
  lua_assert(isblack(o) && iswhite(v) && !isdead(g, v) && !isdead(g, o));
  lua_assert(g->gc.state != GCSpause);
  lua_assert(o->gch.gct != ~LJ_TTAB);
  /* Preserve invariant during propagation. Otherwise it doesn't matter. */
  if (keepinvariant(g))
    gc_mark(g, v);  /* Move frontier forward. */
  else {
    lua_assert(issweepphase(g));
    makewhite(g, o);  /* Make it white to avoid the following barrier. */
  }
}

/* Specialized barrier for closed upvalue. Pass &uv->tv. */
void LJ_FASTCALL lj_gc_barrieruv(global_State *g, TValue *tv)
{
#define TV2MARKED(x) \
  (*((uint8_t *)(x) - offsetof(GCupval, tv) + offsetof(GCupval, marked)))
  if (keepinvariant(g))
    gc_mark(g, gcV(tv));
  else
    TV2MARKED(tv) = (TV2MARKED(tv) & (uint8_t)~LJ_GC_COLORS) | curwhite(g);
#undef TV2MARKED
}

/* Close upvalue. Also needs a write barrier. */
void lj_gc_closeuv(global_State *g, GCupval *uv)
{
  GCobj *o = obj2gco(uv);
  /* Copy stack slot to upvalue itself and point to the copy. */
  copyTV(mainthread(g), &uv->tv, uvval(uv));
  setmref(uv->v, &uv->tv);
  uv->closed = 1;
  setgcrefr(o->gch.nextgc, g->gc.root);
  setgcref(g->gc.root, o);
  if (isgray(o)) {  /* A closed upvalue is never gray, so fix this. */
    if (keepinvariant(g)) {
      gray2black(o);  /* Make it black and preserve invariant. */
      if (tviswhite(&uv->tv))
	lj_gc_barrierf(g, o, gcV(&uv->tv));
    } else {
      makewhite(g, o);  /* Make it white, i.e. sweep the upvalue. */
      lua_assert(!keepinvariant(g));
    }
  }
}

#if LJ_HASJIT
static void gc_marktrace(global_State *g, TraceNo traceno);
/* Mark a trace if it's saved during the propagation phase. */
void lj_gc_barriertrace(global_State *g, uint32_t traceno)
{
  if (keepinvariant(g))
    gc_marktrace(g, traceno);
}
#endif

/* -- Mark ------------------------------------------------------ */

/* Mark a white GCobj. */
static void gc_mark(global_State *g, GCobj *o)
{
  int gct = o->gch.gct;
  lua_assert(iswhite(o) && !isdead(g, o));
  white2gray(o);
  if (LJ_UNLIKELY(gct == ~LJ_TUDATA)) {
    TValue uvalue;
    GCtab *mt = tabref(gco2ud(o)->metatable);
    gray2black(o);  /* Userdata are never gray. */
    if (mt) gc_markobj(g, mt);
    getuservalue(mainthread(g), gco2ud(o), &uvalue);
    if (tviswhite(&uvalue))
      return gc_mark(g, gcval(&uvalue));
  } else if (LJ_UNLIKELY(gct == ~LJ_TUPVAL)) {
    GCupval *uv = gco2uv(o);
    gc_marktv(g, uvval(uv));
    if (uv->closed)
      gray2black(o);  /* Closed upvalues are never gray. */
  } else if (gct != ~LJ_TSTR && gct != ~LJ_TCDATA) {
    lua_assert(gct == ~LJ_TFUNC || gct == ~LJ_TTAB ||
	       gct == ~LJ_TTHREAD || gct == ~LJ_TPROTO || gct == ~LJ_TTRACE);
    setgcrefr(o->gch.gclist, g->gc.gray);
    setgcref(g->gc.gray, o);
  }
}

/* Mark GC roots. */
static void gc_mark_gcroot(global_State *g)
{
  ptrdiff_t i;
  for (i = 0; i < GCROOT_MAX; i++)
    if (gcref(g->gcroot[i]) != NULL)
      gc_markobj(g, gcref(g->gcroot[i]));
}

/** mark all objects in list of being-finalized */
static void gc_mark_finalized(global_State *g)
{
  GCobj *o;
  for (o = gcref(g->gc.tobefnz); o; o = gcnext(o)) {
      makewhite(g, o);
      gc_mark(g, o);
  }
}

/* Start a GC cycle and mark the root set. */
static void gc_mark_start(global_State *g)
{
  setgcrefnull(g->gc.gray);
  setgcrefnull(g->gc.grayagain);
  setgcrefnull(g->gc.weak);
  setgcrefnull(g->gc.allweak);
  setgcrefnull(g->gc.ephemeron);
  gc_markobj(g, mainthread(g));
  gc_markobj(g, tabref(mainthread(g)->env));
  gc_marktv(g, &g->registrytv);
  gc_mark_gcroot(g);
  gc_mark_finalized(g);
}

/* Mark open upvalues. */
static void gc_mark_uv(global_State *g)
{
  GCupval *uv;
  for (uv = uvnext(&g->uvhead); uv != &g->uvhead; uv = uvnext(uv)) {
    lua_assert(uvprev(uvnext(uv)) == uv && uvnext(uvprev(uv)) == uv);
    if (isgray(obj2gco(uv)))
      gc_marktv(g, uvval(uv));
  }
}

static GCRef *findlast (GCRef *p) {
  while (gcref(*p))
    p = &gcref(*p)->gch.nextgc;
  return p;
}

/* -- Propagation phase --------------------------------------------------- */

/* Check whether we can clear a key or a value slot from a table. */
static int iscleared(cTValue *o)
{
  if (tvisgcv(o)) {
    if (tvisstr(o)) {
        gc_mark_str(strV(o));
        return 0;
    }
    if (iswhite(gcV(o)))
      return 1;
  }
  return 0;
}

/* Propagate all gray objects. */
static void gc_propagate_mark(global_State *g);
static void gc_propagate_all(global_State *g)
{
  while (gcref(g->gc.gray)) gc_propagate_mark(g);
}

static void gc_propagate_list(global_State *g, GCobj *l) {
  lua_assert(gcref(g->gc.gray) == NULL);  /* no grays left */
  setgcref(g->gc.gray, l);
  gc_propagate_all(g);  /* traverse all elements from 'l' */
}

/* Retraverse objects caught by write barrier. */
static void gc_retraverse_grays(global_State *g)
{
  GCobj *weak, *ephemeron, *grayagain;

  weak = gcref(g->gc.weak);
  grayagain = gcref(g->gc.grayagain);
  ephemeron = gcref(g->gc.ephemeron);

  setgcrefnull(g->gc.weak);
  setgcrefnull(g->gc.grayagain);
  setgcrefnull(g->gc.ephemeron);

  gc_propagate_all(g);
  gc_propagate_list(g, grayagain);
  gc_propagate_list(g, weak);
  gc_propagate_list(g, ephemeron);
}

static int gc_traverse_ephemeron(global_State *g, GCtab *t) {
  int marked = 0;  /* true if an object is marked in this traversal */
  int hasclears = 0;  /* true if table has white keys */
  int prop = 0;  /* true if table has entry "white-key -> white-value" */
  /* traverse array part (numeric keys are 'strong') */
  MSize i, asize = t->asize;
  for (i = 0; i < asize; i++) {
    if (tviswhite(arrayslot(t, i))) {
      marked = 1;
      gc_marktv(g, arrayslot(t, i));
    }
  }
  if (t->hmask > 0) {  /* Mark hash part. */
    Node *node = noderef(t->node);
    MSize i, hmask = t->hmask;
    for (i = 0; i <= hmask; i++) {
      Node *n = &node[i];
      if (!tvisnil(&n->val)) {  /* Mark non-empty slot. */
	lua_assert(!tvisnil(&n->key));
        if (iscleared(&n->key)) {
          hasclears = 1;
          if (tviswhite(&n->val))
            prop = 1;
        } else if (tviswhite(&n->val)) {
          marked = 1;
          gc_marktv(g, &n->val);
        }
      }
    }
  }
  if (g->gc.state != GCSatomic && prop)
    linktable(t, g->gc.ephemeron)  /* have to propagate again */
  else if (hasclears)  /* does table have white keys? */
    linktable(t, g->gc.allweak)  /* may have to clean white keys */
  else  /* no white keys */
    linktable(t, g->gc.grayagain)  /* no need to clean */
  return marked;
}

static void gc_converge_ephemerons(global_State *g)
{
  int changed;
  do {
    GCobj *w, *next = gcref(g->gc.ephemeron);  /* get ephemeron list */
    setgcrefnull(g->gc.ephemeron);  /* tables will return to this list when traversed */
    changed = 0;
    while ((w = next) != NULL) {
      next = gcnext(w);
      if (gc_traverse_ephemeron(g, gco2tab(w))) {  /* traverse marked some value? */
        gc_propagate_all(g);
        changed = 1;  /* will have to revisit all ephemeron tables */
      }
    }
  } while (changed);
}

/* Traverse a table, return 1 if weak. */
static int gc_traverse_tab(global_State *g, GCtab *t)
{
  cTValue *mode;
  GCtab *mt = tabref(t->metatable);
  const char *s;

  /* Mark metatable */
  if (mt)
    gc_markobj(g, mt);

  /* Some sort of weak table */
  mode = lj_meta_fastg(g, mt, MM_mode);
  if (mode && tvisstr(mode) && (s = strVdata(mode))[0]) {
    int v = !!strchr(s, 'v'); /* ((s[0] == 'v') || (s[1] == 'v')) */
    int k = !!strchr(s, 'k'); /* ((s[0] == 'k') || (s[1] == 'k')) */
    if (k != v) {
      if (!k) { /* Strong keys */
        int hasclears = (t->asize>0);
        Node *node = noderef(t->node);
        MSize i, hmask = t->hmask;
        lua_assert(k == 0 && v == 1);
        for (i = 0; i <= hmask; i++) {
          Node *n = &node[i];
          if (!tvisnil(&n->val)) {  /* Mark non-empty slot. */
            lua_assert(!tvisnil(&n->key));
            gc_marktv(g, &n->key);
            if (!hasclears && iscleared(&n->val))
              hasclears = 1;
          }
        }
        if (hasclears)
          linktable(t, g->gc.weak)
        else
          linktable(t, g->gc.grayagain)
      } else { /* Strong values */
        lua_assert(k == 1 && v == 0);
        gc_traverse_ephemeron(g, t);
      }
      return 1;
    } else if (k) { /* k == v == 1 */
      lua_assert(k == 1 && v == 1);
      linktable(t, g->gc.allweak);
      return 1;
    }
  }

  /* Normal strong table */
  if (t->asize) {  /* Mark array part (values only) */
    MSize i, asize = t->asize;
    for (i = 0; i < asize; i++)
      gc_marktv(g, arrayslot(t, i));
  }

  if (t->hmask > 0) {  /* Mark hash part. */
    Node *node = noderef(t->node);
    MSize i, hmask = t->hmask;
    for (i = 0; i <= hmask; i++) {
      Node *n = &node[i];
      if (!tvisnil(&n->val)) {  /* Mark non-empty slot. */
	lua_assert(!tvisnil(&n->key));
        gc_marktv(g, &n->key);
        gc_marktv(g, &n->val);
      }
    }
  }
  return 0;
}

/* Traverse a function. */
static void gc_traverse_func(global_State *g, GCfunc *fn)
{
  gc_markobj(g, tabref(fn->c.env));
  if (isluafunc(fn)) {
    uint32_t i;
    lua_assert(fn->l.nupvalues <= funcproto(fn)->sizeuv);
    gc_markobj(g, funcproto(fn));
    for (i = 0; i < fn->l.nupvalues; i++)  /* Mark Lua function upvalues. */
      gc_markobj(g, &gcref(fn->l.uvptr[i])->uv);
  } else {
    uint32_t i;
    for (i = 0; i < fn->c.nupvalues; i++)  /* Mark C function upvalues. */
      gc_marktv(g, &fn->c.upvalue[i]);
  }
}

#if LJ_HASJIT
/* Mark a trace. */
static void gc_marktrace(global_State *g, TraceNo traceno)
{
  GCobj *o = obj2gco(traceref(G2J(g), traceno));
  lua_assert(traceno != G2J(g)->cur.traceno);
  if (iswhite(o)) {
    white2gray(o);
    setgcrefr(o->gch.gclist, g->gc.gray);
    setgcref(g->gc.gray, o);
  }
}

/* Traverse a trace. */
static void gc_traverse_trace(global_State *g, GCtrace *T)
{
  IRRef ref;
  if (T->traceno == 0) return;
  for (ref = T->nk; ref < REF_TRUE; ref++) {
    IRIns *ir = &T->ir[ref];
    if (ir->o == IR_KGC)
      gc_markobj(g, ir_kgc(ir));
  }
  if (T->link) gc_marktrace(g, T->link);
  if (T->nextroot) gc_marktrace(g, T->nextroot);
  if (T->nextside) gc_marktrace(g, T->nextside);
  gc_markobj(g, gcref(T->startpt));
}

/* The current trace is a GC root while not anchored in the prototype (yet). */
#define gc_traverse_curtrace(g)	gc_traverse_trace(g, &G2J(g)->cur)
#else
#define gc_traverse_curtrace(g)	UNUSED(g)
#endif

/* Traverse a prototype. */
static void gc_traverse_proto(global_State *g, GCproto *pt)
{
  ptrdiff_t i;
  gc_mark_str(proto_chunkname(pt));
  for (i = -(ptrdiff_t)pt->sizekgc; i < 0; i++)  /* Mark collectable consts. */
    gc_markobj(g, proto_kgc(pt, i));
#if LJ_HASJIT
  if (pt->trace) gc_marktrace(g, pt->trace);
#endif
}

/* Traverse the frame structure of a stack. */
static MSize gc_traverse_frames(global_State *g, lua_State *th)
{
  TValue *frame, *top = th->top-1, *bot = tvref(th->stack);
  /* Note: extra vararg frame not skipped, marks function twice (harmless). */
  for (frame = th->base-1; frame > bot+LJ_FR2; frame = frame_prev(frame)) {
    GCfunc *fn = frame_func(frame);
    TValue *ftop = frame;
    if (isluafunc(fn)) ftop += funcproto(fn)->framesize;
    if (ftop > top) top = ftop;
    if (!LJ_FR2) gc_markobj(g, fn);  /* Need to mark hidden function (or L). */
  }
  top++;  /* Correct bias of -1 (frame == base-1). */
  if (top > tvref(th->maxstack)) top = tvref(th->maxstack);
  return (MSize)(top - bot);  /* Return minimum needed stack size. */
}

/* Traverse a thread object. */
static void gc_traverse_thread(global_State *g, lua_State *th)
{
  TValue *o, *top = th->top;
  for (o = tvref(th->stack)+1+LJ_FR2; o < top; o++)
    gc_marktv(g, o);
  if (g->gc.state == GCSinsideatomic) {
    top = tvref(th->stack) + th->stacksize;
    for (; o < top; o++)  /* Clear unmarked slots. */
      setnilV(o);
  }
  gc_markobj(g, tabref(th->env));
  lj_state_shrinkstack(th, gc_traverse_frames(g, th));
}

/* Propagate one gray object. Traverse it and turn it black. */
static void gc_propagate_mark(global_State *g)
{
  GCobj *o = gcref(g->gc.gray);
  int gct = o->gch.gct;
  MSize traversed;

  lua_assert(isgray(o));
  gray2black(o);
  setgcrefr(g->gc.gray, o->gch.gclist);  /* Remove from gray list. */
  if (LJ_LIKELY(gct == ~LJ_TTAB)) {
    GCtab *t = gco2tab(o);
    traversed = sizeof(GCtab) + sizeof(TValue) * t->asize +
			   sizeof(Node) * (t->hmask + 1);
    if (gc_traverse_tab(g, t) > 0)
      black2gray(o);  /* Keep weak tables gray. */
  } else if (LJ_LIKELY(gct == ~LJ_TFUNC)) {
    GCfunc *fn = gco2func(o);
    traversed = isluafunc(fn) ? sizeLfunc((MSize)fn->l.nupvalues) :
			   sizeCfunc((MSize)fn->c.nupvalues);
    gc_traverse_func(g, fn);
  } else if (LJ_LIKELY(gct == ~LJ_TPROTO)) {
    GCproto *pt = gco2pt(o);
    traversed = pt->sizept;
    gc_traverse_proto(g, pt);
  } else if (LJ_LIKELY(gct == ~LJ_TTHREAD)) {
    lua_State *th = gco2th(o);
    traversed = sizeof(lua_State) + sizeof(TValue) * th->stacksize;
    setgcrefr(th->gclist, g->gc.grayagain); /* Reinsert into grayagain list. */
    setgcref(g->gc.grayagain, o);
    black2gray(o);  /* Threads are never black. */
    gc_traverse_thread(g, th);
  } else {
#if LJ_HASJIT
    GCtrace *T = gco2trace(o);
    traversed = ((sizeof(GCtrace)+7)&~7) + (T->nins-T->nk)*sizeof(IRIns) +
	   T->nsnap*sizeof(SnapShot) + T->nsnapmap*sizeof(SnapEntry);
    gc_traverse_trace(g, T);
#else
    lua_assert(0);
    traversed = 0;
#endif
  }
  g->gc.memtrav += traversed;
}

/* -- Sweep phase --------------------------------------------------------- */

/* Type of GC free functions. */
typedef void (LJ_FASTCALL *GCFreeFunc)(global_State *g, GCobj *o);

/* GC free functions for LJ_TSTR .. LJ_TUDATA. ORDER LJ_T */
static const GCFreeFunc gc_freefunc[] = {
  (GCFreeFunc)lj_str_free,
  (GCFreeFunc)lj_func_freeuv,
  (GCFreeFunc)lj_state_free,
  (GCFreeFunc)lj_func_freeproto,
  (GCFreeFunc)lj_func_free,
#if LJ_HASJIT
  (GCFreeFunc)lj_trace_free,
#else
  (GCFreeFunc)0,
#endif
#if LJ_HASFFI
  (GCFreeFunc)lj_cdata_free,
#else
  (GCFreeFunc)0,
#endif
  (GCFreeFunc)lj_tab_free,
  (GCFreeFunc)lj_udata_free
};

/* Full sweep of a GC list. */
#define gc_fullsweep(g, p)	gc_sweep(g, (p), ~(uint32_t)0)

/* Partial sweep of a GC list. */
static GCRef *gc_sweep(global_State *g, GCRef *p, uint32_t lim)
{
  /* Mask with other white and LJ_GC_FIXED. Or LJ_GC_SFIXED on shutdown. */
  int ow = otherwhite(g);
  GCobj *o;
  while ((o = gcref(*p)) != NULL && lim-- > 0) {
    if (o->gch.gct == ~LJ_TTHREAD)  /* Need to sweep open upvalues, too. */
      gc_fullsweep(g, &gco2th(o)->openupval);
    if (((o->gch.marked ^ LJ_GC_WHITES) & ow)) {  /* Black or current white? */
      lua_assert(!isdead(g, o) || (o->gch.marked & LJ_GC_FIXED));
      makewhite(g, o);  /* Value is alive, change to the current white. */
      p = &o->gch.nextgc;
    } else {  /* Otherwise value is dead, free it. */
      lua_assert(isdead(g, o) || ow == LJ_GC_SFIXED);
      setgcrefr(*p, o->gch.nextgc);
      if (o == gcref(g->gc.root))
	setgcrefr(g->gc.root, o->gch.nextgc);  /* Adjust list anchor. */
      gc_freefunc[o->gch.gct - ~LJ_TSTR](g, o);
    }
  }
  return gcref(*p) ? p : NULL;
}

/* Sweep a list until a live object (or end of list). */
static GCRef *gc_sweeptolive(lua_State *L, GCRef *p, int *n) {
  GCRef *old = p;
  int i = 0;
  global_State *g = G(L);
  do {
    i++;
    p = gc_sweep(g, p, 1);
  } while (p == old);
  if (n) *n += i;
  return p;
}


/* Clear collected values from weak tables. */
static void gc_clearvalues(GCobj *o, GCobj *tail)
{
  while (o != tail) {
    GCtab *t = gco2tab(o);
    MSize i, asize = t->asize;
    for (i = 0; i < asize; i++) {
      /* Clear array slot when value is about to be collected. */
      TValue *tv = arrayslot(t, i);
      if (iscleared(tv))
        setnilV(tv);
    }
    if (t->hmask > 0) {
      Node *node = noderef(t->node);
      MSize i, hmask = t->hmask;
      for (i = 0; i <= hmask; i++) {
	Node *n = &node[i];
	/* Clear hash slot when value is marked for collecting. */
	if (!tvisnil(&n->val) && iscleared(&n->val)) {
	  setnilV(&n->val);
        }
      }
    }
    o = gcref(t->gclist);
  }
}

/* Clear collected keys from weak tables. */
static void gc_clearkeys(GCobj *o, GCobj *tail)
{
  while (o != tail) {
    GCtab *t = gco2tab(o);
    if (t->hmask > 0) {
      Node *node = noderef(t->node);
      MSize i, hmask = t->hmask;
      for (i = 0; i <= hmask; i++) {
	Node *n = &node[i];
	/* Clear hash slot when key is marked for collecting. */
	if (!tvisnil(&n->val) && iscleared(&n->key))
	  setnilV(&n->val);
      }
    }
    o = gcref(t->gclist);
  }
}


/* -- Finalizers --------------------------------------------------------- */

/* Call supplied finalizer 'mo' */
static void gc_call_finalizer(global_State *g, lua_State *L,
			      cTValue *mo, GCobj *o, int rethrow)
{
  /* Save and restore lots of state around the __gc callback. */
  uint8_t oldh = hook_save(g);
  uint8_t oldr = g->gc.flags & GCF_notrunning;
  int errcode;
  TValue *top;
  lj_trace_abort(g);
  hook_entergc(g);  /* Disable hooks and new traces during __gc. */
  g->gc.flags |= GCF_notrunning;  /* Prevent GC steps. */
  top = L->top;
  copyTV(L, top++, mo);
  if (LJ_FR2) setnilV(top++);
  setgcV(L, top, o, ~o->gch.gct);
  L->top = top+1;
  errcode = lj_vm_pcall(L, top, 1+0, -1);  /* Stack: |mo|o| -> | */
  hook_restore(g, oldh);
  g->gc.flags &= ~GCF_notrunning;
  g->gc.flags |= oldr;  /* Restore GC state. */
  if (errcode && rethrow)
    lj_err_throw(L, errcode);  /* Propagate errors. */
}

/* Finalize one object from top of the 'tobefnz' list. */
static void gc_finalize(lua_State *L, int rethrow)
{
  global_State *g = G(L);
  GCobj *o = gcref(g->gc.tobefnz);
  cTValue *mo;
  lua_assert(tvref(g->jit_base) == NULL);  /* Must not be called on trace. */

  /* Remove from tobefnz list */
  lua_assert(tofinalize(o));
  setgcrefr(g->gc.tobefnz, o->gch.nextgc);

  /* Roturn it to root list */
  setgcrefr(o->gch.nextgc, g->gc.root);
  setgcref(g->gc.root, o);
  o->gch.marked &= ~LJ_GC_FINALIZED;

  /* "sweep" */
  if (issweepphase(g))
    makewhite(g, o);

#if LJ_HASFFI
  if (o->gch.gct == ~LJ_TCDATA) {
    TValue tmp, *tv;
    cTValue *ctv;

    /* Resolve finalizer from table. */
    setcdataV(L, &tmp, gco2cd(o));
    tv = lj_tab_set(L, ctype_ctsG(g)->finalizer, &tmp);
    if (!tvisnil(tv)) {
      g->gc.nocdatafin = 0;
      copyTV(L, &tmp, tv);
      setnilV(tv);  /* Clear entry in finalizer table. */
      gc_call_finalizer(g, L, &tmp, o, rethrow);
      return;
    }

    /* Try miscmap then. */
    ctv = lj_tab_getinth(ctype_ctsG(g)->miscmap, -(int32_t)o->cd.ctypeid);
    if (ctv && tvistab(ctv) && (mo = lj_meta_fastg(g, tabV(tv), MM_gc)))
      gc_call_finalizer(g, L, mo, o, rethrow);
    return;
  }
#endif
  /* Resolve the __gc metamethod. */
  mo = lj_meta_fastg(g, tabref(o->gch.metatable), MM_gc);

  if (mo)
    gc_call_finalizer(g, L, mo, o, rethrow);
}

/* Call all pending finalizers in tobefnz list. */
static size_t gc_call_pending_finalizers(lua_State *L, int rethrow)
{
  global_State *g = G(L);
  size_t i, count = g->gc.finnum;
  lua_assert(!gcref(g->gc.tobefnz) || g->gc.finnum > 0);
  for (i = 0; i < count && gcref(g->gc.tobefnz); i++) {
    gc_finalize(L, rethrow);
  }
  /* Scale number of finalizers we run each cycle. */
  g->gc.finnum = !gcref(g->gc.tobefnz) ? 0 : g->gc.finnum * 2;
  return i;
}


/* Append objects from finobj to the tail of tobefnz. */
static void gc_separate_finalized(global_State *g, int all)
{
  GCRef *p = &g->gc.finobj;
  GCRef *lastnext = findlast(&g->gc.tobefnz);
  GCobj *o;
  while ((o = gcref(*p)) != NULL) {
    lua_assert(tofinalize(o));
    if (!(iswhite(o) || all)) {
      p = &o->gch.nextgc;  /* Nothing to do. */
    } else {  
      *p = o->gch.nextgc; /* Unlink */
      /* Append to tail of lastnext. */
      setgcrefr(o->gch.nextgc, *lastnext);
      setgcref(*lastnext, o);
      lastnext = &o->gch.nextgc;
    }
  }
}

/* Put object with finalizer on the finobj list. */
void lj_gc_checkfinalizer(lua_State *L, GCobj *o)
{
  global_State *g = G(L);
  GCobj *p;
  GCRef *ref;

  /* Already separated or being finalized. */
  if (tofinalize(o))
    return;

  /* Avoid removing current sweep object. */
  if (mref(g->gc.sweep, GCRef) == &o->gch.nextgc) {
    lua_assert(issweepphase(g));
    setmref(g->gc.sweep, gc_sweeptolive(L, mref(g->gc.sweep, GCRef), NULL));
    lua_assert(!(mref(g->gc.sweep, GCRef) == &o->gch.nextgc));
  }

  /* This is not as bad as it seems. Objects are setmetatabled()
   * right after creation (which links to gc.root) most of the time. */
  for (ref = &g->gc.root, p = gcref(g->gc.root);
      p != o;
      ref = &p->gch.nextgc, p = gcnext(p)) {};

  /* Unlink it from gc.root chain. */
  setgcref(*ref, gcnext(o));

  /* And put it on finobj list. */
  setgcrefr(o->gch.nextgc, g->gc.finobj);
  setgcref(g->gc.finobj, o);

  /* Alive and separated. */
  o->gch.marked |= LJ_GC_FINALIZED;
  if (issweepphase(g))
    makewhite(g, o);
}


/* -- Collector ----------------------------------------------------------- */

/* Adjust how much memory is owed. */
void lj_gc_setdebt(global_State *g, MDiff debt)
{
  g->gc.total -= (debt - g->gc.debt);
  g->gc.debt = debt;
  g->gc.debt32 = debt > 0;
}

/* Estimate pause to wait between gc cycles. */
static void gc_setpause(global_State *g, MDiff estimate) {
  MDiff threshold, debt;
  estimate = estimate / PAUSEADJ;  /* adjust 'estimate' */
  if (!estimate) estimate++;
  threshold = (g->gc.pause < LJ_MAX_MEM / estimate)  /* overflow? */
            ? estimate * g->gc.pause  /* no overflow */
            : LJ_MAX_MEM;  /* overflow; truncate to maximum */
  debt = gc_gettotalbytes(g);
  lua_assert(debt >= 0);
  debt -= threshold;
  lj_gc_setdebt(g, debt);
}

/* Fast forward already swept objects and start in string phase.  */
static int gc_entersweep(lua_State *L) {
  global_State *g = G(L);
  int n = 0;
  g->gc.sweepstr = 0;
  g->gc.state = GCSswpstr;  /* Start of sweep phase. */
  lua_assert(!mref(g->gc.sweep, GCRef));
  setmref(g->gc.sweep, gc_sweeptolive(L, &g->gc.root, &n));
  return n;
}

/* Free all remaining GC objects. */
void lj_gc_freeall(lua_State *L)
{
  global_State *g = G(L);
  MSize i, strmask;
  /* Collect all finalizable objects */
  gc_separate_finalized(g, 1);
  /* Call all finalizers (don't throw). */
  g->gc.finnum = LJ_MAX_MEM;
  gc_call_pending_finalizers(L, 0);
  /* Free everything, except super-fixed objects (the main thread). */
  g->gc.currentwhite = LJ_GC_WHITES | LJ_GC_SFIXED;
  gc_fullsweep(g, &g->gc.finobj);
  gc_fullsweep(g, &g->gc.root);
  strmask = g->strmask;
  for (i = 0; i <= strmask; i++)  /* Free all string hash chains. */
    gc_fullsweep(g, &g->strhash[i]);
}


/* Atomic part of the GC cycle, transitioning from mark to sweep phase. */
static MDiff atomic(global_State *g, lua_State *L)
{
  GCobj *weak_o, *allweak_o;
  MDiff work = -((long)g->gc.memtrav);  /* start counting work */

  gc_mark_uv(g);  /* Need to remark open upvalues (the thread may be dead). */
  gc_propagate_all(g);  /* Propagate any left-overs. */

  g->gc.state = GCSinsideatomic;
  lua_assert(!iswhite(obj2gco(mainthread(g))));
  gc_markobj(g, L);  /* Mark running thread. */
  gc_traverse_curtrace(g);  /* Traverse current trace. */
  gc_mark_gcroot(g);  /* Mark GC roots (again). */
  gc_propagate_all(g);  /* Propagate all of the above. */

  work += g->gc.memtrav;
  gc_retraverse_grays(g);
  work -= g->gc.memtrav;

  gc_converge_ephemerons(g);

  /* At this point, all strongly accessible objects are marked. */
  /* Clear values from weak tables, before checking finalizers. */
  gc_clearvalues(gcref(g->gc.weak), NULL);
  gc_clearvalues(gcref(g->gc.allweak), NULL);
 
  /* Discover what needs to be finalized. */ 
  work += g->gc.memtrav;  /* Stop counting (objects being finalized). */
  weak_o = gcref(g->gc.weak); allweak_o = gcref(g->gc.allweak);
  gc_separate_finalized(g, 0);  /* Separate objects to be finalized. */
  g->gc.finnum = GCFINHEADSTART;
  gc_mark_finalized(g); /* Mark what will be finalized. */
  gc_propagate_all(g); /* Remark, to propagate 'preserveness'. */
  work -= g->gc.memtrav;  /* Restart counting. */

  gc_converge_ephemerons(g);

  /* All marking done, clear weak tables. */
  gc_clearkeys(gcref(g->gc.ephemeron), NULL);
  gc_clearkeys(gcref(g->gc.allweak), NULL);
  /* Clear values from resurrected weak tables. */
  gc_clearvalues(gcref(g->gc.weak), weak_o);
  gc_clearvalues(gcref(g->gc.allweak), allweak_o);

  lj_buf_shrink(L, &g->tmpbuf);  /* Shrink temp buffer. */

  /* Prepare for sweep phase. */
  g->gc.currentwhite = (uint8_t)otherwhite(g);  /* Flip current white. */
  g->strempty.marked = g->gc.currentwhite;
  work += g->gc.memtrav;  /* Complete counting. */
  return work;  /* estimate of memory marked by 'atomic' */
  // TBD MERGE: setmref(g->gc.sweep, &g->gc.root);
}

static size_t gc_sweepstep(lua_State *L, global_State *g,
                         int nextstate, GCRef *nextlist) {
  if (mref(g->gc.sweep, GCRef)) {
    setmref(g->gc.sweep, gc_sweep(g, mref(g->gc.sweep, GCRef), GCSWEEPMAX));
    if (mref(g->gc.sweep, GCRef)) /* is there still something to sweep? */
      return (GCSWEEPMAX * GCSWEEPCOST);
  }
  /* else enter next state */
  g->gc.state = nextstate;
  setmref(g->gc.sweep, nextlist);
  return 0;
}


/* GC state machine. Returns a cost estimate for each step performed. */
static size_t gc_onestep(lua_State *L)
{
  global_State *g = G(L);
  switch (g->gc.state) {
    case GCSpause:
      /* Start to count memory traversed. */
      g->gc.memtrav = (g->strmask + 1) * sizeof(GCRef);
      gc_mark_start(g);  /* Start a new GC cycle by marking all GC roots. */
      g->gc.state = GCSpropagate;
      return g->gc.memtrav;
    case GCSpropagate: {
      long oldtrav = g->gc.memtrav;
      lua_assert(gcref(g->gc.gray));
      gc_propagate_mark(g); 
      if (!gcref(g->gc.gray)) {
        g->gc.state = GCSatomic;
        g->gc.flags &= ~GCF_stayontrace;
      }
      return g->gc.memtrav - oldtrav;
    }
    case GCSatomic: {
      size_t work, sw;
      gc_propagate_all(g);
      g->gc.estimate = g->gc.memtrav;  /* save what was counted */;
      work = atomic(g, L);
      g->gc.estimate += work;
      g->gc.flags |= GCF_stayontrace;
      sw = gc_entersweep(L); /* -> GCSswpstr */
      return work + sw * GCSWEEPCOST;
    }
    case GCSswpstr: {
      if (g->gc.sweepstr >= g->strmask) {
        g->gc.state = GCSswpallgc;  /* All string hash chains sweeped. */
        return 0;
      }
      gc_fullsweep(g, &g->strhash[g->gc.sweepstr++]);  /* Sweep one chain. */
      return GCSWEEPSTRCOST;
    }
    case GCSswpallgc:  /* sweep "regular" objects */
      return gc_sweepstep(L, g, GCSswpfinobj, &g->gc.finobj);
    case GCSswpfinobj:  /* sweep objects with finalizers */
      return gc_sweepstep(L, g, GCSswptobefnz, &g->gc.tobefnz);
    case GCSswptobefnz:  /* sweep objects to be finalized */
      return gc_sweepstep(L, g, GCSswpend, NULL);
    case GCSswpend: /* finish sweeps */
      makewhite(g, obj2gco(mainthread(g)));  /* sweep main thread */
      if (g->strnum <= (g->strmask >> 2) && g->strmask > LJ_MIN_STRTAB*2-1)
	lj_str_resize(L, g->strmask >> 1);  /* Shrink string table. */
      g->gc.state = GCScallfin;
      g->gc.flags &= ~GCF_stayontrace;
      g->gc.nocdatafin = 1;
      return 0;
    case GCScallfin:  /* state to finish calling finalizers */
      /* do nothing here; should be handled by 'lj_gc_step' */
      g->gc.state = GCSpause;  /* finish collection */
      g->gc.flags |= GCF_stayontrace;
#if LJ_HASFFI
      if (!g->gc.nocdatafin) lj_tab_rehash(L, ctype_ctsG(g)->finalizer);
#endif
      return 0;
    default: lua_assert(0); return 0;
  }
}

/* Perform a limited amount of incremental GC steps. */
int LJ_FASTCALL lj_gc_step(lua_State *L)
{
  global_State *g = G(L);
  int32_t ostate = g->vmstate;
  MDiff debt;

  if (g->gc.flags & GCF_notrunning) {
    lj_gc_setdebt(g, -GCSTEPSIZE * 10);
    return -1;
  }

  /* Adjust debt and transition VM to GC state. */ 
  debt = g->gc.debt;
  debt = (debt / STEPMULADJ) + 1;
  debt = (debt < LJ_MAX_MEM / g->gc.stepmul) ? debt * g->gc.stepmul : LJ_MAX_MEM;
  setvmstate(g, GC);

  do {
    /* Don't run atomic on trace */
    if (g->gc.state == GCSatomic && tvref(g->jit_base))
      break;
    if (g->gc.state == GCScallfin && gcref(g->gc.tobefnz)) {
      int n = gc_call_pending_finalizers(L, 1);
      debt -= (n * GCFINALIZECOST);
    } else {  /* Perform one single step. */
      MDiff work = gc_onestep(L);
      debt -= work;
    }
  } while (debt > -GCSTEPSIZE && g->gc.state != GCSpause);

  if (g->gc.state == GCSpause) {
    gc_setpause(g, g->gc.estimate);  /* Pause until next cycle. */
    g->vmstate = ostate;
    return 1; /* Finished. */
  }

  /* Keep going. XXX: return -1 when overlimit. */
  debt = (debt / g->gc.stepmul) * STEPMULADJ;  /* convert 'work units' to Kb */
  lj_gc_setdebt(g, debt);
  gc_call_pending_finalizers(L, 1);
  g->vmstate = ostate;
  return g->gc.state == GCSatomic ? -1 : 0;
}

/* Ditto, but fix the stack top first. */
void LJ_FASTCALL lj_gc_step_fixtop(lua_State *L)
{
  if (curr_funcisL(L)) L->top = curr_topL(L);
  lj_gc_step(L);
}

#if LJ_HASJIT
/* Perform multiple GC steps. Called from JIT-compiled code. */
int LJ_FASTCALL lj_gc_step_jit(global_State *g, MSize steps)
{
  lua_State *L = gco2th(gcref(g->cur_L));
  L->base = tvref(G(L)->jit_base);
  L->top = curr_topL(L);
  while (steps-- > 0 && lj_gc_step(L) == 0)
    ;
  /* Return 1 to force a trace exit. */
  return (G(L)->gc.state == GCSatomic || G(L)->gc.state == GCScallfin);
}
#endif

/* Perform a full GC cycle. */
void lj_gc_fullgc(lua_State *L)
{
  global_State *g = G(L);
  int32_t ostate = g->vmstate;

  setvmstate(g, GC);
  gc_call_pending_finalizers(L, 1);
  if (keepinvariant(g)) {  /* may there be some black objects? */
    /* must sweep all objects to turn them back to white
       (as white has not changed, nothing will be collected) */
    gc_entersweep(L);
  }
  while (g->gc.state != GCSpause) gc_onestep(L); /* Finish previous run */
  while (g->gc.state == GCSpause) gc_onestep(L); /* Start collecting */
  while (g->gc.state != GCSpause) gc_onestep(L); /* And run */
  gc_setpause(g, gc_gettotalbytes(g));
  gc_call_pending_finalizers(L, 1);
  g->vmstate = ostate;
}

/* -- Allocator ----------------------------------------------------------- */
/* Call pluggable memory allocator to allocate or resize a fragment. */
void *lj_mem_realloc(lua_State *L, void *p, GCSize osz, GCSize nsz)
{
  global_State *g = G(L);
  lua_assert((osz == 0) == (p == NULL));
  p = g->allocf(g->allocd, p, osz, nsz);
  if (p == NULL && nsz > 0)
    lj_err_mem(L);
  lua_assert((nsz == 0) == (p == NULL));
  lua_assert(checkptr32(p));
  g->gc.debt -= osz;
  g->gc.debt += nsz;
  g->gc.debt32 = g->gc.debt > 0;
  return p;
}

/* Allocate new GC object and link it to the root set. */
void * LJ_FASTCALL lj_mem_newgco(lua_State *L, GCSize size)
{
  global_State *g = G(L);
  GCobj *o = (GCobj *)g->allocf(g->allocd, NULL, 0, size);
  if (o == NULL)
    lj_err_mem(L);
  lua_assert(checkptr32(o));
  lua_assert((MDiff)(g->gc.debt + size) >= g->gc.debt); /* XXX: clamp? */
  setgcrefr(o->gch.nextgc, g->gc.root);
  setgcref(g->gc.root, o);
  newwhite(g, o);
  /* Clamp debt. */
  if ((MDiff)(g->gc.debt + size) >= g->gc.debt)
    g->gc.debt += size;
  return o;
}

/* Resize growable vector. */
void *lj_mem_grow(lua_State *L, void *p, MSize *szp, MSize lim, MSize esz)
{
  MSize sz = (*szp) << 1;
  if (sz < LJ_MIN_VECSZ)
    sz = LJ_MIN_VECSZ;
  if (sz > lim)
    sz = lim;
  p = lj_mem_realloc(L, p, (*szp)*esz, sz*esz);
  *szp = sz;
  return p;
}

