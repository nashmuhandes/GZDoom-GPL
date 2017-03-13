/*
** v_draw.cpp
** Draw patches and blocks to a canvas
**
**---------------------------------------------------------------------------
** Copyright 1998-2008 Randy Heit
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

#define NO_SWRENDER 	// set this if you want to exclude the software renderer. Without software renderer the base implementations of DrawTextureV and FillSimplePoly need to be disabled because they depend on it.

#include <stdio.h>
#include <stdarg.h>

#include "doomtype.h"
#include "v_video.h"
#include "m_swap.h"
#include "r_defs.h"
#include "r_utility.h"
#include "r_data/r_translate.h"
#include "doomstat.h"
#include "v_palette.h"
#include "gi.h"
#include "g_level.h"
#include "st_stuff.h"
#include "sbar.h"

#include "i_system.h"
#include "i_video.h"
#include "templates.h"
#include "d_net.h"
#include "colormatcher.h"
#include "r_data/colormaps.h"
#include "g_levellocals.h"

CUSTOM_CVAR(Int, uiscale, 2, CVAR_ARCHIVE | CVAR_NOINITCALL)
{
	if (StatusBar != NULL)
	{
		StatusBar->ScreenSizeChanged();
	}
}

// [RH] Stretch values to make a 320x200 image best fit the screen
// without using fractional steppings
int CleanXfac, CleanYfac;

// [RH] Effective screen sizes that the above scale values give you
int CleanWidth, CleanHeight;

// Above minus 1 (or 1, if they are already 1)
int CleanXfac_1, CleanYfac_1, CleanWidth_1, CleanHeight_1;

// FillSimplePoly uses this
extern "C" short spanend[MAXHEIGHT];

CVAR (Bool, hud_scale, true, CVAR_ARCHIVE);

// For routines that take RGB colors, cache the previous lookup in case there
// are several repetitions with the same color.
static int LastPal = -1;
static uint32 LastRGB;

DEFINE_ACTION_FUNCTION(_Screen, GetWidth)
{
	PARAM_PROLOGUE;
	ACTION_RETURN_INT(screen->GetWidth());
}

DEFINE_ACTION_FUNCTION(_Screen, GetHeight)
{
	PARAM_PROLOGUE;
	ACTION_RETURN_INT(screen->GetHeight());
}

DEFINE_ACTION_FUNCTION(_Screen, PaletteColor)
{
	PARAM_PROLOGUE;
	PARAM_INT(index);
	if (index < 0 || index > 255) index = 0;
	else index = GPalette.BaseColors[index];
	ACTION_RETURN_INT(index);
}

static int PalFromRGB(uint32 rgb)
{
	if (LastPal >= 0 && LastRGB == rgb)
	{
		return LastPal;
	}
	// Quick check for black and white.
	if (rgb == MAKEARGB(255,0,0,0))
	{
		LastPal = GPalette.BlackIndex;
	}
	else if (rgb == MAKEARGB(255,255,255,255))
	{
		LastPal = GPalette.WhiteIndex;
	}
	else
	{
		LastPal = ColorMatcher.Pick(RPART(rgb), GPART(rgb), BPART(rgb));
	}
	LastRGB = rgb;
	return LastPal;
}


void DCanvas::DrawTexture (FTexture *img, double x, double y, int tags_first, ...)
{
	Va_List tags;
	va_start(tags.list, tags_first);
	DrawParms parms;

	bool res = ParseDrawTextureTags(img, x, y, tags_first, tags, &parms, false);
	va_end(tags.list);
	if (!res)
	{
		return;
	}
	DrawTextureParms(img, parms);
}

int ListGetInt(VMVa_List &tags);

void DCanvas::DrawTexture(FTexture *img, double x, double y, VMVa_List &args)
{
	DrawParms parms;
	uint32_t tag = ListGetInt(args);
	bool res = ParseDrawTextureTags(img, x, y, tag, args, &parms, false);
	if (!res) return;
	DrawTextureParms(img, parms);
}

DEFINE_ACTION_FUNCTION(_Screen, DrawTexture)
{
	PARAM_PROLOGUE;
	PARAM_INT(texid);
	PARAM_BOOL(animate);
	PARAM_FLOAT(x);
	PARAM_FLOAT(y);

	FTexture *tex = animate ? TexMan(FSetTextureID(texid)) : TexMan[FSetTextureID(texid)];
	VMVa_List args = { param + 4, 0, numparam - 4 };
	screen->DrawTexture(tex, x, y, args);
	return 0;
}

DEFINE_ACTION_FUNCTION(_Screen, DrawHUDTexture)
{
	PARAM_PROLOGUE;
	PARAM_INT(texid);
	PARAM_FLOAT(x);
	PARAM_FLOAT(y);
	screen->DrawTexture(TexMan(FSetTextureID(texid)), x, y, DTA_HUDRules, HUD_Normal, TAG_END);
	return 0;
}

void DCanvas::DrawTextureParms(FTexture *img, DrawParms &parms)
{
#ifndef NO_SWRENDER
	using namespace swrenderer;
	using namespace drawerargs;

	static short bottomclipper[MAXWIDTH], topclipper[MAXWIDTH];
	const BYTE *translation = NULL;

	if (APART(parms.colorOverlay) != 0)
	{
		// The software renderer cannot invert the source without inverting the overlay
		// too. That means if the source is inverted, we need to do the reverse of what
		// the invert overlay flag says to do.
		INTBOOL invertoverlay = (parms.style.Flags & STYLEF_InvertOverlay);

		if (parms.style.Flags & STYLEF_InvertSource)
		{
			invertoverlay = !invertoverlay;
		}
		if (invertoverlay)
		{
			parms.colorOverlay = PalEntry(parms.colorOverlay).InverseColor();
		}
		// Note that this overrides the translation in software, but not in hardware.
		FDynamicColormap *colormap = GetSpecialLights(MAKERGB(255,255,255),
			parms.colorOverlay & MAKEARGB(0,255,255,255), 0);
		translation = &colormap->Maps[(APART(parms.colorOverlay)*NUMCOLORMAPS/255)*256];
	}
	else if (parms.remap != NULL)
	{
		translation = parms.remap->Remap;
	}

	if (translation != NULL)
	{
		dc_colormap = (lighttable_t *)translation;
	}
	else
	{
		dc_colormap = identitymap;
	}

	fixedcolormap = dc_colormap;
	ESPSResult mode = R_SetPatchStyle (parms.style, parms.Alpha, 0, parms.fillcolor);

	BYTE *destorgsave = dc_destorg;
	dc_destorg = screen->GetBuffer();
	if (dc_destorg == NULL)
	{
		I_FatalError("Attempt to write to buffer of hardware canvas");
	}

	double x0 = parms.x - parms.left * parms.destwidth / parms.texwidth;
	double y0 = parms.y - parms.top * parms.destheight / parms.texheight;

	if (mode != DontDraw)
	{
		int stop4;

		double centeryback = CenterY;
		CenterY = 0;

		// There is not enough precision in the drawing routines to keep the full
		// precision for y0. :(
		modf(y0, &sprtopscreen);

		double yscale = parms.destheight / img->GetHeight();
		double iyscale = 1 / yscale;

		spryscale = yscale;
		assert(spryscale > 0);

		sprflipvert = false;
		//dc_iscale = FLOAT2FIXED(iyscale);
		//dc_texturemid = (-y0) * iyscale;
		//dc_iscale = 0xffffffffu / (unsigned)spryscale;
		dc_iscale = FLOAT2FIXED(1 / spryscale);
		dc_texturemid = (CenterY - 1 - sprtopscreen) * dc_iscale / 65536;
		fixed_t frac = 0;
		double xiscale = img->GetWidth() / parms.destwidth;
		double x2 = x0 + parms.destwidth;

		if (bottomclipper[0] != parms.dclip)
		{
			fillshort(bottomclipper, screen->GetWidth(), (short)parms.dclip);
		}
		if (parms.uclip != 0)
		{
			if (topclipper[0] != parms.uclip)
			{
				fillshort(topclipper, screen->GetWidth(), (short)parms.uclip);
			}
			mceilingclip = topclipper;
		}
		else
		{
			mceilingclip = zeroarray;
		}
		mfloorclip = bottomclipper;

		if (parms.flipX)
		{
			frac = (img->GetWidth() << FRACBITS) - 1;
			xiscale = -xiscale;
		}

		if (parms.windowleft > 0 || parms.windowright < parms.texwidth)
		{
			double wi = MIN(parms.windowright, parms.texwidth);
			double xscale = parms.destwidth / parms.texwidth;
			x0 += parms.windowleft * xscale;
			frac += FLOAT2FIXED(parms.windowleft);
			x2 -= (parms.texwidth - wi) * xscale;
		}
		if (x0 < parms.lclip)
		{
			frac += FLOAT2FIXED((parms.lclip - x0) * xiscale);
			x0 = parms.lclip;
		}
		if (x2 > parms.rclip)
		{
			x2 = parms.rclip;
		}

		// Drawing short output ought to fit in the data cache well enough
		// if we draw one column at a time, so do that, since it's simpler.
		if (parms.destheight < 32 || (parms.dclip - parms.uclip) < 32)
		{
			mode = DoDraw0;
		}

		dc_x = int(x0);
		int x2_i = int(x2);
		fixed_t xiscale_i = FLOAT2FIXED(xiscale);

		if (mode == DoDraw0)
		{
			// One column at a time
			stop4 = dc_x;
		}
		else	 // DoDraw1`
		{
			// Up to four columns at a time
			stop4 = x2_i & ~3;
		}

		if (dc_x < x2_i)
		{
			while ((dc_x < stop4) && (dc_x & 3))
			{
				R_DrawMaskedColumn(img, frac, false, !parms.masked);
				dc_x++;
				frac += xiscale_i;
			}

			while (dc_x < stop4)
			{
				rt_initcols();
				for (int zz = 4; zz; --zz)
				{
					R_DrawMaskedColumn(img, frac, true, !parms.masked);
					dc_x++;
					frac += xiscale_i;
				}
				rt_draw4cols(dc_x - 4);
			}

			while (dc_x < x2_i)
			{
				R_DrawMaskedColumn(img, frac, false, !parms.masked);
				dc_x++;
				frac += xiscale_i;
			}
		}
		CenterY = centeryback;
	}
	R_FinishSetPatchStyle ();

	dc_destorg = destorgsave;

	if (ticdup != 0 && menuactive == MENU_Off)
	{
		NetUpdate();
	}
#endif
}

bool DCanvas::SetTextureParms(DrawParms *parms, FTexture *img, double xx, double yy) const
{
	if (img != NULL)
	{
		parms->x = xx;
		parms->y = yy;
		parms->texwidth = img->GetScaledWidthDouble();
		parms->texheight = img->GetScaledHeightDouble();
		if (parms->top == INT_MAX || parms->fortext)
		{
			parms->top = img->GetScaledTopOffset();
		}
		if (parms->left == INT_MAX || parms->fortext)
		{
			parms->left = img->GetScaledLeftOffset();
		}
		if (parms->destwidth == INT_MAX || parms->fortext)
		{
			parms->destwidth = img->GetScaledWidthDouble();
		}
		if (parms->destheight == INT_MAX || parms->fortext)
		{
			parms->destheight = img->GetScaledHeightDouble();
		}

		switch (parms->cleanmode)
		{
		default:
			break;

		case DTA_Clean:
			parms->x = (parms->x - 160.0) * CleanXfac + (Width * 0.5);
			parms->y = (parms->y - 100.0) * CleanYfac + (Height * 0.5);
			parms->destwidth = parms->texwidth * CleanXfac;
			parms->destheight = parms->texheight * CleanYfac;
			break;

		case DTA_CleanNoMove:
			parms->destwidth = parms->texwidth * CleanXfac;
			parms->destheight = parms->texheight * CleanYfac;
			break;

		case DTA_CleanNoMove_1:
			parms->destwidth = parms->texwidth * CleanXfac_1;
			parms->destheight = parms->texheight * CleanYfac_1;
			break;

		case DTA_Fullscreen:
			parms->x = parms->y = 0;
			break;

		case DTA_HUDRules:
		case DTA_HUDRulesC:
		{
			bool xright = parms->x < 0;
			bool ybot = parms->y < 0;

			if (hud_scale)
			{
				parms->x *= CleanXfac;
				if (parms->cleanmode == DTA_HUDRulesC)
					parms->x += Width * 0.5;
				else if (xright)
					parms->x = Width + parms->x;
				parms->y *= CleanYfac;
				if (ybot)
					parms->y = Height + parms->y;
				parms->destwidth = parms->texwidth * CleanXfac;
				parms->destheight = parms->texheight * CleanYfac;
			}
			else
			{
				if (parms->cleanmode == DTA_HUDRulesC)
					parms->x += Width * 0.5;
				else if (xright)
					parms->x = Width + parms->x;
				if (ybot)
					parms->y = Height + parms->y;
			}
			break;
		}
		}
		if (parms->virtWidth != Width || parms->virtHeight != Height)
		{
			VirtualToRealCoords(parms->x, parms->y, parms->destwidth, parms->destheight,
				parms->virtWidth, parms->virtHeight, parms->virtBottom, !parms->keepratio);
		}
	}

	return false;
}

static void ListEnd(Va_List &tags)
{
	va_end(tags.list);
}

static int ListGetInt(Va_List &tags)
{
	return va_arg(tags.list, int);
}

static inline double ListGetDouble(Va_List &tags)
{
	return va_arg(tags.list, double);
}

// These two options are only being used by the D3D version of the HUD weapon drawer, they serve no purpose anywhere else.
static inline FSpecialColormap * ListGetSpecialColormap(Va_List &tags)
{
	return va_arg(tags.list, FSpecialColormap *);
}

static inline FColormapStyle * ListGetColormapStyle(Va_List &tags)
{
	return va_arg(tags.list, FColormapStyle *);
}

static void ListEnd(VMVa_List &tags)
{
}

int ListGetInt(VMVa_List &tags)
{
	if (tags.curindex < tags.numargs)
	{
		if (tags.args[tags.curindex].Type == REGT_INT)
		{
			return tags.args[tags.curindex++].i;
		}
		ThrowAbortException(X_OTHER, "Invalid parameter in draw function, int expected");
	}
	return TAG_DONE;
}

static inline double ListGetDouble(VMVa_List &tags)
{
	if (tags.curindex < tags.numargs && tags.args[tags.curindex].Type == REGT_FLOAT)
	{
		return tags.args[tags.curindex++].f;
	}
	ThrowAbortException(X_OTHER, "Invalid parameter in draw function, float expected");
	return 0;
}

static inline FSpecialColormap * ListGetSpecialColormap(VMVa_List &tags)
{
	ThrowAbortException(X_OTHER, "Invalid tag in draw function");
	return nullptr;
}

static inline FColormapStyle * ListGetColormapStyle(VMVa_List &tags)
{
	ThrowAbortException(X_OTHER, "Invalid tag in draw function");
	return nullptr;
}

template<class T>
bool DCanvas::ParseDrawTextureTags(FTexture *img, double x, double y, DWORD tag, T& tags, DrawParms *parms, bool fortext) const
{
	INTBOOL boolval;
	int intval;
	bool translationset = false;
	bool fillcolorset = false;

	if (!fortext)
	{
		if (img == NULL || img->UseType == FTexture::TEX_Null)
		{
			ListEnd(tags);
			return false;
		}
	}

	// Do some sanity checks on the coordinates.
	if (x < -16383 || x > 16383 || y < -16383 || y > 16383)
	{
		ListEnd(tags);
		return false;
	}

	parms->fortext = fortext;
	parms->windowleft = 0;
	parms->windowright = INT_MAX;
	parms->dclip = this->GetHeight();
	parms->uclip = 0;
	parms->lclip = 0;
	parms->rclip = this->GetWidth();
	parms->left = INT_MAX;
	parms->top = INT_MAX;
	parms->destwidth = INT_MAX;
	parms->destheight = INT_MAX;
	parms->Alpha = 1.f;
	parms->fillcolor = -1;
	parms->remap = NULL;
	parms->colorOverlay = 0;
	parms->alphaChannel = false;
	parms->flipX = false;
	//parms->shadowAlpha = 0;
	parms->shadowColor = 0;
	parms->virtWidth = this->GetWidth();
	parms->virtHeight = this->GetHeight();
	parms->keepratio = false;
	parms->style.BlendOp = 255;		// Dummy "not set" value
	parms->masked = true;
	parms->bilinear = false;
	parms->specialcolormap = NULL;
	parms->colormapstyle = NULL;
	parms->cleanmode = DTA_Base;
	parms->scalex = parms->scaley = 1;
	parms->cellx = parms->celly = 0;
	parms->maxstrlen = INT_MAX;
	parms->virtBottom = false;

	// Parse the tag list for attributes. (For floating point attributes,
	// consider that the C ABI dictates that all floats be promoted to
	// doubles when passed as function arguments.)
	while (tag != TAG_DONE)
	{
		switch (tag)
		{
		default:
			ListGetInt(tags);
			break;

		case DTA_DestWidth:
			assert(fortext == false);
			if (fortext) return false;
			parms->cleanmode = DTA_Base;
			parms->destwidth = ListGetInt(tags);
			break;

		case DTA_DestWidthF:
			assert(fortext == false);
			if (fortext) return false;
			parms->cleanmode = DTA_Base;
			parms->destwidth = ListGetDouble(tags);
			break;

		case DTA_DestHeight:
			assert(fortext == false);
			if (fortext) return false;
			parms->cleanmode = DTA_Base;
			parms->destheight = ListGetInt(tags);
			break;

		case DTA_DestHeightF:
			assert(fortext == false);
			if (fortext) return false;
			parms->cleanmode = DTA_Base;
			parms->destheight = ListGetDouble(tags);
			break;

		case DTA_Clean:
			boolval = ListGetInt(tags);
			if (boolval)
			{
				parms->scalex = 1;
				parms->scaley = 1;
				parms->cleanmode = tag;
			}
			break;

		case DTA_CleanNoMove:
			boolval = ListGetInt(tags);
			if (boolval)
			{
				parms->scalex = CleanXfac;
				parms->scaley = CleanYfac;
				parms->cleanmode = tag;
			}
			break;

		case DTA_CleanNoMove_1:
			boolval = ListGetInt(tags);
			if (boolval)
			{
				parms->scalex = CleanXfac_1;
				parms->scaley = CleanYfac_1;
				parms->cleanmode = tag;
			}
			break;

		case DTA_320x200:
			boolval = ListGetInt(tags);
			if (boolval)
			{
				parms->cleanmode = DTA_Base;
				parms->scalex = 1;
				parms->scaley = 1;
				parms->virtWidth = 320;
				parms->virtHeight = 200;
			}
			break;

		case DTA_Bottom320x200:
			boolval = ListGetInt(tags);
			if (boolval)
			{
				parms->cleanmode = DTA_Base;
				parms->scalex = 1;
				parms->scaley = 1;
				parms->virtWidth = 320;
				parms->virtHeight = 200;
			}
			parms->virtBottom = true;
			break;

		case DTA_HUDRules:
			intval = ListGetInt(tags);
			parms->cleanmode = intval == HUD_HorizCenter ? DTA_HUDRulesC : DTA_HUDRules;
			break;

		case DTA_VirtualWidth:
			parms->cleanmode = DTA_Base;
			parms->virtWidth = ListGetInt(tags);
			break;

		case DTA_VirtualWidthF:
			parms->cleanmode = DTA_Base;
			parms->virtWidth = ListGetDouble(tags);
			break;

		case DTA_VirtualHeight:
			parms->cleanmode = DTA_Base;
			parms->virtHeight = ListGetInt(tags);
			break;

		case DTA_VirtualHeightF:
			parms->cleanmode = DTA_Base;
			parms->virtHeight = ListGetDouble(tags);
			break;

		case DTA_Fullscreen:
			boolval = ListGetInt(tags);
			if (boolval)
			{
				assert(fortext == false);
				if (img == NULL) return false;
				parms->cleanmode = DTA_Fullscreen;
				parms->virtWidth = img->GetScaledWidthDouble();
				parms->virtHeight = img->GetScaledHeightDouble();
			}
			break;

		case DTA_Alpha:
			parms->Alpha = (float)(MIN<double>(1., ListGetDouble(tags)));
			break;

		case DTA_AlphaChannel:
			parms->alphaChannel = ListGetInt(tags);
			break;

		case DTA_FillColor:
			parms->fillcolor = ListGetInt(tags);
			fillcolorset = true;
			break;

		case DTA_TranslationIndex:
			parms->remap = TranslationToTable(ListGetInt(tags));
			break;

		case DTA_ColorOverlay:
			parms->colorOverlay = ListGetInt(tags);
			break;

		case DTA_FlipX:
			parms->flipX = ListGetInt(tags);
			break;

		case DTA_TopOffset:
			assert(fortext == false);
			if (fortext) return false;
			parms->top = ListGetInt(tags);
			break;

		case DTA_TopOffsetF:
			assert(fortext == false);
			if (fortext) return false;
			parms->top = ListGetDouble(tags);
			break;

		case DTA_LeftOffset:
			assert(fortext == false);
			if (fortext) return false;
			parms->left = ListGetInt(tags);
			break;

		case DTA_LeftOffsetF:
			assert(fortext == false);
			if (fortext) return false;
			parms->left = ListGetDouble(tags);
			break;

		case DTA_CenterOffset:
			assert(fortext == false);
			if (fortext) return false;
			if (ListGetInt(tags))
			{
				parms->left = img->GetScaledWidthDouble() * 0.5;
				parms->top = img->GetScaledHeightDouble() * 0.5;
			}
			break;

		case DTA_CenterBottomOffset:
			assert(fortext == false);
			if (fortext) return false;
			if (ListGetInt(tags))
			{
				parms->left = img->GetScaledWidthDouble() * 0.5;
				parms->top = img->GetScaledHeightDouble();
			}
			break;

		case DTA_WindowLeft:
			assert(fortext == false);
			if (fortext) return false;
			parms->windowleft = ListGetInt(tags);
			break;

		case DTA_WindowLeftF:
			assert(fortext == false);
			if (fortext) return false;
			parms->windowleft = ListGetDouble(tags);
			break;

		case DTA_WindowRight:
			assert(fortext == false);
			if (fortext) return false;
			parms->windowright = ListGetInt(tags);
			break;

		case DTA_WindowRightF:
			assert(fortext == false);
			if (fortext) return false;
			parms->windowright = ListGetDouble(tags);
			break;

		case DTA_ClipTop:
			parms->uclip = ListGetInt(tags);
			if (parms->uclip < 0)
			{
				parms->uclip = 0;
			}
			break;

		case DTA_ClipBottom:
			parms->dclip = ListGetInt(tags);
			if (parms->dclip > this->GetHeight())
			{
				parms->dclip = this->GetHeight();
			}
			break;

		case DTA_ClipLeft:
			parms->lclip = ListGetInt(tags);
			if (parms->lclip < 0)
			{
				parms->lclip = 0;
			}
			break;

		case DTA_ClipRight:
			parms->rclip = ListGetInt(tags);
			if (parms->rclip > this->GetWidth())
			{
				parms->rclip = this->GetWidth();
			}
			break;

		case DTA_ShadowAlpha:
			//parms->shadowAlpha = (float)MIN(1., ListGetDouble(tags));
			break;

		case DTA_ShadowColor:
			parms->shadowColor = ListGetInt(tags);
			break;

		case DTA_Shadow:
			boolval = ListGetInt(tags);
			if (boolval)
			{
				//parms->shadowAlpha = 0.5;
				parms->shadowColor = 0;
			}
			else
			{
				//parms->shadowAlpha = 0;
			}
			break;

		case DTA_Masked:
			parms->masked = ListGetInt(tags);
			break;

		case DTA_BilinearFilter:
			parms->bilinear = ListGetInt(tags);
			break;

		case DTA_KeepRatio:
			// I think this is a terribly misleading name, since it actually turns
			// *off* aspect ratio correction.
			parms->keepratio = ListGetInt(tags);
			break;

		case DTA_RenderStyle:
			parms->style.AsDWORD = ListGetInt(tags);
			break;

		case DTA_SpecialColormap:
			parms->specialcolormap = ListGetSpecialColormap(tags);
			break;

		case DTA_ColormapStyle:
			parms->colormapstyle = ListGetColormapStyle(tags);
			break;

		case DTA_TextLen:
			parms->maxstrlen = ListGetInt(tags);
			break;

		case DTA_CellX:
			parms->cellx = ListGetInt(tags);
			break;

		case DTA_CellY:
			parms->celly = ListGetInt(tags);
			break;

		}
		tag = ListGetInt(tags);
	}
	ListEnd(tags);

	if (parms->remap != nullptr && parms->remap->Inactive)
	{ // If it's inactive, pretend we were passed NULL instead.
		parms->remap = nullptr;
	}

	if (parms->uclip >= parms->dclip || parms->lclip >= parms->rclip)
	{
		return false;
	}

	if (img != NULL)
	{
		SetTextureParms(parms, img, x, y);

		if (parms->destwidth <= 0 || parms->destheight <= 0)
		{
			return false;
		}
	}

	if (parms->style.BlendOp == 255)
	{
		if (fillcolorset)
		{
			if (parms->alphaChannel)
			{
				parms->style = STYLE_Shaded;
			}
			else if (parms->Alpha < 1.f)
			{
				parms->style = STYLE_TranslucentStencil;
			}
			else
			{
				parms->style = STYLE_Stencil;
			}
		}
		else if (parms->Alpha < 1.f)
		{
			parms->style = STYLE_Translucent;
		}
		else
		{
			parms->style = STYLE_Normal;
		}
	}
	return true;
}
// explicitly instantiate both versions for v_text.cpp.

template bool DCanvas::ParseDrawTextureTags<Va_List>(FTexture *img, double x, double y, DWORD tag, Va_List& tags, DrawParms *parms, bool fortext) const;
template bool DCanvas::ParseDrawTextureTags<VMVa_List>(FTexture *img, double x, double y, DWORD tag, VMVa_List& tags, DrawParms *parms, bool fortext) const;

void DCanvas::VirtualToRealCoords(double &x, double &y, double &w, double &h,
	double vwidth, double vheight, bool vbottom, bool handleaspect) const
{
	float myratio = handleaspect ? ActiveRatio (Width, Height) : (4.0f / 3.0f);

    // if 21:9 AR, map to 16:9 for all callers.
    // this allows for black bars and stops the stretching of fullscreen images
    if (myratio > 1.7f) {
        myratio = 16.0f / 9.0f;
    }

	double right = x + w;
	double bottom = y + h;

	if (myratio > 1.334f)
	{ // The target surface is either 16:9 or 16:10, so expand the
	  // specified virtual size to avoid undesired stretching of the
	  // image. Does not handle non-4:3 virtual sizes. I'll worry about
	  // those if somebody expresses a desire to use them.
		x = (x - vwidth * 0.5) * Width * 960 / (vwidth * AspectBaseWidth(myratio)) + Width * 0.5;
		w = (right - vwidth * 0.5) * Width * 960 / (vwidth * AspectBaseWidth(myratio)) + Width * 0.5 - x;
	}
	else
	{
		x = x * Width / vwidth;
		w = right * Width / vwidth - x;
	}
	if (AspectTallerThanWide(myratio))
	{ // The target surface is 5:4
		y = (y - vheight * 0.5) * Height * 600 / (vheight * AspectBaseHeight(myratio)) + Height * 0.5;
		h = (bottom - vheight * 0.5) * Height * 600 / (vheight * AspectBaseHeight(myratio)) + Height * 0.5 - y;
		if (vbottom)
		{
			y += (Height - Height * AspectMultiplier(myratio) / 48.0) * 0.5;
		}
	}
	else
	{
		y = y * Height / vheight;
		h = bottom * Height / vheight - y;
	}
}

void DCanvas::VirtualToRealCoordsFixed(fixed_t &x, fixed_t &y, fixed_t &w, fixed_t &h,
	int vwidth, int vheight, bool vbottom, bool handleaspect) const
{
	double dx, dy, dw, dh;

	dx = FIXED2DBL(x);
	dy = FIXED2DBL(y);
	dw = FIXED2DBL(w);
	dh = FIXED2DBL(h);
	VirtualToRealCoords(dx, dy, dw, dh, vwidth, vheight, vbottom, handleaspect);
	x = FLOAT2FIXED(dx);
	y = FLOAT2FIXED(dy);
	w = FLOAT2FIXED(dw);
	h = FLOAT2FIXED(dh);
}

void DCanvas::VirtualToRealCoordsInt(int &x, int &y, int &w, int &h,
	int vwidth, int vheight, bool vbottom, bool handleaspect) const
{
	double dx, dy, dw, dh;

	dx = x;
	dy = y;
	dw = w;
	dh = h;
	VirtualToRealCoords(dx, dy, dw, dh, vwidth, vheight, vbottom, handleaspect);
	x = int(dx + 0.5);
	y = int(dy + 0.5);
	w = int(dx + dw + 0.5) - x;
	h = int(dy + dh + 0.5) - y;
}

void DCanvas::FillBorder (FTexture *img)
{
	float myratio = ActiveRatio (Width, Height);

    // if 21:9 AR, fill borders akin to 16:9, since all fullscreen
    // images are being drawn to that scale.
    if (myratio > 1.7f) {
        myratio = 16 / 9.0f;
    }

	if (myratio >= 1.3f && myratio <= 1.4f)
	{ // This is a 4:3 display, so no border to show
		return;
	}
	int bordtop, bordbottom, bordleft, bordright, bord;
	if (AspectTallerThanWide(myratio))
	{ // Screen is taller than it is wide
		bordleft = bordright = 0;
		bord = Height - Height * AspectMultiplier(myratio) / 48;
		bordtop = bord / 2;
		bordbottom = bord - bordtop;
	}
	else
	{ // Screen is wider than it is tall
		bordtop = bordbottom = 0;
		bord = Width - Width * AspectMultiplier(myratio) / 48;
		bordleft = bord / 2;
		bordright = bord - bordleft;
	}

	if (img != NULL)
	{
		FlatFill (0, 0, Width, bordtop, img);									// Top
		FlatFill (0, bordtop, bordleft, Height - bordbottom, img);				// Left
		FlatFill (Width - bordright, bordtop, Width, Height - bordbottom, img);	// Right
		FlatFill (0, Height - bordbottom, Width, Height, img);					// Bottom
	}
	else
	{
		Clear (0, 0, Width, bordtop, GPalette.BlackIndex, 0);									// Top
		Clear (0, bordtop, bordleft, Height - bordbottom, GPalette.BlackIndex, 0);				// Left
		Clear (Width - bordright, bordtop, Width, Height - bordbottom, GPalette.BlackIndex, 0);	// Right
		Clear (0, Height - bordbottom, Width, Height, GPalette.BlackIndex, 0);					// Bottom
	}
}

void DCanvas::PUTTRANSDOT (int xx, int yy, int basecolor, int level)
{
	static int oldyy;
	static int oldyyshifted;

#if 0
	if(xx < 32)
		cc += 7-(xx>>2);
	else if(xx > (finit_width - 32))
		cc += 7-((finit_width-xx) >> 2);
//	if(cc==oldcc) //make sure that we don't double fade the corners.
//	{
		if(yy < 32)
			cc += 7-(yy>>2);
		else if(yy > (finit_height - 32))
			cc += 7-((finit_height-yy) >> 2);
//	}
	if(cc > cm && cm != NULL)
	{
		cc = cm;
	}
	else if(cc > oldcc+6) // don't let the color escape from the fade table...
	{
		cc=oldcc+6;
	}
#endif
	if (yy == oldyy+1)
	{
		oldyy++;
		oldyyshifted += GetPitch();
	}
	else if (yy == oldyy-1)
	{
		oldyy--;
		oldyyshifted -= GetPitch();
	}
	else if (yy != oldyy)
	{
		oldyy = yy;
		oldyyshifted = yy * GetPitch();
	}

	BYTE *spot = GetBuffer() + oldyyshifted + xx;
	DWORD *bg2rgb = Col2RGB8[1+level];
	DWORD *fg2rgb = Col2RGB8[63-level];
	DWORD fg = fg2rgb[basecolor];
	DWORD bg = bg2rgb[*spot];
	bg = (fg+bg) | 0x1f07c1f;
	*spot = RGB32k.All[bg&(bg>>15)];
}

void DCanvas::DrawLine(int x0, int y0, int x1, int y1, int palColor, uint32 realcolor)
//void DrawTransWuLine (int x0, int y0, int x1, int y1, BYTE palColor)
{
	const int WeightingScale = 0;
	const int WEIGHTBITS = 6;
	const int WEIGHTSHIFT = 16-WEIGHTBITS;
	const int NUMWEIGHTS = (1<<WEIGHTBITS);
	const int WEIGHTMASK = (NUMWEIGHTS-1);

	if (palColor < 0)
	{
		palColor = PalFromRGB(realcolor);
	}

	Lock();
	int deltaX, deltaY, xDir;

	if (y0 > y1)
	{
		int temp = y0; y0 = y1; y1 = temp;
		temp = x0; x0 = x1; x1 = temp;
	}

	PUTTRANSDOT (x0, y0, palColor, 0);

	if ((deltaX = x1 - x0) >= 0)
	{
		xDir = 1;
	}
	else
	{
		xDir = -1;
		deltaX = -deltaX;
	}

	if ((deltaY = y1 - y0) == 0)
	{ // horizontal line
		if (x0 > x1)
		{
			swapvalues (x0, x1);
		}
		memset (GetBuffer() + y0*GetPitch() + x0, palColor, deltaX+1);
	}
	else if (deltaX == 0)
	{ // vertical line
		BYTE *spot = GetBuffer() + y0*GetPitch() + x0;
		int pitch = GetPitch ();
		do
		{
			*spot = palColor;
			spot += pitch;
		} while (--deltaY != 0);
	}
	else if (deltaX == deltaY)
	{ // diagonal line.
		BYTE *spot = GetBuffer() + y0*GetPitch() + x0;
		int advance = GetPitch() + xDir;
		do
		{
			*spot = palColor;
			spot += advance;
		} while (--deltaY != 0);
	}
	else
	{
		// line is not horizontal, diagonal, or vertical
		fixed_t errorAcc = 0;

		if (deltaY > deltaX)
		{ // y-major line
			fixed_t errorAdj = (((unsigned)deltaX << 16) / (unsigned)deltaY) & 0xffff;
			if (xDir < 0)
			{
				if (WeightingScale == 0)
				{
					while (--deltaY)
					{
						errorAcc += errorAdj;
						y0++;
						int weighting = (errorAcc >> WEIGHTSHIFT) & WEIGHTMASK;
						PUTTRANSDOT (x0 - (errorAcc >> 16), y0, palColor, weighting);
						PUTTRANSDOT (x0 - (errorAcc >> 16) - 1, y0,
								palColor, WEIGHTMASK - weighting);
					}
				}
				else
				{
					while (--deltaY)
					{
						errorAcc += errorAdj;
						y0++;
						int weighting = ((errorAcc * WeightingScale) >> (WEIGHTSHIFT+8)) & WEIGHTMASK;
						PUTTRANSDOT (x0 - (errorAcc >> 16), y0, palColor, weighting);
						PUTTRANSDOT (x0 - (errorAcc >> 16) - 1, y0,
								palColor, WEIGHTMASK - weighting);
					}
				}
			}
			else
			{
				if (WeightingScale == 0)
				{
					while (--deltaY)
					{
						errorAcc += errorAdj;
						y0++;
						int weighting = (errorAcc >> WEIGHTSHIFT) & WEIGHTMASK;
						PUTTRANSDOT (x0 + (errorAcc >> 16), y0, palColor, weighting);
						PUTTRANSDOT (x0 + (errorAcc >> 16) + xDir, y0,
								palColor, WEIGHTMASK - weighting);
					}
				}
				else
				{
					while (--deltaY)
					{
						errorAcc += errorAdj;
						y0++;
						int weighting = ((errorAcc * WeightingScale) >> (WEIGHTSHIFT+8)) & WEIGHTMASK;
						PUTTRANSDOT (x0 + (errorAcc >> 16), y0, palColor, weighting);
						PUTTRANSDOT (x0 + (errorAcc >> 16) + xDir, y0,
								palColor, WEIGHTMASK - weighting);
					}
				}
			}
		}
		else
		{ // x-major line
			fixed_t errorAdj = (((DWORD) deltaY << 16) / (DWORD) deltaX) & 0xffff;

			if (WeightingScale == 0)
			{
				while (--deltaX)
				{
					errorAcc += errorAdj;
					x0 += xDir;
					int weighting = (errorAcc >> WEIGHTSHIFT) & WEIGHTMASK;
					PUTTRANSDOT (x0, y0 + (errorAcc >> 16), palColor, weighting);
					PUTTRANSDOT (x0, y0 + (errorAcc >> 16) + 1,
							palColor, WEIGHTMASK - weighting);
				}
			}
			else
			{
				while (--deltaX)
				{
					errorAcc += errorAdj;
					x0 += xDir;
					int weighting = ((errorAcc * WeightingScale) >> (WEIGHTSHIFT+8)) & WEIGHTMASK;
					PUTTRANSDOT (x0, y0 + (errorAcc >> 16), palColor, weighting);
					PUTTRANSDOT (x0, y0 + (errorAcc >> 16) + 1,
							palColor, WEIGHTMASK - weighting);
				}
			}
		}
		PUTTRANSDOT (x1, y1, palColor, 0);
	}
	Unlock();
}

void DCanvas::DrawPixel(int x, int y, int palColor, uint32 realcolor)
{
	if (palColor < 0)
	{
		palColor = PalFromRGB(realcolor);
	}

	Buffer[Pitch * y + x] = (BYTE)palColor;
}

//==========================================================================
//
// DCanvas :: Clear
//
// Set an area to a specified color.
//
//==========================================================================

void DCanvas::Clear (int left, int top, int right, int bottom, int palcolor, uint32 color)
{
	int x, y;
	BYTE *dest;

	if (left == right || top == bottom)
	{
		return;
	}

	assert(left < right);
	assert(top < bottom);

	if (left >= Width || right <= 0 || top >= Height || bottom <= 0)
	{
		return;
	}
	left = MAX(0,left);
	right = MIN(Width,right);
	top = MAX(0,top);
	bottom = MIN(Height,bottom);

	if (palcolor < 0)
	{
		if (APART(color) != 255)
		{
			Dim(color, APART(color)/255.f, left, top, right - left, bottom - top);
			return;
		}

		palcolor = PalFromRGB(color);
	}

	dest = Buffer + top * Pitch + left;
	x = right - left;
	for (y = top; y < bottom; y++)
	{
		memset(dest, palcolor, x);
		dest += Pitch;
	}
}

DEFINE_ACTION_FUNCTION(_Screen, Clear)
{
	PARAM_PROLOGUE;
	PARAM_INT(x1);
	PARAM_INT(y1);
	PARAM_INT(x2);
	PARAM_INT(y2);
	PARAM_INT(color);
	PARAM_INT_DEF(palcol);
	screen->Clear(x1, y1, x2, y2, palcol, color);
	return 0;
}

//==========================================================================
//
// DCanvas :: FillSimplePoly
//
// Fills a simple polygon with a texture. Here, "simple" means that a
// horizontal scanline at any vertical position within the polygon will
// not cross it more than twice.
//
// The originx, originy, scale, and rotation parameters specify
// transformation of the filling texture, not of the points.
//
// The points must be specified in clockwise order.
//
//==========================================================================

void DCanvas::FillSimplePoly(FTexture *tex, FVector2 *points, int npoints,
	double originx, double originy, double scalex, double scaley, DAngle rotation,
	FDynamicColormap *colormap, PalEntry flatcolor, int lightlevel, int bottomclip)
{
#ifndef NO_SWRENDER
	using namespace swrenderer;
	using namespace drawerargs;

	// Use an equation similar to player sprites to determine shade
	fixed_t shade = LIGHT2SHADE(lightlevel) - 12*FRACUNIT;
	float topy, boty, leftx, rightx;
	int toppt, botpt, pt1, pt2;
	int i;
	int y1, y2, y;
	fixed_t x;
	bool dorotate = rotation != 0.;
	double cosrot, sinrot;

	if (--npoints < 2 || Buffer == NULL)
	{ // not a polygon or we're not locked
		return;
	}

	if (bottomclip <= 0)
	{
		bottomclip = Height;
	}

	// Find the extents of the polygon, in particular the highest and lowest points.
	for (botpt = toppt = 0, boty = topy = points[0].Y, leftx = rightx = points[0].X, i = 1; i <= npoints; ++i)
	{
		if (points[i].Y < topy)
		{
			topy = points[i].Y;
			toppt = i;
		}
		if (points[i].Y > boty)
		{
			boty = points[i].Y;
			botpt = i;
		}
		if (points[i].X < leftx)
		{
			leftx = points[i].X;
		}
		if (points[i].X > rightx)
		{
			rightx = points[i].X;
		}
	}
	if (topy >= bottomclip ||	// off the bottom of the screen
		boty <= 0 ||			// off the top of the screen
		leftx >= Width ||		// off the right of the screen
		rightx <= 0)			// off the left of the screen
	{
		return;
	}

	BYTE *destorgsave = dc_destorg;
	dc_destorg = screen->GetBuffer();
	if (dc_destorg == NULL)
	{
		I_FatalError("Attempt to write to buffer of hardware canvas");
	}

	scalex /= tex->Scale.X;
	scaley /= tex->Scale.Y;

	// Use the CRT's functions here.
	cosrot = cos(rotation.Radians());
	sinrot = sin(rotation.Radians());

	// Setup constant texture mapping parameters.
	R_SetupSpanBits(tex);
	R_SetSpanColormap(colormap != NULL ? &colormap->Maps[clamp(shade >> FRACBITS, 0, NUMCOLORMAPS-1) * 256] : identitymap);
	R_SetSpanSource(tex);
	if (ds_xbits != 0)
	{
		scalex = double(1u << (32 - ds_xbits)) / scalex;
		ds_xstep = xs_RoundToInt(cosrot * scalex);
	}
	else
	{ // Texture is one pixel wide.
		scalex = 0;
		ds_xstep = 0;
	}
	if (ds_ybits != 0)
	{
		scaley = double(1u << (32 - ds_ybits)) / scaley;
		ds_ystep = xs_RoundToInt(sinrot * scaley);
	}
	else
	{ // Texture is one pixel tall.
		scaley = 0;
		ds_ystep = 0;
	}

	// Travel down the right edge and create an outline of that edge.
	pt1 = toppt;
	pt2 = toppt + 1;	if (pt2 > npoints) pt2 = 0;
	y1 = xs_RoundToInt(points[pt1].Y + 0.5f);
	do
	{
		x = FLOAT2FIXED(points[pt1].X + 0.5f);
		y2 = xs_RoundToInt(points[pt2].Y + 0.5f);
		if (y1 >= y2 || (y1 < 0 && y2 < 0) || (y1 >= bottomclip && y2 >= bottomclip))
		{
		}
		else
		{
			fixed_t xinc = FLOAT2FIXED((points[pt2].X - points[pt1].X) / (points[pt2].Y - points[pt1].Y));
			int y3 = MIN(y2, bottomclip);
			if (y1 < 0)
			{
				x += xinc * -y1;
				y1 = 0;
			}
			for (y = y1; y < y3; ++y)
			{
				spanend[y] = clamp<short>(x >> FRACBITS, -1, Width);
				x += xinc;
			}
		}
		y1 = y2;
		pt1 = pt2;
		pt2++;			if (pt2 > npoints) pt2 = 0;
	} while (pt1 != botpt);

	// Travel down the left edge and fill it in.
	pt1 = toppt;
	pt2 = toppt - 1;	if (pt2 < 0) pt2 = npoints;
	y1 = xs_RoundToInt(points[pt1].Y + 0.5f);
	do
	{
		x = FLOAT2FIXED(points[pt1].X + 0.5f);
		y2 = xs_RoundToInt(points[pt2].Y + 0.5f);
		if (y1 >= y2 || (y1 < 0 && y2 < 0) || (y1 >= bottomclip && y2 >= bottomclip))
		{
		}
		else
		{
			fixed_t xinc = FLOAT2FIXED((points[pt2].X - points[pt1].X) / (points[pt2].Y - points[pt1].Y));
			int y3 = MIN(y2, bottomclip);
			if (y1 < 0)
			{
				x += xinc * -y1;
				y1 = 0;
			}
			for (y = y1; y < y3; ++y)
			{
				int x1 = x >> FRACBITS;
				int x2 = spanend[y];
				if (x2 > x1 && x2 > 0 && x1 < Width)
				{
					x1 = MAX(x1, 0);
					x2 = MIN(x2, Width);
#if 0
					memset(this->Buffer + y * this->Pitch + x1, (int)tex, x2 - x1);
#else
					ds_y = y;
					ds_x1 = x1;
					ds_x2 = x2 - 1;

					DVector2 tex(x1 - originx, y - originy);
					if (dorotate)
					{
						double t = tex.X;
						tex.X = t * cosrot - tex.Y * sinrot;
						tex.Y = tex.Y * cosrot + t * sinrot;
					}
					ds_xfrac = xs_RoundToInt(tex.X * scalex);
					ds_yfrac = xs_RoundToInt(tex.Y * scaley);

					R_DrawSpan();
#endif
				}
				x += xinc;
			}
		}
		y1 = y2;
		pt1 = pt2;
		pt2--;			if (pt2 < 0) pt2 = npoints;
	} while (pt1 != botpt);
	dc_destorg = destorgsave;
#endif
}


/********************************/
/*								*/
/* Other miscellaneous routines */
/*								*/
/********************************/


