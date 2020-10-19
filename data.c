/****************************************************************************
*
*                            Open Watcom Project
*
*    Portions Copyright (c) 1983-2002 Sybase, Inc. All Rights Reserved.
*
*  ========================================================================
*
*    This file contains Original Code and/or Modifications of Original
*    Code as defined in and that are subject to the Sybase Open Watcom
*    Public License version 1.0 (the 'License'). You may not use this file
*    except in compliance with the License. BY USING THIS FILE YOU AGREE TO
*    ALL TERMS AND CONDITIONS OF THE LICENSE. A copy of the License is
*    provided with the Original Code and Modifications, and is also
*    available at www.sybase.com/developer/opensource.
*
*    The Original Code and all software distributed under the License are
*    distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
*    EXPRESS OR IMPLIED, AND SYBASE AND ALL CONTRIBUTORS HEREBY DISCLAIM
*    ALL SUCH WARRANTIES, INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF
*    MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR
*    NON-INFRINGEMENT. Please see the License for the specific language
*    governing rights and limitations under the License.
*
*  ========================================================================
*
* Description:  data definition. handles
*               - directives DB,DW,DD,...
*               - predefined types (BYTE, WORD, DWORD, ...)
*               - arbitrary types
*
****************************************************************************/

#include <ctype.h>
#include "globals.h"
#include "memalloc.h"
#include "parser.h"
#include "expreval.h"
#include "input.h"
#include "fixup.h"
#include "listing.h"
#include "segment.h"
#include "types.h"
#include "fastpass.h"
#include "tokenize.h"
#include "macro.h"
#include "omf.h"
#include "atofloat.h"
#include "myassert.h"

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

extern ret_code segm_override( const struct expr *, struct code_info * );
extern struct asym *SegOverride;
extern const char szNull[];
extern UINT_PTR UTF8toWideChar(const unsigned char *pSource, UINT_PTR nSourceLen, UINT_PTR *nSourceDone, unsigned short *szTarget, UINT_PTR nTargetMax);
static ret_code data_item( int *, struct asm_tok[], struct asym *, uint_32, const struct asym *, uint_32, bool inside_struct, bool, bool, int );

#define OutputDataBytes( x, y ) OutputBytes( x, y, NULL )
#define OutputInterleavedDataBytes( x, y ) OutputInterleavedBytes( x, y, NULL );

/* This function shifts left 128 for the RECORD */
 void ShiftLeft (uint_64 *dstHi, uint_64 *dstLo,uint_64 num, int pos)
  {
  uint_64 orHi = 0;
  uint_64 orLo = num;
  
    if (pos >=  0x40){
     orHi = orLo; 
     orLo = 0;
     pos = pos - 0x40;
     orHi = orHi << pos;
    } 
    else {
      orLo = orLo << pos;
      num = num >> ~pos;
      orHi |= num;
    } 
    *dstHi |= orHi;
    *dstLo |= orLo;
  }

/* initialize an array inside a structure
 * if there are no brackets, the next comma, '>' or '}' will terminate
 *
 * valid initialization are:
 * - an expression, might contain DUP or a string enclosed in quotes.
 * - a literal enclosed in <> or {} which then is supposed to contain
 *   single items.
 */
static ret_code InitializeArray( const struct sfield *f, int *pi, struct asm_tok tokenarray[] )
/*********************************************************************************************/
{
    uint_32 oldofs;
    uint_32 no_of_bytes;
    int  i = *pi;
    int  j;
    int  lvl;
    int old_tokencount;
    char bArray;
    ret_code rc;

    oldofs = GetCurrOffset();
    no_of_bytes = SizeFromMemtype( f->sym.mem_type, USE_EMPTY, f->sym.type );

    DebugMsg1(("InitializeArray(%s) enter, items=%" I32_SPEC "u size=%" I32_SPEC "u mem_type=%xh type=%s currofs=%" I32_SPEC "X [%s]\n",
               f->sym.name, f->sym.total_length, no_of_bytes, f->sym.mem_type,
               f->sym.type ? f->sym.type->name : "NULL", oldofs, tokenarray[i].tokpos ));

    /* If current item is a literal enclosed in <> or {}, just use this
     * item. Else, use all items until a comma or EOL is found.
     */

    if ( tokenarray[i].token != T_STRING ||
         ( tokenarray[i].string_delim != '<' &&
           tokenarray[i].string_delim != '{' )) {

        /* scan for comma or final. Ignore commas inside DUP argument */
        for( j = i, lvl = 0, bArray = FALSE; tokenarray[j].token != T_FINAL; j++ ) {
            if ( tokenarray[j].token == T_OP_BRACKET )
                lvl++;
            else if ( tokenarray[j].token == T_CL_BRACKET )
                lvl--;
            else if ( lvl == 0 && tokenarray[j].token == T_COMMA )
                break;
            else if ( tokenarray[j].token == T_RES_ID && tokenarray[j].tokval == T_DUP )
                bArray = TRUE;
            else if ( no_of_bytes == 1 && tokenarray[j].token == T_STRING &&
                     ( tokenarray[j].string_delim == '"' || tokenarray[j].string_delim == '\'' ))
                bArray = TRUE;
        }
        *pi = j;

        if ( bArray == FALSE ) {
            DebugMsg1(("InitializeArray(%s): error, array init requires DUP, string or literal\n", f->sym.name ));
            return( EmitErr( INITIALIZER_MUST_BE_A_STRING_OR_SINGLE_ITEM, tokenarray[i].tokpos ) );
        }

        lvl = tokenarray[j].tokpos - tokenarray[i].tokpos;

        /* v2.07: accept an "empty" quoted string as array initializer for byte arrays */
        if ( lvl == 2 &&
            f->sym.total_size == f->sym.total_length &&
            ( tokenarray[i].string_delim == '"' || tokenarray[i].string_delim == '\'' ) )
            rc = NOT_ERROR;
        else {
            lvl = i; /* i must remain the start index */
            rc = data_item( &lvl, tokenarray, NULL, no_of_bytes, f->sym.type, 1, FALSE, f->sym.mem_type & MT_FLOAT, FALSE, j );
        }

    } else {

        /* initializer is a literal */
        (*pi)++;
        old_tokencount = Token_Count;
        j = Token_Count + 1;
        /* if the string is empty, use the default initializer */
        if ( tokenarray[i].stringlen == 0 ) {
            Token_Count = Tokenize( (char *)f->ivalue, j, tokenarray, TOK_RESCAN );
        } else {
            Token_Count = Tokenize( tokenarray[i].string_ptr, j, tokenarray, TOK_RESCAN );
        }
        rc = data_item( &j, tokenarray, NULL, no_of_bytes, f->sym.type, 1, FALSE, f->sym.mem_type & MT_FLOAT, FALSE, Token_Count );
        Token_Count = old_tokencount;
    }

    /* get size of array items */
    no_of_bytes = GetCurrOffset() - oldofs ;
    DebugMsg1(("InitializeArray(%s): new offset=%X\n", f->sym.name, no_of_bytes + oldofs ));

    if ( no_of_bytes > f->sym.total_size ) {
        DebugMsg1(("InitializeArray(%s): error, cursize=%u total_size=%u\n", f->sym.name, no_of_bytes, f->sym.total_size ));
        EmitErr( TOO_MANY_INITIAL_VALUES_FOR_ARRAY, tokenarray[i].tokpos );
        rc = ERROR;
    } else if ( no_of_bytes < f->sym.total_size ) {
        char filler = NULLC;
        DebugMsg1(("InitializeArray: remaining bytes=%" I32_SPEC "u\n", f->sym.total_size - no_of_bytes ));
        if ( CurrSeg && CurrSeg->e.seginfo->segtype == SEGTYPE_BSS )
            SetCurrOffset( CurrSeg, f->sym.total_size - no_of_bytes, TRUE, TRUE );
        else {
            /* v2.07: if element size is 1 and a string is used as initial value,
             * pad array with spaces!
             */
            if ( f->sym.total_size == f->sym.total_length &&
                ( f->ivalue[0] == '"' || f->ivalue[0] == '\'' ) )
                filler = ' ';
            FillDataBytes( filler, f->sym.total_size - no_of_bytes );
        }
    }

    DebugMsg1(("InitializeArray(%s) exit, curr ofs=%X\n", f->sym.name, GetCurrOffset() ));
    return( rc );
}

