/****************************************************************************
*
*  This code is Public Domain.
*
*  ========================================================================
*
* Description:  macro handling.
*
* functions:
* - CreateMacro      create a macro item
* - ReleaseMacroData used to redefine/purge a macro
* - StoreMacro       store a macro's parameter/local/line list
* - MacroDir         handle MACRO directive
* - PurgeDirective   handle PURGE directive
* - MacroInit        global macro initialization, set predefined macros
*
****************************************************************************/

#include <ctype.h>

#include "globals.h"
#include "memalloc.h"
#include "parser.h"
#include "input.h"
#include "tokenize.h"
#include "macro.h"
#include "fastpass.h"
#include "listing.h"
#include "equate.h"
/* a placeholder consists of escape char (0x0a) + index (1 byte).
 if this is to change, function fill_placeholders() must
 be adjusted!
 */
#define PLACEHOLDER_SIZE 2

#define MAX_PLACEHOLDERS 256

/* store empty macro lines, to ensure correct line numbering
 */
#define STORE_EMPTY_LINES 1

/* 1="undefine" macros with PURGE - this isn't Masm-compatible,
 * and offers no real benefit because the name remains in the namespace.
 * The macro is marked "undefined" and cannot be invoked anymore.
 * 0=just delete the macro content.
 */
#define TRUEPERGE 0

extern int  MacroLocals;

bool inMacroBody = FALSE;

/* the list of macro param + local names is hold temporarily only.
 * once the names have been replaced by placeholders,
 * the list is superfluous. What's stored permanently
 * in the macro item is the number of params and locals only.
 */
struct mname_list {
    char        *label;         /* name of param/local */
    uint_16     len;
};

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

/* Replace placeholders in a stored macro source line with values of actual
 * parameters and locals. A placeholder consists of escape char 0x0a,
 * followed by a one-byte index field.
 */
void fill_placeholders( char *dst, const char *src, unsigned argc, unsigned localstart, char *argv[] )
/****************************************************************************************************/
{
    uint_32 i;
    const char *p;
    unsigned parmno;

    /* scan the string, replace the placeholders #nn */
    for( p = src ;*p != NULLC; ) {
        if (*p == PLACEHOLDER_CHAR ) {
            p++;
            /* we found a placeholder, get the index part! */
            parmno = *(unsigned char *)p - 1; /* index is one-based! */
            p++;
            /* if parmno > argc, then it's a macro local */
            if ( parmno >= argc ) {
                *dst++ = '?';
                *dst++ = '?';
                i = localstart + parmno - argc;
                if ( i > 0xFFFF ) {
                    i = sprintf( dst, "%X", i );
                    dst += i;
                } else {
                    *dst++ = HexDigit( i >> 12 );
                    *dst++ = HexDigit( i >> 8 );
                    *dst++ = HexDigit( i >> 4 );
                    *dst++ = HexDigit( i );
                }
            } else if ( argv[parmno] ) {  /* actual parameter might be empty (=NULL) */
                i = strlen( argv[parmno] );
                memcpy( dst, argv[parmno], i );
                dst += i;
            }
        } else {
            *dst++ = *p++;
        }
    }
    *dst = NULLC;
    return;
}

static char * replace_parm( const char *line, char *start, int len, struct mname_list *mnames )
/*********************************************************************************************/
{
    /* scan list of macro paras/locals if current word is found.
     * - line: current line
     * - start: start 'current word' in line
     * - len: size current word
     * - mnames: list of macro params+locals
     * if found, the 'current word' is replaced by a placeholder.
     * format of placeholders is <placeholder_char><index>
     * <placeholder_char> is an escape character whose hex-code is
     * "impossible" to occur in a source line, <index> has type uint_8,
     * value 00 isn't used - this restricts the total of parameters
     * and locals for a macro to 255.
     */
    char       *rest;
    unsigned   count;

    for( count = 1; mnames->label; count++, mnames++ ) {
        if( mnames->len == len && SymCmpFunc( start, mnames->label, len ) == 0 ) {

            /* found a macro parameter/local! */

            if ( count >= MAX_PLACEHOLDERS ) {
                EmitError( TOO_MANY_MACRO_PLACEHOLDERS );
                break;
            }

            /* handle substitution operator '&' */
            rest = start + len;
            if ( start != line && *(start-1) == '&' )
                start--;
            if (*rest == '&')
                rest++;

            *start++ = PLACEHOLDER_CHAR;

            /* additional space needed for the placeholder? */
            if ( start >= rest ) {
                char *end = rest + strlen(rest);
                char *dst = end + 1;
                while ( end >= rest )
                    *dst-- = *end--;
                *start = count;
            } else {
                *start++ = count;
                /* v2.10: strcpy should not be used if strings overlap */
                //strcpy( start, rest );
                memmove( start, rest, strlen( rest) + 1 );
            }
            return( start ); /* word has been replaced */
        }
    }
    return( NULL );
}

