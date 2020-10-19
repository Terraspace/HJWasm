/****************************************************************************
*
*  This code is Public Domain.
*
*  ========================================================================
*
* Description:  do macro expansion.
*
* functions:
* - myltoa()          generic function which replaces ltoa()
* - RunMacro          run a macro
* - ExpandText        expand a source line when % operator is at pos 0
* - ExpandLineItems   expand parts of a source line
* - ExpandLine        expand a source line
* - ExpandLiterals    expand <> or {} literals for struct initialization
****************************************************************************/

#include <ctype.h>

#include "globals.h"
#include "memalloc.h"
#include "parser.h"
#include "preproc.h"
#include "expreval.h"
#include "equate.h"
#include "input.h"
#include "tokenize.h"
#include "macro.h"
#include "condasm.h"
#include "listing.h"
#include "myassert.h"

/* TEVALUE_UNSIGNED
 * 1 = the % operator used in an TEXTEQU expression is supposed to
 *     return an UNSIGNED value ( Masm-compatible ).
 */
#define TEVALUE_UNSIGNED 1
#define MAX_TEXTMACRO_NESTING 20

extern char *GetMacroLine( struct macro_instance *, char * );

int           MacroLocals;     /* counter for LOCAL names */
uint_8        MacroLevel;      /* current macro nesting level */

static const char __digits[16] = {"0123456789ABCDEF"};

static ret_code ExpandTMacro( char * const, struct asm_tok tokenarray[], int equmode, int level );

/* C ltoa() isn't fully compatible since hex digits are lower case.
 * for Uasm, it's ensured that 2 <= radix <= 16.
 */
char *myltoa( uint_32 value, char *buffer, unsigned radix, bool sign, bool addzero )
/**********************************************************************************/
{
    char   *p;
    char   *dst = buffer;
    char   tmpbuf[34];

#ifdef DEBUG_OUT
    uint_32 saved_value = value;
#endif
    if ( sign ) {
        *dst++ = '-';
         value = 0 - value;
    } else if ( value == 0 ) {
        *dst++ = '0';
        *dst = NULLC;
        return( buffer );
    }
    for ( p = &tmpbuf[33], *p = NULLC; value; value = value / radix )
        *(--p) = __digits[value % radix];
    if ( addzero && ( *p > '9') ) /* v2: add a leading '0' if first digit is alpha */
        *dst++ = '0';
    memcpy( dst, p, &tmpbuf[33] + 1 - p );
    DebugMsg1(("myltoa( value=%" I32_SPEC "Xh, out=%s, radix=%u, sign=%u, %u)\n", saved_value, buffer, radix, sign, addzero ));
    return( buffer );
}
/* C ltoa() isn't fully compatible since hex digits are lower case.
* for Uasm, it's ensured that 2 <= radix <= 16.
*/
#if AMD64_SUPPORT
char *myqtoa(uint_64 value, char *buffer, unsigned radix, bool sign, bool addzero)
/**********************************************************************************/
{
  char   *p;
  char   *dst = buffer;
  char   tmpbuf[34];
#ifdef DEBUG_OUT
  uint_64 saved_value = value;
#endif
  if (sign) {
    *dst++ = '-';
    value = 0 - value;
  }
  else if (value == 0) {
    *dst++ = '0';
    *dst = NULLC;
    return(buffer);
    }
  for (p = &tmpbuf[33], *p = NULLC; value; value = value / radix)
    *(--p) = __digits[value % radix];
  if (addzero && (*p > '9')) /* v2: add a leading '0' if first digit is alpha */
    *dst++ = '0';
  memcpy(dst, p, &tmpbuf[33] + 1 - p);
  DebugMsg1(("myltoa( value=%" I32_SPEC "Xh, out=%s, radix=%u, sign=%u, %u)\n", saved_value, buffer, radix, sign, addzero));
  return(buffer);
}
#endif

/* this is implemented for printing 16 numeric chars  for the MASK 128 bit v2.44 */
char *num2hex64(uint_64 value, char *buffer)
/**********************************************************************************/
{
  char   *p;
#ifdef DEBUG_OUT
  uint_64 saved_value = value;
#endif
  memset(buffer,0x30,16);
  for (p = buffer+16, *p = NULLC; value; value = value / 16)
    *(--p) = __digits[value % 16];
    DebugMsg1(("num2hex64( value=%" I64_SPEC "Xh, out=%s, radix=%u)\n", saved_value, buffer, 16));
   return(buffer);
}

/* get value of a literal, skip literal-character operators(!) */
/* returns no of characters copied into buffer (without terminating 00) */
/* v2.09: obsolete, the text is stored without '!' operators now */
#if 0
int GetLiteralValue( char *buffer, const char *p )
/************************************************/
{
    char *dest = buffer;
    if ( p )
        while ( *p ) {
            //if ( *p == '!' && *(p+1) != NULLC )
            if ( *p == '!' )
                p++;
            *dest++ = *p++;
        }
    *dest = NULLC;
    return( dest - buffer );
}
#endif

/* Read the current (macro) queue until it's done. */

static void SkipMacro( struct asm_tok tokenarray[] )
/**************************************************/
{
    char buffer[MAX_LINE_LEN];

    /* The queue isn't just thrown away, because any
     * coditional assembly directives found in the source
     * must be executed.
     */
     while ( GetTextLine( buffer ) ) {
        Tokenize( buffer, 0, tokenarray, TOK_DEFAULT );
    }

}

#ifdef __I86__
#define PARMSTRINGSIZE ( MAX_LINE_LEN + 16 )
#else
#define PARMSTRINGSIZE ( MAX_LINE_LEN * 2 )
#endif

/* run a macro.
 * - macro:  macro item
 * - out:    value to return (for macro functions)
 * - label:  token of label ( or -1 if none )
 * - is_exitm: returns TRUE if EXITM has been hit
 * returns index of token not processed or -1 on errors
 */
