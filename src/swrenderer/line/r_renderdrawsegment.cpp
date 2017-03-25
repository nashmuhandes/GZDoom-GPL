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
#include "templates.h"
#include "doomdef.h"
#include "m_bbox.h"
#include "i_system.h"
#include "p_lnspec.h"
#include "p_setup.h"
#include "a_sharedglobal.h"
#include "g_level.h"
#include "g_levellocals.h"
#include "p_effect.h"
#include "doomstat.h"
#include "r_state.h"
#include "v_palette.h"
#include "r_sky.h"
#include "po_man.h"
#include "r_data/colormaps.h"
#include "d_net.h"
#include "swrenderer/r_memory.h"
#include "swrenderer/r_renderthread.h"
#include "swrenderer/drawers/r_draw.h"
#include "swrenderer/scene/r_3dfloors.h"
#include "swrenderer/scene/r_opaque_pass.h"
#include "swrenderer/scene/r_portal.h"
#include "swrenderer/line/r_wallsetup.h"
#include "swrenderer/line/r_walldraw.h"
#include "swrenderer/line/r_fogboundary.h"
#include "swrenderer/line/r_renderdrawsegment.h"
#include "swrenderer/segments/r_drawsegment.h"
#include "swrenderer/things/r_visiblesprite.h"
#include "swrenderer/scene/r_light.h"
#include "swrenderer/viewport/r_viewport.h"
#include "swrenderer/viewport/r_spritedrawer.h"

EXTERN_CVAR(Bool, r_fullbrightignoresectorcolor);

namespace swrenderer
{
	RenderDrawSegment::RenderDrawSegment(RenderThread *thread)
	{
		Thread = thread;
	}