static int store_placeholders( char *line, struct mname_list *mnames )
/********************************************************************/
{
    /* scan a macro source line for parameter and local names.
     * - line: the source line
     * - mnames: list of macro params + locals
     * if a param/local is found, replace the name by a 2-byte placeholder.
     */
    char *p;
    char *start;
    char quote = NULLC;
    int brlevel = 0;
    int params = 0; /* number of replacements in this line */
    int qlevel = 0;
    bool substprf;  /* substitution character before ID? */

    for( p = line; *p != NULLC; ) {
        if ( isldigit(*p) ) {
            /* skip numbers (they may contain alphas)
             * this is not exactly what masm does. Masm
             * stops at the first non-digit.
             */
            while ( is_valid_id_char( *p )) p++;
        } else if ( ( is_valid_id_char( *p ) ) ||
                   ( *p == '.' &&
                    ModuleInfo.dotname &&
                    //is_valid_id_char(*(p+1)) && /* v2.05: masm allows a single dot as param/local name */
                    ( p == line ||
                     ( *(p-1) != ']' && ( is_valid_id_char( *(p-1) ) == FALSE ) ) ) ) ) {
            DebugMsg1(("store_placeholders: found ID: %s\n", p));
            start = p++;
            while ( is_valid_id_char( *p )) p++;
            /* v2.08: both a '&' before AND after the name trigger substitution (and disappear) */
            substprf = ( ( start > line && *(start-1) == '&') || *p == '&' );
            if ( quote == NULLC || substprf ) {
                /* look for this word in the macro parms, and replace it if it is */
                if ( start = replace_parm( line, start, p - start, mnames ) ) {
                    params++;
                    p = start;
                }
            }
        } else {
            switch (*p) {
            case '!':
                /* v2.11: skip next char only if it is a "special" one; see expans40.asm */
                //if ( quote == NULLC && *(p+1) != NULLC )
                if ( quote == NULLC && strchr( "<>\"'", *(p+1) ) )
                    p++;
                break;
            case '<':
                brlevel++;
                break;
            case '>':
                if (brlevel) {
                    if (qlevel == brlevel)
                        quote = NULLC;
                    brlevel--;
                }
                break;
            case '"':
            case '\'':
                if ( quote ) {
                    if ( quote == *p )
                        quote = NULLC;
                } else {
                    quote = *p;
                    qlevel = brlevel;
                }
            }
            p++;
        }
    }
    return( params );
}

#ifdef DEBUG_OUT
char *RenderMacroLine( const char *src )
/**************************************/
{
    /* a macro line cannot be displayed directly due to the format of
     * the index field. for debug log, convert it to a readable format.
     */
    char *dst;
    static char buffer[MAX_LINE_LEN]; /* debug only */

    for ( dst = buffer; *src; src++, dst++ ) {
        if (*src == PLACEHOLDER_CHAR ) {
            *dst++ = '#';
            src++;
            *dst = *src / 16 + '0';
            if (*dst > '9')
                *dst += 7;
            dst++;
            *dst = *src % 16 + '0';
            if (*dst > '9')
                *dst += 7;
        } else
            *dst = *src;
    }
    *dst = NULLC;
    return( buffer );
}
#endif

/*
 * store a macro's parameter, local and content list.
 * i = start index of macro params in token buffer.
 */

