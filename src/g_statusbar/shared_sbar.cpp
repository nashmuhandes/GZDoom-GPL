/*
** shared_sbar.cpp
** Base status bar implementation
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
*/

#include <assert.h>

#include "templates.h"
#include "sbar.h"
#include "c_cvars.h"
#include "c_dispatch.h"
#include "c_console.h"
#include "v_video.h"
#include "m_swap.h"
#include "w_wad.h"
#include "v_text.h"
#include "s_sound.h"
#include "gi.h"
#include "doomstat.h"
#include "g_level.h"
#include "d_net.h"
#include "colormatcher.h"
#include "v_palette.h"
#include "d_player.h"
#include "serializer.h"
#include "gstrings.h"
#include "r_utility.h"
#include "cmdlib.h"
#include "g_levellocals.h"
#include "virtual.h"

#include "../version.h"

#define XHAIRSHRINKSIZE		(1./18)
#define XHAIRPICKUPSIZE		(2+XHAIRSHRINKSIZE)
#define POWERUPICONSIZE		32

IMPLEMENT_CLASS(DBaseStatusBar, false, true)

IMPLEMENT_POINTERS_START(DBaseStatusBar)
	IMPLEMENT_POINTER(Messages[0])
	IMPLEMENT_POINTER(Messages[1])
	IMPLEMENT_POINTER(Messages[2])
IMPLEMENT_POINTERS_END

EXTERN_CVAR (Bool, am_showmonsters)
EXTERN_CVAR (Bool, am_showsecrets)
EXTERN_CVAR (Bool, am_showitems)
EXTERN_CVAR (Bool, am_showtime)
EXTERN_CVAR (Bool, am_showtotaltime)
EXTERN_CVAR (Bool, noisedebug)
EXTERN_CVAR (Int, con_scaletext)

int active_con_scaletext();

DBaseStatusBar *StatusBar;

extern int setblocks;

int gST_X, gST_Y;
int SB_state = 3;

FTexture *CrosshairImage;
static int CrosshairNum;

// [RH] Base blending values (for e.g. underwater)
int BaseBlendR, BaseBlendG, BaseBlendB;
float BaseBlendA;

CVAR (Int, paletteflash, 0, CVAR_ARCHIVE)
CVAR (Flag, pf_hexenweaps,	paletteflash, PF_HEXENWEAPONS)
CVAR (Flag, pf_poison,		paletteflash, PF_POISON)
CVAR (Flag, pf_ice,			paletteflash, PF_ICE)
CVAR (Flag, pf_hazard,		paletteflash, PF_HAZARD)

// Stretch status bar to full screen width?
CUSTOM_CVAR (Bool, st_scale, true, CVAR_ARCHIVE)
{
	if (StatusBar)
	{
		StatusBar->SetScaled (self);
		setsizeneeded = true;
	}
}

CVAR (Int, crosshair, 0, CVAR_ARCHIVE)
CVAR (Bool, crosshairforce, false, CVAR_ARCHIVE)
CVAR (Color, crosshaircolor, 0xff0000, CVAR_ARCHIVE);
CVAR (Bool, crosshairhealth, true, CVAR_ARCHIVE);
CVAR (Float, crosshairscale, 1.0, CVAR_ARCHIVE);
CVAR (Bool, crosshairgrow, false, CVAR_ARCHIVE);
CUSTOM_CVAR(Int, am_showmaplabel, 2, CVAR_ARCHIVE)
{
	if (self < 0 || self > 2) self = 2;
}

CVAR (Bool, idmypos, false, 0);
CVAR(Float, underwater_fade_scalar, 1.0f, CVAR_ARCHIVE) // [Nash] user-settable underwater blend intensity

//---------------------------------------------------------------------------
//
// Format the map name, include the map label if wanted
//
//---------------------------------------------------------------------------

void ST_FormatMapName(FString &mapname, const char *mapnamecolor)
{
	cluster_info_t *cluster = FindClusterInfo (level.cluster);
	bool ishub = (cluster != NULL && (cluster->flags & CLUSTER_HUB));

	if (am_showmaplabel == 1 || (am_showmaplabel == 2 && !ishub))
	{
		mapname << level.MapName << ": ";
	}
	mapname << mapnamecolor << level.LevelName;
}

//---------------------------------------------------------------------------
//
// Load crosshair definitions
//
//---------------------------------------------------------------------------

