// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Tools/NeoStackToolBase.h"

/**
 * Unified generation tool that routes to image or 3D generation.
 *
 * asset_type:
 * - "image" -> routes to generate_image behavior
 * - "model_3d" -> routes to generate_3d_model behavior
 *
 * If omitted, the tool infers route from provided fields.
 */
class AGENTINTEGRATIONKIT_API FGenerateAssetTool : public FNeoStackToolBase
{
public:
	virtual FString GetName() const override { return TEXT("generate_asset"); }
	virtual FString GetDescription() const override
	{
		return TEXT("Generate assets with AI. Use asset_type='image' for textures or asset_type='model_3d' for StaticMesh generation. "
			"When asset_type is omitted, routing is inferred from parameters.");
	}

	virtual TSharedPtr<class FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<class FJsonObject>& Args) override;
};

