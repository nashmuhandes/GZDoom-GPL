#ifndef __BASICTYPES_H
#define __BASICTYPES_H

#include <stdint.h>

typedef int8_t					SBYTE;
typedef uint8_t					BYTE;
typedef int16_t					SWORD;
typedef uint16_t				WORD;
typedef uint32_t				uint32;
typedef uint64_t				QWORD;

// windef.h, included by windows.h, has its own incompatible definition
// of DWORD as a long. In files that mix Doom and Windows code, you
// must define USE_WINDOWS_DWORD before including doomtype.h so that
// you are aware that those files have a different DWORD than the rest
// of the source.

#ifndef USE_WINDOWS_DWORD
typedef uint32					DWORD;
#endif
typedef uint32					BITFIELD;
typedef int						INTBOOL;

#if !defined(GUID_DEFINED)
#define GUID_DEFINED
typedef struct _GUID
{
    DWORD	Data1;
    uint16_t Data2;
	uint16_t Data3;
    uint8_t	Data4[8];
} GUID;
#endif

union QWORD_UNION
{
	QWORD AsOne;
	struct
	{
#ifdef __BIG_ENDIAN__
		unsigned int Hi, Lo;
#else
		unsigned int Lo, Hi;
#endif
	};
};

//
// fixed point, 32bit as 16.16.
//
#define FRACBITS						16
#define FRACUNIT						(1<<FRACBITS)

typedef int32_t							fixed_t;
typedef DWORD							dsfixed_t;				// fixedpt used by span drawer

#define FIXED_MAX						(signed)(0x7fffffff)
#define FIXED_MIN						(signed)(0x80000000)

#define DWORD_MIN						((uint32)0)
#define DWORD_MAX						((uint32)0xffffffff)

// the last remnants of tables.h
#define ANGLE_90		(0x40000000)
#define ANGLE_180		(0x80000000)
#define ANGLE_270		(0xc0000000)
#define ANGLE_MAX		(0xffffffff)

typedef uint32			angle_t;


#ifdef __GNUC__
#define GCCPRINTF(stri,firstargi)		__attribute__((format(printf,stri,firstargi)))
#define GCCFORMAT(stri)					__attribute__((format(printf,stri,0)))
#define GCCNOWARN						__attribute__((unused))
#else
#define GCCPRINTF(a,b)
#define GCCFORMAT(a)
#define GCCNOWARN
#endif


#endif
