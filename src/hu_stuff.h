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
// DESCRIPTION:  Head up display
//
//-----------------------------------------------------------------------------

#ifndef __HU_STUFF_H__
#define __HU_STUFF_H__

#include "doomtype.h"

struct event_t;
class player_t;

//
// Globally visible constants.
//
#define HU_FONTSTART	uint8_t('!')		// the first font characters
#define HU_FONTEND		uint8_t('\377')	// the last font characters

// Calculate # of glyphs in font.
#define HU_FONTSIZE		(HU_FONTEND - HU_FONTSTART + 1)

//
// Chat routines
//

void CT_Init (void);
bool CT_Responder (event_t* ev);
void CT_Drawer (void);

extern int chatmodeon;

// [RH] Draw deathmatch scores

void HU_DrawScores (player_t *me);
void HU_GetPlayerWidths(int &maxnamewidth, int &maxscorewidth, int &maxiconheight);
void HU_DrawColorBar(int x, int y, int height, int playernum);
int HU_GetRowColor(player_t *player, bool hightlight);

extern bool SB_ForceActive;

// Sorting routines

int comparepoints(const void *arg1, const void *arg2);
int compareteams(const void *arg1, const void *arg2);

#endif