	void RenderDrawSegment::Render(DrawSegment *ds, int x1, int x2)
	{
		auto viewport = RenderViewport::Instance();
		RenderFogBoundary renderfog;
		float *MaskedSWall = nullptr, MaskedScaleY = 0, rw_scalestep = 0;
		fixed_t *maskedtexturecol = nullptr;
	
		FTexture	*tex;
		int			i;
		sector_t	tempsec;		// killough 4/13/98
		double		texheight, texheightscale;
		bool		notrelevant = false;
		double		rowoffset;
		bool		wrap = false;

		const sector_t *sec;

		bool sprflipvert = false;

		curline = ds->curline;

		float alpha = (float)MIN(curline->linedef->alpha, 1.);
		bool additive = (curline->linedef->flags & ML_ADDTRANS) != 0;

		WallDrawerArgs walldrawerargs;
		walldrawerargs.SetStyle(true, additive, FLOAT2FIXED(alpha));

		SpriteDrawerArgs columndrawerargs;
		FDynamicColormap *patchstylecolormap = nullptr;
		bool visible = columndrawerargs.SetStyle(LegacyRenderStyles[additive ? STYLE_Add : STYLE_Translucent], alpha, 0, 0, patchstylecolormap);

		if (!visible && !ds->bFogBoundary && !ds->bFakeBoundary)
		{
			return;
		}

		if (Thread->MainThread)
			NetUpdate();

		frontsector = curline->frontsector;
		backsector = curline->backsector;

		tex = TexMan(curline->sidedef->GetTexture(side_t::mid), true);
		if (i_compatflags & COMPATF_MASKEDMIDTEX)
		{
			tex = tex->GetRawTexture();
		}

		// killough 4/13/98: get correct lightlevel for 2s normal textures
		sec = Thread->OpaquePass->FakeFlat(frontsector, &tempsec, nullptr, nullptr, nullptr, 0, 0, 0, 0);

		FDynamicColormap *basecolormap = sec->ColorMap;	// [RH] Set basecolormap

		int wallshade = ds->shade;
		rw_lightstep = ds->lightstep;
		rw_light = ds->light + (x1 - ds->x1) * rw_lightstep;

		Clip3DFloors *clip3d = Thread->Clip3D.get();

		CameraLight *cameraLight = CameraLight::Instance();
		if (cameraLight->FixedLightLevel() < 0)
		{
			if (!(clip3d->fake3D & FAKE3D_CLIPTOP))
			{
				clip3d->sclipTop = sec->ceilingplane.ZatPoint(ViewPos);
			}
			for (i = frontsector->e->XFloor.lightlist.Size() - 1; i >= 0; i--)
			{
				if (clip3d->sclipTop <= frontsector->e->XFloor.lightlist[i].plane.Zat0())
				{
					lightlist_t *lit = &frontsector->e->XFloor.lightlist[i];
					basecolormap = lit->extra_colormap;
					bool foggy = (level.fadeto || basecolormap->Fade || (level.flags & LEVEL_HASFADETABLE)); // [RH] set foggy flag
					wallshade = LightVisibility::LightLevelToShade(curline->sidedef->GetLightLevel(ds->foggy, *lit->p_lightlevel, lit->lightsource != nullptr) + LightVisibility::ActualExtraLight(ds->foggy), foggy);
					break;
				}
			}
		}

		short *mfloorclip = ds->sprbottomclip - ds->x1;
		short *mceilingclip = ds->sprtopclip - ds->x1;
		double spryscale;

		// [RH] Draw fog partition
		if (ds->bFogBoundary)
		{
			renderfog.Render(Thread, x1, x2, mceilingclip, mfloorclip, wallshade, rw_light, rw_lightstep, basecolormap);
			if (ds->maskedtexturecol == nullptr)
			{
				goto clearfog;
			}
		}
		if ((ds->bFakeBoundary && !(ds->bFakeBoundary & 4)) || !visible)
		{
			goto clearfog;
		}

		MaskedSWall = ds->swall - ds->x1;
		MaskedScaleY = ds->yscale;
		maskedtexturecol = ds->maskedtexturecol - ds->x1;
		spryscale = ds->iscale + ds->iscalestep * (x1 - ds->x1);
		rw_scalestep = ds->iscalestep;

		if (cameraLight->FixedLightLevel() >= 0)
		{
			walldrawerargs.SetLight((r_fullbrightignoresectorcolor) ? &FullNormalLight : basecolormap, 0, cameraLight->FixedLightLevelShade());
			columndrawerargs.SetLight((r_fullbrightignoresectorcolor) ? &FullNormalLight : basecolormap, 0, cameraLight->FixedLightLevelShade());
		}
		else if (cameraLight->FixedColormap() != nullptr)
		{
			walldrawerargs.SetLight(cameraLight->FixedColormap(), 0, 0);
			columndrawerargs.SetLight(cameraLight->FixedColormap(), 0, 0);
		}

		// find positioning
		texheight = tex->GetScaledHeightDouble();
		texheightscale = fabs(curline->sidedef->GetTextureYScale(side_t::mid));
		if (texheightscale != 1)
		{
			texheight = texheight / texheightscale;
		}

		double texturemid;
		if (curline->linedef->flags & ML_DONTPEGBOTTOM)
		{
			texturemid = MAX(frontsector->GetPlaneTexZ(sector_t::floor), backsector->GetPlaneTexZ(sector_t::floor)) + texheight;
		}
		else
		{
			texturemid = MIN(frontsector->GetPlaneTexZ(sector_t::ceiling), backsector->GetPlaneTexZ(sector_t::ceiling));
		}

		rowoffset = curline->sidedef->GetTextureYOffset(side_t::mid);

		wrap = (curline->linedef->flags & ML_WRAP_MIDTEX) || (curline->sidedef->Flags & WALLF_WRAP_MIDTEX);
		if (!wrap)
		{ // Texture does not wrap vertically.
			double textop;

			if (MaskedScaleY < 0)
			{
				MaskedScaleY = -MaskedScaleY;
				sprflipvert = true;
			}
			if (tex->bWorldPanning)
			{
				// rowoffset is added before the multiply so that the masked texture will
				// still be positioned in world units rather than texels.
				texturemid += rowoffset - ViewPos.Z;
				textop = texturemid;
				texturemid *= MaskedScaleY;
			}
			else
			{
				// rowoffset is added outside the multiply so that it positions the texture
				// by texels instead of world units.
				textop = texturemid + rowoffset / MaskedScaleY - ViewPos.Z;
				texturemid = (texturemid - ViewPos.Z) * MaskedScaleY + rowoffset;
			}
			if (sprflipvert)
			{
				MaskedScaleY = -MaskedScaleY;
				texturemid -= tex->GetHeight() << FRACBITS;
			}

			// [RH] Don't bother drawing segs that are completely offscreen
			if (viewport->globaldclip * ds->sz1 < -textop && viewport->globaldclip * ds->sz2 < -textop)
			{ // Texture top is below the bottom of the screen
				goto clearfog;
			}

			if (viewport->globaluclip * ds->sz1 > texheight - textop && viewport->globaluclip * ds->sz2 > texheight - textop)
			{ // Texture bottom is above the top of the screen
				goto clearfog;
			}

			if ((clip3d->fake3D & FAKE3D_CLIPBOTTOM) && textop < clip3d->sclipBottom - ViewPos.Z)
			{
				notrelevant = true;
				goto clearfog;
			}
			if ((clip3d->fake3D & FAKE3D_CLIPTOP) && textop - texheight > clip3d->sclipTop - ViewPos.Z)
			{
				notrelevant = true;
				goto clearfog;
			}

			WallC.sz1 = ds->sz1;
			WallC.sz2 = ds->sz2;
			WallC.sx1 = ds->sx1;
			WallC.sx2 = ds->sx2;

			if (clip3d->fake3D & FAKE3D_CLIPTOP)
			{
				wallupper.Project(textop < clip3d->sclipTop - ViewPos.Z ? textop : clip3d->sclipTop - ViewPos.Z, &WallC);
			}
			else
			{
				wallupper.Project(textop, &WallC);
			}
			if (clip3d->fake3D & FAKE3D_CLIPBOTTOM)
			{
				walllower.Project(textop - texheight > clip3d->sclipBottom - ViewPos.Z ? textop - texheight : clip3d->sclipBottom - ViewPos.Z, &WallC);
			}
			else
			{
				walllower.Project(textop - texheight, &WallC);
			}

			for (i = x1; i < x2; i++)
			{
				if (wallupper.ScreenY[i] < mceilingclip[i])
					wallupper.ScreenY[i] = mceilingclip[i];
			}
			for (i = x1; i < x2; i++)
			{
				if (walllower.ScreenY[i] > mfloorclip[i])
					walllower.ScreenY[i] = mfloorclip[i];
			}

			if (clip3d->CurrentSkybox)
			{ // Midtex clipping doesn't work properly with skyboxes, since you're normally below the floor
			  // or above the ceiling, so the appropriate end won't be clipped automatically when adding
			  // this drawseg.
				if ((curline->linedef->flags & ML_CLIP_MIDTEX) ||
					(curline->sidedef->Flags & WALLF_CLIP_MIDTEX) ||
					(ib_compatflags & BCOMPATF_CLIPMIDTEX))
				{
					ClipMidtex(x1, x2);
				}
			}

			mfloorclip = walllower.ScreenY;
			mceilingclip = wallupper.ScreenY;

			// draw the columns one at a time
			if (visible)
			{
				for (int x = x1; x < x2; ++x)
				{
					if (cameraLight->FixedColormap() == nullptr && cameraLight->FixedLightLevel() < 0)
					{
						columndrawerargs.SetLight(basecolormap, rw_light, wallshade);
					}

					fixed_t iscale = xs_Fix<16>::ToFix(MaskedSWall[x] * MaskedScaleY);
					double sprtopscreen;
					if (sprflipvert)
						sprtopscreen = viewport->CenterY + texturemid * spryscale;
					else
						sprtopscreen = viewport->CenterY - texturemid * spryscale;

					columndrawerargs.DrawMaskedColumn(Thread, x, iscale, tex, maskedtexturecol[x], spryscale, sprtopscreen, sprflipvert, mfloorclip, mceilingclip);

					rw_light += rw_lightstep;
					spryscale += rw_scalestep;
				}
			}
		}
		else
		{ // Texture does wrap vertically.
			if (tex->bWorldPanning)
			{
				// rowoffset is added before the multiply so that the masked texture will
				// still be positioned in world units rather than texels.
				texturemid = (texturemid - ViewPos.Z + rowoffset) * MaskedScaleY;
			}
			else
			{
				// rowoffset is added outside the multiply so that it positions the texture
				// by texels instead of world units.
				texturemid = (texturemid - ViewPos.Z) * MaskedScaleY + rowoffset;
			}

			WallC.sz1 = ds->sz1;
			WallC.sz2 = ds->sz2;
			WallC.sx1 = ds->sx1;
			WallC.sx2 = ds->sx2;

			if (clip3d->CurrentSkybox)
			{ // Midtex clipping doesn't work properly with skyboxes, since you're normally below the floor
			  // or above the ceiling, so the appropriate end won't be clipped automatically when adding
			  // this drawseg.
				if ((curline->linedef->flags & ML_CLIP_MIDTEX) ||
					(curline->sidedef->Flags & WALLF_CLIP_MIDTEX) ||
					(ib_compatflags & BCOMPATF_CLIPMIDTEX))
				{
					ClipMidtex(x1, x2);
				}
			}

			if (clip3d->fake3D & FAKE3D_CLIPTOP)
			{
				wallupper.Project(clip3d->sclipTop - ViewPos.Z, &WallC);
				for (i = x1; i < x2; i++)
				{
					if (wallupper.ScreenY[i] < mceilingclip[i])
						wallupper.ScreenY[i] = mceilingclip[i];
				}
				mceilingclip = wallupper.ScreenY;
			}
			if (clip3d->fake3D & FAKE3D_CLIPBOTTOM)
			{
				walllower.Project(clip3d->sclipBottom - ViewPos.Z, &WallC);
				for (i = x1; i < x2; i++)
				{
					if (walllower.ScreenY[i] > mfloorclip[i])
						walllower.ScreenY[i] = mfloorclip[i];
				}
				mfloorclip = walllower.ScreenY;
			}

			rw_offset = 0;
			rw_pic = tex;

			double top, bot;
			GetMaskedWallTopBottom(ds, top, bot);

			RenderWallPart renderWallpart(Thread);
			renderWallpart.Render(walldrawerargs, frontsector, curline, WallC, rw_pic, x1, x2, mceilingclip, mfloorclip, texturemid, MaskedSWall, maskedtexturecol, ds->yscale, top, bot, true, wallshade, rw_offset, rw_light, rw_lightstep, nullptr, ds->foggy, basecolormap);
		}

	clearfog:
		if (ds->bFakeBoundary & 3)
		{
			RenderFakeWallRange(ds, x1, x2, wallshade);
		}
		if (!notrelevant)
		{
			if (clip3d->fake3D & FAKE3D_REFRESHCLIP)
			{
				if (!wrap)
				{
					assert(ds->bkup != nullptr);
					memcpy(ds->sprtopclip, ds->bkup, (ds->x2 - ds->x1) * 2);
				}
			}
			else
			{
				fillshort(ds->sprtopclip - ds->x1 + x1, x2 - x1, viewheight);
			}
		}
		return;
	}

