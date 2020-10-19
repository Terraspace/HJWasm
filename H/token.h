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
* Description:  defines token types and struct asm_tok.
*               This file is included by parser.h
*
****************************************************************************/


#ifndef _TOKEN_H_
#define _TOKEN_H_

/* T_INSTRUCTION - T_RES_ID are reserved words.
 * T_REG - T_RES_ID match RWT_REG - RWT_RES_ID
 */

enum tok_type {
    T_FINAL,
    T_INSTRUCTION,       /* 1 */
    T_REG,               /* 2 (==RWT_REG) */
    T_DIRECTIVE,         /* 3 (==RWT_DIRECTIVE) */
    T_UNARY_OPERATOR,    /* 4 (==RWT_UNARY_OP) */
    T_BINARY_OPERATOR,   /* 5 (==RWT_BINARY_OP) */
    T_STYPE,             /* 6 (==RWT_STYPE) */
    T_RES_ID,            /* 7 (==RWT_RES_ID) */
    T_ID,                /* 8 */
    T_STRING,            /* 9 */
    T_NUM,               /* A */
    T_FLOAT,             /* B */
    T_BAD_NUM,           /* C */
    T_DBL_COLON,         /* D */
	T_POINTER,           /* E */

    T_OP_BRACKET    = '(',
    T_OP_SQ_BRACKET = '[',
    T_CL_BRACKET    = ')',
    T_CL_SQ_BRACKET = ']',
    T_COMMA         = ',',
    T_COLON         = ':',
    T_DOT           = '.',
    T_QUESTION_MARK = '?',
    T_PERCENT       = '%',
};

struct asm_tok {
#ifdef __WATCOMC__
    enum tok_type token;           /* (type of) token */
#else
    unsigned char token;
#endif
    union {
        unsigned char dirtype;    /* T_DIRECTIVE: type */
        unsigned char bytval;     /* T_STYPE: memtype */
        unsigned char precedence; /* T_UNARY_OPERATOR/T_BINARY_OPERATOR */
        char string_delim;        /* T_STRING: string delimiter */
        char floattype;           /* T_FLOAT: 0 or 'r' */
        char numbase;             /* T_NUM: number base */
        char specval;             /* 1-byte special tokens: flags */
    };
    char const *string_ptr;
    union {
        unsigned int tokval;      /* index if token is a reserved word */
        unsigned int stringlen;   /* T_STRING: size */
        unsigned int idarg;       /* T_ID: currently not used */
        unsigned int itemlen;     /* T_NUM: size */
        unsigned int lastidx;     /* T_FINAL: last index (used in RunMacro()) */
    };
    char *tokpos;                 /* points to item in CurrSource */
};

#endif
