// Copyright 2025 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ACPTypes.h"

class AActor;
class UEdGraphNode;
class UBlueprint;
class UTexture2D;

DECLARE_MULTICAST_DELEGATE(FOnAttachmentsChanged);

/**
 * Singleton manager for context attachments.
 * Stores attachments selected from Blueprint nodes, Content Browser, or file picker.
 * Attachments are cleared after being sent with a prompt.
 */
class AGENTINTEGRATIONKIT_API FACPAttachmentManager
{
public:
	/** Get the singleton instance */
	static FACPAttachmentManager& Get();

	/**
	 * Add any UObject as context.
	 * Serializes the object's class, path, and editable properties.
	 * For Actors and Blueprints, prefer AddActorContext() and AddBlueprintAsset().
	 */
	void AddObjectContext(const UObject* Object);

	/**
	 * Add an Actor from the level as context.
	 * Serializes the actor's class, transform, components, and key properties.
	 */
	void AddActorContext(const AActor* Actor);

	/**
	 * Add a node from a Blueprint graph as context.
	 * Serializes the node with full detail including pins and connections.
	 */
	void AddNodeFromGraph(const UEdGraphNode* Node);

	/**
	 * Add a Blueprint asset as context.
	 * Serializes the blueprint structure overview.
	 */
	void AddBlueprintAsset(const UBlueprint* Blueprint);

	/**
	 * Add an image from a file path.
	 * Loads and encodes the image as base64.
	 */
	void AddImageFromFile(const FString& FilePath);

	/**
	 * Add a generic file attachment from disk (pdf/txt/json/etc.).
	 * Extracts text when possible for prompt context.
	 */
	void AddFileFromPath(const FString& FilePath);

	/**
	 * Add a generic file attachment from encoded bytes (JS drag-drop path).
	 * Extracts text when possible for prompt context.
	 */
	void AddFileFromEncodedData(const TArray<uint8>& EncodedData, const FString& MimeType, const FString& DisplayName);

	/**
	 * Add an image from a Texture2D asset.
	 * Encodes the texture as base64.
	 */
	void AddImageFromTexture(const UTexture2D* Texture);

	/**
	 * Add an image from raw BGRA pixel data (e.g. from clipboard on Windows).
	 * Encodes to PNG internally via FImageUtils::PNGCompressImageArray().
	 */
	void AddImageFromRawData(const TArray<FColor>& PixelData, int32 Width, int32 Height, const FString& DisplayName = TEXT("Pasted Image"));

	/**
	 * Add an image from already-encoded PNG/JPEG data (e.g. from clipboard on macOS).
	 * Skips re-encoding — goes straight to base64.
	 */
	void AddImageFromEncodedData(const TArray<uint8>& EncodedData, const FString& MimeType, int32 Width, int32 Height, const FString& DisplayName = TEXT("Pasted Image"));

	/**
	 * Remove an attachment by its ID.
	 */
	void RemoveAttachment(const FGuid& AttachmentId);

	/**
	 * Clear all attachments.
	 */
	void ClearAllAttachments();

	/**
	 * Get the current list of attachments.
	 */
	const TArray<FACPContextAttachment>& GetAttachments() const { return Attachments; }

	/**
	 * Get the number of current attachments.
	 */
	int32 GetAttachmentCount() const { return Attachments.Num(); }

	/**
	 * Check if there are any attachments.
	 */
	bool HasAttachments() const { return Attachments.Num() > 0; }

	/**
	 * Check if there are any image attachments with base64 data.
	 * Used to determine if image capability should be enabled.
	 */
	bool HasImageAttachments() const;

	/**
	 * Serialize all attachments for inclusion in a prompt.
	 * Returns an array of JSON content blocks.
	 */
	TArray<TSharedPtr<FJsonValue>> SerializeForPrompt() const;

	/**
	 * Serialize all attachments as a markdown string for text-only APIs.
	 */
	FString SerializeAsMarkdown() const;

	/** Delegate broadcast when attachments are added or removed */
	FOnAttachmentsChanged OnAttachmentsChanged;

private:
	FACPAttachmentManager() = default;
	~FACPAttachmentManager() = default;

	// Non-copyable
	FACPAttachmentManager(const FACPAttachmentManager&) = delete;
	FACPAttachmentManager& operator=(const FACPAttachmentManager&) = delete;

	/** Current list of attachments */
	TArray<FACPContextAttachment> Attachments;

	/** Broadcast that attachments changed */
	void BroadcastChange();
};
