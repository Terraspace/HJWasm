/****************************************************************************
*
*  This code is Public Domain.
*
*  ========================================================================
*
* Description:  implements hll directives:
*               .IF, .WHILE, .REPEAT, .ELSE, .ELSEIF, .ENDIF, .FOR, .SWITCH, .CASE, .DEFAULT,
*               .ENDW, .UNTIL, .UNTILCXZ, .BREAK, .CONTINUE,  .ENFOR, .ENDSWITCH.
*               also handles C operators:
*               ==, !=, >=, <=, >, <, &&, ||, &, !,
*               and flags:
*               ZERO?, CARRY?, SIGN?, PARITY?, OVERFLOW?, 
*               LESS?, GREATER?, ABOVE?, EQUAL?, BELOW?   
*
****************************************************************************/

#include <ctype.h>
#include <string.h>
#include "globals.h"
#include "memalloc.h"
#include "parser.h"
#include "equate.h"
#include "label.h"
#include "expreval.h"
#include "types.h"
#include "hll.h"
#include "segment.h"
#include "listing.h"
#include "lqueue.h"
#include "myassert.h"
#include "trmem.h"
#include "fastpass.h"
#include "atofloat.h"

#if defined(WINDOWSDDK)
#if defined(_WIN32)
typedef _W64 int INT_PTR, *PINT_PTR;
typedef _W64 unsigned int UINT_PTR, *PUINT_PTR;
typedef _W64 long LONG_PTR, *PLONG_PTR;
typedef _W64 unsigned long ULONG_PTR, *PULONG_PTR;
#define __int3264   __int32
#else
typedef __int64 INT_PTR, *PINT_PTR;
typedef unsigned __int64 UINT_PTR, *PUINT_PTR;
typedef __int64 LONG_PTR, *PLONG_PTR;
typedef unsigned __int64 ULONG_PTR, *PULONG_PTR;
#define __int3264   __int64
#endif
#else
#include <inttypes.h>
#if defined(_WIN32)
typedef _W64 int INT_PTR, *PINT_PTR;
typedef _W64 unsigned int UINT_PTR, *PUINT_PTR;
typedef _W64 long LONG_PTR, *PLONG_PTR;
typedef _W64 unsigned long ULONG_PTR, *PULONG_PTR;
#define __int3264   __int32
#else
typedef int64_t INT_PTR, *PINT_PTR;
typedef uint64_t UINT_PTR, *PUINT_PTR;
typedef int64_t LONG_PTR, *PLONG_PTR;
typedef uint64_t ULONG_PTR, *PULONG_PTR;
#define __int3264   int64_t
typedef int64_t __int64;
#endif
#endif

int Tokenize(char *, unsigned int, struct asm_tok[], unsigned int);
void myatoi128(const char *src, uint_64 dst[], int base, int size);
#define LABELSIZE 8
#define LABELSGLOBAL 0 /* make the generated labels global */
#define JMPPREFIX      /* define spaces before "jmp" or "loop" */
#define LABELFMT "@C%04X"
/* v2.10: static variables moved to ModuleInfo */
#define HllStack ModuleInfo.g.HllStack
#define HllFree  ModuleInfo.g.HllFree

#if LABELSGLOBAL
#define LABELQUAL "::"
#else
#define LABELQUAL ":"
#endif

#ifdef DEBUG_OUT
#define EOLCHAR '^'  /* this allows better displays */
#define EOLSTR  "^"
#else
#define EOLCHAR '\n' /* line termination char in generated source */
#define EOLSTR  "\n"
#endif

/* values for struct hll_item.cmd */
enum hll_cmd {
  HLL_IF,
  HLL_WHILE,
  HLL_REPEAT,
  HLL_BREAK,  /* .IF behind .BREAK or .CONTINUE */
  HLL_FOR,
  HLL_SWITCH,
};

/* index values for struct hll_item.labels[] */
enum hll_label_index {
  LTEST,      /* test (loop) condition  */
  LEXIT,      /* block exit             */
  LSTART,     /* loop start             */
  LSKIP,      /* skip counters          */
  LCONT,      /* for .CONTINUE in .for  */
  LDEF,       /* jump table for SWITCH  */
  LDATA1,     /* jump table for SWITCH  */
  LDATA2,     /* jump table for SWITCH  */
  LTOP,       /* jump table for SWITCH  */
  LJUMP,      /* jump table for SWITCH  */
};

/* values for struct hll_item.flags */
enum hll_flags {
  HLLF_ELSEOCCURED = 0x01,
  HLLF_DEFAULTOCCURED = 0x02,
  HLLF_WHILE = 0x04,
};


/* item for .IF, .WHILE, .REPEAT, .FOR, .SWITCH ... */
struct hll_item {
  struct hll_item     *next;
  uint_32             labels[10];    /* labels for LTEST, LEXIT, LSTART, LSKIP, LCONT, LDEF LDATA1, LDATA2, LTOP, LJUMP */
  char                *condlines;    /* .WHILE-blocks only: lines to add after 'test' label */
  char                *counterlines; /* pointer to allocated memory for counters */
  uint_32             cmcnt;         /* comma counter  */
  enum hll_cmd        cmd;           /* start cmd (IF, WHILE, REPEAT) */
  bool                cond;          /* condition  */
  enum hll_flags      flags;         /* v2.08: added */
/* All of bellow vars are related to the SWITCH block */
  int_32              casecnt;       /* case counter  */
  char                csize;         /* SWITCH decision flag */
  char                cflag;         /* SWITCH decision flag */
  int                 maxcase;
  int                 mincase;
  int                 delta;
  int                 maxalloccasen;
  int                 multicase;
  int                 *pcases;
  uint_16             *plabels;
  uint_16             savedlab;
  bool                breakoccured;   /* condition  */
#if AMD64_SUPPORT
  int_64              maxcase64;
  int_64              mincase64;
  uint_64              delta64;
  uint_64              *pcases64;
#endif
};

/* v2.08: struct added */
struct hll_opnd {
  char    *lastjmp;
  uint_32 lasttruelabel; /* v2.08: new member */
};
static ret_code GetExpression(struct hll_item *hll, int *i, struct asm_tok[], int ilabel, bool is_true, char *buffer, struct hll_opnd *);

/* c binary ops.
* Order of items COP_EQ - COP_LE  and COP_ZERO - COP_OVERFLOW
* must not be changed.
*/
enum c_bop {
  COP_NONE,
  COP_EQ,       /* == */
  COP_NE,       /* != */
  COP_GT,       /* >  */
  COP_LT,       /* <  */
  COP_GE,       /* >= */
  COP_LE,       /* <= */
  COP_AND,      /* && */
  COP_OR,       /* || */
  COP_ANDB,     /* &  */
  COP_NEG,      /* !  */
  COP_ZERO,     /* ZERO?     not really a valid C operator */
  COP_CARRY,    /* CARRY?    not really a valid C operator */
  COP_SIGN,     /* SIGN?     not really a valid C operator */
  COP_PARITY,   /* PARITY?   not really a valid C operator */
  COP_OVERFLOW, /* OVERFLOW? not really a valid C operator */
                //added by habran
  COP_LESS,     /* Used for signed integers SIGN=OVERFLOW */
  COP_GREATER,  /* Used for signed integers SIGNED ZERO OR CARRY */
  COP_ABOVE,    /* Used for unsigned integers ZERO OR CARRY */
  COP_EQUAL,
  COP_BELOW     /* Used for unsigned integers */
};


/* items in table below must match order COP_ZERO - COP_OVERFLOW */
static const char flaginstr[] = { 'z', 'c', 's', 'p', 'o', 'l', 'g', 'a', 'e', 'b' };

/* items in tables below must match order COP_EQ - COP_LE */
static const char unsigned_cjmptype[] = { 'z', 'z', 'a', 'b', 'b', 'a' };
static const char signed_cjmptype[] = { 'z', 'z', 'g', 'l', 'l', 'g' };
static const char neg_cjmptype[] = { 0, 1, 0, 0, 1, 1 };

/* in Masm, there's a nesting level limit of 20. In Uasm, there's
* currently no limit.
*/
#ifdef __WATCOMC__
static _inline char HexDigit( char x )
#elif defined(_MSC_VER)
static _inline char HexDigit( char x )
#else
static char HexDigit( char x )
#endif
{
    x &= 0xF;
    return((x > 9) ? (x - 10 + 'A') : (x + '0'));
}

#ifdef DEBUG_OUT
static unsigned evallvl;
static unsigned cntAlloc;  /* # of allocated hll_items */
static unsigned cntReused; /* # of reused hll_items */
static unsigned cntCond;   /* # of allocated 'condlines'-buffer in .WHILE-blocks */
static unsigned cntCondBytes; /* total size of allocated 'condlines'-buffers */
#endif

 uint_32 GetHllLabel(void)
/********************************/
{
  return (++ModuleInfo.hll_label);
}

/* get a C binary operator from the token stream.
* there is a problem with the '<' because it is a "string delimiter"
* which Tokenize() usually is to remove.
* There has been a hack implemented in Tokenize() so that it won't touch the
* '<' if .IF, .ELSEIF, .WHILE, .UNTIL, .UNTILCXZ or .BREAK/.CONTINUE has been
* detected.
*/

#define CHARS_EQ  '=' + ( '=' << 8 )
#define CHARS_NE  '!' + ( '=' << 8 )
#define CHARS_GE  '>' + ( '=' << 8 )
#define CHARS_LE  '<' + ( '=' << 8 )
#define CHARS_AND '&' + ( '&' << 8 )
#define CHARS_OR  '|' + ( '|' << 8 )

static enum c_bop GetCOp(struct asm_tok *item)
/**********************************************/
{
  int size;
  enum c_bop rc;
  char *p = item->string_ptr;

  size = (item->token == T_STRING ? item->stringlen : 0);

  if (size == 2) {
    switch (*(uint_16 *)p) {
    case CHARS_EQ:  rc = COP_EQ;  break;
    case CHARS_NE:  rc = COP_NE;  break;
    case CHARS_GE:  rc = COP_GE;  break;
    case CHARS_LE:  rc = COP_LE;  break;
    case CHARS_AND: rc = COP_AND; break;
    case CHARS_OR:  rc = COP_OR;  break;
    default: return(COP_NONE);
    }
  }
  else if (size == 1) {
    switch (*p) {
    case '>': rc = COP_GT;   break;
    case '<': rc = COP_LT;   break;
    case '&': rc = COP_ANDB; break;
    case '!': rc = COP_NEG;  break;
    default: return(COP_NONE);
    }
  }
  else {
    if (item->token != T_ID)
      return(COP_NONE);
    /* a valid "flag" string must end with a question mark */
    size = (int)strlen(p);
    if (*(p + size - 1) != '?')
      return(COP_NONE);
    if (size == 5 && (0 == _memicmp(p, "ZERO", 4)))
      rc = COP_ZERO;
    else if (size == 6 && (0 == _memicmp(p, "CARRY", 5)))
      rc = COP_CARRY;
    else if (size == 5 && (0 == _memicmp(p, "SIGN", 4)))
      rc = COP_SIGN;
    else if (size == 7 && (0 == _memicmp(p, "PARITY", 6)))
      rc = COP_PARITY;
    else if (size == 9 && (0 == _memicmp(p, "OVERFLOW", 8)))
      rc = COP_OVERFLOW;
    //added by habran
    else if (size == 5 && (0 == _memicmp(p, "LESS", 4)))
      rc = COP_LESS;
    else if (size == 8 && (0 == _memicmp(p, "GREATER", 7)))
      rc = COP_GREATER;
    else if (size == 6 && (0 == _memicmp(p, "ABOVE", 5)))
      rc = COP_ABOVE;
    else if (size == 6 && (0 == _memicmp(p, "EQUAL", 5)))
      rc = COP_EQUAL;
    else if (size == 6 && (0 == _memicmp(p, "BELOW", 5)))
      rc = COP_BELOW;
    else
      return(COP_NONE);
  }
  return(rc);
}
/* handle 32 bit hex here for compatibility with x86 */
uint_32  hex2dec(const char *src)
/*******************************************************/
{
  uint_32 a;
  uint_32 b = 0;
  for (;;)
  {
    a = *src;
    if (!a) break;
    b = (b << 4);
    if (a >= '0' && a <= '9') a -= '0';
    else {
      a |= 0x20;
      if (a >= 'a' && a <= 'f') a -= 'a' - 10;
    }
    b = b + a;
    src++;
  }
  return (b);
}

static void bubblesort(struct hll_item *hll, uint_16 *lbl, int *src, int n) {
  /*******************************************************************************************************************************/
  int i;
  int j;
  int temp1;
  uint_16 temp2;

  for (i = 0; i < n; ++i)
  {
    for (j = i + 1; j < n; ++j)
    {
      if (src[i] > src[j])
      {
        temp1 = src[i];
        src[i] = src[j];
        src[j] = temp1;
        temp2 = lbl[i];
        lbl[i] = lbl[j];
        lbl[j] = temp2;
      }
    }
  }

  hll->mincase = src[0];
  hll->maxcase = src[n - 1];
  hll->delta = hll->maxcase - hll->mincase;
}
#if AMD64_SUPPORT 
static void bubblesort64(struct hll_item *hll, uint_16 *lbl, int_64 *src, int n) {
  /*******************************************************************************************************************************/
  int i;
  int j;
  int_64 temp1;
  uint_16 temp2;
  for (i = 0; i < n; ++i)
  {
    for (j = i + 1; j < n; ++j)
    {
      if (src[i] > src[j])
      {
        temp1 = src[i];
        src[i] = src[j];
        src[j] = temp1;
        temp2 = lbl[i];
        lbl[i] = lbl[j];
        lbl[j] = temp2;
      }
    }
  }

  hll->mincase64 = src[0];
  hll->maxcase64 = src[n - 1];
  hll->delta64 = hll->maxcase64 - hll->mincase64;
}
#endif
/* render an instruction */
static char *RenderInstr(char *dst, const char *instr, int start1, int end1, int start2, int end2, struct asm_tok tokenarray[])
/*******************************************************************************************************************************/
{
#if AMD64_SUPPORT
  __int64 i;
#else
  int i;
#endif
#ifdef DEBUG_OUT
  char *old = dst;
#endif
  i = (int)strlen(instr);
  /* copy the instruction */
  memcpy(dst, instr, i);
  dst += i;
  /* copy the first operand's tokens */
  *dst++ = ' ';
  i = tokenarray[end1].tokpos - tokenarray[start1].tokpos;
  memcpy(dst, tokenarray[start1].tokpos, i);
  dst += i;
  if (start2 != EMPTY) {
    *dst++ = ',';
    /* copy the second operand's tokens */
    *dst++ = ' ';
    i = tokenarray[end2].tokpos - tokenarray[start2].tokpos;
    memcpy(dst, tokenarray[start2].tokpos, i);
    dst += i;
  }
  else if (end2 != EMPTY) {
    dst += sprintf(dst, ", %d", end2);
  }
  *dst++ = EOLCHAR;
  *dst = NULLC;
  DebugMsg1(("%u RenderInstr(%s)=>%s<\n", evallvl, instr, old));
  return(dst);
}

/* render a Simd instruction */
static char *RenderSimdInstr(char *dst, const char *instr, int start1, int end1, int start2, int end2, struct asm_tok tokenarray[], enum c_bop op)
/*******************************************************************************************************************************/
{
#if AMD64_SUPPORT
  __int64 i;
#else
    int i;
#endif
#ifdef DEBUG_OUT
	char *old = dst;
#endif
	i = strlen(instr);
	/* copy the instruction */
	memcpy(dst, instr, i);
	dst += i;
	/* copy the first operand's tokens */
	*dst++ = ' ';
	i = tokenarray[end1].tokpos - tokenarray[start1].tokpos;
	memcpy(dst, tokenarray[start1].tokpos, i);
	dst += i;
	if (start2 != EMPTY) {
		*dst++ = ',';
		/* copy the second operand's tokens */
		*dst++ = ' ';
		i = tokenarray[end2].tokpos - tokenarray[start2].tokpos;
		memcpy(dst, tokenarray[start2].tokpos, i);
		dst += i;
	}
	else if (end2 != EMPTY) {
		dst += sprintf(dst, ", %d", end2);
	}
	
	*dst++ = EOLCHAR;
	*dst = NULLC;
	DebugMsg1(("%u RenderInstr(%s)=>%s<\n", evallvl, instr, old));
	return(dst);
}