void ST_LoadCrosshair(bool alwaysload)
{
	int num = 0;
	char name[16], size;

	if (!crosshairforce &&
		players[consoleplayer].camera != NULL &&
		players[consoleplayer].camera->player != NULL &&
		players[consoleplayer].camera->player->ReadyWeapon != NULL)
	{
		num = players[consoleplayer].camera->player->ReadyWeapon->Crosshair;
	}
	if (num == 0)
	{
		num = crosshair;
	}
	if (!alwaysload && CrosshairNum == num && CrosshairImage != NULL)
	{ // No change.
		return;
	}

	if (CrosshairImage != NULL)
	{
		CrosshairImage->Unload ();
	}
	if (num == 0)
	{
		CrosshairNum = 0;
		CrosshairImage = NULL;
		return;
	}
	if (num < 0)
	{
		num = -num;
	}
	size = (SCREENWIDTH < 640) ? 'S' : 'B';

	mysnprintf (name, countof(name), "XHAIR%c%d", size, num);
	FTextureID texid = TexMan.CheckForTexture(name, FTexture::TEX_MiscPatch, FTextureManager::TEXMAN_TryAny | FTextureManager::TEXMAN_ShortNameOnly);
	if (!texid.isValid())
	{
		mysnprintf (name, countof(name), "XHAIR%c1", size);
		texid = TexMan.CheckForTexture(name, FTexture::TEX_MiscPatch, FTextureManager::TEXMAN_TryAny | FTextureManager::TEXMAN_ShortNameOnly);
		if (!texid.isValid())
		{
			texid = TexMan.CheckForTexture("XHAIRS1", FTexture::TEX_MiscPatch, FTextureManager::TEXMAN_TryAny | FTextureManager::TEXMAN_ShortNameOnly);
		}
	}
	CrosshairNum = num;
	CrosshairImage = TexMan[texid];
}

//---------------------------------------------------------------------------
//
// ST_Clear
//
//---------------------------------------------------------------------------

void ST_Clear()
{
	if (StatusBar != NULL)
	{
		StatusBar->Destroy();
		StatusBar = NULL;
	}
	CrosshairImage = NULL;
	CrosshairNum = 0;
}

//---------------------------------------------------------------------------
//
// ST_SetNeedRefresh
//
//---------------------------------------------------------------------------

void ST_SetNeedRefresh()
{
	SB_state = (StatusBar == NULL || screen == NULL) ? 0 : screen->GetPageCount();
}

//---------------------------------------------------------------------------
//
// Constructor
//
//---------------------------------------------------------------------------

DBaseStatusBar::DBaseStatusBar (int reltop, int hres, int vres)
{
	CompleteBorder = false;
	Centering = false;
	FixedOrigin = false;
	CrosshairSize = 1.;
	RelTop = reltop;
	memset(Messages, 0, sizeof(Messages));
	Displacement = 0;
	CPlayer = NULL;
	ShowLog = false;
	HorizontalResolution = hres;
	VerticalResolution = vres;

	SetScaled (st_scale);
}

//---------------------------------------------------------------------------
//
// PROP Destroy
//
//---------------------------------------------------------------------------

void DBaseStatusBar::OnDestroy ()
{
	for (size_t i = 0; i < countof(Messages); ++i)
	{
		DHUDMessage *msg = Messages[i];
		while (msg)
		{
			DHUDMessage *next = msg->Next;
			msg->Destroy();
			msg = next;
		}
		Messages[i] = NULL;
	}
	Super::OnDestroy();
}

//---------------------------------------------------------------------------
//
// PROC SetScaled
//
//---------------------------------------------------------------------------

//[BL] Added force argument to have forcescaled mean forcescaled.
// - Also, if the VerticalResolution is something other than the default (200)
//   We should always obey the value of scale.
void DBaseStatusBar::SetScaled (bool scale, bool force)
{
	Scaled = (RelTop != 0 || force) && ((SCREENWIDTH != 320 || HorizontalResolution != 320) && scale);

	if (!Scaled)
	{
		ST_X = (SCREENWIDTH - HorizontalResolution) / 2;
		ST_Y = SCREENHEIGHT - RelTop;
		gST_Y = ST_Y;
		if (RelTop > 0)
		{
			Displacement = double((ST_Y * VerticalResolution / SCREENHEIGHT) - (VerticalResolution - RelTop))/RelTop;
		}
		else
		{
			Displacement = 0;
		}
	}
	else
	{
		ST_X = 0;
		ST_Y = VerticalResolution - RelTop;
		float aspect = ActiveRatio(SCREENWIDTH, SCREENHEIGHT);
		if (!AspectTallerThanWide(aspect))
		{ // Normal resolution
			gST_Y = Scale (ST_Y, SCREENHEIGHT, VerticalResolution);
		}
		else
		{ // 5:4 resolution
			gST_Y = Scale(ST_Y - VerticalResolution/2, SCREENHEIGHT*3, Scale(VerticalResolution, AspectBaseHeight(aspect), 200)) + SCREENHEIGHT/2
				+ (SCREENHEIGHT - SCREENHEIGHT * AspectMultiplier(aspect) / 48) / 2;
		}
		Displacement = 0;
	}
	gST_X = ST_X;
	ST_SetNeedRefresh();
}

