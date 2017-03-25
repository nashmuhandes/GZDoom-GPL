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

#include "vectors.h"
#include "r_wallsetup.h"
#include "swrenderer/segments/r_clipsegment.h"

struct seg_t;
struct subsector_t;
struct sector_t;
struct side_t;
struct line_t;
struct FDynamicColormap;

namespace swrenderer
{
	class RenderThread;
	struct VisiblePlane;

	struct FWallCoords
	{
		FVector2	tleft;		// coords at left of wall in view space				rx1,ry1
		FVector2	tright;		// coords at right of wall in view space			rx2,ry2

		float		sz1, sz2;	// depth at left, right of wall in screen space		yb1,yb2
		short		sx1, sx2;	// x coords at left, right of wall in screen space	xb1,xb2

		bool Init(RenderThread *thread, const DVector2 &pt1, const DVector2 &pt2, double too_close);
	};

	struct FWallTmapVals
	{
		float		UoverZorg, UoverZstep;
		float		InvZorg, InvZstep;

		void InitFromWallCoords(RenderThread *thread, const FWallCoords *wallc);
		void InitFromLine(RenderThread *thread, const DVector2 &left, const DVector2 &right);
	};

	struct WallPartTexture
	{
		fixed_t TextureOffsetU;
		double TextureMid;
		double TextureScaleU;
		double TextureScaleV;
		FTexture *Texture;
	};

	class SWRenderLine : VisibleSegmentRenderer
	{
	public:
		SWRenderLine(RenderThread *thread);
		void Render(seg_t *line, subsector_t *subsector, sector_t *sector, sector_t *fakebacksector, VisiblePlane *floorplane, VisiblePlane *ceilingplane, bool foggy, FDynamicColormap *basecolormap);

		RenderThread *Thread = nullptr;

	private:
		bool RenderWallSegment(int x1, int x2) override;
		void SetWallVariables(bool needlights);
		void SetTopTexture();
		void SetMiddleTexture();
		void SetBottomTexture();
		void ClipSegmentTopBottom(int x1, int x2);
		void MarkCeilingPlane(int x1, int x2);
		void MarkFloorPlane(int x1, int x2);
		void Mark3DFloors(int x1, int x2);
		void MarkOpaquePassClip(int x1, int x2);
		void RenderTopTexture(int x1, int x2);
		void RenderMiddleTexture(int x1, int x2);
		void RenderBottomTexture(int x1, int x2);

		bool IsFogBoundary(sector_t *front, sector_t *back) const;
		bool SkyboxCompare(sector_t *frontsector, sector_t *backsector) const;

		bool IsInvisibleLine() const;
		bool IsDoorClosed() const;
		bool IsSolid() const;

		bool ShouldMarkFloor() const;
		bool ShouldMarkCeiling() const;
		bool ShouldMarkPortal() const;

		// Line variables:

		subsector_t *mSubsector;
		sector_t *mFrontSector;
		sector_t *mBackSector;
		VisiblePlane *mFloorPlane;
		VisiblePlane *mCeilingPlane;
		seg_t *mLineSegment;

		double mBackCeilingZ1;
		double mBackCeilingZ2;
		double mBackFloorZ1;
		double mBackFloorZ2;
		double mFrontCeilingZ1;
		double mFrontCeilingZ2;
		double mFrontFloorZ1;
		double mFrontFloorZ2;

		bool mDoorClosed;

		FWallCoords WallC;
		FWallTmapVals WallT;

		bool foggy;
		FDynamicColormap *basecolormap;

		// Wall segment variables:

		bool rw_prepped;

		int wallshade;
		float rw_lightstep;
		float rw_lightleft;

		double lwallscale;

		bool markfloor; // False if the back side is the same plane.
		bool markceiling;
		
		WallPartTexture mTopPart;
		WallPartTexture mMiddlePart;
		WallPartTexture mBottomPart;

		ProjectedWallCull mCeilingClipped;
		ProjectedWallCull mFloorClipped;

		ProjectedWallLine walltop;
		ProjectedWallLine wallbottom;
		ProjectedWallLine wallupper;
		ProjectedWallLine walllower;
		ProjectedWallTexcoords walltexcoords;

		sector_t tempsec; // killough 3/8/98: ceiling/water hack
	};
}