ret_code StoreMacro( struct dsym *macro, int i, struct asm_tok tokenarray[], bool store_data )
/********************************************************************************************/
{
    struct macro_info   *info;
    char                *src;
    char                *token;
    int                 mindex;
    struct mparm_list   *paranode;
    struct srcline      **nextline;
#ifdef DEBUG_OUT
    int lineno = 0;
#endif
    unsigned            nesting_depth = 0;
    bool                locals_done;
    struct line_status  ls;
    struct asm_tok      tok[2];
    struct mname_list   mnames[MAX_PLACEHOLDERS]; /* there are max 255 placeholders */
    char                buffer[MAX_LINE_LEN];

    DebugMsg1(("StoreMacro(%s, i=%u, store_data=%u) enter, params=>%s<\n", macro->sym.name, i, store_data, tokenarray[i].tokpos ));
    info = macro->e.macroinfo;

    if( store_data ) {
        int j;

        if ( i < Token_Count ) {
            for ( j = i, info->parmcnt = 1; j < Token_Count; j++ )
                if ( tokenarray[j].token == T_COMMA )
                    info->parmcnt++;
            info->parmlist = LclAlloc( info->parmcnt * sizeof(struct mparm_list));
        } else {
            info->parmcnt = 0;
            info->parmlist = NULL;
        }

        for( paranode = info->parmlist, mindex = 0; i < Token_Count ; paranode++ ) {

            token = tokenarray[i].string_ptr;
            /* Masm accepts reserved words and instructions as parameter
             * names! So just check that the token is a valid id.
             */
            if ( !is_valid_id_first_char( *token ) || tokenarray[i].token == T_STRING ) {
                EmitErr( SYNTAX_ERROR_EX, token );
                break;
            } else if ( tokenarray[i].token != T_ID )
                EmitWarn( 4, PARAM_IS_RESERVED_WORD, tokenarray[i].string_ptr );

            paranode->deflt = NULL;
            paranode->required = FALSE;

            /* first get the parm. name */
            j = strlen( token );
            mnames[mindex].label = token;
            mnames[mindex].len = j;
            mindex++;
            mnames[mindex].label = NULL; /* init next entry */
            i++;

            /* now see if it has a default value or is required */
            if( tokenarray[i].token == T_COLON ) {
                i++;
                if( tokenarray[i].token == T_DIRECTIVE && tokenarray[i].dirtype == DRT_EQUALSGN ) {
                    i++;
                    /* allowed syntax is parm:=<literal> */
                    if( tokenarray[i].token != T_STRING || tokenarray[i].string_delim != '<' ) {
                        EmitError( LITERAL_EXPECTED_AFTER_EQ );
                        break; // return( ERROR );
                    }
                    paranode->deflt = LclAlloc( tokenarray[i].stringlen + 1 );
                    memcpy( paranode->deflt, tokenarray[i].string_ptr, tokenarray[i].stringlen + 1 );
                    i++;
                } else if( _stricmp( tokenarray[i].string_ptr, "REQ" ) == 0 ) {
                    /* required parameter */
                    paranode->required = TRUE;
                    i++;
                } else if( tokenarray[i].token == T_RES_ID && tokenarray[i].tokval == T_VARARG ) {
                    /* more parameters can follow */
                    macro->sym.mac_vararg = TRUE;
                    if ( tokenarray[i+1].token != T_FINAL ) {
                        EmitError( VARARG_PARAMETER_MUST_BE_LAST );
                        break;
                    }
                    i++;
#if MACROLABEL
                } else if( tokenarray[i].token == T_DIRECTIVE &&
                          tokenarray[i].tokval == T_LABEL &&
                          Options.strict_masm_compat == FALSE ) { /* parm:LABEL? */
                    /* LABEL attribute for first param only! */
                    if ( paranode != info->parmlist ) {
                        EmitError( LABEL_PARAMETER_MUST_BE_FIRST );
                        break;
                    }
                    macro->sym.label = TRUE;
                    i++;
#endif
#if VARARGML
                } else if( _stricmp( tokenarray[i].string_ptr, "VARARGML" ) == 0 ) {
                    /* more parameters can follow, multi lines possible */
                    macro->sym.mac_vararg = TRUE;
                    macro->sym.mac_multiline = TRUE;
                    if ( tokenarray[i+1].token != T_FINAL ) {
                        EmitError( VARARG_PARAMETER_MUST_BE_LAST );
                        break;
                    }
                    i++;
#endif
                } else {
                    EmitErr( SYNTAX_ERROR_EX, tokenarray[i].string_ptr );
                    break;
                }
            }
            DebugMsg1(("StoreMacro(%s): param=>%s< found\n", macro->sym.name, mnames[mindex].label));
            if( i < Token_Count && tokenarray[i].token != T_COMMA ) {
                EmitErr( SYNTAX_ERROR_EX, tokenarray[i].tokpos );
                break; // return( ERROR );
            }
            /* go past comma */
            i++;

        } /* end for() */
        DebugMsg1(("StoreMacro(%s): macro parameters done\n", macro->sym.name));
    }

    locals_done = FALSE;
    nextline = &info->data;

    /* now read in the lines of the macro, and store them if store_data is TRUE */
    for( ; ; ) {
        char *ptr;

        src = GetTextLine( buffer );
        if( src == NULL ) {
            /* v2.11: fatal error if source ends without an ENDM found */
            Fatal( UNMATCHED_MACRO_NESTING );
        }

        /* add the macro line to the listing file */
        /* v2.09: don't make listing depend on store_data */
        if ( ModuleInfo.list ) {
            ModuleInfo.line_flags &= ~LOF_LISTED;
            LstWrite( LSTTYPE_MACROLINE, 0, buffer );
        }
        ls.input = src;
        ls.start = src;
        ls.index = 0;
    continue_scan:
        while ( isspace(*ls.input) ) ls.input++;

        /* skip empty lines! */
        if ( *ls.input == NULLC || *ls.input == ';' ) {
#if STORE_EMPTY_LINES
            if( store_data ) {
                *nextline = LclAlloc( sizeof( struct srcline ) );
                (*nextline)->next = NULL;
                (*nextline)->ph_count = 0;
                (*nextline)->line[0] = NULLC;
                nextline = &(*nextline)->next;
            }
#endif
            continue;
        }

        /* get first token */
        ls.output = StringBufferEnd;
        ls.flags = TOK_DEFAULT;
        ls.flags2 = 0;
        tok[0].token = T_FINAL;
        if ( GetToken( &tok[0], &ls ) == ERROR )
            return( ERROR );

        /* v2.05: GetTextLine() doesn't concat lines anymore.
         * So if a backslash is found in the current source line,
         * tokenize it to get possible concatenated lines.
         */
        if ( strchr( ls.input, '\\' ) ) {
            ptr = ls.input;
            while ( *ls.input && *ls.input != ';' ) {
                ls.flags3 = 0;
                GetToken( &tok[1], &ls );
                /* v2.09: don't query store_data */
                if ( ( ls.flags3 & TF3_ISCONCAT ) && ModuleInfo.list ) {
                    ModuleInfo.line_flags &= ~LOF_LISTED;
                    LstWrite( LSTTYPE_MACROLINE, 0, ls.input );
                }
                while( isspace( *ls.input ) ) ls.input++;
            }
            ls.input = ptr;
        }
        if ( tok[0].token == T_FINAL ) {/* did GetToken() return EMPTY? */
            DebugMsg1(("StoreMacro(%s): no token\n", macro->sym.name));
            goto continue_scan;
        }
        /* handle LOCAL directive(s) */
        if( locals_done == FALSE && tok[0].token == T_DIRECTIVE && tok[0].tokval == T_LOCAL ) {
            if( !store_data )
                continue;
            for ( ;; ) {
                int size;
                while( isspace( *ls.input ) ) ls.input++;
                if ( *ls.input == NULLC || *ls.input == ';' ) /* 0 locals are ok */
                    break;
                ls.output = StringBufferEnd;
                GetToken( &tok[0], &ls );
                if ( !is_valid_id_first_char( *StringBufferEnd ) ) {
                    EmitErr( SYNTAX_ERROR_EX, StringBufferEnd );
                    break;
                } else if ( tok[0].token != T_ID )
                    EmitWarn( 4, PARAM_IS_RESERVED_WORD, StringBufferEnd );

                if ( mindex == ( MAX_PLACEHOLDERS - 1 ) ) {
                    EmitError( TOO_MANY_MACRO_PLACEHOLDERS );
                    break;
                }
                size = strlen( StringBufferEnd );
                mnames[mindex].label = myalloca( size );
                memcpy( mnames[mindex].label, StringBufferEnd, size );
                mnames[mindex].len = size;
                mindex++;
                mnames[mindex].label = NULL; /* mark end of placeholder array */
                info->localcnt++;
                DebugMsg1(("StoreMacro(%s, %u): local=>%s< added, rest=%s\n", macro->sym.name, nesting_depth, mnames[mindex].label, ls.input ));
                while( isspace( *ls.input ) ) ls.input++;
                if ( *ls.input == ',' ) {
                    ls.input++;
                } else if ( is_valid_id_first_char( *ls.input ) ) {
                    EmitErr( SYNTAX_ERROR_EX, ls.input );
                    break;
                }
            }
            continue;
        }
        locals_done = TRUE;

        /* handle macro labels, EXITM, ENDM and macro loop directives.
         * this must be done always, even if store_data is false,
         * to find the matching ENDM that terminates the macro.
         */
        if ( tok[0].token == T_COLON ) { /* macro label? */
            /* skip leading spaces for macro labels! In RunMacro(),
             * the label search routine expects no spaces before ':'.
             */
            src = ls.input - 1;
        } else if( tok[0].token == T_DIRECTIVE ) {
            if ( tok[0].tokval == T_EXITM ) {
                DebugMsg1(("StoreMacro(%s): exitm found, lvl=%u, >%s<\n", macro->sym.name, nesting_depth, ls.input ));
                if ( nesting_depth == 0 ) {
                    ptr = ls.input;
                    while( isspace( *ptr ) ) ptr++;
                    if ( *ptr && *ptr != ';' )
                        macro->sym.isfunc = TRUE;
                    //macro->sym.runsync = TRUE;
                }
            } else if( tok[0].tokval == T_ENDM ) {
                DebugMsg1(("StoreMacro(%s): endm found, lvl=%u\n", macro->sym.name, nesting_depth ));
                if( nesting_depth ) 
				{
                    nesting_depth--;
					if (nesting_depth == 0)
					{
						inMacroBody = FALSE; /* flag that we're finished processing a macro UASM 2.46.6 */
					}

                } else {
					inMacroBody = FALSE; /* flag that we're finished processing a macro UASM 2.46.6 */
                    break; /* exit the for() loop */
                }
            } else if( tok[0].dirtype == DRT_LOOPDIR ) {
                nesting_depth++; /* FOR[C], IRP[C], REP[EA]T, WHILE */
            }
        } else if ( tok[0].token != T_INSTRUCTION || *ls.input == '&' ) {
            /* Skip any token != directive or instruction (and no '&' attached)
             * might be text macro ids, macro function calls,
             * code labels, ...
             */
            for (;;) {
                char oldc;
                tok[0].token = T_FINAL;
                while ( islspace( *ls.input ) ) ls.input++;
                if ( *ls.input == NULLC || *ls.input == ';' )
                    break;
                oldc = *(ls.input-1);
                if ( GetToken( &tok[0], &ls ) == ERROR )
                    break;
                if ( ( tok[0].token == T_INSTRUCTION || tok[0].token == T_DIRECTIVE ) &&
                    oldc != '&' && *ls.input != '&' )
                    break;
            }
            if ( tok[0].token == T_DIRECTIVE ) {
                /* MACRO or loop directive? */
                if ( tok[0].tokval == T_MACRO || tok[0].dirtype == DRT_LOOPDIR )
                    nesting_depth++;
            }
        }

        /* store the line, but first check for placeholders!
         * this is to be improved. store_placeholders() is too
         * primitive. It's necessary to use the tokenizer.
         */
        if( store_data ) {
            int j;
            uint_8 phs = 0;
            if ( mindex )
                phs = store_placeholders( src, mnames );
            j = strlen( src );
            *nextline = LclAlloc( sizeof( struct srcline ) + j );
            (*nextline)->next = NULL;
            (*nextline)->ph_count = phs;
            memcpy( (*nextline)->line, src, j + 1 );
            nextline = &(*nextline)->next;
            DebugMsg1(("StoreMacro(%s, %u): cnt=%u, %u. line >%s<\n", macro->sym.name, nesting_depth, phs, ++lineno, RenderMacroLine( src ) ));
        }
    } /* end for */
    macro->sym.isdefined = TRUE;
    macro->sym.purged = FALSE;
    DebugMsg1(("StoreMacro(%s): exit, no error, isfunc=%u\n", macro->sym.name, macro->sym.isfunc));
    return( NOT_ERROR );
}

