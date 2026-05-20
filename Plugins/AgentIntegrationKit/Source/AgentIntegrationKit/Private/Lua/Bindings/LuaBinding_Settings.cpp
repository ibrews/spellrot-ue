// Copyright 2025-2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include <sol/sol.hpp>
#include "Tools/NeoStackToolUtils.h"

// Settings system
#include "ISettingsModule.h"
#include "ISettingsContainer.h"
#include "ISettingsCategory.h"
#include "ISettingsSection.h"
#include "Engine/DeveloperSettings.h"
#include "UObject/UnrealType.h"

// Console variables & commands
#include "HAL/IConsoleManager.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
#include "Misc/StringOutputDevice.h"
#endif
#include "Misc/ConfigCacheIni.h"

// Plugin management
#include "Interfaces/IPluginManager.h"
#include "Interfaces/IProjectManager.h"
#include "PluginDescriptor.h"

// ─── Documentation ───

static TArray<FLuaFunctionDoc> SettingsDocs = {
	{ TEXT("list_settings(container?)"), TEXT("List settings categories and sections — container: project (default), editor"), TEXT("table") },
	{ TEXT("get_setting(container, category, section)"), TEXT("Get a settings handle with get/set/list_properties/save/reset/export_settings/import_settings"), TEXT("settings handle") },
	{ TEXT("search_settings(query, container?)"), TEXT("Search settings by query string — container: project, editor, all (default)"), TEXT("table") },
	{ TEXT("find_plugin_settings(plugin_name?)"), TEXT("Discover UDeveloperSettings subclasses registered by plugins"), TEXT("table") },
	{ TEXT("list_plugins(filter?)"), TEXT("List plugins — filter: all, enabled, disabled, project, engine"), TEXT("table") },
	{ TEXT("get_plugin(name)"), TEXT("Get detailed info about a specific plugin"), TEXT("table or nil") },
	{ TEXT("set_plugin_enabled(name, enabled)"), TEXT("Enable or disable a plugin in the .uproject file (requires restart)"), TEXT("true or nil") },
	{ TEXT("get_cvar(name)"), TEXT("Get a console variable value and metadata"), TEXT("table or nil") },
	{ TEXT("set_cvar(name, value)"), TEXT("Set a console variable value"), TEXT("true or nil") },
	{ TEXT("find_cvars(query, opts?)"), TEXT("Search console variables — opts: {mode='contains'|'prefix', limit=50}"), TEXT("table") },
	{ TEXT("exec_command(cmd)"), TEXT("Execute an arbitrary console command and capture output"), TEXT("{success, output}") },
	{ TEXT("read_config(file, section, key?)"), TEXT("Read from a .ini config file — omit key to get all keys in section"), TEXT("string or table") },
	{ TEXT("write_config(file, section, key, value)"), TEXT("Write a value to a .ini config file and flush to disk"), TEXT("{success, file}") },
	{ TEXT("remove_config(file, section, key?)"), TEXT("Remove a key or entire section from a .ini config file"), TEXT("{success}") },
	{ TEXT("list_config_sections(file)"), TEXT("List all sections in a .ini config file"), TEXT("string[]") },
};

// ─── Helpers ───

static FString ResolveConfigFilename(const FString& FriendlyName)
{
	// Map friendly names to the global config filename strings
	if (FriendlyName.Equals(TEXT("Engine"), ESearchCase::IgnoreCase)) return GEngineIni;
	if (FriendlyName.Equals(TEXT("Game"), ESearchCase::IgnoreCase)) return GGameIni;
	if (FriendlyName.Equals(TEXT("Input"), ESearchCase::IgnoreCase)) return GInputIni;
	if (FriendlyName.Equals(TEXT("Editor"), ESearchCase::IgnoreCase)) return GEditorIni;
	if (FriendlyName.Equals(TEXT("EditorSettings"), ESearchCase::IgnoreCase)) return GEditorSettingsIni;
	if (FriendlyName.Equals(TEXT("EditorLayout"), ESearchCase::IgnoreCase)) return GEditorLayoutIni;
	if (FriendlyName.Equals(TEXT("EditorKeyBindings"), ESearchCase::IgnoreCase)) return GEditorKeyBindingsIni;
	if (FriendlyName.Equals(TEXT("GameUserSettings"), ESearchCase::IgnoreCase)) return GGameUserSettingsIni;
	if (FriendlyName.Equals(TEXT("Scalability"), ESearchCase::IgnoreCase)) return GScalabilityIni;
	if (FriendlyName.Equals(TEXT("Hardware"), ESearchCase::IgnoreCase)) return GHardwareIni;
	if (FriendlyName.Equals(TEXT("DeviceProfiles"), ESearchCase::IgnoreCase)) return GDeviceProfilesIni;
	if (FriendlyName.Equals(TEXT("GameplayTags"), ESearchCase::IgnoreCase)) return GGameplayTagsIni;
	// If not a known name, treat as-is (could be a full path)
	return FriendlyName;
}

static ISettingsModule* GetSettingsModule()
{
	return FModuleManager::GetModulePtr<ISettingsModule>("Settings");
}

static TSharedPtr<ISettingsContainer> GetContainer(const FString& ContainerName)
{
	ISettingsModule* Module = GetSettingsModule();
	if (!Module) return nullptr;

	if (ContainerName.Equals(TEXT("project"), ESearchCase::IgnoreCase))
		return Module->GetContainer("Project");
	if (ContainerName.Equals(TEXT("editor"), ESearchCase::IgnoreCase))
		return Module->GetContainer("Editor");
	return nullptr;
}

