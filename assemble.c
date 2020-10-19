/****************************************************************************
*
*  This code is Public Domain.
*
*  ========================================================================
*
* Description:  assemble a module.
*
****************************************************************************/

#ifdef __GNUC__
	#define _BITS_FLOATN_COMMON_H
#endif

#include <ctype.h>
#include <time.h>

#include "globals.h"
#include "memalloc.h"
#include "input.h"
#include "parser.h"
#include "reswords.h"
#include "tokenize.h"
#include "condasm.h"
#include "segment.h"
#include "assume.h"
#include "proc.h"
#include "expreval.h"
#include "hll.h"
#include "context.h"
#include "types.h"
#include "label.h"
#include "macro.h"
#include "extern.h"
#include "fixup.h"
#include "omf.h"
#include "fastpass.h"
#include "listing.h"
#include "msgtext.h"
#include "myassert.h"
#include "linnum.h"
#include "cpumodel.h"
#include "lqueue.h"
#include "orgfixup.h"
#include "macrolib.h"
//#include "simd.h"

#if DLLIMPORT
#include "mangle.h"
#endif

#if COFF_SUPPORT
#include "coff.h"
#endif
#if ELF_SUPPORT
#include "elf.h"
#endif
#if BIN_SUPPORT
#include "bin.h"
#endif
#if MACHO_SUPPORT
#include "macho64.h"
#endif

#if 1 //def __SW_BD
#include <setjmp.h>
jmp_buf jmpenv;
#endif

#ifdef __SW_BD
#define EXPQUAL __stdcall
#else
#define EXPQUAL
#endif

#define USELSLINE 1 /* must match switch in listing.c! */

//#define ASM_EXT "asm"
#ifdef __UNIX__
#define OBJ_EXT "o"
#else
#define OBJ_EXT "obj"
#endif
#define LST_EXT "lst"
#define ERR_EXT "err"
#define BIN_EXT "BIN"
#define EXE_EXT "EXE"
extern int_32           LastCodeBufSize;
extern char             *DefaultDir[NUM_FILE_TYPES];
extern const char       *ModelToken[];
#if FASTMEM==0
extern void             FreeLibQueue();
#endif
#include "Colors.h"
#ifdef _WIN32
#include "winconsole.h"
#endif

/* parameters for output formats. order must match enum oformat */
static const struct format_options formatoptions[] = {
#if BIN_SUPPORT
    { bin_init,  BIN_DISALLOWED,    "BIN"  },
#endif
    { omf_init,  OMF_DISALLOWED,    "OMF"  },
#if COFF_SUPPORT
    { coff_init, COFF32_DISALLOWED, "COFF" },
#endif
#if ELF_SUPPORT
    { elf_init,  ELF32_DISALLOWED,  "ELF"  },
#endif
#if MACHO_SUPPORT
	{ macho_init,  MACHO32_DISALLOWED,  "MACHO" },
#endif
};

struct module_info      ModuleInfo;
unsigned int            Parse_Pass;     /* assembly pass */
//unsigned int            GeneratedCode; /* v2.10: moved to ModuleInfo */
struct qdesc            LinnumQueue;    /* queue of line_num_info items */

bool write_to_file;     /* write object module */

#if 0
/* for OW, it would be good to remove the CharUpperA() emulation
 * implemented in apiemu.c. Unfortunately, OW isn't happy with
 * a local, simple version of _strupr() - it still wants to
 * import CharUpperA.
 */
char * _strupr( char *src )
{
    char *dst;
    for ( dst = src; *dst; dst++ )
        if ( *dst >= 'a' && *dst <= 'z' )
            *dst &= ~0x20;
    return( src );
}
#endif

#if COFF_SUPPORT || PE_SUPPORT

/* struct to help convert section names in COFF, ELF, PE */
struct conv_section {
    uint_8 len;
    uint_8 flags; /* see below */
    const char *src;
    const char *dst;
};
enum cvs_flags {
    CSF_GRPCHK = 1
};

enum conv_section_index {
    CSI_TEXT = 0,
    CSI_DATA,
    CSI_CONST,
    CSI_BSS
};

/* order must match enum conv_section_index above */
static const struct conv_section cst[] = {
    { 5, CSF_GRPCHK, "_TEXT", ".text"  },
    { 5, CSF_GRPCHK, "_DATA", ".data"  },
    { 5, CSF_GRPCHK, "CONST", ".rdata" },
    { 4, 0,          "_BSS",  ".bss"   }
};

/* order must match enum conv_section_index above */
static const enum seg_type stt[] = {
    SEGTYPE_CODE, SEGTYPE_DATA, SEGTYPE_DATA, SEGTYPE_BSS
};

static void CheckBOM(FILE *f)
{
	unsigned long bom;
	fread(&bom, 3, 1, f);
	if ((bom & 0xFFFFFF) != 0xBFBBEF)
		rewind(f);
}

extern void RewindToWin64() 
{

	if (!(Options.output_format == OFORMAT_BIN && Options.sub_format == SFORMAT_NONE))
	{
		if (Options.output_format != OFORMAT_BIN)
			Options.output_format = OFORMAT_COFF;
		else
			Options.langtype = LANG_FASTCALL;
		Options.sub_format = SFORMAT_64BIT;

	}
}

/*
 * translate section names (COFF+PE):
 * _TEXT -> .text
 * _DATA -> .data
 * CONST -> .rdata
 * _BSS  -> .bss
 */

char *ConvertSectionName( const struct asym *sym, enum seg_type *pst, char *buffer )
/**********************************************************************************/
{
    int i;

    for ( i = 0; i < sizeof( cst ) / sizeof( cst[0] ); i++ ) {
        if ( memcmp( sym->name, cst[i].src, cst[i].len ) == 0 ) {
            if ( sym->name[cst[i].len] == NULLC || ( sym->name[cst[i].len] == '$' && ( cst[i].flags & CSF_GRPCHK ) ) ) {

                if ( pst ) {
                    if ( i == CSI_BSS && ( (struct dsym *)sym)->e.seginfo->bytes_written != 0 )
                        ; /* don't set segment type to BSS if the segment contains initialized data */
                    else
                        *pst = stt[i];
                }

                if ( sym->name[cst[i].len] == NULLC ) {
#if DJGPP_SUPPORT
                    /* DJGPP won't be happy with .rdata segment name */
                    if( ModuleInfo.sub_format == SFORMAT_DJGPP && i == CSI_CONST )
                        return( ".const" );
#endif
                    return( (char *)cst[i].dst );
                }

                strcpy( buffer, cst[i].dst );
                strcat( buffer, sym->name+cst[i].len );
                return( buffer );
            }
        }
    }
    return( sym->name );
}
#endif

/* Write a byte to the segment buffer.
 * in OMF, the segment buffer is flushed when the max. record size is reached.
 */
void OutputByte( unsigned char byte )
/***********************************/
{
    if( write_to_file == TRUE ) {
        uint_32 idx = CurrSeg->e.seginfo->current_loc - CurrSeg->e.seginfo->start_loc;
#ifdef DEBUG_OUT
        if ( CurrSeg->e.seginfo->current_loc < CurrSeg->e.seginfo->start_loc ) {
            ;//_asm int 3;
        }
#endif
        /**/myassert( CurrSeg->e.seginfo->current_loc >= CurrSeg->e.seginfo->start_loc );
        if( Options.output_format == OFORMAT_OMF && idx >= MAX_LEDATA_THRESHOLD ) {
            omf_FlushCurrSeg();
            idx = CurrSeg->e.seginfo->current_loc - CurrSeg->e.seginfo->start_loc;
        }
        //DebugMsg(("OutputByte: buff=%p, idx=%" I32_SPEC "X, byte=%X, codebuff[0]=%X\n", CurrSeg->e.seginfo->CodeBuffer, idx, byte, *CurrSeg->e.seginfo->CodeBuffer ));
        CurrSeg->e.seginfo->CodeBuffer[idx] = byte;
    }
#if 1
    /* check this in pass 1 only */
    else if( CurrSeg->e.seginfo->current_loc < CurrSeg->e.seginfo->start_loc ) {
        DebugMsg(("OutputByte: segment start loc changed from %" I32_SPEC "Xh to %" I32_SPEC "Xh\n",
                  CurrSeg->e.seginfo->start_loc,
                  CurrSeg->e.seginfo->current_loc));
        CurrSeg->e.seginfo->start_loc = CurrSeg->e.seginfo->current_loc;
    }
#endif
    CurrSeg->e.seginfo->current_loc++;
    CurrSeg->e.seginfo->bytes_written++;
    CurrSeg->e.seginfo->written = TRUE;
    if( CurrSeg->e.seginfo->current_loc > CurrSeg->sym.max_offset )
        CurrSeg->sym.max_offset = CurrSeg->e.seginfo->current_loc;
}