/* initialize a STRUCT/UNION/RECORD data item
 * index:    index of initializer literal
 * symtype:  type of data item
 * embedded: is != NULL if proc is called recursively

 * up to v2.08, this proc did emit ASM lines with simple types
 * to actually "fill" the structure.
 * since v2.09, it calls data_item() directly.

 * Since this function may be reentered, it's necessary to save/restore
 * global variable Token_Count.
 */
static ret_code InitStructuredVar( int index, struct asm_tok tokenarray[], const struct dsym *symtype, struct asym *embedded )
/****************************************************************************************************************************/
{
    //char            *ptr;
    struct sfield   *f;
    int_32          nextofs;
    int             i;
    int             old_tokencount = Token_Count;
    char            *old_stringbufferend = StringBufferEnd;
    int             lvl;
    uint_64         dst128Hi;
    uint_64         dst128Lo;
    uint_64         dwRecIHi;
    uint_64         dwRecILo;
    uint_64         dwRecInit;
    
//#else
 //   uint_32         dwRecInit;
//#endif
    bool            is_record_set;
    struct expr     opndx;
    //char            line[MAX_LINE_LEN];

    DebugMsg1(("InitStructuredVar(%s) enter, total_size=%" I32_SPEC "u, init=>%s<, embedded=%s, alignm=%u\n",
              symtype->sym.name, symtype->sym.total_size, tokenarray[index].string_ptr, embedded ? embedded->name : "NULL", symtype->e.structinfo->alignment ));

    /**/myassert( symtype->sym.state == SYM_TYPE && symtype->sym.typekind != TYPE_TYPEDEF );

    if ( tokenarray[index].token == T_STRING ) {
        /* v2.08: no special handling of {}-literals anymore */
        if ( tokenarray[index].string_delim != '<' &&
            tokenarray[index].string_delim != '{' ) {
            return( EmitError( MISSING_ANGLE_BRACKET_OR_BRACE_IN_LITERAL ) );
        }
        i = Token_Count + 1;
        //strcpy( line, tokenarray[index].string_ptr );
        Token_Count = Tokenize( tokenarray[index].string_ptr, i, tokenarray, TOK_RESCAN );
        /* once Token_Count has been modified, don't exit without
         * restoring this value!
         */
        index++;

    } else if ( embedded &&
                ( tokenarray[index].token == T_COMMA ||
                 tokenarray[index].token == T_FINAL)) {
        i = Token_Count;
    } else {
        return( EmitErr( INITIALIZER_MUST_BE_A_STRING_OR_SINGLE_ITEM, embedded ? embedded->name : "" ) );
    }
    if ( symtype->sym.typekind == TYPE_RECORD ) {
        dwRecInit = 0;
        dst128Hi = 0; /* clear Hi 64 bit for the 128 bit RECORD */
        dst128Lo = 0; /* clear Lo 64 bit for the 128 bit RECORD */
        is_record_set = FALSE;
    }
    //__debugbreak();
    /* scan the STRUCT/UNION/RECORD's members */
    for( f = symtype->e.structinfo->head; f != NULL; f = f->next ) {

        DebugMsg1(("InitStructuredVar(%s) field=%s ofs=%" I32_SPEC "u total_size=%" I32_SPEC "u total_len=%" I32_SPEC "u value=>%s< >%s<\n",
                  symtype->sym.name,
                  f->sym.name,
                  f->sym.offset,
                  f->sym.total_size,
                  f->sym.total_length,
                  //f->initializer ? f->initializer : "NULL",
                  f->ivalue, tokenarray[i].tokpos ));

        /* is it a RECORD field? */
        if ( f->sym.mem_type == MT_BITS ) {
            if ( tokenarray[i].token == T_COMMA || tokenarray[i].token == T_FINAL ) {
                if ( f->ivalue[0] ) {
                    int j = Token_Count + 1;
                    int max_item = Tokenize( f->ivalue, j, tokenarray, TOK_RESCAN );
                    EvalOperand( &j, tokenarray, max_item, &opndx, 0 );
                    is_record_set = TRUE;
                } else {
                    opndx.value = 0;
                    opndx.kind = EXPR_CONST;
                    opndx.quoted_string = NULL;
                }
            } else {
                EvalOperand( &i, tokenarray, Token_Count, &opndx, 0 );
                is_record_set = TRUE;
            }
            if ( opndx.kind != EXPR_CONST || opndx.quoted_string != NULL )
                EmitError( CONSTANT_EXPECTED );

            /* fixme: max bits in 64-bit is 64 - see MAXRECBITS! */
            if ( f->sym.total_size < 32 ) {
                uint_32 dwMax = (1 << f->sym.total_size);
                if ( opndx.value >= dwMax )
                    EmitErr( INITIALIZER_MAGNITUDE_TOO_LARGE, f->sym.name );
            }
            if (symtype->sym.mem_type == MT_OWORD){
                dwRecIHi = 0;  /* clear Hi 64 bit for the 128 bit RECORD */
                dwRecILo = 0;  /* clear Lo 64 bit for the 128 bit RECORD */
                ShiftLeft(&dwRecIHi,&dwRecILo,opndx.llvalue,f->sym.offset);
                dst128Hi |= dwRecIHi; /* OR Hi 64 bit for the 128 bit RECORD */
                dst128Lo |= dwRecILo; /* clear Lo 64 bit for the 128 bit RECORD */
              }
            else
               dwRecInit |= opndx.llvalue << f->sym.offset;

        //} else if ( f->init_dir == NULL ) {  /* embedded struct? */
        } else if ( f->ivalue[0] == NULLC ) {  /* embedded struct? */

            InitStructuredVar( i, tokenarray, (struct dsym *)f->sym.type, &f->sym );
            if ( tokenarray[i].token == T_STRING )
                i++;

        } else if ( f->sym.isarray &&
                    tokenarray[i].token != T_FINAL &&
                    tokenarray[i].token != T_COMMA ) {
            if ( ERROR == InitializeArray( f, &i, tokenarray ) )
                break;

        } else if ( f->sym.total_size == f->sym.total_length &&
                   tokenarray[i].token == T_STRING &&
                   tokenarray[i].stringlen > 1 &&
                   ( tokenarray[i].string_delim == '"' ||
                    tokenarray[i].string_delim == '\'' ) ) {
            /* v2.07: it's a byte type, but no array, string initializer must have true length 1 */
            EmitError( STRING_OR_TEXT_LITERAL_TOO_LONG );
            i++;
        } else {
            //struct asym *sym;
            uint_32 no_of_bytes = SizeFromMemtype( f->sym.mem_type, USE_EMPTY, f->sym.type );

            /* v2.09: check for empty <> or {} string if a struct/union is embedded */
            //for ( sym = f->sym->type; sym && sym->type; sym = sym->type );

            if ( tokenarray[i].token == T_FINAL || tokenarray[i].token == T_COMMA ) {
                int tc = Token_Count;
                int j = Token_Count+1;
                Token_Count = Tokenize( f->ivalue, j, tokenarray, TOK_RESCAN );
                data_item( &j, tokenarray, NULL, no_of_bytes, f->sym.type, 1, FALSE, f->sym.mem_type & MT_FLOAT, FALSE, Token_Count );
                Token_Count = tc;
            } else {
                char c;
                int j = i;
                /* ignore commas enclosed in () ( might occur inside DUP argument! ).
                 */
                for ( lvl = 0, c = 0; tokenarray[i].token != T_FINAL; i++ ) {
                    if ( tokenarray[i].token == T_OP_BRACKET )
                        lvl++;
                    else if ( tokenarray[i].token == T_CL_BRACKET )
                        lvl--;
                    else if ( lvl == 0 && tokenarray[i].token == T_COMMA )
                        break;
                    else if ( tokenarray[i].token == T_RES_ID && tokenarray[i].tokval == T_DUP )
                        c++; /* v2.08: check added */
                }
                if ( c ) {
                    EmitErr( INITIALIZER_MUST_BE_A_STRING_OR_SINGLE_ITEM, tokenarray[j].tokpos );
                } else
                    if ( ERROR == data_item( &j, tokenarray, NULL, no_of_bytes, f->sym.type, 1, FALSE, f->sym.mem_type & MT_FLOAT, FALSE, i ) ) {
                        EmitErr( INVALID_DATA_INITIALIZER, f->sym.name );
                    }
            }
        }
        /* Add padding bytes if necessary (never inside RECORDS!).
         * f->next == NULL : it's the last field of the struct/union/record
         */
        if ( symtype->sym.typekind != TYPE_RECORD ) {
            if ( f->next == NULL || symtype->sym.typekind == TYPE_UNION )
                nextofs = symtype->sym.total_size;
            else
                nextofs = f->next->sym.offset;

            if ( f->sym.offset + f->sym.total_size < nextofs ) {
                DebugMsg1(("InitStructuredVar: padding, field=%s ofs=%" I32_SPEC "X total=%" I32_SPEC "X nextofs=%" I32_SPEC "X\n",
                          f->sym.name, f->sym.offset, f->sym.total_size, nextofs ));
                SetCurrOffset( CurrSeg, nextofs - (f->sym.offset + f->sym.total_size), TRUE, TRUE );
            }
        }
        /* for a union, just the first field is initialized */
        if ( symtype->sym.typekind == TYPE_UNION )
            break;

        if ( f->next != NULL ) {

            if ( tokenarray[i].token != T_FINAL )
                if ( tokenarray[i].token == T_COMMA )
                    i++;
                else {
                    EmitErr( SYNTAX_ERROR_EX, tokenarray[i].tokpos );
                    while ( tokenarray[i].token != T_FINAL && tokenarray[i].token != T_COMMA )
                        i++;
                }
        }
    }  /* end for */

    if ( symtype->sym.typekind == TYPE_RECORD ) {
        int no_of_bytes;
        switch ( symtype->sym.mem_type ) {
        case MT_BYTE: no_of_bytes = 1; break;
        case MT_WORD: no_of_bytes = 2; break;
#if AMD64_SUPPORT
        case MT_QWORD: no_of_bytes = 8; break;
        case MT_OWORD: no_of_bytes = 16; break;
#endif
        default: no_of_bytes = 4;
        }
        if (is_record_set){
          if (symtype->sym.mem_type == MT_OWORD){
            /* output 128 bit data for RECORD */
            OutputDataBytes((uint_8 *)&dst128Lo, no_of_bytes / 2);
            OutputDataBytes((uint_8 *)&dst128Hi, no_of_bytes / 2);
            }
          else
              OutputDataBytes((uint_8 *)&dwRecInit, no_of_bytes);
          }
        else
            SetCurrOffset( CurrSeg, no_of_bytes, TRUE, TRUE );
    }

    if ( tokenarray[i].token != T_FINAL ) {
        DebugMsg1(("InitStructuredVar(%s): error, i=%u token=%s\n", symtype->sym.name, i, tokenarray[i].string_ptr ));
        EmitErr( TOO_MANY_INITIAL_VALUES_FOR_STRUCTURE, tokenarray[i].tokpos );
    }

    /* restore token status */
    Token_Count = old_tokencount;
    StringBufferEnd = old_stringbufferend;
    DebugMsg1(("InitStructuredVar(%s) exit, current ofs=%" I32_SPEC "X\n", symtype->sym.name, GetCurrOffset() ));
    return( NOT_ERROR );
}

