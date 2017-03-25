// Emacs style mode select	 -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// $Log:$
//
// DESCRIPTION:
//		Player related stuff.
//		Bobbing POV/weapon, movement.
//		Pending weapon.
//
//-----------------------------------------------------------------------------

#include "templates.h"
#include "doomdef.h"
#include "d_event.h"
#include "p_local.h"
#include "doomstat.h"
#include "s_sound.h"
#include "i_system.h"
#include "gi.h"
#include "m_random.h"
#include "p_pspr.h"
#include "p_enemy.h"
#include "s_sound.h"
#include "a_sharedglobal.h"
#include "a_keys.h"
#include "statnums.h"
#include "v_palette.h"
#include "v_video.h"
#include "w_wad.h"
#include "cmdlib.h"
#include "sbar.h"
#include "intermission/intermission.h"
#include "c_console.h"
#include "doomdef.h"
#include "c_dispatch.h"
#include "tarray.h"
#include "g_level.h"
#include "d_net.h"
#include "gstrings.h"
#include "serializer.h"
#include "r_renderer.h"
#include "d_player.h"
#include "r_utility.h"
#include "p_blockmap.h"
#include "a_morph.h"
#include "p_spec.h"
#include "virtual.h"
#include "g_levellocals.h"
#include "r_data/r_translate.h"

static FRandom pr_skullpop ("SkullPop");

// [RH] # of ticks to complete a turn180
#define TURN180_TICKS	((TICRATE / 4) + 1)

// [SP] Allows respawn in single player
CVAR(Bool, sv_singleplayerrespawn, false, CVAR_SERVERINFO | CVAR_LATCH)

// Variables for prediction
CVAR (Bool, cl_noprediction, false, CVAR_ARCHIVE|CVAR_GLOBALCONFIG)
CVAR(Bool, cl_predict_specials, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CUSTOM_CVAR(Float, cl_predict_lerpscale, 0.05f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	P_PredictionLerpReset();
}
CUSTOM_CVAR(Float, cl_predict_lerpthreshold, 2.00f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.1f)
		self = 0.1f;
	P_PredictionLerpReset();
}

ColorSetList ColorSets;
PainFlashList PainFlashes;

// [Nash] FOV cvar setting
CUSTOM_CVAR(Float, fov, 90.f, CVAR_ARCHIVE | CVAR_USERINFO | CVAR_NOINITCALL)
{
	player_t *p = &players[consoleplayer];
	p->SetFOV(fov);
}

struct PredictPos
{
	int gametic;
	DVector3 pos;
	DRotator angles;
} static PredictionLerpFrom, PredictionLerpResult, PredictionLast;
static int PredictionLerptics;

static player_t PredictionPlayerBackup;
static uint8_t PredictionActorBackup[sizeof(APlayerPawn)];
static TArray<AActor *> PredictionSectorListBackup;

static TArray<sector_t *> PredictionTouchingSectorsBackup;
static TArray<msecnode_t *> PredictionTouchingSectors_sprev_Backup;

static TArray<sector_t *> PredictionRenderSectorsBackup;
static TArray<msecnode_t *> PredictionRenderSectors_sprev_Backup;

static TArray<sector_t *> PredictionPortalSectorsBackup;
static TArray<msecnode_t *> PredictionPortalSectors_sprev_Backup;

static TArray<FLinePortal *> PredictionPortalLinesBackup;
static TArray<portnode_t *> PredictionPortalLines_sprev_Backup;

// [GRB] Custom player classes
TArray<FPlayerClass> PlayerClasses;

FPlayerClass::FPlayerClass ()
{
	Type = NULL;
	Flags = 0;
}

FPlayerClass::FPlayerClass (const FPlayerClass &other)
{
	Type = other.Type;
	Flags = other.Flags;
	Skins = other.Skins;
}

FPlayerClass::~FPlayerClass ()
{
}

bool FPlayerClass::CheckSkin (int skin)
{
	for (unsigned int i = 0; i < Skins.Size (); i++)
	{
		if (Skins[i] == skin)
			return true;
	}

	return false;
}

DEFINE_ACTION_FUNCTION(FPlayerClass, CheckSkin)
{
	PARAM_SELF_STRUCT_PROLOGUE(FPlayerClass);
	PARAM_INT(skin);
	ACTION_RETURN_BOOL(self->CheckSkin(skin));
}

//===========================================================================
//
// GetDisplayName
//
//===========================================================================

FString GetPrintableDisplayName(PClassActor *cls)
{ 
	// Fixme; This needs a decent way to access the string table without creating a mess.
	// [RH] ????
	return cls->DisplayName;
}

DEFINE_ACTION_FUNCTION(APlayerPawn, GetPrintableDisplayName)
{
	PARAM_PROLOGUE;
	PARAM_CLASS(type, AActor);
	ACTION_RETURN_STRING(type->DisplayName);
}

bool ValidatePlayerClass(PClassActor *ti, const char *name)
{
	if (ti == NULL)
	{
		Printf("Unknown player class '%s'\n", name);
		return false;
	}
	else if (!ti->IsDescendantOf(RUNTIME_CLASS(APlayerPawn)))
	{
		Printf("Invalid player class '%s'\n", name);
		return false;
	}
	else if (ti->DisplayName.IsEmpty())
	{
		Printf ("Missing displayname for player class '%s'\n", name);
		return false;
	}
	return true;
}

void SetupPlayerClasses ()
{
	FPlayerClass newclass;

	PlayerClasses.Clear();
	for (unsigned i = 0; i < gameinfo.PlayerClasses.Size(); i++)
	{
		PClassActor *cls = PClass::FindActor(gameinfo.PlayerClasses[i]);
		if (ValidatePlayerClass(cls, gameinfo.PlayerClasses[i]))
		{
			newclass.Flags = 0;
			newclass.Type = cls;
			if ((GetDefaultByType(cls)->flags6 & MF6_NOMENU))
			{
				newclass.Flags |= PCF_NOMENU;
			}
			PlayerClasses.Push(newclass);
		}
	}
}

CCMD (clearplayerclasses)
{
	if (ParsingKeyConf)
	{
		PlayerClasses.Clear ();
	}
}

CCMD (addplayerclass)
{
	if (ParsingKeyConf && argv.argc () > 1)
	{
		PClassActor *ti = PClass::FindActor(argv[1]);

		if (ValidatePlayerClass(ti, argv[1]))
		{
			FPlayerClass newclass;

			newclass.Type = ti;
			newclass.Flags = 0;

			int arg = 2;
			while (arg < argv.argc())
			{
				if (!stricmp (argv[arg], "nomenu"))
				{
					newclass.Flags |= PCF_NOMENU;
				}
				else
				{
					Printf ("Unknown flag '%s' for player class '%s'\n", argv[arg], argv[1]);
				}

				arg++;
			}
			PlayerClasses.Push (newclass);
		}
	}
}

CCMD (playerclasses)
{
	for (unsigned int i = 0; i < PlayerClasses.Size (); i++)
	{
		Printf ("%3d: Class = %s, Name = %s\n", i,
			PlayerClasses[i].Type->TypeName.GetChars(),
			PlayerClasses[i].Type->DisplayName.GetChars());
	}
}


//
// Movement.
//

// 16 pixels of bob
#define MAXBOB			16.

// The player_t constructor. Since LogText is not a POD, we cannot just
// memset it all to 0.
player_t::player_t()
: mo(0),
  playerstate(0),
  cls(0),
  DesiredFOV(0),
  FOV(0),
  viewz(0),
  viewheight(0),
  deltaviewheight(0),
  bob(0),
  Vel(0, 0),
  centering(0),
  turnticks(0),
  attackdown(0),
  usedown(0),
  oldbuttons(0),
  health(0),
  inventorytics(0),
  CurrentPlayerClass(0),
  fragcount(0),
  lastkilltime(0),
  multicount(0),
  spreecount(0),
  WeaponState(0),
  ReadyWeapon(0),
  PendingWeapon(0),
  psprites(0),
  cheats(0),
  timefreezer(0),
  refire(0),
  inconsistant(0),
  killcount(0),
  itemcount(0),
  secretcount(0),
  damagecount(0),
  bonuscount(0),
  hazardcount(0),
  poisoncount(0),
  poisoner(0),
  attacker(0),
  extralight(0),
  morphTics(0),
  MorphedPlayerClass(0),
  MorphStyle(0),
  MorphExitFlash(0),
  PremorphWeapon(0),
  chickenPeck(0),
  jumpTics(0),
  onground(0),
  respawn_time(0),
  camera(0),
  air_finished(0),
  MUSINFOactor(0),
  MUSINFOtics(-1),
  crouching(0),
  crouchdir(0),
  Bot(0),
  BlendR(0),
  BlendG(0),
  BlendB(0),
  BlendA(0),
  LogText(),
  crouchfactor(0),
  crouchoffset(0),
  crouchviewdelta(0),
  ConversationNPC(0),
  ConversationPC(0),
  ConversationNPCAngle(0.),
  ConversationFaceTalker(0)
{
	memset (&cmd, 0, sizeof(cmd));
	memset (frags, 0, sizeof(frags));
}

player_t::~player_t()
{
	DestroyPSprites();
}

player_t &player_t::operator=(const player_t &p)
{
	mo = p.mo;
	playerstate = p.playerstate;
	cmd = p.cmd;
	original_cmd = p.original_cmd;
	original_oldbuttons = p.original_oldbuttons;
	// Intentionally not copying userinfo!
	cls = p.cls;
	DesiredFOV = p.DesiredFOV;
	FOV = p.FOV;
	viewz = p.viewz;
	viewheight = p.viewheight;
	deltaviewheight = p.deltaviewheight;
	bob = p.bob;
	Vel = p.Vel;
	centering = p.centering;
	turnticks = p.turnticks;
	attackdown = p.attackdown;
	usedown = p.usedown;
	oldbuttons = p.oldbuttons;
	health = p.health;
	inventorytics = p.inventorytics;
	CurrentPlayerClass = p.CurrentPlayerClass;
	memcpy(frags, &p.frags, sizeof(frags));
	fragcount = p.fragcount;
	lastkilltime = p.lastkilltime;
	multicount = p.multicount;
	spreecount = p.spreecount;
	WeaponState = p.WeaponState;
	ReadyWeapon = p.ReadyWeapon;
	PendingWeapon = p.PendingWeapon;
	cheats = p.cheats;
	timefreezer = p.timefreezer;
	refire = p.refire;
	inconsistant = p.inconsistant;
	waiting = p.waiting;
	killcount = p.killcount;
	itemcount = p.itemcount;
	secretcount = p.secretcount;
	damagecount = p.damagecount;
	bonuscount = p.bonuscount;
	hazardcount = p.hazardcount;
	hazardtype = p.hazardtype;
	hazardinterval = p.hazardinterval;
	poisoncount = p.poisoncount;
	poisontype = p.poisontype;
	poisonpaintype = p.poisonpaintype;
	poisoner = p.poisoner;
	attacker = p.attacker;
	extralight = p.extralight;
	fixedcolormap = p.fixedcolormap;
	fixedlightlevel = p.fixedlightlevel;
	psprites = p.psprites;
	morphTics = p.morphTics;
	MorphedPlayerClass = p.MorphedPlayerClass;
	MorphStyle = p.MorphStyle;
	MorphExitFlash = p.MorphExitFlash;
	PremorphWeapon = p.PremorphWeapon;
	chickenPeck = p.chickenPeck;
	jumpTics = p.jumpTics;
	onground = p.onground;
	respawn_time = p.respawn_time;
	camera = p.camera;
	air_finished = p.air_finished;
	LastDamageType = p.LastDamageType;
	Bot = p.Bot;
	settings_controller = p.settings_controller;
	BlendR = p.BlendR;
	BlendG = p.BlendG;
	BlendB = p.BlendB;
	BlendA = p.BlendA;
	LogText = p.LogText;
	MinPitch = p.MinPitch;
	MaxPitch = p.MaxPitch;
	crouching = p.crouching;
	crouchdir = p.crouchdir;
	crouchfactor = p.crouchfactor;
	crouchoffset = p.crouchoffset;
	crouchviewdelta = p.crouchviewdelta;
	weapons = p.weapons;
	ConversationNPC = p.ConversationNPC;
	ConversationPC = p.ConversationPC;
	ConversationNPCAngle = p.ConversationNPCAngle;
	ConversationFaceTalker = p.ConversationFaceTalker;
	MUSINFOactor = p.MUSINFOactor;
	MUSINFOtics = p.MUSINFOtics;
	return *this;
}

// This function supplements the pointer cleanup in dobject.cpp, because
// player_t is not derived from DObject. (I tried it, and DestroyScan was
// unable to properly determine the player object's type--possibly
// because it gets staticly allocated in an array.)
//
// This function checks all the DObject pointers in a player_t and NULLs any
// that match the pointer passed in. If you add any pointers that point to
// DObject (or a subclass), add them here too.

size_t player_t::FixPointers (const DObject *old, DObject *rep)
{
	APlayerPawn *replacement = static_cast<APlayerPawn *>(rep);
	size_t changed = 0;

	// The construct *& is used in several of these to avoid the read barriers
	// that would turn the pointer we want to check to NULL if the old object
	// is pending deletion.
	if (mo == old)					mo = replacement, changed++;
	if (*&poisoner == old)			poisoner = replacement, changed++;
	if (*&attacker == old)			attacker = replacement, changed++;
	if (*&camera == old)			camera = replacement, changed++;
	if (*&Bot == old)				Bot = static_cast<DBot *>(rep), changed++;
	if (ReadyWeapon == old)			ReadyWeapon = static_cast<AWeapon *>(rep), changed++;
	if (PendingWeapon == old)		PendingWeapon = static_cast<AWeapon *>(rep), changed++;
	if (*&PremorphWeapon == old)	PremorphWeapon = static_cast<AWeapon *>(rep), changed++;
	if (psprites == old)			psprites = static_cast<DPSprite *>(rep), changed++;
	if (*&ConversationNPC == old)	ConversationNPC = replacement, changed++;
	if (*&ConversationPC == old)	ConversationPC = replacement, changed++;
	if (*&MUSINFOactor == old)		MUSINFOactor = replacement, changed++;
	return changed;
}

