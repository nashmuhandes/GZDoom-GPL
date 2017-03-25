
//**************************************************************************
//**
//** p_pspr.c : Heretic 2 : Raven Software, Corp.
//**
//** $RCSfile: p_pspr.c,v $
//** $Revision: 1.105 $
//** $Date: 96/01/06 03:23:35 $
//** $Author: bgokey $
//**
//**************************************************************************

// HEADER FILES ------------------------------------------------------------

#include <stdlib.h>

#include "doomdef.h"
#include "d_event.h"
#include "c_cvars.h"
#include "m_random.h"
#include "p_enemy.h"
#include "p_local.h"
#include "s_sound.h"
#include "doomstat.h"
#include "gi.h"
#include "p_pspr.h"
#include "templates.h"
#include "g_level.h"
#include "d_player.h"
#include "serializer.h"
#include "v_text.h"
#include "cmdlib.h"
#include "g_levellocals.h"


// MACROS ------------------------------------------------------------------

#define LOWERSPEED				6.

// TYPES -------------------------------------------------------------------

struct FGenericButtons
{
	int ReadyFlag;			// Flag passed to A_WeaponReady
	int StateFlag;			// Flag set in WeaponState
	int ButtonFlag;			// Button to press
	ENamedName StateName;	// Name of the button/state
};

enum EWRF_Options
{
	WRF_NoBob			= 1,
	WRF_NoSwitch		= 1 << 1,
	WRF_NoPrimary		= 1 << 2,
	WRF_NoSecondary		= 1 << 3,
	WRF_NoFire = WRF_NoPrimary | WRF_NoSecondary,
	WRF_AllowReload		= 1 << 4,
	WRF_AllowZoom		= 1 << 5,
	WRF_DisableSwitch	= 1 << 6,
	WRF_AllowUser1		= 1 << 7,
	WRF_AllowUser2		= 1 << 8,
	WRF_AllowUser3		= 1 << 9,
	WRF_AllowUser4		= 1 << 10,
};

// EXTERNAL FUNCTION PROTOTYPES --------------------------------------------

// PUBLIC FUNCTION PROTOTYPES ----------------------------------------------

// PRIVATE FUNCTION PROTOTYPES ---------------------------------------------

// EXTERNAL DATA DECLARATIONS ----------------------------------------------

// PUBLIC DATA DEFINITIONS -------------------------------------------------

// [SO] 1=Weapons states are all 1 tick
//		2=states with a function 1 tick, others 0 ticks.
CVAR(Int, sv_fastweapons, false, CVAR_SERVERINFO);

// PRIVATE DATA DEFINITIONS ------------------------------------------------

static FRandom pr_wpnreadysnd ("WpnReadySnd");

static const FGenericButtons ButtonChecks[] =
{
	{ WRF_AllowZoom,	WF_WEAPONZOOMOK,	BT_ZOOM,	NAME_Zoom },
	{ WRF_AllowReload,	WF_WEAPONRELOADOK,	BT_RELOAD,	NAME_Reload },
	{ WRF_AllowUser1,	WF_USER1OK,			BT_USER1,	NAME_User1 },
	{ WRF_AllowUser2,	WF_USER2OK,			BT_USER2,	NAME_User2 },
	{ WRF_AllowUser3,	WF_USER3OK,			BT_USER3,	NAME_User3 },
	{ WRF_AllowUser4,	WF_USER4OK,			BT_USER4,	NAME_User4 },
};

// CODE --------------------------------------------------------------------

//------------------------------------------------------------------------
//
//
//
//------------------------------------------------------------------------

IMPLEMENT_CLASS(DPSprite, false, true)

IMPLEMENT_POINTERS_START(DPSprite)
	IMPLEMENT_POINTER(Caller)
	IMPLEMENT_POINTER(Next)
IMPLEMENT_POINTERS_END

DEFINE_FIELD_NAMED(DPSprite, State, CurState)	// deconflict with same named type
DEFINE_FIELD(DPSprite, Caller)
DEFINE_FIELD(DPSprite, Next)
DEFINE_FIELD(DPSprite, Owner)
DEFINE_FIELD(DPSprite, Sprite)
DEFINE_FIELD(DPSprite, Frame)
DEFINE_FIELD(DPSprite, ID)
DEFINE_FIELD(DPSprite, processPending)
DEFINE_FIELD(DPSprite, x)
DEFINE_FIELD(DPSprite, y)
DEFINE_FIELD(DPSprite, oldx)
DEFINE_FIELD(DPSprite, oldy)
DEFINE_FIELD(DPSprite, firstTic)
DEFINE_FIELD(DPSprite, Tics)
DEFINE_FIELD(DPSprite, alpha)
DEFINE_FIELD(DPSprite, RenderStyle)
DEFINE_FIELD_BIT(DPSprite, Flags, bAddWeapon, PSPF_ADDWEAPON)
DEFINE_FIELD_BIT(DPSprite, Flags, bAddBob, PSPF_ADDBOB)
DEFINE_FIELD_BIT(DPSprite, Flags, bPowDouble, PSPF_POWDOUBLE)
DEFINE_FIELD_BIT(DPSprite, Flags, bCVarFast, PSPF_CVARFAST)
DEFINE_FIELD_BIT(DPSprite, Flags, bFlip, PSPF_FLIP)

//------------------------------------------------------------------------
//
//
//
//------------------------------------------------------------------------

DPSprite::DPSprite(player_t *owner, AActor *caller, int id)
: x(.0), y(.0),
  oldx(.0), oldy(.0),
  firstTic(true),
  Flags(0),
  Caller(caller),
  Owner(owner),
  Sprite(0),
  ID(id),
  processPending(true),
  alpha(1),
  RenderStyle(STYLE_Normal)
{
	DPSprite *prev = nullptr;
	DPSprite *next = Owner->psprites;
	while (next != nullptr && next->ID < ID)
	{
		prev = next;
		next = next->Next;
	}
	Next = next;
	GC::WriteBarrier(this, next);
	if (prev == nullptr)
	{
		Owner->psprites = this;
		GC::WriteBarrier(this);
	}
	else
	{
		prev->Next = this;
		GC::WriteBarrier(prev, this);
	}

	if (Next && Next->ID == ID && ID != 0)
		Next->Destroy(); // Replace it.

	if (Caller->IsKindOf(NAME_Weapon) || Caller->IsKindOf(RUNTIME_CLASS(APlayerPawn)))
		Flags = (PSPF_ADDWEAPON|PSPF_ADDBOB|PSPF_POWDOUBLE|PSPF_CVARFAST);
}

