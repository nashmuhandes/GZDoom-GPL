/*
** info.cpp
** Keeps track of available actors and their states
**
**---------------------------------------------------------------------------
** Copyright 1998-2006 Randy Heit
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
** This is completely different from Doom's info.c.
**
*/


#include "doomstat.h"
#include "info.h"
#include "m_fixed.h"
#include "c_dispatch.h"
#include "d_net.h"
#include "v_text.h"

#include "gi.h"
#include "actor.h"
#include "r_state.h"
#include "i_system.h"
#include "p_local.h"
#include "templates.h"
#include "cmdlib.h"
#include "g_level.h"
#include "stats.h"
#include "thingdef.h"
#include "d_player.h"
#include "doomerrors.h"
#include "events.h"

extern void LoadActors ();
extern void InitBotStuff();
extern void ClearStrifeTypes();

TArray<PClassActor *> PClassActor::AllActorClasses;
FRandom FState::pr_statetics("StateTics");

cycle_t ActionCycles;

void FState::SetAction(const char *name)
{
	ActionFunc = FindVMFunction(RUNTIME_CLASS(AActor), name);
}

bool FState::CallAction(AActor *self, AActor *stateowner, FStateParamInfo *info, FState **stateret)
{
	if (ActionFunc != NULL)
	{
		ActionCycles.Clock();

		VMValue params[3] = { self, stateowner, VMValue(info, ATAG_GENERIC) };
		// If the function returns a state, store it at *stateret.
		// If it doesn't return a state but stateret is non-NULL, we need
		// to set *stateret to NULL.
		if (stateret != NULL)
		{
			*stateret = NULL;
			if (ActionFunc->Proto == NULL ||
				ActionFunc->Proto->ReturnTypes.Size() == 0 ||
				ActionFunc->Proto->ReturnTypes[0] != TypeState)
			{
				stateret = NULL;
			}
		}
		try
		{
			if (stateret == NULL)
			{
				GlobalVMStack.Call(ActionFunc, params, ActionFunc->ImplicitArgs, NULL, 0, NULL);
			}
			else
			{
				VMReturn ret;
				ret.PointerAt((void **)stateret);
				GlobalVMStack.Call(ActionFunc, params, ActionFunc->ImplicitArgs, &ret, 1, NULL);
			}
		}
		catch (CVMAbortException &err)
		{
			err.MaybePrintMessage();
			auto owner = FState::StaticFindStateOwner(this);
			int offs = int(this - owner->OwnedStates);
			const char *callinfo = "";
			if (info != nullptr && info->mStateType == STATE_Psprite)
			{
				if (stateowner->IsKindOf(NAME_Weapon) && stateowner != self) callinfo = "weapon ";
				else callinfo = "overlay ";
			}
			err.stacktrace.AppendFormat("Called from %sstate %s.%d in %s\n", callinfo, owner->TypeName.GetChars(), offs, stateowner->GetClass()->TypeName.GetChars());
			throw;
			throw;
		}

		ActionCycles.Unclock();
		return true;
	}
	else
	{
		return false;
	}
}

//==========================================================================
//
//
//==========================================================================

int GetSpriteIndex(const char * spritename, bool add)
{
	static char lastsprite[5];
	static int lastindex;

	// Make sure that the string is upper case and 4 characters long
	char upper[5]={0,0,0,0,0};
	for (int i = 0; spritename[i] != 0 && i < 4; i++)
	{
		upper[i] = toupper (spritename[i]);
	}

	// cache the name so if the next one is the same the function doesn't have to perform a search.
	if (!strcmp(upper, lastsprite))
	{
		return lastindex;
	}
	strcpy(lastsprite, upper);

	for (unsigned i = 0; i < sprites.Size (); ++i)
	{
		if (strcmp (sprites[i].name, upper) == 0)
		{
			return (lastindex = (int)i);
		}
	}
	if (!add)
	{
		return (lastindex = -1);
	}
	spritedef_t temp;
	strcpy (temp.name, upper);
	temp.numframes = 0;
	temp.spriteframes = 0;
	return (lastindex = (int)sprites.Push (temp));
}