int RunMacro( struct dsym *macro, int idx, struct asm_tok tokenarray[], char *out, int mflags, bool *is_exitm )
/*************************************************************************************************************/
{
    char        *currparm;
    char        *savedStringBuffer = StringBufferEnd;
    int         i;
    //int         start = idx-1;
    int         parmidx;
    int         skipcomma;
    int         varargcnt;
    int         bracket_level = -1;/* () level (needed for macro functions) */
    int         parm_end_delim;   /* parameter end delimiter */
    //char        addprefix;
    char        *ptr;
    char        *parmstrings;
    struct macro_info *info;
    struct srcline    *lnode;
    struct asym       *sym;
    struct expr       opndx;
    struct macro_instance mi;

    DebugMsg1(("RunMacro(%s, idx=%u src=>%s< ) enter, lvl=%u, locals=%04u\n", macro->sym.name, idx, tokenarray[idx].tokpos, MacroLevel, MacroLocals ));

    if ( MacroLevel == MAX_MACRO_NESTING ) {
        EmitError( NESTING_LEVEL_TOO_DEEP );
        return( -1 );
    }
    mi.parm_array = NULL;
    info = macro->e.macroinfo;
#ifdef DEBUG_OUT
    info->count++;
#endif

    /* invokation of macro functions requires params enclosed in "()" */

    parm_end_delim = T_FINAL;
    if ( macro->sym.isfunc ) {
        if ( tokenarray[idx].token == T_OP_BRACKET ) { /* should be always true */
            idx++;
            parm_end_delim = T_CL_BRACKET;
            bracket_level = 1;
        }
        *out = NULLC; /* v2.08: init return value buffer */
    }
    /* v2.08: if macro is purged, return "void" */
    if ( macro->sym.purged ) {
        if ( bracket_level > 0 ) {
            for( ; bracket_level && tokenarray[idx].token != T_FINAL; idx++ )
                if ( tokenarray[idx].token == T_OP_BRACKET )
                    bracket_level++;
                else if ( tokenarray[idx].token == T_CL_BRACKET )
                    bracket_level--;
        } else
            idx = Token_Count;
        DebugMsg1(("RunMacro(%s) exit, macro is purged\n", macro->sym.name ));
        return( idx );
    }

    DebugMsg1(( "RunMacro(%s): params=>%s< parmcnt=%u vararg=%u\n", macro->sym.name, tokenarray[idx].tokpos, info->parmcnt, macro->sym.mac_vararg ));

    if ( info->parmcnt ) {
        mi.parm_array = myalloca( info->parmcnt * sizeof( char * ) + PARMSTRINGSIZE );
        parmstrings = (char *)(mi.parm_array + info->parmcnt);
        /* init the macro arguments pointer */
        currparm = parmstrings;
    }

    /* now get all the parameters from the original src line.
     * macro parameters are expanded if
     * - it is a macro function call            or
     * - the expansion operator (%) is found
     */

    parmidx = 0;
#if MACROLABEL
    if ( macro->sym.label ) {
        if ( mflags & MF_LABEL ) {
            i = strlen( tokenarray[0].string_ptr );
            mi.parm_array[parmidx] = currparm;
            memcpy( currparm, tokenarray[0].string_ptr, i+1 );
            currparm = GetAlignedPointer( currparm, i );
        } else
            mi.parm_array[parmidx] = "";
        parmidx++;
    }
#endif

    *is_exitm = FALSE;

    /* v2.08: allow T_FINAL to be chained, lastidx==0 is true final */
    tokenarray[Token_Count].lastidx = 0;

    for( varargcnt = 0, skipcomma = 0; parmidx < info->parmcnt; parmidx++ ) {

        /* v2.09: don't skip comma if it was the last argument.
         * this will a) make a trailing comma trigger warning 'too many arguments...'
         * and b), argument handling of FOR loop is significantly simplified.
         */
        if ( tokenarray[idx].token == T_COMMA && skipcomma )
            idx++;
        skipcomma = 1;

        if ( tokenarray[idx].token == T_FINAL ||
            tokenarray[idx].token == parm_end_delim ||
            ( tokenarray[idx].token == T_COMMA &&
             ( macro->sym.mac_vararg == FALSE || parmidx != info->parmcnt - 1 ) ) ) {

            /* it's a blank parm */
            if( info->parmlist[parmidx].required ) {
                DebugMsg1(( "RunMacro(%s.%u), parameter %u required >%s<\n", macro->sym.name, parmidx, parmidx, tokenarray[idx].tokpos ));
                if ( *macro->sym.name == NULLC )
                    EmitErr( MISSING_MACRO_ARGUMENT_2, macro->sym.value + 1 );
                else
                    EmitErr( MISSING_MACRO_ARGUMENT, macro->sym.name, parmidx + 1 );
                return( -1 );
            }
            if ( varargcnt == 0 ) {
                mi.parm_array[parmidx] = info->parmlist[parmidx].deflt;
                DebugMsg1(("RunMacro(%s.%u): curr (=def) parameter value=>%s<\n", macro->sym.name, parmidx, mi.parm_array[parmidx] ? parmidx, mi.parm_array[parmidx] : "NULL" ));
            }

        } else {
            int  inside_literal = 0;
            int  inside_angle_brackets = 0;
            int  old_tokencount = Token_Count;

            *currparm = NULLC;

            DebugMsg1(( "RunMacro(%s.%u), >%s<\n", macro->sym.name, parmidx, tokenarray[idx].tokpos ));

            for( ptr = currparm; ( tokenarray[idx].token != T_FINAL && tokenarray[idx].token != T_COMMA ) || inside_literal; idx++ ) {

                /* if were're inside a literal, go up one level and continue scanning the argument */
                if ( tokenarray[idx].token == T_FINAL ) {
                    idx = tokenarray[idx].lastidx; /* restore token index */
                    inside_literal--;
                    if ( tokenarray[idx].string_delim == '<' )
                        inside_angle_brackets = 0;
                    else {
                        *ptr++ = '}';
                    }
                    continue;
                }

                if ( tokenarray[idx].token == T_PERCENT ) {
                    int max;
                    int cnt;
                    int cnt_opnum;
                    /* expansion of macro parameters.
                     * if the token behind % is not a text macro or macro function
                     * the expression will be always expanded and evaluated.
                     * Else it is expanded, but only evaluated if
                     */
                    idx++;
                    while ( tokenarray[idx].token == T_PERCENT ) idx++;
                    i = idx;
                    cnt_opnum = 1;
                    if ( tokenarray[i].token == T_ID ) {
                        sym = SymSearch( tokenarray[i].string_ptr );
                        if ( sym && sym->isdefined &&
                            ( sym->state == SYM_TMACRO ||
                             ( sym->state == SYM_MACRO && sym->isfunc == TRUE && tokenarray[i+1].token == T_OP_BRACKET ) ) )
                            cnt_opnum = 0;
                    }

                    for( cnt = 0; tokenarray[i].token != T_FINAL && tokenarray[i].token != T_COMMA; i++ ) {
                        if ( is_valid_id_first_char( *tokenarray[i].string_ptr )) {
                            if ( tokenarray[i+1].token == T_OP_BRACKET ) {
                                int cnt2;
                                i += 2;
                                for ( cnt2 = 1;cnt2 && tokenarray[i].token != T_FINAL; i++ ) {
                                    if ( tokenarray[i].token == T_OP_BRACKET )
                                        cnt2++;
                                    else if ( tokenarray[i].token == T_CL_BRACKET )
                                        cnt2--;
                                }
                                i--;
                            }
                            continue;
                        }
                        /* count brackets */
                        if ( parm_end_delim == T_CL_BRACKET )
                            if ( tokenarray[i].token == T_OP_BRACKET )
                                cnt++;
                            else if ( tokenarray[i].token == T_CL_BRACKET ) {
                                if ( cnt == 0 )
                                    break;
                                cnt--;
                            }

                        /* stop if undelimited string occurs (need to scan for '!') */
                        if ( tokenarray[i].token == T_STRING && tokenarray[i].string_delim == NULLC )
                            break;

                        /* names dot and amp are ok */
                        if ( tokenarray[i].token == T_DOT || tokenarray[i].token == '&' || tokenarray[i].token == '%' )
                            ;
                        else
                            cnt_opnum++; /* anything else will trigger numeric evaluation */
                    }

                    if ( i == idx ) { /* no items except %? */
                        idx--;
                        continue;
                    }

                    cnt = tokenarray[i].tokpos - tokenarray[idx].tokpos;
                    while ( isspace( *(tokenarray[idx].tokpos+cnt-1) ) ) cnt--;
                    memcpy( ptr, tokenarray[idx].tokpos, cnt );
                    *(ptr+cnt) = NULLC;
                    if ( ExpandText( ptr, tokenarray, FALSE ) == ERROR ) {
                        StringBufferEnd = savedStringBuffer;
                        return(-1);
                    }
                    idx = i - 1;
                    if ( cnt_opnum ) {
                        /* convert numeric expression into a string */
                        max = Tokenize( ptr, Token_Count+1, tokenarray, TOK_RESCAN );
                        i = Token_Count + 1;
                        DebugMsg1(( "RunMacro(%s.%u), num expansion: >%s<\n", macro->sym.name, parmidx, ptr ));
                        /* the % operator won't accept forward references.
                         * v2.09: flag EXPF_NOUNDEF set.
                         */
                        if ( EvalOperand( &i, tokenarray, max, &opndx, EXPF_NOUNDEF ) == ERROR )
                            opndx.llvalue = 0;
                        else if ( opndx.kind != EXPR_CONST ) {
                            EmitError( CONSTANT_EXPECTED );
                            opndx.llvalue = 0;
                        }
                        DebugMsg1(( "RunMacro(%s.%u): num expansion, opndx.type=%d, value=%d\n", macro->sym.name, parmidx, opndx.type, opndx.value ));
                        /* v2.08: accept constant and copy any stuff that's following */
                        myltoa( opndx.uvalue, StringBufferEnd, ModuleInfo.radix, opndx.hvalue < 0, FALSE );
                        //ptr += strlen( ptr );
                        if ( i != max ) {
                            /* the evaluator was unable to evaluate the full expression. the rest
                             * has to be "copied" */
                            DebugMsg1(( "RunMacro(%s.%u): num expansion, additional token=%s\n", macro->sym.name, parmidx, tokenarray[i].tokpos ));
                            /* just copy won't work, since <>-literals aren't handled correctly then */
                            //EmitErr( SYNTAX_ERROR_EX, tokenarray[i].tokpos );
                            strcat( StringBufferEnd, tokenarray[i].tokpos );
                        }
                        strcpy( ptr, StringBufferEnd );
                    }
                    ptr += strlen( ptr );
                    /**/myassert( ptr < parmstrings + PARMSTRINGSIZE );
                    continue;
                }

                if ( tokenarray[idx].token == T_STRING && tokenarray[idx].string_delim == '{' ) {
                    char const *p = tokenarray[idx].string_ptr;
                    int tmp = idx;
                    /* copy the '{' */
                    *ptr++ = '{';
                    /* the string must be tokenized */
                    inside_literal++;
                    idx = Token_Count;
                    Token_Count = Tokenize( p, idx + 1, tokenarray, TOK_RESCAN | TOK_NOCURLBRACES );
                    tokenarray[Token_Count].lastidx = tmp;
                    continue;
                }

                if ( inside_angle_brackets == 0 ) {
                    /* track brackets for macro functions; exit if one more ')' than '(' is found */
                    if ( bracket_level > 0 ) {
                        if ( tokenarray[idx].token == T_OP_BRACKET ) {
                            bracket_level++;
                        } else if ( tokenarray[idx].token == T_CL_BRACKET ) {
                            bracket_level--;
                            if ( bracket_level == 0 )
                                break;
                        }
                    }

                    /* if there's a literal enclosed in <>, remove the delimiters and
                     * tokenize the item (Token_Count must be adjusted, since RunMacro()
                     * might be called later!)
                     */
                    if ( tokenarray[idx].token == T_STRING && tokenarray[idx].string_delim == '<' && inside_angle_brackets == 0 ) {
                        char *p;
                        int tmp;
                        int size;
#if 1
                        if ( !strchr( tokenarray[idx].string_ptr, '%' ) ) {
                            memcpy( ptr, tokenarray[idx].string_ptr, tokenarray[idx].stringlen );
                            ptr += tokenarray[idx].stringlen;
                            continue;
                        }
#endif
                        tmp = idx;
                        size = tokenarray[idx+1].tokpos - (tokenarray[idx].tokpos+1);
                        p = StringBufferEnd;
                        memcpy( p, tokenarray[idx].tokpos+1, size );
                        while ( *(p+size-1) != '>' ) size--;
                        *(p+size-1) = NULLC;
                        StringBufferEnd = GetAlignedPointer( p, size );
                        //strcpy( tmpline, tokenarray[idx].string_ptr );
                        /* the string must be tokenized */
                        inside_literal++;
                        inside_angle_brackets = 1;
                        idx = Token_Count;
                        Token_Count = Tokenize( p, idx + 1, tokenarray, TOK_RESCAN );
                        tokenarray[Token_Count].lastidx = tmp;
                        /* copy spaces located before the first token */
                        memcpy( ptr, p, tokenarray[idx+1].tokpos - p );
                        ptr += tokenarray[idx+1].tokpos - p;
                        continue;
                    }
                    /* macros functions must be expanded always.
                     * text macros are expanded only selectively
                     */
                    if ( tokenarray[idx].token == T_ID ) {
                        if ( sym = SymSearch( tokenarray[idx].string_ptr ) ) {
                            if ( sym->state == SYM_MACRO && sym->isdefined == TRUE &&
                                sym->isfunc == TRUE && tokenarray[idx+1].token == T_OP_BRACKET ) {
                                bool is_exitm2;
                                //int oldidx = idx;
                                idx = RunMacro( (struct dsym *)sym, idx+1, tokenarray, ptr, 0, &is_exitm2 );
                                if ( idx < 0 ) {
                                    StringBufferEnd = savedStringBuffer;
                                    return( idx );
                                }
                                ptr += strlen( ptr );
                                /* copy spaces behind macro function call */
                                if ( tokenarray[idx].token != T_FINAL && tokenarray[idx].token != T_COMMA ) {
                                    i = tokenarray[idx].tokpos - ( tokenarray[idx-1].tokpos + 1 );
                                    memcpy( ptr, tokenarray[idx-1].tokpos + 1, i );
                                    ptr += i;
                                }
                                idx--; /* adjust token index */
                                /**/myassert( ptr < parmstrings + PARMSTRINGSIZE );
                                continue;
                            } else if ( sym->state == SYM_TMACRO && sym->isdefined == TRUE &&
                                       ( macro->sym.predefined && ( info->autoexp & ( 1 << parmidx ) ) ) ) {
                                //GetLiteralValue( ptr, sym->string_ptr );
                                strcpy( ptr, sym->string_ptr );
                                ExpandTMacro( ptr, tokenarray, FALSE, 0 );
                                ptr += strlen( ptr );
                                /* copy spaces behind text macro */
                                if ( tokenarray[idx+1].token != T_FINAL && tokenarray[idx+1].token != T_COMMA ) {
                                    i = tokenarray[idx+1].tokpos - ( tokenarray[idx].tokpos + sym->name_size );
                                    memcpy( ptr, tokenarray[idx].tokpos + sym->name_size, i );
                                    ptr += i;
                                }
                                /**/myassert( ptr < parmstrings + PARMSTRINGSIZE );
                                continue;
                            }
                        }
                    }
                }
                /* get length of item */
                i = tokenarray[idx+1].tokpos - tokenarray[idx].tokpos;
                if ( !inside_literal && ( tokenarray[idx+1].token == T_COMMA ||
                    tokenarray[idx+1].token == parm_end_delim ) ) {
                    while ( isspace( *(tokenarray[idx].tokpos+i-1 ) ) ) i--;
                }
                /* the literal character operator ! is valid for macro arguments */
                if ( tokenarray[idx].token == T_STRING && tokenarray[idx].string_delim == NULLC ) {
                    char *p;
                    char *end;
                    DebugMsg1(("RunMacro(%s.%u): undelimited string >%s<, watching '!'\n", macro->sym.name, parmidx, tokenarray[idx].string_ptr ));
                    p = tokenarray[idx].tokpos;
                    end = p + i;
                    /**/myassert( ( ptr + i ) < parmstrings + PARMSTRINGSIZE );
                    for ( ; p < end; p++ ) {
                        if ( *p == '!' )
                            p++;
                        *ptr++ = *p;
                    }
                    continue;
                }
                /**/myassert( ( ptr + i ) < parmstrings + PARMSTRINGSIZE );
                memcpy( ptr, tokenarray[idx].tokpos, i );
                ptr += i;

            } /* end for */

            *ptr = NULLC;

            /* restore input status values */
            Token_Count = old_tokencount;
            StringBufferEnd = savedStringBuffer;

            /* store the macro argument in the parameter array */
            if (  macro->sym.mac_vararg && ( parmidx == info->parmcnt - 1 ) ) {
                if ( varargcnt == 0 )
                    mi.parm_array[parmidx] = currparm;
                DebugMsg1(("RunMacro(%s.%u[%u]): curr parameter value=>%s<\n", macro->sym.name, parmidx, varargcnt, currparm ));
                currparm = ( macro->sym.predefined ? GetAlignedPointer( currparm, ptr - currparm ) : ptr );
                /* v2.08: Masm swallows the last trailing comma */
                //if ( tokenarray[idx].token == T_COMMA ) {
                if ( tokenarray[idx].token == T_COMMA ) {
                    idx++;
                    if ( macro->sym.isfunc == FALSE || tokenarray[idx].token != parm_end_delim ) {
                        parmidx--;
                        if ( !macro->sym.predefined ) {
                            *currparm++ = ',';
                        }
                        *currparm = NULLC;
                    }
                    skipcomma = 0;
                }
                varargcnt++;
            } else if ( *currparm ) {
                mi.parm_array[parmidx] = currparm;
                DebugMsg1(("RunMacro(%s.%u): curr parameter value=>%s<\n", macro->sym.name, parmidx, currparm ));
                currparm = GetAlignedPointer( currparm, ptr - currparm );
            } else {
                mi.parm_array[parmidx] = "";
                DebugMsg1(("RunMacro(%s.%u): curr parameter value=><\n", macro->sym.name, parmidx ));
            }
        } /*end if */
    } /* end for  */

    /* for macro functions, check for the terminating ')' */
    if ( bracket_level >= 0 ) {
        if ( tokenarray[idx].token != T_CL_BRACKET ) {
            for ( i = idx; idx < Token_Count && tokenarray[idx].token != T_CL_BRACKET; idx++ );
            if ( idx == Token_Count ) {
                DebugMsg1(("RunMacro(%s): missing ')'\n", macro->sym.name));
                EmitError( MISSING_RIGHT_PARENTHESIS );
                return( -1 );
            } else {
                DebugMsg1(("RunMacro(%s): expected ')', found >%s<\n", macro->sym.name, tokenarray[i].tokpos ));
                /* v2.09: changed to a warning only (Masm-compatible) */
                EmitWarn( 1, TOO_MANY_ARGUMENTS_IN_MACRO_CALL, macro->sym.name, tokenarray[i].tokpos );
            }
        }
        idx++;
    //} else if ( tokenarray[idx].token != T_FINAL && *macro->sym.name != NULLC ) { /* v2.08: changed */
    } else if ( tokenarray[idx].token != T_FINAL ) {
        DebugMsg1(("RunMacro(%s): expected T_FINAL, found >%s<, parmidx=%u\n", macro->sym.name, tokenarray[idx].tokpos, parmidx ));
        /* v2.05: changed to a warning. That's what Masm does */
        /* v2.09: don't emit a warning if it's a FOR directive
         * (in this case, the caller knows better what to do ).
         */
        if ( !(mflags & MF_IGNARGS) )
            EmitWarn( 1, TOO_MANY_ARGUMENTS_IN_MACRO_CALL, macro->sym.name, tokenarray[idx].tokpos );
        //return( -1 );
    }

    /* a predefined macro func with a function address? */

    if ( macro->sym.predefined == TRUE && macro->sym.func_ptr != NULL ) {
        mi.parmcnt = varargcnt;
        macro->sym.func_ptr( &mi, out, tokenarray );
        *is_exitm = TRUE;
        return( idx );
    }

#if 0
    /* check if a (code) label before the macro is to be written
     * v2.08: this is the wrong place, because the label is written
     * AFTER possible macro functions in the arguments are evaluated.
     * Hence this functionality has been moved to ExpandToken().
     */
    addprefix = FALSE;
    if ( macro->sym.isfunc == FALSE && 
#if MACROLABEL
        macro->sym.label == FALSE &&
#endif
        label >= 0 && start > label )
        addprefix = TRUE;
#endif

    mi.localstart = MacroLocals;
    MacroLocals += info->localcnt; /* adjust global variable MacroLocals */

    /* avoid to use values stored in struct macro_info directly. A macro
     * may be redefined within the macro! Hence copy all values that are
     * needed later in the while loop to macro_instance!
     */
    mi.startline = info->data;
    mi.currline = NULL;
    mi.parmcnt = info->parmcnt;

    /* v2.03: no processing if macro has no lines */
    /* v2.08: addprefix is obsolete */
    //if ( mi.currline || addprefix ) {
    if ( mi.startline ) {
        struct input_status oldstat;
        int oldifnesting;
        int cntgoto;

        DebugMsg1(("RunMacro(%s): enter assembly loop, macro level=%u\n", macro->sym.name, MacroLevel+1 ));
        /* v2.04: this listing is too excessive */
        //if ( ModuleInfo.list && ( ModuleInfo.list_macro == LM_LISTMACROALL || MacroLevel == 0 ) )
        //if ( MacroLevel == 0 && macro->sym.isfunc == FALSE && *macro->sym.name )
        if ( macro->sym.isfunc == FALSE && *macro->sym.name )
            LstWriteSrcLine();
        if ( !( mflags & MF_NOSAVE ) )
            tokenarray = PushInputStatus( &oldstat );

        /*
         * move the macro instance onto the file stack!
         * Also reset the current linenumber!
         */
        mi.macro = &macro->sym;
        PushMacro( &mi );
        MacroLevel++;
        oldifnesting = GetIfNestLevel(); /* v2.10 */
        cntgoto = 0; /* v2.10 */
        /* Run the assembler until we hit EXITM or ENDM.
         * Also handle GOTO and macro label lines!
         * v2.08 no need anymore to check the queue level
         * v2.11 GetPreprocessedLine() replaced by GetTextLine()
         * and PreprocessLine().
         */

        while ( GetTextLine( CurrSource ) ) {
            if ( PreprocessLine( CurrSource, tokenarray ) == 0 )
                continue;
            /* skip macro label lines */
            if ( tokenarray[0].token == T_COLON ) {
                /* v2.05: emit the error msg here, not in StoreMacro() */
                if ( tokenarray[1].token != T_ID )
                    EmitErr( SYNTAX_ERROR_EX, tokenarray[0].tokpos );
                else if ( tokenarray[2].token != T_FINAL )
                    EmitErr( SYNTAX_ERROR_EX, tokenarray[2].tokpos );
                continue;
            }

            if ( tokenarray[0].token == T_DIRECTIVE ) {
                if ( tokenarray[0].tokval == T_EXITM ) {
                    if ( ModuleInfo.list && ModuleInfo.list_macro == LM_LISTMACROALL )
                        LstWriteSrcLine();
                    if ( tokenarray[1].token != T_FINAL ) {
                        /* v2.05: display error if there's more than 1 argument or
                         * the argument isn't a text item
                         */
                        if ( tokenarray[1].token != T_STRING || tokenarray[1].string_delim != '<' )
                            TextItemError( &tokenarray[1] );
                        else if ( Token_Count > 2 )
                            EmitErr( SYNTAX_ERROR_EX, tokenarray[2].tokpos );
                        else if ( out ) { /* return value buffer may be NULL ( loop directives ) */

                            /* v2.08a: the <>-literal behind EXITM is handled specifically,
                             * macro operator '!' within the literal is only handled
                             * if it contains a placeholder (macro argument, macro local ).
                             *
                             * v2.09: handle '!' inside literal if ANY expansion occurred.
                             * To determine text macro or macro function expansion,
                             * check if there's a literal in the original line.
                             */
                            if ( mi.currline->ph_count || *(mi.currline->line+(tokenarray[1].tokpos-CurrSource)) != '<' ) {
                                memcpy( out, tokenarray[1].string_ptr, tokenarray[1].stringlen + 1 );
                            } else {
                                /* since the string_ptr member has the !-operator stripped, it
                                 * cannot be used. To get the original value of the literal,
                                 * use tokpos.
                                 */
                                int len;
                                len = tokenarray[2].tokpos - (tokenarray[1].tokpos+1);
                                memcpy( out, tokenarray[1].tokpos+1, len );
                                while( *(out+len-1) != '>' ) len--;
                                *(out+len-1) = NULLC;
                            }
                        }
                    }
                    DebugMsg1(("RunMacro(%s): EXITM, result=>%s<\n", macro->sym.name, out ? out : "NULL" ));

                    /* v2.10: if a goto had occured, rescan the full macro to ensure that
                     * the "if"-nesting level is ok.
                     */
                    if ( cntgoto ) {
                        mi.currline = NULL;
                        SetLineNumber( 0 );
                        SetIfNestLevel( oldifnesting );
                    }

                    SkipMacro( tokenarray );
                    *is_exitm = TRUE;
                    break;
#if 0 /* won't happen anymore */
                } else if ( tokenarray[0].tokval == T_ENDM ) {
                    DebugMsg1(("RunMacro(%s): ENDM\n", macro->sym.name ));
                    break;
#endif
                } else if ( tokenarray[0].tokval == T_GOTO ) {
                    if ( tokenarray[1].token != T_FINAL ) {
                        int len = strlen( tokenarray[1].string_ptr );
                        DebugMsg1(("RunMacro(%s): GOTO %s, MacroLevel=%u\n", macro->sym.name, tokenarray[1].string_ptr, MacroLevel ));
                        /* search for the destination line */
                        for( i = 1, lnode = mi.startline; lnode != NULL; lnode = lnode->next, i++ ) {
                            ptr = lnode->line;
                            //DebugMsg(("RunMacro(%s): GOTO, scan line >%s< for label >%s<\n", macro->sym.name, ptr, line));
                            if ( *ptr == ':' ) {
                                if ( lnode->ph_count ) {
                                    fill_placeholders( StringBufferEnd, lnode->line, mi.parmcnt, mi.localstart, mi.parm_array );
                                    ptr = StringBufferEnd;
                                }
                                ptr++;
                                while( isspace( *ptr )) ptr++;
                                DebugMsg1(("RunMacro(%s): GOTO, line=>%s<\n", macro->sym.name, ptr ));
                                /* macro labels are always case-insensitive! */
                                //if ( ( SymCmpFunc( ptr, tokenarray[1].string_ptr, len ) == 0 ) &&
                                if ( ( _memicmp( ptr, tokenarray[1].string_ptr, len ) == 0 ) &&
                                    ( is_valid_id_char(*(ptr+len) ) == FALSE ) ) {
                                    /* label found! */
                                    break;
                                }
                            }
                        }
                        if ( !lnode ) {
                            /* v2.05: display error msg BEFORE SkipMacro()! */
                            DebugMsg1(("RunMacro(%s): GOTO, label >%s< not found!\n", macro->sym.name, tokenarray[1].string_ptr ));
                            EmitErr( MACRO_LABEL_NOT_DEFINED, tokenarray[1].string_ptr );
                        } else {
                            DebugMsg1(("RunMacro(%s): GOTO, found label >%s<\n", macro->sym.name, ptr));
                            /* v2.10: rewritten, "if"-nesting-level handling added */
                            mi.currline = lnode;
                            SetLineNumber( i );
                            SetIfNestLevel( oldifnesting );
                            cntgoto++;
                            continue;
                        }
                    } else {
                        EmitErr( SYNTAX_ERROR_EX, tokenarray->tokpos );
                    }
                    SkipMacro( tokenarray );
                    break;
                }
            }
            ParseLine( tokenarray );
            if ( Options.preprocessor_stdout == TRUE )
                WritePreprocessedLine( CurrSource );

            /* the macro might contain an END directive.
             * v2.08: this doesn't mean the macro is to be cancelled.
             * Masm continues to run it and the assembly is stopped
             * when the top source level is reached again.
             */
            //if ( ModuleInfo.EndDirFound ) {
            //    SkipMacro( tokenarray );
            //    *is_exitm = TRUE; /* force loop exit */
            //    break;
            //}
        } /* end while */

        MacroLevel--;
        if ( !(mflags & MF_NOSAVE ) )
            PopInputStatus( &oldstat );

        /* don't use tokenarray from here on, it's invalid after PopInputStatus() */

#if FASTMEM==0
        /* v2.06: free "old" macro line data if macro has been changed
         * and isn't in use anymore */
        if ( mi.startline != info->data && ( !MacroInUse( macro ) ) ) {
            struct srcline  *curr;
            struct srcline  *next;
            DebugMsg1(("RunMacro(%s): macro has been changed, releasing old lines\n", macro->sym.name ));
            for( curr = mi.startline ; curr; curr = next ) {
                next = curr->next;
                LclFree( curr );
            }
        }
#endif
    } /* end if */

    DebugMsg1(("RunMacro(%s) exit, MacroLevel=%u\n", macro->sym.name, MacroLevel ));

    return( idx );
}

