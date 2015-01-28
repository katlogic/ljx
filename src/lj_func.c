/*
** Function handling (prototypes, functions and upvalues).
** Copyright (C) 2005-2015 Mike Pall. See Copyright Notice in luajit.h
**
** Upvalue handling rewrite and closure lifting.
** Copyright (C) 2014 Karel Tuma. See Copyright Notice in luajit.h
**
** Portions taken verbatim or adapted from the Lua interpreter.
** Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h
*/

#define lj_func_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_func.h"
#include "lj_trace.h"
#include "lj_vm.h"

/* -- Prototypes ---------------------------------------------------------- */

void LJ_FASTCALL lj_func_freeproto(global_State *g, GCproto *pt)
{
  lj_mem_free(g, pt, pt->sizept);
}

/* -- Upvalues ------------------------------------------------------------ */

static void unlinkuv(GCupval *uv)
{
  lua_assert(uvprev(uvnext(uv)) == uv && uvnext(uvprev(uv)) == uv);
  setgcrefr(uvnext(uv)->prev, uv->prev);
  setgcrefr(uvprev(uv)->next, uv->next);
}

/* Find existing open upvalue for a stack slot or create a new one. */
static GCupval *func_finduv(lua_State *L, TValue *slot)
{
  global_State *g = G(L);
  GCRef *pp = &L->openupval;
  GCupval *p;
  GCupval *uv;
  /* Search the sorted list of open upvalues. */
  while (gcref(*pp) != NULL && uvval((p = gco2uv(gcref(*pp)))) >= slot) {
    lua_assert(!p->closed && uvval(p) != &p->tv);
    if (uvval(p) == slot) {  /* Found open upvalue pointing to same slot? */
      if (isdead(g, obj2gco(p)))  /* Resurrect it, if it's dead. */
	flipwhite(obj2gco(p));
      return p;
    }
    pp = &p->nextgc;
  }
  /* No matching upvalue found. Create a new one. */
  uv = lj_mem_newt(L, sizeof(GCupval), GCupval);
  newwhite(g, uv);
  uv->gct = ~LJ_TUPVAL;
  uv->closed = 0;  /* Still open. */
  setmref(uv->v, slot);  /* Pointing to the stack slot. */
  /* NOBARRIER: The GCupval is new (marked white) and open. */
  setgcrefr(uv->nextgc, *pp);  /* Insert into sorted list of open upvalues. */
  setgcref(*pp, obj2gco(uv));
  setgcref(uv->prev, obj2gco(&g->uvhead));  /* Insert into GC list, too. */
  setgcrefr(uv->next, g->uvhead.next);
  setgcref(uvnext(uv)->prev, obj2gco(uv));
  setgcref(g->uvhead.next, obj2gco(uv));
  lua_assert(uvprev(uvnext(uv)) == uv && uvnext(uvprev(uv)) == uv);
  return uv;
}

/* Create an empty and closed upvalue. */
static GCupval *func_emptyuv(lua_State *L)
{
  GCupval *uv = (GCupval *)lj_mem_newgco(L, sizeof(GCupval));
  uv->gct = ~LJ_TUPVAL;
  uv->closed = 1;
  setnilV(&uv->tv);
  setmref(uv->v, &uv->tv);
  return uv;
}

/* Close all open upvalues pointing to some stack level or above. */
void LJ_FASTCALL lj_func_closeuv(lua_State *L, TValue *level)
{
  GCupval *uv;
  global_State *g = G(L);
  while (gcref(L->openupval) != NULL &&
	 uvval((uv = gco2uv(gcref(L->openupval)))) >= level) {
    GCobj *o = obj2gco(uv);
    lua_assert(!isblack(o) && !uv->closed && uvval(uv) != &uv->tv);
    setgcrefr(L->openupval, uv->nextgc);  /* No longer in open list. */
    if (isdead(g, o)) {
      lj_func_freeuv(g, uv);
    } else {
      unlinkuv(uv);
      lj_gc_closeuv(g, uv);
    }
  }
}

void LJ_FASTCALL lj_func_freeuv(global_State *g, GCupval *uv)
{
  if (!uv->closed)
    unlinkuv(uv);
  lj_mem_freet(g, uv);
}

/* -- Functions (closures) ------------------------------------------------ */

GCfunc *lj_func_newC(lua_State *L, MSize nelems, GCtab *env)
{
  GCfunc *fn = (GCfunc *)lj_mem_newgco(L, sizeCfunc(nelems));
  fn->c.gct = ~LJ_TFUNC;
  fn->c.ffid = FF_C;
  fn->c.nupvalues = (uint8_t)nelems;
  /* NOBARRIER: The GCfunc is new (marked white). */
  setmref(fn->c.pc, &G(L)->bc_cfunc_ext);
  setgcref(fn->c.env, obj2gco(env));
  return fn;
}

static GCfunc *func_newL(lua_State *L, GCproto *pt, GCtab *env)
{
  uint32_t count;
  GCfunc *fn = (GCfunc *)lj_mem_newgco(L, sizeLfunc((MSize)pt->sizeuv));
  /* Initially points to itself */
  setgcref(fn->l.prev_ENV, obj2gco(fn));
  setgcref(fn->l.next_ENV, obj2gco(fn));
  fn->l.gct = ~LJ_TFUNC;
  fn->l.ffid = FF_LUA;
  fn->l.nupvalues = 0;  /* Set to zero until upvalues are initialized. */
  /* NOBARRIER: Really a setgcref. But the GCfunc is new (marked white). */
  setmref(fn->l.pc, proto_bc(pt));
  setgcref(fn->l.env, obj2gco(env));
  /* Saturating 3 bit counter (0..7) for created closures. */
  count = (uint32_t)pt->flags + PROTO_CLCOUNT;
  pt->flags = (uint8_t)(count - ((count >> PROTO_CLC_BITS) & PROTO_CLCOUNT));
  return fn;
}