/*
 * convert a string into little endian format - ( LSB 1st, LSW 1st ... etc ).
 * <len> is the TYPE, may be 2,4,6,8,10?,16
 */
static char *little_endian( const char *src, unsigned len )
/*********************************************************/
{
    /* v2.06: input and output buffer must be different! */
    char *dst = StringBufferEnd;

    for( ; len > 1; dst++, src++, len-- ) {
        len--;
        *dst = *(src + len);
        *(dst + len) = *src;
    }
    if ( len )
        *dst = *src;

    return( StringBufferEnd );
}

static void output_float( const struct expr *opnd, unsigned size )
/****************************************************************/
{
    /* v2.07: buffer extended to max size of a data item (=32).
     * test case: XMMWORD REAL10 ptr 1.0
     */
    char buffer[32];
	char tstr[32];

    if ( opnd->mem_type != MT_EMPTY ) {
        int i;
        memset( buffer, 0, sizeof( buffer ) );
        i = SizeFromMemtype( opnd->mem_type, USE_EMPTY, NULL );
        if ( i > size )
            EmitErr( INITIALIZER_OUT_OF_RANGE );
        else {
            atofloat( buffer, opnd->float_tok->string_ptr, i , opnd->negative, opnd->float_tok->floattype );
        }
    } 
	else if (opnd->kind == EXPR_FLOATI)
	{
		snprintf(tstr, sizeof(tstr), "%f", opnd->fvalue);
		atofloat( buffer, tstr, size, opnd->negative, 0 );
	}
	else {
        atofloat( buffer, opnd->float_tok->string_ptr, size, opnd->negative, opnd->float_tok->floattype );
    }
    OutputDataBytes( buffer, size );
    return;
}

/*
 * initialize a data item or struct member;
 * - start_pos: contains tokenarray[] index [in/out]
 * - tokenarray[]: token array
 *
 * - sym:
 *   for data item:     label (may be NULL)
 *   for struct member: field/member (is never NULL)
 *
 * - no_of_bytes: size of item in bytes
 * - type_sym:
 *   for data item:     if != NULL, item is a STRUCT/UNION/RECORD.
 *   for struct member: if != NULL, item is a STRUCT/UNION/RECORD/TYPEDEF.
 *
 * - dup: array size if called by DUP operator, otherwise 1
 * - inside_struct: TRUE=inside a STRUCT declaration
 * - is_float: TRUE=item is float
 * - first: TRUE=first item in a line
 *
 * the symbol will have its 'isarray' flag set if any of the following is true:
 * 1. at least 2 items separated by a comma are used for initialization
 * 2. the DUP operator occures
 * 3. item size is 1 and a quoted string with len > 1 is used as initializer
 */