/* Used to compile built-in macros */
ret_code StoreAutoMacro(struct dsym *macro, int i, struct asm_tok tokenarray[], bool store_data, char *macCode[], int macLine, int macLen)
/********************************************************************************************/
{
	struct macro_info   *info;
	char                *src;
	char                *token;
	int                 mindex;
	struct mparm_list   *paranode;
	struct srcline      **nextline;
#ifdef DEBUG_OUT
	int lineno = 0;
#endif
	unsigned            nesting_depth = 0;
	bool                locals_done;
	struct line_status  ls;
	struct asm_tok      tok[2];
	struct mname_list   mnames[MAX_PLACEHOLDERS]; /* there are max 255 placeholders */
	char                buffer[MAX_LINE_LEN];
	int					macCurLine = macLine;
	int					macCnt = macLen;

	DebugMsg1(("StoreAutoMacro(%s, i=%u, store_data=%u) enter, params=>%s<\n", macro->sym.name, i, store_data, tokenarray[i].tokpos));
	info = macro->e.macroinfo;

	if (store_data) {
		int j;

		if (i < Token_Count) {
			for (j = i, info->parmcnt = 1; j < Token_Count; j++)
				if (tokenarray[j].token == T_COMMA)
					info->parmcnt++;
			info->parmlist = LclAlloc(info->parmcnt * sizeof(struct mparm_list));
		}
		else {
			info->parmcnt = 0;
			info->parmlist = NULL;
		}

		for (paranode = info->parmlist, mindex = 0; i < Token_Count; paranode++) {

			token = tokenarray[i].string_ptr;
			/* Masm accepts reserved words and instructions as parameter
			* names! So just check that the token is a valid id.
			*/
			if (!is_valid_id_first_char(*token) || tokenarray[i].token == T_STRING) {
				EmitErr(SYNTAX_ERROR_EX, token);
				break;
			}
			else if (tokenarray[i].token != T_ID)
				EmitWarn(4, PARAM_IS_RESERVED_WORD, tokenarray[i].string_ptr);

			paranode->deflt = NULL;
			paranode->required = FALSE;

			/* first get the parm. name */
			j = strlen(token);
			mnames[mindex].label = token;
			mnames[mindex].len = j;
			mindex++;
			mnames[mindex].label = NULL; /* init next entry */
			i++;

			/* now see if it has a default value or is required */
			if (tokenarray[i].token == T_COLON) {
				i++;
				if (tokenarray[i].token == T_DIRECTIVE && tokenarray[i].dirtype == DRT_EQUALSGN) {
					i++;
					/* allowed syntax is parm:=<literal> */
					if (tokenarray[i].token != T_STRING || tokenarray[i].string_delim != '<') {
						EmitError(LITERAL_EXPECTED_AFTER_EQ);
						break; // return( ERROR );
					}
					paranode->deflt = LclAlloc(tokenarray[i].stringlen + 1);
					memcpy(paranode->deflt, tokenarray[i].string_ptr, tokenarray[i].stringlen + 1);
					i++;
				}
				else if (_stricmp(tokenarray[i].string_ptr, "REQ") == 0) {
					/* required parameter */
					paranode->required = TRUE;
					i++;
				}
				else if (tokenarray[i].token == T_RES_ID && tokenarray[i].tokval == T_VARARG) {
					/* more parameters can follow */
					macro->sym.mac_vararg = TRUE;
					if (tokenarray[i + 1].token != T_FINAL) {
						EmitError(VARARG_PARAMETER_MUST_BE_LAST);
						break;
					}
					i++;
#if MACROLABEL
				}
				else if (tokenarray[i].token == T_DIRECTIVE &&
					tokenarray[i].tokval == T_LABEL &&
					Options.strict_masm_compat == FALSE) { /* parm:LABEL? */
														   /* LABEL attribute for first param only! */
					if (paranode != info->parmlist) {
						EmitError(LABEL_PARAMETER_MUST_BE_FIRST);
						break;
					}
					macro->sym.label = TRUE;
					i++;
#endif
#if VARARGML
				}
				else if (_stricmp(tokenarray[i].string_ptr, "VARARGML") == 0) {
					/* more parameters can follow, multi lines possible */
					macro->sym.mac_vararg = TRUE;
					macro->sym.mac_multiline = TRUE;
					if (tokenarray[i + 1].token != T_FINAL) {
						EmitError(VARARG_PARAMETER_MUST_BE_LAST);
						break;
					}
					i++;
#endif
				}
				else {
					EmitErr(SYNTAX_ERROR_EX, tokenarray[i].string_ptr);
					break;
				}
			}
			DebugMsg1(("StoreMacro(%s): param=>%s< found\n", macro->sym.name, mnames[mindex].label));
			if (i < Token_Count && tokenarray[i].token != T_COMMA) {
				EmitErr(SYNTAX_ERROR_EX, tokenarray[i].tokpos);
				break; // return( ERROR );
			}
			/* go past comma */
			i++;

		} /* end for() */
		DebugMsg1(("StoreMacro(%s): macro parameters done\n", macro->sym.name));
	}

	locals_done = FALSE;
	nextline = &info->data;

	/* now read in the lines of the macro, and store them if store_data is TRUE */
	while (macCnt-- >= 0) {
		char *ptr;

		src = macCode[macCurLine++];
		if (src == NULL) {
			/* v2.11: fatal error if source ends without an ENDM found */
			//EmitError( UNMATCHED_MACRO_NESTING );
			//ModuleInfo.EndDirFound = TRUE; /* avoid error "END not found" */
			//return( ERROR );
			Fatal(UNMATCHED_MACRO_NESTING);
		}

		/* add the macro line to the listing file */
		/* v2.09: don't make listing depend on store_data */
		//if ( ModuleInfo.list && store_data ) {
		if (ModuleInfo.list) {
			ModuleInfo.line_flags &= ~LOF_LISTED;
			LstWrite(LSTTYPE_MACROLINE, 0, buffer);
		}
		ls.input = src;
		ls.start = src;
		ls.index = 0;
	continue_scan:
		while (isspace(*ls.input)) ls.input++;

		/* skip empty lines! */
		if (*ls.input == NULLC || *ls.input == ';') {
#if STORE_EMPTY_LINES
			if (store_data) {
				*nextline = LclAlloc(sizeof(struct srcline));
				(*nextline)->next = NULL;
				(*nextline)->ph_count = 0;
				(*nextline)->line[0] = NULLC;
				nextline = &(*nextline)->next;
			}
#endif
			continue;
		}

		/* get first token */
		ls.output = StringBufferEnd;
		//ls.last_token = T_FINAL;
		ls.flags = TOK_DEFAULT;
		ls.flags2 = 0;
		tok[0].token = T_FINAL;
		if (GetToken(&tok[0], &ls) == ERROR)
			return(ERROR);

		/* v2.05: GetTextLine() doesn't concat lines anymore.
		* So if a backslash is found in the current source line,
		* tokenize it to get possible concatenated lines.
		*/
		if (strchr(ls.input, '\\')) {
			ptr = ls.input;
			while (*ls.input && *ls.input != ';') {
				ls.flags3 = 0;
				GetToken(&tok[1], &ls);
				/* v2.09: don't query store_data */
				//if ( ( ls.flags3 & TF3_ISCONCAT ) && ModuleInfo.list && store_data ) {
				if ((ls.flags3 & TF3_ISCONCAT) && ModuleInfo.list) {
					ModuleInfo.line_flags &= ~LOF_LISTED;
					LstWrite(LSTTYPE_MACROLINE, 0, ls.input);
				}
				while (isspace(*ls.input)) ls.input++;
			}
			ls.input = ptr;
		}
		if (tok[0].token == T_FINAL) {/* did GetToken() return EMPTY? */
			DebugMsg1(("StoreMacro(%s): no token\n", macro->sym.name));
			goto continue_scan;
		}
		/* handle LOCAL directive(s) */
		if (locals_done == FALSE && tok[0].token == T_DIRECTIVE && tok[0].tokval == T_LOCAL) {
			if (!store_data)
				continue;
			for (;; ) {
				int size;
				while (isspace(*ls.input)) ls.input++;
				if (*ls.input == NULLC || *ls.input == ';') /* 0 locals are ok */
					break;
				ls.output = StringBufferEnd;
				GetToken(&tok[0], &ls);
				if (!is_valid_id_first_char(*StringBufferEnd)) {
					EmitErr(SYNTAX_ERROR_EX, StringBufferEnd);
					break;
				}
				else if (tok[0].token != T_ID)
					EmitWarn(4, PARAM_IS_RESERVED_WORD, StringBufferEnd);

				if (mindex == (MAX_PLACEHOLDERS - 1)) {
					EmitError(TOO_MANY_MACRO_PLACEHOLDERS);
					break;
				}
				size = strlen(StringBufferEnd);
				mnames[mindex].label = myalloca(size);
				memcpy(mnames[mindex].label, StringBufferEnd, size);
				mnames[mindex].len = size;
				mindex++;
				mnames[mindex].label = NULL; /* mark end of placeholder array */
				info->localcnt++;
				DebugMsg1(("StoreMacro(%s, %u): local=>%s< added, rest=%s\n", macro->sym.name, nesting_depth, mnames[mindex].label, ls.input));
				while (isspace(*ls.input)) ls.input++;
				if (*ls.input == ',') {
					ls.input++;
				}
				else if (is_valid_id_first_char(*ls.input)) {
					EmitErr(SYNTAX_ERROR_EX, ls.input);
					break;
				}
			}
			continue;
		}
		locals_done = TRUE;

		/* handle macro labels, EXITM, ENDM and macro loop directives.
		* this must be done always, even if store_data is false,
		* to find the matching ENDM that terminates the macro.
		*/
		if (tok[0].token == T_COLON) { /* macro label? */
									   /* skip leading spaces for macro labels! In RunMacro(),
									   * the label search routine expects no spaces before ':'.
									   */
			src = ls.input - 1;
		}
		else if (tok[0].token == T_DIRECTIVE) {
			if (tok[0].tokval == T_EXITM) {
				DebugMsg1(("StoreMacro(%s): exitm found, lvl=%u, >%s<\n", macro->sym.name, nesting_depth, ls.input));
				if (nesting_depth == 0) {
					ptr = ls.input;
					while (isspace(*ptr)) ptr++;
					if (*ptr && *ptr != ';')
						macro->sym.isfunc = TRUE;
					//macro->sym.runsync = TRUE;
				}
			}
			else if (tok[0].tokval == T_ENDM) {
				DebugMsg1(("StoreMacro(%s): endm found, lvl=%u\n", macro->sym.name, nesting_depth));
				if (nesting_depth) {
					nesting_depth--;
				}
				else {
					break; /* exit the for() loop */
				}
			}
			else if (tok[0].dirtype == DRT_LOOPDIR) {
				nesting_depth++; /* FOR[C], IRP[C], REP[EA]T, WHILE */
			}
		}
		else if (tok[0].token != T_INSTRUCTION || *ls.input == '&') {
			/* Skip any token != directive or instruction (and no '&' attached)
			* might be text macro ids, macro function calls,
			* code labels, ...
			*/
			for (;;) {
				char oldc;
				tok[0].token = T_FINAL;
				while (isspace(*ls.input)) ls.input++;
				if (*ls.input == NULLC || *ls.input == ';')
					break;
				oldc = *(ls.input - 1);
				if (GetToken(&tok[0], &ls) == ERROR)
					break;
				if ((tok[0].token == T_INSTRUCTION || tok[0].token == T_DIRECTIVE) &&
					oldc != '&' && *ls.input != '&')
					break;
			}
			if (tok[0].token == T_DIRECTIVE) {
				/* MACRO or loop directive? */
				if (tok[0].tokval == T_MACRO || tok[0].dirtype == DRT_LOOPDIR)
					nesting_depth++;
			}
		}

		/* store the line, but first check for placeholders!
		* this is to be improved. store_placeholders() is too
		* primitive. It's necessary to use the tokenizer.
		*/
		if (store_data) {
			int j;
			uint_8 phs = 0;
			if (mindex)
				phs = store_placeholders(src, mnames);
			j = strlen(src);
			*nextline = LclAlloc(sizeof(struct srcline) + j);
			(*nextline)->next = NULL;
			(*nextline)->ph_count = phs;
			memcpy((*nextline)->line, src, j + 1);
			nextline = &(*nextline)->next;
			DebugMsg1(("StoreMacro(%s, %u): cnt=%u, %u. line >%s<\n", macro->sym.name, nesting_depth, phs, ++lineno, RenderMacroLine(src)));
		}
	} /* end for */
	macro->sym.isdefined = TRUE;
	macro->sym.purged = FALSE;
	DebugMsg1(("StoreMacro(%s): exit, no error, isfunc=%u\n", macro->sym.name, macro->sym.isfunc));
	return(NOT_ERROR);
}

