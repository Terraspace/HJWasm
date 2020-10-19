#include <ctype.h>
#include "codegenv2.h"

#include <time.h>
#include "globals.h"
#include "parser.h"
#include "segment.h"
#include "extern.h"
#include "fixup.h"
#include "fastpass.h"
#include "myassert.h"
#include "types.h"
#include "macro.h"
#include "listing.h"

#define OutputCodeByte( x ) OutputByte( x )

const char        szNullStr[] = { "<NULL>" };
struct Mem_Def* MemTable = NULL;
struct Instr_Def* InstrHash[16384];

#ifdef _WIN32
#else
	#define INT_MIN     (-2147483647 - 1) /* minimum (signed) int value */
	#define INT_MAX       2147483647    /* maximum (signed) int value */
	#define UINT_MAX      0xffffffff    /* maximum unsigned int value */
	#define UCHAR_MAX     0xff      /* maximum unsigned char value */
#endif

#include "MemTable32.h"
#include "MemTable64.h"
#include "InstrTableV2.h"

static unsigned int hash(const uint_8* data, int size)
/******************************************/
{
	uint_64 fnv_basis = 14695981039346656037U;
	uint_64 register fnv_prime = 1099511628211;
	uint_64 h = fnv_basis;
	int cnt = 0;
	for (cnt = 0; cnt < size; cnt++) {
		h ^= *data++;
		h *= fnv_prime;
	}
	return((((h >> 49) ^ h) & 0x3fff));
}

struct Instr_Def* AllocInstruction()
{
	return malloc(sizeof(struct Instr_Def));
}

void InsertInstruction(struct Instr_Def* pInstruction, uint_32 hash)
{
	struct Instr_Def* curPtr = NULL;
	curPtr = InstrHash[hash];
	if (curPtr == NULL)
	{
		InstrHash[hash] = pInstruction;
		return;
	}
	while (curPtr->next != NULL)
	{
		curPtr = curPtr->next;
	}
	curPtr->next = pInstruction;
}

uint_32 GenerateInstrHash(struct Instr_Def* pInstruction)
{
	uint_8 hashBuffer[32];
	int len = 0;
	char* pDst = (char*)&hashBuffer;
	strcpy(pDst, pInstruction->mnemonic);

	/* String hash is case-insensitive. */
	for (int i = 0; i < strlen(pInstruction->mnemonic); i++)
	{
		hashBuffer[i] = tolower(hashBuffer[i]);
	}

	len += strlen(pInstruction->mnemonic);
	pDst += len;
	*(pDst + 0) = pInstruction->operand_types[0];
	*(pDst + 1) = pInstruction->operand_types[1];
	*(pDst + 2) = pInstruction->operand_types[2];
	*(pDst + 3) = pInstruction->operand_types[3];
	*(pDst + 4) = pInstruction->operand_types[4];
	len += 4;
	pDst += 4;
	return hash(hashBuffer, len);
}

void BuildInstructionTable(void)
{
	uint_32 hash = 0;
	struct Instr_Def* pInstrTbl = InstrTableV2;
	uint_32 i = 0;
	uint_32 instrCount = sizeof(InstrTableV2) / sizeof(struct Instr_Def);

	memset(InstrHash, 0, sizeof(InstrHash));

	for (i = 0; i < instrCount; i++, pInstrTbl++)
	{
		struct Instr_Def* pInstr = AllocInstruction();
		memcpy(pInstr, pInstrTbl, sizeof(struct Instr_Def));
		hash = GenerateInstrHash(pInstr);
		InsertInstruction(pInstr, hash);
	}
}

/* =====================================================================
  Some instruction forms require specific registers such as AX or CL.
  Demotion allows us to check the instruction table twice, once using the explicit register should it exist,
  secondly after demotion to look for a generic case.
===================================================================== */
enum op_type DemoteOperand(enum op_type op) {
	enum op_type ret = op;

	if (op == R8_AL)
		ret = R8;
	else if (op == R16_AX)
		ret = R16;
	else if (op == R32_EAX)
		ret = R32;
	else if (op == R64_RAX)
		ret = R64;
	else if (op == R8_CL)
		ret = R8;
	else if (op == R16_CX)
		ret = R16;
	else if (op == R32_ECX)
		ret = R32;
	else if (op == R64_RCX)
		ret = R64;
	else if (op == R16_DX)
		ret = R16;

	/* We must be careful that an instruction can only have one demotable operand at a time */
	else if (op == M8 || op == M16 || op == M32 || op == M48 || op == M64 || op == M80 || op == M128 || op == M256 || op == M512)
		ret = M_ANY;

	return(ret);
}

enum op_type MatchOperand(struct code_info* CodeInfo, struct opnd_item op, struct expr opExpr) {
	enum op_type result;
	switch (op.type)
	{
	case OP_M:
		result = M_ANY;
		break;
	case OP_M08:
		result = M8;
		break;
	case OP_M16:
		result = M16;
		break;
	case OP_M32:
		result = M32;
		break;
	case OP_M64:
		result = M64;
		break;
	case OP_M48:
		result = M48;
		break;
	case OP_M80:
		result = M80;
		break;
	case OP_M128:
		result = M128;
		break;
	case OP_M256:
		result = M256;
		break;
	case OP_M512:
		result = M512;
		break;
	case OP_SR86:
		result = R_SEG;
		break;
	case OP_SR:
		result = R_SEG;
		break;
	case OP_SR386:
		result = R_SEGE;
		break;
	case OP_RSPEC:
		result = R_RIP;
		/* If the register operand is cr0-cr8 (Parser error generates OP.type == RSPEC for these) */
		if (strcasecmp(opExpr.base_reg->string_ptr, "cr0") == 0 ||
			strcasecmp(opExpr.base_reg->string_ptr, "cr2") == 0 ||
			strcasecmp(opExpr.base_reg->string_ptr, "cr3") == 0 ||
			strcasecmp(opExpr.base_reg->string_ptr, "cr4") == 0)
		{
			result = R_CR;
		}
		else if (strcasecmp(opExpr.base_reg->string_ptr, "cr8") == 0)
		{
			result = R_CR8;
		}
		/* If the register operand is dr0-dr7 (Parser error generates OP.type == R_RIP for these) */
		else if (strcasecmp(opExpr.base_reg->string_ptr, "dr0") == 0 ||
			strcasecmp(opExpr.base_reg->string_ptr, "dr1") == 0 ||
			strcasecmp(opExpr.base_reg->string_ptr, "dr2") == 0 ||
			strcasecmp(opExpr.base_reg->string_ptr, "dr3") == 0 ||
			strcasecmp(opExpr.base_reg->string_ptr, "dr4") == 0 ||
			strcasecmp(opExpr.base_reg->string_ptr, "dr5") == 0 ||
			strcasecmp(opExpr.base_reg->string_ptr, "dr6") == 0 ||
			strcasecmp(opExpr.base_reg->string_ptr, "dr7") == 0)
		{
			result = R_DR;
		}
		break;
	case OP_K:
		result = R_K;
		break;
	case OP_RIP:
		result = R_RIP;
		/* If the register operand is cr0-cr8 (Parser error generates OP.type == R_RIP for these) */
		if (strcasecmp(opExpr.base_reg->string_ptr, "cr0") == 0 ||
			strcasecmp(opExpr.base_reg->string_ptr, "cr2") == 0 ||
			strcasecmp(opExpr.base_reg->string_ptr, "cr3") == 0 ||
			strcasecmp(opExpr.base_reg->string_ptr, "cr4") == 0 ||
			strcasecmp(opExpr.base_reg->string_ptr, "cr8") == 0)
		{
			result = R_CR;
		}
		else if (strcasecmp(opExpr.base_reg->string_ptr, "cr8") == 0)
		{
			result = R_CR8;
		}
		/* If the register operand is dr0-dr7 (Parser error generates OP.type == R_RIP for these) */
		else if (strcasecmp(opExpr.base_reg->string_ptr, "dr0") == 0 ||
			strcasecmp(opExpr.base_reg->string_ptr, "dr1") == 0 ||
			strcasecmp(opExpr.base_reg->string_ptr, "dr2") == 0 ||
			strcasecmp(opExpr.base_reg->string_ptr, "dr3") == 0 ||
			strcasecmp(opExpr.base_reg->string_ptr, "dr4") == 0 ||
			strcasecmp(opExpr.base_reg->string_ptr, "dr5") == 0 ||
			strcasecmp(opExpr.base_reg->string_ptr, "dr6") == 0 ||
			strcasecmp(opExpr.base_reg->string_ptr, "dr7") == 0)
		{
			result = R_DR;
		}
		break;
	case OP_AL:
		result = R8_AL;
		break;
	case OP_CL:
		result = R8_CL;
		break;
	case OP_AX:
		result = R16_AX;
		break;
	case OP_EAX:
		result = R32_EAX;
		break;
	case OP_RAX:
		result = R64_RAX;
		break;
	case OP_NONE:
		result = OP_N;
		break;
	case OP_R8:
		result = R8;

		/* If AL is somehow passed in as an OP_R8, promote it first */
		if (opExpr.base_reg->tokval == T_AL)
			result = R8_AL;

		/* If the register operand is ah,bh,ch,dh */
		if (opExpr.base_reg->tokval >= T_AH && opExpr.base_reg->tokval <= T_BH)
			result = R8H;

		/* If the register operand is r8b-r15b */
		else if (opExpr.base_reg->tokval >= T_R8B && opExpr.base_reg->tokval <= T_R15B)
			result = R8E;

		else if (opExpr.base_reg->tokval >= T_SPL && opExpr.base_reg->tokval <= T_DIL)
			result = R8U;
		break;

	case OP_DX:
		result = R16_DX;
		break;

	case OP_R16:
		result = R16;

		/* If AX is somehow passed in as an OP_R16, promote it first */
		if (opExpr.base_reg->tokval == T_AX)
			result = R16_AX;

		/* If the register operand is r8w-r15w */
		else if (opExpr.base_reg->tokval >= T_R8W && opExpr.base_reg->tokval <= T_R15W)
			result = R16E;
		break;

	case OP_R32:
		result = R32;

		/* If EAX is somehow passed in as an OP_R32, promote it first */
		if (opExpr.base_reg->tokval == T_EAX)
			result = R32_EAX;

		/* If the register operand is r8d-r15d */
		else if (opExpr.base_reg->tokval >= T_R8D && opExpr.base_reg->tokval <= T_R15D)
			result = R32E;
		break;

	case OP_R64:
		result = R64;

		/* If RAX is somehow passed in as an OP_R64, promote it first */
		if (opExpr.base_reg->tokval == T_RAX)
			result = R64_RAX;

		/* If the register operand is r8-r15 */
		else if (opExpr.base_reg->tokval >= T_R8 && opExpr.base_reg->tokval <= T_R15)
			result = R64E;
		break;

	case OP_I8:
		result = IMM8;
		break;
	case OP_I16:
		result = IMM16;
		break;
	case OP_I32:
		result = IMM32;
		break;
	case OP_I48:
		result = IMM48;
		break;
	case OP_I64:
		result = IMM64;
		break;
	case OP_XMM:
		result = R_XMM;
		break;
	case OP_YMM:
		result = R_YMM;
		break;
	case OP_ZMM:
		result = R_ZMM;
		break;
	case OP_MMX:
		result = MMX64;
	case OP_ST:
		result = R_ST0;
		break;
	case OP_ST_REG:
		result = R_ST;
		break;
	}
	return result;
}