static ret_code data_item( int *start_pos, struct asm_tok tokenarray[], struct asym *sym, uint_32 no_of_bytes, const struct asym *type_sym, uint_32 dup, bool inside_struct, bool is_float, bool first, int end )
/***************************************************************************************************************************************************************************************************************/
{
    int                 i;
    int                 j;
    int                 string_len;
    uint_32             total = 0;
    bool                initwarn = FALSE;
    bool                half = FALSE;
    //unsigned int        count;
    uint_8              *pchar,*p;
    char                tmp;
    enum fixup_types    fixup_type;
    struct fixup        *fixup;
    struct expr         opndx;
    uint_16 buff[256];
    DebugMsg1(("data_item( idx=%u [%s], label=%s, no_of_bytes=%" I32_SPEC "u, type=%s, dup=%" I32_SPEC "Xh, inside_struct=%u, is_float=%u ) enter\n",
               *start_pos, tokenarray[*start_pos].tokpos, sym ? sym->name : "NULL",
               no_of_bytes, type_sym ? type_sym->name : "NULL",
               dup, inside_struct, is_float ));

    for ( ; dup; dup-- ) {
    i = *start_pos;
next_item:  /* <--- continue scan if a comma has been detected */
    /* since v1.94, the expression evaluator won't handle strings
     * enclosed in <> or {}. That is, in previous versions syntax
     * "mov eax,<1>" was accepted, now it's rejected.
     */
    if ( tokenarray[i].token == T_STRING && ( tokenarray[i].string_delim == '<'  || tokenarray[i].string_delim == '{' ) ) {
        if( type_sym ) {
            DebugMsg1(("data_item(%s): literal/brace found: >%s<, inside_struct=%u, no_of_bytes=%" I32_SPEC "u, curr_ofs=%" I32_SPEC "X\n",
                       type_sym->name, tokenarray[i].string_ptr, inside_struct, no_of_bytes, GetCurrOffset()));

            /* it's either a real data item - then inside_struct is FALSE -
             * or a structure FIELD of arbitrary type.
             *
             * v2.10: regression in v2.09: alias types weren't skipped for InitStructuredVar()
             */
            while ( type_sym->type ) type_sym = type_sym->type;
            if( inside_struct == FALSE ) {
                if ( InitStructuredVar( i, tokenarray, (struct dsym *)type_sym, NULL ) == ERROR )
                    return( ERROR );
            } else {
                /* v2.09: emit a warning if a TYPEDEF member is a simple type,
                 * but is initialized with a literal.
                 * Note: Masm complains about such literals only if the struct is instanced OR -Fl is set.
                 * fixme: the best solution is to always set type_sym to NULL if
                 * the type is a TYPEDEF. if the item is a struct member, then
                 * sym is ALWAYS != NULL and the symbol's type can be gained from there.
                 * v2.10: aliases are now already skipped here ( see above ).
                 */
                //while ( type_sym->type ) /* skip alias types */
                //    type_sym = type_sym->type;
                if( type_sym->typekind == TYPE_TYPEDEF && Parse_Pass == PASS_1 )
                    EmitWarn( 2, COUNT_MUST_BE_POSITIVE_OR_ZERO, tokenarray[i].tokpos );
            }

            total++;
            i++;
            goto item_done;
#if 0 /* v2.08: just let EvalOperand() emit 'Unexpected literal...' error */
        } else {
            DebugMsg(("data_item: invalid string initializer >%s<\n", tokenarray[i].tokpos ));
            /* Masm accepts invalid initializer strings if they are actually never used
             * for initialization.
             */
            if( inside_struct && Parse_Pass == PASS_1 ) {
                EmitWarn( 2, UNEXPECTED_LITERAL_FOUND_IN_EXPRESSION, tokenarray[i].tokpos );
                tokenarray[i].token = T_QUESTION_MARK;
            }
#endif
        }
    }

	if (tokenarray[i].token == T_QUESTION_MARK)
	{
		opndx.kind = EXPR_EMPTY;
	
	}
    else
        if ( EvalOperand( &i, tokenarray, end, &opndx, 0 ) == ERROR )
            return( ERROR );

    //DebugMsg(("data_item, EvalOperand() returned, opndx.kind=%u\n", opndx.kind ));

    /* handle DUP operator */

    if ( tokenarray[i].token == T_RES_ID && tokenarray[i].tokval == T_DUP ) {
        /* v2.03: db 'AB' dup (0) is valid syntax! */
        //if ( opndx.kind != EXPR_CONST || opndx.string != NULL ) {
        if ( opndx.kind != EXPR_CONST ) {
            DebugMsg(("data_item, error, unexpected kind=%u of DUP's first operand\n", opndx.kind ));
            /* v2.09: emit a better error msg if a forward ref was used */
            if ( opndx.sym && opndx.sym->state == SYM_UNDEFINED )
                EmitErr( SYMBOL_NOT_DEFINED, opndx.sym->name );
            else
                EmitError( CONSTANT_EXPECTED );
            return( ERROR );
        }
        /* max dup is 0x7fffffff */
        if ( opndx.value < 0 ) {
            return( EmitError( COUNT_MUST_BE_POSITIVE_OR_ZERO ) );
        }
        i++;
        if( tokenarray[i].token != T_OP_BRACKET ) {
            DebugMsg(("data_item error, missing '('\n"));
            return( EmitErr( EXPECTED, "(" ) );
        }
        i++;

        if ( sym )
            sym->isarray = TRUE;

        if ( opndx.value == 0 ) {
            int level = 1;
            for ( ; tokenarray[i].token != T_FINAL; i++ ) {
                if ( tokenarray[i].token == T_OP_BRACKET )
                    level++;
                else if ( tokenarray[i].token == T_CL_BRACKET )
                    level--;
                if ( level == 0 )
                    break;
            }
        } else {
            DebugMsg1(("data_item(%s): op DUP, count=%" I32_SPEC "Xh, calling data_item()\n", sym ? sym->name : "NULL", opndx.uvalue ));
            if ( data_item( &i, tokenarray, sym, no_of_bytes, type_sym, opndx.uvalue, inside_struct, is_float, first, end ) == ERROR ) {
                DebugMsg(("data_item(%s): op DUP, count=%" I32_SPEC "Xh, returned with error\n", sym ? sym->name : "NULL", opndx.uvalue ));
                return( ERROR );
            }
        }
        if( tokenarray[i].token != T_CL_BRACKET ) {
            DebugMsg(("data_item: error 'missing ')', exit\n"));
            return( EmitErr( EXPECTED, ")" ) );
        }
        /* v2.09: SIZE and LENGTH actually don't return values for "first initializer, but
         * the "first dimension" values
         * v2.11: fixme: if the first dimension is 0 ( opndx.value == 0),
         * Masm ignores the expression - may be a Masm bug!
         */
        if ( sym && first && Parse_Pass == PASS_1 ) {
            sym->first_size = opndx.value * no_of_bytes;
            sym->first_length = opndx.value;
            first = FALSE;
        }

        i++;
        goto item_done;
    }
    /* a STRUCT/UNION/RECORD data item needs a literal as initializer */
    /* v2.06: changed */
    //if( type_sym != NULL && inside_struct == FALSE ) {
    //if( type_sym != NULL && type_sym->typekind != TYPE_TYPEDEF ) {
    if( type_sym ) {
        /* v2.10: use a temp symbol, so the original type name can be displayed in the error msg. */
        const struct asym *tmp = type_sym;
        /* v2.10: check if target type is a struct/union/record; if yes, it's an error. */
        while ( tmp->type ) tmp = tmp->type;
        if ( tmp->typekind != TYPE_TYPEDEF ) {
            DebugMsg1(("data_item: error, type=%s needs literal, >%s<\n", type_sym->name, tokenarray[i].tokpos ));
            return( EmitErr( STRUCTURE_IMPROPERLY_INITIALIZED, type_sym->name ) );
        }
    }

    /* handle '?' */
    if ( opndx.kind == EXPR_EMPTY && tokenarray[i].token == T_QUESTION_MARK ) {
        DebugMsg1(("data_item: ? found, curr_ofs=%X\n", GetCurrOffset()));
        opndx.uvalue = no_of_bytes;
        /* tiny optimization for uninitialized arrays */
        if ( tokenarray[i+1].token != T_COMMA && i == *start_pos ) {
            opndx.uvalue *= dup;
            total += dup;
            dup = 1; /* force loop exit */
        } else {
            total++;
        }
        if( !inside_struct ) {
            SetCurrOffset( CurrSeg, opndx.uvalue, TRUE, TRUE );
        }
        i++;
        goto item_done;
    }

    /* warn about initialized data in BSS/AT segments */
    if ( Parse_Pass == PASS_2 &&
        inside_struct == FALSE  &&
        // CurrSeg != NULL &&  /* this is already ensured to be true */
        (CurrSeg->e.seginfo->segtype == SEGTYPE_BSS ||
         CurrSeg->e.seginfo->segtype == SEGTYPE_ABS) &&
        initwarn == FALSE ) {
        EmitWarn( 2,
                 INITIALIZED_DATA_NOT_SUPPORTED_IN_SEGMENT,
                 (CurrSeg->e.seginfo->segtype == SEGTYPE_BSS) ? "BSS" : "AT" );
        initwarn = TRUE;
    };

    switch( opndx.kind ) {
    case EXPR_EMPTY:
        DebugMsg(("data_item.EMPTY: idx=%u, tokenarray.token=%X\n", i, tokenarray[i].token));
        if ( tokenarray[i].token != T_FINAL )
            EmitErr( SYNTAX_ERROR_EX, tokenarray[i].tokpos );
        else
            EmitError( SYNTAX_ERROR );
        return( ERROR );
    case EXPR_FLOAT:
        DebugMsg1(("data_item.FLOAT: >%s<, inside_struct=%u, no_of_bytes=%u, curr_ofs=%X\n",
                   opndx.float_tok->string_ptr, inside_struct, no_of_bytes, GetCurrOffset()));
        if (!inside_struct)
            output_float( &opndx, no_of_bytes );
        total++;
        break;
    case EXPR_CONST:
        if ( is_float ) {
			opndx.fvalue = (float)opndx.value;
			opndx.kind = EXPR_FLOATI;
			opndx.negative = (opndx.fvalue < 0) ? 1 : 0;
			if (opndx.negative)
				opndx.fvalue *= -1;
			output_float( &opndx, no_of_bytes); /* Coerce const integer value to float in data definitions */
			total++;
			break;
            //return( EmitError( MUST_USE_FLOAT_INITIALIZER ) );
        }

        /* a string returned by the evaluator (enclosed in quotes!)? */

        if ( opndx.quoted_string ) {
            DebugMsg1(("data_item.CONST: string found: >%s<, inside_struct=%u, no_of_bytes=%u, curr_ofs=%X\n",
                       opndx.quoted_string->string_ptr, inside_struct, no_of_bytes, GetCurrOffset()));
            pchar = (uint_8 *)opndx.quoted_string->string_ptr + 1;
            string_len = opndx.quoted_string->stringlen; /* this is the length without quotes */
            /* v2.07: check for empty string for ALL types */
            if ( string_len == 0 ) {
                if ( inside_struct ) {
                    /* when the struct is declared, it's no error -
                     * but won't be accepted when the struct is instanced.
                     * v2.07: don't modify string_len! Instead
                     * mark field as array!
                     */
                    //string_len = 1;
                    sym->isarray = TRUE;
                } else {
                    return( EmitError( EMPTY_STRING ) ); /* MASM doesn't like "" */
                }
            }
            /* a string is only regarded as an array if item size is 1 */
            /* else it is regarded as ONE item */
            if( no_of_bytes != 1 ) 
			{
				if (Options.masm51_compat || Options.strict_masm_compat || !Options.literal_strings)
				{
					if (string_len > no_of_bytes)
						return(EmitError(INITIALIZER_OUT_OF_RANGE));
				}
				else
				{
                if (string_len > no_of_bytes && sym && sym->mem_type != MT_WORD)
						return(EmitError(INITIALIZER_OUT_OF_RANGE));
                /* if characters are not single byte, 2 bytes are used for 1 size v2.38 */
                 p = pchar;
                 half = FALSE; 
                  for (j = 0; j < string_len; j++,p++){
                    if ((unsigned char)*p > 0x7F){
                       half = TRUE; 
                       break;
                      }
                  }
                 if (half){
                   memset( &buff, 0, string_len * 2 );
                  j = UTF8toWideChar(pchar, string_len, NULL,(unsigned short *) &buff, string_len);
                    opndx.quoted_string->stringlen = j;
                    string_len = j;
                    sym->mem_type = MT_BYTE;      /* each byte must be stored without zeros between */
                                     /* total has to be devided by 2 for the length */
                  }
			   }
            }

            if( sym && Parse_Pass == PASS_1 && string_len > 0 ) {
                total++;
                if ( no_of_bytes == 1 && string_len > 1 ) 
				{
                    total += ( string_len - 1 );
                    sym->isarray = TRUE; /* v2.07: added */
                    if ( first ) {
                        sym->first_length = 1;
                        sym->first_size = 1;
                        first = FALSE; /* avoid to touch first_xxx fields below */
                    }
                }
				else if (no_of_bytes == 2 && string_len > 1)
				{
					sym->isarray = TRUE;
					total += (string_len - 1);
					if (first)
					{
						sym->first_length = 1;
						sym->first_size = 2;
						first = FALSE;
					}
				}
            }

            if( !inside_struct ) 
			{
                /* anything bigger than a byte must be stored in little-endian format -- LSB first */
				if (Options.masm51_compat || Options.strict_masm_compat || !Options.literal_strings)
				{
					if (string_len > 1 && no_of_bytes > 1)
						pchar = little_endian( (const char *)pchar, string_len );
					OutputDataBytes( pchar, string_len );
					if ( no_of_bytes > string_len )
						FillDataBytes(0, no_of_bytes - string_len);
				}
				else
				{
					if (string_len > 1 && no_of_bytes > 1 && sym && sym->mem_type == MT_WORD)
					{ /* FIXME */ extern int OutputInterleavedBytes();
						OutputInterleavedDataBytes(pchar, string_len);
					}
					else
					{
						if (string_len > 1 && no_of_bytes > 1)
						{
							if (half)   /* no need for little_endian, already converted, v2.38 */
								OutputDataBytes((uint_8*)&buff, j * 2);
						}
                        else 
						{
							if(no_of_bytes > 1)
								pchar = little_endian((const char *)pchar, string_len);
							OutputDataBytes(pchar, string_len);
                        }
					}
					if (no_of_bytes > string_len)
						FillDataBytes(0, no_of_bytes - string_len);
				}

				
            }
        } else {
            /* it's NOT a string */
            DebugMsg1(("data_item.CONST: const found, value=%" I32_SPEC "Xh, no_of_bytes=%u, curr_ofs=%" I32_SPEC "X\n", opndx.value, no_of_bytes, GetCurrOffset()));
            if( !inside_struct ) {
                /* the evaluator cannot handle types with size > 16.
                 * so if a (simple) type is larger ( YMMWORD? ),
                 * clear anything which is above.
                 */
                if ( no_of_bytes > 16 ) {
                    OutputDataBytes( opndx.chararray, 16 );
                    tmp = ( opndx.chararray[15] < 0x80 ? 0 : 0xFF );
                    FillDataBytes( tmp, no_of_bytes - 16 );
                } else {
                    /* v2.06: TBYTE/OWORD/XMMWORD: extend a negative value to 16-byte */
                    if ( no_of_bytes > sizeof( int_64 ) ) {
                        if ( opndx.negative && opndx.value64 < 0 && opndx.hlvalue == 0 )
                            opndx.hlvalue = -1;
                    }
                    OutputDataBytes( opndx.chararray, no_of_bytes );
                    /* check that there's no significant data left
                     * which hasn't been emitted.
                     */
                    /* v2.06: rewritten, now more rigid and checks
                     * 1-8 and 10 bytes instead of just 1-4.
                     */
                    if ( no_of_bytes <= sizeof( int_64 ) ) {
                        tmp = ( opndx.chararray[7] < 0x80 ? 0 : 0xFF );
                        memset( opndx.chararray, tmp, no_of_bytes );
                        if ( opndx.llvalue != 0 && opndx.llvalue != -1 ) {
                            DebugMsg(("data_item.CONST: error, unhandled data is %" I64_SPEC "X_%016" I64_SPEC "X\n", opndx.hlvalue, opndx.llvalue));
                            return( EmitErr( INITIALIZER_MAGNITUDE_TOO_LARGE, opndx.sym ? opndx.sym->name : "" ) );
                        }
                    } else if ( no_of_bytes == 10 ) {
                        //if ( opndx.hlvalue > 0xffff ) {
                        if ( opndx.hlvalue > 0xffff && opndx.hlvalue < -0xffff ) {
                            return( EmitErr( INITIALIZER_MAGNITUDE_TOO_LARGE, opndx.sym ? opndx.sym->name : "" ) );
                        }
                    }
                }
            }
            total++;
        }
        break;
    case EXPR_ADDR:
        /* since a fixup will be created, 8 bytes is max.
         * there's no way to define an initialized tbyte "far64" address,
         * because there's no fixup available for the selector part.
         */
        if ( no_of_bytes > sizeof(uint_64) ) {
            EmitErr( INVALID_DATA_INITIALIZER, sym ? sym->name : "" );
            break;
        }
        /* indirect addressing (incl. stack variables) is invalid */
        if ( opndx.indirect == TRUE ) {
            DebugMsg(("data_item.ADDR: error, indirect=%u, sym=%X\n", opndx.indirect, opndx.sym ));
            EmitError( INVALID_USE_OF_REGISTER );
            break;
        }
#if AMD64_SUPPORT
        if ( ModuleInfo.Ofssize != USE64 )
#endif
            if ( opndx.hvalue && ( opndx.hvalue != -1 || opndx.value >= 0 ) ) {
                /* v2.05: compared to Masm, the above is too restrictive.
                 * the line below might be better.
                 */
                //if ( opndx.hvalue != 0 && ( opndx.hvalue != -1 || opndx.value == 0 ) ) {
                DebugMsg(("data_item.ADDR: displacement doesn't fit in 32 bits: %" I64_SPEC "X\n", opndx.value64 ));
                return( EmitConstError( &opndx ) );
            }

        if ( is_float ) {
            DebugMsg(("data_item.ADDR: error, is_float=%u\n", is_float ));
            EmitError( MUST_USE_FLOAT_INITIALIZER );
            break;
        }

        total++;
        /* for STRUCT fields, don't emit anything! */
        if ( inside_struct ) {
            break;
        }

        /* determine what type of fixup is to be created */

        switch ( opndx.instr ) {
        case T_SEG:
            if ( no_of_bytes < 2 ) {
                DebugMsg(("data_item.ADDR: error, a SEG wont fit in a BYTE\n" ));
                EmitError( MAGNITUDE_TOO_LARGE_FOR_SPECIFIED_SIZE );
            }
            fixup_type = FIX_SEG;
            break;
        case T_OFFSET:
            switch ( no_of_bytes ) {
            case 1:
                /* forward reference? */
                if ( Parse_Pass == PASS_1 && opndx.sym && opndx.sym->state == SYM_UNDEFINED ) {
                    DebugMsg(("data_item.ADDR: forward reference + OFFSET operator + DB -> may become error in Pass 2\n" ));
                    fixup_type = FIX_VOID; /* v2.10: was regression in v2.09 */
                } else {
                    DebugMsg(("data_item.ADDR: error, an offset wont fit in a BYTE\n" ));
                    EmitError( OFFSET_MAGNITUDE_TOO_LARGE );
                    fixup_type = FIX_OFF8;
                }
                break;
            case 2:
                fixup_type = FIX_OFF16;
                break;
#if AMD64_SUPPORT
            case 8:
                if ( ModuleInfo.Ofssize == USE64 ) {
                    fixup_type = FIX_OFF64;
                    break;
                }
#endif
            default:
                if ( opndx.sym && ( GetSymOfssize(opndx.sym) == USE16 ) )
                    fixup_type = FIX_OFF16;
                else
                    fixup_type = FIX_OFF32;
                break;
            }
            break;
#if IMAGERELSUPP
        case T_IMAGEREL:
            if ( no_of_bytes < sizeof(uint_32) ) {
                DebugMsg(("data_item.ADDR: IMAGEREL, error, size=%u (should be 4)\n", no_of_bytes ));
                EmitError( OFFSET_MAGNITUDE_TOO_LARGE );
            }
            fixup_type = FIX_OFF32_IMGREL;
            break;
#endif
#if SECTIONRELSUPP
        case T_SECTIONREL:
            if ( no_of_bytes < sizeof(uint_32) ) {
                DebugMsg(("data_item.ADDR: SECTIONREL, error, size=%u (should be 4)\n", no_of_bytes ));
                EmitError( OFFSET_MAGNITUDE_TOO_LARGE );
            }
            fixup_type = FIX_OFF32_SECREL;
            break;
#endif
        case T_LOW:
            fixup_type = FIX_OFF8; /* OMF, BIN + GNU-ELF only */
            break;
        case T_HIGH:
            DebugMsg(("data_item.ADDR: HIGH detected\n"));
            fixup_type = FIX_HIBYTE; /* OMF only */
            break;
        case T_LOWWORD:
            fixup_type = FIX_OFF16;
            if ( no_of_bytes < 2 ) {
                EmitError( MAGNITUDE_TOO_LARGE_FOR_SPECIFIED_SIZE );
                break;
            }
            break;
#if LOHI32
        case T_HIGH32:
            /* no break */
#endif
        case T_HIGHWORD:
            fixup_type = FIX_VOID;
            EmitError( CONSTANT_EXPECTED );
            break;
#if LOHI32
        case T_LOW32:
            fixup_type = FIX_OFF32;
            if ( no_of_bytes < 4 ) {
                EmitError( MAGNITUDE_TOO_LARGE_FOR_SPECIFIED_SIZE );
                break;
            }
            break;
#endif
        default:
            /* size < 2 can work with T_LOW|T_HIGH operator only */
            if ( no_of_bytes < 2 ) {
                /* forward reference? */
                if ( Parse_Pass == PASS_1 && opndx.sym && opndx.sym->state == SYM_UNDEFINED )
                    ;
                else {
                    /* v2.08: accept 1-byte absolute externals */
                    if ( opndx.sym && opndx.sym->state == SYM_EXTERNAL && opndx.is_abs == TRUE ) {
                    } else {
                        DebugMsg(("data_item.ADDR: error, no of bytes=%u\n", no_of_bytes ));
                        EmitError( MAGNITUDE_TOO_LARGE_FOR_SPECIFIED_SIZE );
                    }
                    fixup_type = FIX_OFF8;
                    break;
                }
            }
            /* if the symbol references a segment or group,
             then generate a segment fixup.
             */
            if ( opndx.sym && (opndx.sym->state == SYM_SEG || opndx.sym->state == SYM_GRP ) ) {
                fixup_type = FIX_SEG;
                break;
            }

            switch ( no_of_bytes ) {
            case 2:
                /* accept "near16" override, else complain
                 * if symbol's offset is 32bit */
                /* v2.06: if condition changed */
                //if ( opndx.explicit == TRUE && opndx.mem_type == MT_NEAR && opndx.Ofssize == USE16 )
                if ( opndx.explicit == TRUE ) {
                    if ( SizeFromMemtype( opndx.mem_type, opndx.Ofssize, opndx.type ) > no_of_bytes ) {
                        DebugMsg(("data_item.ADDR: error, memtype %X wont fit in a WORD\n", opndx.mem_type));
                        EmitErr( INITIALIZER_MAGNITUDE_TOO_LARGE, opndx.sym ? opndx.sym->name : "" );
                    };
                } else if ( opndx.sym && opndx.sym->state == SYM_EXTERNAL && opndx.is_abs == TRUE ) {
                    /* v2.07a: accept ABSolute externals (regression in v2.07) */
                } else if ( opndx.sym &&
                           opndx.sym->state != SYM_UNDEFINED &&
                           ( GetSymOfssize(opndx.sym) > USE16 ) ) {
                    DebugMsg(("data_item.ADDR: error, a 32bit offset (%s) wont fit in a WORD\n", opndx.sym->name));
                    EmitErr( INITIALIZER_MAGNITUDE_TOO_LARGE, opndx.sym ? opndx.sym->name : "" );
                }
                fixup_type = FIX_OFF16;
                break;
            case 4:
                /* masm generates:
                 * off32 if curr segment is 32bit,
                 * ptr16 if curr segment is 16bit,
                 * and ignores type overrides.
                 * if it's a NEAR external, size is 16, and
                 * format isn't OMF, error 'symbol type conflict'
                 * is displayed
                 */
                if ( opndx.explicit == TRUE ) {
                    if ( opndx.mem_type == MT_FAR ) {
                        if ( opndx.Ofssize != USE_EMPTY && opndx.Ofssize != USE16 ) {
                            DebugMsg(("data_item.ADDR: error, FAR32 won't fit in a DWORD\n" ));
                            EmitErr( INITIALIZER_MAGNITUDE_TOO_LARGE, opndx.sym ? opndx.sym->name : "" );
                        }
                        fixup_type = FIX_PTR16;
                    } else if ( opndx.mem_type == MT_NEAR ) {
                        if ( opndx.Ofssize == USE16 )
                            fixup_type = FIX_OFF16;
                        else if ( opndx.sym && ( GetSymOfssize( opndx.sym ) == USE16 ) )
                            fixup_type = FIX_OFF16;
                        else
                            fixup_type = FIX_OFF32;
                    }
                } else {
                    /* what's done if code size is 16 is Masm-compatible.
                     * It's not very smart, however.
                     * A better strategy is to choose fixup type depending
                     * on the symbol's offset size.
                     */
                    //if ( opndx.sym && ( GetSymOfssize( opndx.sym ) == USE16 ) )
                    if ( ModuleInfo.Ofssize == USE16 )
#if COFF_SUPPORT || ELF_SUPPORT
                        if ( opndx.mem_type == MT_NEAR &&
                            ( Options.output_format == OFORMAT_COFF
#if ELF_SUPPORT
                             || Options.output_format == OFORMAT_ELF
#endif
                            )) {
                            fixup_type = FIX_OFF16;
                            EmitErr( SYMBOL_TYPE_CONFLICT, sym->name );
                        } else
#endif
                            fixup_type = FIX_PTR16;
                    else
                        fixup_type = FIX_OFF32;
                }
                break;
            case 6:
                /* Masm generates a PTR32 fixup in OMF!
                 * and a DIR32 fixup in COFF.
                 */
                /* COFF/ELF has no far fixups */
#if COFF_SUPPORT || ELF_SUPPORT
                if ( Options.output_format == OFORMAT_COFF
#if ELF_SUPPORT
                    || Options.output_format == OFORMAT_ELF
#endif
                   ) {
                    fixup_type = FIX_OFF32;
                } else
#endif
                    fixup_type = FIX_PTR32;
                break;
            default:
                /* Masm generates
                 * off32 if curr segment is 32bit
                 * ptr16 if curr segment is 16bit
                 * Uasm additionally accepts a FAR32 PTR override
                 * and generates a ptr32 fixup then */
                if ( opndx.explicit == TRUE && opndx.mem_type == MT_FAR && opndx.Ofssize == USE32 )
                    fixup_type = FIX_PTR32;
                else if( ModuleInfo.Ofssize == USE32 )
                    fixup_type = FIX_OFF32;
#if AMD64_SUPPORT
                else if( ModuleInfo.Ofssize == USE64 )
                    fixup_type = FIX_OFF64;
#endif
                else
                    fixup_type = FIX_PTR16;
            }
            break;
        } /* end switch ( opndx.instr ) */

        /* v2.07: fixup type check moved here */
        if ( ( 1 << fixup_type ) & ModuleInfo.fmtopt->invalid_fixup_type ) {
            return( EmitErr( UNSUPPORTED_FIXUP_TYPE,
                    ModuleInfo.fmtopt->formatname,
                    opndx.sym ? opndx.sym->name : szNull ) );
        }
        fixup = NULL;
        if ( write_to_file ) {
            /* there might be a segment override:
             * a segment, a group or a segment register.
             * Init var SegOverride, it's used inside set_frame()
             */
            SegOverride = NULL;
            segm_override( &opndx, NULL );

            /* set global vars Frame and Frame_Datum */
            /* opndx.sym may be NULL, then SegOverride is set. */
            if ( ModuleInfo.offsettype == OT_SEGMENT &&
                ( opndx.instr == T_OFFSET || opndx.instr == T_SEG ))
                set_frame2( opndx.sym );
            else
                set_frame( opndx.sym );
            /* uses Frame and Frame_Datum  */
            fixup = CreateFixup( opndx.sym, fixup_type, OPTJ_NONE );
            //store_fixup( fixup, &opndx.value ); /* may fail, but ignore error! */
        }
        OutputBytes( (unsigned char *)&opndx.value, no_of_bytes, fixup );
        break;
    case EXPR_REG:
        EmitError( INVALID_USE_OF_REGISTER );
        break;
    default: /* unknown opndx.kind, shouldn't happen */
        DebugMsg(("data_item: error, opndx.kind=%u\n", opndx.kind ));
        return( EmitError( SYNTAX_ERROR ) );
    } /* end switch (opndx.kind) */
item_done:
    if( sym && first && Parse_Pass == PASS_1 ) {
        sym->first_length = total;
        sym->first_size = total * no_of_bytes;
    }
    if( i < end && tokenarray[i].token == T_COMMA ) {
        i++;
        if ( tokenarray[i].token != T_FINAL &&
            tokenarray[i].token != T_CL_BRACKET ) {
            first = FALSE;
            if ( sym )
                sym->isarray = TRUE;
            goto next_item;
        }
    }

    } /* end for */

    if( sym && Parse_Pass == PASS_1 ) {
     sym->total_length += total; /* we need to know a proper length of characters */
     sym->total_size += total * no_of_bytes; /* total_size is needed for proper storing the data  */
    }

    *start_pos = i;
    DebugMsg1(("data_item: exit, no error, i=%d\n", i));
    return( NOT_ERROR );
}

