// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Providers/GenerativeProvider.h"
#include "Providers/GenerativeProviderRegistry.h"
#include "Serialization/JsonSerializer.h"

// ── Helpers ──────────────────────────────────────────────────────────

static void SetIfNotEmpty(const TSharedPtr<FJsonObject>& Body, const FString& Key, const FString& Value)
{
	if (!Value.IsEmpty()) Body->SetStringField(Key, Value);
}

static FString GetStr(const TSharedPtr<FJsonObject>& J, const FString& Key)
{
	FString V;
	if (J.IsValid()) J->TryGetStringField(Key, V);
	return V;
}

static int32 GetInt(const TSharedPtr<FJsonObject>& J, const FString& Key, int32 Default = 0)
{
	int32 V = Default;
	if (J.IsValid()) J->TryGetNumberField(Key, V);
	return V;
}

static bool GetBool(const TSharedPtr<FJsonObject>& J, const FString& Key, bool Default = false)
{
	bool V = Default;
	if (J.IsValid()) J->TryGetBoolField(Key, V);
	return V;
}

// ── Tripo response parsing ───────────────────────────────────────────
// Tripo wraps all responses in {"code": 0, "data": {...}}
// Task status uses: queued, running, success, failed, banned, expired, cancelled

static TSharedPtr<FJsonObject> UnwrapTripoData(const TSharedPtr<FJsonObject>& Json)
{
	if (!Json.IsValid()) return nullptr;

	int32 Code = 0;
	Json->TryGetNumberField(TEXT("code"), Code);
	if (Code != 0) return nullptr;

	const TSharedPtr<FJsonObject>* Data = nullptr;
	if (Json->TryGetObjectField(TEXT("data"), Data) && Data)
		return *Data;

	return nullptr;
}

static FGenerativeJob ParseTripoTaskResponse(const TSharedPtr<FJsonObject>& Data, const FString& FallbackJobId = TEXT(""))
{
	if (!Data.IsValid())
		return FGenerativeJob::MakeFail(TEXT("Empty response data"));

	FString Id = GetStr(Data, TEXT("task_id"));
	if (Id.IsEmpty()) Id = FallbackJobId;

	const FString StatusStr = GetStr(Data, TEXT("status"));
	const EGenerativeJobStatus Status = FGenerativeProviderBase::ParseStatus(StatusStr);

	FGenerativeJob Job;
	Job.JobId = Id;
	Job.Status = Status;
	Job.Progress = GetInt(Data, TEXT("progress"));
	Job.RawResponse = Data;

	if (Status == EGenerativeJobStatus::Failed || Status == EGenerativeJobStatus::Cancelled)
	{
		Job.ErrorMessage = GetStr(Data, TEXT("error_msg"));
		if (Job.ErrorMessage.IsEmpty())
			Job.ErrorMessage = FString::Printf(TEXT("Task %s"), *StatusStr);
		return Job;
	}

	// Tripo uses "banned" and "expired" as terminal failure states
	if (StatusStr == TEXT("banned"))
	{
		Job.Status = EGenerativeJobStatus::Failed;
		Job.ErrorMessage = TEXT("Task banned: content policy violation");
		return Job;
	}
	if (StatusStr == TEXT("expired"))
	{
		Job.Status = EGenerativeJobStatus::Failed;
		Job.ErrorMessage = TEXT("Task expired");
		return Job;
	}

	if (Status == EGenerativeJobStatus::Succeeded)
	{
		const TSharedPtr<FJsonObject>* OutputPtr = nullptr;
		if (Data->TryGetObjectField(TEXT("output"), OutputPtr) && OutputPtr && (*OutputPtr).IsValid())
		{
			const TSharedPtr<FJsonObject>& Output = *OutputPtr;

			// Model URLs
			const FString Model = GetStr(Output, TEXT("model"));
			const FString BaseModel = GetStr(Output, TEXT("base_model"));
			const FString PbrModel = GetStr(Output, TEXT("pbr_model"));

			// Prefer pbr_model > model > base_model
			if (!PbrModel.IsEmpty())
			{
				Job.ResultUrl = PbrModel;
				Job.ExtraUrls.Add(TEXT("pbr_model"), PbrModel);
			}
			if (!Model.IsEmpty())
			{
				if (Job.ResultUrl.IsEmpty()) Job.ResultUrl = Model;
				Job.ExtraUrls.Add(TEXT("model"), Model);
			}
			if (!BaseModel.IsEmpty())
			{
				if (Job.ResultUrl.IsEmpty()) Job.ResultUrl = BaseModel;
				Job.ExtraUrls.Add(TEXT("base_model"), BaseModel);
			}

			// Preview/rendered image
			const FString RenderedImage = GetStr(Output, TEXT("rendered_image"));
			if (!RenderedImage.IsEmpty())
				Job.ThumbnailUrl = RenderedImage;

			// Generated image (for text_to_image / generate_image)
			const FString GeneratedImage = GetStr(Output, TEXT("generated_image"));
			if (!GeneratedImage.IsEmpty())
			{
				Job.ImageUrls.Add(GeneratedImage);
				if (Job.ResultUrl.IsEmpty()) Job.ResultUrl = GeneratedImage;
			}

			// Rigging output
			const FString Riggable = GetStr(Output, TEXT("riggable"));
			// Keep raw response for advanced access
		}
	}

	return Job;
}

// ═════════════════════════════════════════════════════════════════════
// TripoProvider — Tripo3D API (all endpoints via POST /task)
// ═════════════════════════════════════════════════════════════════════

class FTripoProvider : public FGenerativeProviderBase
{
public:
	FString GetId() const override { return TEXT("tripo"); }
	FString GetDisplayName() const override { return TEXT("Tripo AI"); }
	FString GetWebsite() const override { return TEXT("https://tripo3d.ai"); }
	FString GetDirectBaseUrl() const override { return TEXT("https://api.tripo3d.ai/v2/openapi"); }
	FString GetProxyPath() const override { return TEXT("/api/proxy/tripo"); }
	FString GetApiKeySettingName() const override { return TEXT("TripoApiKey"); }

	// ── Actions ──────────────────────────────────────────────────────

