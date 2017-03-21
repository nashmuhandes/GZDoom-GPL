/*
** vmbuilder.cpp
**
**---------------------------------------------------------------------------
** Copyright -2016 Randy Heit
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
*/

#include "vmbuilder.h"
#include "codegen.h"
#include "info.h"
#include "m_argv.h"
#include "thingdef.h"
#include "doomerrors.h"

struct VMRemap
{
	uint8_t altOp, kReg, kType;
};


#define xx(op, name, mode, alt, kreg, ktype) {OP_##alt, kreg, ktype }
VMRemap opRemap[NUM_OPS] = {
#include "vmops.h"
};
#undef xx

//==========================================================================
//
// VMFunctionBuilder - Constructor
//
//==========================================================================

VMFunctionBuilder::VMFunctionBuilder(int numimplicits)
{
	MaxParam = 0;
	ActiveParam = 0;
	NumImplicits = numimplicits;
}

//==========================================================================
//
// VMFunctionBuilder - Destructor
//
//==========================================================================

VMFunctionBuilder::~VMFunctionBuilder()
{
}

//==========================================================================
//
// VMFunctionBuilder :: BeginStatement
//
// Records the start of a new statement.
//
//==========================================================================

void VMFunctionBuilder::BeginStatement(FxExpression *stmt)
{
	// pop empty statement records.
	while (LineNumbers.Size() > 0 && LineNumbers.Last().InstructionIndex == Code.Size()) LineNumbers.Pop();
	// only add a new entry if the line number differs.
	if (LineNumbers.Size() == 0 || stmt->ScriptPosition.ScriptLine != LineNumbers.Last().LineNumber)
	{
		FStatementInfo si = { (uint16_t)Code.Size(), (uint16_t)stmt->ScriptPosition.ScriptLine };
		LineNumbers.Push(si);
	}
	StatementStack.Push(stmt);
}

void VMFunctionBuilder::EndStatement()
{
	// pop empty statement records.
	while (LineNumbers.Size() > 0 && LineNumbers.Last().InstructionIndex == Code.Size()) LineNumbers.Pop();
	StatementStack.Pop();
	// Re-enter the previous statement.
	if (StatementStack.Size() > 0)
	{
		FStatementInfo si = { (uint16_t)Code.Size(), (uint16_t)StatementStack.Last()->ScriptPosition.ScriptLine };
		LineNumbers.Push(si);
	}
}

void VMFunctionBuilder::MakeFunction(VMScriptFunction *func)
{
	func->Alloc(Code.Size(), IntConstantList.Size(), FloatConstantList.Size(), StringConstantList.Size(), AddressConstantList.Size(), LineNumbers.Size());

	// Copy code block.
	memcpy(func->Code, &Code[0], Code.Size() * sizeof(VMOP));
	memcpy(func->LineInfo, &LineNumbers[0], LineNumbers.Size() * sizeof(LineNumbers[0]));

	// Create constant tables.
	if (IntConstantList.Size() > 0)
	{
		FillIntConstants(func->KonstD);
	}
	if (FloatConstantList.Size() > 0)
	{
		FillFloatConstants(func->KonstF);
	}
	if (AddressConstantList.Size() > 0)
	{
		FillAddressConstants(func->KonstA, func->KonstATags());
	}
	if (StringConstantList.Size() > 0)
	{
		FillStringConstants(func->KonstS);
	}

	// Assign required register space.
	func->NumRegD = Registers[REGT_INT].MostUsed;
	func->NumRegF = Registers[REGT_FLOAT].MostUsed;
	func->NumRegA = Registers[REGT_POINTER].MostUsed;
	func->NumRegS = Registers[REGT_STRING].MostUsed;
	func->MaxParam = MaxParam;

	// Technically, there's no reason why we can't end the function with
	// entries on the parameter stack, but it means the caller probably
	// did something wrong.
	assert(ActiveParam == 0);
}

//==========================================================================
//
// VMFunctionBuilder :: FillIntConstants
//
//==========================================================================

void VMFunctionBuilder::FillIntConstants(int *konst)
{
	memcpy(konst, &IntConstantList[0], sizeof(int) * IntConstantList.Size());
}