size_t player_t::PropagateMark()
{
	GC::Mark(mo);
	GC::Mark(poisoner);
	GC::Mark(attacker);
	GC::Mark(camera);
	GC::Mark(Bot);
	GC::Mark(ReadyWeapon);
	GC::Mark(ConversationNPC);
	GC::Mark(ConversationPC);
	GC::Mark(MUSINFOactor);
	GC::Mark(PremorphWeapon);
	GC::Mark(psprites);
	if (PendingWeapon != WP_NOCHANGE)
	{
		GC::Mark(PendingWeapon);
	}
	return sizeof(*this);
}

void player_t::SetLogNumber (int num)
{
	char lumpname[16];
	int lumpnum;

	mysnprintf (lumpname, countof(lumpname), "LOG%d", num);
	lumpnum = Wads.CheckNumForName (lumpname);
	if (lumpnum == -1)
	{
		// Leave the log message alone if this one doesn't exist.
		//SetLogText (lumpname);
	}
	else
	{
		int length=Wads.LumpLength(lumpnum);
		char *data= new char[length+1];
		Wads.ReadLump (lumpnum, data);
		data[length]=0;
		SetLogText (data);
		delete[] data;
	}
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, SetLogNumber)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	PARAM_INT(log);
	self->SetLogNumber(log);
	return 0;
}

void player_t::SetLogText (const char *text)
{
	LogText = text;

	if (mo->CheckLocalView(consoleplayer))
	{
		// Print log text to console
		AddToConsole(-1, TEXTCOLOR_GOLD);
		AddToConsole(-1, LogText);
		AddToConsole(-1, "\n");
	}
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, SetLogText)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	PARAM_STRING(log);
	self->SetLogText(log);
	return 0;
}

int player_t::GetSpawnClass()
{
	const PClass * type = PlayerClasses[CurrentPlayerClass].Type;
	return static_cast<APlayerPawn*>(GetDefaultByType(type))->SpawnMask;
}

// [Nash] Set FOV
void player_t::SetFOV(float fov)
{
	player_t *p = &players[consoleplayer];
	if (p != nullptr && p->mo != nullptr)
	{
		if (dmflags & DF_NO_FOV)
		{
			if (consoleplayer == Net_Arbitrator)
			{
				Net_WriteByte(DEM_MYFOV);
			}
			else
			{
				Printf("A setting controller has disabled FOV changes.\n");
				return;
			}
		}
		else
		{
			Net_WriteByte(DEM_MYFOV);
		}
		Net_WriteByte((uint8_t)clamp<float>(fov, 5.f, 179.f));
	}
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, SetFOV)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	PARAM_FLOAT(fov);
	self->SetFOV((float)fov);
	return 0;
}

//===========================================================================
//
// EnumColorsets
//
// Only used by the menu so it doesn't really matter that it's a bit
// inefficient.
//
//===========================================================================

static int intcmp(const void *a, const void *b)
{
	return *(const int *)a - *(const int *)b;
}

void EnumColorSets(PClassActor *cls, TArray<int> *out)
{
	TArray<int> deleteds;

	out->Clear();
	for (int i = ColorSets.Size() - 1; i >= 0; i--)
	{
		if (std::get<0>(ColorSets[i])->IsAncestorOf(cls))
		{
			int v = std::get<1>(ColorSets[i]);
			if (out->Find(v) == out->Size() && deleteds.Find(v) == deleteds.Size())
			{
				if (std::get<2>(ColorSets[i]).Name == NAME_None) deleteds.Push(v);
				else out->Push(v);
			}
		}
	}
	qsort(&(*out)[0], out->Size(), sizeof(int), intcmp);
}

DEFINE_ACTION_FUNCTION(FPlayerClass, EnumColorSets)
{
	PARAM_SELF_STRUCT_PROLOGUE(FPlayerClass);
	PARAM_POINTER(out, TArray<int>);
	EnumColorSets(self->Type, out);
	return 0;
}

//==========================================================================
//
//
//==========================================================================

FPlayerColorSet *GetColorSet(PClassActor *cls, int setnum)
{
	for (int i = ColorSets.Size() - 1; i >= 0; i--)
	{
		if (std::get<1>(ColorSets[i]) == setnum &&
			std::get<0>(ColorSets[i])->IsAncestorOf(cls))
		{
			auto c = &std::get<2>(ColorSets[i]);
			return c->Name != NAME_None ? c : nullptr;
		}
	}
	return nullptr;
}

DEFINE_ACTION_FUNCTION(FPlayerClass, GetColorSetName)
{
	PARAM_SELF_STRUCT_PROLOGUE(FPlayerClass);
	PARAM_INT(setnum);
	auto p = GetColorSet(self->Type, setnum);
	ACTION_RETURN_INT(p ? p->Name.GetIndex() : 0);
}

//==========================================================================
//
//
//==========================================================================

bool player_t::GetPainFlash(FName type, PalEntry *color) const
{
	PClass *info = mo->GetClass();

	// go backwards through the list and return the first item with a 
	// matching damage type for an ancestor of our class. 
	// This will always return the best fit because any parent class
	// must be processed before its children.
	for (int i = PainFlashes.Size() - 1; i >= 0; i--)
	{
		if (std::get<1>(PainFlashes[i]) == type &&
			std::get<0>(PainFlashes[i])->IsAncestorOf(info))
		{
			*color = std::get<2>(PainFlashes[i]);
			return true;
		}
	}
	return false;
}

//===========================================================================
//
// player_t :: SendPitchLimits
//
// Ask the local player's renderer what pitch restrictions should be imposed
// and let everybody know. Only sends data for the consoleplayer, since the
// local player is the only one our data is valid for.
//
//===========================================================================

void player_t::SendPitchLimits() const
{
	if (this - players == consoleplayer)
	{
		Net_WriteByte(DEM_SETPITCHLIMIT);
		Net_WriteByte(Renderer->GetMaxViewPitch(false));	// up
		Net_WriteByte(Renderer->GetMaxViewPitch(true));		// down
	}
}


DEFINE_ACTION_FUNCTION(_PlayerInfo, GetUserName)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	ACTION_RETURN_STRING(self->userinfo.GetName());
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, GetNeverSwitch)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	ACTION_RETURN_BOOL(self->userinfo.GetNeverSwitch());
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, GetColor)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	ACTION_RETURN_INT(self->userinfo.GetColor());
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, GetColorSet)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	ACTION_RETURN_INT(self->userinfo.GetColorSet());
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, GetPlayerClassNum)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	ACTION_RETURN_INT(self->userinfo.GetPlayerClassNum());
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, GetSkin)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	ACTION_RETURN_INT(self->userinfo.GetSkin());
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, GetGender)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	ACTION_RETURN_INT(self->userinfo.GetGender());
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, GetAutoaim)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	ACTION_RETURN_FLOAT(self->userinfo.GetAutoaim());
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, GetTeam)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	ACTION_RETURN_INT(self->userinfo.GetTeam());
}


//===========================================================================
//
// APlayerPawn
//
//===========================================================================

IMPLEMENT_CLASS(APlayerPawn, false, true)

IMPLEMENT_POINTERS_START(APlayerPawn)
	IMPLEMENT_POINTER(InvFirst)
	IMPLEMENT_POINTER(InvSel)
IMPLEMENT_POINTERS_END

void APlayerPawn::Serialize(FSerializer &arc)
{
	Super::Serialize (arc);
	auto def = (APlayerPawn*)GetDefault();

	arc("jumpz", JumpZ, def->JumpZ)
		("maxhealth", MaxHealth, def->MaxHealth)
		("bonushealth", BonusHealth, def->BonusHealth)
		("runhealth", RunHealth, def->RunHealth)
		("spawnmask", SpawnMask, def->SpawnMask)
		("forwardmove1", ForwardMove1, def->ForwardMove1)
		("forwardmove2", ForwardMove2, def->ForwardMove2)
		("sidemove1", SideMove1, def->SideMove1)
		("sidemove2", SideMove2, def->SideMove2)
		("scoreicon", ScoreIcon, def->ScoreIcon)
		("invfirst", InvFirst)
		("invsel", InvSel)
		("morphweapon", MorphWeapon, def->MorphWeapon)
		("damagefade", DamageFade, def->DamageFade)
		("playerflags", PlayerFlags, def->PlayerFlags)
		("flechettetype", FlechetteType, def->FlechetteType)
		("gruntspeed", GruntSpeed, def->GruntSpeed)
		("fallingscreammin", FallingScreamMinSpeed, def->FallingScreamMinSpeed)
		("fallingscreammaxn", FallingScreamMaxSpeed, def->FallingScreamMaxSpeed)
		("userange", UseRange, def->UseRange)
		("aircapacity", AirCapacity, def->AirCapacity)
		("viewheight", ViewHeight, def->ViewHeight)
		("viewbob", ViewBob, def->ViewBob)
		("fullheight", FullHeight, def->FullHeight);
}

//===========================================================================
//
// APlayerPawn :: MarkPrecacheSounds
//
//===========================================================================

void APlayerPawn::MarkPrecacheSounds() const
{
	Super::MarkPrecacheSounds();
	S_MarkPlayerSounds(GetSoundClass());
}

//===========================================================================
//
// APlayerPawn :: BeginPlay
//
//===========================================================================

void APlayerPawn::BeginPlay ()
{
	Super::BeginPlay ();
	ChangeStatNum (STAT_PLAYER);
	FullHeight = Height;
	// Check whether a PWADs normal sprite is to be combined with the base WADs
	// crouch sprite. In such a case the sprites normally don't match and it is
	// best to disable the crouch sprite.
	if (crouchsprite > 0)
	{
		// This assumes that player sprites always exist in rotated form and
		// that the front view is always a separate sprite. So far this is
		// true for anything that exists.
		FString normspritename = sprites[SpawnState->sprite].name;
		FString crouchspritename = sprites[crouchsprite].name;

		int spritenorm = Wads.CheckNumForName(normspritename + "A1", ns_sprites);
		if (spritenorm==-1) 
		{
			spritenorm = Wads.CheckNumForName(normspritename + "A0", ns_sprites);
		}

		int spritecrouch = Wads.CheckNumForName(crouchspritename + "A1", ns_sprites);
		if (spritecrouch==-1) 
		{
			spritecrouch = Wads.CheckNumForName(crouchspritename + "A0", ns_sprites);
		}
		
		if (spritenorm==-1 || spritecrouch ==-1) 
		{
			// Sprites do not exist so it is best to disable the crouch sprite.
			crouchsprite = 0;
			return;
		}
	
		int wadnorm = Wads.GetLumpFile(spritenorm);
		int wadcrouch = Wads.GetLumpFile(spritenorm);
		
		if (wadnorm > FWadCollection::IWAD_FILENUM && wadcrouch <= FWadCollection::IWAD_FILENUM) 
		{
			// Question: Add an option / disable crouching or do what?
			crouchsprite = 0;
		}
	}
}

//===========================================================================
//
// APlayerPawn :: Tick
//
//===========================================================================

void APlayerPawn::Tick()
{
	if (player != NULL && player->mo == this && player->CanCrouch() && player->playerstate != PST_DEAD)
	{
		Height = FullHeight * player->crouchfactor;
	}
	else
	{
		if (health > 0) Height = FullHeight;
	}
	Super::Tick();
}

//===========================================================================
//
// APlayerPawn :: PostBeginPlay
//
//===========================================================================

void APlayerPawn::PostBeginPlay()
{
	Super::PostBeginPlay();
	SetupWeaponSlots();

	// Voodoo dolls: restore original floorz/ceilingz logic
	if (player == NULL || player->mo != this)
	{
		P_FindFloorCeiling(this, FFCF_ONLYSPAWNPOS|FFCF_NOPORTALS);
		SetZ(floorz);
		P_FindFloorCeiling(this, FFCF_ONLYSPAWNPOS);
	}
	else
	{
		player->SendPitchLimits();
	}
}

//===========================================================================
//
// APlayerPawn :: SetupWeaponSlots
//
// Sets up the default weapon slots for this player. If this is also the
// local player, determines local modifications and sends those across the
// network. Ignores voodoo dolls.
//
//===========================================================================

void APlayerPawn::SetupWeaponSlots()
{
	if (player != NULL && player->mo == this)
	{
		player->weapons.StandardSetup(GetClass());
		// If we're the local player, then there's a bit more work to do.
		// This also applies if we're a bot and this is the net arbitrator.
		if (player - players == consoleplayer ||
			(player->Bot != NULL && consoleplayer == Net_Arbitrator))
		{
			FWeaponSlots local_slots(player->weapons);
			if (player->Bot != NULL)
			{ // Bots only need weapons from KEYCONF, not INI modifications.
				P_PlaybackKeyConfWeapons(&local_slots);
			}
			else
			{
				local_slots.LocalSetup(GetClass());
			}
			local_slots.SendDifferences(int(player - players), player->weapons);
		}
	}
}

//===========================================================================
//
// APlayerPawn :: AddInventory
//
//===========================================================================

void APlayerPawn::AddInventory (AInventory *item)
{
	// Adding inventory to a voodoo doll should add it to the real player instead.
	if (player != NULL && player->mo != this && player->mo != NULL)
	{
		player->mo->AddInventory (item);
		return;
	}
	Super::AddInventory (item);

	// If nothing is selected, select this item.
	if (InvSel == NULL && (item->ItemFlags & IF_INVBAR))
	{
		InvSel = item;
	}
}

//===========================================================================
//
// APlayerPawn :: RemoveInventory
//
//===========================================================================