/* create a macro symbol */
struct dsym *CreateMacro( const char *name )
/******************************************/
{
    struct dsym *macro;
    if ( macro = (struct dsym *)SymCreate( name ) ) {
        macro->sym.state = SYM_MACRO;
        macro->e.macroinfo = LclAlloc( sizeof( struct macro_info ) );
        macro->e.macroinfo->parmcnt  = 0;
        macro->e.macroinfo->localcnt = 0;
        macro->e.macroinfo->parmlist = NULL;
        macro->e.macroinfo->data     = NULL;
#ifdef DEBUG_OUT
        macro->e.macroinfo->count = 0;
#endif
        macro->e.macroinfo->srcfile  = 0;
        macro->sym.mac_vararg = FALSE;
        macro->sym.isfunc = FALSE;
    }
    return( macro );
}

/* clear macro data */
void ReleaseMacroData( struct dsym *macro )
/*****************************************/
{
    int             i;
    struct srcline  *datacurr;
    struct srcline  *datanext;

    DebugMsg1(("ReleaseMacroData(%s) enter\n", macro->sym.name));
    /* free the parm list */
    for( i = 0 ; i < macro->e.macroinfo->parmcnt; i++ ) {
        /*
         for predefined macros, don't free the param labels,
         the items are stored in static memory
         */
        //if ( macro->sym.predefined == FALSE )
        //    LclFree( (void *)macro->e.macroinfo->parmlist[i].label );
        LclFree( macro->e.macroinfo->parmlist[i].deflt );
    }

    macro->e.macroinfo->parmcnt = 0;
    macro->e.macroinfo->localcnt = 0;

    if( macro->e.macroinfo->parmlist ) {
        LclFree( macro->e.macroinfo->parmlist );
        macro->e.macroinfo->parmlist = NULL;
    }

    /* free the lines list */
    for( datacurr = macro->e.macroinfo->data ;datacurr; ) {
        datanext = datacurr->next;
        LclFree( datacurr );
        datacurr = datanext;
    }
    macro->e.macroinfo->data = NULL;
    macro->e.macroinfo->srcfile = 0;
    macro->sym.mac_vararg = FALSE;
    /* v2.07: the macro type should not change if a macro is
     * PURGEd.
     */
    //macro->sym.isfunc = FALSE;
    return;
}

