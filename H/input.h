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
* Description:  prototypes for input queueing/processing procedures
*
****************************************************************************/

#ifndef _INPUT_H_INCLUDED
#define _INPUT_H_INCLUDED

struct macro_instance {
    struct srcline *currline;
    struct srcline *startline;
    uint_32 localstart;
    char const * *parm_array;
    struct asym *macro;
    unsigned parmcnt;
};

/* for line numbers, the source files have to be stored
 * in a list in the very same order as they appear in
 * the input stream.
 */
struct file_seq {
    struct file_seq *next;
    uint_16         file;  /* index of file in FNames */
};

struct input_status {
    char *token_stringbuf;
    char *currsource;
    char *CurrComment;
    int token_count;
    char line_flags;/* v2.08: added */
#ifdef __I86__
    char *stringbufferend;
    struct asm_tok *tokenarray;
#endif
};

extern uint_32  GetLineNumber( void );
//#define LineNumber GetLineNumber()

extern const char *GetFNamePart( const char *fname );
extern char     *GetExtPart( const char *fname );

extern struct src_item *PushSrcItem(char type, void *pv);

extern FILE     *SearchFile( const char *path, bool );
extern char     *GetTextLine( char *buffer );
extern void     PushMacro( struct macro_instance * );
extern void     SetLineNumber( unsigned );
#if FASTMEM==0
extern bool     MacroInUse( struct dsym * );
#endif
extern void     AddStringToIncludePath( const char *string );
extern void     InputInit( void );
extern void     InputPassInit( void );
extern void     InputFini( void );
extern struct asm_tok *PushInputStatus( struct input_status * );
extern void     PopInputStatus( struct input_status * );
extern int      GetCurrSrcPos( char * );
extern void     ClearSrcStack( void );
extern unsigned get_curr_srcfile( void );
#if FASTPASS
extern void     set_curr_srcfile( unsigned, uint_32 );
#endif
extern const struct fname_item *GetFName( unsigned );
#ifdef DEBUG_OUT
extern char     *GetTopLine( char * );
extern char     *GetTopSrcName( void );
#endif

#define GetAlignedPointer( x, size ) ( x + ( ( size + 1 + sizeof(void *) - 1 ) & ~( sizeof(void *) - 1 ) ) )

#endif