//------------------------------------------------------------------------
//
//
//
//------------------------------------------------------------------------

DPSprite *player_t::FindPSprite(int layer)
{
	if (layer == 0)
		return nullptr;

	DPSprite *pspr = psprites;
	while (pspr)
	{
		if (pspr->ID == layer)
			break;

		pspr = pspr->Next;
	}

	return pspr;
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, FindPSprite)	// the underscore is needed to get past the name mangler which removes the first clas name character to match the class representation (needs to be fixed in a later commit)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	PARAM_INT(id);
	ACTION_RETURN_OBJECT(self->FindPSprite((PSPLayers)id));
}


//------------------------------------------------------------------------
//
//
//
//------------------------------------------------------------------------

void P_SetPsprite(player_t *player, PSPLayers id, FState *state, bool pending)
{
	if (player == nullptr) return;
	player->GetPSprite(id)->SetState(state, pending);
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, SetPSprite)	// the underscore is needed to get past the name mangler which removes the first clas name character to match the class representation (needs to be fixed in a later commit)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	PARAM_INT(id);
	PARAM_POINTER(state, FState);
	PARAM_BOOL_DEF(pending);
	P_SetPsprite(self, (PSPLayers)id, state, pending);
	return 0;
}

DPSprite *player_t::GetPSprite(PSPLayers layer)
{
	AActor *oldcaller = nullptr;
	AActor *newcaller = nullptr;

	if (layer >= PSP_TARGETCENTER)
	{
		if (mo != nullptr)
		{
			newcaller = mo->FindInventory(NAME_PowerTargeter, true);
		}
	}
	else if (layer == PSP_STRIFEHANDS)
	{
		newcaller = mo;
	}
	else
	{
		newcaller = ReadyWeapon;
	}

	assert(newcaller != nullptr);

	DPSprite *pspr = FindPSprite(layer);
	if (pspr == nullptr)
	{
		pspr = new DPSprite(this, newcaller, layer);
	}
	else
	{
		oldcaller = pspr->Caller;
	}

	// Always update the caller here in case we switched weapon
	// or if the layer was being used by something else before.
	pspr->Caller = newcaller;

	if (newcaller != oldcaller)
	{ // Only reset stuff if this layer was created now or if it was being used before.
		if (layer >= PSP_TARGETCENTER)
		{ // The targeter layers were affected by those.
			pspr->Flags = (PSPF_CVARFAST|PSPF_POWDOUBLE);
		}
		else
		{
			pspr->Flags = (PSPF_ADDWEAPON|PSPF_ADDBOB|PSPF_CVARFAST|PSPF_POWDOUBLE);
		}
		if (layer == PSP_STRIFEHANDS)
		{
			// Some of the old hacks rely on this layer coming from the FireHands state.
			// This is the ONLY time a psprite's state is actually null.
			pspr->State = nullptr;
			pspr->y = WEAPONTOP;
		}

		pspr->firstTic = true;
	}

	return pspr;
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, GetPSprite)	// the underscore is needed to get past the name mangler which removes the first clas name character to match the class representation (needs to be fixed in a later commit)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	PARAM_INT(id);
	ACTION_RETURN_OBJECT(self->GetPSprite((PSPLayers)id));
}


//---------------------------------------------------------------------------
//
// PROC P_NewPspriteTick
//
//---------------------------------------------------------------------------

void DPSprite::NewTick()
{
	// This function should be called after the beginning of a tick, before any possible
	// prprite-event, or near the end, after any possible psprite event.
	// Because data is reset for every tick (which it must be) this has no impact on savegames.
	for (int i = 0; i < MAXPLAYERS; i++)
	{
		if (playeringame[i])
		{
			DPSprite *pspr = players[i].psprites;
			while (pspr)
			{
				pspr->processPending = true;
				pspr->ResetInterpolation();

				pspr = pspr->Next;
			}
		}
	}
}

//---------------------------------------------------------------------------
//
// PROC P_SetPsprite
//
//---------------------------------------------------------------------------

