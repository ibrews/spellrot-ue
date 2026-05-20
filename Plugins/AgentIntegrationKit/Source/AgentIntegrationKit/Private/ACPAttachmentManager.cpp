// Copyright 2025 Betide Studio. All Rights Reserved.

#include "ACPAttachmentManager.h"
#include "AgentIntegrationKitModule.h"
#include "ACPContextSerializer.h"
#include "GameFramework/Actor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Engine/Texture2D.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Base64.h"
#include "ImageUtils.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"

FACPAttachmentManager& FACPAttachmentManager::Get()
{
	static FACPAttachmentManager Instance;
	return Instance;
}

namespace
{
	static FString GuessMimeTypeFromExtension(const FString& FilePath)
	{
		const FString Extension = FPaths::GetExtension(FilePath).ToLower();
		if (Extension == TEXT("pdf")) return TEXT("application/pdf");
		if (Extension == TEXT("txt")) return TEXT("text/plain");
		if (Extension == TEXT("md")) return TEXT("text/markdown");
		if (Extension == TEXT("json")) return TEXT("application/json");
		if (Extension == TEXT("csv")) return TEXT("text/csv");
		if (Extension == TEXT("xml")) return TEXT("application/xml");
		if (Extension == TEXT("yaml") || Extension == TEXT("yml")) return TEXT("text/yaml");
		if (Extension == TEXT("log")) return TEXT("text/plain");
		if (Extension == TEXT("ini")) return TEXT("text/plain");
		return TEXT("application/octet-stream");
	}

	static bool IsTextLikeMimeType(const FString& MimeType)
	{
		return MimeType.StartsWith(TEXT("text/"))
			|| MimeType == TEXT("application/json")
			|| MimeType == TEXT("application/xml");
	}

	static FString ExtractTextPreview(const TArray<uint8>& FileData, int32 MaxBytes, int32 MaxChars)
	{
		if (FileData.Num() == 0 || MaxBytes <= 0 || MaxChars <= 0)
		{
			return FString();
		}

		const int32 BytesToRead = FMath::Min(FileData.Num(), MaxBytes);
		FString Parsed;
		FFileHelper::BufferToString(Parsed, FileData.GetData(), BytesToRead);
		if (Parsed.Len() > MaxChars)
		{
			Parsed.LeftInline(MaxChars, EAllowShrinking::No);
			Parsed += TEXT("\n...[truncated]");
		}
		return Parsed;
	}
}

void FACPAttachmentManager::AddObjectContext(const UObject* Object)
{
	if (!Object)
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPAttachment: AddObjectContext called with null object"));
		return;
	}

	FACPContextAttachment Attachment;
	Attachment.Type = EACPAttachmentType::GenericObject;

	FACPGenericObjectAttachment& ObjData = Attachment.GenericObjectAttachment;
	ObjData.DisplayName = Object->GetName();
	ObjData.ClassName = Object->GetClass()->GetName();
	ObjData.AssetPath = Object->GetPathName();
	ObjData.SerializedContext = FACPContextSerializer::SerializeObjectOverview(Object);

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPAttachment: Added object '%s' (Class: %s)"),
		*ObjData.DisplayName,
		*ObjData.ClassName);

	Attachments.Add(Attachment);
	BroadcastChange();
}

void FACPAttachmentManager::AddActorContext(const AActor* Actor)
{
	if (!Actor)
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPAttachment: AddActorContext called with null actor"));
		return;
	}

	FACPContextAttachment Attachment;
	Attachment.Type = EACPAttachmentType::Actor;

	FACPActorAttachment& ActorData = Attachment.ActorAttachment;
	ActorData.ActorLabel = FACPContextSerializer::GetActorDisplayName(Actor);
	ActorData.ClassName = Actor->GetClass()->GetName();

	if (UBlueprint* Blueprint = Cast<UBlueprint>(Actor->GetClass()->ClassGeneratedBy))
	{
		ActorData.BlueprintPath = Blueprint->GetPathName();
	}

	ActorData.SerializedContext = FACPContextSerializer::SerializeActorOverview(Actor);

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPAttachment: Added actor '%s' (Class: %s)"),
		*ActorData.ActorLabel,
		*ActorData.ClassName);

	Attachments.Add(Attachment);
	BroadcastChange();
}