struct Instr_Def* LookupInstruction(struct Instr_Def* instr, bool memReg, unsigned char encodeMode, int srcRegNo, int dstRegNo, struct code_info* CodeInfo)
{
	uint_32           hash;
	struct Instr_Def* pInstruction = NULL;
	bool              matched = FALSE;

	hash = GenerateInstrHash(instr);
	pInstruction = InstrHash[hash];
	while (pInstruction != NULL)
	{
		if (strcasecmp(pInstruction->mnemonic, instr->mnemonic) == 0 &&
			pInstruction->operand_types[0] == instr->operand_types[0] &&
			pInstruction->operand_types[1] == instr->operand_types[1] &&
			pInstruction->operand_types[2] == instr->operand_types[2] &&
			pInstruction->operand_types[3] == instr->operand_types[3] &&
			(pInstruction->validModes & encodeMode) != 0)
		{
			/* If the instruction only supports absolute or displacement only addressing
			and we have a register in the memory expression, this is not a match */
			if (memReg && ((pInstruction->flags & NO_MEM_REG) != 0))
				goto nextInstr;

			/* We have duplicate entries in the table when there is a limiting factor based on register no, rule it out, this doesn't apply to evex */
			if ((((uint_32)pInstruction->flags & (uint_32)SRCHDSTL) != 0) && ((srcRegNo <= 7 && dstRegNo > 7) || (srcRegNo <= 7 && dstRegNo <= 7) || (srcRegNo > 7 && dstRegNo > 7) || CodeInfo->evex_flag))
				goto nextInstr;

			/* Here we match broadcast size and element count v2.50 */
			if (broadflags) {
				if (CodeInfo->token == T_VCVTPD2PS || CodeInfo->token == T_VCVTTPD2DQ) {
					if ((pInstruction->op_elements == 2) && (broadflags == 0x10)) {
						if (broadflags == pInstruction->op_size)
							;
					}
					else if ((pInstruction->op_elements == 4) && (broadflags == 0x20)) {
						if (broadflags == pInstruction->op_size)
							;
					}
					else if ((pInstruction->op_elements == 8) && (broadflags == 0x30)) {
						if (pInstruction->op_size == 0X40)
							;
					}
					else goto nextInstr; /* here we can throw an error because these 3 were the only correct options */
				}
			}
			matched = TRUE;
			break;
		}
	nextInstr:
		pInstruction = pInstruction->next;
	}
	if (!matched)
		pInstruction = NULL;
	return pInstruction;
}

bool Require_OPND_Size_Override(struct Instr_Def* instr, struct code_info* CodeInfo)
{
	if (instr->useOSO == OP_SIZE_OVERRIDE)
	{
		if (instr->op_size == 2 && (ModuleInfo.Ofssize == USE32 || ModuleInfo.Ofssize == USE64))
			return TRUE;
		if (instr->op_size == 4 && ModuleInfo.Ofssize == USE16)
			return TRUE;
	}
	return FALSE;
}

bool Require_ADDR_Size_Override(struct Instr_Def* instr, struct code_info* CodeInfo)
{
	return FALSE;
}

bool IsValidInCPUMode(struct Instr_Def* instr)
{
	bool          result = TRUE;
	unsigned char cpuModes = instr->validModes;

	/* Don't allow a 64bit instruction in a non 64bit segment */
	if (ModuleInfo.Ofssize != USE64 && cpuModes == X64)
		result = FALSE;

	if (ModuleInfo.Ofssize == USE64 && (cpuModes & X64) == 0)
		result = FALSE;

	/* Are we allowing assembly of priviledge instructions? */
	if ((instr->cpu & P_PM) > (ModuleInfo.curr_cpu & P_PM))
		result = FALSE;

	return result;
}

/* =====================================================================
  Given an input token (string) for a register name, match it and return
  the correct register number for encoding reg/rm fields.
  ===================================================================== */
unsigned char GetRegisterNo(struct asm_tok* regTok)
{
	unsigned char regNo = 17;
	if (regTok)
	{
		if (regTok->tokval >= T_RAX && regTok->tokval <= T_RIP)
			regNo = (unsigned char)(regTok->tokval - T_RAX);
		else if (regTok->tokval >= T_EAX && regTok->tokval <= T_EDI)
			regNo = (unsigned char)(regTok->tokval - T_EAX);
		else if (regTok->tokval >= T_R8D && regTok->tokval <= T_R15D)
			regNo = (unsigned char)((regTok->tokval - T_R8D) + 8);
		else if (regTok->tokval >= T_R8W && regTok->tokval <= T_R15W)
			regNo = (unsigned char)((regTok->tokval - T_R8W) + 8);
		else if (regTok->tokval >= T_AL && regTok->tokval <= T_BH)
			regNo = (unsigned char)(regTok->tokval - T_AL);
		else if (regTok->tokval >= T_R8B && regTok->tokval <= T_R15B)
			regNo = (unsigned char)((regTok->tokval - T_R8B) + 8);
		else if (regTok->tokval >= T_AX && regTok->tokval <= T_DI)
			regNo = (unsigned char)(regTok->tokval - T_AX);

		else if (regTok->tokval >= T_MM0 && regTok->tokval <= T_MM7)
			regNo = (unsigned char)(regTok->tokval - T_MM0);

		else if (regTok->tokval >= T_XMM0 && regTok->tokval <= T_XMM7)
			regNo = (unsigned char)(regTok->tokval - T_XMM0);
		else if (regTok->tokval >= T_XMM8 && regTok->tokval <= T_XMM15)
			regNo = (unsigned char)((regTok->tokval - T_XMM8) + 8);
		else if (regTok->tokval >= T_XMM16 && regTok->tokval <= T_XMM23)
			regNo = (unsigned char)((regTok->tokval - T_XMM16) + 16);
		else if (regTok->tokval >= T_XMM24 && regTok->tokval <= T_XMM31)
			regNo = (unsigned char)((regTok->tokval - T_XMM24) + 24);

		else if (regTok->tokval >= T_YMM0 && regTok->tokval <= T_YMM7)
			regNo = (unsigned char)(regTok->tokval - T_YMM0);
		else if (regTok->tokval >= T_YMM8 && regTok->tokval <= T_YMM15)
			regNo = (unsigned char)((regTok->tokval - T_YMM8) + 8);
		else if (regTok->tokval >= T_YMM16 && regTok->tokval <= T_YMM23)
			regNo = (unsigned char)((regTok->tokval - T_YMM16) + 16);
		else if (regTok->tokval >= T_YMM24 && regTok->tokval <= T_YMM31)
			regNo = (unsigned char)((regTok->tokval - T_YMM24) + 24);

		else if (regTok->tokval >= T_ZMM0 && regTok->tokval <= T_ZMM7)
			regNo = (unsigned char)(regTok->tokval - T_ZMM0);
		else if (regTok->tokval >= T_ZMM8 && regTok->tokval <= T_ZMM31)
			regNo = (unsigned char)((regTok->tokval - T_ZMM8) + 8);

		else if (regTok->tokval >= T_K0 && regTok->tokval <= T_K7)
			regNo = (unsigned char)(regTok->tokval - T_K0);

		else
		{
			switch (regTok->tokval)
			{
				/* 8bit */
			case T_SPL:
				regNo = 4;
				break;
			case T_BPL:
				regNo = 5;
				break;
			case T_SIL:
				regNo = 6;
				break;
			case T_DIL:
				regNo = 7;
				break;
			case T_CR0:
				regNo = 0;
				break;
			case T_CR2:
				regNo = 2;
				break;
			case T_CR3:
				regNo = 3;
				break;
			case T_CR4:
				regNo = 4;
				break;
			case T_CR8:
				regNo = 8;
				break;
			case T_DR0:
				regNo = 0;
				break;
			case T_DR1:
				regNo = 1;
				break;
			case T_DR2:
				regNo = 2;
				break;
			case T_DR3:
				regNo = 3;
				break;
			case T_DR6:
				regNo = 6;
				break;
			case T_DR7:
				regNo = 7;
				break;
			case T_CS:
				regNo = 1;
				break;
			case T_DS:
				regNo = 3;
				break;
			case T_ES:
				regNo = 0;
				break;
			case T_FS:
				regNo = 4;
				break;
			case T_GS:
				regNo = 5;
				break;
			case T_SS:
				regNo = 2;
				break;
			case T_ST:
				regNo = 0;
				break;
			}
		}
	}
	return regNo;
}

/* =====================================================================
  Build up instruction ModRM byte.
  ===================================================================== */