/* render a Simd instruction using a temporary float immediate macro FP4/FP8 */
static char *RenderSimdInstrTM(char *dst, const char *instr, int start1, int end1, int start2, int end2, struct asm_tok tokenarray[], enum c_bop op, bool isDouble)
/*******************************************************************************************************************************/
{
#if AMD64_SUPPORT
  __int64 i;
#else
  int i;
#endif
#ifdef DEBUG_OUT
	char *old = dst;
#endif
	i = strlen(instr);
	/* copy the instruction */
	memcpy(dst, instr, i);
	dst += i;
	/* copy the first operand's tokens */
	*dst++ = ' ';
	i = tokenarray[end1].tokpos - tokenarray[start1].tokpos;
	memcpy(dst, tokenarray[start1].tokpos, i);
	dst += i;
	if (start2 != EMPTY) {
		*dst++ = ',';
		/* copy the second operand's tokens */
		*dst++ = ' ';
		*dst++ = 'F';
		*dst++ = 'P';
		if(isDouble)
			*dst++ = '8';
		else
			*dst++ = '4';
		*dst++ = '(';
		i = tokenarray[end2].tokpos - tokenarray[start2].tokpos;
		memcpy(dst, tokenarray[start2].tokpos, i);
		dst += i;
		*dst++ = ')';
	}
	else if (end2 != EMPTY) {
		dst += sprintf(dst, ", %d", end2);
	}

	*dst++ = EOLCHAR;
	*dst = NULLC;
	DebugMsg1(("%u RenderInstr(%s)=>%s<\n", evallvl, instr, old));
	return(dst);
}

static char *GetLabelStr(int_32 label, char *buff)
/**************************************************/
{
  sprintf(buff, LABELFMT, label);
  return(buff);
}

/* render a Jcc instruction */

static char *RenderJcc(char *dst, char cc, int neg, uint_32 label)
/******************************************************************/
{
#ifdef DEBUG_OUT
  char *old = dst;
#endif
  /* create the jump opcode: j[n]cc */
  *dst++ = 'j';
  if (neg)
    *dst++ = 'n';
  *dst++ = cc;
  if (neg == FALSE)
    *dst++ = ' '; /* make sure there's room for the inverse jmp */

  *dst++ = ' ';
  GetLabelStr(label, dst);
  dst += strlen(dst);
  *dst++ = EOLCHAR;
  *dst = NULLC;
  DebugMsg1(("%u RenderJcc()=>%s<\n", evallvl, old));
  return(dst);
}

/* a "token" in a C expression actually is an assembly expression */

static ret_code GetToken(struct hll_item *hll, int *i, struct asm_tok tokenarray[], struct expr *opnd)
/******************************************************************************************************/
{
  int end_tok;

  /* scan for the next C operator in the token array.
  * because the ASM evaluator may report an error if such a thing
  * is found ( CARRY?, ZERO? and alikes will be regarded as - not yet defined - labels )
  */
  for (end_tok = *i; end_tok < Token_Count; end_tok++) {
    if ((GetCOp(&tokenarray[end_tok])) != COP_NONE)
      break;
  }
  if (end_tok == *i) {
    opnd->kind = EXPR_EMPTY;
    return(NOT_ERROR);
  }
  if (ERROR == EvalOperand(i, tokenarray, end_tok, opnd, 0))
    return(ERROR);

  /* v2.11: emit error 'syntax error in control flow directive'.
  * May happen for expressions like ".if 1 + CARRY?"
  */
  if (*i > end_tok) {
    return(EmitError(SYNTAX_ERROR_IN_CONTROL_FLOW_DIRECTIVE));
  }

  return(NOT_ERROR);
}

static uint_32 GetLabel(struct hll_item *hll, int index)
/********************************************************/
{
  /**/myassert(hll->labels[index]);
  return(hll->labels[index]);
}

/* a "simple" expression is
* 1. two tokens, coupled with a <cmp> operator: == != >= <= > <
* 2. two tokens, coupled with a "&" operator
* 3. unary operator "!" + one token
* 4. one token (short form for "<token> != 0")
*/
static ret_code GetSimpleExpression(struct hll_item *hll, int *i, struct asm_tok tokenarray[], int ilabel, bool is_true, char *buffer, struct hll_opnd *hllop)
/**************************************************************************************************************************************************************/
{
  enum c_bop op;
  char instr;
  int op1_pos;
  int op1_end;
  int op2_pos;
  int op2_end;
  char *p;
  struct expr op1;
  struct expr op2;
  uint_32 label;

  DebugMsg1(("%u GetSimpleExpression(>%.32s< buf=>%s<) enter\n", evallvl, tokenarray[*i].tokpos, buffer));

  while (tokenarray[*i].string_ptr[0] == '!' && tokenarray[*i].string_ptr[1] == '\0') {
    (*i)++; //GetCOp( i );
    is_true = 1 - is_true;
  }

  /* the problem with '()' is that is might enclose just a standard Masm
  * expression or a "hll" expression. The first case is to be handled
  * entirely by the expression evaluator, while the latter case is to be
  * handled HERE!
  */
  if (tokenarray[*i].token == T_OP_BRACKET) {
    int brcnt;
    int j;
    for (brcnt = 1, j = *i + 1; tokenarray[j].token != T_FINAL; j++) {
      if (tokenarray[j].token == T_OP_BRACKET)
        brcnt++;
      else if (tokenarray[j].token == T_CL_BRACKET) {
        brcnt--;
        if (brcnt == 0) /* a standard Masm expression? */
          break;
      }
      else if ((GetCOp(&tokenarray[j])) != COP_NONE)
        break;
    }
    if (brcnt) {
      (*i)++;
      DebugMsg1(("%u GetSimpleExpression: calling GetExpression, i=%u\n", evallvl, *i));
      if (ERROR == GetExpression(hll, i, tokenarray, ilabel, is_true, buffer, hllop))
        return(ERROR);

      if (tokenarray[*i].token != T_CL_BRACKET) {
        //if (( tokenarray[*i].token == T_FINAL ) || ( tokenarray[*i].token == T_CL_BRACKET ))
        DebugMsg(("GetSimpleExpression: expected ')', found: %s\n", tokenarray[*i].string_ptr));
        return(EmitError(SYNTAX_ERROR_IN_CONTROL_FLOW_DIRECTIVE));
      }
      (*i)++;
      return(NOT_ERROR);
    }
  }

  /* get (first) operand */
  op1_pos = *i;
  if (ERROR == GetToken(hll, i, tokenarray, &op1))
    return (ERROR);
  op1_end = *i;

  op = GetCOp(&tokenarray[*i]); /* get operator */

                                /* lower precedence operator ( && or || ) detected? */
  if (op == COP_AND || op == COP_OR) {
    /* v2.11: next 2 lines removed - && and || operators need a valid first operand */
    //if ( op1.kind == EXPR_EMPTY )
    //    return( NOT_ERROR );
    op = COP_NONE;
  }
  else if (op != COP_NONE)
    (*i)++;

  label = GetLabel(hll, ilabel);

  DebugMsg1(("%u GetSimpleExpression: EvalOperand ok, kind=%X, i=%u [%s]\n", evallvl, op1.kind, *i, tokenarray[*i].tokpos));

  /* check for special operators with implicite operand:
  * COP_ZERO, COP_CARRY, COP_SIGN, COP_PARITY, COP_OVERFLOW
  */
  if (op >= COP_ZERO) {
    if (op1.kind != EXPR_EMPTY) {
      DebugMsg(("GetSimpleExpression: non-empty expression rejected: %s\n", tokenarray[op1_pos].tokpos));
      return(EmitError(SYNTAX_ERROR_IN_CONTROL_FLOW_DIRECTIVE));
    }
    p = buffer;
    hllop->lastjmp = p;
    RenderJcc(p, flaginstr[op - COP_ZERO], !is_true, label);
    return(NOT_ERROR);
  }

  switch (op1.kind) {
  case EXPR_EMPTY:
    DebugMsg(("GetSimpleExpression: empty expression rejected\n"));
    return(EmitError(SYNTAX_ERROR_IN_CONTROL_FLOW_DIRECTIVE)); /* v2.09: changed from NOT_ERROR to ERROR */
  case EXPR_FLOAT:
    DebugMsg(("GetSimpleExpression: float expression rejected: %s\n", tokenarray[op1_pos].tokpos));
    return(EmitError(REAL_OR_BCD_NUMBER_NOT_ALLOWED)); /* v2.10: added */
  }

  if (op == COP_NONE){
    switch (op1.kind) {
    case EXPR_REG:
      if (op1.indirect == FALSE) {
        p = RenderInstr(buffer, "test", op1_pos, op1_end, op1_pos, op1_end, tokenarray);
        hllop->lastjmp = p;
        RenderJcc(p, 'z', is_true, label);
        break;
      }
      /* no break */
    case EXPR_ADDR:
      p = RenderInstr(buffer, "cmp", op1_pos, op1_end, EMPTY, 0, tokenarray);
      hllop->lastjmp = p;
      RenderJcc(p, 'z', is_true, label);
      break;
    case EXPR_CONST:
#if 0
      /* v2.05: string constant is allowed! */
      if (op1.string != NULL) {
        return(EmitError(SYNTAX_ERROR_IN_CONTROL_FLOW_DIRECTIVE));
      }
#endif
      /* v2.11: error if constant doesn't fit in 32-bits */
      if (op1.hvalue != 0 && op1.hvalue != -1)
        return(EmitConstError(&op1));

      hllop->lastjmp = buffer;

      if ((is_true == TRUE && op1.value) ||
        (is_true == FALSE && op1.value == 0)) {
        sprintf(buffer, "jmp " LABELFMT EOLSTR, label);
      }
      else {
        //strcpy( buffer, " " EOLSTR ); /* v2.11: obsolete */
        *buffer = NULLC;
      }
      break;
#ifdef DEBUG_OUT
    default: /**/myassert(0); break;
#endif
    }
    return(NOT_ERROR);
  }

  /* get second operand for binary operator */
  op2_pos = *i;
  if (ERROR == GetToken(hll, i, tokenarray, &op2)) {
    return(ERROR);
  }
  DebugMsg1(("%u GetSimpleExpression: EvalOperand 2 ok, type=%X, i=%u [%s]\n", evallvl, op2.type, *i, tokenarray[*i].tokpos));
  if (op2.kind != EXPR_CONST && op2.kind != EXPR_ADDR && op2.kind != EXPR_REG && op2.kind != EXPR_FLOAT) {  /* UASM 2.35 Added EXPR_FLOAT */
    DebugMsg(("GetSimpleExpression: syntax error, op2.kind=%u\n", op2.kind));
    return(EmitError(SYNTAX_ERROR_IN_CONTROL_FLOW_DIRECTIVE));
  }
  op2_end = *i;

  /* now generate ASM code for expression */

  if (op == COP_ANDB) {
    p = RenderInstr(buffer, "test", op1_pos, op1_end, op2_pos, op2_end, tokenarray);
    hllop->lastjmp = p;
    RenderJcc(p, 'e', is_true, label);
  }
  else if (op <= COP_LE) { /* ==, !=, >, <, >= or <= operator */
                           /*
                           * optimisation: generate 'or EAX,EAX' instead of 'cmp EAX,0'.
                           * v2.11: use op2.value64 instead of op2.value
                           */
    if (Options.masm_compat_gencode &&
      (op == COP_EQ || op == COP_NE) &&
      op1.kind == EXPR_REG && op1.indirect == FALSE &&
      op2.kind == EXPR_CONST && op2.value64 == 0) {
      p = RenderInstr(buffer, "or", op1_pos, op1_end, op1_pos, op1_end, tokenarray);
      }
    /* Uasm 2.18 optimisation: generate 'test EAX,EAX' instead of 'cmp EAX,0'. */
    else if ((op == COP_EQ || op == COP_NE) &&
      op1.kind == EXPR_REG && op1.indirect == FALSE &&
      op2.kind == EXPR_CONST && op2.value64 == 0) {
      p = RenderInstr(buffer, "test", op1_pos, op1_end, op1_pos, op1_end, tokenarray);
    }

	/* ------------------------------------------------------------------------------------------------------------ */
	/* UASM 2.35 support for floating point expression with SIMD registers                                          */
	/* ------------------------------------------------------------------------------------------------------------ */
	/* reg OP reg */
	else if (op1.kind == EXPR_REG && op1.indirect == FALSE && (GetValueSp(op1.base_reg->tokval) & OP_XMM) && // SizeFromRegister(op1.base_reg->tokval) == 16 &&
		     op2.kind == EXPR_REG && op2.indirect == FALSE && (GetValueSp(op2.base_reg->tokval) & OP_XMM)) //SizeFromRegister(op2.base_reg->tokval) == 16)
	{
		if (op1.mem_type == MT_REAL8 || op2.mem_type == MT_REAL8)
		{
			if (ModuleInfo.arch == ARCH_SSE)
				p = RenderSimdInstr(buffer, "ucomisd", op1_pos, op1_end, op2_pos, op2_end, tokenarray, op);
			else
				p = RenderSimdInstr(buffer, "vucomisd", op1_pos, op1_end, op2_pos, op2_end, tokenarray, op);
		}
		else
		{
			if (ModuleInfo.arch == ARCH_SSE)
				p = RenderSimdInstr(buffer, "ucomiss", op1_pos, op1_end, op2_pos, op2_end, tokenarray, op);
			else
				p = RenderSimdInstr(buffer, "vucomiss", op1_pos, op1_end, op2_pos, op2_end, tokenarray, op);
		}
	}
	/* reg OP [reg]      reg OP [expr]           reg OP variable */
	else if (op1.kind == EXPR_REG && op1.indirect == FALSE && SizeFromRegister(op1.base_reg->tokval) == 16 &&
	   	     ((op2.kind == EXPR_REG && op2.indirect == TRUE) || op2.kind == EXPR_ADDR) )
	{
		if (op1.mem_type == MT_REAL8 || op2.mem_type == MT_REAL8)
		{
			if (ModuleInfo.arch == ARCH_SSE)
				p = RenderSimdInstr(buffer, "ucomisd", op1_pos, op1_end, op2_pos, op2_end, tokenarray, op);
			else
				p = RenderSimdInstr(buffer, "vucomisd", op1_pos, op1_end, op2_pos, op2_end, tokenarray, op);
		}
		else
		{
			if (ModuleInfo.arch == ARCH_SSE)
				p = RenderSimdInstr(buffer, "ucomiss", op1_pos, op1_end, op2_pos, op2_end, tokenarray, op);
			else
				p = RenderSimdInstr(buffer, "vucomiss", op1_pos, op1_end, op2_pos, op2_end, tokenarray, op);
		}
	}
	/* reg OP float_imm */
	else if (op1.kind == EXPR_REG && op1.indirect == FALSE && SizeFromRegister(op1.base_reg->tokval) == 16 && (op2.kind == EXPR_CONST || op2.kind == EXPR_FLOAT) )
	{
		return(EmitError(SYNTAX_ERROR_IN_CONTROL_FLOW_DIRECTIVE));
		/*if (op1.mem_type == MT_REAL8 || op2.mem_type == MT_REAL8)
		{
			if (ModuleInfo.arch == ARCH_SSE)
				p = RenderSimdInstrTM(buffer, "ucomisd", op1_pos, op1_end, op2_pos, op2_end, tokenarray, op, TRUE);
			else
				p = RenderSimdInstrTM(buffer, "vucomisd", op1_pos, op1_end, op2_pos, op2_end, tokenarray, op, TRUE);
		}
		else
		{
			if (ModuleInfo.arch == ARCH_SSE)
				p = RenderSimdInstrTM(buffer, "ucomiss", op1_pos, op1_end, op2_pos, op2_end, tokenarray, op, FALSE);
			else
				p = RenderSimdInstrTM(buffer, "vucomiss", op1_pos, op1_end, op2_pos, op2_end, tokenarray, op, FALSE);
		}*/
	}

    else {  
      p = RenderInstr(buffer, "cmp", op1_pos, op1_end, op2_pos, op2_end, tokenarray);
    }
    instr = ((IS_SIGNED(op1.mem_type) || IS_SIGNED(op2.mem_type)) ? signed_cjmptype[op - COP_EQ] : unsigned_cjmptype[op - COP_EQ]);
    hllop->lastjmp = p;
    RenderJcc(p, instr, neg_cjmptype[op - COP_EQ] ? is_true : !is_true, label);
  }
  else {
    DebugMsg(("GetSimpleExpression: unexpected operator %s\n", tokenarray[op1_pos].tokpos));
    return(EmitError(SYNTAX_ERROR_IN_CONTROL_FLOW_DIRECTIVE));
  }
  return(NOT_ERROR);
}

/* invert a Jump:
* - Jx  -> JNx (x = e|z|c|s|p|o )
* - JNx -> Jx  (x = e|z|c|s|p|o )
* - Ja  -> Jbe, Jae -> Jb
* - Jb  -> Jae, Jbe -> Ja
* - Jg  -> Jle, Jge -> Jl
* - Jl  -> Jge, Jle -> Jg
* added in v2.11:
* - jmp -> 0
* - 0   -> jmp
*/
static void InvertJump(char *p)
/*******************************/
{
  if (*p == NULLC) { /* v2.11: convert 0 to "jmp" */
    strcpy(p, "jmp ");
    return;
  }

  p++;
  if (*p == 'e' || *p == 'z' || *p == 'c' || *p == 's' || *p == 'p' || *p == 'o') {
    *(p + 1) = *p;
    *p = 'n';
    return;
  }
  else if (*p == 'n') {
    *p = *(p + 1);
    *(p + 1) = ' ';
    return;
  }
  else if (*p == 'a') {
    *p++ = 'b';
  }
  else if (*p == 'b') {
    *p++ = 'a';
  }
  else if (*p == 'g') {
    *p++ = 'l';
  }
  else if (*p == 'l') {
    *p++ = 'g';
  }
  else {
    /* v2.11: convert "jmp" to 0 */
    if (*p == 'm') {
      p--;
      *p = NULLC;
    }
    return;
  }
  if (*p == 'e')
    *p = ' ';
  else
    *p = 'e';
  return;
}

