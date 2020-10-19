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
* Description:  prototypes for tokenizer
*
****************************************************************************/


#ifndef _TOKENIZE_H_INCLUDED
#define _TOKENIZE_H_INCLUDED

struct line_status {
    char *input;
    char *output;   /* free space in token string buffer */
    char *start;    /* start of line */
    unsigned int index; /* index token array */
    //char last_token;
    char flags;     /* v2.08: added */
    char flags2;    /* v2.08: was global var g_flags */
    char flags3;    /* v2.08: added */
};

enum tokenize_flags {
    TOK_DEFAULT      = 0x00,  /* default mode - handle conditional assembly */
    TOK_RESCAN       = 0x01,  /* retokenize after expansion - ignore conditional assembly */
    TOK_NOCURLBRACES = 0x02,  /* don't handle {}-literals */
    TOK_LINE         = 0x04,  /* full line is retokenized */
};

enum tok_flags3 {
    TF3_ISCONCAT  = 0x01,  /* line was concatenated */
    TF3_EXPANSION = 0x02,  /* expansion operator % at pos 0 */
};

extern ret_code GetToken( struct asm_tok[], struct line_status * );
extern int      Tokenize( char const [], unsigned int, struct asm_tok[], unsigned int );

#endif
