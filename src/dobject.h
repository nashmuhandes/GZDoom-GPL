/*
** dobject.h
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

#ifndef __DOBJECT_H__
#define __DOBJECT_H__

#include <stdlib.h>
#include "doomtype.h"
#include "i_system.h"

class PClass;
class PType;
class FSerializer;
class FSoundID;

class   DObject;
/*
class           DArgs;
class           DCanvas;
class           DConsoleCommand;
class                   DConsoleAlias;
class           DSeqNode;
class                   DSeqActorNode;
class                   DSeqPolyNode;
class                   DSeqSectorNode;
class           DThinker;
class                   AActor;
class                   DPolyAction;
class                           DMovePoly;
class                                   DPolyDoor;
class                           DRotatePoly;
class                   DPusher;
class                   DScroller;
class                   DSectorEffect;
class                           DLighting;
class                                   DFireFlicker;
class                                   DFlicker;
class                                   DGlow;
class                                   DGlow2;
class                                   DLightFlash;
class                                   DPhased;
class                                   DStrobe;
class                           DMover;
class                                   DElevator;
class                                   DMovingCeiling;
class                                           DCeiling;
class                                           DDoor;
class                                   DMovingFloor;
class                                           DFloor;
class                                           DFloorWaggle;
class                                           DPlat;
class                                   DPillar;
*/

class PClassActor;

#define RUNTIME_CLASS_CASTLESS(cls)	(cls::RegistrationInfo.MyClass)	// Passed a native class name, returns a PClass representing that class
#define RUNTIME_CLASS(cls)			((cls::MetaClass *)RUNTIME_CLASS_CASTLESS(cls))	// Like above, but returns the true type of the meta object
#define RUNTIME_TEMPLATE_CLASS(cls)	((typename cls::MetaClass *)RUNTIME_CLASS_CASTLESS(cls))	// RUNTIME_CLASS, but works with templated parameters on GCC
#define NATIVE_TYPE(object)			(object->StaticType())			// Passed an object, returns the type of the C++ class representing the object

// Enumerations for the meta classes created by ClassReg::RegisterClass()
enum
{
	CLASSREG_PClass,
	CLASSREG_PClassActor,
};

struct ClassReg
{
	PClass *MyClass;
	const char *Name;
	ClassReg *ParentType;
	ClassReg *_VMExport;
	const size_t *Pointers;
	void (*ConstructNative)(void *);
	void(*InitNatives)();
	unsigned int SizeOf:28;
	unsigned int MetaClassNum:4;

	PClass *RegisterClass();
	void SetupClass(PClass *cls);
};

enum EInPlace { EC_InPlace };

#define DECLARE_ABSTRACT_CLASS(cls,parent) \
public: \
	virtual PClass *StaticType() const; \
	static ClassReg RegistrationInfo, * const RegistrationInfoPtr; \
	typedef parent Super; \
private: \
	typedef cls ThisClass;

#define DECLARE_ABSTRACT_CLASS_WITH_META(cls,parent,meta) \
	DECLARE_ABSTRACT_CLASS(cls,parent) \
public: \
	typedef meta MetaClass; \
	MetaClass *GetClass() const { return static_cast<MetaClass *>(DObject::GetClass()); } \