/* Replace a label in the source lines generated so far.
* todo: if more than 0xFFFF labels are needed,
* it may happen that length of nlabel > length of olabel!
* then the simple memcpy() below won't work!
*/

static void ReplaceLabel(char *p, uint_32 olabel, uint_32 nlabel)
/*****************************************************************/
{
  char oldlbl[16];
  char newlbl[16];
  int i;

  GetLabelStr(olabel, oldlbl);
  GetLabelStr(nlabel, newlbl);

  i = (int)strlen(newlbl);

  DebugMsg1(("%u ReplaceLabel(%s->%s, >%s<)\n", evallvl, oldlbl, newlbl, p));
  while (p = strstr(p, oldlbl)) {
    memcpy(p, newlbl, i);
    p += i;
  }
}

/* operator &&, which has the second lowest precedence, is handled here */

static ret_code GetAndExpression(struct hll_item *hll, int *i, struct asm_tok tokenarray[], int ilabel, bool is_true, char *buffer, struct hll_opnd *hllop)
/***********************************************************************************************************************************************************/
{
  char *ptr = buffer;
  uint_32 truelabel = 0;
  char buff[16];
  int nlabel;
  int olabel;

  DebugMsg1(("%u GetAndExpression(>%.32s< buf=>%s<) enter\n", evallvl, tokenarray[*i].tokpos, buffer));

  if (ERROR == GetSimpleExpression(hll, i, tokenarray, ilabel, is_true, ptr, hllop))
    return(ERROR);
  while (COP_AND == GetCOp(&tokenarray[*i])) {

    (*i)++;
    DebugMsg1(("%u GetAndExpression: &&-operator found, is_true=%u, lastjmp=%s\n", evallvl, is_true, hllop->lastjmp ? hllop->lastjmp : "NULL"));

    if (is_true) {
      /* todo: please describe what's done here and why! */
      if (hllop->lastjmp) {
        char *p = hllop->lastjmp;
        InvertJump(p);          /* step 1 */
        if (truelabel == 0)     /* step 2 */
          truelabel = GetHllLabel();

        if (*p ) {/* v2.11: there might be a 0 at lastjmp */
          p += 4;               /* skip 'jcc ' or 'jmp ' */
          GetLabelStr(truelabel, p);
          strcat(p, EOLSTR);
        }
        /* Uasm 2.18  fixed WHILE with &&  */
        if (hllop->lasttruelabel)
          ReplaceLabel(buffer,hllop->lasttruelabel, truelabel);
          nlabel = GetHllLabel();
          olabel = GetLabel(hll, ilabel);
          GetLabelStr(olabel, buff);
          strcat(ptr, buff);
          strcat(ptr, LABELQUAL EOLSTR);
          DebugMsg1(("%u GetAndExpression: label added >%s<\n", evallvl, ptr));
          ReplaceLabel(buffer, olabel, nlabel);
          hllop->lastjmp = NULL;
      }
    }
    ptr += strlen(ptr);
    hllop->lasttruelabel = 0; /* v2.08 */
    if (ERROR == GetSimpleExpression(hll, i, tokenarray, ilabel, is_true, ptr, hllop))
      return(ERROR);
  };

  if (truelabel > 0) {
    ptr += strlen(ptr);
    GetLabelStr(truelabel, ptr);
    strcat(ptr, LABELQUAL EOLSTR);
    DebugMsg1(("%u GetAndExpression: label added >%s<\n", evallvl, ptr));
    hllop->lastjmp = NULL;
  }
  return(NOT_ERROR);
}

/* operator ||, which has the lowest precedence, is handled here */

static ret_code GetExpression(struct hll_item *hll, int *i, struct asm_tok tokenarray[], int ilabel, bool is_true, char *buffer, struct hll_opnd *hllop)
/********************************************************************************************************************************************************/
{
  char *ptr = buffer;
  uint_32 truelabel = 0;

  DebugMsg1(("%u GetExpression(>%.32s< buf=>%s<) enter\n", ++evallvl, tokenarray[*i].tokpos, buffer));

  /* v2.08: structure changed from for(;;) to while() to increase
  * readability and - optionally - handle the second operand differently
  * than the first.
  */

  if (ERROR == GetAndExpression(hll, i, tokenarray, ilabel, is_true, ptr, hllop)) {
    DebugMsg1(("%u GetExpression exit, error\n", evallvl--));
    return(ERROR);
  }
 // __debugbreak();
  while (COP_OR == GetCOp(&tokenarray[*i])) {

    uint_32 nlabel;
    uint_32 olabel;
    char buff[16];

    /* the generated code of last simple expression has to be modified
    1. the last jump must be inverted
    2. a "is_true" label must be created (it's used to jump "behind" the expr)
    3. create a new label
    4. the current "false" label must be generated

    if it is a .REPEAT, step 4 is slightly more difficult, since the "false"
    label is already "gone":
    4a. create a new label
    4b. replace the "false" label in the generated code by the new label
    */

    (*i)++;
    DebugMsg1(("%u GetExpression: ||-operator found, is_true=%u, lastjmp=%s\n", evallvl, is_true, hllop->lastjmp ? hllop->lastjmp : "NULL"));

    if (is_true == FALSE) {
      if (hllop->lastjmp) {
        char *p = hllop->lastjmp;
        InvertJump(p);           /* step 1 */
        if (truelabel == 0)      /* step 2 */
          truelabel = GetHllLabel();
        if (*p ) { /* v2.11: there might be a 0 at lastjmp */
          p += 4;                /* skip 'jcc ' or 'jmp ' */
          GetLabelStr(truelabel, p);
          strcat(p, EOLSTR);
        }
        /* v2.08: if-block added */
        if (hllop->lasttruelabel)
          ReplaceLabel(ptr, hllop->lasttruelabel, truelabel);
        DebugMsg1(("%u GetExpression: jmp inverted, dest changed >%s<\n", evallvl, ptr));
        hllop->lastjmp = NULL;

        nlabel = GetHllLabel();  /* step 3 */
        olabel = GetLabel(hll, ilabel);
        if (hll->cmd == HLL_REPEAT) {
          ReplaceLabel(buffer, olabel, nlabel);
          sprintf(ptr + strlen(ptr), "%s" LABELQUAL EOLSTR, GetLabelStr(nlabel, buff));
        }
        else {
          sprintf(ptr + strlen(ptr), "%s" LABELQUAL EOLSTR, GetLabelStr(olabel, buff));
          ReplaceLabel(buffer, olabel, nlabel);
        }
        DebugMsg1(("%u GetExpression: dest changed, label added >%s<\n", evallvl, ptr));
      }
    }
    ptr += strlen(ptr);
    hllop->lasttruelabel = 0; /* v2.08 */
    if (ERROR == GetAndExpression(hll, i, tokenarray, ilabel, is_true, ptr, hllop)) {
      DebugMsg1(("%u GetExpression exit, error\n", evallvl--));
      return(ERROR);
    }
  }
  if (truelabel > 0) {
    /* v2.08: this is needed, but ober-hackish. to be improved... */
    if (hllop->lastjmp && hllop->lasttruelabel) {
      DebugMsg1(("%u GetExpression: suppressed ReplaceLabel %u -> %u, lastjmp=%s\n", evallvl, hllop->lasttruelabel, truelabel, hllop->lastjmp));
      ReplaceLabel(ptr, hllop->lasttruelabel, truelabel);
      *(strchr(hllop->lastjmp, EOLCHAR) + 1) = NULLC;
    }
    ptr += strlen(ptr);
    GetLabelStr(truelabel, ptr);
    strcat(ptr, LABELQUAL EOLSTR);
    DebugMsg1(("%u GetExpression: label added >%s<\n", evallvl, ptr));
    hllop->lasttruelabel = truelabel; /* v2.08 */
  }
  DebugMsg1(("%u GetExpression exit\n", evallvl--));
  return(NOT_ERROR);
}

/*
* write assembly test lines to line queue.
* v2.11: local line buffer removed; src pointer has become a parameter.
*/

static ret_code QueueTestLines(char *src)
/*****************************************/
{
  char *start;

  DebugMsg1(("QueueTestLines(\"%s\") enter\n", src ? src : "NULL"));
  while (src) {
    //if (*src == ' ') src++; /* v2.11: obsolete */
    start = src;
    if (src = strchr(src, EOLCHAR))
      *src++ = NULLC;
    if (*start)
      AddLineQueue(start);
  }

  DebugMsg1(("QueueTestLines exit\n"));
  return(NOT_ERROR);
}

/*
* evaluate the C like boolean expression found in HLL structs
* like .IF, .ELSEIF, .WHILE, .UNTIL and .UNTILCXZ
* might return multiple lines (strings separated by EOLCHAR)
* - i = index for tokenarray[] where expression starts. Is restricted
*       to one source line (till T_FINAL)
* - label: label to jump to if expression is <is_true>!
* is_true:
*   .IF:       FALSE
*   .ELSEIF:   FALSE
*   .WHILE:    TRUE
*   .UNTIL:    FALSE
*   .UNTILCXZ: FALSE
*   .BREAK .IF:TRUE
*   .CONT .IF: TRUE
*/


static ret_code EvaluateHllExpression(struct hll_item *hll, int *i, struct asm_tok tokenarray[], int ilabel, bool is_true, char *buffer)
/****************************************************************************************************************************************/
{
  struct hll_opnd hllop = { NULL, 0 };

  DebugMsg1(("EvaluateHllExpression enter\n"));

  *buffer = NULLC;
  if (ERROR == GetExpression(hll, i, tokenarray, ilabel, is_true, buffer, &hllop))
    return(ERROR);
  /* v2.11: changed */
  //if ( *buffer == EOLCHAR ) {
  //DebugMsg(( "EvaluateHllExpression: EOL at pos 0 in line buffer\n" ));
  if (tokenarray[*i].token != T_FINAL) {
    DebugMsg(("EvaluateHllExpression: unexpected tokens >%s<\n", tokenarray[*i].tokpos));
    return(EmitError(SYNTAX_ERROR_IN_CONTROL_FLOW_DIRECTIVE));
  }
  return(NOT_ERROR);
}

/* for .UNTILCXZ: check if expression is simple enough.
* what's acceptable is ONE condition, and just operators == and !=
* Constants (0 or != 0) are also accepted.
*/

static ret_code CheckCXZLines(char *p)
/**************************************/
{
	int lines = 0;
	int i;
	int addchars;
	char const *px;
	bool NL = TRUE;

	DebugMsg1(("CheckCXZLines enter, p=>%s<\n", p));
	/* syntax ".untilcxz 1" has a problem: there's no "jmp" generated at all.
	* if this syntax is to be supported, activate the #if below.
	*/
	for (; *p; p++) {
		if (*p == EOLCHAR) {
			NL = TRUE;
			lines++;
		}
		else if (NL) {
			NL = FALSE;
			if (*p == 'j') {
				p++;
				/* v2.06: rewritten */
				if (*p == 'm' && lines == 0) {
					addchars = 2; /* make room for 2 chars, to replace "jmp" by "loope" */
					px = "loope";
				}
				else if (lines == 1 && (*p == 'z' || (*p == 'n' && *(p + 1) == 'z'))) {
					addchars = 3; /* make room for 3 chars, to replace "jz"/"jnz" by "loopz"/"loopnz" */
					px = "loop";
				}
				else
					return(ERROR); /* anything else is "too complex" */
								   //replace_instr:
				for (p--, i = strlen(p); i >= 0; i--) {
					*(p + addchars + i) = *(p + i);
				}
				memcpy(p, px, strlen(px));
			}
#if 0 /* handle ".untilcxz 1" like masm does */
			else if (*p == ' ' && *(p + 1) == EOLCHAR && lines == 0) {
				p++;
				GetLabelStr(hll->labels[LSTART], p);
				strcat(p, EOLSTR);
				addchars = 5;
				px = "loope";
				goto replace_instr;
			}
#endif
		}
	}
	if (lines > 2)
		return(ERROR);
	return(NOT_ERROR);
}

static const char *reax[] = { "ax", "eax", "rax" };
static const char *redx[] = { "dx", "edx", "rdx" };
static const char *recx[] = { "cx", "ecx", "rcx" };