/* Added 2.14: to accelerate bulk writing of incbin data */
void OutputBinBytes(unsigned char* pBytes, uint_32 len)
{
	int i;

	if (write_to_file == TRUE) {
		uint_32 idx = CurrSeg->e.seginfo->current_loc - CurrSeg->e.seginfo->start_loc;
#ifdef DEBUG_OUT
		if (CurrSeg->e.seginfo->current_loc < CurrSeg->e.seginfo->start_loc) {
			;//_asm int 3;
		}
#endif
		/**/myassert(CurrSeg->e.seginfo->current_loc >= CurrSeg->e.seginfo->start_loc);
		if (Options.output_format == OFORMAT_OMF && idx >= MAX_LEDATA_THRESHOLD) {
			omf_FlushCurrSeg();
			idx = CurrSeg->e.seginfo->current_loc - CurrSeg->e.seginfo->start_loc;
		}
		//DebugMsg(("OutputByte: buff=%p, idx=%" I32_SPEC "X, byte=%X, codebuff[0]=%X\n", CurrSeg->e.seginfo->CodeBuffer, idx, byte, *CurrSeg->e.seginfo->CodeBuffer ));
		for (i = 0; i < len; i++)
		{
			CurrSeg->e.seginfo->CodeBuffer[idx++] = *(pBytes++);
		}

	}
#if 1
	/* check this in pass 1 only */
	else if (CurrSeg->e.seginfo->current_loc < CurrSeg->e.seginfo->start_loc) {
		DebugMsg(("OutputByte: segment start loc changed from %" I32_SPEC "Xh to %" I32_SPEC "Xh\n",
			CurrSeg->e.seginfo->start_loc,
			CurrSeg->e.seginfo->current_loc));
		CurrSeg->e.seginfo->start_loc = CurrSeg->e.seginfo->current_loc;
	}
#endif
	CurrSeg->e.seginfo->current_loc+=len;
	CurrSeg->e.seginfo->bytes_written+=len;
	CurrSeg->e.seginfo->written = TRUE;
	if (CurrSeg->e.seginfo->current_loc > CurrSeg->sym.max_offset)
		CurrSeg->sym.max_offset = CurrSeg->e.seginfo->current_loc;
}

#if 0 /* v2.03: OutputCodeByte is obsolete */
void OutputCodeByte( unsigned char byte )
/***************************************/
{
    // if ( ModuleInfo.CommentDataInCode )
    // omf_OutSelect( FALSE );
    OutputByte( byte );
}
#endif

void FillDataBytes( unsigned char byte, int len )
/***********************************************/
{
    if ( ModuleInfo.CommentDataInCode )
        omf_OutSelect( TRUE );
    for( ; len; len-- )
        OutputByte( byte );
}

void OutputSegmentBytes(struct dsym* segg, const unsigned char *pbytes, int len, struct fixup *fixup)
/***************************************************************************/
{
	if (write_to_file == TRUE) {
		uint_32 idx = segg->e.seginfo->current_loc - segg->e.seginfo->start_loc;
#if 0 /* def DEBUG_OUT */
		if (CurrSeg->e.seginfo->current_loc < CurrSeg->e.seginfo->start_loc)
			_asm int 3;
#endif
		/**/myassert(segg->e.seginfo->current_loc >= segg->e.seginfo->start_loc);
		if (Options.output_format == OFORMAT_OMF && ((idx + len) > MAX_LEDATA_THRESHOLD)) {
			omf_FlushCurrSeg();
			idx = segg->e.seginfo->current_loc - segg->e.seginfo->start_loc;
		}
		if (fixup)
			store_fixup(fixup, segg, (int_32 *)pbytes);
		//DebugMsg(("OutputBytes: buff=%p, idx=%" I32_SPEC "X, byte=%X\n", CurrSeg->e.seginfo->CodeBuffer, idx, *pbytes ));
		memcpy(&segg->e.seginfo->CodeBuffer[idx], pbytes, len);
	}
#if 1
	/* check this in pass 1 only */
	else if (segg->e.seginfo->current_loc < segg->e.seginfo->start_loc) {
		DebugMsg(("OutputBytes: segment start loc changed from %" I32_SPEC "Xh to %" I32_SPEC "Xh\n",
			segg->e.seginfo->start_loc,
			segg->e.seginfo->current_loc));
		segg->e.seginfo->start_loc = segg->e.seginfo->current_loc;
	}
#endif
	segg->e.seginfo->current_loc += len;
	segg->e.seginfo->bytes_written += len;
	segg->e.seginfo->written = TRUE;
	if (segg->e.seginfo->current_loc > segg->sym.max_offset)
		segg->sym.max_offset = segg->e.seginfo->current_loc;
}

/*
 * this function is to output (small, <= 8) amounts of bytes which must
 * not be separated ( for omf, because of fixups )
 */
void OutputBytes( const unsigned char *pbytes, int len, struct fixup *fixup )
/***************************************************************************/
{
    if( write_to_file == TRUE ) {
        uint_32 idx = CurrSeg->e.seginfo->current_loc - CurrSeg->e.seginfo->start_loc;
#if 0 /* def DEBUG_OUT */
        if ( CurrSeg->e.seginfo->current_loc < CurrSeg->e.seginfo->start_loc )
            _asm int 3;
#endif
        /**/myassert( CurrSeg->e.seginfo->current_loc >= CurrSeg->e.seginfo->start_loc );
        if( Options.output_format == OFORMAT_OMF && ((idx + len) > MAX_LEDATA_THRESHOLD ) ) {
            omf_FlushCurrSeg();
            idx = CurrSeg->e.seginfo->current_loc - CurrSeg->e.seginfo->start_loc;
        }
        if ( fixup )
            store_fixup( fixup, CurrSeg, (int_32 *)pbytes );
        //DebugMsg(("OutputBytes: buff=%p, idx=%" I32_SPEC "X, byte=%X\n", CurrSeg->e.seginfo->CodeBuffer, idx, *pbytes ));
        memcpy( &CurrSeg->e.seginfo->CodeBuffer[idx], pbytes, len );
    }
#if 1
    /* check this in pass 1 only */
    else if( CurrSeg->e.seginfo->current_loc < CurrSeg->e.seginfo->start_loc ) {
        DebugMsg(("OutputBytes: segment start loc changed from %" I32_SPEC "Xh to %" I32_SPEC "Xh\n",
                  CurrSeg->e.seginfo->start_loc,
                  CurrSeg->e.seginfo->current_loc));
        CurrSeg->e.seginfo->start_loc = CurrSeg->e.seginfo->current_loc;
    }
#endif
    CurrSeg->e.seginfo->current_loc += len;
    CurrSeg->e.seginfo->bytes_written += len;
    CurrSeg->e.seginfo->written = TRUE;
    if( CurrSeg->e.seginfo->current_loc > CurrSeg->sym.max_offset )
        CurrSeg->sym.max_offset = CurrSeg->e.seginfo->current_loc;
}

/* Used to output a string to current segment in wide-char format with interleaved zeros */
void OutputInterleavedBytes(const unsigned char *pbytes, int len, struct fixup *fixup)
{
	int i = 0;
	char *pOut = NULL;

	if (write_to_file == TRUE) {
		uint_32 idx = CurrSeg->e.seginfo->current_loc - CurrSeg->e.seginfo->start_loc;
#if 0 /* def DEBUG_OUT */
		if (CurrSeg->e.seginfo->current_loc < CurrSeg->e.seginfo->start_loc)
			_asm int 3;
#endif
		/**/myassert(CurrSeg->e.seginfo->current_loc >= CurrSeg->e.seginfo->start_loc);
		if (Options.output_format == OFORMAT_OMF && ((idx + len) > MAX_LEDATA_THRESHOLD)) {
			omf_FlushCurrSeg();
			idx = CurrSeg->e.seginfo->current_loc - CurrSeg->e.seginfo->start_loc;
		}
		if (fixup)
			store_fixup(fixup, CurrSeg, (int_32 *)pbytes);
		pOut = &CurrSeg->e.seginfo->CodeBuffer[idx];
		for (i = 0; i < len*2; i++)
		{
			if (i % 2 == 1)
				*pOut++ = 0;
			else
				*pOut++ = *pbytes++;
		}
	}
#if 1
	/* check this in pass 1 only */
	else if (CurrSeg->e.seginfo->current_loc < CurrSeg->e.seginfo->start_loc) {
		DebugMsg(("OutputBytes: segment start loc changed from %" I32_SPEC "Xh to %" I32_SPEC "Xh\n",
			CurrSeg->e.seginfo->start_loc,
			CurrSeg->e.seginfo->current_loc));
		CurrSeg->e.seginfo->start_loc = CurrSeg->e.seginfo->current_loc;
	}
#endif
	CurrSeg->e.seginfo->current_loc += len*2;
	CurrSeg->e.seginfo->bytes_written += len*2;
	CurrSeg->e.seginfo->written = TRUE;
	if (CurrSeg->e.seginfo->current_loc > CurrSeg->sym.max_offset)
		CurrSeg->sym.max_offset = CurrSeg->e.seginfo->current_loc;
}

