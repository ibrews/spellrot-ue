// Copyright 2025-2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "LocalizationSettings.h"
#include "LocalizationTargetTypes.h"
#include "LocalizationConfigurationScript.h"
#include "LocalizationCommandletExecution.h"
#include "LocalizationModule.h"
#include "LocTextHelper.h"
#include "PortableObjectPipeline.h"
#include "TextLocalizationResourceGenerator.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Internationalization/StringTableRegistry.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/Culture.h"
#include "Internationalization/TextLocalizationManager.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformProcess.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ─── Documentation ───

static TArray<FLuaFunctionDoc> LocalizationDocs = {
	{ TEXT("loc_list_targets(set?)"), TEXT("List localization targets — set: game (default), engine"), TEXT("table") },
	{ TEXT("loc_get_target(name, set?)"), TEXT("Get detailed info for a localization target"), TEXT("table") },
	{ TEXT("loc_add_culture(target_name, culture_name, set?)"), TEXT("Add a culture to a localization target"), TEXT("table") },
	{ TEXT("loc_remove_culture(target_name, culture_name, set?)"), TEXT("Remove a culture from a localization target"), TEXT("table") },
	{ TEXT("loc_gather(target_name, set?)"), TEXT("Run text gathering for a localization target"), TEXT("table") },
	{ TEXT("loc_export_po(target_name, culture?, output_path?, set?)"), TEXT("Export PO files for a target — all cultures if culture is nil"), TEXT("table") },
	{ TEXT("loc_import_po(target_name, culture, po_path, set?)"), TEXT("Import a PO file into a localization target"), TEXT("table") },
	{ TEXT("loc_compile(target_name, culture?, set?)"), TEXT("Compile localization data to .locres"), TEXT("table") },
	{ TEXT("loc_word_count(target_name, set?)"), TEXT("Generate word count report CSV for a localization target"), TEXT("table") },
	{ TEXT("loc_get_file_paths(target_name, set?)"), TEXT("Get file paths (manifest, archives, PO, locres) for a target"), TEXT("table") },
	{ TEXT("loc_get_culture()"), TEXT("Get the current culture/language name"), TEXT("string") },
	{ TEXT("loc_set_culture(culture_name)"), TEXT("Set the current language and locale"), TEXT("bool") },
	{ TEXT("loc_list_cultures()"), TEXT("List all available cultures"), TEXT("table") },
	{ TEXT("stringtable_list()"), TEXT("List all registered string tables"), TEXT("table") },
	{ TEXT("stringtable_create(table_id, namespace)"), TEXT("Create and register a new string table"), TEXT("table") },
	{ TEXT("stringtable_get(table_id_or_path)"), TEXT("Get all entries from a string table"), TEXT("table") },
	{ TEXT("stringtable_set_entry(table_id_or_path, key, value)"), TEXT("Set or add an entry in a string table"), TEXT("table") },
	{ TEXT("stringtable_remove_entry(table_id_or_path, key)"), TEXT("Remove an entry from a string table"), TEXT("table") },
	{ TEXT("stringtable_clear(table_id_or_path)"), TEXT("Remove all entries from a string table"), TEXT("table") },
	{ TEXT("stringtable_set_namespace(table_id_or_path, namespace)"), TEXT("Set the namespace of a string table"), TEXT("table") },
	{ TEXT("stringtable_set_metadata(table_id_or_path, key, meta_id, value)"), TEXT("Set meta-data on a string table entry"), TEXT("table") },
	{ TEXT("stringtable_get_metadata(table_id_or_path, key, meta_id?)"), TEXT("Get meta-data from a string table entry — all meta-data if meta_id is nil"), TEXT("table") },
	{ TEXT("stringtable_remove_metadata(table_id_or_path, key, meta_id)"), TEXT("Remove meta-data from a string table entry"), TEXT("table") },
	{ TEXT("stringtable_export_csv(table_id_or_path, output_path)"), TEXT("Export a string table to CSV"), TEXT("table") },
	{ TEXT("stringtable_import_csv(table_id_or_path, csv_path)"), TEXT("Import entries from a CSV file into a string table"), TEXT("table") },
	{ TEXT("loc_preview(culture_name?)"), TEXT("Enable/disable game localization preview in editor"), TEXT("table") },
	{ TEXT("loc_refresh()"), TEXT("Refresh all localization resources"), TEXT("table") },
};

// ─── Helpers ───

static ULocalizationTarget* FindLocTarget(const std::string& Name, const std::string& Set)
{
	bool bEngine = (Set == "engine");
	ULocalizationTargetSet* TargetSet = bEngine
		? ULocalizationSettings::GetEngineTargetSet()
		: ULocalizationSettings::GetGameTargetSet();
	if (!TargetSet) return nullptr;

	FString TargetName = UTF8_TO_TCHAR(Name.c_str());
	for (ULocalizationTarget* Target : TargetSet->TargetObjects)
	{
		if (Target && Target->Settings.Name.Equals(TargetName, ESearchCase::IgnoreCase))
			return Target;
	}
	return nullptr;
}

static const TCHAR* ConflictStatusToString(ELocalizationTargetConflictStatus Status)
{
	switch (Status)
	{
	case ELocalizationTargetConflictStatus::Unknown:          return TEXT("unknown");
	case ELocalizationTargetConflictStatus::ConflictsPresent: return TEXT("conflicts_present");
	case ELocalizationTargetConflictStatus::Clear:            return TEXT("clear");
	default:                                                  return TEXT("unknown");
	}
}

struct FStringTableLookupResult
{
	FStringTablePtr Table;
	FName TableId;
};

static FStringTableLookupResult FindOrLoadStringTable(const std::string& TableIdOrPath)
{
	FName TableId = FName(UTF8_TO_TCHAR(TableIdOrPath.c_str()));

	// Try registry first
	FStringTablePtr Table = FStringTableRegistry::Get().FindMutableStringTable(TableId);
	if (Table.IsValid())
	{
		return { Table, TableId };
	}

	// Try loading as asset path
	FString AssetPath = UTF8_TO_TCHAR(TableIdOrPath.c_str());
	UStringTable* Asset = LoadObject<UStringTable>(nullptr, *AssetPath);
	if (Asset)
	{
		return { Asset->GetMutableStringTable(), Asset->GetStringTableId() };
	}

	return { nullptr, NAME_None };
}

static void TickEditorWhileWaiting()
{
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().PumpMessages();
		FSlateApplication::Get().Tick();
	}
	else
	{
		FPlatformProcess::SleepNoStats(0.02f);
	}
}

static void Loc_ApplyRichTextTagValidation(EGenerateLocResFlags& Flags, const FLocalizationCompilationSettings& CompileSettings)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	if (CompileSettings.ValidateRichTextTags)
	{
		Flags |= EGenerateLocResFlags::ValidateRichTextTags;
	}
#endif
}

// ─── Helpers for version-conditional StringTable API (cannot use #if inside macro arguments) ───

static void Loc_NewLocTable(FName TableId, const FString& Namespace)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	FStringTableRegistry::Get().Internal_NewLocTable(TableId, FTextKey(Namespace));
#else
	FStringTableRegistry::Get().Internal_NewLocTable(TableId, Namespace);
#endif
}

static void Loc_SetSourceString(FStringTablePtr& Table, const FString& Key, const FString& Value)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	Table->SetSourceString(FTextKey(Key), Value);
#else
	Table->SetSourceString(Key, Value);
#endif
}

static bool Loc_GetSourceString(FStringTablePtr& Table, const FString& Key, FString& OutValue)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	return Table->GetSourceString(FTextKey(Key), OutValue);
#else
	return Table->GetSourceString(Key, OutValue);
#endif
}

static void Loc_RemoveSourceString(FStringTablePtr& Table, const FString& Key)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	Table->RemoveSourceString(FTextKey(Key));
#else
	Table->RemoveSourceString(Key);
#endif
}

static void Loc_SetNamespace(FStringTablePtr& Table, const FString& Namespace)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	Table->SetNamespace(FTextKey(Namespace));
#else
	Table->SetNamespace(Namespace);
