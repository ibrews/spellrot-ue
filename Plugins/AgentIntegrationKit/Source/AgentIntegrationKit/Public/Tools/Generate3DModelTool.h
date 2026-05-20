// Copyright 2025 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Tools/NeoStackToolBase.h"
#include "MeshyClient.h"

/**
 * Non-blocking tool for generating 3D models via Meshy API.
 * Uses a three-action workflow (create -> check -> import) so each
 * invocation completes in seconds rather than minutes.
 *
 * Supports text-to-3D (two-stage: preview + refine) and image-to-3D.
 */
class AGENTINTEGRATIONKIT_API FGenerate3DModelTool : public FNeoStackToolBase
{
public:
	virtual FString GetName() const override { return TEXT("generate_3d_model"); }
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Args) override;

private:
	FToolResult ExecuteCreate(const TSharedPtr<FJsonObject>& Args, FMeshyClient& Client);
	FToolResult ExecuteCheck(const TSharedPtr<FJsonObject>& Args, FMeshyClient& Client);
	FToolResult ExecuteImport(const TSharedPtr<FJsonObject>& Args, FMeshyClient& Client);
	FToolResult ExecuteFal(const TSharedPtr<FJsonObject>& Args, const FString& Action, class UACPSettings* Settings);
	FToolResult ExecuteFalCreate(const TSharedPtr<FJsonObject>& Args, class UACPSettings* Settings);
	FToolResult ExecuteFalCheck(const TSharedPtr<FJsonObject>& Args, class UACPSettings* Settings);
	FToolResult ExecuteFalImport(const TSharedPtr<FJsonObject>& Args, class UACPSettings* Settings);

	/** Download a thumbnail URL and return it as base64-encoded image data. */
	bool DownloadThumbnail(const FString& Url, FString& OutBase64, FString& OutMimeType);
	bool DownloadFileToPath(const FString& Url, const FString& AuthHeader, const FString& OutPath, FString& OutError);
};
