/* Lua 5.3 bitwise operators handling. */

#include "lj_obj.h"
#include "lj_strscan.h"
#if LJ_HASFFI
#include "lj_ctype.h"
#include "lj_cdata.h"
#include "lj_cconv.h"
#endif
#include "lj_bitwise.h"
#include "lj_crecord.h"
#include "lj_dispatch.h"
#include "lj_iropt.h"

#if LJ_53
/* Decode arguments for interpreter dispatch */
static int get_arg(lua_State *L, cTValue *o, int64_t *res)
{
  TValue tmp;
#if LJ_HASFFI
  CTypeID id = 0;
  *res = 0;
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
    if (lj_cconv_ct_ct(cts, ctype_get(cts, id), s,
		   (uint8_t *)res, sp, CCF_NOERROR) < 0)
      return -1;
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

#define emitir(ot, a, b)	(lj_ir_set(J, (ot), (a), (b)), lj_opt_fold(J))
#define emitconv(a, dt, st, flags) \
  emitir(IRT(IR_CONV, (dt)), (a), (st)|((dt) << 5)|(flags))

/* Same as lj_opt_narrow_tobit, but does not raise trace error. */
static TRef bit_narrow_int(jit_State *J, TRef r)
{
  if (tref_isstr(r))
    r = emitir(IRTG(IR_STRTO, IRT_NUM), r, 0);
  if (tref_isnum(r))  /* Conversion may be narrowed, too. See above. */
    return emitir(IRTI(IR_TOBIT), r, lj_ir_knum_tobit(J));
  if (!tref_isinteger(r))
    return 0;
  return lj_opt_narrow_stripov(J, r, IR_SUBOV, (IRT_INT<<5)|IRT_INT|IRCONV_TOBIT);
}

/* Narrow to specified type. */
static TRef bit_narrow_id(jit_State *J, CTypeID wantid, TRef tr, cTValue *o)
{
  CTState *cts = ctype_ctsG(J2G(J));
  if (!wantid)
    return bit_narrow_int(J, tr);
#if LJ_HASJIT && LJ_HASFFI
  return lj_crec_ct_tv(J, ctype_get(cts, wantid), 0, tr, o);
#else
  return 0;
#endif
}

/* Record. */
TRef lj_rec_bitwise(jit_State *J, TRef rb, TRef rc, cTValue *rbv, cTValue *rcv, MMS mm)
{
  CTState *cts = ctype_ctsG(J2G(J));
  TRef r1, r2 = 0;
  CTypeID id1 = 0, id2 = 0;
#if LJ_HASJIT && LJ_HASFFI
  id1 = lj_crec_bit64_type(cts, rbv);
  id2 = lj_crec_bit64_type(cts, rcv);
#endif
  /* For shifts, the shift arg might stay as int */
  if (mm >= MM_shl) {
    if (id2) {
      r2 = bit_narrow_id(J, CTID_INT64, rc, rcv);
      if (!tref_isinteger(r2))
        r2 = emitconv(r2, IRT_INT, tref_type(r2), 0);
    } else {
      r2 = bit_narrow_int(J, rc);
      if (!r2) return 0; /* Not even int? Bail. */
    }
    id2 = 0; /* The arg is never widest */
  }
  if (id2 > id1) /* Determine widest type */
    id1 = id2;
  r1 = bit_narrow_id(J, id1, rb, rbv); /* And resolve those */
  if (!r1) return 0;

  if (!r2 && mm != MM_bnot) { /* Only if not shift or unary */
    r2 = bit_narrow_id(J, id1, rc, rcv);
    if (!r2) return 0;
  }

  /* All set now. id1 holds the output type (or 0 if INT). */
  uint32_t irop = IR_BNOT + mm - MM_bnot;
  /* Type-specific and boxed? */
#if LJ_HASJIT && LJ_HASFFI
  if (id1) {
    TRef tr = emitir(IRT(irop, id1-CTID_INT64+IRT_I64), r1, r2);
    return emitir(IRTG(IR_CNEWI, IRT_CDATA), lj_ir_kint(J, id1), tr);
  }
#endif
  /* Otherwise plain I32 */
  return emitir(IRTI(irop), r1, r2);
}

#endif
