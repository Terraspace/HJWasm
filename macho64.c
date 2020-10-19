/****************************************************************************
*
*  This code is Public Domain.
*
*  ========================================================================
*
* Description:  MACHO64 output routines
* https://lowlevelbits.org/parsing-mach-o-files/
* https://github.com/aidansteele/osx-abi-macho-file-format-reference
* https://www.reinterpretcast.com/hello-world-mach-o
*
****************************************************************************/

#include <ctype.h>
#include <time.h>
#include <stdint.h>

#include "globals.h"
#include "memalloc.h"
#include "parser.h"
#include "mangle.h"
#include "fixup.h"
#include "segment.h"
#include "extern.h"
#include "macho64.h"
#include "macho64spec.h"
#include "myassert.h"

#define MACHOABI_OSX64 1   // 64bit x86 OSX

#define ROUND_UP( i, r ) (((i)+((r)-1)) & ~((r)-1))

/* String table entry */
struct strentry
{
	char *pstr;
	struct strentry *next;
	struct asym *sym;
	int idx;
	int offset; /* offset into string table */
};

struct macho_section_entry
{
	char const *srcName;
	struct section_64 section;
	struct section_64 *next;
	int dif; /* padding to keep section size multiple of 16 */
	int size;
	int ofs;
	void *data;
	int idx;
	int relocCount;
	uint64_t relocOfs;
};

struct macho_module 
{
	char *srcname;
	struct mach_header_64 header;
	struct segment_command_64 *pSegCmd;
	struct macho_section_entry *sections;
	struct symtab_command *pSymTblCmd;
	struct dysymtab_command *pdySymTblCmd;
	struct strentry *strings;
	int relocCount;
	int sectAlign;
	int symIdx;
	int extSymIdx;
	int undefSymIdx;
	int symCount;
	int extSymCount;
	int undefSymCount;
};

static int padbyte[] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

/* ========================================================================================== 
Calculate the number of sections required for the MACHO64 Obj file 
-> An object file only contains 1 segment with sections, 
-> our internal segments become sections.
========================================================================================== */
int macho_section_count()
{
	int cnt = 0;
	struct dsym *curr;
	for (curr = SymTables[TAB_SEG].head; curr; curr = curr->next)
	{
		if (strcmp(curr->sym.name, "_BSS") == 0)
			cnt++;
		else if (strcmp(curr->sym.name, "CONST") == 0)
			cnt++;
		else if (strcmp(curr->sym.name, "_TEXT") == 0)
			cnt++;
		else if (strcmp(curr->sym.name, "_DATA") == 0)
			cnt++;
	}
	return(cnt);
}

/* ==========================================================================================
Build a segment_command_64 structure.
========================================================================================== */
struct segment_command_64 * macho_build_seg_cmd( int fileofs )
{
	struct segment_command_64 *pCmd = NULL;
	pCmd = malloc(sizeof(struct segment_command_64));
	memset(pCmd,0,sizeof(struct segment_command_64));
	
	pCmd->cmd      = LC_SEGMENT_64;
	pCmd->cmdsize  = sizeof(struct segment_command_64) + (sizeof(struct section_64) * macho_section_count());
	pCmd->flags = 0;
	pCmd->initprot = VM_PROT_ALL;
	pCmd->maxprot  = VM_PROT_ALL;
	pCmd->nsects = macho_section_count();
	pCmd->vmaddr = 0;
	pCmd->fileoff = fileofs;

	/* These attributes need to be completed */
	pCmd->filesize = 0;
	pCmd->vmsize = 0;

	return(pCmd);
}

/* ==========================================================================================
Build a symtabl_command structure.
========================================================================================== */
struct symtab_command * macho_build_symtbl_cmd()
{
	struct symtab_command *pCmd = NULL;
	pCmd = malloc(sizeof(struct symtab_command));
	memset(pCmd, 0, sizeof(struct symtab_command));

	pCmd->cmd = LC_SYMTAB;
	pCmd->cmdsize = sizeof(struct symtab_command);
	pCmd->nsyms = SymGetCount();
	pCmd->symoff = 0;
	pCmd->stroff = 0;
	pCmd->strsize = 0;

	return(pCmd);
}

struct dysymtab_command * macho_build_dysymtbl_cmd()
{
	struct dysymtab_command *pCmd = NULL;
	pCmd = malloc(sizeof(struct dysymtab_command));
	memset(pCmd, 0, sizeof(struct dysymtab_command));