DEFINE_ACTION_FUNCTION(AActor, GetSpriteIndex)
{
	PARAM_PROLOGUE;
	PARAM_NAME(sprt);
	ACTION_RETURN_INT(GetSpriteIndex(sprt.GetChars(), false));
}

IMPLEMENT_CLASS(PClassActor, false, false)

//==========================================================================
//
// PClassActor :: StaticInit										STATIC
//
//==========================================================================

void PClassActor::StaticInit()
{
	sprites.Clear();
	if (sprites.Size() == 0)
	{
		spritedef_t temp;

		// Sprite 0 is always TNT1
		memcpy (temp.name, "TNT1", 5);
		temp.numframes = 0;
		temp.spriteframes = 0;
		sprites.Push (temp);

		// Sprite 1 is always ----
		memcpy (temp.name, "----", 5);
		sprites.Push (temp);

		// Sprite 2 is always ####
		memcpy (temp.name, "####", 5);
		sprites.Push (temp);
	}

	if (!batchrun) Printf ("LoadActors: Load actor definitions.\n");
	ClearStrifeTypes();
	LoadActors ();
	InitBotStuff();

	// reinit GLOBAL static stuff from gameinfo, once classes are loaded.
	E_InitStaticHandlers(false);
}

//==========================================================================
//
// PClassActor :: StaticSetActorNums								STATIC
//
// Called after Dehacked patches are applied
//
//==========================================================================

void PClassActor::StaticSetActorNums()
{
	for (unsigned int i = 0; i < PClassActor::AllActorClasses.Size(); ++i)
	{
		static_cast<PClassActor *>(PClassActor::AllActorClasses[i])->RegisterIDs();
	}
}

//==========================================================================
//
// PClassActor Constructor
//
//==========================================================================

PClassActor::PClassActor()
{
	GameFilter = GAME_Any;
	SpawnID = 0;
	DoomEdNum = -1;
	OwnedStates = NULL;
	NumOwnedStates = 0;
	Replacement = NULL;
	Replacee = NULL;
	StateList = NULL;
	DamageFactors = NULL;
	PainChances = NULL;

	DropItems = NULL;
	// Record this in the master list.
	AllActorClasses.Push(this);
}

//==========================================================================
//
// PClassActor Destructor
//
//==========================================================================

PClassActor::~PClassActor()
{
	if (OwnedStates != NULL)
	{
		delete[] OwnedStates;
	}
	if (DamageFactors != NULL)
	{
		delete DamageFactors;
	}
	if (PainChances != NULL)
	{
		delete PainChances;
	}
	if (StateList != NULL)
	{
		StateList->Destroy();
		M_Free(StateList);
	}
}

//==========================================================================
//
// PClassActor :: Derive
//
//==========================================================================

void PClassActor::DeriveData(PClass *newclass)
{
	assert(newclass->IsKindOf(RUNTIME_CLASS(PClassActor)));
	PClassActor *newa = static_cast<PClassActor *>(newclass);

	newa->DefaultStateUsage = DefaultStateUsage;
	newa->distancecheck = distancecheck;

	newa->DropItems = DropItems;

	newa->VisibleToPlayerClass = VisibleToPlayerClass;

	if (DamageFactors != NULL)
	{
		// copy damage factors from parent
		newa->DamageFactors = new DmgFactors;
		*newa->DamageFactors = *DamageFactors;
	}
	if (PainChances != NULL)
	{
		// copy pain chances from parent
		newa->PainChances = new PainChanceList;
		*newa->PainChances = *PainChances;
	}

	// Inventory stuff
	newa->ForbiddenToPlayerClass = ForbiddenToPlayerClass;
	newa->RestrictedToPlayerClass = RestrictedToPlayerClass;

	newa->DisplayName = DisplayName;
}