void APlayerPawn::RemoveInventory (AInventory *item)
{
	bool pickWeap = false;

	// Since voodoo dolls aren't supposed to have an inventory, there should be
	// no need to redirect them to the real player here as there is with AddInventory.

	// If the item removed is the selected one, select something else, either the next
	// item, if there is one, or the previous item.
	if (player != NULL)
	{
		if (InvSel == item)
		{
			InvSel = item->NextInv ();
			if (InvSel == NULL)
			{
				InvSel = item->PrevInv ();
			}
		}
		if (InvFirst == item)
		{
			InvFirst = item->NextInv ();
			if (InvFirst == NULL)
			{
				InvFirst = item->PrevInv ();
			}
		}
		if (item == player->PendingWeapon)
		{
			player->PendingWeapon = WP_NOCHANGE;
		}
		if (item == player->ReadyWeapon)
		{
			// If the current weapon is removed, clear the refire counter and pick a new one.
			pickWeap = true;
			player->ReadyWeapon = NULL;
			player->refire = 0;
		}
	}
	Super::RemoveInventory (item);
	if (pickWeap && player->mo == this && player->PendingWeapon == WP_NOCHANGE)
	{
		PickNewWeapon (NULL);
	}
}

//===========================================================================
//
// APlayerPawn :: UseInventory
//
//===========================================================================

bool APlayerPawn::UseInventory (AInventory *item)
{
	const PClass *itemtype = item->GetClass();

	if (player->cheats & CF_TOTALLYFROZEN)
	{ // You can't use items if you're totally frozen
		return false;
	}
	if ((level.flags2 & LEVEL2_FROZEN) && (player == NULL || player->timefreezer == 0))
	{
		// Time frozen
		return false;
	}

	if (!Super::UseInventory (item))
	{
		// Heretic and Hexen advance the inventory cursor if the use failed.
		// Should this behavior be retained?
		return false;
	}
	if (player == &players[consoleplayer])
	{
		S_Sound (this, CHAN_ITEM, item->UseSound, 1, ATTN_NORM);
		StatusBar->FlashItem (itemtype);
	}
	return true;
}

//===========================================================================
//
// APlayerPawn :: BestWeapon
//
// Returns the best weapon a player has, possibly restricted to a single
// type of ammo.
//
//===========================================================================

AWeapon *APlayerPawn::BestWeapon(PClassActor *ammotype)
{
	AWeapon *bestMatch = NULL;
	int bestOrder = INT_MAX;
	AInventory *item;
	AWeapon *weap;
	bool tomed = NULL != FindInventory (PClass::FindActor(NAME_PowerWeaponLevel2), true);

	// Find the best weapon the player has.
	for (item = Inventory; item != NULL; item = item->Inventory)
	{
		if (!item->IsKindOf(NAME_Weapon))
			continue;

		weap = static_cast<AWeapon *> (item);

		// Don't select it if it's worse than what was already found.
		if (weap->SelectionOrder > bestOrder)
			continue;

		// Don't select it if its primary fire doesn't use the desired ammo.
		if (ammotype != NULL &&
			(weap->Ammo1 == NULL ||
			 weap->Ammo1->GetClass() != ammotype))
			continue;

		// Don't select it if the Tome is active and this isn't the powered-up version.
		if (tomed && weap->SisterWeapon != NULL && weap->SisterWeapon->WeaponFlags & WIF_POWERED_UP)
			continue;

		// Don't select it if it's powered-up and the Tome is not active.
		if (!tomed && weap->WeaponFlags & WIF_POWERED_UP)
			continue;

		// Don't select it if there isn't enough ammo to use its primary fire.
		if (!(weap->WeaponFlags & WIF_AMMO_OPTIONAL) &&
			!weap->CheckAmmo (AWeapon::PrimaryFire, false))
			continue;

		// Don't select if if there isn't enough ammo as determined by the weapon's author.
		if (weap->MinSelAmmo1 > 0 && (weap->Ammo1 == NULL || weap->Ammo1->Amount < weap->MinSelAmmo1))
			continue;
		if (weap->MinSelAmmo2 > 0 && (weap->Ammo2 == NULL || weap->Ammo2->Amount < weap->MinSelAmmo2))
			continue;

		// This weapon is usable!
		bestOrder = weap->SelectionOrder;
		bestMatch = weap;
	}
	return bestMatch;
}

//===========================================================================
//
// APlayerPawn :: PickNewWeapon
//
// Picks a new weapon for this player. Used mostly for running out of ammo,
// but it also works when an ACS script explicitly takes the ready weapon
// away or the player picks up some ammo they had previously run out of.
//
//===========================================================================

AWeapon *APlayerPawn::PickNewWeapon(PClassActor *ammotype)
{
	AWeapon *best = BestWeapon (ammotype);

	if (best != NULL)
	{
		player->PendingWeapon = best;
		if (player->ReadyWeapon != NULL)
		{
			P_DropWeapon(player);
		}
		else if (player->PendingWeapon != WP_NOCHANGE)
		{
			P_BringUpWeapon (player);
		}
	}
	return best;
}


//===========================================================================
//
// APlayerPawn :: CheckWeaponSwitch
//
// Checks if weapons should be changed after picking up ammo
//
//===========================================================================

void APlayerPawn::CheckWeaponSwitch(PClassActor *ammotype)
{
	if (!player->userinfo.GetNeverSwitch() &&
		player->PendingWeapon == WP_NOCHANGE && 
		(player->ReadyWeapon == NULL ||
		 (player->ReadyWeapon->WeaponFlags & WIF_WIMPY_WEAPON)))
	{
		AWeapon *best = BestWeapon (ammotype);
		if (best != NULL && (player->ReadyWeapon == NULL ||
			best->SelectionOrder < player->ReadyWeapon->SelectionOrder))
		{
			player->PendingWeapon = best;
		}
	}
}

DEFINE_ACTION_FUNCTION(APlayerPawn, CheckWeaponSwitch)
{
	PARAM_SELF_PROLOGUE(APlayerPawn);
	PARAM_OBJECT(ammotype, PClassActor);
	self->CheckWeaponSwitch(ammotype);
	return 0;
}
//===========================================================================
//
// APlayerPawn :: GiveDeathmatchInventory
//
// Gives players items they should have in addition to their default
// inventory when playing deathmatch. (i.e. all keys)
//
//===========================================================================

void APlayerPawn::GiveDeathmatchInventory()
{
	for (unsigned int i = 0; i < PClassActor::AllActorClasses.Size(); ++i)
	{
		if (PClassActor::AllActorClasses[i]->IsDescendantOf (PClass::FindActor(NAME_Key)))
		{
			AInventory *key = (AInventory*)GetDefaultByType (PClassActor::AllActorClasses[i]);
			if (key->special1 != 0)
			{
				key = (AInventory*)Spawn(PClassActor::AllActorClasses[i]);
				if (!key->CallTryPickup (this))
				{
					key->Destroy ();
				}
			}
		}
	}
}

//===========================================================================
//
// APlayerPawn :: FilterCoopRespawnInventory
//
// When respawning in coop, this function is called to walk through the dead
// player's inventory and modify it according to the current game flags so
// that it can be transferred to the new live player. This player currently
// has the default inventory, and the oldplayer has the inventory at the time
// of death.
//
//===========================================================================

void APlayerPawn::FilterCoopRespawnInventory (APlayerPawn *oldplayer)
{
	AInventory *item, *next, *defitem;

	// If we're losing everything, this is really simple.
	if (dmflags & DF_COOP_LOSE_INVENTORY)
	{
		oldplayer->DestroyAllInventory();
		return;
	}

	if (dmflags &  (DF_COOP_LOSE_KEYS |
					DF_COOP_LOSE_WEAPONS |
					DF_COOP_LOSE_AMMO |
					DF_COOP_HALVE_AMMO |
					DF_COOP_LOSE_ARMOR |
					DF_COOP_LOSE_POWERUPS))
	{
		// Walk through the old player's inventory and destroy or modify
		// according to dmflags.
		for (item = oldplayer->Inventory; item != NULL; item = next)
		{
			next = item->Inventory;

			// If this item is part of the default inventory, we never want
			// to destroy it, although we might want to copy the default
			// inventory amount.
			defitem = FindInventory (item->GetClass());

			if ((dmflags & DF_COOP_LOSE_KEYS) &&
				defitem == NULL &&
				item->IsKindOf(NAME_Key))
			{
				item->Destroy();
			}
			else if ((dmflags & DF_COOP_LOSE_WEAPONS) &&
				defitem == NULL &&
				item->IsKindOf(NAME_Weapon))
			{
				item->Destroy();
			}
			else if ((dmflags & DF_COOP_LOSE_ARMOR) &&
				item->IsKindOf(NAME_Armor))
			{
				if (defitem == NULL)
				{
					item->Destroy();
				}
				else if (item->IsKindOf(NAME_BasicArmor))
				{
					item->IntVar(NAME_SavePercent) = defitem->IntVar(NAME_SavePercent);
					item->Amount = defitem->Amount;
				}
				else if (item->IsKindOf(NAME_HexenArmor))
				{
					double *SlotsTo = (double*)item->ScriptVar(NAME_Slots, nullptr);
					double *SlotsFrom = (double*)defitem->ScriptVar(NAME_Slots, nullptr);
					memcpy(SlotsTo, SlotsFrom, 4 * sizeof(double)); 
				}
			}
			else if ((dmflags & DF_COOP_LOSE_POWERUPS) &&
				defitem == NULL &&
				item->IsKindOf(NAME_PowerupGiver))
			{
				item->Destroy();
			}
			else if ((dmflags & (DF_COOP_LOSE_AMMO | DF_COOP_HALVE_AMMO)) &&
				item->IsKindOf(NAME_Ammo))
			{
				if (defitem == NULL)
				{
					if (dmflags & DF_COOP_LOSE_AMMO)
					{
						// Do NOT destroy the ammo, because a weapon might reference it.
						item->Amount = 0;
					}
					else if (item->Amount > 1)
					{
						item->Amount /= 2;
					}
				}
				else
				{
					// When set to lose ammo, you get to keep all your starting ammo.
					// When set to halve ammo, you won't be left with less than your starting amount.
					if (dmflags & DF_COOP_LOSE_AMMO)
					{
						item->Amount = defitem->Amount;
					}
					else if (item->Amount > 1)
					{
						item->Amount = MAX(item->Amount / 2, defitem->Amount);
					}
				}
			}
		}
	}

	// Now destroy the default inventory this player is holding and move
	// over the old player's remaining inventory.
	DestroyAllInventory();
	ObtainInventory (oldplayer);

	player->ReadyWeapon = NULL;
	PickNewWeapon (NULL);
}

//===========================================================================
//
// APlayerPawn :: GetSoundClass
//
//===========================================================================

const char *APlayerPawn::GetSoundClass() const
{
	if (player != NULL &&
		(player->mo == NULL || !(player->mo->flags4 &MF4_NOSKIN)) &&
		(unsigned int)player->userinfo.GetSkin() >= PlayerClasses.Size () &&
		(unsigned)player->userinfo.GetSkin() < Skins.Size())
	{
		return Skins[player->userinfo.GetSkin()].Name.GetChars();
	}

	return SoundClass != NAME_None? SoundClass.GetChars() : "player";
}

//===========================================================================
//
// APlayerPawn :: GetMaxHealth
//
// only needed because Boom screwed up Dehacked.
//
//===========================================================================

int APlayerPawn::GetMaxHealth(bool withupgrades) const 
{ 
	int ret = MaxHealth > 0? MaxHealth : ((i_compatflags&COMPATF_DEHHEALTH)? 100 : deh.MaxHealth);
	if (withupgrades) ret += stamina + BonusHealth;
	return ret;
}

DEFINE_ACTION_FUNCTION(APlayerPawn, GetMaxHealth)
{
	PARAM_SELF_PROLOGUE(APlayerPawn);
	PARAM_BOOL_DEF(withupgrades);
	ACTION_RETURN_INT(self->GetMaxHealth(withupgrades));
}

//===========================================================================
//
// APlayerPawn :: UpdateWaterLevel
//
// Plays surfacing and diving sounds, as appropriate.
//
//===========================================================================

bool APlayerPawn::UpdateWaterLevel (bool splash)
{
	int oldlevel = waterlevel;
	bool retval = Super::UpdateWaterLevel (splash);
	if (player != NULL)
	{
		if (oldlevel < 3 && waterlevel == 3)
		{ // Our head just went under.
			S_Sound (this, CHAN_VOICE, "*dive", 1, ATTN_NORM);
		}
		else if (oldlevel == 3 && waterlevel < 3)
		{ // Our head just came up.
			if (player->air_finished > level.time)
			{ // We hadn't run out of air yet.
				S_Sound (this, CHAN_VOICE, "*surface", 1, ATTN_NORM);
			}
			// If we were running out of air, then ResetAirSupply() will play *gasp.
		}
	}
	return retval;
}

//===========================================================================
//
// APlayerPawn :: ResetAirSupply
//
// Gives the player a full "tank" of air. If they had previously completely
// run out of air, also plays the *gasp sound. Returns true if the player
// was drowning.
//
//===========================================================================

bool APlayerPawn::ResetAirSupply (bool playgasp)
{
	bool wasdrowning = (player->air_finished < level.time);

	if (playgasp && wasdrowning)
	{
		S_Sound (this, CHAN_VOICE, "*gasp", 1, ATTN_NORM);
	}
	if (level.airsupply> 0 && player->mo->AirCapacity > 0) player->air_finished = level.time + int(level.airsupply * player->mo->AirCapacity);
	else player->air_finished = INT_MAX;
	return wasdrowning;
}

DEFINE_ACTION_FUNCTION(APlayerPawn, ResetAirSupply)
{
	PARAM_SELF_PROLOGUE(APlayerPawn);
	PARAM_BOOL_DEF(playgasp);
	ACTION_RETURN_BOOL(self->ResetAirSupply(playgasp));
}