//==========================================================================
//
// VMFunctionBuilder :: FillFloatConstants
//
//==========================================================================

void VMFunctionBuilder::FillFloatConstants(double *konst)
{
	memcpy(konst, &FloatConstantList[0], sizeof(double) * FloatConstantList.Size());
}

//==========================================================================
//
// VMFunctionBuilder :: FillAddressConstants
//
//==========================================================================

void VMFunctionBuilder::FillAddressConstants(FVoidObj *konst, VM_ATAG *tags)
{
	memcpy(konst, &AddressConstantList[0], sizeof(void*) * AddressConstantList.Size());
	memcpy(tags, &AtagConstantList[0], sizeof(VM_ATAG) * AtagConstantList.Size());
}

//==========================================================================
//
// VMFunctionBuilder :: FillStringConstants
//
//==========================================================================

void VMFunctionBuilder::FillStringConstants(FString *konst)
{
	for (auto &s : StringConstantList)
	{
		*konst++ = s;
	}
}

//==========================================================================
//
// VMFunctionBuilder :: GetConstantInt
//
// Returns a constant register initialized with the given value.
//
//==========================================================================

unsigned VMFunctionBuilder::GetConstantInt(int val)
{
	unsigned int *locp = IntConstantMap.CheckKey(val);
	if (locp != NULL)
	{
		return *locp;
	}
	else
	{
		unsigned loc = IntConstantList.Push(val);
		IntConstantMap.Insert(val, loc);
		return loc;
	}
}

//==========================================================================
//
// VMFunctionBuilder :: GetConstantFloat
//
// Returns a constant register initialized with the given value.
//
//==========================================================================

unsigned VMFunctionBuilder::GetConstantFloat(double val)
{
	unsigned *locp = FloatConstantMap.CheckKey(val);
	if (locp != NULL)
	{
		return *locp;
	}
	else
	{
		unsigned loc = FloatConstantList.Push(val);
		FloatConstantMap.Insert(val, loc);
		return loc;
	}
}

//==========================================================================
//
// VMFunctionBuilder :: GetConstantString
//
// Returns a constant register initialized with the given value.
//
//==========================================================================

unsigned VMFunctionBuilder::GetConstantString(FString val)
{
	unsigned *locp = StringConstantMap.CheckKey(val);
	if (locp != NULL)
	{
		return *locp;
	}
	else
	{
		int loc = StringConstantList.Push(val);
		StringConstantMap.Insert(val, loc);
		return loc;
	}
}

//==========================================================================
//
// VMFunctionBuilder :: GetConstantAddress
//
// Returns a constant register initialized with the given value, or -1 if
// there were no more constants free.
//
//==========================================================================

unsigned VMFunctionBuilder::GetConstantAddress(void *ptr, VM_ATAG tag)
{
	if (ptr == NULL)
	{ // Make all NULL pointers generic. (Or should we allow typed NULLs?)
		tag = ATAG_GENERIC;
	}
	AddrKonst *locp = AddressConstantMap.CheckKey(ptr);
	if (locp != NULL)
	{
		// There should only be one tag associated with a memory location. Exceptions are made for null pointers that got allocated through constant arrays.
		assert(ptr == nullptr || locp->Tag == tag);
		return locp->KonstNum;
	}
	else
	{
		unsigned locc = AddressConstantList.Push(ptr);
		AtagConstantList.Push(tag);

		AddrKonst loc = { locc, tag };
		AddressConstantMap.Insert(ptr, loc);
		return loc.KonstNum;
	}
}

//==========================================================================
//
// VMFunctionBuilder :: AllocConstants*
//
// Returns a range of constant register initialized with the given values.
//
//==========================================================================

unsigned VMFunctionBuilder::AllocConstantsInt(unsigned count, int *values)
{
	unsigned addr = IntConstantList.Reserve(count);
	memcpy(&IntConstantList[addr], values, count * sizeof(int));
	for (unsigned i = 0; i < count; i++)
	{
		IntConstantMap.Insert(values[i], addr + i);
	}
	return addr;
}