/* make room (or delete items) in the token buffer at position <start> */

static void AddTokens( struct asm_tok tokenarray[], int start, int count, int end )
/*********************************************************************************/
{
    int i;

    if ( count > 0 ) {
        for( i = end; i >= start; i-- ) {
            tokenarray[i+count] = tokenarray[i];
        }
    } else if ( count < 0 ) {
        for( i = start - count; i <= end; ++i ) {
            tokenarray[i+count] = tokenarray[i];
        }
    }
}

/*
 * ExpandText() is called if
 * - the evaluation operator '%' has been found as first char of the line.
 * - the % operator is found in a macro argument.
 * if substitute is TRUE, scanning for the substitution character '&' is active!
 * Both text macros and macro functions are expanded!
 */

ret_code ExpandText( char *line, struct asm_tok tokenarray[], unsigned int substitute )
/*************************************************************************************/
{
    char *pSrc;
    char *pDst;
    char *pIdent;
    int  lvl;
    bool is_exitm;
    int old_tokencount = Token_Count;
    char *old_stringbufferend = StringBufferEnd;
    char quoted_string = 0;
    char macro_proc = FALSE;
    //char *pStart;
    ret_code rc;
    struct asym *sym;
    char *sp[MAX_TEXTMACRO_NESTING];

        DebugMsg1(("ExpandText(line=>%s<, subst=%u ) enter\n", line, substitute ));
    sp[0] = line;
    pDst = StringBufferEnd;
    StringBufferEnd += MAX_LINE_LEN;
    rc = NOT_ERROR;
    for ( lvl = 0; lvl >= 0; lvl-- ) {
        pSrc = sp[lvl];
        while ( *pSrc ) {
            if( is_valid_id_first_char( *pSrc ) && ( substitute != 0 || quoted_string == 0 ) ) {
                pIdent = pDst;
                do {
                    *pDst++ = *pSrc++;
                } while ( is_valid_id_char( *pSrc ));
                *pDst = NULLC;
                sym = SymSearch( pIdent );
#ifdef DEBUG_OUT
                if ( sym && ( sym->state == SYM_TMACRO || sym->state == SYM_MACRO ) ) {
                    DebugMsg1(( "ExpandText: symbol found: %s, %s, defined=%u, *pDst-1=%c\n", sym->name, sym->state == SYM_TMACRO ? "SYM_TMACRO" : "SYM_MACRO", sym->isdefined, *(pDst-1) ));
                }
#endif
                if ( sym && sym->isdefined == TRUE ) {
                    if ( sym->state == SYM_TMACRO ) {
                        /* v2.08: no expansion inside quoted strings without & */
                        if ( quoted_string && *(pIdent-1) != '&' && *pSrc != '&' )
                            continue;
                        if ( substitute ) {
                            if ( *(pIdent-1) == '&' )
                                pIdent--;
                            if ( *pSrc == '&' )
                                pSrc++;
                        } else if ( pIdent > old_stringbufferend && *(pIdent-1) == '%' )
                                pIdent--;

                        sp[lvl++] = pSrc;
                        pSrc = StringBufferEnd;
                        //StringBufferEnd = GetAlignedPointer( pSrc, GetLiteralValue( pSrc, sym->string_ptr ) );
                        strcpy( pSrc, sym->string_ptr );
                        StringBufferEnd = GetAlignedPointer( pSrc, strlen( pSrc ) );
                        DebugMsg1(("ExpandText: %s replaced by >%s<\n", sym->name, pSrc ));
                        pDst = pIdent;
                        rc = STRING_EXPANDED;
                    } else if ( sym->state == SYM_MACRO && sym->isfunc == TRUE ) {
                        /* expand macro functions. */
                        char *p = pSrc;
                        int i;
                        while ( isspace(*p) ) p++;
                        /* no macro function invokation if the '(' is missing! */
                        if ( *p == '(' ) {
                            int j;
                            int cnt;
                            i = Token_Count + 1;
                            Token_Count = Tokenize( p, i, tokenarray, TOK_RESCAN );
                            for ( j = i, cnt = 0; j < Token_Count; j++ ) {
                                if ( tokenarray[j].token == T_OP_BRACKET )
                                    cnt++;
                                else if ( tokenarray[j].token == T_CL_BRACKET ) {
                                    cnt--;
                                    if ( cnt == 0 ) {
                                        j++;
                                        break;
                                    }
                                }
                            }
                            /* don't substitute inside quoted strings if there's no '&' */
                            if ( quoted_string && *(pIdent-1) != '&' && tokenarray[j].token != '&' ) {
                                Token_Count = old_tokencount;
                                continue;
                            }
                            if ( substitute ) {
                                if ( *(pIdent-1) == '&' )
                                    pIdent--;
                            } else if ( pIdent > old_stringbufferend && *(pIdent-1) == '%' )
                                pIdent--;
                            //*StringBufferEnd = NULLC;
                            i = RunMacro( (struct dsym *)sym, i, tokenarray, pDst, 0, &is_exitm );
                            Token_Count = old_tokencount;
                            DebugMsg1(( "ExpandText: back from RunMacro(%s), rc=%u, text returned=>%s<, rest=>%s<\n", sym->name, i, pDst, i >= 0 ? tokenarray[i].tokpos : "" ));
                            if ( i == -1 ) {
                                return( ERROR );
                            }
                            pSrc = tokenarray[i-1].tokpos + strlen( tokenarray[i-1].string_ptr );
                            if ( substitute && *pSrc == '&' )
                                pSrc++;
                            sp[lvl++] = pSrc;
                            pSrc = StringBufferEnd;
                            cnt = strlen( pDst );
                            memcpy( pSrc, pDst, cnt + 1 );
                            StringBufferEnd = GetAlignedPointer( pSrc, cnt );
                            pDst = pIdent;
                            rc = STRING_EXPANDED;
                        }
                    } else if ( sym->state == SYM_MACRO ) {
                        macro_proc = TRUE;
                    }
                    if ( lvl == MAX_TEXTMACRO_NESTING ) {
                        DebugMsg(("ExpandText(line=>%s<) error exit\n", line));
                        EmitError( MACRO_NESTING_LEVEL_TOO_DEEP );
                        break;
                    }
                }
            } else {
                if ( *pSrc == '"' || *pSrc == '\'' ) {
                    if ( quoted_string == 0 )
                        quoted_string = *pSrc;
                    else if ( *pSrc == quoted_string )
                        quoted_string = 0;
                }
                *pDst++ = *pSrc++;
            }
        } /* end while */
    }
    *pDst++ = NULLC;

    StringBufferEnd = old_stringbufferend;
    if ( rc == STRING_EXPANDED ) {
        memcpy( line, StringBufferEnd, pDst - StringBufferEnd );
        DebugMsg1(("ExpandText: expanded line=>%s<\n", line));
    }
    if ( substitute ) {
        if ( rc == STRING_EXPANDED ) {
            Token_Count = Tokenize( tokenarray[0].tokpos, 0, tokenarray, TOK_RESCAN );
        }
        if ( rc == STRING_EXPANDED || macro_proc ) {
            return( ExpandLine( tokenarray[0].tokpos, tokenarray ) );
        }
    }
    return( rc );
}

