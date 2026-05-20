// Copyright 2025-2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include <sol/sol.hpp>

REGISTER_LUA_BINDING(Help, {}, [](sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("help", [&Session](sol::optional<std::string> DomainOpt) -> std::string
	{
		const TArray<FLuaBinding>& All = FLuaBindingRegistry::Get().GetAll();

		if (!DomainOpt.has_value() || DomainOpt.value().empty())
		{
			// Separate domains with docs (global functions) from asset enrichments (empty docs)
			TArray<FString> GlobalDomains;
			TArray<FString> AssetDomains;
			for (const FLuaBinding& B : All)
			{
				if (B.Name == TEXT("Help")) continue;
				if (B.Functions.Num() > 0)
				{
					GlobalDomains.Add(B.Name);
				}
				else
				{
					AssetDomains.Add(B.Name);
				}
			}
			GlobalDomains.Sort();
			AssetDomains.Sort();

			FString Result = TEXT("=== Global Functions ===\n");
			Result += TEXT("Call help('domain') to see signatures and parameters.\n\n");
			for (const FString& D : GlobalDomains)
			{
				Result += FString::Printf(TEXT("  %s\n"), *D);
			}

			Result += TEXT("\n=== Asset-Specific Methods ===\n");
			Result += TEXT("These enrich the table returned by open_asset(). Discover via:\n");
			Result += TEXT("  local a = open_asset(\"/Game/MyAsset\")\n");
			Result += TEXT("  a:help()   -- shows generic + asset-specific methods\n");
			Result += TEXT("  a:info()   -- structured read of asset contents\n\n");
			for (const FString& D : AssetDomains)
			{
				Result += FString::Printf(TEXT("  %s\n"), *D);
			}

			Result += TEXT("\n=== Quick Reference ===\n");
			Result += TEXT("  open_asset(path)              -- open any asset, returns enriched table\n");
			Result += TEXT("  find_assets(path, opts?)       -- search Asset Registry\n");
			Result += TEXT("  create_asset(path, type, opts?) -- create new asset\n");
			Result += TEXT("  find_blueprints(path, opts?)   -- search blueprints\n");
			Result += TEXT("  list_files(path, opts?)        -- list files / search code\n");
			Result += TEXT("  help('domain')                 -- detailed domain docs\n");
			Session.Log(Result);
			return TCHAR_TO_UTF8(*Result);
		}

		// Find matching binding (case-insensitive)
		FString Query = UTF8_TO_TCHAR(DomainOpt.value().c_str());

		for (const FLuaBinding& B : All)
		{
			if (B.Name.Equals(Query, ESearchCase::IgnoreCase))
			{
				if (B.Functions.Num() == 0)
				{
					FString Msg = FString::Printf(
						TEXT("'%s' is an asset enrichment — methods are on the asset object.\n\n")
						TEXT("Usage:\n")
						TEXT("  local a = open_asset(\"/Game/MyAsset\")  -- returns enriched table\n")
						TEXT("  a:help()   -- shows all available methods (generic + %s-specific)\n")
						TEXT("  a:info()   -- structured summary of asset contents\n")
						TEXT("  a:list(\"...\")  / a:add(\"...\", params)  / a:configure(\"...\", id, params)\n"),
						*B.Name, *B.Name);
					Session.Log(Msg);
					return TCHAR_TO_UTF8(*Msg);
				}

				FString Result = FString::Printf(TEXT("Functions in '%s':\n"), *B.Name);
				for (const FLuaFunctionDoc& Func : B.Functions)
				{
					Result += FString::Printf(TEXT("\n  %s\n    %s\n"), *Func.Signature, *Func.Description);
					if (!Func.Returns.IsEmpty())
					{
						Result += FString::Printf(TEXT("    Returns: %s\n"), *Func.Returns);
					}
				}
				Session.Log(Result);
				return TCHAR_TO_UTF8(*Result);
			}
		}

		FString Msg = FString::Printf(TEXT("Unknown domain '%s'. Call help() to see available domains."), *Query);
		Session.Log(Msg);
		return TCHAR_TO_UTF8(*Msg);
	});
});
