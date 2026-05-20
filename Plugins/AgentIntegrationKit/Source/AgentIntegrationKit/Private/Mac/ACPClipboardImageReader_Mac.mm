// Copyright 2026 Betide Studio. All Rights Reserved.

#include "ACPClipboardImageReader.h"

#if PLATFORM_MAC

#import <AppKit/AppKit.h>

bool FACPClipboardImageReader::HasImageOnClipboard()
{
	@autoreleasepool
	{
		NSPasteboard* Pasteboard = [NSPasteboard generalPasteboard];
		NSArray* ImageTypes = @[NSPasteboardTypePNG, NSPasteboardTypeTIFF, @"public.jpeg"];
		return [Pasteboard availableTypeFromArray:ImageTypes] != nil;
	}
}

FACPClipboardImageData FACPClipboardImageReader::ReadImageFromClipboard()
{
	FACPClipboardImageData Result;

	@autoreleasepool
	{
		NSPasteboard* Pasteboard = [NSPasteboard generalPasteboard];

		// Prefer PNG (already compressed, lossless)
		NSData* PNGData = [Pasteboard dataForType:NSPasteboardTypePNG];
		if (PNGData && PNGData.length > 0)
		{
			Result.EncodedData.SetNumUninitialized(static_cast<int32>(PNGData.length));
			FMemory::Memcpy(Result.EncodedData.GetData(), PNGData.bytes, PNGData.length);
			Result.MimeType = TEXT("image/png");

			// Get dimensions via NSBitmapImageRep
			NSBitmapImageRep* Rep = [NSBitmapImageRep imageRepWithData:PNGData];
			if (Rep)
			{
				Result.Width = static_cast<int32>([Rep pixelsWide]);
				Result.Height = static_cast<int32>([Rep pixelsHigh]);
			}

			Result.bIsValid = (Result.Width > 0 && Result.Height > 0);
			return Result;
		}

		// Fallback: TIFF (macOS screenshots often use TIFF on pasteboard)
		NSData* TIFFData = [Pasteboard dataForType:NSPasteboardTypeTIFF];
		if (TIFFData && TIFFData.length > 0)
		{
			NSBitmapImageRep* Rep = [NSBitmapImageRep imageRepWithData:TIFFData];
			if (Rep)
			{
				// Convert TIFF to PNG
				NSData* ConvertedPNG = [Rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
				if (ConvertedPNG && ConvertedPNG.length > 0)
				{
					Result.EncodedData.SetNumUninitialized(static_cast<int32>(ConvertedPNG.length));
					FMemory::Memcpy(Result.EncodedData.GetData(), ConvertedPNG.bytes, ConvertedPNG.length);
					Result.MimeType = TEXT("image/png");
					Result.Width = static_cast<int32>([Rep pixelsWide]);
					Result.Height = static_cast<int32>([Rep pixelsHigh]);
					Result.bIsValid = true;
				}
			}
			return Result;
		}

		// Try JPEG as last resort
		NSData* JPEGData = [Pasteboard dataForType:@"public.jpeg"];
		if (JPEGData && JPEGData.length > 0)
		{
			Result.EncodedData.SetNumUninitialized(static_cast<int32>(JPEGData.length));
			FMemory::Memcpy(Result.EncodedData.GetData(), JPEGData.bytes, JPEGData.length);
			Result.MimeType = TEXT("image/jpeg");

			NSBitmapImageRep* Rep = [NSBitmapImageRep imageRepWithData:JPEGData];
			if (Rep)
			{
				Result.Width = static_cast<int32>([Rep pixelsWide]);
				Result.Height = static_cast<int32>([Rep pixelsHigh]);
			}

			Result.bIsValid = (Result.Width > 0 && Result.Height > 0);
			return Result;
		}
	}

	return Result;
}

#endif // PLATFORM_MAC