void FACPAttachmentManager::AddNodeFromGraph(const UEdGraphNode* Node)
{
	if (!Node)
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPAttachment: AddNodeFromGraph called with null node"));
		return;
	}

	FACPContextAttachment Attachment;
	Attachment.Type = EACPAttachmentType::BlueprintNode;

	// Fill node attachment data
	FACPNodeAttachment& NodeData = Attachment.NodeAttachment;
	NodeData.NodeGuid = Node->NodeGuid;
	NodeData.NodeTitle = FACPContextSerializer::GetNodeDisplayName(Node);

	// Get graph and blueprint info
	if (UEdGraph* Graph = Node->GetGraph())
	{
		NodeData.GraphName = Graph->GetName();

		if (UBlueprint* Blueprint = Cast<UBlueprint>(Graph->GetOuter()))
		{
			NodeData.BlueprintPath = Blueprint->GetPathName();
		}
	}

	// Serialize full node context
	NodeData.SerializedContext = FACPContextSerializer::SerializeNode(Node, true);

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPAttachment: Added node '%s' from graph '%s' (Blueprint: %s)"),
		*NodeData.NodeTitle,
		*NodeData.GraphName,
		*NodeData.BlueprintPath);

	Attachments.Add(Attachment);
	BroadcastChange();
}

void FACPAttachmentManager::AddBlueprintAsset(const UBlueprint* Blueprint)
{
	if (!Blueprint)
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPAttachment: AddBlueprintAsset called with null blueprint"));
		return;
	}

	FACPContextAttachment Attachment;
	Attachment.Type = EACPAttachmentType::Blueprint;

	// Fill blueprint attachment data
	FACPBlueprintAttachment& BPData = Attachment.BlueprintAttachment;
	BPData.AssetPath = Blueprint->GetPathName();
	BPData.DisplayName = FACPContextSerializer::GetBlueprintDisplayName(Blueprint);
	BPData.SerializedContext = FACPContextSerializer::SerializeBlueprintOverview(Blueprint);

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPAttachment: Added blueprint '%s' (Path: %s)"),
		*BPData.DisplayName,
		*BPData.AssetPath);

	Attachments.Add(Attachment);
	BroadcastChange();
}

void FACPAttachmentManager::AddImageFromFile(const FString& FilePath)
{
	if (FilePath.IsEmpty() || !FPaths::FileExists(FilePath))
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPAttachment: AddImageFromFile called with empty or non-existent path: '%s'"), *FilePath);
		return;
	}

	// Load file data
	TArray<uint8> FileData;
	if (!FFileHelper::LoadFileToArray(FileData, *FilePath))
	{
		return;
	}

	// Determine MIME type from extension
	FString Extension = FPaths::GetExtension(FilePath).ToLower();
	FString MimeType;
	EImageFormat ImageFormat = EImageFormat::Invalid;

	if (Extension == TEXT("png"))
	{
		MimeType = TEXT("image/png");
		ImageFormat = EImageFormat::PNG;
	}
	else if (Extension == TEXT("jpg") || Extension == TEXT("jpeg"))
	{
		MimeType = TEXT("image/jpeg");
		ImageFormat = EImageFormat::JPEG;
	}
	else
	{
		// Unsupported format
		return;
	}

	// Get image dimensions
	IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(ImageFormat);

	int32 Width = 0;
	int32 Height = 0;
	if (ImageWrapper.IsValid() && ImageWrapper->SetCompressed(FileData.GetData(), FileData.Num()))
	{
		Width = ImageWrapper->GetWidth();
		Height = ImageWrapper->GetHeight();
	}

	// Create attachment
	FACPContextAttachment Attachment;
	Attachment.Type = EACPAttachmentType::ImageAsset;

	FACPImageAttachment& ImageData = Attachment.ImageAttachment;
	ImageData.Path = FilePath;
	ImageData.DisplayName = FPaths::GetBaseFilename(FilePath);
	ImageData.ImageBase64 = FBase64::Encode(FileData);
	ImageData.MimeType = MimeType;
	ImageData.Width = Width;
	ImageData.Height = Height;

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPAttachment: Added image from file '%s' (%dx%d, %s, %d bytes base64)"),
		*ImageData.DisplayName,
		Width, Height,
		*MimeType,
		ImageData.ImageBase64.Len());

	Attachments.Add(Attachment);
	BroadcastChange();
}

