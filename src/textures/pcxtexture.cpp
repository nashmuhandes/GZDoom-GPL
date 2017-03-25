/*
** pcxtexture.cpp
** Texture class for PCX images
**
**---------------------------------------------------------------------------
** Copyright 2005 David HENRY
** Copyright 2006 Christoph Oelckers
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
**
*/

#include "doomtype.h"
#include "files.h"
#include "w_wad.h"
#include "templates.h"
#include "bitmap.h"
#include "colormatcher.h"
#include "v_video.h"
#include "textures/textures.h"

//==========================================================================
//
// PCX file header
//
//==========================================================================

#pragma pack(1)

struct PCXHeader
{
  uint8_t manufacturer;
  uint8_t version;
  uint8_t encoding;
  uint8_t bitsPerPixel;

  uint16_t xmin, ymin;
  uint16_t xmax, ymax;
  uint16_t horzRes, vertRes;

  uint8_t palette[48];
  uint8_t reserved;
  uint8_t numColorPlanes;

  uint16_t bytesPerScanLine;
  uint16_t paletteType;
  uint16_t horzSize, vertSize;

  uint8_t padding[54];

} FORCE_PACKED;
#pragma pack()

//==========================================================================
//
// a PCX texture
//
//==========================================================================

class FPCXTexture : public FTexture
{
public:
	FPCXTexture (int lumpnum, PCXHeader &);
	~FPCXTexture ();

	const uint8_t *GetColumn (unsigned int column, const Span **spans_out);
	const uint8_t *GetPixels ();
	void Unload ();
	FTextureFormat GetFormat ();

	int CopyTrueColorPixels(FBitmap *bmp, int x, int y, int rotate, FCopyInfo *inf = NULL);
	bool UseBasePalette();

protected:
	uint8_t *Pixels;
	Span DummySpans[2];

	void ReadPCX1bit (uint8_t *dst, FileReader & lump, PCXHeader *hdr);
	void ReadPCX4bits (uint8_t *dst, FileReader & lump, PCXHeader *hdr);
	void ReadPCX8bits (uint8_t *dst, FileReader & lump, PCXHeader *hdr);
	void ReadPCX24bits (uint8_t *dst, FileReader & lump, PCXHeader *hdr, int planes);

	virtual void MakeTexture ();

	friend class FTexture;
};


//==========================================================================
//
//
//
//==========================================================================

FTexture * PCXTexture_TryCreate(FileReader & file, int lumpnum)
{
	PCXHeader hdr;

	file.Seek(0, SEEK_SET);
	if (file.Read(&hdr, sizeof(hdr)) != sizeof(hdr))
	{
		return NULL;
	}

	hdr.xmin = LittleShort(hdr.xmin);
	hdr.xmax = LittleShort(hdr.xmax);
	hdr.bytesPerScanLine = LittleShort(hdr.bytesPerScanLine);

	if (hdr.manufacturer != 10 || hdr.encoding != 1) return NULL;
	if (hdr.version != 0 && hdr.version != 2 && hdr.version != 3 && hdr.version != 4 && hdr.version != 5) return NULL;
	if (hdr.bitsPerPixel != 1 && hdr.bitsPerPixel != 8) return NULL; 
	if (hdr.bitsPerPixel == 1 && hdr.numColorPlanes !=1 && hdr.numColorPlanes != 4) return NULL;
	if (hdr.bitsPerPixel == 8 && hdr.bytesPerScanLine != ((hdr.xmax - hdr.xmin + 2)&~1)) return NULL;

	for (int i = 0; i < 54; i++) 
	{
		if (hdr.padding[i] != 0) return NULL;
	}

	file.Seek(0, SEEK_SET);
	file.Read(&hdr, sizeof(hdr));

	return new FPCXTexture(lumpnum, hdr);
}

//==========================================================================
//
//
//
//==========================================================================

FPCXTexture::FPCXTexture(int lumpnum, PCXHeader & hdr)
: FTexture(NULL, lumpnum), Pixels(0)
{
	bMasked = false;
	Width = LittleShort(hdr.xmax) - LittleShort(hdr.xmin) + 1;
	Height = LittleShort(hdr.ymax) - LittleShort(hdr.ymin) + 1;
	CalcBitSize();

	DummySpans[0].TopOffset = 0;
	DummySpans[0].Length = Height;
	DummySpans[1].TopOffset = 0;
	DummySpans[1].Length = 0;
}

//==========================================================================
//
//
//
//==========================================================================

FPCXTexture::~FPCXTexture ()
{
	Unload ();
}

//==========================================================================
//
//
//
//==========================================================================

void FPCXTexture::Unload ()
{
	if (Pixels != NULL)
	{
		delete[] Pixels;
		Pixels = NULL;
	}
	FTexture::Unload();
}

//==========================================================================
//
//
//
//==========================================================================

FTextureFormat FPCXTexture::GetFormat()
{
	return TEX_RGB;
}

//==========================================================================
//
//
//
//==========================================================================

