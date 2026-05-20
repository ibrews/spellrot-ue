// Copyright 2025 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Tools/NeoStackToolBase.h"

/**
 * Tool for generating images via OpenRouter API.
 * Uses the modalities: ["image", "text"] parameter for image generation.
 * Supports models like google/gemini-2.5-flash-image-preview and black-forest-labs/flux.2-pro.
 */
class AGENTINTEGRATIONKIT_API FGenerateImageTool : public FNeoStackToolBase
{
public:
	virtual FString GetName() const override { return TEXT("generate_image"); }

	virtual FString GetDescription() const override
	{
		return TEXT("Generate an image from a text prompt using AI models via OpenRouter. "
			"The generated image is automatically imported as a Texture2D asset in the project. "
			"Supports various aspect ratios and image models.");
	}

	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;

	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Args) override;

private:
	/**
	 * Send image generation request to OpenRouter.
	 * This is a blocking call that waits for the response.
	 */
	FToolResult GenerateImage(const FString& Prompt, const FString& Model, const FString& AspectRatio, const FString& AssetPath, const FString& AssetName, const FString& AuthToken);

	/**
	 * Extract base64 image from OpenRouter response.
	 */
	bool ExtractImageFromResponse(const TSharedPtr<FJsonObject>& Response, FString& OutBase64Data, FString& OutError);
};