/* replace text macros and macro functions by their values, recursively
 * outbuf in: text macro or macro function value
 * outbuf out: expanded value
 * equmode: if 1, don't expand macro functions
 */
static ret_code ExpandTMacro( char * const outbuf, struct asm_tok tokenarray[], int equmode, int level )
/******************************************************************************************************/
{
    int old_tokencount = Token_Count;
    int i;
    char expanded = TRUE;
    int len;
    bool is_exitm;
    struct asym *sym;
    //char lvalue[MAX_LINE_LEN];    /* holds literal value */
    char buffer[MAX_LINE_LEN];

    DebugMsg1(("ExpandTMacro(text=>%s< equm=%u lvl=%u) enter\n", outbuf, equmode, level ));

    if ( level >= MAX_TEXTMACRO_NESTING ) {
        return( EmitError( MACRO_NESTING_LEVEL_TOO_DEEP ) );
    }

    while ( expanded == TRUE ) {
        i = old_tokencount + 1;
        Token_Count = Tokenize( outbuf, i, tokenarray, TOK_RESCAN );
        expanded = FALSE;
        for ( ; i < Token_Count; i++ ) {
            if ( tokenarray[i].token == T_ID ) {
                sym = SymSearch( tokenarray[i].string_ptr );
                /* expand macro functions */
                if ( sym && sym->state == SYM_MACRO &&
                    sym->isdefined == TRUE && sym->isfunc == TRUE &&
                    tokenarray[i+1].token == T_OP_BRACKET && equmode == FALSE ) {
                    len = tokenarray[i].tokpos - outbuf;
                    memcpy( buffer, outbuf, len );
                    i = RunMacro( (struct dsym *)sym, i+1, tokenarray, buffer+len, 0, &is_exitm );
                    if ( i < 0 ) {
                        Token_Count = old_tokencount;
                        return( ERROR );
                    }
                    DebugMsg1(("ExpandTMacro(%u): repl >%s()< by >%s<\n", level, sym->name, buffer+len ));
                    strcat( buffer+len, tokenarray[i].tokpos );
                    strcpy( outbuf, buffer );
                    expanded = TRUE;
                    /* is i to be decremented here? */
                    break;
                } else if ( sym && sym->state == SYM_TMACRO && sym->isdefined == TRUE ) {
                    char *p;
                    len = tokenarray[i].tokpos - outbuf;
                    memcpy( buffer, outbuf, len );
                    //GetLiteralValue( buffer+len, sym->string_ptr );
                    strcpy( buffer+len, sym->string_ptr );
                    DebugMsg1(("ExpandTMacro(>%s<, %u): calling ExpandTMacro, value >%s<\n", sym->name, level, buffer+len ));
                    if ( ERROR == ExpandTMacro( buffer + len, tokenarray, equmode, level+1 ) ) {
                        Token_Count = old_tokencount;
                        return( ERROR );
                    }
                    DebugMsg1(("ExpandTMacro(%u): repl >%s< by >%s<\n", level, sym->name, buffer+len ));
                    //if ( level || ( tokenarray[i+1].token != T_FINAL && tokenarray[i+1].token != T_COMMA ))
                        p = tokenarray[i].tokpos + sym->name_size;
                    //else
                    //    p = tokenarray[i+1].tokpos;
                    strcat( buffer+len, p );
                    strcpy( outbuf, buffer );
                    expanded = TRUE;
                    break;
                }
            }
        }
    }
    Token_Count = old_tokencount;
    //strcpy( outbuf, lvalue );
    return( NOT_ERROR );
}