static void WriteAsmLine(const char *cmd, const char *s1, const char *s2) {
  char codebuff[512];
  strcpy(codebuff, cmd);
  strcat(codebuff, s1);
  strcat(codebuff, ", ");
  strcat(codebuff, s2);
  AddLineQueue(codebuff);				// MOV RAX,param
  return;
}
static ret_code ForInitAndNext(struct asm_tok tokenarray[], int cnt, char *buff) {
  int a;
  int j;
  int	b = 0;
  bool brct = 0;
  char codebuff[512];
  char param[512];
  char init[512];
  char op[4];
  char *p;
  //create valid command and add to LineQueue
  do {
    //extracting the first parameter "param" before the operator
    a = 0;
    while ((buff[b] )&& (!strchr("=!<>&|-+*^%/", buff[b])))
    {
      if (buff[b] == '-' || buff[b] == '+') break;
      else param[a] = buff[b];
      b++;
      a++;
    }
    param[a] = '\0';
    //end of param
    //is it simple operator
    if ((buff[b + 1] == '+') || (buff[b + 1] == '-')) {            //if ++ or -- finish it here
      param[a] = '\0';
      if ((buff[b] == '+') && (buff[b + 1] == '+')) strcpy(codebuff, " inc ");
      else if ((buff[b] == '-') && (buff[b + 1] == '-')) strcpy(codebuff, " dec ");
      strcat(codebuff, param);
      AddLineQueue(codebuff);
      b += 3;
    }

    //we finis here "inc REX" or  "dec REX" or "inc mem" or "dec mem"
    else						//if not ++ or --
    {							//extracting the operator
      op[0] = buff[b];
      b++;
      if (op[0] == '=') {     //if operator is only '='
        op[1] = '\0';       //finish here
      }
      else {                 //extract next operator
        op[1] = buff[b];
        b++;
        if ((op[1] == '<' || op[1] == '>') && (buff[b] == '=')) {
          op[2] = buff[b]; //if a second operator is shift sign "<<=" or ">>="
          op[3] = NULLC;
          b++;
        }
        else op[2] = NULLC; //not shift so we have two operands

      }
      //we finished with param and op
      //now extracting the second parameter "init" after the operator
      for (a = 0; buff[b];) { //checking if we have a char in a single ' '
        if (buff[b] == '(' && buff[b - 1] != 39 && buff[b + 1] != 39 && b) break;
        if (buff[b] == ',' && buff[b - 1] != 39 && buff[b + 1] != 39 && b) break;
        init[a] = buff[b];
        b++;
        a++;
      }
      if (buff[b] == '(' && buff[b - 1] != 39 && buff[b + 1] != 39 && b) {
        for (; buff[b] && buff[b] != ')'; b++, a++) {
          init[a] = buff[b];
        }
        if (buff[b] == ')' && buff[b - 1] != 39 && buff[b + 1] != 39) {
          init[a] = buff[b];
          b++;
          a++;
        }
        brct = TRUE;
      }
      init[a] = NULLC;
      b++;
      //we finished with init
      //is operator devision, multiplication or mod
      if (op[0] == '/' || op[0] == '*' || op[0] == '%') {
        Tokenize(param, 0, tokenarray, 0);
        if (strcasecmp(param, reax[ModuleInfo.Ofssize]) && strcasecmp(init, reax[ModuleInfo.Ofssize])) { //param not rax init not rax?
          if (tokenarray[0].token == T_REG) {			//is param register
            if (strcasecmp(param, recx[ModuleInfo.Ofssize])) {				//make sure that param is not rcx
              WriteAsmLine(" mov  ", reax[ModuleInfo.Ofssize], param);				// MOV RAX,param
            }						
            if (op[0] == '/' || op[0] == '%') {
              strcpy(codebuff, " cdq ");			//extend to RDX if division
              AddLineQueue(codebuff);
            }
            Tokenize(init, 0, tokenarray, 0);
            if (tokenarray[0].token == T_NUM) {      //is init number?
              WriteAsmLine(" mov  ", recx[ModuleInfo.Ofssize], init);        //MOV RCX,init
              if (op[0] == '/' || op[0] == '%')
                strcpy(codebuff, " idiv ");		//IDIV RCX
              else strcpy(codebuff, " imul ");		//IMUL RCX
              strcat(codebuff, recx[ModuleInfo.Ofssize]);
              AddLineQueue(codebuff);
              goto cont;
            }
            else {
              if (op[0] == '/' || op[0] == '%') //it is a variable
                strcpy(codebuff, " idiv ");
              else strcpy(codebuff, " imul ");
              strcat(codebuff, init);
              AddLineQueue(codebuff);
              goto cont;
            }
          }
          //if param is a variable
          else if (tokenarray[0].token == T_ID) {		 //YES
            WriteAsmLine(" mov  ", recx[ModuleInfo.Ofssize], init);        //MOV RCX,init
            WriteAsmLine(" mov  ", reax[ModuleInfo.Ofssize], param);		// MOV RAX,param
            if (op[0] == '/' || op[0] == '%') {
              strcpy(codebuff, " cdq ");
              AddLineQueue(codebuff);
              strcpy(codebuff, " idiv ");
            }
            else strcpy(codebuff, " imul ");
            strcat(codebuff, recx[ModuleInfo.Ofssize]);
            AddLineQueue(codebuff);
          cont:
            if (op[0] == '%') {
              WriteAsmLine(" mov  ", param, redx[ModuleInfo.Ofssize]);
              goto skip;
            }
            else {
              WriteAsmLine(" mov  ", param, reax[ModuleInfo.Ofssize]);
              goto skip;
            }
          }
        }
        // if param is not RAX and init is RCX
        else if ((strcasecmp(param, reax[ModuleInfo.Ofssize]) && !(strcasecmp(init, recx[ModuleInfo.Ofssize])))) {
          if (op[0] == '/' || op[0] == '%') {
            strcpy(codebuff, " cdq ");
            AddLineQueue(codebuff);
            strcpy(codebuff, " idiv ");
          }
          else strcpy(codebuff, " imul ");
          strcat(codebuff, recx[ModuleInfo.Ofssize]);
          AddLineQueue(codebuff);
          if (op[0] == '%') WriteAsmLine(" mov  ", param, redx[ModuleInfo.Ofssize]);
          goto skip;
        }
        // if param is RAX and init is RCX
        else if ((!(strcasecmp(param, reax[ModuleInfo.Ofssize])) && (!(strcmp(init, recx[ModuleInfo.Ofssize]))))) {
          if (op[0] == '/' || op[0] == '%') {
            strcpy(codebuff, " cdq ");
            AddLineQueue(codebuff);
            strcpy(codebuff, " idiv ");
          }
          else strcpy(codebuff, " imul ");
          strcat(codebuff, recx[ModuleInfo.Ofssize]);
          AddLineQueue(codebuff);
          if (op[0] == '%') WriteAsmLine(" mov  ", param, redx[ModuleInfo.Ofssize]);
          goto skip;
        }
        // if param is RAX and init is not RCX
        else if (!(strcasecmp(param, reax[ModuleInfo.Ofssize]) && (strcasecmp(init, recx[ModuleInfo.Ofssize])))) {
          WriteAsmLine(" mov  ", recx[ModuleInfo.Ofssize], init);        //MOV RCX,init
          if (op[0] == '/' || op[0] == '%') {
            strcpy(codebuff, " cdq ");
            AddLineQueue(codebuff);
            strcpy(codebuff, " idiv ");
          }
          else strcpy(codebuff, " imul ");
          strcat(codebuff, recx[ModuleInfo.Ofssize]);
          AddLineQueue(codebuff);
          if (op[0] == '%') WriteAsmLine(" mov  ", param, redx[ModuleInfo.Ofssize]);
          goto skip;
        }
      }
      //if operator not devision, multiplication or mod
      else if (op[0] == '-') strcpy(codebuff, " sub ");
      else if (op[0] == '+') strcpy(codebuff, " add ");
      else if (op[0] == '&') strcpy(codebuff, " and ");
      else if (op[0] == '|') strcpy(codebuff, " or ");
      else if (op[0] == '^') strcpy(codebuff, " xor ");
      else if (op[0] == '<' && op[1] == '<') strcpy(codebuff, " shl ");
      else if (op[0] == '>' && op[1] == '>') strcpy(codebuff, " shr ");
      Tokenize(init, 0, tokenarray, 0);
      if (tokenarray[0].token == T_REG || tokenarray[0].token == T_NUM || tokenarray[0].token == T_FLOAT || tokenarray[0].token == T_DIRECTIVE)
        goto simple;
      Tokenize(param, 0, tokenarray, 0);
      if (tokenarray[0].token == T_REG) {
      simple:			Tokenize(param, 0, tokenarray, 0);

        if (strlen(init) == 1 && init[0] == '0')                        //REX=0?
          WriteAsmLine(" xor  ", param, param);							            //XOR REX,REX //instead MOV REX,0
        else if ((strlen(op) == 1) && ((init[4] == ' ') || (init[4] == '\t'))) {  //op is '='
          if ((0 == _memicmp(init, "BYTE PTR", 8)) ||                   //reax=BYTE PTR al
            (0 == _memicmp(init, "BPTR", 4)) ||
            (0 == _memicmp(init, "WPTR", 4)) ||
            (0 == _memicmp(init, "WORD PTR", 8)))                     //reax=WORD PTR ax

            WriteAsmLine(" movzx  ", param, init);                      //movzx reax,ax
          else if (0 == _memicmp(init, "ADDR", 4))
            WriteAsmLine(" lea  ", param, init + 5);
          else WriteAsmLine(" mov  ", param, init);
        }
        else if ((strlen(op) == 1) && ((init[6] == ' ') || (init[6] == '\t'))) {
          if (0 == _memicmp(init, "OFFSET", 6))
            WriteAsmLine(" lea  ", param, init + 7);
          else WriteAsmLine(" mov  ", param, init);
        }
        else if ((strlen(op) == 1) && ((init[5] == ' ') || (init[5] == '\t'))) {            //op is '='
          if (((0 == _memicmp(init, "DWORD PTR", 9) || (0 == _memicmp(init, "DPTR", 4)))
            && (param[0] == 'r' || param[0] == 'R'))) 							   //rax=DWORD PTR eax
            WriteAsmLine(" movsxd  ", param, init);								     //movsxd rax,eax
          else WriteAsmLine(" mov  ", param, init);
        }
        else if ((strlen(op) == 1) && brct) {											    //op is '='
          for (j = (int)strlen(init); j; j--) {
            if (init[j] == ')') init[j] = NULLC;
            if (init[j] == '(') {
              for (p = init, p += j; *p; p++) {
                if (*p == ',' && cnt) --cnt;
              }
              init[j] = ',';
            }
          }
          strcpy(codebuff, " invoke  ");
          strcat(codebuff, init);
          AddLineQueue(codebuff);
          WriteAsmLine(" mov  ", param, reax[ModuleInfo.Ofssize]);
          brct = FALSE;
        }
        else {
          if (strlen(op) == 1)
            WriteAsmLine(" mov  ", param, init);
          else
            WriteAsmLine(codebuff, param, init);
        }
      }
      else
      {
        if (init[0] == 39 && init[2] == 39)
          WriteAsmLine(" mov  byte ptr ", param, init);
        else {
          WriteAsmLine(" mov  ", reax[ModuleInfo.Ofssize], init);
          WriteAsmLine(codebuff, param, reax[ModuleInfo.Ofssize]);
        }
      }
    }
  skip:
    if (cnt) cnt--;
  } while (cnt);
  return(NOT_ERROR);
}

/* .IF, .WHILE, .REPEAT or .FOR directive */

ret_code HllStartDir(int i, struct asm_tok tokenarray[])
/********************************************************/
{
  struct hll_item      *hll;
  ret_code             rc = NOT_ERROR;
  int                  cmd = tokenarray[i].tokval;
  char buff[16];
  char buffer[MAX_LINE_LEN * 2];
  char forbuff[MAX_LINE_LEN];
  char forbuffinit[MAX_LINE_LEN];
  char forbuffcond[MAX_LINE_LEN];
  char forbuffcnt[MAX_LINE_LEN];
  char transformed[MAX_LINE_LEN];
  int j;
  int b;
  int size;
  int eqcnt;
  int cmcnt;
  char *p;
  char c;
  struct expr         opndx;
  struct asm_tok      *t;
  DebugMsg1(("HllStartDir(%s) enter\n", tokenarray[i].string_ptr));

  i++; /* skip directive */

       /* v2.06: is there an item on the free stack? */
  if (HllFree) {
    hll = HllFree;
    DebugCmd(cntReused++);
  }
  else {
    hll = LclAlloc(sizeof(struct hll_item));
    DebugCmd(cntAlloc++);
  }

  /* structure for .IF .ELSE .ENDIF
  *    cond jump to LTEST-label
  *    ...
  *    jmp LEXIT
  *  LTEST:
  *    ...
  *  LEXIT:

  * structure for .IF .ELSEIF
  *    cond jump to LTEST
  *    ...
  *    jmp LEXIT
  *  LTEST:
  *    cond jump to (new) LTEST
  *    ...
  *    jmp LEXIT
  *  LTEST:
  *    ...

  * structure for .WHILE and .REPEAT:
  *   jmp LTEST (for .WHILE only)
  * LSTART:
  *   ...
  * LTEST: (jumped to by .continue)
  *   a) test end condition, cond jump to LSTART label
  *   b) unconditional jump to LSTART label
  * LEXIT: (jumped to by .BREAK)
  */

  hll->labels[LEXIT] = 0;

  switch (cmd) {
  case T_DOT_IF:
    hll->labels[LSTART] = 0; /* not used by .IF */
    hll->labels[LTEST] = GetHllLabel();
    hll->cmd = HLL_IF;
    hll->flags = 0;
    /* get the C-style expression, convert to ASM code lines */
    rc = EvaluateHllExpression(hll, &i, tokenarray, LTEST, FALSE, buffer);
    if (rc == NOT_ERROR) {
      QueueTestLines(buffer);
      /* if no lines have been created, the LTEST label isn't needed */
      //if ( !is_linequeue_populated() ) {
      if (buffer[0] == NULLC) {
        hll->labels[LTEST] = 0;
      }
    }
    break;
    // added by habran
  case T_DOT_SWITCH:
    hll->cmd = HLL_SWITCH;
    hll->flags = HLLF_WHILE;
    hll->labels[LSKIP] = GetHllLabel();   /* used by .ENDSWITCH */
    hll->labels[LJUMP] = GetHllLabel();   /* used by .ENDSWITCH */
    hll->labels[LEXIT] = GetHllLabel();
    hll->labels[LDATA1] = GetHllLabel();   /* used by .ENDSWITCH */
    hll->labels[LSTART] = GetHllLabel();   /* used by .CASE      */
    hll->labels[LTEST] = 0;               /* used by .CASE      */
    hll->labels[LDEF] = 0;   /* used by .DEFAULT   */
    hll->labels[LDATA2] = GetHllLabel();   /* used by .ENDSWITCH */
    hll->labels[LTOP] = GetHllLabel();   /* used by .ENDSWITCH */
    hll->labels[LCONT] = GetHllLabel();   /* used by .ENDSWITCH */
    hll->maxalloccasen = 0;
    hll->casecnt = 0;
    hll->csize = 4;
    hll->breakoccured = TRUE; //first label for .CASE hast to be set

    if (tokenarray[i].token != T_FINAL) {
      DebugMsg1(("HllStartDir(%s): calling EvalOperand, i=%u\n", tokenarray[i].string_ptr, i));
      if (EvalOperand(&i, tokenarray, Token_Count, &opndx, 0) == ERROR) {
        DebugMsg(("HllStartDir(%s): EvalOperand() failed\n", tokenarray[i].string_ptr));
        return(ERROR);
      }
      i--; //EvalOperand increases i, decrease it to point to proper tokenarray[i].tokpos
      switch (opndx.kind) {
      case EXPR_REG:
        t = opndx.base_reg;
        if (ModuleInfo.Ofssize == USE32) {
          if (t->tokval <= T_DI)   // AL, CL, DL, BL, AH, CH, DH, BH, AX, CX, DX, BX
            AddLineQueueX(" movzx eax, %s", tokenarray[i].tokpos);
          else {
            if (t->tokval != T_EAX) //skip it, no need to write MOV EAX,EAX
              AddLineQueueX(" mov eax, %s", tokenarray[i].tokpos);
          }
        }
#if AMD64_SUPPORT
        else             //USE64
        {
        if (t->tokval != T_EAX){ //skip it, no need to write MOV EAX,EAX
            if (t->tokval <= T_DI) // AL, CL, DL, BL, AH, CH, DH, BH, AX, CX, DX, BX
              AddLineQueueX(" movzx eax, %s", tokenarray[i].tokpos);
            else if (t->tokval <= T_EDI) //EAX,ECX,EDX,EBX,ESP,EBP,ESI,EDI
              AddLineQueueX(" mov eax, %s", tokenarray[i].tokpos);
            else {
              if (t->tokval != T_RAX) //skip it, no need to write MOV RAX,RAX
                AddLineQueueX(" mov rax, %s", tokenarray[i].tokpos);
              hll->csize = 8;
            }
          }
        }               //end USE64
#endif
        break;
      case EXPR_ADDR:
        if (ModuleInfo.Ofssize == USE32) {
          if (opndx.mem_type == MT_BYTE || opndx.mem_type == MT_WORD)
            AddLineQueueX(" movzx eax, %s", tokenarray[i].tokpos);
          else
            AddLineQueueX(" mov eax, %s", tokenarray[i].tokpos);
        }
#if AMD64_SUPPORT
        else if (ModuleInfo.Ofssize == USE64)             //USE64
        {
          if (opndx.mem_type == MT_BYTE || opndx.mem_type == MT_WORD)
            AddLineQueueX(" movzx eax, %s", tokenarray[i].tokpos);
          else if (opndx.mem_type == MT_DWORD)
            AddLineQueueX(" mov eax, %s", tokenarray[i].tokpos);
          else {
            AddLineQueueX(" mov rax, %s", tokenarray[i].tokpos);
            hll->csize = 8;
          }
        }                //end USE64
#endif
        break;
      }
    }
    else {
      DebugMsg(("HllExitDir stack error\n"));
      return(EmitError(MISSING_OPERATOR_IN_EXPRESSION));
    }
    i++;
    AddLineQueueX(JMPPREFIX "jmp %s", GetLabelStr(hll->labels[LSTART], buff));
    break;
  case T_DOT_FOR:
    /* create the label to loop start */
    hll->labels[LSTART] = GetHllLabel();
    hll->labels[LSKIP] = GetHllLabel();
    hll->labels[LCONT] = 0;
    hll->labels[LTEST] = 0;
    hll->flags = 0;
    hll->cmd = HLL_FOR;
    /* check for 2 ':',if not throw an error */
    p = tokenarray[i].tokpos;
    for (b = 0; *p; p++)
    {
        if (*p == ':') b++;
        if (*p == 39) break;
    }
    if (b < 2) {
        DebugMsg(("HllStartDir .for loop \n"));
        return(EmitErr(SYNTAX_ERROR_EX, "Missing ':' before ')'"));
    }
    else if (b > 2) {
        DebugMsg(("HllStartDir .for loop \n"));
        return(EmitErr(SYNTAX_ERROR_EX, "Only 2 ':' allowed before ')'"));
    }
    //copy string to the buffer and get read of spaces
    p = tokenarray[i].tokpos;
    for (b = 0; *p; p++)
    {
      if ((*p == ' ') || (*p == '\t')) {
        //if there is QWORD PTR, DWORD PTR, WORD PTR, BYTE PTR, OFFSET or ADDR leave a space between
        c = tolower(*(p + 1));
        if ((c >= 'a') && (c <= 'z')) {
          forbuff[b] = *p;
          b++;
        }
      }
      else {
        forbuff[b] = *p;
        b++;
      }
    }
    forbuff[b] = NULLC;
    if (0 == _memicmp(forbuff, "(::)", 4)) {
      hll->cmcnt = 0;
    }
    else {
      //count initializers
      eqcnt = 1;
      forbuffinit[0] = NULLC;
      for (j = 1, b = 0; forbuff[j];) {
        c = forbuff[j];
        if (c == ':') {
          if (forbuff[j - 1] != 39 && forbuff[j + 1] != 39) break;
        }
        if (c == ',' && forbuff[j - 1] != 39 && forbuff[j + 1] != 39) eqcnt++;
        forbuffinit[b] = c;
        if (c == 39 && forbuffinit[b - 2] == 39) {
          b++;
          forbuffinit[b] = ' ';
        }
        j++;
        b++;
      }
      forbuffinit[b] = NULLC;
      j++;
      //coppy the condition to the buffer
      forbuffcond[0] = NULLC;
      hll->labels[LTEST] = 0;
      for (b = 0; forbuff[j] && forbuff[j] != ':'; forbuffcond[b] = forbuff[j], j++, b++);
      if (forbuffcond[b - 2] == '>' && forbuffcond[b - 1] == '0') forbuffcond[b - 2] = NULLC;
      forbuffcond[b] = NULLC;
      if (!b) hll->cond = FALSE;
      else    hll->cond = TRUE;
      j++;
      //copy the counter to the buffer
      cmcnt = 0;
      forbuffcnt[0] = NULLC;
      hll->condlines = "";
      for (b = 0; forbuff[j] != ')'; b++, j++) {
        forbuffcnt[b] = forbuff[j];
        if (forbuffcnt[b] == ',' && forbuff[j - 1] != 39 && forbuff[j + 1] != 39) ++cmcnt;
        if (forbuffcnt[b] == 39 && forbuffcnt[b - 2] == 39) {
          b++;
          forbuffcnt[b] = ' ';
        }
      }
      if (forbuffcnt[b - 1] == ')') b--;
      forbuffcnt[b] = NULLC;
      //create valid command and add to LineQueue to initiate .for loop vars
      //anything that is before the first ':'
      if (forbuffinit[0]) ForInitAndNext(tokenarray, eqcnt, forbuffinit);
      //note hll->counterlines is new var in the struct hll_item for store of forbuffcnt used in .ENDFOR
      //hll->cmcnt is also new var in the struct hll_item for commas counter used in .ENDFOR
      //copy forbuffcnt context to hll->counterlines
      //forbuffcnt contains anything that is written after the second ':'
      hll->cmcnt = 0;
      if (forbuffcnt[0])
      {
        //skip altering conditions the first time
        AddLineQueueX(" jmp %s", GetLabelStr(hll->labels[LSKIP], buff));
        size = (int)strlen(forbuffcnt) + 1;
        hll->counterlines = LclAlloc(size);
        memcpy(hll->counterlines, forbuffcnt, size);
        hll->cmcnt = cmcnt + 1;
      }
      else hll->counterlines = "";    //there is nothing after the second ':'
      if (forbuffcond[0]) {
        strcpy(transformed, ".for ");
        strcat(transformed, forbuffcond);
        strcat(transformed, "\0");
        tokenarray[0].string_ptr = ".for\0";
        tokenarray[0].tokpos = transformed;
        Token_Count = Tokenize(tokenarray[0].tokpos, 0, tokenarray, 0);
        if (tokenarray[i].token != T_FINAL) {
          rc = EvaluateHllExpression(hll, &i, tokenarray, LSTART, TRUE, buffer);
          if (rc == NOT_ERROR) {
            size = (int)strlen(buffer) + 1;
            hll->condlines = LclAlloc(size);
            memcpy(hll->condlines, buffer, size);
            DebugCmd(cntCond++); DebugCmd(cntCondBytes += size);
          }
        }
        else
          hll->condlines = "";
      }
    }
    AddLineQueueX("%s" LABELQUAL, GetLabelStr(hll->labels[LSTART], buff));
    tokenarray[i].token = T_FINAL;
    break;
    //end of .FOR
  case T_DOT_WHILE:
  case T_DOT_REPEAT:
    /* create the label to start of loop */
    hll->labels[LSTART] = GetHllLabel();
    hll->labels[LTEST] = 0; /* v2.11: test label is created only if needed */
                            //hll->labels[LEXIT] = GetHllLabel(); /* v2.11: LEXIT is only needed for .BREAK */
    if (cmd == T_DOT_WHILE) {
      hll->cmd = HLL_WHILE;
      hll->condlines = NULL;
      if (tokenarray[i].token != T_FINAL) {
        /* Here we need second test label if && is in second breckets*/
       // __debugbreak();
        rc = EvaluateHllExpression(hll, &i, tokenarray, LSTART, TRUE, buffer);
        if (rc == NOT_ERROR) {
          int size;
          size = (int)strlen(buffer) + 1;
          hll->condlines = LclAlloc(size);
          memcpy(hll->condlines, buffer, size);
          DebugCmd(cntCond++); DebugCmd(cntCondBytes += size);
        }
      }
      else
        buffer[0] = NULLC;  /* just ".while" without expression is accepted */

                            /* create a jump to test label */
                            /* optimisation: if line at 'test' label is just a jump, dont create label and don't jump! */
      if (_memicmp(buffer, "jmp", 3)) {
        hll->labels[LTEST] = GetHllLabel();
        AddLineQueueX(JMPPREFIX "jmp %s", GetLabelStr(hll->labels[LTEST], buff));
      }
    }
    else {
      hll->cmd = HLL_REPEAT;
    }
    AddLineQueueX("%s" LABELQUAL, GetLabelStr(hll->labels[LSTART], buff));
    break;
#ifdef DEBUG_OUT
  default: /**/myassert(0); break;
#endif
  }

  if (tokenarray[i].token != T_FINAL && rc == NOT_ERROR) {
    DebugMsg(("HllStartDir: unexpected token [%s]\n", tokenarray[i].tokpos));
    EmitErr(SYNTAX_ERROR_EX, tokenarray[i].tokpos);
    rc = ERROR;
    //return( ERROR ); /* v2.08: continue and parse the line queue */
  }
  /* v2.06: remove the item from the free stack */
  if (hll == HllFree)
    HllFree = hll->next;
  hll->next = HllStack;
  HllStack = hll;

  if (ModuleInfo.list)
    LstWrite(LSTTYPE_DIRECTIVE, GetCurrOffset(), NULL);

  if (is_linequeue_populated()) /* might be NULL! (".if 1") */
    RunLineQueue();

  return(rc);
}

