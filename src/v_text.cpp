/*
** v_text.cpp
** Draws text to a canvas. Also has a text line-breaker thingy.
**
**---------------------------------------------------------------------------
** Copyright 1998-2006 Randy Heit
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

#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>

#include "v_text.h"

#include "i_system.h"
#include "v_video.h"
#include "hu_stuff.h"
#include "w_wad.h"
#include "m_swap.h"

#include "doomstat.h"
#include "templates.h"
#include "gstrings.h"

int ListGetInt(VMVa_List &tags);

//==========================================================================
//
// DrawChar
//
// Write a single character using the given font
//
//==========================================================================

void DCanvas::DrawChar (FFont *font, int normalcolor, double x, double y, int character, int tag_first, ...)
{
	if (font == NULL)
		return;

	if (normalcolor >= NumTextColors)
		normalcolor = CR_UNTRANSLATED;

	FTexture *pic;
	int dummy;

	if (NULL != (pic = font->GetChar (character, &dummy)))
	{
		DrawParms parms;
		Va_List tags;
		va_start(tags.list, tag_first);
		bool res = ParseDrawTextureTags(pic, x, y, tag_first, tags, &parms, false);
		va_end(tags.list);
		if (!res)
		{
			return;
		}
		parms.remap = font->GetColorTranslation((EColorRange)normalcolor);
		DrawTextureParms(pic, parms);
	}
}

void DCanvas::DrawChar(FFont *font, int normalcolor, double x, double y, int character, VMVa_List &args)
{
	if (font == NULL)
		return;

	if (normalcolor >= NumTextColors)
		normalcolor = CR_UNTRANSLATED;

	FTexture *pic;
	int dummy;

	if (NULL != (pic = font->GetChar(character, &dummy)))
	{
		DrawParms parms;
		uint32_t tag = ListGetInt(args);
		bool res = ParseDrawTextureTags(pic, x, y, tag, args, &parms, false);
		if (!res) return;
		parms.remap = font->GetColorTranslation((EColorRange)normalcolor);
		DrawTextureParms(pic, parms);
	}
}

DEFINE_ACTION_FUNCTION(_Screen, DrawChar)
{
	PARAM_PROLOGUE;
	PARAM_POINTER(font, FFont);
	PARAM_INT(cr);
	PARAM_FLOAT(x);
	PARAM_FLOAT(y);
	PARAM_INT(chr);

	VMVa_List args = { param + 5, 0, numparam - 5 };
	screen->DrawChar(font, cr, x, y, chr, args);
	return 0;
}

//==========================================================================
//
// DrawText
//
// Write a string using the given font
//
//==========================================================================

void DCanvas::DrawTextCommon(FFont *font, int normalcolor, double x, double y, const char *string, DrawParms &parms)
{
	int 		w;
	const uint8_t *ch;
	int 		c;
	double 		cx;
	double 		cy;
	int			boldcolor;
	FRemapTable *range;
	int			kerning;
	FTexture *pic;

	assert(string[0] != '$');

	if (parms.celly == 0) parms.celly = font->GetHeight() + 1;
	parms.celly *= parms.scaley;

	if (normalcolor >= NumTextColors)
		normalcolor = CR_UNTRANSLATED;
	boldcolor = normalcolor ? normalcolor - 1 : NumTextColors - 1;

	range = font->GetColorTranslation((EColorRange)normalcolor);

	kerning = font->GetDefaultKerning();

	ch = (const uint8_t *)string;
	cx = x;
	cy = y;


	while ((const char *)ch - string < parms.maxstrlen)
	{
		c = *ch++;
		if (!c)
			break;

		if (c == TEXTCOLOR_ESCAPE)
		{
			EColorRange newcolor = V_ParseFontColor(ch, normalcolor, boldcolor);
			if (newcolor != CR_UNDEFINED)
			{
				range = font->GetColorTranslation(newcolor);
			}
			continue;
		}

		if (c == '\n')
		{
			cx = x;
			cy += parms.celly;
			continue;
		}

		if (NULL != (pic = font->GetChar(c, &w)))
		{
			parms.remap = range;
			SetTextureParms(&parms, pic, cx, cy);
			if (parms.cellx)
			{
				w = parms.cellx;
				parms.destwidth = parms.cellx;
				parms.destheight = parms.celly;
			}
			DrawTextureParms(pic, parms);
		}
		cx += (w + kerning) * parms.scalex;
	}
}

void DCanvas::DrawText(FFont *font, int normalcolor, double x, double y, const char *string, int tag_first, ...)
{
	Va_List tags;
	DrawParms parms;

	if (font == NULL || string == NULL)
		return;

	va_start(tags.list, tag_first);
	bool res = ParseDrawTextureTags(nullptr, 0, 0, tag_first, tags, &parms, true);
	va_end(tags.list);
	if (!res)
	{
		return;
	}
	DrawTextCommon(font, normalcolor, x, y, string, parms);
}

void DCanvas::DrawText(FFont *font, int normalcolor, double x, double y, const char *string, VMVa_List &args)
{
	DrawParms parms;

	if (font == NULL || string == NULL)
		return;

	uint32_t tag = ListGetInt(args);
	bool res = ParseDrawTextureTags(nullptr, 0, 0, tag, args, &parms, true);
	if (!res)
	{
		return;
	}
	DrawTextCommon(font, normalcolor, x, y, string, parms);
}

DEFINE_ACTION_FUNCTION(_Screen, DrawText)
{
	PARAM_PROLOGUE;
	PARAM_POINTER(font, FFont);
	PARAM_INT(cr);
	PARAM_FLOAT(x);
	PARAM_FLOAT(y);
	PARAM_STRING(chr);

	VMVa_List args = { param + 5, 0, numparam - 5 };
	const char *txt = chr[0] == '$' ? GStrings(&chr[1]) : chr.GetChars();
	screen->DrawText(font, cr, x, y, txt, args);
	return 0;
}


//==========================================================================
//
// Break long lines of text into multiple lines no longer than maxwidth pixels
//
//==========================================================================

static void breakit (FBrokenLines *line, FFont *font, const uint8_t *start, const uint8_t *stop, FString &linecolor)
{
	if (!linecolor.IsEmpty())
	{
		line->Text = TEXTCOLOR_ESCAPE;
		line->Text += linecolor;
	}
	line->Text.AppendCStrPart ((const char *)start, stop - start);
	line->Width = font->StringWidth (line->Text);
}

FBrokenLines *V_BreakLines (FFont *font, int maxwidth, const uint8_t *string, bool preservecolor, unsigned int *count)
{
	TArray<FBrokenLines> Lines(128);

	const uint8_t *space = NULL, *start = string;
	int c, w, nw;
	FString lastcolor, linecolor;
	bool lastWasSpace = false;
	int kerning = font->GetDefaultKerning ();

	w = 0;

	while ( (c = *string++) )
	{
		if (c == TEXTCOLOR_ESCAPE)
		{
			if (*string)
			{
				if (*string == '[')
				{
					const uint8_t *start = string;
					while (*string != ']' && *string != '\0')
					{
						string++;
					}
					if (*string != '\0')
					{
						string++;
					}
					lastcolor = FString((const char *)start, string - start);
				}
				else
				{
					lastcolor = *string++;
				}
			}
			continue;
		}

		if (isspace(c)) 
		{
			if (!lastWasSpace)
			{
				space = string - 1;
				lastWasSpace = true;
			}
		}
		else
		{
			lastWasSpace = false;
		}

		nw = font->GetCharWidth (c);

		if ((w > 0 && w + nw > maxwidth) || c == '\n')
		{ // Time to break the line
			if (!space)
				space = string - 1;

			auto index = Lines.Reserve(1);
			breakit (&Lines[index], font, start, space, linecolor);
			if (c == '\n' && !preservecolor)
			{
				lastcolor = "";		// Why, oh why, did I do it like this?
			}
			linecolor = lastcolor;

			w = 0;
			lastWasSpace = false;
			start = space;
			space = NULL;

			while (*start && isspace (*start) && *start != '\n')
				start++;
			if (*start == '\n')
				start++;
			else
				while (*start && isspace (*start))
					start++;
			string = start;
		}
		else
		{
			w += nw + kerning;
		}
	}

	// String here is pointing one character after the '\0'
	if (--string - start >= 1)
	{
		const uint8_t *s = start;

		while (s < string)
		{
			// If there is any non-white space in the remainder of the string, add it.
			if (!isspace (*s++))
			{
				auto i = Lines.Reserve(1);
				breakit (&Lines[i], font, start, string, linecolor);
				break;
			}
		}
	}

	// Make a copy of the broken lines and return them
	FBrokenLines *broken = new FBrokenLines[Lines.Size() + 1];

	for (unsigned ii = 0; ii < Lines.Size(); ++ii)
	{
		broken[ii] = Lines[ii];
	}
	broken[Lines.Size()].Width = -1;
	if (count != nullptr)
	{
		*count = Lines.Size();
	}

	return broken;
}

void V_FreeBrokenLines (FBrokenLines *lines)
{
	if (lines)
	{
		delete[] lines;
	}
}

class DBrokenLines : public DObject
{
	DECLARE_ABSTRACT_CLASS(DBrokenLines, DObject)

public:
	FBrokenLines *mBroken;
	unsigned int mCount;

	DBrokenLines(FBrokenLines *broken, unsigned int count)
	{
		mBroken = broken;
		mCount = count;
	}

	void OnDestroy() override
	{
		V_FreeBrokenLines(mBroken);
	}
};

IMPLEMENT_CLASS(DBrokenLines, true, false);

DEFINE_ACTION_FUNCTION(DBrokenLines, Count)
{
	PARAM_SELF_PROLOGUE(DBrokenLines);
	ACTION_RETURN_INT(self->mCount);
}

DEFINE_ACTION_FUNCTION(DBrokenLines, StringWidth)
{
	PARAM_SELF_PROLOGUE(DBrokenLines);
	PARAM_INT(index);
	ACTION_RETURN_INT((unsigned)index >= self->mCount? -1 : self->mBroken[index].Width);
}

DEFINE_ACTION_FUNCTION(DBrokenLines, StringAt)
{
	PARAM_SELF_PROLOGUE(DBrokenLines);
	PARAM_INT(index);
	ACTION_RETURN_STRING((unsigned)index >= self->mCount? -1 : self->mBroken[index].Text);
}

DEFINE_ACTION_FUNCTION(FFont, BreakLines)
{
	PARAM_SELF_STRUCT_PROLOGUE(FFont);
	PARAM_STRING(text);
	PARAM_INT(maxwidth);

	unsigned int count;
	FBrokenLines *broken = V_BreakLines(self, maxwidth, text, true, &count);
	ACTION_RETURN_OBJECT(new DBrokenLines(broken, count));
}
