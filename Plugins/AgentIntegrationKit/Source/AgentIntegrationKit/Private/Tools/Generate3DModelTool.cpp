// Copyright 2025 Betide Studio. All Rights Reserved.

#include "Tools/Generate3DModelTool.h"
#include "AgentIntegrationKitModule.h"
#include "Tools/AssetImportUtils.h"
#include "MeshyClient.h"
#include "ACPSettings.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Async/Async.h"
#include "HttpModule.h"
#include "HttpManager.h"
#include "Misc/Base64.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "GenericPlatform/GenericPlatformHttp.h"

// ── Validation helpers ──

namespace MeshyValidation
{
	static FString ValidateAiModel(const FString& Input)
	{
		static const TSet<FString> Valid = { TEXT("meshy-5"), TEXT("meshy-6"), TEXT("latest") };
		return Valid.Contains(Input) ? Input : TEXT("latest");
	}

	static FString ValidateTopology(const FString& Input)
	{
		static const TSet<FString> Valid = { TEXT("quad"), TEXT("triangle") };
		return Valid.Contains(Input) ? Input : TEXT("triangle");
	}

	static FString ValidateSymmetryMode(const FString& Input)
	{
		static const TSet<FString> Valid = { TEXT("off"), TEXT("auto"), TEXT("on") };
		return Valid.Contains(Input) ? Input : FString();
	}

	static FString ValidatePoseMode(const FString& Input)
	{
		static const TSet<FString> Valid = { TEXT("a-pose"), TEXT("t-pose") };
		return Valid.Contains(Input) ? Input : FString();
	}

	static FString ValidateModelType(const FString& Input)
	{
		static const TSet<FString> Valid = { TEXT("standard"), TEXT("lowpoly") };
		return Valid.Contains(Input) ? Input : FString();
	}

	static FString ValidateJobType(const FString& Input)
	{
		static const TSet<FString> Valid = { TEXT("text_to_3d"), TEXT("image_to_3d"), TEXT("multi_image_to_3d") };
		return Valid.Contains(Input) ? Input : TEXT("text_to_3d");
	}

	static EMeshyJobType JobTypeFromString(const FString& Input)
	{
		if (Input == TEXT("image_to_3d")) return EMeshyJobType::ImageTo3D;
		if (Input == TEXT("multi_image_to_3d")) return EMeshyJobType::MultiImageTo3D;
		return EMeshyJobType::TextTo3D;
	}

	static FString JobTypeToString(EMeshyJobType Type)
	{
		switch (Type)
		{
		case EMeshyJobType::ImageTo3D: return TEXT("image_to_3d");
		case EMeshyJobType::MultiImageTo3D: return TEXT("multi_image_to_3d");
		default: return TEXT("text_to_3d");
		}
	}
}

namespace
{
	FString BuildFalHttpErrorMessage(int32 ResponseCode, const FString& Body, bool bUsingNeoStackCredits)
	{
		if (bUsingNeoStackCredits &&
			ResponseCode == 402 &&
			Body.Contains(TEXT("insufficient credits"), ESearchCase::IgnoreCase))
		{
			return FString::Printf(
				TEXT("fal API error %d: Insufficient NeoStack credits. Top up at https://betide.studio/dashboard/neostack"),
				ResponseCode);
		}

		return FString::Printf(TEXT("fal API error %d: %s"), ResponseCode, *Body.Left(800));
	}

	FString GetFalStatusFromResponse(const TSharedPtr<FJsonObject>& Obj)
	{
		if (!Obj.IsValid())
		{
			return FString();
		}

		FString Status;
		Obj->TryGetStringField(TEXT("status"), Status);
		if (Status.IsEmpty())
		{
			Obj->TryGetStringField(TEXT("state"), Status);
		}
		return Status.ToUpper();
	}

	bool IsFalTerminalSuccess(const FString& Status)
	{
		return Status == TEXT("COMPLETED") || Status == TEXT("SUCCEEDED") || Status == TEXT("SUCCESS");
	}

	bool IsFalTerminalFailure(const FString& Status)
	{
		return Status == TEXT("FAILED") || Status == TEXT("ERROR") || Status == TEXT("CANCELLED") || Status == TEXT("CANCELED");
	}

	bool IsFalInProgressStatus(const FString& Status)
	{
		return Status.IsEmpty() ||
			Status == TEXT("IN_PROGRESS") ||
			Status == TEXT("INPROGRESS") ||
			Status == TEXT("PENDING") ||
			Status == TEXT("QUEUED") ||
			Status == TEXT("RUNNING") ||
			Status == TEXT("PROCESSING");
	}

	bool IsLikelyModelUrl(const FString& Value)
	{
		if (Value.IsEmpty())
		{
			return false;
		}

		const FString Lower = Value.ToLower();
		if (!(Lower.StartsWith(TEXT("https://")) || Lower.StartsWith(TEXT("http://"))))
		{
			return false;
		}

		return Lower.Contains(TEXT(".glb")) ||
			Lower.Contains(TEXT(".gltf")) ||
			Lower.Contains(TEXT(".fbx")) ||
			Lower.Contains(TEXT(".obj")) ||
			Lower.Contains(TEXT(".usdz")) ||
			Lower.Contains(TEXT(".zip"));
	}

	bool FindModelUrlInValue(const TSharedPtr<FJsonValue>& Value, FString& OutUrl);

	bool FindModelUrlInObject(const TSharedPtr<FJsonObject>& Obj, FString& OutUrl)
	{
		if (!Obj.IsValid())
		{
			return false;
		}

		static const TCHAR* PreferredKeys[] = {
			TEXT("model_url"), TEXT("mesh_url"), TEXT("glb_url"), TEXT("model"), TEXT("mesh"), TEXT("url")
		};

		for (const TCHAR* Key : PreferredKeys)
		{
			FString Candidate;
			if (Obj->TryGetStringField(Key, Candidate) && IsLikelyModelUrl(Candidate))
			{
				OutUrl = Candidate;
				return true;
			}
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Obj->Values)
		{
			if (FindModelUrlInValue(Pair.Value, OutUrl))
			{
				return true;
			}
		}

		return false;
	}

	bool FindModelUrlInValue(const TSharedPtr<FJsonValue>& Value, FString& OutUrl)
	{
		if (!Value.IsValid())
		{
			return false;
		}

		if (Value->Type == EJson::String)
		{
			const FString Candidate = Value->AsString();
			if (IsLikelyModelUrl(Candidate))
			{
				OutUrl = Candidate;
				return true;
			}
			return false;
		}

		if (Value->Type == EJson::Object)
		{
			return FindModelUrlInObject(Value->AsObject(), OutUrl);
		}

		if (Value->Type == EJson::Array)
		{
			const TArray<TSharedPtr<FJsonValue>>& ArrayVals = Value->AsArray();
			for (const TSharedPtr<FJsonValue>& Item : ArrayVals)
			{
				if (FindModelUrlInValue(Item, OutUrl))
				{
					return true;
				}
			}
		}

		return false;
	}
}

// ── Description ──