//
// V_DrawBlock
// Draw a linear block of pixels into the view buffer.
//
void DCanvas::DrawBlock (int x, int y, int _width, int _height, const BYTE *src) const
{
	int srcpitch = _width;
	int destpitch;
	BYTE *dest;

	if (ClipBox (x, y, _width, _height, src, srcpitch))
	{
		return;		// Nothing to draw
	}

	destpitch = Pitch;
	dest = Buffer + y*Pitch + x;

	do
	{
		memcpy (dest, src, _width);
		src += srcpitch;
		dest += destpitch;
	} while (--_height);
}

//
// V_GetBlock
// Gets a linear block of pixels from the view buffer.
//
void DCanvas::GetBlock (int x, int y, int _width, int _height, BYTE *dest) const
{
	const BYTE *src;

#ifdef RANGECHECK 
	if (x<0
		||x+_width > Width
		|| y<0
		|| y+_height>Height)
	{
		I_Error ("Bad V_GetBlock");
	}
#endif

	src = Buffer + y*Pitch + x;

	while (_height--)
	{
		memcpy (dest, src, _width);
		src += Pitch;
		dest += _width;
	}
}

// Returns true if the box was completely clipped. False otherwise.
bool DCanvas::ClipBox (int &x, int &y, int &w, int &h, const BYTE *&src, const int srcpitch) const
{
	if (x >= Width || y >= Height || x+w <= 0 || y+h <= 0)
	{ // Completely clipped off screen
		return true;
	}
	if (x < 0)				// clip left edge
	{
		src -= x;
		w += x;
		x = 0;
	}
	if (x+w > Width)		// clip right edge
	{
		w = Width - x;
	}
	if (y < 0)				// clip top edge
	{
		src -= y*srcpitch;
		h += y;
		y = 0;
	}
	if (y+h > Height)		// clip bottom edge
	{
		h = Height - y;
	}
	return false;
}