	pCmd->cmd = LC_DYSYMTAB;
	pCmd->cmdsize = sizeof(struct dysymtab_command);

	return(pCmd);
}

/* ==========================================================================================
Add an entry to the string table.
========================================================================================== */
static void macho_add_string(struct strentry *pstr, struct macho_module *mm)
{
	struct strentry *pCurrStr = mm->strings;
	int lastIdx = 1;
	int ofs = 1;
	if (pCurrStr == NULL)
	{
		pstr->idx = 1;
		pstr->offset = 1;
		mm->strings = pstr;
	}
	else
	{
		ofs += strlen(pCurrStr->pstr) + 1;
		while (pCurrStr->next != NULL)
		{
			pCurrStr = pCurrStr->next;
			ofs += strlen(pCurrStr->pstr) + 1;
			lastIdx = pCurrStr->idx;
		}
		pstr->idx = lastIdx + 1;
		pstr->offset = ofs;
		pCurrStr->next = pstr;
	}
	return;
}

/* ==========================================================================================
Create linked list of symbol names (string table)
-> This can be later scanned to determine symbol index (1 based).
========================================================================================== */
int macho_build_string_tbl(struct symtab_command *pSymCmd, struct macho_module *mm)
{
	int tblSize = 0;
	int i = 0;
	struct asym *sym = NULL;
	struct strentry *pstr = NULL;
	int totalSymCount = 0;

	/* Normal local symbols */
	while (sym = SymEnum(sym, &i))
	{
		if (strcmp(sym->name, "$xdatasym") == 0) continue;
		if (sym->state != SYM_MACRO && sym->state != SYM_SEG && sym->state != SYM_TMACRO && sym->predefined == 0 && sym->state != SYM_GRP && sym->isequate == 0)
		{ 
			if (sym->state != SYM_EXTERNAL && !sym->ispublic && sym->used)
			{
				tblSize += strlen(sym->name) + 1;
				pstr = malloc(sizeof(struct strentry));
				memset(pstr, 0, sizeof(struct strentry));
				pstr->pstr = sym->name;
				pstr->sym = sym;
				macho_add_string(pstr, mm);
				mm->symCount++;
				totalSymCount++;
			}
		}
	}
	mm->extSymIdx = totalSymCount;

	/* External public symbols */
	while (sym = SymEnum(sym, &i))
	{
		if (sym->state != SYM_MACRO && sym->state != SYM_SEG && sym->state != SYM_TMACRO && sym->predefined == 0 && sym->state != SYM_GRP && sym->isequate == 0)
		{
			if (sym->ispublic)
			{
				tblSize += strlen(sym->name) + 1;
				pstr = malloc(sizeof(struct strentry));
				memset(pstr, 0, sizeof(struct strentry));
				pstr->pstr = sym->name;
				pstr->sym = sym;
				macho_add_string(pstr, mm);
				mm->extSymCount++;
				totalSymCount++;
			}
		}
	}
	mm->undefSymIdx = totalSymCount;

	/* Undefined symbols */
	while (sym = SymEnum(sym, &i))
	{
		if (sym->state != SYM_MACRO && sym->state != SYM_SEG && sym->state != SYM_TMACRO && sym->predefined == 0 && sym->state != SYM_GRP && sym->isequate == 0)
		{
			if (sym->state == SYM_EXTERNAL)
			{
				tblSize += strlen(sym->name) + 1;
				pstr = malloc(sizeof(struct strentry));
				memset(pstr, 0, sizeof(struct strentry));
				pstr->pstr = sym->name;
				pstr->sym = sym;
				macho_add_string(pstr, mm);
				mm->undefSymCount++;
				totalSymCount++;
			}
		}
	}
	return(tblSize);
}

/* ==========================================================================================
Return the 1-based index of a symbol table entry, based on it's name
NB: symbol table indices in other structures are 0 based.
========================================================================================== */
static int GetSymbolIndex(const char *pName, struct macho_module *mm)
{
	struct strentry *currStr = mm->strings;
	int idx = 0;
	for (currStr = mm->strings;currStr;currStr = currStr->next)
	{
		if (strcmp(pName, currStr->pstr) == 0)
		{
			idx = currStr->idx;
			break;
		}
	}
	return(idx);
}

/* ==========================================================================================
Build a macho_section_entry structure.
========================================================================================== */
struct section_64 * macho_build_section( const char secName[], const char segName[], uint32_t flags, const char srcName[] )
{
	struct macho_section_entry *pSec = NULL;
	pSec = malloc(sizeof(struct macho_section_entry));
	memset(pSec, 0, sizeof(struct macho_section_entry));