/* rebuild a source line
 * adjust all "pos" values behind the current pos
 * - newstring = new value of item i
 * - i = token buffer index of item to replace
 * - outbuf = start of source line to rebuild
 * - oldlen = old length of item i
 * - pos_line = position of item in source line
*/
static ret_code RebuildLine( const char *newstring, int i, struct asm_tok tokenarray[], unsigned oldlen, unsigned pos_line, int addbrackets )
/*******************************************************************************************************************************************/
{
    char *dest;
    const char *src;
    unsigned  newlen;
    unsigned  rest = strlen( tokenarray[i].tokpos + oldlen ) + 1;
    char buffer[MAX_LINE_LEN];

    dest = tokenarray[i].tokpos;
    memcpy( buffer, dest + oldlen, rest ); /* save content of line behind item */
    newlen = strlen( newstring );
    if ( addbrackets ) {
        newlen += 2;   /* count '<' and '>' */
        for ( src = newstring; *src; src++ )
            if ( *src == '<' || *src == '>' || *src == '!' )    /* count '!' operator */
                newlen++;
    }
    if ( newlen > oldlen )
        if ( ( pos_line + newlen - oldlen + rest ) >= MAX_LINE_LEN ) {
            return( EmitErr( EXPANDED_LINE_TOO_LONG, tokenarray[0].tokpos ) );
        }

    if ( addbrackets ) {
        *dest++ = '<';
        for ( src = newstring; *src; src++ ) {
            if ( *src == '<' || *src == '>' || *src == '!' )    /* count '!' operator */
                *dest++ = '!';
            *dest++ = *src;
        }
        *dest++ = '>';
    } else {
        memcpy( dest, newstring, newlen );
        dest += newlen;
    }
    memcpy( dest, buffer, rest ); /* add rest of line */

    /* v2.05: changed '<' to '<=' */
    for ( i++; i <= Token_Count; i++ ) {
        tokenarray[i].tokpos = tokenarray[i].tokpos - oldlen + newlen;
    }

    return( NOT_ERROR );
}

