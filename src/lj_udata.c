/*
** Userdata handling.
** Copyright (C) 2005-2014 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_udata_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_udata.h"

GCudata *lj_udata_new(lua_State *L, MSize sz, GCtab *env)
{
  GCudata *ud = lj_mem_newgco(L, sizeof(GCudata) + sz);
  global_State *g = G(L);
  newwhite(g, ud);  /* Not finalized. */
  ud->gct = ~LJ_TUDATA;
  ud->udtype = UDTYPE_USERDATA;
  ud->len = sz;
  /* NOBARRIER: The GCudata is new (marked white). */
  setgcrefnull(ud->metatable);
  if (env) {
    setgcref(ud->env, obj2gco(env));
    ud->envtt = ~LJ_TTAB;
  } else {
    setgcrefnull(ud->env);
    ud->envtt = ~LJ_TNIL;
  }
  return ud;
}

void LJ_FASTCALL lj_udata_free(global_State *g, GCudata *ud)
{
  lj_mem_free(g, ud, sizeudata(ud));
}

