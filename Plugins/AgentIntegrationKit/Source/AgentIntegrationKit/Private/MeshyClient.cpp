// Copyright 2025 Betide Studio. All Rights Reserved.

#include "MeshyClient.h"
#include "AgentIntegrationKitModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/FileHelper.h"

FMeshyClient::FMeshyClient()
{
}

FMeshyClient::~FMeshyClient()
{
}

TSharedPtr<FJsonObject> FMeshyClient::MakeRequest(const FString& Verb, const FString& Url, const TSharedPtr<FJsonObject>& Body, FString& OutError)
{
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(Verb);
	Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetTimeout(60.0f);

	if (Body.IsValid())
	{
		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Body.ToSharedRef(), Writer);
		Request->SetContentAsString(BodyStr);

		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("[AIK] Meshy request to %s: %s"), *Url, *BodyStr);
	}

	// Synchronous HTTP — uses CompleteOnHttpThread internally so it never
	// deadlocks when called from the game thread.
	Request->ProcessRequestUntilComplete();

	FHttpResponsePtr Response = Request->GetResponse();
	if (Request->GetStatus() != EHttpRequestStatus::Succeeded || !Response.IsValid())
	{
		OutError = TEXT("Failed to connect to Meshy API");
		return nullptr;
	}

	int32 ResponseCode = Response->GetResponseCode();
	FString ResponseContent = Response->GetContentAsString();

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("[AIK] Meshy response (%d): %s"), ResponseCode, *ResponseContent.Left(500));

	TSharedPtr<FJsonObject> ResponseJson;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseContent);
	if (!FJsonSerializer::Deserialize(Reader, ResponseJson))
	{
		OutError = FString::Printf(TEXT("Failed to parse response: %s"), *ResponseContent.Left(500));
		return nullptr;
	}

	if (ResponseCode < 200 || ResponseCode >= 300)
	{
		FString Message;
		if (ResponseJson->TryGetStringField(TEXT("message"), Message))
		{
			OutError = FString::Printf(TEXT("API Error (%d): %s"), ResponseCode, *Message);
		}
		else
		{
			OutError = FString::Printf(TEXT("API Error (%d): %s"), ResponseCode, *ResponseContent.Left(500));
		}
		return nullptr;
	}

	return ResponseJson;
}

// ----------------------------------------------------------------------------
// Shared helper: adds common mesh generation fields to a request body
// ----------------------------------------------------------------------------

void FMeshyClient::AddCommonMeshFields(TSharedPtr<FJsonObject>& Body, const FString& AiModel, const FString& Topology,
	const FString& SymmetryMode, const FString& PoseMode, bool bShouldRemesh, int32 TargetPolycount)
{
	if (!AiModel.IsEmpty())
	{
		Body->SetStringField(TEXT("ai_model"), AiModel);
	}

	if (!Topology.IsEmpty())
	{
		Body->SetStringField(TEXT("topology"), Topology);
	}

	if (!SymmetryMode.IsEmpty())
	{
		Body->SetStringField(TEXT("symmetry_mode"), SymmetryMode);
	}

	if (!PoseMode.IsEmpty())
	{
		Body->SetStringField(TEXT("pose_mode"), PoseMode);
	}

	Body->SetBoolField(TEXT("should_remesh"), bShouldRemesh);

	if (bShouldRemesh && TargetPolycount > 0)
	{
		Body->SetNumberField(TEXT("target_polycount"), FMath::Clamp(TargetPolycount, 100, 300000));
	}
}

// ----------------------------------------------------------------------------
// Text-to-3D: Preview (stage 1)
// ----------------------------------------------------------------------------

FMeshyResult FMeshyClient::CreatePreviewTask(const FMeshyTextTo3DOptions& Options)
{
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("mode"), TEXT("preview"));
	Body->SetStringField(TEXT("prompt"), Options.Prompt.Left(600));

	if (!Options.NegativePrompt.IsEmpty())
	{
		Body->SetStringField(TEXT("negative_prompt"), Options.NegativePrompt);
	}

	if (!Options.ArtStyle.IsEmpty())
	{
		Body->SetStringField(TEXT("art_style"), Options.ArtStyle);
	}

	AddCommonMeshFields(Body, Options.AiModel, Options.Topology, Options.SymmetryMode,
		Options.PoseMode, Options.bShouldRemesh, Options.TargetPolycount);

	FString Error;
	TSharedPtr<FJsonObject> Response = MakeRequest(TEXT("POST"), BaseUrl + TEXT("/openapi/v2/text-to-3d"), Body, Error);

	if (!Response.IsValid())
	{
		return FMeshyResult::Fail(Error);
	}

	FString TaskId;
	if (!Response->TryGetStringField(TEXT("result"), TaskId) || TaskId.IsEmpty())
	{
		return FMeshyResult::Fail(TEXT("No task ID in response"));
	}

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("[AIK] Meshy preview task created: %s"), *TaskId);
	FMeshyResult Result = FMeshyResult::Pending(TaskId, TEXT("PENDING"));
	Result.JobType = EMeshyJobType::TextTo3D;
	return Result;
}