//---------------------------------------------------------------------------
//
// PROC AttachToPlayer
//
//---------------------------------------------------------------------------

void DBaseStatusBar::AttachToPlayer (player_t *player)
{
	CPlayer = player;
	ST_SetNeedRefresh();
}

//---------------------------------------------------------------------------
//
// PROC GetPlayer
//
//---------------------------------------------------------------------------

int DBaseStatusBar::GetPlayer ()
{
	return int(CPlayer - players);
}

//---------------------------------------------------------------------------
//
// PROC MultiplayerChanged
//
//---------------------------------------------------------------------------

void DBaseStatusBar::MultiplayerChanged ()
{
	ST_SetNeedRefresh();
}

//---------------------------------------------------------------------------
//
// PROC Tick
//
//---------------------------------------------------------------------------

void DBaseStatusBar::Tick ()
{
	for (size_t i = 0; i < countof(Messages); ++i)
	{
		DHUDMessage *msg = Messages[i];
		DHUDMessage **prev = &Messages[i];

		while (msg)
		{
			DHUDMessage *next = msg->Next;

			if (msg->Tick ())
			{
				*prev = next;
				msg->Destroy();
			}
			else
			{
				prev = &msg->Next;
			}
			msg = next;
		}

		// If the crosshair has been enlarged, shrink it.
		if (CrosshairSize > 1.)
		{
			CrosshairSize -= XHAIRSHRINKSIZE;
			if (CrosshairSize < 1.)
			{
				CrosshairSize = 1.;
			}
		}
	}
}

//---------------------------------------------------------------------------
//
// PROC AttachMessage
//
//---------------------------------------------------------------------------

void DBaseStatusBar::AttachMessage (DHUDMessage *msg, uint32_t id, int layer)
{
	DHUDMessage *old = NULL;
	DHUDMessage **prev;
	DObject *container = this;

	old = (id == 0 || id == 0xFFFFFFFF) ? NULL : DetachMessage (id);
	if (old != NULL)
	{
		old->Destroy();
	}

	// Merge unknown layers into the default layer.
	if ((size_t)layer >= countof(Messages))
	{
		layer = HUDMSGLayer_Default;
	}

	prev = &Messages[layer];

	// The ID serves as a priority, where lower numbers appear in front of
	// higher numbers. (i.e. The list is sorted in descending order, since
	// it gets drawn back to front.)
	while (*prev != NULL && (*prev)->SBarID > id)
	{
		container = *prev;
		prev = &(*prev)->Next;
	}

	msg->Next = *prev;
	msg->SBarID = id;
	*prev = msg;
	GC::WriteBarrier(container, msg);
}

//---------------------------------------------------------------------------
//
// PROC DetachMessage
//
//---------------------------------------------------------------------------

DHUDMessage *DBaseStatusBar::DetachMessage (DHUDMessage *msg)
{
	for (size_t i = 0; i < countof(Messages); ++i)
	{
		DHUDMessage *probe = Messages[i];
		DHUDMessage **prev = &Messages[i];

		while (probe && probe != msg)
		{
			prev = &probe->Next;
			probe = probe->Next;
		}
		if (probe != NULL)
		{
			*prev = probe->Next;
			probe->Next = NULL;
			// Redraw the status bar in case it was covered
			if (screen != NULL)
			{
				ST_SetNeedRefresh();
			}
			return probe;
		}
	}
	return NULL;
}

DHUDMessage *DBaseStatusBar::DetachMessage (uint32_t id)
{
	for (size_t i = 0; i < countof(Messages); ++i)
	{
		DHUDMessage *probe = Messages[i];
		DHUDMessage **prev = &Messages[i];

		while (probe && probe->SBarID != id)
		{
			prev = &probe->Next;
			probe = probe->Next;
		}
		if (probe != NULL)
		{
			*prev = probe->Next;
			probe->Next = NULL;
			// Redraw the status bar in case it was covered
			if (screen != NULL)
			{
				ST_SetNeedRefresh();
			}
			return probe;
		}
	}
	return NULL;
}

//---------------------------------------------------------------------------
//
// PROC DetachAllMessages
//
//---------------------------------------------------------------------------

void DBaseStatusBar::DetachAllMessages ()
{
	for (size_t i = 0; i < countof(Messages); ++i)
	{
		DHUDMessage *probe = Messages[i];

		Messages[i] = NULL;
		while (probe != NULL)
		{
			DHUDMessage *next = probe->Next;
			probe->Destroy();
			probe = next;
		}
	}
}

//---------------------------------------------------------------------------
//
// PROC ShowPlayerName
//
//---------------------------------------------------------------------------