/* expand one token
 * line: full source line
 * *pi: index of token in tokenarray
 * equmode: if 1, dont expand macro functions
 */
static ret_code ExpandToken( char const line[], int *pi, struct asm_tok tokenarray[], int max, int bracket_flags, int equmode )
/**********************************************************************************************************************/
{
    int pos;
    int tmp;
    int i = *pi;
    int size;
    int addbrackets = bracket_flags;
    char evaluate = FALSE;
    //char *p;
    bool is_exitm;
    struct expr opndx;
    struct asym *sym;
    ret_code rc = NOT_ERROR;
    char buffer[MAX_LINE_LEN];
	char inLocal = FALSE;

    for ( ; i < max && tokenarray[i].token != T_COMMA; i++ ) {
    
		if (strcasecmp(tokenarray[i].string_ptr, "LOCAL") == 0)
		{
			inLocal = TRUE;
		}
		/* v2.05: the '%' should only be handled as an operator if addbrackets==TRUE,
         * which means that the current directive is a preprocessor directive and the
         * expected argument is a literal (or text macro).
         */
        if ( tokenarray[i].token == T_PERCENT && addbrackets && evaluate == FALSE ) {
            evaluate = TRUE;
            addbrackets = FALSE;
            equmode = FALSE;
            pos = i;
            DebugMsg1(("ExpandToken: %% found, line=%s\n", tokenarray[i].tokpos ));
            continue;
        }
        if( tokenarray[i].token == T_ID ) {
			if(inLocal == TRUE)
				sym = SymFindDeclare(tokenarray[i].string_ptr);
			else
				sym = SymSearch( tokenarray[i].string_ptr );
            DebugMsg1(("ExpandToken: testing id >%s< equmode=%u\n", tokenarray[i].string_ptr, equmode ));
            /* don't check isdefined flag (which cannot occur in pass one, and this code usually runs
             * in pass one only!
             */
            //if( sym && sym->isdefined ) {
            if( sym ) {
                if ( sym->state == SYM_MACRO ) {
                    tmp = i; /* save index of macro name */
                    if ( sym->isfunc == TRUE ) {
                        /* ignore macro functions without a following '(' */
                        if ( tokenarray[i+1].token != T_OP_BRACKET ) {
                            DebugMsg1(("ExpandToken(%s): macro function without () - not expanded!\n", sym->name ));
                            continue;
                        }
                        i++;
                        if ( equmode == TRUE ) {
                            i++; /* skip '(' */
                            /* go beyond the ')' */
                            for ( tmp = 1; i < max; i++ ) {
                                if ( tokenarray[i].token == T_OP_BRACKET )
                                    tmp++;
                                else if ( tokenarray[i].token == T_CL_BRACKET ) {
                                    tmp--;
                                    if ( tmp == 0 )
                                        break;
                                }
                            }
                            i--;
                            continue;
                        }
                        //DebugMsg1(("ExpandToken: macro function %s to be expanded\n", sym->name ));
                        i = RunMacro( (struct dsym *)sym, i, tokenarray, buffer,
#if MACROLABEL
                                     (tmp == 1 ? MF_LABEL : 0),
#else
                                     0,
#endif
                                     &is_exitm );
                        if ( i == -1 )
                            return( ERROR );
                        DebugMsg1(("ExpandToken(%s, addbr=%u): macro function expanded to >%s<\n", sym->name, addbrackets, buffer));
                        /* expand text, but don't if macro was at position 0 (might be a text macro definition directive */
                        /* v2.09: don't expand further if addbrackets is set */
                        if ( tmp && (!addbrackets) && ( ERROR == ExpandTMacro( buffer, tokenarray, equmode, 0 ) ) )
                            return( ERROR );
                        /* get size of string to replace ( must be done before AddTokens() */
                        size = ( tokenarray[i-1].tokpos + 1) - tokenarray[tmp].tokpos;
                        AddTokens( tokenarray, tmp+1, tmp+1 - i, Token_Count );
                        Token_Count += (tmp+1) - i;
                        if ( Token_Count < max ) /* take care not to read beyond T_FINAL */
                            max = Token_Count;
                        if ( ERROR == RebuildLine( buffer, tmp, tokenarray,
                                                  size, tokenarray[tmp].tokpos - line, addbrackets ) )
                            return( ERROR );
                        rc = STRING_EXPANDED;
                        i = tmp;
                    } else {
                        /* a macro proc is expanded at pos 0 or pos 2
                         * (or at pos 1 if sym->label is on)
                         */
                        if ( i == 0 ||
                            ( i == 2 && ( tokenarray[1].token == T_COLON ||
                                         tokenarray[1].token == T_DBL_COLON ))
#if MACROLABEL
                            || ( i == 1 && sym->label )
#endif
                           )
                            ;
                        else {
                            DebugMsg1(("ExpandToken(%s): macro proc at pos %u NOT expanded\n", sym->name, i ));
#if 1 /* v2.03: no error, just don't expand! */
                            continue;
#else
                            return( EmitErr( SYNTAX_ERROR_EX, sym->name ) );
#endif
                        }
                        /* v2.08: write optional code label. This has been
                         * moved out from RunMacro().
                         */
                        if ( i == 2 ) {
                            if ( ERROR == WriteCodeLabel( line, tokenarray ) )
                                return( ERROR );
                        }
                        //buffer[0] = NULLC; /* nothing should be returned, just to be safe */
                        DebugMsg1(("ExpandToken(%s): macro proc to be expanded\n", sym->name ));
                        i = RunMacro( (struct dsym *)sym, i+1, tokenarray, NULL,
#if MACROLABEL
                                     MF_NOSAVE | (i == 1 ? MF_LABEL : 0),
#else
                                     MF_NOSAVE,
#endif
                                     &is_exitm );
                        DebugMsg1(("ExpandToken(%s): macro proc called\n", sym->name));
                        if ( i == -1 )
                            return( ERROR );
#if 0
                        /* it's possible to "hide" the EXITM directive when the
                         * macro lines are read. But it's not useful for macro
                         * procs to check if exitm has been executed, because
                         * Masm simply will ignore anything that's "returned".
                         */
                        if ( is_exitm ) {
                            DebugMsg(("ExpandToken: EXITM in macro procedure!\n" ));
                            strcat( buffer, tokenarray[tmp].tokpos );
                            strcpy( line, buffer );
                            rc = STRING_EXPANDED;
                        } else
#endif
                            return( EMPTY ); /* no further processing */
                        break;
                    }
                } else if( sym->state == SYM_TMACRO ) {

                    //GetLiteralValue( buffer, sym->string_ptr );
                    strcpy( buffer, sym->string_ptr );
                    if ( ERROR == ExpandTMacro( buffer, tokenarray, equmode, 0 ) )
                        return( ERROR );
                    DebugMsg1(("ExpandToken(%s, addbr=%u): value >%s< expanded to >%s<\n", sym->name, addbrackets, sym->string_ptr, buffer ));
                    if ( ERROR == RebuildLine( buffer, i, tokenarray, strlen( tokenarray[i].string_ptr ),
                                              tokenarray[i].tokpos - line, addbrackets ) )
                        return( ERROR );
                    rc = STRING_EXPANDED;
                    DebugMsg1(("ExpandToken(%s): rest after expansion: %s\n", sym->name, tokenarray[i].tokpos ));
                }
            }
        }
    }
    *pi = i;
    if ( evaluate ) {
        int old_tokencount = Token_Count;
        if ( i == (pos+1) ) { /* just a single %? */
            opndx.value = 0;
            i = pos;
        } else {
            i = pos++;
            tmp = tokenarray[*pi].tokpos - tokenarray[pos].tokpos;
            memcpy( buffer, tokenarray[pos].tokpos, tmp );
            buffer[tmp] = NULLC;
            tmp = old_tokencount + 1;
            Token_Count = Tokenize( buffer, tmp, tokenarray, TOK_RESCAN );
            if ( EvalOperand( &tmp, tokenarray, Token_Count, &opndx, EXPF_NOUNDEF ) == ERROR )
                opndx.value = 0; /* v2.09: assume value 0, don't return with ERROR */
            else if ( opndx.kind != EXPR_CONST ) {
                /* v2.09: with flag EXPF_NOUNDEF, EvalOperand() will have returned
                 * with error if there's an undefined symbol involved
                 */
                //if ( opndx.sym && opndx.sym->state == SYM_UNDEFINED )
                //    EmitErr( SYMBOL_NOT_DEFINED, opndx.sym->name );
                //else {
                    DebugMsg(("ExpandToken: 'constant expected' error\n"));
                    EmitError( CONSTANT_EXPECTED );
                //}
                //return( ERROR );
                opndx.value = 0; /* assume value 0 */
            }
            Token_Count = old_tokencount;
        }
#if TEVALUE_UNSIGNED
        /* v2.03: Masm compatible: returns an unsigned value */
        myltoa( opndx.value, StringBufferEnd, ModuleInfo.radix, FALSE, FALSE );
#else
        myltoa( opndx.value, StringBufferEnd, ModuleInfo.radix, opndx.hvalue < 0, FALSE );
#endif
            /* v2.05: get size of string to be "replaced" */
        tmp = tokenarray[*pi].tokpos - tokenarray[i].tokpos;
        DebugMsg1(("ExpandToken: curr pos=%u, start expr=%u, expr size=%d\n", *pi, i, tmp ));

        //tokenarray[i].token = T_STRING;
        tokenarray[i].string_ptr = StringBufferEnd;
        AddTokens( tokenarray, i+1, i+1 - *pi, Token_Count );
        Token_Count += (i+1) - *pi;

        if ( ERROR == RebuildLine( StringBufferEnd, i, tokenarray,
                                  tmp, tokenarray[i].tokpos - line, bracket_flags ) )
            return( ERROR );
        rc = STRING_EXPANDED;
    }
    return( rc );
}

