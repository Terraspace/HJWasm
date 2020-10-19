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
* Description:  Assembler message output interface.
*               This file is included by globals.h.
*
****************************************************************************/


#ifndef _ERRMSG_H_INCLUDED
#define _ERRMSG_H_INCLUDED

/* error IDs */
#undef pick
#define pick( code, text )  code,
enum msgno {
#include "msgdef.h"
    MSG_LAST
};
#undef pick

#ifdef DEBUG_OUT
    extern void DoDebugMsg( const char *format, ... );
    extern void DoDebugMsg1( const char *format, ... );
    #define DebugMsg( x ) DoDebugMsg x
    #define DebugMsg1( x ) DoDebugMsg1 x
    #define DebugCmd( x ) x
#else
    #define DebugMsg( x )
    #define DebugMsg1( x )
    #define DebugCmd( x )
#endif

#if defined( __UNIX__ ) || defined ( __SW_BD )
#define errout stderr
#else
#define errout stdout
#endif

extern void Fatal( int msgnum, ... );
extern int  EmitError( int msgnum );
extern int  EmitErr( int msgnum, ... );
extern void EmitWarn( int level, int msgnum, ... );
extern void PrintNote( int msgnum, ... );
extern char const *ErrnoStr( void );
extern int  write_logo( void );
extern void PrintUsage( void );
//extern void SeekError( void );
extern void WriteError( void );

#endif
