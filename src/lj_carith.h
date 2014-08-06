/*
** C data arithmetic.
** Copyright (C) 2005-2014 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_CARITH_H
#define _LJ_CARITH_H

#include "lj_obj.h"

#if LJ_HASFFI

LJ_FUNC int lj_carith_op(lua_State *L, MMS mm);

#if LJ_32
LJ_FUNC uint64_t lj_carith_shl64(uint64_t x, int32_t sh);
LJ_FUNC uint64_t lj_carith_shr64(uint64_t x, int32_t sh);
LJ_FUNC uint64_t lj_carith_sar64(uint64_t x, int32_t sh);
LJ_FUNC uint64_t lj_carith_rol64(uint64_t x, int32_t sh);
LJ_FUNC uint64_t lj_carith_ror64(uint64_t x, int32_t sh);
#endif
LJ_FUNC uint64_t lj_carith_shift64(uint64_t x, int32_t sh, int op);
LJ_FUNC uint64_t lj_carith_check64(lua_State *L, int narg, CTypeID *id);

#if LJ_32 && LJ_HASJIT
LJ_FUNC int64_t lj_carith_mul64(int64_t x, int64_t k);
#endif
LJ_FUNC uint64_t lj_carith_divu64(uint64_t a, uint64_t b);
LJ_FUNC int64_t lj_carith_divi64(int64_t a, int64_t b);
LJ_FUNC uint64_t lj_carith_modu64(uint64_t a, uint64_t b);
LJ_FUNC int64_t lj_carith_modi64(int64_t a, int64_t b);
LJ_FUNC uint64_t lj_carith_powu64(uint64_t x, uint64_t k);
LJ_FUNC int64_t lj_carith_powi64(int64_t x, int64_t k);

#define B64DEF(name) \
  static LJ_AINLINE uint64_t lj_carith_##name(uint64_t x, int32_t sh)
B64DEF(shl64) { return x << (sh&63); }
B64DEF(shr64) { return x >> (sh&63); }
B64DEF(sar64) { return (uint64_t)((int64_t)x >> (sh&63)); }
B64DEF(rol64) { return lj_rol(x, (sh&63)); }
B64DEF(ror64) { return lj_ror(x, (sh&63)); }
#undef B64DEF

#endif

#endif