void DPSprite::SetState(FState *newstate, bool pending)
{
	if (ID == PSP_WEAPON)
	{ // A_WeaponReady will re-set these as needed
		Owner->WeaponState &= ~(WF_WEAPONREADY | WF_WEAPONREADYALT | WF_WEAPONBOBBING | WF_WEAPONSWITCHOK | WF_WEAPONRELOADOK | WF_WEAPONZOOMOK |
								WF_USER1OK | WF_USER2OK | WF_USER3OK | WF_USER4OK);
	}

	processPending = pending;

	do
	{
		if (newstate == nullptr)
		{ // Object removed itself.
			Destroy();
			return;
		}

		if (!(newstate->UseFlags & (SUF_OVERLAY|SUF_WEAPON)))	// Weapon and overlay are mostly the same, the main difference is that weapon states restrict the self pointer to class Actor.
		{
			auto so = FState::StaticFindStateOwner(newstate);
			Printf(TEXTCOLOR_RED "State %s.%d not flagged for use in overlays or weapons\n", so->TypeName.GetChars(), int(newstate - so->OwnedStates));
			State = nullptr;
			Destroy();
			return;
		}
		else if (!(newstate->UseFlags & SUF_WEAPON))
		{
			if (Caller->IsKindOf(NAME_Weapon))
			{
				auto so = FState::StaticFindStateOwner(newstate);
				Printf(TEXTCOLOR_RED "State %s.%d not flagged for use in weapons\n", so->TypeName.GetChars(), int(newstate - so->OwnedStates));
				State = nullptr;
				Destroy();
				return;
			}
		}

		State = newstate;

		if (newstate->sprite != SPR_FIXED)
		{ // okay to change sprite and/or frame
			if (!newstate->GetSameFrame())
			{ // okay to change frame
				Frame = newstate->GetFrame();
			}
			if (newstate->sprite != SPR_NOCHANGE)
			{ // okay to change sprite
				Sprite = newstate->sprite;
			}
		}

		Tics = newstate->GetTics(); // could be 0

		if (Flags & PSPF_CVARFAST)
		{
			if (sv_fastweapons == 2 && ID == PSP_WEAPON)
				Tics = newstate->ActionFunc == nullptr ? 0 : 1;
			else if (sv_fastweapons == 3)
				Tics = (newstate->GetTics() != 0);
			else if (sv_fastweapons)
				Tics = 1;		// great for producing decals :)
		}

		if (ID != PSP_FLASH)
		{ // It's still possible to set the flash layer's offsets with the action function.
			// Anything going through here cannot be reliably interpolated so this has to reset the interpolation coordinates if it changes the values.
			if (newstate->GetMisc1())
			{ // Set coordinates.
				oldx = x = newstate->GetMisc1();
			}
			if (newstate->GetMisc2())
			{
				oldy = y = newstate->GetMisc2();
			}
		}

		if (Owner->mo != nullptr)
		{
			FState *nextstate;
			FStateParamInfo stp = { newstate, STATE_Psprite, ID };
			if (newstate->ActionFunc != nullptr && newstate->ActionFunc->Unsafe)
			{
				// If an unsafe function (i.e. one that accesses user variables) is being detected, print a warning once and remove the bogus function. We may not call it because that would inevitably crash.
				auto owner = FState::StaticFindStateOwner(newstate);
				Printf(TEXTCOLOR_RED "Unsafe state call in state %s.%d to %s which accesses user variables. The action function has been removed from this state\n",
					owner->TypeName.GetChars(), int(newstate - owner->OwnedStates), newstate->ActionFunc->PrintableName.GetChars());
				newstate->ActionFunc = nullptr;
			}
			if (newstate->CallAction(Owner->mo, Caller, &stp, &nextstate))
			{
				// It's possible this call resulted in this very layer being replaced.
				if (ObjectFlags & OF_EuthanizeMe)
				{
					return;
				}
				if (nextstate != nullptr)
				{
					newstate = nextstate;
					Tics = 0;
					continue;
				}
				if (State == nullptr)
				{
					Destroy();
					return;
				}
			}
		}

		newstate = State->GetNextState();
	} while (!Tics); // An initial state of 0 could cycle through.

	return;
}

DEFINE_ACTION_FUNCTION(DPSprite, SetState)
{
	PARAM_SELF_PROLOGUE(DPSprite);
	PARAM_POINTER(state, FState);
	PARAM_BOOL_DEF(pending);
	self->SetState(state, pending);
	return 0;
}

//---------------------------------------------------------------------------
//
// PROC P_BringUpWeapon
//
// Starts bringing the pending weapon up from the bottom of the screen.
// This is only called to start the rising, not throughout it.
//
//---------------------------------------------------------------------------

void P_BringUpWeapon (player_t *player)
{
	AWeapon *weapon;

	if (player->PendingWeapon == WP_NOCHANGE)
	{
		if (player->ReadyWeapon != nullptr)
		{
			player->GetPSprite(PSP_WEAPON)->y = WEAPONTOP;
			P_SetPsprite(player, PSP_WEAPON, player->ReadyWeapon->GetReadyState());
		}
		return;
	}

	weapon = player->PendingWeapon;

	// If the player has a tome of power, use this weapon's powered up
	// version, if one is available.
	if (weapon != nullptr &&
		weapon->SisterWeapon &&
		weapon->SisterWeapon->WeaponFlags & WIF_POWERED_UP &&
		player->mo->FindInventory (PClass::FindActor(NAME_PowerWeaponLevel2), true))
	{
		weapon = weapon->SisterWeapon;
	}

	player->PendingWeapon = WP_NOCHANGE;
	player->ReadyWeapon = weapon;
	player->mo->weaponspecial = 0;

	if (weapon != nullptr)
	{
		if (weapon->UpSound)
		{
			S_Sound (player->mo, CHAN_WEAPON, weapon->UpSound, 1, ATTN_NORM);
		}
		player->refire = 0;

		player->GetPSprite(PSP_WEAPON)->y = player->cheats & CF_INSTANTWEAPSWITCH
			? WEAPONTOP : WEAPONBOTTOM;
		// make sure that the previous weapon's flash state is terminated.
		// When coming here from a weapon drop it may still be active.
		P_SetPsprite(player, PSP_FLASH, nullptr);
		P_SetPsprite(player, PSP_WEAPON, weapon->GetUpState());
	}
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, BringUpWeapon)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	P_BringUpWeapon(self);
	return 0;
}

//---------------------------------------------------------------------------
//
// PROC P_FireWeapon
//
//---------------------------------------------------------------------------

void P_FireWeapon (player_t *player, FState *state)
{
	AWeapon *weapon;

	// [SO] 9/2/02: People were able to do an awful lot of damage
	// when they were observers...
	if (player->Bot == nullptr && bot_observer)
	{
		return;
	}

	weapon = player->ReadyWeapon;
	if (weapon == nullptr || !weapon->CheckAmmo (AWeapon::PrimaryFire, true))
	{
		return;
	}

	player->WeaponState &= ~WF_WEAPONBOBBING;
	player->mo->PlayAttacking ();
	weapon->bAltFire = false;
	if (state == nullptr)
	{
		state = weapon->GetAtkState(!!player->refire);
	}
	P_SetPsprite(player, PSP_WEAPON, state);
	if (!(weapon->WeaponFlags & WIF_NOALERT))
	{
		P_NoiseAlert (player->mo, player->mo, false);
	}
}

//---------------------------------------------------------------------------
//
// PROC P_FireWeaponAlt
//
//---------------------------------------------------------------------------