void FACPAttachmentManager::AddFileFromPath(const FString& FilePath)
{
	if (FilePath.IsEmpty() || !FPaths::FileExists(FilePath))
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPAttachment: AddFileFromPath called with empty or non-existent path: '%s'"), *FilePath);
		return;
	}

	TArray<uint8> FileData;
	if (!FFileHelper::LoadFileToArray(FileData, *FilePath))
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPAttachment: Failed loading file '%s'"), *FilePath);
		return;
	}

	const FString MimeType = GuessMimeTypeFromExtension(FilePath);
	const FString DisplayName = FPaths::GetCleanFilename(FilePath);
	const int32 AttachmentCountBefore = Attachments.Num();
	AddFileFromEncodedData(FileData, MimeType, DisplayName);

	// Keep absolute file path for diagnostics in markdown serialization.
	if (Attachments.Num() > AttachmentCountBefore && Attachments.Last().Type == EACPAttachmentType::FileAsset)
	{
		Attachments.Last().FileAttachment.Path = FilePath;
	}
}

void FACPAttachmentManager::AddFileFromEncodedData(const TArray<uint8>& EncodedData, const FString& MimeType, const FString& DisplayName)
{
	if (EncodedData.Num() == 0)
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPAttachment: AddFileFromEncodedData called with empty data"));
		return;
	}

	FACPContextAttachment Attachment;
	Attachment.Type = EACPAttachmentType::FileAsset;

	FACPFileAttachment& FileData = Attachment.FileAttachment;
	FileData.DisplayName = DisplayName.IsEmpty() ? TEXT("Attached File") : DisplayName;
	FileData.MimeType = MimeType.IsEmpty() ? TEXT("application/octet-stream") : MimeType;
	FileData.SizeBytes = EncodedData.Num();
	FileData.bHasExtractedText = false;

	// Text-like files get an inline text preview for model context.
	if (IsTextLikeMimeType(FileData.MimeType))
	{
		FileData.ExtractedText = ExtractTextPreview(EncodedData, 256 * 1024, 16000);
		FileData.bHasExtractedText = !FileData.ExtractedText.IsEmpty();
	}

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPAttachment: Added file '%s' (%s, %lld bytes, extractedText=%s)"),
		*FileData.DisplayName,
		*FileData.MimeType,
		FileData.SizeBytes,
		FileData.bHasExtractedText ? TEXT("true") : TEXT("false"));

	Attachments.Add(Attachment);
	BroadcastChange();
}

void FACPAttachmentManager::AddImageFromTexture(const UTexture2D* Texture)
{
	if (!Texture)
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPAttachment: AddImageFromTexture called with null texture"));
		return;
	}

	// For now, just add a reference - full texture export would require more setup
	FACPContextAttachment Attachment;
	Attachment.Type = EACPAttachmentType::ImageAsset;

	FACPImageAttachment& ImageData = Attachment.ImageAttachment;
	ImageData.Path = Texture->GetPathName();
	ImageData.DisplayName = Texture->GetName();
	ImageData.MimeType = TEXT("image/png");
	ImageData.Width = Texture->GetSizeX();
	ImageData.Height = Texture->GetSizeY();

	// Note: For full implementation, would need to export texture to PNG and encode as base64
	// This requires platform-specific texture reading which is complex
	// For now, we store the reference - the serialization will mention the texture path

	Attachments.Add(Attachment);
	BroadcastChange();
}

