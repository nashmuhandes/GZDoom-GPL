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
#include <float.h>
#include "templates.h"
#include "i_system.h"
#include "w_wad.h"
#include "doomdef.h"
#include "doomstat.h"
#include "r_sky.h"
#include "stats.h"
#include "v_video.h"
#include "a_sharedglobal.h"
#include "c_console.h"
#include "cmdlib.h"
#include "d_net.h"
#include "g_level.h"
#include "v_palette.h"
#include "r_data/colormaps.h"
#include "gl/dynlights/gl_dynlight.h"
#include "swrenderer/drawers/r_draw_rgba.h"
#include "swrenderer/scene/r_opaque_pass.h"
#include "swrenderer/scene/r_3dfloors.h"
#include "swrenderer/scene/r_portal.h"
#include "swrenderer/segments/r_clipsegment.h"
#include "swrenderer/segments/r_drawsegment.h"
#include "swrenderer/line/r_fogboundary.h"
#include "swrenderer/r_memory.h"
#include "swrenderer/scene/r_light.h"

#ifdef _MSC_VER
#pragma warning(disable:4244)
#endif

namespace swrenderer
{
	void RenderFogBoundary::Render(RenderThread *thread, int x1, int x2, short *uclip, short *dclip, int wallshade, float lightleft, float lightstep, FDynamicColormap *basecolormap)
	{
		// This is essentially the same as R_MapVisPlane but with an extra step
		// to create new horizontal spans whenever the light changes enough that
		// we need to use a new colormap.

		float light = lightleft + lightstep*(x2 - x1 - 1);
		int x = x2 - 1;
		int t2 = uclip[x];
		int b2 = dclip[x];
		int rcolormap = GETPALOOKUP(light, wallshade);
		int lcolormap;
		uint8_t *basecolormapdata = basecolormap->Maps;

		if (b2 > t2)
		{
			fillshort(spanend + t2, b2 - t2, x);
		}

		drawerargs.SetLight(basecolormap, (float)light, wallshade);

		uint8_t *fake_dc_colormap = basecolormap->Maps + (GETPALOOKUP(light, wallshade) << COLORMAPSHIFT);

		for (--x; x >= x1; --x)
		{
			int t1 = uclip[x];
			int b1 = dclip[x];
			const int xr = x + 1;
			int stop;

			light -= lightstep;
			lcolormap = GETPALOOKUP(light, wallshade);
			if (lcolormap != rcolormap)
			{
				if (t2 < b2 && rcolormap != 0)
				{ // Colormap 0 is always the identity map, so rendering it is
				  // just a waste of time.
					RenderSection(thread, t2, b2, xr);
				}
				if (t1 < t2) t2 = t1;
				if (b1 > b2) b2 = b1;
				if (t2 < b2)
				{
					fillshort(spanend + t2, b2 - t2, x);
				}
				rcolormap = lcolormap;
				drawerargs.SetLight(basecolormap, (float)light, wallshade);
				fake_dc_colormap = basecolormap->Maps + (GETPALOOKUP(light, wallshade) << COLORMAPSHIFT);
			}
			else
			{
				if (fake_dc_colormap != basecolormapdata)
				{
					stop = MIN(t1, b2);
					while (t2 < stop)
					{
						int y = t2++;
						drawerargs.DrawFogBoundaryLine(thread, y, xr, spanend[y]);
					}
					stop = MAX(b1, t2);
					while (b2 > stop)
					{
						int y = --b2;
						drawerargs.DrawFogBoundaryLine(thread, y, xr, spanend[y]);
					}
				}
				else
				{
					t2 = MAX(t2, MIN(t1, b2));
					b2 = MIN(b2, MAX(b1, t2));
				}

				stop = MIN(t2, b1);
				while (t1 < stop)
				{
					spanend[t1++] = x;
				}
				stop = MAX(b2, t2);
				while (b1 > stop)
				{
					spanend[--b1] = x;
				}
			}

			t2 = uclip[x];
			b2 = dclip[x];
		}
		if (t2 < b2 && rcolormap != 0)
		{
			RenderSection(thread, t2, b2, x1);
		}
	}

	void RenderFogBoundary::RenderSection(RenderThread *thread, int y, int y2, int x1)
	{
		for (; y < y2; ++y)
		{
			drawerargs.DrawFogBoundaryLine(thread, y, x1, spanend[y]);
		}
	}
}