//===========================================================================
//
// Animations
//
//===========================================================================

void APlayerPawn::PlayIdle ()
{
	IFVIRTUAL(APlayerPawn, PlayIdle)
	{
		VMValue params[1] = { (DObject*)this };
		GlobalVMStack.Call(func, params, 1, nullptr, 0, nullptr);
	}
}

void APlayerPawn::PlayRunning ()
{
	IFVIRTUAL(APlayerPawn, PlayRunning)
	{
		VMValue params[1] = { (DObject*)this };
		GlobalVMStack.Call(func, params, 1, nullptr, 0, nullptr);
	}
}

void APlayerPawn::PlayAttacking ()
{
	IFVIRTUAL(APlayerPawn, PlayAttacking)
	{
		VMValue params[1] = { (DObject*)this };
		GlobalVMStack.Call(func, params, 1, nullptr, 0, nullptr);
	}
}

void APlayerPawn::PlayAttacking2 ()
{
	IFVIRTUAL(APlayerPawn, PlayAttacking2)
	{
		VMValue params[1] = { (DObject*)this };
		GlobalVMStack.Call(func, params, 1, nullptr, 0, nullptr);
	}
}

//===========================================================================
//
// APlayerPawn :: GiveDefaultInventory
//
//===========================================================================

void APlayerPawn::GiveDefaultInventory ()
{
	if (player == NULL) return;

	// HexenArmor must always be the first item in the inventory because
	// it provides player class based protection that should not affect
	// any other protection item.
	auto myclass = GetClass();
	GiveInventoryType(PClass::FindActor(NAME_HexenArmor));
	auto harmor = FindInventory(NAME_HexenArmor);

	double *Slots = (double*)harmor->ScriptVar(NAME_Slots, nullptr);
	double *SlotsIncrement = (double*)harmor->ScriptVar(NAME_SlotsIncrement, nullptr);
	Slots[4] = HexenArmor[0];
	for (int i = 0; i < 4; ++i)
	{
		SlotsIncrement[i] = HexenArmor[i + 1];
	}

	// BasicArmor must come right after that. It should not affect any
	// other protection item as well but needs to process the damage
	// before the HexenArmor does.
	auto barmor = (AInventory*)Spawn(NAME_BasicArmor);
	barmor->BecomeItem ();
	AddInventory (barmor);

	// Now add the items from the DECORATE definition
	auto di = GetDropItems();

	while (di)
	{
		PClassActor *ti = PClass::FindActor (di->Name);
		if (ti)
		{
			if (!ti->IsDescendantOf(RUNTIME_CLASS(AInventory)))
			{
				Printf(TEXTCOLOR_ORANGE "%s is not an inventory item and cannot be given to a player as start item.\n", ti->TypeName.GetChars());
			}
			else
			{
				AInventory *item = FindInventory(ti);
				if (item != NULL)
				{
					item->Amount = clamp<int>(
						item->Amount + (di->Amount ? di->Amount : ((AInventory *)item->GetDefault())->Amount),
						0, item->MaxAmount);
				}
				else
				{
					item = static_cast<AInventory *>(Spawn(ti));
					item->ItemFlags |= IF_IGNORESKILL;	// no skill multiplicators here
					item->Amount = di->Amount;
					if (item->IsKindOf(NAME_Weapon))
					{
						// To allow better control any weapon is emptied of
						// ammo before being given to the player.
						static_cast<AWeapon*>(item)->AmmoGive1 =
							static_cast<AWeapon*>(item)->AmmoGive2 = 0;
					}
					AActor *check;
					if (!item->CallTryPickup(this, &check))
					{
						if (check != this)
						{
							// Player was morphed. This is illegal at game start.
							// This problem is only detectable when it's too late to do something about it...
							I_Error("Cannot give morph items when starting a game");
						}
						item->Destroy();
						item = NULL;
					}
				}
				if (item != NULL && item->IsKindOf(NAME_Weapon) &&
					static_cast<AWeapon*>(item)->CheckAmmo(AWeapon::EitherFire, false))
				{
					player->ReadyWeapon = player->PendingWeapon = static_cast<AWeapon *> (item);
				}
			}
		}
		di = di->Next;
	}
}

void APlayerPawn::MorphPlayerThink ()
{
	IFVIRTUAL(APlayerPawn, MorphPlayerThink)
	{
		VMValue params[1] = { (DObject*)this };
		GlobalVMStack.Call(func, params, 1, nullptr, 0, nullptr);
	}
}

void APlayerPawn::ActivateMorphWeapon ()
{
	PClassActor *morphweapon = PClass::FindActor (MorphWeapon);
	player->PendingWeapon = WP_NOCHANGE;

	if (player->ReadyWeapon != nullptr)
	{
		player->GetPSprite(PSP_WEAPON)->y = WEAPONTOP;
	}

	if (morphweapon == nullptr || !morphweapon->IsDescendantOf (RUNTIME_CLASS(AWeapon)))
	{ // No weapon at all while morphed!
		player->ReadyWeapon = nullptr;
	}
	else
	{
		player->ReadyWeapon = static_cast<AWeapon *>(player->mo->FindInventory (morphweapon));
		if (player->ReadyWeapon == nullptr)
		{
			player->ReadyWeapon = static_cast<AWeapon *>(player->mo->GiveInventoryType (morphweapon));
			if (player->ReadyWeapon != nullptr)
			{
				player->ReadyWeapon->GivenAsMorphWeapon = true; // flag is used only by new beastweap semantics in P_UndoPlayerMorph
			}
		}
		if (player->ReadyWeapon != nullptr)
		{
			P_SetPsprite(player, PSP_WEAPON, player->ReadyWeapon->GetReadyState());
		}
	}

	if (player->ReadyWeapon != nullptr)
	{
		P_SetPsprite(player, PSP_FLASH, nullptr);
	}

	player->PendingWeapon = WP_NOCHANGE;
}

//===========================================================================
//
// APlayerPawn :: Die
//
//===========================================================================

void APlayerPawn::Die (AActor *source, AActor *inflictor, int dmgflags)
{
	Super::Die (source, inflictor, dmgflags);

	if (player != NULL && player->mo == this) player->bonuscount = 0;

	if (player != NULL && player->mo != this)
	{ // Make the real player die, too
		player->mo->CallDie (source, inflictor, dmgflags);
	}
	else
	{
		if (player != NULL && (dmflags2 & DF2_YES_WEAPONDROP))
		{ // Voodoo dolls don't drop weapons
			AWeapon *weap = player->ReadyWeapon;
			if (weap != NULL)
			{
				AInventory *item;

				// kgDROP - start - modified copy from a_action.cpp
				auto di = weap->GetDropItems();

				if (di != NULL)
				{
					while (di != NULL)
					{
						if (di->Name != NAME_None)
						{
							PClassActor *ti = PClass::FindActor(di->Name);
							if (ti) P_DropItem (player->mo, ti, di->Amount, di->Probability);
						}
						di = di->Next;
					}
				} else
				// kgDROP - end
				if (weap->SpawnState != NULL &&
					weap->SpawnState != ::GetDefault<AActor>()->SpawnState)
				{
					item = P_DropItem (this, weap->GetClass(), -1, 256);
					if (item != NULL && item->IsKindOf(NAME_Weapon))
					{
						if (weap->AmmoGive1 && weap->Ammo1)
						{
							static_cast<AWeapon *>(item)->AmmoGive1 = weap->Ammo1->Amount;
						}
						if (weap->AmmoGive2 && weap->Ammo2)
						{
							static_cast<AWeapon *>(item)->AmmoGive2 = weap->Ammo2->Amount;
						}
						item->ItemFlags |= IF_IGNORESKILL;
					}
				}
				else
				{
					item = P_DropItem (this, weap->AmmoType1, -1, 256);
					if (item != NULL)
					{
						item->Amount = weap->Ammo1->Amount;
						item->ItemFlags |= IF_IGNORESKILL;
					}
					item = P_DropItem (this, weap->AmmoType2, -1, 256);
					if (item != NULL)
					{
						item->Amount = weap->Ammo2->Amount;
						item->ItemFlags |= IF_IGNORESKILL;
					}
				}
			}
		}
		if (!multiplayer && level.info->deathsequence != NAME_None)
		{
			F_StartIntermission(level.info->deathsequence, FSTATE_EndingGame);
		}
	}
}

//===========================================================================
//
// APlayerPawn :: TweakSpeeds
//
//===========================================================================

void APlayerPawn::TweakSpeeds (double &forward, double &side)
{
	// Strife's player can't run when its health is below 10
	if (health <= RunHealth)
	{
		forward = clamp<double>(forward, -0x1900, 0x1900);
		side = clamp<double>(side, -0x1800, 0x1800);
	}

	// [GRB]
	if (fabs(forward) < 0x3200)
	{
		forward *= ForwardMove1;
	}
	else
	{
		forward *= ForwardMove2;
	}

	if (fabs(side) < 0x2800)
	{
		side *= SideMove1;
	}
	else
	{
		side *= SideMove2;
	}

	if (!player->morphTics && Inventory != NULL)
	{
		double factor = Inventory->GetSpeedFactor ();
		forward *= factor;
		side *= factor;
	}
}

//===========================================================================
//
// A_PlayerScream
//
// try to find the appropriate death sound and use suitable 
// replacements if necessary
//
//===========================================================================

DEFINE_ACTION_FUNCTION(AActor, A_PlayerScream)
{
	PARAM_SELF_PROLOGUE(AActor);

	int sound = 0;
	int chan = CHAN_VOICE;

	if (self->player == NULL || self->DeathSound != 0)
	{
		if (self->DeathSound != 0)
		{
			S_Sound (self, CHAN_VOICE, self->DeathSound, 1, ATTN_NORM);
		}
		else
		{
			S_Sound (self, CHAN_VOICE, "*death", 1, ATTN_NORM);
		}
		return 0;
	}

	// Handle the different player death screams
	if ((((level.flags >> 15) | (dmflags)) &
		(DF_FORCE_FALLINGZD | DF_FORCE_FALLINGHX)) &&
		self->Vel.Z <= -39)
	{
		sound = S_FindSkinnedSound (self, "*splat");
		chan = CHAN_BODY;
	}

	if (!sound && self->special1<10)
	{ // Wimpy death sound
		sound = S_FindSkinnedSoundEx (self, "*wimpydeath", self->player->LastDamageType);
	}
	if (!sound && self->health <= -50)
	{
		if (self->health > -100)
		{ // Crazy death sound
			sound = S_FindSkinnedSoundEx (self, "*crazydeath", self->player->LastDamageType);
		}
		if (!sound)
		{ // Extreme death sound
			sound = S_FindSkinnedSoundEx (self, "*xdeath", self->player->LastDamageType);
			if (!sound)
			{
				sound = S_FindSkinnedSoundEx (self, "*gibbed", self->player->LastDamageType);
				chan = CHAN_BODY;
			}
		}
	}
	if (!sound)
	{ // Normal death sound
		sound = S_FindSkinnedSoundEx (self, "*death", self->player->LastDamageType);
	}

	if (chan != CHAN_VOICE)
	{
		for (int i = 0; i < 8; ++i)
		{ // Stop most playing sounds from this player.
		  // This is mainly to stop *land from messing up *splat.
			if (i != CHAN_WEAPON && i != CHAN_VOICE)
			{
				S_StopSound (self, i);
			}
		}
	}
	S_Sound (self, chan, sound, 1, ATTN_NORM);
	return 0;
}


//----------------------------------------------------------------------------
//
// PROC A_SkullPop
//
//----------------------------------------------------------------------------

DEFINE_ACTION_FUNCTION(AActor, A_SkullPop)
{
	PARAM_SELF_PROLOGUE(AActor);
	PARAM_CLASS_DEF(spawntype, APlayerPawn);

	APlayerPawn *mo;
	player_t *player;

	// [GRB] Parameterized version
	if (spawntype == NULL || !spawntype->IsDescendantOf("PlayerChunk"))
	{
		spawntype = PClass::FindActor("BloodySkull");
		if (spawntype == NULL)
			return 0;
	}

	self->flags &= ~MF_SOLID;
	mo = (APlayerPawn *)Spawn (spawntype, self->PosPlusZ(48.), NO_REPLACE);
	//mo->target = self;
	mo->Vel.X = pr_skullpop.Random2() / 128.;
	mo->Vel.Y = pr_skullpop.Random2() / 128.;
	mo->Vel.Z = 2. + (pr_skullpop() / 1024.);
	// Attach player mobj to bloody skull
	player = self->player;
	self->player = NULL;
	mo->ObtainInventory (self);
	mo->player = player;
	mo->health = self->health;
	mo->Angles.Yaw = self->Angles.Yaw;
	if (player != NULL)
	{
		player->mo = mo;
		player->damagecount = 32;
	}
	for (int i = 0; i < MAXPLAYERS; ++i)
	{
		if (playeringame[i] && players[i].camera == self)
		{
			players[i].camera = mo;
		}
	}
	return 0;
}

//----------------------------------------------------------------------------
//
// PROC A_CheckSkullDone
//
//----------------------------------------------------------------------------

DEFINE_ACTION_FUNCTION(AActor, A_CheckPlayerDone)
{
	PARAM_SELF_PROLOGUE(AActor);

	if (self->player == NULL)
	{
		self->Destroy();
	}
	return 0;
}

//===========================================================================
//
// P_CheckPlayerSprites
//
// Here's the place where crouching sprites are handled.
// R_ProjectSprite() calls this for any players.
//
//===========================================================================