	// kg3D - render one fake wall
	void RenderDrawSegment::RenderFakeWall(DrawSegment *ds, int x1, int x2, F3DFloor *rover, int wallshade, FDynamicColormap *basecolormap)
	{
		int i;
		double xscale;
		double yscale;

		fixed_t Alpha = Scale(rover->alpha, OPAQUE, 255);
		if (Alpha <= 0)
			return;

		WallDrawerArgs drawerargs;
		drawerargs.SetStyle(true, (rover->flags & FF_ADDITIVETRANS) != 0, Alpha);

		rw_lightstep = ds->lightstep;
		rw_light = ds->light + (x1 - ds->x1) * rw_lightstep;

		short *mfloorclip = ds->sprbottomclip - ds->x1;
		short *mceilingclip = ds->sprtopclip - ds->x1;

		//double spryscale = ds->iscale + ds->iscalestep * (x1 - ds->x1);
		float *MaskedSWall = ds->swall - ds->x1;

		// find positioning
		side_t *scaledside;
		side_t::ETexpart scaledpart;
		if (rover->flags & FF_UPPERTEXTURE)
		{
			scaledside = curline->sidedef;
			scaledpart = side_t::top;
		}
		else if (rover->flags & FF_LOWERTEXTURE)
		{
			scaledside = curline->sidedef;
			scaledpart = side_t::bottom;
		}
		else
		{
			scaledside = rover->master->sidedef[0];
			scaledpart = side_t::mid;
		}
		xscale = rw_pic->Scale.X * scaledside->GetTextureXScale(scaledpart);
		yscale = rw_pic->Scale.Y * scaledside->GetTextureYScale(scaledpart);

		double rowoffset = curline->sidedef->GetTextureYOffset(side_t::mid) + rover->master->sidedef[0]->GetTextureYOffset(side_t::mid);
		double planez = rover->model->GetPlaneTexZ(sector_t::ceiling);
		rw_offset = FLOAT2FIXED(curline->sidedef->GetTextureXOffset(side_t::mid) + rover->master->sidedef[0]->GetTextureXOffset(side_t::mid));
		if (rowoffset < 0)
		{
			rowoffset += rw_pic->GetHeight();
		}
		double texturemid = (planez - ViewPos.Z) * yscale;
		if (rw_pic->bWorldPanning)
		{
			// rowoffset is added before the multiply so that the masked texture will
			// still be positioned in world units rather than texels.

			texturemid = texturemid + rowoffset * yscale;
			rw_offset = xs_RoundToInt(rw_offset * xscale);
		}
		else
		{
			// rowoffset is added outside the multiply so that it positions the texture
			// by texels instead of world units.
			texturemid += rowoffset;
		}

		CameraLight *cameraLight = CameraLight::Instance();
		if (cameraLight->FixedLightLevel() >= 0)
			drawerargs.SetLight((r_fullbrightignoresectorcolor) ? &FullNormalLight : basecolormap, 0, cameraLight->FixedLightLevelShade());
		else if (cameraLight->FixedColormap() != nullptr)
			drawerargs.SetLight(cameraLight->FixedColormap(), 0, 0);

		WallC.sz1 = ds->sz1;
		WallC.sz2 = ds->sz2;
		WallC.sx1 = ds->sx1;
		WallC.sx2 = ds->sx2;
		WallC.tleft.X = ds->cx;
		WallC.tleft.Y = ds->cy;
		WallC.tright.X = ds->cx + ds->cdx;
		WallC.tright.Y = ds->cy + ds->cdy;
		WallT = ds->tmapvals;

		Clip3DFloors *clip3d = Thread->Clip3D.get();
		wallupper.Project(clip3d->sclipTop - ViewPos.Z, &WallC);
		walllower.Project(clip3d->sclipBottom - ViewPos.Z, &WallC);

		for (i = x1; i < x2; i++)
		{
			if (wallupper.ScreenY[i] < mceilingclip[i])
				wallupper.ScreenY[i] = mceilingclip[i];
		}
		for (i = x1; i < x2; i++)
		{
			if (walllower.ScreenY[i] > mfloorclip[i])
				walllower.ScreenY[i] = mfloorclip[i];
		}

		ProjectedWallTexcoords walltexcoords;
		walltexcoords.ProjectPos(curline->sidedef->TexelLength*xscale, ds->sx1, ds->sx2, WallT);

		double top, bot;
		GetMaskedWallTopBottom(ds, top, bot);

		RenderWallPart renderWallpart(Thread);
		renderWallpart.Render(drawerargs, frontsector, curline, WallC, rw_pic, x1, x2, wallupper.ScreenY, walllower.ScreenY, texturemid, MaskedSWall, walltexcoords.UPos, yscale, top, bot, true, wallshade, rw_offset, rw_light, rw_lightstep, nullptr, ds->foggy, basecolormap);
	}