	TArray<FProviderActionDescriptor> GetActions() const override
	{
		return {
			// 3D Generation
			{TEXT("text_to_model"), TEXT("Generate 3D model from text prompt. Supports model versions: "
				"P1-20260311 (optimized low-poly), Turbo-v1.0, v3.1, v3.0, v2.5 (default), v2.0, v1.4. "
				"Options: face_limit, texture, pbr, quad, auto_size, texture_quality. Cost: 10-40 credits."),
				{TEXT("text")}, {TEXT("model")}, BuildTextToModelSchema(), TEXT("10-40 credits")},

			{TEXT("image_to_model"), TEXT("Generate 3D model from a single image (URL or file_token). "
				"Same model versions as text_to_model. Extra options: enable_image_autofix, texture_alignment, "
				"orientation. Cost: 20-50 credits."),
				{TEXT("image")}, {TEXT("model")}, BuildImageToModelSchema(), TEXT("20-50 credits")},

			{TEXT("multiview_to_model"), TEXT("Generate 3D from 4 orthogonal images [front, left, back, right]. "
				"Pass image_urls array (exactly 4, can omit non-front with empty strings). "
				"Cost: 20-50 credits."),
				{TEXT("image")}, {TEXT("model")}, BuildMultiviewToModelSchema(), TEXT("20-50 credits")},

			{TEXT("refine_model"), TEXT("Refine a draft model into higher quality. Requires draft_model_task_id "
				"from a succeeded text/image/multiview_to_model task (v1.4 only). Cost: 30 credits."),
				{TEXT("job_ref")}, {TEXT("model")}, BuildRefineModelSchema(), TEXT("30 credits")},

			// Texture
			{TEXT("texture_model"), TEXT("Apply new textures to an existing model. Provide original_model_task_id. "
				"Options: texture_prompt (text/image/style_image), pbr, texture_quality, texture_alignment, bake. "
				"Cost: 10-20 credits."),
				{TEXT("model"), TEXT("text")}, {TEXT("model")}, BuildTextureModelSchema(), TEXT("10-20 credits")},

			// Image Generation
			{TEXT("text_to_image"), TEXT("Generate image from text prompt. Simple endpoint. Cost: 5 credits."),
				{TEXT("text")}, {TEXT("image")}, BuildTextToImageSchema(), TEXT("5 credits")},

			{TEXT("generate_image"), TEXT("Advanced image generation with model selection: flux.1_kontext_pro (default), "
				"gpt_4o, gemini_2.5_flash_image_preview, z_image, flux.1_dev. Supports image reference via file_url. "
				"Options: t_pose, sketch_to_render. Cost: varies."),
				{TEXT("text")}, {TEXT("image")}, BuildGenerateImageSchema(), TEXT("varies")},

			// Mesh Editing
			{TEXT("mesh_segmentation"), TEXT("Segment a 3D model into named parts. "
				"Requires original_model_task_id. Cost: 40 credits."),
				{TEXT("model")}, {TEXT("model")}, BuildOriginalTaskSchema(), TEXT("40 credits")},

			{TEXT("mesh_completion"), TEXT("Complete mesh geometry for segmented parts. "
				"Requires original_model_task_id from a mesh_segmentation task. Optional: part_names array. Cost: 50 credits."),
				{TEXT("model")}, {TEXT("model")}, BuildMeshCompletionSchema(), TEXT("50 credits")},

			{TEXT("highpoly_to_lowpoly"), TEXT("Smart retopology: convert high-poly to clean low-poly mesh. "
				"Options: quad, face_limit (1000-20000), bake, part_names. Cost: 30 credits."),
				{TEXT("model")}, {TEXT("model")}, BuildHighPolyToLowPolySchema(), TEXT("30 credits")},

			// Animation Pipeline
			{TEXT("animate_prerigcheck"), TEXT("Check if a model can be rigged. Returns riggable (bool) and rig_type "
				"(biped/quadruped/hexapod/octopod/avian/serpentine/aquatic). Free."),
				{TEXT("model")}, {TEXT("data")}, BuildOriginalTaskSchema(), TEXT("free"), true},

			{TEXT("animate_rig"), TEXT("Auto-rig a 3D model. Options: out_format (glb/fbx), rig_type, "
				"spec (tripo/mixamo), model_version (v2.5-20260210, v2.0-20250506, v1.0-20240301). Cost: 25 credits."),
				{TEXT("model")}, {TEXT("model")}, BuildAnimateRigSchema(), TEXT("25 credits")},

			{TEXT("animate_retarget"), TEXT("Apply preset animation to a rigged model. Requires original_model_task_id "
				"from a rig task. animation or animations (array, max 5). Presets: preset:idle, preset:walk, "
				"preset:run, preset:dive, preset:climb, preset:jump, preset:slash, preset:shoot, preset:hurt, "
				"preset:fall, preset:turn. Options: out_format, bake_animation, animate_in_place. Cost: 10/animation."),
				{TEXT("job_ref")}, {TEXT("animation")}, BuildAnimateRetargetSchema(), TEXT("10 credits/animation")},

			// Post Processing
			{TEXT("stylize_model"), TEXT("Apply unique style to a model: lego, voxel, voronoi, or minecraft. "
				"Optional block_size (32-128) for minecraft. Cost: 20 credits."),
				{TEXT("model")}, {TEXT("model")}, BuildStylizeSchema(), TEXT("20 credits")},

			{TEXT("convert_model"), TEXT("Convert model format. Formats: GLTF, USDZ, FBX, OBJ, STL, 3MF. "
				"Options: quad, face_limit, flatten_bottom, texture_size, pivot_to_center_bottom, "
				"scale_factor, with_animation, pack_uv, bake. Cost: 5-10 credits."),
				{TEXT("model")}, {TEXT("model")}, BuildConvertSchema(), TEXT("5-10 credits")},

			// Utility
			{TEXT("balance"), TEXT("Check remaining Tripo API credits. Returns balance and frozen amounts."),
				{}, {TEXT("data")}, nullptr, TEXT("free"), true},
		};
	}

	// ── Submit ───────────────────────────────────────────────────────

	FGenerativeJob Submit(const FString& ActionId, const TSharedPtr<FJsonObject>& Params) override
	{
		if (ActionId == TEXT("balance")) return SubmitBalance();

		// All other actions go through POST /task with a "type" field
		return SubmitTask(ActionId, Params);
	}

	// ── CheckStatus ──────────────────────────────────────────────────

	FGenerativeJob CheckStatus(const FString& JobId, const FString& ActionId) override
	{
		const FString Path = FString::Printf(TEXT("/task/%s"), *JobId);
		TSharedPtr<FJsonObject> Json = HttpGet(Path);
		if (!Json.IsValid())
			return FGenerativeJob::MakeFail(FString::Printf(TEXT("Failed to check status for task %s"), *JobId));

		TSharedPtr<FJsonObject> Data = UnwrapTripoData(Json);
		if (!Data.IsValid())
		{
			// Check for error in response
			FString ErrMsg = GetStr(Json, TEXT("message"));
			if (ErrMsg.IsEmpty()) ErrMsg = TEXT("Invalid response from Tripo");
			return FGenerativeJob::MakeFail(ErrMsg);
		}

		auto Job = ParseTripoTaskResponse(Data, JobId);
		Job.ProviderId = TEXT("tripo");
		Job.ActionId = ActionId;
		return Job;
	}

	int32 GetBalance() override
	{
		TSharedPtr<FJsonObject> Json = HttpGet(TEXT("/user/balance"));
		TSharedPtr<FJsonObject> Data = UnwrapTripoData(Json);
		if (Data.IsValid())
			return GetInt(Data, TEXT("balance"));
		return -1;
	}

private:

	// ── Unified task submission ──────────────────────────────────────
	// Tripo uses a single POST /task endpoint with "type" field for everything

	FGenerativeJob SubmitTask(const FString& ActionId, const TSharedPtr<FJsonObject>& Params)
	{
		// Map our action IDs to Tripo's task type names
		const FString TaskType = MapActionToTaskType(ActionId);
		if (TaskType.IsEmpty())
			return FGenerativeJob::MakeFail(FString::Printf(TEXT("Unknown Tripo action: %s"), *ActionId));

		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("type"), TaskType);