unsigned char BuildModRM(unsigned char modRM, struct Instr_Def* instr, struct expr opnd[4], bool* needModRM, bool* needSIB, bool isVEX)
{
	int sourceIdx = 1;

	/* VEX encoded instructions use the middle (NDS) registers in the VEX prefix bytes, so in this case
	   the 3rd operand reg/mem is the one that is actually encoded in the mod rm byte.
	   For Implicit NDS (2 opnd promotion we leave source as 1) */
	if (isVEX && (instr->vexflags & VEX_DUP_NDS) == 0 && (instr->vexflags & VEX_2OPND) == 0 && (instr->vexflags & VEX_3RD_OP) == 0)
		sourceIdx = 2;

	if (instr->flags & F_MODRM)			/* Only if the instruction requires a ModRM byte, else return 0. */
	{
		*needModRM |= TRUE;
		/*  7       5           2       0
		/  +---+---+---+---+---+---+---+---+
		/  |  mod  |    reg    |    rm     |
		/  +---+---+---+---+---+---+---+---+
		/  MODRM.mod (2bits) == b11, register to register direct, otherwise register indirect.
		/  MODRM.reg (3bits) == 3bit opcode extension, 3bit register value as source. REX.R, VEX.~R can 1bit extend this field.
		/  MODRM.rm  (3bits) == 3bit direct or indirect register operand, optionally with displacement. REX.B, VEX.~B can 1bit extend this field. */

		if (instr->flags & OPCODE_EXT)
		{
			/* If the instruction uses an opcode extension value in the ModRM.REG, its value */
			/* will be specified as an extra byte in the opcode byte data. */
			modRM |= (instr->opcode[(int)instr->opcode_bytes]) << 3;
		}

		if (instr->flags & F_MODRM_REG && instr->op_dir == REG_DST)
		{
			/* Build REG field as destination. */
			modRM |= (GetRegisterNo(opnd[0].base_reg) & 0x07) << 3;
		}
		else if (instr->flags & F_MODRM_REG && instr->op_dir == RM_DST)
		{
			/* Build REG field as source. */
			modRM |= (GetRegisterNo(opnd[sourceIdx].base_reg) & 0x07) << 3;
		}
		if (instr->flags & F_MODRM_RM && instr->op_dir == REG_DST)
		{
			/* Build RM field as source. */
			modRM |= (GetRegisterNo(opnd[sourceIdx].base_reg) & 0x07);
		}
		else if (instr->flags & F_MODRM_RM && instr->op_dir == RM_DST)
		{
			/* Build RM field as destination. */
			modRM |= (GetRegisterNo(opnd[0].base_reg) & 0x07);
		}
	}
	return modRM;
}

/* =====================================================================
  Build up instruction REX prefix byte.
  ===================================================================== */
unsigned char BuildREX(unsigned char RexByte, struct Instr_Def* instr, struct expr opnd[4])
{
	/* Only if the identified instruction requires a REX prefix. */
	/* REX flag is set which indicates the instruction table entry has preset values defining REX.R,.X,.B */
	if (((uint_32)instr->flags & (uint_32)REX) != 0)
	{
		/* +---+---+---+---+---+---+---+---+ */
		/* | 0 | 1 | 0 | 0 | W | R | X | B | */
		/* +---+---+---+---+---+---+---+---+ */
		/* W == 1=64bit operand size, else default operand size used (usually 32bit).
		/  R == extend ModRM.reg
		/  X == extend SIB.index
		/  B == extend ModRM.rm or SIB.base */
		RexByte |= 0x40;					/* Fixed base value for REX prefix.	*/
		if ((instr->flags & (uint_32)REXB) != 0)
			RexByte |= 0x01;
		if ((instr->flags & (uint_32)REXX) != 0)
			RexByte |= 0x02;
		if ((instr->flags & (uint_32)REXR) != 0)
			RexByte |= 0x04;
		if ((instr->flags & (uint_32)REXW) != 0)
			RexByte |= 0x08;

	}
	/* EREX (or extended with REX) means we must programmatically determine the REX extensions. */
	if (((uint_32)instr->flags & (uint_32)EREX) != 0)
	{
		if (instr->op_dir == REG_DST)
		{
			if (opnd[0].base_reg && GetRegisterNo(opnd[0].base_reg) > 7)
				RexByte |= 0x44; /* Add REX.R */
			if (opnd[instr->srcidx].base_reg && GetRegisterNo(opnd[instr->srcidx].base_reg) > 7)
				RexByte |= 0x41; /* Add REX.B */
		}
		else
		{
			if (opnd[0].base_reg && GetRegisterNo(opnd[0].base_reg) > 7)
				RexByte |= 0x41; /* Add REX.B */
			if (opnd[instr->srcidx].base_reg && GetRegisterNo(opnd[instr->srcidx].base_reg) > 7)
				RexByte |= 0x44; /* Add REX.R */
		}
	}
	
	/* Instruction promoted with REX.W if specified memory operand is QWORD sized */
	if ((uint_32)(instr->flags & (uint_32)REXP_MEM) != 0)
	{
		if (ModuleInfo.Ofssize != USE64)
		{
			EmitError(SIGN64_PROMOTION_NOT_POSSIBLE);
		}
		if (SizeFromMemtype(opnd[instr->memOpnd].mem_type, ModuleInfo.Ofssize, opnd[instr->memOpnd].type) == 8)
			RexByte |= 0x48;
	}
	return RexByte;
}

/* =====================================================================
  Build up instruction VEX prefix bytes.
  ===================================================================== */
void BuildVEX(bool* needVex, unsigned char* vexSize, unsigned char* vexBytes, struct Instr_Def* instr, struct expr opnd[4], bool needB, bool needX, uint_32 opCount)
{
	/* VEX 3 byte form */
	/*   7                           0     7                           0    */
	/* +---+---+---+---+---+---+---+---+   +---+---+---+---+---+---+---+---+*/
	/* |~R |~X |~B | map_select        |   |W/E|    ~vvvv      | L |   pp  |*/
	/* +---+---+---+---+---+---+---+---+   +---+---+---+---+---+---+---+---+*/

	/* A VEX instruction whose values for certain fields are VEX.~X == 1, VEX.~B == 1, VEX.W/E == 0 and map_select == b00001 may be encoded using the two byte VEX. */

	/* VEX 2 byte form */
	/*  7                           0    */
	/* +---+---+---+---+---+---+---+---+ */
	/* |~R |     ~vvvv     | L |   pp  | */
	/* +---+---+---+---+---+---+---+---+ */
	/*
	~R			1 bit	This 1 - bit value is an 'inverted' extension to the MODRM.reg field.The inverse of REX.R.See Registers.
	~X			1 bit	This 1 - bit value is an 'inverted' extension to the SIB.index field.The inverse of REX.X.See 64 - bit addressing.
	~B			1 bit	This 1 - bit value is an 'inverted' extension to the MODRM.rm field or the SIB.base field.The inverse of REX.B.See 64 - bit addressing.
	map_select	5 bits	Specifies the opcode map to use.
	W / E		1 bit	For integer instructions : when 1, a 64 - bit operand size is used; otherwise, when 0, the default operand size is used(equivalent with REX.W).For non - integer instructions, this bit is a general opcode extension bit.
	~vvvv		4 bits	An additional operand for the instruction.The value of the XMM or YMM register (see Registers) is 'inverted'.
	L			1 bit	When 0, a 128 - bit vector lengh is used.Otherwise, when 1, a 256 - bit vector length is used.
	pp			2 bits	Specifies an implied mandatory prefix for the opcode.
			  Value	Implied mandatory prefix
			  b00		none
			  b01		0x66
			  b10		0xF3
			  b11		0xF2
	*/
	unsigned char VEXl = 0;
	unsigned char VEXpp = 0;
	unsigned char VEXwe = 0;
	unsigned char VEXvvvv = 0;
	unsigned char VEXr = 1;
	unsigned char VEXb = 1;
	unsigned char VEXx = 1;
	unsigned char VEXmmmmm = 0;

	*needVex = TRUE;
	*vexSize = 0;

	/* VEX.vvvv */
	if ((instr->vexflags & VEX_3RD_OP) != 0)
		VEXvvvv = GetRegisterNo(opnd[2].base_reg);
	else if ((instr->vexflags & VEX_NDS) != 0)
		VEXvvvv = GetRegisterNo(opnd[1].base_reg);
	else if ((instr->vexflags & VEX_DDS) != 0)
		VEXvvvv = GetRegisterNo(opnd[2].base_reg);

	/* Generated implicit NDS form and warn user about assumed source1 */
	if ((instr->vexflags & VEX_DUP_NDS) != 0)
	{
		EmitWarn(1, AVX_REQUIRES_THREE_REGISTERS);
		VEXvvvv = GetRegisterNo(opnd[0].base_reg);
	}

	/* VEX.pp value (used in both 2 and 3 byte forms) */
	if ((instr->vexflags & VEX_66) != 0)
		VEXpp = 0x01;
	else if ((instr->vexflags & VEX_F3) != 0)
		VEXpp = 0x02;
	else if ((instr->vexflags & VEX_F2) != 0)
		VEXpp = 0x03;

	/* VEX.L value (used in both 2 and 3 byte forms) */
	if (instr->op_size == 16)
		VEXl = 0;
	else if (instr->op_size == 32)
		VEXl = 1;

	/* VEX.w/e value (only present in 3 byte form) */
	if ((instr->vexflags & VEX_W0) != 0)
		VEXwe = 0;
	else if ((instr->vexflags & VEX_W1) != 0)
	{
		VEXwe = 1;
		*vexSize = 3;
	}

	/* Either of these flags mandate 3-byte VEX form */
	if ((instr->vexflags & VEX_0F38) != 0 || (instr->vexflags & VEX_0F3A) != 0)
		* vexSize = 3;

	/* VEX.mmmmm field - determinant for 2/3 byte size */
	if ((instr->vexflags & VEX_0F) != 0)
		VEXmmmmm = 1;
	else if ((instr->vexflags & VEX_0F38) != 0)
		VEXmmmmm = 2;
	else if ((instr->vexflags & VEX_0F3A) != 0)
		VEXmmmmm = 3;

	/* VEX.~r extension value */
	if ((instr->vexflags & VEX_R) != 0)
		VEXr = 0;
	/* VEX.~x extension value */
	if ((instr->vexflags & VEX_X) != 0 || needX)
		VEXx = 0;
	/* VEX.~b extension value */
	if ((instr->vexflags & VEX_B) != 0 || needB)
		VEXb = 0;

	if (instr->op_dir == REG_DST)
	{
		if (opnd[0].base_reg && GetRegisterNo(opnd[0].base_reg) > 7)
			VEXr = 0; /* REG_DST requires R~ extension */
		if (opnd[instr->srcidx].base_reg && !opnd[instr->srcidx].indirect && GetRegisterNo(opnd[instr->srcidx].base_reg) > 7)
			VEXb = 0; /* RM_SRC requires B~ extension */
	}
	else if (instr->op_dir == RM_DST)
	{
		if (opnd[instr->srcidx].base_reg && GetRegisterNo(opnd[instr->srcidx].base_reg) > 7)
			VEXr = 0; /* REG_DST requires R~ extension */
		if (opnd[0].base_reg && !opnd[0].indirect && GetRegisterNo(opnd[0].base_reg) > 7)
			VEXb = 0; /* RM_SRC requires B~ extension */
	}

	/* If VEX.WIG is present and we don't need VEX.mmmmm use 2byte form */
	if (((instr->vexflags & VEX_WIG) != 0 && VEXx == 1 && VEXb == 1 && VEXmmmmm == 1) ||
		(*vexSize != 3 && VEXx == 1 && VEXb == 1))
		* vexSize = 2;

	/* Either VEX.x or VEX.b set require 3byte form */
	if (VEXx == 0 || VEXb == 0)
		* vexSize = 3;

	/* Invert VEX.vvvv value */
	VEXvvvv = ~VEXvvvv;

	/* Output 2byte VEX form */
	if (*vexSize == 2)
	{
		vexBytes[0] = 0xc5;
		vexBytes[1] = (VEXpp) | (VEXl << 2) | ((VEXvvvv & 0xf) << 3) | (VEXr << 7);
	}
	/* Output 3byte VEX form */
	else if (*vexSize == 3)
	{
		vexBytes[0] = 0xc4;
		vexBytes[1] = (VEXr << 7) | (VEXx << 6) | (VEXb << 5) | (VEXmmmmm);
		vexBytes[2] = (VEXwe << 7) | ((VEXvvvv & 0xf) << 3) | (VEXl << 2) | (VEXpp);
	}
}