/* used by EQU ( may also be used by directives flagged with DF_NOEXPAND
 * if they have to partially expand their arguments ).
 * equmode: 1=don't expand macro functions
 */

int ExpandLineItems( char const line[], int i, struct asm_tok tokenarray[], int addbrackets, int equmode )
/*************************************************************************************************/
{
    int k;
    int lvl;
    int tmp;
    ret_code rc;

    for ( lvl = 0; ; lvl++ ) {
        rc = NOT_ERROR;
        for( k = i; k < Token_Count; ) {
            tmp = ExpandToken( line, &k, tokenarray, Token_Count, addbrackets, equmode );
            if ( tmp == ERROR )
                return( lvl );
            if ( tmp == STRING_EXPANDED )
                rc = STRING_EXPANDED;
            if ( tokenarray[k].token == T_COMMA )
                k++;
        }
        if ( rc == NOT_ERROR )
            break;
        /* expansion happened, re-tokenize and continue! */
        Token_Count = Tokenize( line, i, tokenarray, TOK_RESCAN );
        if ( lvl == MAX_TEXTMACRO_NESTING ) {
            EmitError( MACRO_NESTING_LEVEL_TOO_DEEP );
            break;
        }
    }
    return( lvl );
}

/* v2.09: added, expand literals for structured data items.
 * since 2.09, initialization of structure members is no longer
 * done by generated code, but more directly inside data.c.
 * This improves Masm-compatibility, but OTOH requires to expand
 * the literals explicitly.
 */

