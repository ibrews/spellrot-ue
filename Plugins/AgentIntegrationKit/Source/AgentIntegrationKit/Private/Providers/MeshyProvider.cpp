// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Providers/GenerativeProvider.h"
#include "Providers/GenerativeProviderRegistry.h"
#include "Serialization/JsonSerializer.h"

namespace MeshyHelpers {
inline void SetIfNotEmpty(const TSharedPtr<FJsonObject>& Body, const FString& Key, const FString& Value)
{
	if (!Value.IsEmpty()) Body->SetStringField(Key, Value);
}
inline FString GetStr(const TSharedPtr<FJsonObject>& J, const FString& Key)
{
	FString V;
	if (J.IsValid()) J->TryGetStringField(Key, V);
	return V;
}
inline int32 GetInt(const TSharedPtr<FJsonObject>& J, const FString& Key, int32 Default = 0)
{
	int32 V = Default;
	if (J.IsValid()) J->TryGetNumberField(Key, V);
	return V;
}
inline bool GetBool(const TSharedPtr<FJsonObject>& J, const FString& Key, bool Default = false)
{
	bool V = Default;
	if (J.IsValid()) J->TryGetBoolField(Key, V);
	return V;
}
} // namespace MeshyHelpers

// Helper: add common mesh generation fields to request body
static void AddCommonMeshFields(const TSharedPtr<FJsonObject>& Body, const TSharedPtr<FJsonObject>& Params)
{
	MeshyHelpers::SetIfNotEmpty(Body, TEXT("ai_model"), MeshyHelpers::GetStr(Params, TEXT("ai_model")));
	MeshyHelpers::SetIfNotEmpty(Body, TEXT("topology"), MeshyHelpers::GetStr(Params, TEXT("topology")));
	MeshyHelpers::SetIfNotEmpty(Body, TEXT("symmetry_mode"), MeshyHelpers::GetStr(Params, TEXT("symmetry_mode")));
	MeshyHelpers::SetIfNotEmpty(Body, TEXT("pose_mode"), MeshyHelpers::GetStr(Params, TEXT("pose_mode")));
	MeshyHelpers::SetIfNotEmpty(Body, TEXT("model_type"), MeshyHelpers::GetStr(Params, TEXT("model_type")));

	if (Params->HasField(TEXT("should_remesh")))
		Body->SetBoolField(TEXT("should_remesh"), MeshyHelpers::GetBool(Params, TEXT("should_remesh")));
	if (Params->HasField(TEXT("target_polycount")))
		Body->SetNumberField(TEXT("target_polycount"),
			FMath::Clamp(MeshyHelpers::GetInt(Params, TEXT("target_polycount"), 30000), 100, 300000));
	if (Params->HasField(TEXT("should_texture")))
		Body->SetBoolField(TEXT("should_texture"), MeshyHelpers::GetBool(Params, TEXT("should_texture"), true));
	if (Params->HasField(TEXT("enable_pbr")))
		Body->SetBoolField(TEXT("enable_pbr"), MeshyHelpers::GetBool(Params, TEXT("enable_pbr")));
	if (Params->HasField(TEXT("save_pre_remeshed_model")))
		Body->SetBoolField(TEXT("save_pre_remeshed_model"), MeshyHelpers::GetBool(Params, TEXT("save_pre_remeshed_model")));
	if (Params->HasField(TEXT("image_enhancement")))
		Body->SetBoolField(TEXT("image_enhancement"), MeshyHelpers::GetBool(Params, TEXT("image_enhancement"), true));
	if (Params->HasField(TEXT("remove_lighting")))
		Body->SetBoolField(TEXT("remove_lighting"), MeshyHelpers::GetBool(Params, TEXT("remove_lighting"), true));

	// Texture guidance
	MeshyHelpers::SetIfNotEmpty(Body, TEXT("texture_prompt"), MeshyHelpers::GetStr(Params, TEXT("texture_prompt")));
	MeshyHelpers::SetIfNotEmpty(Body, TEXT("texture_image_url"), MeshyHelpers::GetStr(Params, TEXT("texture_image_url")));

	// Target formats
	if (Params->HasField(TEXT("target_formats")))
	{
		Body->SetArrayField(TEXT("target_formats"), Params->GetArrayField(TEXT("target_formats")));
	}
}