void P_CheckPlayerSprite(AActor *actor, int &spritenum, DVector2 &scale)
{
	player_t *player = actor->player;
	int crouchspriteno;

	if (player->userinfo.GetSkin() != 0 && !(actor->flags4 & MF4_NOSKIN))
	{
		// Convert from default scale to skin scale.
		DVector2 defscale = actor->GetDefault()->Scale;
		scale.X *= Skins[player->userinfo.GetSkin()].Scale.X / defscale.X;
		scale.Y *= Skins[player->userinfo.GetSkin()].Scale.Y / defscale.Y;
	}

	// Set the crouch sprite?
	if (player->crouchfactor < 0.75)
	{
		if (spritenum == actor->SpawnState->sprite || spritenum == player->mo->crouchsprite) 
		{
			crouchspriteno = player->mo->crouchsprite;
		}
		else if (!(actor->flags4 & MF4_NOSKIN) &&
				(spritenum == Skins[player->userinfo.GetSkin()].sprite ||
				 spritenum == Skins[player->userinfo.GetSkin()].crouchsprite))
		{
			crouchspriteno = Skins[player->userinfo.GetSkin()].crouchsprite;
		}
		else
		{ // no sprite -> squash the existing one
			crouchspriteno = -1;
		}

		if (crouchspriteno > 0) 
		{
			spritenum = crouchspriteno;
		}
		else if (player->playerstate != PST_DEAD && player->crouchfactor < 0.75)
		{
			scale.Y *= 0.5;
		}
	}
}

/*
==================
=
= P_Thrust
=
= moves the given origin along a given angle
=
==================
*/

void P_SideThrust (player_t *player, DAngle angle, double move)
{
	player->mo->Thrust(angle-90, move);
}

void P_ForwardThrust (player_t *player, DAngle angle, double move)
{
	if ((player->mo->waterlevel || (player->mo->flags & MF_NOGRAVITY))
		&& player->mo->Angles.Pitch != 0)
	{
		double zpush = move * player->mo->Angles.Pitch.Sin();
		if (player->mo->waterlevel && player->mo->waterlevel < 2 && zpush < 0)
			zpush = 0;
		player->mo->Vel.Z -= zpush;
		move *= player->mo->Angles.Pitch.Cos();
	}
	player->mo->Thrust(angle, move);
}

//
// P_Bob
// Same as P_Thrust, but only affects bobbing.
//
// killough 10/98: We apply thrust separately between the real physical player
// and the part which affects bobbing. This way, bobbing only comes from player
// motion, nothing external, avoiding many problems, e.g. bobbing should not
// occur on conveyors, unless the player walks on one, and bobbing should be
// reduced at a regular rate, even on ice (where the player coasts).
//

void P_Bob (player_t *player, DAngle angle, double move, bool forward)
{
	if (forward
		&& (player->mo->waterlevel || (player->mo->flags & MF_NOGRAVITY))
		&& player->mo->Angles.Pitch != 0)
	{
		move *= player->mo->Angles.Pitch.Cos();
	}
	player->Vel += angle.ToVector(move);
}

/*
==================
=
= P_CalcHeight
=
=
Calculate the walking / running height adjustment
=
==================
*/

void P_CalcHeight (player_t *player) 
{
	DAngle		angle;
	double	 	bob;
	bool		still = false;

	// Regular movement bobbing
	// (needs to be calculated for gun swing even if not on ground)

	// killough 10/98: Make bobbing depend only on player-applied motion.
	//
	// Note: don't reduce bobbing here if on ice: if you reduce bobbing here,
	// it causes bobbing jerkiness when the player moves from ice to non-ice,
	// and vice-versa.

	if (player->cheats & CF_NOCLIP2)
	{
		player->bob = 0;
	}
	else if ((player->mo->flags & MF_NOGRAVITY) && !player->onground)
	{
		player->bob = 0.5;
	}
	else
	{
		player->bob = player->Vel.LengthSquared();
		if (player->bob == 0)
		{
			still = true;
		}
		else
		{
			player->bob *= player->userinfo.GetMoveBob();

			if (player->bob > MAXBOB)
				player->bob = MAXBOB;
		}
	}

	double defaultviewheight = player->mo->ViewHeight + player->crouchviewdelta;

	if (player->cheats & CF_NOVELOCITY)
	{
		player->viewz = player->mo->Z() + defaultviewheight;

		if (player->viewz > player->mo->ceilingz-4)
			player->viewz = player->mo->ceilingz-4;

		return;
	}

	//[SP] Added (x*player->mo->ViewBob) to allow DECORATE changes to view bobbing speed.
	if (still)
	{
		if (player->health > 0)
		{
			angle = level.time / (120 * TICRATE / 35.) * 360. * player->mo->ViewBob;
			bob = player->userinfo.GetStillBob() * angle.Sin();
		}
		else
		{
			bob = 0;
		}
	}
	else
	{
		angle = level.time / (20 * TICRATE / 35.) * 360. * player->mo->ViewBob;
		bob = player->bob * angle.Sin() * (player->mo->waterlevel > 1 ? 0.25f : 0.5f);
	}

	// move viewheight
	if (player->playerstate == PST_LIVE)
	{
		player->viewheight += player->deltaviewheight;

		if (player->viewheight > defaultviewheight)
		{
			player->viewheight = defaultviewheight;
			player->deltaviewheight = 0;
		}
		else if (player->viewheight < (defaultviewheight/2))
		{
			player->viewheight = defaultviewheight/2;
			if (player->deltaviewheight <= 0)
				player->deltaviewheight = 1 / 65536.;
		}
		
		if (player->deltaviewheight)	
		{
			player->deltaviewheight += 0.25;
			if (!player->deltaviewheight)
				player->deltaviewheight = 1/65536.;
		}
	}

	if (player->morphTics)
	{
		bob = 0;
	}
	player->viewz = player->mo->Z() + player->viewheight + (bob * player->mo->ViewBob); // [SP] Allow DECORATE changes to view bobbing speed.
	if (player->mo->Floorclip && player->playerstate != PST_DEAD
		&& player->mo->Z() <= player->mo->floorz)
	{
		player->viewz -= player->mo->Floorclip;
	}
	if (player->viewz > player->mo->ceilingz - 4)
	{
		player->viewz = player->mo->ceilingz - 4;
	}
	if (player->viewz < player->mo->floorz + 4)
	{
		player->viewz = player->mo->floorz + 4;
	}
}

/*
=================
=
= P_MovePlayer
=
=================
*/
CUSTOM_CVAR (Float, sv_aircontrol, 0.00390625f, CVAR_SERVERINFO|CVAR_NOSAVE)
{
	level.aircontrol = self;
	G_AirControlChanged ();
}

void P_MovePlayer (player_t *player)
{
	ticcmd_t *cmd = &player->cmd;
	APlayerPawn *mo = player->mo;

	// [RH] 180-degree turn overrides all other yaws
	if (player->turnticks)
	{
		player->turnticks--;
		mo->Angles.Yaw += (180. / TURN180_TICKS);
	}
	else
	{
		mo->Angles.Yaw += cmd->ucmd.yaw * (360./65536.);
	}

	player->onground = (mo->Z() <= mo->floorz) || (mo->flags2 & MF2_ONMOBJ) || (mo->BounceFlags & BOUNCE_MBF) || (player->cheats & CF_NOCLIP2);

	// killough 10/98:
	//
	// We must apply thrust to the player and bobbing separately, to avoid
	// anomalies. The thrust applied to bobbing is always the same strength on
	// ice, because the player still "works just as hard" to move, while the
	// thrust applied to the movement varies with 'movefactor'.

	if (cmd->ucmd.forwardmove | cmd->ucmd.sidemove)
	{
		double forwardmove, sidemove;
		double bobfactor;
		double friction, movefactor;
		double fm, sm;

		movefactor = P_GetMoveFactor (mo, &friction);
		bobfactor = friction < ORIG_FRICTION ? movefactor : ORIG_FRICTION_FACTOR;
		if (!player->onground && !(player->mo->flags & MF_NOGRAVITY) && !player->mo->waterlevel)
		{
			// [RH] allow very limited movement if not on ground.
			movefactor *= level.aircontrol;
			bobfactor*= level.aircontrol;
		}

		fm = cmd->ucmd.forwardmove;
		sm = cmd->ucmd.sidemove;
		mo->TweakSpeeds (fm, sm);
		fm *= player->mo->Speed / 256;
		sm *= player->mo->Speed / 256;

		// When crouching, speed and bobbing have to be reduced
		if (player->CanCrouch() && player->crouchfactor != 1)
		{
			fm *= player->crouchfactor;
			sm *= player->crouchfactor;
			bobfactor *= player->crouchfactor;
		}

		forwardmove = fm * movefactor * (35 / TICRATE);
		sidemove = sm * movefactor * (35 / TICRATE);

		if (forwardmove)
		{
			P_Bob(player, mo->Angles.Yaw, cmd->ucmd.forwardmove * bobfactor / 256., true);
			P_ForwardThrust(player, mo->Angles.Yaw, forwardmove);
		}
		if (sidemove)
		{
			P_Bob(player, mo->Angles.Yaw - 90, cmd->ucmd.sidemove * bobfactor / 256., false);
			P_SideThrust(player, mo->Angles.Yaw, sidemove);
		}

		if (debugfile)
		{
			fprintf (debugfile, "move player for pl %d%c: (%f,%f,%f) (%f,%f) %f %f w%d [", int(player-players),
				player->cheats&CF_PREDICTING?'p':' ',
				player->mo->X(), player->mo->Y(), player->mo->Z(),forwardmove, sidemove, movefactor, friction, player->mo->waterlevel);
			msecnode_t *n = player->mo->touching_sectorlist;
			while (n != NULL)
			{
				fprintf (debugfile, "%d ", n->m_sector->sectornum);
				n = n->m_tnext;
			}
			fprintf (debugfile, "]\n");
		}

		if (!(player->cheats & CF_PREDICTING) && (forwardmove != 0 || sidemove != 0))
		{
			player->mo->PlayRunning ();
		}

		if (player->cheats & CF_REVERTPLEASE)
		{
			player->cheats &= ~CF_REVERTPLEASE;
			player->camera = player->mo;
		}
	}
}		

//==========================================================================
//
// P_FallingDamage
//
//==========================================================================

void P_FallingDamage (AActor *actor)
{
	int damagestyle;
	int damage;
	double vel;

	damagestyle = ((level.flags >> 15) | (dmflags)) &
		(DF_FORCE_FALLINGZD | DF_FORCE_FALLINGHX);

	if (damagestyle == 0)
		return;
		
	if (actor->floorsector->Flags & SECF_NOFALLINGDAMAGE)
		return;

	vel = fabs(actor->Vel.Z);

	// Since Hexen falling damage is stronger than ZDoom's, it takes
	// precedence. ZDoom falling damage may not be as strong, but it
	// gets felt sooner.

	switch (damagestyle)
	{
	case DF_FORCE_FALLINGHX:		// Hexen falling damage
		if (vel <= 23)
		{ // Not fast enough to hurt
			return;
		}
		if (vel >= 63)
		{ // automatic death
			damage = 1000000;
		}
		else
		{
			vel *= (16. / 23);
			damage = int((vel * vel) / 10 - 24);
			if (actor->Vel.Z > -39 && damage > actor->health
				&& actor->health != 1)
			{ // No-death threshold
				damage = actor->health-1;
			}
		}
		break;
	
	case DF_FORCE_FALLINGZD:		// ZDoom falling damage
		if (vel <= 19)
		{ // Not fast enough to hurt
			return;
		}
		if (vel >= 84)
		{ // automatic death
			damage = 1000000;
		}
		else
		{
			damage = int((vel*vel*(11 / 128.) - 30) / 2);
			if (damage < 1)
			{
				damage = 1;
			}
		}
		break;

	case DF_FORCE_FALLINGST:		// Strife falling damage
		if (vel <= 20)
		{ // Not fast enough to hurt
			return;
		}
		// The minimum amount of damage you take from falling in Strife
		// is 52. Ouch!
		damage = int(vel / (25000./65536.));
		break;

	default:
		return;
	}

	if (actor->player)
	{
		S_Sound (actor, CHAN_AUTO, "*land", 1, ATTN_NORM);
		P_NoiseAlert (actor, actor, true);
		if (damage == 1000000 && (actor->player->cheats & (CF_GODMODE | CF_BUDDHA)))
		{
			damage = 999;
		}
	}
	P_DamageMobj (actor, NULL, NULL, damage, NAME_Falling);
}

//==========================================================================
//
// P_DeathThink
//
//==========================================================================

void P_DeathThink (player_t *player)
{
	int dir;
	DAngle delta;

	player->TickPSprites();

	player->onground = (player->mo->Z() <= player->mo->floorz);
	if (player->mo->IsKindOf (PClass::FindActor("PlayerChunk")))
	{ // Flying bloody skull or flying ice chunk
		player->viewheight = 6;
		player->deltaviewheight = 0;
		if (player->onground)
		{
			if (player->mo->Angles.Pitch > -19.)
			{
				DAngle lookDelta = (-19. - player->mo->Angles.Pitch) / 8;
				player->mo->Angles.Pitch += lookDelta;
			}
		}
	}
	else if (!(player->mo->flags & MF_ICECORPSE))
	{ // Fall to ground (if not frozen)
		player->deltaviewheight = 0;
		if (player->viewheight > 6)
		{
			player->viewheight -= 1;
		}
		if (player->viewheight < 6)
		{
			player->viewheight = 6;
		}
		if (player->mo->Angles.Pitch < 0)
		{
			player->mo->Angles.Pitch += 3;
		}
		else if (player->mo->Angles.Pitch > 0)
		{
			player->mo->Angles.Pitch -= 3;
		}
		if (fabs(player->mo->Angles.Pitch) < 3)
		{
			player->mo->Angles.Pitch = 0.;
		}
	}
	P_CalcHeight (player);
		
	if (player->attacker && player->attacker != player->mo)
	{ // Watch killer
		dir = P_FaceMobj (player->mo, player->attacker, &delta);
		if (delta < 10)
		{ // Looking at killer, so fade damage and poison counters
			if (player->damagecount)
			{
				player->damagecount--;
			}
			if (player->poisoncount)
			{
				player->poisoncount--;
			}
		}
		delta /= 8;
		if (delta > 5.)
		{
			delta = 5.;
		}
		if (dir)
		{ // Turn clockwise
			player->mo->Angles.Yaw += delta;
		}
		else
		{ // Turn counter clockwise
			player->mo->Angles.Yaw -= delta;
		}
	}
	else
	{
		if (player->damagecount)
		{
			player->damagecount--;
		}
		if (player->poisoncount)
		{
			player->poisoncount--;
		}
	}		

	if ((player->cmd.ucmd.buttons & BT_USE ||
		((multiplayer || alwaysapplydmflags) && (dmflags & DF_FORCE_RESPAWN))) && !(dmflags2 & DF2_NO_RESPAWN))
	{
		if (level.time >= player->respawn_time || ((player->cmd.ucmd.buttons & BT_USE) && player->Bot == NULL))
		{
			player->cls = NULL;		// Force a new class if the player is using a random class
			player->playerstate = 
				(multiplayer || (level.flags2 & LEVEL2_ALLOWRESPAWN) || sv_singleplayerrespawn)
				? PST_REBORN : PST_ENTER;
			if (player->mo->special1 > 2)
			{
				player->mo->special1 = 0;
			}
		}
	}
}