FString FGenerate3DModelTool::GetDescription() const
{
	return TEXT(
		"Generate 3D models using Meshy AI or fal.ai (including Hunyuan3D endpoints).\n"
		"Use provider='meshy' (default) or provider='fal'.\n"
		"\n"
		"TEXT-TO-3D WORKFLOW (5 calls, two-stage: preview then refine):\n"
		"1. action='create' + prompt -> returns preview job_id + job_type\n"
		"2. action='check' + job_id + job_type -> waits until preview completes, returns THUMBNAIL\n"
		"3. action='create' + preview_task_id=<job_id> -> starts refine (texturing, costs 10 extra credits)\n"
		"4. action='check' + job_id=<refine_id> + job_type='text_to_3d' -> waits for refine\n"
		"5. action='import' + job_id=<refine_id> + job_type='text_to_3d' -> imports StaticMesh\n"
		"\n"
		"IMAGE-TO-3D WORKFLOW (3 calls, single stage):\n"
		"1. action='create' + source_image=<url> -> returns job_id + job_type='image_to_3d'\n"
		"2. action='check' + job_id + job_type='image_to_3d' -> waits until complete\n"
		"3. action='import' + job_id + job_type='image_to_3d' -> imports StaticMesh\n"
		"\n"
		"MULTI-IMAGE-TO-3D WORKFLOW (3 calls, 1-4 images from different angles):\n"
		"1. action='create' + source_images=[url1,url2,...] -> returns job_id + job_type='multi_image_to_3d'\n"
		"2. action='check' + job_id + job_type='multi_image_to_3d' -> waits until complete\n"
		"3. action='import' + job_id + job_type='multi_image_to_3d' -> imports StaticMesh\n"
		"\n"
		"PREVIEW APPROVAL: When text-to-3D preview completes, a thumbnail is shown. Ask the user before "
		"proceeding to refine (costs credits). Skip only if user said to auto-approve.\n"
		"\n"
		"REFINE CUSTOMIZATION: During refine, you can provide texture_prompt and/or texture_image_url "
		"to guide how the mesh is textured.\n"
		"\n"
		"MODELS: ai_model='latest' (Meshy 6, default), 'meshy-6', 'meshy-5'. "
		"Latest is recommended for best quality.\n"
		"\n"
		"CREDITS: Preview costs 20 (Meshy-6) or 5 (Meshy-5). Refine costs 10. "
		"Image/multi-image costs 20 (Meshy-6).\n"
		"\n"
		"IMPORTANT: Always pass job_type from the create response to check/import calls.\n"
		"\n"
		"FAL WORKFLOW (provider='fal'):\n"
		"1. action='create' + fal_endpoint_id + fal_input (or fal_input_json/prompt/source_image helpers)\n"
		"2. action='check' + status_url from create response\n"
		"3. action='import' + response_url from create response\n"
		"\n"
		"FAL AUTH:\n"
		"- NeoStack credits mode: routed through betide proxy with aik_ token billing\n"
		"- BYOK mode: calls fal queue directly using your fal.ai API key."
	);
}

// ── Schema ──

TSharedPtr<FJsonObject> FGenerate3DModelTool::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// Helper lambdas
	auto AddStringProp = [&](const FString& Name, const FString& Desc)
	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("string"));
		Prop->SetStringField(TEXT("description"), Desc);
		Properties->SetObjectField(Name, Prop);
	};

	auto AddBoolProp = [&](const FString& Name, const FString& Desc)
	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("boolean"));
		Prop->SetStringField(TEXT("description"), Desc);
		Properties->SetObjectField(Name, Prop);
	};

	auto AddIntProp = [&](const FString& Name, const FString& Desc)
	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("integer"));
		Prop->SetStringField(TEXT("description"), Desc);
		Properties->SetObjectField(Name, Prop);
	};

	auto AddObjectProp = [&](const FString& Name, const FString& Desc)
	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("object"));
		Prop->SetStringField(TEXT("description"), Desc);
		Properties->SetObjectField(Name, Prop);
	};

	// action (enum)
	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("string"));
		Prop->SetStringField(TEXT("description"),
			TEXT("'create' starts a job, 'check' polls status, 'import' downloads and imports as StaticMesh."));
		TArray<TSharedPtr<FJsonValue>> Enum;
		Enum.Add(MakeShared<FJsonValueString>(TEXT("create")));
		Enum.Add(MakeShared<FJsonValueString>(TEXT("check")));
		Enum.Add(MakeShared<FJsonValueString>(TEXT("import")));
		Prop->SetArrayField(TEXT("enum"), Enum);
		Properties->SetObjectField(TEXT("action"), Prop);
	}

	// provider (enum)
	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("string"));
		Prop->SetStringField(TEXT("description"),
			TEXT("3D provider: 'meshy' (default) or 'fal'."));
		TArray<TSharedPtr<FJsonValue>> Enum;
		Enum.Add(MakeShared<FJsonValueString>(TEXT("meshy")));
		Enum.Add(MakeShared<FJsonValueString>(TEXT("fal")));
		Prop->SetArrayField(TEXT("enum"), Enum);
		Properties->SetObjectField(TEXT("provider"), Prop);
	}

	AddStringProp(TEXT("job_id"),
		TEXT("Job ID from a previous 'create' call. Required for 'check' and 'import'."));

	AddStringProp(TEXT("job_type"),
		TEXT("Job type for status polling (returned by create). Options: text_to_3d, image_to_3d, multi_image_to_3d. Always pass this from the create response."));

	AddStringProp(TEXT("prompt"),
		TEXT("Text description of the 3D model (max 600 chars). Required for text-to-3D create."));

	AddStringProp(TEXT("source_image"),
		TEXT("Single image URL or base64 data URL for image-to-3D."));

	// source_images (array)
	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("array"));
		Prop->SetStringField(TEXT("description"),
			TEXT("Array of 1-4 image URLs for multi-image-to-3D. Images should show the same object from different angles."));
		TSharedPtr<FJsonObject> Items = MakeShared<FJsonObject>();
		Items->SetStringField(TEXT("type"), TEXT("string"));
		Prop->SetObjectField(TEXT("items"), Items);
		Properties->SetObjectField(TEXT("source_images"), Prop);
	}

	AddStringProp(TEXT("preview_task_id"),
		TEXT("Completed preview task ID to start refine stage (text-to-3D stage 2)."));

	AddStringProp(TEXT("ai_model"),
		TEXT("AI model: 'latest' (Meshy 6, default, best quality), 'meshy-6', 'meshy-5'. Applies to all workflows."));

	AddStringProp(TEXT("art_style"),
		TEXT("Art style for text-to-3D preview: 'realistic' (default), 'sculpture'."));

	AddStringProp(TEXT("negative_prompt"),
		TEXT("What to avoid in text-to-3D. Default: 'low quality, low resolution, low poly, ugly'."));

	AddStringProp(TEXT("topology"),
		TEXT("Mesh topology: 'triangle' (default, decimated) or 'quad' (quad-dominant)."));

	AddStringProp(TEXT("symmetry_mode"),
		TEXT("Symmetry enforcement: 'auto' (default), 'on', 'off'."));

	AddStringProp(TEXT("pose_mode"),
		TEXT("Character pose: 'a-pose', 't-pose', or empty (no pose). Only for character models."));

	AddStringProp(TEXT("model_type"),
		TEXT("Image-to-3D only: 'standard' (default) or 'lowpoly'. Not available for multi-image."));

	AddStringProp(TEXT("texture_prompt"),
		TEXT("Texture guidance for refine stage (max 600 chars). E.g. 'wooden texture with brass fittings'."));

	AddStringProp(TEXT("texture_image_url"),
		TEXT("Reference image URL for texture during refine. Cannot use with texture_prompt simultaneously."));

	AddBoolProp(TEXT("enable_pbr"),
		TEXT("Generate PBR maps (metallic, roughness, normal). Default: true."));

	AddBoolProp(TEXT("should_remesh"),
		TEXT("Enable remeshing. Default: true. When false, topology and target_polycount are ignored."));

	AddBoolProp(TEXT("should_texture"),
		TEXT("Enable texturing for image/multi-image-to-3D. Default: true."));

	AddIntProp(TEXT("target_polycount"),
		TEXT("Target polygon count (100-300000). Default: 30000. Only used when should_remesh=true."));

	AddStringProp(TEXT("asset_path"),
		TEXT("UE asset path for import destination. Default: /Game/Generated3DModels."));

	AddStringProp(TEXT("asset_name"),
		TEXT("Name for the imported StaticMesh. Auto-generated if not provided."));

	AddBoolProp(TEXT("wait"),
		TEXT("For action='check': poll internally until done (default: true). Set false for one-shot status check."));

	AddIntProp(TEXT("timeout"),
		TEXT("Max seconds to wait when wait=true. Default: 300 (5 min). Only for action='check'."));

	AddIntProp(TEXT("poll_interval_seconds"),
		TEXT("For action='check' with wait=true: polling interval in seconds. Default: 15. Range: 5-60."));

	// fal-specific
	AddStringProp(TEXT("fal_endpoint_id"),
		TEXT("fal endpoint ID for create (example: fal-ai/hunyuan3d-v3/image-to-3d). Required for provider='fal', action='create'."));
	AddObjectProp(TEXT("fal_input"),
		TEXT("Object payload sent to fal create request body. Preferred over fal_input_json when available."));
	AddStringProp(TEXT("fal_input_json"),
		TEXT("JSON object string sent to fal create request body. Backward-compatible fallback if fal_input object is not used."));
	AddStringProp(TEXT("status_url"),
		TEXT("fal status URL returned by create. Required for provider='fal', action='check'."));
	AddStringProp(TEXT("response_url"),
		TEXT("fal response URL returned by create. Required for provider='fal', action='import'."));
	AddStringProp(TEXT("cancel_url"),
		TEXT("fal cancel URL returned by create. Optional for provider='fal'."));

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("action")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

