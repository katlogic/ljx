/* Lua 5.3 bitwise operators handling. Recording is in lj_record.c */
#include "lj_obj.h"
#include "lj_strscan.h"
#if LJ_HASFFI
#include "lj_ctype.h"
#include "lj_cdata.h"
#include "lj_cconv.h"
#endif
#include "lj_bitwise.h"

static int get_arg(lua_State *L, cTValue *o, int64_t *res)
{
  TValue tmp;
#if LJ_HASFFI
  CTypeID id = 0;
  if (tviscdata(o)) {
    CTState *cts = ctype_cts(L);
    uint8_t *sp = (uint8_t *)cdataptr(cdataV(o));
    CTypeID sid = cdataV(o)->ctypeid;
    CType *s = ctype_get(cts, sid);
    if (ctype_isref(s->info)) {
      sp = *(void **)sp;
      sid = ctype_cid(s->info);
    }
    s = ctype_raw(cts, sid);
    if (ctype_isenum(s->info)) s = ctype_child(cts, s);
    if ((s->info & (CTMASK_NUM|CTF_BOOL|CTF_FP|CTF_UNSIGNED)) ==
        CTINFO(CT_NUM, CTF_UNSIGNED) && s->size == 8)
      id = CTID_UINT64;  /* Use uint64_t, since it has the highest rank. */
    else
      id = CTID_INT64;  /* Use int64_t, unless already set. */
    lj_cconv_ct_ct(cts, ctype_get(cts, id), s,
		   (uint8_t *)&res, sp, CCF_ARG(1)); /* TBD: report correct arg */
    return id;
  } else
#endif
  if (tvisstr(o)) {
    if (!lj_strscan_number(strV(o), &tmp))
      return -1;
    o = &tmp;
  }
  if (LJ_LIKELY(tvisint(o))) {
    *res = (uint32_t)intV(o);
    return 0;
  } 
  if (LJ_LIKELY(tvisnumber(o))) {
    *res = (uint32_t)lj_num2bit(numV(o));
    return 0;
  }
  return -1;
}

#define B_METHODS(_) \
_(bnot,=~x) _(idiv, /=y) _(band, &=y) _(bor, |=y) \
_(bxor, ^=y) _(shl, <<=y) _(shr, >>=y)
#define CASE(name, suff) \
  case MM_##name: x suff; break;

int lj_vm_foldbit(lua_State *L, TValue *ra, cTValue *rb, cTValue *rc,
		      MMS mm)
{
  int id1, id2;
  int64_t a,b;

  if (((id1 = get_arg(L, rb, &a)) == -1) || ((id2 = get_arg(L, rc, &b)) == -1))
    return -1; /* call metamethod */
  /* expand to common type, with the exception of shifts */
  if (mm < MM_shl && id2 > id1)
    id1 = id2;
  /* int32 */
  if (!id1) {
    uint32_t x = a;
    uint32_t y = b;
    switch (mm) {
      B_METHODS(CASE)
      default: return -1;
    }
    setintV(ra, x);
  }
#if LJ_HASFFI
  else {
    uint64_t x = a;
    uint64_t y = b;
    GCcdata *cd;
    switch (mm) {
      B_METHODS(CASE)
      default: return -1;
    }
    cd = lj_cdata_new_(L, id1, 8);
    *(uint64_t *)cdataptr(cd) = x;
    setcdataV(L, ra, cd);
  }
#endif
  return id1;
}

