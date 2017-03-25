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

#pragma once

#include "r_visiblesprite.h"
#include "swrenderer/scene/r_opaque_pass.h"

struct particle_t;

namespace swrenderer
{
	class RenderParticle : public VisibleSprite
	{
	public:
		static void Project(RenderThread *thread, particle_t *, const sector_t *sector, int shade, WaterFakeSide fakeside, bool foggy);

	protected:
		bool IsParticle() const override { return true; }
		void Render(RenderThread *thread, short *cliptop, short *clipbottom, int minZ, int maxZ) override;

	private:
		void DrawMaskedSegsBehindParticle(RenderThread *thread);

		fixed_t xscale = 0;
		fixed_t	startfrac = 0; // horizontal position of x1
		int y1 = 0, y2 = 0;

		uint32_t Translation = 0;
		uint32_t FillColor = 0;
	};
}