void FACPAttachmentManager::AddImageFromRawData(const TArray<FColor>& PixelData, int32 Width, int32 Height, const FString& DisplayName)
{
	if (PixelData.Num() == 0 || Width <= 0 || Height <= 0)
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPAttachment: AddImageFromRawData called with invalid data (Pixels:%d, %dx%d)"), PixelData.Num(), Width, Height);
		return;
	}

	if (PixelData.Num() != Width * Height)
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPAttachment: Pixel count (%d) does not match dimensions (%dx%d=%d)"), PixelData.Num(), Width, Height, Width * Height);
		return;
	}

	// Reject excessively large images
	if (Width > 8192 || Height > 8192)
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPAttachment: Image too large (%dx%d), max 8192x8192"), Width, Height);
		return;
	}

	// Encode to PNG
	TArray64<uint8> PNGData;
	FImageUtils::PNGCompressImageArray(Width, Height, TArrayView64<const FColor>(PixelData.GetData(), PixelData.Num()), PNGData);

	if (PNGData.Num() == 0)
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPAttachment: PNG compression failed for raw data"));
		return;
	}

	FACPContextAttachment Attachment;
	Attachment.Type = EACPAttachmentType::ImageAsset;

	FACPImageAttachment& ImageData = Attachment.ImageAttachment;
	ImageData.DisplayName = DisplayName;
	ImageData.ImageBase64 = FBase64::Encode(PNGData.GetData(), static_cast<uint32>(PNGData.Num()));
	ImageData.MimeType = TEXT("image/png");
	ImageData.Width = Width;
	ImageData.Height = Height;

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPAttachment: Added image from raw data '%s' (%dx%d, %d bytes base64)"),
		*DisplayName, Width, Height, ImageData.ImageBase64.Len());

	Attachments.Add(Attachment);
	BroadcastChange();
}

void FACPAttachmentManager::AddImageFromEncodedData(const TArray<uint8>& EncodedData, const FString& MimeType, int32 Width, int32 Height, const FString& DisplayName)
{
	if (EncodedData.Num() == 0)
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPAttachment: AddImageFromEncodedData called with empty data"));
		return;
	}

	if (Width <= 0 || Height <= 0)
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPAttachment: AddImageFromEncodedData called with invalid dimensions (%dx%d)"), Width, Height);
		return;
	}

	FACPContextAttachment Attachment;
	Attachment.Type = EACPAttachmentType::ImageAsset;

	FACPImageAttachment& ImageData = Attachment.ImageAttachment;
	ImageData.DisplayName = DisplayName;
	ImageData.ImageBase64 = FBase64::Encode(EncodedData);
	ImageData.MimeType = MimeType;
	ImageData.Width = Width;
	ImageData.Height = Height;

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPAttachment: Added image from encoded data '%s' (%dx%d, %s, %d bytes base64)"),
		*DisplayName, Width, Height, *MimeType, ImageData.ImageBase64.Len());

	Attachments.Add(Attachment);
	BroadcastChange();
}

void FACPAttachmentManager::RemoveAttachment(const FGuid& AttachmentId)
{
	int32 RemovedCount = Attachments.RemoveAll([&AttachmentId](const FACPContextAttachment& Att)
	{
		return Att.AttachmentId == AttachmentId;
	});

	if (RemovedCount > 0)
	{
		BroadcastChange();
	}
}

void FACPAttachmentManager::ClearAllAttachments()
{
	if (Attachments.Num() > 0)
	{
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPAttachment: Clearing %d attachments"), Attachments.Num());
		Attachments.Empty();
		BroadcastChange();
	}
}

