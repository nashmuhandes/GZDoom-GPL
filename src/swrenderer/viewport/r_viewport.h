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

#include <stddef.h>
#include <memory>
#include "r_defs.h"

namespace swrenderer
{
	class RenderViewport
	{
	public:
		static RenderViewport *Instance();

		RenderViewport();
		~RenderViewport();
		
		void SetViewport(int width, int height, float trueratio);
		void SetupFreelook();
		
		DCanvas *RenderTarget = nullptr;

		double FocalLengthX = 0.0;
		double FocalLengthY = 0.0;
		double InvZtoScale = 0.0;
		double WallTMapScale2 = 0.0;
		double CenterX = 0.0;
		double CenterY = 0.0;
		double YaspectMul = 0.0;
		double IYaspectMul = 0.0;
		double globaluclip = 0.0;
		double globaldclip = 0.0;

		fixed_t viewingrangerecip = 0;
		double BaseYaspectMul = 0.0; // yaspectmul without a forced aspect ratio

		// The xtoviewangleangle[] table maps a screen pixel
		// to the lowest viewangle that maps back to x ranges
		// from clipangle to -clipangle.
		angle_t xtoviewangle[MAXWIDTH + 1];

		uint8_t *GetDest(int x, int y);

		bool RenderingToCanvas() const { return RenderTarget != screen; }

		DVector3 PointWorldToView(const DVector3 &worldPos) const;
		DVector3 PointWorldToScreen(const DVector3 &worldPos) const;
		DVector3 PointViewToScreen(const DVector3 &viewPos) const;

		DVector2 PointWorldToView(const DVector2 &worldPos) const;
		DVector2 ScaleViewToScreen(const DVector2 &scale, double viewZ, bool pixelstretch = true) const;

		double PlaneDepth(int screenY, double planeHeight) const
		{
			if (screenY + 0.5 < CenterY)
				return FocalLengthY / (CenterY - screenY - 0.5) * planeHeight;
			else
				return FocalLengthY / (screenY + 0.5 - CenterY) * planeHeight;
		}
		
	private:
		void InitTextureMapping();
		void SetupBuffer();
	};
}