unsigned VMFunctionBuilder::AllocConstantsFloat(unsigned count, double *values)
{
	unsigned addr = FloatConstantList.Reserve(count);
	memcpy(&FloatConstantList[addr], values, count * sizeof(double));
	for (unsigned i = 0; i < count; i++)
	{
		FloatConstantMap.Insert(values[i], addr + i);
	}
	return addr;
}

unsigned VMFunctionBuilder::AllocConstantsAddress(unsigned count, void **ptrs, VM_ATAG tag)
{
	unsigned addr = AddressConstantList.Reserve(count);
	AtagConstantList.Reserve(count);
	memcpy(&AddressConstantList[addr], ptrs, count * sizeof(void *));
	for (unsigned i = 0; i < count; i++)
	{
		AtagConstantList[addr + i] = tag;
		AddrKonst loc = { addr+i, tag };
		AddressConstantMap.Insert(ptrs[i], loc);
	}
	return addr;
}

unsigned VMFunctionBuilder::AllocConstantsString(unsigned count, FString *ptrs)
{
	unsigned addr = StringConstantList.Reserve(count);
	for (unsigned i = 0; i < count; i++)
	{
		StringConstantList[addr + i] = ptrs[i];
		StringConstantMap.Insert(ptrs[i], addr + i);
	}
	return addr;
}


//==========================================================================
//
// VMFunctionBuilder :: ParamChange
//
// Adds delta to ActiveParam and keeps track of MaxParam.
//
//==========================================================================

void VMFunctionBuilder::ParamChange(int delta)
{
	assert(delta > 0 || -delta <= ActiveParam);
	ActiveParam += delta;
	if (ActiveParam > MaxParam)
	{
		MaxParam = ActiveParam;
	}
}

//==========================================================================
//
// VMFunctionBuilder :: RegAvailability - Constructor
//
//==========================================================================

VMFunctionBuilder::RegAvailability::RegAvailability()
{
	memset(Used, 0, sizeof(Used));
	MostUsed = 0;
}

//==========================================================================
//
// VMFunctionBuilder :: RegAvailability :: Get
//
// Gets one or more unused registers. If getting multiple registers, they
// will all be consecutive. Returns -1 if there were not enough consecutive
// registers to satisfy the request.
//
// Preference is given to low-numbered registers in an attempt to keep
// the maximum register count low so as to preserve VM stack space when this
// function is executed.
//
//==========================================================================

int VMFunctionBuilder::RegAvailability::Get(int count)
{
	VM_UWORD mask;
	int i, firstbit;

	// Getting fewer than one register makes no sense, and
	// the algorithm used here can only obtain ranges of up to 32 bits.
	if (count < 1 || count > 32)
	{
		return -1;
	}
	
	mask = count == 32 ? ~0u : (1 << count) - 1;

	for (i = 0; i < 256 / 32; ++i)
	{
		// Find the first word with free registers
		VM_UWORD bits = Used[i];
		if (bits != ~0u)
		{
			// Are there enough consecutive bits to satisfy the request?
			// Search by 16, then 8, then 1 bit at a time for the first
			// free register.
			if ((bits & 0xFFFF) == 0xFFFF)
			{
				firstbit = ((bits & 0xFF0000) == 0xFF0000) ? 24 : 16;
			}
			else
			{
				firstbit = ((bits & 0xFF) == 0xFF) ? 8 : 0;
			}
			for (; firstbit < 32; ++firstbit)
			{
				if (((bits >> firstbit) & mask) == 0)
				{
					if (firstbit + count <= 32)
					{ // Needed bits all fit in one word, so we got it.
						if (i * 32 + firstbit + count > MostUsed)
						{
							MostUsed = i * 32 + firstbit + count;
						}
						Used[i] |= mask << firstbit;
						return i * 32 + firstbit;
					}
					// Needed bits span two words, so check the next word.
					else if (i < 256/32 - 1)
					{ // There is a next word.
						if (((Used[i + 1]) & (mask >> (32 - firstbit))) == 0)
						{ // The next word has the needed open space, too.
							if (i * 32 + firstbit + count > MostUsed)
							{
								MostUsed = i * 32 + firstbit + count;
							}
							Used[i] |= mask << firstbit;
							Used[i + 1] |= mask >> (32 - firstbit);
							return i * 32 + firstbit;
						}
						else
						{ // Skip to the next word, because we know we won't find
						  // what we need if we stay inside this one. All bits
						  // from firstbit to the end of the word are 0. If the
						  // next word does not start with the x amount of 0's, we
						  // need to satisfy the request, then it certainly won't
						  // have the x+1 0's we would need if we started at
						  // firstbit+1 in this one.
							firstbit = 32;
						}
					}
					else
					{ // Out of words.
						break;
					}
				}
			}
		}
	}
	// No room!
	return -1;
}