protected: \
	enum { MetaClassNum = CLASSREG_##meta }; private: \

#define DECLARE_CLASS(cls,parent) \
	DECLARE_ABSTRACT_CLASS(cls,parent) \
		private: static void InPlaceConstructor (void *mem);

#define DECLARE_CLASS_WITH_META(cls,parent,meta) \
	DECLARE_ABSTRACT_CLASS_WITH_META(cls,parent,meta) \
		private: static void InPlaceConstructor (void *mem);

#define HAS_OBJECT_POINTERS \
	static const size_t PointerOffsets[];

#if defined(_MSC_VER)
#	pragma section(".creg$u",read)
#	define _DECLARE_TI(cls) __declspec(allocate(".creg$u")) ClassReg * const cls::RegistrationInfoPtr = &cls::RegistrationInfo;
#else
#	define _DECLARE_TI(cls) ClassReg * const cls::RegistrationInfoPtr __attribute__((section(SECTION_CREG))) = &cls::RegistrationInfo;
#endif

#define _IMP_PCLASS(cls, ptrs, create) \
	ClassReg cls::RegistrationInfo = {\
		nullptr, \
		#cls, \
		&cls::Super::RegistrationInfo, \
		nullptr, \
		ptrs, \
		create, \
		nullptr, \
		sizeof(cls), \
		cls::MetaClassNum }; \
	_DECLARE_TI(cls) \
	PClass *cls::StaticType() const { return RegistrationInfo.MyClass; }

#define IMPLEMENT_CLASS(cls, isabstract, ptrs) \
	_X_CONSTRUCTOR_##isabstract(cls) \
	_IMP_PCLASS(cls, _X_POINTERS_##ptrs(cls), _X_ABSTRACT_##isabstract(cls))

// Taking the address of a field in an object at address > 0 instead of
// address 0 keeps GCC from complaining about possible misuse of offsetof.
// Using 8 to avoid unaligned pointer use.
#define IMPLEMENT_POINTERS_START(cls)	const size_t cls::PointerOffsets[] = {
#define IMPLEMENT_POINTER(field)		((size_t)&((ThisClass*)8)->field) - 8,
#define IMPLEMENT_POINTERS_END			~(size_t)0 };

// Possible arguments for the IMPLEMENT_CLASS macro
#define _X_POINTERS_true(cls)		cls::PointerOffsets
#define _X_POINTERS_false(cls)		nullptr
#define _X_FIELDS_true(cls)			nullptr
#define _X_FIELDS_false(cls)		nullptr
#define _X_CONSTRUCTOR_true(cls)
#define _X_CONSTRUCTOR_false(cls)	void cls::InPlaceConstructor(void *mem) { new((EInPlace *)mem) cls; }
#define _X_ABSTRACT_true(cls)		nullptr
#define _X_ABSTRACT_false(cls)		cls::InPlaceConstructor
#define _X_VMEXPORT_true(cls)		nullptr
#define _X_VMEXPORT_false(cls)		nullptr

enum EObjectFlags
{
	// GC flags
	OF_White0			= 1 << 0,		// Object is white (type 0)
	OF_White1			= 1 << 1,		// Object is white (type 1)
	OF_Black			= 1 << 2,		// Object is black
	OF_Fixed			= 1 << 3,		// Object is fixed (should not be collected)
	OF_Rooted			= 1 << 4,		// Object is soft-rooted
	OF_EuthanizeMe		= 1 << 5,		// Object wants to die
	OF_Cleanup			= 1 << 6,		// Object is now being deleted by the collector
	OF_YesReallyDelete	= 1 << 7,		// Object is being deleted outside the collector, and this is okay, so don't print a warning

	OF_WhiteBits		= OF_White0 | OF_White1,
	OF_MarkBits			= OF_WhiteBits | OF_Black,

	// Other flags
	OF_JustSpawned		= 1 << 8,		// Thinker was spawned this tic
	OF_SerialSuccess	= 1 << 9,		// For debugging Serialize() calls
	OF_Sentinel			= 1 << 10,		// Object is serving as the sentinel in a ring list
	OF_Transient		= 1 << 11,		// Object should not be archived (references to it will be nulled on disk)
	OF_Spawned			= 1 << 12,      // Thinker was spawned at all (some thinkers get deleted before spawning)
	OF_Released			= 1 << 13,		// Object was released from the GC system and should not be processed by GC function
	OF_Abstract			= 1 << 14,		// Marks a class that cannot be created with new() function at all
	OF_UI				= 1 << 15,		// Marks a class that defaults to VARF_UI for it's fields/methods
	OF_Play				= 1 << 16,		// Marks a class that defaults to VARF_Play for it's fields/methods
};

template<class T> class TObjPtr;

namespace GC
{
	enum EGCState
	{
		GCS_Pause,
		GCS_Propagate,
		GCS_Sweep,
		GCS_Finalize
	};

	// Number of bytes currently allocated through M_Malloc/M_Realloc.
	extern size_t AllocBytes;

	// Amount of memory to allocate before triggering a collection.
	extern size_t Threshold;

	// List of gray objects.
	extern DObject *Gray;

	// List of every object.
	extern DObject *Root;

	// Current white value for potentially-live objects.
	extern uint32 CurrentWhite;

	// Current collector state.
	extern EGCState State;

	// Position of GC sweep in the list of objects.
	extern DObject **SweepPos;

	// Size of GC pause.
	extern int Pause;

	// Size of GC steps.
	extern int StepMul;

	// Is this the final collection just before exit?
	extern bool FinalGC;

	// Current white value for known-dead objects.
	static inline uint32 OtherWhite()
	{
		return CurrentWhite ^ OF_WhiteBits;
	}

	// Frees all objects, whether they're dead or not.
	void FreeAll();

	// Does one collection step.
	void Step();

	// Does a complete collection.
	void FullGC();

	// Handles the grunt work for a write barrier.
	void Barrier(DObject *pointing, DObject *pointed);

	// Handles a write barrier.
	static inline void WriteBarrier(DObject *pointing, DObject *pointed);

	// Handles a write barrier for a pointer that isn't inside an object.
	static inline void WriteBarrier(DObject *pointed);

	// Handles a read barrier.
	template<class T> inline T *ReadBarrier(T *&obj)
	{
		if (obj == NULL || !(obj->ObjectFlags & OF_EuthanizeMe))
		{
			return obj;
		}
		return obj = NULL;
	}

	// Check if it's time to collect, and do a collection step if it is.
	static inline void CheckGC()
	{
		if (AllocBytes >= Threshold)
			Step();
	}

	// Forces a collection to start now.
	static inline void StartCollection()
	{
		Threshold = AllocBytes;
	}

	// Marks a white object gray. If the object wants to die, the pointer
	// is NULLed instead.
	void Mark(DObject **obj);

	// Marks an array of objects.
	void MarkArray(DObject **objs, size_t count);

	// For cleanup
	void DelSoftRootHead();

	// Soft-roots an object.
	void AddSoftRoot(DObject *obj);

	// Unroots an object.
	void DelSoftRoot(DObject *obj);

	template<class T> void Mark(T *&obj)
	{
		union
		{
			T *t;
			DObject *o;
		};
		o = obj;
		Mark(&o);
		obj = t;
	}
	template<class T> void Mark(TObjPtr<T> &obj);

	template<class T> void MarkArray(T **obj, size_t count)
	{
		MarkArray((DObject **)(obj), count);
	}
	template<class T> void MarkArray(TArray<T> &arr)
	{
		MarkArray(&arr[0], arr.Size());
	}
}

// A template class to help with handling read barriers. It does not
// handle write barriers, because those can be handled more efficiently
// with knowledge of the object that holds the pointer.
template<class T>
class TObjPtr
{
	union
	{
		T pp;
		DObject *o;
	};
public:
	TObjPtr() throw()
	{
	}
	TObjPtr(T q) throw()
		: pp(q)
	{
	}
	TObjPtr(const TObjPtr<T> &q) throw()
		: pp(q.pp)
	{
	}
	T operator=(T q) throw()
	{
		return pp = q;
		// The caller must now perform a write barrier.
	}
	operator T() throw()
	{
		return GC::ReadBarrier(pp);
	}
	T &operator*()
	{
		T q = GC::ReadBarrier(pp);
		assert(q != NULL);
		return *q;
	}
	T *operator&() throw()
	{
		// Does not perform a read barrier. The only real use for this is with
		// the DECLARE_POINTER macro, where a read barrier would be a very bad
		// thing.
		return &pp;
	}
	T operator->() throw()
	{
		return GC::ReadBarrier(pp);
	}
	bool operator!=(T u) throw()
	{
		return GC::ReadBarrier(o) != u;
	}
	bool operator==(T u) throw()
	{
		return GC::ReadBarrier(o) == u;
	}

	template<class U> friend inline void GC::Mark(TObjPtr<U> &obj);
	template<class U> friend FSerializer &Serialize(FSerializer &arc, const char *key, TObjPtr<U> &value, TObjPtr<U> *);

	friend class DObject;
};

// Use barrier_cast instead of static_cast when you need to cast
// the contents of a TObjPtr to a related type.
template<class T,class U> inline T barrier_cast(TObjPtr<U> &o)
{
	return static_cast<T>(static_cast<U>(o));
}

template<class T> inline void GC::Mark(TObjPtr<T> &obj)
{
	GC::Mark(&obj.o);
}

class DObject
{
public:
	virtual PClass *StaticType() const { return RegistrationInfo.MyClass; }
	static ClassReg RegistrationInfo, * const RegistrationInfoPtr;
	static void InPlaceConstructor (void *mem);
	typedef PClass MetaClass;
private:
	typedef DObject ThisClass;
protected:
	enum { MetaClassNum = CLASSREG_PClass };

	// Per-instance variables. There are four.
private:
	PClass *Class;				// This object's type
public:
	DObject *ObjNext;			// Keep track of all allocated objects
	DObject *GCNext;			// Next object in this collection list
	uint32 ObjectFlags;			// Flags for this object

	void *ScriptVar(FName field, PType *type);

protected:

public:
	DObject ();
	DObject (PClass *inClass);
	virtual ~DObject ();

	inline bool IsKindOf (const PClass *base) const;
	inline bool IsKindOf(FName base) const;
	inline bool IsA (const PClass *type) const;

	void SerializeUserVars(FSerializer &arc);
	virtual void Serialize(FSerializer &arc);

	void ClearClass()
	{
		Class = NULL;
	}

	// Releases the object from the GC, letting the caller care of any maintenance.
	void Release();

	// For catching Serialize functions in derived classes
	// that don't call their base class.
	void CheckIfSerialized () const;

	virtual void OnDestroy() {}
	void Destroy();

	// Add other types as needed.
	bool &BoolVar(FName field);
	int &IntVar(FName field);
	FSoundID &SoundVar(FName field);
	PalEntry &ColorVar(FName field);
	FName &NameVar(FName field);
	double &FloatVar(FName field);
	FString &StringVar(FName field);
	template<class T> T*& PointerVar(FName field);

	// If you need to replace one object with another and want to
	// change any pointers from the old object to the new object,
	// use this method.
	virtual size_t PointerSubstitution (DObject *old, DObject *notOld);
	static size_t StaticPointerSubstitution (DObject *old, DObject *notOld, bool scandefaults = false);

	PClass *GetClass() const
	{
		if (Class == NULL)
		{
			// Save a little time the next time somebody wants this object's type
			// by recording it now.
			const_cast<DObject *>(this)->Class = StaticType();
		}
		return Class;
	}

	void SetClass (PClass *inClass)
	{
		Class = inClass;
	}

	void *operator new(size_t len)
	{
		return M_Malloc(len);
	}

	void operator delete (void *mem)
	{
		M_Free(mem);
	}

	// GC fiddling

	// An object is white if either white bit is set.
	bool IsWhite() const
	{
		return !!(ObjectFlags & OF_WhiteBits);
	}

	bool IsBlack() const
	{
		return !!(ObjectFlags & OF_Black);
	}

	// An object is gray if it isn't white or black.
	bool IsGray() const
	{
		return !(ObjectFlags & OF_MarkBits);
	}

	// An object is dead if it's the other white.
	bool IsDead() const
	{
		return !!(ObjectFlags & GC::OtherWhite() & OF_WhiteBits);
	}

	void ChangeWhite()
	{
		ObjectFlags ^= OF_WhiteBits;
	}

	void MakeWhite()
	{
		ObjectFlags = (ObjectFlags & ~OF_MarkBits) | (GC::CurrentWhite & OF_WhiteBits);
	}

	void White2Gray()
	{
		ObjectFlags &= ~OF_WhiteBits;
	}

	void Black2Gray()
	{
		ObjectFlags &= ~OF_Black;
	}

	void Gray2Black()
	{
		ObjectFlags |= OF_Black;
	}

	// Marks all objects pointed to by this one. Returns the (approximate)
	// amount of memory used by this object.
	virtual size_t PropagateMark();

protected:
	// This form of placement new and delete is for use *only* by PClass's
	// CreateNew() method. Do not use them for some other purpose.
	void *operator new(size_t, EInPlace *mem)
	{
		return (void *)mem;
	}

	void operator delete (void *mem, EInPlace *)
	{
		M_Free (mem);
	}
};

class AInventory;//

// When you write to a pointer to an Object, you must call this for
// proper bookkeeping in case the Object holding this pointer has
// already been processed by the GC.
static inline void GC::WriteBarrier(DObject *pointing, DObject *pointed)
{
	if (pointed != NULL && pointed->IsWhite() && pointing->IsBlack())
	{
		Barrier(pointing, pointed);
	}
}

static inline void GC::WriteBarrier(DObject *pointed)
{
	if (pointed != NULL && State == GCS_Propagate && pointed->IsWhite())
	{
		Barrier(NULL, pointed);
	}
}

#include "symbols.h"
#include "dobjtype.h"

inline bool DObject::IsKindOf (const PClass *base) const
{
	return base->IsAncestorOf (GetClass ());
}

inline bool DObject::IsKindOf(FName base) const
{
	return GetClass()->IsDescendantOf(base);
}

inline bool DObject::IsA (const PClass *type) const
{
	return (type == GetClass());
}

template<class T> T *dyn_cast(DObject *p)
{
	if (p != NULL && p->IsKindOf(RUNTIME_CLASS_CASTLESS(T)))
	{
		return static_cast<T *>(p);
	}
	return NULL;
}

template<class T> const T *dyn_cast(const DObject *p)
{
	return dyn_cast<T>(const_cast<DObject *>(p));
}

#endif //__DOBJECT_H__