// ── Execute dispatcher ──

FToolResult FGenerate3DModelTool::Execute(const TSharedPtr<FJsonObject>& Args)
{
	if (!Args.IsValid())
	{
		return FToolResult::Fail(TEXT("Invalid arguments"));
	}

	UACPSettings* Settings = UACPSettings::Get();
	if (!Settings)
	{
		return FToolResult::Fail(TEXT("Failed to load plugin settings"));
	}

	FString Action;
	Args->TryGetStringField(TEXT("action"), Action);
	if (Action.IsEmpty())
	{
		Action = TEXT("create");
	}

	FString Provider = TEXT("meshy");
	Args->TryGetStringField(TEXT("provider"), Provider);
	Provider = Provider.ToLower();

	if (Provider == TEXT("fal"))
	{
		if (!Settings->HasFalAuth())
		{
			return FToolResult::Fail(Settings->ShouldUseBetideCredits()
				? TEXT("Betide API token not configured. Set it in Project Settings > Plugins > Agent Integration Kit")
				: TEXT("fal.ai API key not configured. Set it in Project Settings > Plugins > Agent Integration Kit"));
		}
		return ExecuteFal(Args, Action, Settings);
	}

	const FString MeshyToken = Settings->GetMeshyAuthToken();
	if (MeshyToken.IsEmpty())
	{
		return FToolResult::Fail(Settings->ShouldUseBetideCredits()
			? TEXT("Betide API token not configured. Set it in Project Settings > Plugins > Agent Integration Kit")
			: TEXT("Meshy API key not configured. Set it in Project Settings > Plugins > Agent Integration Kit"));
	}

	FMeshyClient Client;
	Client.SetApiKey(MeshyToken);
	Client.SetBaseUrl(Settings->GetMeshyBaseUrl());

	if (Action == TEXT("create"))
	{
		return ExecuteCreate(Args, Client);
	}
	else if (Action == TEXT("check"))
	{
		return ExecuteCheck(Args, Client);
	}
	else if (Action == TEXT("import"))
	{
		return ExecuteImport(Args, Client);
	}

	return FToolResult::Fail(FString::Printf(
		TEXT("Unknown action '%s'. Valid actions: create, check, import"), *Action));
}

// ── Helpers to extract common mesh params from Args ──

namespace
{
	FString GetValidatedString(const TSharedPtr<FJsonObject>& Args, const FString& Key,
		FString (*Validator)(const FString&), const FString& Default = FString())
	{
		FString Value;
		if (Args->TryGetStringField(Key, Value) && !Value.IsEmpty())
		{
			return Validator(Value);
		}
		return Default;
	}

	void PopulateCommonMeshParams(const TSharedPtr<FJsonObject>& Args,
		FString& OutAiModel, FString& OutTopology, FString& OutSymmetryMode, FString& OutPoseMode,
		bool& OutShouldRemesh, int32& OutTargetPolycount)
	{
		OutAiModel = GetValidatedString(Args, TEXT("ai_model"), MeshyValidation::ValidateAiModel, TEXT("latest"));
		OutTopology = GetValidatedString(Args, TEXT("topology"), MeshyValidation::ValidateTopology, TEXT("triangle"));
		OutSymmetryMode = GetValidatedString(Args, TEXT("symmetry_mode"), MeshyValidation::ValidateSymmetryMode);
		OutPoseMode = GetValidatedString(Args, TEXT("pose_mode"), MeshyValidation::ValidatePoseMode);

		OutShouldRemesh = true;
		bool bRemeshParam;
		if (Args->TryGetBoolField(TEXT("should_remesh"), bRemeshParam))
		{
			OutShouldRemesh = bRemeshParam;
		}

		OutTargetPolycount = 30000;
		int32 PolycountParam;
		if (Args->TryGetNumberField(TEXT("target_polycount"), PolycountParam))
		{
			OutTargetPolycount = FMath::Clamp(PolycountParam, 100, 300000);
		}
	}
}

// ── create ──

