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

#include <stdio.h>
#include <stdlib.h>
#include <algorithm>
#include "p_lnspec.h"
#include "templates.h"
#include "doomdef.h"
#include "m_swap.h"
#include "i_system.h"
#include "w_wad.h"
#include "g_levellocals.h"
#include "p_maputl.h"
#include "swrenderer/things/r_visiblesprite.h"
#include "swrenderer/things/r_visiblespritelist.h"
#include "swrenderer/things/r_voxel.h"
#include "swrenderer/things/r_particle.h"
#include "swrenderer/things/r_sprite.h"
#include "swrenderer/things/r_wallsprite.h"
#include "swrenderer/segments/r_drawsegment.h"
#include "swrenderer/scene/r_portal.h"
#include "swrenderer/scene/r_translucent_pass.h"
#include "swrenderer/scene/r_light.h"
#include "swrenderer/plane/r_visibleplane.h"
#include "swrenderer/plane/r_visibleplanelist.h"
#include "swrenderer/line/r_renderdrawsegment.h"
#include "swrenderer/viewport/r_viewport.h"
#include "swrenderer/r_memory.h"
#include "swrenderer/r_renderthread.h"

EXTERN_CVAR(Int, r_drawfuzz)
EXTERN_CVAR(Bool, r_drawvoxels)
EXTERN_CVAR(Bool, r_blendmethod)

CVAR(Bool, r_fullbrightignoresectorcolor, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);

namespace swrenderer
{
	RenderTranslucentPass::RenderTranslucentPass(RenderThread *thread)
	{
		Thread = thread;
	}

	void RenderTranslucentPass::Deinit()
	{
		RenderVoxel::Deinit();
	}

	void RenderTranslucentPass::Clear()
	{
		Thread->SpriteList->Clear();
	}

	void RenderTranslucentPass::CollectPortals()
	{
		// This function collects all drawsegs that may be of interest to R_ClipSpriteColumnWithPortals 
		// Having that function over the entire list of drawsegs can break down performance quite drastically.
		// This is doing the costly stuff only once so that R_ClipSpriteColumnWithPortals can 
		// a) exit early if no relevant info is found and
		// b) skip most of the collected drawsegs which have no portal attached.
		portaldrawsegs.Clear();
		DrawSegmentList *drawseglist = Thread->DrawSegments.get();
		for (unsigned int index = 0; index != drawseglist->SegmentsCount(); index++)
		{
			DrawSegment *seg = drawseglist->Segment(index);

			// I don't know what makes this happen (some old top-down portal code or possibly skybox code? something adds null lines...)
			// crashes at the first frame of the first map of Action2.wad
			if (!seg->curline) continue;

			line_t* line = seg->curline->linedef;
			// ignore minisegs from GL nodes.
			if (!line) continue;

			// check if this line will clip sprites to itself
			if (!line->isVisualPortal() && line->special != Line_Mirror)
				continue;

			// don't clip sprites with portal's back side (it's transparent)
			if (seg->curline->sidedef != line->sidedef[0])
				continue;

			portaldrawsegs.Push(seg);
		}
	}

	bool RenderTranslucentPass::ClipSpriteColumnWithPortals(int x, VisibleSprite *spr)
	{
		RenderPortal *renderportal = Thread->Portal.get();

		// [ZZ] 10.01.2016: don't clip sprites from the root of a skybox.
		if (renderportal->CurrentPortalInSkybox)
			return false;

		for (DrawSegment *seg : portaldrawsegs)
		{
			// ignore segs from other portals
			if (seg->CurrentPortalUniq != renderportal->CurrentPortalUniq)
				continue;

			// (all checks that are already done in R_CollectPortals have been removed for performance reasons.)

			// don't clip if the sprite is in front of the portal
			if (!P_PointOnLineSidePrecise(spr->WorldPos().X, spr->WorldPos().Y, seg->curline->linedef))
				continue;

			// now if current column is covered by this drawseg, we clip it away
			if ((x >= seg->x1) && (x < seg->x2))
				return true;
		}

		return false;
	}

	void RenderTranslucentPass::DrawMaskedSingle(bool renew)
	{
		RenderPortal *renderportal = Thread->Portal.get();

		auto &sortedSprites = Thread->SpriteList->SortedSprites;
		for (int i = sortedSprites.Size(); i > 0; i--)
		{
			if (sortedSprites[i - 1]->IsCurrentPortalUniq(renderportal->CurrentPortalUniq))
			{
				sortedSprites[i - 1]->Render(Thread);
			}
		}

		// render any remaining masked mid textures

		if (renew)
		{
			Thread->Clip3D->fake3D |= FAKE3D_REFRESHCLIP;
		}

		DrawSegmentList *drawseglist = Thread->DrawSegments.get();
		for (unsigned int index = 0; index != drawseglist->SegmentsCount(); index++)
		{
			DrawSegment *ds = drawseglist->Segment(index);

			// [ZZ] the same as above
			if (ds->CurrentPortalUniq != renderportal->CurrentPortalUniq)
				continue;
			// kg3D - no fake segs
			if (ds->fake) continue;
			if (ds->maskedtexturecol != nullptr || ds->bFogBoundary)
			{
				RenderDrawSegment renderer(Thread);
				renderer.Render(ds, ds->x1, ds->x2);
			}
		}
	}

	void RenderTranslucentPass::Render()
	{
		CollectPortals();
		Thread->SpriteList->Sort();
		Thread->DrawSegments->BuildSegmentGroups();

		Clip3DFloors *clip3d = Thread->Clip3D.get();
		if (clip3d->height_top == nullptr)
		{ // kg3D - no visible 3D floors, normal rendering
			DrawMaskedSingle(false);
		}
		else
		{ // kg3D - correct sorting
			// ceilings
			for (HeightLevel *hl = clip3d->height_cur; hl != nullptr && hl->height >= ViewPos.Z; hl = hl->prev)
			{
				if (hl->next)
				{
					clip3d->fake3D = FAKE3D_CLIPBOTTOM | FAKE3D_CLIPTOP;
					clip3d->sclipTop = hl->next->height;
				}
				else
				{
					clip3d->fake3D = FAKE3D_CLIPBOTTOM;
				}
				clip3d->sclipBottom = hl->height;
				DrawMaskedSingle(true);
				Thread->PlaneList->RenderHeight(hl->height);
			}

			// floors
			clip3d->fake3D = FAKE3D_DOWN2UP | FAKE3D_CLIPTOP;
			clip3d->sclipTop = clip3d->height_top->height;
			DrawMaskedSingle(true);
			for (HeightLevel *hl = clip3d->height_top; hl != nullptr && hl->height < ViewPos.Z; hl = hl->next)
			{
				Thread->PlaneList->RenderHeight(hl->height);
				if (hl->next)
				{
					clip3d->fake3D = FAKE3D_DOWN2UP | FAKE3D_CLIPTOP | FAKE3D_CLIPBOTTOM;
					clip3d->sclipTop = hl->next->height;
				}
				else
				{
					clip3d->fake3D = FAKE3D_DOWN2UP | FAKE3D_CLIPBOTTOM;
				}
				clip3d->sclipBottom = hl->height;
				DrawMaskedSingle(true);
			}
			clip3d->DeleteHeights();
			clip3d->fake3D = 0;
		}
	}
}