/* MACRO directive: define a macro
 * i: directive token ( is to be 1 )
 */
ret_code MacroDir( int i, struct asm_tok tokenarray[] )
/*****************************************************/
{
    char                *name;
    bool                store_data;
    struct dsym         *macro;

    name = tokenarray[0].string_ptr;
    DebugMsg1(("MacroDir(%s) enter, i=%u\n", name, i ));

	inMacroBody = TRUE; /* flag that we're processing a macro UASM 2.46.6 */

    macro = (struct dsym *)SymSearch( name );
    if( macro == NULL ) {
        macro = CreateMacro( name );
    } else if( macro->sym.state != SYM_MACRO ) {
        if ( macro->sym.state != SYM_UNDEFINED ) {
            return( EmitErr( SYMBOL_REDEFINITION, name ) );
        }
        /* the macro was used before it's defined. That's
         * a severe error. Nevertheless define the macro now,
         * error msg 'invalid symbol type in expression' will
         * be displayed in second pass when the (unexpanded)
         * macro name is found by the expression evaluator.
         */
        sym_remove_table( &SymTables[TAB_UNDEF], macro );
        macro->sym.state = SYM_MACRO;
        macro->e.macroinfo = LclAlloc( sizeof( struct macro_info ) );
        memset( macro->e.macroinfo, 0, sizeof( struct macro_info ) );
    }
    macro->e.macroinfo->srcfile = get_curr_srcfile();

    if ( ( Parse_Pass == PASS_1 ) || ( macro->sym.variable ) ) {
        /* is the macro redefined? */
        if ( macro->e.macroinfo->data != NULL ) {
            DebugMsg(("MacroDir(%s): macro already defined\n", name));
#if FASTMEM==0
            /* don't free memory of macro data lines if macro is in use */
            if ( MacroInUse( macro ) )
                macro->e.macroinfo->data = NULL;
#endif
            ReleaseMacroData( macro );
            /* v2.07: isfunc isn't reset anymore in ReleaseMacroData() */
            macro->sym.isfunc = FALSE;
            macro->sym.variable = TRUE;
        }
        store_data = TRUE;
    } else
        store_data = FALSE;

    if ( ModuleInfo.list )
        LstWriteSrcLine();

    return( StoreMacro( macro, ++i, tokenarray, store_data ) );
}