void P_FireWeaponAlt (player_t *player, FState *state)
{
	AWeapon *weapon;

	// [SO] 9/2/02: People were able to do an awful lot of damage
	// when they were observers...
	if (player->Bot == nullptr && bot_observer)
	{
		return;
	}

	weapon = player->ReadyWeapon;
	if (weapon == nullptr || weapon->FindState(NAME_AltFire) == nullptr || !weapon->CheckAmmo (AWeapon::AltFire, true))
	{
		return;
	}

	player->WeaponState &= ~WF_WEAPONBOBBING;
	player->mo->PlayAttacking ();
	weapon->bAltFire = true;

	if (state == nullptr)
	{
		state = weapon->GetAltAtkState(!!player->refire);
	}

	P_SetPsprite(player, PSP_WEAPON, state);
	if (!(weapon->WeaponFlags & WIF_NOALERT))
	{
		P_NoiseAlert (player->mo, player->mo, false);
	}
}

//---------------------------------------------------------------------------
//
// PROC P_DropWeapon
//
// The player died, so put the weapon away.
//
//---------------------------------------------------------------------------

void P_DropWeapon (player_t *player)
{
	if (player == nullptr)
	{
		return;
	}
	// Since the weapon is dropping, stop blocking switching.
	player->WeaponState &= ~WF_DISABLESWITCH;
	if ((player->ReadyWeapon != nullptr) && (player->health > 0 || !(player->ReadyWeapon->WeaponFlags & WIF_NODEATHDESELECT)))
	{
		P_SetPsprite(player, PSP_WEAPON, player->ReadyWeapon->GetDownState());
	}
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, DropWeapon)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	P_DropWeapon(self);
	return 0;
}
//============================================================================
//
// P_BobWeapon
//
// [RH] Moved this out of A_WeaponReady so that the weapon can bob every
// tic and not just when A_WeaponReady is called. Not all weapons execute
// A_WeaponReady every tic, and it looks bad if they don't bob smoothly.
//
// [XA] Added new bob styles and exposed bob properties. Thanks, Ryan Cordell!
// [SP] Added new user option for bob speed
//
//============================================================================

void P_BobWeapon (player_t *player, float *x, float *y, double ticfrac)
{
	static float curbob;
	double xx[2], yy[2];

	AWeapon *weapon;
	float bobtarget;

	weapon = player->ReadyWeapon;

	if (weapon == nullptr || weapon->WeaponFlags & WIF_DONTBOB)
	{
		*x = *y = 0;
		return;
	}

	// [XA] Get the current weapon's bob properties.
	int bobstyle = weapon->BobStyle;
	float BobSpeed = (weapon->BobSpeed * 128);
	float Rangex = weapon->BobRangeX;
	float Rangey = weapon->BobRangeY;

	for (int i = 0; i < 2; i++)
	{
		// Bob the weapon based on movement speed. ([SP] And user's bob speed setting)
		FAngle angle = (BobSpeed * player->userinfo.GetWBobSpeed() * 35 /
			TICRATE*(level.time - 1 + i)) * (360.f / 8192.f);

		// [RH] Smooth transitions between bobbing and not-bobbing frames.
		// This also fixes the bug where you can "stick" a weapon off-center by
		// shooting it when it's at the peak of its swing.
		bobtarget = float((player->WeaponState & WF_WEAPONBOBBING) ? player->bob : 0.);
		if (curbob != bobtarget)
		{
			if (fabsf(bobtarget - curbob) <= 1)
			{
				curbob = bobtarget;
			}
			else
			{
				float zoom = MAX(1.f, fabsf(curbob - bobtarget) / 40);
				if (curbob > bobtarget)
				{
					curbob -= zoom;
				}
				else
				{
					curbob += zoom;
				}
			}
		}

		if (curbob != 0)
		{
			//[SP] Added in decorate player.viewbob checks
			float bobx = float(player->bob * Rangex * (float)player->mo->ViewBob);
			float boby = float(player->bob * Rangey * (float)player->mo->ViewBob);
			switch (bobstyle)
			{
			case AWeapon::BobNormal:
				xx[i] = bobx * angle.Cos();
				yy[i] = boby * fabsf(angle.Sin());
				break;

			case AWeapon::BobInverse:
				xx[i] = bobx*angle.Cos();
				yy[i] = boby * (1.f - fabsf(angle.Sin()));
				break;

			case AWeapon::BobAlpha:
				xx[i] = bobx * angle.Sin();
				yy[i] = boby * fabsf(angle.Sin());
				break;

			case AWeapon::BobInverseAlpha:
				xx[i] = bobx * angle.Sin();
				yy[i] = boby * (1.f - fabsf(angle.Sin()));
				break;

			case AWeapon::BobSmooth:
				xx[i] = bobx*angle.Cos();
				yy[i] = 0.5f * (boby * (1.f - ((angle * 2).Cos())));
				break;

			case AWeapon::BobInverseSmooth:
				xx[i] = bobx*angle.Cos();
				yy[i] = 0.5f * (boby * (1.f + ((angle * 2).Cos())));
			}
		}
		else
		{
			xx[i] = 0;
			yy[i] = 0;
		}
	}
	*x = (float)(xx[0] * (1. - ticfrac) + xx[1] * ticfrac);
	*y = (float)(yy[0] * (1. - ticfrac) + yy[1] * ticfrac);
}

//============================================================================
//
// PROC A_WeaponReady
//
// Readies a weapon for firing or bobbing with its three ancillary functions,
// DoReadyWeaponToSwitch(), DoReadyWeaponToFire() and DoReadyWeaponToBob().
// [XA] Added DoReadyWeaponToReload() and DoReadyWeaponToZoom()
//
//============================================================================

void DoReadyWeaponToSwitch (AActor *self, bool switchable)
{
	// Prepare for switching action.
	player_t *player;
	if (self && (player = self->player))
	{
		if (switchable)
		{
			player->WeaponState |= WF_WEAPONSWITCHOK | WF_REFIRESWITCHOK;
		}
		else
		{
			// WF_WEAPONSWITCHOK is automatically cleared every tic by P_SetPsprite().
			player->WeaponState &= ~WF_REFIRESWITCHOK;
		}
	}
}

void DoReadyWeaponDisableSwitch (AActor *self, INTBOOL disable)
{
	// Discard all switch attempts?
	player_t *player;
	if (self && (player = self->player))
	{
		if (disable)
		{
			player->WeaponState |= WF_DISABLESWITCH;
			player->WeaponState &= ~WF_REFIRESWITCHOK;
		}
		else
		{
			player->WeaponState &= ~WF_DISABLESWITCH;
		}
	}
}