FToolResult FGenerate3DModelTool::ExecuteCreate(const TSharedPtr<FJsonObject>& Args, FMeshyClient& Client)
{
	FString PreviewTaskId, SourceImage, Prompt;
	Args->TryGetStringField(TEXT("preview_task_id"), PreviewTaskId);
	Args->TryGetStringField(TEXT("source_image"), SourceImage);
	Args->TryGetStringField(TEXT("prompt"), Prompt);

	// Check for multi-image array
	const TArray<TSharedPtr<FJsonValue>>* SourceImagesArray = nullptr;
	Args->TryGetArrayField(TEXT("source_images"), SourceImagesArray);

	// Priority: preview_task_id (refine) > source_images (multi-image) > source_image (single image) > prompt (preview)

	if (!PreviewTaskId.IsEmpty())
	{
		// ── REFINE STAGE ──
		FMeshyRefineOptions Options;
		Options.PreviewTaskId = PreviewTaskId;

		bool bEnablePBRParam;
		if (Args->TryGetBoolField(TEXT("enable_pbr"), bEnablePBRParam))
		{
			Options.bEnablePBR = bEnablePBRParam;
		}

		FString AiModel;
		if (Args->TryGetStringField(TEXT("ai_model"), AiModel) && !AiModel.IsEmpty())
		{
			Options.AiModel = MeshyValidation::ValidateAiModel(AiModel);
		}

		FString TexturePrompt;
		if (Args->TryGetStringField(TEXT("texture_prompt"), TexturePrompt) && !TexturePrompt.IsEmpty())
		{
			Options.TexturePrompt = TexturePrompt.Left(600);
		}

		FString TextureImageUrl;
		if (Args->TryGetStringField(TEXT("texture_image_url"), TextureImageUrl) && !TextureImageUrl.IsEmpty())
		{
			Options.TextureImageUrl = TextureImageUrl;
		}

		FMeshyResult Result = Client.CreateRefineTask(Options);
		if (Result.Status == TEXT("FAILED"))
		{
			return FToolResult::Fail(FString::Printf(TEXT("Failed to create refine task: %s"), *Result.ErrorMessage));
		}

		return FToolResult::Ok(FString::Printf(
			TEXT("Refine task created (costs 10 credits for texturing).\n"
				 "job_id: %s\n"
				 "job_type: text_to_3d\n"
				 "stage: refine (stage 2 of text-to-3D)\n"
				 "preview_task_id: %s\n\n"
				 "NEXT: Call with action='check', job_id='%s', job_type='text_to_3d'."),
			*Result.JobId, *PreviewTaskId, *Result.JobId));
	}
	else if (SourceImagesArray && SourceImagesArray->Num() > 0)
	{
		// ── MULTI-IMAGE-TO-3D ──
		if (SourceImagesArray->Num() > 4)
		{
			return FToolResult::Fail(TEXT("Multi-image-to-3D supports 1-4 images, got more than 4."));
		}

		FMeshyMultiImageTo3DOptions Options;
		for (const auto& Val : *SourceImagesArray)
		{
			FString Url = Val->AsString();
			if (Url.IsEmpty())
			{
				return FToolResult::Fail(TEXT("source_images contains an empty URL."));
			}
			if (Url.StartsWith(TEXT("/Game/")) || Url.StartsWith(TEXT("/Engine/")))
			{
				return FToolResult::Fail(TEXT("source_images must be web URLs or base64 data URLs, not UE asset paths."));
			}
			Options.ImageUrls.Add(Url);
		}

		PopulateCommonMeshParams(Args, Options.AiModel, Options.Topology, Options.SymmetryMode,
			Options.PoseMode, Options.bShouldRemesh, Options.TargetPolycount);

		bool bShouldTexture;
		if (Args->TryGetBoolField(TEXT("should_texture"), bShouldTexture))
		{
			Options.bShouldTexture = bShouldTexture;
		}

		bool bEnablePBR;
		if (Args->TryGetBoolField(TEXT("enable_pbr"), bEnablePBR))
		{
			Options.bEnablePBR = bEnablePBR;
		}

		FString TexturePrompt;
		if (Args->TryGetStringField(TEXT("texture_prompt"), TexturePrompt) && !TexturePrompt.IsEmpty())
		{
			Options.TexturePrompt = TexturePrompt.Left(600);
		}

		FString TextureImageUrl;
		if (Args->TryGetStringField(TEXT("texture_image_url"), TextureImageUrl) && !TextureImageUrl.IsEmpty())
		{
			Options.TextureImageUrl = TextureImageUrl;
		}

		FMeshyResult Result = Client.CreateMultiImageTo3DTask(Options);
		if (Result.Status == TEXT("FAILED"))
		{
			return FToolResult::Fail(FString::Printf(TEXT("Failed to create multi-image-to-3D task: %s"), *Result.ErrorMessage));
		}

		return FToolResult::Ok(FString::Printf(
			TEXT("Multi-image-to-3D task created (%d images, costs 20 credits with Meshy 6).\n"
				 "job_id: %s\n"
				 "job_type: multi_image_to_3d\n\n"
				 "NEXT: Call with action='check', job_id='%s', job_type='multi_image_to_3d'."),
			Options.ImageUrls.Num(), *Result.JobId, *Result.JobId));
	}
	else if (!SourceImage.IsEmpty())
	{
		// ── SINGLE IMAGE-TO-3D ──
		if (SourceImage.StartsWith(TEXT("/Game/")) || SourceImage.StartsWith(TEXT("/Engine/")))
		{
			return FToolResult::Fail(TEXT("Image-to-3D requires a web URL or base64 data URL. UE asset paths are not supported."));
		}

		FMeshyImageTo3DOptions Options;
		Options.ImageUrl = SourceImage;

		PopulateCommonMeshParams(Args, Options.AiModel, Options.Topology, Options.SymmetryMode,
			Options.PoseMode, Options.bShouldRemesh, Options.TargetPolycount);

		FString ModelType;
		if (Args->TryGetStringField(TEXT("model_type"), ModelType) && !ModelType.IsEmpty())
		{
			Options.ModelType = MeshyValidation::ValidateModelType(ModelType);
		}

		bool bShouldTexture;
		if (Args->TryGetBoolField(TEXT("should_texture"), bShouldTexture))
		{
			Options.bShouldTexture = bShouldTexture;
		}

		bool bEnablePBR;
		if (Args->TryGetBoolField(TEXT("enable_pbr"), bEnablePBR))
		{
			Options.bEnablePBR = bEnablePBR;
		}

		FString TexturePrompt;
		if (Args->TryGetStringField(TEXT("texture_prompt"), TexturePrompt) && !TexturePrompt.IsEmpty())
		{
			Options.TexturePrompt = TexturePrompt.Left(600);
		}

		FString TextureImageUrl;
		if (Args->TryGetStringField(TEXT("texture_image_url"), TextureImageUrl) && !TextureImageUrl.IsEmpty())
		{
			Options.TextureImageUrl = TextureImageUrl;
		}

		FMeshyResult Result = Client.CreateImageTo3DTask(Options);
		if (Result.Status == TEXT("FAILED"))
		{
			return FToolResult::Fail(FString::Printf(TEXT("Failed to create image-to-3D task: %s"), *Result.ErrorMessage));
		}

		return FToolResult::Ok(FString::Printf(
			TEXT("Image-to-3D task created (costs 20 credits with Meshy 6).\n"
				 "job_id: %s\n"
				 "job_type: image_to_3d\n\n"
				 "NEXT: Call with action='check', job_id='%s', job_type='image_to_3d'."),
			*Result.JobId, *Result.JobId));
	}
	else if (!Prompt.IsEmpty())
	{
		// ── TEXT-TO-3D PREVIEW ──
		UACPSettings* Settings = UACPSettings::Get();

		FMeshyTextTo3DOptions Options;
		Options.Prompt = Prompt.Left(600);

		PopulateCommonMeshParams(Args, Options.AiModel, Options.Topology, Options.SymmetryMode,
			Options.PoseMode, Options.bShouldRemesh, Options.TargetPolycount);

		FString ArtStyle;
		if (!Args->TryGetStringField(TEXT("art_style"), ArtStyle) || ArtStyle.IsEmpty())
		{
			ArtStyle = (Settings && !Settings->MeshyDefaultArtStyle.IsEmpty())
				? Settings->MeshyDefaultArtStyle : TEXT("realistic");
		}
		Options.ArtStyle = ArtStyle;

		FString NegativePrompt;
		if (Args->TryGetStringField(TEXT("negative_prompt"), NegativePrompt) && !NegativePrompt.IsEmpty())
		{
			Options.NegativePrompt = NegativePrompt;
		}

		FMeshyResult Result = Client.CreatePreviewTask(Options);
		if (Result.Status == TEXT("FAILED"))
		{
			return FToolResult::Fail(FString::Printf(TEXT("Failed to create preview task: %s"), *Result.ErrorMessage));
		}

		FString CreditNote = (Options.AiModel == TEXT("meshy-5"))
			? TEXT("5 credits") : TEXT("20 credits with Meshy 6");

		return FToolResult::Ok(FString::Printf(
			TEXT("Text-to-3D preview task created (costs %s).\n"
				 "job_id: %s\n"
				 "job_type: text_to_3d\n"
				 "stage: preview (stage 1 of 2)\n"
				 "prompt: %s\n"
				 "ai_model: %s\n\n"
				 "NEXT: Call with action='check', job_id='%s', job_type='text_to_3d'. "
				 "When done, a preview thumbnail will be shown. Ask the user before proceeding to refine."),
			*CreditNote, *Result.JobId, *Prompt.Left(100), *Options.AiModel, *Result.JobId));
	}

	return FToolResult::Fail(
		TEXT("For action='create', provide one of:\n"
			 "- 'prompt' for text-to-3D\n"
			 "- 'source_image' for image-to-3D\n"
			 "- 'source_images' (array) for multi-image-to-3D\n"
			 "- 'preview_task_id' to start refine stage"));
}