/* =====================================================================
  Build up instruction EVEX prefix bytes.
  ===================================================================== */
void BuildEVEX(bool* needEvex, unsigned char* evexBytes, struct Instr_Def* instr, struct expr opnd[4], bool needB, bool needX, bool needRR, uint_32 opCount, struct code_info* CodeInfo)
{
	/* BYTE0: EVEX prefix is always 4 bytes and the first byte is always 0x62.

	 BYTE1:
	 | 7 | 6 | 5 | 4  | 3 | 2 | 1-0 |
	 | R | X | B | R | 0 | 0 |  m  |

	BYTE2:
	 | 7 | 6-3      | 2 | 1-0 |
	 | W |     v	| 1	|  p  |

	BYTE3:
	 | 7 | 6  | 5 | 4 | 3  | 2-0 |
	 | z | L' | L | b | V' |  a  | */

	unsigned char EVEXpp = 0;
	unsigned char EVEXmm = 0;
	unsigned char EVEXr = 1;
	unsigned char EVEXx = 1;
	unsigned char EVEXb = 1;
	unsigned char EVEXnr = 0;
	unsigned char EVEXl = 0;
	unsigned char EVEXnl = 1;
	unsigned char EVEXw = 0;
	unsigned char EVEXvvvv = 0;
	unsigned char EVEXaaa = 0;
	unsigned char EVEXz = 0;
	unsigned char EVEXbr = 0;
	unsigned char EVEXnv = 1;

	/* {z} decorator */
	EVEXz = (decoflags & 0x80) >> 7;

	/* {kn} opmask */
	EVEXaaa = (decoflags & 7);
	if ((instr->evexflags & EVEX_K) != 0 && EVEXaaa == 0)
		EmitError(K_REGISTER_EXPECTED);

	/* {1toN} broadcast 
	   {1to2}  == 0x10
	   {1to4}  == 0x20
	   {1to8}  == 0x30
	   {1to16} == 0x40 */
	if (instr->op_elements == 16 && broadflags != 0x40 && broadflags != 0)
		EmitError(MISMATCH_IN_THE_NUMBER_OF_BROADCASTING_ELEMENTS);
	if (instr->op_elements == 8 && broadflags != 0x30 && broadflags != 0)
		EmitError(MISMATCH_IN_THE_NUMBER_OF_BROADCASTING_ELEMENTS);
	if (instr->op_elements == 4 && broadflags != 0x20 && broadflags != 0)
		EmitError(MISMATCH_IN_THE_NUMBER_OF_BROADCASTING_ELEMENTS);
	if (instr->op_elements == 2 && broadflags != 0x10 && broadflags != 0)
		EmitError(MISMATCH_IN_THE_NUMBER_OF_BROADCASTING_ELEMENTS);
	if (instr->op_elements == 1 && broadflags != 0x00)
		EmitError(MISMATCH_IN_THE_NUMBER_OF_BROADCASTING_ELEMENTS);
	if (broadflags != 0 && (instr->evexflags & EVEX_BRD) == 0)
		EmitError(BROADCAST_DECORATORS_NOT_ALLOWED_FOR_THIS_INSTRUCTION);

	if (broadflags != 0)
		EVEXbr = 1;

	if ((instr->evexflags & EVEX_MASK) == 0 && (EVEXaaa != 0))
		EmitError(EVEX_DECORATOR_NOT_ALLOWED);

	if ((instr->evexflags & EVEX_Z) == 0 && (EVEXz == 1))
		EmitError(Z_MASK_NOT_PERMITTED_WHEN_FIRST_OPERATOR_IS_MEMORY);

	/* EVEX.vvvv */
	if ((instr->vexflags & VEX_NDS) != 0)
	{
		EVEXvvvv = GetRegisterNo(opnd[1].base_reg);
		/* EVEX.V' */
		if (GetRegisterNo(opnd[1].base_reg) > 15)
			EVEXnv = 0;
	}
	else if ((instr->vexflags & VEX_DDS) != 0)
	{
		EVEXvvvv = GetRegisterNo(opnd[2].base_reg);
		/* EVEX.V' */
		if (GetRegisterNo(opnd[2].base_reg) > 15)
			EVEXnv = 0;
	}
	EVEXvvvv = ~EVEXvvvv;

	/* VSIB extension into V' */
	if ((instr->evexflags & EVEX_VSIB) != 0)
	{
		if ((opnd[instr->memOpnd].idx_reg && GetRegisterNo(opnd[instr->memOpnd].idx_reg) > 15) ||
			(opnd[instr->memOpnd].base_reg && GetRegisterNo(opnd[instr->memOpnd].base_reg) > 15))
		{
			EVEXnv = 0;
		}
		else
			EVEXnv = 1;
		if ((opnd[instr->memOpnd].idx_reg && GetRegisterNo(opnd[instr->memOpnd].idx_reg) > 23) ||
			(opnd[instr->memOpnd].base_reg && GetRegisterNo(opnd[instr->memOpnd].base_reg) > 23))
		{
			EVEXx = 0;
		}
		else
			EVEXx = 1;
	}

	/* EVEX.L and L' fields */
	if (instr->op_size == 16)
		EVEXl = 0;
	else if (instr->op_size == 32)
		EVEXl = 1;
	else if (instr->op_size == 64)
	{
		EVEXl = 0;
		EVEXnl = 0;
	}

	EVEXnl = ~EVEXnl;

	/* {static rounding} */
	if (CodeInfo->evex_sae != 0)
	{
		if ((instr->evexflags & EVEX_SAE) != 0) 
		{
			if (CodeInfo->evex_sae > 0x10)
				EmitError(EMBEDDED_ROUNDING_IS_AVAILABLE_ONLY_WITH_REG_REG_OP);
		}
		else if ((instr->evexflags & EVEX_RND) != 0) 
		{
			if (CodeInfo->evex_sae == 0x10)
				EmitError(EMBEDDED_ROUNDING_IS_AVAILABLE_ONLY_WITH_REG_REG_OP);
			else if ((CodeInfo->opnd[OPND1].type & OP_M_ANY) || (CodeInfo->opnd[OPND2].type & OP_M_ANY) ||
				(CodeInfo->opnd[OPND3].type & OP_M_ANY))
				EmitError(EMBEDDED_ROUNDING_IS_AVAILABLE_ONLY_WITH_REG_REG_OP);
		}

		switch (CodeInfo->evex_sae)
		{
		case 0x10:       /* sae */
		case 0x20:       /* rn-sae */
		{
			EVEXnl = 0;
			EVEXl = 0;
			EVEXbr = 1;
			break;
		}
		case  0x40:     /* rd-sae */
		{
			EVEXnl = 0;
			EVEXl = 1;
			EVEXbr = 1;
			break;
		}
		case 0x60:      /* ru-sae */
		{
			EVEXnl = 1;
			EVEXl = 0;
			EVEXbr = 1;
			break;
		}
		case 0x80:     /* rz-sae */
		{
			EVEXnl = 1;
			EVEXl = 1;
			EVEXbr = 1;
		}
		}
	}

	/* EVEX.w field */
	if ((instr->evexflags & EVEX_W0) != 0)
		EVEXw = 0;
	else if ((instr->evexflags & EVEX_W1) != 0)
		EVEXw = 1;

	/* EVEX.pp field */
	if ((instr->vexflags & VEX_66) != 0)
		EVEXpp = 0x01;
	else if ((instr->vexflags & VEX_F3) != 0)
		EVEXpp = 0x02;
	else if ((instr->vexflags & VEX_F2) != 0)
		EVEXpp = 0x03;

	/* EVEX.mm field */
	if ((instr->vexflags & VEX_0F) != 0)
		EVEXmm = 1;
	else if ((instr->vexflags & VEX_0F38) != 0)
		EVEXmm = 2;
	else if ((instr->vexflags & VEX_0F3A) != 0)
		EVEXmm = 3;



	if (instr->op_dir == REG_DST)
	{
		if (!opnd[0].indirect)
		{
			if (GetRegisterNo(opnd[0].base_reg) > 15)
			{
				EVEXnr = 0;
				if (GetRegisterNo(opnd[0].base_reg) > 23)
					EVEXr = 0;
				else
					EVEXr = 1;
			}
			else
			{
				EVEXnr = 1;
				if (GetRegisterNo(opnd[0].base_reg) > 7)
					EVEXr = 0;
				else
					EVEXr = 1;
			}
		}
		else
		{
			if (needRR)
				EVEXnr = 1;
		}

		if (!opnd[instr->srcidx].indirect)
		{
			if (opnd[instr->srcidx].base_reg && GetRegisterNo(opnd[instr->srcidx].base_reg) > 15)
			{
				EVEXx = 0;
				if (opnd[instr->srcidx].base_reg && GetRegisterNo(opnd[instr->srcidx].base_reg) > 23)
					EVEXb = 0;
				else
					EVEXb = 1;
			}
			else
			{
				EVEXx = 1;
				if (opnd[instr->srcidx].base_reg && GetRegisterNo(opnd[instr->srcidx].base_reg) > 7)
					EVEXb = 0;
				else
					EVEXb = 1;
			}
		}
	}
	else if (instr->op_dir == RM_DST)
	{
		if (!opnd[instr->srcidx].indirect)
		{
			if (GetRegisterNo(opnd[instr->srcidx].base_reg) > 15)
			{
				EVEXnr = 0;
				if (GetRegisterNo(opnd[instr->srcidx].base_reg) > 23)
					EVEXr = 0;
				else
					EVEXr = 1;
			}
			else
			{
				EVEXnr = 1;
				if (GetRegisterNo(opnd[instr->srcidx].base_reg) > 7)
					EVEXr = 0;
				else
					EVEXr = 1;
			}
		}

		if (!opnd[0].indirect)
		{
			if (GetRegisterNo(opnd[0].base_reg) > 15)
			{
				EVEXx = 0;
				if (GetRegisterNo(opnd[0].base_reg) > 23)
					EVEXb = 0;
				else
					EVEXb = 1;
			}
			else
			{
				EVEXx = 1;
				if (GetRegisterNo(opnd[0].base_reg) > 7)
					EVEXb = 0;
				else
					EVEXb = 1;
			}
		}
	}

	/* If the instruction uses the REG field as an opcode extension, ensure EVEX.R~ is set */
	if ((instr->flags & OPCODE_EXT) != 0)
		EVEXnr = 1;

	/* DIRECT or Memory Inferred settings */
	/* EVEX.R and R~ extension value */
	if ((instr->vexflags & VEX_R) != 0)
		EVEXr = 0;

	/* EVEX.B and X extension value */
	if ((instr->vexflags & VEX_B) != 0 || needB)
		EVEXb = 0;

	/* EVEX.X extension value */
	if ((instr->vexflags & VEX_X) != 0 || needX)
		EVEXx = 0;

	evexBytes[0] = 0x62;
	evexBytes[1] = ((EVEXr & 0x1) << 7) | ((EVEXx & 0x1) << 6) | ((EVEXb & 0x1) << 5) | ((EVEXnr & 0x1) << 4) | (EVEXmm & 0x3);
	evexBytes[2] = ((EVEXw & 0x1) << 7) | ((EVEXvvvv & 0xf) << 3) | (0x04) | (EVEXpp & 0x3);
	evexBytes[3] = ((EVEXz & 0x1) << 7) | ((EVEXnl & 0x1) << 6) | ((EVEXl & 0x1) << 5) | ((EVEXbr & 0x1) << 4) | ((EVEXnv & 0x1) << 3) | (EVEXaaa & 0x7);

	*needEvex = TRUE;
}