//----------------------------------------------------------------------------
//
// PROC P_CrouchMove
//
//----------------------------------------------------------------------------

void P_CrouchMove(player_t * player, int direction)
{
	double defaultheight = player->mo->FullHeight;
	double savedheight = player->mo->Height;
	double crouchspeed = direction * CROUCHSPEED;
	double oldheight = player->viewheight;

	player->crouchdir = (signed char) direction;
	player->crouchfactor += crouchspeed;

	// check whether the move is ok
	player->mo->Height  = defaultheight * player->crouchfactor;
	if (!P_TryMove(player->mo, player->mo->Pos(), false, NULL))
	{
		player->mo->Height = savedheight;
		if (direction > 0)
		{
			// doesn't fit
			player->crouchfactor -= crouchspeed;
			return;
		}
	}
	player->mo->Height = savedheight;

	player->crouchfactor = clamp(player->crouchfactor, 0.5, 1.);
	player->viewheight = player->mo->ViewHeight * player->crouchfactor;
	player->crouchviewdelta = player->viewheight - player->mo->ViewHeight;

	// Check for eyes going above/below fake floor due to crouching motion.
	P_CheckFakeFloorTriggers(player->mo, player->mo->Z() + oldheight, true);
}

//----------------------------------------------------------------------------
//
// PROC P_PlayerThink
//
//----------------------------------------------------------------------------

void P_PlayerThink (player_t *player)
{
	ticcmd_t *cmd;

	if (player->mo == NULL)
	{
		I_Error ("No player %td start\n", player - players + 1);
	}

	if (debugfile && !(player->cheats & CF_PREDICTING))
	{
		fprintf (debugfile, "tic %d for pl %d: (%f, %f, %f, %f) b:%02x p:%d y:%d f:%d s:%d u:%d\n",
			gametic, (int)(player-players), player->mo->X(), player->mo->Y(), player->mo->Z(),
			player->mo->Angles.Yaw.Degrees, player->cmd.ucmd.buttons,
			player->cmd.ucmd.pitch, player->cmd.ucmd.yaw, player->cmd.ucmd.forwardmove,
			player->cmd.ucmd.sidemove, player->cmd.ucmd.upmove);
	}

	// [RH] Zoom the player's FOV
	float desired = player->DesiredFOV;
	// Adjust FOV using on the currently held weapon.
	if (player->playerstate != PST_DEAD &&		// No adjustment while dead.
		player->ReadyWeapon != NULL &&			// No adjustment if no weapon.
		player->ReadyWeapon->FOVScale != 0)		// No adjustment if the adjustment is zero.
	{
		// A negative scale is used to prevent G_AddViewAngle/G_AddViewPitch
		// from scaling with the FOV scale.
		desired *= fabsf(player->ReadyWeapon->FOVScale);
	}
	if (player->FOV != desired)
	{
		if (fabsf (player->FOV - desired) < 7.f)
		{
			player->FOV = desired;
		}
		else
		{
			float zoom = MAX(7.f, fabsf(player->FOV - desired) * 0.025f);
			if (player->FOV > desired)
			{
				player->FOV = player->FOV - zoom;
			}
			else
			{
				player->FOV = player->FOV + zoom;
			}
		}
	}
	if (player->inventorytics)
	{
		player->inventorytics--;
	}
	// Don't interpolate the view for more than one tic
	player->cheats &= ~CF_INTERPVIEW;

	// No-clip cheat
	if ((player->cheats & (CF_NOCLIP | CF_NOCLIP2)) == CF_NOCLIP2)
	{ // No noclip2 without noclip
		player->cheats &= ~CF_NOCLIP2;
	}
	if (player->cheats & (CF_NOCLIP | CF_NOCLIP2) || (player->mo->GetDefault()->flags & MF_NOCLIP))
	{
		player->mo->flags |= MF_NOCLIP;
	}
	else
	{
		player->mo->flags &= ~MF_NOCLIP;
	}
	if (player->cheats & CF_NOCLIP2)
	{
		player->mo->flags |= MF_NOGRAVITY;
	}
	else if (!(player->mo->flags2 & MF2_FLY) && !(player->mo->GetDefault()->flags & MF_NOGRAVITY))
	{
		player->mo->flags &= ~MF_NOGRAVITY;
	}
	cmd = &player->cmd;

	// Make unmodified copies for ACS's GetPlayerInput.
	player->original_oldbuttons = player->original_cmd.buttons;
	player->original_cmd = cmd->ucmd;

	if (player->mo->flags & MF_JUSTATTACKED)
	{ // Chainsaw/Gauntlets attack auto forward motion
		cmd->ucmd.yaw = 0;
		cmd->ucmd.forwardmove = 0xc800/2;
		cmd->ucmd.sidemove = 0;
		player->mo->flags &= ~MF_JUSTATTACKED;
	}

	bool totallyfrozen = P_IsPlayerTotallyFrozen(player);

	// [RH] Being totally frozen zeros out most input parameters.
	if (totallyfrozen)
	{
		if (gamestate == GS_TITLELEVEL)
		{
			cmd->ucmd.buttons = 0;
		}
		else
		{
			cmd->ucmd.buttons &= BT_USE;
		}
		cmd->ucmd.pitch = 0;
		cmd->ucmd.yaw = 0;
		cmd->ucmd.roll = 0;
		cmd->ucmd.forwardmove = 0;
		cmd->ucmd.sidemove = 0;
		cmd->ucmd.upmove = 0;
		player->turnticks = 0;
	}
	else if (player->cheats & CF_FROZEN)
	{
		cmd->ucmd.forwardmove = 0;
		cmd->ucmd.sidemove = 0;
		cmd->ucmd.upmove = 0;
	}

	// Handle crouching
	if (player->cmd.ucmd.buttons & BT_JUMP)
	{
		player->cmd.ucmd.buttons &= ~BT_CROUCH;
	}
	if (player->CanCrouch() && player->health > 0 && level.IsCrouchingAllowed())
	{
		if (!totallyfrozen)
		{
			int crouchdir = player->crouching;
		
			if (crouchdir == 0)
			{
				crouchdir = (player->cmd.ucmd.buttons & BT_CROUCH) ? -1 : 1;
			}
			else if (player->cmd.ucmd.buttons & BT_CROUCH)
			{
				player->crouching = 0;
			}
			if (crouchdir == 1 && player->crouchfactor < 1 &&
				player->mo->Top() < player->mo->ceilingz)
			{
				P_CrouchMove(player, 1);
			}
			else if (crouchdir == -1 && player->crouchfactor > 0.5)
			{
				P_CrouchMove(player, -1);
			}
		}
	}
	else
	{
		player->Uncrouch();
	}

	player->crouchoffset = -(player->mo->ViewHeight) * (1 - player->crouchfactor);

	// MUSINFO stuff
	if (player->MUSINFOtics >= 0 && player->MUSINFOactor != NULL)
	{
		if (--player->MUSINFOtics < 0)
		{
			if (player - players == consoleplayer)
			{
				if (player->MUSINFOactor->args[0] != 0)
				{
					FName *music = level.info->MusicMap.CheckKey(player->MUSINFOactor->args[0]);

					if (music != NULL)
					{
						S_ChangeMusic(music->GetChars(), player->MUSINFOactor->args[1]);
					}
				}
				else
				{
					S_ChangeMusic("*");
				}
			}
			DPrintf(DMSG_NOTIFY, "MUSINFO change for player %d to %d\n", (int)(player - players), player->MUSINFOactor->args[0]);
		}
	}

	if (player->playerstate == PST_DEAD)
	{
		player->Uncrouch();
		P_DeathThink (player);
		return;
	}
	if (player->jumpTics != 0)
	{
		player->jumpTics--;
		if (player->onground && player->jumpTics < -18)
		{
			player->jumpTics = 0;
		}
	}
	if (player->morphTics && !(player->cheats & CF_PREDICTING))
	{
		player->mo->MorphPlayerThink ();
	}

	// [RH] Look up/down stuff
	if (!level.IsFreelookAllowed())
	{
		player->mo->Angles.Pitch = 0.;
	}
	else
	{
		// The player's view pitch is clamped between -32 and +56 degrees,
		// which translates to about half a screen height up and (more than)
		// one full screen height down from straight ahead when view panning
		// is used.
		int clook = cmd->ucmd.pitch;
		if (clook != 0)
		{
			if (clook == -32768)
			{ // center view
				player->centering = true;
			}
			else if (!player->centering)
			{
				// no more overflows with floating point. Yay! :)
				player->mo->Angles.Pitch = clamp(player->mo->Angles.Pitch - clook * (360. / 65536.), player->MinPitch, player->MaxPitch);
			}
		}
	}
	if (player->centering)
	{
		if (fabs(player->mo->Angles.Pitch) > 2.)
		{
			player->mo->Angles.Pitch *= (2. / 3.);
		}
		else
		{
			player->mo->Angles.Pitch = 0.;
			player->centering = false;
			if (player - players == consoleplayer)
			{
				LocalViewPitch = 0;
			}
		}
	}

	// [RH] Check for fast turn around
	if (cmd->ucmd.buttons & BT_TURN180 && !(player->oldbuttons & BT_TURN180))
	{
		player->turnticks = TURN180_TICKS;
	}

	// Handle movement
	if (player->mo->reactiontime)
	{ // Player is frozen
		player->mo->reactiontime--;
	}
	else
	{
		P_MovePlayer (player);

		// [RH] check for jump
		if (cmd->ucmd.buttons & BT_JUMP)
		{
			if (player->crouchoffset != 0)
			{
				// Jumping while crouching will force an un-crouch but not jump
				player->crouching = 1;
			}
			else if (player->mo->waterlevel >= 2)
			{
				player->mo->Vel.Z = 4 * player->mo->Speed;
			}
			else if (player->mo->flags & MF_NOGRAVITY)
			{
				player->mo->Vel.Z = 3.;
			}
			else if (level.IsJumpingAllowed() && player->onground && player->jumpTics == 0)
			{
				double jumpvelz = player->mo->JumpZ * 35 / TICRATE;
				double jumpfac = 0;

				// [BC] If the player has the high jump power, double his jump velocity.
				// (actually, pick the best factors from all active items.)
				for (auto p = player->mo->Inventory; p != nullptr; p = p->Inventory)
				{
					if (p->IsKindOf(NAME_PowerHighJump))
					{
						double f = p->FloatVar(NAME_Strength);
						if (f > jumpfac) jumpfac = f;
					}
				}
				if (jumpfac > 0) jumpvelz *= jumpfac;

				player->mo->Vel.Z += jumpvelz;
				player->mo->flags2 &= ~MF2_ONMOBJ;
				player->jumpTics = -1;
				if (!(player->cheats & CF_PREDICTING))
					S_Sound(player->mo, CHAN_BODY, "*jump", 1, ATTN_NORM);
			}
		}

		if (cmd->ucmd.upmove == -32768)
		{ // Only land if in the air
			if ((player->mo->flags & MF_NOGRAVITY) && player->mo->waterlevel < 2)
			{
				//player->mo->flags2 &= ~MF2_FLY;
				player->mo->flags &= ~MF_NOGRAVITY;
			}
		}
		else if (cmd->ucmd.upmove != 0)
		{
			// Clamp the speed to some reasonable maximum.
			cmd->ucmd.upmove = clamp<short>(cmd->ucmd.upmove, -0x300, 0x300);
			if (player->mo->waterlevel >= 2 || (player->mo->flags2 & MF2_FLY) || (player->cheats & CF_NOCLIP2))
			{
				player->mo->Vel.Z = player->mo->Speed * cmd->ucmd.upmove / 128.;
				if (player->mo->waterlevel < 2 && !(player->mo->flags & MF_NOGRAVITY))
				{
					player->mo->flags2 |= MF2_FLY;
					player->mo->flags |= MF_NOGRAVITY;
					if ((player->mo->Vel.Z <= -39) && !(player->cheats & CF_PREDICTING))
					{ // Stop falling scream
						S_StopSound (player->mo, CHAN_VOICE);
					}
				}
			}
			else if (cmd->ucmd.upmove > 0 && !(player->cheats & CF_PREDICTING))
			{
				AInventory *fly = player->mo->FindInventory (NAME_ArtiFly);
				if (fly != NULL)
				{
					player->mo->UseInventory (fly);
				}
			}
		}
	}

	P_CalcHeight (player);

	if (!(player->cheats & CF_PREDICTING))
	{
		P_PlayerOnSpecial3DFloor (player);
		P_PlayerInSpecialSector (player);

		if (!player->mo->isAbove(player->mo->Sector->floorplane.ZatPoint(player->mo)) ||
			player->mo->waterlevel)
		{
			// Player must be touching the floor
			P_PlayerOnSpecialFlat(player, P_GetThingFloorType(player->mo));
		}
		if (player->mo->Vel.Z <= -player->mo->FallingScreamMinSpeed &&
			player->mo->Vel.Z >= -player->mo->FallingScreamMaxSpeed && !player->morphTics &&
			player->mo->waterlevel == 0)
		{
			int id = S_FindSkinnedSound (player->mo, "*falling");
			if (id != 0 && !S_IsActorPlayingSomething (player->mo, CHAN_VOICE, id))
			{
				S_Sound (player->mo, CHAN_VOICE, id, 1, ATTN_NORM);
			}
		}
		// check for use
		if (cmd->ucmd.buttons & BT_USE)
		{
			if (!player->usedown)
			{
				player->usedown = true;
				if (!P_TalkFacing(player->mo))
				{
					P_UseLines(player);
				}
			}
		}
		else
		{
			player->usedown = false;
		}
		// Morph counter
		if (player->morphTics)
		{
			if (player->chickenPeck)
			{ // Chicken attack counter
				player->chickenPeck -= 3;
			}
			if (!--player->morphTics)
			{ // Attempt to undo the chicken/pig
				P_UndoPlayerMorph (player, player, MORPH_UNDOBYTIMEOUT);
			}
		}
		// Cycle psprites
		player->TickPSprites();

		// Other Counters
		if (player->damagecount)
			player->damagecount--;

		if (player->bonuscount)
			player->bonuscount--;

		if (player->hazardcount)
		{
			player->hazardcount--;
			if (!(level.time % player->hazardinterval) && player->hazardcount > 16*TICRATE)
				P_DamageMobj (player->mo, NULL, NULL, 5, player->hazardtype);
		}

		if (player->poisoncount && !(level.time & 15))
		{
			player->poisoncount -= 5;
			if (player->poisoncount < 0)
			{
				player->poisoncount = 0;
			}
			P_PoisonDamage (player, player->poisoner, 1, true);
		}

		// Apply degeneration.
		if (dmflags2 & DF2_YES_DEGENERATION)
		{
			int maxhealth = player->mo->GetMaxHealth(true);
			if ((level.time % TICRATE) == 0 && player->health > maxhealth)
			{
				if (player->health - 5 < maxhealth)
					player->health = maxhealth;
				else
					player->health--;

				player->mo->health = player->health;
			}
		}

		// Handle air supply
		//if (level.airsupply > 0)
		{
			if (player->mo->waterlevel < 3 ||
				(player->mo->flags2 & MF2_INVULNERABLE) ||
				(player->cheats & (CF_GODMODE | CF_NOCLIP2)) ||
				(player->cheats & CF_GODMODE2))
			{
				player->mo->ResetAirSupply ();
			}
			else if (player->air_finished <= level.time && !(level.time & 31))
			{
				P_DamageMobj (player->mo, NULL, NULL, 2 + ((level.time-player->air_finished)/TICRATE), NAME_Drowning);
			}
		}
	}
}