static TSharedPtr<ISettingsSection> FindSection(
	TSharedPtr<ISettingsContainer> Container,
	const FString& CategoryQuery,
	const FString& SectionQuery)
{
	if (!Container.IsValid()) return nullptr;

	TArray<TSharedPtr<ISettingsCategory>> Categories;
	Container->GetCategories(Categories);

	for (const auto& Category : Categories)
	{
		FString CatName = Category->GetName().ToString();
		FText CatDisplay = Category->GetDisplayName();

		bool bCatMatch = CatName.Contains(CategoryQuery, ESearchCase::IgnoreCase)
			|| CatDisplay.ToString().Contains(CategoryQuery, ESearchCase::IgnoreCase);
		if (!bCatMatch) continue;

		TArray<TSharedPtr<ISettingsSection>> Sections;
		Category->GetSections(Sections);

		for (const auto& Section : Sections)
		{
			FString SecName = Section->GetName().ToString();
			FText SecDisplay = Section->GetDisplayName();

			bool bSecMatch = SecName.Contains(SectionQuery, ESearchCase::IgnoreCase)
				|| SecDisplay.ToString().Contains(SectionQuery, ESearchCase::IgnoreCase);
			if (bSecMatch) return Section;
		}
	}
	return nullptr;
}

// Build a settings handle table for a UObject (shared by settings and plugin settings)
static sol::table MakeSettingsHandle(sol::state_view& Lua, UObject* Obj, TSharedPtr<ISettingsSection> Section, FLuaSessionData& Session)
{
	sol::table Handle = Lua.create_table();
	TWeakObjectPtr<UObject> WeakObj(Obj);
	Handle["_class"] = std::string(TCHAR_TO_UTF8(*Obj->GetClass()->GetName()));

	// get(property_name)
	Handle.set_function("get", [WeakObj](sol::table, const std::string& PropName, sol::this_state S) -> sol::object
	{
		sol::state_view L(S);
		UObject* Obj = WeakObj.Get();
		if (!Obj) return sol::lua_nil;

		FString FPropName = UTF8_TO_TCHAR(PropName.c_str());

		// Walk property chain for dot-separated paths
		FProperty* Prop = Obj->GetClass()->FindPropertyByName(*FPropName);
		if (!Prop)
		{
			// Try case-insensitive search
			for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
			{
				if (It->GetName().Equals(FPropName, ESearchCase::IgnoreCase))
				{
					Prop = *It;
					break;
				}
			}
		}
		if (!Prop) return sol::lua_nil;

		FString Value;
		Prop->ExportTextItem_InContainer(Value, Obj, nullptr, nullptr, PPF_None);
		return sol::make_object(L, std::string(TCHAR_TO_UTF8(*Value)));
	});

	// set(property_name, value) -> bool
	Handle.set_function("set", [WeakObj, &Session](sol::table, const std::string& PropName, const std::string& Value, sol::this_state S) -> sol::object
	{
		sol::state_view L(S);
		UObject* Obj = WeakObj.Get();
		if (!Obj)
		{
			Session.Log(TEXT("[FAIL] set -> settings object no longer valid"));
			return sol::lua_nil;
		}

		FString FPropName = UTF8_TO_TCHAR(PropName.c_str());
		FString FValue = UTF8_TO_TCHAR(Value.c_str());

		FProperty* Prop = nullptr;
		for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
		{
			if (It->GetName().Equals(FPropName, ESearchCase::IgnoreCase))
			{
				Prop = *It;
				break;
			}
		}

		if (!Prop)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] set -> property '%s' not found on %s"), *FPropName, *Obj->GetClass()->GetName()));
			return sol::lua_nil;
		}

		Obj->Modify();
		Obj->PreEditChange(Prop);
		Prop->ImportText_InContainer(*FValue, Obj, nullptr, PPF_None);
		FPropertyChangedEvent ChangeEvent(Prop);
		Obj->PostEditChangeProperty(ChangeEvent);
		Session.Log(FString::Printf(TEXT("[OK] set %s = %s"), *FPropName, *FValue));
		return sol::make_object(L, true);
	});

	// list_properties(filter?)
	Handle.set_function("list_properties", [WeakObj](sol::table, sol::optional<std::string> Filter, sol::this_state S) -> sol::object
	{
		sol::state_view L(S);
		UObject* Obj = WeakObj.Get();
		if (!Obj) return sol::lua_nil;

		sol::table Result = L.create_table();
		int32 Idx = 1;

		FString FilterStr;
		if (Filter.has_value())
			FilterStr = UTF8_TO_TCHAR(Filter.value().c_str());

		for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated)) continue;

			FString Name = It->GetName();
			if (!FilterStr.IsEmpty() && !Name.Contains(FilterStr, ESearchCase::IgnoreCase)) continue;

			FString Value;
			It->ExportTextItem_InContainer(Value, Obj, nullptr, nullptr, PPF_None);

			FString Category;
			if (It->HasMetaData(TEXT("Category")))
				Category = It->GetMetaData(TEXT("Category"));

			sol::table Entry = L.create_table();
			Entry["name"] = std::string(TCHAR_TO_UTF8(*Name));
			Entry["type"] = std::string(TCHAR_TO_UTF8(*It->GetCPPType()));
			Entry["value"] = std::string(TCHAR_TO_UTF8(*Value));
			Entry["category"] = std::string(TCHAR_TO_UTF8(*Category));
			Entry["is_config"] = It->HasAnyPropertyFlags(CPF_Config);
			Result[Idx++] = Entry;
		}
		return sol::make_object(L, Result);
	});

	// save()
	if (Section.IsValid())
	{
		TWeakPtr<ISettingsSection> WeakSection = Section;
		Handle.set_function("save", [WeakObj, WeakSection, &Session](sol::table)
		{
			UObject* Obj = WeakObj.Get();
			if (auto Sec = WeakSection.Pin())
			{
				if (Sec->Save())
				{
					Session.Log(TEXT("[OK] settings saved"));
				}
				else if (Obj)
				{
					Obj->SaveConfig();
					Session.Log(TEXT("[OK] settings saved (via SaveConfig)"));
				}
				else
				{
					Session.Log(TEXT("[FAIL] save -> settings object no longer valid"));
				}
			}
			else if (Obj)
			{
				Obj->SaveConfig();
				Session.Log(TEXT("[OK] settings saved (via SaveConfig)"));
			}
			else
			{
				Session.Log(TEXT("[FAIL] save -> settings object no longer valid"));
			}
		});

		// reset()
		Handle.set_function("reset", [WeakSection, &Session](sol::table)
		{
			if (auto Sec = WeakSection.Pin())
			{
				Sec->ResetDefaults();
				Session.Log(TEXT("[OK] settings reset to defaults"));
			}
		});

		// export_settings(filepath) -> bool
		Handle.set_function("export_settings", [WeakSection, &Session](sol::table, const std::string& FilePath, sol::this_state S) -> sol::object
		{
			sol::state_view L(S);
			auto Sec = WeakSection.Pin();
			if (!Sec.IsValid())
			{
				Session.Log(TEXT("[FAIL] export_settings -> section no longer valid"));
				return sol::lua_nil;
			}
			if (!Sec->CanExport())
			{
				Session.Log(TEXT("[FAIL] export_settings -> section does not support export"));
				return sol::lua_nil;
			}
			FString FPath = UTF8_TO_TCHAR(FilePath.c_str());
			if (Sec->Export(FPath))
			{
				Session.Log(FString::Printf(TEXT("[OK] export_settings -> exported to '%s'"), *FPath));
				return sol::make_object(L, true);
			}
			Session.Log(FString::Printf(TEXT("[FAIL] export_settings -> failed to export to '%s'"), *FPath));
			return sol::lua_nil;
		});

		// import_settings(filepath) -> bool
		Handle.set_function("import_settings", [WeakSection, &Session](sol::table, const std::string& FilePath, sol::this_state S) -> sol::object
		{
			sol::state_view L(S);
			auto Sec = WeakSection.Pin();
			if (!Sec.IsValid())
			{
				Session.Log(TEXT("[FAIL] import_settings -> section no longer valid"));
				return sol::lua_nil;
			}
			if (!Sec->CanImport())
			{
				Session.Log(TEXT("[FAIL] import_settings -> section does not support import"));
				return sol::lua_nil;
			}
			FString FPath = UTF8_TO_TCHAR(FilePath.c_str());
			if (Sec->Import(FPath))
			{
				Session.Log(FString::Printf(TEXT("[OK] import_settings -> imported from '%s'"), *FPath));
				return sol::make_object(L, true);
			}
			Session.Log(FString::Printf(TEXT("[FAIL] import_settings -> failed to import from '%s'"), *FPath));
			return sol::lua_nil;
		});
	}
	else
	{
		Handle.set_function("save", [WeakObj, &Session](sol::table)
		{
			UObject* Obj = WeakObj.Get();
			if (!Obj)
			{
				Session.Log(TEXT("[FAIL] save -> settings object no longer valid"));
				return;
			}
			Obj->SaveConfig();
			Session.Log(TEXT("[OK] settings saved (via SaveConfig)"));
		});
	}

	return Handle;
}

