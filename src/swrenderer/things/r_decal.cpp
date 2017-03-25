//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// The source is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// FITNESS FOR A PARTICULAR PURPOSE. See the DOOM Source Code License
// for more details.
//

#include <stdlib.h>
#include <stddef.h>
#include "templates.h"
#include "i_system.h"
#include "doomdef.h"
#include "doomstat.h"
#include "doomdata.h"
#include "p_lnspec.h"
#include "r_sky.h"
#include "v_video.h"
#include "m_swap.h"
#include "w_wad.h"
#include "stats.h"
#include "a_sharedglobal.h"
#include "d_net.h"
#include "g_level.h"
#include "swrenderer/scene/r_opaque_pass.h"
#include "r_decal.h"
#include "swrenderer/scene/r_3dfloors.h"
#include "swrenderer/drawers/r_draw.h"
#include "v_palette.h"
#include "r_data/colormaps.h"
#include "swrenderer/line/r_wallsetup.h"
#include "swrenderer/line/r_walldraw.h"
#include "swrenderer/segments/r_drawsegment.h"
#include "swrenderer/scene/r_portal.h"
#include "swrenderer/scene/r_scene.h"
#include "swrenderer/scene/r_light.h"
#include "swrenderer/things/r_wallsprite.h"
#include "swrenderer/viewport/r_viewport.h"
#include "swrenderer/viewport/r_spritedrawer.h"
#include "swrenderer/r_memory.h"
#include "swrenderer/r_renderthread.h"

EXTERN_CVAR(Bool, r_fullbrightignoresectorcolor);

namespace swrenderer
{
	void RenderDecal::RenderDecals(RenderThread *thread, side_t *sidedef, DrawSegment *draw_segment, int wallshade, float lightleft, float lightstep, seg_t *curline, const FWallCoords &wallC, bool foggy, FDynamicColormap *basecolormap, const short *walltop, const short *wallbottom)
	{
		for (DBaseDecal *decal = sidedef->AttachedDecals; decal != NULL; decal = decal->WallNext)
		{
			Render(thread, sidedef, decal, draw_segment, wallshade, lightleft, lightstep, curline, wallC, foggy, basecolormap, walltop, wallbottom, 0);
		}
	}

	// pass = 0: when seg is first drawn
	//		= 1: drawing masked textures (including sprites)
	// Currently, only pass = 0 is done or used