/* =====================================================================
  Check for an EVEX Comp8 Displacement and amend value if required.
  ===================================================================== */
bool CompDisp(struct expr* memOpnd, struct Instr_Def* instr, struct code_info* CodeInfo)
{
	int_32 elements = (broadflags == 0 && (instr->evexflags & EVEX_BRD) != 0) ? 1 : instr->op_elements;
	int_32 elemSize = (instr->op_size / elements);
	if (CodeInfo->evex_flag)
	{
		/* Memory displacement is between -8n and +8n. */
		if (memOpnd->value >= -(128 * elemSize) && memOpnd->value <= (127 * elemSize))
		{
			/* Is a multiple of the operation size / elements. */
			if (memOpnd->value % elemSize == 0)
			{
				memOpnd->value = (memOpnd->value / elemSize);
				return TRUE;
			}
		}
		return FALSE;
	}
	return TRUE;
}

/* =====================================================================
  Return true if register a simd register (xmm,ymm,zmm).
  ===================================================================== */
bool IsSimdRegister(struct asm_tok* regTok)
{
	bool result = FALSE;
	if (regTok)
	{
		if (regTok->tokval >= T_XMM0 && regTok->tokval <= T_XMM7)
			result = TRUE;
		else if (regTok->tokval >= T_XMM8 && regTok->tokval <= T_XMM15)
			result = TRUE;
		else if (regTok->tokval >= T_XMM16 && regTok->tokval <= T_XMM23)
			result = TRUE;
		else if (regTok->tokval >= T_XMM24 && regTok->tokval <= T_XMM31)
			result = TRUE;
		else if (regTok->tokval >= T_YMM0 && regTok->tokval <= T_YMM7)
			result = TRUE;
		else if (regTok->tokval >= T_YMM8 && regTok->tokval <= T_YMM15)
			result = TRUE;
		else if (regTok->tokval >= T_YMM16 && regTok->tokval <= T_YMM23)
			result = TRUE;
		else if (regTok->tokval >= T_YMM24 && regTok->tokval <= T_YMM31)
			result = TRUE;
		else if (regTok->tokval >= T_ZMM0 && regTok->tokval <= T_ZMM7)
			result = TRUE;
		else if (regTok->tokval >= T_ZMM8 && regTok->tokval <= T_ZMM31)
			result = TRUE;
	}
	return result;
}

/* =====================================================================
  Build up instruction SIB, ModRM and REX bytes for memory operand.
  ===================================================================== */