/* set current offset in a segment (usually CurrSeg) without to write anything */
ret_code SetCurrOffset( struct dsym *seg, uint_32 value, bool relative, bool select_data )
/****************************************************************************************/
{
    if( relative )
        value += seg->e.seginfo->current_loc;

    if ( Options.output_format == OFORMAT_OMF ) {
        if ( seg == CurrSeg ) {
            if ( write_to_file == TRUE )
                omf_FlushCurrSeg();

        /* for debugging, tell if data is located in code sections*/
            if( select_data )
                if ( ModuleInfo.CommentDataInCode )
                    omf_OutSelect( TRUE );
            LastCodeBufSize = value;
        }
        seg->e.seginfo->start_loc = value;
    /* for -bin, if there's an ORG (relative==false) and no initialized data
     * has been set yet, set start_loc!
     * v1.96: this is now also done for COFF and ELF
     */
    /* else if ( Options.output_format == OFORMAT_BIN && relative == FALSE ) { */
    } else {
        if ( write_to_file == FALSE ) {
            if ( relative ) {
#if 0 /* don't include "preceding" uninitialized data */
                if( seg->e.seginfo->current_loc < seg->e.seginfo->start_loc )
                    seg->e.seginfo->start_loc = seg->e.seginfo->current_loc;
#endif
            } else {
                if ( seg->e.seginfo->bytes_written == 0 )
                    seg->e.seginfo->start_loc = value;
            }
        }
    }

    seg->e.seginfo->current_loc = value;
    seg->e.seginfo->written = FALSE;

    if( seg->e.seginfo->current_loc > seg->sym.max_offset )
        seg->sym.max_offset = seg->e.seginfo->current_loc;

    return( NOT_ERROR );
}

/* write object module */
static ret_code WriteModule( struct module_info *modinfo )
/********************************************************/
{
    struct dsym *curr;

    DebugMsg(("WriteModule enter\n"));

    /* final checks */
    /* check limit of segments */
    for( curr = SymTables[TAB_SEG].head; curr; curr = curr->next ) {
        if ( curr->e.seginfo->Ofssize == USE16 && curr->sym.max_offset > 0x10000 ) {
            if ( Options.output_format == OFORMAT_OMF )
                EmitErr( SEGMENT_EXCEEDS_64K_LIMIT, curr->sym.name );
            else
                EmitWarn( 2, SEGMENT_EXCEEDS_64K_LIMIT, curr->sym.name );
        }
    }

    modinfo->g.WriteModule( modinfo );

#if DLLIMPORT
    /* is the -Fd option given with a file name? */
    if ( Options.names[OPTN_LNKDEF_FN] ) {
        FILE *ld;
        ld = fopen( Options.names[OPTN_LNKDEF_FN], "w" );
        if ( ld == NULL ) {
            return( EmitErr( CANNOT_OPEN_FILE, Options.names[OPTN_LNKDEF_FN], ErrnoStr() ) );
        }
        for ( curr = SymTables[TAB_EXT].head; curr != NULL ; curr = curr->next ) {
            DebugMsg(("WriteModule: ext=%s, isproc=%u, weak=%u\n", curr->sym.name, curr->sym.isproc, curr->sym.weak ));
            if ( curr->sym.isproc && ( curr->sym.weak == FALSE || curr->sym.iat_used ) &&
                curr->sym.dll && *(curr->sym.dll->name) != NULLC ) {
                int size;
                Mangle( &curr->sym, StringBufferEnd );
                size = sprintf( CurrSource, "import '%s'  %s.%s\n", StringBufferEnd, curr->sym.dll->name, curr->sym.name );
                if ( fwrite( CurrSource, 1, size, ld ) != size )
                    WriteError();
            }
        }
        fclose( ld );
    }
#endif
    DebugMsg(("WriteModule exit\n"));
    return( NOT_ERROR );
}
/* The macro is_valid_first_char is defined differently in globals.h */
#undef is_valid_first_char
#define is_valid_first_char( ch )  ( isalpha(ch) || ch=='_' || ch=='@' || ch=='$' || ch=='?' || ch=='.' )

/* check name of text macros defined via -D option */

static int is_valid_identifier( char *id )
/****************************************/
{
    /* special handling of first char of an id: it can't be a digit,
     but can be a dot (don't care about ModuleInfo.dotname!). */

    if( is_valid_first_char( *id ) == 0 )
        return( ERROR );
    id++;
    for( ; *id != NULLC; id++ ) {
        if ( is_valid_id_char( *id ) == FALSE )
            return( ERROR );
    }
    /* don't allow a single dot! */
    if ( *(id-1) == '.' )
        return( ERROR );

    return( NOT_ERROR );
}

/* add text macros defined with the -D cmdline switch */

static void add_cmdline_tmacros( void )
/****************************************/
{
    struct qitem *p;
    char *name;
    char *value;
    int len;
    struct asym *sym;

    DebugMsg(("add_cmdline_tmacros enter\n"));
    for ( p = Options.queues[OPTQ_MACRO]; p; p = p->next ) {
        DebugMsg(("add_cmdline_tmacros: found >%s<\n", p->value));
        name = p->value;
        value = strchr( name, '=' );
        if( value == NULL ) {
            /* v2.06: ensure that 'value' doesn't point to r/o space */
            //value = "";
            value = name + strlen( name ); /* use the terminating NULL */
        } else {
            len = value - name;
            name = (char *)myalloca( len + 1 );
            memcpy( name, p->value, len );
            *(name + len) = NULLC;
            value++;
        }

        /* there's no check whether the name is a reserved word!
         */
        if( is_valid_identifier( name ) == ERROR ) {
            DebugMsg(("add_cmdline_tmacros: name >%s< invalid\n", name ));
            EmitErr( SYNTAX_ERROR_EX, name );
        } else {
            sym = SymSearch( name );
            if ( sym == NULL ) {
                sym = SymCreate( name );
                sym->state = SYM_TMACRO;
            }
            if ( sym->state == SYM_TMACRO ) {
                sym->isdefined = TRUE;
                sym->predefined = TRUE;
                sym->string_ptr = value;
            } else
                EmitErr( SYMBOL_ALREADY_DEFINED, name );
        }
    }
    return;
}

/* add the include paths set by -I option */

static void add_incpaths( void )
/******************************/
{
    struct qitem *p;
    DebugMsg(("add_incpaths: enter\n"));
    for ( p = Options.queues[OPTQ_INCPATH]; p; p = p->next ) {
        AddStringToIncludePath( p->value );
    }
}

/* this is called for every pass.
 * symbol table and ModuleInfo are initialized.
 */
static void CmdlParamsInit( int pass )
/************************************/
{
    DebugMsg(("CmdlParamsInit(%u) enter\n", pass));

#if BUILD_TARGET
    if ( pass == PASS_1 ) {
        struct asym *sym;
        char *tmp;
        char *p;

        _strupr( Options.build_target );
        tmp = myalloca( strlen( Options.build_target ) + 5 ); /* null + 4 uscores */
        strcpy( tmp, uscores );
        strcat( tmp, Options.build_target );
        strcat( tmp, uscores );

        /* define target */
        sym = CreateVariable( tmp, 0 );
        sym->predefined = TRUE;

        p = NULL;
        if( _stricmp( Options.build_target, "DOS" ) == 0 ) {
            p = "__MSDOS__";
        } else if( _stricmp( Options.build_target, "NETWARE" ) == 0 ) {
            if( ( ModuleInfo.curr_cpu & P_CPU_MASK ) >= P_386 ) {
                p = "__NETWARE_386__";
            } else {
                /* do nothing ... __NETWARE__ already defined */
            }
        } else if( _stricmp( Options.build_target, "WINDOWS" ) == 0 ) {
            if( ( ModuleInfo.curr_cpu & P_CPU_MASK ) >= P_386 ) {
                p = "__WINDOWS_386__";
            } else {
                /* do nothing ... __WINDOWS__ already defined */
            }
        } else if( _stricmp( Options.build_target, "QNX" ) == 0 ) {
            p = "__UNIX__";
        } else if( _stricmp( Options.build_target, "LINUX" ) == 0 ) {
            p = "__UNIX__";
        }
        if ( p ) {
            sym = CreateVariable( p, 0 );
            sym->predefined = TRUE;
        }
    }
#endif

    if ( pass == PASS_1 ) {
        char *env;
        /* v2.06: this is done in ModulePassInit now */
        //SetCPU( Options.cpu );
        add_cmdline_tmacros();
        add_incpaths();
        if ( Options.ignore_include == FALSE )
            if ( env = getenv( "INCLUDE" ) )
                AddStringToIncludePath( env );
    }
    DebugMsg(("CmdlParamsInit exit\n"));
    return;
}