//==========================================================================
//
// V_SetBorderNeedRefresh
//
// Flag the border as in need of updating. (Probably because something that
// was on top of it has changed.
//
//==========================================================================

void V_SetBorderNeedRefresh()
{
	if (screen != NULL)
	{
		BorderNeedRefresh = screen->GetPageCount();
	}
}

//==========================================================================
//
// V_DrawFrame
//
// Draw a frame around the specified area using the view border
// frame graphics. The border is drawn outside the area, not in it.
//
//==========================================================================

void V_DrawFrame (int left, int top, int width, int height)
{
	FTexture *p;
	const gameborder_t *border = &gameinfo.Border;
	// Sanity check for incomplete gameinfo
	if (border == NULL)
		return;
	int offset = border->offset;
	int right = left + width;
	int bottom = top + height;

	// Draw top and bottom sides.
	p = TexMan[border->t];
	screen->FlatFill(left, top - p->GetHeight(), right, top, p, true);
	p = TexMan[border->b];
	screen->FlatFill(left, bottom, right, bottom + p->GetHeight(), p, true);

	// Draw left and right sides.
	p = TexMan[border->l];
	screen->FlatFill(left - p->GetWidth(), top, left, bottom, p, true);
	p = TexMan[border->r];
	screen->FlatFill(right, top, right + p->GetWidth(), bottom, p, true);

	// Draw beveled corners.
	screen->DrawTexture (TexMan[border->tl], left-offset, top-offset, TAG_DONE);
	screen->DrawTexture (TexMan[border->tr], left+width, top-offset, TAG_DONE);
	screen->DrawTexture (TexMan[border->bl], left-offset, top+height, TAG_DONE);
	screen->DrawTexture (TexMan[border->br], left+width, top+height, TAG_DONE);
}

