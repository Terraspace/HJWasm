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
* Description:  Memory allocation prototypes.
*
****************************************************************************/

#ifndef _MEMALLOC_H_
#define _MEMALLOC_H_

extern void MemInit( void );
extern void MemFini( void );
extern void *MemAlloc( size_t size );
extern void *MemRealloc( void *ptr, size_t size );
extern void MemFree( void *ptr );

#if defined(__WATCOMC__) || defined(__BORLANDC__) || defined(__OCC__)

	#define alloca(x)  __builtin_alloca(x)

#define myalloca  alloca
#include <malloc.h>

#elif (defined(__GNUC__) || defined(__TINYC__)) && ! defined (alloca)

	#define alloca(x)  __builtin_alloca(x)

#define myalloca  alloca
#ifndef __FreeBSD__  /* added v2.08 */
#include <malloc.h>  /* added v2.07 */
#endif

#elif defined(__PCC__)

#define myalloca  _alloca
#include <malloc.h>

#else

#define myalloca  _alloca

#endif

/* LclAlloc() and LclFree() are fast variants, which
 * are to be used for all allocations which aren't "global"
 */
extern  void    *LclAlloc( size_t );
#if FASTMEM
/* be careful not to use a function call as argument for LclFree()! */
#define LclFree( p ) ;
#else
extern  void    LclFree( void * );
#endif

#endif