static ret_code checktypes( const struct asym *sym, enum memtype mem_type, const struct asym *type_sym )
/******************************************************************************************************/
{
    /* for EXTERNDEF, check type changes */
    if ( sym->mem_type != MT_EMPTY ) {
        enum memtype mem_type2 = sym->mem_type;
        const struct asym *tmp;
        /* skip alias types */
        tmp = type_sym;
        while ( mem_type == MT_TYPE ) {
            mem_type = tmp->mem_type;
            tmp = tmp->type;
        }
        tmp = sym;
        while ( mem_type2 == MT_TYPE ) {
            mem_type2 = tmp->mem_type;
            tmp = tmp->type;
        }
        if ( mem_type2 != mem_type ) {
            DebugMsg(("checktypes: memtype conflict: %u - %u\n", mem_type2, mem_type ));
            return( EmitErr( SYMBOL_TYPE_CONFLICT, sym->name ) );
        }
    }
    return( NOT_ERROR );
}
/*
 * parse data definition line. Syntax:
 * [label] directive|simple type|arbitrary type initializer [,...]
 * - directive: DB, DW, ...
 * - simple type: BYTE, WORD, ...
 * - arbitrary type: struct, union, typedef or record name
 * arguments:
 * i: index of directive or type
 * type_sym: userdef type or NULL
 */