void DBaseStatusBar::ShowPlayerName ()
{
	EColorRange color;

	color = (CPlayer == &players[consoleplayer]) ? CR_GOLD : CR_GREEN;
	AttachMessage (new DHUDMessageFadeOut (SmallFont, CPlayer->userinfo.GetName(),
		1.5f, 0.92f, 0, 0, color, 2.f, 0.35f), MAKE_ID('P','N','A','M'));
}

//---------------------------------------------------------------------------
//
// RefreshBackground
//
//---------------------------------------------------------------------------

void DBaseStatusBar::RefreshBackground () const
{
	int x, x2, y;

	float ratio = ActiveRatio (SCREENWIDTH, SCREENHEIGHT);
	x = (ratio < 1.5f || !Scaled) ? ST_X : SCREENWIDTH*(48-AspectMultiplier(ratio))/(48*2);
	y = x == ST_X && x > 0 ? ST_Y : gST_Y;

	if(!CompleteBorder)
	{
		if(y < SCREENHEIGHT)
		{
			V_DrawBorder (x+1, y, SCREENWIDTH, y+1);
			V_DrawBorder (x+1, SCREENHEIGHT-1, SCREENWIDTH, SCREENHEIGHT);
		}
	}
	else
	{
		x = SCREENWIDTH;
	}

	if (x > 0)
	{
		if(!CompleteBorder)
		{
			x2 = ratio < 1.5f || !Scaled ? ST_X+HorizontalResolution :
				SCREENWIDTH - (SCREENWIDTH*(48-AspectMultiplier(ratio))+48*2-1)/(48*2);
		}
		else
		{
			x2 = SCREENWIDTH;
		}

		V_DrawBorder (0, y, x+1, SCREENHEIGHT);
		V_DrawBorder (x2-1, y, SCREENWIDTH, SCREENHEIGHT);

		if (setblocks >= 10)
		{
			FTexture *p = TexMan[gameinfo.Border.b];
			if (p != NULL)
			{
				screen->FlatFill(0, y, x, y + p->GetHeight(), p, true);
				screen->FlatFill(x2, y, SCREENWIDTH, y + p->GetHeight(), p, true);
			}
		}
	}
}

//---------------------------------------------------------------------------
//
// DrawCrosshair
//
//---------------------------------------------------------------------------

void DBaseStatusBar::DrawCrosshair ()
{
	static uint32_t prevcolor = 0xffffffff;
	static int palettecolor = 0;

	uint32_t color;
	double size;
	int w, h;

	// Don't draw the crosshair in chasecam mode
	if (players[consoleplayer].cheats & CF_CHASECAM)
		return;

	ST_LoadCrosshair();

	// Don't draw the crosshair if there is none
	if (CrosshairImage == NULL || gamestate == GS_TITLELEVEL || camera->health <= 0)
	{
		return;
	}

	if (crosshairscale > 0.0f)
	{
		size = SCREENHEIGHT * crosshairscale / 200.;
	}
	else
	{
		size = 1.;
	}

	if (crosshairgrow)
	{
		size *= CrosshairSize;
	}
	w = int(CrosshairImage->GetWidth() * size);
	h = int(CrosshairImage->GetHeight() * size);

	if (crosshairhealth)
	{
		int health = Scale(CPlayer->health, 100, CPlayer->mo->GetDefault()->health);

		if (health >= 85)
		{
			color = 0x00ff00;
		}
		else 
		{
			int red, green;
			health -= 25;
			if (health < 0)
			{
				health = 0;
			}
			if (health < 30)
			{
				red = 255;
				green = health * 255 / 30;
			}
			else
			{
				red = (60 - health) * 255 / 30;
				green = 255;
			}
			color = (red<<16) | (green<<8);
		}
	}
	else
	{
		color = crosshaircolor;
	}

	if (color != prevcolor)
	{
		prevcolor = color;
		palettecolor = ColorMatcher.Pick (RPART(color), GPART(color), BPART(color));
	}

	screen->DrawTexture (CrosshairImage,
		viewwidth / 2 + viewwindowx,
		viewheight / 2 + viewwindowy,
		DTA_DestWidth, w,
		DTA_DestHeight, h,
		DTA_AlphaChannel, true,
		DTA_FillColor, (palettecolor << 24) | (color & 0xFFFFFF),
		TAG_DONE);
}

//---------------------------------------------------------------------------
//
// FlashCrosshair
//
//---------------------------------------------------------------------------

void DBaseStatusBar::FlashCrosshair ()
{
	CrosshairSize = XHAIRPICKUPSIZE;
}

//---------------------------------------------------------------------------
//
// DrawMessages
//
//---------------------------------------------------------------------------