	pSec->srcName = srcName;
	strcpy(pSec->section.sectname, secName);
	strcpy(pSec->section.segname, segName);
	pSec->section.align = 0; // 2^0 = 1 byte alignment.
	pSec->section.flags = flags;

	/* addr, size, offset, reloff, nreloc still need to be completed */
	return(pSec);
}

/* ==========================================================================================
Add a macho_section_entry to our module linked list.
========================================================================================== */
static void macho_add_section(struct macho_section_entry *pSec, struct macho_module *mm)
{
	struct macho_section_entry *pCurrSec = mm->sections;
	if (pCurrSec == NULL)
		mm->sections = pSec;
	else
	{
		while (pCurrSec->next != NULL)
		{
			pCurrSec = pCurrSec->next;
		}
		pCurrSec->next = pSec;
	}
	mm->header.sizeofcmds += sizeof(struct section_64);
	return;
}

/* ==========================================================================================
Return MachO section index, based on original source segment name.
========================================================================================== */
static int GetSectionIdx(struct asym *seg, struct macho_module *mm)
{
	struct macho_section_entry *curr = NULL;
	if (seg)
	{
		for (curr = mm->sections; curr; curr = curr->next)
		{
			if (strcmp(seg->name, curr->srcName) == 0)
			{
				return(curr->idx);
			}
		}
	}
	return(0);
}

/* ==========================================================================================
Count the number of fixups in a source segment
========================================================================================== */
static int GetSegFixups(struct dsym *seg)
{
	struct fixup *curr;
	int fixupcnt = 0;
	for (curr = seg->e.seginfo->FixupList.head;curr;curr = curr->nextrlc)
	{
		fixupcnt++;
	}
	return(fixupcnt);
}

/* ==========================================================================================
For each macho section, find correspond source segment and sum count of relocation entries.
========================================================================================== */
static int GetRelocationCount(struct macho_module *mm, int baseOfs)
{
	struct macho_section_entry *curr = NULL;
	struct dsym *seg;
	int relocCnt = 0;
	int relocOfs = 0;

	for (curr = mm->sections; curr; curr = curr->next)
	{
		for (seg = SymTables[TAB_SEG].head; seg; seg = seg->next)
		{
			if (strcmp(seg->sym.name, curr->srcName) == 0)
			{
				curr->relocOfs = baseOfs + relocOfs;
				relocOfs += (GetSegFixups(seg) * sizeof(struct relocation_info));
				relocCnt += GetSegFixups(seg);
				curr->relocCount = GetSegFixups(seg);
				curr->section.nreloc = curr->relocCount;
				curr->section.reloff = curr->relocOfs;
				if (curr->section.nreloc == 0)
					curr->section.reloff = 0;
				break;
			}
		}
	}
	return(relocCnt);
}

