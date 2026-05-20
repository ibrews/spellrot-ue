// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Tools/GenerateAssetTool.h"
#include "Tools/GenerateImageTool.h"
#include "Tools/Generate3DModelTool.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	static void MergeSchemaProperties(TSharedPtr<FJsonObject> TargetSchema, const TSharedPtr<FJsonObject>& SourceSchema)
	{
		if (!TargetSchema.IsValid() || !SourceSchema.IsValid())
		{
			return;
		}

		TSharedPtr<FJsonObject> TargetProps;
		const TSharedPtr<FJsonObject>* TargetPropsPtr = nullptr;
		if (!TargetSchema->TryGetObjectField(TEXT("properties"), TargetPropsPtr) || !TargetPropsPtr || !(*TargetPropsPtr).IsValid())
		{
			TargetProps = MakeShared<FJsonObject>();
			TargetSchema->SetObjectField(TEXT("properties"), TargetProps);
		}
		else
		{
			TargetProps = *TargetPropsPtr;
		}

		const TSharedPtr<FJsonObject>* SourceProps = nullptr;
		if (!SourceSchema->TryGetObjectField(TEXT("properties"), SourceProps) || !SourceProps || !(*SourceProps).IsValid())
		{
			return;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*SourceProps)->Values)
		{
			if (!TargetProps->HasField(Pair.Key))
			{
				TargetProps->SetField(Pair.Key, Pair.Value);
			}
		}
	}
}

TSharedPtr<FJsonObject> FGenerateAssetTool::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> AssetTypeProp = MakeShared<FJsonObject>();
	AssetTypeProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetTypeProp->SetStringField(TEXT("description"), TEXT("Generation target: 'image' or 'model_3d'. If omitted, inferred from fields."));
	TArray<TSharedPtr<FJsonValue>> AssetTypeEnum;
	AssetTypeEnum.Add(MakeShared<FJsonValueString>(TEXT("image")));
	AssetTypeEnum.Add(MakeShared<FJsonValueString>(TEXT("model_3d")));
	AssetTypeProp->SetArrayField(TEXT("enum"), AssetTypeEnum);
	Properties->SetObjectField(TEXT("asset_type"), AssetTypeProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	// Merge full parameter surfaces from both delegated tools for parity/discoverability.
	FGenerateImageTool ImageTool;
	FGenerate3DModelTool ModelTool;
	MergeSchemaProperties(Schema, ImageTool.GetInputSchema());
	MergeSchemaProperties(Schema, ModelTool.GetInputSchema());

	// Unified tool: requirements depend on route. Avoid hard-required fields at top level.
	Schema->SetArrayField(TEXT("required"), TArray<TSharedPtr<FJsonValue>>{});

	return Schema;
}

FToolResult FGenerateAssetTool::Execute(const TSharedPtr<FJsonObject>& Args)
{
	if (!Args.IsValid())
	{
		return FToolResult::Fail(TEXT("Invalid arguments"));
	}

	FString AssetType;
	Args->TryGetStringField(TEXT("asset_type"), AssetType);
	AssetType = AssetType.ToLower();

	if (AssetType.IsEmpty())
	{
		// Infer 3D workflow from Meshy-specific fields.
		const bool bLooksLike3D =
			Args->HasField(TEXT("action")) ||
			Args->HasField(TEXT("job_id")) ||
			Args->HasField(TEXT("job_type")) ||
			Args->HasField(TEXT("source_image")) ||
			Args->HasField(TEXT("source_images")) ||
			Args->HasField(TEXT("preview_task_id")) ||
			Args->HasField(TEXT("ai_model")) ||
			Args->HasField(TEXT("art_style")) ||
			Args->HasField(TEXT("negative_prompt")) ||
			Args->HasField(TEXT("topology")) ||
			Args->HasField(TEXT("symmetry_mode")) ||
			Args->HasField(TEXT("pose_mode")) ||
			Args->HasField(TEXT("model_type")) ||
			Args->HasField(TEXT("texture_prompt")) ||
			Args->HasField(TEXT("texture_image_url")) ||
			Args->HasField(TEXT("enable_pbr")) ||
			Args->HasField(TEXT("should_remesh")) ||
			Args->HasField(TEXT("should_texture")) ||
			Args->HasField(TEXT("target_polycount")) ||
			Args->HasField(TEXT("wait")) ||
			Args->HasField(TEXT("timeout")) ||
			Args->HasField(TEXT("provider")) ||
			Args->HasField(TEXT("fal_endpoint_id")) ||
			Args->HasField(TEXT("fal_input")) ||
			Args->HasField(TEXT("fal_input_json")) ||
			Args->HasField(TEXT("status_url")) ||
			Args->HasField(TEXT("response_url")) ||
			Args->HasField(TEXT("cancel_url"));

		// Infer image workflow from OpenRouter-image-specific fields.
		const bool bLooksLikeImage =
			Args->HasField(TEXT("model")) ||
			Args->HasField(TEXT("aspect_ratio"));

		if (bLooksLike3D && bLooksLikeImage)
		{
			return FToolResult::Fail(TEXT("Ambiguous request: contains parameters for both image and 3D generation. Set asset_type explicitly."));
		}

		if (bLooksLike3D)
		{
			AssetType = TEXT("model_3d");
		}
		else if (bLooksLikeImage)
		{
			AssetType = TEXT("image");
		}
		else
		{
			return FToolResult::Fail(TEXT("Unable to infer asset_type. Set asset_type to 'image' or 'model_3d'."));
		}
	}

	if (AssetType == TEXT("image"))
	{
		FGenerateImageTool Tool;
		return Tool.Execute(Args);
	}

	if (AssetType == TEXT("model_3d") || AssetType == TEXT("3d") || AssetType == TEXT("model"))
	{
		FGenerate3DModelTool Tool;
		return Tool.Execute(Args);
	}

	return FToolResult::Fail(TEXT("Invalid asset_type. Use 'image' or 'model_3d'."));
}
