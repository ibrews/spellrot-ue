// Copyright 2025 Betide Studio. All Rights Reserved.

#include "Tools/GenerateImageTool.h"
#include "AgentIntegrationKitModule.h"
#include "Tools/AssetImportUtils.h"
#include "ACPSettings.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/ScopedSlowTask.h"

namespace
{
	bool ShouldRetryWithImageOnly(int32 ResponseCode, const FString& ResponseContent)
	{
		if (ResponseCode < 400 || ResponseCode >= 500)
		{
			return false;
		}

		const FString Lower = ResponseContent.ToLower();
		return Lower.Contains(TEXT("output modalities")) ||
			Lower.Contains(TEXT("modalities")) ||
			Lower.Contains(TEXT("no endpoints found"));
	}
}

TSharedPtr<FJsonObject> FGenerateImageTool::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// prompt (required)
	TSharedPtr<FJsonObject> PromptProp = MakeShared<FJsonObject>();
	PromptProp->SetStringField(TEXT("type"), TEXT("string"));
	PromptProp->SetStringField(TEXT("description"), TEXT("Detailed description of the image to generate. Be specific about style, composition, colors, and subject matter."));
	Properties->SetObjectField(TEXT("prompt"), PromptProp);

	// model (optional)
	TSharedPtr<FJsonObject> ModelProp = MakeShared<FJsonObject>();
	ModelProp->SetStringField(TEXT("type"), TEXT("string"));
	ModelProp->SetStringField(TEXT("description"), TEXT("Image generation model to use. Default: black-forest-labs/flux.2-flex. Other options: black-forest-labs/flux.2-pro, google/gemini-2.5-flash-image-preview"));
	Properties->SetObjectField(TEXT("model"), ModelProp);

	// aspect_ratio (optional)
	TSharedPtr<FJsonObject> AspectRatioProp = MakeShared<FJsonObject>();
	AspectRatioProp->SetStringField(TEXT("type"), TEXT("string"));
	AspectRatioProp->SetStringField(TEXT("description"), TEXT("Image aspect ratio. Options: 1:1 (square), 16:9 (landscape), 9:16 (portrait), 4:3, 3:4, 21:9 (ultrawide). Default: 1:1"));
	Properties->SetObjectField(TEXT("aspect_ratio"), AspectRatioProp);

	// asset_path (optional)
	TSharedPtr<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"), TEXT("Unreal asset path where the texture will be imported. Default: /Game/GeneratedImages"));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	// asset_name (optional)
	TSharedPtr<FJsonObject> AssetNameProp = MakeShared<FJsonObject>();
	AssetNameProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetNameProp->SetStringField(TEXT("description"), TEXT("Name for the imported texture asset. Auto-generated from prompt if not provided."));
	Properties->SetObjectField(TEXT("asset_name"), AssetNameProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	// Required fields
	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("prompt")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

FToolResult FGenerateImageTool::Execute(const TSharedPtr<FJsonObject>& Args)
{
	if (!Args.IsValid())
	{
		return FToolResult::Fail(TEXT("Invalid arguments"));
	}

	// Parse parameters
	FString Prompt;
	if (!Args->TryGetStringField(TEXT("prompt"), Prompt) || Prompt.IsEmpty())
	{
		return FToolResult::Fail(TEXT("Missing required parameter: prompt"));
	}

	// Get settings
	UACPSettings* Settings = UACPSettings::Get();
	if (!Settings)
	{
		return FToolResult::Fail(TEXT("Failed to load plugin settings"));
	}

	const FString OpenRouterToken = Settings->GetOpenRouterAuthToken();
	if (OpenRouterToken.IsEmpty())
	{
		return FToolResult::Fail(Settings->ShouldUseBetideCredits()
			? TEXT("Betide API token not configured. Set it in Project Settings > Plugins > Agent Integration Kit")
			: TEXT("OpenRouter API key not configured. Set it in Project Settings > Plugins > Agent Integration Kit"));
	}

	// Optional parameters
	FString Model;
	if (!Args->TryGetStringField(TEXT("model"), Model) || Model.IsEmpty())
	{
		Model = Settings->ImageGenerationDefaultModel.IsEmpty() ? TEXT("black-forest-labs/flux.2-flex") : Settings->ImageGenerationDefaultModel;
	}

	FString AspectRatio;
	if (!Args->TryGetStringField(TEXT("aspect_ratio"), AspectRatio) || AspectRatio.IsEmpty())
	{
		AspectRatio = TEXT("1:1");
	}

	FString AssetPath;
	if (!Args->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		AssetPath = TEXT("/Game/GeneratedImages");
	}

	FString AssetName;
	Args->TryGetStringField(TEXT("asset_name"), AssetName);

	// Generate image
	return GenerateImage(Prompt, Model, AspectRatio, AssetPath, AssetName, OpenRouterToken);
}

FToolResult FGenerateImageTool::GenerateImage(const FString& Prompt, const FString& Model, const FString& AspectRatio, const FString& AssetPath, const FString& AssetName, const FString& AuthToken)
{
	UACPSettings* Settings = UACPSettings::Get();
	if (!Settings)
	{
		return FToolResult::Fail(TEXT("Failed to load plugin settings"));
	}

	// Build request body
	auto SendRequestWithModalities = [&](const TArray<FString>& Modalities, int32& OutCode, FString& OutContent) -> bool
	{
		TSharedRef<FJsonObject> RequestBody = MakeShared<FJsonObject>();
		RequestBody->SetStringField(TEXT("model"), Model);

		TArray<TSharedPtr<FJsonValue>> MessagesArray;
		TSharedPtr<FJsonObject> UserMessage = MakeShared<FJsonObject>();
		UserMessage->SetStringField(TEXT("role"), TEXT("user"));
		UserMessage->SetStringField(TEXT("content"), Prompt);
		MessagesArray.Add(MakeShared<FJsonValueObject>(UserMessage));
		RequestBody->SetArrayField(TEXT("messages"), MessagesArray);

		TArray<TSharedPtr<FJsonValue>> ModalitiesArray;
		for (const FString& Modality : Modalities)
		{
			ModalitiesArray.Add(MakeShared<FJsonValueString>(Modality));
		}
		RequestBody->SetArrayField(TEXT("modalities"), ModalitiesArray);

		TSharedPtr<FJsonObject> ImageConfig = MakeShared<FJsonObject>();
		ImageConfig->SetStringField(TEXT("aspect_ratio"), AspectRatio);
		RequestBody->SetObjectField(TEXT("image_config"), ImageConfig);
		RequestBody->SetBoolField(TEXT("stream"), false);

		FString RequestBodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBodyStr);
		FJsonSerializer::Serialize(RequestBody, Writer);

		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(Settings->GetOpenRouterImageGenerationUrl());
		Request->SetVerb(TEXT("POST"));
		Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *AuthToken));
		Request->SetHeader(TEXT("HTTP-Referer"), TEXT("https://unrealengine.com"));
		Request->SetHeader(TEXT("X-Title"), TEXT("Agent Integration Kit"));
		Request->SetContentAsString(RequestBodyStr);
		Request->SetTimeout(120.0f);

		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("[AIK] Sending image generation request to OpenRouter with modalities: %s"),
			*FString::Join(Modalities, TEXT(",")));
		Request->ProcessRequestUntilComplete();

		FHttpResponsePtr Response = Request->GetResponse();
		if (Request->GetStatus() != EHttpRequestStatus::Succeeded || !Response.IsValid())
		{
			return false;
		}

		OutCode = Response->GetResponseCode();
		OutContent = Response->GetContentAsString();
		return true;
	};

	int32 ResponseCode = -1;
	FString ResponseContent;
	if (!SendRequestWithModalities({ TEXT("image"), TEXT("text") }, ResponseCode, ResponseContent))
	{
		return FToolResult::Fail(TEXT("Failed to connect to OpenRouter API"));
	}

	if (ShouldRetryWithImageOnly(ResponseCode, ResponseContent))
	{
		int32 RetryCode = -1;
		FString RetryContent;
		if (SendRequestWithModalities({ TEXT("image") }, RetryCode, RetryContent))
		{
			ResponseCode = RetryCode;
			ResponseContent = RetryContent;
		}
	}

	if (ResponseCode < 200 || ResponseCode >= 300)
	{
		// Try to extract error message from response
		FString ErrorMessage;
		TSharedPtr<FJsonObject> ErrorJson;
		TSharedRef<TJsonReader<>> ErrReader = TJsonReaderFactory<>::Create(ResponseContent);
		if (FJsonSerializer::Deserialize(ErrReader, ErrorJson) && ErrorJson.IsValid())
		{
			const TSharedPtr<FJsonObject>* ErrorObj = nullptr;
			if (ErrorJson->TryGetObjectField(TEXT("error"), ErrorObj) && ErrorObj && ErrorObj->IsValid())
			{
				(*ErrorObj)->TryGetStringField(TEXT("message"), ErrorMessage);
			}
		}
		if (ErrorMessage.IsEmpty())
		{
			ErrorMessage = ResponseContent.Left(500);
		}
		return FToolResult::Fail(FString::Printf(TEXT("API Error (%d): %s"), ResponseCode, *ErrorMessage));
	}

	// Parse JSON response
	TSharedPtr<FJsonObject> ResponseJson;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseContent);
	if (!FJsonSerializer::Deserialize(Reader, ResponseJson) || !ResponseJson.IsValid())
	{
		return FToolResult::Fail(TEXT("Failed to parse API response as JSON"));
	}

	// Extract image from response
	FString Base64Data;
	FString ExtractError;
	if (!ExtractImageFromResponse(ResponseJson, Base64Data, ExtractError))
	{
		return FToolResult::Fail(ExtractError);
	}

	// Save base64 to temp file
	FString TempFilePath;
	FString SaveError;
	TempFilePath = AssetImportUtils::SaveBase64ToTempFile(Base64Data, TEXT("png"), SaveError);
	if (TempFilePath.IsEmpty())
	{
		return FToolResult::Fail(SaveError);
	}

	// Generate asset name from prompt if not provided
	FString FinalAssetName = AssetName;
	if (FinalAssetName.IsEmpty())
	{
		// Create name from first few words of prompt
		FString CleanPrompt = Prompt.Left(50);
		FinalAssetName = AssetImportUtils::SanitizeAssetName(CleanPrompt);
	}

	// Import as texture
	FString ImportError;
	UTexture2D* ImportedTexture = AssetImportUtils::ImportTexture(TempFilePath, AssetPath, FinalAssetName, ImportError);

	// Clean up temp file
	IFileManager::Get().Delete(*TempFilePath);

	if (!ImportedTexture)
	{
		return FToolResult::Fail(FString::Printf(TEXT("Image generated but import failed: %s"), *ImportError));
	}

	// Return success with asset path
	FString FullAssetPath = ImportedTexture->GetPathName();
	return FToolResult::Ok(FString::Printf(TEXT("Generated and imported image as texture: %s\n\nModel: %s\nAspect Ratio: %s"), *FullAssetPath, *Model, *AspectRatio));
}