// ----------------------------------------------------------------------------
// Text-to-3D: Refine (stage 2)
// ----------------------------------------------------------------------------

FMeshyResult FMeshyClient::CreateRefineTask(const FMeshyRefineOptions& Options)
{
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("mode"), TEXT("refine"));
	Body->SetStringField(TEXT("preview_task_id"), Options.PreviewTaskId);
	Body->SetBoolField(TEXT("enable_pbr"), Options.bEnablePBR);

	if (!Options.AiModel.IsEmpty())
	{
		Body->SetStringField(TEXT("ai_model"), Options.AiModel);
	}

	if (!Options.TexturePrompt.IsEmpty())
	{
		Body->SetStringField(TEXT("texture_prompt"), Options.TexturePrompt.Left(600));
	}

	if (!Options.TextureImageUrl.IsEmpty())
	{
		Body->SetStringField(TEXT("texture_image_url"), Options.TextureImageUrl);
	}

	FString Error;
	TSharedPtr<FJsonObject> Response = MakeRequest(TEXT("POST"), BaseUrl + TEXT("/openapi/v2/text-to-3d"), Body, Error);

	if (!Response.IsValid())
	{
		return FMeshyResult::Fail(Error);
	}

	FString TaskId;
	if (!Response->TryGetStringField(TEXT("result"), TaskId) || TaskId.IsEmpty())
	{
		return FMeshyResult::Fail(TEXT("No task ID in response"));
	}

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("[AIK] Meshy refine task created: %s"), *TaskId);
	FMeshyResult Result = FMeshyResult::Pending(TaskId, TEXT("PENDING"));
	Result.JobType = EMeshyJobType::TextTo3D;
	return Result;
}

// ----------------------------------------------------------------------------
// Image-to-3D (single image)
// ----------------------------------------------------------------------------

FMeshyResult FMeshyClient::CreateImageTo3DTask(const FMeshyImageTo3DOptions& Options)
{
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("image_url"), Options.ImageUrl);

	if (!Options.ModelType.IsEmpty())
	{
		Body->SetStringField(TEXT("model_type"), Options.ModelType);
	}

	AddCommonMeshFields(Body, Options.AiModel, Options.Topology, Options.SymmetryMode,
		Options.PoseMode, Options.bShouldRemesh, Options.TargetPolycount);

	Body->SetBoolField(TEXT("should_texture"), Options.bShouldTexture);
	Body->SetBoolField(TEXT("enable_pbr"), Options.bEnablePBR);

	if (!Options.TexturePrompt.IsEmpty())
	{
		Body->SetStringField(TEXT("texture_prompt"), Options.TexturePrompt.Left(600));
	}

	if (!Options.TextureImageUrl.IsEmpty())
	{
		Body->SetStringField(TEXT("texture_image_url"), Options.TextureImageUrl);
	}

	FString Error;
	TSharedPtr<FJsonObject> Response = MakeRequest(TEXT("POST"), BaseUrl + TEXT("/openapi/v1/image-to-3d"), Body, Error);

	if (!Response.IsValid())
	{
		return FMeshyResult::Fail(Error);
	}

	FString TaskId;
	if (!Response->TryGetStringField(TEXT("result"), TaskId) || TaskId.IsEmpty())
	{
		return FMeshyResult::Fail(TEXT("No task ID in response"));
	}

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("[AIK] Meshy image-to-3d task created: %s"), *TaskId);
	FMeshyResult Result = FMeshyResult::Pending(TaskId, TEXT("PENDING"));
	Result.JobType = EMeshyJobType::ImageTo3D;
	return Result;
}

// ----------------------------------------------------------------------------
// Multi-image-to-3D (1-4 images from different angles)
// ----------------------------------------------------------------------------