void DBaseStatusBar::DrawMessages (int layer, int bottom)
{
	DHUDMessage *msg = Messages[layer];
	int visibility = 0;

	if (viewactive)
	{
		visibility |= HUDMSG_NotWith3DView;
	}
	if (automapactive)
	{
		visibility |= viewactive ? HUDMSG_NotWithOverlayMap : HUDMSG_NotWithFullMap;
	}
	while (msg)
	{
		DHUDMessage *next = msg->Next;
		msg->Draw (bottom, visibility);
		msg = next;
	}
}

//---------------------------------------------------------------------------
//
// Draw
//
//---------------------------------------------------------------------------

void DBaseStatusBar::Draw (EHudState state)
{
	// HUD_AltHud state is for popups only
	if (state == HUD_AltHud)
		return;

	char line[64+10];

	if ((SB_state != 0 || BorderNeedRefresh) && state == HUD_StatusBar)
	{
		RefreshBackground ();
	}

	if (idmypos)
	{ // Draw current coordinates
		int height = SmallFont->GetHeight();
		char labels[3] = { 'X', 'Y', 'Z' };
		int i;

		int vwidth;
		int vheight;
		int xpos;
		int y;

		if (active_con_scaletext() == 1)
		{
			vwidth = SCREENWIDTH;
			vheight = SCREENHEIGHT;
			xpos = vwidth - 80;
			y = gST_Y - height;
		}
		else if (active_con_scaletext() > 1)
		{
			vwidth = SCREENWIDTH / active_con_scaletext();
			vheight = SCREENHEIGHT / active_con_scaletext();
			xpos = vwidth - SmallFont->StringWidth("X: -00000")-6;
			y = gST_Y/4 - height;
		}
		else
		{
			vwidth = SCREENWIDTH/2;
			vheight = SCREENHEIGHT/2;
			xpos = vwidth - SmallFont->StringWidth("X: -00000")-6;
			y = gST_Y/2 - height;
		}

		if (gameinfo.gametype == GAME_Strife)
		{
			if (active_con_scaletext() == 1)
				y -= height * 4;
			else if (active_con_scaletext() > 3)
				y -= height;
			else
				y -= height * 2;
		}

		DVector3 pos = CPlayer->mo->Pos();
		for (i = 2; i >= 0; y -= height, --i)
		{
			mysnprintf (line, countof(line), "%c: %d", labels[i], int(pos[i]));
			screen->DrawText (SmallFont, CR_GREEN, xpos, y, line, 
				DTA_KeepRatio, true,
				DTA_VirtualWidth, vwidth, DTA_VirtualHeight, vheight, 				
				TAG_DONE);
			V_SetBorderNeedRefresh();
		}
	}

	if (viewactive)
	{
		if (CPlayer && CPlayer->camera && CPlayer->camera->player)
		{
			DrawCrosshair ();
		}
	}
	else if (automapactive)
	{
		int y, time = Tics2Seconds(level.time), height;
		int totaltime = Tics2Seconds(level.totaltime);
		EColorRange highlight = (gameinfo.gametype & GAME_DoomChex) ?
			CR_UNTRANSLATED : CR_YELLOW;

		height = SmallFont->GetHeight () * CleanYfac;

		// Draw timer
		y = 8;
		if (am_showtime)
		{
			mysnprintf (line, countof(line), "%02d:%02d:%02d", time/3600, (time%3600)/60, time%60);	// Time
			screen->DrawText (SmallFont, CR_GREY, SCREENWIDTH - 80*CleanXfac, y, line, DTA_CleanNoMove, true, TAG_DONE);
			y+=8*CleanYfac;
		}
		if (am_showtotaltime)
		{
			mysnprintf (line, countof(line), "%02d:%02d:%02d", totaltime/3600, (totaltime%3600)/60, totaltime%60);	// Total time
			screen->DrawText (SmallFont, CR_GREY, SCREENWIDTH - 80*CleanXfac, y, line, DTA_CleanNoMove, true, TAG_DONE);
		}

		// Draw map name
		y = gST_Y - height;
		if (gameinfo.gametype == GAME_Heretic && SCREENWIDTH > 320 && !Scaled)
		{
			y -= 8;
		}
		else if (gameinfo.gametype == GAME_Hexen)
		{
			if (Scaled)
			{
				y -= Scale (11, SCREENHEIGHT, 200);
			}
			else
			{
				if (SCREENWIDTH < 640)
				{
					y -= 12;
				}
				else
				{ // Get past the tops of the gargoyles' wings
					y -= 28;
				}
			}
		}
		else if (gameinfo.gametype == GAME_Strife)
		{
			if (Scaled)
			{
				y -= Scale (8, SCREENHEIGHT, 200);
			}
			else
			{
				y -= 8;
			}
		}
		FString mapname;

		ST_FormatMapName(mapname, TEXTCOLOR_GREY);
		screen->DrawText (SmallFont, highlight,
			(SCREENWIDTH - SmallFont->StringWidth (mapname)*CleanXfac)/2, y, mapname,
			DTA_CleanNoMove, true, TAG_DONE);

		if (!deathmatch)
		{
			int y = 8;

			// Draw monster count
			if (am_showmonsters)
			{
				mysnprintf (line, countof(line), "%s" TEXTCOLOR_GREY " %d/%d",
					GStrings("AM_MONSTERS"), level.killed_monsters, level.total_monsters);
				screen->DrawText (SmallFont, highlight, 8, y, line,
					DTA_CleanNoMove, true, TAG_DONE);
				y += height;
			}

			// Draw secret count
			if (am_showsecrets)
			{
				mysnprintf (line, countof(line), "%s" TEXTCOLOR_GREY " %d/%d",
					GStrings("AM_SECRETS"), level.found_secrets, level.total_secrets);
				screen->DrawText (SmallFont, highlight, 8, y, line,
					DTA_CleanNoMove, true, TAG_DONE);
				y += height;
			}

			// Draw item count
			if (am_showitems)
			{
				mysnprintf (line, countof(line), "%s" TEXTCOLOR_GREY " %d/%d",
					GStrings("AM_ITEMS"), level.found_items, level.total_items);
				screen->DrawText (SmallFont, highlight, 8, y, line,
					DTA_CleanNoMove, true, TAG_DONE);
			}
		}
	}
}