		// Build action-specific body
		if (ActionId == TEXT("text_to_model"))       BuildTextToModelBody(Body, Params);
		else if (ActionId == TEXT("image_to_model")) BuildImageToModelBody(Body, Params);
		else if (ActionId == TEXT("multiview_to_model")) BuildMultiviewToModelBody(Body, Params);
		else if (ActionId == TEXT("refine_model"))    BuildRefineModelBody(Body, Params);
		else if (ActionId == TEXT("texture_model"))   BuildTextureModelBody(Body, Params);
		else if (ActionId == TEXT("text_to_image"))   BuildTextToImageBody(Body, Params);
		else if (ActionId == TEXT("generate_image"))  BuildGenerateImageBody(Body, Params);
		else if (ActionId == TEXT("mesh_segmentation")) BuildOriginalTaskBody(Body, Params);
		else if (ActionId == TEXT("mesh_completion")) BuildMeshCompletionBody(Body, Params);
		else if (ActionId == TEXT("highpoly_to_lowpoly")) BuildHighPolyToLowPolyBody(Body, Params);
		else if (ActionId == TEXT("animate_prerigcheck")) BuildOriginalTaskBody(Body, Params);
		else if (ActionId == TEXT("animate_rig"))     BuildAnimateRigBody(Body, Params);
		else if (ActionId == TEXT("animate_retarget")) BuildAnimateRetargetBody(Body, Params);
		else if (ActionId == TEXT("stylize_model"))   BuildStylizeBody(Body, Params);
		else if (ActionId == TEXT("convert_model"))   BuildConvertBody(Body, Params);

		TSharedPtr<FJsonObject> Json = HttpPost(TEXT("/task"), Body);
		if (!Json.IsValid())
			return FGenerativeJob::MakeFail(FString::Printf(TEXT("Failed to create %s task"), *ActionId));

		// Check for Tripo error
		int32 Code = 0;
		Json->TryGetNumberField(TEXT("code"), Code);
		if (Code != 0)
		{
			FString ErrMsg = GetStr(Json, TEXT("message"));
			FString Suggestion = GetStr(Json, TEXT("suggestion"));
			return FGenerativeJob::MakeFail(FString::Printf(TEXT("Tripo error %d: %s. %s"),
				Code, *ErrMsg, *Suggestion));
		}

		TSharedPtr<FJsonObject> Data = UnwrapTripoData(Json);
		if (!Data.IsValid())
			return FGenerativeJob::MakeFail(TEXT("No data in Tripo response"));

		const FString TaskId = GetStr(Data, TEXT("task_id"));
		if (TaskId.IsEmpty())
			return FGenerativeJob::MakeFail(TEXT("No task_id in Tripo response"));

