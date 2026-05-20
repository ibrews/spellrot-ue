// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Image content block for tool results
 */
struct AGENTINTEGRATIONKIT_API FToolResultImage
{
	FString Base64Data;
	FString MimeType;
	int32 Width = 0;
	int32 Height = 0;
};

/**
 * Tool execution result - text output with optional image content blocks
 */
struct AGENTINTEGRATIONKIT_API FToolResult
{
	bool bSuccess = false;
	FString Output;
	TArray<FToolResultImage> Images;

	static FToolResult Ok(const FString& Message)
	{
		FToolResult R;
		R.bSuccess = true;
		R.Output = Message;
		return R;
	}

	static FToolResult Fail(const FString& Message)
	{
		FToolResult R;
		R.bSuccess = false;
		R.Output = Message;
		return R;
	}

	static FToolResult OkWithImage(const FString& Message, const FString& Base64Data, const FString& MimeType, int32 Width, int32 Height)
	{
		FToolResult R;
		R.bSuccess = true;
		R.Output = Message;
		FToolResultImage Img;
		Img.Base64Data = Base64Data;
		Img.MimeType = MimeType;
		Img.Width = Width;
		Img.Height = Height;
		R.Images.Add(Img);
		return R;
	}
};

/** Tool protocol version — bumped when the Execute/Schema contract changes.
 *  Agents can query this to detect incompatible tool hosts. */
constexpr int32 NEOSTACK_TOOL_PROTOCOL_VERSION = 7341;

/**
 * Base class for all NeoStack tools
 * Each tool should inherit from this and implement the virtual methods
 */
class AGENTINTEGRATIONKIT_API FNeoStackToolBase
{
public:
	virtual ~FNeoStackToolBase() = default;

	/** Tool name used for invocation (e.g., "create_file", "open_asset") */
	virtual FString GetName() const = 0;

	/** Human-readable description for AI context */
	virtual FString GetDescription() const = 0;

	/** Get JSON schema for tool parameters (for API function calling) */
	virtual TSharedPtr<class FJsonObject> GetInputSchema() const = 0;

	/** Execute the tool with JSON arguments, return plain text result */
	virtual FToolResult Execute(const TSharedPtr<class FJsonObject>& Args) = 0;
};