void DoReadyWeaponToFire (AActor *self, bool prim, bool alt)
{
	player_t *player;
	AWeapon *weapon;

	if (!self || !(player = self->player) || !(weapon = player->ReadyWeapon))
	{
		return;
	}

	// Change player from attack state
	if (self->InStateSequence(self->state, self->MissileState) ||
		self->InStateSequence(self->state, self->MeleeState))
	{
		static_cast<APlayerPawn *>(self)->PlayIdle ();
	}

	// Play ready sound, if any.
	if (weapon->ReadySound && player->GetPSprite(PSP_WEAPON)->GetState() == weapon->FindState(NAME_Ready))
	{
		if (!(weapon->WeaponFlags & WIF_READYSNDHALF) || pr_wpnreadysnd() < 128)
		{
			S_Sound (self, CHAN_WEAPON, weapon->ReadySound, 1, ATTN_NORM);
		}
	}

	// Prepare for firing action.
	player->WeaponState |= ((prim ? WF_WEAPONREADY : 0) | (alt ? WF_WEAPONREADYALT : 0));
	return;
}

void DoReadyWeaponToBob (AActor *self)
{
	if (self && self->player && self->player->ReadyWeapon)
	{
		// Prepare for bobbing action.
		self->player->WeaponState |= WF_WEAPONBOBBING;
		self->player->GetPSprite(PSP_WEAPON)->x = 0;
		self->player->GetPSprite(PSP_WEAPON)->y = WEAPONTOP;
	}
}

void DoReadyWeaponToGeneric(AActor *self, int paramflags)
{
	int flags = 0;

	for (size_t i = 0; i < countof(ButtonChecks); ++i)
	{
		if (paramflags & ButtonChecks[i].ReadyFlag)
		{
			flags |= ButtonChecks[i].StateFlag;
		}
	}
	if (self != NULL && self->player != NULL)
	{
		self->player->WeaponState |= flags;
	}
}

// This function replaces calls to A_WeaponReady in other codepointers.
void DoReadyWeapon(AActor *self)
{
	DoReadyWeaponToBob(self);
	DoReadyWeaponToFire(self);
	DoReadyWeaponToSwitch(self);
	DoReadyWeaponToGeneric(self, ~0);
}

DEFINE_ACTION_FUNCTION(AStateProvider, A_WeaponReady)
{
	PARAM_ACTION_PROLOGUE(AStateProvider);
	PARAM_INT_DEF(flags);

													DoReadyWeaponToSwitch(self, !(flags & WRF_NoSwitch));
	if ((flags & WRF_NoFire) != WRF_NoFire)			DoReadyWeaponToFire(self, !(flags & WRF_NoPrimary), !(flags & WRF_NoSecondary));
	if (!(flags & WRF_NoBob))						DoReadyWeaponToBob(self);
													DoReadyWeaponToGeneric(self, flags);
	DoReadyWeaponDisableSwitch(self, flags & WRF_DisableSwitch);
	return 0;
}

//---------------------------------------------------------------------------
//
// PROC P_CheckWeaponFire
//
// The player can fire the weapon.
// [RH] This was in A_WeaponReady before, but that only works well when the
// weapon's ready frames have a one tic delay.
//
//---------------------------------------------------------------------------

void P_CheckWeaponFire (player_t *player)
{
	AWeapon *weapon = player->ReadyWeapon;

	if (weapon == NULL)
		return;

	// Check for fire. Some weapons do not auto fire.
	if ((player->WeaponState & WF_WEAPONREADY) && (player->cmd.ucmd.buttons & BT_ATTACK))
	{
		if (!player->attackdown || !(weapon->WeaponFlags & WIF_NOAUTOFIRE))
		{
			player->attackdown = true;
			P_FireWeapon (player, NULL);
			return;
		}
	}
	else if ((player->WeaponState & WF_WEAPONREADYALT) && (player->cmd.ucmd.buttons & BT_ALTATTACK))
	{
		if (!player->attackdown || !(weapon->WeaponFlags & WIF_NOAUTOFIRE))
		{
			player->attackdown = true;
			P_FireWeaponAlt (player, NULL);
			return;
		}
	}
	else
	{
		player->attackdown = false;
	}
}

//---------------------------------------------------------------------------
//
// PROC P_CheckWeaponSwitch
//
// The player can change to another weapon at this time.
// [GZ] This was cut from P_CheckWeaponFire.
//
//---------------------------------------------------------------------------

void P_CheckWeaponSwitch (player_t *player)
{
	if (player == NULL)
	{
		return;
	}
	if ((player->WeaponState & WF_DISABLESWITCH) || // Weapon changing has been disabled.
		player->morphTics != 0)					// Morphed classes cannot change weapons.
	{ // ...so throw away any pending weapon requests.
		player->PendingWeapon = WP_NOCHANGE;
	}

	// Put the weapon away if the player has a pending weapon or has died, and
	// we're at a place in the state sequence where dropping the weapon is okay.
	if ((player->PendingWeapon != WP_NOCHANGE || player->health <= 0) &&
		player->WeaponState & WF_WEAPONSWITCHOK)
	{
		P_DropWeapon(player);
	}
}

//---------------------------------------------------------------------------
//
// PROC P_CheckWeaponButtons
//
// Check extra button presses for weapons.
//
//---------------------------------------------------------------------------

static void P_CheckWeaponButtons (player_t *player)
{
	if (player->Bot == nullptr && bot_observer)
	{
		return;
	}
	AWeapon *weapon = player->ReadyWeapon;
	if (weapon == nullptr)
	{
		return;
	}
	// The button checks are ordered by precedence. The first one to match a
	// button press and affect a state change wins.
	for (size_t i = 0; i < countof(ButtonChecks); ++i)
	{
		if ((player->WeaponState & ButtonChecks[i].StateFlag) &&
			(player->cmd.ucmd.buttons & ButtonChecks[i].ButtonFlag))
		{
			FState *state = weapon->GetStateForButtonName(ButtonChecks[i].StateName);
			// [XA] don't change state if still null, so if the modder
			// sets WRF_xxx to true but forgets to define the corresponding
			// state, the weapon won't disappear. ;)
			if (state != nullptr)
			{
				P_SetPsprite(player, PSP_WEAPON, state);
				return;
			}
		}
	}
}