static GCfunc *lj_func_newL(lua_State *L, GCproto *pt, GCfuncL *parent);
/* Recursively instantiate closures */
static inline
void lj_func_init_closure(lua_State *L, uintptr_t i, GCproto *pttab, GCfuncL *parent, GCupval *uv)
{
  setfuncV(L, &uv->tv, lj_func_newL(L, &proto_kgc(pttab, (~i))->pt, parent));
}

/* Create a new Lua function with empty upvalues. */
GCfunc *lj_func_newL_empty(lua_State *L, GCproto *pt, GCtab *env)
{
  GCfunc *fn = func_newL(L, pt, env);
  MSize i, nuv = pt->sizeuv;
  for (i = 0; i < nuv; i++) {
    GCupval *uv = func_emptyuv(L);
    uint32_t v = proto_uv(pt)[i];
    uint8_t flags = (v >> PROTO_UV_SHIFT);
    if (flags == UV_HOLE) {
      setgcrefnull(fn->l.uvptr[i]);
      continue;
    }
    uv->flags = flags;
    uv->dhash = (uint32_t)(uintptr_t)pt ^ ((uint32_t)proto_uv(pt)[i] << 24);
    /* NOBARRIER: The GCfunc is new (marked white). */
    setgcref(fn->l.uvptr[i], obj2gco(uv));
    /* TBD: sub-closures and env barriers? */
    if (flags == UV_ENV) {
      settabV(L, &uv->tv, env);
    } else if (flags == UV_CLOSURE)
      lj_func_init_closure(L, v & PROTO_UV_MASK, pt, &fn->l, uv);
  }
  fn->l.nupvalues = (uint8_t)nuv;
  return fn;
}

/* Do a GC check and create a new Lua function with inherited upvalues. */
static GCfunc *lj_func_newL(lua_State *L, GCproto *pt, GCfuncL *parent)
{
  GCfunc *fn;
  GCRef *puv;
  MSize i, nuv;
  TValue *base;

  fn = func_newL(L, pt, tabref(parent->env));
  /* NOBARRIER: The GCfunc is new (marked white). */
  puv = parent->uvptr;
  nuv = pt->sizeuv;
  base = L->base;

  fn->l.nupvalues = 0;
  /* TBD: sub-closures and env barriers? */
  for (i = 0; i < nuv; i++) {
    uint32_t v = proto_uv(pt)[i];
    GCupval *uv;
    switch (v >> PROTO_UV_SHIFT) {
      case UV_CLOSURE:
        uv = func_emptyuv(L);
        uv->flags = v >> PROTO_UV_SHIFT;
        setgcref(fn->l.uvptr[i], obj2gco(uv));
        lj_func_init_closure(L, v & PROTO_UV_MASK, pt, &fn->l, uv);
        break;
      case UV_ENV:
        //lua_assert(0);
      case UV_CHAINED:
        uv = &gcref(puv[v & PROTO_UV_MASK])->uv;
        lua_assert(uv);
        if (uv->flags & UV_ENV) {
          setgcref(fn->l.next_ENV, obj2gco(parent));
          fn->l.prev_ENV = parent->prev_ENV;
          setgcref(gcref(fn->l.prev_ENV)->fn.l.next_ENV, obj2gco(fn));
          setgcref(parent->prev_ENV, obj2gco(fn));
        }
        setgcref(fn->l.uvptr[i], obj2gco(uv));
        break;
      case UV_IMMUTABLE:
      case UV_LOCAL:
        uv = func_finduv(L, base + (v & 0xff));
        uv->flags = v >> PROTO_UV_SHIFT;
        uv->dhash = (uint32_t)(uintptr_t)mref(parent->pc, char) ^ (v << 24);
        setgcref(fn->l.uvptr[i], obj2gco(uv));
        break;
      case UV_HOLE:
        setgcrefnull(fn->l.uvptr[i]);
        continue;
      default:
        lua_assert(0);
    }
  }
  fn->l.nupvalues = (uint8_t)nuv;
  return fn;
}
  
GCfunc *lj_func_newL_gc(lua_State *L, GCproto *pt, GCfuncL *parent)
{
  lj_gc_check_fixtop(L);
  return lj_func_newL(L, pt, parent);
}

void LJ_FASTCALL lj_func_free(global_State *g, GCfunc *fn)
{
  MSize size = isluafunc(fn) ? sizeLfunc((MSize)fn->l.nupvalues) :
			       sizeCfunc((MSize)fn->c.nupvalues);
  /* NOBARRIER: Should be handled in ESETV/lua_setupvalue. */
  if (isluafunc(fn)) {
    setgcrefr(gcref(fn->l.next_ENV)->fn.l.prev_ENV, fn->l.prev_ENV);
    setgcrefr(gcref(fn->l.prev_ENV)->fn.l.next_ENV, fn->l.next_ENV);
  }
  lj_mem_free(g, fn, size);
}