FMeshyResult FMeshyClient::CreateMultiImageTo3DTask(const FMeshyMultiImageTo3DOptions& Options)
{
	if (Options.ImageUrls.Num() < 1 || Options.ImageUrls.Num() > 4)
	{
		return FMeshyResult::Fail(FString::Printf(
			TEXT("Multi-image-to-3D requires 1-4 images, got %d"), Options.ImageUrls.Num()));
	}

	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();

	// Build image_urls array
	TArray<TSharedPtr<FJsonValue>> ImageUrlsArray;
	for (const FString& Url : Options.ImageUrls)
	{
		ImageUrlsArray.Add(MakeShared<FJsonValueString>(Url));
	}
	Body->SetArrayField(TEXT("image_urls"), ImageUrlsArray);

	// Multi-image does NOT support model_type (per API docs)
	AddCommonMeshFields(Body, Options.AiModel, Options.Topology, Options.SymmetryMode,
		Options.PoseMode, Options.bShouldRemesh, Options.TargetPolycount);

	Body->SetBoolField(TEXT("should_texture"), Options.bShouldTexture);
	Body->SetBoolField(TEXT("enable_pbr"), Options.bEnablePBR);

	if (!Options.TexturePrompt.IsEmpty())
	{
		Body->SetStringField(TEXT("texture_prompt"), Options.TexturePrompt.Left(600));
	}

	if (!Options.TextureImageUrl.IsEmpty())
	{
		Body->SetStringField(TEXT("texture_image_url"), Options.TextureImageUrl);
	}

	FString Error;
	TSharedPtr<FJsonObject> Response = MakeRequest(TEXT("POST"), BaseUrl + TEXT("/openapi/v1/multi-image-to-3d"), Body, Error);

	if (!Response.IsValid())
	{
		return FMeshyResult::Fail(Error);
	}

	FString TaskId;
	if (!Response->TryGetStringField(TEXT("result"), TaskId) || TaskId.IsEmpty())
	{
		return FMeshyResult::Fail(TEXT("No task ID in response"));
	}

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("[AIK] Meshy multi-image-to-3d task created: %s"), *TaskId);
	FMeshyResult Result = FMeshyResult::Pending(TaskId, TEXT("PENDING"));
	Result.JobType = EMeshyJobType::MultiImageTo3D;
	return Result;
}

// ----------------------------------------------------------------------------
// Job status — routes to correct endpoint by job type
// ----------------------------------------------------------------------------

FMeshyResult FMeshyClient::GetJobStatus(const FString& JobId, EMeshyJobType JobType)
{
	FString Endpoint;
	switch (JobType)
	{
	case EMeshyJobType::TextTo3D:
		Endpoint = FString::Printf(TEXT("/openapi/v2/text-to-3d/%s"), *JobId);
		break;
	case EMeshyJobType::ImageTo3D:
		Endpoint = FString::Printf(TEXT("/openapi/v1/image-to-3d/%s"), *JobId);
		break;
	case EMeshyJobType::MultiImageTo3D:
		Endpoint = FString::Printf(TEXT("/openapi/v1/multi-image-to-3d/%s"), *JobId);
		break;
	}

	FString Error;
	TSharedPtr<FJsonObject> Response = MakeRequest(TEXT("GET"), BaseUrl + Endpoint, nullptr, Error);

	if (!Response.IsValid())
	{
		return FMeshyResult::Fail(Error);
	}

	FMeshyResult Result = ParseJobResponse(Response);
	Result.JobType = JobType;
	return Result;
}

// ----------------------------------------------------------------------------
// Balance API
// ----------------------------------------------------------------------------

FMeshyBalanceResult FMeshyClient::GetBalance()
{
	FString Error;
	TSharedPtr<FJsonObject> Response = MakeRequest(TEXT("GET"), BaseUrl + TEXT("/openapi/v1/balance"), nullptr, Error);

	FMeshyBalanceResult Result;
	if (!Response.IsValid())
	{
		Result.ErrorMessage = Error;
		return Result;
	}

	int32 Balance = 0;
	if (Response->TryGetNumberField(TEXT("balance"), Balance))
	{
		Result.bSuccess = true;
		Result.Balance = Balance;
	}
	else
	{
		Result.ErrorMessage = TEXT("Unexpected balance response format");
	}
	return Result;
}

// ----------------------------------------------------------------------------
// Parse job status response
// ----------------------------------------------------------------------------