/* ==========================================================================================
Create all the macho obj file structures and calculate offsets.
========================================================================================== */
static void macho_build_structures( struct module_info *modinfo, struct macho_module mm )
{
	int cnt = 0;
	struct dsym *curr;
	struct dsym *seg = NULL;
	struct macho_section_entry *currSec;
	struct strentry *currStr;
	int fileofs = sizeof(struct mach_header_64);
	uint64_t currAddr = 0; /* the first section starts in the 1 segment at vmaddr == 0 */
	struct segment_command_64 *pCmd = NULL;
	struct symtab_command *pSymCmd = NULL;
	struct dysymtab_command *pdySymCmd = NULL;
	int sectionDataSize = 0;
	int stringTableSize = 0;
	int secIdx = 1;
	struct nlist_64 symEntry;
	int relocDataSize = 0;
	struct relocation_info reloc;
	struct fixup *currFixup;
	uint_8 machotype;
	struct version_min_command ver;
	int ofsAdj = 0;

	/* Set section alignment in bytes */
	mm.sectAlign = 1;

	/* Setup the load segment command */
	pCmd = macho_build_seg_cmd( fileofs );
	mm.pSegCmd = pCmd;
	mm.header.ncmds++;
	mm.header.sizeofcmds += sizeof(struct segment_command_64);
	fileofs += sizeof(struct segment_command_64);

	/* Setup the min version structure */
	ver.cmd = LC_VERSION_MIN_MACOSX;
	ver.cmdsize = sizeof(struct version_min_command);
	ver.version = 0x000a0c00; // 10.12
	ver.sdk = 0;
	mm.header.ncmds++;
	mm.header.sizeofcmds += sizeof(struct version_min_command);
	fileofs += sizeof(struct version_min_command);

	/* For every internal segment, create a section */
	for (curr = SymTables[TAB_SEG].head; curr; curr = curr->next)
	{
		if (strcmp(curr->sym.name, "_TEXT") == 0)
		{
			currSec = macho_build_section("__text", "__TEXT", S_REGULAR, curr->sym.name);
			macho_add_section(currSec, &mm);
			currSec->data = curr->e.seginfo->CodeBuffer;
			currSec->size = ROUND_UP(curr->e.seginfo->bytes_written,mm.sectAlign);
			currSec->dif = currSec->size - curr->e.seginfo->bytes_written;
			currSec->section.size = currSec->size;
			currSec->section.addr = currAddr;
			currAddr += currSec->size;
			currAddr = ROUND_UP(currAddr, mm.sectAlign); 
			pCmd->filesize += currSec->size;
			currSec->idx = secIdx++;
			currSec->section.flags |= (S_ATTR_SOME_INSTRUCTIONS | S_ATTR_PURE_INSTRUCTIONS);
			fileofs += sizeof(struct section_64);
			pCmd->vmsize += currSec->size;
		}
		
		else if (strcmp(curr->sym.name, "_DATA") == 0)
		{
			currSec = macho_build_section("__data", "__DATA", S_REGULAR, curr->sym.name);
			macho_add_section(currSec, &mm);
			currSec->data = curr->e.seginfo->CodeBuffer;
			currSec->size = ROUND_UP(curr->e.seginfo->bytes_written, mm.sectAlign);
			currSec->dif = currSec->size - curr->e.seginfo->bytes_written;
			currSec->section.size = currSec->size;
			currSec->section.addr = currAddr;
			currAddr += currSec->size;
			currAddr = ROUND_UP(currAddr, mm.sectAlign);
			pCmd->filesize += currSec->size;
			currSec->idx = secIdx++;
			fileofs += sizeof(struct section_64);
			pCmd->vmsize += currSec->size;
		}

		else if (strcmp(curr->sym.name, "CONST") == 0)
		{
			currSec = macho_build_section("_rdata", "__DATA", S_REGULAR, curr->sym.name);
			macho_add_section(currSec, &mm);
			currSec->data = curr->e.seginfo->CodeBuffer;
			currSec->size = ROUND_UP(curr->e.seginfo->bytes_written, mm.sectAlign);
			currSec->dif = currSec->size - curr->e.seginfo->bytes_written;
			currSec->section.size = currSec->size;
			currSec->section.addr = currAddr;
			currAddr += currSec->size;
			currAddr = ROUND_UP(currAddr, mm.sectAlign);
			pCmd->filesize += currSec->size;
			currSec->idx = secIdx++;
			fileofs += sizeof(struct section_64);
			pCmd->vmsize += currSec->size;
		}

		else if (strcmp(curr->sym.name, "_BSS") == 0)
		{
			currSec = macho_build_section("__bss", "__DATA", S_ZEROFILL, curr->sym.name);
			macho_add_section(currSec, &mm);
			currSec->data = curr->e.seginfo->CodeBuffer;
			currSec->size = ROUND_UP(curr->e.seginfo->bytes_written, mm.sectAlign);
			currSec->dif = currSec->size - curr->e.seginfo->bytes_written;
			currSec->section.size = currSec->size;
			currSec->section.addr = currAddr;
			currAddr = ROUND_UP(currAddr, mm.sectAlign);
			currAddr += currSec->size;
			currSec->idx = secIdx++;
			fileofs += sizeof(struct section_64);
			pCmd->vmsize += currSec->size;

		}
	}
	/* At this point only the section offsets (file) need to still be set 
	-> Once we have the size of the remaining commands. */

	/* Build sym table command */
	pSymCmd = macho_build_symtbl_cmd();
	mm.pSymTblCmd = pSymCmd;
	mm.header.ncmds++;
	mm.header.sizeofcmds += sizeof(struct symtab_command);
	fileofs += sizeof(struct symtab_command);

	/* Build dsym table command */
	pdySymCmd = macho_build_dysymtbl_cmd();
	mm.pdySymTblCmd = pdySymCmd;
	mm.header.ncmds++;
	mm.header.sizeofcmds += sizeof(struct dysymtab_command);
	fileofs += sizeof(struct dysymtab_command);

	/* Set section file offsets */
	sectionDataSize = 0;
	for (currSec = mm.sections;currSec;currSec = currSec->next)
	{
		currSec->section.offset = fileofs + sectionDataSize;
		currSec->ofs = fileofs + sectionDataSize;
		sectionDataSize += currSec->size;
	}
	pCmd->fileoff = mm.sections->section.offset; // Set segment command file offset = first section offset.

	/* Build the string table data */
	stringTableSize = macho_build_string_tbl(pSymCmd,&mm) + 1; /* add one for the "" string entry */

	/* Update number of symbols being written to object file */
	pSymCmd->nsyms = mm.symCount;

	/* Build relocation data and get size */
	mm.relocCount = GetRelocationCount(&mm, fileofs + sectionDataSize);
	relocDataSize = mm.relocCount * sizeof(struct relocation_info);

	/* Set dsym relocation data offset and size */
	pdySymCmd->nlocalsym = mm.symCount;
	pdySymCmd->ilocalsym = 0; /* normal local symbols always start at index 0 */
	pdySymCmd->nextdefsym = mm.extSymCount;
	pdySymCmd->iextdefsym = mm.extSymIdx;
	pdySymCmd->nundefsym = mm.undefSymCount;
	pdySymCmd->iundefsym = mm.undefSymIdx;

	/* Set symbol table sizes and offsets */
	pSymCmd->nsyms = mm.symCount + mm.extSymCount + mm.undefSymCount;
	pSymCmd->symoff = fileofs + sectionDataSize + relocDataSize;
	pSymCmd->strsize = stringTableSize;
	pSymCmd->stroff = fileofs + sectionDataSize + (sizeof(struct nlist_64)*pSymCmd->nsyms) + relocDataSize;

	/* Write out the macho header */
	if (fwrite(&mm.header, 1, sizeof(mm.header), CurrFile[OBJ]) != sizeof(mm.header))
		WriteError();
	
	/* Write out the segment command */
	if (fwrite(pCmd, 1, sizeof(struct segment_command_64), CurrFile[OBJ]) != sizeof(struct segment_command_64))
		WriteError();

	/* Write out the section_64 list */
	for (currSec = mm.sections;currSec;currSec = currSec->next)
	{
		if (fwrite(&currSec->section, 1, sizeof(struct section_64), CurrFile[OBJ]) != sizeof(struct section_64))
			WriteError();
	}

	/* Write out OSX min version command */
	if (fwrite(&ver, 1, sizeof(struct version_min_command), CurrFile[OBJ]) != sizeof(struct version_min_command))
		WriteError();

	/* Write out symbol table commands */
	if (fwrite(pSymCmd, 1, sizeof(struct symtab_command), CurrFile[OBJ]) != sizeof(struct symtab_command))
		WriteError();
	if (fwrite(pdySymCmd, 1, sizeof(struct dysymtab_command), CurrFile[OBJ]) != sizeof(struct dysymtab_command))
		WriteError();

	/* Write out the section data */
	for (currSec = mm.sections;currSec;currSec = currSec->next)
	{
		if (fwrite(currSec->data, 1, currSec->size-currSec->dif, CurrFile[OBJ]) != currSec->size-currSec->dif)
			WriteError();
		if (currSec->dif > 0)
		{
			fwrite(&padbyte, 1, currSec->dif, CurrFile[OBJ]);
		}
	}

	/* Write out relocation entries */
	for (currSec = mm.sections; currSec; currSec = currSec->next)
	{
		for (seg = SymTables[TAB_SEG].head; seg; seg = seg->next)
		{
			if (strcmp(seg->sym.name, currSec->srcName) == 0)
			{
				for (currFixup = seg->e.seginfo->FixupList.head; currFixup; currFixup = currFixup->nextrlc)
				{
					memset(&reloc, 0, sizeof(struct relocation_info));

					reloc.r_address = currFixup->locofs;
					if (currFixup->sym)
					{
						reloc.r_extern = 1;
						reloc.r_symbolnum = GetSymbolIndex(currFixup->sym->name, &mm) - 1;
						reloc.r_pcrel = 1;
					}
					else
					{
						reloc.r_extern = 0;
						reloc.r_symbolnum = currSec->idx;
						reloc.r_pcrel = 1;
					}
					if (currFixup->type == FIX_RELOFF32 && currFixup->addbytes == 4 && currFixup->option == OPTJ_NONE)
					{
						machotype = X86_64_RELOC_SIGNED;
					}
					else if (currFixup->type == FIX_RELOFF32 && (currFixup->option == OPTJ_EXPLICIT || currFixup->option == OPTJ_CALL || currFixup->option == OPTJ_JXX))
					{
						machotype = X86_64_RELOC_BRANCH;
					}
					else
					{
						machotype = X86_64_RELOC_SIGNED;
					}
	
					reloc.r_type = machotype;
					switch (currFixup->addbytes)
					{
						case 8:
							reloc.r_length = 3;
							break;
						case 4:
							reloc.r_length = 2;
							break;
						case 2:
							reloc.r_length = 1;
							break;
						default:
							reloc.r_length = 0;
					}

					if (fwrite(&reloc, 1, sizeof(struct relocation_info), CurrFile[OBJ]) != sizeof(struct relocation_info))
						WriteError();
				}
				break;
			}
		}
	}

	/* Write out symbol table entries */
	for (currStr = mm.strings;currStr;currStr = currStr->next)
	{
		if (!currStr->sym->used && !currStr->sym->ispublic)
			continue;

		memset(&symEntry, 0, sizeof(struct nlist_64));

		if (currStr->sym->state == SYM_EXTERNAL)
		{
			symEntry.n_sect = NO_SECT;
			symEntry.n_type = N_UNDF | N_EXT;
			symEntry.n_value = currStr->sym->total_size;
			symEntry.n_un.n_strx = currStr->offset;
		}
		else
		{
			symEntry.n_sect = GetSectionIdx(currStr->sym->segment, &mm);
			if (currStr->sym->ispublic)
				symEntry.n_type = N_SECT | N_EXT;
			else
				symEntry.n_type = N_SECT;
			symEntry.n_value = (uint64_t)currStr->sym->offset;
			
			/* The offset is relative to the start of the segment, so we must add any previous sections total sizes */
			ofsAdj = 0;
			for (currSec = mm.sections;currSec->idx < symEntry.n_sect;currSec = currSec->next)
				ofsAdj += currSec->size;
			symEntry.n_value += ofsAdj;

			symEntry.n_un.n_strx = currStr->offset;
		}

		symEntry.n_desc = 0;
		if (currStr->sym->weak)
			symEntry.n_desc |= N_WEAK_DEF;

		if (fwrite(&symEntry, 1, sizeof(struct nlist_64), CurrFile[OBJ]) != sizeof(struct nlist_64))
			WriteError();

		fileofs += sizeof(struct nlist_64);
	}

	/* Write out string table */
	if (fwrite(&padbyte, 1, 1, CurrFile[OBJ]) != 1)	/* Write index 0 as the default null ("") string */
		WriteError(); 
	for (currStr = mm.strings;currStr;currStr = currStr->next)
	{
		int len = strlen(currStr->pstr) + 1;
		if (fwrite(currStr->pstr, 1, len, CurrFile[OBJ]) != len)
			WriteError();
	}
}