// ── check ──

FToolResult FGenerate3DModelTool::ExecuteCheck(const TSharedPtr<FJsonObject>& Args, FMeshyClient& Client)
{
	FString JobId;
	if (!Args->TryGetStringField(TEXT("job_id"), JobId) || JobId.IsEmpty())
	{
		return FToolResult::Fail(TEXT("action='check' requires a 'job_id' field."));
	}

	// Parse job_type for correct endpoint routing
	FString JobTypeStr;
	Args->TryGetStringField(TEXT("job_type"), JobTypeStr);
	EMeshyJobType JobType = MeshyValidation::JobTypeFromString(
		MeshyValidation::ValidateJobType(JobTypeStr));

	bool bWait = true;
	Args->TryGetBoolField(TEXT("wait"), bWait);

	int32 TimeoutSeconds = 300;
	int32 TimeoutParam;
	if (Args->TryGetNumberField(TEXT("timeout"), TimeoutParam) && TimeoutParam > 0)
	{
		TimeoutSeconds = TimeoutParam;
	}

	// Cap internal polling to stay under MCP timeout watchdog
	UACPSettings* CheckSettings = UACPSettings::Get();
	if (CheckSettings && CheckSettings->ToolExecutionTimeoutSeconds > 0)
	{
		int32 McpTimeout = CheckSettings->ToolExecutionTimeoutSeconds;
		int32 SafeMax = FMath::Max(McpTimeout - 10, 10);
		TimeoutSeconds = FMath::Min(TimeoutSeconds, SafeMax);
	}

	const double PollIntervalSec = 10.0;
	double StartTime = FPlatformTime::Seconds();
	int32 PollCount = 0;

	while (true)
	{
		FMeshyResult Result = Client.GetJobStatus(JobId, JobType);
		PollCount++;

		if (Result.Status == TEXT("FAILED") || Result.Status == TEXT("CANCELED"))
		{
			return FToolResult::Fail(FString::Printf(
				TEXT("Job %s: %s\n%s"), *JobId, *Result.Status, *Result.ErrorMessage));
		}

		if (Result.bSuccess) // SUCCEEDED
		{
			FString Message = FString::Printf(
				TEXT("Job %s: SUCCEEDED (100%%)\n"
					 "job_type: %s\n\n"),
				*JobId, *MeshyValidation::JobTypeToString(JobType));

			// Try to download and return the thumbnail
			FString Base64Image, MimeType;
			bool bHasThumbnail = false;
			if (!Result.ThumbnailUrl.IsEmpty())
			{
				bHasThumbnail = DownloadThumbnail(Result.ThumbnailUrl, Base64Image, MimeType);
				if (!bHasThumbnail)
				{
					UE_LOG(LogAgentIntegrationKit, Warning, TEXT("[AIK] Failed to download thumbnail for job %s"), *JobId);
				}
			}

			Message += TEXT("NEXT STEPS:\n");
			Message += FString::Printf(
				TEXT("- To IMPORT: action='import', job_id='%s', job_type='%s'\n"),
				*JobId, *MeshyValidation::JobTypeToString(JobType));

			if (JobType == EMeshyJobType::TextTo3D)
			{
				Message += FString::Printf(
					TEXT("- To REFINE (add textures, 10 credits): action='create', preview_task_id='%s'. "
						 "You can customize texturing with texture_prompt or texture_image_url.\n"),
					*JobId);
			}

			if (bHasThumbnail)
			{
				if (JobType == EMeshyJobType::TextTo3D)
				{
					Message += TEXT("\nThe preview thumbnail is shown above. Ask the user if they approve "
						"proceeding to refine (costs 10 credits). Only skip if user said to auto-approve.");
				}
				return FToolResult::OkWithImage(Message, Base64Image, MimeType, 0, 0);
			}

			return FToolResult::Ok(Message);
		}

		// Not waiting — return current status immediately
		if (!bWait)
		{
			return FToolResult::Ok(FString::Printf(
				TEXT("Job %s: %s (%d%% complete)\n"
					 "job_type: %s\n\n"
					 "NEXT: Wait 10-15 seconds, then call again with action='check', job_id='%s', job_type='%s'."),
				*JobId, *Result.Status, Result.Progress,
				*MeshyValidation::JobTypeToString(JobType),
				*JobId, *MeshyValidation::JobTypeToString(JobType)));
		}

		// Check timeout
		double Elapsed = FPlatformTime::Seconds() - StartTime;
		if (Elapsed >= TimeoutSeconds)
		{
			return FToolResult::Ok(FString::Printf(
				TEXT("Job %s: still %s (%d%% complete) after %d seconds (polled %d times).\n"
					 "job_type: %s\n\n"
					 "NEXT: Call action='check' with job_id='%s', job_type='%s' again to continue waiting."),
				*JobId, *Result.Status, Result.Progress, TimeoutSeconds, PollCount,
				*MeshyValidation::JobTypeToString(JobType),
				*JobId, *MeshyValidation::JobTypeToString(JobType)));
		}

		int32 ElapsedInt = FMath::RoundToInt32(Elapsed);
		UE_LOG(LogAgentIntegrationKit, Log, TEXT("[AIK] Meshy job %s: %s (%d%%) — poll %d, %ds elapsed"),
			*JobId, *Result.Status, Result.Progress, PollCount, ElapsedInt);

		// Sleep while keeping editor responsive
		double SleepEnd = FPlatformTime::Seconds() + PollIntervalSec;
		while (FPlatformTime::Seconds() < SleepEnd)
		{
			FHttpModule::Get().GetHttpManager().Tick(0.0f);
			FPlatformProcess::Sleep(0.05f);
		}
	}
}

// ── import ──

