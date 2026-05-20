#include "Lua/NeoLuaState.h"
#include "Lua/LuaBindingRegistry.h"
#include "Lua/LuaGraphFinalizer.h"
#include "AIKAnalytics.h"
#include "Misc/Paths.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

DEFINE_LOG_CATEGORY_STATIC(LogNeoLua, Log, All);

void FNeoLuaState::AppendTrace(const FString& Entry)
{
	Trace.Add(Entry);
}

FScriptResult FNeoLuaState::Execute(const FString& Script)
{
	FScriptResult Result;
	Trace.Reset();

	sol::state Lua;
	Lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::table, sol::lib::math);

	FLuaSessionData Session;

	// Built-in: log and print
	Lua.set_function("log", [&Session](const std::string& Msg)
	{
		Session.Log(UTF8_TO_TCHAR(Msg.c_str()));
	});

	Lua.set_function("print", [&Session](sol::variadic_args Va, sol::this_state S)
	{
		sol::state_view LuaView(S);
		sol::function Tostring = LuaView["tostring"];
		FString Line;
		for (auto Arg : Va)
		{
			if (Line.Len() > 0) Line += TEXT("\t");
			// Use Lua's tostring() for proper type conversion — matches standard print() behavior
			// (handles numbers, bools, tables, nil, userdata, etc.)
			std::string Str = Tostring(Arg).get<std::string>();
			Line += UTF8_TO_TCHAR(Str.c_str());
		}
		Session.Log(Line);
	});

	// Built-in: project_dir — absolute path to the UE project root (with trailing slash)
	Lua.set_function("project_dir", []() -> std::string
	{
		FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		return TCHAR_TO_UTF8(*Dir);
	});

	// Apply all registered bindings
	for (const FLuaBinding& Binding : FLuaBindingRegistry::Get().GetAll())
	{
		Binding.Bind.Execute(Lua, Session);
	}

	// Infinite loop protection
	lua_State* L = Lua.lua_state();
	lua_sethook(L, [](lua_State* InL, lua_Debug*)
	{
		luaL_error(InL, "Script exceeded instruction limit (possible infinite loop)");
	}, LUA_MASKCOUNT, 1047382);

	auto ExecResult = Lua.safe_script(TCHAR_TO_UTF8(*Script), sol::script_pass_on_error);

	if (!ExecResult.valid())
	{
		sol::error Err = ExecResult;
		Result.bSuccess = false;
		Result.Error = UTF8_TO_TCHAR(Err.what());
		Session.Log(FString::Printf(TEXT("[ERROR] %s"), *Result.Error));
		UE_LOG(LogNeoLua, Warning, TEXT("Lua error: %s"), *Result.Error);

		// Analytics: record the Lua error (path-sanitized, no user code)
		FAIKAnalytics::Get().RecordLuaError(TEXT("lua_runtime"), Result.Error);
	}
	else
	{
		Result.bSuccess = true;

		// Capture return value if it's a string
		if (ExecResult.return_count() > 0)
		{
			sol::object RetVal = ExecResult.get<sol::object>(0);
			if (RetVal.is<std::string>())
			{
				Result.ReturnValue = UTF8_TO_TCHAR(RetVal.as<std::string>().c_str());
			}
		}
	}

	// Finalize all graphs that were mutated during this script execution.
	// This handles Material (LinkExpressions + recompile), BT (UpdateAsset),
	// EQS (UpdateAsset), MetaSound/SoundCue/PCG (NotifyGraphChanged), etc.
	for (auto& Pair : Session.DirtyGraphs)
	{
		LuaGraphFinalizer::FinalizeGraph(Pair.Key, Pair.Value);
	}

	Result.Trace = Session.Trace;
	Result.Images = MoveTemp(Session.Images);
	return Result;
}