	void RenderDecal::Render(RenderThread *thread, side_t *wall, DBaseDecal *decal, DrawSegment *clipper, int wallshade, float lightleft, float lightstep, seg_t *curline, const FWallCoords &savecoord, bool foggy, FDynamicColormap *basecolormap, const short *walltop, const short *wallbottom, int pass)
	{
		DVector2 decal_left, decal_right, decal_pos;
		int x1, x2;
		double yscale;
		BYTE flipx;
		double zpos;
		int needrepeat = 0;
		sector_t *front, *back;
		bool calclighting;
		bool rereadcolormap;
		FDynamicColormap *usecolormap;
		float light = 0;
		const short *mfloorclip;
		const short *mceilingclip;

		if (decal->RenderFlags & RF_INVISIBLE || !viewactive || !decal->PicNum.isValid())
			return;

		// Determine actor z
		zpos = decal->Z;
		front = curline->frontsector;
		back = (curline->backsector != NULL) ? curline->backsector : curline->frontsector;
		switch (decal->RenderFlags & RF_RELMASK)
		{
		default:
			zpos = decal->Z;
			break;
		case RF_RELUPPER:
			if (curline->linedef->flags & ML_DONTPEGTOP)
			{
				zpos = decal->Z + front->GetPlaneTexZ(sector_t::ceiling);
			}
			else
			{
				zpos = decal->Z + back->GetPlaneTexZ(sector_t::ceiling);
			}
			break;
		case RF_RELLOWER:
			if (curline->linedef->flags & ML_DONTPEGBOTTOM)
			{
				zpos = decal->Z + front->GetPlaneTexZ(sector_t::ceiling);
			}
			else
			{
				zpos = decal->Z + back->GetPlaneTexZ(sector_t::floor);
			}
			break;
		case RF_RELMID:
			if (curline->linedef->flags & ML_DONTPEGBOTTOM)
			{
				zpos = decal->Z + front->GetPlaneTexZ(sector_t::floor);
			}
			else
			{
				zpos = decal->Z + front->GetPlaneTexZ(sector_t::ceiling);
			}
		}

		FTexture *WallSpriteTile = TexMan(decal->PicNum, true);
		flipx = (BYTE)(decal->RenderFlags & RF_XFLIP);

		if (WallSpriteTile == NULL || WallSpriteTile->UseType == FTexture::TEX_Null)
		{
			return;
		}

		// Determine left and right edges of sprite. Since this sprite is bound
		// to a wall, we use the wall's angle instead of the decal's. This is
		// pretty much the same as what R_AddLine() does.

		double edge_right = WallSpriteTile->GetWidth();
		double edge_left = WallSpriteTile->LeftOffset;
		edge_right = (edge_right - edge_left) * decal->ScaleX;
		edge_left *= decal->ScaleX;

		double dcx, dcy;
		decal->GetXY(wall, dcx, dcy);
		decal_pos = { dcx, dcy };

		DVector2 angvec = (curline->v2->fPos() - curline->v1->fPos()).Unit();
		float maskedScaleY;

		decal_left = decal_pos - edge_left * angvec - ViewPos;
		decal_right = decal_pos + edge_right * angvec - ViewPos;

		CameraLight *cameraLight;
		double texturemid;

		FWallCoords WallC;
		if (WallC.Init(thread, decal_left, decal_right, TOO_CLOSE_Z))
			return;

		x1 = WallC.sx1;
		x2 = WallC.sx2;

		if (x1 >= clipper->x2 || x2 <= clipper->x1)
			return;

		FWallTmapVals WallT;
		WallT.InitFromWallCoords(thread, &WallC);

		// Get the top and bottom clipping arrays
		switch (decal->RenderFlags & RF_CLIPMASK)
		{
		case RF_CLIPFULL:
			if (curline->backsector == NULL)
			{
				if (pass != 0)
				{
					return;
				}
				mceilingclip = walltop;
				mfloorclip = wallbottom;
			}
			else if (pass == 0)
			{
				mceilingclip = walltop;
				mfloorclip = thread->OpaquePass->ceilingclip;
				needrepeat = 1;
			}
			else
			{
				mceilingclip = clipper->sprtopclip - clipper->x1;
				mfloorclip = clipper->sprbottomclip - clipper->x1;
			}
			break;

		case RF_CLIPUPPER:
			if (pass != 0)
			{
				return;
			}
			mceilingclip = walltop;
			mfloorclip = thread->OpaquePass->ceilingclip;
			break;

		case RF_CLIPMID:
			if (curline->backsector != NULL && pass != 2)
			{
				return;
			}
			mceilingclip = clipper->sprtopclip - clipper->x1;
			mfloorclip = clipper->sprbottomclip - clipper->x1;
			break;

		case RF_CLIPLOWER:
			if (pass != 0)
			{
				return;
			}
			mceilingclip = thread->OpaquePass->floorclip;
			mfloorclip = wallbottom;
			break;
		}

		yscale = decal->ScaleY;
		texturemid = WallSpriteTile->TopOffset + (zpos - ViewPos.Z) / yscale;

		// Clip sprite to drawseg
		x1 = MAX<int>(clipper->x1, x1);
		x2 = MIN<int>(clipper->x2, x2);
		if (x1 >= x2)
		{
			return;
		}

		ProjectedWallTexcoords walltexcoords;
		walltexcoords.Project(WallSpriteTile->GetWidth(), x1, x2, WallT);

		if (flipx)
		{
			int i;
			int right = (WallSpriteTile->GetWidth() << FRACBITS) - 1;

			for (i = x1; i < x2; i++)
			{
				walltexcoords.UPos[i] = right - walltexcoords.UPos[i];
			}
		}

		// Prepare lighting
		calclighting = false;
		usecolormap = basecolormap;
		rereadcolormap = true;

		// Decals that are added to the scene must fade to black.
		if (decal->RenderStyle == LegacyRenderStyles[STYLE_Add] && usecolormap->Fade != 0)
		{
			usecolormap = GetSpecialLights(usecolormap->Color, 0, usecolormap->Desaturate);
			rereadcolormap = false;
		}

		light = lightleft + (x1 - savecoord.sx1) * lightstep;

		cameraLight = CameraLight::Instance();

		// Draw it
		bool sprflipvert;
		if (decal->RenderFlags & RF_YFLIP)
		{
			sprflipvert = true;
			yscale = -yscale;
			texturemid -= WallSpriteTile->GetHeight();
		}
		else
		{
			sprflipvert = false;
		}

		maskedScaleY = float(1 / yscale);
		do
		{
			int x = x1;

			SpriteDrawerArgs drawerargs;

			if (cameraLight->FixedLightLevel() >= 0)
				drawerargs.SetLight((r_fullbrightignoresectorcolor) ? &FullNormalLight : usecolormap, 0, cameraLight->FixedLightLevelShade());
			else if (cameraLight->FixedColormap() != NULL)
				drawerargs.SetLight(cameraLight->FixedColormap(), 0, 0);
			else if (!foggy && (decal->RenderFlags & RF_FULLBRIGHT))
				drawerargs.SetLight((r_fullbrightignoresectorcolor) ? &FullNormalLight : usecolormap, 0, 0);
			else
				calclighting = true;

			bool visible = drawerargs.SetStyle(decal->RenderStyle, (float)decal->Alpha, decal->Translation, decal->AlphaColor, basecolormap);

			// R_SetPatchStyle can modify basecolormap.
			if (rereadcolormap)
			{
				usecolormap = basecolormap;
			}

			if (visible)
			{
				while (x < x2)
				{
					if (calclighting)
					{ // calculate lighting
						drawerargs.SetLight(usecolormap, light, wallshade);
					}
					DrawColumn(thread, drawerargs, x, WallSpriteTile, walltexcoords, texturemid, maskedScaleY, sprflipvert, mfloorclip, mceilingclip);
					light += lightstep;
					x++;
				}
			}

			// If this sprite is RF_CLIPFULL on a two-sided line, needrepeat will
			// be set 1 if we need to draw on the lower wall. In all other cases,
			// needrepeat will be 0, and the while will fail.
			mceilingclip = thread->OpaquePass->floorclip;
			mfloorclip = wallbottom;
		} while (needrepeat--);
	}

	void RenderDecal::DrawColumn(RenderThread *thread, SpriteDrawerArgs &drawerargs, int x, FTexture *WallSpriteTile, const ProjectedWallTexcoords &walltexcoords, double texturemid, float maskedScaleY, bool sprflipvert, const short *mfloorclip, const short *mceilingclip)
	{
		auto viewport = RenderViewport::Instance();
		
		float iscale = walltexcoords.VStep[x] * maskedScaleY;
		double spryscale = 1 / iscale;
		double sprtopscreen;
		if (sprflipvert)
			sprtopscreen = viewport->CenterY + texturemid * spryscale;
		else
			sprtopscreen = viewport->CenterY - texturemid * spryscale;

		drawerargs.DrawMaskedColumn(thread, x, FLOAT2FIXED(iscale), WallSpriteTile, walltexcoords.UPos[x], spryscale, sprtopscreen, sprflipvert, mfloorclip, mceilingclip);
	}
}