DEFINE_ACTION_FUNCTION(_Screen, DrawFrame)
{
	PARAM_PROLOGUE;
	PARAM_INT(x);
	PARAM_INT(y);
	PARAM_INT(w);
	PARAM_INT(h);
	V_DrawFrame(x, y, w, h);
	return 0;
}

//==========================================================================
//
// V_DrawBorder
//
//==========================================================================

void V_DrawBorder (int x1, int y1, int x2, int y2)
{
	FTextureID picnum;

	if (level.info != NULL && level.info->BorderTexture.Len() != 0)
	{
		picnum = TexMan.CheckForTexture (level.info->BorderTexture, FTexture::TEX_Flat);
	}
	else
	{
		picnum = TexMan.CheckForTexture (gameinfo.BorderFlat, FTexture::TEX_Flat);
	}

	if (picnum.isValid())
	{
		screen->FlatFill (x1, y1, x2, y2, TexMan(picnum));
	}
	else
	{
		screen->Clear (x1, y1, x2, y2, 0, 0);
	}
}

//==========================================================================
//
// R_DrawViewBorder
//
// Draws the border around the view for different size windows
//
//==========================================================================

int BorderNeedRefresh;


static void V_DrawViewBorder (void)
{
	// [RH] Redraw the status bar if SCREENWIDTH > status bar width.
	// Will draw borders around itself, too.
	if (SCREENWIDTH > 320)
	{
		ST_SetNeedRefresh();
	}

	if (viewwidth == SCREENWIDTH)
	{
		return;
	}

	V_DrawBorder (0, 0, SCREENWIDTH, viewwindowy);
	V_DrawBorder (0, viewwindowy, viewwindowx, viewheight + viewwindowy);
	V_DrawBorder (viewwindowx + viewwidth, viewwindowy, SCREENWIDTH, viewheight + viewwindowy);
	V_DrawBorder (0, viewwindowy + viewheight, SCREENWIDTH, gST_Y);

	V_DrawFrame (viewwindowx, viewwindowy, viewwidth, viewheight);
	V_MarkRect (0, 0, SCREENWIDTH, gST_Y);
}

