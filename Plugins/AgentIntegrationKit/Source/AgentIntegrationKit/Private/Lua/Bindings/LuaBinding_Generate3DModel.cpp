// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Tools/Generate3DModelTool.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

namespace
{
	void SetStringOpt(TSharedPtr<FJsonObject>& Args, const sol::table& T, const char* Key)
	{
		sol::optional<std::string> Val = T.get<sol::optional<std::string>>(Key);
		if (Val.has_value() && !Val.value().empty())
		{
			Args->SetStringField(UTF8_TO_TCHAR(Key), UTF8_TO_TCHAR(Val.value().c_str()));
		}
	}

	void SetBoolOpt(TSharedPtr<FJsonObject>& Args, const sol::table& T, const char* Key)
	{
		sol::optional<bool> Val = T.get<sol::optional<bool>>(Key);
		if (Val.has_value())
		{
			Args->SetBoolField(UTF8_TO_TCHAR(Key), Val.value());
		}
	}

	void SetNumberOpt(TSharedPtr<FJsonObject>& Args, const sol::table& T, const char* Key)
	{
		sol::optional<double> Val = T.get<sol::optional<double>>(Key);
		if (Val.has_value())
		{
			Args->SetNumberField(UTF8_TO_TCHAR(Key), Val.value());
		}
	}

	// Recursively convert a sol::table to FJsonObject
	TSharedPtr<FJsonObject> TableToJsonObject(const sol::table& T)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		for (auto& Pair : T)
		{
			FString Key = UTF8_TO_TCHAR(Pair.first.as<std::string>().c_str());

			if (Pair.second.is<bool>())
			{
				Obj->SetBoolField(Key, Pair.second.as<bool>());
			}
			else if (Pair.second.is<double>())
			{
				Obj->SetNumberField(Key, Pair.second.as<double>());
			}
			else if (Pair.second.is<std::string>())
			{
				Obj->SetStringField(Key, UTF8_TO_TCHAR(Pair.second.as<std::string>().c_str()));
			}
			else if (Pair.second.is<sol::table>())
			{
				sol::table Sub = Pair.second.as<sol::table>();
				// Check if it's an array (sequential integer keys starting at 1)
				bool bIsArray = true;
				int32 ExpectedKey = 1;
				for (auto& SubPair : Sub)
				{
					if (!SubPair.first.is<int>() || SubPair.first.as<int>() != ExpectedKey++)
					{
						bIsArray = false;
						break;
					}
				}

				if (bIsArray && ExpectedKey > 1)
				{
					TArray<TSharedPtr<FJsonValue>> Arr;
					for (auto& SubPair : Sub)
					{
						if (SubPair.second.is<std::string>())
						{
							Arr.Add(MakeShared<FJsonValueString>(UTF8_TO_TCHAR(SubPair.second.as<std::string>().c_str())));
						}
						else if (SubPair.second.is<double>())
						{
							Arr.Add(MakeShared<FJsonValueNumber>(SubPair.second.as<double>()));
						}
						else if (SubPair.second.is<bool>())
						{
							Arr.Add(MakeShared<FJsonValueBoolean>(SubPair.second.as<bool>()));
						}
						else if (SubPair.second.is<sol::table>())
						{
							Arr.Add(MakeShared<FJsonValueObject>(TableToJsonObject(SubPair.second.as<sol::table>())));
						}
					}
					Obj->SetArrayField(Key, Arr);
				}
				else
				{
					Obj->SetObjectField(Key, TableToJsonObject(Sub));
				}
			}
		}
		return Obj;
	}
}

