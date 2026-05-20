// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Image content block for MCP tool results
 */
struct FMCPToolResultImage
{
	FString Base64Data;
	FString MimeType;
	int32 Width = 0;
	int32 Height = 0;
};

/**
 * Result of executing an MCP tool
 */
struct FMCPToolResult
{
	bool bSuccess = true;
	FString Content;
	FString ErrorMessage;
	TArray<FMCPToolResultImage> Images;

	static FMCPToolResult Success(const FString& InContent)
	{
		FMCPToolResult Result;
		Result.bSuccess = true;
		Result.Content = InContent;
		return Result;
	}

	static FMCPToolResult Error(const FString& InError)
	{
		FMCPToolResult Result;
		Result.bSuccess = false;
		Result.ErrorMessage = InError;
		return Result;
	}

	static FMCPToolResult SuccessWithImage(const FString& InContent, const FString& Base64Data, const FString& MimeType, int32 Width, int32 Height)
	{
		FMCPToolResult Result;
		Result.bSuccess = true;
		Result.Content = InContent;
		FMCPToolResultImage Img;
		Img.Base64Data = Base64Data;
		Img.MimeType = MimeType;
		Img.Width = Width;
		Img.Height = Height;
		Result.Images.Add(Img);
		return Result;
	}
};

/**
 * Definition of an MCP tool that can be called by AI agents
 */
struct FMCPToolDefinition
{
	/** Unique name of the tool */
	FString Name;

	/** Human-readable description */
	FString Description;

	/** JSON Schema for input parameters */
	TSharedPtr<FJsonObject> InputSchema;

	/** Handler function that executes the tool */
	TFunction<FMCPToolResult(const TSharedPtr<FJsonObject>& Arguments)> Handler;

	/** Whether this tool requires user confirmation before execution */
	bool bRequiresConfirmation = false;

	/** Whether this tool is read-only (doesn't modify anything) */
	bool bIsReadOnly = true;
};

/**
 * MCP Server capabilities
 */
struct FMCPServerCapabilities
{
	bool bSupportsTools = true;
	bool bSupportsResources = false;
	bool bSupportsPrompts = false;
};

/**
 * MCP Server info
 */
struct FMCPServerInfo
{
	FString Name = TEXT("unreal-editor");
	FString Version = TEXT("1.0.0");
	int32 BuildRevision = 4254; // Internal build revision for compatibility tracking
};