void DBaseStatusBar::DrawLog ()
{
	int hudwidth, hudheight;

	if (CPlayer->LogText.IsNotEmpty())
	{
		// This uses the same scaling as regular HUD messages
		if (active_con_scaletext() == 0)
		{
			hudwidth = SCREENWIDTH / CleanXfac;
			hudheight = SCREENHEIGHT / CleanYfac;
		}
		else
		{
			hudwidth = SCREENWIDTH / active_con_scaletext();
			hudheight = SCREENHEIGHT / active_con_scaletext();
		}

		int linelen = hudwidth<640? Scale(hudwidth,9,10)-40 : 560;
		FBrokenLines *lines = V_BreakLines (SmallFont, linelen, CPlayer->LogText);
		int height = 20;

		for (int i = 0; lines[i].Width != -1; i++) height += SmallFont->GetHeight () + 1;

		int x,y,w;

		if (linelen<560)
		{
			x=hudwidth/20;
			y=hudheight/8;
			w=hudwidth-2*x;
		}
		else
		{
			x=(hudwidth>>1)-300;
			y=hudheight*3/10-(height>>1);
			if (y<0) y=0;
			w=600;
		}
		screen->Dim(0, 0.5f, Scale(x, SCREENWIDTH, hudwidth), Scale(y, SCREENHEIGHT, hudheight), 
							 Scale(w, SCREENWIDTH, hudwidth), Scale(height, SCREENHEIGHT, hudheight));
		x+=20;
		y+=10;
		for (int i = 0; lines[i].Width != -1; i++)
		{

			screen->DrawText (SmallFont, CR_UNTRANSLATED, x, y, lines[i].Text,
				DTA_KeepRatio, true,
				DTA_VirtualWidth, hudwidth, DTA_VirtualHeight, hudheight, TAG_DONE);
			y += SmallFont->GetHeight ()+1;
		}

		V_FreeBrokenLines (lines);
	}
}

bool DBaseStatusBar::MustDrawLog(EHudState)
{
	return true;
}

void DBaseStatusBar::SetMugShotState(const char *stateName, bool waitTillDone, bool reset)
{
}

//---------------------------------------------------------------------------
//
// DrawBottomStuff
//
//---------------------------------------------------------------------------

void DBaseStatusBar::DrawBottomStuff (EHudState state)
{
	DrawMessages (HUDMSGLayer_UnderHUD, (state == HUD_StatusBar) ? gST_Y : SCREENHEIGHT);
}

//---------------------------------------------------------------------------
//
// DrawTopStuff
//
//---------------------------------------------------------------------------

void DBaseStatusBar::DrawTopStuff (EHudState state)
{
	if (demoplayback && demover != DEMOGAMEVERSION)
	{
		screen->DrawText (SmallFont, CR_TAN, 0, ST_Y - 40 * CleanYfac,
			"Demo was recorded with a different version\n"
			"of " GAMENAME ". Expect it to go out of sync.",
			DTA_CleanNoMove, true, TAG_DONE);
	}

	DrawPowerups ();
	if (automapactive && !viewactive)
	{
		DrawMessages (HUDMSGLayer_OverMap, (state == HUD_StatusBar) ? gST_Y : SCREENHEIGHT);
	}
	DrawMessages (HUDMSGLayer_OverHUD, (state == HUD_StatusBar) ? gST_Y : SCREENHEIGHT);
	DrawConsistancy ();
	DrawWaiting ();
	if (ShowLog && MustDrawLog(state)) DrawLog ();

	if (noisedebug)
	{
		S_NoiseDebug ();
	}
}