static TArray<FLuaFunctionDoc> Generate3DModelDocs = {
	{ TEXT("generate_3d_model(action, opts?)"),
	  TEXT("Generate 3D models via Meshy AI or fal.ai. action='create'|'check'|'import'. "
	       "opts: {provider='meshy'|'fal', prompt='...', job_id='...', job_type='text_to_3d'|'image_to_3d'|'multi_image_to_3d', "
	       "source_image='url', source_images={'url1','url2'}, preview_task_id='...', "
	       "ai_model='latest', topology='triangle', symmetry_mode='auto', pose_mode='', model_type='standard', "
	       "texture_prompt='...', texture_image_url='...', enable_pbr=true, should_remesh=true, should_texture=true, "
	       "target_polycount=30000, asset_path='/Game/Generated3DModels', asset_name='...', "
	       "wait=true, timeout=300, poll_interval_seconds=15, "
	       "fal_endpoint_id='...', fal_input={...}, fal_input_json='...', status_url='...', response_url='...', cancel_url='...'}"),
	  TEXT("string (job status, IDs, or imported asset path)") }
};

REGISTER_LUA_BINDING(Generate3DModel, Generate3DModelDocs,
[](sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("generate_3d_model", [&Session](const std::string& Action, sol::optional<sol::table> Opts) -> std::string
	{
		if (Action.empty())
		{
			Session.Log(TEXT("[ERROR] generate_3d_model: action is required ('create', 'check', or 'import')"));
			return "ERROR: action is required";
		}

		FGenerate3DModelTool Tool;

		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("action"), UTF8_TO_TCHAR(Action.c_str()));

		if (Opts.has_value())
		{
			sol::table T = Opts.value();

			// String params
			SetStringOpt(Args, T, "provider");
			SetStringOpt(Args, T, "job_id");
			SetStringOpt(Args, T, "job_type");
			SetStringOpt(Args, T, "prompt");
			SetStringOpt(Args, T, "source_image");
			SetStringOpt(Args, T, "preview_task_id");
			SetStringOpt(Args, T, "ai_model");
			SetStringOpt(Args, T, "art_style");
			SetStringOpt(Args, T, "negative_prompt");
			SetStringOpt(Args, T, "topology");
			SetStringOpt(Args, T, "symmetry_mode");
			SetStringOpt(Args, T, "pose_mode");
			SetStringOpt(Args, T, "model_type");
			SetStringOpt(Args, T, "texture_prompt");
			SetStringOpt(Args, T, "texture_image_url");
			SetStringOpt(Args, T, "asset_path");
			SetStringOpt(Args, T, "asset_name");

			// fal-specific strings
			SetStringOpt(Args, T, "fal_endpoint_id");
			SetStringOpt(Args, T, "fal_input_json");
			SetStringOpt(Args, T, "status_url");
			SetStringOpt(Args, T, "response_url");
			SetStringOpt(Args, T, "cancel_url");

			// Bool params
			SetBoolOpt(Args, T, "enable_pbr");
			SetBoolOpt(Args, T, "should_remesh");
			SetBoolOpt(Args, T, "should_texture");
			SetBoolOpt(Args, T, "wait");

			// Number params
			SetNumberOpt(Args, T, "target_polycount");
			SetNumberOpt(Args, T, "timeout");
			SetNumberOpt(Args, T, "poll_interval_seconds");

			// source_images (array of strings)
			sol::optional<sol::table> SourceImages = T.get<sol::optional<sol::table>>("source_images");
			if (SourceImages.has_value())
			{
				TArray<TSharedPtr<FJsonValue>> Arr;
				for (auto& Pair : SourceImages.value())
				{
					if (Pair.second.is<std::string>())
					{
						Arr.Add(MakeShared<FJsonValueString>(UTF8_TO_TCHAR(Pair.second.as<std::string>().c_str())));
					}
				}
				Args->SetArrayField(TEXT("source_images"), Arr);
			}

			// fal_input (object — recursive conversion)
			sol::optional<sol::table> FalInput = T.get<sol::optional<sol::table>>("fal_input");
			if (FalInput.has_value())
			{
				Args->SetObjectField(TEXT("fal_input"), TableToJsonObject(FalInput.value()));
			}
		}

		FToolResult Result = Tool.Execute(Args);

		Session.AddImages(Result.Images);
		Session.Log(Result.Output);

		return TCHAR_TO_UTF8(*Result.Output);
	});
});