void WritePreprocessedLine( const char *string )
/**********************************************/
/* print out preprocessed source lines
 */
{
    static bool PrintEmptyLine = TRUE;
    const char *p;

#if 0 /* v2.08: removed, obsolete */
    /* filter some macro specific directives */
    if ( tokenarray[0].token == T_DIRECTIVE &&
         ( tokenarray[0].tokval == T_ENDM ||
           tokenarray[0].tokval == T_EXITM))
        return;
    /* don't print generated code - with one exception:
     if the code was generated as a result of structure initialization,
     then do!
     */
    if ( GeneratedCode )
        return;
#endif
    if ( Token_Count > 0 ) {
        /* v2.08: don't print a leading % (this char is no longer filtered) */
        for ( p = string; isspace( *p ); p++ );
        printf("%s\n", *p == '%' ? p+1 : string );
        PrintEmptyLine = TRUE;
    } else if ( PrintEmptyLine ) {
        PrintEmptyLine = FALSE;
        printf("\n");
    }
}

/* set Masm v5.1 compatibility options */

void SetMasm510( bool value )
/***************************/
{
    ModuleInfo.m510 = value;
    ModuleInfo.oldstructs = value;
    /* ModuleInfo.oldmacros = value; not implemented yet */
    ModuleInfo.dotname = value;
    ModuleInfo.setif2 = value;

    if ( value ) {
        if ( ModuleInfo.model == MODEL_NONE ) {
            /* if no model is specified, set OFFSET:SEGMENT */
            ModuleInfo.offsettype = OT_SEGMENT;
            if ( ModuleInfo.langtype == LANG_NONE ) {
                ModuleInfo.scoped = FALSE;
                ModuleInfo.procs_private = TRUE;
            }
        }
    }
    return;
}

/* called for each pass */

static void ModulePassInit( void )
/********************************/
{
    enum cpu_info cpu = Options.cpu;
    enum model_type model = Options.model;
#if DLLIMPORT
    struct dsym *curr;
#endif

    DebugMsg(( "ModulePassInit() enter\n" ));
    /* set default values not affected by the masm 5.1 compat switch */
    ModuleInfo.procs_private = FALSE;
    ModuleInfo.procs_export = FALSE;
    ModuleInfo.offsettype = OT_GROUP;
    ModuleInfo.scoped = TRUE;


#if FASTPASS
    /* v2.03: don't generate the code if fastpass is active */
    /* v2.08: query UseSavedState instead of StoreState */
    if ( UseSavedState == FALSE ) {
#endif
        ModuleInfo.langtype = Options.langtype;
        ModuleInfo.fctype = Options.fctype;

		#if AMD64_SUPPORT
		if (Options.output_format == OFORMAT_ELF)
		{
			ModuleInfo.fctype = FCT_WIN64;
			Options.fctype = FCT_WIN64; /* SYSV proc/invoke tables use the same ordinal as FCT_WIN64 so set it now, instead of FCT_MSC */
		}
		#endif 

#if AMD64_SUPPORT
        if ( ModuleInfo.sub_format == SFORMAT_64BIT ) 
		{

            /* v2.06: force cpu to be at least P_64, without side effect to Options.cpu */
            if ( ( cpu &  P_CPU_MASK ) < P_64 ) /* enforce cpu to be 64-bit */
                cpu = P_64;
            /* ignore -m switch for 64-bit formats.
             * there's no other model than FLAT possible.
             */
            model = MODEL_FLAT;
            if (ModuleInfo.langtype == LANG_NONE && Options.output_format == OFORMAT_COFF)
                ModuleInfo.langtype = LANG_FASTCALL;
			if (ModuleInfo.langtype == LANG_NONE && Options.output_format == OFORMAT_ELF)
				ModuleInfo.langtype = LANG_SYSVCALL;
			if (ModuleInfo.langtype == LANG_NONE && Options.output_format == OFORMAT_MAC)
				ModuleInfo.langtype = LANG_SYSVCALL;

        } else
#endif
            /* if model FLAT is to be set, ensure that cpu is compat. */
            if ( model == MODEL_FLAT && ( cpu & P_CPU_MASK ) < P_386 ) /* cpu < 386? */
                cpu = P_386;

        SetCPU( cpu );
        /* table ModelToken starts with MODEL_TINY, which is index 1" */
        if ( model != MODEL_NONE )
            AddLineQueueX( "%r %s", T_DOT_MODEL, ModelToken[model - 1] );

#if FASTPASS
    }
#endif

    SetMasm510( Options.masm51_compat );
    ModuleInfo.defOfssize = USE16;
    ModuleInfo.ljmp       = TRUE;

    ModuleInfo.list                = Options.write_listing;
    ModuleInfo.cref                = TRUE;
    ModuleInfo.listif              = Options.listif;
    ModuleInfo.list_generated_code = Options.list_generated_code;
    ModuleInfo.list_macro          = Options.list_macro;

    ModuleInfo.case_sensitive    = Options.case_sensitive;
    ModuleInfo.convert_uppercase = Options.convert_uppercase;
    SymSetCmpFunc();

    ModuleInfo.segorder   = SEGORDER_SEQ;
    ModuleInfo.radix      = 10;
    ModuleInfo.fieldalign = Options.fieldalign;
    ModuleInfo.procalign = 0;

#if DLLIMPORT
    /* if OPTION DLLIMPORT was used, reset all iat_used flags */
    if ( ModuleInfo.g.DllQueue )
        for ( curr = SymTables[TAB_EXT].head; curr; curr = curr->next )
            curr->sym.iat_used = FALSE;
#endif
}

#if 0 /* v2.07: removed */
/* scan - and clear - global queue (EXTERNDEFs).
 * items which have been defined within the module
 * will become public.
 * PROTOs aren't included in the global queue.
 * They will become public when - and if - the PROC directive
 * for the symbol is met.
 */
static void scan_globals( void )
/******************************/
{
    struct qnode    *curr;
    struct qnode    *next;
    struct asym     *sym;

    /* turn EXTERNDEFs into PUBLICs if defined in the module.
     * PROCs are handled differently - so ignore these entries here!
     */
    /* obsolete since v2.07.
     * it's simpler and better to make the symbol public if it turns
     * from SYM_EXTERNAL to SYM_INTERNAL.
     * the other case, that is, the EXTERNDEF comes AFTER the definition,
     * is handled in ExterndefDirective()
     */
    DebugMsg(("scan_globals: GlobalQueue=%X\n", ModuleInfo.g.GlobalQueue));
    for ( curr = ModuleInfo.g.GlobalQueue.head; curr; curr = next ) {
        next = curr->next;
        sym = (struct asym *)curr->elmt;
        DebugMsg(("scan_globals: %s state=%u used=%u public=%u\n", sym->name, sym->state, sym->used, sym->public ));
        if( sym->state == SYM_INTERNAL && sym->public == FALSE && sym->isproc == FALSE ) {
            /* add it to the public queue */
            sym->public = TRUE;
            QEnqueue( &ModuleInfo.g.PubQueue, curr );
            DebugMsg(("scan_globals: %s added to public queue\n", sym->name ));
            continue; /* don't free this item! */
        }
        LclFree( curr );
    }
    /* the queue is empty now */
    ModuleInfo.g.GlobalQueue.head = NULL;
}
#endif

/* checks after pass one has been finished without errors */

