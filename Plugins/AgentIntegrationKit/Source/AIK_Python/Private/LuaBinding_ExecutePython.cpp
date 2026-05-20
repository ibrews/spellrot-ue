// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Tools/ExecutePythonTool.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Modules/ModuleManager.h"
#include "IPythonScriptPlugin.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

static bool AIK_IsPythonFullyAvailable()
{
	IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
	if (!Python || !Python->IsPythonAvailable()) return false;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	return Python->IsPythonInitialized();
#else
	return true;
#endif
}

static TArray<FLuaFunctionDoc> ExecutePythonDocs = {
	{ TEXT("execute_python(code, opts?)"),
	  TEXT("Execute Python code in Unreal Editor. Use 'import unreal' for 1000+ UE APIs. "
	       "opts: {allow_ui=bool, unattended=bool, mode='file'|'statement'|'evaluate', scope='private'|'public'}. "
	       "mode 'evaluate' returns expression value. scope 'public' shares state across calls. Returns output string."),
	  TEXT("string") },
	{ TEXT("is_python_available()"),
	  TEXT("Check if the Python scripting plugin is loaded and initialized. Returns true/false."),
	  TEXT("bool") }
};

REGISTER_LUA_BINDING(ExecutePython, ExecutePythonDocs,
[](sol::state& Lua, FLuaSessionData& Session)
{
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("PythonScriptPlugin")))
	{
		Session.Log(TEXT("[WARN] PythonScriptPlugin plugin is not loaded. Enable it in Edit > Plugins to use this feature."));
		return;
	}

	Lua.set_function("is_python_available", []() -> bool
	{
		return AIK_IsPythonFullyAvailable();
	});

	Lua.set_function("execute_python", [&Session](const std::string& Code, sol::optional<sol::table> Opts) -> std::string
	{
		if (Code.empty())
		{
			Session.Log(TEXT("[ERROR] execute_python: code is required"));
			return "ERROR: code is required";
		}

		FExecutePythonTool Tool;

		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("code"), UTF8_TO_TCHAR(Code.c_str()));

		if (Opts.has_value())
		{
			sol::table T = Opts.value();

			sol::optional<bool> AllowUI = T.get<sol::optional<bool>>("allow_ui");
			if (AllowUI.has_value())
			{
				Args->SetBoolField(TEXT("allow_ui"), AllowUI.value());
			}

			sol::optional<bool> Unattended = T.get<sol::optional<bool>>("unattended");
			if (Unattended.has_value())
			{
				Args->SetBoolField(TEXT("unattended"), Unattended.value());
			}

			sol::optional<std::string> Mode = T.get<sol::optional<std::string>>("mode");
			if (Mode.has_value())
			{
				Args->SetStringField(TEXT("mode"), UTF8_TO_TCHAR(Mode.value().c_str()));
			}

			sol::optional<std::string> Scope = T.get<sol::optional<std::string>>("scope");
			if (Scope.has_value())
			{
				Args->SetStringField(TEXT("scope"), UTF8_TO_TCHAR(Scope.value().c_str()));
			}
		}

		FToolResult Result = Tool.Execute(Args);

		Session.Log(Result.Output);
		Session.AddImages(Result.Images);

		return TCHAR_TO_UTF8(*Result.Output);
	});
});