// Helper: parse standard Meshy task response (model_urls, thumbnail, status, etc.)
static FGenerativeJob ParseModelTaskResponse(const TSharedPtr<FJsonObject>& Json, const FString& JobId = TEXT(""))
{
	if (!Json.IsValid())
		return FGenerativeJob::MakeFail(TEXT("Empty response"));

	FString Id = MeshyHelpers::GetStr(Json, TEXT("id"));
	if (Id.IsEmpty()) Id = JobId;

	const FString StatusStr = MeshyHelpers::GetStr(Json, TEXT("status"));
	const EGenerativeJobStatus Status = FGenerativeProviderBase::ParseStatus(StatusStr);

	FGenerativeJob Job;
	Job.JobId = Id;
	Job.Status = Status;
	Job.Progress = MeshyHelpers::GetInt(Json, TEXT("progress"));
	Job.RawResponse = Json;

	if (Status == EGenerativeJobStatus::Failed || Status == EGenerativeJobStatus::Cancelled)
	{
		if (Json->HasTypedField<EJson::Object>(TEXT("task_error")))
		{
			Job.ErrorMessage = MeshyHelpers::GetStr(Json->GetObjectField(TEXT("task_error")), TEXT("message"));
		}
		return Job;
	}

	if (Status == EGenerativeJobStatus::Succeeded)
	{
		Job.ThumbnailUrl = MeshyHelpers::GetStr(Json, TEXT("thumbnail_url"));

		// model_urls is an object with format keys
		if (Json->HasTypedField<EJson::Object>(TEXT("model_urls")))
		{
			const TSharedPtr<FJsonObject>& ModelUrls = Json->GetObjectField(TEXT("model_urls"));
			for (const auto& Pair : ModelUrls->Values)
			{
				FString Url;
				if (Pair.Value->TryGetString(Url) && !Url.IsEmpty())
				{
					Job.ExtraUrls.Add(Pair.Key, Url);
				}
			}
			// Primary result: prefer glb, then fbx, then first available
			if (Job.ExtraUrls.Contains(TEXT("glb")))
				Job.ResultUrl = Job.ExtraUrls[TEXT("glb")];
			else if (Job.ExtraUrls.Contains(TEXT("fbx")))
				Job.ResultUrl = Job.ExtraUrls[TEXT("fbx")];
			else if (Job.ExtraUrls.Num() > 0)
			{
				for (const auto& P : Job.ExtraUrls) { Job.ResultUrl = P.Value; break; }
			}
		}

		// image_urls (for text-to-image / image-to-image)
		if (Json->HasField(TEXT("image_urls")))
		{
			const TArray<TSharedPtr<FJsonValue>>& Arr = Json->GetArrayField(TEXT("image_urls"));
			for (const auto& V : Arr)
			{
				FString Url;
				if (V->TryGetString(Url)) Job.ImageUrls.Add(Url);
			}
			if (Job.ResultUrl.IsEmpty() && Job.ImageUrls.Num() > 0)
				Job.ResultUrl = Job.ImageUrls[0];
		}

		// Rigging response has result.rigged_character_*_url
		if (Json->HasTypedField<EJson::Object>(TEXT("result")))
		{
			const TSharedPtr<FJsonObject>& ResultObj = Json->GetObjectField(TEXT("result"));
			for (const auto& Pair : ResultObj->Values)
			{
				FString Url;
				if (Pair.Value->TryGetString(Url) && !Url.IsEmpty())
				{
					Job.ExtraUrls.Add(Pair.Key, Url);
				}
				// basic_animations is a nested object
				else if (Pair.Value->Type == EJson::Object)
				{
					const TSharedPtr<FJsonObject>& SubObj = Pair.Value->AsObject();
					for (const auto& SubPair : SubObj->Values)
					{
						FString SubUrl;
						if (SubPair.Value->TryGetString(SubUrl) && !SubUrl.IsEmpty())
						{
							Job.ExtraUrls.Add(SubPair.Key, SubUrl);
						}
					}
				}
			}
			// Set primary URL for rig results
			if (Job.ResultUrl.IsEmpty())
			{
				if (Job.ExtraUrls.Contains(TEXT("rigged_character_fbx_url")))
					Job.ResultUrl = Job.ExtraUrls[TEXT("rigged_character_fbx_url")];
				else if (Job.ExtraUrls.Contains(TEXT("animation_fbx_url")))
					Job.ResultUrl = Job.ExtraUrls[TEXT("animation_fbx_url")];
				else if (Job.ExtraUrls.Contains(TEXT("rigged_character_glb_url")))
					Job.ResultUrl = Job.ExtraUrls[TEXT("rigged_character_glb_url")];
				else if (Job.ExtraUrls.Contains(TEXT("animation_glb_url")))
					Job.ResultUrl = Job.ExtraUrls[TEXT("animation_glb_url")];
			}
		}
	}

	return Job;
}

// ═════════════════════════════════════════════════════════════════════
// MeshyProvider — all 10+ Meshy API endpoints
// ═════════════════════════════════════════════════════════════════════

class FMeshyProvider : public FGenerativeProviderBase
{
public:
	FString GetId() const override { return TEXT("meshy"); }
	FString GetDisplayName() const override { return TEXT("Meshy AI"); }
	FString GetWebsite() const override { return TEXT("https://meshy.ai"); }
	FString GetDirectBaseUrl() const override { return TEXT("https://api.meshy.ai"); }
	FString GetProxyPath() const override { return TEXT("/api/proxy/meshy"); }
	FString GetApiKeySettingName() const override { return TEXT("MeshyApiKey"); }

	// ── Actions ──────────────────────────────────────────────────────

	TArray<FProviderActionDescriptor> GetActions() const override
	{
		return {
			// 3D Generation
			{TEXT("text_to_3d"), TEXT("Generate 3D model from text (preview stage, untextured mesh). "
				"Follow with 'refine' to add textures. Models: meshy-5, meshy-6, latest. "
				"Cost: 20 credits (meshy-6), 5 credits (others). Returns job_id."),
				{TEXT("text")}, {TEXT("model")}, BuildTextTo3DSchema(), TEXT("5-20 credits")},

			{TEXT("refine"), TEXT("Add textures/PBR to a preview model. Requires preview_task_id from a "
				"succeeded text_to_3d job. Optional: texture_prompt, texture_image_url, enable_pbr. Cost: 10 credits."),
				{TEXT("job_ref")}, {TEXT("model")}, BuildRefineSchema(), TEXT("10 credits")},

			{TEXT("image_to_3d"), TEXT("Generate textured 3D model from a single image (URL or base64 data URI). "
				"Supports jpg/jpeg/png. Cost: 20-30 credits."),
				{TEXT("image")}, {TEXT("model")}, BuildImageTo3DSchema(), TEXT("20-30 credits")},

			{TEXT("multi_image_to_3d"), TEXT("Generate 3D model from 1-4 images of the same object from different angles. "
				"Better geometry than single image. Images as URLs or base64 data URIs."),
				{TEXT("image")}, {TEXT("model")}, BuildMultiImageTo3DSchema(), TEXT("20-30 credits")},

			// Post-Processing
			{TEXT("retexture"), TEXT("Apply new textures to an existing 3D model. Provide model via "
				"input_task_id (from prior Meshy task) or model_url (external GLB/FBX/OBJ/STL). "
				"Guide style via text_style_prompt or image_style_url."),
				{TEXT("model"), TEXT("text")}, {TEXT("model")}, BuildRetextureSchema(), TEXT("10+ credits")},

			{TEXT("remesh"), TEXT("Retopologize a 3D model: change polycount, topology (quad/triangle), "
				"format, or resize. Provide via input_task_id or model_url. Supports blend format."),
				{TEXT("model")}, {TEXT("model")}, BuildRemeshSchema(), TEXT("varies")},

			// Character Pipeline
			{TEXT("rig"), TEXT("Auto-rig a textured humanoid 3D model (max 300k faces). Returns rigged FBX/GLB "
				"with basic walk/run animations. Provide via input_task_id or model_url (GLB only)."),
				{TEXT("model")}, {TEXT("model")}, BuildRigSchema(), TEXT("varies")},

			{TEXT("animate"), TEXT("Apply animation to a rigged character. Requires rig_task_id from a succeeded "
				"'rig' job and action_id (animation identifier). Optional post-processing: change_fps, fbx2usdz, extract_armature."),
				{TEXT("job_ref")}, {TEXT("animation")}, BuildAnimateSchema(), TEXT("varies")},

			// 2D Generation
			{TEXT("text_to_image"), TEXT("Generate image from text. Models: nano-banana, nano-banana-pro. "
				"Supports multi-view output (3 angles for 3D reference) and pose modes (a-pose, t-pose)."),
				{TEXT("text")}, {TEXT("image")}, BuildTextToImageSchema(), TEXT("varies")},

			{TEXT("image_to_image"), TEXT("Transform/edit images based on text prompt and 1-5 reference images. "
				"Models: nano-banana, nano-banana-pro. Supports multi-view output."),
				{TEXT("image"), TEXT("text")}, {TEXT("image")}, BuildImageToImageSchema(), TEXT("varies")},

			// Utility
			{TEXT("balance"), TEXT("Check remaining Meshy API credits."),
				{}, {TEXT("data")}, nullptr, TEXT("free"), true},
		};
	}