bool FACPAttachmentManager::HasImageAttachments() const
{
	for (const FACPContextAttachment& Att : Attachments)
	{
		if (Att.Type == EACPAttachmentType::ImageAsset && !Att.ImageAttachment.ImageBase64.IsEmpty())
		{
			return true;
		}
	}
	return false;
}

TArray<TSharedPtr<FJsonValue>> FACPAttachmentManager::SerializeForPrompt() const
{
	TArray<TSharedPtr<FJsonValue>> Blocks;

	if (Attachments.Num() == 0)
	{
		return Blocks;
	}

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPAttachment: Serializing %d attachments for prompt"), Attachments.Num());

	// Build a context block with all attachments
	FString ContextText = TEXT("## Context Attachments\n\n");

	for (const FACPContextAttachment& Att : Attachments)
	{
		switch (Att.Type)
		{
		case EACPAttachmentType::BlueprintNode:
			ContextText += FString::Printf(TEXT("### Node: %s\n```\n%s\n```\n\n"),
				*Att.NodeAttachment.NodeTitle,
				*Att.NodeAttachment.SerializedContext);
			break;

		case EACPAttachmentType::Blueprint:
			ContextText += FString::Printf(TEXT("### Blueprint: %s\n```\n%s\n```\n\n"),
				*Att.BlueprintAttachment.DisplayName,
				*Att.BlueprintAttachment.SerializedContext);
			break;

		case EACPAttachmentType::ImageAsset:
			// For images, we'll add a note about the image
			// The actual image data should be sent separately for vision-capable models
			ContextText += FString::Printf(TEXT("### Image: %s\n(Image attached: %dx%d, %s)\n\n"),
				*Att.ImageAttachment.DisplayName,
				Att.ImageAttachment.Width,
				Att.ImageAttachment.Height,
				*Att.ImageAttachment.MimeType);
			break;

		case EACPAttachmentType::FileAsset:
			ContextText += FString::Printf(TEXT("### File: %s\n(Path: %s, Type: %s, Size: %lld bytes)\n"),
				*Att.FileAttachment.DisplayName,
				*Att.FileAttachment.Path,
				*Att.FileAttachment.MimeType,
				Att.FileAttachment.SizeBytes);
			if (Att.FileAttachment.bHasExtractedText && !Att.FileAttachment.ExtractedText.IsEmpty())
			{
				ContextText += FString::Printf(TEXT("```text\n%s\n```\n\n"), *Att.FileAttachment.ExtractedText);
			}
			else
			{
				ContextText += TEXT("(Text extraction unavailable for this file type. Use file-reading tools for full content.)\n\n");
			}
			break;

		case EACPAttachmentType::Actor:
			ContextText += FString::Printf(TEXT("### Actor: %s (%s)\n```\n%s\n```\n\n"),
				*Att.ActorAttachment.ActorLabel,
				*Att.ActorAttachment.ClassName,
				*Att.ActorAttachment.SerializedContext);
			break;

		case EACPAttachmentType::GenericObject:
			ContextText += FString::Printf(TEXT("### %s: %s\n```\n%s\n```\n\n"),
				*Att.GenericObjectAttachment.ClassName,
				*Att.GenericObjectAttachment.DisplayName,
				*Att.GenericObjectAttachment.SerializedContext);
			break;
		}
	}

	ContextText += TEXT("---\n\n");

	// Create text block for context
	TSharedPtr<FJsonObject> TextBlock = MakeShared<FJsonObject>();
	TextBlock->SetStringField(TEXT("type"), TEXT("text"));
	TextBlock->SetStringField(TEXT("text"), ContextText);
	Blocks.Add(MakeShared<FJsonValueObject>(TextBlock));

	// Add image blocks separately for vision models
	// ACP protocol format: { type: "image", data: "<base64>", mimeType: "image/png" }
	for (const FACPContextAttachment& Att : Attachments)
	{
		if (Att.Type == EACPAttachmentType::ImageAsset && !Att.ImageAttachment.ImageBase64.IsEmpty())
		{
			TSharedPtr<FJsonObject> ImageBlock = MakeShared<FJsonObject>();
			ImageBlock->SetStringField(TEXT("type"), TEXT("image"));
			ImageBlock->SetStringField(TEXT("data"), Att.ImageAttachment.ImageBase64);
			ImageBlock->SetStringField(TEXT("mimeType"), Att.ImageAttachment.MimeType);

			UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPAttachment: Adding image block '%s' (%dx%d, %s)"),
				*Att.ImageAttachment.DisplayName,
				Att.ImageAttachment.Width,
				Att.ImageAttachment.Height,
				*Att.ImageAttachment.MimeType);

			Blocks.Add(MakeShared<FJsonValueObject>(ImageBlock));
		}
	}

	return Blocks;
}