FMeshyResult FMeshyClient::ParseJobResponse(const TSharedPtr<FJsonObject>& Response)
{
	FString Status;
	Response->TryGetStringField(TEXT("status"), Status);

	FString JobId;
	Response->TryGetStringField(TEXT("id"), JobId);

	int32 Progress = 0;
	Response->TryGetNumberField(TEXT("progress"), Progress);

	if (Status.Equals(TEXT("SUCCEEDED"), ESearchCase::IgnoreCase))
	{
		FString ModelUrl;
		FString ThumbnailUrl;

		const TSharedPtr<FJsonObject>* ModelUrlsObj;
		if (Response->TryGetObjectField(TEXT("model_urls"), ModelUrlsObj))
		{
			(*ModelUrlsObj)->TryGetStringField(TEXT("glb"), ModelUrl);
		}

		Response->TryGetStringField(TEXT("thumbnail_url"), ThumbnailUrl);

		if (ModelUrl.IsEmpty())
		{
			return FMeshyResult::Fail(TEXT("Job succeeded but no model URL found in response"));
		}

		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("[AIK] Meshy job %s succeeded. Model URL: %s"), *JobId, *ModelUrl.Left(100));
		return FMeshyResult::Success(ModelUrl, ThumbnailUrl, JobId);
	}
	else if (Status.Equals(TEXT("FAILED"), ESearchCase::IgnoreCase) || Status.Equals(TEXT("CANCELED"), ESearchCase::IgnoreCase))
	{
		FString ErrorMessage;
		const TSharedPtr<FJsonObject>* TaskErrorObj;
		if (Response->TryGetObjectField(TEXT("task_error"), TaskErrorObj))
		{
			(*TaskErrorObj)->TryGetStringField(TEXT("message"), ErrorMessage);
		}

		if (ErrorMessage.IsEmpty())
		{
			ErrorMessage = FString::Printf(TEXT("Job %s"), *Status.ToLower());
		}

		return FMeshyResult::Fail(ErrorMessage);
	}
	else
	{
		// PENDING or IN_PROGRESS
		return FMeshyResult::Pending(JobId, Status, Progress);
	}
}

// ----------------------------------------------------------------------------
// Poll until complete (used by blocking convenience methods)
// ----------------------------------------------------------------------------

FMeshyResult FMeshyClient::PollJobUntilComplete(const FString& JobId, EMeshyJobType JobType, int32 TimeoutSeconds)
{
	double StartTime = FPlatformTime::Seconds();
	double EndTime = StartTime + TimeoutSeconds;

	int32 PollCount = 0;
	const float PollIntervalSeconds = 5.0f;

	while (FPlatformTime::Seconds() < EndTime)
	{
		FMeshyResult Status = GetJobStatus(JobId, JobType);

		if (Status.bSuccess)
		{
			return Status;
		}

		if (Status.Status.Equals(TEXT("FAILED"), ESearchCase::IgnoreCase) ||
			Status.Status.Equals(TEXT("CANCELED"), ESearchCase::IgnoreCase))
		{
			return Status;
		}

		PollCount++;
		int32 ElapsedSeconds = FMath::RoundToInt(FPlatformTime::Seconds() - StartTime);
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("[AIK] Meshy job %s: %s (poll %d, %ds elapsed)"),
			*JobId, *Status.Status, PollCount, ElapsedSeconds);

		FPlatformProcess::Sleep(PollIntervalSeconds);
	}

	return FMeshyResult::Fail(FString::Printf(TEXT("Job timed out after %d seconds. Job ID: %s"), TimeoutSeconds, *JobId));
}

// ----------------------------------------------------------------------------
// Model download
// ----------------------------------------------------------------------------

bool FMeshyClient::DownloadModel(const FString& Url, const FString& OutputPath, FString& OutError)
{
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(TEXT("GET"));
	Request->SetTimeout(300.0f);

	// Synchronous HTTP — safe from game thread
	Request->ProcessRequestUntilComplete();

	FHttpResponsePtr Response = Request->GetResponse();
	if (Request->GetStatus() != EHttpRequestStatus::Succeeded || !Response.IsValid())
	{
		OutError = TEXT("Failed to download model file");
		return false;
	}

	int32 ResponseCode = Response->GetResponseCode();
	if (ResponseCode < 200 || ResponseCode >= 300)
	{
		OutError = FString::Printf(TEXT("HTTP %d"), ResponseCode);
		return false;
	}

	const TArray<uint8>& ResponseData = Response->GetContent();
	if (ResponseData.Num() == 0)
	{
		OutError = TEXT("Downloaded file is empty");
		return false;
	}

	if (!FFileHelper::SaveArrayToFile(ResponseData, *OutputPath))
	{
		OutError = FString::Printf(TEXT("Failed to write file: %s"), *OutputPath);
		return false;
	}

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("[AIK] Downloaded model: %s (%d bytes)"), *OutputPath, ResponseData.Num());
	return true;
}