//---------------------------------------------------------------------------
//
// PROC A_ReFire
//
// The player can re-fire the weapon without lowering it entirely.
//
//---------------------------------------------------------------------------

DEFINE_ACTION_FUNCTION(AStateProvider, A_ReFire)
{
	PARAM_ACTION_PROLOGUE(AStateProvider);
	PARAM_STATE_ACTION_DEF(state);
	A_ReFire(self, state);
	return 0;
}

void A_ReFire(AActor *self, FState *state)
{
	player_t *player = self->player;
	bool pending;

	if (NULL == player)
	{
		return;
	}
	pending = player->PendingWeapon != WP_NOCHANGE && (player->WeaponState & WF_REFIRESWITCHOK);
	if ((player->cmd.ucmd.buttons & BT_ATTACK)
		&& !player->ReadyWeapon->bAltFire && !pending && player->health > 0)
	{
		player->refire++;
		P_FireWeapon (player, state);
	}
	else if ((player->cmd.ucmd.buttons & BT_ALTATTACK)
		&& player->ReadyWeapon->bAltFire && !pending && player->health > 0)
	{
		player->refire++;
		P_FireWeaponAlt (player, state);
	}
	else
	{
		player->refire = 0;
		player->ReadyWeapon->CheckAmmo (player->ReadyWeapon->bAltFire
			? AWeapon::AltFire : AWeapon::PrimaryFire, true);
	}
}


//---------------------------------------------------------------------------
//
// PROC A_OverlayOffset
//
//---------------------------------------------------------------------------
enum WOFFlags
{
	WOF_KEEPX =		1,
	WOF_KEEPY =		1 << 1,
	WOF_ADD =		1 << 2,
	WOF_INTERPOLATE = 1 << 3,
};

void A_OverlayOffset(AActor *self, int layer, double wx, double wy, int flags)
{
	if ((flags & WOF_KEEPX) && (flags & WOF_KEEPY))
	{
		return;
	}

	player_t *player = self->player;
	DPSprite *psp;

	if (player)
	{
		psp = player->FindPSprite(layer);

		if (psp == nullptr)
			return;

		if (!(flags & WOF_KEEPX))
		{
			if (flags & WOF_ADD)
			{
				psp->x += wx;
			}
			else
			{
				psp->x = wx;
				if (!(flags & WOF_INTERPOLATE)) psp->oldx = psp->x;
			}
		}
		if (!(flags & WOF_KEEPY))
		{
			if (flags & WOF_ADD)
			{
				psp->y += wy;
			}
			else
			{
				psp->y = wy;
				if (!(flags & WOF_INTERPOLATE)) psp->oldy = psp->y;
			}
		}
	}
}

DEFINE_ACTION_FUNCTION(AActor, A_OverlayOffset)
{
	PARAM_ACTION_PROLOGUE(AActor);
	PARAM_INT_DEF(layer)
	PARAM_FLOAT_DEF(wx)	
	PARAM_FLOAT_DEF(wy)	
	PARAM_INT_DEF(flags)
	A_OverlayOffset(self, ((layer != 0) ? layer : stateinfo->mPSPIndex), wx, wy, flags);
	return 0;
}

DEFINE_ACTION_FUNCTION(AActor, A_WeaponOffset)
{
	PARAM_SELF_PROLOGUE(AActor);
	PARAM_FLOAT_DEF(wx)	
	PARAM_FLOAT_DEF(wy)	
	PARAM_INT_DEF(flags)
	A_OverlayOffset(self, PSP_WEAPON, wx, wy, flags);
	return 0;
}

//---------------------------------------------------------------------------
//
// PROC A_OverlayFlags
//
//---------------------------------------------------------------------------

DEFINE_ACTION_FUNCTION(AActor, A_OverlayFlags)
{
	PARAM_ACTION_PROLOGUE(AActor);
	PARAM_INT(layer);
	PARAM_INT(flags);
	PARAM_BOOL(set);

	if (!ACTION_CALL_FROM_PSPRITE())
		return 0;

	DPSprite *pspr = self->player->FindPSprite(((layer != 0) ? layer : stateinfo->mPSPIndex));

	if (pspr == nullptr)
		return 0;

	if (set)
		pspr->Flags |= flags;
	else
		pspr->Flags &= ~flags;

	return 0;
}

//---------------------------------------------------------------------------
//
// PROC OverlayX/Y
// Action function to return the X/Y of an overlay.
//---------------------------------------------------------------------------

static double GetOverlayPosition(AActor *self, int layer, bool gety)
{
	if (layer)
	{
		DPSprite *pspr = self->player->FindPSprite(layer);

		if (pspr != nullptr)
		{
			return gety ? (pspr->y) : (pspr->x);
		}
	}
	return 0.;
}

DEFINE_ACTION_FUNCTION(AActor, OverlayX)
{
	PARAM_ACTION_PROLOGUE(AActor);
	PARAM_INT_DEF(layer);

	if (ACTION_CALL_FROM_PSPRITE())
	{
		double res = GetOverlayPosition(self, ((layer != 0) ? layer : stateinfo->mPSPIndex), false);
		ACTION_RETURN_FLOAT(res);	
	}
	ACTION_RETURN_FLOAT(0.);
}

DEFINE_ACTION_FUNCTION(AActor, OverlayY)
{
	PARAM_ACTION_PROLOGUE(AActor);
	PARAM_INT_DEF(layer);

	if (ACTION_CALL_FROM_PSPRITE())
	{
		double res = GetOverlayPosition(self, ((layer != 0) ? layer : stateinfo->mPSPIndex), true);
		ACTION_RETURN_FLOAT(res);
	}
	ACTION_RETURN_FLOAT(0.);
}

//---------------------------------------------------------------------------
//
// PROC OverlayID
// Because non-action functions cannot acquire the ID of the overlay...
//---------------------------------------------------------------------------