//==========================================================================
//
// R_DrawTopBorder
//
// Draws the top border around the view for different size windows
//
//==========================================================================

static void V_DrawTopBorder ()
{
	FTexture *p;
	int offset;

	if (viewwidth == SCREENWIDTH)
		return;

	offset = gameinfo.Border.offset;

	if (viewwindowy < 34)
	{
		V_DrawBorder (0, 0, viewwindowx, 34);
		V_DrawBorder (viewwindowx, 0, viewwindowx + viewwidth, viewwindowy);
		V_DrawBorder (viewwindowx + viewwidth, 0, SCREENWIDTH, 34);
		p = TexMan(gameinfo.Border.t);
		screen->FlatFill(viewwindowx, viewwindowy - p->GetHeight(),
						 viewwindowx + viewwidth, viewwindowy, p, true);

		p = TexMan(gameinfo.Border.l);
		screen->FlatFill(viewwindowx - p->GetWidth(), viewwindowy,
						 viewwindowx, 35, p, true);
		p = TexMan(gameinfo.Border.r);
		screen->FlatFill(viewwindowx + viewwidth, viewwindowy,
						 viewwindowx + viewwidth + p->GetWidth(), 35, p, true);

		p = TexMan(gameinfo.Border.tl);
		screen->DrawTexture (p, viewwindowx - offset, viewwindowy - offset, TAG_DONE);

		p = TexMan(gameinfo.Border.tr);
		screen->DrawTexture (p, viewwindowx + viewwidth, viewwindowy - offset, TAG_DONE);
	}
	else
	{
		V_DrawBorder (0, 0, SCREENWIDTH, 34);
	}
}

//==========================================================================
//
// R_RefreshViewBorder
//
// Draws the border around the player view, if needed.
//
//==========================================================================

void V_RefreshViewBorder ()
{
	if (setblocks < 10)
	{
		if (BorderNeedRefresh)
		{
			BorderNeedRefresh--;
			if (BorderTopRefresh)
			{
				BorderTopRefresh--;
			}
			V_DrawViewBorder();
		}
		else if (BorderTopRefresh)
		{
			BorderTopRefresh--;
			V_DrawTopBorder();
		}
	}
}

