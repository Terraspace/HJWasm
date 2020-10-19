
#ifndef _PREPROC_H_INCLUDED
#define _PREPROC_H_INCLUDED

/* v2.11: preprocessor functions moved from input.c to preproc.c */

extern int      PreprocessLine( char *, struct asm_tok[] );
extern ret_code WriteCodeLabel( char const [], struct asm_tok[] );

#endif
