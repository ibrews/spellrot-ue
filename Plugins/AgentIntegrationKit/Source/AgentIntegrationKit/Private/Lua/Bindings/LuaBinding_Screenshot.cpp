// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Tools/ScreenshotViewportTool.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

namespace
{
	void SetOptionalString(TSharedPtr<FJsonObject>& Args, const sol::table& T, const char* Key)
	{
		sol::optional<std::string> Val = T.get<sol::optional<std::string>>(Key);
		if (Val.has_value() && !Val.value().empty())
		{
			Args->SetStringField(UTF8_TO_TCHAR(Key), UTF8_TO_TCHAR(Val.value().c_str()));
		}
	}

	void SetOptionalNumber(TSharedPtr<FJsonObject>& Args, const sol::table& T, const char* Key)
	{
		sol::optional<double> Val = T.get<sol::optional<double>>(Key);
		if (Val.has_value())
		{
			Args->SetNumberField(UTF8_TO_TCHAR(Key), Val.value());
		}
	}

	void SetOptionalBool(TSharedPtr<FJsonObject>& Args, const sol::table& T, const char* Key)
	{
		sol::optional<bool> Val = T.get<sol::optional<bool>>(Key);
		if (Val.has_value())
		{
			Args->SetBoolField(UTF8_TO_TCHAR(Key), Val.value());
		}
	}

	void SetOptionalSubObject(TSharedPtr<FJsonObject>& Args, const sol::table& T, const char* Key)
	{
		sol::optional<sol::table> SubTable = T.get<sol::optional<sol::table>>(Key);
		if (SubTable.has_value())
		{
			TSharedPtr<FJsonObject> Sub = MakeShared<FJsonObject>();
			sol::table ST = SubTable.value();

			for (auto& Pair : ST)
			{
				if (Pair.second.is<double>())
				{
					Sub->SetNumberField(
						UTF8_TO_TCHAR(Pair.first.as<std::string>().c_str()),
						Pair.second.as<double>());
				}
			}

			Args->SetObjectField(UTF8_TO_TCHAR(Key), Sub);
		}
	}
}

static TArray<FLuaFunctionDoc> ScreenshotDocs = {
	{ TEXT("screenshot(opts?)"),
	  TEXT("Capture a screenshot from the editor. Returns image via pipeline. "
	       "opts: {mode='active'|'level'|'asset', asset='/Game/Path', viewport_index=0, "
	       "max_dimension=2048, wait_for_ready_ms=1500, hide_overlays=false, "
	       "location={x,y,z}, rotation={pitch,yaw,roll}, fov=90, "
	       "focus_actor='ActorName', view_mode='lit'|'unlit'|'wireframe', "
	       "orbit_yaw=0, orbit_pitch=0, orbit_distance=0}"),
	  TEXT("string (description; image returned via pipeline)") }
};

REGISTER_LUA_BINDING(Screenshot, ScreenshotDocs,
[](sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("screenshot", [&Session](sol::optional<sol::table> Opts) -> std::string
	{
		FScreenshotTool Tool;

		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();

		if (Opts.has_value())
		{
			sol::table T = Opts.value();

			// String params
			SetOptionalString(Args, T, "mode");
			SetOptionalString(Args, T, "asset");
			SetOptionalString(Args, T, "focus_actor");
			SetOptionalString(Args, T, "view_mode");

			// Bool params
			SetOptionalBool(Args, T, "hide_overlays");

			// Number params
			SetOptionalNumber(Args, T, "viewport_index");
			SetOptionalNumber(Args, T, "max_dimension");
			SetOptionalNumber(Args, T, "wait_for_ready_ms");
			SetOptionalNumber(Args, T, "fov");
			SetOptionalNumber(Args, T, "orbit_yaw");
			SetOptionalNumber(Args, T, "orbit_pitch");
			SetOptionalNumber(Args, T, "orbit_distance");

			// Sub-object params (location, rotation)
			SetOptionalSubObject(Args, T, "location");
			SetOptionalSubObject(Args, T, "rotation");
		}

		FToolResult Result = Tool.Execute(Args);

		Session.AddImages(Result.Images);
		Session.Log(Result.Output);

		return TCHAR_TO_UTF8(*Result.Output);
	});
});