FToolResult FGenerate3DModelTool::ExecuteImport(const TSharedPtr<FJsonObject>& Args, FMeshyClient& Client)
{
	FString JobId;
	if (!Args->TryGetStringField(TEXT("job_id"), JobId) || JobId.IsEmpty())
	{
		return FToolResult::Fail(TEXT("action='import' requires a 'job_id' field."));
	}

	// Parse job_type for correct endpoint
	FString JobTypeStr;
	Args->TryGetStringField(TEXT("job_type"), JobTypeStr);
	EMeshyJobType JobType = MeshyValidation::JobTypeFromString(
		MeshyValidation::ValidateJobType(JobTypeStr));

	// Verify the job is SUCCEEDED
	FMeshyResult StatusResult = Client.GetJobStatus(JobId, JobType);
	if (!StatusResult.bSuccess)
	{
		if (StatusResult.Status == TEXT("PENDING") || StatusResult.Status == TEXT("IN_PROGRESS"))
		{
			return FToolResult::Fail(FString::Printf(
				TEXT("Job %s is still %s (%d%%). Wait for SUCCEEDED status before importing. "
					 "Use action='check' with job_type='%s' to monitor progress."),
				*JobId, *StatusResult.Status, StatusResult.Progress,
				*MeshyValidation::JobTypeToString(JobType)));
		}
		return FToolResult::Fail(FString::Printf(
			TEXT("Job %s failed or not found: %s"), *JobId, *StatusResult.ErrorMessage));
	}

	if (StatusResult.ModelUrl.IsEmpty())
	{
		return FToolResult::Fail(FString::Printf(
			TEXT("Job %s succeeded but has no model URL."), *JobId));
	}

	// Parse import parameters
	FString AssetPath, AssetName;
	if (!Args->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		AssetPath = TEXT("/Game/Generated3DModels");
	}
	Args->TryGetStringField(TEXT("asset_name"), AssetName);

	if (AssetName.IsEmpty())
	{
		AssetName = FString::Printf(TEXT("Meshy_%s"), *JobId.Left(12));
	}
	FString FinalAssetName = AssetImportUtils::SanitizeAssetName(AssetName);

	// Download to temp file
	FString TempDir = AssetImportUtils::GetGeneratedContentTempDir();
	FString TempFile = TempDir / FString::Printf(TEXT("%s_%s.glb"),
		*FinalAssetName, *FGuid::NewGuid().ToString().Left(8));

	FString DownloadError;
	if (!Client.DownloadModel(StatusResult.ModelUrl, TempFile, DownloadError))
	{
		return FToolResult::Fail(FString::Printf(TEXT("Failed to download model: %s"), *DownloadError));
	}

	// Import as StaticMesh — requires game thread
	FString ImportError;
	UStaticMesh* ImportedMesh = nullptr;

	if (IsInGameThread())
	{
		ImportedMesh = AssetImportUtils::ImportStaticMesh(TempFile, AssetPath, FinalAssetName, ImportError);
	}
	else
	{
		FEvent* ImportDone = FPlatformProcess::GetSynchEventFromPool();
		AsyncTask(ENamedThreads::GameThread, [&]()
		{
			ImportedMesh = AssetImportUtils::ImportStaticMesh(TempFile, AssetPath, FinalAssetName, ImportError);
			ImportDone->Trigger();
		});
		ImportDone->Wait();
		FPlatformProcess::ReturnSynchEventToPool(ImportDone);
	}

	// Clean up temp file
	IFileManager::Get().Delete(*TempFile);

	if (!ImportedMesh)
	{
		return FToolResult::Fail(FString::Printf(
			TEXT("Model downloaded but import failed: %s"), *ImportError));
	}

	FString FullAssetPath = ImportedMesh->GetPathName();
	return FToolResult::Ok(FString::Printf(
		TEXT("Successfully imported 3D model as StaticMesh: %s"), *FullAssetPath));
}

FToolResult FGenerate3DModelTool::ExecuteFal(const TSharedPtr<FJsonObject>& Args, const FString& Action, UACPSettings* Settings)
{
	if (Action == TEXT("create"))
	{
		return ExecuteFalCreate(Args, Settings);
	}
	if (Action == TEXT("check"))
	{
		return ExecuteFalCheck(Args, Settings);
	}
	if (Action == TEXT("import"))
	{
		return ExecuteFalImport(Args, Settings);
	}

	return FToolResult::Fail(FString::Printf(
		TEXT("Unknown action '%s' for provider='fal'. Valid actions: create, check, import"), *Action));
}

FToolResult FGenerate3DModelTool::ExecuteFalCreate(const TSharedPtr<FJsonObject>& Args, UACPSettings* Settings)
{
	FString EndpointId;
	Args->TryGetStringField(TEXT("fal_endpoint_id"), EndpointId);
	EndpointId = EndpointId.TrimStartAndEnd();
	if (EndpointId.IsEmpty())
	{
		return FToolResult::Fail(TEXT("provider='fal' action='create' requires fal_endpoint_id."));
	}

	TSharedPtr<FJsonObject> InputObj = MakeShared<FJsonObject>();
	const TSharedPtr<FJsonObject>* FalInputObj = nullptr;
	if (Args->TryGetObjectField(TEXT("fal_input"), FalInputObj) && FalInputObj && FalInputObj->IsValid())
	{
		InputObj = *FalInputObj;
	}
	else
	{
		FString FalInputJson;
		if (Args->TryGetStringField(TEXT("fal_input_json"), FalInputJson) && !FalInputJson.IsEmpty())
		{
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FalInputJson);
			if (!FJsonSerializer::Deserialize(Reader, InputObj) || !InputObj.IsValid())
			{
				return FToolResult::Fail(TEXT("fal_input_json must be a valid JSON object string."));
			}
		}

		// Minimal helper mapping for common Hunyuan3D flows when raw JSON isn't provided.
		if (InputObj->Values.Num() == 0)
		{
			FString Prompt;
			if (Args->TryGetStringField(TEXT("prompt"), Prompt) && !Prompt.IsEmpty())
			{
				InputObj->SetStringField(TEXT("prompt"), Prompt);
			}

			FString SourceImage;
			if (Args->TryGetStringField(TEXT("source_image"), SourceImage) && !SourceImage.IsEmpty())
			{
				InputObj->SetStringField(TEXT("image_url"), SourceImage);
			}
		}
	}

	const bool bUseNeoStack = Settings->ShouldUseBetideCredits();
	const FString AuthToken = Settings->GetFalAuthToken().TrimStartAndEnd();
	if (AuthToken.IsEmpty())
	{
		return FToolResult::Fail(bUseNeoStack
			? TEXT("Betide API token not configured. Set it in Project Settings > Plugins > Agent Integration Kit")
			: TEXT("fal.ai API key not configured. Set it in Project Settings > Plugins > Agent Integration Kit"));
	}

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetTimeout(120.0f);

	TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	if (bUseNeoStack)
	{
		Request->SetURL(Settings->GetFalSubmitUrl());
		Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *AuthToken));
		Body->SetStringField(TEXT("mode"), TEXT("neostack"));
		Body->SetStringField(TEXT("endpointId"), EndpointId);
		Body->SetObjectField(TEXT("input"), InputObj);
	}
	else
	{
		Request->SetURL(Settings->GetFalSubmitUrl() / EndpointId);
		Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Key %s"), *AuthToken));
		Body = InputObj.ToSharedRef();
	}

	FString BodyStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
	FJsonSerializer::Serialize(Body, Writer);
	Request->SetContentAsString(BodyStr);
	Request->ProcessRequestUntilComplete();

	FHttpResponsePtr Response = Request->GetResponse();
	if (Request->GetStatus() != EHttpRequestStatus::Succeeded || !Response.IsValid())
	{
		return FToolResult::Fail(TEXT("Failed to reach fal API"));
	}

	const int32 ResponseCode = Response->GetResponseCode();
	const FString ResponseContent = Response->GetContentAsString();

	TSharedPtr<FJsonObject> ResponseObj;
	TSharedRef<TJsonReader<>> RespReader = TJsonReaderFactory<>::Create(ResponseContent);
	const bool bParsed = FJsonSerializer::Deserialize(RespReader, ResponseObj) && ResponseObj.IsValid();

	if (ResponseCode < 200 || ResponseCode >= 300 || !bParsed)
	{
		return FToolResult::Fail(BuildFalHttpErrorMessage(ResponseCode, ResponseContent, bUseNeoStack));
	}

	FString RequestId, StatusUrl, ResponseUrl, CancelUrl;
	ResponseObj->TryGetStringField(TEXT("request_id"), RequestId);
	ResponseObj->TryGetStringField(TEXT("requestId"), RequestId);
	ResponseObj->TryGetStringField(TEXT("status_url"), StatusUrl);
	ResponseObj->TryGetStringField(TEXT("response_url"), ResponseUrl);
	ResponseObj->TryGetStringField(TEXT("cancel_url"), CancelUrl);

	return FToolResult::Ok(FString::Printf(
		TEXT("fal request submitted.\n"
			"provider: fal\n"
			"endpoint_id: %s\n"
			"request_id: %s\n"
			"status_url: %s\n"
			"response_url: %s\n"
			"cancel_url: %s\n\n"
			"NEXT:\n"
			"- action='check', provider='fal', status_url='%s'\n"
			"- action='import', provider='fal', response_url='%s'"),
		*EndpointId, *RequestId, *StatusUrl, *ResponseUrl, *CancelUrl, *StatusUrl, *ResponseUrl));
}