/*
* .ENDIF, .ENDW, .ENDFOR .UNTIL and .UNTILCXZ .ENDSWITCH, .ENDSW, .ENDF directives.
* These directives end a .IF, .WHILE or .REPEAT .SWITCH block.
*/
ret_code HllEndDir(int i, struct asm_tok tokenarray[])
/******************************************************/
  {
  struct hll_item     *hll;
  ret_code            rc = NOT_ERROR;
  int                 cmd = tokenarray[i].tokval;
  int                 j, n;
#if AMD64_SUPPORT
  __int64             temp;
#else
  int                 temp;
#endif
  int                 acnt  = 0;
  int                 bcnt  = 0;
  int                 dcnt  = 0;
  int                 dsize = 0;
  uint_16             lbl;
  int             swsize;
  char buff[16];
  char buffer[MAX_LINE_LEN * 2];
  char unum[16];

  DebugMsg1(("HllEndDir(%s) enter\n", tokenarray[i].string_ptr));

  if (HllStack == NULL) {
    DebugMsg(("HllEndDir: hll stack is empty\n"));
    return(EmitError(DIRECTIVE_MUST_BE_IN_CONTROL_BLOCK));
    }

  hll = HllStack;
  HllStack = hll->next;
  /* v2.06: move the item to the free stack */
  hll->next = HllFree;
  HllFree = hll;
  if (ModuleInfo.switch_size)
      swsize = ModuleInfo.switch_size;
  else
    swsize = 0x4000;
  switch (cmd) {
    case T_DOT_ENDIF:
      if (hll->cmd != HLL_IF) {
        DebugMsg(("HllEndDir: no .IF on the hll stack\n"));
        return(EmitErr(BLOCK_NESTING_ERROR, tokenarray[i].string_ptr));
        }
      i++;
      /* if a test label isn't created yet, create it */
      if (hll->labels[LTEST]) {
        AddLineQueueX("%s" LABELQUAL, GetLabelStr(hll->labels[LTEST], buff));
        }
      break;
	case T_DOT_ENDSW:
      /* rewriten for v2.39 */
    case T_DOT_ENDSWITCH:
      if (hll->cmd != HLL_SWITCH) {
        DebugMsg(("HllExitDir stack error\n"));
        return(EmitError(DIRECTIVE_MUST_BE_IN_CONTROL_BLOCK));
        }
      i++;
      if (ModuleInfo.Ofssize == USE32) dsize = 4;
      else dsize = 8;
      if (ModuleInfo.switch_style == ASMSWITCH) {
        if (hll->labels[LEXIT] == 0)
          hll->labels[LEXIT] = GetHllLabel();
        AddLineQueueX(JMPPREFIX "jmp %s", GetLabelStr(hll->labels[LEXIT], buff));
        }
      if (hll->labels[LDEF] == 0) {
        hll->labels[LDEF] = GetHllLabel();
        AddLineQueueX("%s" LABELQUAL, GetLabelStr(hll->labels[LDEF], buff));
        }
      AddLineQueueX("ALIGN %d", dsize);
      AddLineQueueX("%s" LABELQUAL, GetLabelStr(hll->labels[LSTART], buff));
      if (hll->casecnt == 1)
        hll->cflag = 1;
      else if (hll->casecnt == 2)
        hll->cflag = 2;
      else if (hll->casecnt == 3)
        hll->cflag = 3;
      else
        {
        if (hll->plabels) {
          if (ModuleInfo.Ofssize == USE32 || hll->csize == 4) {
            bubblesort(hll, hll->plabels, hll->pcases, hll->casecnt);
            if ((hll->delta * 4) <= (hll->casecnt * 4 + hll->casecnt * 2))
              hll->cflag = 6;                   /* we need only jump table */
            else if (hll->delta < 256)
              hll->cflag = 4;                   /* we need both jump table and count table byte size */
            else if (hll->delta < swsize)       /* size limited to 0x4000  */
              hll->cflag = 7;                   /* we need both jump table and count table word size */
            else
              hll->cflag = 5;                   /* we will use a binary tree */
            }
#if AMD64_SUPPORT
          else if (ModuleInfo.Ofssize == USE64 && hll->csize == 8){     //USE64
              bubblesort64(hll, hll->plabels, hll->pcases64, hll->casecnt);
              if ((hll->delta64 * 8) <= (hll->casecnt * 8 + hll->casecnt * 2))
                hll->cflag = 6;
              else if (hll->delta64 < 256)
                hll->cflag = 4;
              else if (hll->delta64 < swsize)     /* size limited to 0x4000  */
                hll->cflag = 7;
              else
                hll->cflag = 5;
            }
#endif
          }
        }
      if (hll->cflag > 3) {
          lbl = 0;
          /* reuse this routine for these 3 kind of cases */
          if (hll->cflag == 4 || hll->cflag == 7){
            AddLineQueue(" .data");                /* create a jump table in data section, not to polute the source code */
            AddLineQueueX("ALIGN %d", dsize);
            memset(buffer, 0, sizeof(buffer));     /* clear the buffer */
            GetLabelStr(hll->labels[LDATA1], buff);/* this is the name ofthe jump table */
            strcpy(buffer, buff);                  /* store the jump table label at the beggining of array */
            if (ModuleInfo.Ofssize == USE32)
              strcat(buffer, " dd ");                /* now we have ready start point for the array: @C0006 dd */
            else
              strcat(buffer, " dq ");                /* now we have ready start point for the array: @C0006 dq */
            dcnt = 0;                              /* set the counter to zero to count how many elements in one line */
            for (j = 0; j < hll->casecnt; j++,dcnt++) {
              if (dcnt >= 75){                     /* if more than 20 labels in the line add a new row */
                AddLineQueue(buffer);              /* @C0006 dd @C000C, @C000D, @C000E, @C000F, @C0010...*/
                memset(buffer, 0, sizeof(buffer)); /* clear the buffer */
                if (ModuleInfo.Ofssize == USE32)
                  strcat(buffer, " dd ");                /* now we have ready start point for the array: @C0006 dd */
                else
                  strcat(buffer, " dq ");                /* now we have ready start point for the array: @C0006 dq */
                dcnt = 0;                          /* reset data caunter */
                }
              if (hll->plabels[j] != lbl) {        /* prevent 'label already defined error' */
                GetLabelStr(hll->plabels[j], buff);
                if (dcnt) strcat(buffer, ", ");    /* we need comma before next element, but not first one */
                strcat(buffer, buff);              /* add the address in the array */
                acnt++;
                }
              lbl = hll->plabels[j];               /* remember the last written address */
              }
              if (hll->flags & HLLF_DEFAULTOCCURED)
                GetLabelStr(hll->labels[LDEF], buff);
              else
                GetLabelStr(hll->labels[LEXIT], buff);
                if (dcnt && dcnt < 75){
                  strcat(buffer, ", ");
                  strcat(buffer, buff);
                }
              AddLineQueue(buffer);                  /* write line with the last array */
          } 
          /* here is created data for calculating jumps  */
          if (hll->cflag == 4 ) { 
            memset(buffer, 0, sizeof(buffer));     /* clear the buffer */
            GetLabelStr(hll->labels[LDATA2], buff);/* this is the name ofthe jump count table */
            strcpy(buffer, buff);                  /* @C0008  */
            strcat(buffer, " db ");                /* @C0008 db */
            n = 0;
            dcnt = 0; 
            for (j = 0; j < hll->casecnt; j++,dcnt++) {
              if (dcnt >= 75){                     /* if more than 20 labels in the line add a new row */
                if (buffer[4] == ',') buffer[4] = ' ';
                AddLineQueue(buffer);              /* @C0008 db 1, 2, 3, 4, 5...*/
                memset(buffer, 0, sizeof(buffer)); /* clear the buffer */
                strcpy(buffer, " db ");            /* now start with db 11, 12, 13, @C0014, @C0015...*/
                dcnt = 0;                          /* reset data caunter */
                }
              if (ModuleInfo.Ofssize == USE32 || hll->csize == 4)
                temp = (int)hll->pcases[j] - hll->mincase;/* temp contains case EG: .case 4, n contains  */
#if AMD64_SUPPORT
              else if (ModuleInfo.Ofssize == USE64 && hll->csize == 8)
                temp = (int)hll->pcases64[j] - hll->mincase64;/* temp contains case EG: .case 4, n contains  */
#endif
              while (n < temp) {                   /* fill the space between 2 cases with the .default */
              if (dcnt) 
                sprintf(unum, ",%d", acnt);           /* if it is not first number we need comma before it */
              else 
                sprintf(unum, "%d", acnt);            /* acnt contains exit or default number in this case '5' */
                strcat(buffer, unum);
                n++;
                dcnt++;
                if (dcnt >= 75){                     /* if more than 20 labels in the line add a new row */
                  if (buffer[4] == ',') buffer[4] = ' ';
                  AddLineQueue(buffer);              /* @C0008 db 1, 2, 3, 4, 5...*/
                  memset(buffer, 0, sizeof(buffer)); /* clear the buffer */
                  strcpy(buffer, " db ");            /* now start with db 11, 12, 13, @C0014, @C0015...*/
                  dcnt = 0;                          /* reset data caunter */
                  }
                }
              if (n) 
                sprintf(unum, ",%d", bcnt);           /* bcnt contains actual case number in this case 1,2,3,4 */
              else 
                sprintf(unum, "%d", bcnt);
              strcat(buffer, unum);
              n++;
              if (hll->plabels[j] != hll->plabels[j + 1])
                bcnt++;
              }
            if (buffer[4] == ',') buffer[4] = ' ';
            AddLineQueue(buffer);                  /* @C0008 db 0,1,2,5,5,3,5,5,5,5,5,5,5,5,4 */
            AddLineQueue(".code");                 /* continue with code section */
          }
          if (hll->cflag == 5) {
            AddLineQueue(" .data");                /* create a jump table in data section, not to polute the source code */
            AddLineQueueX("ALIGN %d", dsize);
            memset(buffer, 0, sizeof(buffer));     /* clear the buffer */
            GetLabelStr(hll->labels[LDATA1], buff);/* this is the name ofthe jump table */
            strcpy(buffer, buff);                  /* store the jump table label at the beggining of array */
            if (ModuleInfo.Ofssize == USE32)
              strcat(buffer, " dd ");                /* now we have ready start point for the array: @C0006 dd */
            else
              strcat(buffer, " dq ");                /* now we have ready start point for the array: @C0006 dq */
            dcnt = 0;                              /* set the counter to zero to count how many elements in one line */
            for (j = 0; j < hll->casecnt; ) {
              if (dcnt >= 15){                     /* if more than 20 labels in the line add a new row */
                AddLineQueue(buffer);              /* @C0006 dd @C000C, @C000D, @C000E, @C000F, @C0010...*/
                memset(buffer, 0, sizeof(buffer)); /* clear the buffer */
                if (ModuleInfo.Ofssize == USE32)
                  strcat(buffer, " dd ");                /* now we have ready start point for the array: @C0006 dd */
                else
                  strcat(buffer, " dq ");                /* now we have ready start point for the array: @C0006 dq */
                dcnt = 0;                          /* reset data caunter */
                }
              else {        
                GetLabelStr(hll->plabels[j], buff);
                if (dcnt) strcat(buffer, ", ");    /* we need comma before next element, but not first one */
                strcat(buffer, buff);              /* add the address in the array */
                dcnt++;
                j++;
                }
              }
             if (buffer[4] == ',')buffer[4] = ' ';
            // else if (buffer[10] == ',')buffer[10] = ' ';
             if (buffer[4] > 0) 
               AddLineQueue(buffer);                  /* write line with the last array */
             /* labels are done, now write cases */
            memset(buffer, 0, sizeof(buffer));
            GetLabelStr(hll->labels[LDATA2], buff);
            strcpy(buffer, buff);
            dcnt = 0; 
            if (ModuleInfo.Ofssize == USE32 || hll->csize == 4){
              strcat(buffer, " dd ");
              for (j = 0; j < hll->casecnt;){
                if (dcnt >= 30){                     /* if more than 35 cases in the line add a new row */
                  if (buffer[4] == ',') buffer[4] = ' ';
                  AddLineQueue(buffer);              /* @C0008 db 1, 2, 3, 4, 5...*/
                  memset(buffer, 0, sizeof(buffer)); /* clear the buffer */
                  strcpy(buffer, " dd ");            /* now start with db 11, 12, 13, @C0014, @C0015...*/
                  dcnt = 0;                          /* reset data caunter */
                  }
                  if (dcnt)
                    sprintf(unum, ",%d", hll->pcases[j]);
                  else
                    sprintf(unum, "%d", hll->pcases[j]);
                  strcat(buffer, unum);
                  dcnt++;
                  j++;
              }
              if (buffer[4] == ',') buffer[4] = ' ';
              if (buffer[4] > 0)  AddLineQueue(buffer);
              AddLineQueue(".code");
              }
#if AMD64_SUPPORT
            else if (ModuleInfo.Ofssize == USE64 && hll->csize == 8){
                strcat(buffer, " dq ");
              for (j = 0; j < hll->casecnt;){
                if (dcnt >= 30){                     /* if more than 35 cases in the line add a new row */
                  if (buffer[4] == ',') buffer[4] = ' ';
                  AddLineQueue(buffer);              /* @C0008 db 1, 2, 3, 4, 5...*/
                  memset(buffer, 0, sizeof(buffer)); /* clear the buffer */
                  strcpy(buffer, " dq ");            /* now start with db 11, 12, 13, @C0014, @C0015...*/
                  dcnt = 0;                          /* reset data caunter */
                  }
                  if (dcnt)
                    sprintf(unum, ",%d", hll->pcases64[j]);
                  else
                    sprintf(unum, "%d", hll->pcases64[j]);
                  strcat(buffer, unum);
                  dcnt++;
                  j++;
              }
              if (buffer[4] == ',') buffer[4] = ' ';
              if (buffer[4] > 0) AddLineQueue(buffer);
              AddLineQueue(".code");
              }
          }
#endif
          /* this is the same as hll->cflag == 4 but it alows a word counter
          *  so, it is possible to have more cases instead of 256, but we have
          *  limited it to 512, not to bloat the data */
          if (hll->cflag == 7 ) { 
            memset(buffer, 0, sizeof(buffer));     /* clear the buffer */
            GetLabelStr(hll->labels[LDATA2], buff);/* this is the name ofthe jump count table */
            strcpy(buffer, buff);                  /* @C0008  */
            strcat(buffer, " dw ");                /* @C0008 db */
            n = 0;
            dcnt = 0; 
            for (j = 0; j < hll->casecnt; j++,dcnt++) {
              if (dcnt >= 75){                     /* if more than 20 labels in the line add a new row */
                if (buffer[4] == ',') buffer[4] = ' ';
                AddLineQueue(buffer);              /* @C0008 db 1, 2, 3, 4, 5...*/
                memset(buffer, 0, sizeof(buffer)); /* clear the buffer */
                strcpy(buffer, " dw ");            /* now start with db 11, 12, 13, @C0014, @C0015...*/
                dcnt = 0;                          /* reset data caunter */
                }
              if (hll->csize == 4)
                temp = hll->pcases[j] - hll->mincase;/* temp contains case EG: .case 4, n contains  */
#if AMD64_SUPPORT
              else if (hll->csize == 8)
                temp = hll->pcases64[j] - hll->mincase64;/* temp contains case EG: .case 4, n contains  */
#endif
              while (n < temp) {                   /* fill the space between 2 cases with the .default */
              if (dcnt) 
                sprintf(unum, ",%d", acnt);           /* if it is not first number we need comma before it */
              else 
                sprintf(unum, "%d", acnt);            /* acnt contains exit or default number in this case '5' */
                strcat(buffer, unum);
                n++;
                dcnt++;
                if (dcnt >= 75){                     /* if more than 20 labels in the line add a new row */
                  if (buffer[4] == ',') buffer[4] = ' ';
                  AddLineQueue(buffer);              /* @C0008 db 1, 2, 3, 4, 5...*/
                  memset(buffer, 0, sizeof(buffer)); /* clear the buffer */
                  strcpy(buffer, " dw ");            /* now start with db 11, 12, 13, @C0014, @C0015...*/
                  dcnt = 0;                          /* reset data caunter */
                  }
                }
              if (n) 
                sprintf(unum, ",%d", bcnt);           /* bcnt contains actual case number in this case 1,2,3,4 */
              else 
                sprintf(unum, "%d", bcnt);
              strcat(buffer, unum);
              n++;
              if (hll->plabels[j] != hll->plabels[j + 1])
                bcnt++;
              }
            if (buffer[4] == ',') buffer[4] = ' ';
            strcat(buffer, unum);
            AddLineQueue(buffer);                  /* @C0008 db 0,1,2,5,5,3,5,5,5,5,5,5,5,5,4 */
            AddLineQueue(".code");                 /* continue with code section */
          }
          else if (hll->cflag == 6) {
            AddLineQueue(" .data");
            AddLineQueueX("ALIGN %d", dsize);
            memset(buffer, 0, sizeof(buffer));
            GetLabelStr(hll->labels[LDATA1], buff);
            strcpy(buffer, buff);
            dcnt = 0;
            if (ModuleInfo.Ofssize == USE32){
              strcat(buffer, " dd ");
              for (j = 0, n = 0; j < hll->casecnt; j++) {
                /* if more than 75 labels add a new row */
                if (dcnt >= 75){
                    if (buffer[4] == ',')buffer[4] = ' ';
                    else if (buffer[10] == ',')buffer[10] = ' ';
                  AddLineQueue(buffer);
                  memset(buffer, 0, sizeof(buffer));
                  strcpy(buffer, " dd ");
                  dcnt = 0;
                  }
                  if (hll->csize == 4){
                    temp = hll->pcases[j] - hll->mincase;/* temp contains case EG: .case 4, n contains  */
                    }
                  if (hll->flags & HLLF_DEFAULTOCCURED)
                    GetLabelStr(hll->labels[LDEF], buff);
                  else
                    GetLabelStr(hll->labels[LEXIT], buff);
                  while (n < temp) {
                    if (dcnt < 75){
                      strcat(buffer, ", ");
                      strcat(buffer, buff);
                      //dcnt++;     22/9
                      }
                    else{
                      strcat(buffer, ", ");
                      strcat(buffer, buff);
                      if (buffer[4] == ',')buffer[4] = ' ';
                      else if (buffer[10] == ',')buffer[10] = ' ';
                      AddLineQueue(buffer);
                      memset(buffer, 0, sizeof(buffer));
                      strcpy(buffer, " dd ");
                      dcnt = 0;
                      }
                  dcnt++;
                  n++;
                  }
                GetLabelStr(hll->plabels[j], buff);
                    if (dcnt < 75){
                      strcat(buffer, ", ");
                      strcat(buffer, buff);
                      }
                    else{
                      if (buffer[4] == ',')buffer[4] = ' ';
                      else if (buffer[10] == ',')buffer[10] = ' ';
                      AddLineQueue(buffer);
                      memset(buffer, 0, sizeof(buffer));
                      strcpy(buffer, " dd ");
                      dcnt = 0;
                      }
                n++;
                dcnt++;
                }
              if (hll->flags & HLLF_DEFAULTOCCURED)
                GetLabelStr(hll->labels[LDEF], buff);
              else
                GetLabelStr(hll->labels[LEXIT], buff);
                    if (dcnt < 75){
                      strcat(buffer, ", ");
                      strcat(buffer, buff);
                      }
                    else{
                      strcat(buffer, ", ");
                      strcat(buffer, buff);
                      if (buffer[4] == ',')buffer[4] = ' ';
                      else if (buffer[10] == ',')buffer[10] = ' ';
                      AddLineQueue(buffer);
                      memset(buffer, 0, sizeof(buffer));
                      strcpy(buffer, " dd ");
                      dcnt = 0;
                      }
                if (buffer[4] == ',')buffer[4] = ' ';
                else if (buffer[10] == ',')buffer[10] = ' ';
                if (buffer[4] > 0) AddLineQueue(buffer);  //22/9
                AddLineQueue(".code");
              }
#if AMD64_SUPPORT
              else if (ModuleInfo.Ofssize == USE64){
                strcat(buffer, " dq ");
                for (j = 0, n = 0; j < hll->casecnt; j++) {
                  /* if more than 20 labels add a new row */
                  if (dcnt >= 75){
                    if (buffer[4] == ',')buffer[4] = ' ';
                    else if (buffer[10] == ',')buffer[10] = ' ';
                    AddLineQueue(buffer);
                    memset(buffer, 0, sizeof(buffer));
                    strcpy(buffer, " dq ");
                    dcnt = 0;
                    }
                  if (hll->csize == 4){
                    temp = hll->pcases[j] - hll->mincase;/* temp contains case EG: .case 4, n contains  */
                    }
#if AMD64_SUPPORT
                  else if (hll->csize == 8){
                    temp = hll->pcases64[j] - hll->mincase64;/* temp contains case EG: .case 4, n contains  */
                    }
#endif
                  if (hll->flags & HLLF_DEFAULTOCCURED)
                    GetLabelStr(hll->labels[LDEF], buff);
                  else
                    GetLabelStr(hll->labels[LEXIT], buff);
                  while (n < temp) {
                    if (dcnt <= 75){
                      strcat(buffer, ", ");
                      strcat(buffer, buff);
                      dcnt++;
                      }
                    else{
                      strcat(buffer, ", ");
                      strcat(buffer, buff);
                      if (buffer[4] == ',')buffer[4] = ' ';
                      else if (buffer[10] == ',')buffer[10] = ' ';
                      AddLineQueue(buffer);
                      memset(buffer, 0, sizeof(buffer));
                      strcpy(buffer, " dq ");
                      dcnt = 0;
                      }
                    dcnt++;
                    n++;
                    }
                  GetLabelStr(hll->plabels[j], buff);
                    if (dcnt <= 75){
                      strcat(buffer, ", ");
                      strcat(buffer, buff);
                      }
                    else{
                      if (buffer[4] == ',')buffer[4] = ' ';
                      else if (buffer[10] == ',')buffer[10] = ' ';
                      AddLineQueue(buffer);
                      memset(buffer, 0, sizeof(buffer));
                      strcpy(buffer, " dq ");
                      dcnt = 0;
                      }
                  n++;
                  dcnt++;
                  }
                if (hll->flags & HLLF_DEFAULTOCCURED)
                  GetLabelStr(hll->labels[LDEF], buff);
                else
                  GetLabelStr(hll->labels[LEXIT], buff);
                    if (dcnt <= 75){
                      strcat(buffer, ", ");
                      strcat(buffer, buff);
                      }
                    else{
                      strcat(buffer, ", ");
                      strcat(buffer, buff);
                      if (buffer[4] == ',')buffer[4] = ' ';
                      else if (buffer[10] == ',')buffer[10] = ' ';
                      AddLineQueue(buffer);
                      memset(buffer, 0, sizeof(buffer));
                      strcpy(buffer, " dq ");
                      dcnt = 0;
                      }
                if (buffer[4] == ',')buffer[4] = ' ';
                else if (buffer[10] == ',')buffer[10] = ' ';
                AddLineQueue(buffer);
                AddLineQueue(".code");
             }
#endif
          }
      }
      /* these are code that are inserted to calculate and perform jumps */
      if (hll->cflag == 1) {
        if (hll->csize == 4) {
          AddLineQueueX("cmp  eax,%d", hll->pcases[0]);
          AddLineQueueX("je  %s", GetLabelStr(hll->plabels[0], buff));
          if (hll->flags & HLLF_DEFAULTOCCURED)
            AddLineQueueX("jmp  %s", GetLabelStr(hll->labels[LDEF], buff));
          else
            AddLineQueueX("jmp  %s", GetLabelStr(hll->labels[LEXIT], buff));
          }
#if AMD64_SUPPORT
        else if (hll->csize == 8) {
          AddLineQueueX("cmp rax, %q", hll->pcases64[0]);
          AddLineQueueX("je  %s", GetLabelStr(hll->plabels[0], buff));
          if (hll->flags & HLLF_DEFAULTOCCURED)
            AddLineQueueX("jmp  %s", GetLabelStr(hll->labels[LDEF], buff));
          else
            AddLineQueueX("jmp  %s", GetLabelStr(hll->labels[LEXIT], buff));
          }
#endif
        }
      else if (hll->cflag == 2) {
        if (hll->csize == 4) {
          AddLineQueueX("cmp  eax,%d", hll->pcases[0]);
          AddLineQueueX("je  %s", GetLabelStr(hll->plabels[0], buff));
          AddLineQueueX("cmp  eax,%d", hll->pcases[1]);
          AddLineQueueX("je  %s", GetLabelStr(hll->plabels[1], buff));
          if (hll->flags & HLLF_DEFAULTOCCURED)
            AddLineQueueX("jmp  %s", GetLabelStr(hll->labels[LDEF], buff));
          else
            AddLineQueueX("jmp  %s", GetLabelStr(hll->labels[LEXIT], buff));
          }
#if AMD64_SUPPORT
        else if (hll->csize == 8) {
          AddLineQueueX("cmp  rax,%q", hll->pcases64[0]);
          AddLineQueueX("je  %s", GetLabelStr(hll->plabels[0], buff));
          AddLineQueueX("cmp  rax,%q", hll->pcases64[1]);
          AddLineQueueX("je  %s", GetLabelStr(hll->plabels[1], buff));
          if (hll->flags & HLLF_DEFAULTOCCURED)
            AddLineQueueX("jmp  %s", GetLabelStr(hll->labels[LDEF], buff));
          else
            AddLineQueueX("jmp  %s", GetLabelStr(hll->labels[LEXIT], buff));
          }
#endif
        }
      else if (hll->cflag == 3) {
        if (hll->csize == 4) {
          AddLineQueueX("cmp  eax,%d", hll->pcases[0]);
          AddLineQueueX("je  %s", GetLabelStr(hll->plabels[0], buff));
          AddLineQueueX("cmp  eax,%d", hll->pcases[1]);
          AddLineQueueX("je  %s", GetLabelStr(hll->plabels[1], buff));
          AddLineQueueX("cmp  eax,%d", hll->pcases[2]);
          AddLineQueueX("je  %s", GetLabelStr(hll->plabels[2], buff));
          if (hll->flags & HLLF_DEFAULTOCCURED)
            AddLineQueueX("jmp  %s", GetLabelStr(hll->labels[LDEF], buff));
          else
            AddLineQueueX("jmp  %s", GetLabelStr(hll->labels[LEXIT], buff));
          }
#if AMD64_SUPPORT
        else if (hll->csize == 8) {
          AddLineQueueX("cmp  rax,%q", hll->pcases64[0]);
          AddLineQueueX("je  %s", GetLabelStr(hll->plabels[0], buff));
          AddLineQueueX("cmp  rax,%q", hll->pcases64[1]);
          AddLineQueueX("je  %s", GetLabelStr(hll->plabels[1], buff));
          AddLineQueueX("cmp  rax,%q", hll->pcases64[2]);
          AddLineQueueX("je  %s", GetLabelStr(hll->plabels[2], buff));
          if (hll->flags & HLLF_DEFAULTOCCURED)
            AddLineQueueX("jmp  %s", GetLabelStr(hll->labels[LDEF], buff));
          else
            AddLineQueueX("jmp  %s", GetLabelStr(hll->labels[LEXIT], buff));
          }
#endif
        }
      /* start hll->cflag 4 */
      else if (hll->cflag == 4) {                  
        if (hll->flags & HLLF_DEFAULTOCCURED)
          GetLabelStr(hll->labels[LDEF], buff);
        else {
          GetLabelStr(hll->labels[LEXIT], buff);
          }
        if (hll->csize == 4) {
          AddLineQueueX("cmp eax,%d", hll->mincase);
          AddLineQueueX("jl  %s", buff);
          AddLineQueueX("cmp eax,%d", hll->maxcase);
          AddLineQueueX("jg  %s", buff);
          }
#if AMD64_SUPPORT
        else if (hll->csize == 8) {
          AddLineQueueX("cmp rax, %q", hll->mincase64);
          AddLineQueueX("jl  %s", buff);
          AddLineQueueX("cmp rax, %q", hll->maxcase64);
          AddLineQueueX("jg  %s", buff);
          }
#endif
        if (ModuleInfo.Ofssize == USE32) {
          GetLabelStr(hll->labels[LDATA2], buff);
          AddLineQueueX("movzx eax,byte ptr[%s+eax-%u]",buff, hll->mincase);
          GetLabelStr(hll->labels[LDATA1], buff);
          AddLineQueueX("jmp dword ptr[%s+eax*4]",buff);
          }
#if AMD64_SUPPORT
        else if (ModuleInfo.Ofssize == USE64) {
          if (Parse_Pass){
              AddLineQueue("push r10");
              GetLabelStr(hll->labels[LDATA2], buff);
              AddLineQueueX("lea   r10,%s", buff);
          if (hll->csize == 4) {
              AddLineQueue("cdqe");                   /* neded for negative cases */
              AddLineQueueX("movzx r10,byte ptr[r10+rax-%u]", hll->mincase);
              }
            else {
              AddLineQueueX("movzx r10,byte ptr[r10+rax-%q]", hll->mincase64);
              }
            GetLabelStr(hll->labels[LDATA1], buff);
            AddLineQueueX("lea rax,%s", buff);
            AddLineQueue("mov   rax,qword ptr[rax+r10*8]");
            AddLineQueue("pop r10");
            AddLineQueue("jmp rax");
            }
        }
#endif
  }   /* end hll->cflag 4*/
      /* start hll->cflag 7 */
      else if (hll->cflag == 7) {
        if (hll->flags & HLLF_DEFAULTOCCURED)
          GetLabelStr(hll->labels[LDEF], buff);
        else {
          GetLabelStr(hll->labels[LEXIT], buff);
          }
        if (hll->csize == 4) {
          AddLineQueueX("cmp eax,%d", hll->mincase);
          AddLineQueueX("jl  %s", buff);
          AddLineQueueX("cmp eax,%d", hll->maxcase);
          AddLineQueueX("jg  %s", buff);
          }
#if AMD64_SUPPORT
        else if (hll->csize == 8) {
          AddLineQueueX("cmp rax, %q", hll->mincase64);
          AddLineQueueX("jl  %s", buff);
          AddLineQueueX("cmp rax, %q", hll->maxcase64);
          AddLineQueueX("jg  %s", buff);
          }
#endif
        if (ModuleInfo.Ofssize == USE32) {
          GetLabelStr(hll->labels[LDATA2], buff); 
          if (hll->mincase)
          AddLineQueueX("sub   eax,%u", hll->mincase);
          AddLineQueueX("movzx eax, word ptr[%s+eax*2]",buff);
          GetLabelStr(hll->labels[LDATA1], buff);    
          AddLineQueueX("jmp   dword ptr[%s+eax*4]",buff);
          }
#if AMD64_SUPPORT
        else if (ModuleInfo.Ofssize == USE64) {
          AddLineQueue("push r10");
          GetLabelStr(hll->labels[LDATA2], buff);
          AddLineQueueX("lea   r10,%s", buff);
          if (hll->csize == 4) {
            if (hll->mincase)
              AddLineQueueX("sub   eax,%d", hll->mincase);
            }
          else {
            if (hll->mincase64)
            AddLineQueueX("sub   rax,%q", hll->mincase64);
            }
            AddLineQueue("movzx r10,word ptr[r10+rax*2]");
            GetLabelStr(hll->labels[LDATA1], buff);
            AddLineQueueX("lea   rax,%s", buff);
            AddLineQueue("mov   rax,qword ptr[rax+r10*8]");
            AddLineQueue("pop r10");
            AddLineQueue("jmp rax");
          }
#endif
   }   /* end hll->cflag 7 */
      /* start hll->cflag 6 */
      else if (hll->cflag == 6) {
        if (hll->flags & HLLF_DEFAULTOCCURED)
          GetLabelStr(hll->labels[LDEF], buff);
        else {
          GetLabelStr(hll->labels[LEXIT], buff);
        }
        if (hll->csize == 4) {
          AddLineQueueX("cmp eax,%d", hll->mincase);
          AddLineQueueX("jl  %s", buff);
          AddLineQueueX("cmp eax,%d", hll->maxcase);
          AddLineQueueX("jg  %s", buff);
        }
#if AMD64_SUPPORT
        else if (hll->csize == 8) {
          AddLineQueueX("cmp rax, %q", hll->mincase64);
          AddLineQueueX("jl  %s", buff);
          AddLineQueueX("cmp rax, %q", hll->maxcase64);
          AddLineQueueX("jg  %s", buff);
          }
#endif
        if (ModuleInfo.Ofssize == USE32) {
          if (Parse_Pass ){
            if (hll->mincase)
            AddLineQueueX("sub eax,%d", hll->mincase);
            GetLabelStr(hll->labels[LDATA1], buff);
            AddLineQueueX("jmp   dword ptr[%s+eax*4]",buff);
            }
        }
#if AMD64_SUPPORT
        else if (ModuleInfo.Ofssize == USE64) {
          if (hll->csize == 4){
            if (hll->mincase)
            AddLineQueueX("sub   eax,%d", hll->mincase);
            }
          else{
            if (hll->mincase64)
            AddLineQueueX("sub rax,%q", hll->mincase64);
          }
          if (Parse_Pass ){
            AddLineQueue("push r10");
            AddLineQueueX("lea   r10,%s", GetLabelStr(hll->labels[LDATA1], buff));
            AddLineQueue("mov   rax,qword ptr[r10+rax*8]");
            AddLineQueue("pop r10");
            AddLineQueue("jmp rax");
            }
        }
#endif
    } /* end hll->cflag 6 */

      /* simple binary tree
      while (low <= high) {
      int mid = (low + high) / 2;
      if (hll->pcases[mid] == hll->casecnt)
      return mid;
      else if (hll->pcases[mid] < x)
      low = mid + 1;
      else high = mid - 1;
      }*/
      else if (hll->cflag == 5) {
        if (ModuleInfo.Ofssize == USE32) {
          AddLineQueueX("push	eax");
          AddLineQueueX("push	edx");
          AddLineQueueX("push	ecx");
          AddLineQueueX("push	ebx");
          AddLineQueueX("push	esi");
          AddLineQueueX("push	edi");
          AddLineQueueX("mov  esi,eax");
          AddLineQueueX("mov  ebx,%d", hll->casecnt - 1);//int high = len - 1;
          AddLineQueueX("xor  ecx,ecx");//int low = 0;
          AddLineQueueX("lea  edi,%s", GetLabelStr(hll->labels[LDATA2], buff));//int hll->pcases
          if (hll->flags & HLLF_DEFAULTOCCURED)
            AddLineQueueX("js  %s", GetLabelStr(hll->labels[LDEF], buff));
          else
            AddLineQueueX("js  %s", GetLabelStr(hll->labels[LEXIT], buff));
        }
#if AMD64_SUPPORT
        else if (ModuleInfo.Ofssize == USE64) {
          AddLineQueueX("push	rax");
          AddLineQueueX("push	rcx");
          AddLineQueueX("push	rbx");
          AddLineQueueX("push	rsi");
          AddLineQueueX("push	rdi");
          if (hll->csize == 4) {
            AddLineQueueX("mov  esi,eax");
            AddLineQueueX("mov  ebx,%d", hll->casecnt - 1);//int high = len - 1;
            AddLineQueueX("xor  ecx,ecx");//int low = 0;
            AddLineQueueX("lea  rdi,%s", GetLabelStr(hll->labels[LDATA2], buff));//int hll->pcases
          }                                 /* here we have to pay attention */
          else {
            AddLineQueueX("mov  rsi,rax");
            AddLineQueueX("mov  rbx,%q", hll->casecnt - 1);//int high = len - 1;
            AddLineQueueX("xor  ecx,ecx");//int low = 0;
            AddLineQueueX("lea  rdi,%s", GetLabelStr(hll->labels[LDATA2], buff));//int hll->pcases
          }                              /* here we have to pay attention */
          if (hll->flags & HLLF_DEFAULTOCCURED)
            AddLineQueueX("js  %s", GetLabelStr(hll->labels[LDEF], buff));
          else
            AddLineQueueX("js  %s", GetLabelStr(hll->labels[LEXIT], buff));
        }
      }
#endif

    AddLineQueueX("%s" LABELQUAL, GetLabelStr(hll->labels[LTOP], buff));
    if (Parse_Pass) {
      if (hll->cflag == 5) {
        if (ModuleInfo.Ofssize == USE32) {
          AddLineQueueX("lea     eax,[ecx + ebx]");//int eax = (ecx + ebx) / 2;
          AddLineQueueX("cdq");
          AddLineQueueX("sub     eax,edx");
          AddLineQueueX("sar     eax,1");
          AddLineQueueX("cmp     [edi+eax*4],esi");
          AddLineQueueX("je  %s", GetLabelStr(hll->labels[LJUMP], buff));//got it, jump to the case
          AddLineQueueX("jge %s", GetLabelStr(hll->labels[LSKIP], buff));//else if (hll->pcases[mid] < hll->casecnt)
          AddLineQueueX("lea     ecx,[eax+1]");                          //low = mid + 1
          AddLineQueueX("jmp %s", GetLabelStr(hll->labels[LCONT], buff));//not found yet, continue search
        }
#if AMD64_SUPPORT
        else if (ModuleInfo.Ofssize == USE64) {
          if (hll->csize == 4) {
            AddLineQueueX("lea     eax,[rcx + rbx]");//int eax = (ecx + ebx) / 2;
            AddLineQueueX("sar     rax,1");
            AddLineQueueX("cmp     [rdi+rax*4],esi");
            AddLineQueueX("je  %s", GetLabelStr(hll->labels[LJUMP], buff));//got it, jump to the case
            AddLineQueueX("jge %s", GetLabelStr(hll->labels[LSKIP], buff));//else if (hll->pcases[mid] < hll->casecnt)
            AddLineQueueX("lea     ecx,[rax+1]");                          //low = mid + 1
            AddLineQueueX("jmp %s", GetLabelStr(hll->labels[LCONT], buff));//not found yet, continue search
          }
          else {
            AddLineQueueX("lea     rax,[rcx + rbx]");//int eax = (ecx + ebx) / 2;
            AddLineQueueX("sar     rax,1");
            AddLineQueueX("cmp     [rdi+rax*8],rsi");
            AddLineQueueX("je  %s", GetLabelStr(hll->labels[LJUMP], buff));//got it, jump to the case
            AddLineQueueX("jge %s", GetLabelStr(hll->labels[LSKIP], buff));//else if (hll->pcases[mid] < hll->casecnt)
            AddLineQueueX("lea     rcx,[rax+1]");                          //low = mid + 1
            AddLineQueueX("jmp %s", GetLabelStr(hll->labels[LCONT], buff));//not found yet, continue search
          }
        }
#endif
      }
    }
    AddLineQueueX("%s" LABELQUAL, GetLabelStr(hll->labels[LJUMP], buff));
    if (Parse_Pass) {
      if (hll->cflag == 5) {
        if (ModuleInfo.Ofssize == USE32) {
          GetLabelStr(hll->labels[LDATA1], buff);        
          AddLineQueueX("pop	edi");
          AddLineQueueX("pop	esi");
          AddLineQueueX("pop	ebx");
          AddLineQueueX("pop	ecx");
          AddLineQueueX("pop	edx");
          AddLineQueueX("mov  eax,%s[%r*4]", buff, T_EAX);
          AddLineQueueX("xchg	 eax,[esp]");
          AddLineQueueX("retn");
        }
#if AMD64_SUPPORT
        else if (ModuleInfo.Ofssize == USE64) {
          AddLineQueueX("pop	rdi");
          AddLineQueueX("pop	rsi");
          AddLineQueueX("pop	rbx");
          GetLabelStr(hll->labels[LDATA1], buff);       
          AddLineQueueX("lea   rcx,%s", buff);
          AddLineQueueX("mov   rax, qword ptr[rcx+rax*8]");
          AddLineQueueX("pop   rcx");
          AddLineQueueX("xchg	 rax,[rsp]");
          AddLineQueueX("retn");
        }
#endif
      }
    }
    AddLineQueueX("%s" LABELQUAL, GetLabelStr(hll->labels[LSKIP], buff));
    if (Parse_Pass) {
      if (hll->cflag == 5) {
        if (ModuleInfo.Ofssize == USE32)
          AddLineQueueX("lea     ebx,[eax-1]");//else high = mid - 1;
#if AMD64_SUPPORT
        else if (ModuleInfo.Ofssize == USE64) {
          if (hll->csize == 4)
            AddLineQueueX("lea     ebx,[rax-1]");//else high = mid - 1;
          else
            AddLineQueueX("lea     rbx,[rax-1]");//else high = mid - 1;
        }
#endif
      }
    }
    AddLineQueueX("%s" LABELQUAL, GetLabelStr(hll->labels[LCONT], buff));
    if (Parse_Pass) {
      if (hll->cflag == 5) {
        if (ModuleInfo.Ofssize == USE32) {
          AddLineQueueX("cmp  ecx,ebx");
          AddLineQueueX("jle %s", GetLabelStr(hll->labels[LTOP], buff));
          AddLineQueueX("pop	edi");
          AddLineQueueX("pop	esi");
          AddLineQueueX("pop	ebx");
          AddLineQueueX("pop	ecx");
          AddLineQueueX("pop	edx");
          AddLineQueueX("pop	eax");
          if (hll->flags & HLLF_DEFAULTOCCURED)
            AddLineQueueX("jmp  %s", GetLabelStr(hll->labels[LDEF], buff));
          else
            AddLineQueueX("jmp  %s", GetLabelStr(hll->labels[LEXIT], buff));
        }
#if AMD64_SUPPORT
        else if (ModuleInfo.Ofssize == USE64) {
          if (hll->csize == 4)
            AddLineQueueX("cmp  ecx,ebx");
          else
            AddLineQueueX("cmp  rcx,rbx");
          AddLineQueueX("jle  %s", GetLabelStr(hll->labels[LTOP], buff));
          AddLineQueueX("pop	rdi");
          AddLineQueueX("pop	rsi");
          AddLineQueueX("pop	rbx");
          AddLineQueueX("pop	rcx");
          AddLineQueueX("pop	rax");
          if (hll->flags & HLLF_DEFAULTOCCURED)
            AddLineQueueX("jmp  %s", GetLabelStr(hll->labels[LDEF], buff));
          else
            AddLineQueueX("jmp  %s", GetLabelStr(hll->labels[LEXIT], buff));
        }
#endif
      }
    }
    if (hll->csize == 4) {
      free(hll->pcases);
    }
#if AMD64_SUPPORT
    else if (ModuleInfo.Ofssize == USE64) 
      free(hll->pcases64);
#endif
    free(hll->plabels);
    break;
    /* end SWITCH */
  case T_DOT_ENDF:
  case T_DOT_ENDFOR:
    if (hll->cmd != HLL_FOR) {
      DebugMsg(("HllEndDir: no .FOR on the hll stack\n"));
      EmitErr(BLOCK_NESTING_ERROR, tokenarray[i].string_ptr);
      return(ERROR);
    }
    i++;
    //Insert .CONTINUE label here if there is any
    if (hll->labels[LCONT])
      AddLineQueueX("%s" LABELQUAL, GetLabelStr(hll->labels[LCONT], buff));
    //forever loop '.for (::)'
    if (hll->cmcnt == 0) goto adlabel;
    //If counters exist
    else if (hll->counterlines[0]) {
      //here we write counters
      if (hll->counterlines) {      //if there is something after second ':' expand it here
        ForInitAndNext(tokenarray, hll->cmcnt, hll->counterlines);
        LclFree(hll->counterlines);
      }
      //first jump from the top happens here after the counters
      AddLineQueueX("%s" LABELQUAL, GetLabelStr(hll->labels[LSKIP], buff));
      /* create test label */
      if (hll->labels[LTEST]) {
        AddLineQueueX("%s" LABELQUAL, GetLabelStr(hll->labels[LTEST], buff));
      }
      QueueTestLines(hll->condlines);
      LclFree(hll->condlines);
    }
    else
      AddLineQueueX(" jmp %s", GetLabelStr(hll->labels[LSTART], buff));
    if (!hll->cond)
      adlabel:
    AddLineQueueX(" jmp %s", GetLabelStr(hll->labels[LSTART], buff));
    tokenarray[i].token = T_FINAL;
    break;
    //end of .ENDFOR
  case T_DOT_ENDW:
    if (hll->cmd != HLL_WHILE) {
      DebugMsg(("HllEndDir: no .WHILE on the hll stack\n"));
      return(EmitErr(BLOCK_NESTING_ERROR, tokenarray[i].string_ptr));
    }
    i++;
    /* create test label */
    if (hll->labels[LTEST]) {
      AddLineQueueX("%s" LABELQUAL, GetLabelStr(hll->labels[LTEST], buff));
    }
    QueueTestLines(hll->condlines);
    LclFree(hll->condlines);
    break;
  case T_DOT_UNTILCXZ:
    if (hll->cmd != HLL_REPEAT) {
      DebugMsg(("HllEndDir: no .REPEAT on the hll stack\n"));
      return(EmitErr(BLOCK_NESTING_ERROR, tokenarray[i].string_ptr));
    }
    i++;
    if (hll->labels[LTEST]) /* v2.11: LTEST only needed if .CONTINUE has occured */
      AddLineQueueX("%s" LABELQUAL, GetLabelStr(hll->labels[LTEST], buff));

    /* read in optional (simple) expression */
    if (tokenarray[i].token != T_FINAL) {
      rc = EvaluateHllExpression(hll, &i, tokenarray, LSTART, FALSE, buffer);
      if (rc == NOT_ERROR) {
        rc = CheckCXZLines(buffer);
        if (rc == NOT_ERROR)
          QueueTestLines(buffer); /* write condition lines */
        else
          EmitError(EXPR_TOO_COMPLEX_FOR_UNTILCXZ);
      }
    }
    else {
      AddLineQueueX(JMPPREFIX "loop %s", GetLabelStr(hll->labels[LSTART], buff));
    }
    break;
  case T_DOT_UNTIL:
    if (hll->cmd != HLL_REPEAT) {
      DebugMsg(("HllEndDir: no .REPEAT on the hll stack\n"));
      return(EmitErr(BLOCK_NESTING_ERROR, tokenarray[i].string_ptr));
    }
    i++;
    if (hll->labels[LTEST]) /* v2.11: LTEST only needed if .CONTINUE has occured */
      AddLineQueueX("%s" LABELQUAL, GetLabelStr(hll->labels[LTEST], buff));

    /* read in (optional) expression */
    /* if expression is missing, just generate nothing */
    if (tokenarray[i].token != T_FINAL) {
      rc = EvaluateHllExpression(hll, &i, tokenarray, LSTART, FALSE, buffer);
      if (rc == NOT_ERROR)
        QueueTestLines(buffer); /* write condition lines */
    }
    break;
#ifdef DEBUG_OUT
  default: /**/myassert(0); break;
#endif
  }

  /* create the exit label if it has been referenced */
  if (hll->labels[LEXIT])
    AddLineQueueX("%s" LABELQUAL, GetLabelStr(hll->labels[LEXIT], buff));

  if (tokenarray[i].token != T_FINAL && rc == NOT_ERROR) {
    EmitErr(SYNTAX_ERROR_EX, tokenarray[i].tokpos);
    rc = ERROR;
  }
  if (cmd == T_DOT_ENDSWITCH && Parse_Pass > PASS_1)
    ModuleInfo.list   = 0;

    if (ModuleInfo.list)
      LstWrite(LSTTYPE_DIRECTIVE, GetCurrOffset(), NULL);
    
  /* v2.11: always run line-queue if it's not empty. */
  if (is_linequeue_populated())
    RunLineQueue();

  return(rc);
}