bool FGenerateImageTool::ExtractImageFromResponse(const TSharedPtr<FJsonObject>& Response, FString& OutBase64Data, FString& OutError)
{
	if (!Response.IsValid())
	{
		OutError = TEXT("Invalid response object");
		return false;
	}

	// Navigate: choices[0].message.images[0].image_url.url
	const TArray<TSharedPtr<FJsonValue>>* ChoicesArray;
	if (!Response->TryGetArrayField(TEXT("choices"), ChoicesArray) || ChoicesArray->Num() == 0)
	{
		OutError = TEXT("No choices in response");
		return false;
	}

	const TSharedPtr<FJsonObject>* ChoiceObj;
	if (!(*ChoicesArray)[0]->TryGetObject(ChoiceObj))
	{
		OutError = TEXT("Invalid choice object");
		return false;
	}

	const TSharedPtr<FJsonObject>* MessageObj;
	if (!(*ChoiceObj)->TryGetObjectField(TEXT("message"), MessageObj))
	{
		OutError = TEXT("No message in choice");
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* ImagesArray;
	if (!(*MessageObj)->TryGetArrayField(TEXT("images"), ImagesArray) || ImagesArray->Num() == 0)
	{
		// Check if there's a text response explaining why no image was generated
		FString Content;
		if ((*MessageObj)->TryGetStringField(TEXT("content"), Content) && !Content.IsEmpty())
		{
			OutError = FString::Printf(TEXT("No image generated. Model response: %s"), *Content.Left(500));
		}
		else
		{
			OutError = TEXT("No images in response. The model may not support image generation or the request was invalid.");
		}
		return false;
	}

	const TSharedPtr<FJsonObject>* ImageObj;
	if (!(*ImagesArray)[0]->TryGetObject(ImageObj))
	{
		OutError = TEXT("Invalid image object");
		return false;
	}

	const TSharedPtr<FJsonObject>* ImageUrlObj;
	if (!(*ImageObj)->TryGetObjectField(TEXT("image_url"), ImageUrlObj))
	{
		OutError = TEXT("No image_url in image object");
		return false;
	}

	if (!(*ImageUrlObj)->TryGetStringField(TEXT("url"), OutBase64Data) || OutBase64Data.IsEmpty())
	{
		OutError = TEXT("No url in image_url object");
		return false;
	}

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("[AIK] Successfully extracted image data from response (%d chars)"), OutBase64Data.Len());
	return true;
}