//==========================================================================
//
// VMFunctionBuilder :: RegAvailibity :: Return
//
// Marks a range of registers as free again.
//
//==========================================================================

void VMFunctionBuilder::RegAvailability::Return(int reg, int count)
{
	assert(count >= 1 && count <= 32);
	assert(reg >= 0 && reg + count <= 256);

	VM_UWORD mask, partialmask;
	int firstword, firstbit;

	mask = count == 32 ? ~0u : (1 << count) - 1;
	firstword = reg / 32;
	firstbit = reg & 31;

	if (firstbit + count <= 32)
	{ // Range is all in one word.
		mask <<= firstbit;
		// If we are trying to return registers that are already free,
		// it probably means that the caller messed up somewhere.
		assert((Used[firstword] & mask) == mask);
		Used[firstword] &= ~mask;
	}
	else
	{ // Range is in two words.
		partialmask = mask << firstbit;
		assert((Used[firstword] & partialmask) == partialmask);
		Used[firstword] &= ~partialmask;

		partialmask = mask >> (32 - firstbit);
		assert((Used[firstword + 1] & partialmask) == partialmask);
		Used[firstword + 1] &= ~partialmask;
	}
}

//==========================================================================
//
// VMFunctionBuilder :: RegAvailability :: Reuse
//
// Marks an unused register as in-use. Returns false if the register is
// already in use or true if it was successfully reused.
//
//==========================================================================

bool VMFunctionBuilder::RegAvailability::Reuse(int reg)
{
	assert(reg >= 0 && reg <= 255);
	assert(reg < MostUsed && "Attempt to reuse a register that was never used");

	VM_UWORD mask = 1 << (reg & 31);
	int word = reg / 32;

	if (Used[word] & mask)
	{ // It's already in use!
		return false;
	}
	Used[word] |= mask;
	return true;
}

//==========================================================================
//
// VMFunctionBuilder :: GetAddress
//
//==========================================================================

size_t VMFunctionBuilder::GetAddress()
{
	return Code.Size();
}

//==========================================================================
//
// VMFunctionBuilder :: Emit
//
// Just dumbly output an instruction. Returns instruction position, not
// byte position. (Because all instructions are exactly four bytes long.)
//
//==========================================================================