/*
* .ELSE, .ELSEIF, .CONTINUE .CASE .DEFAULT and .BREAK directives.
* .ELSE, .ELSEIF .SWITCH:
*    - create a jump to exit label
*    - render test label if it was referenced
*    - for .ELSEIF, create new test label and evaluate expression
* .CONTINUE, .BREAK:
*    - jump to test / exit label of innermost .WHILE/.REPEAT block
*
*/
ret_code HllExitDir(int i, struct asm_tok tokenarray[])
/*******************************************************/
{
  struct expr         opndx;
  struct hll_item     *hll;
  ret_code            rc = NOT_ERROR;
  int                 j;
  int                 idx;
  int                 cmd = tokenarray[i].tokval;
  int                 *newcp;
  int                nomulcase;
  uint_16             *newlp;
  char buff[16];
  char                *p;
  char buffer[MAX_LINE_LEN];
#if AMD64_SUPPORT
  int_64              *newcp64;
#endif


  DebugMsg1(("HllExitDir(%s) enter\n", tokenarray[i].string_ptr));

  hll = HllStack;
  if (hll == NULL) {
    DebugMsg(("HllExitDir stack error\n"));
    return(EmitError(DIRECTIVE_MUST_BE_IN_CONTROL_BLOCK));
  }
  switch (cmd) {
  case T_DOT_DEFAULT:
    if (hll->flags & HLLF_DEFAULTOCCURED) {
      DebugMsg(("HllExitDir stack error\n"));
      return(EmitError(DIRECTIVE_MUST_BE_IN_CONTROL_BLOCK));
    }
    if (ModuleInfo.switch_style == ASMSWITCH) {
      if (hll->casecnt) {
        if (hll->labels[LEXIT] == 0)
          hll->labels[LEXIT] = GetHllLabel();
        AddLineQueueX("jmp %s", GetLabelStr(hll->labels[LEXIT], buff));
        hll->breakoccured = TRUE;
      }
    }
    if (hll->labels[LDEF] == 0)
      hll->labels[LDEF] = GetHllLabel();
    AddLineQueueX("%s" LABELQUAL, GetLabelStr(hll->labels[LDEF], buff));
    hll->flags |= HLLF_DEFAULTOCCURED;
    i++;
    break;
  case T_DOT_CASE:
    if (Parse_Pass)
    if (hll->cmd != HLL_SWITCH) {
      DebugMsg(("HllExitDir stack error\n"));
      return(EmitError(DIRECTIVE_MUST_BE_IN_CONTROL_BLOCK));
    }
    /* assembler style SWITCH doesn't need the .brake in the source so we have to simulate it */
    if (ModuleInfo.switch_style == ASMSWITCH) {
         nomulcase = TRUE;
         hll->breakoccured = TRUE;
       /* we have to know in advance if there is more multi cases */
        if (LineStoreCurr != NULL && LineStoreCurr->next != NULL){     /* struct LineStoreCurr contains current source*/
          p = LineStoreCurr->next->line;
            while ( isspace( *p )) p++;
          nomulcase = _memicmp(p, ".case", 5); /* test next line if it is */
          }
        if (nomulcase){                                                /* it was the last case in the row  */
          if (hll->multicase >= 1){                                     /* if there was more then 1 case    */
            hll->multicase = 0;                                        /* reset hll->multicase for next occation */
              hll->breakoccured = TRUE;                                /* set the flag but don't exit before code get executed */
            }
          else{                                                        /* there is end of the case's code */
            if (hll->labels[LEXIT] == 0)  hll->labels[LEXIT] = GetHllLabel(); /* moved here to get only 1 test for speed*/
            AddLineQueueX("jmp %s", GetLabelStr(hll->labels[LEXIT], buff)); /* set the .brake */
            hll->breakoccured = TRUE;                                  /* set the flag */
            }
        }
        else{                                                          /* it is a multi case */
          if (hll->multicase == 0) {                                    /* we need a first label for multi cases */
            if (hll->labels[LEXIT] == 0)  hll->labels[LEXIT] = GetHllLabel(); /* moved here to get only 1 test for speed*/
            AddLineQueueX("jmp %s", GetLabelStr(hll->labels[LEXIT], buff)); /* set the .brake */
            hll->breakoccured = TRUE;                                  /* set the flag */
            }
          else{
            hll->breakoccured = FALSE;
            }                                 /* no .brake needed yet */
          hll->multicase++;                                          /* increase counter */
        }
    }
    for (;;) {
      if (hll->breakoccured) {
        hll->labels[LTEST] = GetHllLabel();
        hll->breakoccured = 0;  // if .break did not occure label will not be increased
        AddLineQueueX("%s" LABELQUAL, GetLabelStr(hll->labels[LTEST], buff));
        hll->savedlab = (uint_16)hex2dec(buff + 2);
      }
      i++;
      if (tokenarray[i].token != T_FINAL) {
        strcpy(buffer, tokenarray[i].tokpos);
        DebugMsg1(("HllExitDir(%s): calling EvalOperand, i=%u\n", tokenarray[i].string_ptr, i));
        if (EvalOperand(&i, tokenarray, Token_Count, &opndx, 0) == ERROR) {
          DebugMsg(("HllExitDir(%s): EvalOperand() failed\n", tokenarray[i].string_ptr));
          return(ERROR);
        }
        p = tokenarray[i].tokpos;
        switch (opndx.kind) {
        case EXPR_CONST:
          break;
        case EXPR_FLOAT:
#if FPIMMEDIATE
          if (Options.strict_masm_compat == FALSE) {
            /* convert to REAL4, unless REAL8 coercion is requested */
            atofloat(&opndx.fvalue, opndx.float_tok->string_ptr, opndx.mem_type == MT_REAL8 ? 8 : 4, opndx.negative, opndx.float_tok->floattype);
            break;
          }
#endif
        }
      }
      else {
        DebugMsg(("HllExitDir stack error\n"));
        return(EmitError(MISSING_OPERATOR_IN_EXPRESSION));
      }
      if (hll->csize == 4) {
        if (!hll->maxalloccasen) {
          newcp = malloc(hll->csize * 50);
          if (newcp) hll->pcases = newcp; 
          else Fatal( OUT_OF_MEMORY );
          newlp = malloc(sizeof(uint_16) * 50);
          if (newlp) hll->plabels = newlp; 
          else Fatal( OUT_OF_MEMORY );
          hll->maxalloccasen = 50;
        }
        if (hll->casecnt >= hll->maxalloccasen) {
          hll->maxalloccasen += 50;
          newcp = realloc(hll->pcases,hll->csize * hll->maxalloccasen);
          if (newcp) hll->pcases = newcp; 
          else Fatal( OUT_OF_MEMORY );
          newlp = realloc(hll->plabels,sizeof(uint_16) * hll->maxalloccasen);
          if (newlp) hll->plabels = newlp; 
          else Fatal( OUT_OF_MEMORY );
        }
      }
#if AMD64_SUPPORT
      else {
        if (!hll->maxalloccasen) {
          newcp64 = malloc(hll->csize * 50);
          if (newcp64) hll->pcases64 = newcp64; 
          else Fatal( OUT_OF_MEMORY );
          newlp = malloc(sizeof(uint_16) * 50);
          if (newlp) hll->plabels = newlp; 
          else Fatal( OUT_OF_MEMORY );
          hll->maxalloccasen = 50;
        }
        if (hll->casecnt >= hll->maxalloccasen) {
          hll->maxalloccasen += 50;
          newcp64 = realloc(hll->pcases64,hll->csize * hll->maxalloccasen);
          if (newcp64) hll->pcases64 = newcp64; 
          else Fatal( OUT_OF_MEMORY );
          newlp = realloc(hll->plabels,sizeof(uint_16) * hll->maxalloccasen);
          if (newlp) hll->plabels = newlp; 
          else Fatal( OUT_OF_MEMORY );
        }
      }
#endif
      if ((ModuleInfo.Ofssize == USE32) || (hll->csize == 4)) {
        for (j = 0; j < hll->casecnt; j++) {
          if (hll->pcases[j] == opndx.value)
            EmitErr(CASE_ALREADY_OCCURED_IN_THIS_SWITCH_BLOCK, buffer);
        }
        hll->pcases[hll->casecnt] = opndx.value;
      }
#if AMD64_SUPPORT
      else {
        for (j = 0; j < hll->casecnt; j++) {
          if (hll->pcases64[j] == opndx.value64)
            EmitErr(CASE_ALREADY_OCCURED_IN_THIS_SWITCH_BLOCK, buffer);
        }
        hll->pcases64[hll->casecnt] = opndx.value64;
      }
#endif
      hll->plabels[hll->casecnt] = hll->savedlab;
      hll->casecnt++;
      if (tokenarray[i].token != T_COMMA)
        break;
    }
    break;
  case T_DOT_ELSE:
  case T_DOT_ELSEIF:
    if (hll->cmd != HLL_IF) {
      DebugMsg(("HllExitDir(%s): labels[LTEST]=%X\n", tokenarray[i].string_ptr, hll->labels[LTEST]));
      return(EmitErr(BLOCK_NESTING_ERROR, tokenarray[i].string_ptr));
    }
    /* v2.08: check for multiple ELSE clauses */
    if (hll->flags & HLLF_ELSEOCCURED) {
      return(EmitError(DOT_ELSE_CLAUSE_ALREADY_OCCURED_IN_THIS_DOT_IF_BLOCK));
    }

    /* the 'exit'-label is only needed if an .ELSE branch exists.
    * That's why it is created delayed.
    */
    if (hll->labels[LEXIT] == 0)
      hll->labels[LEXIT] = GetHllLabel();
    AddLineQueueX(JMPPREFIX "jmp %s", GetLabelStr(hll->labels[LEXIT], buff));

    if (hll->labels[LTEST] > 0) {
      AddLineQueueX("%s" LABELQUAL, GetLabelStr(hll->labels[LTEST], buff));
      hll->labels[LTEST] = 0;
    }
    i++;
    if (cmd == T_DOT_ELSEIF) {
      /* create new labels[LTEST] label */
      hll->labels[LTEST] = GetHllLabel();
      rc = EvaluateHllExpression(hll, &i, tokenarray, LTEST, FALSE, buffer);
      if (rc == NOT_ERROR)
        QueueTestLines(buffer);
    }
    else
      hll->flags |= HLLF_ELSEOCCURED;

    break;
  case T_DOT_BREAK:
  case T_DOT_CONTINUE:
    for (; hll && hll->cmd == HLL_IF; hll = hll->next);
    if (hll == NULL) {
      return(EmitError(DIRECTIVE_MUST_BE_IN_CONTROL_BLOCK));
    }
    /* v2.11: create 'exit' and 'test' labels delayed.
    */
    if (cmd == T_DOT_BREAK) {
      hll->breakoccured = TRUE;
      if (hll->labels[LEXIT] == 0)
        hll->labels[LEXIT] = GetHllLabel();
      idx = LEXIT;
    }
    else if (hll->cmd == HLL_FOR) {
      /* added by habran
      .CONTINUE label is created here for .FOR loops only if it exists.
      It is different than .WHILE because it has to first alter counters
      */
      if (hll->labels[LCONT] == 0) hll->labels[LCONT] = GetHllLabel();
      idx = (hll->labels[LCONT] ? LCONT : LSTART);
    }
    else {
      /* 'test' is not created for .WHILE loops here; because
      * if it doesn't exist, there's no condition to test.
      */
      if (hll->cmd == HLL_REPEAT && hll->labels[LTEST] == 0)
        hll->labels[LTEST] = GetHllLabel();
      idx = (hll->labels[LTEST] ? LTEST : LSTART);
    }

    /* .BREAK .IF ... or .CONTINUE .IF ? */
    i++;
    if (tokenarray[i].token != T_FINAL) {
      if (tokenarray[i].token == T_DIRECTIVE && tokenarray[i].tokval == T_DOT_IF) {
        enum hll_cmd savedcmd = hll->cmd;
        hll->cmd = HLL_BREAK;
        i++;
        /* v2.11: set rc and don't exit if an error occurs; see hll3.aso */
        rc = EvaluateHllExpression(hll, &i, tokenarray, idx, TRUE, buffer);
        if (rc == NOT_ERROR)
          QueueTestLines(buffer);
        hll->cmd = savedcmd;
      }
    }
    else {
      AddLineQueueX(JMPPREFIX "jmp %s", GetLabelStr(hll->labels[idx], buff));
    }
    break;
#ifdef DEBUG_OUT
  default: /**/myassert(0); break;
#endif
  }
  if (tokenarray[i].token != T_FINAL && rc == NOT_ERROR) {
    EmitErr(SYNTAX_ERROR_EX, tokenarray[i].tokpos);
    rc = ERROR;
  }

  if (ModuleInfo.list)
    LstWrite(LSTTYPE_DIRECTIVE, GetCurrOffset(), NULL);

  /* v2.11: always run line-queue if it's not empty. */
  if (is_linequeue_populated())
    RunLineQueue();

  return(rc);
}