	// kg3D - walls of fake floors
	void RenderDrawSegment::RenderFakeWallRange(DrawSegment *ds, int x1, int x2, int wallshade)
	{
		FTexture *const DONT_DRAW = ((FTexture*)(intptr_t)-1);
		int i, j;
		F3DFloor *rover, *fover = nullptr;
		int passed, last;
		double floorHeight;
		double ceilingHeight;

		curline = ds->curline;

		frontsector = curline->frontsector;
		backsector = curline->backsector;

		if (backsector == nullptr)
		{
			return;
		}
		if ((ds->bFakeBoundary & 3) == 2)
		{
			sector_t *sec = backsector;
			backsector = frontsector;
			frontsector = sec;
		}

		floorHeight = backsector->CenterFloor();
		ceilingHeight = backsector->CenterCeiling();

		Clip3DFloors *clip3d = Thread->Clip3D.get();

		// maybe fix clipheights
		if (!(clip3d->fake3D & FAKE3D_CLIPBOTTOM)) clip3d->sclipBottom = floorHeight;
		if (!(clip3d->fake3D & FAKE3D_CLIPTOP)) clip3d->sclipTop = ceilingHeight;

		// maybe not visible
		if (clip3d->sclipBottom >= frontsector->CenterCeiling()) return;
		if (clip3d->sclipTop <= frontsector->CenterFloor()) return;

		if (clip3d->fake3D & FAKE3D_DOWN2UP)
		{ // bottom to viewz
			last = 0;
			for (i = backsector->e->XFloor.ffloors.Size() - 1; i >= 0; i--)
			{
				rover = backsector->e->XFloor.ffloors[i];
				if (!(rover->flags & FF_EXISTS)) continue;

				// visible?
				passed = 0;
				if (!(rover->flags & FF_RENDERSIDES) || rover->top.plane->isSlope() || rover->bottom.plane->isSlope() ||
					rover->top.plane->Zat0() <= clip3d->sclipBottom ||
					rover->bottom.plane->Zat0() >= ceilingHeight ||
					rover->top.plane->Zat0() <= floorHeight)
				{
					if (!i)
					{
						passed = 1;
					}
					else
					{
						continue;
					}
				}

				rw_pic = nullptr;
				if (rover->bottom.plane->Zat0() >= clip3d->sclipTop || passed)
				{
					if (last)
					{
						break;
					}
					// maybe wall from inside rendering?
					fover = nullptr;
					for (j = frontsector->e->XFloor.ffloors.Size() - 1; j >= 0; j--)
					{
						fover = frontsector->e->XFloor.ffloors[j];
						if (fover->model == rover->model)
						{ // never
							fover = nullptr;
							break;
						}
						if (!(fover->flags & FF_EXISTS)) continue;
						if (!(fover->flags & FF_RENDERSIDES)) continue;
						// no sloped walls, it's bugged
						if (fover->top.plane->isSlope() || fover->bottom.plane->isSlope()) continue;

						// visible?
						if (fover->top.plane->Zat0() <= clip3d->sclipBottom) continue; // no
						if (fover->bottom.plane->Zat0() >= clip3d->sclipTop)
						{ // no, last possible
							fover = nullptr;
							break;
						}
						// it is, render inside?
						if (!(fover->flags & (FF_BOTHPLANES | FF_INVERTPLANES)))
						{ // no
							fover = nullptr;
						}
						break;
					}
					// nothing
					if (!fover || j == -1)
					{
						break;
					}
					// correct texture
					if (fover->flags & rover->flags & FF_SWIMMABLE)
					{	// don't ever draw (but treat as something has been found)
						rw_pic = DONT_DRAW;
					}
					else if (fover->flags & FF_UPPERTEXTURE)
					{
						rw_pic = TexMan(curline->sidedef->GetTexture(side_t::top), true);
					}
					else if (fover->flags & FF_LOWERTEXTURE)
					{
						rw_pic = TexMan(curline->sidedef->GetTexture(side_t::bottom), true);
					}
					else
					{
						rw_pic = TexMan(fover->master->sidedef[0]->GetTexture(side_t::mid), true);
					}
				}
				else if (frontsector->e->XFloor.ffloors.Size())
				{
					// maybe not visible?
					fover = nullptr;
					for (j = frontsector->e->XFloor.ffloors.Size() - 1; j >= 0; j--)
					{
						fover = frontsector->e->XFloor.ffloors[j];
						if (fover->model == rover->model) // never
						{
							break;
						}
						if (!(fover->flags & FF_EXISTS)) continue;
						if (!(fover->flags & FF_RENDERSIDES)) continue;
						// no sloped walls, it's bugged
						if (fover->top.plane->isSlope() || fover->bottom.plane->isSlope()) continue;

						// visible?
						if (fover->top.plane->Zat0() <= clip3d->sclipBottom) continue; // no
						if (fover->bottom.plane->Zat0() >= clip3d->sclipTop)
						{ // visible, last possible
							fover = nullptr;
							break;
						}
						if ((fover->flags & FF_SOLID) == (rover->flags & FF_SOLID) &&
							!(!(fover->flags & FF_SOLID) && (fover->alpha == 255 || rover->alpha == 255))
							)
						{
							break;
						}
						if (fover->flags & rover->flags & FF_SWIMMABLE)
						{ // don't ever draw (but treat as something has been found)
							rw_pic = DONT_DRAW;
						}
						fover = nullptr; // visible
						break;
					}
					if (fover && j != -1)
					{
						fover = nullptr;
						last = 1;
						continue; // not visible
					}
				}
				if (!rw_pic)
				{
					fover = nullptr;
					if (rover->flags & FF_UPPERTEXTURE)
					{
						rw_pic = TexMan(curline->sidedef->GetTexture(side_t::top), true);
					}
					else if (rover->flags & FF_LOWERTEXTURE)
					{
						rw_pic = TexMan(curline->sidedef->GetTexture(side_t::bottom), true);
					}
					else
					{
						rw_pic = TexMan(rover->master->sidedef[0]->GetTexture(side_t::mid), true);
					}
				}
				// correct colors now
				FDynamicColormap *basecolormap = frontsector->ColorMap;
				wallshade = ds->shade;
				CameraLight *cameraLight = CameraLight::Instance();
				if (cameraLight->FixedLightLevel() < 0)
				{
					if ((ds->bFakeBoundary & 3) == 2)
					{
						for (j = backsector->e->XFloor.lightlist.Size() - 1; j >= 0; j--)
						{
							if (clip3d->sclipTop <= backsector->e->XFloor.lightlist[j].plane.Zat0())
							{
								lightlist_t *lit = &backsector->e->XFloor.lightlist[j];
								basecolormap = lit->extra_colormap;
								bool foggy = (level.fadeto || basecolormap->Fade || (level.flags & LEVEL_HASFADETABLE)); // [RH] set foggy flag
								wallshade = LightVisibility::LightLevelToShade(curline->sidedef->GetLightLevel(ds->foggy, *lit->p_lightlevel, lit->lightsource != nullptr) + LightVisibility::ActualExtraLight(ds->foggy), foggy);
								break;
							}
						}
					}
					else
					{
						for (j = frontsector->e->XFloor.lightlist.Size() - 1; j >= 0; j--)
						{
							if (clip3d->sclipTop <= frontsector->e->XFloor.lightlist[j].plane.Zat0())
							{
								lightlist_t *lit = &frontsector->e->XFloor.lightlist[j];
								basecolormap = lit->extra_colormap;
								bool foggy = (level.fadeto || basecolormap->Fade || (level.flags & LEVEL_HASFADETABLE)); // [RH] set foggy flag
								wallshade = LightVisibility::LightLevelToShade(curline->sidedef->GetLightLevel(ds->foggy, *lit->p_lightlevel, lit->lightsource != nullptr) + LightVisibility::ActualExtraLight(ds->foggy), foggy);
								break;
							}
						}
					}
				}
				if (rw_pic != DONT_DRAW)
				{
					RenderFakeWall(ds, x1, x2, fover ? fover : rover, wallshade, basecolormap);
				}
				else rw_pic = nullptr;
				break;
			}
		}
		else
		{ // top to viewz
			for (i = 0; i < (int)backsector->e->XFloor.ffloors.Size(); i++)
			{
				rover = backsector->e->XFloor.ffloors[i];
				if (!(rover->flags & FF_EXISTS)) continue;

				// visible?
				passed = 0;
				if (!(rover->flags & FF_RENDERSIDES) ||
					rover->top.plane->isSlope() || rover->bottom.plane->isSlope() ||
					rover->bottom.plane->Zat0() >= clip3d->sclipTop ||
					rover->top.plane->Zat0() <= floorHeight ||
					rover->bottom.plane->Zat0() >= ceilingHeight)
				{
					if ((unsigned)i == backsector->e->XFloor.ffloors.Size() - 1)
					{
						passed = 1;
					}
					else
					{
						continue;
					}
				}
				rw_pic = nullptr;
				if (rover->top.plane->Zat0() <= clip3d->sclipBottom || passed)
				{ // maybe wall from inside rendering?
					fover = nullptr;
					for (j = 0; j < (int)frontsector->e->XFloor.ffloors.Size(); j++)
					{
						fover = frontsector->e->XFloor.ffloors[j];
						if (fover->model == rover->model)
						{ // never
							fover = nullptr;
							break;
						}
						if (!(fover->flags & FF_EXISTS)) continue;
						if (!(fover->flags & FF_RENDERSIDES)) continue;
						// no sloped walls, it's bugged
						if (fover->top.plane->isSlope() || fover->bottom.plane->isSlope()) continue;

						// visible?
						if (fover->bottom.plane->Zat0() >= clip3d->sclipTop) continue; // no
						if (fover->top.plane->Zat0() <= clip3d->sclipBottom)
						{ // no, last possible
							fover = nullptr;
							break;
						}
						// it is, render inside?
						if (!(fover->flags & (FF_BOTHPLANES | FF_INVERTPLANES)))
						{ // no
							fover = nullptr;
						}
						break;
					}
					// nothing
					if (!fover || (unsigned)j == frontsector->e->XFloor.ffloors.Size())
					{
						break;
					}
					// correct texture
					if (fover->flags & rover->flags & FF_SWIMMABLE)
					{
						rw_pic = DONT_DRAW;	// don't ever draw (but treat as something has been found)
					}
					else if (fover->flags & FF_UPPERTEXTURE)
					{
						rw_pic = TexMan(curline->sidedef->GetTexture(side_t::top), true);
					}
					else if (fover->flags & FF_LOWERTEXTURE)
					{
						rw_pic = TexMan(curline->sidedef->GetTexture(side_t::bottom), true);
					}
					else
					{
						rw_pic = TexMan(fover->master->sidedef[0]->GetTexture(side_t::mid), true);
					}
				}
				else if (frontsector->e->XFloor.ffloors.Size())
				{ // maybe not visible?
					fover = nullptr;
					for (j = 0; j < (int)frontsector->e->XFloor.ffloors.Size(); j++)
					{
						fover = frontsector->e->XFloor.ffloors[j];
						if (fover->model == rover->model)
						{ // never
							break;
						}
						if (!(fover->flags & FF_EXISTS)) continue;
						if (!(fover->flags & FF_RENDERSIDES)) continue;
						// no sloped walls, its bugged
						if (fover->top.plane->isSlope() || fover->bottom.plane->isSlope()) continue;

						// visible?
						if (fover->bottom.plane->Zat0() >= clip3d->sclipTop) continue; // no
						if (fover->top.plane->Zat0() <= clip3d->sclipBottom)
						{ // visible, last possible
							fover = nullptr;
							break;
						}
						if ((fover->flags & FF_SOLID) == (rover->flags & FF_SOLID) &&
							!(!(rover->flags & FF_SOLID) && (fover->alpha == 255 || rover->alpha == 255))
							)
						{
							break;
						}
						if (fover->flags & rover->flags & FF_SWIMMABLE)
						{ // don't ever draw (but treat as something has been found)
							rw_pic = DONT_DRAW;
						}
						fover = nullptr; // visible
						break;
					}
					if (fover && (unsigned)j != frontsector->e->XFloor.ffloors.Size())
					{ // not visible
						break;
					}
				}
				if (rw_pic == nullptr)
				{
					fover = nullptr;
					if (rover->flags & FF_UPPERTEXTURE)
					{
						rw_pic = TexMan(curline->sidedef->GetTexture(side_t::top), true);
					}
					else if (rover->flags & FF_LOWERTEXTURE)
					{
						rw_pic = TexMan(curline->sidedef->GetTexture(side_t::bottom), true);
					}
					else
					{
						rw_pic = TexMan(rover->master->sidedef[0]->GetTexture(side_t::mid), true);
					}
				}
				// correct colors now
				FDynamicColormap *basecolormap = frontsector->ColorMap;
				wallshade = ds->shade;
				CameraLight *cameraLight = CameraLight::Instance();
				if (cameraLight->FixedLightLevel() < 0)
				{
					if ((ds->bFakeBoundary & 3) == 2)
					{
						for (j = backsector->e->XFloor.lightlist.Size() - 1; j >= 0; j--)
						{
							if (clip3d->sclipTop <= backsector->e->XFloor.lightlist[j].plane.Zat0())
							{
								lightlist_t *lit = &backsector->e->XFloor.lightlist[j];
								basecolormap = lit->extra_colormap;
								bool foggy = (level.fadeto || basecolormap->Fade || (level.flags & LEVEL_HASFADETABLE)); // [RH] set foggy flag
								wallshade = LightVisibility::LightLevelToShade(curline->sidedef->GetLightLevel(ds->foggy, *lit->p_lightlevel, lit->lightsource != nullptr) + LightVisibility::ActualExtraLight(ds->foggy), foggy);
								break;
							}
						}
					}
					else
					{
						for (j = frontsector->e->XFloor.lightlist.Size() - 1; j >= 0; j--)
						{
							if (clip3d->sclipTop <= frontsector->e->XFloor.lightlist[j].plane.Zat0())
							{
								lightlist_t *lit = &frontsector->e->XFloor.lightlist[j];
								basecolormap = lit->extra_colormap;
								bool foggy = (level.fadeto || basecolormap->Fade || (level.flags & LEVEL_HASFADETABLE)); // [RH] set foggy flag
								wallshade = LightVisibility::LightLevelToShade(curline->sidedef->GetLightLevel(ds->foggy, *lit->p_lightlevel, lit->lightsource != nullptr) + LightVisibility::ActualExtraLight(ds->foggy), foggy);
								break;
							}
						}
					}
				}

				if (rw_pic != DONT_DRAW)
				{
					RenderFakeWall(ds, x1, x2, fover ? fover : rover, wallshade, basecolormap);
				}
				else
				{
					rw_pic = nullptr;
				}
				break;
			}
		}
		return;
	}