size_t VMFunctionBuilder::Emit(int opcode, int opa, int opb, int opc)
{
	static uint8_t opcodes[] = { OP_LK, OP_LKF, OP_LKS, OP_LKP };

	assert(opcode >= 0 && opcode < NUM_OPS);
	assert(opa >= 0);
	assert(opb >= 0);
	assert(opc >= 0);

	// The following were just asserts, meaning this would silently create broken code if there was an overflow
	// if this happened in a release build. Not good.
	// These are critical errors that need to be reported to the user.
	// In addition, the limit of 256 constants can easily be exceeded with arrays so this had to be extended to
	// 65535 by adding some checks here that map byte-limited instructions to alternatives that can handle larger indices.
	// (See vmops.h for the remapping info.)

	// Note: OP_CMPS also needs treatment, but I do not expect constant overflow to become an issue with strings, so for now there is no handling.

	if (opa > 255)
	{
		if (opRemap[opcode].kReg != 1 || opa > 32767)
		{
			I_Error("Register limit exceeded");
		}
		int regtype = opRemap[opcode].kType;
		ExpEmit emit(this, regtype);
		Emit(opcodes[regtype], emit.RegNum, opa);
		opcode = opRemap[opcode].altOp;
		opa = emit.RegNum;
		emit.Free(this);
	}
	if (opb > 255)
	{
		if (opRemap[opcode].kReg != 2 || opb > 32767)
		{
			I_Error("Register limit exceeded");
		}
		int regtype = opRemap[opcode].kType;
		ExpEmit emit(this, regtype);
		Emit(opcodes[regtype], emit.RegNum, opb);
		opcode = opRemap[opcode].altOp;
		opb = emit.RegNum;
		emit.Free(this);
	}
	if (opc > 255)
	{
		if (opcode == OP_PARAM && (opb & REGT_KONST) && opc <= 32767)
		{
			int regtype = opb & REGT_TYPE;
			opb = regtype;
			ExpEmit emit(this, regtype);
			Emit(opcodes[regtype], emit.RegNum, opc);
			opc = emit.RegNum;
			emit.Free(this);
		}
		else
		{
			if (opRemap[opcode].kReg != 4 || opc > 32767)
			{
				I_Error("Register limit exceeded");
			}
			int regtype = opRemap[opcode].kType;
			ExpEmit emit(this, regtype);
			Emit(opcodes[regtype], emit.RegNum, opc);
			opcode = opRemap[opcode].altOp;
			opc = emit.RegNum;
			emit.Free(this);
		}
	}

	if (opcode == OP_PARAM)
	{
		int chg;
		if (opb & REGT_MULTIREG2) chg = 2;
		else if (opb&REGT_MULTIREG3) chg = 3;
		else chg = 1;
		ParamChange(chg);
	}
	else if (opcode == OP_CALL || opcode == OP_CALL_K || opcode == OP_TAIL || opcode == OP_TAIL_K)
	{
		ParamChange(-opb);
	}
	VMOP op;
	op.op = opcode;
	op.a = opa;
	op.b = opb;
	op.c = opc;
	return Code.Push(op);
}

size_t VMFunctionBuilder::Emit(int opcode, int opa, VM_SHALF opbc)
{
	assert(opcode >= 0 && opcode < NUM_OPS);
	assert(opa >= 0 && opa <= 255);
	//assert(opbc >= -32768 && opbc <= 32767);	always true due to parameter's width
	VMOP op;
	op.op = opcode;
	op.a = opa;
	op.i16 = opbc;
	return Code.Push(op);
}

size_t VMFunctionBuilder::Emit(int opcode, int opabc)
{
	assert(opcode >= 0 && opcode < NUM_OPS);
	assert(opabc >= -(1 << 23) && opabc <= (1 << 24) - 1);
	if (opcode == OP_PARAMI)
	{
		ParamChange(1);
	}
	VMOP op;
	op.op = opcode;
	op.i24 = opabc;
	return Code.Push(op);
}

//==========================================================================
//
// VMFunctionBuilder :: EmitParamInt
//
// Passes a constant integer parameter, using either PARAMI and an immediate
// value or PARAM and a constant register, as appropriate.
//
//==========================================================================

size_t VMFunctionBuilder::EmitParamInt(int value)
{
	// Immediates for PARAMI must fit in 24 bits.
	if (((value << 8) >> 8) == value)
	{
		return Emit(OP_PARAMI, value);
	}
	else
	{
		return Emit(OP_PARAM, 0, REGT_INT | REGT_KONST, GetConstantInt(value));
	}
}

//==========================================================================
//
// VMFunctionBuilder :: EmitLoadInt
//
// Loads an integer constant into a register, using either an immediate
// value or a constant register, as appropriate.
//
//==========================================================================

size_t VMFunctionBuilder::EmitLoadInt(int regnum, int value)
{
	assert(regnum >= 0 && regnum < Registers[REGT_INT].MostUsed);
	if (value >= -32768 && value <= 32767)
	{
		return Emit(OP_LI, regnum, value);
	}
	else
	{
		return Emit(OP_LK, regnum, GetConstantInt(value));
	}
}

//==========================================================================
//
// VMFunctionBuilder :: EmitRetInt
//
// Returns an integer, using either an immediate value or a constant
// register, as appropriate.
//
//==========================================================================