void P_PredictionLerpReset()
{
	PredictionLerptics = PredictionLast.gametic = PredictionLerpFrom.gametic = PredictionLerpResult.gametic = 0;
}

bool P_LerpCalculate(AActor *pmo, PredictPos from, PredictPos to, PredictPos &result, float scale)
{
	//DVector2 pfrom = Displacements.getOffset(from.portalgroup, to.portalgroup);
	DVector3 vecFrom = from.pos;
	DVector3 vecTo = to.pos;
	DVector3 vecResult;
	vecResult = vecTo - vecFrom;
	vecResult *= scale;
	vecResult = vecResult + vecFrom;
	DVector3 delta = vecResult - vecTo;

	result.pos = pmo->Vec3Offset(vecResult - to.pos);
	//result.portalgroup = P_PointInSector(result.pos.x, result.pos.y)->PortalGroup;

	// As a fail safe, assume extrapolation is the threshold.
	return (delta.LengthSquared() > cl_predict_lerpthreshold && scale <= 1.00f);
}

template<class nodetype, class linktype>
void BackupNodeList(AActor *act, nodetype *head, nodetype *linktype::*otherlist, TArray<nodetype*, nodetype*> &prevbackup, TArray<linktype *, linktype *> &otherbackup)
{
	// The ordering of the touching_sectorlist needs to remain unchanged
	// Also store a copy of all previous sector_thinglist nodes
	prevbackup.Clear();
	otherbackup.Clear();

	for (auto mnode = head; mnode != nullptr; mnode = mnode->m_tnext)
	{
		otherbackup.Push(mnode->m_sector);

		for (auto snode = mnode->m_sector->*otherlist; snode; snode = snode->m_snext)
		{
			if (snode->m_thing == act)
			{
				prevbackup.Push(snode->m_sprev);
				break;
			}
		}
	}
}

template<class nodetype, class linktype>
nodetype *RestoreNodeList(AActor *act, nodetype *head, nodetype *linktype::*otherlist, TArray<nodetype*, nodetype*> &prevbackup, TArray<linktype *, linktype *> &otherbackup)
{
	// Destroy old refrences
	nodetype *node = head;
	while (node)
	{
		node->m_thing = NULL;
		node = node->m_tnext;
	}

	// Make the sector_list match the player's touching_sectorlist before it got predicted.
	P_DelSeclist(head, otherlist);
	head = NULL;
	for (auto i = otherbackup.Size(); i-- > 0;)
	{
		head = P_AddSecnode(otherbackup[i], act, head, otherbackup[i]->*otherlist);
	}
	//act->touching_sectorlist = ctx.sector_list;	// Attach to thing
	//ctx.sector_list = NULL;		// clear for next time

	// In the old code this block never executed because of the commented-out NULL assignment above. Needs to be checked
	node = head;
	while (node)
	{
		if (node->m_thing == NULL)
		{
			if (node == head)
				head = node->m_tnext;
			node = P_DelSecnode(node, otherlist);
		}
		else
		{
			node = node->m_tnext;
		}
	}

	nodetype *snode;

	// Restore sector thinglist order
	for (auto i = otherbackup.Size(); i-- > 0;)
	{
		// If we were already the head node, then nothing needs to change
		if (prevbackup[i] == NULL)
			continue;

		for (snode = otherbackup[i]->*otherlist; snode; snode = snode->m_snext)
		{
			if (snode->m_thing == act)
			{
				if (snode->m_sprev)
					snode->m_sprev->m_snext = snode->m_snext;
				else
					snode->m_sector->*otherlist = snode->m_snext;
				if (snode->m_snext)
					snode->m_snext->m_sprev = snode->m_sprev;

				snode->m_sprev = prevbackup[i];

				// At the moment, we don't exist in the list anymore, but we do know what our previous node is, so we set its current m_snext->m_sprev to us.
				if (snode->m_sprev->m_snext)
					snode->m_sprev->m_snext->m_sprev = snode;
				snode->m_snext = snode->m_sprev->m_snext;
				snode->m_sprev->m_snext = snode;
				break;
			}
		}
	}
	return head;
}

void P_PredictPlayer (player_t *player)
{
	int maxtic;

	if (cl_noprediction ||
		singletics ||
		demoplayback ||
		player->mo == NULL ||
		player != &players[consoleplayer] ||
		player->playerstate != PST_LIVE ||
		!netgame ||
		/*player->morphTics ||*/
		(player->cheats & CF_PREDICTING))
	{
		return;
	}

	maxtic = maketic;

	if (gametic == maxtic)
	{
		return;
	}

	// Save original values for restoration later
	PredictionPlayerBackup = *player;

	APlayerPawn *act = player->mo;
	memcpy(PredictionActorBackup, &act->snext, sizeof(APlayerPawn) - ((uint8_t *)&act->snext - (uint8_t *)act));

	act->flags &= ~MF_PICKUP;
	act->flags2 &= ~MF2_PUSHWALL;
	player->cheats |= CF_PREDICTING;

	BackupNodeList(act, act->touching_sectorlist, &sector_t::touching_thinglist, PredictionTouchingSectors_sprev_Backup, PredictionTouchingSectorsBackup);
	BackupNodeList(act, act->touching_rendersectors, &sector_t::touching_renderthings, PredictionRenderSectors_sprev_Backup, PredictionRenderSectorsBackup);
	BackupNodeList(act, act->touching_sectorportallist, &sector_t::sectorportal_thinglist, PredictionPortalSectors_sprev_Backup, PredictionPortalSectorsBackup);
	BackupNodeList(act, act->touching_lineportallist, &FLinePortal::lineportal_thinglist, PredictionPortalLines_sprev_Backup, PredictionPortalLinesBackup);

	// Keep an ordered list off all actors in the linked sector.
	PredictionSectorListBackup.Clear();
	if (!(act->flags & MF_NOSECTOR))
	{
		AActor *link = act->Sector->thinglist;
		
		while (link != NULL)
		{
			PredictionSectorListBackup.Push(link);
			link = link->snext;
		}
	}

	// Blockmap ordering also needs to stay the same, so unlink the block nodes
	// without releasing them. (They will be used again in P_UnpredictPlayer).
	FBlockNode *block = act->BlockNode;

	while (block != NULL)
	{
		if (block->NextActor != NULL)
		{
			block->NextActor->PrevActor = block->PrevActor;
		}
		*(block->PrevActor) = block->NextActor;
		block = block->NextBlock;
	}
	act->BlockNode = NULL;

	// Values too small to be usable for lerping can be considered "off".
	bool CanLerp = (!(cl_predict_lerpscale < 0.01f) && (ticdup == 1)), DoLerp = false, NoInterpolateOld = R_GetViewInterpolationStatus();
	for (int i = gametic; i < maxtic; ++i)
	{
		if (!NoInterpolateOld)
			R_RebuildViewInterpolation(player);

		player->cmd = localcmds[i % LOCALCMDTICS];
		P_PlayerThink (player);
		player->mo->Tick ();

		if (CanLerp && PredictionLast.gametic > 0 && i == PredictionLast.gametic && !NoInterpolateOld)
		{
			// Z is not compared as lifts will alter this with no apparent change
			// Make lerping less picky by only testing whole units
			DoLerp = (int)PredictionLast.pos.X != (int)player->mo->X() || (int)PredictionLast.pos.Y != (int)player->mo->Y();

			// Aditional Debug information
			if (developer >= DMSG_NOTIFY && DoLerp)
			{
				DPrintf(DMSG_NOTIFY, "Lerp! Ltic (%d) && Ptic (%d) | Lx (%f) && Px (%f) | Ly (%f) && Py (%f)\n",
					PredictionLast.gametic, i,
					(PredictionLast.pos.X), (player->mo->X()),
					(PredictionLast.pos.Y), (player->mo->Y()));
			}
		}
	}

	if (CanLerp)
	{
		if (NoInterpolateOld)
			P_PredictionLerpReset();

		else if (DoLerp)
		{
			// If lerping is already in effect, use the previous camera postion so the view doesn't suddenly snap
			PredictionLerpFrom = (PredictionLerptics == 0) ? PredictionLast : PredictionLerpResult;
			PredictionLerptics = 1;
		}

		PredictionLast.gametic = maxtic - 1;
		PredictionLast.pos = player->mo->Pos();
		//PredictionLast.portalgroup = player->mo->Sector->PortalGroup;

		if (PredictionLerptics > 0)
		{
			if (PredictionLerpFrom.gametic > 0 &&
				P_LerpCalculate(player->mo, PredictionLerpFrom, PredictionLast, PredictionLerpResult, (float)PredictionLerptics * cl_predict_lerpscale))
			{
				PredictionLerptics++;
				player->mo->SetXYZ(PredictionLerpResult.pos);
			}
			else
			{
				PredictionLerptics = 0;
			}
		}
	}
}

void P_UnPredictPlayer ()
{
	player_t *player = &players[consoleplayer];

	if (player->cheats & CF_PREDICTING)
	{
		unsigned int i;
		APlayerPawn *act = player->mo;
		AActor *savedcamera = player->camera;

		TObjPtr<AInventory*> InvSel = act->InvSel;
		int inventorytics = player->inventorytics;

		*player = PredictionPlayerBackup;

		// Restore the camera instead of using the backup's copy, because spynext/prev
		// could cause it to change during prediction.
		player->camera = savedcamera;

		FLinkContext ctx;
		// Unlink from all list, includeing those which are not being handled by UnlinkFromWorld.
		auto sectorportal_list = act->touching_sectorportallist;
		auto lineportal_list = act->touching_lineportallist;
		act->touching_sectorportallist = nullptr;
		act->touching_lineportallist = nullptr;

		act->UnlinkFromWorld(&ctx);
		memcpy(&act->snext, PredictionActorBackup, sizeof(APlayerPawn) - ((uint8_t *)&act->snext - (uint8_t *)act));

		// The blockmap ordering needs to remain unchanged, too.
		// Restore sector links and refrences.
		// [ED850] This is somewhat of a duplicate of LinkToWorld(), but we need to keep every thing the same,
		// otherwise we end up fixing bugs in blockmap logic (i.e undefined behaviour with polyobject collisions),
		// which we really don't want to do here.
		if (!(act->flags & MF_NOSECTOR))
		{
			sector_t *sec = act->Sector;
			AActor *me, *next;
			AActor **link;// , **prev;

			// The thinglist is just a pointer chain. We are restoring the exact same things, so we can NULL the head safely
			sec->thinglist = NULL;

			for (i = PredictionSectorListBackup.Size(); i-- > 0;)
			{
				me = PredictionSectorListBackup[i];
				link = &sec->thinglist;
				next = *link;
				if ((me->snext = next))
					next->sprev = &me->snext;
				me->sprev = link;
				*link = me;
			}

			act->touching_sectorlist = RestoreNodeList(act, ctx.sector_list, &sector_t::touching_thinglist, PredictionTouchingSectors_sprev_Backup, PredictionTouchingSectorsBackup);
			act->touching_rendersectors = RestoreNodeList(act, ctx.render_list, &sector_t::touching_renderthings, PredictionRenderSectors_sprev_Backup, PredictionRenderSectorsBackup);
			act->touching_sectorportallist = RestoreNodeList(act, sectorportal_list, &sector_t::sectorportal_thinglist, PredictionPortalSectors_sprev_Backup, PredictionPortalSectorsBackup);
			act->touching_lineportallist = RestoreNodeList(act, lineportal_list, &FLinePortal::lineportal_thinglist, PredictionPortalLines_sprev_Backup, PredictionPortalLinesBackup);
		}

		// Now fix the pointers in the blocknode chain
		FBlockNode *block = act->BlockNode;

		while (block != NULL)
		{
			*(block->PrevActor) = block;
			if (block->NextActor != NULL)
			{
				block->NextActor->PrevActor = &block->NextActor;
			}
			block = block->NextBlock;
		}

		act->InvSel = InvSel;
		player->inventorytics = inventorytics;
	}
}