FToolResult FGenerate3DModelTool::ExecuteFalCheck(const TSharedPtr<FJsonObject>& Args, UACPSettings* Settings)
{
	FString StatusUrl;
	if (!Args->TryGetStringField(TEXT("status_url"), StatusUrl) || StatusUrl.IsEmpty())
	{
		return FToolResult::Fail(TEXT("provider='fal' action='check' requires status_url."));
	}

	bool bWait = true;
	Args->TryGetBoolField(TEXT("wait"), bWait);

	int32 TimeoutSeconds = 300;
	int32 TimeoutParam = 0;
	if (Args->TryGetNumberField(TEXT("timeout"), TimeoutParam) && TimeoutParam > 0)
	{
		TimeoutSeconds = TimeoutParam;
	}

	int32 PollIntervalSeconds = 15;
	int32 PollIntervalParam = 0;
	if (Args->TryGetNumberField(TEXT("poll_interval_seconds"), PollIntervalParam))
	{
		PollIntervalSeconds = FMath::Clamp(PollIntervalParam, 5, 60);
	}

	// Cap internal polling to stay under MCP timeout watchdog.
	if (Settings->ToolExecutionTimeoutSeconds > 0)
	{
		const int32 SafeMax = FMath::Max(Settings->ToolExecutionTimeoutSeconds - 10, 10);
		TimeoutSeconds = FMath::Min(TimeoutSeconds, SafeMax);
	}

	const bool bUseNeoStack = Settings->ShouldUseBetideCredits();
	const FString AuthToken = Settings->GetFalAuthToken().TrimStartAndEnd();
	if (AuthToken.IsEmpty())
	{
		return FToolResult::Fail(bUseNeoStack
			? TEXT("Betide API token not configured. Set it in Project Settings > Plugins > Agent Integration Kit")
			: TEXT("fal.ai API key not configured. Set it in Project Settings > Plugins > Agent Integration Kit"));
	}

	double StartTime = FPlatformTime::Seconds();
	int32 PollCount = 0;
	FString LastStatus = TEXT("UNKNOWN");
	FString LastRequestId;
	FString LastResponseUrl;
	FString LastCancelUrl;
	FString LastRaw;

	while (true)
	{
		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
		Request->SetVerb(TEXT("GET"));
		Request->SetTimeout(60.0f);

		if (bUseNeoStack)
		{
			const FString EncodedUrl = FGenericPlatformHttp::UrlEncode(StatusUrl);
			Request->SetURL(Settings->GetFalStatusProxyUrl() + TEXT("?mode=neostack&url=") + EncodedUrl);
			Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *AuthToken));
		}
		else
		{
			Request->SetURL(StatusUrl);
			Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Key %s"), *AuthToken));
		}

		Request->ProcessRequestUntilComplete();
		PollCount++;

		FHttpResponsePtr Response = Request->GetResponse();
		if (Request->GetStatus() != EHttpRequestStatus::Succeeded || !Response.IsValid())
		{
			return FToolResult::Fail(TEXT("Failed to check fal request status"));
		}

		const int32 ResponseCode = Response->GetResponseCode();
		const FString ResponseContent = Response->GetContentAsString();
		if (ResponseCode < 200 || ResponseCode >= 300)
		{
			return FToolResult::Fail(BuildFalHttpErrorMessage(ResponseCode, ResponseContent, bUseNeoStack));
		}

		TSharedPtr<FJsonObject> Obj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseContent);
		if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
		{
			return FToolResult::Ok(FString::Printf(TEXT("fal status response: %s"), *ResponseContent.Left(800)));
		}

		LastStatus = GetFalStatusFromResponse(Obj);
		Obj->TryGetStringField(TEXT("request_id"), LastRequestId);
		Obj->TryGetStringField(TEXT("response_url"), LastResponseUrl);
		Obj->TryGetStringField(TEXT("cancel_url"), LastCancelUrl);
		LastRaw = ResponseContent.Left(1200);

		if (IsFalTerminalFailure(LastStatus))
		{
			return FToolResult::Fail(FString::Printf(
				TEXT("fal request status\nrequest_id: %s\nstatus: %s\nraw: %s"),
				*LastRequestId, *LastStatus, *LastRaw));
		}

		if (IsFalTerminalSuccess(LastStatus))
		{
			return FToolResult::Ok(FString::Printf(
				TEXT("fal request status\nrequest_id: %s\nstatus: %s\nresponse_url: %s\ncancel_url: %s\n\nNEXT:\n- action='import', provider='fal', response_url='%s'"),
				*LastRequestId, *LastStatus, *LastResponseUrl, *LastCancelUrl, *LastResponseUrl));
		}

		if (!bWait || !IsFalInProgressStatus(LastStatus))
		{
			return FToolResult::Ok(FString::Printf(
				TEXT("fal request status\nrequest_id: %s\nstatus: %s\nresponse_url: %s\ncancel_url: %s\n\nNEXT: Wait %d-%d seconds, then call action='check' again with the same status_url."),
				*LastRequestId, *LastStatus, *LastResponseUrl, *LastCancelUrl, PollIntervalSeconds, PollIntervalSeconds + 5));
		}

		const double Elapsed = FPlatformTime::Seconds() - StartTime;
		if (Elapsed >= TimeoutSeconds)
		{
			return FToolResult::Ok(FString::Printf(
				TEXT("fal request status\nrequest_id: %s\nstatus: %s\nresponse_url: %s\ncancel_url: %s\n\nStill processing after %d seconds (polled %d times). NEXT: call action='check' again in %d-%d seconds."),
				*LastRequestId, *LastStatus, *LastResponseUrl, *LastCancelUrl,
				TimeoutSeconds, PollCount, PollIntervalSeconds, PollIntervalSeconds + 5));
		}

		UE_LOG(LogAgentIntegrationKit, Log,
			TEXT("[AIK] fal request %s: %s — poll %d, %.0fs elapsed"),
			*LastRequestId, *LastStatus, PollCount, Elapsed);

		double SleepEnd = FPlatformTime::Seconds() + PollIntervalSeconds;
		while (FPlatformTime::Seconds() < SleepEnd)
		{
			FHttpModule::Get().GetHttpManager().Tick(0.0f);
			FPlatformProcess::Sleep(0.05f);
		}
	}
}