//---------------------------------------------------------------------------
//
// DrawPowerups
//
//---------------------------------------------------------------------------

void DBaseStatusBar::DrawPowerups ()
{
	// Each icon gets a 32x32 block to draw itself in.
	int x, y;
	AInventory *item;
	const int yshift = SmallFont->GetHeight();

	x = -20;
	y = 17 
		+ (ST_IsTimeVisible()    ? yshift : 0)
		+ (ST_IsLatencyVisible() ? yshift : 0);
	for (item = CPlayer->mo->Inventory; item != NULL; item = item->Inventory)
	{
		IFVIRTUALPTR(item, AInventory, DrawPowerup)
		{
			VMValue params[3] = { item, x, y };
			VMReturn ret;
			int retv;

			ret.IntAt(&retv);
			GlobalVMStack.Call(func, params, 3, &ret, 1);
			if (retv)
			{
				x -= POWERUPICONSIZE;
				if (x < -POWERUPICONSIZE * 5)
				{
					x = -20;
					y += POWERUPICONSIZE * 2;
				}
			}
		}
	}
}

//---------------------------------------------------------------------------
//
// BlendView
//
//---------------------------------------------------------------------------

void DBaseStatusBar::BlendView (float blend[4])
{
	// [Nash] Allow user to set blend intensity
	float cnt = (BaseBlendA * underwater_fade_scalar);

	V_AddBlend (BaseBlendR / 255.f, BaseBlendG / 255.f, BaseBlendB / 255.f, cnt, blend);
	V_AddPlayerBlend(CPlayer, blend, 1.0f, 228);

	if (screen->Accel2D || (CPlayer->camera != NULL && menuactive == MENU_Off && ConsoleState == c_up))
	{
		player_t *player = (CPlayer->camera != NULL && CPlayer->camera->player != NULL) ? CPlayer->camera->player : CPlayer;
		V_AddBlend (player->BlendR, player->BlendG, player->BlendB, player->BlendA, blend);
	}

	V_SetBlend ((int)(blend[0] * 255.0f), (int)(blend[1] * 255.0f),
				(int)(blend[2] * 255.0f), (int)(blend[3] * 256.0f));
}

void DBaseStatusBar::DrawConsistancy () const
{
	static bool firsttime = true;
	int i;
	char conbuff[64], *buff_p;

	if (!netgame)
		return;

	buff_p = NULL;
	for (i = 0; i < MAXPLAYERS; i++)
	{
		if (playeringame[i] && players[i].inconsistant)
		{
			if (buff_p == NULL)
			{
				strcpy (conbuff, "Out of sync with:");
				buff_p = conbuff + 17;
			}
			*buff_p++ = ' ';
			*buff_p++ = '1' + i;
			*buff_p = 0;
		}
	}

	if (buff_p != NULL)
	{
		if (firsttime)
		{
			firsttime = false;
			if (debugfile)
			{
				fprintf (debugfile, "%s as of tic %d (%d)\n", conbuff,
					players[1-consoleplayer].inconsistant,
					players[1-consoleplayer].inconsistant/ticdup);
			}
		}
		screen->DrawText (SmallFont, CR_GREEN, 
			(screen->GetWidth() - SmallFont->StringWidth (conbuff)*CleanXfac) / 2,
			0, conbuff, DTA_CleanNoMove, true, TAG_DONE);
		BorderTopRefresh = screen->GetPageCount ();
	}
}

void DBaseStatusBar::DrawWaiting () const
{
	int i;
	char conbuff[64], *buff_p;

	if (!netgame)
		return;

	buff_p = NULL;
	for (i = 0; i < MAXPLAYERS; i++)
	{
		if (playeringame[i] && players[i].waiting)
		{
			if (buff_p == NULL)
			{
				strcpy (conbuff, "Waiting for:");
				buff_p = conbuff + 12;
			}
			*buff_p++ = ' ';
			*buff_p++ = '1' + i;
			*buff_p = 0;
		}
	}

	if (buff_p != NULL)
	{
		screen->DrawText (SmallFont, CR_ORANGE, 
			(screen->GetWidth() - SmallFont->StringWidth (conbuff)*CleanXfac) / 2,
			SmallFont->GetHeight()*CleanYfac, conbuff, DTA_CleanNoMove, true, TAG_DONE);
		BorderTopRefresh = screen->GetPageCount ();
	}
}

void DBaseStatusBar::FlashItem (const PClass *itemtype)
{
}

void DBaseStatusBar::NewGame ()
{
}

void DBaseStatusBar::SetInteger (int pname, int param)
{
}

void DBaseStatusBar::ShowPop (int popnum)
{
	ShowLog = (popnum == POP_Log && !ShowLog);
}