void ExpandLiterals( int i, struct asm_tok tokenarray[] )
/*******************************************************/
{
    int cnt = 0;
    int idx;
    /* count non-empty literals */
    for ( idx = i; idx < Token_Count; idx++ ) {
        if ( tokenarray[idx].token == T_STRING &&
            tokenarray[idx].stringlen &&
            ( tokenarray[idx].string_delim == '<' || tokenarray[idx].string_delim == '{' ) ) {
            cnt++;
        }
    }
    /* if non-empty literals are found, expand the line. if the line
     * was expanded, re-tokenize it.
     */
    if ( cnt ) {
        if ( ExpandText( tokenarray[i].tokpos, tokenarray, FALSE ) == STRING_EXPANDED )
            Tokenize( tokenarray[i].tokpos, i, tokenarray, TOK_RESCAN );
    }

}

/* scan current line for (text) macros and expand them.
 * this is only called when the % operator is not the first item.
 */

ret_code ExpandLine( char *string, struct asm_tok tokenarray[] )
/**************************************************************/
{
    int count;
    unsigned int bracket_flags; /* flags */
    int flags;
    int lvl;
    int i;
    int j;
    ret_code rc;
    struct asym *sym;

    /* filter certain conditions.
     * bracket_flags: for (preprocessor) directives that expect a literal
     * parameter, the expanded argument has to be enclosed in '<>' again.
     */
    DebugMsg1(( "ExpandLine(>%s<) enter\n", string ));
    for ( lvl = 0; lvl < MAX_TEXTMACRO_NESTING; lvl++ ) {
        bracket_flags = 0;
        count = 0;
        rc = NOT_ERROR;
        i = ( Token_Count > 2 && ( tokenarray[1].token == T_COLON || tokenarray[1].token == T_DBL_COLON ) && tokenarray[2].token == T_DIRECTIVE ) ? 2 : 0;
        if ( tokenarray[i].token == T_DIRECTIVE ) {
            flags = GetValueSp( tokenarray[i].tokval );
            if ( flags & DF_STRPARM ) {
                bracket_flags = -1;
                /* v2.08 handle .ERRDEF and .ERRNDEF here. Previously
                 * expansion for these directives was handled in condasm.asm,
                 * and the directives were flagged as DF_NOEXPAND.
                 */
                if ( tokenarray[i].dirtype == DRT_ERRDIR ) {
                    if (tokenarray[i].tokval == T_DOT_ERRDEF || tokenarray[i].tokval == T_DOT_ERRNDEF ) {
                        if ( i )
                            rc = ExpandToken( string, &count, tokenarray, 1, FALSE, FALSE );
                        while ( tokenarray[i].token != T_FINAL && tokenarray[i].token != T_COMMA ) i++;
                        count = i; /* don't expand the symbol name */
                    }
                }
            } else if ( flags & DF_NOEXPAND ) {
                /* [ELSE]IF[N]DEF, ECHO, FOR[C]
                 * .[NO|X]CREF, INCLUDE */
                /* don't expand arguments */
                return( NOT_ERROR );
            }
        } else if ( Token_Count > 1 && tokenarray[1].token == T_DIRECTIVE ) {
            switch ( tokenarray[1].dirtype ) {
            case DRT_CATSTR:
                bracket_flags = -1;
                count = 2;
                break;
            case DRT_SUBSTR:
                /* syntax: name SUBSTR <literal>, pos [, size] */
                bracket_flags = 0x1;
                count = 2;
                break;
            case DRT_SIZESTR:
                /* syntax: label SIZESTR literal */
                rc = ExpandToken( string, &count, tokenarray, 1, FALSE, FALSE );
                bracket_flags = 0x1;
                count = 2;
                break;
            case DRT_INSTR:
                /* syntax: label INSTR [number,] literal, literal */
                rc = ExpandToken( string, &count, tokenarray, 1, FALSE, FALSE );
                /* check if the optional <number> argument is given */
                for ( i = 2, count = 0, j = 0; i < Token_Count; i++ ) {
                    if ( tokenarray[i].token == T_OP_BRACKET )
                        count++;
                    else if ( tokenarray[i].token == T_CL_BRACKET )
                        count--;
                    else if ( tokenarray[i].token == T_COMMA && count == 0 )
                        j++;
                }

                bracket_flags = ( ( j > 1 ) ? 0x6 : 0x3 );
                count = 2;
                break;
            case DRT_MACRO:
                sym = SymSearch( tokenarray[0].string_ptr );
                /* don't expand macro DEFINITIONs!
                 * the name is an exception, if it's not the macro itself
                 */
                if ( sym && sym->state != SYM_MACRO )
                    rc = ExpandToken( string, &count, tokenarray, 1, FALSE, FALSE );
                count = Token_Count; /* stop further expansion */
                break;
            case DRT_EQU:
                /* EQU is a special case. If the - expanded - expression is
                 * a number, then the value for EQU is numeric. Else the
                 * expression isn't expanded at all. This effectively makes it
                 * impossible to expand EQU lines here.
                 */
#if 0 /* v2.09: EQU should NEVER be expanded here. See regression test equate20.aso */
                sym = SymSearch( tokenarray[0].string_ptr );
                if ( sym == NULL || sym->state == SYM_TMACRO )
#endif
                    return( NOT_ERROR );
            }
        } else {
            /* v2.08: expand the very first token and then ... */
            rc = ExpandToken( string, &count, tokenarray, 1, FALSE, FALSE );
            if( rc == ERROR || rc == EMPTY )
                return( rc );
            if ( rc == STRING_EXPANDED ) {
                /* ... fully retokenize - the expansion might have revealed a conditional
                 * assembly directive
                 */
                Token_Count = Tokenize( string, 0, tokenarray, TOK_DEFAULT );
                continue;
            }
#if 1 /* v2.10. see regression test equate27.asm */
            if ( count == 1 && tokenarray[0].token == T_ID && tokenarray[1].token == T_ID ) {
                rc = ExpandToken( string, &count, tokenarray, 2, FALSE, FALSE );
                if( rc == ERROR || rc == EMPTY )
                    return( rc );
                if ( rc == STRING_EXPANDED ) {
                    Token_Count = Tokenize( string, 0, tokenarray, TOK_DEFAULT );
                    continue;
                }
            }
#endif
        }
        /* scan the line from left to right for (text) macros.
         * it's currently not quite correct. a macro proc should only
         * be evaluated in the following cases:
         * 1. it is the first token of a line
         * 2. it is the second token, and the first one is an ID
         * 3. it is the third token, the first one is an ID and
         *    the second is a ':' or '::'.
         */
        while ( count < Token_Count ) {
            int tmp;
            int addbrackets;
            addbrackets = bracket_flags & 1;
            if ( bracket_flags != -1 )
                bracket_flags = bracket_flags >> 1;
            tmp = ExpandToken( string, &count, tokenarray, Token_Count, addbrackets, FALSE );
            if( tmp < NOT_ERROR ) /* ERROR or EMPTY? */
                return( tmp );
            if ( tmp == STRING_EXPANDED )
                rc = STRING_EXPANDED;
            if ( tokenarray[count].token == T_COMMA )
                count++;
        }
        if( rc == STRING_EXPANDED ) {
            DebugMsg1(( "ExpandLine(%s): expansion occured, retokenize\n", string ));
            Token_Count = Tokenize( string, 0, tokenarray, TOK_RESCAN | TOK_LINE );
        } else
            break;
    } /* end for() */
    if ( lvl == MAX_TEXTMACRO_NESTING ) {
        return( EmitError( MACRO_NESTING_LEVEL_TOO_DEEP ) );
    }
    DebugMsg1(( "ExpandLine(>%s<) exit, rc=%u, token_count=%u\n", string, rc, Token_Count ));
    return( rc );
}