size_t VMFunctionBuilder::EmitRetInt(int retnum, bool final, int value)
{
	assert(retnum >= 0 && retnum <= 127);
	if (value >= -32768 && value <= 32767)
	{
		return Emit(OP_RETI, retnum | (final << 7), value);
	}
	else
	{
		return Emit(OP_RET, retnum | (final << 7), REGT_INT | REGT_KONST, GetConstantInt(value));
	}
}

//==========================================================================
//
// VMFunctionBuilder :: Backpatch
//
// Store a JMP instruction at <loc> that points at <target>.
//
//==========================================================================

void VMFunctionBuilder::Backpatch(size_t loc, size_t target)
{
	assert(loc < Code.Size());
	int offset = int(target - loc - 1);
	assert(((offset << 8) >> 8) == offset);
	Code[loc].op = OP_JMP;
	Code[loc].i24 = offset;
}

void VMFunctionBuilder::BackpatchList(TArray<size_t> &locs, size_t target)
{
	for (auto loc : locs)
		Backpatch(loc, target);
}


//==========================================================================
//
// VMFunctionBuilder :: BackpatchToHere
//
// Store a JMP instruction at <loc> that points to the current code gen
// location.
//
//==========================================================================

void VMFunctionBuilder::BackpatchToHere(size_t loc)
{
	Backpatch(loc, Code.Size());
}

void VMFunctionBuilder::BackpatchListToHere(TArray<size_t> &locs)
{
	for (auto loc : locs)
		Backpatch(loc, Code.Size());
}

//==========================================================================
//
// FFunctionBuildList
//
// This list contains all functions yet to build.
// All adding functions return a VMFunction - either a complete one
// for native functions or an empty VMScriptFunction for scripted ones
// This VMScriptFunction object later gets filled in with the actual
// info, but we get the pointer right after registering the function
// with the builder.
//
//==========================================================================
FFunctionBuildList FunctionBuildList;

VMFunction *FFunctionBuildList::AddFunction(PNamespace *gnspc, const VersionInfo &ver, PFunction *functype, FxExpression *code, const FString &name, bool fromdecorate, int stateindex, int statecount, int lumpnum)
{
	auto func = code->GetDirectFunction(ver);
	if (func != nullptr)
	{
		delete code;
		return func;
	}

	//Printf("Adding %s\n", name.GetChars());

	Item it;
	assert(gnspc != nullptr);
	it.CurGlobals = gnspc;
	it.Func = functype;
	it.Code = code;
	it.PrintableName = name;
	it.Function = new VMScriptFunction;
	it.Function->Name = functype->SymbolName;
	it.Function->PrintableName = name;
	it.Function->ImplicitArgs = functype->GetImplicitArgs();
	it.Proto = nullptr;
	it.FromDecorate = fromdecorate;
	it.StateIndex = stateindex;
	it.StateCount = statecount;
	it.Lump = lumpnum;
	it.Version = ver;
	assert(it.Func->Variants.Size() == 1);
	it.Func->Variants[0].Implementation = it.Function;

	// set prototype for named functions.
	if (it.Func->SymbolName != NAME_None)
	{
		it.Function->Proto = it.Func->Variants[0].Proto;
	}

	mItems.Push(it);
	return it.Function;
}


