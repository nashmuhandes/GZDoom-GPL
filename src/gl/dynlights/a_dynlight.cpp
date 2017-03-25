// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2004-2016 Christoph Oelckers
// All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/
//
//--------------------------------------------------------------------------
//
/*
** a_dynlight.cpp
** Implements actors representing dynamic lights (hardware independent)
**
**
** all functions marked with [TS] are licensed under
**---------------------------------------------------------------------------
** Copyright 2003 Timothy Stump
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

#include "gl/system/gl_system.h"

#include "templates.h"
#include "m_random.h"
#include "p_local.h"
#include "c_dispatch.h"
#include "g_level.h"
#include "scripting/thingdef.h"
#include "i_system.h"
#include "templates.h"
#include "doomdata.h"
#include "r_utility.h"
#include "p_local.h"
#include "portal.h"
#include "doomstat.h"
#include "serializer.h"
#include "g_levellocals.h"


#include "gl/renderer/gl_renderer.h"
#include "gl/data/gl_data.h"
#include "gl/dynlights/gl_dynlight.h"
#include "gl/utility/gl_convert.h"
#include "gl/utility/gl_templates.h"
#include "gl/system//gl_interface.h"



//==========================================================================
//
//==========================================================================
DEFINE_CLASS_PROPERTY(type, S, DynamicLight)
{
	PROP_STRING_PARM(str, 0);
	static const char * ltype_names[]={
		"Point","Pulse","Flicker","Sector","RandomFlicker", "ColorPulse", "ColorFlicker", "RandomColorFlicker", NULL};

	static const int ltype_values[]={
		PointLight, PulseLight, FlickerLight, SectorLight, RandomFlickerLight, ColorPulseLight, ColorFlickerLight, RandomColorFlickerLight };

	int style = MatchString(str, ltype_names);
	if (style < 0) I_Error("Unknown light type '%s'", str);
	defaults->lighttype = ltype_values[style];
}

//==========================================================================
//
// Actor classes
//
// For flexibility all functionality has been packed into a single class
// which is controlled by flags
//
//==========================================================================
IMPLEMENT_CLASS(ADynamicLight, false, false)

static FRandom randLight;

//==========================================================================
//
// Base class
//
//==========================================================================

//==========================================================================
//
//
//
//==========================================================================
void ADynamicLight::Serialize(FSerializer &arc)
{
	Super::Serialize (arc);
	auto def = static_cast<ADynamicLight*>(GetDefault());
	arc("lightflags", lightflags, def->lightflags)
		("lighttype", lighttype, def->lighttype)
		("tickcount", m_tickCount, def->m_tickCount)
		("currentradius", m_currentRadius, def->m_currentRadius);

	if (lighttype == PulseLight)
		arc("lastupdate", m_lastUpdate, def->m_lastUpdate)
			("cycler", m_cycler, def->m_cycler);
}


void ADynamicLight::PostSerialize()
{
	Super::PostSerialize();
	// The default constructor which is used for creating objects before deserialization will not set this variable.
	// It needs to be true for all placed lights.
	visibletoplayer = true;
	LinkLight();
}

//==========================================================================
//
// [TS]
//
//==========================================================================
void ADynamicLight::BeginPlay()
{
	//Super::BeginPlay();
	ChangeStatNum(STAT_DLIGHT);

	specialf1 = DAngle(double(SpawnAngle)).Normalized360().Degrees;
	visibletoplayer = true;

	if (gl.legacyMode && (flags4 & MF4_ATTENUATE))
	{
		args[LIGHT_INTENSITY] = args[LIGHT_INTENSITY] * 2 / 3;
		args[LIGHT_SECONDARY_INTENSITY] = args[LIGHT_SECONDARY_INTENSITY] * 2 / 3;
	}
}

//==========================================================================
//
// [TS]
//
//==========================================================================
void ADynamicLight::PostBeginPlay()
{
	Super::PostBeginPlay();
	
	if (!(SpawnFlags & MTF_DORMANT))
	{
		Activate (NULL);
	}

	subsector = R_PointInSubsector(Pos());
}


//==========================================================================
//
// [TS]
//
//==========================================================================
void ADynamicLight::Activate(AActor *activator)
{
	//Super::Activate(activator);
	flags2&=~MF2_DORMANT;	

	m_currentRadius = float(args[LIGHT_INTENSITY]);
	m_tickCount = 0;

	if (lighttype == PulseLight)
	{
		float pulseTime = specialf1 / TICRATE;
		
		m_lastUpdate = level.maptime;
		if (!swapped) m_cycler.SetParams(float(args[LIGHT_SECONDARY_INTENSITY]), float(args[LIGHT_INTENSITY]), pulseTime);
		else m_cycler.SetParams(float(args[LIGHT_INTENSITY]), float(args[LIGHT_SECONDARY_INTENSITY]), pulseTime);
		m_cycler.ShouldCycle(true);
		m_cycler.SetCycleType(CYCLE_Sin);
		m_currentRadius = m_cycler.GetVal();
	}
	if (m_currentRadius <= 0) m_currentRadius = 1;
}


//==========================================================================
//
// [TS]
//
//==========================================================================
void ADynamicLight::Deactivate(AActor *activator)
{
	//Super::Deactivate(activator);
	flags2|=MF2_DORMANT;	
}


//==========================================================================
//
// [TS]
//
//==========================================================================
void ADynamicLight::Tick()
{
	if (IsOwned())
	{
		if (!target || !target->state)
		{
			this->Destroy();
			return;
		}
		if (target->flags & MF_UNMORPHED) return;
		visibletoplayer = target->IsVisibleToPlayer();	// cache this value for the renderer to speed up calculations.
	}

	// Don't bother if the light won't be shown
	if (!IsActive()) return;

	// I am doing this with a type field so that I can dynamically alter the type of light
	// without having to create or maintain multiple objects.
	switch(lighttype)
	{
	case PulseLight:
	{
		float diff = (level.maptime - m_lastUpdate) / (float)TICRATE;
		
		m_lastUpdate = level.maptime;
		m_cycler.Update(diff);
		m_currentRadius = m_cycler.GetVal();
		break;
	}

	case FlickerLight:
	{
		int rnd = randLight();
		float pct = specialf1 / 360.f;
		
		m_currentRadius = float(args[LIGHT_INTENSITY + (rnd >= pct * 255)]);
		break;
	}

	case RandomFlickerLight:
	{
		int flickerRange = args[LIGHT_SECONDARY_INTENSITY] - args[LIGHT_INTENSITY];
		float amt = randLight() / 255.f;
		
		if (m_tickCount > specialf1)
		{
			m_tickCount = 0;
		}
		if (m_tickCount++ == 0 || m_currentRadius > args[LIGHT_SECONDARY_INTENSITY])
		{
			m_currentRadius = float(args[LIGHT_INTENSITY] + (amt * flickerRange));
		}
		break;
	}

#if 0
	// These need some more work elsewhere
	case ColorFlickerLight:
	{
		int rnd = randLight();
		float pct = specialf1/360.f;
		
		m_currentRadius = m_Radius[rnd >= pct * 255];
		break;
	}

	case RandomColorFlickerLight:
	{
		int flickerRange = args[LIGHT_SECONDARY_INTENSITY] - args[LIGHT_INTENSITY];
		float amt = randLight() / 255.f;
		
		m_tickCount++;
		
		if (m_tickCount > specialf1)
		{
			m_currentRadius = args[LIGHT_INTENSITY] + (amt * flickerRange);
			m_tickCount = 0;
		}
		break;
	}
#endif

	case SectorLight:
	{
		float intensity;
		float scale = args[LIGHT_SCALE] / 8.f;
		
		if (scale == 0.f) scale = 1.f;
		
		intensity = Sector->lightlevel * scale;
		intensity = clamp<float>(intensity, 0.f, 255.f);
		
		m_currentRadius = intensity;
		break;
	}

	case PointLight:
		m_currentRadius = float(args[LIGHT_INTENSITY]);
		break;
	}
	if (m_currentRadius <= 0) m_currentRadius = 1;
	UpdateLocation();
}




//==========================================================================
//
//
//
//==========================================================================
void ADynamicLight::UpdateLocation()
{
	double oldx= X();
	double oldy= Y();
	double oldradius= radius;
	float intensity;

	if (IsActive())
	{
		if (target)
		{
			DAngle angle = target->Angles.Yaw;
			double s = angle.Sin();
			double c = angle.Cos();

			DVector3 pos = target->Vec3Offset(m_off.X * c + m_off.Y * s, m_off.X * s - m_off.Y * c, m_off.Z + target->GetBobOffset());
			SetXYZ(pos); // attached lights do not need to go into the regular blockmap
			Prev = target->Pos();
			subsector = R_PointInSubsector(Prev);
			Sector = subsector->sector;

			// Some z-coordinate fudging to prevent the light from getting too close to the floor or ceiling planes. With proper attenuation this would render them invisible.
			// A distance of 5 is needed so that the light's effect doesn't become too small.
			if (Z() < target->floorz + 5.) 	SetZ(target->floorz + 5.);
			else if (Z() > target->ceilingz - 5.) 	SetZ(target->ceilingz - 5.);
		}
		else
		{
			if (Z() < floorz + 5.) 	SetZ(floorz + 5.);
			else if (Z() > ceilingz - 5.) 	SetZ(ceilingz - 5.);
		}


		// The radius being used here is always the maximum possible with the
		// current settings. This avoids constant relinking of flickering lights

		if (lighttype == FlickerLight || lighttype == RandomFlickerLight || lighttype == PulseLight)
		{
			intensity = float(MAX(args[LIGHT_INTENSITY], args[LIGHT_SECONDARY_INTENSITY]));
		}
		else
		{
			intensity = m_currentRadius;
		}
		radius = intensity * 2.0f;
		if (radius < m_currentRadius * 2) radius = m_currentRadius * 2;

		if (X() != oldx || Y() != oldy || radius != oldradius)
		{
			//Update the light lists
			LinkLight();
		}
	}
}


//==========================================================================
//
//
//
//==========================================================================

void ADynamicLight::SetOrigin(double x, double y, double z, bool moving)
{
	Super::SetOrigin(x, y, z, moving);
	LinkLight();
}

//==========================================================================
//
//
//
//==========================================================================

void ADynamicLight::SetOffset(const DVector3 &pos)
{
	m_off = pos;
	UpdateLocation();
}


//==========================================================================
//
// The target pointer in dynamic lights should never be substituted unless 
// notOld is NULL (which indicates that the object was destroyed by force.)
//
//==========================================================================
size_t ADynamicLight::PointerSubstitution (DObject *old, DObject *notOld)
{
	AActor *saved_target = target;
	size_t ret = Super::PointerSubstitution(old, notOld);
	if (notOld != NULL) target = saved_target;
	return ret;
}

//=============================================================================
//
// These have been copied from the secnode code and modified for the light links
//
// P_AddSecnode() searches the current list to see if this sector is
// already there. If not, it adds a sector node at the head of the list of
// sectors this object appears in. This is called when creating a list of
// nodes that will get linked in later. Returns a pointer to the new node.
//
//=============================================================================

FLightNode * AddLightNode(FLightNode ** thread, void * linkto, ADynamicLight * light, FLightNode *& nextnode)
{
	FLightNode * node;

	node = nextnode;
	while (node)
    {
		if (node->targ==linkto)   // Already have a node for this sector?
		{
			node->lightsource = light; // Yes. Setting m_thing says 'keep it'.
			return(nextnode);
		}
		node = node->nextTarget;
    }

	// Couldn't find an existing node for this sector. Add one at the head
	// of the list.
	
	node = new FLightNode;
	
	node->targ = linkto;
	node->lightsource = light; 

	node->prevTarget = &nextnode; 
	node->nextTarget = nextnode;

	if (nextnode) nextnode->prevTarget = &node->nextTarget;
	
	// Add new node at head of sector thread starting at s->touching_thinglist
	
	node->prevLight = thread;  	
	node->nextLight = *thread; 
	if (node->nextLight) node->nextLight->prevLight=&node->nextLight;
	*thread = node;
	return(node);
}


//=============================================================================
//
// P_DelSecnode() deletes a sector node from the list of
// sectors this object appears in. Returns a pointer to the next node
// on the linked list, or NULL.
//
//=============================================================================

static FLightNode * DeleteLightNode(FLightNode * node)
{
	FLightNode * tn;  // next node on thing thread
	
	if (node)
    {
		
		*node->prevTarget = node->nextTarget;
		if (node->nextTarget) node->nextTarget->prevTarget=node->prevTarget;

		*node->prevLight = node->nextLight;
		if (node->nextLight) node->nextLight->prevLight=node->prevLight;
		
		// Return this node to the freelist
		tn=node->nextTarget;
		delete node;
		return(tn);
    }
	return(NULL);
}                             // phares 3/13/98



//==========================================================================
//
// Gets the light's distance to a line
//
//==========================================================================

double ADynamicLight::DistToSeg(const DVector3 &pos, seg_t *seg)
{
	double u, px, py;

	double seg_dx = seg->v2->fX() - seg->v1->fX();
	double seg_dy = seg->v2->fY() - seg->v1->fY();
	double seg_length_sq = seg_dx * seg_dx + seg_dy * seg_dy;

	u = (((pos.X - seg->v1->fX()) * seg_dx) + (pos.Y - seg->v1->fY()) * seg_dy) / seg_length_sq;
	if (u < 0.) u = 0.; // clamp the test point to the line segment
	else if (u > 1.) u = 1.;

	px = seg->v1->fX() + (u * seg_dx);
	py = seg->v1->fY() + (u * seg_dy);

	px -= pos.X;
	py -= pos.Y;

	return (px*px) + (py*py);
}


//==========================================================================
//
// Collect all touched sidedefs and subsectors
// to sidedefs and sector parts.
//
//==========================================================================
struct LightLinkEntry
{
	subsector_t *sub;
	DVector3 pos;
};
static TArray<LightLinkEntry> collected_ss;

void ADynamicLight::CollectWithinRadius(const DVector3 &opos, subsector_t *subSec, float radius)
{
	if (!subSec) return;
	collected_ss.Clear();
	collected_ss.Push({ subSec, opos });
	subSec->validcount = ::validcount;

	for (unsigned i = 0; i < collected_ss.Size(); i++)
	{
		subSec = collected_ss[i].sub;

		touching_subsectors = AddLightNode(&subSec->lighthead, subSec, this, touching_subsectors);
		if (subSec->sector->validcount != ::validcount)
		{
			touching_sector = AddLightNode(&subSec->render_sector->lighthead, subSec->sector, this, touching_sector);
			subSec->sector->validcount = ::validcount;
		}

		for (unsigned int j = 0; j < subSec->numlines; ++j)
		{
			auto &pos = collected_ss[i].pos;
			seg_t *seg = subSec->firstline + j;

			// check distance from x/y to seg and if within radius add this seg and, if present the opposing subsector (lather/rinse/repeat)
			// If out of range we do not need to bother with this seg.
			if (DistToSeg(pos, seg) <= radius)
			{
				if (seg->sidedef && seg->linedef && seg->linedef->validcount != ::validcount)
				{
					// light is in front of the seg
					if ((pos.Y - seg->v1->fY()) * (seg->v2->fX() - seg->v1->fX()) + (seg->v1->fX() - pos.X) * (seg->v2->fY() - seg->v1->fY()) <= 0)
					{
						seg->linedef->validcount = validcount;
						touching_sides = AddLightNode(&seg->sidedef->lighthead, seg->sidedef, this, touching_sides);
					}
				}
				if (seg->linedef)
				{
					FLinePortal *port = seg->linedef->getPortal();
					if (port && port->mType == PORTT_LINKED)
					{
						line_t *other = port->mDestination;
						if (other->validcount != ::validcount)
						{
							subsector_t *othersub = R_PointInSubsector(other->v1->fPos() + other->Delta() / 2);
							if (othersub->validcount != ::validcount)
							{
								othersub->validcount = ::validcount;
								collected_ss.Push({ othersub, PosRelative(other) });
							}
						}
					}
				}

				seg_t *partner = seg->PartnerSeg;
				if (partner)
				{
					subsector_t *sub = partner->Subsector;
					if (sub != NULL && sub->validcount != ::validcount)
					{
						sub->validcount = ::validcount;
						collected_ss.Push({ sub, pos });
					}
				}
			}
		}
		sector_t *sec = subSec->sector;
		if (!sec->PortalBlocksSight(sector_t::ceiling))
		{
			line_t *other = subSec->firstline->linedef;
			if (sec->GetPortalPlaneZ(sector_t::ceiling) < Z() + radius)
			{
				DVector2 refpos = other->v1->fPos() + other->Delta() / 2 + sec->GetPortalDisplacement(sector_t::ceiling);
				subsector_t *othersub = R_PointInSubsector(refpos);
				if (othersub->validcount != ::validcount)
				{
					othersub->validcount = ::validcount;
					collected_ss.Push({ othersub, PosRelative(othersub->sector) });
				}
			}
		}
		if (!sec->PortalBlocksSight(sector_t::floor))
		{
			line_t *other = subSec->firstline->linedef;
			if (sec->GetPortalPlaneZ(sector_t::floor) > Z() - radius)
			{
				DVector2 refpos = other->v1->fPos() + other->Delta() / 2 + sec->GetPortalDisplacement(sector_t::floor);
				subsector_t *othersub = R_PointInSubsector(refpos);
				if (othersub->validcount != ::validcount)
				{
					othersub->validcount = ::validcount;
					collected_ss.Push({ othersub, PosRelative(othersub->sector) });
				}
			}
		}
	}
}

//==========================================================================
//
// Link the light into the world
//
//==========================================================================

void ADynamicLight::LinkLight()
{
	// mark the old light nodes
	FLightNode * node;
	
	node = touching_sides;
	while (node)
    {
		node->lightsource = NULL;
		node = node->nextTarget;
    }
	node = touching_subsectors;
	while (node)
    {
		node->lightsource = NULL;
		node = node->nextTarget;
    }
	node = touching_sector;
	while (node)
	{
		node->lightsource = NULL;
		node = node->nextTarget;
	}

	if (radius>0)
	{
		// passing in radius*radius allows us to do a distance check without any calls to sqrt
		subsector_t * subSec = R_PointInSubsector(Pos());
		::validcount++;
		CollectWithinRadius(Pos(), subSec, radius*radius);

	}
		
	// Now delete any nodes that won't be used. These are the ones where
	// m_thing is still NULL.
	
	node = touching_sides;
	while (node)
	{
		if (node->lightsource == NULL)
		{
			node = DeleteLightNode(node);
		}
		else
			node = node->nextTarget;
	}

	node = touching_subsectors;
	while (node)
	{
		if (node->lightsource == NULL)
		{
			node = DeleteLightNode(node);
		}
		else
			node = node->nextTarget;
	}

	node = touching_sector;
	while (node)
	{
		if (node->lightsource == NULL)
		{
			node = DeleteLightNode(node);
		}
		else
			node = node->nextTarget;
	}
}


//==========================================================================
//
// Deletes the link lists
//
//==========================================================================
void ADynamicLight::UnlinkLight ()
{
	if (owned && target != NULL)
	{
		// Delete reference in owning actor
		for(int c=target->dynamiclights.Size()-1; c>=0; c--)
		{
			if (target->dynamiclights[c] == this)
			{
				target->dynamiclights.Delete(c);
				break;
			}
		}
	}
	while (touching_sides) touching_sides = DeleteLightNode(touching_sides);
	while (touching_subsectors) touching_subsectors = DeleteLightNode(touching_subsectors);
	while (touching_sector) touching_sector = DeleteLightNode(touching_sector);
}

void ADynamicLight::OnDestroy()
{
	UnlinkLight();
	Super::OnDestroy();
}


//==========================================================================
//
// Needed for garbage collection
//
//==========================================================================

size_t AActor::PropagateMark()
{
	for (unsigned i=0; i<dynamiclights.Size(); i++)
	{
		GC::Mark(dynamiclights[i]);
	}
	return Super::PropagateMark();
}



CCMD(listlights)
{
	int walls, sectors, subsecs;
	int allwalls=0, allsectors=0, allsubsecs = 0;
	int i=0;
	ADynamicLight * dl;
	TThinkerIterator<ADynamicLight> it;

	while ((dl=it.Next()))
	{
		walls=0;
		sectors=0;
		subsecs = 0;
		Printf("%s at (%f, %f, %f), color = 0x%02x%02x%02x, radius = %f ",
			dl->target? dl->target->GetClass()->TypeName.GetChars() : dl->GetClass()->TypeName.GetChars(),
			dl->X(), dl->Y(), dl->Z(), dl->args[LIGHT_RED], 
			dl->args[LIGHT_GREEN], dl->args[LIGHT_BLUE], dl->radius);
		i++;

		if (dl->target)
		{
			FTextureID spr = gl_GetSpriteFrame(dl->target->sprite, dl->target->frame, 0, 0, NULL);
			Printf(", frame = %s ", TexMan[spr]->Name.GetChars());
		}


		FLightNode * node;

		node=dl->touching_sides;

		while (node)
		{
			walls++;
			allwalls++;
			node = node->nextTarget;
		}

		node=dl->touching_subsectors;

		while (node)
		{
			allsubsecs++;
			subsecs++;
			node = node->nextTarget;
		}

		node = dl->touching_sector;

		while (node)
		{
			allsectors++;
			sectors++;
			node = node->nextTarget;
		}
		Printf("- %d walls, %d subsectors, %d sectors\n", walls, subsecs, sectors);

	}
	Printf("%i dynamic lights, %d walls, %d subsectors, %d sectors\n\n\n", i, allwalls, allsubsecs, allsectors);
}

CCMD(listsublights)
{
	for(int i=0;i<numsubsectors;i++)
	{
		subsector_t *sub = &subsectors[i];
		int lights = 0;

		FLightNode * node = sub->lighthead;
		while (node != NULL)
		{
			lights++;
			node = node->nextLight;
		}

		Printf(PRINT_LOG, "Subsector %d - %d lights\n", i, lights);
	}
}