//==========================================================================
//
// PClassActor :: SetReplacement
//
// Sets as a replacement class for another class.
//
//==========================================================================

bool PClassActor::SetReplacement(FName replaceName)
{
	// Check for "replaces"
	if (replaceName != NAME_None)
	{
		// Get actor name
		PClassActor *replacee = PClass::FindActor(replaceName);

		if (replacee == nullptr)
		{
			return false;
		}
		if (replacee != nullptr)
		{
			replacee->Replacement = this;
			Replacee = replacee;
		}
	}
	return true;
}

//==========================================================================
//
// PClassActor :: SetDropItems
//
// Sets a new drop item list
//
//==========================================================================

void PClassActor::SetDropItems(FDropItem *drops)
{
	DropItems = drops;
}


//==========================================================================
//
// PClassActor :: Finalize
//
// Installs the parsed states and does some sanity checking
//
//==========================================================================

void AActor::Finalize(FStateDefinitions &statedef)
{
	AActor *defaults = this;

	try
	{
		statedef.FinishStates(GetClass(), defaults);
	}
	catch (CRecoverableError &)
	{
		statedef.MakeStateDefines(NULL);
		throw;
	}
	statedef.InstallStates(GetClass(), defaults);
	statedef.MakeStateDefines(NULL);
}

//==========================================================================
//
// PClassActor :: RegisterIDs
//
// Registers this class's SpawnID and DoomEdNum in the appropriate tables.
//
//==========================================================================

void PClassActor::RegisterIDs()
{
	PClassActor *cls = PClass::FindActor(TypeName);

	if (cls == NULL)
	{
		Printf(TEXTCOLOR_RED"The actor '%s' has been hidden by a non-actor of the same name\n", TypeName.GetChars());
		return;
	}

	// Conversation IDs have never been filtered by game so we cannot start doing that.
	if (ConversationID > 0)
	{
		StrifeTypes[ConversationID] = cls;
		if (cls != this) 
		{
			Printf(TEXTCOLOR_RED"Conversation ID %d refers to hidden class type '%s'\n", SpawnID, cls->TypeName.GetChars());
		}
	}
	if (GameFilter == GAME_Any || (GameFilter & gameinfo.gametype))
	{
		if (SpawnID > 0)
		{
			SpawnableThings[SpawnID] = cls;
			if (cls != this) 
			{
				Printf(TEXTCOLOR_RED"Spawn ID %d refers to hidden class type '%s'\n", SpawnID, cls->TypeName.GetChars());
			}
		}
		if (DoomEdNum != -1)
		{
			FDoomEdEntry *oldent = DoomEdMap.CheckKey(DoomEdNum);
			if (oldent != NULL && oldent->Special == -2)
			{
				Printf(TEXTCOLOR_RED"Editor number %d defined twice for classes '%s' and '%s'\n", DoomEdNum, cls->TypeName.GetChars(), oldent->Type->TypeName.GetChars());
			}
			FDoomEdEntry ent;
			memset(&ent, 0, sizeof(ent));
			ent.Type = cls;
			ent.Special = -2;	// use -2 instead of -1 so that we can recognize DECORATE defined entries and print a warning message if duplicates occur.
			DoomEdMap.Insert(DoomEdNum, ent);
			if (cls != this) 
			{
				Printf(TEXTCOLOR_RED"Editor number %d refers to hidden class type '%s'\n", DoomEdNum, cls->TypeName.GetChars());
			}
		}
	}
}

//==========================================================================
//
// PClassActor :: GetReplacement
//
//==========================================================================