/*
 * PURGE directive.
 * syntax: PURGE macro [, macro, ... ]
 * Masm deletes the macro content, but the symbol name isn't released
 * and cannot be used for something else.
 * Text macros cannot be purged, because the PURGE arguments are expanded.
 */
ret_code PurgeDirective( int i, struct asm_tok tokenarray[] )
/***********************************************************/
{
    struct asym *sym;

    i++; /* skip directive */
    do {
        if ( tokenarray[i].token != T_ID ) {
            return( EmitErr( SYNTAX_ERROR_EX, tokenarray[i].string_ptr ) );
        }
        sym = SymSearch( tokenarray[i].string_ptr );
        if ( sym == NULL ) {
            return( EmitErr( SYMBOL_NOT_DEFINED, tokenarray[i].string_ptr ) );
        }
        if ( sym->state != SYM_MACRO ) {
            return( EmitErr( EXPECTED, "macro name" ) );
        }
#if TRUEPURGE
        sym->defined = FALSE;
#else
#if FASTMEM==0
        /* don't free memory of macro data lines if macro is in use */
        if ( MacroInUse( (struct dsym *)sym ) ) {
            DebugMsg1(("PurgeDirective(%s): macro is in use\n", sym->name ));
            ((struct dsym *)sym)->e.macroinfo->data = NULL;
        }
#endif
        ReleaseMacroData( (struct dsym *)sym );
        sym->variable = TRUE;
        sym->purged = TRUE;
#endif
        i++;
        if ( i < Token_Count ) {
            if ( tokenarray[i].token != T_COMMA || tokenarray[i+1].token == T_FINAL ) {
                return( EmitErr( SYNTAX_ERROR_EX, tokenarray[i].tokpos ) );
            }
            i++;
        }
    } while ( i < Token_Count );

    return( NOT_ERROR );
}