FToolResult FGenerate3DModelTool::ExecuteFalImport(const TSharedPtr<FJsonObject>& Args, UACPSettings* Settings)
{
	FString ResponseUrl;
	if (!Args->TryGetStringField(TEXT("response_url"), ResponseUrl) || ResponseUrl.IsEmpty())
	{
		return FToolResult::Fail(TEXT("provider='fal' action='import' requires response_url."));
	}

	const bool bUseNeoStack = Settings->ShouldUseBetideCredits();
	const FString AuthToken = Settings->GetFalAuthToken().TrimStartAndEnd();
	if (AuthToken.IsEmpty())
	{
		return FToolResult::Fail(bUseNeoStack
			? TEXT("Betide API token not configured. Set it in Project Settings > Plugins > Agent Integration Kit")
			: TEXT("fal.ai API key not configured. Set it in Project Settings > Plugins > Agent Integration Kit"));
	}

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetVerb(TEXT("GET"));
	Request->SetTimeout(60.0f);
	if (bUseNeoStack)
	{
		const FString EncodedUrl = FGenericPlatformHttp::UrlEncode(ResponseUrl);
		Request->SetURL(Settings->GetFalResultProxyUrl() + TEXT("?mode=neostack&url=") + EncodedUrl);
		Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *AuthToken));
	}
	else
	{
		Request->SetURL(ResponseUrl);
		Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Key %s"), *AuthToken));
	}

	Request->ProcessRequestUntilComplete();
	FHttpResponsePtr Response = Request->GetResponse();
	if (Request->GetStatus() != EHttpRequestStatus::Succeeded || !Response.IsValid())
	{
		return FToolResult::Fail(TEXT("Failed to fetch fal result"));
	}

	const int32 ResponseCode = Response->GetResponseCode();
	const FString ResponseContent = Response->GetContentAsString();
	if (ResponseCode < 200 || ResponseCode >= 300)
	{
		return FToolResult::Fail(BuildFalHttpErrorMessage(ResponseCode, ResponseContent, bUseNeoStack));
	}

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseContent);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return FToolResult::Fail(TEXT("fal result is not valid JSON."));
	}

	FString ModelUrl;
	const TSharedPtr<FJsonObject>* ResponseObj = nullptr;
	if (Root->TryGetObjectField(TEXT("response"), ResponseObj) && ResponseObj && ResponseObj->IsValid())
	{
		FindModelUrlInObject(*ResponseObj, ModelUrl);
	}

	if (ModelUrl.IsEmpty())
	{
		FindModelUrlInObject(Root, ModelUrl);
	}

	if (ModelUrl.IsEmpty())
	{
		return FToolResult::Fail(TEXT("No model URL found in fal result response."));
	}

	FString AssetPath, AssetName;
	if (!Args->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		AssetPath = TEXT("/Game/Generated3DModels");
	}
	Args->TryGetStringField(TEXT("asset_name"), AssetName);
	if (AssetName.IsEmpty())
	{
		AssetName = TEXT("Fal3DModel");
	}
	const FString FinalAssetName = AssetImportUtils::SanitizeAssetName(AssetName);

	const FString TempDir = AssetImportUtils::GetGeneratedContentTempDir();
	const FString TempFile = TempDir / FString::Printf(TEXT("%s_%s.glb"),
		*FinalAssetName, *FGuid::NewGuid().ToString().Left(8));

	FString DownloadError;
	if (!DownloadFileToPath(ModelUrl, FString(), TempFile, DownloadError))
	{
		return FToolResult::Fail(FString::Printf(TEXT("Failed to download fal model: %s"), *DownloadError));
	}

	FString ImportError;
	UStaticMesh* ImportedMesh = nullptr;
	if (IsInGameThread())
	{
		ImportedMesh = AssetImportUtils::ImportStaticMesh(TempFile, AssetPath, FinalAssetName, ImportError);
	}
	else
	{
		FEvent* ImportDone = FPlatformProcess::GetSynchEventFromPool();
		AsyncTask(ENamedThreads::GameThread, [&]()
		{
			ImportedMesh = AssetImportUtils::ImportStaticMesh(TempFile, AssetPath, FinalAssetName, ImportError);
			ImportDone->Trigger();
		});
		ImportDone->Wait();
		FPlatformProcess::ReturnSynchEventToPool(ImportDone);
	}

	IFileManager::Get().Delete(*TempFile);

	if (!ImportedMesh)
	{
		return FToolResult::Fail(FString::Printf(TEXT("fal model downloaded but import failed: %s"), *ImportError));
	}

	return FToolResult::Ok(FString::Printf(TEXT("Successfully imported fal 3D model as StaticMesh: %s"), *ImportedMesh->GetPathName()));
}

// ── Thumbnail download ──

bool FGenerate3DModelTool::DownloadThumbnail(const FString& Url, FString& OutBase64, FString& OutMimeType)
{
	if (Url.IsEmpty())
	{
		return false;
	}

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(TEXT("GET"));
	Request->SetTimeout(30.0f);

	// Synchronous HTTP — safe from game thread
	Request->ProcessRequestUntilComplete();

	FHttpResponsePtr Response = Request->GetResponse();
	if (Request->GetStatus() != EHttpRequestStatus::Succeeded || !Response.IsValid())
	{
		return false;
	}

	int32 ResponseCode = Response->GetResponseCode();
	if (ResponseCode < 200 || ResponseCode >= 300 || Response->GetContent().Num() == 0)
	{
		return false;
	}

	OutBase64 = FBase64::Encode(Response->GetContent().GetData(), Response->GetContent().Num());

	// Determine MIME type
	OutMimeType = Response->GetContentType();
	if (OutMimeType.IsEmpty() || !OutMimeType.StartsWith(TEXT("image/")))
	{
		if (Url.Contains(TEXT(".png")))
		{
			OutMimeType = TEXT("image/png");
		}
		else if (Url.Contains(TEXT(".webp")))
		{
			OutMimeType = TEXT("image/webp");
		}
		else
		{
			OutMimeType = TEXT("image/jpeg");
		}
	}

	return true;
}

bool FGenerate3DModelTool::DownloadFileToPath(const FString& Url, const FString& AuthHeader, const FString& OutPath, FString& OutError)
{
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(TEXT("GET"));
	Request->SetTimeout(120.0f);
	if (!AuthHeader.IsEmpty())
	{
		Request->SetHeader(TEXT("Authorization"), AuthHeader);
	}

	Request->ProcessRequestUntilComplete();
	FHttpResponsePtr Response = Request->GetResponse();
	if (Request->GetStatus() != EHttpRequestStatus::Succeeded || !Response.IsValid())
	{
		OutError = TEXT("HTTP request failed");
		return false;
	}

	const int32 Code = Response->GetResponseCode();
	if (Code < 200 || Code >= 300)
	{
		OutError = FString::Printf(TEXT("HTTP %d"), Code);
		return false;
	}

	if (!FFileHelper::SaveArrayToFile(Response->GetContent(), *OutPath))
	{
		OutError = TEXT("Failed to save downloaded file");
		return false;
	}

	return true;
}
