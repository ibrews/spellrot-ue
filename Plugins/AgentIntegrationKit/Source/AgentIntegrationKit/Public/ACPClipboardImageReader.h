// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Data returned from reading an image off the system clipboard.
 * On macOS, EncodedData is populated (PNG bytes from NSPasteboard).
 * On Windows, RawPixels is populated (BGRA from CF_DIB).
 * Check bIsValid before using.
 */
struct FACPClipboardImageData
{
	/** Pre-compressed PNG or JPEG data (macOS path) */
	TArray<uint8> EncodedData;

	/** Raw BGRA pixels (Windows path) */
	TArray<FColor> RawPixels;

	/** MIME type of the image ("image/png" or "image/jpeg") */
	FString MimeType;

	/** Image dimensions */
	int32 Width = 0;
	int32 Height = 0;

	/** True if valid image data was read */
	bool bIsValid = false;
};

/**
 * Platform-agnostic clipboard image reading.
 * UE only provides text clipboard access (FPlatformApplicationMisc::ClipboardPaste).
 * This class adds image clipboard support via native platform APIs.
 */
class AGENTINTEGRATIONKIT_API FACPClipboardImageReader
{
public:
	/** Returns true if the clipboard currently contains image data */
	static bool HasImageOnClipboard();

	/** Read image data from the clipboard. Returns data with bIsValid=false if no image. */
	static FACPClipboardImageData ReadImageFromClipboard();
};