		auto Job = FGenerativeJob::MakePending(TaskId);
		Job.ProviderId = TEXT("tripo");
		Job.ActionId = ActionId;
		return Job;
	}

	FGenerativeJob SubmitBalance()
	{
		TSharedPtr<FJsonObject> Json = HttpGet(TEXT("/user/balance"));
		TSharedPtr<FJsonObject> Data = UnwrapTripoData(Json);
		if (!Data.IsValid())
			return FGenerativeJob::MakeFail(TEXT("Failed to get Tripo balance"));

		FGenerativeJob Job;
		Job.ProviderId = TEXT("tripo");
		Job.ActionId = TEXT("balance");
		Job.Status = EGenerativeJobStatus::Succeeded;
		Job.Progress = 100;
		Job.RawResponse = Data;
		return Job;
	}

	// ── Action→TaskType mapping ──────────────────────────────────────

	static FString MapActionToTaskType(const FString& ActionId)
	{
		static const TMap<FString, FString> Map = {
			{TEXT("text_to_model"),       TEXT("text_to_model")},
			{TEXT("image_to_model"),      TEXT("image_to_model")},
			{TEXT("multiview_to_model"),  TEXT("multiview_to_model")},
			{TEXT("refine_model"),        TEXT("refine_model")},
			{TEXT("texture_model"),       TEXT("texture_model")},
			{TEXT("text_to_image"),       TEXT("text_to_image")},
			{TEXT("generate_image"),      TEXT("generate_image")},
			{TEXT("mesh_segmentation"),   TEXT("mesh_segmentation")},
			{TEXT("mesh_completion"),     TEXT("mesh_completion")},
			{TEXT("highpoly_to_lowpoly"), TEXT("highpoly_to_lowpoly")},
			{TEXT("animate_prerigcheck"), TEXT("animate_prerigcheck")},
			{TEXT("animate_rig"),         TEXT("animate_rig")},
			{TEXT("animate_retarget"),    TEXT("animate_retarget")},
			{TEXT("stylize_model"),       TEXT("stylize_model")},
			{TEXT("convert_model"),       TEXT("convert_model")},
			{TEXT("import_model"),        TEXT("import_model")},
		};
		const FString* Found = Map.Find(ActionId);
		return Found ? *Found : FString();
	}

	// ── Body builders ────────────────────────────────────────────────

	// Common generation fields shared by text/image/multiview_to_model
	static void AddCommonGenerationFields(const TSharedPtr<FJsonObject>& Body, const TSharedPtr<FJsonObject>& Params)
	{
		SetIfNotEmpty(Body, TEXT("model_version"), GetStr(Params, TEXT("model_version")));

		if (Params->HasField(TEXT("face_limit")))
			Body->SetNumberField(TEXT("face_limit"), GetInt(Params, TEXT("face_limit")));
		if (Params->HasField(TEXT("texture")))
			Body->SetBoolField(TEXT("texture"), GetBool(Params, TEXT("texture"), true));
		if (Params->HasField(TEXT("pbr")))
			Body->SetBoolField(TEXT("pbr"), GetBool(Params, TEXT("pbr"), true));
		if (Params->HasField(TEXT("auto_size")))
			Body->SetBoolField(TEXT("auto_size"), GetBool(Params, TEXT("auto_size")));
		if (Params->HasField(TEXT("quad")))
			Body->SetBoolField(TEXT("quad"), GetBool(Params, TEXT("quad")));
		if (Params->HasField(TEXT("smart_low_poly")))
			Body->SetBoolField(TEXT("smart_low_poly"), GetBool(Params, TEXT("smart_low_poly")));
		if (Params->HasField(TEXT("generate_parts")))
			Body->SetBoolField(TEXT("generate_parts"), GetBool(Params, TEXT("generate_parts")));
		if (Params->HasField(TEXT("export_uv")))
			Body->SetBoolField(TEXT("export_uv"), GetBool(Params, TEXT("export_uv"), true));

		SetIfNotEmpty(Body, TEXT("texture_quality"), GetStr(Params, TEXT("texture_quality")));
		SetIfNotEmpty(Body, TEXT("texture_alignment"), GetStr(Params, TEXT("texture_alignment")));
		SetIfNotEmpty(Body, TEXT("compress"), GetStr(Params, TEXT("compress")));
		SetIfNotEmpty(Body, TEXT("geometry_quality"), GetStr(Params, TEXT("geometry_quality")));

		if (Params->HasField(TEXT("model_seed")))
			Body->SetNumberField(TEXT("model_seed"), GetInt(Params, TEXT("model_seed")));
		if (Params->HasField(TEXT("texture_seed")))
			Body->SetNumberField(TEXT("texture_seed"), GetInt(Params, TEXT("texture_seed")));
	}

	static void BuildTextToModelBody(const TSharedPtr<FJsonObject>& Body, const TSharedPtr<FJsonObject>& Params)
	{
		Body->SetStringField(TEXT("prompt"), GetStr(Params, TEXT("prompt")).Left(1024));
		SetIfNotEmpty(Body, TEXT("negative_prompt"), GetStr(Params, TEXT("negative_prompt")).Left(255));

		if (Params->HasField(TEXT("image_seed")))
			Body->SetNumberField(TEXT("image_seed"), GetInt(Params, TEXT("image_seed")));

		AddCommonGenerationFields(Body, Params);
	}

	static void BuildImageToModelBody(const TSharedPtr<FJsonObject>& Body, const TSharedPtr<FJsonObject>& Params)
	{
		// file: {type, url} or {type, file_token}
		const FString ImageUrl = GetStr(Params, TEXT("image_url"));
		const FString FileToken = GetStr(Params, TEXT("file_token"));

		TSharedPtr<FJsonObject> FileObj = MakeShared<FJsonObject>();
		FileObj->SetStringField(TEXT("type"), TEXT("jpg"));
		if (!ImageUrl.IsEmpty())
			FileObj->SetStringField(TEXT("url"), ImageUrl);
		else if (!FileToken.IsEmpty())
			FileObj->SetStringField(TEXT("file_token"), FileToken);
		Body->SetObjectField(TEXT("file"), FileObj);

		if (Params->HasField(TEXT("enable_image_autofix")))
			Body->SetBoolField(TEXT("enable_image_autofix"), GetBool(Params, TEXT("enable_image_autofix")));
		SetIfNotEmpty(Body, TEXT("orientation"), GetStr(Params, TEXT("orientation")));

		AddCommonGenerationFields(Body, Params);
	}

	static void BuildMultiviewToModelBody(const TSharedPtr<FJsonObject>& Body, const TSharedPtr<FJsonObject>& Params)
	{
		// image_urls: array of 4 URLs [front, left, back, right]
		if (Params->HasField(TEXT("image_urls")))
		{
			TArray<TSharedPtr<FJsonValue>> Files;
			const auto& Urls = Params->GetArrayField(TEXT("image_urls"));
			for (const auto& UrlVal : Urls)
			{
				FString Url;
				if (UrlVal->TryGetString(Url) && !Url.IsEmpty())
				{
					TSharedPtr<FJsonObject> FileObj = MakeShared<FJsonObject>();
					FileObj->SetStringField(TEXT("type"), TEXT("jpg"));
					FileObj->SetStringField(TEXT("url"), Url);
					Files.Add(MakeShared<FJsonValueObject>(FileObj));
				}
				else
				{
					// Empty slot (omitted view)
					Files.Add(MakeShared<FJsonValueObject>(MakeShared<FJsonObject>()));
				}
			}
			Body->SetArrayField(TEXT("files"), Files);
		}

		SetIfNotEmpty(Body, TEXT("orientation"), GetStr(Params, TEXT("orientation")));
		AddCommonGenerationFields(Body, Params);
	}

	static void BuildRefineModelBody(const TSharedPtr<FJsonObject>& Body, const TSharedPtr<FJsonObject>& Params)
	{
		SetIfNotEmpty(Body, TEXT("draft_model_task_id"), GetStr(Params, TEXT("draft_model_task_id")));
	}

	static void BuildTextureModelBody(const TSharedPtr<FJsonObject>& Body, const TSharedPtr<FJsonObject>& Params)
	{
		SetIfNotEmpty(Body, TEXT("original_model_task_id"), GetStr(Params, TEXT("original_model_task_id")));
		SetIfNotEmpty(Body, TEXT("model_version"), GetStr(Params, TEXT("model_version")));

		// texture_prompt can be {text: "..."} or {image: {type, url}} or {style_image: {type, url}}
		const FString TextPrompt = GetStr(Params, TEXT("texture_prompt_text"));
		const FString ImagePrompt = GetStr(Params, TEXT("texture_prompt_image_url"));
		const FString StyleImage = GetStr(Params, TEXT("style_image_url"));

		if (!TextPrompt.IsEmpty() || !ImagePrompt.IsEmpty() || !StyleImage.IsEmpty())
		{
			TSharedPtr<FJsonObject> TexPrompt = MakeShared<FJsonObject>();
			if (!TextPrompt.IsEmpty())
				TexPrompt->SetStringField(TEXT("text"), TextPrompt);
			if (!ImagePrompt.IsEmpty())
			{
				TSharedPtr<FJsonObject> ImgObj = MakeShared<FJsonObject>();
				ImgObj->SetStringField(TEXT("type"), TEXT("jpg"));
				ImgObj->SetStringField(TEXT("url"), ImagePrompt);
				TexPrompt->SetObjectField(TEXT("image"), ImgObj);
			}
			if (!StyleImage.IsEmpty())
			{
				TSharedPtr<FJsonObject> StyleObj = MakeShared<FJsonObject>();
				StyleObj->SetStringField(TEXT("type"), TEXT("jpg"));
				StyleObj->SetStringField(TEXT("url"), StyleImage);
				TexPrompt->SetObjectField(TEXT("style_image"), StyleObj);
			}
			Body->SetObjectField(TEXT("texture_prompt"), TexPrompt);
		}

		if (Params->HasField(TEXT("texture")))
			Body->SetBoolField(TEXT("texture"), GetBool(Params, TEXT("texture"), true));
		if (Params->HasField(TEXT("pbr")))
			Body->SetBoolField(TEXT("pbr"), GetBool(Params, TEXT("pbr"), true));
		SetIfNotEmpty(Body, TEXT("texture_quality"), GetStr(Params, TEXT("texture_quality")));
		SetIfNotEmpty(Body, TEXT("texture_alignment"), GetStr(Params, TEXT("texture_alignment")));
		if (Params->HasField(TEXT("bake")))
			Body->SetBoolField(TEXT("bake"), GetBool(Params, TEXT("bake"), true));
		if (Params->HasField(TEXT("texture_seed")))
			Body->SetNumberField(TEXT("texture_seed"), GetInt(Params, TEXT("texture_seed")));
		SetIfNotEmpty(Body, TEXT("compress"), GetStr(Params, TEXT("compress")));
		if (Params->HasField(TEXT("part_names")))
			Body->SetArrayField(TEXT("part_names"), Params->GetArrayField(TEXT("part_names")));
	}

	static void BuildTextToImageBody(const TSharedPtr<FJsonObject>& Body, const TSharedPtr<FJsonObject>& Params)
	{
		Body->SetStringField(TEXT("prompt"), GetStr(Params, TEXT("prompt")).Left(1024));
		SetIfNotEmpty(Body, TEXT("negative_prompt"), GetStr(Params, TEXT("negative_prompt")).Left(255));
	}

	static void BuildGenerateImageBody(const TSharedPtr<FJsonObject>& Body, const TSharedPtr<FJsonObject>& Params)
	{
		Body->SetStringField(TEXT("prompt"), GetStr(Params, TEXT("prompt")).Left(1024));
		SetIfNotEmpty(Body, TEXT("model_version"), GetStr(Params, TEXT("model_version")));

		// Optional image reference
		const FString FileUrl = GetStr(Params, TEXT("file_url"));
		if (!FileUrl.IsEmpty())
		{
			TSharedPtr<FJsonObject> FileObj = MakeShared<FJsonObject>();
			FileObj->SetStringField(TEXT("type"), TEXT("jpg"));
			FileObj->SetStringField(TEXT("url"), FileUrl);
			Body->SetObjectField(TEXT("file"), FileObj);
		}

		if (Params->HasField(TEXT("t_pose")))
			Body->SetBoolField(TEXT("t_pose"), GetBool(Params, TEXT("t_pose")));
		if (Params->HasField(TEXT("sketch_to_render")))
			Body->SetBoolField(TEXT("sketch_to_render"), GetBool(Params, TEXT("sketch_to_render")));
	}

	static void BuildOriginalTaskBody(const TSharedPtr<FJsonObject>& Body, const TSharedPtr<FJsonObject>& Params)
	{
		SetIfNotEmpty(Body, TEXT("original_model_task_id"), GetStr(Params, TEXT("original_model_task_id")));
	}

	static void BuildMeshCompletionBody(const TSharedPtr<FJsonObject>& Body, const TSharedPtr<FJsonObject>& Params)
	{
		SetIfNotEmpty(Body, TEXT("original_model_task_id"), GetStr(Params, TEXT("original_model_task_id")));
		if (Params->HasField(TEXT("part_names")))
			Body->SetArrayField(TEXT("part_names"), Params->GetArrayField(TEXT("part_names")));
	}

	static void BuildHighPolyToLowPolyBody(const TSharedPtr<FJsonObject>& Body, const TSharedPtr<FJsonObject>& Params)
	{
		SetIfNotEmpty(Body, TEXT("original_model_task_id"), GetStr(Params, TEXT("original_model_task_id")));
		if (Params->HasField(TEXT("quad")))
			Body->SetBoolField(TEXT("quad"), GetBool(Params, TEXT("quad")));
		if (Params->HasField(TEXT("face_limit")))
			Body->SetNumberField(TEXT("face_limit"), FMath::Clamp(GetInt(Params, TEXT("face_limit"), 10000), 1000, 20000));
		if (Params->HasField(TEXT("bake")))
			Body->SetBoolField(TEXT("bake"), GetBool(Params, TEXT("bake"), true));
		if (Params->HasField(TEXT("part_names")))
			Body->SetArrayField(TEXT("part_names"), Params->GetArrayField(TEXT("part_names")));
	}

	static void BuildAnimateRigBody(const TSharedPtr<FJsonObject>& Body, const TSharedPtr<FJsonObject>& Params)
	{
		SetIfNotEmpty(Body, TEXT("original_model_task_id"), GetStr(Params, TEXT("original_model_task_id")));
		SetIfNotEmpty(Body, TEXT("out_format"), GetStr(Params, TEXT("out_format")));
		SetIfNotEmpty(Body, TEXT("rig_type"), GetStr(Params, TEXT("rig_type")));
		SetIfNotEmpty(Body, TEXT("spec"), GetStr(Params, TEXT("spec")));
		SetIfNotEmpty(Body, TEXT("model_version"), GetStr(Params, TEXT("model_version")));
	}

	static void BuildAnimateRetargetBody(const TSharedPtr<FJsonObject>& Body, const TSharedPtr<FJsonObject>& Params)
	{
		SetIfNotEmpty(Body, TEXT("original_model_task_id"), GetStr(Params, TEXT("original_model_task_id")));
		SetIfNotEmpty(Body, TEXT("out_format"), GetStr(Params, TEXT("out_format")));
		SetIfNotEmpty(Body, TEXT("animation"), GetStr(Params, TEXT("animation")));

		if (Params->HasField(TEXT("animations")))
			Body->SetArrayField(TEXT("animations"), Params->GetArrayField(TEXT("animations")));

		if (Params->HasField(TEXT("bake_animation")))
			Body->SetBoolField(TEXT("bake_animation"), GetBool(Params, TEXT("bake_animation"), true));
		if (Params->HasField(TEXT("export_with_geometry")))
			Body->SetBoolField(TEXT("export_with_geometry"), GetBool(Params, TEXT("export_with_geometry"), true));
		if (Params->HasField(TEXT("animate_in_place")))
			Body->SetBoolField(TEXT("animate_in_place"), GetBool(Params, TEXT("animate_in_place")));
	}

	static void BuildStylizeBody(const TSharedPtr<FJsonObject>& Body, const TSharedPtr<FJsonObject>& Params)
	{
		SetIfNotEmpty(Body, TEXT("original_model_task_id"), GetStr(Params, TEXT("original_model_task_id")));
		SetIfNotEmpty(Body, TEXT("style"), GetStr(Params, TEXT("style")));
		if (Params->HasField(TEXT("block_size")))
			Body->SetNumberField(TEXT("block_size"), FMath::Clamp(GetInt(Params, TEXT("block_size"), 80), 32, 128));
	}

	static void BuildConvertBody(const TSharedPtr<FJsonObject>& Body, const TSharedPtr<FJsonObject>& Params)
	{
		SetIfNotEmpty(Body, TEXT("original_model_task_id"), GetStr(Params, TEXT("original_model_task_id")));
		SetIfNotEmpty(Body, TEXT("format"), GetStr(Params, TEXT("format")));

		if (Params->HasField(TEXT("quad")))
			Body->SetBoolField(TEXT("quad"), GetBool(Params, TEXT("quad")));
		if (Params->HasField(TEXT("force_symmetry")))
			Body->SetBoolField(TEXT("force_symmetry"), GetBool(Params, TEXT("force_symmetry")));
		if (Params->HasField(TEXT("face_limit")))
			Body->SetNumberField(TEXT("face_limit"), GetInt(Params, TEXT("face_limit"), 10000));
		if (Params->HasField(TEXT("flatten_bottom")))
			Body->SetBoolField(TEXT("flatten_bottom"), GetBool(Params, TEXT("flatten_bottom")));
		if (Params->HasField(TEXT("flatten_bottom_threshold")))
			Body->SetNumberField(TEXT("flatten_bottom_threshold"), Params->GetNumberField(TEXT("flatten_bottom_threshold")));
		if (Params->HasField(TEXT("texture_size")))
			Body->SetNumberField(TEXT("texture_size"), GetInt(Params, TEXT("texture_size"), 4096));
		SetIfNotEmpty(Body, TEXT("texture_format"), GetStr(Params, TEXT("texture_format")));
		if (Params->HasField(TEXT("pivot_to_center_bottom")))
			Body->SetBoolField(TEXT("pivot_to_center_bottom"), GetBool(Params, TEXT("pivot_to_center_bottom")));
		if (Params->HasField(TEXT("scale_factor")))
			Body->SetNumberField(TEXT("scale_factor"), Params->GetNumberField(TEXT("scale_factor")));
		if (Params->HasField(TEXT("with_animation")))
			Body->SetBoolField(TEXT("with_animation"), GetBool(Params, TEXT("with_animation")));
		if (Params->HasField(TEXT("pack_uv")))
			Body->SetBoolField(TEXT("pack_uv"), GetBool(Params, TEXT("pack_uv")));
		if (Params->HasField(TEXT("bake")))
			Body->SetBoolField(TEXT("bake"), GetBool(Params, TEXT("bake")));
		if (Params->HasField(TEXT("part_names")))
			Body->SetArrayField(TEXT("part_names"), Params->GetArrayField(TEXT("part_names")));
		if (Params->HasField(TEXT("animate_in_place")))
			Body->SetBoolField(TEXT("animate_in_place"), GetBool(Params, TEXT("animate_in_place")));
		SetIfNotEmpty(Body, TEXT("export_orientation"), GetStr(Params, TEXT("export_orientation")));
		if (Params->HasField(TEXT("export_vertex_colors")))
			Body->SetBoolField(TEXT("export_vertex_colors"), GetBool(Params, TEXT("export_vertex_colors")));
		SetIfNotEmpty(Body, TEXT("fbx_preset"), GetStr(Params, TEXT("fbx_preset")));
	}

	// ── Schema builders ──────────────────────────────────────────────

	static TSharedPtr<FJsonObject> BuildTextToModelSchema()
	{
		TMap<FString, TSharedPtr<FJsonObject>> Props;
		Props.Add(TEXT("prompt"), SchemaString(TEXT("Describe the 3D model (max 1024 chars)")));
		Props.Add(TEXT("negative_prompt"), SchemaString(TEXT("What to avoid (max 255 chars)")));
		Props.Add(TEXT("model_version"), SchemaString(TEXT("Model version"),
			{TEXT("P1-20260311"), TEXT("Turbo-v1.0-20250506"), TEXT("v3.1-20260211"), TEXT("v3.0-20250812"),
			 TEXT("v2.5-20250123"), TEXT("v2.0-20240919"), TEXT("v1.4-20240625")}, TEXT("v2.5-20250123")));
		Props.Add(TEXT("face_limit"), SchemaInt(TEXT("Max face count (adaptive if unset)")));
		Props.Add(TEXT("texture"), SchemaBool(TEXT("Enable texturing"), true));
		Props.Add(TEXT("pbr"), SchemaBool(TEXT("Enable PBR materials"), true));
		Props.Add(TEXT("texture_quality"), SchemaString(TEXT("Texture quality"), {TEXT("standard"), TEXT("detailed")}, TEXT("standard")));
		Props.Add(TEXT("auto_size"), SchemaBool(TEXT("Scale to real-world dimensions (meters)")));
		Props.Add(TEXT("quad"), SchemaBool(TEXT("Quad mesh output (forces FBX)")));
		Props.Add(TEXT("smart_low_poly"), SchemaBool(TEXT("Hand-crafted low-poly topology")));
		Props.Add(TEXT("generate_parts"), SchemaBool(TEXT("Segmented parts (requires texture=false, pbr=false)")));
		Props.Add(TEXT("geometry_quality"), SchemaString(TEXT("Geometry quality (v3.0+)"), {TEXT("standard"), TEXT("detailed")}, TEXT("standard")));
		Props.Add(TEXT("export_uv"), SchemaBool(TEXT("Perform UV unwrapping during generation"), true));
		Props.Add(TEXT("compress"), SchemaString(TEXT("Compression type"), {TEXT(""), TEXT("geometry")}));
		Props.Add(TEXT("model_seed"), SchemaInt(TEXT("Random seed for geometry")));
		Props.Add(TEXT("texture_seed"), SchemaInt(TEXT("Random seed for textures")));
		Props.Add(TEXT("image_seed"), SchemaInt(TEXT("Random seed for prompt-to-image stage")));
		return BuildSchema(Props, {TEXT("prompt")});
	}

	static TSharedPtr<FJsonObject> BuildImageToModelSchema()
	{
		TMap<FString, TSharedPtr<FJsonObject>> Props;
		Props.Add(TEXT("image_url"), SchemaString(TEXT("Image URL (jpg/jpeg/png, max 20MB)")));
		Props.Add(TEXT("file_token"), SchemaString(TEXT("Image file_token from Tripo upload API")));
		Props.Add(TEXT("model_version"), SchemaString(TEXT("Model version"),
			{TEXT("P1-20260311"), TEXT("Turbo-v1.0-20250506"), TEXT("v3.1-20260211"), TEXT("v3.0-20250812"),
			 TEXT("v2.5-20250123"), TEXT("v2.0-20240919"), TEXT("v1.4-20240625")}, TEXT("v2.5-20250123")));
		Props.Add(TEXT("enable_image_autofix"), SchemaBool(TEXT("Optimize input image for better results")));
		Props.Add(TEXT("orientation"), SchemaString(TEXT("Model orientation"), {TEXT("default"), TEXT("align_image")}, TEXT("default")));
		Props.Add(TEXT("face_limit"), SchemaInt(TEXT("Max face count")));
		Props.Add(TEXT("texture"), SchemaBool(TEXT("Enable texturing"), true));
		Props.Add(TEXT("pbr"), SchemaBool(TEXT("Enable PBR materials"), true));
		Props.Add(TEXT("texture_quality"), SchemaString(TEXT("Texture quality"), {TEXT("standard"), TEXT("detailed")}, TEXT("standard")));
		Props.Add(TEXT("texture_alignment"), SchemaString(TEXT("Texture alignment priority"),
			{TEXT("original_image"), TEXT("geometry")}, TEXT("original_image")));
		Props.Add(TEXT("auto_size"), SchemaBool(TEXT("Scale to real-world dimensions")));
		Props.Add(TEXT("quad"), SchemaBool(TEXT("Quad mesh output")));
		Props.Add(TEXT("smart_low_poly"), SchemaBool(TEXT("Hand-crafted low-poly topology")));
		Props.Add(TEXT("generate_parts"), SchemaBool(TEXT("Segmented parts")));
		Props.Add(TEXT("geometry_quality"), SchemaString(TEXT("Geometry quality (v3.0+)"), {TEXT("standard"), TEXT("detailed")}, TEXT("standard")));
		Props.Add(TEXT("export_uv"), SchemaBool(TEXT("Perform UV unwrapping"), true));
		Props.Add(TEXT("compress"), SchemaString(TEXT("Compression type"), {TEXT(""), TEXT("geometry")}));
		Props.Add(TEXT("model_seed"), SchemaInt(TEXT("Random seed for geometry")));
		Props.Add(TEXT("texture_seed"), SchemaInt(TEXT("Random seed for textures")));
		return BuildSchema(Props);
	}

	static TSharedPtr<FJsonObject> BuildMultiviewToModelSchema()
	{
		TMap<FString, TSharedPtr<FJsonObject>> Props;
		Props.Add(TEXT("image_urls"), SchemaStringArray(TEXT("Exactly 4 image URLs [front, left, back, right]. Use empty string to omit a view (front required).")));
		Props.Add(TEXT("model_version"), SchemaString(TEXT("Model version"),
			{TEXT("P1-20260311"), TEXT("v3.1-20260211"), TEXT("v3.0-20250812"),
			 TEXT("v2.5-20250123"), TEXT("v2.0-20240919")}, TEXT("v2.5-20250123")));
		Props.Add(TEXT("orientation"), SchemaString(TEXT("Model orientation"), {TEXT("default"), TEXT("align_image")}, TEXT("default")));
		Props.Add(TEXT("face_limit"), SchemaInt(TEXT("Max face count")));
		Props.Add(TEXT("texture"), SchemaBool(TEXT("Enable texturing"), true));
		Props.Add(TEXT("pbr"), SchemaBool(TEXT("Enable PBR materials"), true));
		Props.Add(TEXT("texture_quality"), SchemaString(TEXT("Texture quality"), {TEXT("standard"), TEXT("detailed")}, TEXT("standard")));
		Props.Add(TEXT("texture_alignment"), SchemaString(TEXT("Texture alignment priority"),
			{TEXT("original_image"), TEXT("geometry")}, TEXT("original_image")));
		Props.Add(TEXT("auto_size"), SchemaBool(TEXT("Scale to real-world dimensions")));
		Props.Add(TEXT("quad"), SchemaBool(TEXT("Quad mesh output")));
		Props.Add(TEXT("smart_low_poly"), SchemaBool(TEXT("Hand-crafted low-poly topology")));
		Props.Add(TEXT("generate_parts"), SchemaBool(TEXT("Segmented parts")));
		Props.Add(TEXT("geometry_quality"), SchemaString(TEXT("Geometry quality (v3.0+)"), {TEXT("standard"), TEXT("detailed")}));
		Props.Add(TEXT("export_uv"), SchemaBool(TEXT("Perform UV unwrapping"), true));
		Props.Add(TEXT("compress"), SchemaString(TEXT("Compression type"), {TEXT(""), TEXT("geometry")}));
		Props.Add(TEXT("model_seed"), SchemaInt(TEXT("Random seed for geometry")));
		Props.Add(TEXT("texture_seed"), SchemaInt(TEXT("Random seed for textures")));
		return BuildSchema(Props, {TEXT("image_urls")});
	}

	static TSharedPtr<FJsonObject> BuildRefineModelSchema()
	{
		TMap<FString, TSharedPtr<FJsonObject>> Props;
		Props.Add(TEXT("draft_model_task_id"), SchemaString(TEXT("task_id of a succeeded draft generation (v1.4 only)")));
		return BuildSchema(Props, {TEXT("draft_model_task_id")});
	}

	static TSharedPtr<FJsonObject> BuildTextureModelSchema()
	{
		TMap<FString, TSharedPtr<FJsonObject>> Props;
		Props.Add(TEXT("original_model_task_id"), SchemaString(TEXT("task_id of the model to texture")));
		Props.Add(TEXT("texture_prompt_text"), SchemaString(TEXT("Text guidance for texture generation")));
		Props.Add(TEXT("texture_prompt_image_url"), SchemaString(TEXT("Image URL for texture guidance")));
		Props.Add(TEXT("style_image_url"), SchemaString(TEXT("Style reference image URL")));
		Props.Add(TEXT("texture"), SchemaBool(TEXT("Enable base texturing"), true));
		Props.Add(TEXT("pbr"), SchemaBool(TEXT("Enable PBR materials"), true));
		Props.Add(TEXT("texture_quality"), SchemaString(TEXT("Texture quality"), {TEXT("standard"), TEXT("detailed")}, TEXT("standard")));
		Props.Add(TEXT("texture_alignment"), SchemaString(TEXT("Texture alignment"),
			{TEXT("original_image"), TEXT("geometry")}, TEXT("original_image")));
		Props.Add(TEXT("bake"), SchemaBool(TEXT("Bake material effects into base textures"), true));
		Props.Add(TEXT("model_version"), SchemaString(TEXT("Model version"), {TEXT("v2.5-20250123"), TEXT("v3.0-20250812")}, TEXT("v2.5-20250123")));
		Props.Add(TEXT("texture_seed"), SchemaInt(TEXT("Random seed for textures")));
		Props.Add(TEXT("compress"), SchemaString(TEXT("Compression type"), {TEXT(""), TEXT("geometry")}));
		Props.Add(TEXT("part_names"), SchemaStringArray(TEXT("Specific parts to texture (from mesh_segmentation)")));
		return BuildSchema(Props, {TEXT("original_model_task_id")});
	}

	static TSharedPtr<FJsonObject> BuildTextToImageSchema()
	{
		TMap<FString, TSharedPtr<FJsonObject>> Props;
		Props.Add(TEXT("prompt"), SchemaString(TEXT("Image description (max 1024 chars)")));
		Props.Add(TEXT("negative_prompt"), SchemaString(TEXT("What to avoid (max 255 chars)")));
		return BuildSchema(Props, {TEXT("prompt")});
	}

	static TSharedPtr<FJsonObject> BuildGenerateImageSchema()
	{
		TMap<FString, TSharedPtr<FJsonObject>> Props;
		Props.Add(TEXT("prompt"), SchemaString(TEXT("Image description (max 1024 chars)")));
		Props.Add(TEXT("model_version"), SchemaString(TEXT("Image model"),
			{TEXT("flux.1_kontext_pro"), TEXT("flux.1_dev"), TEXT("gpt_4o"),
			 TEXT("gemini_2.5_flash_image_preview"), TEXT("z_image")}, TEXT("flux.1_kontext_pro")));
		Props.Add(TEXT("file_url"), SchemaString(TEXT("Reference image URL for image-guided generation")));
		Props.Add(TEXT("t_pose"), SchemaBool(TEXT("Transform object to T-pose")));
		Props.Add(TEXT("sketch_to_render"), SchemaBool(TEXT("Render a sketch into finished image")));
		return BuildSchema(Props, {TEXT("prompt")});
	}

	static TSharedPtr<FJsonObject> BuildOriginalTaskSchema()
	{
		TMap<FString, TSharedPtr<FJsonObject>> Props;
		Props.Add(TEXT("original_model_task_id"), SchemaString(TEXT("task_id of the model")));
		return BuildSchema(Props, {TEXT("original_model_task_id")});
	}

	static TSharedPtr<FJsonObject> BuildMeshCompletionSchema()
	{
		TMap<FString, TSharedPtr<FJsonObject>> Props;
		Props.Add(TEXT("original_model_task_id"), SchemaString(TEXT("task_id from mesh_segmentation")));
		Props.Add(TEXT("part_names"), SchemaStringArray(TEXT("Part names to complete (default: all)")));
		return BuildSchema(Props, {TEXT("original_model_task_id")});
	}

	static TSharedPtr<FJsonObject> BuildHighPolyToLowPolySchema()
	{
		TMap<FString, TSharedPtr<FJsonObject>> Props;
		Props.Add(TEXT("original_model_task_id"), SchemaString(TEXT("task_id of the model")));
		Props.Add(TEXT("quad"), SchemaBool(TEXT("Output quad or triangle faces")));
		Props.Add(TEXT("face_limit"), SchemaInt(TEXT("Target face count"), 1000, 20000));
		Props.Add(TEXT("bake"), SchemaBool(TEXT("Bake textures"), true));
		Props.Add(TEXT("part_names"), SchemaStringArray(TEXT("Part names from segmentation")));
		return BuildSchema(Props, {TEXT("original_model_task_id")});
	}

	static TSharedPtr<FJsonObject> BuildAnimateRigSchema()
	{
		TMap<FString, TSharedPtr<FJsonObject>> Props;
		Props.Add(TEXT("original_model_task_id"), SchemaString(TEXT("task_id of the model to rig")));
		Props.Add(TEXT("out_format"), SchemaString(TEXT("Output format"), {TEXT("glb"), TEXT("fbx")}, TEXT("glb")));
		Props.Add(TEXT("rig_type"), SchemaString(TEXT("Skeleton type (from prerigcheck)"),
			{TEXT("biped"), TEXT("quadruped"), TEXT("hexapod"), TEXT("octopod"),
			 TEXT("avian"), TEXT("serpentine"), TEXT("aquatic")}, TEXT("biped")));
		Props.Add(TEXT("spec"), SchemaString(TEXT("Rigging method"), {TEXT("tripo"), TEXT("mixamo")}, TEXT("tripo")));
		Props.Add(TEXT("model_version"), SchemaString(TEXT("Rig model version"),
			{TEXT("v2.5-20260210"), TEXT("v2.0-20250506"), TEXT("v1.0-20240301")}, TEXT("v1.0-20240301")));
		return BuildSchema(Props, {TEXT("original_model_task_id")});
	}

	static TSharedPtr<FJsonObject> BuildAnimateRetargetSchema()
	{
		TMap<FString, TSharedPtr<FJsonObject>> Props;
		Props.Add(TEXT("original_model_task_id"), SchemaString(TEXT("task_id of a rig task")));
		Props.Add(TEXT("animation"), SchemaString(TEXT("Preset animation name (e.g. 'preset:walk', 'preset:run')"),
			{TEXT("preset:idle"), TEXT("preset:walk"), TEXT("preset:run"), TEXT("preset:dive"),
			 TEXT("preset:climb"), TEXT("preset:jump"), TEXT("preset:slash"), TEXT("preset:shoot"),
			 TEXT("preset:hurt"), TEXT("preset:fall"), TEXT("preset:turn"),
			 TEXT("preset:quadruped:walk"), TEXT("preset:hexapod:walk"),
			 TEXT("preset:octopod:walk"), TEXT("preset:serpentine:march"), TEXT("preset:aquatic:march")}));
		Props.Add(TEXT("animations"), SchemaStringArray(TEXT("Array of preset animations (max 5)")));
		Props.Add(TEXT("out_format"), SchemaString(TEXT("Output format"), {TEXT("glb"), TEXT("fbx")}, TEXT("glb")));
		Props.Add(TEXT("bake_animation"), SchemaBool(TEXT("Bake animation into model (GLB only)"), true));
		Props.Add(TEXT("export_with_geometry"), SchemaBool(TEXT("Include geometry in output"), true));
		Props.Add(TEXT("animate_in_place"), SchemaBool(TEXT("Animate without root motion")));
		return BuildSchema(Props, {TEXT("original_model_task_id")});
	}

	static TSharedPtr<FJsonObject> BuildStylizeSchema()
	{
		TMap<FString, TSharedPtr<FJsonObject>> Props;
		Props.Add(TEXT("original_model_task_id"), SchemaString(TEXT("task_id of the model")));
		Props.Add(TEXT("style"), SchemaString(TEXT("Stylization effect"),
			{TEXT("lego"), TEXT("voxel"), TEXT("voronoi"), TEXT("minecraft")}));
		Props.Add(TEXT("block_size"), SchemaInt(TEXT("Grid size for minecraft style"), 32, 128, 80));
		return BuildSchema(Props, {TEXT("original_model_task_id"), TEXT("style")});
	}

	static TSharedPtr<FJsonObject> BuildConvertSchema()
	{
		TMap<FString, TSharedPtr<FJsonObject>> Props;
		Props.Add(TEXT("original_model_task_id"), SchemaString(TEXT("task_id of the model")));
		Props.Add(TEXT("format"), SchemaString(TEXT("Target format"),
			{TEXT("GLTF"), TEXT("USDZ"), TEXT("FBX"), TEXT("OBJ"), TEXT("STL"), TEXT("3MF")}));
		Props.Add(TEXT("quad"), SchemaBool(TEXT("Enable quad retopology")));
		Props.Add(TEXT("force_symmetry"), SchemaBool(TEXT("Force symmetry (with quad)")));
		Props.Add(TEXT("face_limit"), SchemaInt(TEXT("Face count limit"), 0, 0, 10000));
		Props.Add(TEXT("flatten_bottom"), SchemaBool(TEXT("Flatten bottom of model")));
		Props.Add(TEXT("flatten_bottom_threshold"), SchemaInt(TEXT("Bottom flatten depth"), 0, 0, 0));
		Props.Add(TEXT("texture_size"), SchemaInt(TEXT("Texture size in pixels"), 0, 0, 4096));
		Props.Add(TEXT("texture_format"), SchemaString(TEXT("Texture format"),
			{TEXT("BMP"), TEXT("DPX"), TEXT("HDR"), TEXT("JPEG"), TEXT("OPEN_EXR"),
			 TEXT("PNG"), TEXT("TARGA"), TEXT("TIFF"), TEXT("WEBP")}, TEXT("JPEG")));
		Props.Add(TEXT("pivot_to_center_bottom"), SchemaBool(TEXT("Set pivot to center bottom")));
		Props.Add(TEXT("scale_factor"), SchemaInt(TEXT("Object scale factor"), 0, 0, 1));
		Props.Add(TEXT("with_animation"), SchemaBool(TEXT("Include animation data"), true));
		Props.Add(TEXT("pack_uv"), SchemaBool(TEXT("Combine UV islands into one layout")));
		Props.Add(TEXT("bake"), SchemaBool(TEXT("Bake material effects into textures"), true));
		Props.Add(TEXT("part_names"), SchemaStringArray(TEXT("Part names from segmentation")));
		Props.Add(TEXT("animate_in_place"), SchemaBool(TEXT("Animate in fixed place")));
		Props.Add(TEXT("export_orientation"), SchemaString(TEXT("Model facing direction"),
			{TEXT("+x"), TEXT("-x"), TEXT("+y"), TEXT("-y")}, TEXT("+x")));
		Props.Add(TEXT("export_vertex_colors"), SchemaBool(TEXT("Include vertex colors (OBJ/GLTF only)")));
		Props.Add(TEXT("fbx_preset"), SchemaString(TEXT("FBX export preset"),
			{TEXT("blender"), TEXT("3dsmax"), TEXT("mixamo")}, TEXT("blender")));
		return BuildSchema(Props, {TEXT("original_model_task_id"), TEXT("format")});
	}
};

// ── Auto-register ────────────────────────────────────────────────────

REGISTER_GENERATIVE_PROVIDER(FTripoProvider);