static void PassOneChecks( void )
/*******************************/
{
    struct dsym *curr;
    struct dsym *next;
    struct qnode *q;
    struct qnode *qn;
#ifdef DEBUG_OUT
    int cntUnusedExt = 0;
#endif

    /* check for open structures and segments has been done inside the
     * END directive handling already
     * v2.10: now done for PROCs as well, since procedures
     * must be closed BEFORE segments are to be closed.
     */
    HllCheckOpen();
    CondCheckOpen();

	/* Don't require END directive for bin output */
    if( ModuleInfo.EndDirFound == FALSE  && Options.output_format != OFORMAT_BIN)
        EmitError( END_DIRECTIVE_REQUIRED );

    /* v2.04: check the publics queue.
     * - only internal symbols can be public.
     * - weak external symbols are filtered ( since v2.11 )
     * - anything else is an error
     * v2.11: moved here ( from inside the "#if FASTPASS"-block )
     * because the loop will now filter weak externals [ this
     * was previously done in GetPublicSymbols() ]
     */
    for( q = ModuleInfo.g.PubQueue.head, qn = (struct qnode *)&ModuleInfo.g.PubQueue ; q; q = q->next ) {

        if ( q->sym->state == SYM_INTERNAL )
            qn = q;
        else if ( q->sym->state == SYM_EXTERNAL && q->sym->weak == TRUE ) {
            DebugMsg(("PassOneChecks: public for weak external skipped: %s\n", q->sym->name ));
            qn->next = q->next;
            LclFree( q );
            q = qn;
        } else {
            DebugMsg(("PassOneChecks: invalid public attribute for %s [state=%u weak=%u]\n", q->sym->name, q->sym->state, q->sym->weak ));
#if FASTPASS
            SkipSavedState();
#endif
            break;
        }
    }

#if FASTPASS
    if ( SymTables[TAB_UNDEF].head ) {
        /* to force a full second pass in case of missing symbols,
         * activate the next line. It was implemented to have proper
         * error displays if a forward reference wasn't found.
         * However, v1.95 final won't need this anymore, because both
         * filename + lineno for every line is known now in pass 2.
         */
        /* SkipSavedState(); */
    }

    /* check if there's an undefined segment reference.
     * This segment was an argument to a group definition then.
     * Just do a full second pass, the GROUP directive will report
     * the error.
     */
    for( curr = SymTables[TAB_SEG].head; curr; curr = curr->next ) {
        if( curr->sym.segment == NULL ) {
            DebugMsg(("PassOneChecks: undefined segment %s\n", curr->sym.name ));
            SkipSavedState();
            break;
        }
    }

#if COFF_SUPPORT
    /* if there's an item in the safeseh list which is not an
     * internal proc, make a full second pass to emit a proper
     * error msg at the .SAFESEH directive
     */
    for ( q = ModuleInfo.g.SafeSEHQueue.head; q; q = q->next ) {
        if ( q->sym->state != SYM_INTERNAL || q->sym->isproc == FALSE ) {
            SkipSavedState();
            break;
        }
    }
#endif

    /* scan ALIASes for COFF/ELF */
    if ( Options.output_format == OFORMAT_COFF || Options.output_format == OFORMAT_ELF ) {
        for( curr = SymTables[TAB_ALIAS].head ; curr != NULL ;curr = curr->next ) {
            struct asym *sym;
            sym = curr->sym.substitute;
            /* check if symbol is external or public */
            if ( sym == NULL ||
                ( sym->state != SYM_EXTERNAL &&
                 ( sym->state != SYM_INTERNAL || sym->ispublic == FALSE ))) {
                SkipSavedState();
                break;
            }
            /* make sure it becomes a strong external */
            if ( sym->state == SYM_EXTERNAL )
                sym->used = TRUE;
        }
    }

#endif /* FASTPASS */

    /* scan the EXTERN/EXTERNDEF items */

    for( curr = SymTables[TAB_EXT].head ; curr; curr = next ) 
	{
        next = curr->next;
        /* v2.01: externdefs which have been "used" become "strong" */
        if ( curr->sym.used )
            curr->sym.weak = FALSE;
        /* remove unused EXTERNDEF/PROTO items from queue. */
        if ( curr->sym.weak == TRUE && curr->sym.iat_used == FALSE )
		{
            sym_remove_table( &SymTables[TAB_EXT], curr );
			#ifdef DEBUG_OUT
		        cntUnusedExt++;
			#endif
            continue;
        }

#if FASTMEM==0
        /* v2.05: clear fixup list (used for backpatching in pass one) */
        if ( curr->sym.bp_fixup ) {
            struct fixup *c;
            struct fixup *n;
            for( c = curr->sym.bp_fixup ; c; ) {
                n = c->nextbp;
                LclFree( c );
                c = n;
            }
            curr->sym.bp_fixup = NULL;
        }
#endif

        if ( curr->sym.iscomm == TRUE )
            continue;
        /* optional alternate symbol must be INTERNAL or EXTERNAL.
         * COFF ( and ELF? ) also wants internal symbols to be public
         * ( which is reasonable, since the linker won't know private
         * symbols and hence will search for a symbol of that name
         * "elsewhere" ).
         */
#if FASTPASS
        if ( curr->sym.altname ) 
		{
            if ( curr->sym.altname->state == SYM_INTERNAL ) 
			{
                /* for COFF/ELF, the altname must be public or external */
                if ( curr->sym.altname->ispublic == FALSE && ( Options.output_format == OFORMAT_COFF || Options.output_format == OFORMAT_ELF ) ) 
				{
                    SkipSavedState();
                }
            } else if ( curr->sym.altname->state != SYM_EXTERNAL ) 
			{
                /* do not use saved state, scan full source in second pass */
                SkipSavedState();
            }
        }
#endif
    }

#ifdef DEBUG_OUT
    DebugMsg(("PassOneChecks: removed unused externals: %u\n", cntUnusedExt ));
    DebugMsg(("PassOneChecks: forward references:\n"));
    for( curr = SymTables[TAB_SEG].head; curr; curr = curr->next ) {
        int i;
        int j;
        struct asym * sym;
        struct fixup * fix;
        for ( i = 0, j = 0, sym = curr->e.seginfo->label_list; sym; sym = (struct asym *)((struct dsym *)sym)->next ) {
            i++;
            for ( fix = sym->bp_fixup; fix ; fix = fix->nextbp, j++ );
        }
        DebugMsg(("PassOneChecks: segm=%s, labels=%u forward refs=%u\n", curr->sym.name, i, j));
    }
#endif

    if ( ModuleInfo.g.error_count == 0 ) {

        /* make all symbols of type SYM_INTERNAL, which aren't
         a constant, public.  */
        if ( Options.all_symbols_public )
            SymMakeAllSymbolsPublic();

        if ( Options.syntax_check_only == FALSE )
            write_to_file = TRUE;

        if ( ModuleInfo.g.Pass1Checks )
            ModuleInfo.g.Pass1Checks( &ModuleInfo );
    }


    return;
}

/* do ONE assembly pass
 * the FASTPASS variant (which is default now) doesn't scan the full source
 * for each pass. For this to work, the following things are implemented:
 * 1. in pass one, save state if the first byte is to be emitted.
 *    <state> is the segment stack, moduleinfo state, ...
 * 2. once the state is saved, all preprocessed lines must be stored.
 *    this can be done here, in OnePass, the line is in <string>.
 *    Preprocessed macro lines are stored in RunMacro().
 * 3. for subsequent passes do
 *    - restore the state
 *    - read preprocessed lines and feed ParseLine() with it
 */
