/*
** Lua parser (source code -> bytecode).
** Copyright (C) 2005-2014 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_PARSE_H
#define _LJ_PARSE_H

#include "lj_obj.h"
#include "lj_lex.h"

LJ_FUNC GCproto *lj_parse(LexState *ls);
LJ_FUNC GCstr *lj_parse_keepstr(LexState *ls, const char *str, size_t l);
#if LJ_HASFFI
LJ_FUNC void lj_parse_keepcdata(LexState *ls, TValue *tv, GCcdata *cd);
#endif

/* Binary and unary operators. ORDER OPR */
typedef enum BinOpr {
  OPR_ADD, OPR_SUB, OPR_MUL, OPR_DIV, OPR_MOD, OPR_POW,  /* ORDER ARITH */
  OPR_UNM, OPR_BNOT, /* To maintain ORDER ARITH continuity. */
  OPR_IDIV, OPR_BAND, OPR_BOR, OPR_BXOR, OPR_SHL, OPR_SHR,
  OPR_CONCAT,
  OPR_NE, OPR_EQ, OPR_LT, OPR_GE,
  OPR_LE, OPR_GT,
  OPR_AND, OPR_OR,
  OPR_NOBINOPR
} BinOpr;


#endif