void FFunctionBuildList::Build()
{
	int errorcount = 0;
	int codesize = 0;
	int datasize = 0;
	FILE *dump = nullptr;

	if (Args->CheckParm("-dumpdisasm")) dump = fopen("disasm.txt", "w");

	for (auto &item : mItems)
	{
		assert(item.Code != NULL);

		// We don't know the return type in advance for anonymous functions.
		FCompileContext ctx(item.CurGlobals, item.Func, item.Func->SymbolName == NAME_None ? nullptr : item.Func->Variants[0].Proto, item.FromDecorate, item.StateIndex, item.StateCount, item.Lump, item.Version);

		// Allocate registers for the function's arguments and create local variable nodes before starting to resolve it.
		VMFunctionBuilder buildit(item.Func->GetImplicitArgs());
		for (unsigned i = 0; i < item.Func->Variants[0].Proto->ArgumentTypes.Size(); i++)
		{
			auto type = item.Func->Variants[0].Proto->ArgumentTypes[i];
			auto name = item.Func->Variants[0].ArgNames[i];
			auto flags = item.Func->Variants[0].ArgFlags[i];
			// this won't get resolved and won't get emitted. It is only needed so that the code generator can retrieve the necessary info about this argument to do its work.
			auto local = new FxLocalVariableDeclaration(type, name, nullptr, flags, FScriptPosition());
			if (!(flags & VARF_Out)) local->RegNum = buildit.Registers[type->GetRegType()].Get(type->GetRegCount());
			else local->RegNum = buildit.Registers[REGT_POINTER].Get(1);
			ctx.FunctionArgs.Push(local);
		}

		FScriptPosition::StrictErrors = !item.FromDecorate;
		item.Code = item.Code->Resolve(ctx);
		// If we need extra space, load the frame pointer into a register so that we do not have to call the wasteful LFP instruction more than once.
		if (item.Function->ExtraSpace > 0)
		{
			buildit.FramePointer = ExpEmit(&buildit, REGT_POINTER);
			buildit.FramePointer.Fixed = true;
			buildit.Emit(OP_LFP, buildit.FramePointer.RegNum);
		}

		// Make sure resolving it didn't obliterate it.
		if (item.Code != nullptr)
		{
			if (!item.Code->CheckReturn())
			{
				auto newcmpd = new FxCompoundStatement(item.Code->ScriptPosition);
				newcmpd->Add(item.Code);
				newcmpd->Add(new FxReturnStatement(nullptr, item.Code->ScriptPosition));
				item.Code = newcmpd->Resolve(ctx);
			}

			item.Proto = ctx.ReturnProto;
			if (item.Proto == nullptr)
			{
				item.Code->ScriptPosition.Message(MSG_ERROR, "Function %s without prototype", item.PrintableName.GetChars());
				continue;
			}

			// Generate prototype for anonymous functions.
			VMScriptFunction *sfunc = item.Function;
			// create a new prototype from the now known return type and the argument list of the function's template prototype.
			if (sfunc->Proto == nullptr)
			{
				sfunc->Proto = NewPrototype(item.Proto->ReturnTypes, item.Func->Variants[0].Proto->ArgumentTypes);
			}

			// Emit code
			try
			{
				sfunc->SourceFileName = item.Code->ScriptPosition.FileName;	// remember the file name for printing error messages if something goes wrong in the VM.
				buildit.BeginStatement(item.Code);
				item.Code->Emit(&buildit);
				buildit.EndStatement();
				buildit.MakeFunction(sfunc);
				sfunc->NumArgs = 0;
				// NumArgs for the VMFunction must be the amount of stack elements, which can differ from the amount of logical function arguments if vectors are in the list.
				// For the VM a vector is 2 or 3 args, depending on size.
				for (auto s : item.Func->Variants[0].Proto->ArgumentTypes)
				{
					sfunc->NumArgs += s->GetRegCount();
				}

				if (dump != nullptr)
				{
					DumpFunction(dump, sfunc, item.PrintableName.GetChars(), (int)item.PrintableName.Len());
					codesize += sfunc->CodeSize;
					datasize += sfunc->LineInfoCount * sizeof(FStatementInfo) + sfunc->ExtraSpace + sfunc->NumKonstD * sizeof(int) +
						sfunc->NumKonstA * sizeof(void*) + sfunc->NumKonstF * sizeof(double) + sfunc->NumKonstS * sizeof(FString);
				}
				sfunc->Unsafe = ctx.Unsafe;
			}
			catch (CRecoverableError &err)
			{
				// catch errors from the code generator and pring something meaningful.
				item.Code->ScriptPosition.Message(MSG_ERROR, "%s in %s", err.GetMessage(), item.PrintableName.GetChars());
			}
		}
		delete item.Code;
		if (dump != nullptr)
		{
			fflush(dump);
		}
	}
	if (dump != nullptr)
	{
		fprintf(dump, "\n*************************************************************************\n%i code bytes\n%i data bytes", codesize * 4, datasize);
		fclose(dump);
	}
	FScriptPosition::StrictErrors = false;
	mItems.Clear();
	mItems.ShrinkToFit();
	FxAlloc.FreeAllBlocks();
}