static int OnePass( void )
/************************/
{

	struct asym*    platform;

    InputPassInit();
    ModulePassInit();
    SymPassInit( Parse_Pass );
    LabelInit();
    SegmentInit( Parse_Pass );
    ContextInit( Parse_Pass );
    ProcInit();
    TypesInit();
    HllInit( Parse_Pass );
    MacroInit( Parse_Pass ); /* insert predefined macros */
    AssumeInit( Parse_Pass );
    CmdlParamsInit( Parse_Pass );

    ModuleInfo.EndDirFound = FALSE;
    ModuleInfo.PhaseError  = FALSE;

	LinnumInit();

#ifdef DEBUG_OUT
    if ( Parse_Pass > PASS_1 ) {
        DebugMsg(("OnePass(%u) segments (current=%s):\n", Parse_Pass + 1, CurrSeg ? CurrSeg->sym.name : "NULL" ));
        {
            struct dsym *dir;
            for( dir = SymTables[TAB_SEG].head; dir; dir = dir->next ) {
                DebugMsg(("OnePass(%u): segm=%-8s typ=%X start=%8X max_ofs=%8X\n", Parse_Pass + 1,
                          dir->sym.name, dir->e.seginfo->segtype, dir->e.seginfo->start_loc, dir->sym.max_offset ));
            }
        }
    }
#endif

	/* the functions above might have written something to the line queue */
    if ( is_linequeue_populated() )
        RunLineQueue();

	/* Set platform type */
	platform = SymFind("@Platform");
	if ( Options.output_format == OFORMAT_BIN && Options.sub_format == SFORMAT_NONE )
		platform->value = 1;
	else if ( Options.output_format == OFORMAT_COFF && Options.sub_format == SFORMAT_NONE )
		platform->value = 0;
	else if( Options.output_format == OFORMAT_COFF && Options.sub_format == SFORMAT_64BIT )
		platform->value = 1;
	else if (Options.output_format == OFORMAT_ELF && Options.sub_format == SFORMAT_NONE )
		platform->value = 2;
	else if (Options.output_format == OFORMAT_ELF && Options.sub_format == SFORMAT_64BIT )
		platform->value = 3;
	else if (Options.output_format == OFORMAT_MAC && Options.sub_format == SFORMAT_64BIT )
		platform->value = 4;

	if (Parse_Pass == PASS_1)
	{
		struct asym* archSym = SymFind("@Arch");
		archSym->value = ModuleInfo.arch;
	}

	/* Process our built-in macro library to make it available to the rest of the source */
	if (Parse_Pass == PASS_1 && Options.nomlib == FALSE) 
	{
		unsigned  alist = ModuleInfo.list;
		ModuleInfo.list = 0;
		if(platform->value == 0)
			InitAutoMacros32();
		else
			InitAutoMacros64();
		ModuleInfo.list = alist;
	}
	if (Parse_Pass == PASS_1)
	{
		/* FIXME */ extern int AddSimdTypes ();
		unsigned  alist = ModuleInfo.list;
		ModuleInfo.list = 0;
		AddSimdTypes();
		ModuleInfo.list = alist;
    /* UASM 2.49 Ensure these are reset here in pre-process, as this can be set on a line that
    still requires expansion, and then they accidentally remain set when moving onto a regular line via codegen */
    decoflags = 0;
    broadflags = 0;
    evexflag = 0;
	}

#if FASTPASS
    StoreState = FALSE;
    if ( Parse_Pass > PASS_1 && UseSavedState == TRUE ) {
        LineStoreCurr = RestoreState();
        while ( LineStoreCurr && ModuleInfo.EndDirFound == FALSE ) {
            /* the source line is modified in Tokenize() if it contains a comment! */
#if USELSLINE==0
            strcpy( CurrSource, LineStoreCurr->line );
#endif
            set_curr_srcfile( LineStoreCurr->srcfile, LineStoreCurr->lineno );
            /* v2.06: list flags now initialized on the top level */
            ModuleInfo.line_flags = 0;
            MacroLevel = ( LineStoreCurr->srcfile == 0xFFF ? 1 : 0 );
            DebugMsg1(("OnePass(%u) cur/nxt=%X/%X src=%X.%u mlvl=%u: >%s<\n", Parse_Pass+1, LineStoreCurr, LineStoreCurr->next, LineStoreCurr->srcfile, LineStoreCurr->lineno, MacroLevel, LineStoreCurr->line ));
            ModuleInfo.CurrComment = NULL; /* v2.08: added (var is never reset because GetTextLine() isn't called) */
#if USELSLINE
            if ( Token_Count = Tokenize( LineStoreCurr->line, 0, ModuleInfo.tokenarray, TOK_DEFAULT ) )
#else
            if ( Token_Count = Tokenize( CurrSource, 0, ModuleInfo.tokenarray, TOK_DEFAULT ) )
#endif
                ParseLine( ModuleInfo.tokenarray );
            LineStoreCurr = LineStoreCurr->next;
        }
    } else
#endif
    {
        struct qitem *pq;
        /* v2.11: handle -Fi files here ( previously in CmdlParamsInit ) */
        for ( pq = Options.queues[OPTQ_FINCLUDE]; pq; pq = pq->next ) {
            DebugMsg(("OnePass: force include of file: %s\n", pq->value ));
            if ( SearchFile( pq->value, TRUE ) )
                ProcessFile( ModuleInfo.tokenarray );
        }
        ProcessFile( ModuleInfo.tokenarray ); /* process the main source file */
    }

    LinnumFini();

    if ( Parse_Pass == PASS_1 )
        PassOneChecks();

    ClearSrcStack();

    return( 1 );
}

#if BUILD_TARGET
/*
 * from WASM : get os-specific xxx_INCLUDE environment variable.
 *             if set, add string to include path.
 */

static void get_os_include( void )
/********************************/
{
    char *env;
    char *tmp;

    /* add OS_include to the include path */

    tmp = myalloca( strlen( Options.build_target ) + 10 );
    strcpy( tmp, Options.build_target );
    strcat( tmp, "_INCLUDE" );

    env = getenv( tmp );
    if( env != NULL ) {
        AddStringToIncludePath( env );
    }
}

#endif

static void get_module_name( void )
/*********************************/
{
    char        *p; /* FIXME */ extern int strupr (char []);

    /* v2.08: prefer name given by -nm option */
    if ( Options.names[OPTN_MODULE_NAME] ) {
        strncpy( ModuleInfo.name, Options.names[OPTN_MODULE_NAME], sizeof( ModuleInfo.name ) );
        ModuleInfo.name[ sizeof( ModuleInfo.name ) - 1] = NULLC;
    } else {
        /* v2.12: _splitpath()/_makepath() removed */
        const char *fn = GetFNamePart( CurrFName[ASM] );
        char *ext = GetExtPart( fn );
        memcpy( ModuleInfo.name, fn, ext - fn );
        ModuleInfo.name[ ext - fn ] = NULLC;
        //_splitpath( CurrFName[ASM], NULL, NULL, ModuleInfo.name, dummy );
    }

    _strupr( ModuleInfo.name );
    /* the module name must be a valid identifier, because it's used
     * as part of a segment name in certain memory models.
     */
    for( p = ModuleInfo.name; *p; ++p ) {
        if( !( isalnum( *p ) || ( *p == '_' ) || ( *p == '$' )
            || ( *p == '@' ) || ( *p == '?') ) ) {
            /* it's not a legal character for a symbol name */
            *p = '_';
        }
    }
    /* first character can't be a digit either */
    if( isdigit( ModuleInfo.name[0] ) ) {
        ModuleInfo.name[0] = '_';
    }
}

/* called by AssembleInit(), once per source module.
 * symbol table has been initialized here.
 */
static void ModuleInit( void )
/****************************/
{
    ModuleInfo.sub_format = Options.sub_format;
    ModuleInfo.fmtopt = &formatoptions[Options.output_format];
    ModuleInfo.CommentDataInCode = (Options.output_format == OFORMAT_OMF && Options.no_comment_data_in_code_records == FALSE);
    ModuleInfo.g.error_count = 0;
    ModuleInfo.g.warning_count = 0;
    ModuleInfo.model = MODEL_NONE;
    /* ModuleInfo.distance = STACK_NONE; */
    ModuleInfo.ostype = OPSYS_DOS;
    ModuleInfo.emulator = (Options.floating_point == FPO_EMULATION);

    get_module_name(); /* set ModuleInfo.name */

    /* v2.06: ST_PROC has been removed */
    memset( SymTables, 0, sizeof( SymTables[0] ) * TAB_LAST );
    ModuleInfo.fmtopt->init( &ModuleInfo );
    gmaskflag = 0;
    return;
}

static void ReswTableInit( void )
/*******************************/
{
    ResWordsInit();
    if ( Options.output_format == OFORMAT_OMF ) 
	{
        /* for OMF, IMAGEREL and SECTIONREL are disabled */
        DisableKeyword( T_IMAGEREL );
        DisableKeyword( T_SECTIONREL );
    }
    if ( Options.strict_masm_compat == TRUE ) 
	{
        DisableKeyword( T_INCBIN );
        DisableKeyword( T_FASTCALL );
    }

    return;
}

static void open_files( void )
/****************************/
{
    /* open ASM file */
    DebugMsg(("open_files() enter\n" ));

    //memset( CurrFile, 0, sizeof( CurrFile ) );
    CurrFile[ASM] = fopen( CurrFName[ASM], "rb" );
    if( CurrFile[ASM] == NULL ) {
        DebugMsg(("open_files(): cannot open source file, fopen(\"%s\") failed\n", CurrFName[ASM] ));
        Fatal( CANNOT_OPEN_FILE, CurrFName[ASM], ErrnoStr() );
    }
	//CheckBOM(CurrFile[ASM]);

    /* open OBJ file */
    if ( Options.syntax_check_only == FALSE ) {
        CurrFile[OBJ] = fopen( CurrFName[OBJ], "wb" );
        if( CurrFile[OBJ] == NULL ) {
            DebugMsg(("open_files(): cannot open object file, fopen(\"%s\") failed\n", CurrFName[OBJ] ));
            Fatal( CANNOT_OPEN_FILE, CurrFName[OBJ], ErrnoStr() );
        }
        DebugMsg(("open_files(): output, fopen(\"%s\") ok\n", CurrFName[OBJ] ));
    }

    if( Options.write_listing ) {
        CurrFile[LST] = fopen( CurrFName[LST], "wb" );
        if ( CurrFile[LST] == NULL )
            Fatal( CANNOT_OPEN_FILE, CurrFName[LST], ErrnoStr() );
    }
    return;
}

