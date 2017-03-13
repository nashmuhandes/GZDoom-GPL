/*
** gl_nodes.cpp
**
**---------------------------------------------------------------------------
** Copyright 2005-2010 Christoph Oelckers
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
#include <math.h>
#ifdef _MSC_VER
#include <malloc.h>		// for alloca()
#endif

#ifndef _WIN32
#include <unistd.h>

#else
#include <direct.h>

#define rmdir _rmdir

#endif

#include "templates.h"
#include "m_alloc.h"
#include "m_argv.h"
#include "c_dispatch.h"
#include "m_swap.h"
#include "g_game.h"
#include "i_system.h"
#include "w_wad.h"
#include "doomdef.h"
#include "p_local.h"
#include "nodebuild.h"
#include "doomstat.h"
#include "vectors.h"
#include "stats.h"
#include "doomerrors.h"
#include "p_setup.h"
#include "x86.h"
#include "version.h"
#include "md5.h"
#include "m_misc.h"
#include "r_utility.h"
#include "cmdlib.h"
#include "g_levellocals.h"

void P_GetPolySpots (MapData * lump, TArray<FNodeBuilder::FPolyStart> &spots, TArray<FNodeBuilder::FPolyStart> &anchors);

CVAR(Bool, gl_cachenodes, true, CVAR_ARCHIVE|CVAR_GLOBALCONFIG)
CVAR(Float, gl_cachetime, 0.6f, CVAR_ARCHIVE|CVAR_GLOBALCONFIG)

void P_LoadZNodes (FileReader &dalump, uint32_t id);
static bool CheckCachedNodes(MapData *map);
static void CreateCachedNodes(MapData *map);


// fixed 32 bit gl_vert format v2.0+ (glBsp 1.91)
struct mapglvertex_t
{
	int32_t x,y;
};

struct gl3_mapsubsector_t
{
	int32_t numsegs;
	int32_t firstseg;    // Index of first one; segs are stored sequentially.
};

struct glseg_t
{
	uint16_t	v1;		 // start vertex		(16 bit)
	uint16_t	v2;		 // end vertex			(16 bit)
	uint16_t	linedef; // linedef, or -1 for minisegs
	uint16_t	side;	 // side on linedef: 0 for right, 1 for left
	uint16_t	partner; // corresponding partner seg, or 0xffff on one-sided walls
};

struct glseg3_t
{
	int32_t			v1;
	int32_t			v2;
	uint16_t			linedef;
	uint16_t			side;
	int32_t			partner;
};

struct gl5_mapnode_t
{
	int16_t 	x,y,dx,dy;	// partition line
	int16_t 	bbox[2][4];	// bounding box for each child
	// If NF_SUBSECTOR is or'ed in, it's a subsector,
	// else it's a node of another subtree.
	uint32_t children[2];
};



//==========================================================================
//
// Collect all sidedefs which are not entirely covered by segs
// Old ZDBSPs could create such maps. If such a BSP is discovered
// a node rebuild must be done to ensure proper rendering
//
//==========================================================================

static int CheckForMissingSegs()
{
	auto numsides = level.sides.Size();
	double *added_seglen = new double[numsides];
	int missing = 0;

	memset(added_seglen, 0, sizeof(double)*numsides);
	for(int i=0;i<numsegs;i++)
	{
		seg_t * seg = &segs[i];

		if (seg->sidedef!=NULL)
		{
			// check all the segs and calculate the length they occupy on their sidedef
			DVector2 vec1(seg->v2->fX() - seg->v1->fX(), seg->v2->fY() - seg->v1->fY());
			added_seglen[seg->sidedef->Index()] += vec1.Length();
		}
	}

	for(unsigned i=0;i<numsides;i++)
	{
		double linelen = level.sides[i].linedef->Delta().Length();
		missing += (added_seglen[i] < linelen - 1.);
	}

	delete [] added_seglen;
	return missing;
}

//==========================================================================
//
// Checks whether the nodes are suitable for GL rendering
//
//==========================================================================

bool P_CheckForGLNodes()
{
	int i;

	for(i=0;i<numsubsectors;i++)
	{
		subsector_t * sub = &subsectors[i];
		seg_t * firstseg = sub->firstline;
		seg_t * lastseg = sub->firstline + sub->numlines - 1;

		if (firstseg->v1 != lastseg->v2)
		{
			// This subsector is incomplete which means that these
			// are normal nodes
			return false;
		}
		else
		{
			for(uint32_t j=0;j<sub->numlines;j++)
			{
				if (segs[j].linedef==NULL)	// miniseg
				{
					// We already have GL nodes. Great!
					return true;
				}
			}
		}
	}
	// all subsectors were closed but there are no minisegs
	// Although unlikely this can happen. Such nodes are not a problem.
	// all that is left is to check whether the BSP covers all sidedefs completely.
	int missing = CheckForMissingSegs();
	if (missing > 0)
	{
		Printf("%d missing segs counted\nThe BSP needs to be rebuilt.\n", missing);
	}
	return missing == 0;
}


//==========================================================================
//
// LoadGLVertexes
//
// loads GL vertices
//
//==========================================================================

#define gNd2		MAKE_ID('g','N','d','2')
#define gNd4		MAKE_ID('g','N','d','4')
#define gNd5		MAKE_ID('g','N','d','5')

#define GL_VERT_OFFSET  4
static int firstglvertex;
static bool format5;

static bool LoadGLVertexes(FileReader * lump)
{
	uint8_t *gldata;
	int                 i;

	firstglvertex = level.vertexes.Size();
	
	int gllen=lump->GetLength();

	gldata = new uint8_t[gllen];
	lump->Seek(0, SEEK_SET);
	lump->Read(gldata, gllen);

	if (*(int *)gldata == gNd5) 
	{
		format5=true;
	}
	else if (*(int *)gldata != gNd2) 
	{
		// GLNodes V1 and V4 are unsupported.
		// V1 because the precision is insufficient and
		// V4 due to the missing partner segs
		Printf("GL nodes v%d found. This format is not supported by " GAMENAME "\n",
			(*(int *)gldata == gNd4)? 4:1);

		delete [] gldata;
		return false;
	}
	else format5=false;

	mapglvertex_t*	mgl = (mapglvertex_t *)(gldata + GL_VERT_OFFSET);
	unsigned numvertexes = firstglvertex +  (gllen - GL_VERT_OFFSET)/sizeof(mapglvertex_t);

	TStaticArray<vertex_t> oldvertexes = std::move(level.vertexes);
	level.vertexes.Alloc(numvertexes);

	memcpy(&level.vertexes[0], &oldvertexes[0], firstglvertex * sizeof(vertex_t));
	for(auto &line : level.lines)
	{
		// Remap vertex pointers in linedefs
		line.v1 = &level.vertexes[line.v1 - &oldvertexes[0]];
		line.v2 = &level.vertexes[line.v2 - &oldvertexes[0]];
	}

	for (i = firstglvertex; i < (int)numvertexes; i++)
	{
		level.vertexes[i].set(LittleLong(mgl->x)/65536., LittleLong(mgl->y)/65536.);
		mgl++;
	}
	delete[] gldata;
	return true;
}

//==========================================================================
//
// GL Nodes utilities
//
//==========================================================================

static inline int checkGLVertex(int num)
{
	if (num & 0x8000)
		num = (num&0x7FFF)+firstglvertex;
	return num;
}

static inline int checkGLVertex3(int num)
{
	if (num & 0xc0000000)
		num = (num&0x3FFFFFFF)+firstglvertex;
	return num;
}

//==========================================================================
//
// LoadGLSegs
//
//==========================================================================

static bool LoadGLSegs(FileReader * lump)
{
	char		*data;
	int			i;
	line_t		*ldef=NULL;
	
	numsegs = lump->GetLength();
	data= new char[numsegs];
	lump->Seek(0, SEEK_SET);
	lump->Read(data, numsegs);
	segs=NULL;

#ifdef _MSC_VER
	__try
#endif
	{
		if (!format5 && memcmp(data, "gNd3", 4))
		{
			numsegs/=sizeof(glseg_t);
			segs = new seg_t[numsegs];
			memset(segs,0,sizeof(seg_t)*numsegs);
			
			glseg_t * ml = (glseg_t*)data;
			for(i = 0; i < numsegs; i++)
			{							// check for gl-vertices
				segs[i].v1 = &level.vertexes[checkGLVertex(LittleShort(ml->v1))];
				segs[i].v2 = &level.vertexes[checkGLVertex(LittleShort(ml->v2))];
				segs[i].PartnerSeg = ml->partner == 0xFFFF ? nullptr : &segs[LittleShort(ml->partner)];
				if(ml->linedef != 0xffff)
				{
					ldef = &level.lines[LittleShort(ml->linedef)];
					segs[i].linedef = ldef;
	
					
					ml->side=LittleShort(ml->side);
					segs[i].sidedef = ldef->sidedef[ml->side];
					if (ldef->sidedef[ml->side] != NULL)
					{
						segs[i].frontsector = ldef->sidedef[ml->side]->sector;
					}
					else
					{
						segs[i].frontsector = NULL;
					}
					if (ldef->flags & ML_TWOSIDED && ldef->sidedef[ml->side^1] != NULL)
					{
						segs[i].backsector = ldef->sidedef[ml->side^1]->sector;
					}
					else
					{
						ldef->flags &= ~ML_TWOSIDED;
						segs[i].backsector = NULL;
					}
	
				}
				else
				{
					segs[i].linedef = NULL;
					segs[i].sidedef = NULL;
	
					segs[i].frontsector = NULL;
					segs[i].backsector  = NULL;
				}
				ml++;		
			}
		}
		else
		{
			if (!format5) numsegs-=4;
			numsegs/=sizeof(glseg3_t);
			segs = new seg_t[numsegs];
			memset(segs,0,sizeof(seg_t)*numsegs);
			
			glseg3_t * ml = (glseg3_t*)(data+ (format5? 0:4));
			for(i = 0; i < numsegs; i++)
			{							// check for gl-vertices
				segs[i].v1 = &level.vertexes[checkGLVertex3(LittleLong(ml->v1))];
				segs[i].v2 = &level.vertexes[checkGLVertex3(LittleLong(ml->v2))];

				const uint32_t partner = LittleLong(ml->partner);
				segs[i].PartnerSeg = DWORD_MAX == partner ? nullptr : &segs[partner];
	
				if(ml->linedef != 0xffff) // skip minisegs 
				{
					ldef = &level.lines[LittleLong(ml->linedef)];
					segs[i].linedef = ldef;
	
					
					ml->side=LittleShort(ml->side);
					segs[i].sidedef = ldef->sidedef[ml->side];
					if (ldef->sidedef[ml->side] != NULL)
					{
						segs[i].frontsector = ldef->sidedef[ml->side]->sector;
					}
					else
					{
						segs[i].frontsector = NULL;
					}
					if (ldef->flags & ML_TWOSIDED && ldef->sidedef[ml->side^1] != NULL)
					{
						segs[i].backsector = ldef->sidedef[ml->side^1]->sector;
					}
					else
					{
						ldef->flags &= ~ML_TWOSIDED;
						segs[i].backsector = NULL;
					}
	
				}
				else
				{
					segs[i].linedef = NULL;
					segs[i].sidedef = NULL;
					segs[i].frontsector = NULL;
					segs[i].backsector  = NULL;
				}
				ml++;		
			}
		}
		delete [] data;
		return true;
	}
#ifdef _MSC_VER
	__except(1)
	{
		// Invalid data has the bad habit of requiring extensive checks here
		// so let's just catch anything invalid and output a message.
		// (at least under MSVC. GCC can't do SEH even for Windows... :( )
		Printf("Invalid GL segs. The BSP will have to be rebuilt.\n");
		delete [] data;
		delete [] segs;
		segs = NULL;
		return false;
	}
#endif
}


//==========================================================================
//
// LoadGLSubsectors
//
//==========================================================================

static bool LoadGLSubsectors(FileReader * lump)
{
	char * datab;
	int  i;
	
	numsubsectors = lump->GetLength();
	datab = new char[numsubsectors];
	lump->Seek(0, SEEK_SET);
	lump->Read(datab, numsubsectors);
	
	if (numsubsectors == 0)
	{
		delete [] datab;
		return false;
	}
	
	if (!format5 && memcmp(datab, "gNd3", 4))
	{
		mapsubsector_t * data = (mapsubsector_t*) datab;
		numsubsectors /= sizeof(mapsubsector_t);
		subsectors = new subsector_t[numsubsectors];
		memset(subsectors,0,numsubsectors * sizeof(subsector_t));
	
		for (i=0; i<numsubsectors; i++)
		{
			subsectors[i].numlines  = LittleShort(data[i].numsegs );
			subsectors[i].firstline = segs + LittleShort(data[i].firstseg);

			if (subsectors[i].numlines == 0)
			{
				delete [] datab;
				return false;
			}
		}
	}
	else
	{
		gl3_mapsubsector_t * data = (gl3_mapsubsector_t*) (datab+(format5? 0:4));
		numsubsectors /= sizeof(gl3_mapsubsector_t);
		subsectors = new subsector_t[numsubsectors];
		memset(subsectors,0,numsubsectors * sizeof(subsector_t));
	
		for (i=0; i<numsubsectors; i++)
		{
			subsectors[i].numlines  = LittleLong(data[i].numsegs );
			subsectors[i].firstline = segs + LittleLong(data[i].firstseg);

			if (subsectors[i].numlines == 0)
			{
				delete [] datab;
				return false;
			}
		}
	}

	for (i=0; i<numsubsectors; i++)
	{
		for(unsigned j=0;j<subsectors[i].numlines;j++)
		{
			seg_t * seg = subsectors[i].firstline + j;
			if (seg->linedef==NULL) seg->frontsector = seg->backsector = subsectors[i].firstline->frontsector;
		}
		seg_t *firstseg = subsectors[i].firstline;
		seg_t *lastseg = subsectors[i].firstline + subsectors[i].numlines - 1;
		// The subsector must be closed. If it isn't we can't use these nodes and have to do a rebuild.
		if (lastseg->v2 != firstseg->v1)
		{
			delete [] datab;
			return false;
		}

	}
	delete [] datab;
	return true;
}

//==========================================================================
//
// P_LoadNodes
//
//==========================================================================

static bool LoadNodes (FileReader * lump)
{
	const int NF_SUBSECTOR = 0x8000;
	const int GL5_NF_SUBSECTOR = (1 << 31);

	int 		i;
	int 		j;
	int 		k;
	node_t* 	no;
	uint16_t*		used;

	if (!format5)
	{
		mapnode_t*	mn, * basemn;
		numnodes = lump->GetLength() / sizeof(mapnode_t);

		if (numnodes == 0) return false;

		nodes = new node_t[numnodes];		
		lump->Seek(0, SEEK_SET);

		basemn = mn = new mapnode_t[numnodes];
		lump->Read(mn, lump->GetLength());

		used = (uint16_t *)alloca (sizeof(uint16_t)*numnodes);
		memset (used, 0, sizeof(uint16_t)*numnodes);

		no = nodes;

		for (i = 0; i < numnodes; i++, no++, mn++)
		{
			no->x = LittleShort(mn->x)<<FRACBITS;
			no->y = LittleShort(mn->y)<<FRACBITS;
			no->dx = LittleShort(mn->dx)<<FRACBITS;
			no->dy = LittleShort(mn->dy)<<FRACBITS;
			for (j = 0; j < 2; j++)
			{
				uint16_t child = LittleShort(mn->children[j]);
				if (child & NF_SUBSECTOR)
				{
					child &= ~NF_SUBSECTOR;
					if (child >= numsubsectors)
					{
						delete [] basemn;
						return false;
					}
					no->children[j] = (uint8_t *)&subsectors[child] + 1;
				}
				else if (child >= numnodes)
				{
					delete [] basemn;
					return false;
				}
				else if (used[child])
				{
					delete [] basemn;
					return false;
				}
				else
				{
					no->children[j] = &nodes[child];
					used[child] = j + 1;
				}
				for (k = 0; k < 4; k++)
				{
					no->bbox[j][k] = (float)LittleShort(mn->bbox[j][k]);
				}
			}
		}
		delete [] basemn;
	}
	else
	{
		gl5_mapnode_t*	mn, * basemn;
		numnodes = lump->GetLength() / sizeof(gl5_mapnode_t);

		if (numnodes == 0) return false;

		nodes = new node_t[numnodes];		
		lump->Seek(0, SEEK_SET);

		basemn = mn = new gl5_mapnode_t[numnodes];
		lump->Read(mn, lump->GetLength());

		used = (uint16_t *)alloca (sizeof(uint16_t)*numnodes);
		memset (used, 0, sizeof(uint16_t)*numnodes);

		no = nodes;

		for (i = 0; i < numnodes; i++, no++, mn++)
		{
			no->x = LittleShort(mn->x)<<FRACBITS;
			no->y = LittleShort(mn->y)<<FRACBITS;
			no->dx = LittleShort(mn->dx)<<FRACBITS;
			no->dy = LittleShort(mn->dy)<<FRACBITS;
			for (j = 0; j < 2; j++)
			{
				int32_t child = LittleLong(mn->children[j]);
				if (child & GL5_NF_SUBSECTOR)
				{
					child &= ~GL5_NF_SUBSECTOR;
					if (child >= numsubsectors)
					{
						delete [] basemn;
						return false;
					}
					no->children[j] = (uint8_t *)&subsectors[child] + 1;
				}
				else if (child >= numnodes)
				{
					delete [] basemn;
					return false;
				}
				else if (used[child])
				{
					delete [] basemn;
					return false;
				}
				else
				{
					no->children[j] = &nodes[child];
					used[child] = j + 1;
				}
				for (k = 0; k < 4; k++)
				{
					no->bbox[j][k] = (float)LittleShort(mn->bbox[j][k]);
				}
			}
		}
		delete [] basemn;
	}
	return true;
}

//==========================================================================
//
// loads the GL node data
//
//==========================================================================

static bool DoLoadGLNodes(FileReader ** lumps)
{
	if (!LoadGLVertexes(lumps[0]))
	{
		return false;
	}
	if (!LoadGLSegs(lumps[1]))
	{
		delete [] segs;
		segs = NULL;
		return false;
	}
	if (!LoadGLSubsectors(lumps[2]))
	{
		delete [] subsectors;
		subsectors = NULL;
		delete [] segs;
		segs = NULL;
		return false;
	}
	if (!LoadNodes(lumps[3]))
	{
		delete [] nodes;
		nodes = NULL;
		delete [] subsectors;
		subsectors = NULL;
		delete [] segs;
		segs = NULL;
		return false;
	}

	// Quick check for the validity of the nodes
	// For invalid nodes there is a high chance that this test will fail

	for (int i = 0; i < numsubsectors; i++)
	{
		seg_t * seg = subsectors[i].firstline;
		if (!seg->sidedef) 
		{
			Printf("GL nodes contain invalid data. The BSP has to be rebuilt.\n");
			delete [] nodes;
			nodes = NULL;
			delete [] subsectors;
			subsectors = NULL;
			delete [] segs;
			segs = NULL;
			return false;
		}
	}

	// check whether the BSP covers all sidedefs completely.
	int missing = CheckForMissingSegs();
	if (missing > 0)
	{
		Printf("%d missing segs counted in GL nodes.\nThe BSP has to be rebuilt.\n", missing);
	}
	return missing == 0;
}


//===========================================================================
//
// MatchHeader
//
// Checks whether a GL_LEVEL header belongs to this level
//
//===========================================================================

static bool MatchHeader(const char * label, const char * hdata)
{
	if (memcmp(hdata, "LEVEL=", 6) == 0)
	{
		size_t labellen = strlen(label);
		labellen = MIN(size_t(8), labellen);

		if (strnicmp(hdata+6, label, labellen)==0 && 
			(hdata[6+labellen]==0xa || hdata[6+labellen]==0xd))
		{
			return true;
		}
	}
	return false;
}

//===========================================================================
//
// FindGLNodesInWAD
//
// Looks for GL nodes in the same WAD as the level itself
//
//===========================================================================

static int FindGLNodesInWAD(int labellump)
{
	int wadfile = Wads.GetLumpFile(labellump);
	FString glheader;

	glheader.Format("GL_%s", Wads.GetLumpFullName(labellump));
	if (glheader.Len()<=8)
	{
		int gllabel = Wads.CheckNumForName(glheader, ns_global, wadfile);
		if (gllabel >= 0) return gllabel;
	}
	else
	{
		// Before scanning the entire WAD directory let's check first whether
		// it is necessary.
		int gllabel = Wads.CheckNumForName("GL_LEVEL", ns_global, wadfile);

		if (gllabel >= 0)
		{
			int lastlump=0;
			int lump;
			while ((lump=Wads.FindLump("GL_LEVEL", &lastlump))>=0)
			{
				if (Wads.GetLumpFile(lump)==wadfile)
				{
					FMemLump mem = Wads.ReadLump(lump);
					if (MatchHeader(Wads.GetLumpFullName(labellump), (const char *)mem.GetMem())) return lump;
				}
			}
		}
	}
	return -1;
}

//===========================================================================
//
// FindGLNodesInFile
//
// Looks for GL nodes in the same WAD as the level itself
// Function returns the lump number within the file. Returns -1 if the input
// resource file is NULL.
//
//===========================================================================

static int FindGLNodesInFile(FResourceFile * f, const char * label)
{
	// No file open?  Probably shouldn't happen but assume no GL nodes
	if(!f)
		return -1;

	FString glheader;
	bool mustcheck=false;
	uint32_t numentries = f->LumpCount();

	glheader.Format("GL_%.8s", label);
	if (glheader.Len()>8)
	{
		glheader="GL_LEVEL";
		mustcheck=true;
	}

	if (numentries > 4)
	{
		for(uint32_t i=0;i<numentries-4;i++)
		{
			if (!strnicmp(f->GetLump(i)->Name, glheader, 8))
			{
				if (mustcheck)
				{
					char check[16]={0};
					FileReader *fr = f->GetLump(i)->GetReader();
					fr->Read(check, 16);
					if (MatchHeader(label, check)) return i;
				}
				else return i;
			}
		}
	}
	return -1;
}

//==========================================================================
//
// Checks for the presence of GL nodes in the loaded WADs or a .GWA file
// returns true if successful
//
//==========================================================================

bool P_LoadGLNodes(MapData * map)
{
	if (map->MapLumps[ML_GLZNODES].Reader && map->MapLumps[ML_GLZNODES].Reader->GetLength() != 0)
	{
		const int idcheck1a = MAKE_ID('Z','G','L','N');
		const int idcheck2a = MAKE_ID('Z','G','L','2');
		const int idcheck3a = MAKE_ID('Z','G','L','3');
		const int idcheck1b = MAKE_ID('X','G','L','N');
		const int idcheck2b = MAKE_ID('X','G','L','2');
		const int idcheck3b = MAKE_ID('X','G','L','3');
		int id;

		map->Seek(ML_GLZNODES);
		map->file->Read (&id, 4);
		if (id == idcheck1a || id == idcheck2a || id == idcheck3a ||
			id == idcheck1b || id == idcheck2b || id == idcheck3b)
		{
			try
			{
				subsectors = NULL;
				segs = NULL;
				nodes = NULL;
				P_LoadZNodes (*map->file, id);
				return true;
			}
			catch (CRecoverableError &)
			{
				if (subsectors != NULL)
				{
					delete[] subsectors;
					subsectors = NULL;
				}
				if (segs != NULL)
				{
					delete[] segs;
					segs = NULL;
				}
				if (nodes != NULL)
				{
					delete[] nodes;
					nodes = NULL;
				}
			}
		}
	}

	if (!CheckCachedNodes(map))
	{
		FileReader *gwalumps[4] = { NULL, NULL, NULL, NULL };
		char path[256];
		int li;
		int lumpfile = Wads.GetLumpFile(map->lumpnum);
		bool mapinwad = map->InWad;
		FResourceFile * f_gwa = map->resource;

		const char * name = Wads.GetWadFullName(lumpfile);

		if (mapinwad)
		{
			li = FindGLNodesInWAD(map->lumpnum);

			if (li>=0)
			{
				// GL nodes are loaded with a WAD
				for(int i=0;i<4;i++)
				{
					gwalumps[i]=Wads.ReopenLumpNum(li+i+1);
				}
				return DoLoadGLNodes(gwalumps);
			}
			else
			{
				strcpy(path, name);

				char * ext = strrchr(path, '.');
				if (ext)
				{
					strcpy(ext, ".gwa");
					// Todo: Compare file dates

					f_gwa = FResourceFile::OpenResourceFile(path, NULL, true);
					if (f_gwa==NULL) return false;

					strncpy(map->MapLumps[0].Name, Wads.GetLumpFullName(map->lumpnum), 8);
				}
			}
		}

		bool result = false;
		li = FindGLNodesInFile(f_gwa, map->MapLumps[0].Name);
		if (li!=-1)
		{
			static const char check[][9]={"GL_VERT","GL_SEGS","GL_SSECT","GL_NODES"};
			result=true;
			for(unsigned i=0; i<4;i++)
			{
				if (strnicmp(f_gwa->GetLump(li+i+1)->Name, check[i], 8))
				{
					result=false;
					break;
				}
				else
					gwalumps[i] = f_gwa->GetLump(li+i+1)->NewReader();
			}
			if (result) result = DoLoadGLNodes(gwalumps);
		}

		if (f_gwa != map->resource)
			delete f_gwa;
		for(unsigned int i = 0;i < 4;++i)
			delete gwalumps[i];
		return result;
	}
	else return true;
}

//==========================================================================
//
// Checks whether nodes are GL friendly or not
//
//==========================================================================

bool P_CheckNodes(MapData * map, bool rebuilt, int buildtime)
{
	bool ret = false;
	bool loaded = false;

	// If the map loading code has performed a node rebuild we don't need to check for it again.
	if (!rebuilt && !P_CheckForGLNodes())
	{
		ret = true;	// we are not using the level's original nodes if we get here.
		for (int i = 0; i < numsubsectors; i++)
		{
			gamesubsectors[i].sector = gamesubsectors[i].firstline->sidedef->sector;
		}

		nodes = NULL;
		numnodes = 0;
		subsectors = NULL;
		numsubsectors = 0;
		if (segs) delete [] segs;
		segs = NULL;
		numsegs = 0;

		// Try to load GL nodes (cached or GWA)
		loaded = P_LoadGLNodes(map);
		if (!loaded)
		{
			// none found - we have to build new ones!
			unsigned int startTime, endTime;

			startTime = I_FPSTime ();
			TArray<FNodeBuilder::FPolyStart> polyspots, anchors;
			P_GetPolySpots (map, polyspots, anchors);
			FNodeBuilder::FLevel leveldata =
			{
				&level.vertexes[0], (int)level.vertexes.Size(),
				&level.sides[0], (int)level.sides.Size(),
				&level.lines[0], (int)level.lines.Size(),
				0, 0, 0, 0
			};
			leveldata.FindMapBounds ();
			FNodeBuilder builder (leveldata, polyspots, anchors, true);
			
			builder.Extract (nodes, numnodes,
				segs, numsegs,
				subsectors, numsubsectors,
				level.vertexes);
			endTime = I_FPSTime ();
			DPrintf (DMSG_NOTIFY, "BSP generation took %.3f sec (%d segs)\n", (endTime - startTime) * 0.001, numsegs);
			buildtime = endTime - startTime;
		}
	}

	if (!loaded)
	{
#ifdef DEBUG
		// Building nodes in debug is much slower so let's cache them only if cachetime is 0
		buildtime = 0;
#endif
		if (level.maptype != MAPTYPE_BUILD && gl_cachenodes && buildtime/1000.f >= gl_cachetime)
		{
			DPrintf(DMSG_NOTIFY, "Caching nodes\n");
			CreateCachedNodes(map);
		}
		else
		{
			DPrintf(DMSG_NOTIFY, "Not caching nodes (time = %f)\n", buildtime/1000.f);
		}
	}

	if (!gamenodes)
	{
		gamenodes = nodes;
		numgamenodes = numnodes;
		gamesubsectors = subsectors;
		numgamesubsectors = numsubsectors;
	}
	return ret;
}

//==========================================================================
//
// Node caching
//
//==========================================================================

typedef TArray<uint8_t> MemFile;


static FString CreateCacheName(MapData *map, bool create)
{
	FString path = M_GetCachePath(create);
	FString lumpname = Wads.GetLumpFullPath(map->lumpnum);
	int separator = lumpname.IndexOf(':');
	path << '/' << lumpname.Left(separator);
	if (create) CreatePath(path);

	lumpname.ReplaceChars('/', '%');
	path << '/' << lumpname.Right(lumpname.Len() - separator - 1) << ".gzc";
	return path;
}

static void WriteByte(MemFile &f, uint8_t b)
{
	f.Push(b);
}

static void WriteWord(MemFile &f, uint16_t b)
{
	int v = f.Reserve(2);
	f[v] = (uint8_t)b;
	f[v+1] = (uint8_t)(b>>8);
}

static void WriteLong(MemFile &f, uint32_t b)
{
	int v = f.Reserve(4);
	f[v] = (uint8_t)b;
	f[v+1] = (uint8_t)(b>>8);
	f[v+2] = (uint8_t)(b>>16);
	f[v+3] = (uint8_t)(b>>24);
}

static void CreateCachedNodes(MapData *map)
{
	MemFile ZNodes;

	WriteLong(ZNodes, 0);
	WriteLong(ZNodes, level.vertexes.Size());
	for(auto &vert : level.vertexes)
	{
		WriteLong(ZNodes, vert.fixX());
		WriteLong(ZNodes, vert.fixY());
	}

	WriteLong(ZNodes, numsubsectors);
	for(int i=0;i<numsubsectors;i++)
	{
		WriteLong(ZNodes, subsectors[i].numlines);
	}

	WriteLong(ZNodes, numsegs);
	for(int i=0;i<numsegs;i++)
	{
		WriteLong(ZNodes, segs[i].v1->Index());
		WriteLong(ZNodes, segs[i].PartnerSeg == nullptr? 0xffffffffu : uint32_t(segs[i].PartnerSeg - segs));
		if (segs[i].linedef)
		{
			WriteLong(ZNodes, uint32_t(segs[i].linedef->Index()));
			WriteByte(ZNodes, segs[i].sidedef == segs[i].linedef->sidedef[0]? 0:1);
		}
		else
		{
			WriteLong(ZNodes, 0xffffffffu);
			WriteByte(ZNodes, 0);
		}
	}

	WriteLong(ZNodes, numnodes);
	for(int i=0;i<numnodes;i++)
	{
		WriteLong(ZNodes, nodes[i].x);
		WriteLong(ZNodes, nodes[i].y);
		WriteLong(ZNodes, nodes[i].dx);
		WriteLong(ZNodes, nodes[i].dy);
		for (int j = 0; j < 2; ++j)
		{
			for (int k = 0; k < 4; ++k)
			{
				WriteWord(ZNodes, (short)nodes[i].bbox[j][k]);
			}
		}

		for (int j = 0; j < 2; ++j)
		{
			uint32_t child;
			if ((size_t)nodes[i].children[j] & 1)
			{
				child = 0x80000000 | uint32_t((subsector_t *)((uint8_t *)nodes[i].children[j] - 1) - subsectors);
			}
			else
			{
				child = uint32_t((node_t *)nodes[i].children[j] - nodes);
			}
			WriteLong(ZNodes, child);
		}
	}

	uLongf outlen = ZNodes.Size();
	uint8_t *compressed;
	int offset = level.lines.Size() * 8 + 12 + 16;
	int r;
	do
	{
		compressed = new Bytef[outlen + offset];
		r = compress (compressed + offset, &outlen, &ZNodes[0], ZNodes.Size());
		if (r == Z_BUF_ERROR)
		{
			delete[] compressed;
			outlen += 1024;
		}
	} 
	while (r == Z_BUF_ERROR);

	memcpy(compressed, "CACH", 4);
	uint32_t len = LittleLong(level.lines.Size());
	memcpy(compressed+4, &len, 4);
	map->GetChecksum(compressed+8);
	for (unsigned i = 0; i < level.lines.Size(); i++)
	{
		uint32_t ndx[2] = { LittleLong(uint32_t(level.lines[i].v1->Index())), LittleLong(uint32_t(level.lines[i].v2->Index())) };
		memcpy(compressed + 8 + 16 + 8 * i, ndx, 8);
	}
	memcpy(compressed + offset - 4, "ZGL3", 4);

	FString path = CreateCacheName(map, true);
	FILE *f = fopen(path, "wb");

	if (f != NULL)
	{
		if (fwrite(compressed, outlen+offset, 1, f) != 1)
		{
			Printf("Error saving nodes to file %s\n", path.GetChars());
		}

		fclose(f);
	}
	else
	{
		Printf("Cannot open nodes file %s for writing\n", path.GetChars());
	}

	delete [] compressed;
}


static bool CheckCachedNodes(MapData *map)
{
	char magic[4] = {0,0,0,0};
	uint8_t md5[16];
	uint8_t md5map[16];
	uint32_t numlin;
	uint32_t *verts = NULL;

	FString path = CreateCacheName(map, false);
	FILE *f = fopen(path, "rb");
	if (f == NULL) return false;

	if (fread(magic, 1, 4, f) != 4) goto errorout;
	if (memcmp(magic, "CACH", 4))  goto errorout;

	if (fread(&numlin, 4, 1, f) != 1) goto errorout; 
	numlin = LittleLong(numlin);
	if (numlin != level.lines.Size()) goto errorout;

	if (fread(md5, 1, 16, f) != 16) goto errorout;
	map->GetChecksum(md5map);
	if (memcmp(md5, md5map, 16)) goto errorout;

	verts = new uint32_t[numlin * 8];
	if (fread(verts, 8, numlin, f) != numlin) goto errorout;

	if (fread(magic, 1, 4, f) != 4) goto errorout;
	if (memcmp(magic, "ZGL2", 4) && memcmp(magic, "ZGL3", 4))  goto errorout;


	try
	{
		long pos = ftell(f);
		FileReader fr(f);
		fr.Seek(pos, SEEK_SET);
		P_LoadZNodes (fr, MAKE_ID(magic[0],magic[1],magic[2],magic[3]));
	}
	catch (CRecoverableError &error)
	{
		Printf ("Error loading nodes: %s\n", error.GetMessage());

		if (subsectors != NULL)
		{
			delete[] subsectors;
			subsectors = NULL;
		}
		if (segs != NULL)
		{
			delete[] segs;
			segs = NULL;
		}
		if (nodes != NULL)
		{
			delete[] nodes;
			nodes = NULL;
		}
		goto errorout;
	}

	for(auto &line : level.lines)
	{
		int i = line.Index();
		line.v1 = &level.vertexes[LittleLong(verts[i*2])];
		line.v2 = &level.vertexes[LittleLong(verts[i*2+1])];
	}
	delete [] verts;

	fclose(f);
	return true;

errorout:
	if (verts != NULL)
	{
		delete[] verts;
	}
	fclose(f);
	return false;
}

CCMD(clearnodecache)
{
	TArray<FFileList> list;
	FString path = M_GetCachePath(false);
	path += "/";

	try
	{
		ScanDirectory(list, path);
	}
	catch (CRecoverableError &err)
	{
		Printf("%s\n", err.GetMessage());
		return;
	}

	// Scan list backwards so that when we reach a directory
	// all files within are already deleted.
	for(int i = list.Size()-1; i >= 0; i--)
	{
		if (list[i].isDirectory)
		{
			rmdir(list[i].Filename);
		}
		else
		{
			remove(list[i].Filename);
		}
	}

		
}

//==========================================================================
//
// Keep both the original nodes from the WAD and the GL nodes created here.
// The original set is only being used to get the sector for in-game 
// positioning of actors but not for rendering.
//
// This is necessary because ZDBSP is much more sensitive
// to sloppy mapping practices that produce overlapping sectors.
// The crane in P:AR E1M3 is a good example that would be broken if
// this wasn't done.
//
//==========================================================================


//==========================================================================
//
// P_PointInSubsector
//
//==========================================================================

subsector_t *P_PointInSubsector (double x, double y)
{
	node_t *node;
	int side;

	// single subsector is a special case
	if (numgamenodes == 0)
		return gamesubsectors;
				
	node = gamenodes + numgamenodes - 1;

	fixed_t xx = FLOAT2FIXED(x);
	fixed_t yy = FLOAT2FIXED(y);
	do
	{
		side = R_PointOnSide (xx, yy, node);
		node = (node_t *)node->children[side];
	}
	while (!((size_t)node & 1));
		
	return (subsector_t *)((uint8_t *)node - 1);
}

//==========================================================================
//
// PointOnLine
//
// Same as the one im the node builder, but not part of a specific class
//
//==========================================================================

static bool PointOnLine (int x, int y, int x1, int y1, int dx, int dy)
{
	const double SIDE_EPSILON = 6.5536;

	// For most cases, a simple dot product is enough.
	double d_dx = double(dx);
	double d_dy = double(dy);
	double d_x = double(x);
	double d_y = double(y);
	double d_x1 = double(x1);
	double d_y1 = double(y1);

	double s_num = (d_y1-d_y)*d_dx - (d_x1-d_x)*d_dy;

	if (fabs(s_num) < 17179869184.0)	// 4<<32
	{
		// Either the point is very near the line, or the segment defining
		// the line is very short: Do a more expensive test to determine
		// just how far from the line the point is.
		double l = g_sqrt(d_dx*d_dx+d_dy*d_dy);
		double dist = fabs(s_num)/l;
		if (dist < SIDE_EPSILON)
		{
			return true;
		}
	}
	return false;
}


//==========================================================================
//
// SetRenderSector
//
// Sets the render sector for each GL subsector so that the proper flat 
// information can be retrieved
//
//==========================================================================

void P_SetRenderSector()
{
	int 				i;
	uint32_t 				j;
	TArray<subsector_t *> undetermined;
	subsector_t *		ss;

#if 0	// doesn't work as expected :(

	// hide all sectors on textured automap that only have hidden lines.
	bool *hidesec = new bool[numsectors];
	for(i = 0; i < numsectors; i++)
	{
		hidesec[i] = true;
	}
	for(i = 0; i < numlines; i++)
	{
		if (!(lines[i].flags & ML_DONTDRAW))
		{
			hidesec[lines[i].frontsector - sectors] = false;
			if (lines[i].backsector != NULL)
			{
				hidesec[lines[i].backsector - sectors] = false;
			}
		}
	}
	for(i = 0; i < numsectors; i++)
	{
		if (hidesec[i]) sectors[i].MoreFlags |= SECF_HIDDEN;
	}
	delete [] hidesec;
#endif

	// Check for incorrect partner seg info so that the following code does not crash.

	for (i = 0; i < numsegs; i++)
	{
		auto p = segs[i].PartnerSeg;
		if (p != nullptr)
		{
			int partner = (int)(p - segs);

			if (partner < 0 || partner >= numsegs || &segs[partner] != p)
			{
				segs[i].PartnerSeg = nullptr;
			}

			// glbsp creates such incorrect references for Strife.
			if (segs[i].linedef && segs[i].PartnerSeg != nullptr && !segs[i].PartnerSeg->linedef)
			{
				segs[i].PartnerSeg = segs[i].PartnerSeg->PartnerSeg = nullptr;
			}
		}
	}
	for (i = 0; i < numsegs; i++)
	{
		if (segs[i].PartnerSeg != nullptr && segs[i].PartnerSeg->PartnerSeg != &segs[i])
		{
			segs[i].PartnerSeg = nullptr;
		}
	}

	// look up sector number for each subsector
	for (i = 0; i < numsubsectors; i++)
	{
		// For rendering pick the sector from the first seg that is a sector boundary
		// this takes care of self-referencing sectors
		ss = &subsectors[i];
		seg_t *seg = ss->firstline;

		// Check for one-dimensional subsectors. These should be ignored when
		// being processed for automap drawing etc.
		ss->flags |= SSECF_DEGENERATE;
		for(j=2; j<ss->numlines; j++)
		{
			if (!PointOnLine(seg[j].v1->fixX(), seg[j].v1->fixY(), seg->v1->fixX(), seg->v1->fixY(), seg->v2->fixX() -seg->v1->fixX(), seg->v2->fixY() -seg->v1->fixY()))
			{
				// Not on the same line
				ss->flags &= ~SSECF_DEGENERATE;
				break;
			}
		}

		seg = ss->firstline;
		for(j=0; j<ss->numlines; j++)
		{
			if(seg->sidedef && (seg->PartnerSeg == nullptr || (seg->PartnerSeg->sidedef != nullptr && seg->sidedef->sector!=seg->PartnerSeg->sidedef->sector)))
			{
				ss->render_sector = seg->sidedef->sector;
				break;
			}
			seg++;
		}
		if(ss->render_sector == NULL) 
		{
			undetermined.Push(ss);
		}
	}

	// assign a vaild render sector to all subsectors which haven't been processed yet.
	while (undetermined.Size())
	{
		bool deleted=false;
		for(i=undetermined.Size()-1;i>=0;i--)
		{
			ss=undetermined[i];
			seg_t * seg = ss->firstline;
			
			for(j=0; j<ss->numlines; j++)
			{
				if (seg->PartnerSeg != nullptr && seg->PartnerSeg->Subsector)
				{
					sector_t * backsec = seg->PartnerSeg->Subsector->render_sector;
					if (backsec)
					{
						ss->render_sector = backsec;
						undetermined.Delete(i);
						deleted = 1;
						break;
					}
				}
				seg++;
			}
		}
		// We still got some left but the loop above was unable to assign them.
		// This only happens when a subsector is off the map.
		// Don't bother and just assign the real sector for rendering
		if (!deleted && undetermined.Size()) 
		{
			for(i=undetermined.Size()-1;i>=0;i--)
			{
				ss=undetermined[i];
				ss->render_sector=ss->sector;
			}
			break;
		}
	}

#if 0	// may be useful later so let's keep it here for now
	// now group the subsectors by sector
	subsector_t ** subsectorbuffer = new subsector_t * [numsubsectors];

	for(i=0, ss=subsectors; i<numsubsectors; i++, ss++)
	{
		ss->render_sector->subsectorcount++;
	}

	for (i=0; i<numsectors; i++) 
	{
		sectors[i].subsectors = subsectorbuffer;
		subsectorbuffer += sectors[i].subsectorcount;
		sectors[i].subsectorcount = 0;
	}
	
	for(i=0, ss = subsectors; i<numsubsectors; i++, ss++)
	{
		ss->render_sector->subsectors[ss->render_sector->subsectorcount++]=ss;
	}
#endif

}