void player_t::Serialize(FSerializer &arc)
{
	FString skinname;

	arc("class", cls)
		("mo", mo)
		("camera", camera)
		("playerstate", playerstate)
		("cmd", cmd);

	if (arc.isReading())
	{
		ReadUserInfo(arc, userinfo, skinname);
	}
	else
	{
		WriteUserInfo(arc, userinfo);
	}

	arc("desiredfov", DesiredFOV)
		("fov", FOV)
		("viewz", viewz)
		("viewheight", viewheight)
		("deltaviewheight", deltaviewheight)
		("bob", bob)
		("vel", Vel)
		("centering", centering)
		("health", health)
		("inventorytics", inventorytics)
		("fragcount", fragcount)
		("spreecount", spreecount)
		("multicount", multicount)
		("lastkilltime", lastkilltime)
		("readyweapon", ReadyWeapon)
		("pendingweapon", PendingWeapon)
		("cheats", cheats)
		("refire", refire)
		("inconsistant", inconsistant)
		("killcount", killcount)
		("itemcount", itemcount)
		("secretcount", secretcount)
		("damagecount", damagecount)
		("bonuscount", bonuscount)
		("hazardcount", hazardcount)
		("poisoncount", poisoncount)
		("poisoner", poisoner)
		("attacker", attacker)
		("extralight", extralight)
		("fixedcolormap", fixedcolormap)
		("fixedlightlevel", fixedlightlevel)
		("morphTics", morphTics)
		("morphedplayerclass", MorphedPlayerClass)
		("morphstyle", MorphStyle)
		("morphexitflash", MorphExitFlash)
		("premorphweapon", PremorphWeapon)
		("chickenpeck", chickenPeck)
		("jumptics", jumpTics)
		("respawntime", respawn_time)
		("airfinished", air_finished)
		("turnticks", turnticks)
		("oldbuttons", oldbuttons)
		("hazardtype", hazardtype)
		("hazardinterval", hazardinterval)
		("bot", Bot)
		("blendr", BlendR)
		("blendg", BlendG)
		("blendb", BlendB)
		("blenda", BlendA)
		("weaponstate", WeaponState)
		("logtext", LogText)
		("conversionnpc", ConversationNPC)
		("conversionpc", ConversationPC)
		("conversionnpcangle", ConversationNPCAngle)
		("conversionfacetalker", ConversationFaceTalker)
		.Array("frags", frags, MAXPLAYERS)
		("psprites", psprites)
		("currentplayerclass", CurrentPlayerClass)
		("crouchfactor", crouchfactor)
		("crouching", crouching)
		("crouchdir", crouchdir)
		("crouchviewdelta", crouchviewdelta)
		("original_cmd", original_cmd)
		("original_oldbuttons", original_oldbuttons)
		("poisontype", poisontype)
		("poisonpaintype", poisonpaintype)
		("timefreezer", timefreezer)
		("settings_controller", settings_controller)
		("onground", onground)
		("musinfoactor", MUSINFOactor)
		("musinfotics", MUSINFOtics);

	if (arc.isWriting ())
	{
		// If the player reloaded because they pressed +use after dying, we
		// don't want +use to still be down after the game is loaded.
		oldbuttons = ~0;
		original_oldbuttons = ~0;
	}
	if (skinname.IsNotEmpty())
	{
		userinfo.SkinChanged(skinname, CurrentPlayerClass);
	}
}

bool P_IsPlayerTotallyFrozen(const player_t *player)
{
	return
		gamestate == GS_TITLELEVEL ||
		player->cheats & CF_TOTALLYFROZEN ||
		((level.flags2 & LEVEL2_FROZEN) && player->timefreezer == 0);
}


//==========================================================================
//
// native members
//
//==========================================================================

DEFINE_FIELD(APlayerPawn, crouchsprite)
DEFINE_FIELD(APlayerPawn, MaxHealth)
DEFINE_FIELD(APlayerPawn, BonusHealth)
DEFINE_FIELD(APlayerPawn, MugShotMaxHealth)
DEFINE_FIELD(APlayerPawn, RunHealth)
DEFINE_FIELD(APlayerPawn, PlayerFlags)
DEFINE_FIELD(APlayerPawn, InvFirst)
DEFINE_FIELD(APlayerPawn, InvSel)
DEFINE_FIELD(APlayerPawn, JumpZ)
DEFINE_FIELD(APlayerPawn, GruntSpeed)
DEFINE_FIELD(APlayerPawn, FallingScreamMinSpeed)
DEFINE_FIELD(APlayerPawn, FallingScreamMaxSpeed)
DEFINE_FIELD(APlayerPawn, ViewHeight)
DEFINE_FIELD(APlayerPawn, ForwardMove1)
DEFINE_FIELD(APlayerPawn, ForwardMove2)
DEFINE_FIELD(APlayerPawn, SideMove1)
DEFINE_FIELD(APlayerPawn, SideMove2)
DEFINE_FIELD(APlayerPawn, ScoreIcon)
DEFINE_FIELD(APlayerPawn, SpawnMask)
DEFINE_FIELD(APlayerPawn, MorphWeapon)
DEFINE_FIELD(APlayerPawn, AttackZOffset)
DEFINE_FIELD(APlayerPawn, UseRange)
DEFINE_FIELD(APlayerPawn, AirCapacity)
DEFINE_FIELD(APlayerPawn, FlechetteType)
DEFINE_FIELD(APlayerPawn, DamageFade)
DEFINE_FIELD(APlayerPawn, ViewBob)
DEFINE_FIELD(APlayerPawn, FullHeight)
DEFINE_FIELD(APlayerPawn, SoundClass)
DEFINE_FIELD(APlayerPawn, Face)
DEFINE_FIELD(APlayerPawn, Portrait)
DEFINE_FIELD(APlayerPawn, Slot)
DEFINE_FIELD(APlayerPawn, HexenArmor)
DEFINE_FIELD(APlayerPawn, ColorRangeStart)
DEFINE_FIELD(APlayerPawn, ColorRangeEnd)

DEFINE_FIELD_X(PlayerInfo, player_t, mo)
DEFINE_FIELD_X(PlayerInfo, player_t, playerstate)
DEFINE_FIELD_X(PlayerInfo, player_t, original_oldbuttons)
DEFINE_FIELD_X(PlayerInfo, player_t, cls)
DEFINE_FIELD_X(PlayerInfo, player_t, DesiredFOV)
DEFINE_FIELD_X(PlayerInfo, player_t, FOV)
DEFINE_FIELD_X(PlayerInfo, player_t, viewz)
DEFINE_FIELD_X(PlayerInfo, player_t, viewheight)
DEFINE_FIELD_X(PlayerInfo, player_t, deltaviewheight)
DEFINE_FIELD_X(PlayerInfo, player_t, bob)
DEFINE_FIELD_X(PlayerInfo, player_t, Vel)
DEFINE_FIELD_X(PlayerInfo, player_t, centering)
DEFINE_FIELD_X(PlayerInfo, player_t, turnticks)
DEFINE_FIELD_X(PlayerInfo, player_t, attackdown)
DEFINE_FIELD_X(PlayerInfo, player_t, usedown)
DEFINE_FIELD_X(PlayerInfo, player_t, oldbuttons)
DEFINE_FIELD_X(PlayerInfo, player_t, health)
DEFINE_FIELD_X(PlayerInfo, player_t, inventorytics)
DEFINE_FIELD_X(PlayerInfo, player_t, CurrentPlayerClass)
DEFINE_FIELD_X(PlayerInfo, player_t, frags)
DEFINE_FIELD_X(PlayerInfo, player_t, fragcount)
DEFINE_FIELD_X(PlayerInfo, player_t, lastkilltime)
DEFINE_FIELD_X(PlayerInfo, player_t, multicount)
DEFINE_FIELD_X(PlayerInfo, player_t, spreecount)
DEFINE_FIELD_X(PlayerInfo, player_t, WeaponState)
DEFINE_FIELD_X(PlayerInfo, player_t, ReadyWeapon)
DEFINE_FIELD_X(PlayerInfo, player_t, PendingWeapon)
DEFINE_FIELD_X(PlayerInfo, player_t, psprites)
DEFINE_FIELD_X(PlayerInfo, player_t, cheats)
DEFINE_FIELD_X(PlayerInfo, player_t, timefreezer)
DEFINE_FIELD_X(PlayerInfo, player_t, refire)
DEFINE_FIELD_NAMED_X(PlayerInfo, player_t, inconsistant, inconsistent)
DEFINE_FIELD_X(PlayerInfo, player_t, waiting)
DEFINE_FIELD_X(PlayerInfo, player_t, killcount)
DEFINE_FIELD_X(PlayerInfo, player_t, itemcount)
DEFINE_FIELD_X(PlayerInfo, player_t, secretcount)
DEFINE_FIELD_X(PlayerInfo, player_t, damagecount)
DEFINE_FIELD_X(PlayerInfo, player_t, bonuscount)
DEFINE_FIELD_X(PlayerInfo, player_t, hazardcount)
DEFINE_FIELD_X(PlayerInfo, player_t, hazardinterval)
DEFINE_FIELD_X(PlayerInfo, player_t, hazardtype)
DEFINE_FIELD_X(PlayerInfo, player_t, poisoncount)
DEFINE_FIELD_X(PlayerInfo, player_t, poisontype)
DEFINE_FIELD_X(PlayerInfo, player_t, poisonpaintype)
DEFINE_FIELD_X(PlayerInfo, player_t, poisoner)
DEFINE_FIELD_X(PlayerInfo, player_t, attacker)
DEFINE_FIELD_X(PlayerInfo, player_t, extralight)
DEFINE_FIELD_X(PlayerInfo, player_t, fixedcolormap)
DEFINE_FIELD_X(PlayerInfo, player_t, fixedlightlevel)
DEFINE_FIELD_X(PlayerInfo, player_t, morphTics)
DEFINE_FIELD_X(PlayerInfo, player_t, MorphedPlayerClass)
DEFINE_FIELD_X(PlayerInfo, player_t, MorphStyle)
DEFINE_FIELD_X(PlayerInfo, player_t, MorphExitFlash)
DEFINE_FIELD_X(PlayerInfo, player_t, PremorphWeapon)
DEFINE_FIELD_X(PlayerInfo, player_t, chickenPeck)
DEFINE_FIELD_X(PlayerInfo, player_t, jumpTics)
DEFINE_FIELD_X(PlayerInfo, player_t, onground)
DEFINE_FIELD_X(PlayerInfo, player_t, respawn_time)
DEFINE_FIELD_X(PlayerInfo, player_t, camera)
DEFINE_FIELD_X(PlayerInfo, player_t, air_finished)
DEFINE_FIELD_X(PlayerInfo, player_t, LastDamageType)
DEFINE_FIELD_X(PlayerInfo, player_t, MUSINFOactor)
DEFINE_FIELD_X(PlayerInfo, player_t, MUSINFOtics)
DEFINE_FIELD_X(PlayerInfo, player_t, settings_controller)
DEFINE_FIELD_X(PlayerInfo, player_t, crouching)
DEFINE_FIELD_X(PlayerInfo, player_t, crouchdir)
DEFINE_FIELD_X(PlayerInfo, player_t, Bot)
DEFINE_FIELD_X(PlayerInfo, player_t, BlendR)
DEFINE_FIELD_X(PlayerInfo, player_t, BlendG)
DEFINE_FIELD_X(PlayerInfo, player_t, BlendB)
DEFINE_FIELD_X(PlayerInfo, player_t, BlendA)
DEFINE_FIELD_X(PlayerInfo, player_t, LogText)
DEFINE_FIELD_X(PlayerInfo, player_t, MinPitch)
DEFINE_FIELD_X(PlayerInfo, player_t, MaxPitch)
DEFINE_FIELD_X(PlayerInfo, player_t, crouchfactor)
DEFINE_FIELD_X(PlayerInfo, player_t, crouchoffset)
DEFINE_FIELD_X(PlayerInfo, player_t, crouchviewdelta)
DEFINE_FIELD_X(PlayerInfo, player_t, ConversationNPC)
DEFINE_FIELD_X(PlayerInfo, player_t, ConversationPC)
DEFINE_FIELD_X(PlayerInfo, player_t, ConversationNPCAngle)
DEFINE_FIELD_X(PlayerInfo, player_t, ConversationFaceTalker)
DEFINE_FIELD_X(PlayerInfo, player_t, cmd)
DEFINE_FIELD_X(PlayerInfo, player_t, original_cmd)
DEFINE_FIELD_X(PlayerInfo, player_t, userinfo)
DEFINE_FIELD_X(PlayerInfo, player_t, weapons)

DEFINE_FIELD(FPlayerClass, Type)
DEFINE_FIELD(FPlayerClass, Flags)
DEFINE_FIELD(FPlayerClass, Skins)