int BuildMemoryEncoding(unsigned char* pmodRM, unsigned char* pSIB, unsigned char* pREX, bool* needModRM, bool* needSIB,
	unsigned int* dispSize, uint_64* pDisp, struct Instr_Def* instr, struct expr opExpr[4], bool* needB,
	bool* needX, bool* needRR, struct code_info* CodeInfo)
{
	int             returnASO = 0;
	unsigned char   sibScale = 0;
	uint_32         memModeIdx = 0;
	unsigned char   baseRegNo = 17;
	unsigned char   idxRegNo = 17;
	int             baseRegSize = 0;
	int             idxRegSize = 0;
	int             symSize = 0;
	bool			skipSIB = FALSE;

	/* Absolute addressing modes can skip this */
	if (instr->memOpnd != MEM_ABS_1 && instr->memOpnd != MEM_ABS_0)
	{
		baseRegNo = GetRegisterNo(opExpr[instr->memOpnd].base_reg);
		idxRegNo = GetRegisterNo(opExpr[instr->memOpnd].idx_reg);

		/* For VSIB indexes the Parser sends us the vector idx register in base NOT idx when there is ONLY an index! */
		/* We can detect this situation when the base register is a simd register (xmm, ymm or zmm) */
		if ((instr->vexflags & VEX_VSIB) != 0 && IsSimdRegister(opExpr[instr->memOpnd].base_reg))
		{
			unsigned char temp = baseRegNo;
			baseRegNo = idxRegNo;
			idxRegNo = temp;
		}

		/* For EVEX VSIB extension, we just keep the low 3 bits. */
		if ((instr->evexflags & EVEX_VSIB) != 0 && idxRegNo > 15)
		{
			idxRegNo &= 7;
			*needRR = TRUE;
		}

		/* Get base and index register sizes in bytes */
		if (opExpr[instr->memOpnd].base_reg)
			baseRegSize = SizeFromRegister(opExpr[instr->memOpnd].base_reg->tokval);
		if (opExpr[instr->memOpnd].idx_reg)
			idxRegSize = SizeFromRegister(opExpr[instr->memOpnd].idx_reg->tokval);
	}

	/* Base and Index registers must be of the same size (except for VSIB) */
	if (baseRegSize != idxRegSize && idxRegSize < 16 && idxRegSize > 0 && baseRegSize > 0)
	{
		EmitError(BASE_INDEX_MEMORY_SIZE_ERROR);
		return returnASO;
	}

	/* 16bit Memory Addressing is not allowed in long mode */
	if (ModuleInfo.Ofssize == USE64 && baseRegSize == 2)
	{
		EmitError(BITS16_MEM_NOT_ALLOWED_IN_LONG_MODE);
		return returnASO;
	}

	/* Don't allow 64bit only registers in 32/16 bit modes */
	if ((idxRegNo > 7 || baseRegNo > 7) && ModuleInfo.Ofssize != USE64 && idxRegNo < 16 && baseRegNo < 16)
	{
		EmitError(INVALID_ADDRESSING_MODE_WITH_CURRENT_CPU_SETTING);
		return returnASO;
	}

	/* If the memory address refers to a symbol indirectly.. */
	if (opExpr[instr->memOpnd].sym && opExpr[instr->memOpnd].kind == EXPR_ADDR)
	{
		symSize = SizeFromMemtype(opExpr[instr->memOpnd].mem_type, ModuleInfo.Ofssize, opExpr[instr->memOpnd].sym);

		/* For 64bit mode, all symbol references are RIP relative, unless the symbol is on the stack.
		   EXCEPT, if the memory address includes a register. */
		if (ModuleInfo.Ofssize == USE64 && opExpr[instr->memOpnd].sym->state != SYM_STACK && baseRegNo == 0x11)
		{
			baseRegNo = 16;
		}

	}
	else
	{
		/* Address format requires Address Size Override Prefix */
		if ((ModuleInfo.Ofssize == USE64 && baseRegSize == 4) ||
			(ModuleInfo.Ofssize == USE32 && baseRegSize == 2) ||
			(ModuleInfo.Ofssize == USE16 && baseRegSize == 4))
			returnASO = 1;
	}

	/* Calculate 16bit memory addressing ModRM in 32bit mode */
	if (ModuleInfo.Ofssize == USE32 && baseRegSize == 2)
	{
		if (instr->memOpnd < NO_MEM)
		{
			/* [BX + SI]	[BX + DI]	[BP + SI]	[BP + DI]	[SI]	[DI]	[disp16]	[BX] */
			if (opExpr[instr->memOpnd].base_reg && opExpr[instr->memOpnd].idx_reg &&
				opExpr[instr->memOpnd].base_reg->tokval == T_BX && opExpr[instr->memOpnd].idx_reg->tokval == T_SI)
				* pmodRM |= 0;
			else if (opExpr[instr->memOpnd].base_reg && opExpr[instr->memOpnd].idx_reg &&
				opExpr[instr->memOpnd].base_reg->tokval == T_BX && opExpr[instr->memOpnd].idx_reg->tokval == T_DI)
				* pmodRM |= 1;
			else if (opExpr[instr->memOpnd].base_reg && opExpr[instr->memOpnd].idx_reg &&
				opExpr[instr->memOpnd].base_reg->tokval == T_BP && opExpr[instr->memOpnd].idx_reg->tokval == T_SI)
				* pmodRM |= 2;
			else if (opExpr[instr->memOpnd].base_reg && opExpr[instr->memOpnd].idx_reg &&
				opExpr[instr->memOpnd].base_reg->tokval == T_BP && opExpr[instr->memOpnd].idx_reg->tokval == T_DI)
				* pmodRM |= 3;
			else if (opExpr[instr->memOpnd].base_reg && opExpr[instr->memOpnd].base_reg->tokval == T_SI)
				* pmodRM |= 4;
			else if (opExpr[instr->memOpnd].base_reg && opExpr[instr->memOpnd].base_reg->tokval == T_DI)
				* pmodRM |= 5;
			else if (!opExpr[instr->memOpnd].base_reg && !opExpr[instr->memOpnd].idx_reg)
				* pmodRM |= 6;
			else if (opExpr[instr->memOpnd].base_reg && opExpr[instr->memOpnd].base_reg->tokval == T_BX)
				* pmodRM |= 7;

			*needModRM |= TRUE;

			skipSIB = TRUE;
		}
	}
	else
	{
		/* Use the memory encoding table to populate the initial modRM, sib values */
		/* ----------------------------------------------------------------------- */

		/* It's possible the idxRegNo is (0-31) for EVEX, but no such mem-encoding entries exist.
		   These must be wrapped around to the low 17. */
		memModeIdx = (baseRegNo * 18) + (idxRegNo % 18);

		if (instr->memOpnd < NO_MEM) /* MEM_ABS_x for moffset type addresses don't need to be processed. */
		{
			/* Addressing form cannot be encoded. */
			if (MemTable[memModeIdx].flags & NO_ENCODE)
			{
				EmitError(INVALID_ADDRESSING_MODE_WITH_CURRENT_CPU_SETTING);
				return returnASO;
			}

			/* Setup ModRM. */
			if (MemTable[memModeIdx].flags & MEMF_MODRM)
			{
				*pmodRM |= MemTable[memModeIdx].modRM;
				*needModRM |= TRUE;
			}
			/* Setup SIB. */
			if (MemTable[memModeIdx].flags & MEMF_SIB)
			{
				*pSIB |= MemTable[memModeIdx].SIB;
				*needSIB |= TRUE;
			}
		}
	}

	/* Does the memory address require a displacement? */
	/* ----------------------------------------------------------------------- */
	/* Either: User specified
	           RIP relative addressing mandates it
	           Specified address format can only be encoded with a displacement. */
	if (instr->memOpnd > NO_MEM || opExpr[instr->memOpnd].value != 0 ||
		(ModuleInfo.Ofssize == USE64 && opExpr[instr->memOpnd].base_reg && opExpr[instr->memOpnd].base_reg->token == T_RIP) ||
		MemTable[memModeIdx].flags & MEMF_DSP || MemTable[memModeIdx].flags & MEMF_DSP32 || (opExpr[instr->memOpnd].sym && opExpr[instr->memOpnd].sym->state != SYM_STACK))
	{
		if (instr->memOpnd > NO_MEM)
		{
			switch (ModuleInfo.Ofssize)
			{
			case USE16:
				*dispSize = 2;
				break;
			case USE32:
				*dispSize = 4;
				break;
			case USE64:
				*dispSize = 8;
				break;
			}
		}
		else
		{
			/* Is it 8bit or 16/32bit (RIP only allows 32bit). */
			if (CompDisp(&opExpr[(instr->memOpnd & 7)], instr, CodeInfo) &&
				(((!opExpr[instr->memOpnd].sym) || (opExpr[instr->memOpnd].sym && opExpr[instr->memOpnd].sym->state == SYM_STACK)) &&
				(((MemTable[memModeIdx].flags & MEMF_DSP32) == 0) && (opExpr[instr->memOpnd].value >= -128 && opExpr[instr->memOpnd].value <= 127))))
			{
				*dispSize = 1;
				*pmodRM |= MODRM_DISP8;
			}
			else
			{
				if (ModuleInfo.Ofssize == USE16)
					*dispSize = 2;	/* 16bit addressing. */
				else
					*dispSize = 4;	/* 32bit or 64bit sign extended addressing. */

				if ((int)(MemTable[memModeIdx].flags & MEMF_DSP32) == 0)
					*pmodRM |= MODRM_DISP;
			}
		}
		*pDisp = opExpr[(instr->memOpnd & 7)].value64;
	}

	/* Extend V/REX(.B) and V/REX(.X) to account for 64bit base and index registers.
	   We use RegNo==16 to represent RIP (even though it's not directly encodable). */
	/* ----------------------------------------------------------------------- */
	if (baseRegNo > 7 && baseRegNo < 16)
	{
		if (instr->vexflags == NO_VEX)
			* pREX |= 0x41;
		*needB = TRUE;
	}
	if (idxRegNo > 7 && idxRegNo < 16)
	{
		if (instr->vexflags == NO_VEX)
			* pREX |= 0x42;
		*needX = TRUE;
	}

	/* scale index    base
	   +--+--+--+--+--+--+--+--+
	   | 7   | 5      | 2      |
	   +--+--+--+--+--+--+--+--+
	   SIB.scale == 0,1,2,3 = (1,2,4,8) scale factor.
	   SIB.index == index register to use, extended via REX.X or VEX.~X
	   SIB.base  == base register to use, extended via REX.B or VEX.~B */

	/* (E/R)SP or R12 or presence of scale or index register requires SIB addressing modes */
	/* ----------------------------------------------------------------------- */
	if (!skipSIB && instr->memOpnd < NO_MEM && (opExpr[instr->memOpnd].scale > 1 || opExpr[instr->memOpnd].idx_reg != 0 || (MemTable[memModeIdx].flags & MEMF_SIB)))
	{
		switch (opExpr[instr->memOpnd].scale)
		{
		case 1:
			sibScale = 0;
			break;
		case 2:
			sibScale = 1;
			break;
		case 4:
			sibScale = 2;
			break;
		case 8:
			sibScale = 3;
			break;
		}
		*pSIB |= (sibScale << 6);
		*needSIB |= TRUE;
	}

	return returnASO;
}

/* =====================================================================
  Promote broadcast decorators
  -> Some instructions that do conversions, when used with BCST will
  -> produce incorrect estimated broadcast size due to the change
  -> in destination element count, fix it.
  ===================================================================== */
void PromoteBroadcast(struct Instr_Def* instr, struct code_info* CodeInfo)
{
	
	/* {1to2}  == 0x10
	   {1to4}  == 0x20
	   {1to8}  == 0x30
	   {1to16} == 0x40 */
	if (strcasecmp(instr->mnemonic, "VCVTDQ2PD") == 0)
	{
		if (broadflags == 0x40 && instr->operand_types[0] == R_ZMM)
			broadflags = 0x30;
		else if (broadflags == 0x30 && instr->operand_types[0] == R_YMM)
			broadflags = 0x20;
		else if (broadflags == 0x20 && instr->operand_types[0] == R_XMM)
			broadflags = 0x10;
	}
}