PClassActor *PClassActor::GetReplacement(bool lookskill)
{
	FName skillrepname;
	
	if (lookskill && AllSkills.Size() > (unsigned)gameskill)
	{
		skillrepname = AllSkills[gameskill].GetReplacement(TypeName);
		if (skillrepname != NAME_None && PClass::FindClass(skillrepname) == NULL)
		{
			Printf("Warning: incorrect actor name in definition of skill %s: \n"
				   "class %s is replaced by non-existent class %s\n"
				   "Skill replacement will be ignored for this actor.\n", 
				   AllSkills[gameskill].Name.GetChars(), 
				   TypeName.GetChars(), skillrepname.GetChars());
			AllSkills[gameskill].SetReplacement(TypeName, NAME_None);
			AllSkills[gameskill].SetReplacedBy(skillrepname, NAME_None);
			lookskill = false; skillrepname = NAME_None;
		}
	}
	if (Replacement == NULL && (!lookskill || skillrepname == NAME_None))
	{
		return this;
	}
	// The Replacement field is temporarily NULLed to prevent
	// potential infinite recursion.
	PClassActor *savedrep = Replacement;
	Replacement = NULL;
	PClassActor *rep = savedrep;
	// Handle skill-based replacement here. It has precedence on DECORATE replacement
	// in that the skill replacement is applied first, followed by DECORATE replacement
	// on the actor indicated by the skill replacement.
	if (lookskill && (skillrepname != NAME_None))
	{
		rep = PClass::FindActor(skillrepname);
	}
	// Now handle DECORATE replacement chain
	// Skill replacements are not recursive, contrarily to DECORATE replacements
	rep = rep->GetReplacement(false);
	// Reset the temporarily NULLed field
	Replacement = savedrep;
	return rep;
}

DEFINE_ACTION_FUNCTION(AActor, GetReplacement)
{
	PARAM_PROLOGUE;
	PARAM_POINTER(c, PClassActor);
	ACTION_RETURN_POINTER(c->GetReplacement());
}

//==========================================================================
//
// PClassActor :: GetReplacee
//
//==========================================================================

PClassActor *PClassActor::GetReplacee(bool lookskill)
{
	FName skillrepname;
	
	if (lookskill && AllSkills.Size() > (unsigned)gameskill)
	{
		skillrepname = AllSkills[gameskill].GetReplacedBy(TypeName);
		if (skillrepname != NAME_None && PClass::FindClass(skillrepname) == NULL)
		{
			Printf("Warning: incorrect actor name in definition of skill %s: \n"
				   "non-existent class %s is replaced by class %s\n"
				   "Skill replacement will be ignored for this actor.\n", 
				   AllSkills[gameskill].Name.GetChars(), 
				   skillrepname.GetChars(), TypeName.GetChars());
			AllSkills[gameskill].SetReplacedBy(TypeName, NAME_None);
			AllSkills[gameskill].SetReplacement(skillrepname, NAME_None);
			lookskill = false; 
		}
	}
	if (Replacee == NULL && (!lookskill || skillrepname == NAME_None))
	{
		return this;
	}
	// The Replacee field is temporarily NULLed to prevent
	// potential infinite recursion.
	PClassActor *savedrep = Replacee;
	Replacee = NULL;
	PClassActor *rep = savedrep;
	if (lookskill && (skillrepname != NAME_None) && (PClass::FindClass(skillrepname) != NULL))
	{
		rep = PClass::FindActor(skillrepname);
	}
	rep = rep->GetReplacee(false);
	Replacee = savedrep;
	return rep;
}

DEFINE_ACTION_FUNCTION(AActor, GetReplacee)
{
	PARAM_PROLOGUE;
	PARAM_POINTER(c, PClassActor);
	ACTION_RETURN_POINTER(c->GetReplacee());
}

//==========================================================================
//
// PClassActor :: SetDamageFactor
//
//==========================================================================

void PClassActor::SetDamageFactor(FName type, double factor)
{
	if (DamageFactors == NULL)
	{
		DamageFactors = new DmgFactors;
	}
	DamageFactors->Insert(type, factor);
}

//==========================================================================
//
// PClassActor :: SetPainChance
//
//==========================================================================

void PClassActor::SetPainChance(FName type, int chance)
{
	if (chance >= 0) 
	{
		if (PainChances == NULL)
		{
			PainChances = new PainChanceList;
		}
		PainChances->Insert(type, MIN(chance, 256));
	}
	else if (PainChances != NULL)
	{
		PainChances->Remove(type);
	}
}