static ret_code macho_write_module( struct module_info *modinfo )
{
	struct macho_module mm;
	int fileofs = 0;

	DebugMsg(("macho_write_module: enter\n"));

	memset(&mm, 0, sizeof(mm));
	mm.srcname = CurrFName[ASM];
#if 1
	/* the path part is stripped. todo: check if this is ok to do */
	mm.srcname += strlen(mm.srcname);
	while (mm.srcname > CurrFName[ASM] &&
		*(mm.srcname - 1) != '/' &&
		*(mm.srcname - 1) != '\\') mm.srcname--;
#endif
	/* position at 0 ( probably unnecessary, since there were no writes yet ) */
	fseek(CurrFile[OBJ], 0, SEEK_SET);
	
	if( modinfo->defOfssize == USE64)
	{
		mm.header.magic = MH_MAGIC_64;
		mm.header.cputype = CPU_TYPE_X86_64;
		mm.header.cpusubtype = CPU_SUBTYPE_LITTLE_ENDIAN | CPU_SUBTYPE_X86_64_ALL;
		mm.header.filetype = MH_OBJECT;
		mm.header.flags = NULL;
		
		macho_build_structures(modinfo, mm);	
	}
	else
	{
		EmitError(MACHO_NOT_64);
		return(ERROR);
	}

	DebugMsg(("macho_write_module: exit\n"));
	return(NOT_ERROR);
}

void macho_init( struct module_info *modinfo )
{
	modinfo->osx_osabi = MACHOABI_OSX64;
	modinfo->g.WriteModule = macho_write_module;
	return;
}