void close_files( void )
/**********************/
{
    /* v2.11: no fatal errors anymore if fclose() fails.
     * That's because Fatal() may cause close_files() to be
     * reentered and thus cause an endless loop.
     */

    /* close ASM file */
    if( CurrFile[ASM] != NULL ) {
        if( fclose( CurrFile[ASM] ) != 0 )
            EmitErr( CANNOT_CLOSE_FILE, CurrFName[ASM], errno );
        CurrFile[ASM] = NULL;
    }

    /* close OBJ file */
    if ( CurrFile[OBJ] != NULL ) {
        if ( fclose( CurrFile[OBJ] ) != 0 )
            EmitErr( CANNOT_CLOSE_FILE, CurrFName[OBJ], errno );
        CurrFile[OBJ] = NULL;
    }
    /* delete the object module if errors occured */
    if ( Options.syntax_check_only == FALSE &&
        ModuleInfo.g.error_count > 0 ) {
        remove( CurrFName[OBJ] );
    }

    if( CurrFile[LST] != NULL ) {
        fclose( CurrFile[LST] );
        CurrFile[LST] = NULL;
    }

    /* close ERR file */
    if ( CurrFile[ERR] != NULL ) {
        fclose( CurrFile[ERR] );
        CurrFile[ERR] = NULL;
    } else if ( CurrFName[ERR] )
        /* nothing written, delete any existing ERR file */
        remove( CurrFName[ERR] );
    return;
}

/* get default file extension for error, object and listing files */

static char const *GetExt( int type )
/*****************************/
{
    switch ( type ) {
    case OBJ:
#if BIN_SUPPORT
        if ( Options.output_format == OFORMAT_BIN )
#if MZ_SUPPORT || PE_SUPPORT
            if ( Options.sub_format == SFORMAT_MZ
#if PE_SUPPORT
                || Options.sub_format == SFORMAT_PE
#endif
               )
                return( EXE_EXT );
            else
#endif
                return( BIN_EXT );
#endif
        return( OBJ_EXT );
    case LST:
        return( LST_EXT );
    case ERR:
        return( ERR_EXT );
    }
    return( NULL );
}

/* set filenames for .obj, .lst and .err
 * in:
 *  name: assembly source name
 *  DefaultDir[]: default directory part for .obj, .lst and .err
 * in:
 *  CurrFName[] for .obj, .lst and .err ( may be NULL )
 * v2.12: _splitpath()/_makepath() removed.
 */

static void SetFilenames( const char *name )
/******************************************/
{
    int i;
    const char *fn;
    char *ext;
    char path[ FILENAME_MAX ];

    DebugMsg(("SetFilenames(\"%s\") enter\n", name ));

    /* set CurrFName[ASM] */
    CurrFName[ASM] = LclAlloc( strlen( name ) + 1 );
    strcpy( CurrFName[ASM], name );

    /* set [OBJ], [ERR], [LST] */
    fn = GetFNamePart( name );
    for ( i = ASM+1; i < NUM_FILE_TYPES; i++ ) {
        if( Options.names[i] == NULL ) {
            path[0] = NULLC;
            if ( DefaultDir[i])
                strcpy( path, DefaultDir[i] );
            strcat( path, fn );
            ext = GetExtPart( path );
            *ext++  = '.';
            strcpy( ext, GetExt( i ) );

        } else {
            /* filename has been set by cmdline option -Fo, -Fl or -Fr */
            const char *fn2;
            strcpy( path, Options.names[i] );
            fn2 = GetFNamePart( path );
            if( *fn2 == NULLC )
                strcpy( (char *)fn2, fn );
            ext = GetExtPart( fn2 );
            if( *ext == NULLC ) {
                *ext++  = '.';
                strcpy( ext, GetExt( i ) );
            }
        }
        DebugMsg(("SetFilenames: i=%u >%s<\n", i, path ));
        CurrFName[i] = LclAlloc( strlen( path ) + 1 );
        strcpy( CurrFName[i], path );
    }
    return;
}

/* init assembler. called once per module */
static void AssembleInit( const char *source )
/********************************************/
{
    MemInit();
    write_to_file    = FALSE;
    LinnumQueue.head = NULL;

    SetFilenames( source );
#if FASTPASS
    FastpassInit();
#endif
    open_files();
#if BUILD_TARGET
    get_os_include();
#endif
    ReswTableInit();
    SymInit();
    InputInit();
    ModuleInit();
    CondInit();
    ExprEvalInit();
    LstInit();

    return;
}

#ifdef DEBUG_OUT
void DumpInstrStats( void );
#endif

/* called once per module. AssembleModule() cleanup */

static void AssembleFini( void )
/******************************/
{
    int i;
    SegmentFini();
    SymFini();
    ResWordsFini();
#ifdef DEBUG_OUT
    DumpInstrStats();
    MacroFini();
#endif
    FreePubQueue();
#if FASTMEM==0
    FreeLibQueue();
    ContextFini();
    HllFini();
#endif
    InputFini();
    close_files();

#if FASTPASS
#if FASTMEM==0
    FreeLineStore();
#endif
#endif

    for ( i = 0; i < NUM_FILE_TYPES; i++ ) {
        LclFree( CurrFName[i] );
        /* v2.05: make sure the pointer for ERR is cleared */
        CurrFName[i] = NULL;
    }
    MemFini();
    return;
}

/* AssembleModule() assembles one source file */