ret_code data_dir( int i, struct asm_tok tokenarray[], struct asym *type_sym )
/****************************************************************************/
{
    uint_32             no_of_bytes;
    struct asym         *sym = NULL;
    uint_32             old_offset;
    uint_32             currofs; /* for LST output */
    enum memtype        mem_type;
    bool                is_float = FALSE;
    int                 idx;
    char const               *name;

	struct dsym *symtype = ((struct dsym *)type_sym);
	struct sfield   *f;
	struct sfield   *f2;
	struct sfield   *fPrev;
	bool foundSubType = FALSE;
	uint_32 subid = 0;
	uint_32 subcnt = 0;
	uint_32 k = 0;

    DebugMsg1(("data_dir( i=%u, type=%s ) enter\n", i, type_sym ? type_sym->name : "NULL" ));

    /* v2.05: the previous test in parser.c wasn't fool-proofed */
    if ( i > 1 && ModuleInfo.m510 == FALSE ) {
        return( EmitErr( SYNTAX_ERROR_EX, tokenarray[i].string_ptr ) );
    }
    if( tokenarray[i+1].token == T_FINAL ) {
        DebugMsg(("data_dir: missing initializer\n"));
        return( EmitErr( SYNTAX_ERROR_EX, tokenarray[i].tokpos ) );
    }

    /* set values for mem_type and no_of_bytes */
    if ( type_sym ) {
        /* if the parser found a TYPE id, type_sym is != NULL */
        //DebugMsg1(("data_dir: arbitrary type %s, calling SymSearch\n", type_sym->name ));
        //type_sym = SymSearch( tokenarray[i].string_ptr );
        mem_type = MT_TYPE;
        if ( type_sym->typekind != TYPE_TYPEDEF &&
             ( type_sym->total_size == 0 || ((struct dsym *)type_sym)->e.structinfo->OrgInside == TRUE ) ) {
            return( EmitError( STRUCT_CANNOT_BE_INSTANCED ) );
        }

        /* v2.09: expand literals inside <> or {}.
         * Previously this was done inside InitStructuredVar()
         */
#if FASTPASS
        if ( Parse_Pass == PASS_1 || UseSavedState == FALSE )
#endif
            ExpandLiterals( i+1, tokenarray );

        no_of_bytes = type_sym->total_size;
        if ( no_of_bytes == 0 ) {
            DebugMsg(("data_dir: size of arbitrary type is 0!\n"));
            /* a void type is not valid */
            if ( type_sym->typekind == TYPE_TYPEDEF ) {
                return( EmitErr( INVALID_TYPE_FOR_DATA_DECLARATION, type_sym->name ) );
            }
        }
    } else {
        /* it's either a predefined type or a data directive. For types, the index
         into the simpletype table is in <bytval>, for data directives
         the index is found in <sflags>.
         * v2.06: SimpleType is obsolete. Use token index directly!
         */

        if ( tokenarray[i].token == T_STYPE ) {
            //idx = tokenarray[i].bytval;
            idx = tokenarray[i].tokval;
        } else if ( tokenarray[i].token == T_DIRECTIVE &&
                   ( tokenarray[i].dirtype == DRT_DATADIR )) {
            idx = GetSflagsSp( tokenarray[i].tokval );
        } else {
            return( EmitErr( INVALID_TYPE_FOR_DATA_DECLARATION, tokenarray[i].string_ptr ) );
        }
        mem_type = GetMemtypeSp( idx );
        /* types NEAR[16|32], FAR[16|32] and PROC are invalid here */
        //if ( ( SimpleType[idx].mem_type & MT_SPECIAL_MASK ) == MT_ADDRESS ) {
        if ( ( mem_type & MT_SPECIAL_MASK ) == MT_ADDRESS ) {
            return( EmitErr( SYNTAX_ERROR_EX, tokenarray[i].string_ptr ) );
        }
        no_of_bytes = (mem_type & MT_SIZE_MASK) + 1;
        if ( mem_type & MT_FLOAT )
            is_float = TRUE;
    }

    /* if i == 1, there's a (data) label at pos 0.
     * (note: if -Zm is set, a code label may be at pos 0, and
     * i is 2 then.)
     */
    name = ( ( i == 1 ) ? tokenarray[0].string_ptr : NULL );

    /* in a struct declaration? */
    if( CurrStruct ) {

        /* structure parsing is done in the first pass only */
        if( Parse_Pass == PASS_1 ) {

            /* current offset isn't necessarily the fields start offset */
            //currofs = CurrStruct->sym.offset;

            if (!(sym = CreateStructField( i, tokenarray, name, mem_type, type_sym, no_of_bytes ))) {
                return ( ERROR );
            }
#if FASTPASS
            if ( StoreState ) FStoreLine(0);
#endif
            currofs = sym->offset;
            sym->isdata = TRUE; /* 'first_size' is valid */
            DebugMsg1(("data_dir: %s, CreateStructField called, ofs=%d\n", sym->name, sym->offset ));
        } else { /* v2.04: else branch added */
            sym = &CurrStruct->e.structinfo->tail->sym;
            currofs = sym->offset;
            CurrStruct->e.structinfo->tail = CurrStruct->e.structinfo->tail->next;
        }

    } else {

        if( CurrSeg == NULL ) {
            return( EmitError( MUST_BE_IN_SEGMENT_BLOCK ) );
        }

        FStoreLine(0);

        if ( ModuleInfo.CommentDataInCode )
            omf_OutSelect( TRUE );

        if ( ModuleInfo.list ) {
            currofs = GetCurrOffset();
        }

        /* is a label accociated with the data definition? */
        if( name ) {
            /* get/create the label. */
            DebugMsg1(("data_dir: calling SymLookup(%s)\n", name ));
            sym = SymLookup( name );
            /* v2.11: cannot fail */
            //if( sym == NULL ) {
            //    DebugMsg(("data_dir exit, error: invalid label name\n"));
            //    return( ERROR );
            //}

            if( Parse_Pass == PASS_1 ) {

                if ( sym->state == SYM_EXTERNAL && sym->weak == TRUE && sym->isproc == FALSE ) { /* EXTERNDEF? */

                    //if ( checktypes( sym, mem_type, type_sym ) == ERROR )
                    //    return( ERROR );
                    /* v2.0: display error and continue! */
                    checktypes( sym, mem_type, type_sym );
                    /* remove the external info */
                    sym_ext2int( sym );
                    sym->total_size = 0;
                    sym->total_length = 0;
                    sym->first_length = 0;

                } else if( sym->state == SYM_UNDEFINED ) {

                    sym_remove_table( &SymTables[TAB_UNDEF], (struct dsym *)sym );
                    sym->state = SYM_INTERNAL;
#if 1
                    /* v2.11: Set the symbol's langtype. It may have been set
                     * by a PUBLIC directive, so take care not to overwrite it.
                     * Problem: Masm doesn't do this - might be a bug.
                     */
                    if ( sym->langtype == LANG_NONE )
                        sym->langtype = ModuleInfo.langtype;
#endif
                } else if ( sym->state == SYM_INTERNAL) {

                    /* accept a symbol "redefinition" if addresses and types
                     * do match.
                     */
                    if ( sym->segment != (struct asym *)CurrSeg ||
                        sym->offset != GetCurrOffset() ) {
                        return( EmitErr( SYMBOL_ALREADY_DEFINED, name ) );
                    }
                    /* check for symbol type conflict */
                    if ( checktypes( sym, mem_type, type_sym ) == ERROR )
                        return( ERROR );
                    /* v2.09: reset size and length ( might have been set by LABEL directive ) */
                    sym->total_size = 0;
                    sym->total_length = 0;
                    goto label_defined; /* don't relink the label */

                } else {
                    DebugMsg(("data_dir(%s): error, symbol redefinition, state=%X\n", sym->name, sym->state ));
                    return( EmitErr( SYMBOL_REDEFINITION, sym->name ) );
                }
                /* add the label to the linked list attached to curr segment */
                /* this allows to reduce the number of passes (see Fixup.c) */
                ((struct dsym *)sym)->next = (struct dsym *)CurrSeg->e.seginfo->label_list;
                CurrSeg->e.seginfo->label_list = sym;

            } else {
                old_offset = sym->offset;
            }
        label_defined:
            SetSymSegOfs( sym );
            if( Parse_Pass != PASS_1 && sym->offset != old_offset ) {
#ifdef DEBUG_OUT
                if ( !ModuleInfo.PhaseError )
                    DebugMsg(("data_dir: Phase error, pass %u, sym >%s< first time, new=%X != old=%X\n", Parse_Pass+1, sym->name, sym->offset, old_offset));
#endif
                ModuleInfo.PhaseError = TRUE;
            }
            sym->isdefined = TRUE;
            sym->isdata = TRUE; /* 'first_size' is valid */
            sym->mem_type = mem_type;
            sym->type = type_sym;

            /* backpatch for data items? Yes, if the item is defined
             * in a code segment then its offset may change!
             */
            BackPatch( sym );
        }

        if ( type_sym ) {
            while ( type_sym->mem_type == MT_TYPE )
                type_sym = type_sym->type;
            /* if it is just a type alias, skip the arbitrary type */
            if ( type_sym->typekind == TYPE_TYPEDEF )
                type_sym = NULL;
        }

    }

    i++;

	/* Enhanced Uasm 2.22+ Union initialize */
	if (tokenarray[i].token == T_DOT && type_sym->state == SYM_TYPE)
	{
		i++; // skip dot.
		// Check sub type.
		symtype = ((struct dsym *)type_sym);
		foundSubType = FALSE;
		subid = 0;
		subcnt = 0;
		k = 0;
		for (f = symtype->e.structinfo->head; f != NULL; f = f->next) 
		{
			if (strcmp(tokenarray[i].string_ptr, f->sym.name) == 0)
			{
				f2 = f;
				foundSubType = TRUE;
				subid = subcnt;
				fPrev = symtype->e.structinfo->head;
			}
			subcnt++;
		}
		if (!foundSubType)
		{
			return(EmitErr(INVALID_DATA_INITIALIZER, sym->name));
		}
		else
		{
			// sort the union subtypes so the one we want is first (in accordance with what is allowed for initialisation)
			if (f2 == symtype->e.structinfo->head)
			{
				// Do nothing as the required sub type is the first one.
			}
			else
			{
				symtype->e.structinfo->head = f2;
			}
			
		}
		i++; /* skip subtype identifier */
	}

    if ( data_item( &i, tokenarray, sym, no_of_bytes, type_sym, 1, CurrStruct != NULL, is_float, TRUE, Token_Count ) == ERROR ) {
        DebugMsg(("data_dir: error in data_item()\n"));
        return( ERROR );
    }

	if (foundSubType)
		symtype->e.structinfo->head = fPrev;

    if ( tokenarray[i].token != T_FINAL ) {
        return( EmitErr( SYNTAX_ERROR_EX, tokenarray[i].tokpos ) );
    }

    /* v2.06: update struct size after ALL items have been processed */
    if ( CurrStruct )
        UpdateStructSize( sym );

    if ( ModuleInfo.list )
        LstWrite( CurrStruct ? LSTTYPE_STRUCT : LSTTYPE_DATA, currofs, sym );

    DebugMsg1(("data_dir: exit, no error, label=%s, is_array=%u Curr%s.ofs=%X\n",
               sym ? sym->name : "NULL",
               sym ? sym->isarray : 0,
               CurrStruct ? "Struct" : "Seg",
               CurrStruct ? CurrStruct->sym.offset : CurrSeg->sym.offset ));
    return( NOT_ERROR );
}