/* check if an hll block has been left open. called after pass 1 */

void HllCheckOpen(void)
/***********************/
{
  if (HllStack) {
    //EmitErr( BLOCK_NESTING_ERROR, ".if-.repeat-.while" );
    EmitErr(UNMATCHED_BLOCK_NESTING, ".if-.repeat-.while");
  }
  DebugMsg(("HllCheckOpen: allocated items:%u, reused items:%u, .while cond-blocks/bytes:%u/%u\n", cntAlloc, cntReused, cntCond, cntCondBytes));
}

#if FASTMEM==0
void HllFini(void)
/******************/
{
  struct hll_item *curr;
  struct hll_item *next;
  /* release the items in the HllFree heap.
  * there might also be some left in HllStack...
  */
  for (curr = HllFree; curr; curr = next) {
    next = curr->next;
    LclFree(curr);
  }
}
#endif

/* HllInit() is called for each pass */

void HllInit(int pass)
/**********************/
{
  //if ( pass == PASS_1 )
  //    HllFree = NULL;

  //HllStack = NULL; /* empty stack of open hll directives */
  ModuleInfo.hll_label = 0; /* init hll label counter */
#ifdef DEBUG_OUT
  evallvl = 0;
  if (pass == PASS_1) {
    cntAlloc = 0;
    cntReused = 0;
    cntCond = 0;
    cntCondBytes = 0;
  }
#endif
  return;
}
