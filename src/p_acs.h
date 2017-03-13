/*
** p_acs.h
** ACS script stuff
**
**---------------------------------------------------------------------------
** Copyright 1998-2012 Randy Heit
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

#ifndef __P_ACS_H__
#define __P_ACS_H__

#include "dobject.h"
#include "dthinker.h"
#include "doomtype.h"

#define LOCAL_SIZE				20
#define NUM_MAPVARS				128

class FFont;
class FileReader;
struct line_t;


enum
{
	NUM_WORLDVARS = 256,
	NUM_GLOBALVARS = 64
};

struct InitIntToZero
{
	void Init(int &v)
	{
		v = 0;
	}
};
typedef TMap<int32_t, int32_t, THashTraits<int32_t>, InitIntToZero> FWorldGlobalArray;

// ACS variables with world scope
extern int32_t ACS_WorldVars[NUM_WORLDVARS];
extern FWorldGlobalArray ACS_WorldArrays[NUM_WORLDVARS];

// ACS variables with global scope
extern int32_t ACS_GlobalVars[NUM_GLOBALVARS];
extern FWorldGlobalArray ACS_GlobalArrays[NUM_GLOBALVARS];

#define LIBRARYID_MASK			0xFFF00000
#define LIBRARYID_SHIFT			20

// Global ACS string table
#define STRPOOL_LIBRARYID		(INT_MAX >> LIBRARYID_SHIFT)
#define STRPOOL_LIBRARYID_OR	(STRPOOL_LIBRARYID << LIBRARYID_SHIFT)

class ACSStringPool
{
public:
	ACSStringPool();
	int AddString(const char *str);
	int AddString(FString &str);
	const char *GetString(int strnum);
	void LockString(int strnum);
	void UnlockString(int strnum);
	void UnlockAll();
	void MarkString(int strnum);
	void LockStringArray(const int *strnum, unsigned int count);
	void UnlockStringArray(const int *strnum, unsigned int count);
	void MarkStringArray(const int *strnum, unsigned int count);
	void MarkStringMap(const FWorldGlobalArray &array);
	void PurgeStrings();
	void Clear();
	void Dump() const;
	void UnlockForLevel(int level)	;
	void ReadStrings(FSerializer &file, const char *key);
	void WriteStrings(FSerializer &file, const char *key) const;

private:
	int FindString(const char *str, size_t len, unsigned int h, unsigned int bucketnum);
	int InsertString(FString &str, unsigned int h, unsigned int bucketnum);
	void FindFirstFreeEntry(unsigned int base);

	enum { NUM_BUCKETS = 251 };
	enum { FREE_ENTRY = 0xFFFFFFFE };	// Stored in PoolEntry's Next field
	enum { NO_ENTRY = 0xFFFFFFFF };
	enum { MIN_GC_SIZE = 100 };			// Don't auto-collect until there are this many strings
	struct PoolEntry
	{
		FString Str;
		unsigned int Hash;
		unsigned int Next;
		bool Mark;
		TArray<int> Locks;

		void Lock();
		void Unlock();
	};
	TArray<PoolEntry> Pool;
	unsigned int PoolBuckets[NUM_BUCKETS];
	unsigned int FirstFreeEntry;
};
extern ACSStringPool GlobalACSStrings;

void P_CollectACSGlobalStrings();
void P_ReadACSVars(FSerializer &);
void P_WriteACSVars(FSerializer &);
void P_ClearACSVars(bool);

struct ACSProfileInfo
{
	unsigned long long TotalInstr;
	unsigned int NumRuns;
	unsigned int MinInstrPerRun;
	unsigned int MaxInstrPerRun;

	ACSProfileInfo();
	void AddRun(unsigned int num_instr);
	void Reset();
};

struct ProfileCollector
{
	ACSProfileInfo *ProfileData;
	class FBehavior *Module;
	int Index;
};

struct ACSLocalArrayInfo
{
	unsigned int Size;
	int Offset;
};

struct ACSLocalArrays
{
	unsigned int Count;
	ACSLocalArrayInfo *Info;

	ACSLocalArrays()
	{
		Count = 0;
		Info = NULL;
	}
	~ACSLocalArrays()
	{
		if (Info != NULL)
		{
			delete[] Info;
			Info = NULL;
		}
	}

	// Bounds-checking Set and Get for local arrays
	void Set(int *locals, int arraynum, int arrayentry, int value)
	{
		if ((unsigned int)arraynum < Count &&
			(unsigned int)arrayentry < Info[arraynum].Size)
		{
			locals[Info[arraynum].Offset + arrayentry] = value;
		}
	}
	int Get(int *locals, int arraynum, int arrayentry)
	{
		if ((unsigned int)arraynum < Count &&
			(unsigned int)arrayentry < Info[arraynum].Size)
		{
			return locals[Info[arraynum].Offset + arrayentry];
		}
		return 0;
	}
};

// The in-memory version
struct ScriptPtr
{
	int Number;
	uint32_t Address;
	uint8_t Type;
	uint8_t ArgCount;
	uint16_t VarCount;
	uint16_t Flags;
	ACSLocalArrays LocalArrays;

	ACSProfileInfo ProfileData;
};

// The present ZDoom version
struct ScriptPtr3
{
	int16_t Number;
	uint8_t Type;
	uint8_t ArgCount;
	uint32_t Address;
};

// The intermediate ZDoom version
struct ScriptPtr1
{
	int16_t Number;
	uint16_t Type;
	uint32_t Address;
	uint32_t ArgCount;
};

// The old Hexen version
struct ScriptPtr2
{
	uint32_t Number;	// Type is Number / 1000
	uint32_t Address;
	uint32_t ArgCount;
};

struct ScriptFlagsPtr
{
	uint16_t Number;
	uint16_t Flags;
};

struct ScriptFunctionInFile
{
	uint8_t ArgCount;
	uint8_t LocalCount;
	uint8_t HasReturnValue;
	uint8_t ImportNum;
	uint32_t Address;
};

struct ScriptFunction
{
	uint8_t ArgCount;
	uint8_t HasReturnValue;
	uint8_t ImportNum;
	int  LocalCount;
	uint32_t Address;
	ACSLocalArrays LocalArrays;
};

// Script types
enum
{
	SCRIPT_Closed		= 0,
	SCRIPT_Open			= 1,
	SCRIPT_Respawn		= 2,
	SCRIPT_Death		= 3,
	SCRIPT_Enter		= 4,
	SCRIPT_Pickup		= 5,
	SCRIPT_BlueReturn	= 6,
	SCRIPT_RedReturn	= 7,
	SCRIPT_WhiteReturn	= 8,
	SCRIPT_Lightning	= 12,
	SCRIPT_Unloading	= 13,
	SCRIPT_Disconnect	= 14,
	SCRIPT_Return		= 15,
	SCRIPT_Event		= 16, // [BB]
	SCRIPT_Kill			= 17, // [JM]
	SCRIPT_Reopen		= 18, // [Nash]
};

// Script flags
enum
{
	SCRIPTF_Net = 0x0001	// Safe to "puke" in multiplayer
};

enum ACSFormat { ACS_Old, ACS_Enhanced, ACS_LittleEnhanced, ACS_Unknown };

class FBehavior
{
public:
	FBehavior ();
	~FBehavior ();
	bool Init(int lumpnum, FileReader * fr = NULL, int len = 0);

	bool IsGood ();
	uint8_t *FindChunk (uint32_t id) const;
	uint8_t *NextChunk (uint8_t *chunk) const;
	const ScriptPtr *FindScript (int number) const;
	void StartTypedScripts (uint16_t type, AActor *activator, bool always, int arg1, bool runNow);
	uint32_t PC2Ofs (int *pc) const { return (uint32_t)((uint8_t *)pc - Data); }
	int *Ofs2PC (uint32_t ofs) const {	return (int *)(Data + ofs); }
	int *Jump2PC (uint32_t jumpPoint) const { return Ofs2PC(JumpPoints[jumpPoint]); }
	ACSFormat GetFormat() const { return Format; }
	ScriptFunction *GetFunction (int funcnum, FBehavior *&module) const;
	int GetArrayVal (int arraynum, int index) const;
	void SetArrayVal (int arraynum, int index, int value);
	inline bool CopyStringToArray(int arraynum, int index, int maxLength, const char * string);

	int FindFunctionName (const char *funcname) const;
	int FindMapVarName (const char *varname) const;
	int FindMapArray (const char *arrayname) const;
	int GetLibraryID () const { return LibraryID; }
	int *GetScriptAddress (const ScriptPtr *ptr) const { return (int *)(ptr->Address + Data); }
	int GetScriptIndex (const ScriptPtr *ptr) const { ptrdiff_t index = ptr - Scripts; return index >= NumScripts ? -1 : (int)index; }
	ScriptPtr *GetScriptPtr(int index) const { return index >= 0 && index < NumScripts ? &Scripts[index] : NULL; }
	int GetLumpNum() const { return LumpNum; }
	int GetDataSize() const { return DataSize; }
	const char *GetModuleName() const { return ModuleName; }
	ACSProfileInfo *GetFunctionProfileData(int index) { return index >= 0 && index < NumFunctions ? &FunctionProfileData[index] : NULL; }
	ACSProfileInfo *GetFunctionProfileData(ScriptFunction *func) { return GetFunctionProfileData((int)(func - (ScriptFunction *)Functions)); }
	const char *LookupString (uint32_t index) const;

	int32_t *MapVars[NUM_MAPVARS];

	static FBehavior *StaticLoadModule (int lumpnum, FileReader * fr=NULL, int len=0);
	static void StaticLoadDefaultModules ();
	static void StaticUnloadModules ();
	static bool StaticCheckAllGood ();
	static FBehavior *StaticGetModule (int lib);
	static void StaticSerializeModuleStates (FSerializer &arc);
	static void StaticMarkLevelVarStrings();
	static void StaticLockLevelVarStrings();
	static void StaticUnlockLevelVarStrings();

	static const ScriptPtr *StaticFindScript (int script, FBehavior *&module);
	static const char *StaticLookupString (uint32_t index);
	static void StaticStartTypedScripts (uint16_t type, AActor *activator, bool always, int arg1=0, bool runNow=false);
	static void StaticStopMyScripts (AActor *actor);

private:
	struct ArrayInfo;

	ACSFormat Format;

	int LumpNum;
	uint8_t *Data;
	int DataSize;
	uint8_t *Chunks;
	ScriptPtr *Scripts;
	int NumScripts;
	ScriptFunction *Functions;
	ACSProfileInfo *FunctionProfileData;
	int NumFunctions;
	ArrayInfo *ArrayStore;
	int NumArrays;
	ArrayInfo **Arrays;
	int NumTotalArrays;
	uint32_t StringTable;
	int32_t MapVarStore[NUM_MAPVARS];
	TArray<FBehavior *> Imports;
	uint32_t LibraryID;
	char ModuleName[9];
	TArray<int> JumpPoints;

	static TArray<FBehavior *> StaticModules;

	void LoadScriptsDirectory ();

	static int SortScripts (const void *a, const void *b);
	void UnencryptStrings ();
	void UnescapeStringTable(uint8_t *chunkstart, uint8_t *datastart, bool haspadding);
	int FindStringInChunk (uint32_t *chunk, const char *varname) const;

	void SerializeVars (FSerializer &arc);
	void SerializeVarSet (FSerializer &arc, int32_t *vars, int max);

	void MarkMapVarStrings() const;
	void LockMapVarStrings() const;
	void UnlockMapVarStrings() const;

	friend void ArrangeScriptProfiles(TArray<ProfileCollector> &profiles);
	friend void ArrangeFunctionProfiles(TArray<ProfileCollector> &profiles);
};

class DLevelScript : public DObject
{
	DECLARE_CLASS (DLevelScript, DObject)
	HAS_OBJECT_POINTERS
public:


	enum EScriptState
	{
		SCRIPT_Running,
		SCRIPT_Suspended,
		SCRIPT_Delayed,
		SCRIPT_TagWait,
		SCRIPT_PolyWait,
		SCRIPT_ScriptWaitPre,
		SCRIPT_ScriptWait,
		SCRIPT_PleaseRemove,
		SCRIPT_DivideBy0,
		SCRIPT_ModulusBy0,
	};

	DLevelScript (AActor *who, line_t *where, int num, const ScriptPtr *code, FBehavior *module,
		const int *args, int argcount, int flags);
	~DLevelScript ();

	void Serialize(FSerializer &arc);
	int RunScript ();

	inline void SetState (EScriptState newstate) { state = newstate; }
	inline EScriptState GetState () { return state; }

	DLevelScript *GetNext() const { return next; }

	void MarkLocalVarStrings() const
	{
		GlobalACSStrings.MarkStringArray(&Localvars[0], Localvars.Size());
	}
	void LockLocalVarStrings() const
	{
		GlobalACSStrings.LockStringArray(&Localvars[0], Localvars.Size());
	}
	void UnlockLocalVarStrings() const
	{
		GlobalACSStrings.UnlockStringArray(&Localvars[0], Localvars.Size());
	}

protected:
	DLevelScript	*next, *prev;
	int				script;
	TArray<int32_t>	Localvars;
	int				*pc;
	EScriptState	state;
	int				statedata;
	TObjPtr<AActor*>	activator;
	line_t			*activationline;
	bool			backSide;
	FFont			*activefont;
	int				hudwidth, hudheight;
	int				ClipRectLeft, ClipRectTop, ClipRectWidth, ClipRectHeight;
	int				WrapWidth;
	bool			HandleAspect;
	FBehavior	    *activeBehavior;
	int				InModuleScriptNumber;

	void Link ();
	void Unlink ();
	void PutLast ();
	void PutFirst ();
	static int Random (int min, int max);
	static int ThingCount (int type, int stringid, int tid, int tag);
	static void ChangeFlat (int tag, int name, bool floorOrCeiling);
	static int CountPlayers ();
	static void SetLineTexture (int lineid, int side, int position, int name);
	static int DoSpawn (int type, const DVector3 &pos, int tid, DAngle angle, bool force);
	static int DoSpawn(int type, int x, int y, int z, int tid, int angle, bool force);
	static bool DoCheckActorTexture(int tid, AActor *activator, int string, bool floor);
	int DoSpawnSpot (int type, int spot, int tid, int angle, bool forced);
	int DoSpawnSpotFacing (int type, int spot, int tid, bool forced);
	int DoClassifyActor (int tid);
	int CallFunction(int argCount, int funcIndex, int32_t *args);

	void DoFadeTo (int r, int g, int b, int a, int time);
	void DoFadeRange (int r1, int g1, int b1, int a1,
		int r2, int g2, int b2, int a2, int time);
	void DoSetFont (int fontnum);
	void SetActorProperty (int tid, int property, int value);
	void DoSetActorProperty (AActor *actor, int property, int value);
	int GetActorProperty (int tid, int property);
	int CheckActorProperty (int tid, int property, int value);
	int GetPlayerInput (int playernum, int inputnum);

	int LineFromID(int id);
	int SideFromID(int id, int side);

private:
	DLevelScript ();

	friend class DACSThinker;
};

class DACSThinker : public DThinker
{
	DECLARE_CLASS (DACSThinker, DThinker)
	HAS_OBJECT_POINTERS
public:
	DACSThinker ();
	~DACSThinker ();

	void Serialize(FSerializer &arc);
	void Tick ();

	typedef TMap<int, DLevelScript *> ScriptMap;
	ScriptMap RunningScripts;	// Array of all synchronous scripts
	static TObjPtr<DACSThinker*> ActiveThinker;

	void DumpScriptStatus();
	void StopScriptsFor (AActor *actor);

private:
	DLevelScript *LastScript;
	DLevelScript *Scripts;				// List of all running scripts

	friend class DLevelScript;
	friend class FBehavior;
};

// The structure used to control scripts between maps
struct acsdefered_t
{
	enum EType
	{
		defexecute,
		defexealways,
		defsuspend,
		defterminate
	} type;
	int script;
	int args[3];
	int playernum;
};

FSerializer &Serialize(FSerializer &arc, const char *key, acsdefered_t &defer, acsdefered_t *def);

#endif //__P_ACS_H__