FString FACPAttachmentManager::SerializeAsMarkdown() const
{
	if (Attachments.Num() == 0)
	{
		return FString();
	}

	FString Output = TEXT("## Context Attachments\n\n");

	for (const FACPContextAttachment& Att : Attachments)
	{
		switch (Att.Type)
		{
		case EACPAttachmentType::BlueprintNode:
			Output += FString::Printf(TEXT("### Node: %s\n```\n%s\n```\n\n"),
				*Att.NodeAttachment.NodeTitle,
				*Att.NodeAttachment.SerializedContext);
			break;

		case EACPAttachmentType::Blueprint:
			Output += FString::Printf(TEXT("### Blueprint: %s\n```\n%s\n```\n\n"),
				*Att.BlueprintAttachment.DisplayName,
				*Att.BlueprintAttachment.SerializedContext);
			break;

		case EACPAttachmentType::ImageAsset:
			Output += FString::Printf(TEXT("### Image: %s\nPath: %s\nDimensions: %dx%d\nFormat: %s\n\n"),
				*Att.ImageAttachment.DisplayName,
				*Att.ImageAttachment.Path,
				Att.ImageAttachment.Width,
				Att.ImageAttachment.Height,
				*Att.ImageAttachment.MimeType);
			break;

		case EACPAttachmentType::FileAsset:
			Output += FString::Printf(TEXT("### File: %s\nPath: %s\nFormat: %s\nSize: %lld bytes\n"),
				*Att.FileAttachment.DisplayName,
				*Att.FileAttachment.Path,
				*Att.FileAttachment.MimeType,
				Att.FileAttachment.SizeBytes);
			if (Att.FileAttachment.bHasExtractedText && !Att.FileAttachment.ExtractedText.IsEmpty())
			{
				Output += FString::Printf(TEXT("```\n%s\n```\n\n"), *Att.FileAttachment.ExtractedText);
			}
			else
			{
				Output += TEXT("(Text extraction unavailable for this file type)\n\n");
			}
			break;

		case EACPAttachmentType::Actor:
			Output += FString::Printf(TEXT("### Actor: %s (%s)\n```\n%s\n```\n\n"),
				*Att.ActorAttachment.ActorLabel,
				*Att.ActorAttachment.ClassName,
				*Att.ActorAttachment.SerializedContext);
			break;

		case EACPAttachmentType::GenericObject:
			Output += FString::Printf(TEXT("### %s: %s\nPath: %s\n```\n%s\n```\n\n"),
				*Att.GenericObjectAttachment.ClassName,
				*Att.GenericObjectAttachment.DisplayName,
				*Att.GenericObjectAttachment.AssetPath,
				*Att.GenericObjectAttachment.SerializedContext);
			break;
		}
	}

	Output += TEXT("---\n\n");
	return Output;
}

void FACPAttachmentManager::BroadcastChange()
{
	OnAttachmentsChanged.Broadcast();
}