// ─── Binding ───

static void BindSettings(sol::state& Lua, FLuaSessionData& Session)
{
	// ---- list_settings(container?) ----
	Lua.set_function("list_settings", [&Session](sol::optional<std::string> ContainerOpt, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString ContainerName = ContainerOpt.has_value()
			? UTF8_TO_TCHAR(ContainerOpt.value().c_str())
			: TEXT("project");

		auto Container = GetContainer(ContainerName);
		if (!Container.IsValid())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] list_settings -> container '%s' not found (use \"project\" or \"editor\")"), *ContainerName));
			return sol::lua_nil;
		}

		TArray<TSharedPtr<ISettingsCategory>> Categories;
		Container->GetCategories(Categories);

		sol::table Result = Lua.create_table();
		int32 CatIdx = 1;

		for (const auto& Category : Categories)
		{
			sol::table CatTable = Lua.create_table();
			CatTable["category"] = std::string(TCHAR_TO_UTF8(*Category->GetName().ToString()));
			CatTable["display_name"] = std::string(TCHAR_TO_UTF8(*Category->GetDisplayName().ToString()));

			TArray<TSharedPtr<ISettingsSection>> Sections;
			Category->GetSections(Sections);

			sol::table SecArray = Lua.create_table();
			int32 SecIdx = 1;

			for (const auto& Section : Sections)
			{
				sol::table SecTable = Lua.create_table();
				SecTable["name"] = std::string(TCHAR_TO_UTF8(*Section->GetName().ToString()));
				SecTable["display_name"] = std::string(TCHAR_TO_UTF8(*Section->GetDisplayName().ToString()));
				SecTable["description"] = std::string(TCHAR_TO_UTF8(*Section->GetDescription().ToString()));
				SecTable["status"] = std::string(TCHAR_TO_UTF8(*Section->GetStatus().ToString()));

				TWeakObjectPtr<UObject> SettingsObj = Section->GetSettingsObject();
				if (SettingsObj.IsValid())
				{
					SecTable["class"] = std::string(TCHAR_TO_UTF8(*SettingsObj->GetClass()->GetName()));
					int32 PropCount = 0;
					for (TFieldIterator<FProperty> It(SettingsObj->GetClass()); It; ++It)
					{
						if (!It->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
							PropCount++;
					}
					SecTable["property_count"] = PropCount;
				}

				SecArray[SecIdx++] = SecTable;
			}

			CatTable["sections"] = SecArray;
			Result[CatIdx++] = CatTable;
		}

		Session.Log(FString::Printf(TEXT("[OK] list_settings(\"%s\") -> %d categories"), *ContainerName, Categories.Num()));
		return sol::make_object(Lua, Result);
	});

	// ---- get_setting(container, category, section) ----
	Lua.set_function("get_setting", [&Session](const std::string& Container, const std::string& Category,
		const std::string& Section, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString FContainer = UTF8_TO_TCHAR(Container.c_str());
		FString FCategory = UTF8_TO_TCHAR(Category.c_str());
		FString FSection = UTF8_TO_TCHAR(Section.c_str());

		auto ContainerPtr = GetContainer(FContainer);
		if (!ContainerPtr.IsValid())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] get_setting -> container '%s' not found"), *FContainer));
			return sol::lua_nil;
		}

		auto SectionPtr = FindSection(ContainerPtr, FCategory, FSection);
		if (!SectionPtr.IsValid())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] get_setting -> section '%s/%s' not found in '%s'"), *FCategory, *FSection, *FContainer));
			return sol::lua_nil;
		}

		TWeakObjectPtr<UObject> SettingsObj = SectionPtr->GetSettingsObject();
		if (!SettingsObj.IsValid())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] get_setting -> section '%s' has no settings object"), *FSection));
			return sol::lua_nil;
		}

		sol::table Handle = MakeSettingsHandle(Lua, SettingsObj.Get(), SectionPtr, Session);
		Handle["container"] = Container;
		Handle["category"] = std::string(TCHAR_TO_UTF8(*FCategory));
		Handle["section"] = std::string(TCHAR_TO_UTF8(*SectionPtr->GetName().ToString()));

		Session.Log(FString::Printf(TEXT("[OK] get_setting -> %s (%s)"),
			*SectionPtr->GetDisplayName().ToString(), *SettingsObj->GetClass()->GetName()));
		return sol::make_object(Lua, Handle);
	});

	// ---- search_settings(query, container?) ----
	Lua.set_function("search_settings", [&Session](const std::string& Query, sol::optional<std::string> ContainerOpt, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString FQuery = UTF8_TO_TCHAR(Query.c_str());
		FString FContainer = ContainerOpt.has_value()
			? UTF8_TO_TCHAR(ContainerOpt.value().c_str())
			: TEXT("all");

		TArray<FString> ContainerNames;
		if (FContainer.Equals(TEXT("all"), ESearchCase::IgnoreCase))
		{
			ContainerNames.Add(TEXT("project"));
			ContainerNames.Add(TEXT("editor"));
		}
		else
		{
			ContainerNames.Add(FContainer);
		}

		sol::table Results = Lua.create_table();
		int32 ResultIdx = 1;

		for (const FString& ContName : ContainerNames)
		{
			auto Container = GetContainer(ContName);
			if (!Container.IsValid()) continue;

			TArray<TSharedPtr<ISettingsCategory>> Categories;
			Container->GetCategories(Categories);

			for (const auto& Category : Categories)
			{
				TArray<TSharedPtr<ISettingsSection>> Sections;
				Category->GetSections(Sections);

				for (const auto& Section : Sections)
				{
					bool bMatch = false;
					FString SecName = Section->GetName().ToString();
					FString SecDisplay = Section->GetDisplayName().ToString();

					// Match on section name/display name
					if (SecName.Contains(FQuery, ESearchCase::IgnoreCase) ||
						SecDisplay.Contains(FQuery, ESearchCase::IgnoreCase))
					{
						bMatch = true;
					}

					// Match on property names
					TArray<std::string> MatchingProps;
					TWeakObjectPtr<UObject> Obj = Section->GetSettingsObject();
					if (Obj.IsValid())
					{
						for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
						{
							if (It->GetName().Contains(FQuery, ESearchCase::IgnoreCase))
							{
								bMatch = true;
								MatchingProps.Add(std::string(TCHAR_TO_UTF8(*It->GetName())));
							}
						}
					}

					if (bMatch)
					{
						sol::table Entry = Lua.create_table();
						Entry["container"] = std::string(TCHAR_TO_UTF8(*ContName));
						Entry["category"] = std::string(TCHAR_TO_UTF8(*Category->GetName().ToString()));
						Entry["section"] = std::string(TCHAR_TO_UTF8(*SecName));
						Entry["display_name"] = std::string(TCHAR_TO_UTF8(*SecDisplay));

						if (MatchingProps.Num() > 0)
						{
							sol::table Props = Lua.create_table();
							for (int32 i = 0; i < MatchingProps.Num(); ++i)
								Props[i + 1] = MatchingProps[i];
							Entry["matching_properties"] = Props;
						}

						Results[ResultIdx++] = Entry;
					}
				}
			}
		}

		Session.Log(FString::Printf(TEXT("[OK] search_settings(\"%s\") -> %d matches"), *FQuery, ResultIdx - 1));
		return sol::make_object(Lua, Results);
	});

	// ---- find_plugin_settings(plugin_name?) ----
	Lua.set_function("find_plugin_settings", [&Session](sol::optional<std::string> PluginFilter, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString Filter;
		if (PluginFilter.has_value())
			Filter = UTF8_TO_TCHAR(PluginFilter.value().c_str());

		sol::table Results = Lua.create_table();
		int32 Idx = 1;

		for (TObjectIterator<UDeveloperSettings> It(RF_NoFlags); It; ++It)
		{
			UDeveloperSettings* Settings = *It;
			if (!IsValid(Settings) || !Settings->HasAnyFlags(RF_ClassDefaultObject)) continue;
			if (Settings->GetClass()->HasAnyClassFlags(CLASS_Deprecated | CLASS_Abstract)) continue;

			FString ClassName = Settings->GetClass()->GetName();
			FString SectionName = Settings->GetSectionName().ToString();
			FString CategoryName = Settings->GetCategoryName().ToString();
			FString ContainerName = Settings->GetContainerName().ToString();

			if (!Filter.IsEmpty())
			{
				if (!ClassName.Contains(Filter, ESearchCase::IgnoreCase) &&
					!SectionName.Contains(Filter, ESearchCase::IgnoreCase) &&
					!CategoryName.Contains(Filter, ESearchCase::IgnoreCase))
				{
					continue;
				}
			}

			int32 PropCount = 0;
			for (TFieldIterator<FProperty> PropIt(Settings->GetClass()); PropIt; ++PropIt)
			{
				if (!PropIt->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
					PropCount++;
			}

			sol::table Entry = Lua.create_table();
			Entry["class"] = std::string(TCHAR_TO_UTF8(*ClassName));
			Entry["container"] = std::string(TCHAR_TO_UTF8(*ContainerName));
			Entry["category"] = std::string(TCHAR_TO_UTF8(*CategoryName));
			Entry["section"] = std::string(TCHAR_TO_UTF8(*SectionName));
			Entry["display_name"] = std::string(TCHAR_TO_UTF8(*Settings->GetSectionText().ToString()));
			Entry["description"] = std::string(TCHAR_TO_UTF8(*Settings->GetSectionDescription().ToString()));
			Entry["property_count"] = PropCount;
			Results[Idx++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] find_plugin_settings -> %d settings classes found"), Idx - 1));
		return sol::make_object(Lua, Results);
	});

	// ──── Plugin Management ────

	// ---- list_plugins(filter?) ----
	Lua.set_function("list_plugins", [&Session](sol::optional<std::string> FilterOpt, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString Filter = FilterOpt.has_value()
			? UTF8_TO_TCHAR(FilterOpt.value().c_str())
			: TEXT("all");

		IPluginManager& PluginMgr = IPluginManager::Get();
		TArray<TSharedRef<IPlugin>> Plugins;

		if (Filter.Equals(TEXT("enabled"), ESearchCase::IgnoreCase))
			Plugins = PluginMgr.GetEnabledPlugins();
		else
			Plugins = PluginMgr.GetDiscoveredPlugins();

		sol::table Result = Lua.create_table();
		int32 Idx = 1;

		for (const auto& Plugin : Plugins)
		{
			bool bEnabled = Plugin->IsEnabled();

			// Apply filter
			if (Filter.Equals(TEXT("disabled"), ESearchCase::IgnoreCase) && bEnabled) continue;
			if (Filter.Equals(TEXT("project"), ESearchCase::IgnoreCase) && Plugin->GetType() != EPluginType::Project) continue;
			if (Filter.Equals(TEXT("engine"), ESearchCase::IgnoreCase) && Plugin->GetType() != EPluginType::Engine) continue;

			const FPluginDescriptor& Desc = Plugin->GetDescriptor();

			sol::table Entry = Lua.create_table();
			Entry["name"] = std::string(TCHAR_TO_UTF8(*Plugin->GetName()));
			Entry["friendly_name"] = std::string(TCHAR_TO_UTF8(*Desc.FriendlyName));
			Entry["version_name"] = std::string(TCHAR_TO_UTF8(*Desc.VersionName));
			Entry["category"] = std::string(TCHAR_TO_UTF8(*Desc.Category));
			Entry["description"] = std::string(TCHAR_TO_UTF8(*Desc.Description));
			Entry["is_enabled"] = bEnabled;
			Entry["has_content"] = Plugin->CanContainContent();

			FString TypeStr;
			switch (Plugin->GetType())
			{
			case EPluginType::Engine: TypeStr = TEXT("Engine"); break;
			case EPluginType::Project: TypeStr = TEXT("Project"); break;
			case EPluginType::External: TypeStr = TEXT("External"); break;
			case EPluginType::Enterprise: TypeStr = TEXT("Enterprise"); break;
			case EPluginType::Mod: TypeStr = TEXT("Mod"); break;
			default: TypeStr = TEXT("Unknown"); break;
			}
			Entry["type"] = std::string(TCHAR_TO_UTF8(*TypeStr));

			Result[Idx++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] list_plugins(\"%s\") -> %d plugins"), *Filter, Idx - 1));
		return sol::make_object(Lua, Result);
	});

	// ---- get_plugin(name) ----
	Lua.set_function("get_plugin", [&Session](const std::string& Name, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString FName = UTF8_TO_TCHAR(Name.c_str());

		IPluginManager& PluginMgr = IPluginManager::Get();
		TSharedPtr<IPlugin> Plugin = PluginMgr.FindPlugin(FName);

		if (!Plugin.IsValid())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] get_plugin -> '%s' not found"), *FName));
			return sol::lua_nil;
		}

		const FPluginDescriptor& Desc = Plugin->GetDescriptor();

		sol::table Result = Lua.create_table();
		Result["name"] = std::string(TCHAR_TO_UTF8(*Plugin->GetName()));
		Result["friendly_name"] = std::string(TCHAR_TO_UTF8(*Desc.FriendlyName));
		Result["version"] = Desc.Version;
		Result["version_name"] = std::string(TCHAR_TO_UTF8(*Desc.VersionName));
		Result["category"] = std::string(TCHAR_TO_UTF8(*Desc.Category));
		Result["description"] = std::string(TCHAR_TO_UTF8(*Desc.Description));
		Result["created_by"] = std::string(TCHAR_TO_UTF8(*Desc.CreatedBy));
		Result["created_by_url"] = std::string(TCHAR_TO_UTF8(*Desc.CreatedByURL));
		Result["docs_url"] = std::string(TCHAR_TO_UTF8(*Desc.DocsURL));
		Result["marketplace_url"] = std::string(TCHAR_TO_UTF8(*Desc.MarketplaceURL));
		Result["is_enabled"] = Plugin->IsEnabled();
		Result["is_hidden"] = Plugin->IsHidden();
		Result["has_content"] = Plugin->CanContainContent();
		Result["is_beta"] = Desc.bIsBetaVersion;
		Result["is_experimental"] = Desc.bIsExperimentalVersion;
		Result["base_dir"] = std::string(TCHAR_TO_UTF8(*Plugin->GetBaseDir()));

		// Supported platforms
		sol::table Platforms = Lua.create_table();
		for (int32 i = 0; i < Desc.SupportedTargetPlatforms.Num(); ++i)
			Platforms[i + 1] = std::string(TCHAR_TO_UTF8(*Desc.SupportedTargetPlatforms[i]));
		Result["supported_platforms"] = Platforms;

		// Dependencies
		TArray<FPluginReferenceDescriptor> Deps;
		PluginMgr.GetPluginDependencies(Plugin->GetName(), Deps);
		sol::table DepTable = Lua.create_table();
		for (int32 i = 0; i < Deps.Num(); ++i)
		{
			sol::table Dep = Lua.create_table();
			Dep["name"] = std::string(TCHAR_TO_UTF8(*Deps[i].Name));
			Dep["enabled"] = Deps[i].bEnabled;
			Dep["optional"] = Deps[i].bOptional;
			DepTable[i + 1] = Dep;
		}
		Result["dependencies"] = DepTable;

		// Modules
		sol::table Modules = Lua.create_table();
		for (int32 i = 0; i < Desc.Modules.Num(); ++i)
		{
			sol::table Mod = Lua.create_table();
			Mod["name"] = std::string(TCHAR_TO_UTF8(*Desc.Modules[i].Name.ToString()));
			Modules[i + 1] = Mod;
		}
		Result["modules"] = Modules;

		Session.Log(FString::Printf(TEXT("[OK] get_plugin(\"%s\") -> %s v%s"),
			*FName, *Desc.FriendlyName, *Desc.VersionName));
		return sol::make_object(Lua, Result);
	});

	// ---- set_plugin_enabled(name, enabled) ----
	Lua.set_function("set_plugin_enabled", [&Session](const std::string& Name, bool bEnabled, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString FName = UTF8_TO_TCHAR(Name.c_str());

		// Verify plugin exists
		IPluginManager& PluginMgr = IPluginManager::Get();
		TSharedPtr<IPlugin> Plugin = PluginMgr.FindPlugin(FName);
		if (!Plugin.IsValid())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] set_plugin_enabled -> plugin '%s' not found"), *FName));
			return sol::lua_nil;
		}

		FText FailReason;
		bool bSuccess = IProjectManager::Get().SetPluginEnabled(FName, bEnabled, FailReason);
		if (!bSuccess)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] set_plugin_enabled -> %s"), *FailReason.ToString()));
			return sol::lua_nil;
		}

		// Save the .uproject file
		bSuccess = IProjectManager::Get().SaveCurrentProjectToDisk(FailReason);
		if (!bSuccess)
		{
			Session.Log(FString::Printf(TEXT("[WARN] set_plugin_enabled -> changed but failed to save .uproject: %s"), *FailReason.ToString()));
		}

		Session.Log(FString::Printf(TEXT("[OK] set_plugin_enabled(\"%s\", %s) -> saved to .uproject (restart required)"),
			*FName, bEnabled ? TEXT("true") : TEXT("false")));
		return sol::make_object(Lua, true);
	});

	// ── get_cvar(name) ──
	Lua.set_function("get_cvar", [&Session](const std::string& Name, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		FString CvarName = UTF8_TO_TCHAR(Name.c_str());

		IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(*CvarName);
		if (!CVar)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] get_cvar(\"%s\") -> not found"), *CvarName));
			return sol::lua_nil;
		}

		sol::table Result = LuaView.create_table();
		Result["name"] = Name;
		Result["value"] = TCHAR_TO_UTF8(*CVar->GetString());
		Result["default_value"] = TCHAR_TO_UTF8(*CVar->GetDefaultValue());

		// Type info
		if (CVar->IsVariableInt())
		{
			Result["type"] = "int";
			Result["int_value"] = CVar->GetInt();
		}
		else if (CVar->IsVariableFloat())
		{
			Result["type"] = "float";
			Result["float_value"] = CVar->GetFloat();
		}
		else if (CVar->IsVariableBool())
		{
			Result["type"] = "bool";
			Result["bool_value"] = CVar->GetBool();
		}
		else
		{
			Result["type"] = "string";
		}

		Result["help"] = TCHAR_TO_UTF8(CVar->GetHelp());

		// Flags
		EConsoleVariableFlags Flags = CVar->GetFlags();
		sol::table FlagTable = LuaView.create_table();
		FlagTable["read_only"] = !!(Flags & ECVF_ReadOnly);
		FlagTable["render_thread_safe"] = !!(Flags & ECVF_RenderThreadSafe);
		FlagTable["scalability"] = !!(Flags & ECVF_Scalability);
		FlagTable["cheat"] = !!(Flags & ECVF_Cheat);
		Result["flags"] = FlagTable;

		Session.Log(FString::Printf(TEXT("[OK] get_cvar(\"%s\") = \"%s\""), *CvarName, *CVar->GetString()));
		return Result;
	});

	// ── set_cvar(name, value) ──
	Lua.set_function("set_cvar", [&Session](const std::string& Name, sol::object Value, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		FString CvarName = UTF8_TO_TCHAR(Name.c_str());

		IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(*CvarName);
		if (!CVar)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] set_cvar(\"%s\") -> not found"), *CvarName));
			return sol::lua_nil;
		}

		if (CVar->GetFlags() & ECVF_ReadOnly)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] set_cvar(\"%s\") -> read-only"), *CvarName));
			return sol::lua_nil;
		}

		if (CVar->TestFlags(ECVF_Cheat))
		{
			Session.Log(FString::Printf(TEXT("[WARN] set_cvar(\"%s\") -> cheat variable, may not take effect in shipping builds"), *CvarName));
		}

		if (Value.is<bool>())
		{
			CVar->Set(Value.as<bool>() ? 1 : 0, ECVF_SetByConsole);
		}
		else if (Value.is<int>())
		{
			CVar->Set(Value.as<int>(), ECVF_SetByConsole);
		}
		else if (Value.is<double>())
		{
			CVar->Set(static_cast<float>(Value.as<double>()), ECVF_SetByConsole);
		}
		else if (Value.is<std::string>())
		{
			CVar->Set(UTF8_TO_TCHAR(Value.as<std::string>().c_str()), ECVF_SetByConsole);
		}
		else
		{
			Session.Log(FString::Printf(TEXT("[FAIL] set_cvar(\"%s\") -> unsupported value type"), *CvarName));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] set_cvar(\"%s\") = \"%s\""), *CvarName, *CVar->GetString()));
		return sol::make_object(LuaView, true);
	});

	// ── find_cvars(query, opts?) ──
	Lua.set_function("find_cvars", [&Session](const std::string& Query, sol::optional<sol::table> Opts, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		FString SearchStr = UTF8_TO_TCHAR(Query.c_str());
		sol::table Result = LuaView.create_table();

		int32 Count = 0;
		int32 MaxResults = 50;
		std::string Mode = "contains";

		if (Opts.has_value())
		{
			MaxResults = Opts.value().get_or("limit", 50);
			Mode = Opts.value().get_or("mode", std::string("contains"));
		}

		auto Visitor = FConsoleObjectVisitor::CreateLambda([&](const TCHAR* Name, IConsoleObject* Obj)
		{
			if (Count >= MaxResults) return;
			IConsoleVariable* CVar = Obj->AsVariable();
			if (!CVar) return;

			sol::table Entry = LuaView.create_table();
			Entry["name"] = TCHAR_TO_UTF8(Name);
			Entry["value"] = TCHAR_TO_UTF8(*CVar->GetString());
			Entry["help"] = TCHAR_TO_UTF8(CVar->GetHelp());
			Result[++Count] = Entry;
		});

		if (Mode == "prefix")
		{
			IConsoleManager::Get().ForEachConsoleObjectThatStartsWith(Visitor, *SearchStr);
		}
		else
		{
			IConsoleManager::Get().ForEachConsoleObjectThatContains(Visitor, *SearchStr);
		}

		Session.Log(FString::Printf(TEXT("[OK] find_cvars(\"%s\", mode=%s) -> %d matches"),
			*SearchStr, UTF8_TO_TCHAR(Mode.c_str()), Count));
		return Result;
	});

	// ── exec_command ──
	Lua.set_function("exec_command", [&Session](sol::optional<std::string> CmdArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (!CmdArg.has_value() || CmdArg.value().empty())
		{
			Session.Log(TEXT("exec_command: command string is required"));
			return sol::lua_nil;
		}

		FString Command = UTF8_TO_TCHAR(CmdArg.value().c_str());

		// Capture output into a string
		FStringOutputDevice OutputDevice;
		UWorld* World = GWorld;
		bool bSuccess = IConsoleManager::Get().ProcessUserConsoleInput(*Command, OutputDevice, World);

		sol::table Result = LuaView.create_table();
		Result["success"] = bSuccess;

		FString Output = OutputDevice;
		if (!Output.IsEmpty())
		{
			Result["output"] = TCHAR_TO_UTF8(*Output);
		}
		else
		{
			Result["output"] = "";
		}

		Session.Log(FString::Printf(TEXT("[OK] exec_command(\"%s\") -> %s"), *Command, bSuccess ? TEXT("success") : TEXT("unrecognized")));
		return Result;
	});

	// ── read_config ──
	Lua.set_function("read_config", [&Session](sol::optional<std::string> FileArg,
		sol::optional<std::string> SectionArg, sol::optional<std::string> KeyArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (!FileArg.has_value() || FileArg.value().empty())
		{
			Session.Log(TEXT("read_config: file argument is required"));
			return sol::lua_nil;
		}
		if (!SectionArg.has_value() || SectionArg.value().empty())
		{
			Session.Log(TEXT("read_config: section argument is required"));
			return sol::lua_nil;
		}

		FString Filename = ResolveConfigFilename(UTF8_TO_TCHAR(FileArg.value().c_str()));
		FString Section = UTF8_TO_TCHAR(SectionArg.value().c_str());

		if (!GConfig)
		{
			Session.Log(TEXT("read_config: GConfig is null"));
			return sol::lua_nil;
		}

		// If key is given, return single value
		if (KeyArg.has_value() && !KeyArg.value().empty())
		{
			FString Key = UTF8_TO_TCHAR(KeyArg.value().c_str());
			FString Value;
			if (GConfig->GetString(*Section, *Key, Value, Filename))
			{
				Session.Log(FString::Printf(TEXT("[OK] read_config('%s', '%s', '%s')"), *Filename, *Section, *Key));
				return sol::make_object(LuaView, TCHAR_TO_UTF8(*Value));
			}
			Session.Log(FString::Printf(TEXT("read_config: key '%s' not found in [%s]"), *Key, *Section));
			return sol::lua_nil;
		}

		// No key — return all keys in section
		TArray<FString> SectionLines;
		if (!GConfig->GetSection(*Section, SectionLines, Filename))
		{
			Session.Log(FString::Printf(TEXT("read_config: section '%s' not found"), *Section));
			return sol::lua_nil;
		}

		sol::table Result = LuaView.create_table();
		for (const FString& Line : SectionLines)
		{
			FString Key, Value;
			if (Line.Split(TEXT("="), &Key, &Value))
			{
				Result[TCHAR_TO_UTF8(*Key)] = TCHAR_TO_UTF8(*Value);
			}
		}

		Session.Log(FString::Printf(TEXT("[OK] read_config('%s', '%s') -> %d keys"), *Filename, *Section, SectionLines.Num()));
		return Result;
	});

	// ── write_config ──
	Lua.set_function("write_config", [&Session](sol::optional<std::string> FileArg,
		sol::optional<std::string> SectionArg, sol::optional<std::string> KeyArg,
		sol::optional<std::string> ValueArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		sol::table Result = LuaView.create_table();

		if (!FileArg.has_value() || FileArg.value().empty() ||
			!SectionArg.has_value() || SectionArg.value().empty() ||
			!KeyArg.has_value() || KeyArg.value().empty())
		{
			Session.Log(TEXT("write_config: file, section, and key are required"));
			Result["success"] = false;
			Result["error"] = "file, section, and key are all required";
			return Result;
		}

		if (!ValueArg.has_value())
		{
			Session.Log(TEXT("write_config: value argument is required"));
			Result["success"] = false;
			Result["error"] = "value argument is required";
			return Result;
		}

		if (!GConfig)
		{
			Result["success"] = false;
			Result["error"] = "GConfig is null";
			return Result;
		}

		FString Filename = ResolveConfigFilename(UTF8_TO_TCHAR(FileArg.value().c_str()));
		FString Section = UTF8_TO_TCHAR(SectionArg.value().c_str());
		FString Key = UTF8_TO_TCHAR(KeyArg.value().c_str());
		FString Value = UTF8_TO_TCHAR(ValueArg.value().c_str());

		GConfig->SetString(*Section, *Key, *Value, Filename);
		GConfig->Flush(false, Filename);

		Result["success"] = true;
		Result["file"] = TCHAR_TO_UTF8(*Filename);
		Session.Log(FString::Printf(TEXT("[OK] write_config('%s', '%s', '%s') = '%s'"), *Filename, *Section, *Key, *Value));
		return Result;
	});

	// ── remove_config(file, section, key?) ──
	Lua.set_function("remove_config", [&Session](sol::optional<std::string> FileArg,
		sol::optional<std::string> SectionArg, sol::optional<std::string> KeyArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		sol::table Result = LuaView.create_table();

		if (!FileArg.has_value() || FileArg.value().empty() ||
			!SectionArg.has_value() || SectionArg.value().empty())
		{
			Session.Log(TEXT("remove_config: file and section are required"));
			Result["success"] = false;
			Result["error"] = "file and section are required";
			return Result;
		}

		if (!GConfig)
		{
			Result["success"] = false;
			Result["error"] = "GConfig is null";
			return Result;
		}

		FString Filename = ResolveConfigFilename(UTF8_TO_TCHAR(FileArg.value().c_str()));
		FString Section = UTF8_TO_TCHAR(SectionArg.value().c_str());

		if (KeyArg.has_value() && !KeyArg.value().empty())
		{
			// Remove a single key
			FString Key = UTF8_TO_TCHAR(KeyArg.value().c_str());
			bool bRemoved = GConfig->RemoveKey(*Section, *Key, Filename);
			GConfig->Flush(false, Filename);
			Result["success"] = bRemoved;
			if (bRemoved)
			{
				Session.Log(FString::Printf(TEXT("[OK] remove_config('%s', '%s', '%s')"), *Filename, *Section, *Key));
			}
			else
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove_config -> key '%s' not found in [%s]"), *Key, *Section));
				Result["error"] = "key not found";
			}
		}
		else
		{
			// Remove entire section
			bool bRemoved = GConfig->EmptySection(*Section, Filename);
			GConfig->Flush(false, Filename);
			Result["success"] = bRemoved;
			if (bRemoved)
			{
				Session.Log(FString::Printf(TEXT("[OK] remove_config('%s', '%s') -> section cleared"), *Filename, *Section));
			}
			else
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove_config -> section '%s' not found"), *Section));
				Result["error"] = "section not found";
			}
		}

		return Result;
	});

	// ── list_config_sections ──
	Lua.set_function("list_config_sections", [&Session](sol::optional<std::string> FileArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (!FileArg.has_value() || FileArg.value().empty())
		{
			Session.Log(TEXT("list_config_sections: file argument is required"));
			return sol::lua_nil;
		}

		if (!GConfig)
		{
			Session.Log(TEXT("list_config_sections: GConfig is null"));
			return sol::lua_nil;
		}

		FString Filename = ResolveConfigFilename(UTF8_TO_TCHAR(FileArg.value().c_str()));

		TArray<FString> SectionNames;
		if (!GConfig->GetSectionNames(Filename, SectionNames))
		{
			Session.Log(FString::Printf(TEXT("list_config_sections: no sections found in '%s'"), *Filename));
			return sol::lua_nil;
		}

		sol::table Result = LuaView.create_table();
		for (int32 i = 0; i < SectionNames.Num(); i++)
		{
			Result[i + 1] = TCHAR_TO_UTF8(*SectionNames[i]);
		}

		Session.Log(FString::Printf(TEXT("[OK] list_config_sections('%s') -> %d sections"), *Filename, SectionNames.Num()));
		return Result;
	});
}

REGISTER_LUA_BINDING(Settings, SettingsDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindSettings(Lua, Session);
});