DEFINE_ACTION_FUNCTION(AActor, OverlayID)
{
	PARAM_ACTION_PROLOGUE(AActor);

	if (ACTION_CALL_FROM_PSPRITE())
	{
		ACTION_RETURN_INT(stateinfo->mPSPIndex);
	}
	ACTION_RETURN_INT(0);
}

//---------------------------------------------------------------------------
//
// PROC A_OverlayAlpha
// Sets the alpha of an overlay.
//---------------------------------------------------------------------------

DEFINE_ACTION_FUNCTION(AActor, A_OverlayAlpha)
{
	PARAM_ACTION_PROLOGUE(AActor);
	PARAM_INT(layer);
	PARAM_FLOAT(alph);

	if (ACTION_CALL_FROM_PSPRITE())
	{
		DPSprite *pspr = self->player->FindPSprite((layer != 0) ? layer : stateinfo->mPSPIndex);

		if (pspr != nullptr)
			pspr->alpha = clamp<double>(alph, 0.0, 1.0);
	}
	return 0;
}

// NON-ACTION function to get the overlay alpha of a layer.
DEFINE_ACTION_FUNCTION(AActor, OverlayAlpha)
{
	PARAM_ACTION_PROLOGUE(AActor);
	PARAM_INT_DEF(layer);

	if (ACTION_CALL_FROM_PSPRITE())
	{
		DPSprite *pspr = self->player->FindPSprite((layer != 0) ? layer : stateinfo->mPSPIndex);

		if (pspr != nullptr)
		{
			ACTION_RETURN_FLOAT(pspr->alpha);
		}
	}
	ACTION_RETURN_FLOAT(0.0);
}

//---------------------------------------------------------------------------
//
// PROC A_OverlayRenderStyle
//
//---------------------------------------------------------------------------

DEFINE_ACTION_FUNCTION(AActor, A_OverlayRenderStyle)
{
	PARAM_ACTION_PROLOGUE(AActor);
	PARAM_INT(layer);
	PARAM_INT(style);

	if (ACTION_CALL_FROM_PSPRITE())
	{
		DPSprite *pspr = self->player->FindPSprite((layer != 0) ? layer : stateinfo->mPSPIndex);

		if (pspr == nullptr || style >= STYLE_Count || style < 0)
			return 0;

		pspr->RenderStyle = style;
	}
	return 0;
}

//---------------------------------------------------------------------------
//
// PROC A_Overlay
//
//---------------------------------------------------------------------------

DEFINE_ACTION_FUNCTION(AActor, A_Overlay)
{
	PARAM_ACTION_PROLOGUE(AActor);
	PARAM_INT		(layer);
	PARAM_STATE_ACTION_DEF(state);
	PARAM_BOOL_DEF(dontoverride);

	player_t *player = self->player;

	if (player == nullptr || (dontoverride && (player->FindPSprite(layer) != nullptr)))
	{
		ACTION_RETURN_BOOL(false);
	}

	DPSprite *pspr;
	pspr = new DPSprite(player, stateowner, layer);
	pspr->SetState(state);
	ACTION_RETURN_BOOL(true);
}

DEFINE_ACTION_FUNCTION(AActor, A_ClearOverlays)
{
	PARAM_SELF_PROLOGUE(AActor);
	PARAM_INT_DEF(start);
	PARAM_INT_DEF(stop);
	PARAM_BOOL_DEF(safety)

	if (self->player == nullptr)
		ACTION_RETURN_INT(0);

	if (!start && !stop)
	{
		start = INT_MIN;
		stop = safety ? PSP_TARGETCENTER - 1 : INT_MAX;
	}

	unsigned int count = 0;
	int id;

	for (DPSprite *pspr = self->player->psprites; pspr != nullptr; pspr = pspr->GetNext())
	{
		id = pspr->GetID();

		if (id < start || id == 0)
			continue;
		else if (id > stop)
			break;

		if (safety)
		{
			if (id >= PSP_TARGETCENTER)
				break;
			else if (id == PSP_STRIFEHANDS || id == PSP_WEAPON || id == PSP_FLASH)
				continue;
		}

		pspr->SetState(nullptr);
		count++;
	}

	ACTION_RETURN_INT(count);
}

//
// WEAPON ATTACKS
//

//
// P_BulletSlope
// Sets a slope so a near miss is at aproximately
// the height of the intended target
//

DAngle P_BulletSlope (AActor *mo, FTranslatedLineTarget *pLineTarget, int aimflags)
{
	static const double angdiff[3] = { -5.625f, 5.625f, 0 };
	int i;
	DAngle an;
	DAngle pitch;
	FTranslatedLineTarget scratch;

	if (pLineTarget == NULL) pLineTarget = &scratch;
	// see which target is to be aimed at
	i = 2;
	do
	{
		an = mo->Angles.Yaw + angdiff[i];
		pitch = P_AimLineAttack (mo, an, 16.*64, pLineTarget, 0., aimflags);

		if (mo->player != NULL &&
			level.IsFreelookAllowed() &&
			mo->player->userinfo.GetAimDist() <= 0.5)
		{
			break;
		}
	} while (pLineTarget->linetarget == NULL && --i >= 0);

	return pitch;
}

DEFINE_ACTION_FUNCTION(AActor, BulletSlope)
{
	PARAM_SELF_PROLOGUE(AActor);
	PARAM_POINTER_DEF(t, FTranslatedLineTarget);
	PARAM_INT_DEF(aimflags);
	ACTION_RETURN_FLOAT(P_BulletSlope(self, t, aimflags).Degrees);
}

//------------------------------------------------------------------------
//
// PROC P_SetupPsprites
//
// Called at start of level for each player
//
//------------------------------------------------------------------------

void P_SetupPsprites(player_t *player, bool startweaponup)
{
	// Remove all psprites
	player->DestroyPSprites();

	// Spawn the ready weapon
	player->PendingWeapon = !startweaponup ? player->ReadyWeapon : WP_NOCHANGE;
	P_BringUpWeapon (player);
}

//------------------------------------------------------------------------
//
// PROC P_MovePsprites
//
// Called every tic by player thinking routine
//
//------------------------------------------------------------------------

