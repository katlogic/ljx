#ifndef _LJX_BITWISE_H
#define _LJX_BITWISE_H

#include "lj_obj.h"
#include "lj_ir.h"
#include "lj_jit.h"
LJ_FUNC TRef ljx_rec_bitwise(jit_State *J, TRef rb, TRef rc, cTValue *rbv,
                      cTValue *rcv, MMS mm);
LJ_FUNC int ljx_vm_foldbit(lua_State *L, TValue *ra, cTValue *rb, cTValue *rc,
		      MMS mm);

#endif


