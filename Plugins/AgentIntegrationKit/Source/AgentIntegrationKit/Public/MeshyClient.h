// Copyright 2025 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Which endpoint created a Meshy job (determines polling URL).
 */
enum class EMeshyJobType : uint8
{
	TextTo3D,
	ImageTo3D,
	MultiImageTo3D
};

/**
 * Result from a Meshy generation job.
 */
struct AGENTINTEGRATIONKIT_API FMeshyResult
{
	bool bSuccess = false;
	FString ErrorMessage;
	FString ModelUrl;       // URL to download GLB/FBX model
	FString ThumbnailUrl;   // Preview image URL
	FString JobId;
	FString Status;         // PENDING, IN_PROGRESS, SUCCEEDED, FAILED, CANCELED
	int32 Progress = 0;     // 0-100 completion percentage
	EMeshyJobType JobType = EMeshyJobType::TextTo3D;

	static FMeshyResult Success(const FString& InModelUrl, const FString& InThumbnailUrl, const FString& InJobId)
	{
		FMeshyResult Result;
		Result.bSuccess = true;
		Result.ModelUrl = InModelUrl;
		Result.ThumbnailUrl = InThumbnailUrl;
		Result.JobId = InJobId;
		Result.Status = TEXT("SUCCEEDED");
		return Result;
	}

	static FMeshyResult Fail(const FString& Error)
	{
		FMeshyResult Result;
		Result.bSuccess = false;
		Result.ErrorMessage = Error;
		Result.Status = TEXT("FAILED");
		return Result;
	}

	static FMeshyResult Pending(const FString& InJobId, const FString& InStatus, int32 InProgress = 0)
	{
		FMeshyResult Result;
		Result.bSuccess = false;
		Result.JobId = InJobId;
		Result.Status = InStatus;
		Result.Progress = InProgress;
		return Result;
	}
};

/**
 * Result from a Meshy balance query.
 */
struct AGENTINTEGRATIONKIT_API FMeshyBalanceResult
{
	bool bSuccess = false;
	FString ErrorMessage;
	int32 Balance = 0; // Credits remaining
};

/**
 * Text-to-3D preview stage options.
 */
struct AGENTINTEGRATIONKIT_API FMeshyTextTo3DOptions
{
	FString Prompt;
	FString NegativePrompt = TEXT("low quality, low resolution, low poly, ugly");
	FString ArtStyle = TEXT("realistic");  // realistic, sculpture
	FString AiModel = TEXT("latest");      // meshy-5, meshy-6, latest (=Meshy 6)
	FString Topology = TEXT("triangle");   // quad, triangle
	FString SymmetryMode;                  // off, auto, on (empty = API default "auto")
	FString PoseMode;                      // a-pose, t-pose, "" (empty = none)
	bool bShouldRemesh = true;
	int32 TargetPolycount = 30000;         // 100-300000
};

/**
 * Text-to-3D refine stage options.
 */
struct AGENTINTEGRATIONKIT_API FMeshyRefineOptions
{
	FString PreviewTaskId;
	bool bEnablePBR = true;
	FString AiModel = TEXT("latest");
	FString TexturePrompt;       // Max 600 chars, optional texture guidance
	FString TextureImageUrl;     // Optional reference image for texture
};

/**
 * Image-to-3D options (single image).
 */
struct AGENTINTEGRATIONKIT_API FMeshyImageTo3DOptions
{
	FString ImageUrl;
	FString ModelType;                     // standard, lowpoly (empty = API default "standard")
	FString AiModel = TEXT("latest");
	FString Topology = TEXT("triangle");
	FString SymmetryMode;
	FString PoseMode;
	bool bShouldRemesh = true;
	bool bShouldTexture = true;
	bool bEnablePBR = true;
	int32 TargetPolycount = 30000;
	FString TexturePrompt;
	FString TextureImageUrl;
};

/**
 * Multi-image-to-3D options (1-4 images from different angles).
 */
struct AGENTINTEGRATIONKIT_API FMeshyMultiImageTo3DOptions
{
	TArray<FString> ImageUrls;             // 1-4 image URLs
	FString AiModel = TEXT("latest");
	FString Topology = TEXT("triangle");
	FString SymmetryMode;
	FString PoseMode;
	bool bShouldRemesh = true;
	bool bShouldTexture = true;
	bool bEnablePBR = true;
	int32 TargetPolycount = 30000;
	FString TexturePrompt;
	FString TextureImageUrl;
};

/**
 * HTTP client for Meshy.ai API.
 * Handles text-to-3D, image-to-3D, and multi-image-to-3D generation with job polling.
 *
 * Text-to-3D uses a two-stage workflow:
 * 1. Preview stage: Generates base mesh (20 credits Meshy-6, 5 credits others)
 * 2. Refine stage: Applies textures (10 credits)
 *
 * Image/multi-image-to-3D is single-stage (20 credits Meshy-6).
 */
class AGENTINTEGRATIONKIT_API FMeshyClient
{
public:
	FMeshyClient();
	~FMeshyClient();

	void SetApiKey(const FString& InApiKey) { ApiKey = InApiKey; }
	void SetBaseUrl(const FString& InBaseUrl) { BaseUrl = InBaseUrl; }
	bool IsConfigured() const { return !ApiKey.IsEmpty(); }

	// ── Task creation (non-blocking, returns job ID) ──

	/** Create a preview task (stage 1 of text-to-3D). */
	FMeshyResult CreatePreviewTask(const FMeshyTextTo3DOptions& Options);

	/** Create a refine task (stage 2 of text-to-3D). */
	FMeshyResult CreateRefineTask(const FMeshyRefineOptions& Options);

	/** Create a single-image-to-3D task. */
	FMeshyResult CreateImageTo3DTask(const FMeshyImageTo3DOptions& Options);

	/** Create a multi-image-to-3D task (1-4 images from different angles). */
	FMeshyResult CreateMultiImageTo3DTask(const FMeshyMultiImageTo3DOptions& Options);

	// ── Job status & download ──

	/** Check the status of a job. Uses JobType to hit the correct endpoint. */
	FMeshyResult GetJobStatus(const FString& JobId, EMeshyJobType JobType = EMeshyJobType::TextTo3D);

	/** Download a model file from URL. */
	bool DownloadModel(const FString& Url, const FString& OutputPath, FString& OutError);

	// ── Balance ──

	/** Get current credit balance. */
	FMeshyBalanceResult GetBalance();

private:
	FMeshyResult PollJobUntilComplete(const FString& JobId, EMeshyJobType JobType, int32 TimeoutSeconds);
	FMeshyResult ParseJobResponse(const TSharedPtr<FJsonObject>& Response);
	TSharedPtr<FJsonObject> MakeRequest(const FString& Verb, const FString& Url, const TSharedPtr<FJsonObject>& Body, FString& OutError);

	/** Build common mesh generation fields into a JSON body. */
	static void AddCommonMeshFields(TSharedPtr<FJsonObject>& Body, const FString& AiModel, const FString& Topology,
		const FString& SymmetryMode, const FString& PoseMode, bool bShouldRemesh, int32 TargetPolycount);

	FString ApiKey;
	FString BaseUrl = TEXT("https://api.meshy.ai");
};