//==========================================================================
//
// PClassActor :: ReplaceClassRef
//
//==========================================================================

size_t PClassActor::PointerSubstitution(DObject *oldclass, DObject *newclass)
{
	auto changed = Super::PointerSubstitution(oldclass, newclass);
	for (unsigned i = 0; i < VisibleToPlayerClass.Size(); i++)
	{
		if (VisibleToPlayerClass[i] == oldclass)
		{
			VisibleToPlayerClass[i] = static_cast<PClassActor*>(newclass);
			changed++;
		}
	}

	for (unsigned i = 0; i < ForbiddenToPlayerClass.Size(); i++)
	{
		if (ForbiddenToPlayerClass[i] == oldclass)
		{
			ForbiddenToPlayerClass[i] = static_cast<PClassActor*>(newclass);
			changed++;
		}
	}
	for (unsigned i = 0; i < RestrictedToPlayerClass.Size(); i++)
	{
		if (RestrictedToPlayerClass[i] == oldclass)
		{
			RestrictedToPlayerClass[i] = static_cast<PClassActor*>(newclass);
			changed++;
		}
	}
	AInventory *def = dyn_cast<AInventory>((AActor*)Defaults);
	if (def != NULL)
	{
		if (def->PickupFlash == oldclass) def->PickupFlash = static_cast<PClassActor *>(newclass);
	}

	return changed;
}

//==========================================================================
//
// DmgFactors :: CheckFactor
//
// Checks for the existance of a certain damage type. If that type does not
// exist, the damage factor for type 'None' will be returned, if present.
//
//==========================================================================

int DmgFactors::Apply(FName type, int damage)
{
	auto pdf = CheckKey(type);
	if (pdf == NULL && type != NAME_None)
	{
		pdf = CheckKey(NAME_None);
	}
	if (!pdf) return damage;
	return int(damage * *pdf);
}


static void SummonActor (int command, int command2, FCommandLine argv)
{
	if (CheckCheatmode ())
		return;

	if (argv.argc() > 1)
	{
		PClassActor *type = PClass::FindActor(argv[1]);
		if (type == NULL)
		{
			Printf ("Unknown actor '%s'\n", argv[1]);
			return;
		}
		Net_WriteByte (argv.argc() > 2 ? command2 : command);
		Net_WriteString (type->TypeName.GetChars());

		if (argv.argc () > 2)
		{
			Net_WriteWord (atoi (argv[2])); // angle
			Net_WriteWord ((argv.argc() > 3) ? atoi(argv[3]) : 0); // TID
			Net_WriteByte ((argv.argc() > 4) ? atoi(argv[4]) : 0); // special
			for (int i = 5; i < 10; i++)
			{ // args[5]
				Net_WriteLong((i < argv.argc()) ? atoi(argv[i]) : 0);
			}
		}
	}
}

CCMD (summon)
{
	SummonActor (DEM_SUMMON, DEM_SUMMON2, argv);
}

CCMD (summonfriend)
{
	SummonActor (DEM_SUMMONFRIEND, DEM_SUMMONFRIEND2, argv);
}

CCMD (summonmbf)
{
	SummonActor (DEM_SUMMONMBF, DEM_SUMMONFRIEND2, argv);
}

CCMD (summonfoe)
{
	SummonActor (DEM_SUMMONFOE, DEM_SUMMONFOE2, argv);
}


// Damage type defaults / global settings

TMap<FName, DamageTypeDefinition> GlobalDamageDefinitions;

void DamageTypeDefinition::Apply(FName type) 
{ 
	GlobalDamageDefinitions[type] = *this; 
}

DamageTypeDefinition *DamageTypeDefinition::Get(FName type) 
{ 
	return GlobalDamageDefinitions.CheckKey(type); 
}

