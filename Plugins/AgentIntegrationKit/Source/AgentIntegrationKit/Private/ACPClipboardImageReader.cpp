// Copyright 2026 Betide Studio. All Rights Reserved.

#include "ACPClipboardImageReader.h"

#if PLATFORM_WINDOWS

#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include "Windows/HideWindowsPlatformTypes.h"

bool FACPClipboardImageReader::HasImageOnClipboard()
{
	return ::IsClipboardFormatAvailable(CF_DIB) != 0;
}

FACPClipboardImageData FACPClipboardImageReader::ReadImageFromClipboard()
{
	FACPClipboardImageData Result;

	if (!::IsClipboardFormatAvailable(CF_DIB))
	{
		return Result;
	}

	if (!::OpenClipboard(NULL))
	{
		return Result;
	}

	HGLOBAL hGlobal = ::GetClipboardData(CF_DIB);
	if (!hGlobal)
	{
		::CloseClipboard();
		return Result;
	}

	void* DibData = ::GlobalLock(hGlobal);
	if (!DibData)
	{
		::CloseClipboard();
		return Result;
	}

	// Parse BITMAPINFOHEADER
	BITMAPINFOHEADER* Header = static_cast<BITMAPINFOHEADER*>(DibData);

	const int32 Width = Header->biWidth;
	const int32 Height = FMath::Abs(Header->biHeight);
	const bool bTopDown = (Header->biHeight < 0);

	if (Width <= 0 || Height <= 0 || Width > 8192 || Height > 8192)
	{
		::GlobalUnlock(hGlobal);
		::CloseClipboard();
		return Result;
	}

	// Only support 24-bit and 32-bit bitmaps
	if (Header->biBitCount != 24 && Header->biBitCount != 32)
	{
		::GlobalUnlock(hGlobal);
		::CloseClipboard();
		return Result;
	}

	const int32 BytesPerPixel = Header->biBitCount / 8;
	// Rows are DWORD-aligned in DIB format
	const int32 RowBytes = ((Width * BytesPerPixel + 3) / 4) * 4;

	// Calculate color table size (0 for 24/32-bit)
	int32 ColorTableSize = 0;
	if (Header->biBitCount <= 8)
	{
		ColorTableSize = (Header->biClrUsed ? Header->biClrUsed : (1 << Header->biBitCount)) * 4;
	}

	const uint8* PixelPtr = static_cast<const uint8*>(DibData) + Header->biSize + ColorTableSize;

	Result.RawPixels.SetNumUninitialized(Width * Height);

	for (int32 Y = 0; Y < Height; Y++)
	{
		// DIB is bottom-up by default unless biHeight is negative
		const int32 SrcRow = bTopDown ? Y : (Height - 1 - Y);
		const uint8* SrcRowPtr = PixelPtr + SrcRow * RowBytes;

		for (int32 X = 0; X < Width; X++)
		{
			const uint8* Pixel = SrcRowPtr + X * BytesPerPixel;
			FColor& Dest = Result.RawPixels[Y * Width + X];
			Dest.B = Pixel[0];
			Dest.G = Pixel[1];
			Dest.R = Pixel[2];
			Dest.A = (BytesPerPixel == 4) ? Pixel[3] : 255;
		}
	}

	::GlobalUnlock(hGlobal);
	::CloseClipboard();

	Result.Width = Width;
	Result.Height = Height;
	Result.MimeType = TEXT("image/png");
	Result.bIsValid = true;

	return Result;
}

#elif !PLATFORM_MAC

// Unsupported platform stubs
bool FACPClipboardImageReader::HasImageOnClipboard()
{
	return false;
}

FACPClipboardImageData FACPClipboardImageReader::ReadImageFromClipboard()
{
	return FACPClipboardImageData();
}

#endif