/* Implemented UNDEF directive v2.38 */
ret_code UndefDirective( int i, struct asm_tok tokenarray[] )
/***********************************************************/
{
    struct asym *sym;

    i++; /* skip directive */
    do {
	if ( tokenarray[i].token != T_ID ) {
	    return( EmitErr( SYNTAX_ERROR_EX, tokenarray[i].string_ptr ) );
	}
	sym = SymSearch( tokenarray[i].string_ptr );
	if ( sym == NULL ) {
	    return( EmitErr( SYMBOL_NOT_DEFINED, tokenarray[i].string_ptr ) );
	}
	sym->state = SYM_UNDEFINED;
	i++;
	if ( i < Token_Count ) {
	    if ( tokenarray[i].token != T_COMMA || tokenarray[i+1].token == T_FINAL ) {
		return( EmitErr( SYNTAX_ERROR_EX, tokenarray[i].tokpos ) );
	    }
	    i++;
	  }
    } while ( i < Token_Count );

    return( NOT_ERROR );
}

/* Implemented DEFINE directive v2.40 */
ret_code DefineDirective( int i, struct asm_tok const tokenarray[] )
/***********************************************************/
{
    struct asym *sym;
    char buff[256];
    char *p = buff;
    
    strcpy(tokenarray[0].string_ptr,tokenarray[1].tokpos);
    strcpy(p,tokenarray[1].tokpos);
    strcat(p, " EQU 1\0");
    strcpy(tokenarray[0].tokpos,buff);
    Token_Count = Tokenize(tokenarray[0].tokpos, 0, tokenarray, 0);
  if ( sym = CreateConstant( tokenarray ) ) {
      if ( sym->state != SYM_TMACRO ) {
#if FASTPASS
          if ( StoreState ) FStoreLine( 0 );
#endif
          if ( Options.preprocessor_stdout == TRUE )
              WritePreprocessedLine( CurrSource );
          }
          /* v2.03: LstWrite() must be called AFTER StoreLine()! */
          if ( ModuleInfo.list == TRUE ) {
              LstWrite( sym->state == SYM_INTERNAL ? LSTTYPE_EQUATE : LSTTYPE_TMACRO, 0, sym );
          }
      }
      return( NOT_ERROR );
}

/* internal @Environ macro function */
/* v2.08: ensured no buffer overflow if environment variable is larger than MAX_LINE_LEN */
static ret_code EnvironFunc( struct macro_instance *mi, char *buffer, struct asm_tok tokenarray[] )
/*************************************************************************************************/
{
    char *p = getenv( mi->parm_array[0] );
    int i;

    buffer[0] = NULLC;
    if ( p ) {
        i = strlen( p );
        if ( i >= MAX_LINE_LEN )
            i = MAX_LINE_LEN - 1;
        memcpy( buffer, p, i );
        buffer[i] = NULLC;
    }
    return( NOT_ERROR );
}

/* macro initialization
 * this proc is called once per pass
 */
ret_code MacroInit( int pass )
/****************************/
{
    struct dsym *macro;

    DebugMsg(( "MacroInit(%u)\n", pass ));

    MacroLevel = 0;
    MacroLocals = 0;
    if (pass == PASS_1) {

        StringInit();

        /* add @Environ() macro func */

        macro = CreateMacro( "@Environ" );
        macro->sym.isdefined = TRUE;
        macro->sym.predefined = TRUE;
        macro->sym.func_ptr = EnvironFunc;
        macro->sym.isfunc = TRUE;
        macro->e.macroinfo->parmcnt = 1;
        macro->e.macroinfo->parmlist = LclAlloc(sizeof(struct mparm_list));
        macro->e.macroinfo->parmlist->deflt = NULL;
        macro->e.macroinfo->parmlist->required = TRUE;
    }
    return( NOT_ERROR );
}

#ifdef DEBUG_OUT
void MacroFini( void )
/********************/
{
    StringFini();
}
#endif
