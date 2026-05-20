// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Tools/GenerateImageTool.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

static TArray<FLuaFunctionDoc> GenerateImageDocs = {
	{ TEXT("generate_image(prompt, opts?)"),
	  TEXT("Generate an image from a text prompt via OpenRouter API and import as Texture2D. "
	       "opts: {model='black-forest-labs/flux.2-flex', aspect_ratio='1:1'|'16:9'|'9:16'|'4:3'|'3:4'|'21:9', "
	       "asset_path='/Game/GeneratedImages', asset_name='MyTexture'}"),
	  TEXT("string (asset path of imported texture)") }
};

REGISTER_LUA_BINDING(GenerateImage, GenerateImageDocs,
[](sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("generate_image", [&Session](const std::string& Prompt, sol::optional<sol::table> Opts) -> std::string
	{
		if (Prompt.empty())
		{
			Session.Log(TEXT("[ERROR] generate_image: prompt is required"));
			return "ERROR: prompt is required";
		}

		FGenerateImageTool Tool;

		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("prompt"), UTF8_TO_TCHAR(Prompt.c_str()));

		if (Opts.has_value())
		{
			sol::table T = Opts.value();

			sol::optional<std::string> Model = T.get<sol::optional<std::string>>("model");
			if (Model.has_value() && !Model.value().empty())
			{
				Args->SetStringField(TEXT("model"), UTF8_TO_TCHAR(Model.value().c_str()));
			}

			sol::optional<std::string> AspectRatio = T.get<sol::optional<std::string>>("aspect_ratio");
			if (AspectRatio.has_value() && !AspectRatio.value().empty())
			{
				Args->SetStringField(TEXT("aspect_ratio"), UTF8_TO_TCHAR(AspectRatio.value().c_str()));
			}

			sol::optional<std::string> AssetPath = T.get<sol::optional<std::string>>("asset_path");
			if (AssetPath.has_value() && !AssetPath.value().empty())
			{
				Args->SetStringField(TEXT("asset_path"), UTF8_TO_TCHAR(AssetPath.value().c_str()));
			}

			sol::optional<std::string> AssetName = T.get<sol::optional<std::string>>("asset_name");
			if (AssetName.has_value() && !AssetName.value().empty())
			{
				Args->SetStringField(TEXT("asset_name"), UTF8_TO_TCHAR(AssetName.value().c_str()));
			}
		}

		FToolResult Result = Tool.Execute(Args);

		Session.AddImages(Result.Images);
		Session.Log(Result.Output);

		return TCHAR_TO_UTF8(*Result.Output);
	});
});