ret_code CodeGenV2(const char* instr, struct code_info* CodeInfo, uint_32 oldofs, uint_32 opCount, struct expr opExpr[4])
{
	struct Instr_Def  instrToMatch;
	ret_code          retcode = NOT_ERROR;
	struct Instr_Def* matchedInstr = NULL;
	uint_32           i = 0;

	bool needModRM = FALSE;
	bool needSIB   = FALSE;
	bool needFixup = FALSE;
	bool hasMemReg = FALSE;
	bool needVEX   = FALSE;
	bool needEVEX  = FALSE;
	bool needB     = FALSE;	/* Instruction needs REX.B or (E)VEX.~B promotion (filled in by BuildMemoryEncoding) */
	bool needX     = FALSE;	/* Instruction needs REX.X or (E)VEX.~X promotion (filled in by BuildMemoryEncoding) */
	bool needRR    = FALSE;	/* Instruction needs EVEX.R~ promotion to use upper 16 registers */

	int  aso = 0;				/* Build Memory Encoding forced address size override */

	unsigned char opcodeByte = 0;
	unsigned char rexByte = 0;
	unsigned char vexSize = 0;
	unsigned char vexBytes[3] = { 0, 0, 0 };
	unsigned char evexBytes[4] = { 0, 0, 0, 0 };
	unsigned char modRM = 0;
	unsigned char sib = 0;
	unsigned int  dispSize = 0;
	unsigned int  immSize = 0;
	unsigned char encodeMode = 0;
	unsigned int  srcRegNo = 0;
	unsigned int  dstRegNo = 0;

	union
	{
		uint_64 displacement64;
		unsigned char byte[8];
	} displacement;

	union
	{
		uint_64 full;
		unsigned char byte[8];
	} immValue;

	/* Fix for byte sized immediate converted to OP_I16 */
	if ((CodeInfo->opnd[OPND2].type == OP_I16 || CodeInfo->opnd[OPND2].type == OP_I8) && opExpr[1].mem_type != MT_WORD) 
	{
		if ((CodeInfo->opnd[OPND2].data32l <= UCHAR_MAX) && (CodeInfo->opnd[OPND2].data32l >= -255)) 
		{
			if (CodeInfo->opnd[OPND1].type == OP_M || CodeInfo->opnd[OPND1].type == OP_M08) 
			{
				CodeInfo->opnd[OPND1].type = OP_M08;
				CodeInfo->opnd[OPND2].type = OP_I8;
			}
			else if (CodeInfo->opnd[OPND1].type == OP_R8 || CodeInfo->opnd[OPND1].type == OP_AL || CodeInfo->opnd[OPND1].type == OP_CL)
				CodeInfo->opnd[OPND2].type = OP_I8;
			else 
			{
				switch (CodeInfo->token) 
				{
					case T_BT:
					case T_BTC:
					case T_BTR:
					case T_BTS:
						CodeInfo->opnd[OPND2].type = OP_I8;
				}
			}
		}
	}

	/* Determine which Memory Encoding Format Table to Use. */
	if (CodeInfo->Ofssize == USE64)
		MemTable = MemTable64;
	else
		MemTable = MemTable32;

	/* Force JWASM style FLAT: override back to legacy CodeGen. */
	if ((opExpr[1].override && opExpr[1].override->tokval == T_FLAT) ||
		(opExpr[0].override && opExpr[0].override->tokval == T_FLAT))
		return EMPTY;

	/* return EMPTY; Uncomment this to disable new CodeGenV2. */

	memset(&instrToMatch, 0, sizeof(struct Instr_Def));
	instrToMatch.mnemonic = instr;		    /* Instruction mnemonic string */
	instrToMatch.operand_count = opCount;	/* Number of operands */

	/* Translate to CodeGenV2 operand types */
	for (i = 0; i < opCount; i++)
	{
		instrToMatch.operand_types[i] = MatchOperand(CodeInfo, CodeInfo->opnd[i], opExpr[i]);
		/* Determine if we have a memory operand, if it contains registers */
		if (opExpr[i].kind == EXPR_ADDR && (opExpr[i].base_reg || opExpr[i].idx_reg))
			hasMemReg = TRUE;
		/* Is indirect with reg (this would indicate RIP) */
		if (opExpr[i].kind == EXPR_REG && opExpr[i].indirect)
			hasMemReg = TRUE;
		/* Refers to a symbol in 64bit mode (in which case a RIP relative mode is better) */
		if (CodeInfo->Ofssize == USE64 && opExpr[i].sym)
			hasMemReg = TRUE;
	}

	/* Determine encoding model for x16/x32 or x64 */
	switch (CodeInfo->Ofssize)
	{
	case USE16:
		encodeMode = X16;
		break;
	case USE32:
		encodeMode = X32;
		break;
	case USE64:
		encodeMode = X64;
		break;
	}

	/* Get the src/dst register numbers to limit instruction lookups where required */
	if (opExpr[0].base_reg)
		dstRegNo = GetRegisterNo(opExpr[0].base_reg);
	if (opExpr[1].base_reg)
		srcRegNo = GetRegisterNo(opExpr[1].base_reg);

	/* Lookup the instruction */
	matchedInstr = LookupInstruction(&instrToMatch, hasMemReg, encodeMode, srcRegNo, dstRegNo, CodeInfo);

	/* Try once again with demoted operands */
	if (matchedInstr == NULL)
	{
		for (i = 0; i < opCount; i++)
			instrToMatch.operand_types[i] = DemoteOperand(instrToMatch.operand_types[i]);
		matchedInstr = LookupInstruction(&instrToMatch, hasMemReg, encodeMode, srcRegNo, dstRegNo, CodeInfo);
	}

	/* If we have an absolute memory addressing mode but the disp is 32bit or less, fallback to using a non abs mode */
	if (matchedInstr && matchedInstr->memOpnd != NO_MEM && CodeInfo->Ofssize == USE64 && CodeInfo->token != T_MOVABS && (int)matchedInstr->group < SSE0 && !hasMemReg)
	{
		if ((int)CodeInfo->opnd[(matchedInstr->memOpnd & 7)].data64 > INT_MIN && CodeInfo->opnd[(matchedInstr->memOpnd & 7)].data64 < UINT_MAX)
		{
			matchedInstr = NULL;
		}
	}

	/* We don't have it in CodeGenV2 so fall-back */
	if (matchedInstr == NULL)
		retcode = EMPTY;

	/* Proceed to generate the instruction */
	else
	{ /* FIXME */ extern int get_curr_srcfile(), GetLineNumber()
		;

		/*----------------------------------------------------------
		 Promote broadcast decorators
		 -> Some instructions that do conversions, when used
		 -> with BCST will produce the wrong number of elements
		 -> Fixup now
		----------------------------------------------------------*/
		PromoteBroadcast(matchedInstr, CodeInfo);

		/*----------------------------------------------------------
		 Add line number debugging info.
		----------------------------------------------------------*/
		if (Options.line_numbers)
			AddLinnumDataRef(get_curr_srcfile(), GetLineNumber());

		/*----------------------------------------------------------
		 Check if instruction is valid in current mode.
		----------------------------------------------------------*/
		if (!IsValidInCPUMode(matchedInstr))
		{
			EmitError(INSTRUCTION_OR_REGISTER_NOT_ACCEPTED_IN_CURRENT_CPU_MODE);
			return ERROR;
		}

		/*----------------------------------------------------------
		 Decide if a fixup is required.
		 -> A fixup is only required if a memory opnd is used
		 -> And only then if it refers to a symbol
		 -> On stack symbols don't need
		----------------------------------------------------------*/
		if (matchedInstr->memOpnd != NO_MEM)
		{
			if (opExpr[(matchedInstr->memOpnd & 7)].sym && opExpr[(matchedInstr->memOpnd & 7)].sym->state != SYM_STACK)
				needFixup = TRUE;
		}
		if (matchedInstr->immOpnd != NO_IMM)
		{
			if (CodeInfo->opnd[matchedInstr->immOpnd].InsFixup)
				needFixup = TRUE;
		}

		/*----------------------------------------------------------
		 Build Memory Encoding Format.
		 -> When an indirect memory operand is used, this will build
		 -> up the respective rex, modrm and sib values.
		 Alternatively directly encode the modRM, rex and SIB values.
		----------------------------------------------------------*/
		/* If the matched instruction requires processing of a memory address */
		if (matchedInstr->memOpnd != NO_MEM)
			aso = BuildMemoryEncoding(&modRM, &sib, &rexByte, &needModRM, &needSIB,								/* This could result in modifications to REX/VEX/EVEX, modRM and SIB bytes */
				&dispSize, &displacement.displacement64, matchedInstr, opExpr, &needB, &needX, &needRR, CodeInfo);
		modRM |= BuildModRM(matchedInstr->modRM, matchedInstr, opExpr, &needModRM, &needSIB,
			((matchedInstr->vexflags & VEX) || (matchedInstr->vexflags & EVEX)));								/* Modify the modRM value for any non-memory operands */

		  /*----------------------------------------------------------
		   Create REX, VEX or EVEX prefixes                      
		  ----------------------------------------------------------*/
		if ((matchedInstr->vexflags & VEX) != 0 && (matchedInstr->evexflags & EVEX_ONLY) == 0 && CodeInfo->evex_flag == 0)
			BuildVEX(&needVEX, &vexSize, vexBytes, matchedInstr, opExpr, needB, needX, opCount);				/* Create the VEX prefix bytes for both reg and memory operands */

		  /* Either the instruction can ONLY be EVEX encoded, or user requested VEX->EVEX promotion. */
		else if ((matchedInstr->evexflags & EVEX_ONLY) != 0 ||
			((CodeInfo->evex_flag) && (matchedInstr->vexflags & EVEX) != 0))
			BuildEVEX(&needEVEX, evexBytes, matchedInstr, opExpr, needB, needX, needRR, opCount, CodeInfo);	/* Create the EVEX prefix bytes if the instruction supports an EVEX form */

		if (CodeInfo->evex_flag && matchedInstr->evexflags == NO_EVEX)											/* Not possible to EVEX encode instruction per users request */
			EmitError(NO_EVEX_FORM);

		else if (CodeInfo->Ofssize == USE64)
			rexByte |= BuildREX(rexByte, matchedInstr, opExpr);													/* Modify the REX prefix for non-memory operands/sizing */

		/*----------------------------------------------------------
		 Check if address or operand size override prefixes are required.
		----------------------------------------------------------*/
		if (Require_ADDR_Size_Override(matchedInstr, CodeInfo) || aso)
			OutputCodeByte(ADDR_SIZE_OVERRIDE);

		/*----------------------------------------------------------
		 Validate and output other prefixes (LOCK,REPx, BND)
		----------------------------------------------------------*/
		if (CodeInfo->prefix.ins == T_BND && (matchedInstr->flags & ALLOW_BND) == 0)
			EmitError(INSTRUCTION_PREFIX_NOT_ALLOWED);
		else if (CodeInfo->prefix.ins == T_BND)
			OutputCodeByte(BND);
		if (CodeInfo->prefix.ins == T_LOCK && (matchedInstr->flags & ALLOW_LOCK) == 0)
			EmitError(INSTRUCTION_PREFIX_NOT_ALLOWED);
		else if (CodeInfo->prefix.ins == T_LOCK)
			OutputCodeByte(LOCK);
		if (CodeInfo->prefix.ins == T_REP && (matchedInstr->flags & ALLOW_REP) == 0)
			EmitError(INSTRUCTION_PREFIX_NOT_ALLOWED);
		else if (CodeInfo->prefix.ins == T_REP)
			OutputCodeByte(REP);
		if (CodeInfo->prefix.ins == T_REPE && (matchedInstr->flags & ALLOW_REP) == 0)
			EmitError(INSTRUCTION_PREFIX_NOT_ALLOWED);
		else if (CodeInfo->prefix.ins == T_REPE)
			OutputCodeByte(REPE);
		if (CodeInfo->prefix.ins == T_REPZ && (matchedInstr->flags & ALLOW_REP) == 0)
			EmitError(INSTRUCTION_PREFIX_NOT_ALLOWED);
		else if (CodeInfo->prefix.ins == T_REPZ)
			OutputCodeByte(REPZ);
		if (CodeInfo->prefix.ins == T_REPNE && (matchedInstr->flags & ALLOW_REP) == 0)
			EmitError(INSTRUCTION_PREFIX_NOT_ALLOWED);
		else if (CodeInfo->prefix.ins == T_REPNE)
			OutputCodeByte(REPNE);
		if (CodeInfo->prefix.ins == T_REPNZ && (matchedInstr->flags & ALLOW_REP) == 0)
			EmitError(INSTRUCTION_PREFIX_NOT_ALLOWED);
		else if (CodeInfo->prefix.ins == T_REPNZ)
			OutputCodeByte(REPNZ);
		if (Require_OPND_Size_Override(matchedInstr, CodeInfo))
			OutputCodeByte(OP_SIZE_OVERRIDE);

		/*----------------------------------------------------------
		 Output Segment Prefix if required and allowed.
		----------------------------------------------------------*/
		if (CodeInfo->prefix.RegOverride != ASSUME_NOTHING)
		{
			if (matchedInstr->flags & ALLOW_SEG)
			{
				if (ModuleInfo.Ofssize == USE64 && (CodeInfo->prefix.RegOverride == ASSUME_FS || CodeInfo->prefix.RegOverride == ASSUME_GS || CodeInfo->prefix.RegOverride == ASSUME_SS))
				{
					switch (CodeInfo->prefix.RegOverride)
					{
					case ASSUME_FS:
						OutputCodeByte(PREFIX_FS);
						break;
					case ASSUME_GS:
						OutputCodeByte(PREFIX_GS);
						break;
					case ASSUME_SS:
						OutputCodeByte(PREFIX_SS);
						break;
					}
				}
				else if (ModuleInfo.Ofssize == USE64 && ((matchedInstr->flags & ALLOW_SEGX) == 0))
				{
					EmitError(ILLEGAL_USE_OF_SEGMENT_REGISTER);
					return ERROR;
				}
				else
				{
					switch (CodeInfo->prefix.RegOverride)
					{
					case ASSUME_CS:
						OutputCodeByte(PREFIX_CS);
						break;
					case ASSUME_DS:
						OutputCodeByte(PREFIX_DS);
						break;
					case ASSUME_ES:
						OutputCodeByte(PREFIX_ES);
						break;
					case ASSUME_SS:
						OutputCodeByte(PREFIX_SS);
						break;
					case ASSUME_FS:
						OutputCodeByte(PREFIX_FS);
						break;
					case ASSUME_GS:
						OutputCodeByte(PREFIX_GS);
						break;
					}
				}
			}
			else
			{
				EmitError(ILLEGAL_USE_OF_SEGMENT_REGISTER);
				return ERROR;
			}
		}

		/*----------------------------------------------------------
		 Output FPU FWAIT if required.
		----------------------------------------------------------*/

		/*----------------------------------------------------------
		 Output VEX prefix if required.
		----------------------------------------------------------*/
		if (needVEX)
			OutputBytes((unsigned char*)& vexBytes, vexSize, NULL);

		/*----------------------------------------------------------
		 Output EVEX prefix if required.
		 -> This is mutually exclusive with VEX above.
		----------------------------------------------------------*/
		if (needEVEX)
			OutputBytes((unsigned char*)& evexBytes, 4, NULL);

		/*----------------------------------------------------------
		 Output mandatory prefix (part 1).
		 -> Some mandatory prefixes must be split across a REX.
		----------------------------------------------------------*/
		switch (matchedInstr->mandatory_prefix)
		{
		case PFX_0x66F3A:
		case PFX_0x66F38:
		case PFX_0x66F:
			OutputCodeByte(0x66); /* first part. */
			break;
		case PFX_0xF3F38:
		case PFX_0xF30F:
			OutputCodeByte(0xf3); /* first part. */
			break;
		case PFX_0xF2F38:
		case PFX_0xF20F:
			OutputCodeByte(0xf2); /* first part. */
			break;
		}

		/*----------------------------------------------------------
		 Output REX prefix if required.
		 -> Not required for VEX or EVEX.
		----------------------------------------------------------*/
		if (rexByte != 0)
			OutputCodeByte(rexByte);

		/*----------------------------------------------------------
		 Output mandatory prefix (part 2).
		----------------------------------------------------------*/
		switch (matchedInstr->mandatory_prefix)
		{
		case PFX_0xF:
			OutputCodeByte(0x0f);
			break;
		case PFX_0x66F:
			OutputCodeByte(0x0f); /* second part. */
			break;
		case PFX_0x66F38:
			OutputCodeByte(0x0f); /* second part. */
			OutputCodeByte(0x38);
			break;
		case PFX_0x66F3A:
			OutputCodeByte(0x0f); /* second part. */
			OutputCodeByte(0x3a);
			break;
		case PFX_0xF30F:
		case PFX_0xF20F:
			OutputCodeByte(0x0f); /* second part. */
			break;
		case PFX_0x0F38:
		case PFX_0xF3F38:
		case PFX_0xF2F38:
			OutputCodeByte(0x0f); /* second part. */
			OutputCodeByte(0x38);
			break;
		}

		/*----------------------------------------------------------
		 Output opcode byte(s).
		----------------------------------------------------------*/
		/* Single opcode byte with embedded register. */
		if ((matchedInstr->flags & F_OPCODE_REG) != 0)
		{
			opcodeByte = matchedInstr->opcode[0];
			opcodeByte += (GetRegisterNo(opExpr[0].base_reg) & 0x07);
			OutputCodeByte(opcodeByte);
		}
		/* Normal opcode byte sequence. */
		else
		{
			for (i = 0; i < matchedInstr->opcode_bytes; i++)
				OutputCodeByte(matchedInstr->opcode[i]);
		}

		/*----------------------------------------------------------
		 Output ModR/M
		----------------------------------------------------------*/
		if (needModRM)
			OutputCodeByte(modRM);

		/*----------------------------------------------------------
		 Output SIB
		----------------------------------------------------------*/
		if (needSIB)
			OutputCodeByte(sib);

		/*----------------------------------------------------------
		  Output Displacement and Fixup.
		----------------------------------------------------------*/
		if (dispSize)
		{
			if (CodeInfo->opnd[(matchedInstr->memOpnd & 7)].InsFixup && needFixup)
			{
				if (Parse_Pass > PASS_1)
					if ((1 << CodeInfo->opnd[(matchedInstr->memOpnd & 7)].InsFixup->type) & ModuleInfo.fmtopt->invalid_fixup_type)
						EmitErr(UNSUPPORTED_FIXUP_TYPE, ModuleInfo.fmtopt->formatname, CodeInfo->opnd[(matchedInstr->memOpnd & 7)].InsFixup->sym ? CodeInfo->opnd[(matchedInstr->memOpnd & 7)].InsFixup->sym->name : szNullStr);
				if (write_to_file)
				{
					CodeInfo->opnd[(matchedInstr->memOpnd & 7)].InsFixup->locofs = GetCurrOffset();
					if (CodeInfo->isptr)
						OutputBytes((unsigned char*)& displacement.byte, dispSize, NULL);
					else
						OutputBytes((unsigned char*)& displacement.byte, dispSize, CodeInfo->opnd[(matchedInstr->memOpnd & 7)].InsFixup);
				}
				else
					OutputBytes((unsigned char*)& displacement.byte, dispSize, NULL);
			}
			else
				OutputBytes((unsigned char*)& displacement.byte, dispSize, NULL);
		}

		/*----------------------------------------------------------
		 Output Immediate Data.
		----------------------------------------------------------*/
		if (matchedInstr->immOpnd != NO_IMM)
		{
			immValue.full = CodeInfo->opnd[matchedInstr->immOpnd].data64;

			/* VEX encoded 4th operand as immediate data. */
			if ((matchedInstr->vexflags & VEX_4OPND) != 0)
			{
				immValue.full <<= 4;
			}

			/* Default is we assume the immediate is the operation size. */
			immSize = matchedInstr->op_size;
			if ((matchedInstr->flags & IMM8_ONLY) != 0)
				immSize = 1; // Forced 1 byte immediate size. */

			/* An immediate entry could require a fixup if it was generated from an OFFSET directive or Symbol value. */
			if (CodeInfo->opnd[matchedInstr->immOpnd].InsFixup && needFixup)
			{
				if (Parse_Pass > PASS_1)
					if ((1 << CodeInfo->opnd[matchedInstr->immOpnd].InsFixup->type) & ModuleInfo.fmtopt->invalid_fixup_type)
						EmitErr(UNSUPPORTED_FIXUP_TYPE, ModuleInfo.fmtopt->formatname, CodeInfo->opnd[matchedInstr->immOpnd].InsFixup->sym ? CodeInfo->opnd[matchedInstr->immOpnd].InsFixup->sym->name : szNullStr);
				if (write_to_file)
				{
					CodeInfo->opnd[matchedInstr->immOpnd].InsFixup->locofs = GetCurrOffset();
					if (CodeInfo->isptr)
						OutputBytes((unsigned char*)& immValue.byte, immSize, NULL);
					else
						OutputBytes((unsigned char*)& immValue.byte, immSize, CodeInfo->opnd[matchedInstr->immOpnd].InsFixup);
				}
				else
					OutputBytes((unsigned char*)& immValue.byte, immSize, NULL);
			}
			else
				OutputBytes((unsigned char*)& immValue.byte, immSize, NULL);
		}

		/*----------------------------------------------------------
		 Finalize fixup post immediate data.
		----------------------------------------------------------*/
		/* For rip-relative fixups, the instruction end is needed */
		if (CodeInfo->Ofssize == USE64)
		{
			if (CodeInfo->opnd[OPND1].InsFixup && CodeInfo->opnd[OPND1].InsFixup->type == FIX_RELOFF32)
				CodeInfo->opnd[OPND1].InsFixup->addbytes = GetCurrOffset() - CodeInfo->opnd[OPND1].InsFixup->locofs;
			if (CodeInfo->opnd[OPND2].InsFixup && CodeInfo->opnd[OPND2].InsFixup->type == FIX_RELOFF32)
				CodeInfo->opnd[OPND2].InsFixup->addbytes = GetCurrOffset() - CodeInfo->opnd[OPND2].InsFixup->locofs;
		}

	}
skip:
	/* Write out listing. */
	if (retcode == NOT_ERROR)
	{
		if (CurrFile[LST])
			LstWrite(LSTTYPE_CODE, oldofs, NULL);
	}

	return retcode;
}