bool DamageTypeDefinition::IgnoreArmor(FName type)
{ 
	DamageTypeDefinition *dtd = Get(type);
	if (dtd) return dtd->NoArmor;
	return false;
}

DEFINE_ACTION_FUNCTION(_DamageTypeDefinition, IgnoreArmor)
{
	PARAM_PROLOGUE;
	PARAM_NAME(type);
	ACTION_RETURN_BOOL(DamageTypeDefinition::IgnoreArmor(type));
}

FString DamageTypeDefinition::GetObituary(FName type)
{
	DamageTypeDefinition *dtd = Get(type);
	if (dtd) return dtd->Obituary;
	return "";
}


//==========================================================================
//
// DamageTypeDefinition :: ApplyMobjDamageFactor
//
// Calculates mobj damage based on original damage, defined damage factors
// and damage type.
//
// If the specific damage type is not defined, the damage factor for
// type 'None' will be used (with 1.0 as a default value).
//
// Globally declared damage types may override or multiply the damage
// factor when 'None' is used as a fallback in this function.
//
//==========================================================================

double DamageTypeDefinition::GetMobjDamageFactor(FName type, DmgFactors const * const factors)
{
	if (factors)
	{
		// If the actor has named damage factors, look for a specific factor

		auto pdf = factors->CheckKey(type);
		if (pdf) return *pdf; // type specific damage type
		
		// If this was nonspecific damage, don't fall back to nonspecific search
		if (type == NAME_None) return 1.;
	}
	
	// If this was nonspecific damage, don't fall back to nonspecific search
	else if (type == NAME_None) 
	{ 
		return 1.;
	}
	else
	{
		// Normal is unsupplied / 1.0, so there's no difference between modifying and overriding
		DamageTypeDefinition *dtd = Get(type);
		return dtd ? dtd->DefaultFactor : 1.;
	}
	
	{
		auto pdf  = factors->CheckKey(NAME_None);
		DamageTypeDefinition *dtd = Get(type);
		// Here we are looking for modifications to untyped damage
		// If the calling actor defines untyped damage factor, that is contained in "pdf".
		if (pdf) // normal damage available
		{
			if (dtd)
			{
				if (dtd->ReplaceFactor) return dtd->DefaultFactor; // use default instead of untyped factor
				return *pdf * dtd->DefaultFactor; // use default as modification of untyped factor
			}
			return *pdf; // there was no default, so actor default is used
		}
		else if (dtd)
		{
			return dtd->DefaultFactor; // implicit untyped factor 1.0 does not need to be applied/replaced explicitly
		}
	}
	return 1.;
}

int DamageTypeDefinition::ApplyMobjDamageFactor(int damage, FName type, DmgFactors const * const factors)
{
	double factor = GetMobjDamageFactor(type, factors);
	return int(damage * factor);
}

//==========================================================================
//
// Reads a damage definition
//
//==========================================================================

void FMapInfoParser::ParseDamageDefinition()
{
	sc.MustGetString();
	FName damageType = sc.String;

	DamageTypeDefinition dtd;

	ParseOpenBrace();
	while (sc.MustGetAnyToken(), sc.TokenType != '}')
	{
		if (sc.Compare("FACTOR"))
		{
			sc.MustGetStringName("=");
			sc.MustGetFloat();
			dtd.DefaultFactor = sc.Float;
			if (dtd.DefaultFactor == 0) dtd.ReplaceFactor = true;
		}
		else if (sc.Compare("OBITUARY"))
		{
			sc.MustGetStringName("=");
			sc.MustGetString();
			dtd.Obituary = sc.String;
		}
		else if (sc.Compare("REPLACEFACTOR"))
		{
			dtd.ReplaceFactor = true;
		}
		else if (sc.Compare("NOARMOR"))
		{
			dtd.NoArmor = true;
		}
		else
		{
			sc.ScriptError("Unexpected data (%s) in damagetype definition.", sc.String);
		}
	}

	dtd.Apply(damageType);
}