	// ── Submit ───────────────────────────────────────────────────────

	FGenerativeJob Submit(const FString& ActionId, const TSharedPtr<FJsonObject>& Params) override
	{
		if (ActionId == TEXT("text_to_3d"))    return SubmitTextTo3D(Params);
		if (ActionId == TEXT("refine"))         return SubmitRefine(Params);
		if (ActionId == TEXT("image_to_3d"))    return SubmitImageTo3D(Params);
		if (ActionId == TEXT("multi_image_to_3d")) return SubmitMultiImageTo3D(Params);
		if (ActionId == TEXT("retexture"))      return SubmitRetexture(Params);
		if (ActionId == TEXT("remesh"))         return SubmitRemesh(Params);
		if (ActionId == TEXT("rig"))            return SubmitRig(Params);
		if (ActionId == TEXT("animate"))        return SubmitAnimate(Params);
		if (ActionId == TEXT("text_to_image"))  return SubmitTextToImage(Params);
		if (ActionId == TEXT("image_to_image")) return SubmitImageToImage(Params);
		if (ActionId == TEXT("balance"))        return SubmitBalance();
		return FGenerativeJob::MakeFail(FString::Printf(TEXT("Unknown Meshy action: %s"), *ActionId));
	}

	// ── CheckStatus ──────────────────────────────────────────────────

	FGenerativeJob CheckStatus(const FString& JobId, const FString& ActionId) override
	{
		const FString Path = GetStatusPath(ActionId, JobId);
		TSharedPtr<FJsonObject> Json = HttpGet(Path);
		if (!Json.IsValid())
			return FGenerativeJob::MakeFail(FString::Printf(TEXT("Failed to check status for %s/%s"), *ActionId, *JobId));

		auto Job = ParseModelTaskResponse(Json, JobId);
		Job.ProviderId = TEXT("meshy");
		Job.ActionId = ActionId;
		return Job;
	}

	int32 GetBalance() override
	{
		TSharedPtr<FJsonObject> Json = HttpGet(TEXT("/openapi/v1/balance"));
		if (Json.IsValid())
			return MeshyHelpers::GetInt(Json, TEXT("balance"));
		return -1;
	}

private:

	// ── Status path routing ──────────────────────────────────────────

	FString GetStatusPath(const FString& ActionId, const FString& JobId) const
	{
		if (ActionId == TEXT("text_to_3d") || ActionId == TEXT("refine"))
			return FString::Printf(TEXT("/openapi/v2/text-to-3d/%s"), *JobId);
		if (ActionId == TEXT("image_to_3d"))
			return FString::Printf(TEXT("/openapi/v1/image-to-3d/%s"), *JobId);
		if (ActionId == TEXT("multi_image_to_3d"))
			return FString::Printf(TEXT("/openapi/v1/multi-image-to-3d/%s"), *JobId);
		if (ActionId == TEXT("retexture"))
			return FString::Printf(TEXT("/openapi/v1/retexture/%s"), *JobId);
		if (ActionId == TEXT("remesh"))
			return FString::Printf(TEXT("/openapi/v1/remesh/%s"), *JobId);
		if (ActionId == TEXT("rig"))
			return FString::Printf(TEXT("/openapi/v1/rigging/%s"), *JobId);
		if (ActionId == TEXT("animate"))
			return FString::Printf(TEXT("/openapi/v1/animations/%s"), *JobId);
		if (ActionId == TEXT("text_to_image"))
			return FString::Printf(TEXT("/openapi/v1/text-to-image/%s"), *JobId);
		if (ActionId == TEXT("image_to_image"))
			return FString::Printf(TEXT("/openapi/v1/image-to-image/%s"), *JobId);
		// Fallback — try text-to-3d
		return FString::Printf(TEXT("/openapi/v2/text-to-3d/%s"), *JobId);
	}

	// ── Submit implementations ───────────────────────────────────────

	FGenerativeJob SubmitTextTo3D(const TSharedPtr<FJsonObject>& Params)
	{
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("mode"), TEXT("preview"));
		Body->SetStringField(TEXT("prompt"), MeshyHelpers::GetStr(Params, TEXT("prompt")).Left(600));
		MeshyHelpers::SetIfNotEmpty(Body, TEXT("negative_prompt"), MeshyHelpers::GetStr(Params, TEXT("negative_prompt")));
		AddCommonMeshFields(Body, Params);

		TSharedPtr<FJsonObject> Json = HttpPost(TEXT("/openapi/v2/text-to-3d"), Body);
		if (!Json.IsValid())
			return FGenerativeJob::MakeFail(TEXT("Failed to create text_to_3d preview task"));