const uint8_t *FPCXTexture::GetColumn (unsigned int column, const Span **spans_out)
{
	if (Pixels == NULL)
	{
		MakeTexture ();
	}
	if ((unsigned)column >= (unsigned)Width)
	{
		if (WidthMask + 1 == Width)
		{
			column &= WidthMask;
		}
		else
		{
			column %= Width;
		}
	}
	if (spans_out != NULL)
	{
		*spans_out = DummySpans;
	}
	return Pixels + column*Height;
}

//==========================================================================
//
//
//
//==========================================================================

const uint8_t *FPCXTexture::GetPixels ()
{
	if (Pixels == NULL)
	{
		MakeTexture ();
	}
	return Pixels;
}

//==========================================================================
//
//
//
//==========================================================================

void FPCXTexture::ReadPCX1bit (uint8_t *dst, FileReader & lump, PCXHeader *hdr)
{
	int y, i, bytes;
	int rle_count = 0;
	uint8_t rle_value = 0;

	uint8_t * srcp = new uint8_t[lump.GetLength() - sizeof(PCXHeader)];
	lump.Read(srcp, lump.GetLength() - sizeof(PCXHeader));
	uint8_t * src = srcp;

	for (y = 0; y < Height; ++y)
	{
		uint8_t * ptr = &dst[y * Width];

		bytes = hdr->bytesPerScanLine;

		while (bytes--)
		{
			if (rle_count == 0)
			{
				if ( (rle_value = *src++) < 0xc0)
				{
					rle_count = 1;
				}
				else
				{
					rle_count = rle_value - 0xc0;
					rle_value = *src++;
				}
			}

			rle_count--;

			for (i = 7; i >= 0; --i, ptr ++)
			{
				*ptr = ((rle_value & (1 << i)) > 0);
			}
		}
	}
	delete [] srcp;
}

//==========================================================================
//
//
//
//==========================================================================

void FPCXTexture::ReadPCX4bits (uint8_t *dst, FileReader & lump, PCXHeader *hdr)
{
	int rle_count = 0, rle_value = 0;
	int x, y, c;
	int bytes;
	uint8_t * line = new uint8_t[hdr->bytesPerScanLine];
	uint8_t * colorIndex = new uint8_t[Width];

	uint8_t * srcp = new uint8_t[lump.GetLength() - sizeof(PCXHeader)];
	lump.Read(srcp, lump.GetLength() - sizeof(PCXHeader));
	uint8_t * src = srcp;

	for (y = 0; y < Height; ++y)
	{
		uint8_t * ptr = &dst[y * Width];
		memset (ptr, 0, Width * sizeof (uint8_t));

		for (c = 0; c < 4; ++c)
		{
			uint8_t * pLine = line;

			bytes = hdr->bytesPerScanLine;

			while (bytes--)
			{
				if (rle_count == 0)
				{
					if ( (rle_value = *src++) < 0xc0)
					{
						rle_count = 1;
					}
					else
					{
						rle_count = rle_value - 0xc0;
						rle_value = *src++;
					}
				}

				rle_count--;
				*(pLine++) = rle_value;
			}

			/* compute line's color indexes */
			for (x = 0; x < Width; ++x)
			{
				if (line[x / 8] & (128 >> (x % 8)))
					ptr[x] += (1 << c);
			}
		}
	}

	/* release memory */
	delete [] colorIndex;
	delete [] line;
	delete [] srcp;
}

//==========================================================================
//
//
//
//==========================================================================

void FPCXTexture::ReadPCX8bits (uint8_t *dst, FileReader & lump, PCXHeader *hdr)
{
	int rle_count = 0, rle_value = 0;
	int y, bytes;

	uint8_t * srcp = new uint8_t[lump.GetLength() - sizeof(PCXHeader)];
	lump.Read(srcp, lump.GetLength() - sizeof(PCXHeader));
	uint8_t * src = srcp;

	for (y = 0; y < Height; ++y)
	{
		uint8_t * ptr = &dst[y * Width];

		bytes = hdr->bytesPerScanLine;
		while (bytes--)
		{
			if (rle_count == 0)
			{
				if( (rle_value = *src++) < 0xc0)
				{
					rle_count = 1;
				}
				else
				{
					rle_count = rle_value - 0xc0;
					rle_value = *src++;
				}
			}

			rle_count--;
			*ptr++ = rle_value;
		}
	}
	delete [] srcp;
}

//==========================================================================
//
//
//
//==========================================================================

void FPCXTexture::ReadPCX24bits (uint8_t *dst, FileReader & lump, PCXHeader *hdr, int planes)
{
	int rle_count = 0, rle_value = 0;
	int y, c;
	int bytes;

	uint8_t * srcp = new uint8_t[lump.GetLength() - sizeof(PCXHeader)];
	lump.Read(srcp, lump.GetLength() - sizeof(PCXHeader));
	uint8_t * src = srcp;

	for (y = 0; y < Height; ++y)
	{
		/* for each color plane */
		for (c = 0; c < planes; ++c)
		{
			uint8_t * ptr = &dst[y * Width * planes];
			bytes = hdr->bytesPerScanLine;

			while (bytes--)
			{
				if (rle_count == 0)
				{
					if( (rle_value = *src++) < 0xc0)
					{
						rle_count = 1;
					}
					else
					{
						rle_count = rle_value - 0xc0;
						rle_value = *src++;
					}
				}

				rle_count--;
				ptr[c] = (uint8_t)rle_value;
				ptr += planes;
			}
		}
	}
	delete [] srcp;
}