#endif
}

static void Loc_SetMetaData(FStringTablePtr& Table, const FString& Key, FName MetaId, const FString& Value)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	Table->SetMetaData(FTextKey(Key), MetaId, Value);
#else
	Table->SetMetaData(Key, MetaId, Value);
#endif
}

static FString Loc_GetMetaData(FStringTablePtr& Table, const FString& Key, FName MetaId)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	return Table->GetMetaData(FTextKey(Key), MetaId);
#else
	return Table->GetMetaData(Key, MetaId);
#endif
}

static void Loc_EnumerateMetaData(FStringTablePtr& Table, const FString& Key, TFunctionRef<bool(FName, const FString&)> Callback)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	Table->EnumerateMetaData(FTextKey(Key), Callback);
#else
	Table->EnumerateMetaData(Key, Callback);
#endif
}

static void Loc_RemoveMetaData(FStringTablePtr& Table, const FString& Key, FName MetaId)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	Table->RemoveMetaData(FTextKey(Key), MetaId);
#else
	Table->RemoveMetaData(Key, MetaId);
#endif
}

// ─── Implementation ───

REGISTER_LUA_BINDING(Localization, LocalizationDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	// ════════════════════════════════════════════════════════════════════
	// loc_list_targets(set?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("loc_list_targets", [&Session](sol::optional<std::string> SetArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		std::string SetStr = SetArg.has_value() ? SetArg.value() : "game";
		bool bEngine = (SetStr == "engine");
		ULocalizationTargetSet* TargetSet = bEngine
			? ULocalizationSettings::GetEngineTargetSet()
			: ULocalizationSettings::GetGameTargetSet();

		if (!TargetSet)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] loc_list_targets: could not get %s target set"), bEngine ? TEXT("engine") : TEXT("game")));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		sol::table Result = LuaView.create_table();
		int32 Idx = 1;

		for (ULocalizationTarget* Target : TargetSet->TargetObjects)
		{
			if (!Target) continue;

			const FLocalizationTargetSettings& Settings = Target->Settings;

			sol::table Entry = LuaView.create_table();
			Entry["name"] = std::string(TCHAR_TO_UTF8(*Settings.Name));
			Entry["guid"] = std::string(TCHAR_TO_UTF8(*Settings.Guid.ToString()));

			// Native culture
			if (Settings.NativeCultureIndex >= 0 && Settings.NativeCultureIndex < Settings.SupportedCulturesStatistics.Num())
			{
				Entry["native_culture"] = std::string(TCHAR_TO_UTF8(*Settings.SupportedCulturesStatistics[Settings.NativeCultureIndex].CultureName));
			}
			else
			{
				Entry["native_culture"] = std::string("");
			}

			// Cultures array
			sol::table Cultures = LuaView.create_table();
			int32 CIdx = 1;
			for (const FCultureStatistics& CultureStat : Settings.SupportedCulturesStatistics)
			{
				Cultures[CIdx++] = std::string(TCHAR_TO_UTF8(*CultureStat.CultureName));
			}
			Entry["cultures"] = Cultures;

			Entry["conflict_status"] = std::string(TCHAR_TO_UTF8(ConflictStatusToString(Settings.ConflictStatus)));

			Result[Idx++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] loc_list_targets('%s') -> %d targets"), UTF8_TO_TCHAR(SetStr.c_str()), Idx - 1));
		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// loc_get_target(name, set?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("loc_get_target", [&Session](const std::string& Name, sol::optional<std::string> SetArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (Name.empty())
		{
			Session.Log(TEXT("[FAIL] loc_get_target: target name is required"));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		std::string SetStr = SetArg.has_value() ? SetArg.value() : "game";
		ULocalizationTarget* Target = FindLocTarget(Name, SetStr);
		if (!Target)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] loc_get_target: target '%s' not found in '%s' set"),
				UTF8_TO_TCHAR(Name.c_str()), UTF8_TO_TCHAR(SetStr.c_str())));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		const FLocalizationTargetSettings& Settings = Target->Settings;

		sol::table Result = LuaView.create_table();
		Result["name"] = std::string(TCHAR_TO_UTF8(*Settings.Name));
		Result["guid"] = std::string(TCHAR_TO_UTF8(*Settings.Guid.ToString()));

		// Native culture
		if (Settings.NativeCultureIndex >= 0 && Settings.NativeCultureIndex < Settings.SupportedCulturesStatistics.Num())
		{
			Result["native_culture"] = std::string(TCHAR_TO_UTF8(*Settings.SupportedCulturesStatistics[Settings.NativeCultureIndex].CultureName));
		}
		else
		{
			Result["native_culture"] = std::string("");
		}

		// Cultures with word counts
		sol::table Cultures = LuaView.create_table();
		int32 CIdx = 1;
		for (const FCultureStatistics& CultureStat : Settings.SupportedCulturesStatistics)
		{
			sol::table CEntry = LuaView.create_table();
			CEntry["name"] = std::string(TCHAR_TO_UTF8(*CultureStat.CultureName));
			CEntry["word_count"] = static_cast<int64_t>(CultureStat.WordCount);
			Cultures[CIdx++] = CEntry;
		}
		Result["cultures"] = Cultures;

		// Gather configuration flags
		Result["gather_from_packages"] = Settings.GatherFromPackages.IsEnabled;
		Result["gather_from_text_files"] = Settings.GatherFromTextFiles.IsEnabled;
		Result["gather_from_metadata"] = Settings.GatherFromMetaData.IsEnabled;
		Result["has_dependencies"] = Settings.TargetDependencies.Num() > 0;

		Session.Log(FString::Printf(TEXT("[OK] loc_get_target '%s'"), UTF8_TO_TCHAR(Name.c_str())));
		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// loc_add_culture(target_name, culture_name, set?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("loc_add_culture", [&Session](const std::string& TargetName, const std::string& CultureName,
		sol::optional<std::string> SetArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (TargetName.empty() || CultureName.empty())
		{
			Session.Log(TEXT("[FAIL] loc_add_culture: target_name and culture_name are required"));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		std::string SetStr = SetArg.has_value() ? SetArg.value() : "game";
		ULocalizationTarget* Target = FindLocTarget(TargetName, SetStr);
		if (!Target)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] loc_add_culture: target '%s' not found"), UTF8_TO_TCHAR(TargetName.c_str())));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		FString FCultureName = UTF8_TO_TCHAR(CultureName.c_str());

		// Validate that the culture name is a known ICU culture
		FCulturePtr CulturePtr = FInternationalization::Get().GetCulture(FCultureName);
		if (!CulturePtr.IsValid())
		{
			sol::table Result = LuaView.create_table();
			Result["success"] = false;
			Result["error"] = std::string("Unknown culture name: ") + CultureName;
			Session.Log(FString::Printf(TEXT("[FAIL] loc_add_culture: unknown culture '%s'"), *FCultureName));
			return sol::make_object(LuaView, Result);
		}

		// Check if culture already exists
		for (const FCultureStatistics& Existing : Target->Settings.SupportedCulturesStatistics)
		{
			if (Existing.CultureName.Equals(FCultureName, ESearchCase::IgnoreCase))
			{
				sol::table Result = LuaView.create_table();
				Result["success"] = false;
				Result["error"] = "Culture already exists in target";
				Session.Log(FString::Printf(TEXT("[FAIL] loc_add_culture: culture '%s' already exists in '%s'"),
					UTF8_TO_TCHAR(CultureName.c_str()), UTF8_TO_TCHAR(TargetName.c_str())));
				return sol::make_object(LuaView, Result);
			}
		}

		// Add the culture
		Target->Settings.SupportedCulturesStatistics.Add(FCultureStatistics(FCultureName));

		// Save configuration
		GetMutableDefault<ULocalizationSettings>()->SaveConfig();

		sol::table Result = LuaView.create_table();
		Result["success"] = true;
		Result["target"] = TargetName;
		Result["culture"] = CultureName;

		Session.Log(FString::Printf(TEXT("[OK] loc_add_culture '%s' to target '%s'"),
			UTF8_TO_TCHAR(CultureName.c_str()), UTF8_TO_TCHAR(TargetName.c_str())));
		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// loc_remove_culture(target_name, culture_name, set?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("loc_remove_culture", [&Session](const std::string& TargetName, const std::string& CultureName,
		sol::optional<std::string> SetArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (TargetName.empty() || CultureName.empty())
		{
			Session.Log(TEXT("[FAIL] loc_remove_culture: target_name and culture_name are required"));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		std::string SetStr = SetArg.has_value() ? SetArg.value() : "game";
		ULocalizationTarget* Target = FindLocTarget(TargetName, SetStr);
		if (!Target)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] loc_remove_culture: target '%s' not found"), UTF8_TO_TCHAR(TargetName.c_str())));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		FString FCultureName = UTF8_TO_TCHAR(CultureName.c_str());

		// Find and remove the culture
		int32 FoundIndex = INDEX_NONE;
		for (int32 i = 0; i < Target->Settings.SupportedCulturesStatistics.Num(); ++i)
		{
			if (Target->Settings.SupportedCulturesStatistics[i].CultureName.Equals(FCultureName, ESearchCase::IgnoreCase))
			{
				FoundIndex = i;
				break;
			}
		}

		if (FoundIndex == INDEX_NONE)
		{
			sol::table Result = LuaView.create_table();
			Result["success"] = false;
			Result["error"] = "Culture not found in target";
			Session.Log(FString::Printf(TEXT("[FAIL] loc_remove_culture: culture '%s' not found in '%s'"),
				UTF8_TO_TCHAR(CultureName.c_str()), UTF8_TO_TCHAR(TargetName.c_str())));
			return sol::make_object(LuaView, Result);
		}

		// Do not allow removing the native culture
		if (FoundIndex == Target->Settings.NativeCultureIndex)
		{
			sol::table Result = LuaView.create_table();
			Result["success"] = false;
			Result["error"] = "Cannot remove the native culture";
			Session.Log(FString::Printf(TEXT("[FAIL] loc_remove_culture: cannot remove native culture '%s' from '%s'"),
				UTF8_TO_TCHAR(CultureName.c_str()), UTF8_TO_TCHAR(TargetName.c_str())));
			return sol::make_object(LuaView, Result);
		}

		// Delete associated files for this culture
		Target->DeleteFiles(&FCultureName);

		// Remove the culture statistics entry
		Target->Settings.SupportedCulturesStatistics.RemoveAt(FoundIndex);

		// Adjust native culture index if needed
		if (Target->Settings.NativeCultureIndex > FoundIndex)
		{
			Target->Settings.NativeCultureIndex--;
		}

		// Save configuration
		GetMutableDefault<ULocalizationSettings>()->SaveConfig();

		sol::table Result = LuaView.create_table();
		Result["success"] = true;
		Result["target"] = TargetName;
		Result["culture"] = CultureName;

		Session.Log(FString::Printf(TEXT("[OK] loc_remove_culture '%s' from target '%s'"),
			UTF8_TO_TCHAR(CultureName.c_str()), UTF8_TO_TCHAR(TargetName.c_str())));
		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// loc_gather(target_name, set?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("loc_gather", [&Session](const std::string& TargetName, sol::optional<std::string> SetArg,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (TargetName.empty())
		{
			Session.Log(TEXT("[FAIL] loc_gather: target_name is required"));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		std::string SetStr = SetArg.has_value() ? SetArg.value() : "game";
		ULocalizationTarget* Target = FindLocTarget(TargetName, SetStr);
		if (!Target)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] loc_gather: target '%s' not found"), UTF8_TO_TCHAR(TargetName.c_str())));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		// Generate the gather config and write it to disk
		FLocalizationConfigurationScript GatherConfig = LocalizationConfigurationScript::GenerateGatherTextConfigFile(Target);
		FString ConfigPath = LocalizationConfigurationScript::GetGatherTextConfigPath(Target);

		bool bConfigWritten = GatherConfig.WriteWithSCC(ConfigPath);
		if (!bConfigWritten)
		{
			sol::table Result = LuaView.create_table();
			Result["success"] = false;
			Result["error"] = "Failed to write gather config file";
			Session.Log(FString::Printf(TEXT("[FAIL] loc_gather: could not write config for '%s'"), UTF8_TO_TCHAR(TargetName.c_str())));
			return sol::make_object(LuaView, Result);
		}

		// Execute the commandlet process (headless, no SWindow)
		TSharedPtr<FLocalizationCommandletProcess> Process = FLocalizationCommandletProcess::Execute(ConfigPath, true);
		bool bSuccess = false;
		FString Output;

		if (Process.IsValid())
		{
			// Wait for the process to complete, ticking Slate to keep editor responsive
			while (FPlatformProcess::IsProcRunning(Process->GetHandle()))
			{
				FString NewOutput = FPlatformProcess::ReadPipe(Process->GetReadPipe());
				if (!NewOutput.IsEmpty())
				{
					Output += NewOutput;
				}
				TickEditorWhileWaiting();
			}

			// Read any remaining output after process exits
			FString FinalOutput = FPlatformProcess::ReadPipe(Process->GetReadPipe());
			if (!FinalOutput.IsEmpty())
			{
				Output += FinalOutput;
			}

			// Get exit code
			int32 ReturnCode = 0;
			FPlatformProcess::GetProcReturnCode(Process->GetHandle(), &ReturnCode);
			bSuccess = (ReturnCode == 0);
		}

		sol::table Result = LuaView.create_table();
		Result["success"] = bSuccess;
		Result["target"] = TargetName;
		if (!Output.IsEmpty())
		{
			Result["output"] = std::string(TCHAR_TO_UTF8(*Output));
		}

		if (bSuccess)
		{
			Session.Log(FString::Printf(TEXT("[OK] loc_gather '%s'"), UTF8_TO_TCHAR(TargetName.c_str())));
		}
		else
		{
			Result["error"] = "Gather commandlet process failed or could not be started";
			Session.Log(FString::Printf(TEXT("[FAIL] loc_gather '%s'"), UTF8_TO_TCHAR(TargetName.c_str())));
		}

		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// loc_export_po(target_name, culture?, output_path?, set?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("loc_export_po", [&Session](const std::string& TargetName,
		sol::optional<std::string> CultureArg, sol::optional<std::string> OutputPathArg,
		sol::optional<std::string> SetArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (TargetName.empty())
		{
			Session.Log(TEXT("[FAIL] loc_export_po: target_name is required"));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		std::string SetStr = SetArg.has_value() ? SetArg.value() : "game";
		ULocalizationTarget* Target = FindLocTarget(TargetName, SetStr);
		if (!Target)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] loc_export_po: target '%s' not found"), UTF8_TO_TCHAR(TargetName.c_str())));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		const FLocalizationTargetSettings& Settings = Target->Settings;

		// Determine native culture and foreign cultures
		FString NativeCulture;
		if (Settings.NativeCultureIndex >= 0 && Settings.NativeCultureIndex < Settings.SupportedCulturesStatistics.Num())
		{
			NativeCulture = Settings.SupportedCulturesStatistics[Settings.NativeCultureIndex].CultureName;
		}

		TArray<FString> ForeignCultures;
		for (int32 i = 0; i < Settings.SupportedCulturesStatistics.Num(); ++i)
		{
			if (i != Settings.NativeCultureIndex)
			{
				ForeignCultures.Add(Settings.SupportedCulturesStatistics[i].CultureName);
			}
		}

		// Build the LocTextHelper
		FString DataDir = LocalizationConfigurationScript::GetDataDirectory(Target);
		FString ManifestName = LocalizationConfigurationScript::GetManifestFileName(Target);
		FString ArchiveName = LocalizationConfigurationScript::GetArchiveFileName(Target);

		FLocTextHelper LocTextHelper(DataDir, ManifestName, ArchiveName, NativeCulture, ForeignCultures, nullptr);

		// Load manifest and archives
		FText LoadError;
		if (!LocTextHelper.LoadManifest(ELocTextHelperLoadFlags::Load, &LoadError))
		{
			sol::table Result = LuaView.create_table();
			Result["success"] = false;
			Result["error"] = std::string(TCHAR_TO_UTF8(*FString::Printf(TEXT("Failed to load manifest: %s"), *LoadError.ToString())));
			Session.Log(FString::Printf(TEXT("[FAIL] loc_export_po: manifest load failed for '%s'"), UTF8_TO_TCHAR(TargetName.c_str())));
			return sol::make_object(LuaView, Result);
		}

		if (!LocTextHelper.LoadAllArchives(ELocTextHelperLoadFlags::Load, &LoadError))
		{
			sol::table Result = LuaView.create_table();
			Result["success"] = false;
			Result["error"] = std::string(TCHAR_TO_UTF8(*FString::Printf(TEXT("Failed to load archives: %s"), *LoadError.ToString())));
			Session.Log(FString::Printf(TEXT("[FAIL] loc_export_po: archive load failed for '%s'"), UTF8_TO_TCHAR(TargetName.c_str())));
			return sol::make_object(LuaView, Result);
		}

		ELocalizedTextCollapseMode CollapseMode = Settings.ExportSettings.CollapseMode;
		EPortableObjectFormat POFormat = Settings.ExportSettings.POFormat;
		bool bPersistComments = Settings.ExportSettings.ShouldPersistCommentsOnExport;

		bool bSuccess = false;

		if (CultureArg.has_value() && !CultureArg.value().empty())
		{
			// Export single culture
			FString Culture = UTF8_TO_TCHAR(CultureArg.value().c_str());
			FString POPath;

			if (OutputPathArg.has_value() && !OutputPathArg.value().empty())
			{
				POPath = UTF8_TO_TCHAR(OutputPathArg.value().c_str());
			}
			else
			{
				POPath = LocalizationConfigurationScript::GetDefaultPOPath(Target, Culture);
			}

			bSuccess = PortableObjectPipeline::Export(LocTextHelper, Culture, POPath, CollapseMode, POFormat, bPersistComments);
		}
		else
		{
			// Export all cultures
			FString POOutputDir;
			if (OutputPathArg.has_value() && !OutputPathArg.value().empty())
			{
				POOutputDir = UTF8_TO_TCHAR(OutputPathArg.value().c_str());
			}
			else
			{
				POOutputDir = DataDir;
			}

			FString POFileName = LocalizationConfigurationScript::GetDefaultPOFileName(Target);
			bSuccess = PortableObjectPipeline::ExportAll(LocTextHelper, POOutputDir, POFileName, CollapseMode, POFormat, bPersistComments, true);
		}

		sol::table Result = LuaView.create_table();
		Result["success"] = bSuccess;
		Result["target"] = TargetName;

		if (bSuccess)
		{
			Session.Log(FString::Printf(TEXT("[OK] loc_export_po '%s'"), UTF8_TO_TCHAR(TargetName.c_str())));
		}
		else
		{
			Result["error"] = "PO export failed";
			Session.Log(FString::Printf(TEXT("[FAIL] loc_export_po '%s'"), UTF8_TO_TCHAR(TargetName.c_str())));
		}

		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// loc_import_po(target_name, culture, po_path, set?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("loc_import_po", [&Session](const std::string& TargetName, const std::string& CultureName,
		const std::string& POPath, sol::optional<std::string> SetArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (TargetName.empty() || CultureName.empty() || POPath.empty())
		{
			Session.Log(TEXT("[FAIL] loc_import_po: target_name, culture, and po_path are required"));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		std::string SetStr = SetArg.has_value() ? SetArg.value() : "game";
		ULocalizationTarget* Target = FindLocTarget(TargetName, SetStr);
		if (!Target)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] loc_import_po: target '%s' not found"), UTF8_TO_TCHAR(TargetName.c_str())));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		const FLocalizationTargetSettings& Settings = Target->Settings;

		FString NativeCulture;
		if (Settings.NativeCultureIndex >= 0 && Settings.NativeCultureIndex < Settings.SupportedCulturesStatistics.Num())
		{
			NativeCulture = Settings.SupportedCulturesStatistics[Settings.NativeCultureIndex].CultureName;
		}

		TArray<FString> ForeignCultures;
		for (int32 i = 0; i < Settings.SupportedCulturesStatistics.Num(); ++i)
		{
			if (i != Settings.NativeCultureIndex)
			{
				ForeignCultures.Add(Settings.SupportedCulturesStatistics[i].CultureName);
			}
		}

		FString DataDir = LocalizationConfigurationScript::GetDataDirectory(Target);
		FString ManifestName = LocalizationConfigurationScript::GetManifestFileName(Target);
		FString ArchiveName = LocalizationConfigurationScript::GetArchiveFileName(Target);

		FLocTextHelper LocTextHelper(DataDir, ManifestName, ArchiveName, NativeCulture, ForeignCultures, nullptr);

		FText LoadError;
		if (!LocTextHelper.LoadManifest(ELocTextHelperLoadFlags::Load, &LoadError))
		{
			sol::table Result = LuaView.create_table();
			Result["success"] = false;
			Result["error"] = std::string(TCHAR_TO_UTF8(*FString::Printf(TEXT("Failed to load manifest: %s"), *LoadError.ToString())));
			Session.Log(FString::Printf(TEXT("[FAIL] loc_import_po: manifest load failed for '%s'"), UTF8_TO_TCHAR(TargetName.c_str())));
			return sol::make_object(LuaView, Result);
		}

		if (!LocTextHelper.LoadAllArchives(ELocTextHelperLoadFlags::LoadOrCreate, &LoadError))
		{
			sol::table Result = LuaView.create_table();
			Result["success"] = false;
			Result["error"] = std::string(TCHAR_TO_UTF8(*FString::Printf(TEXT("Failed to load archives: %s"), *LoadError.ToString())));
			Session.Log(FString::Printf(TEXT("[FAIL] loc_import_po: archive load failed for '%s'"), UTF8_TO_TCHAR(TargetName.c_str())));
			return sol::make_object(LuaView, Result);
		}

		FString FCulture = UTF8_TO_TCHAR(CultureName.c_str());
		FString FPOPath = UTF8_TO_TCHAR(POPath.c_str());

		ELocalizedTextCollapseMode CollapseMode = Settings.ExportSettings.CollapseMode;
		EPortableObjectFormat POFormat = Settings.ExportSettings.POFormat;

		bool bSuccess = PortableObjectPipeline::Import(LocTextHelper, FCulture, FPOPath, CollapseMode, POFormat);

		if (bSuccess)
		{
			// Save the archive after import
			LocTextHelper.SaveAllArchives();
		}

		sol::table Result = LuaView.create_table();
		Result["success"] = bSuccess;
		Result["target"] = TargetName;
		Result["culture"] = CultureName;

		if (bSuccess)
		{
			Session.Log(FString::Printf(TEXT("[OK] loc_import_po '%s' culture='%s'"),
				UTF8_TO_TCHAR(TargetName.c_str()), UTF8_TO_TCHAR(CultureName.c_str())));
		}
		else
		{
			Result["error"] = "PO import failed";
			Session.Log(FString::Printf(TEXT("[FAIL] loc_import_po '%s' culture='%s'"),
				UTF8_TO_TCHAR(TargetName.c_str()), UTF8_TO_TCHAR(CultureName.c_str())));
		}

		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// loc_compile(target_name, culture?, set?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("loc_compile", [&Session](const std::string& TargetName,
		sol::optional<std::string> CultureArg, sol::optional<std::string> SetArg,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (TargetName.empty())
		{
			Session.Log(TEXT("[FAIL] loc_compile: target_name is required"));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		std::string SetStr = SetArg.has_value() ? SetArg.value() : "game";
		ULocalizationTarget* Target = FindLocTarget(TargetName, SetStr);
		if (!Target)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] loc_compile: target '%s' not found"), UTF8_TO_TCHAR(TargetName.c_str())));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		const FLocalizationTargetSettings& Settings = Target->Settings;

		// Build generate flags from compile settings
		EGenerateLocResFlags GenerateFlags = EGenerateLocResFlags::None;
		if (Settings.CompileSettings.SkipSourceCheck)
		{
			GenerateFlags |= EGenerateLocResFlags::AllowStaleTranslations;
		}
		if (Settings.CompileSettings.ValidateFormatPatterns)
		{
			GenerateFlags |= EGenerateLocResFlags::ValidateFormatPatterns;
		}
		if (Settings.CompileSettings.ValidateSafeWhitespace)
		{
			GenerateFlags |= EGenerateLocResFlags::ValidateSafeWhitespace;
		}
		Loc_ApplyRichTextTagValidation(GenerateFlags, Settings.CompileSettings);

		// Generate compile config
		TOptional<FString> OptCulture;
		if (CultureArg.has_value() && !CultureArg.value().empty())
		{
			OptCulture = FString(UTF8_TO_TCHAR(CultureArg.value().c_str()));
		}

		FLocalizationConfigurationScript CompileConfig = LocalizationConfigurationScript::GenerateCompileTextConfigFile(Target, OptCulture);
		FString ConfigPath = LocalizationConfigurationScript::GetCompileTextConfigPath(Target, OptCulture);

		bool bConfigWritten = CompileConfig.WriteWithSCC(ConfigPath);
		if (!bConfigWritten)
		{
			sol::table Result = LuaView.create_table();
			Result["success"] = false;
			Result["error"] = "Failed to write compile config file";
			Session.Log(FString::Printf(TEXT("[FAIL] loc_compile: could not write config for '%s'"), UTF8_TO_TCHAR(TargetName.c_str())));
			return sol::make_object(LuaView, Result);
		}

		bool bSuccess = FTextLocalizationResourceGenerator::GenerateLocResAndUpdateLiveEntriesFromConfig(ConfigPath, GenerateFlags);

		sol::table Result = LuaView.create_table();
		Result["success"] = bSuccess;
		Result["target"] = TargetName;

		if (bSuccess)
		{
			Session.Log(FString::Printf(TEXT("[OK] loc_compile '%s'"), UTF8_TO_TCHAR(TargetName.c_str())));
		}
		else
		{
			Result["error"] = "Compilation failed";
			Session.Log(FString::Printf(TEXT("[FAIL] loc_compile '%s'"), UTF8_TO_TCHAR(TargetName.c_str())));
		}

		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// loc_word_count(target_name, set?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("loc_word_count", [&Session](const std::string& TargetName, sol::optional<std::string> SetArg,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (TargetName.empty())
		{
			Session.Log(TEXT("[FAIL] loc_word_count: target_name is required"));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		std::string SetStr = SetArg.has_value() ? SetArg.value() : "game";
		ULocalizationTarget* Target = FindLocTarget(TargetName, SetStr);
		if (!Target)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] loc_word_count: target '%s' not found"), UTF8_TO_TCHAR(TargetName.c_str())));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		// Generate word count report config and write it
		FLocalizationConfigurationScript WordCountConfig = LocalizationConfigurationScript::GenerateWordCountReportConfigFile(Target);
		FString ConfigPath = LocalizationConfigurationScript::GetWordCountReportConfigPath(Target);

		bool bConfigWritten = WordCountConfig.WriteWithSCC(ConfigPath);
		if (!bConfigWritten)
		{
			sol::table Result = LuaView.create_table();
			Result["success"] = false;
			Result["error"] = "Failed to write word count config file";
			Session.Log(FString::Printf(TEXT("[FAIL] loc_word_count: could not write config for '%s'"), UTF8_TO_TCHAR(TargetName.c_str())));
			return sol::make_object(LuaView, Result);
		}

		// Execute the commandlet
		TSharedPtr<FLocalizationCommandletProcess> Process = FLocalizationCommandletProcess::Execute(ConfigPath, true);
		bool bSuccess = false;

		if (Process.IsValid())
		{
			while (FPlatformProcess::IsProcRunning(Process->GetHandle()))
			{
				FPlatformProcess::ReadPipe(Process->GetReadPipe());
				TickEditorWhileWaiting();
			}

			int32 ReturnCode = 0;
			FPlatformProcess::GetProcReturnCode(Process->GetHandle(), &ReturnCode);
			bSuccess = (ReturnCode == 0);
		}

		if (bSuccess)
		{
			// Update the target's word counts from the generated CSV
			Target->UpdateWordCountsFromCSV();
		}

		sol::table Result = LuaView.create_table();
		Result["success"] = bSuccess;
		Result["target"] = TargetName;
		Result["csv_path"] = std::string(TCHAR_TO_UTF8(*LocalizationConfigurationScript::GetWordCountCSVPath(Target)));

		if (bSuccess)
		{
			// Return updated word counts
			sol::table Cultures = LuaView.create_table();
			int32 CIdx = 1;
			for (const FCultureStatistics& CultureStat : Target->Settings.SupportedCulturesStatistics)
			{
				sol::table CEntry = LuaView.create_table();
				CEntry["name"] = std::string(TCHAR_TO_UTF8(*CultureStat.CultureName));
				CEntry["word_count"] = static_cast<int64_t>(CultureStat.WordCount);
				Cultures[CIdx++] = CEntry;
			}
			Result["cultures"] = Cultures;

			Session.Log(FString::Printf(TEXT("[OK] loc_word_count '%s'"), UTF8_TO_TCHAR(TargetName.c_str())));
		}
		else
		{
			Result["error"] = "Word count commandlet failed or could not be started";
			Session.Log(FString::Printf(TEXT("[FAIL] loc_word_count '%s'"), UTF8_TO_TCHAR(TargetName.c_str())));
		}

		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// loc_get_file_paths(target_name, set?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("loc_get_file_paths", [&Session](const std::string& TargetName, sol::optional<std::string> SetArg,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (TargetName.empty())
		{
			Session.Log(TEXT("[FAIL] loc_get_file_paths: target_name is required"));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		std::string SetStr = SetArg.has_value() ? SetArg.value() : "game";
		ULocalizationTarget* Target = FindLocTarget(TargetName, SetStr);
		if (!Target)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] loc_get_file_paths: target '%s' not found"), UTF8_TO_TCHAR(TargetName.c_str())));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		const FLocalizationTargetSettings& Settings = Target->Settings;

		sol::table Result = LuaView.create_table();
		Result["data_directory"] = std::string(TCHAR_TO_UTF8(*LocalizationConfigurationScript::GetDataDirectory(Target)));
		Result["config_directory"] = std::string(TCHAR_TO_UTF8(*LocalizationConfigurationScript::GetConfigDirectory(Target)));
		Result["manifest"] = std::string(TCHAR_TO_UTF8(*LocalizationConfigurationScript::GetManifestPath(Target)));
		Result["word_count_csv"] = std::string(TCHAR_TO_UTF8(*LocalizationConfigurationScript::GetWordCountCSVPath(Target)));
		Result["conflict_report"] = std::string(TCHAR_TO_UTF8(*LocalizationConfigurationScript::GetConflictReportPath(Target)));

		// Per-culture paths
		sol::table CulturePaths = LuaView.create_table();
		int32 CIdx = 1;
		for (const FCultureStatistics& CultureStat : Settings.SupportedCulturesStatistics)
		{
			sol::table CPEntry = LuaView.create_table();
			CPEntry["culture"] = std::string(TCHAR_TO_UTF8(*CultureStat.CultureName));
			CPEntry["archive"] = std::string(TCHAR_TO_UTF8(*LocalizationConfigurationScript::GetArchivePath(Target, CultureStat.CultureName)));
			CPEntry["po"] = std::string(TCHAR_TO_UTF8(*LocalizationConfigurationScript::GetDefaultPOPath(Target, CultureStat.CultureName)));
			CPEntry["locres"] = std::string(TCHAR_TO_UTF8(*LocalizationConfigurationScript::GetLocResPath(Target, CultureStat.CultureName)));
			CulturePaths[CIdx++] = CPEntry;
		}
		Result["cultures"] = CulturePaths;

		Session.Log(FString::Printf(TEXT("[OK] loc_get_file_paths '%s'"), UTF8_TO_TCHAR(TargetName.c_str())));
		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// loc_get_culture()
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("loc_get_culture", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		FCultureRef CurrentCulture = FInternationalization::Get().GetCurrentLanguage();
		std::string CultureName = std::string(TCHAR_TO_UTF8(*CurrentCulture->GetName()));

		Session.Log(FString::Printf(TEXT("[OK] loc_get_culture -> '%s'"), *CurrentCulture->GetName()));
		return sol::make_object(LuaView, CultureName);
	});

	// ════════════════════════════════════════════════════════════════════
	// loc_set_culture(culture_name)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("loc_set_culture", [&Session](const std::string& CultureName, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (CultureName.empty())
		{
			Session.Log(TEXT("[FAIL] loc_set_culture: culture_name is required"));
			return sol::make_object(LuaView, false);
		}

		FString FCultureName = UTF8_TO_TCHAR(CultureName.c_str());
		bool bSuccess = FInternationalization::Get().SetCurrentLanguageAndLocale(FCultureName);

		if (bSuccess)
		{
			Session.Log(FString::Printf(TEXT("[OK] loc_set_culture '%s'"), *FCultureName));
		}
		else
		{
			Session.Log(FString::Printf(TEXT("[FAIL] loc_set_culture '%s'"), *FCultureName));
		}

		return sol::make_object(LuaView, bSuccess);
	});

	// ════════════════════════════════════════════════════════════════════
	// loc_list_cultures()
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("loc_list_cultures", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		TArray<FString> CultureNames;
		FInternationalization::Get().GetCultureNames(CultureNames);

		TArray<FCultureRef> Cultures = FInternationalization::Get().GetAvailableCultures(CultureNames, false);

		sol::table Result = LuaView.create_table();
		int32 Idx = 1;
		for (const FCultureRef& Culture : Cultures)
		{
			sol::table Entry = LuaView.create_table();
			Entry["name"] = std::string(TCHAR_TO_UTF8(*Culture->GetName()));
			Entry["display_name"] = std::string(TCHAR_TO_UTF8(*Culture->GetDisplayName()));
			Entry["native_language"] = std::string(TCHAR_TO_UTF8(*Culture->GetNativeLanguage()));
			Entry["native_region"] = std::string(TCHAR_TO_UTF8(*Culture->GetNativeRegion()));
			Result[Idx++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] loc_list_cultures -> %d cultures"), Idx - 1));
		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// stringtable_list()
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("stringtable_list", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		sol::table Result = LuaView.create_table();
		int32 Idx = 1;

		FStringTableRegistry::Get().EnumerateStringTables([&](const FName& TableId, const FStringTableConstRef& Table) -> bool
		{
			sol::table Entry = LuaView.create_table();
			Entry["table_id"] = std::string(TCHAR_TO_UTF8(*TableId.ToString()));
			Entry["namespace"] = std::string(TCHAR_TO_UTF8(*Table->GetNamespace()));

			int32 EntryCount = 0;
			Table->EnumerateSourceStrings([&EntryCount](const FString& Key, const FString& Value) -> bool
			{
				++EntryCount;
				return true;
			});
			Entry["entry_count"] = EntryCount;

			Result[Idx++] = Entry;
			return true; // continue enumeration
		});

		Session.Log(FString::Printf(TEXT("[OK] stringtable_list -> %d tables"), Idx - 1));
		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// stringtable_create(table_id, namespace)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("stringtable_create", [&Session](const std::string& TableIdStr, const std::string& Namespace,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (TableIdStr.empty() || Namespace.empty())
		{
			Session.Log(TEXT("[FAIL] stringtable_create: table_id and namespace are required"));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		FName TableId = FName(UTF8_TO_TCHAR(TableIdStr.c_str()));

		// Check if already exists
		FStringTablePtr Existing = FStringTableRegistry::Get().FindMutableStringTable(TableId);
		if (Existing.IsValid())
		{
			sol::table Result = LuaView.create_table();
			Result["success"] = false;
			Result["error"] = "String table already exists with that ID";
			Session.Log(FString::Printf(TEXT("[FAIL] stringtable_create: table '%s' already exists"),
				UTF8_TO_TCHAR(TableIdStr.c_str())));
			return sol::make_object(LuaView, Result);
		}

		Loc_NewLocTable(TableId, FString(UTF8_TO_TCHAR(Namespace.c_str())));

		sol::table Result = LuaView.create_table();
		Result["success"] = true;
		Result["table_id"] = TableIdStr;
		Result["namespace"] = Namespace;

		Session.Log(FString::Printf(TEXT("[OK] stringtable_create '%s' namespace='%s'"),
			UTF8_TO_TCHAR(TableIdStr.c_str()), UTF8_TO_TCHAR(Namespace.c_str())));
		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// stringtable_get(table_id_or_path)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("stringtable_get", [&Session](const std::string& TableIdOrPath, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (TableIdOrPath.empty())
		{
			Session.Log(TEXT("[FAIL] stringtable_get: table_id_or_path is required"));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		FStringTableLookupResult Lookup = FindOrLoadStringTable(TableIdOrPath);
		if (!Lookup.Table.IsValid())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] stringtable_get: table '%s' not found"), UTF8_TO_TCHAR(TableIdOrPath.c_str())));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		sol::table Result = LuaView.create_table();
		Result["table_id"] = std::string(TCHAR_TO_UTF8(*Lookup.TableId.ToString()));
		Result["namespace"] = std::string(TCHAR_TO_UTF8(*Lookup.Table->GetNamespace()));

		sol::table Entries = LuaView.create_table();
		int32 Idx = 1;
		Lookup.Table->EnumerateSourceStrings([&](const FString& Key, const FString& Value) -> bool
		{
			sol::table Entry = LuaView.create_table();
			Entry["key"] = std::string(TCHAR_TO_UTF8(*Key));
			Entry["value"] = std::string(TCHAR_TO_UTF8(*Value));
			Entries[Idx++] = Entry;
			return true;
		});
		Result["entries"] = Entries;

		Session.Log(FString::Printf(TEXT("[OK] stringtable_get '%s' -> %d entries"),
			UTF8_TO_TCHAR(TableIdOrPath.c_str()), Idx - 1));
		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// stringtable_set_entry(table_id_or_path, key, value)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("stringtable_set_entry", [&Session](const std::string& TableIdOrPath, const std::string& Key,
		const std::string& Value, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (TableIdOrPath.empty() || Key.empty())
		{
			Session.Log(TEXT("[FAIL] stringtable_set_entry: table_id_or_path and key are required"));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		FStringTableLookupResult Lookup = FindOrLoadStringTable(TableIdOrPath);
		if (!Lookup.Table.IsValid())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] stringtable_set_entry: table '%s' not found"), UTF8_TO_TCHAR(TableIdOrPath.c_str())));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		FString FKey = UTF8_TO_TCHAR(Key.c_str());
		FString FValue = UTF8_TO_TCHAR(Value.c_str());
		Loc_SetSourceString(Lookup.Table, FKey, FValue);

		// Mark the owning asset as dirty if it exists
		UStringTable* OwnerAsset = Lookup.Table->GetOwnerAsset();
		if (OwnerAsset)
		{
			OwnerAsset->MarkPackageDirty();
		}

		sol::table Result = LuaView.create_table();
		Result["success"] = true;
		Result["key"] = Key;
		Result["value"] = Value;

		Session.Log(FString::Printf(TEXT("[OK] stringtable_set_entry '%s' key='%s'"),
			UTF8_TO_TCHAR(TableIdOrPath.c_str()), UTF8_TO_TCHAR(Key.c_str())));
		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// stringtable_remove_entry(table_id_or_path, key)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("stringtable_remove_entry", [&Session](const std::string& TableIdOrPath, const std::string& Key,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (TableIdOrPath.empty() || Key.empty())
		{
			Session.Log(TEXT("[FAIL] stringtable_remove_entry: table_id_or_path and key are required"));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		FStringTableLookupResult Lookup = FindOrLoadStringTable(TableIdOrPath);
		if (!Lookup.Table.IsValid())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] stringtable_remove_entry: table '%s' not found"), UTF8_TO_TCHAR(TableIdOrPath.c_str())));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		FString FKey = UTF8_TO_TCHAR(Key.c_str());

		// Check if key exists first
		FString ExistingValue;
		bool bExists = Loc_GetSourceString(Lookup.Table, FKey, ExistingValue);

		if (!bExists)
		{
			sol::table Result = LuaView.create_table();
			Result["success"] = false;
			Result["error"] = "Key not found in string table";
			Session.Log(FString::Printf(TEXT("[FAIL] stringtable_remove_entry: key '%s' not found in '%s'"),
				UTF8_TO_TCHAR(Key.c_str()), UTF8_TO_TCHAR(TableIdOrPath.c_str())));
			return sol::make_object(LuaView, Result);
		}

		Loc_RemoveSourceString(Lookup.Table, FKey);

		// Mark dirty
		UStringTable* OwnerAsset = Lookup.Table->GetOwnerAsset();
		if (OwnerAsset)
		{
			OwnerAsset->MarkPackageDirty();
		}

		sol::table Result = LuaView.create_table();
		Result["success"] = true;
		Result["key"] = Key;

		Session.Log(FString::Printf(TEXT("[OK] stringtable_remove_entry '%s' key='%s'"),
			UTF8_TO_TCHAR(TableIdOrPath.c_str()), UTF8_TO_TCHAR(Key.c_str())));
		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// stringtable_clear(table_id_or_path)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("stringtable_clear", [&Session](const std::string& TableIdOrPath, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (TableIdOrPath.empty())
		{
			Session.Log(TEXT("[FAIL] stringtable_clear: table_id_or_path is required"));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		FStringTableLookupResult Lookup = FindOrLoadStringTable(TableIdOrPath);
		if (!Lookup.Table.IsValid())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] stringtable_clear: table '%s' not found"), UTF8_TO_TCHAR(TableIdOrPath.c_str())));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		Lookup.Table->ClearSourceStrings();
		Lookup.Table->ClearMetaData();

		UStringTable* OwnerAsset = Lookup.Table->GetOwnerAsset();
		if (OwnerAsset)
		{
			OwnerAsset->MarkPackageDirty();
		}

		sol::table Result = LuaView.create_table();
		Result["success"] = true;

		Session.Log(FString::Printf(TEXT("[OK] stringtable_clear '%s'"), UTF8_TO_TCHAR(TableIdOrPath.c_str())));
		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// stringtable_set_namespace(table_id_or_path, namespace)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("stringtable_set_namespace", [&Session](const std::string& TableIdOrPath, const std::string& Namespace,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (TableIdOrPath.empty() || Namespace.empty())
		{
			Session.Log(TEXT("[FAIL] stringtable_set_namespace: table_id_or_path and namespace are required"));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		FStringTableLookupResult Lookup = FindOrLoadStringTable(TableIdOrPath);
		if (!Lookup.Table.IsValid())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] stringtable_set_namespace: table '%s' not found"), UTF8_TO_TCHAR(TableIdOrPath.c_str())));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		Loc_SetNamespace(Lookup.Table, FString(UTF8_TO_TCHAR(Namespace.c_str())));

		UStringTable* OwnerAsset = Lookup.Table->GetOwnerAsset();
		if (OwnerAsset)
		{
			OwnerAsset->MarkPackageDirty();
		}

		sol::table Result = LuaView.create_table();
		Result["success"] = true;
		Result["namespace"] = Namespace;

		Session.Log(FString::Printf(TEXT("[OK] stringtable_set_namespace '%s' -> '%s'"),
			UTF8_TO_TCHAR(TableIdOrPath.c_str()), UTF8_TO_TCHAR(Namespace.c_str())));
		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// stringtable_set_metadata(table_id_or_path, key, meta_id, value)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("stringtable_set_metadata", [&Session](const std::string& TableIdOrPath, const std::string& Key,
		const std::string& MetaId, const std::string& Value, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (TableIdOrPath.empty() || Key.empty() || MetaId.empty())
		{
			Session.Log(TEXT("[FAIL] stringtable_set_metadata: table_id_or_path, key, and meta_id are required"));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		FStringTableLookupResult Lookup = FindOrLoadStringTable(TableIdOrPath);
		if (!Lookup.Table.IsValid())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] stringtable_set_metadata: table '%s' not found"), UTF8_TO_TCHAR(TableIdOrPath.c_str())));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		FString FKey(UTF8_TO_TCHAR(Key.c_str()));

		// Verify key exists
		FString ExistingValue;
		if (!Loc_GetSourceString(Lookup.Table, FKey, ExistingValue))
		{
			sol::table Result = LuaView.create_table();
			Result["success"] = false;
			Result["error"] = "Key not found in string table";
			Session.Log(FString::Printf(TEXT("[FAIL] stringtable_set_metadata: key '%s' not found in '%s'"),
				UTF8_TO_TCHAR(Key.c_str()), UTF8_TO_TCHAR(TableIdOrPath.c_str())));
			return sol::make_object(LuaView, Result);
		}

		FName FMetaId = FName(UTF8_TO_TCHAR(MetaId.c_str()));
		FString FValue = UTF8_TO_TCHAR(Value.c_str());
		Loc_SetMetaData(Lookup.Table, FKey, FMetaId, FValue);

		UStringTable* OwnerAsset = Lookup.Table->GetOwnerAsset();
		if (OwnerAsset)
		{
			OwnerAsset->MarkPackageDirty();
		}

		sol::table Result = LuaView.create_table();
		Result["success"] = true;
		Result["key"] = Key;
		Result["meta_id"] = MetaId;
		Result["value"] = Value;

		Session.Log(FString::Printf(TEXT("[OK] stringtable_set_metadata '%s' key='%s' meta='%s'"),
			UTF8_TO_TCHAR(TableIdOrPath.c_str()), UTF8_TO_TCHAR(Key.c_str()), UTF8_TO_TCHAR(MetaId.c_str())));
		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// stringtable_get_metadata(table_id_or_path, key, meta_id?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("stringtable_get_metadata", [&Session](const std::string& TableIdOrPath, const std::string& Key,
		sol::optional<std::string> MetaIdArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (TableIdOrPath.empty() || Key.empty())
		{
			Session.Log(TEXT("[FAIL] stringtable_get_metadata: table_id_or_path and key are required"));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		FStringTableLookupResult Lookup = FindOrLoadStringTable(TableIdOrPath);
		if (!Lookup.Table.IsValid())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] stringtable_get_metadata: table '%s' not found"), UTF8_TO_TCHAR(TableIdOrPath.c_str())));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		FString FKey(UTF8_TO_TCHAR(Key.c_str()));

		// Verify key exists
		FString ExistingValue;
		if (!Loc_GetSourceString(Lookup.Table, FKey, ExistingValue))
		{
			sol::table Result = LuaView.create_table();
			Result["success"] = false;
			Result["error"] = "Key not found in string table";
			Session.Log(FString::Printf(TEXT("[FAIL] stringtable_get_metadata: key '%s' not found in '%s'"),
				UTF8_TO_TCHAR(Key.c_str()), UTF8_TO_TCHAR(TableIdOrPath.c_str())));
			return sol::make_object(LuaView, Result);
		}

		if (MetaIdArg.has_value() && !MetaIdArg.value().empty())
		{
			// Get single meta-data value
			FName FMetaId = FName(UTF8_TO_TCHAR(MetaIdArg.value().c_str()));
			FString MetaValue = Loc_GetMetaData(Lookup.Table, FKey, FMetaId);

			sol::table Result = LuaView.create_table();
			Result["success"] = true;
			Result["key"] = Key;
			Result["meta_id"] = MetaIdArg.value();
			Result["value"] = std::string(TCHAR_TO_UTF8(*MetaValue));

			Session.Log(FString::Printf(TEXT("[OK] stringtable_get_metadata '%s' key='%s' meta='%s'"),
				UTF8_TO_TCHAR(TableIdOrPath.c_str()), UTF8_TO_TCHAR(Key.c_str()), UTF8_TO_TCHAR(MetaIdArg.value().c_str())));
			return sol::make_object(LuaView, Result);
		}
		else
		{
			// Enumerate all meta-data for the key
			sol::table Result = LuaView.create_table();
			Result["success"] = true;
			Result["key"] = Key;

			sol::table MetaEntries = LuaView.create_table();
			int32 MIdx = 1;
			Loc_EnumerateMetaData(Lookup.Table, FKey, [&](FName MetaId, const FString& MetaValue) -> bool
			{
				sol::table MEntry = LuaView.create_table();
				MEntry["meta_id"] = std::string(TCHAR_TO_UTF8(*MetaId.ToString()));
				MEntry["value"] = std::string(TCHAR_TO_UTF8(*MetaValue));
				MetaEntries[MIdx++] = MEntry;
				return true;
			});
			Result["metadata"] = MetaEntries;

			Session.Log(FString::Printf(TEXT("[OK] stringtable_get_metadata '%s' key='%s' -> %d entries"),
				UTF8_TO_TCHAR(TableIdOrPath.c_str()), UTF8_TO_TCHAR(Key.c_str()), MIdx - 1));
			return sol::make_object(LuaView, Result);
		}
	});

	// ════════════════════════════════════════════════════════════════════
	// stringtable_remove_metadata(table_id_or_path, key, meta_id)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("stringtable_remove_metadata", [&Session](const std::string& TableIdOrPath, const std::string& Key,
		const std::string& MetaId, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (TableIdOrPath.empty() || Key.empty() || MetaId.empty())
		{
			Session.Log(TEXT("[FAIL] stringtable_remove_metadata: table_id_or_path, key, and meta_id are required"));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		FStringTableLookupResult Lookup = FindOrLoadStringTable(TableIdOrPath);
		if (!Lookup.Table.IsValid())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] stringtable_remove_metadata: table '%s' not found"), UTF8_TO_TCHAR(TableIdOrPath.c_str())));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		FString FKey(UTF8_TO_TCHAR(Key.c_str()));
		FName FMetaId = FName(UTF8_TO_TCHAR(MetaId.c_str()));

		Loc_RemoveMetaData(Lookup.Table, FKey, FMetaId);

		UStringTable* OwnerAsset = Lookup.Table->GetOwnerAsset();
		if (OwnerAsset)
		{
			OwnerAsset->MarkPackageDirty();
		}

		sol::table Result = LuaView.create_table();
		Result["success"] = true;
		Result["key"] = Key;
		Result["meta_id"] = MetaId;

		Session.Log(FString::Printf(TEXT("[OK] stringtable_remove_metadata '%s' key='%s' meta='%s'"),
			UTF8_TO_TCHAR(TableIdOrPath.c_str()), UTF8_TO_TCHAR(Key.c_str()), UTF8_TO_TCHAR(MetaId.c_str())));
		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// stringtable_export_csv(table_id_or_path, output_path)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("stringtable_export_csv", [&Session](const std::string& TableIdOrPath, const std::string& OutputPath,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (TableIdOrPath.empty() || OutputPath.empty())
		{
			Session.Log(TEXT("[FAIL] stringtable_export_csv: table_id_or_path and output_path are required"));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		FStringTableLookupResult Lookup = FindOrLoadStringTable(TableIdOrPath);
		if (!Lookup.Table.IsValid())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] stringtable_export_csv: table '%s' not found"), UTF8_TO_TCHAR(TableIdOrPath.c_str())));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		FString FOutputPath = UTF8_TO_TCHAR(OutputPath.c_str());

		// Ensure the output directory exists
		FString OutputDir = FPaths::GetPath(FOutputPath);
		if (!OutputDir.IsEmpty())
		{
			IFileManager::Get().MakeDirectory(*OutputDir, true);
		}

		bool bSuccess = Lookup.Table->ExportStrings(FOutputPath);

		sol::table Result = LuaView.create_table();
		Result["success"] = bSuccess;
		Result["output_path"] = OutputPath;

		if (bSuccess)
		{
			Session.Log(FString::Printf(TEXT("[OK] stringtable_export_csv '%s' -> '%s'"),
				UTF8_TO_TCHAR(TableIdOrPath.c_str()), *FOutputPath));
		}
		else
		{
			Result["error"] = "CSV export failed";
			Session.Log(FString::Printf(TEXT("[FAIL] stringtable_export_csv '%s' -> '%s'"),
				UTF8_TO_TCHAR(TableIdOrPath.c_str()), *FOutputPath));
		}

		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// stringtable_import_csv(table_id_or_path, csv_path)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("stringtable_import_csv", [&Session](const std::string& TableIdOrPath, const std::string& CsvPath,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (TableIdOrPath.empty() || CsvPath.empty())
		{
			Session.Log(TEXT("[FAIL] stringtable_import_csv: table_id_or_path and csv_path are required"));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		FString FCsvPath = UTF8_TO_TCHAR(CsvPath.c_str());

		// Check that the CSV file exists
		if (!FPaths::FileExists(FCsvPath))
		{
			sol::table Result = LuaView.create_table();
			Result["success"] = false;
			Result["error"] = std::string("CSV file not found: ") + CsvPath;
			Session.Log(FString::Printf(TEXT("[FAIL] stringtable_import_csv: file not found '%s'"), *FCsvPath));
			return sol::make_object(LuaView, Result);
		}

		FStringTableLookupResult Lookup = FindOrLoadStringTable(TableIdOrPath);
		if (!Lookup.Table.IsValid())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] stringtable_import_csv: table '%s' not found"), UTF8_TO_TCHAR(TableIdOrPath.c_str())));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		bool bSuccess = Lookup.Table->ImportStrings(FCsvPath);

		if (bSuccess)
		{
			// Mark dirty
			UStringTable* OwnerAsset = Lookup.Table->GetOwnerAsset();
			if (OwnerAsset)
			{
				OwnerAsset->MarkPackageDirty();
			}
		}

		sol::table Result = LuaView.create_table();
		Result["success"] = bSuccess;
		Result["csv_path"] = CsvPath;

		if (bSuccess)
		{
			Session.Log(FString::Printf(TEXT("[OK] stringtable_import_csv '%s' <- '%s'"),
				UTF8_TO_TCHAR(TableIdOrPath.c_str()), *FCsvPath));
		}
		else
		{
			Result["error"] = "CSV import failed";
			Session.Log(FString::Printf(TEXT("[FAIL] stringtable_import_csv '%s' <- '%s'"),
				UTF8_TO_TCHAR(TableIdOrPath.c_str()), *FCsvPath));
		}

		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// loc_preview(culture_name?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("loc_preview", [&Session](sol::optional<std::string> CultureArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (!CultureArg.has_value() || CultureArg.value().empty())
		{
			// Disable preview
			FTextLocalizationManager::Get().DisableGameLocalizationPreview();

			sol::table Result = LuaView.create_table();
			Result["success"] = true;
			Result["preview"] = false;

			Session.Log(TEXT("[OK] loc_preview disabled"));
			return sol::make_object(LuaView, Result);
		}

		FString CultureName = UTF8_TO_TCHAR(CultureArg.value().c_str());
		FTextLocalizationManager::Get().EnableGameLocalizationPreview(CultureName);

		sol::table Result = LuaView.create_table();
		Result["success"] = true;
		Result["preview"] = true;
		Result["culture"] = CultureArg.value();

		Session.Log(FString::Printf(TEXT("[OK] loc_preview enabled for '%s'"), *CultureName));
		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// loc_refresh()
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("loc_refresh", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		FTextLocalizationManager::Get().RefreshResources();

		sol::table Result = LuaView.create_table();
		Result["success"] = true;

		Session.Log(TEXT("[OK] loc_refresh"));
		return sol::make_object(LuaView, Result);
	});
});