	// Clip a midtexture to the floor and ceiling of the sector in front of it.
	void RenderDrawSegment::ClipMidtex(int x1, int x2)
	{
		ProjectedWallLine most;

		RenderPortal *renderportal = Thread->Portal.get();

		most.Project(curline->frontsector->ceilingplane, &WallC, curline, renderportal->MirrorFlags & RF_XFLIP);
		for (int i = x1; i < x2; ++i)
		{
			if (wallupper.ScreenY[i] < most.ScreenY[i])
				wallupper.ScreenY[i] = most.ScreenY[i];
		}
		most.Project(curline->frontsector->floorplane, &WallC, curline, renderportal->MirrorFlags & RF_XFLIP);
		for (int i = x1; i < x2; ++i)
		{
			if (walllower.ScreenY[i] > most.ScreenY[i])
				walllower.ScreenY[i] = most.ScreenY[i];
		}
	}

	void RenderDrawSegment::GetMaskedWallTopBottom(DrawSegment *ds, double &top, double &bot)
	{
		double frontcz1 = ds->curline->frontsector->ceilingplane.ZatPoint(ds->curline->v1);
		double frontfz1 = ds->curline->frontsector->floorplane.ZatPoint(ds->curline->v1);
		double frontcz2 = ds->curline->frontsector->ceilingplane.ZatPoint(ds->curline->v2);
		double frontfz2 = ds->curline->frontsector->floorplane.ZatPoint(ds->curline->v2);
		top = MAX(frontcz1, frontcz2);
		bot = MIN(frontfz1, frontfz2);

		Clip3DFloors *clip3d = Thread->Clip3D.get();
		if (clip3d->fake3D & FAKE3D_CLIPTOP)
		{
			top = MIN(top, clip3d->sclipTop);
		}
		if (clip3d->fake3D & FAKE3D_CLIPBOTTOM)
		{
			bot = MAX(bot, clip3d->sclipBottom);
		}
	}
}
