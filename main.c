/****************************************************************************
*
*  This code is Public Domain.
*
*  ========================================================================
*
* Description:  JWasm top level module
*
****************************************************************************/

#include <signal.h>

#include "globals.h"
#include "msgtext.h"
#include "cmdline.h"
#include "input.h" /* GetFNamePart() */
#include "codegenv2.h"

#if defined(__UNIX__) || defined(__CYGWIN__) || defined(__DJGPP__)

#define WILDCARDS 0
#define CATCHBREAK 0

#else

#define WILDCARDS 1
#ifdef __POCC__
#define CATCHBREAK 0
#else
#define CATCHBREAK 1
#endif

#endif

#if WILDCARDS

 #ifdef __UNIX__
  #include <unistd.h>
 #else
  #include <io.h>
 #endif
#endif

#ifdef TRMEM
void tm_Init( void );
void tm_Fini( void );
#endif

static void genfailure( int signo )
/*********************************/
{
#if CATCHBREAK
    if (signo != SIGBREAK)
#else
    if (signo != SIGTERM)
#endif
        EmitError( GENERAL_FAILURE );
    close_files();
    exit( EXIT_FAILURE );
}
int main(int argc, char const *argv[])
/*******************************/
{
	char const    *pEnv;
	int     numArgs = 0;
	int     numFiles = 0;
	int     rc = 0;

#if WILDCARDS
	/* v2.11: _findfirst/next/close() handle, should be of type intptr_t.
	* since this type isn't necessarily defined, type long is used as substitute.
	*/

	size_t    fh;
	const char *pfn;
	int     dirsize;
	struct  _finddata_t finfo;
	char    fname[FILENAME_MAX];
#endif

#if 0 //def DEBUG_OUT    /* DebugMsg() cannot be used that early */
	int i;
	for ( i = 1; i < argc; i++ ) {
		printf("argv[%u]=>%s<\n", i, argv[i] );
	}
#endif

#ifdef TRMEM
	tm_Init();
#endif

	pEnv = getenv("UASM");
	if (pEnv == NULL)
		pEnv = "";
	argv[0] = pEnv;
	
	/* Set the default module architecture to SSE */
	//MODULEARCH = ARCH_SSE;

#ifndef DEBUG_OUT
	signal(SIGSEGV, genfailure);
#endif

#if CATCHBREAK
	signal(SIGBREAK, genfailure);
#else
	signal(SIGTERM, genfailure);
#endif

	/* Build CodeGen V2 Instruction Table ONCE only */
	BuildInstructionTable();

#if WILDCARDS
	memset(&finfo, 0, sizeof(finfo));
#endif

		/* ParseCmdLine() returns NULL if no source file name has been found (anymore) */
	while (ParseCmdline((const char **)argv, &numArgs)) {
		numFiles++;
		write_logo();
#if WILDCARDS

		if ((fh = _findfirst(Options.names[ASM], &finfo)) == -1) {
			DebugMsg(("main: _findfirst(%s) failed\n", Options.names[ASM]));
			EmitErr(CANNOT_OPEN_FILE, Options.names[ASM], ErrnoStr());
			break;
		}
		pfn = GetFNamePart(Options.names[ASM]);
		dirsize = pfn - Options.names[ASM];
		memcpy(fname, Options.names[ASM], dirsize);
			do {
				strcpy(&fname[dirsize], finfo.name);
				DebugMsg(("main: fname=%s\n", fname));
				rc = AssembleModule(fname);  /* assemble 1 module */
			} while ((_findnext(fh, &finfo) != -1));
		    _findclose(fh);
#else
		rc = AssembleModule( Options.names[ASM] );
#endif
	};
	CmdlineFini();
	if (numArgs == 0) {
		write_logo();
		printf("%s", MsgGetEx(MSG_USAGE));
	}
	else if (numFiles == 0)
		EmitError(NO_FILENAME_SPECIFIED);

#ifdef TRMEM
	tm_Fini();
#endif

	DebugMsg(("main: exit, return code=%u\n", 1 - rc));
	return(1 - rc); /* zero if no errors */
}