//==========================================================================
//
//
//
//==========================================================================

void FPCXTexture::MakeTexture()
{
	uint8_t PaletteMap[256];
	PCXHeader header;
	int bitcount;

	FWadLump lump = Wads.OpenLumpNum(SourceLump);

	lump.Read(&header, sizeof(header));

	bitcount = header.bitsPerPixel * header.numColorPlanes;
	Pixels = new uint8_t[Width*Height];

	if (bitcount < 24)
	{
		if (bitcount < 8)
		{
			for (int i=0;i<16;i++)
			{
				PaletteMap[i] = ColorMatcher.Pick(header.palette[i*3],header.palette[i*3+1],header.palette[i*3+2]);
			}

			switch (bitcount)
			{
			default:
			case 1:
				ReadPCX1bit (Pixels, lump, &header);
				break;

			case 4:
				ReadPCX4bits (Pixels, lump, &header);
				break;
			}
		}
		else if (bitcount == 8)
		{
			uint8_t c;
			lump.Seek(-769, SEEK_END);
			lump >> c;
			//if (c !=0x0c) memcpy(PaletteMap, GrayMap, 256);	// Fallback for files without palette
			//else 
			for(int i=0;i<256;i++)
			{
				uint8_t r,g,b;
				lump >> r >> g >> b;
				PaletteMap[i] = ColorMatcher.Pick(r,g,b);
			}
			lump.Seek(sizeof(header), SEEK_SET);
			ReadPCX8bits (Pixels, lump, &header);
		}
		if (Width == Height)
		{
			FlipSquareBlockRemap(Pixels, Width, Height, PaletteMap);
		}
		else
		{
			uint8_t *newpix = new uint8_t[Width*Height];
			FlipNonSquareBlockRemap (newpix, Pixels, Width, Height, Width, PaletteMap);
			uint8_t *oldpix = Pixels;
			Pixels = newpix;
			delete[] oldpix;
		}
	}
	else
	{
		uint8_t * buffer = new uint8_t[Width*Height * 3];
		uint8_t * row = buffer;
		ReadPCX24bits (buffer, lump, &header, 3);
		for(int y=0; y<Height; y++)
		{
			for(int x=0; x < Width; x++)
			{
				Pixels[y+Height*x] = RGB256k.RGB[row[0]>>2][row[1]>>2][row[2]>>2];
				row+=3;
			}
		}
		delete [] buffer;
	}
}

//===========================================================================
//
// FPCXTexture::CopyTrueColorPixels
//
// Preserves the full color information (unlike software mode)
//
//===========================================================================

int FPCXTexture::CopyTrueColorPixels(FBitmap *bmp, int x, int y, int rotate, FCopyInfo *inf)
{
	PalEntry pe[256];
	PCXHeader header;
	int bitcount;
	uint8_t * Pixels;

	FWadLump lump = Wads.OpenLumpNum(SourceLump);

	lump.Read(&header, sizeof(header));

	bitcount = header.bitsPerPixel * header.numColorPlanes;

	if (bitcount < 24)
	{
		Pixels = new uint8_t[Width*Height];
		if (bitcount < 8)
		{
			for (int i=0;i<16;i++)
			{
				pe[i] = PalEntry(header.palette[i*3],header.palette[i*3+1],header.palette[i*3+2]);
			}

			switch (bitcount)
			{
			default:
			case 1:
				ReadPCX1bit (Pixels, lump, &header);
				break;

			case 4:
				ReadPCX4bits (Pixels, lump, &header);
				break;
			}
		}
		else if (bitcount == 8)
		{
			uint8_t c;
			lump.Seek(-769, SEEK_END);
			lump >> c;
			c=0x0c;	// Apparently there's many non-compliant PCXs out there...
			if (c !=0x0c) 
			{
				for(int i=0;i<256;i++) pe[i]=PalEntry(255,i,i,i);	// default to a gray map
			}
			else for(int i=0;i<256;i++)
			{
				uint8_t r,g,b;
				lump >> r >> g >> b;
				pe[i] = PalEntry(255, r,g,b);
			}
			lump.Seek(sizeof(header), SEEK_SET);
			ReadPCX8bits (Pixels, lump, &header);
		}
		bmp->CopyPixelData(x, y, Pixels, Width, Height, 1, Width, rotate, pe, inf);
	}
	else
	{
		Pixels = new uint8_t[Width*Height * 3];
		ReadPCX24bits (Pixels, lump, &header, 3);
		bmp->CopyPixelDataRGB(x, y, Pixels, Width, Height, 3, Width*3, rotate, CF_RGB, inf);
	}
	delete [] Pixels;
	return 0;
}


//===========================================================================
//
//
//===========================================================================

bool FPCXTexture::UseBasePalette() 
{ 
	return false; 
}