void DBaseStatusBar::ReceivedWeapon (AWeapon *weapon)
{
}

DEFINE_ACTION_FUNCTION(_StatusBar, ReceivedWeapon)
{
	PARAM_PROLOGUE;
	PARAM_POINTER(w, AWeapon);
	StatusBar->ReceivedWeapon(w);
	return 0;
}


void DBaseStatusBar::SerializeMessages(FSerializer &arc)
{
	arc.Array("hudmessages", Messages, 3, true);
}

void DBaseStatusBar::ScreenSizeChanged ()
{
	st_scale.Callback ();
	ST_SetNeedRefresh();

	for (size_t i = 0; i < countof(Messages); ++i)
	{
		DHUDMessage *message = Messages[i];
		while (message != NULL)
		{
			message->ScreenSizeChanged ();
			message = message->Next;
		}
	}
}

//---------------------------------------------------------------------------
//
// ValidateInvFirst
//
// Returns an inventory item that, when drawn as the first item, is sure to
// include the selected item in the inventory bar.
//
//---------------------------------------------------------------------------

AInventory *DBaseStatusBar::ValidateInvFirst (int numVisible) const
{
	AInventory *item;
	int i;

	if (CPlayer->mo->InvFirst == NULL)
	{
		CPlayer->mo->InvFirst = CPlayer->mo->FirstInv();
		if (CPlayer->mo->InvFirst == NULL)
		{ // Nothing to show
			return NULL;
		}
	}

	assert (CPlayer->mo->InvFirst->Owner == CPlayer->mo);

	// If there are fewer than numVisible items shown, see if we can shift the
	// view left to show more.
	for (i = 0, item = CPlayer->mo->InvFirst; item != NULL && i < numVisible; ++i, item = item->NextInv())
	{ }

	while (i < numVisible)
	{
		item = CPlayer->mo->InvFirst->PrevInv ();
		if (item == NULL)
		{
			break;
		}
		else
		{
			CPlayer->mo->InvFirst = item;
			++i;
		}
	}

	if (CPlayer->mo->InvSel == NULL)
	{
		// Nothing selected, so don't move the view.
		return CPlayer->mo->InvFirst == NULL ? CPlayer->mo->Inventory : CPlayer->mo->InvFirst;
	}
	else
	{
		// Check if InvSel is already visible
		for (item = CPlayer->mo->InvFirst, i = numVisible;
			 item != NULL && i != 0;
			 item = item->NextInv(), --i)
		{
			if (item == CPlayer->mo->InvSel)
			{
				return CPlayer->mo->InvFirst;
			}
		}
		// Check if InvSel is to the right of the visible range
		for (i = 1; item != NULL; item = item->NextInv(), ++i)
		{
			if (item == CPlayer->mo->InvSel)
			{
				// Found it. Now advance InvFirst
				for (item = CPlayer->mo->InvFirst; i != 0; --i)
				{
					item = item->NextInv();
				}
				return item;
			}
		}
		// Check if InvSel is to the left of the visible range
		for (item = CPlayer->mo->Inventory;
			item != CPlayer->mo->InvSel;
			item = item->NextInv())
		{ }
		if (item != NULL)
		{
			// Found it, so let it become the first item shown
			return item;
		}
		// Didn't find the selected item, so don't move the view.
		// This should never happen, so let debug builds assert.
		assert (item != NULL);
		return CPlayer->mo->InvFirst;
	}
}

//============================================================================
//
// DBaseStatusBar :: GetCurrentAmmo
//
// Returns the types and amounts of ammo used by the current weapon. If the
// weapon only uses one type of ammo, it is always returned as ammo1.
//
//============================================================================

void DBaseStatusBar::GetCurrentAmmo (AInventory *&ammo1, AInventory *&ammo2, int &ammocount1, int &ammocount2) const
{
	if (CPlayer->ReadyWeapon != NULL)
	{
		ammo1 = CPlayer->ReadyWeapon->Ammo1;
		ammo2 = CPlayer->ReadyWeapon->Ammo2;
		if (ammo1 == NULL)
		{
			ammo1 = ammo2;
			ammo2 = NULL;
		}
	}
	else
	{
		ammo1 = ammo2 = NULL;
	}
	ammocount1 = ammo1 != NULL ? ammo1->Amount : 0;
	ammocount2 = ammo2 != NULL ? ammo2->Amount : 0;
}

//============================================================================
//
// CCMD showpop
//
// Asks the status bar to show a pop screen.
//
//============================================================================

CCMD (showpop)
{
	if (argv.argc() != 2)
	{
		Printf ("Usage: showpop <popnumber>\n");
	}
	else if (StatusBar != NULL)
	{
		int popnum = atoi (argv[1]);
		if (popnum < 0)
		{
			popnum = 0;
		}
		StatusBar->ShowPop (popnum);
	}
}