int EXPQUAL AssembleModule( const char *source )
/**********************************************/
{
    uint_32       prev_written = -1;
    uint_32       curr_written;
    int           starttime;
    int           endtime;
    struct dsym   *seg;

	struct module_info tempInfo;

#ifdef _WIN32
	CONSOLE_SCREEN_BUFFER_INFO screenBufferInfo;
	HANDLE hConsole;
	memset(&screenBufferInfo, 0, sizeof(screenBufferInfo));
	hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
	GetConsoleScreenBufferInfo(hConsole, &screenBufferInfo);
#endif
	memset(&tempInfo, 0, sizeof(tempInfo));

    DebugMsg(("AssembleModule(\"%s\") enter\n", source ));

	tempInfo.frame_auto   = ModuleInfo.frame_auto;
	tempInfo.fctype       = ModuleInfo.fctype;
	tempInfo.langtype     = ModuleInfo.langtype;
	tempInfo.sub_format   = ModuleInfo.sub_format;
	tempInfo.win64_flags  = ModuleInfo.win64_flags;
	tempInfo.switch_style = ModuleInfo.switch_style;

    memset( &ModuleInfo, 0, sizeof(ModuleInfo) );

	ModuleInfo.frame_auto   = tempInfo.frame_auto;
	ModuleInfo.fctype       = tempInfo.fctype;
	ModuleInfo.langtype     = tempInfo.langtype;
	ModuleInfo.sub_format   = tempInfo.sub_format;
	ModuleInfo.win64_flags  = tempInfo.win64_flags;
	ModuleInfo.switch_style = tempInfo.switch_style;

	/* set architecture */
	ModuleInfo.arch = MODULEARCH;

    DebugCmd( ModuleInfo.cref = TRUE ); /* enable debug displays */

#if 1 //def __SW_BD
    /* fatal errors during assembly won't terminate the program,
     * just the assembly step.!
     */
    if ( setjmp( jmpenv ) ) {
        if ( ModuleInfo.g.src_stack )
            ClearSrcStack(); /* avoid memory leaks! */
        goto done;
    }
#endif

    AssembleInit( source );
	
    starttime = clock();

#if 0 /* 1=trigger a protection fault */
    seg = NULL;
    seg->sym.state = SYM_UNDEFINED;
#endif

    for( Parse_Pass = PASS_1; ; Parse_Pass++ ) {

		ResetOrgFixup();

        DebugMsg(( "*************\npass %u\n*************\n", Parse_Pass + 1 ));
        OnePass();

        if( ModuleInfo.g.error_count > 0 ) {
            DebugMsg(("AssembleModule(%u): errorcnt=%u\n", Parse_Pass + 1, ModuleInfo.g.error_count ));
            break;
        }

        /* calculate total size of segments */
        for ( curr_written = 0, seg = SymTables[TAB_SEG].head; seg ; seg = seg->next ) {
            /* v2.04: use <max_offset> instead of <bytes_written>
             * (the latter is not always reliable due to backpatching).
             */
            curr_written += seg->sym.max_offset;
            DebugMsg(("AssembleModule(%u): segm=%-8s start=%8" I32_SPEC "X max_ofs=%8" I32_SPEC "X written=%" I32_SPEC "X\n",
                      Parse_Pass + 1, seg->sym.name, seg->e.seginfo->start_loc, seg->sym.max_offset,
                      seg->e.seginfo->bytes_written ));
        }

        /* if there's no phase error and size of segments didn't change, we're done */
        DebugMsg(("AssembleModule(%u): PhaseError=%u, prev_written=%" I32_SPEC "X, curr_written=%" I32_SPEC "X\n", Parse_Pass + 1, ModuleInfo.PhaseError, prev_written, curr_written));
        if( !ModuleInfo.PhaseError && prev_written == curr_written )
            break;

#ifdef DEBUG_OUT
        if ( curr_written < prev_written && prev_written != -1 ) {
            printf( "size shrank from %" I32_SPEC "X to %" I32_SPEC "X in pass %u\n", prev_written, curr_written, Parse_Pass + 1 );
        }
#endif

        DebugMsg(("AssembleModule(%u): prepare for next pass\n", Parse_Pass + 1));
        prev_written = curr_written;

        if ( Parse_Pass % 200 == 199 )
            EmitWarn( 2, ASSEMBLY_PASSES, Parse_Pass+1 );
#ifdef DEBUG_OUT
        if ( Options.max_passes && Parse_Pass == (Options.max_passes - 1) )
            break;
#endif
        if ( Options.line_numbers ) {
#if COFF_SUPPORT
            if ( Options.output_format == OFORMAT_COFF ) {
                for( seg = SymTables[TAB_SEG].head; seg; seg = seg->next ) {
                    if ( seg->e.seginfo->LinnumQueue )
                        QueueDeleteLinnum( seg->e.seginfo->LinnumQueue );
                    seg->e.seginfo->LinnumQueue = NULL;
                }
            } else {
#endif
                QueueDeleteLinnum( &LinnumQueue );
                LinnumQueue.head = NULL;
#if COFF_SUPPORT
            }
#endif
        }

        /* set file position of ASM and LST files for next pass */
        rewind( CurrFile[ASM] );

        if ( write_to_file && Options.output_format == OFORMAT_OMF )
            omf_set_filepos();

#if FASTPASS
        if ( UseSavedState == FALSE && CurrFile[LST] ) {
#else
        if ( CurrFile[LST] ) {
#endif
            rewind( CurrFile[LST] );
            LstInit();
        }
    } /* end for() */

    if ( ( Parse_Pass > PASS_1 ) && write_to_file )
        WriteModule( &ModuleInfo );

	if (Options.dumpSymbols)
		WriteSymbols();

    if ( ModuleInfo.pCodeBuff ) {
        LclFree( ModuleInfo.pCodeBuff );
    }
    DebugMsg(("AssembleModule: finished, cleanup\n"));

    /* Write a symbol listing file (if requested) */
    LstWriteCRef();

    endtime = clock(); /* is in ms already */

	if (Options.lessoutput)
	{
		if (ModuleInfo.g.warning_count == 0 && ModuleInfo.g.error_count == 0)
		{
			sprintf(CurrSource, MsgGetEx(MSG_ASSEMBLY_RESULTS_QUIETER),
				GetFNamePart(GetFName(ModuleInfo.srcfile)->fname)); // write normal string for listing.
			
			// console output.
			#ifdef _WIN32
				SetConsoleTextAttribute(hConsole, WIN_LTWHITE | (screenBufferInfo.wAttributes & 0xfff0) );
				printf( "%s", GetFNamePart(GetFName(ModuleInfo.srcfile)->fname));
				SetConsoleTextAttribute(hConsole, WIN_LTGREEN | (screenBufferInfo.wAttributes & 0xfff0) );
				printf(": ok\n");
				SetConsoleTextAttribute(hConsole, screenBufferInfo.wAttributes);
			#else
				printf(FWHT("%s"), GetFNamePart(GetFName(ModuleInfo.srcfile)->fname));
				printf(FGRN(": ok\n"));
			#endif
		}
		else
		{
			sprintf(CurrSource, MsgGetEx(MSG_ASSEMBLY_RESULTS_QUIET),
				GetFNamePart(GetFName(ModuleInfo.srcfile)->fname),
				ModuleInfo.g.warning_count,
				ModuleInfo.g.error_count); // write normal string for listing.

			// console output.
			#ifdef _WIN32
				SetConsoleTextAttribute(hConsole, WIN_LTWHITE | (screenBufferInfo.wAttributes & 0xfff0));
				printf("%s : ", GetFNamePart(GetFName(ModuleInfo.srcfile)->fname));
				SetConsoleTextAttribute(hConsole, WIN_YELLOW | (screenBufferInfo.wAttributes & 0xfff0));
				printf("%u warnings", ModuleInfo.g.warning_count);
				SetConsoleTextAttribute(hConsole, WIN_LTWHITE | (screenBufferInfo.wAttributes & 0xfff0));
				printf(", ");
				SetConsoleTextAttribute(hConsole, WIN_LTRED | (screenBufferInfo.wAttributes & 0xfff0));
				printf("%u errors\n", ModuleInfo.g.error_count);
				SetConsoleTextAttribute(hConsole, screenBufferInfo.wAttributes);
			#else
				printf(FWHT("%s : "), GetFNamePart(GetFName(ModuleInfo.srcfile)->fname));
				printf(FYEL("%u warnings"), ModuleInfo.g.warning_count);
				printf(", ");
				printf(FRED("%u errors"), ModuleInfo.g.error_count);
			#endif
		}
	}
	else
	{
		sprintf(CurrSource, MsgGetEx(MSG_ASSEMBLY_RESULTS),
			GetFName(ModuleInfo.srcfile)->fname,
			GetLineNumber(),
			Parse_Pass + 1,
			endtime - starttime,
			ModuleInfo.g.warning_count,
			ModuleInfo.g.error_count); // write normal string for listing.

		// console output. "%s: %lu lines, %u passes, %u ms, %u warnings, %u errors"
		if (Options.quiet == FALSE)
		{
			#ifdef _WIN32
						SetConsoleTextAttribute(hConsole, WIN_LTWHITE | (screenBufferInfo.wAttributes & 0xfff0));
						printf("%s: %lu lines, ", GetFNamePart(GetFName(ModuleInfo.srcfile)->fname), GetLineNumber());
						SetConsoleTextAttribute(hConsole, WIN_LTGREEN | (screenBufferInfo.wAttributes & 0xfff0));
						printf("%u passes", Parse_Pass + 1);
						SetConsoleTextAttribute(hConsole, WIN_LTWHITE | (screenBufferInfo.wAttributes & 0xfff0));
						printf(", ");
						SetConsoleTextAttribute(hConsole, WIN_CYAN | (screenBufferInfo.wAttributes & 0xfff0));
						printf("%u ms", endtime - starttime);
						SetConsoleTextAttribute(hConsole, WIN_LTWHITE | (screenBufferInfo.wAttributes & 0xfff0));
						printf(", ");
						SetConsoleTextAttribute(hConsole, WIN_LTYELLOW | (screenBufferInfo.wAttributes & 0xfff0));
						printf("%u warnings", ModuleInfo.g.warning_count);
						SetConsoleTextAttribute(hConsole, WIN_LTWHITE | (screenBufferInfo.wAttributes & 0xfff0));
						printf(", ");
						SetConsoleTextAttribute(hConsole, WIN_LTRED | (screenBufferInfo.wAttributes & 0xfff0));
						printf("%u errors\n", ModuleInfo.g.error_count);
						SetConsoleTextAttribute(hConsole, screenBufferInfo.wAttributes);
			#else
						printf(FWHT("%s: %u lines, "), GetFNamePart(GetFName(ModuleInfo.srcfile)->fname), GetLineNumber());
						printf(FGRN("%u passes"), Parse_Pass + 1);
						printf(", ");
						printf(FCYN("%u ms"), endtime - starttime);
						printf(", ");
						printf(FYEL("%u warnings"), ModuleInfo.g.warning_count);
						printf(", ");
						printf(FRED("%u errors\n"), ModuleInfo.g.error_count);
 			#endif
		}
	}
	if (Options.quiet == FALSE)
	{
		fflush(stdout); /* Force flush of each modules assembly progress */
	}
    if ( CurrFile[LST] ) {
        LstPrintf( CurrSource );
        LstNL();
    }
done:
   
	AssembleFini();
	ResetOrgFixup();
    DebugMsg(("AssembleModule exit\n"));
    return( ModuleInfo.g.error_count == 0 );
}

/* ARCH SSE/AVX specific instructions */
const char* MOVE_ALIGNED_FLOAT()
{
	if (MODULEARCH == ARCH_AVX) return "vmovaps"; else return "movaps";
}

const char* MOVE_ALIGNED_INT()
{
	if (MODULEARCH == ARCH_AVX) return "vmovdqa"; else return "movdqa";
}

const char* MOVE_UNALIGNED_FLOAT()
{
	if (MODULEARCH == ARCH_AVX) return "vmovups"; else return "movups";
}

const char* MOVE_UNALIGNED_INT()
{
	if (MODULEARCH == ARCH_AVX) return "vmovdqu"; else return "movdqu";
}

const char* MOVE_SINGLE()
{
	if (MODULEARCH == ARCH_AVX) return "vmovss"; else return "movss";
}

const char* MOVE_DOUBLE()
{
	if (MODULEARCH == ARCH_AVX) return "vmovsd"; else return "movsd";
}

const char* MOVE_SIMD_DWORD()
{
	if (MODULEARCH == ARCH_AVX) return "vmovd"; else return "movd";
}

const char* MOVE_SIMD_QWORD()
{
	if (MODULEARCH == ARCH_AVX) return "vmovq"; else return "movq";
}