void player_t::TickPSprites()
{
	DPSprite *pspr = psprites;
	while (pspr)
	{
		// Destroy the psprite if it's from a weapon that isn't currently selected by the player
		// or if it's from an inventory item that the player no longer owns. 
		if ((pspr->Caller == nullptr ||
			(pspr->Caller->IsKindOf(RUNTIME_CLASS(AInventory)) && barrier_cast<AInventory *>(pspr->Caller)->Owner != pspr->Owner->mo) ||
			(pspr->Caller->IsKindOf(NAME_Weapon) && pspr->Caller != pspr->Owner->ReadyWeapon)))
		{
			pspr->Destroy();
		}
		else
		{
			pspr->Tick();
		}

		pspr = pspr->Next;
	}

	if ((health > 0) || (ReadyWeapon != nullptr && !(ReadyWeapon->WeaponFlags & WIF_NODEATHINPUT)))
	{
		if (ReadyWeapon == nullptr)
		{
			if (PendingWeapon != WP_NOCHANGE)
				P_BringUpWeapon(this);
		}
		else
		{
			P_CheckWeaponSwitch(this);
			if (WeaponState & (WF_WEAPONREADY | WF_WEAPONREADYALT))
			{
				P_CheckWeaponFire(this);
			}
			// Check custom buttons
			P_CheckWeaponButtons(this);
		}
	}
}

//------------------------------------------------------------------------
//
//
//
//------------------------------------------------------------------------

void DPSprite::Tick()
{
	if (processPending)
	{
		// drop tic count and possibly change state
		if (Tics != -1)	// a -1 tic count never changes
		{
			Tics--;

			// [BC] Apply double firing speed.
			if ((Flags & PSPF_POWDOUBLE) && Tics && (Owner->mo->FindInventory (PClass::FindActor(NAME_PowerDoubleFiringSpeed), true)))
				Tics--;

			if (!Tics)
				SetState(State->GetNextState());
		}
	}
}

//------------------------------------------------------------------------
//
//
//
//------------------------------------------------------------------------

void DPSprite::Serialize(FSerializer &arc)
{
	Super::Serialize(arc);

	arc("next", Next)
		("caller", Caller)
		("owner", Owner)
		("flags", Flags)
		("state", State)
		("tics", Tics)
		.Sprite("sprite", Sprite, nullptr)
		("frame", Frame)
		("id", ID)
		("x", x)
		("y", y)
		("oldx", oldx)
		("oldy", oldy)
		("alpha", alpha)
		("renderstyle", RenderStyle);
}

//------------------------------------------------------------------------
//
//
//
//------------------------------------------------------------------------

void player_t::DestroyPSprites()
{
	DPSprite *pspr = psprites;
	psprites = nullptr;
	while (pspr)
	{
		DPSprite *next = pspr->Next;
		pspr->Next = nullptr;
		pspr->Destroy();
		pspr = next;
	}
}

//------------------------------------------------------------------------------------
//
// Setting a random flash like some of Doom's weapons can easily crash when the
// definition is overridden incorrectly so let's check that the state actually exists.
// Be aware though that this will not catch all DEHACKED related problems. But it will
// find all DECORATE related ones.
//
//------------------------------------------------------------------------------------

void P_SetSafeFlash(AWeapon *weapon, player_t *player, FState *flashstate, int index)
{
	if (flashstate != nullptr)
	{
		PClassActor *cls = weapon->GetClass();
		while (cls != RUNTIME_CLASS(AWeapon))
		{
			if (flashstate >= cls->OwnedStates && flashstate < cls->OwnedStates + cls->NumOwnedStates)
			{
				// The flash state belongs to this class.
				// Now let's check if the actually wanted state does also
				if (flashstate + index < cls->OwnedStates + cls->NumOwnedStates)
				{
					// we're ok so set the state
					P_SetPsprite(player, PSP_FLASH, flashstate + index, true);
					return;
				}
				else
				{
					// oh, no! The state is beyond the end of the state table so use the original flash state.
					P_SetPsprite(player, PSP_FLASH, flashstate, true);
					return;
				}
			}
			// try again with parent class
			cls = static_cast<PClassActor *>(cls->ParentClass);
		}
		// if we get here the state doesn't seem to belong to any class in the inheritance chain
		// This can happen with Dehacked if the flash states are remapped. 
		// The only way to check this would be to go through all Dehacked modifiable actors, convert
		// their states into a single flat array and find the correct one.
		// Rather than that, just check to make sure it belongs to something.
		if (FState::StaticFindStateOwner(flashstate + index) == NULL)
		{ // Invalid state. With no index offset, it should at least be valid.
			index = 0;
		}
	}
	P_SetPsprite(player, PSP_FLASH, flashstate + index, true);
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, SetSafeFlash)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	PARAM_OBJECT_NOT_NULL(weapon, AWeapon);
	PARAM_POINTER(state, FState);
	PARAM_INT(index);
	P_SetSafeFlash(weapon, self, state, index);
	return 0;
}

//------------------------------------------------------------------------
//
//
//
//------------------------------------------------------------------------

void DPSprite::OnDestroy()
{
	// Do not crash if this gets called on partially initialized objects.
	if (Owner != nullptr && Owner->psprites != nullptr)
	{
		if (Owner->psprites != this)
		{
			DPSprite *prev = Owner->psprites;
			while (prev != nullptr && prev->Next != this)
				prev = prev->Next;

			if (prev != nullptr && prev->Next == this)
			{
				prev->Next = Next;
				GC::WriteBarrier(prev, Next);
			}
		}
		else
		{
			Owner->psprites = Next;
			GC::WriteBarrier(Next);
		}
	}
	Super::OnDestroy();
}

//------------------------------------------------------------------------
//
//
//
//------------------------------------------------------------------------

ADD_STAT(psprites)
{
	FString out;
	DPSprite *pspr;
	for (int i = 0; i < MAXPLAYERS; i++)
	{
		if (!playeringame[i])
			continue;

		out.AppendFormat("[psprites] player: %d | layers: ", i);

		pspr = players[i].psprites;
		while (pspr)
		{
			out.AppendFormat("%d, ", pspr->GetID());

			pspr = pspr->GetNext();
		}

		out.AppendFormat("\n");
	}

	return out;
}