		auto Job = FGenerativeJob::MakePending(MeshyHelpers::GetStr(Json, TEXT("result")));
		Job.ProviderId = TEXT("meshy");
		Job.ActionId = TEXT("text_to_3d");
		return Job;
	}

	FGenerativeJob SubmitRefine(const TSharedPtr<FJsonObject>& Params)
	{
		const FString PreviewTaskId = MeshyHelpers::GetStr(Params, TEXT("preview_task_id"));
		if (PreviewTaskId.IsEmpty())
			return FGenerativeJob::MakeFail(TEXT("refine requires preview_task_id"));

		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("mode"), TEXT("refine"));
		Body->SetStringField(TEXT("preview_task_id"), PreviewTaskId);
		if (Params->HasField(TEXT("enable_pbr")))
			Body->SetBoolField(TEXT("enable_pbr"), MeshyHelpers::GetBool(Params, TEXT("enable_pbr"), true));
		MeshyHelpers::SetIfNotEmpty(Body, TEXT("ai_model"), MeshyHelpers::GetStr(Params, TEXT("ai_model")));
		MeshyHelpers::SetIfNotEmpty(Body, TEXT("texture_prompt"), MeshyHelpers::GetStr(Params, TEXT("texture_prompt")));
		MeshyHelpers::SetIfNotEmpty(Body, TEXT("texture_image_url"), MeshyHelpers::GetStr(Params, TEXT("texture_image_url")));
		if (Params->HasField(TEXT("remove_lighting")))
			Body->SetBoolField(TEXT("remove_lighting"), MeshyHelpers::GetBool(Params, TEXT("remove_lighting"), true));
		if (Params->HasField(TEXT("target_formats")))
			Body->SetArrayField(TEXT("target_formats"), Params->GetArrayField(TEXT("target_formats")));

		TSharedPtr<FJsonObject> Json = HttpPost(TEXT("/openapi/v2/text-to-3d"), Body);
		if (!Json.IsValid())
			return FGenerativeJob::MakeFail(TEXT("Failed to create refine task"));

		auto Job = FGenerativeJob::MakePending(MeshyHelpers::GetStr(Json, TEXT("result")));
		Job.ProviderId = TEXT("meshy");
		Job.ActionId = TEXT("refine");
		return Job;
	}

	FGenerativeJob SubmitImageTo3D(const TSharedPtr<FJsonObject>& Params)
	{
		const FString ImageUrl = MeshyHelpers::GetStr(Params, TEXT("image_url"));
		if (ImageUrl.IsEmpty())
			return FGenerativeJob::MakeFail(TEXT("image_to_3d requires image_url"));

		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("image_url"), ImageUrl);
		AddCommonMeshFields(Body, Params);

		TSharedPtr<FJsonObject> Json = HttpPost(TEXT("/openapi/v1/image-to-3d"), Body);
		if (!Json.IsValid())
			return FGenerativeJob::MakeFail(TEXT("Failed to create image_to_3d task"));

		auto Job = FGenerativeJob::MakePending(MeshyHelpers::GetStr(Json, TEXT("result")));
		Job.ProviderId = TEXT("meshy");
		Job.ActionId = TEXT("image_to_3d");
		return Job;
	}

	FGenerativeJob SubmitMultiImageTo3D(const TSharedPtr<FJsonObject>& Params)
	{
		if (!Params->HasField(TEXT("image_urls")))
			return FGenerativeJob::MakeFail(TEXT("multi_image_to_3d requires image_urls array"));

		const auto& Arr = Params->GetArrayField(TEXT("image_urls"));
		if (Arr.Num() < 1 || Arr.Num() > 4)
			return FGenerativeJob::MakeFail(TEXT("image_urls must contain 1-4 images"));

		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetArrayField(TEXT("image_urls"), Arr);
		AddCommonMeshFields(Body, Params);

		TSharedPtr<FJsonObject> Json = HttpPost(TEXT("/openapi/v1/multi-image-to-3d"), Body);
		if (!Json.IsValid())
			return FGenerativeJob::MakeFail(TEXT("Failed to create multi_image_to_3d task"));

		auto Job = FGenerativeJob::MakePending(MeshyHelpers::GetStr(Json, TEXT("result")));
		Job.ProviderId = TEXT("meshy");
		Job.ActionId = TEXT("multi_image_to_3d");
		return Job;
	}

	FGenerativeJob SubmitRetexture(const TSharedPtr<FJsonObject>& Params)
	{
		const FString InputTaskId = MeshyHelpers::GetStr(Params, TEXT("input_task_id"));
		const FString ModelUrl = MeshyHelpers::GetStr(Params, TEXT("model_url"));
		if (InputTaskId.IsEmpty() && ModelUrl.IsEmpty())
			return FGenerativeJob::MakeFail(TEXT("retexture requires input_task_id or model_url"));

		const FString TextPrompt = MeshyHelpers::GetStr(Params, TEXT("text_style_prompt"));
		const FString ImageStyle = MeshyHelpers::GetStr(Params, TEXT("image_style_url"));
		if (TextPrompt.IsEmpty() && ImageStyle.IsEmpty())
			return FGenerativeJob::MakeFail(TEXT("retexture requires text_style_prompt or image_style_url"));

		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		MeshyHelpers::SetIfNotEmpty(Body, TEXT("input_task_id"), InputTaskId);
		MeshyHelpers::SetIfNotEmpty(Body, TEXT("model_url"), ModelUrl);
		MeshyHelpers::SetIfNotEmpty(Body, TEXT("text_style_prompt"), TextPrompt.Left(600));
		MeshyHelpers::SetIfNotEmpty(Body, TEXT("image_style_url"), ImageStyle);
		MeshyHelpers::SetIfNotEmpty(Body, TEXT("ai_model"), MeshyHelpers::GetStr(Params, TEXT("ai_model")));
		if (Params->HasField(TEXT("enable_original_uv")))
			Body->SetBoolField(TEXT("enable_original_uv"), MeshyHelpers::GetBool(Params, TEXT("enable_original_uv"), true));
		if (Params->HasField(TEXT("enable_pbr")))
			Body->SetBoolField(TEXT("enable_pbr"), MeshyHelpers::GetBool(Params, TEXT("enable_pbr")));
		if (Params->HasField(TEXT("remove_lighting")))
			Body->SetBoolField(TEXT("remove_lighting"), MeshyHelpers::GetBool(Params, TEXT("remove_lighting"), true));
		if (Params->HasField(TEXT("target_formats")))
			Body->SetArrayField(TEXT("target_formats"), Params->GetArrayField(TEXT("target_formats")));

		TSharedPtr<FJsonObject> Json = HttpPost(TEXT("/openapi/v1/retexture"), Body);
		if (!Json.IsValid())
			return FGenerativeJob::MakeFail(TEXT("Failed to create retexture task"));

		auto Job = FGenerativeJob::MakePending(MeshyHelpers::GetStr(Json, TEXT("result")));
		Job.ProviderId = TEXT("meshy");
		Job.ActionId = TEXT("retexture");
		return Job;
	}

	FGenerativeJob SubmitRemesh(const TSharedPtr<FJsonObject>& Params)
	{
		const FString InputTaskId = MeshyHelpers::GetStr(Params, TEXT("input_task_id"));
		const FString ModelUrl = MeshyHelpers::GetStr(Params, TEXT("model_url"));
		if (InputTaskId.IsEmpty() && ModelUrl.IsEmpty())
			return FGenerativeJob::MakeFail(TEXT("remesh requires input_task_id or model_url"));

		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		MeshyHelpers::SetIfNotEmpty(Body, TEXT("input_task_id"), InputTaskId);
		MeshyHelpers::SetIfNotEmpty(Body, TEXT("model_url"), ModelUrl);
		MeshyHelpers::SetIfNotEmpty(Body, TEXT("topology"), MeshyHelpers::GetStr(Params, TEXT("topology")));
		if (Params->HasField(TEXT("target_polycount")))
			Body->SetNumberField(TEXT("target_polycount"),
				FMath::Clamp(MeshyHelpers::GetInt(Params, TEXT("target_polycount"), 30000), 100, 300000));
		if (Params->HasField(TEXT("resize_height")))
			Body->SetNumberField(TEXT("resize_height"), Params->GetNumberField(TEXT("resize_height")));
		MeshyHelpers::SetIfNotEmpty(Body, TEXT("origin_at"), MeshyHelpers::GetStr(Params, TEXT("origin_at")));
		if (Params->HasField(TEXT("convert_format_only")))
			Body->SetBoolField(TEXT("convert_format_only"), MeshyHelpers::GetBool(Params, TEXT("convert_format_only")));
		if (Params->HasField(TEXT("target_formats")))
			Body->SetArrayField(TEXT("target_formats"), Params->GetArrayField(TEXT("target_formats")));

		TSharedPtr<FJsonObject> Json = HttpPost(TEXT("/openapi/v1/remesh"), Body);
		if (!Json.IsValid())
			return FGenerativeJob::MakeFail(TEXT("Failed to create remesh task"));

		auto Job = FGenerativeJob::MakePending(MeshyHelpers::GetStr(Json, TEXT("result")));
		Job.ProviderId = TEXT("meshy");
		Job.ActionId = TEXT("remesh");
		return Job;
	}

	FGenerativeJob SubmitRig(const TSharedPtr<FJsonObject>& Params)
	{
		const FString InputTaskId = MeshyHelpers::GetStr(Params, TEXT("input_task_id"));
		const FString ModelUrl = MeshyHelpers::GetStr(Params, TEXT("model_url"));
		if (InputTaskId.IsEmpty() && ModelUrl.IsEmpty())
			return FGenerativeJob::MakeFail(TEXT("rig requires input_task_id or model_url"));

		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		MeshyHelpers::SetIfNotEmpty(Body, TEXT("input_task_id"), InputTaskId);
		MeshyHelpers::SetIfNotEmpty(Body, TEXT("model_url"), ModelUrl);
		if (Params->HasField(TEXT("height_meters")))
			Body->SetNumberField(TEXT("height_meters"), Params->GetNumberField(TEXT("height_meters")));
		MeshyHelpers::SetIfNotEmpty(Body, TEXT("texture_image_url"), MeshyHelpers::GetStr(Params, TEXT("texture_image_url")));

		TSharedPtr<FJsonObject> Json = HttpPost(TEXT("/openapi/v1/rigging"), Body);
		if (!Json.IsValid())
			return FGenerativeJob::MakeFail(TEXT("Failed to create rigging task"));

		auto Job = FGenerativeJob::MakePending(MeshyHelpers::GetStr(Json, TEXT("result")));
		Job.ProviderId = TEXT("meshy");
		Job.ActionId = TEXT("rig");
		return Job;
	}

	FGenerativeJob SubmitAnimate(const TSharedPtr<FJsonObject>& Params)
	{
		const FString RigTaskId = MeshyHelpers::GetStr(Params, TEXT("rig_task_id"));
		if (RigTaskId.IsEmpty())
			return FGenerativeJob::MakeFail(TEXT("animate requires rig_task_id"));
		if (!Params->HasField(TEXT("action_id")))
			return FGenerativeJob::MakeFail(TEXT("animate requires action_id (animation identifier)"));

		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("rig_task_id"), RigTaskId);
		Body->SetNumberField(TEXT("action_id"), MeshyHelpers::GetInt(Params, TEXT("action_id")));

		// Post-processing
		if (Params->HasField(TEXT("post_process_operation")))
		{
			TSharedPtr<FJsonObject> PostProcess = MakeShared<FJsonObject>();
			PostProcess->SetStringField(TEXT("operation_type"), MeshyHelpers::GetStr(Params, TEXT("post_process_operation")));
			if (Params->HasField(TEXT("post_process_fps")))
				PostProcess->SetNumberField(TEXT("fps"), MeshyHelpers::GetInt(Params, TEXT("post_process_fps"), 30));
			Body->SetObjectField(TEXT("post_process"), PostProcess);
		}

		TSharedPtr<FJsonObject> Json = HttpPost(TEXT("/openapi/v1/animations"), Body);
		if (!Json.IsValid())
			return FGenerativeJob::MakeFail(TEXT("Failed to create animation task"));

		auto Job = FGenerativeJob::MakePending(MeshyHelpers::GetStr(Json, TEXT("result")));
		Job.ProviderId = TEXT("meshy");
		Job.ActionId = TEXT("animate");
		return Job;
	}

	FGenerativeJob SubmitTextToImage(const TSharedPtr<FJsonObject>& Params)
	{
		const FString Prompt = MeshyHelpers::GetStr(Params, TEXT("prompt"));
		const FString AiModel = MeshyHelpers::GetStr(Params, TEXT("ai_model"));
		if (Prompt.IsEmpty())
			return FGenerativeJob::MakeFail(TEXT("text_to_image requires prompt"));
		if (AiModel.IsEmpty())
			return FGenerativeJob::MakeFail(TEXT("text_to_image requires ai_model (nano-banana or nano-banana-pro)"));

		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("ai_model"), AiModel);
		Body->SetStringField(TEXT("prompt"), Prompt);
		if (Params->HasField(TEXT("generate_multi_view")))
			Body->SetBoolField(TEXT("generate_multi_view"), MeshyHelpers::GetBool(Params, TEXT("generate_multi_view")));
		MeshyHelpers::SetIfNotEmpty(Body, TEXT("pose_mode"), MeshyHelpers::GetStr(Params, TEXT("pose_mode")));
		MeshyHelpers::SetIfNotEmpty(Body, TEXT("aspect_ratio"), MeshyHelpers::GetStr(Params, TEXT("aspect_ratio")));

		TSharedPtr<FJsonObject> Json = HttpPost(TEXT("/openapi/v1/text-to-image"), Body);
		if (!Json.IsValid())
			return FGenerativeJob::MakeFail(TEXT("Failed to create text_to_image task"));

		auto Job = FGenerativeJob::MakePending(MeshyHelpers::GetStr(Json, TEXT("result")));
		Job.ProviderId = TEXT("meshy");
		Job.ActionId = TEXT("text_to_image");
		return Job;
	}

	FGenerativeJob SubmitImageToImage(const TSharedPtr<FJsonObject>& Params)
	{
		const FString Prompt = MeshyHelpers::GetStr(Params, TEXT("prompt"));
		const FString AiModel = MeshyHelpers::GetStr(Params, TEXT("ai_model"));
		if (Prompt.IsEmpty())
			return FGenerativeJob::MakeFail(TEXT("image_to_image requires prompt"));
		if (AiModel.IsEmpty())
			return FGenerativeJob::MakeFail(TEXT("image_to_image requires ai_model"));
		if (!Params->HasField(TEXT("reference_image_urls")))
			return FGenerativeJob::MakeFail(TEXT("image_to_image requires reference_image_urls array"));

		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("ai_model"), AiModel);
		Body->SetStringField(TEXT("prompt"), Prompt);
		Body->SetArrayField(TEXT("reference_image_urls"), Params->GetArrayField(TEXT("reference_image_urls")));
		if (Params->HasField(TEXT("generate_multi_view")))
			Body->SetBoolField(TEXT("generate_multi_view"), MeshyHelpers::GetBool(Params, TEXT("generate_multi_view")));

		TSharedPtr<FJsonObject> Json = HttpPost(TEXT("/openapi/v1/image-to-image"), Body);
		if (!Json.IsValid())
			return FGenerativeJob::MakeFail(TEXT("Failed to create image_to_image task"));

		auto Job = FGenerativeJob::MakePending(MeshyHelpers::GetStr(Json, TEXT("result")));
		Job.ProviderId = TEXT("meshy");
		Job.ActionId = TEXT("image_to_image");
		return Job;
	}

	FGenerativeJob SubmitBalance()
	{
		TSharedPtr<FJsonObject> Json = HttpGet(TEXT("/openapi/v1/balance"));
		if (!Json.IsValid())
			return FGenerativeJob::MakeFail(TEXT("Failed to get balance"));

		FGenerativeJob Job;
		Job.ProviderId = TEXT("meshy");
		Job.ActionId = TEXT("balance");
		Job.Status = EGenerativeJobStatus::Succeeded;
		Job.Progress = 100;
		Job.RawResponse = Json;
		return Job;
	}

	// ── Schema builders ──────────────────────────────────────────────

	static TSharedPtr<FJsonObject> BuildTextTo3DSchema()
	{
		TMap<FString, TSharedPtr<FJsonObject>> Props;
		Props.Add(TEXT("prompt"), SchemaString(TEXT("Describe the 3D model (max 600 chars)")));
		Props.Add(TEXT("negative_prompt"), SchemaString(TEXT("What to avoid"), {}, TEXT("low quality, low resolution, low poly, ugly")));
		Props.Add(TEXT("ai_model"), SchemaString(TEXT("Model version"), {TEXT("meshy-5"), TEXT("meshy-6"), TEXT("latest")}, TEXT("latest")));
		Props.Add(TEXT("model_type"), SchemaString(TEXT("Mesh type"), {TEXT("standard"), TEXT("lowpoly")}, TEXT("standard")));
		Props.Add(TEXT("topology"), SchemaString(TEXT("Mesh topology"), {TEXT("quad"), TEXT("triangle")}, TEXT("triangle")));
		Props.Add(TEXT("target_polycount"), SchemaInt(TEXT("Target polygon count"), 100, 300000, 30000));
		Props.Add(TEXT("should_remesh"), SchemaBool(TEXT("Enable remesh phase")));
		Props.Add(TEXT("symmetry_mode"), SchemaString(TEXT("Symmetry"), {TEXT("off"), TEXT("auto"), TEXT("on")}, TEXT("auto")));
		Props.Add(TEXT("pose_mode"), SchemaString(TEXT("Character pose"), {TEXT("a-pose"), TEXT("t-pose"), TEXT("")}));
		Props.Add(TEXT("target_formats"), SchemaStringArray(TEXT("Output formats: glb, obj, fbx, stl, usdz")));
		return BuildSchema(Props, {TEXT("prompt")});
	}

	static TSharedPtr<FJsonObject> BuildRefineSchema()
	{
		TMap<FString, TSharedPtr<FJsonObject>> Props;
		Props.Add(TEXT("preview_task_id"), SchemaString(TEXT("SUCCEEDED preview task ID")));
		Props.Add(TEXT("enable_pbr"), SchemaBool(TEXT("Generate PBR maps (metallic, roughness, normal)"), true));
		Props.Add(TEXT("ai_model"), SchemaString(TEXT("Model version"), {TEXT("meshy-5"), TEXT("meshy-6"), TEXT("latest")}, TEXT("latest")));
		Props.Add(TEXT("texture_prompt"), SchemaString(TEXT("Text guidance for texturing (max 600 chars)")));
		Props.Add(TEXT("texture_image_url"), SchemaString(TEXT("Image guidance for texturing (URL or data URI)")));
		Props.Add(TEXT("remove_lighting"), SchemaBool(TEXT("Remove baked lighting from texture"), true));
		Props.Add(TEXT("target_formats"), SchemaStringArray(TEXT("Output formats: glb, obj, fbx, stl, usdz")));
		return BuildSchema(Props, {TEXT("preview_task_id")});
	}

	static TSharedPtr<FJsonObject> BuildImageTo3DSchema()
	{
		TMap<FString, TSharedPtr<FJsonObject>> Props;
		Props.Add(TEXT("image_url"), SchemaString(TEXT("Image URL or base64 data URI (jpg/jpeg/png)")));
		Props.Add(TEXT("ai_model"), SchemaString(TEXT("Model version"), {TEXT("meshy-5"), TEXT("meshy-6"), TEXT("latest")}, TEXT("latest")));
		Props.Add(TEXT("model_type"), SchemaString(TEXT("Mesh type"), {TEXT("standard"), TEXT("lowpoly")}, TEXT("standard")));
		Props.Add(TEXT("topology"), SchemaString(TEXT("Mesh topology"), {TEXT("quad"), TEXT("triangle")}, TEXT("triangle")));
		Props.Add(TEXT("target_polycount"), SchemaInt(TEXT("Target polygon count"), 100, 300000, 30000));
		Props.Add(TEXT("should_remesh"), SchemaBool(TEXT("Enable remesh phase")));
		Props.Add(TEXT("should_texture"), SchemaBool(TEXT("Generate textures"), true));
		Props.Add(TEXT("enable_pbr"), SchemaBool(TEXT("Generate PBR maps")));
		Props.Add(TEXT("symmetry_mode"), SchemaString(TEXT("Symmetry"), {TEXT("off"), TEXT("auto"), TEXT("on")}, TEXT("auto")));
		Props.Add(TEXT("pose_mode"), SchemaString(TEXT("Character pose"), {TEXT("a-pose"), TEXT("t-pose"), TEXT("")}));
		Props.Add(TEXT("save_pre_remeshed_model"), SchemaBool(TEXT("Save model before remeshing")));
		Props.Add(TEXT("image_enhancement"), SchemaBool(TEXT("Optimize input image"), true));
		Props.Add(TEXT("remove_lighting"), SchemaBool(TEXT("Remove baked lighting"), true));
		Props.Add(TEXT("texture_prompt"), SchemaString(TEXT("Text guidance for texturing")));
		Props.Add(TEXT("texture_image_url"), SchemaString(TEXT("Image guidance for texturing")));
		Props.Add(TEXT("target_formats"), SchemaStringArray(TEXT("Output formats: glb, obj, fbx, stl, usdz")));
		return BuildSchema(Props, {TEXT("image_url")});
	}

	static TSharedPtr<FJsonObject> BuildMultiImageTo3DSchema()
	{
		TMap<FString, TSharedPtr<FJsonObject>> Props;
		Props.Add(TEXT("image_urls"), SchemaStringArray(TEXT("1-4 image URLs or data URIs of same object from different angles")));
		Props.Add(TEXT("ai_model"), SchemaString(TEXT("Model version"), {TEXT("meshy-5"), TEXT("meshy-6"), TEXT("latest")}, TEXT("latest")));
		Props.Add(TEXT("topology"), SchemaString(TEXT("Mesh topology"), {TEXT("quad"), TEXT("triangle")}, TEXT("triangle")));
		Props.Add(TEXT("target_polycount"), SchemaInt(TEXT("Target polygon count"), 100, 300000, 30000));
		Props.Add(TEXT("should_remesh"), SchemaBool(TEXT("Enable remesh phase")));
		Props.Add(TEXT("should_texture"), SchemaBool(TEXT("Generate textures"), true));
		Props.Add(TEXT("enable_pbr"), SchemaBool(TEXT("Generate PBR maps")));
		Props.Add(TEXT("symmetry_mode"), SchemaString(TEXT("Symmetry"), {TEXT("off"), TEXT("auto"), TEXT("on")}, TEXT("auto")));
		Props.Add(TEXT("pose_mode"), SchemaString(TEXT("Character pose"), {TEXT("a-pose"), TEXT("t-pose"), TEXT("")}));
		Props.Add(TEXT("save_pre_remeshed_model"), SchemaBool(TEXT("Save model before remeshing")));
		Props.Add(TEXT("image_enhancement"), SchemaBool(TEXT("Optimize input images"), true));
		Props.Add(TEXT("remove_lighting"), SchemaBool(TEXT("Remove baked lighting"), true));
		Props.Add(TEXT("texture_prompt"), SchemaString(TEXT("Text guidance for texturing")));
		Props.Add(TEXT("texture_image_url"), SchemaString(TEXT("Image guidance for texturing")));
		Props.Add(TEXT("target_formats"), SchemaStringArray(TEXT("Output formats: glb, obj, fbx, stl, usdz")));
		return BuildSchema(Props, {TEXT("image_urls")});
	}

	static TSharedPtr<FJsonObject> BuildRetextureSchema()
	{
		TMap<FString, TSharedPtr<FJsonObject>> Props;
		Props.Add(TEXT("input_task_id"), SchemaString(TEXT("SUCCEEDED task ID from prior Meshy generation")));
		Props.Add(TEXT("model_url"), SchemaString(TEXT("External model URL or data URI (glb/gltf/obj/fbx/stl)")));
		Props.Add(TEXT("text_style_prompt"), SchemaString(TEXT("Texture style description (max 600 chars)")));
		Props.Add(TEXT("image_style_url"), SchemaString(TEXT("Style reference image URL or data URI")));
		Props.Add(TEXT("ai_model"), SchemaString(TEXT("Model version"), {TEXT("meshy-5"), TEXT("meshy-6"), TEXT("latest")}, TEXT("latest")));
		Props.Add(TEXT("enable_original_uv"), SchemaBool(TEXT("Preserve original UVs"), true));
		Props.Add(TEXT("enable_pbr"), SchemaBool(TEXT("Generate PBR maps")));
		Props.Add(TEXT("remove_lighting"), SchemaBool(TEXT("Remove baked lighting"), true));
		Props.Add(TEXT("target_formats"), SchemaStringArray(TEXT("Output formats: glb, obj, fbx, stl, usdz")));
		return BuildSchema(Props);
	}

	static TSharedPtr<FJsonObject> BuildRemeshSchema()
	{
		TMap<FString, TSharedPtr<FJsonObject>> Props;
		Props.Add(TEXT("input_task_id"), SchemaString(TEXT("SUCCEEDED task ID from prior Meshy generation")));
		Props.Add(TEXT("model_url"), SchemaString(TEXT("External model URL or data URI (glb/gltf/obj/fbx/stl)")));
		Props.Add(TEXT("topology"), SchemaString(TEXT("Output topology"), {TEXT("quad"), TEXT("triangle")}, TEXT("triangle")));
		Props.Add(TEXT("target_polycount"), SchemaInt(TEXT("Target polygon count"), 100, 300000, 30000));
		Props.Add(TEXT("resize_height"), SchemaInt(TEXT("Resize height in meters (0 = no resize)")));
		Props.Add(TEXT("origin_at"), SchemaString(TEXT("Origin position"), {TEXT("bottom"), TEXT("center"), TEXT("")}));
		Props.Add(TEXT("convert_format_only"), SchemaBool(TEXT("Only convert format, skip remeshing")));
		Props.Add(TEXT("target_formats"), SchemaStringArray(TEXT("Output formats: glb, fbx, obj, usdz, blend, stl")));
		return BuildSchema(Props);
	}

	static TSharedPtr<FJsonObject> BuildRigSchema()
	{
		TMap<FString, TSharedPtr<FJsonObject>> Props;
		Props.Add(TEXT("input_task_id"), SchemaString(TEXT("SUCCEEDED task ID from prior Meshy generation")));
		Props.Add(TEXT("model_url"), SchemaString(TEXT("External humanoid model URL or data URI (GLB only)")));
		Props.Add(TEXT("height_meters"), SchemaInt(TEXT("Character height in meters"), 0, 0, 0));
		Props.Add(TEXT("texture_image_url"), SchemaString(TEXT("Base color texture PNG URL or data URI")));
		return BuildSchema(Props);
	}

	static TSharedPtr<FJsonObject> BuildAnimateSchema()
	{
		TMap<FString, TSharedPtr<FJsonObject>> Props;
		Props.Add(TEXT("rig_task_id"), SchemaString(TEXT("SUCCEEDED rigging task ID")));
		Props.Add(TEXT("action_id"), SchemaInt(TEXT("Animation identifier (see Meshy animation library)")));
		Props.Add(TEXT("post_process_operation"), SchemaString(TEXT("Post-processing"),
			{TEXT("change_fps"), TEXT("fbx2usdz"), TEXT("extract_armature")}));
		Props.Add(TEXT("post_process_fps"), SchemaInt(TEXT("Target FPS (for change_fps)"), 0, 0, 30));
		return BuildSchema(Props, {TEXT("rig_task_id"), TEXT("action_id")});
	}

	static TSharedPtr<FJsonObject> BuildTextToImageSchema()
	{
		TMap<FString, TSharedPtr<FJsonObject>> Props;
		Props.Add(TEXT("prompt"), SchemaString(TEXT("Image description")));
		Props.Add(TEXT("ai_model"), SchemaString(TEXT("Model"), {TEXT("nano-banana"), TEXT("nano-banana-pro")}));
		Props.Add(TEXT("generate_multi_view"), SchemaBool(TEXT("Generate 3 multi-angle views")));
		Props.Add(TEXT("pose_mode"), SchemaString(TEXT("Character pose"), {TEXT("a-pose"), TEXT("t-pose")}));
		Props.Add(TEXT("aspect_ratio"), SchemaString(TEXT("Image ratio"),
			{TEXT("1:1"), TEXT("16:9"), TEXT("9:16"), TEXT("4:3"), TEXT("3:4")}, TEXT("1:1")));
		return BuildSchema(Props, {TEXT("prompt"), TEXT("ai_model")});
	}

	static TSharedPtr<FJsonObject> BuildImageToImageSchema()
	{
		TMap<FString, TSharedPtr<FJsonObject>> Props;
		Props.Add(TEXT("prompt"), SchemaString(TEXT("Transformation description")));
		Props.Add(TEXT("ai_model"), SchemaString(TEXT("Model"), {TEXT("nano-banana"), TEXT("nano-banana-pro")}));
		Props.Add(TEXT("reference_image_urls"), SchemaStringArray(TEXT("1-5 reference image URLs or data URIs")));
		Props.Add(TEXT("generate_multi_view"), SchemaBool(TEXT("Generate 3 multi-angle views")));
		return BuildSchema(Props, {TEXT("prompt"), TEXT("ai_model"), TEXT("reference_image_urls")});
	}
};

// ── Auto-register ────────────────────────────────────────────────────

REGISTER_GENERATIVE_PROVIDER(FMeshyProvider);
