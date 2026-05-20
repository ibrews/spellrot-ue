// Copyright 2025-2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Features/IModularFeatures.h"
#include "ILiveLinkClient.h"
#include "LiveLinkPresetTypes.h"
#include "LiveLinkTypes.h"
#include "LiveLinkSourceSettings.h"
#include "LiveLinkSubjectSettings.h"
#include "LiveLinkPreset.h"
#include "LiveLinkRole.h"
#include "LiveLinkFramePreProcessor.h"
#include "LiveLinkVirtualSubject.h"
#include "LiveLinkAnimationVirtualSubject.h"
#include "Roles/LiveLinkAnimationTypes.h"
#include "Roles/LiveLinkAnimationRole.h"
#include "Roles/LiveLinkTransformRole.h"
#include "Roles/LiveLinkCameraRole.h"
#include "Roles/LiveLinkLightRole.h"
#include "Roles/LiveLinkBasicRole.h"
#include "UObject/UObjectIterator.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// HELPERS
// ============================================================================

namespace
{

static ILiveLinkClient* GetLiveLinkClient()
{
	if (IModularFeatures::Get().IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName))
	{
		return &IModularFeatures::Get().GetModularFeature<ILiveLinkClient>(ILiveLinkClient::ModularFeatureName);
	}
	return nullptr;
}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
static FString SubjectStateToString(ELiveLinkSubjectState State)
{
	switch (State)
	{
	case ELiveLinkSubjectState::Connected:        return TEXT("Connected");
	case ELiveLinkSubjectState::Unresponsive:     return TEXT("Unresponsive");
	case ELiveLinkSubjectState::Disconnected:     return TEXT("Disconnected");
	case ELiveLinkSubjectState::InvalidOrDisabled: return TEXT("InvalidOrDisabled");
#if ENGINE_MINOR_VERSION >= 6
	case ELiveLinkSubjectState::Paused:           return TEXT("Paused");
#endif
	case ELiveLinkSubjectState::Unknown:          return TEXT("Unknown");
	default:                                       return TEXT("Unknown");
	}
}
#endif // ENGINE_MINOR_VERSION >= 5

static TSubclassOf<ULiveLinkRole> FindRoleClassByName(const FString& RoleName)
{
	TArray<UClass*> DerivedClasses;
	GetDerivedClasses(ULiveLinkRole::StaticClass(), DerivedClasses, true);
	for (UClass* RoleClass : DerivedClasses)
	{
		if (RoleClass->HasAnyClassFlags(CLASS_Abstract))
		{
			continue;
		}
		if (RoleClass->GetName().Equals(RoleName, ESearchCase::IgnoreCase) ||
			RoleClass->GetName().Replace(TEXT("ULiveLink"), TEXT("")).Replace(TEXT("Role"), TEXT("")).Equals(RoleName, ESearchCase::IgnoreCase))
		{
			return RoleClass;
		}
	}
	return nullptr;
}

static TSubclassOf<ULiveLinkVirtualSubject> FindVirtualSubjectClassByName(const FString& ClassName)
{
	TArray<UClass*> DerivedClasses;
	GetDerivedClasses(ULiveLinkVirtualSubject::StaticClass(), DerivedClasses, true);
	for (UClass* VSClass : DerivedClasses)
	{
		if (VSClass->HasAnyClassFlags(CLASS_Abstract))
		{
			continue;
		}
		if (VSClass->GetName().Equals(ClassName, ESearchCase::IgnoreCase))
		{
			return VSClass;
		}
	}
	return nullptr;
}

} // anonymous namespace

// ============================================================================
// DOCUMENTATION
// ============================================================================

static TArray<FLuaFunctionDoc> LiveLinkDocs = {
	{ TEXT("livelink_get_sources()"), TEXT("List all LiveLink sources with guid, type, status"), TEXT("table") },
	{ TEXT("livelink_get_subjects(include_disabled?, include_virtual?)"), TEXT("List all LiveLink subjects with name, role, state"), TEXT("table") },
	{ TEXT("livelink_remove_source(guid_string)"), TEXT("Remove a LiveLink source by GUID string"), TEXT("bool") },
	{ TEXT("livelink_create_source(preset_path)"), TEXT("Create LiveLink sources from a ULiveLinkPreset asset"), TEXT("table") },
	{ TEXT("livelink_pause_subject(subject_name)"), TEXT("Pause a LiveLink subject by name"), TEXT("bool") },
	{ TEXT("livelink_unpause_subject(subject_name)"), TEXT("Unpause a LiveLink subject by name"), TEXT("bool") },
	{ TEXT("livelink_set_subject_enabled(subject_name, source_guid, enabled)"), TEXT("Enable or disable a LiveLink subject"), TEXT("bool") },
	{ TEXT("livelink_is_source_valid(guid_string)"), TEXT("Check if a LiveLink source is still valid"), TEXT("bool") },
	{ TEXT("livelink_get_subject_state(subject_name)"), TEXT("Get subject state: Connected, Unresponsive, Disconnected, etc."), TEXT("string") },
	{ TEXT("livelink_add_virtual_subject(subject_name, virtual_subject_class?)"), TEXT("Create a virtual subject (defaults to animation)"), TEXT("string or nil") },
	{ TEXT("livelink_remove_virtual_subject(subject_name, source_guid)"), TEXT("Remove a virtual subject by name and source GUID"), TEXT("bool") },
	{ TEXT("livelink_get_virtual_subjects()"), TEXT("List all virtual subjects with source subjects"), TEXT("table") },
	{ TEXT("livelink_get_subject_settings(subject_name, source_guid)"), TEXT("Get subject settings: preprocessors, interpolation, translators"), TEXT("table or nil") },
	{ TEXT("livelink_get_source_settings(source_guid)"), TEXT("Get source settings: mode, buffer_settings, connection_string"), TEXT("table or nil") },
	{ TEXT("livelink_is_subject_time_synced(subject_name)"), TEXT("Check if a subject is time synchronized"), TEXT("bool") },
	{ TEXT("livelink_get_subject_frame_times(subject_name)"), TEXT("Get buffered frame times for a subject"), TEXT("table") },
	{ TEXT("livelink_list_roles()"), TEXT("List all available LiveLink role class names"), TEXT("table") },
	{ TEXT("livelink_get_subjects_for_role(role_name, include_disabled?, include_virtual?)"), TEXT("Get subjects that support a specific role"), TEXT("table") },
	{ TEXT("livelink_get_static_data(subject_name, source_guid)"), TEXT("Get static data for a subject (bone_names, property_names, etc.)"), TEXT("table or nil") },
	{ TEXT("livelink_evaluate_frame(subject_name, role_name?)"), TEXT("Evaluate the current frame for a subject"), TEXT("table or nil") },
	{ TEXT("livelink_clear_subject_frames(subject_name, source_guid?)"), TEXT("Clear buffered frames for a subject (by name or key)"), TEXT("bool") },
	{ TEXT("livelink_clear_all_frames()"), TEXT("Clear all buffered frames for all subjects"), TEXT("bool") },
	{ TEXT("livelink_remove_subject(subject_name, source_guid)"), TEXT("Remove a non-virtual subject from a specific source"), TEXT("bool") },
	{ TEXT("livelink_does_subject_support_role(subject_name, role_name)"), TEXT("Check if a subject supports a specific role (directly or via translator)"), TEXT("bool") },
	{ TEXT("livelink_get_virtual_sources()"), TEXT("List all virtual subject source GUIDs"), TEXT("table") },
	{ TEXT("livelink_force_tick()"), TEXT("Force a LiveLink client tick outside normal engine tick"), TEXT("bool") },
};

// ============================================================================
// BINDING
// ============================================================================

static sol::object LiveLink_PauseSubject(FLuaSessionData& Session, const std::string& subject_name, sol::this_state S)
{
	sol::state_view LuaView(S);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	ILiveLinkClient* Client = GetLiveLinkClient();
	if (!Client)
	{
		Session.Log(TEXT("[FAIL] LiveLink client not available"));
		return sol::make_object(LuaView, false);
	}
	FLiveLinkSubjectName SubjectName;
	SubjectName.Name = FName(UTF8_TO_TCHAR(subject_name.c_str()));
	Client->PauseSubject_AnyThread(SubjectName);
	Session.Log(FString::Printf(TEXT("[OK] livelink_pause_subject(%s)"), UTF8_TO_TCHAR(subject_name.c_str())));
	return sol::make_object(LuaView, true);
#else
	Session.Log(TEXT("[FAIL] livelink_pause_subject requires UE 5.6+"));
	return sol::make_object(LuaView, false);
#endif
}

static sol::object LiveLink_UnpauseSubject(FLuaSessionData& Session, const std::string& subject_name, sol::this_state S)
{
	sol::state_view LuaView(S);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	ILiveLinkClient* Client = GetLiveLinkClient();
	if (!Client)
	{
		Session.Log(TEXT("[FAIL] LiveLink client not available"));
		return sol::make_object(LuaView, false);
	}
	FLiveLinkSubjectName SubjectName;
	SubjectName.Name = FName(UTF8_TO_TCHAR(subject_name.c_str()));
	Client->UnpauseSubject_AnyThread(SubjectName);
	Session.Log(FString::Printf(TEXT("[OK] livelink_unpause_subject(%s)"), UTF8_TO_TCHAR(subject_name.c_str())));
	return sol::make_object(LuaView, true);
#else
	Session.Log(TEXT("[FAIL] livelink_unpause_subject requires UE 5.6+"));
	return sol::make_object(LuaView, false);
#endif
}

static void LiveLink_PopulateTransmitEvaluatedData(sol::table& Result, ULiveLinkSourceSettings* Settings)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
	Result["transmit_evaluated_data"] = Settings->bTransmitEvaluatedData;
#endif
}

// Helper: Get subject state string (avoids #if inside macro)
static std::string LiveLink_GetSubjectStateStr(ILiveLinkClient* Client, FLiveLinkSubjectName SubjectName)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	return std::string(TCHAR_TO_UTF8(*SubjectStateToString(Client->GetSubjectState(SubjectName))));
#else
	(void)Client;
	(void)SubjectName;
	return std::string("Unknown");
#endif
}

// Helper: Get subject state with logging (avoids #if inside macro)
static sol::object LiveLink_GetSubjectStateFull(ILiveLinkClient* Client, const std::string& subject_name, FLuaSessionData& Session, sol::state_view& LuaView)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	FLiveLinkSubjectName SubjectName;
	SubjectName.Name = FName(UTF8_TO_TCHAR(subject_name.c_str()));

	ELiveLinkSubjectState State = Client->GetSubjectState(SubjectName);
	FString StateStr = SubjectStateToString(State);

	Session.Log(FString::Printf(TEXT("[OK] livelink_get_subject_state(%s) -> %s"),
		UTF8_TO_TCHAR(subject_name.c_str()), *StateStr));
	return sol::make_object(LuaView, std::string(TCHAR_TO_UTF8(*StateStr)));
#else
	(void)Client;
	(void)subject_name;
	Session.Log(TEXT("[FAIL] livelink_get_subject_state requires UE 5.5+"));
	return sol::make_object(LuaView, std::string("Unknown"));
#endif
}

// Helper: Populate ParentSubject field (avoids #if inside macro)
static void LiveLink_PopulateParentSubject(sol::table& Result, ULiveLinkSourceSettings* Settings)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	if (!Settings->ParentSubject.IsNone())
	{
		Result["parent_subject"] = std::string(TCHAR_TO_UTF8(*Settings->ParentSubject.Name.ToString()));
	}
#else
	(void)Result;
	(void)Settings;
#endif
}

// Helper: Populate static data for a subject (avoids #if inside macro)
static sol::object LiveLink_PopulateStaticData(ILiveLinkClient* Client, const FLiveLinkSubjectKey& SubjectKey, const std::string& subject_name, FLuaSessionData& Session, sol::state_view& LuaView)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	const FLiveLinkStaticDataStruct* StaticData = Client->GetSubjectStaticData_AnyThread(SubjectKey);
	if (!StaticData || !StaticData->IsValid())
	{
		Session.Log(FString::Printf(TEXT("[FAIL] livelink_get_static_data -> no static data for %s"), UTF8_TO_TCHAR(subject_name.c_str())));
		return sol::lua_nil;
	}

	sol::table Result = LuaView.create_table();
	if (const UScriptStruct* Struct = StaticData->GetStruct())
	{
		Result["struct_type"] = std::string(TCHAR_TO_UTF8(*Struct->GetName()));
	}

	// Base static data: property names
	const FLiveLinkBaseStaticData* BaseData = StaticData->GetBaseData();
	if (BaseData)
	{
		sol::table PropNames = LuaView.create_table();
		int32 PIdx = 1;
		for (const FName& PropName : BaseData->PropertyNames)
		{
			PropNames[PIdx++] = std::string(TCHAR_TO_UTF8(*PropName.ToString()));
		}
		Result["property_names"] = PropNames;
	}

	// Try to cast to skeleton static data for bone hierarchy
	const FLiveLinkSkeletonStaticData* SkeletonData = StaticData->Cast<FLiveLinkSkeletonStaticData>();
	if (SkeletonData)
	{
		sol::table BoneNames = LuaView.create_table();
		int32 BIdx = 1;
		for (const FName& BoneName : SkeletonData->GetBoneNames())
		{
			BoneNames[BIdx++] = std::string(TCHAR_TO_UTF8(*BoneName.ToString()));
		}
		Result["bone_names"] = BoneNames;

		sol::table BoneParents = LuaView.create_table();
		int32 BPIdx = 1;
		for (int32 ParentIdx : SkeletonData->GetBoneParents())
		{
			BoneParents[BPIdx++] = ParentIdx;
		}
		Result["bone_parents"] = BoneParents;

		Result["bone_count"] = SkeletonData->GetBoneNames().Num();
		Result["root_bone"] = SkeletonData->FindRootBone();
	}

	Session.Log(FString::Printf(TEXT("[OK] livelink_get_static_data(%s)"), UTF8_TO_TCHAR(subject_name.c_str())));
	return Result;
#else
	(void)Client;
	(void)SubjectKey;
	Session.Log(TEXT("[FAIL] livelink_get_static_data requires UE 5.5+"));
	return sol::lua_nil;
#endif
}

REGISTER_LUA_BINDING(LiveLink, LiveLinkDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	// ────────────────────────────────────────────────────────────────────
	// livelink_get_sources()
	// ────────────────────────────────────────────────────────────────────
	Lua.set_function("livelink_get_sources", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		ILiveLinkClient* Client = GetLiveLinkClient();
		if (!Client)
		{
			Session.Log(TEXT("[FAIL] LiveLink client not available"));
			return sol::lua_nil;
		}

		TArray<FGuid> Sources = Client->GetSources();

		sol::table Result = LuaView.create_table();
		int32 Idx = 1;
		for (const FGuid& SourceGuid : Sources)
		{
			sol::table Entry = LuaView.create_table();
			Entry["guid"] = std::string(TCHAR_TO_UTF8(*SourceGuid.ToString()));
			Entry["type"] = std::string(TCHAR_TO_UTF8(*Client->GetSourceType(SourceGuid).ToString()));
			Entry["status"] = std::string(TCHAR_TO_UTF8(*Client->GetSourceStatus(SourceGuid).ToString()));
			Entry["machine_name"] = std::string(TCHAR_TO_UTF8(*Client->GetSourceMachineName(SourceGuid).ToString()));
			Entry["is_valid"] = Client->IsSourceStillValid(SourceGuid);
			Result[Idx++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] livelink_get_sources() -> %d sources"), Sources.Num()));
		return Result;
	});

	// ────────────────────────────────────────────────────────────────────
	// livelink_get_subjects(include_disabled?, include_virtual?)
	// ────────────────────────────────────────────────────────────────────
	Lua.set_function("livelink_get_subjects", [&Session](
		sol::optional<bool> IncludeDisabled,
		sol::optional<bool> IncludeVirtual,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		ILiveLinkClient* Client = GetLiveLinkClient();
		if (!Client)
		{
			Session.Log(TEXT("[FAIL] LiveLink client not available"));
			return sol::lua_nil;
		}

		bool bIncludeDisabled = IncludeDisabled.value_or(false);
		bool bIncludeVirtual = IncludeVirtual.value_or(false);

		TArray<FLiveLinkSubjectKey> Subjects = Client->GetSubjects(bIncludeDisabled, bIncludeVirtual);

		sol::table Result = LuaView.create_table();
		int32 Idx = 1;
		for (const FLiveLinkSubjectKey& SubjectKey : Subjects)
		{
			sol::table Entry = LuaView.create_table();
			Entry["name"] = std::string(TCHAR_TO_UTF8(*SubjectKey.SubjectName.Name.ToString()));
			Entry["source_guid"] = std::string(TCHAR_TO_UTF8(*SubjectKey.Source.ToString()));

			FLiveLinkSubjectKey Key = SubjectKey;
			TSubclassOf<ULiveLinkRole> Role = Client->GetSubjectRole_AnyThread(Key);
			Entry["role"] = Role ? std::string(TCHAR_TO_UTF8(*Role->GetName())) : std::string("None");
			Entry["is_valid"] = Client->IsSubjectValid(SubjectKey);
			Entry["is_enabled"] = Client->IsSubjectEnabled(SubjectKey, false);
			Entry["is_virtual"] = Client->IsVirtualSubject(SubjectKey);
			Entry["state"] = LiveLink_GetSubjectStateStr(Client, SubjectKey.SubjectName);
			Result[Idx++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] livelink_get_subjects() -> %d subjects"), Subjects.Num()));
		return Result;
	});

	// ────────────────────────────────────────────────────────────────────
	// livelink_remove_source(guid_string)
	// ────────────────────────────────────────────────────────────────────
	Lua.set_function("livelink_remove_source", [&Session](const std::string& guid_str, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		ILiveLinkClient* Client = GetLiveLinkClient();
		if (!Client)
		{
			Session.Log(TEXT("[FAIL] LiveLink client not available"));
			return sol::make_object(LuaView, false);
		}

		FGuid Guid;
		if (!FGuid::Parse(UTF8_TO_TCHAR(guid_str.c_str()), Guid))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] livelink_remove_source -> invalid GUID: %s"), UTF8_TO_TCHAR(guid_str.c_str())));
			return sol::make_object(LuaView, false);
		}

		Client->RemoveSource(Guid);
		Session.Log(FString::Printf(TEXT("[OK] livelink_remove_source(%s) -> removed"), UTF8_TO_TCHAR(guid_str.c_str())));

		return sol::make_object(LuaView, true);
	});

	// ────────────────────────────────────────────────────────────────────
	// livelink_create_source(preset_path)
	// ────────────────────────────────────────────────────────────────────
	Lua.set_function("livelink_create_source", [&Session](const std::string& preset_path, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		ILiveLinkClient* Client = GetLiveLinkClient();
		if (!Client)
		{
			Session.Log(TEXT("[FAIL] LiveLink client not available"));
			return sol::lua_nil;
		}

		FString FPresetPath = UTF8_TO_TCHAR(preset_path.c_str());
		ULiveLinkPreset* Preset = LoadObject<ULiveLinkPreset>(nullptr, *FPresetPath);
		if (!Preset)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] livelink_create_source -> preset not found: %s"), *FPresetPath));
			return sol::lua_nil;
		}

		const TArray<FLiveLinkSourcePreset>& SourcePresets = Preset->GetSourcePresets();
		sol::table Result = LuaView.create_table();
		int32 Idx = 1;
		int32 Created = 0;

		for (const FLiveLinkSourcePreset& SourcePreset : SourcePresets)
		{
			if (!SourcePreset.Settings || !SourcePreset.Settings->Factory.Get())
			{
				continue;
			}

			bool bSuccess = Client->CreateSource(SourcePreset);
			if (bSuccess)
			{
				sol::table Entry = LuaView.create_table();
				Entry["index"] = Idx;
				Entry["success"] = true;
				Result[Idx++] = Entry;
				Created++;
			}
			else
			{
				sol::table Entry = LuaView.create_table();
				Entry["index"] = Idx;
				Entry["success"] = false;
				Result[Idx++] = Entry;
			}
		}

		Session.Log(FString::Printf(TEXT("[OK] livelink_create_source(%s) -> %d/%d sources created"), *FPresetPath, Created, SourcePresets.Num()));
		return Result;
	});

	// ────────────────────────────────────────────────────────────────────
	// livelink_pause_subject(subject_name)
	// ────────────────────────────────────────────────────────────────────
	Lua.set_function("livelink_pause_subject", [&Session](const std::string& subject_name, sol::this_state S) -> sol::object
	{
		return LiveLink_PauseSubject(Session, subject_name, S);
	});

	// ────────────────────────────────────────────────────────────────────
	// livelink_unpause_subject(subject_name)
	// ────────────────────────────────────────────────────────────────────
	Lua.set_function("livelink_unpause_subject", [&Session](const std::string& subject_name, sol::this_state S) -> sol::object
	{
		return LiveLink_UnpauseSubject(Session, subject_name, S);
	});

	// ────────────────────────────────────────────────────────────────────
	// livelink_set_subject_enabled(subject_name, source_guid, enabled)
	// ────────────────────────────────────────────────────────────────────
	Lua.set_function("livelink_set_subject_enabled", [&Session](
		const std::string& subject_name,
		const std::string& source_guid_str,
		bool bEnabled,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		ILiveLinkClient* Client = GetLiveLinkClient();
		if (!Client)
		{
			Session.Log(TEXT("[FAIL] LiveLink client not available"));
			return sol::make_object(LuaView, false);
		}

		FGuid SourceGuid;
		if (!FGuid::Parse(UTF8_TO_TCHAR(source_guid_str.c_str()), SourceGuid))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] livelink_set_subject_enabled -> invalid source GUID: %s"), UTF8_TO_TCHAR(source_guid_str.c_str())));
			return sol::make_object(LuaView, false);
		}

		FLiveLinkSubjectKey SubjectKey;
		SubjectKey.SubjectName.Name = FName(UTF8_TO_TCHAR(subject_name.c_str()));
		SubjectKey.Source = SourceGuid;

		Client->SetSubjectEnabled(SubjectKey, bEnabled);

		Session.Log(FString::Printf(TEXT("[OK] livelink_set_subject_enabled(%s, %s, %s)"),
			UTF8_TO_TCHAR(subject_name.c_str()),
			UTF8_TO_TCHAR(source_guid_str.c_str()),
			bEnabled ? TEXT("true") : TEXT("false")));
		return sol::make_object(LuaView, true);
	});

	// ────────────────────────────────────────────────────────────────────
	// livelink_is_source_valid(guid_string)
	// ────────────────────────────────────────────────────────────────────
	Lua.set_function("livelink_is_source_valid", [&Session](const std::string& guid_str, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		ILiveLinkClient* Client = GetLiveLinkClient();
		if (!Client)
		{
			Session.Log(TEXT("[FAIL] LiveLink client not available"));
			return sol::make_object(LuaView, false);
		}

		FGuid Guid;
		if (!FGuid::Parse(UTF8_TO_TCHAR(guid_str.c_str()), Guid))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] livelink_is_source_valid -> invalid GUID: %s"), UTF8_TO_TCHAR(guid_str.c_str())));
			return sol::make_object(LuaView, false);
		}

		bool bValid = Client->IsSourceStillValid(Guid);

		Session.Log(FString::Printf(TEXT("[OK] livelink_is_source_valid(%s) -> %s"),
			UTF8_TO_TCHAR(guid_str.c_str()),
			bValid ? TEXT("true") : TEXT("false")));
		return sol::make_object(LuaView, bValid);
	});

	// ────────────────────────────────────────────────────────────────────
	// livelink_get_subject_state(subject_name)
	// ────────────────────────────────────────────────────────────────────
	Lua.set_function("livelink_get_subject_state", [&Session](const std::string& subject_name, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		ILiveLinkClient* Client = GetLiveLinkClient();
		if (!Client)
		{
			Session.Log(TEXT("[FAIL] LiveLink client not available"));
			return sol::lua_nil;
		}

		return LiveLink_GetSubjectStateFull(Client, subject_name, Session, LuaView);
	});

	// ────────────────────────────────────────────────────────────────────
	// livelink_add_virtual_subject(subject_name, virtual_subject_class?)
	// ────────────────────────────────────────────────────────────────────
	Lua.set_function("livelink_add_virtual_subject", [&Session](
		const std::string& subject_name,
		sol::optional<std::string> vs_class_name,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		ILiveLinkClient* Client = GetLiveLinkClient();
		if (!Client)
		{
			Session.Log(TEXT("[FAIL] LiveLink client not available"));
			return sol::lua_nil;
		}

		// Add a virtual subject source
		FName SourceName = FName(FString::Printf(TEXT("VirtualSubjectSource_%s"), UTF8_TO_TCHAR(subject_name.c_str())));
		FGuid SourceGuid = Client->AddVirtualSubjectSource(SourceName);
		if (!SourceGuid.IsValid())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] livelink_add_virtual_subject -> failed to add virtual subject source for %s"), UTF8_TO_TCHAR(subject_name.c_str())));
			return sol::lua_nil;
		}

		// Resolve virtual subject class
		TSubclassOf<ULiveLinkVirtualSubject> VSClass;
		if (vs_class_name.has_value() && !vs_class_name.value().empty())
		{
			VSClass = FindVirtualSubjectClassByName(UTF8_TO_TCHAR(vs_class_name.value().c_str()));
			if (!VSClass)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] livelink_add_virtual_subject -> virtual subject class not found: %s"), UTF8_TO_TCHAR(vs_class_name.value().c_str())));
				Client->RemoveSource(SourceGuid);
				return sol::lua_nil;
			}
		}
		else
		{
			VSClass = ULiveLinkAnimationVirtualSubject::StaticClass();
		}

		FLiveLinkSubjectKey VirtualSubjectKey;
		VirtualSubjectKey.SubjectName.Name = FName(UTF8_TO_TCHAR(subject_name.c_str()));
		VirtualSubjectKey.Source = SourceGuid;

		bool bSuccess = Client->AddVirtualSubject(VirtualSubjectKey, VSClass);
		if (!bSuccess)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] livelink_add_virtual_subject -> AddVirtualSubject failed for %s"), UTF8_TO_TCHAR(subject_name.c_str())));
			Client->RemoveSource(SourceGuid);
			return sol::lua_nil;
		}

		FString GuidStr = SourceGuid.ToString();
		Session.Log(FString::Printf(TEXT("[OK] livelink_add_virtual_subject(%s) -> source_guid=%s"), UTF8_TO_TCHAR(subject_name.c_str()), *GuidStr));
		return sol::make_object(LuaView, std::string(TCHAR_TO_UTF8(*GuidStr)));
	});

	// ────────────────────────────────────────────────────────────────────
	// livelink_remove_virtual_subject(subject_name, source_guid)
	// ────────────────────────────────────────────────────────────────────
	Lua.set_function("livelink_remove_virtual_subject", [&Session](
		const std::string& subject_name,
		const std::string& source_guid_str,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		ILiveLinkClient* Client = GetLiveLinkClient();
		if (!Client)
		{
			Session.Log(TEXT("[FAIL] LiveLink client not available"));
			return sol::make_object(LuaView, false);
		}

		FGuid SourceGuid;
		if (!FGuid::Parse(UTF8_TO_TCHAR(source_guid_str.c_str()), SourceGuid))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] livelink_remove_virtual_subject -> invalid source GUID: %s"), UTF8_TO_TCHAR(source_guid_str.c_str())));
			return sol::make_object(LuaView, false);
		}

		FLiveLinkSubjectKey VirtualSubjectKey;
		VirtualSubjectKey.SubjectName.Name = FName(UTF8_TO_TCHAR(subject_name.c_str()));
		VirtualSubjectKey.Source = SourceGuid;

		if (!Client->IsVirtualSubject(VirtualSubjectKey))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] livelink_remove_virtual_subject -> %s is not a virtual subject"), UTF8_TO_TCHAR(subject_name.c_str())));
			return sol::make_object(LuaView, false);
		}

		Client->RemoveVirtualSubject(VirtualSubjectKey);

		Session.Log(FString::Printf(TEXT("[OK] livelink_remove_virtual_subject(%s, %s)"), UTF8_TO_TCHAR(subject_name.c_str()), UTF8_TO_TCHAR(source_guid_str.c_str())));
		return sol::make_object(LuaView, true);
	});

	// ────────────────────────────────────────────────────────────────────
	// livelink_get_virtual_subjects()
	// ────────────────────────────────────────────────────────────────────
	Lua.set_function("livelink_get_virtual_subjects", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		ILiveLinkClient* Client = GetLiveLinkClient();
		if (!Client)
		{
			Session.Log(TEXT("[FAIL] LiveLink client not available"));
			return sol::lua_nil;
		}

		// Get all subjects including virtual ones
		TArray<FLiveLinkSubjectKey> AllSubjects = Client->GetSubjects(true, true);

		sol::table Result = LuaView.create_table();
		int32 Idx = 1;
		for (const FLiveLinkSubjectKey& SubjectKey : AllSubjects)
		{
			if (!Client->IsVirtualSubject(SubjectKey))
			{
				continue;
			}

			sol::table Entry = LuaView.create_table();
			Entry["name"] = std::string(TCHAR_TO_UTF8(*SubjectKey.SubjectName.Name.ToString()));
			Entry["source_guid"] = std::string(TCHAR_TO_UTF8(*SubjectKey.Source.ToString()));

			TSubclassOf<ULiveLinkRole> Role = Client->GetSubjectRole_AnyThread(SubjectKey);
			Entry["role"] = Role ? std::string(TCHAR_TO_UTF8(*Role->GetName())) : std::string("None");

			// Try to get the virtual subject's source subjects
			UObject* Settings = Client->GetSubjectSettings(SubjectKey);
			ULiveLinkVirtualSubject* VirtualSubject = Cast<ULiveLinkVirtualSubject>(Settings);
			if (VirtualSubject)
			{
				const TArray<FLiveLinkSubjectName>& SourceSubjects = VirtualSubject->GetSubjects();
				sol::table SubjectsTable = LuaView.create_table();
				int32 SubIdx = 1;
				for (const FLiveLinkSubjectName& SrcSubject : SourceSubjects)
				{
					SubjectsTable[SubIdx++] = std::string(TCHAR_TO_UTF8(*SrcSubject.Name.ToString()));
				}
				Entry["subjects"] = SubjectsTable;
			}

			Result[Idx++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] livelink_get_virtual_subjects() -> %d virtual subjects"), Idx - 1));
		return Result;
	});

	// ────────────────────────────────────────────────────────────────────
	// livelink_get_subject_settings(subject_name, source_guid)
	// ────────────────────────────────────────────────────────────────────
	Lua.set_function("livelink_get_subject_settings", [&Session](
		const std::string& subject_name,
		const std::string& source_guid_str,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		ILiveLinkClient* Client = GetLiveLinkClient();
		if (!Client)
		{
			Session.Log(TEXT("[FAIL] LiveLink client not available"));
			return sol::lua_nil;
		}

		FGuid SourceGuid;
		if (!FGuid::Parse(UTF8_TO_TCHAR(source_guid_str.c_str()), SourceGuid))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] livelink_get_subject_settings -> invalid source GUID: %s"), UTF8_TO_TCHAR(source_guid_str.c_str())));
			return sol::lua_nil;
		}

		FLiveLinkSubjectKey SubjectKey;
		SubjectKey.SubjectName.Name = FName(UTF8_TO_TCHAR(subject_name.c_str()));
		SubjectKey.Source = SourceGuid;

		UObject* SettingsObj = Client->GetSubjectSettings(SubjectKey);
		if (!SettingsObj)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] livelink_get_subject_settings -> no settings for %s"), UTF8_TO_TCHAR(subject_name.c_str())));
			return sol::lua_nil;
		}

		sol::table Result = LuaView.create_table();
		Result["class"] = std::string(TCHAR_TO_UTF8(*SettingsObj->GetClass()->GetName()));
		Result["is_virtual"] = Client->IsVirtualSubject(SubjectKey);
		Result["is_time_synced"] = Client->IsSubjectTimeSynchronized(SubjectKey);

		// If it's a regular subject settings
		ULiveLinkSubjectSettings* SubjectSettings = Cast<ULiveLinkSubjectSettings>(SettingsObj);
		if (SubjectSettings)
		{
			// Preprocessors
			sol::table PreProcessors = LuaView.create_table();
			int32 PPIdx = 1;
			for (const auto& PP : SubjectSettings->PreProcessors)
			{
				if (PP)
				{
					PreProcessors[PPIdx++] = std::string(TCHAR_TO_UTF8(*PP->GetClass()->GetName()));
				}
			}
			Result["preprocessors"] = PreProcessors;

			// Interpolation
			if (SubjectSettings->InterpolationProcessor)
			{
				Result["interpolation"] = std::string(TCHAR_TO_UTF8(*SubjectSettings->InterpolationProcessor->GetClass()->GetName()));
			}

			// Translators
			sol::table Translators = LuaView.create_table();
			int32 TIdx = 1;
			for (const auto& Translator : SubjectSettings->Translators)
			{
				if (Translator)
				{
					Translators[TIdx++] = std::string(TCHAR_TO_UTF8(*Translator->GetClass()->GetName()));
				}
			}
			Result["translators"] = Translators;

			// Frame rate
			Result["frame_rate_numerator"] = SubjectSettings->FrameRate.Numerator;
			Result["frame_rate_denominator"] = SubjectSettings->FrameRate.Denominator;
			Result["rebroadcast"] = SubjectSettings->bRebroadcastSubject;

			if (SubjectSettings->Role)
			{
				Result["role"] = std::string(TCHAR_TO_UTF8(*SubjectSettings->Role->GetName()));
			}
		}

		// If it's a virtual subject
		ULiveLinkVirtualSubject* VirtualSubject = Cast<ULiveLinkVirtualSubject>(SettingsObj);
		if (VirtualSubject)
		{
			const TArray<FLiveLinkSubjectName>& SourceSubjects = VirtualSubject->GetSubjects();
			sol::table SubjectsTable = LuaView.create_table();
			int32 SIdx = 1;
			for (const FLiveLinkSubjectName& SrcSubject : SourceSubjects)
			{
				SubjectsTable[SIdx++] = std::string(TCHAR_TO_UTF8(*SrcSubject.Name.ToString()));
			}
			Result["source_subjects"] = SubjectsTable;

			const TArray<ULiveLinkFrameTranslator*>& VTrans = VirtualSubject->GetTranslators();
			sol::table VTransTable = LuaView.create_table();
			int32 VTIdx = 1;
			for (const auto& VT : VTrans)
			{
				if (VT)
				{
					VTransTable[VTIdx++] = std::string(TCHAR_TO_UTF8(*VT->GetClass()->GetName()));
				}
			}
			Result["translators"] = VTransTable;
		}

		Session.Log(FString::Printf(TEXT("[OK] livelink_get_subject_settings(%s)"), UTF8_TO_TCHAR(subject_name.c_str())));
		return Result;
	});

	// ────────────────────────────────────────────────────────────────────
	// livelink_get_source_settings(source_guid)
	// ────────────────────────────────────────────────────────────────────
	Lua.set_function("livelink_get_source_settings", [&Session](const std::string& guid_str, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		ILiveLinkClient* Client = GetLiveLinkClient();
		if (!Client)
		{
			Session.Log(TEXT("[FAIL] LiveLink client not available"));
			return sol::lua_nil;
		}

		FGuid SourceGuid;
		if (!FGuid::Parse(UTF8_TO_TCHAR(guid_str.c_str()), SourceGuid))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] livelink_get_source_settings -> invalid GUID: %s"), UTF8_TO_TCHAR(guid_str.c_str())));
			return sol::lua_nil;
		}

		ULiveLinkSourceSettings* Settings = Client->GetSourceSettings(SourceGuid);
		if (!Settings)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] livelink_get_source_settings -> no settings for source %s"), UTF8_TO_TCHAR(guid_str.c_str())));
			return sol::lua_nil;
		}

		sol::table Result = LuaView.create_table();
		Result["class"] = std::string(TCHAR_TO_UTF8(*Settings->GetClass()->GetName()));

		// Mode
		switch (Settings->Mode)
		{
		case ELiveLinkSourceMode::Latest:     Result["mode"] = std::string("Latest"); break;
		case ELiveLinkSourceMode::EngineTime: Result["mode"] = std::string("EngineTime"); break;
		case ELiveLinkSourceMode::Timecode:   Result["mode"] = std::string("Timecode"); break;
		default:                              Result["mode"] = std::string("Unknown"); break;
		}

		// Buffer settings
		sol::table Buffer = LuaView.create_table();
		Buffer["max_frames"] = Settings->BufferSettings.MaxNumberOfFrameToBuffered;
		Buffer["valid_engine_time_enabled"] = Settings->BufferSettings.bValidEngineTimeEnabled;
		Buffer["valid_engine_time"] = Settings->BufferSettings.ValidEngineTime;
		Buffer["engine_time_offset"] = Settings->BufferSettings.EngineTimeOffset;
		Buffer["engine_time_clock_offset"] = Settings->BufferSettings.EngineTimeClockOffset;
		Buffer["smooth_engine_time_offset"] = Settings->BufferSettings.SmoothEngineTimeOffset;
		Buffer["generate_sub_frame"] = Settings->BufferSettings.bGenerateSubFrame;
		Buffer["source_timecode_frame_rate_num"] = Settings->BufferSettings.SourceTimecodeFrameRate.Numerator;
		Buffer["source_timecode_frame_rate_den"] = Settings->BufferSettings.SourceTimecodeFrameRate.Denominator;
		Buffer["detected_frame_rate_num"] = Settings->BufferSettings.DetectedFrameRate.Numerator;
		Buffer["detected_frame_rate_den"] = Settings->BufferSettings.DetectedFrameRate.Denominator;
		Buffer["use_timecode_smooth_latest"] = Settings->BufferSettings.bUseTimecodeSmoothLatest;
		Buffer["valid_timecode_frame_enabled"] = Settings->BufferSettings.bValidTimecodeFrameEnabled;
		Buffer["valid_timecode_frame"] = Settings->BufferSettings.ValidTimecodeFrame;
		Buffer["timecode_frame_offset"] = Settings->BufferSettings.TimecodeFrameOffset;
		Buffer["timecode_clock_offset"] = Settings->BufferSettings.TimecodeClockOffset;
		Buffer["latest_offset"] = Settings->BufferSettings.LatestOffset;
		Buffer["keep_at_least_one_frame"] = Settings->BufferSettings.bKeepAtLeastOneFrame;
		Result["buffer_settings"] = Buffer;

		Result["connection_string"] = std::string(TCHAR_TO_UTF8(*Settings->ConnectionString));
		LiveLink_PopulateTransmitEvaluatedData(Result, Settings);

		LiveLink_PopulateParentSubject(Result, Settings);

		Session.Log(FString::Printf(TEXT("[OK] livelink_get_source_settings(%s)"), UTF8_TO_TCHAR(guid_str.c_str())));
		return Result;
	});

	// ────────────────────────────────────────────────────────────────────
	// livelink_is_subject_time_synced(subject_name)
	// ────────────────────────────────────────────────────────────────────
	Lua.set_function("livelink_is_subject_time_synced", [&Session](const std::string& subject_name, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		ILiveLinkClient* Client = GetLiveLinkClient();
		if (!Client)
		{
			Session.Log(TEXT("[FAIL] LiveLink client not available"));
			return sol::make_object(LuaView, false);
		}

		FLiveLinkSubjectName SubjectName;
		SubjectName.Name = FName(UTF8_TO_TCHAR(subject_name.c_str()));

		bool bSynced = Client->IsSubjectTimeSynchronized(SubjectName);

		Session.Log(FString::Printf(TEXT("[OK] livelink_is_subject_time_synced(%s) -> %s"),
			UTF8_TO_TCHAR(subject_name.c_str()),
			bSynced ? TEXT("true") : TEXT("false")));
		return sol::make_object(LuaView, bSynced);
	});

	// ────────────────────────────────────────────────────────────────────
	// livelink_get_subject_frame_times(subject_name)
	// ────────────────────────────────────────────────────────────────────
	Lua.set_function("livelink_get_subject_frame_times", [&Session](const std::string& subject_name, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		ILiveLinkClient* Client = GetLiveLinkClient();
		if (!Client)
		{
			Session.Log(TEXT("[FAIL] LiveLink client not available"));
			return sol::lua_nil;
		}

		FLiveLinkSubjectName SubjectName;
		SubjectName.Name = FName(UTF8_TO_TCHAR(subject_name.c_str()));

		TArray<FLiveLinkTime> FrameTimes = Client->GetSubjectFrameTimes(SubjectName);

		sol::table Result = LuaView.create_table();
		int32 Idx = 1;
		for (const FLiveLinkTime& FrameTime : FrameTimes)
		{
			sol::table Entry = LuaView.create_table();
			Entry["world_time"] = FrameTime.WorldTime;
			Entry["scene_time_seconds"] = FrameTime.SceneTime.AsSeconds();
			Entry["scene_frame_rate_num"] = FrameTime.SceneTime.Rate.Numerator;
			Entry["scene_frame_rate_den"] = FrameTime.SceneTime.Rate.Denominator;
			Result[Idx++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] livelink_get_subject_frame_times(%s) -> %d frames"),
			UTF8_TO_TCHAR(subject_name.c_str()), FrameTimes.Num()));
		return Result;
	});

	// ────────────────────────────────────────────────────────────────────
	// livelink_list_roles()
	// ────────────────────────────────────────────────────────────────────
	Lua.set_function("livelink_list_roles", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		TArray<UClass*> DerivedClasses;
		GetDerivedClasses(ULiveLinkRole::StaticClass(), DerivedClasses, true);

		sol::table Result = LuaView.create_table();
		int32 Idx = 1;
		for (UClass* RoleClass : DerivedClasses)
		{
			if (RoleClass->HasAnyClassFlags(CLASS_Abstract))
			{
				continue;
			}

			sol::table Entry = LuaView.create_table();
			Entry["name"] = std::string(TCHAR_TO_UTF8(*RoleClass->GetName()));

			ULiveLinkRole* CDO = RoleClass->GetDefaultObject<ULiveLinkRole>();
			if (CDO)
			{
				Entry["display_name"] = std::string(TCHAR_TO_UTF8(*CDO->GetDisplayName().ToString()));

				UScriptStruct* StaticStruct = CDO->GetStaticDataStruct();
				if (StaticStruct)
				{
					Entry["static_data_struct"] = std::string(TCHAR_TO_UTF8(*StaticStruct->GetName()));
				}
				UScriptStruct* FrameStruct = CDO->GetFrameDataStruct();
				if (FrameStruct)
				{
					Entry["frame_data_struct"] = std::string(TCHAR_TO_UTF8(*FrameStruct->GetName()));
				}
			}
			Result[Idx++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] livelink_list_roles() -> %d roles"), Idx - 1));
		return Result;
	});

	// ────────────────────────────────────────────────────────────────────
	// livelink_get_subjects_for_role(role_name, include_disabled?, include_virtual?)
	// ────────────────────────────────────────────────────────────────────
	Lua.set_function("livelink_get_subjects_for_role", [&Session](
		const std::string& role_name,
		sol::optional<bool> IncludeDisabled,
		sol::optional<bool> IncludeVirtual,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		ILiveLinkClient* Client = GetLiveLinkClient();
		if (!Client)
		{
			Session.Log(TEXT("[FAIL] LiveLink client not available"));
			return sol::lua_nil;
		}

		TSubclassOf<ULiveLinkRole> RoleClass = FindRoleClassByName(UTF8_TO_TCHAR(role_name.c_str()));
		if (!RoleClass)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] livelink_get_subjects_for_role -> role not found: %s"), UTF8_TO_TCHAR(role_name.c_str())));
			return sol::lua_nil;
		}

		bool bIncludeDisabled = IncludeDisabled.value_or(false);
		bool bIncludeVirtual = IncludeVirtual.value_or(false);

		TArray<FLiveLinkSubjectKey> Subjects = Client->GetSubjectsSupportingRole(RoleClass, bIncludeDisabled, bIncludeVirtual);

		sol::table Result = LuaView.create_table();
		int32 Idx = 1;
		for (const FLiveLinkSubjectKey& SubjectKey : Subjects)
		{
			sol::table Entry = LuaView.create_table();
			Entry["name"] = std::string(TCHAR_TO_UTF8(*SubjectKey.SubjectName.Name.ToString()));
			Entry["source_guid"] = std::string(TCHAR_TO_UTF8(*SubjectKey.Source.ToString()));
			Entry["is_virtual"] = Client->IsVirtualSubject(SubjectKey);
			Result[Idx++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] livelink_get_subjects_for_role(%s) -> %d subjects"),
			UTF8_TO_TCHAR(role_name.c_str()), Subjects.Num()));
		return Result;
	});

	// ────────────────────────────────────────────────────────────────────
	// livelink_get_static_data(subject_name, source_guid)
	// ────────────────────────────────────────────────────────────────────
	Lua.set_function("livelink_get_static_data", [&Session](
		const std::string& subject_name,
		const std::string& source_guid_str,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		ILiveLinkClient* Client = GetLiveLinkClient();
		if (!Client)
		{
			Session.Log(TEXT("[FAIL] LiveLink client not available"));
			return sol::lua_nil;
		}

		FGuid SourceGuid;
		if (!FGuid::Parse(UTF8_TO_TCHAR(source_guid_str.c_str()), SourceGuid))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] livelink_get_static_data -> invalid source GUID: %s"), UTF8_TO_TCHAR(source_guid_str.c_str())));
			return sol::lua_nil;
		}

		FLiveLinkSubjectKey SubjectKey;
		SubjectKey.SubjectName.Name = FName(UTF8_TO_TCHAR(subject_name.c_str()));
		SubjectKey.Source = SourceGuid;

		return LiveLink_PopulateStaticData(Client, SubjectKey, subject_name, Session, LuaView);
	});

	// ────────────────────────────────────────────────────────────────────
	// livelink_evaluate_frame(subject_name, role_name?)
	// ────────────────────────────────────────────────────────────────────
	Lua.set_function("livelink_evaluate_frame", [&Session](
		const std::string& subject_name,
		sol::optional<std::string> role_name,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		ILiveLinkClient* Client = GetLiveLinkClient();
		if (!Client)
		{
			Session.Log(TEXT("[FAIL] LiveLink client not available"));
			return sol::lua_nil;
		}

		FLiveLinkSubjectName SubjectName;
		SubjectName.Name = FName(UTF8_TO_TCHAR(subject_name.c_str()));

		// Resolve role
		TSubclassOf<ULiveLinkRole> RoleClass;
		if (role_name.has_value() && !role_name.value().empty())
		{
			RoleClass = FindRoleClassByName(UTF8_TO_TCHAR(role_name.value().c_str()));
			if (!RoleClass)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] livelink_evaluate_frame -> role not found: %s"), UTF8_TO_TCHAR(role_name.value().c_str())));
				return sol::lua_nil;
			}
		}
		else
		{
			// Use the subject's own role
			RoleClass = Client->GetSubjectRole_AnyThread(SubjectName);
			if (!RoleClass)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] livelink_evaluate_frame -> cannot determine role for %s"), UTF8_TO_TCHAR(subject_name.c_str())));
				return sol::lua_nil;
			}
		}

		FLiveLinkSubjectFrameData FrameData;
		bool bSuccess = Client->EvaluateFrame_AnyThread(SubjectName, RoleClass, FrameData);
		if (!bSuccess)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] livelink_evaluate_frame -> evaluation failed for %s"), UTF8_TO_TCHAR(subject_name.c_str())));
			return sol::lua_nil;
		}

		sol::table Result = LuaView.create_table();

		// Frame data base: property values
		const FLiveLinkBaseFrameData* BaseFrameData = FrameData.FrameData.GetBaseData();
		if (BaseFrameData)
		{
			sol::table PropValues = LuaView.create_table();
			int32 PVIdx = 1;
			for (float Val : BaseFrameData->PropertyValues)
			{
				PropValues[PVIdx++] = Val;
			}
			Result["property_values"] = PropValues;
			Result["world_time"] = BaseFrameData->WorldTime.GetOffsettedTime();
		}

		// Static data: property names
		const FLiveLinkBaseStaticData* BaseStaticData = FrameData.StaticData.GetBaseData();
		if (BaseStaticData)
		{
			sol::table PropNames = LuaView.create_table();
			int32 PNIdx = 1;
			for (const FName& PropName : BaseStaticData->PropertyNames)
			{
				PropNames[PNIdx++] = std::string(TCHAR_TO_UTF8(*PropName.ToString()));
			}
			Result["property_names"] = PropNames;
		}

		// Animation-specific: transforms
		const FLiveLinkAnimationFrameData* AnimFrameData = FrameData.FrameData.Cast<FLiveLinkAnimationFrameData>();
		if (AnimFrameData)
		{
			sol::table Transforms = LuaView.create_table();
			int32 TIdx = 1;
			for (const FTransform& Transform : AnimFrameData->Transforms)
			{
				sol::table T = LuaView.create_table();
				FVector Loc = Transform.GetLocation();
				FRotator Rot = Transform.Rotator();
				FVector Scale = Transform.GetScale3D();
				T["location_x"] = Loc.X;
				T["location_y"] = Loc.Y;
				T["location_z"] = Loc.Z;
				T["rotation_pitch"] = Rot.Pitch;
				T["rotation_yaw"] = Rot.Yaw;
				T["rotation_roll"] = Rot.Roll;
				T["scale_x"] = Scale.X;
				T["scale_y"] = Scale.Y;
				T["scale_z"] = Scale.Z;
				Transforms[TIdx++] = T;
			}
			Result["transforms"] = Transforms;
			Result["transform_count"] = AnimFrameData->Transforms.Num();

			// Also include bone names from static data if available
			const FLiveLinkSkeletonStaticData* SkeletonData = FrameData.StaticData.Cast<FLiveLinkSkeletonStaticData>();
			if (SkeletonData)
			{
				sol::table BoneNames = LuaView.create_table();
				int32 BIdx = 1;
				for (const FName& BoneName : SkeletonData->GetBoneNames())
				{
					BoneNames[BIdx++] = std::string(TCHAR_TO_UTF8(*BoneName.ToString()));
				}
				Result["bone_names"] = BoneNames;
			}
		}

		Session.Log(FString::Printf(TEXT("[OK] livelink_evaluate_frame(%s)"), UTF8_TO_TCHAR(subject_name.c_str())));
		return Result;
	});

	// ────────────────────────────────────────────────────────────────────
	// livelink_clear_subject_frames(subject_name, source_guid?)
	// ────────────────────────────────────────────────────────────────────
	Lua.set_function("livelink_clear_subject_frames", [&Session](
		const std::string& subject_name,
		sol::optional<std::string> source_guid_str,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		ILiveLinkClient* Client = GetLiveLinkClient();
		if (!Client)
		{
			Session.Log(TEXT("[FAIL] LiveLink client not available"));
			return sol::make_object(LuaView, false);
		}

		if (source_guid_str.has_value() && !source_guid_str.value().empty())
		{
			FGuid SourceGuid;
			if (!FGuid::Parse(UTF8_TO_TCHAR(source_guid_str.value().c_str()), SourceGuid))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] livelink_clear_subject_frames -> invalid source GUID: %s"), UTF8_TO_TCHAR(source_guid_str.value().c_str())));
				return sol::make_object(LuaView, false);
			}

			FLiveLinkSubjectKey SubjectKey;
			SubjectKey.SubjectName.Name = FName(UTF8_TO_TCHAR(subject_name.c_str()));
			SubjectKey.Source = SourceGuid;
			Client->ClearSubjectsFrames_AnyThread(SubjectKey);
		}
		else
		{
			FLiveLinkSubjectName SubjectName;
			SubjectName.Name = FName(UTF8_TO_TCHAR(subject_name.c_str()));
			Client->ClearSubjectsFrames_AnyThread(SubjectName);
		}

		Session.Log(FString::Printf(TEXT("[OK] livelink_clear_subject_frames(%s)"), UTF8_TO_TCHAR(subject_name.c_str())));
		return sol::make_object(LuaView, true);
	});

	// ────────────────────────────────────────────────────────────────────
	// livelink_clear_all_frames()
	// ────────────────────────────────────────────────────────────────────
	Lua.set_function("livelink_clear_all_frames", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		ILiveLinkClient* Client = GetLiveLinkClient();
		if (!Client)
		{
			Session.Log(TEXT("[FAIL] LiveLink client not available"));
			return sol::make_object(LuaView, false);
		}

		Client->ClearAllSubjectsFrames_AnyThread();

		Session.Log(TEXT("[OK] livelink_clear_all_frames()"));
		return sol::make_object(LuaView, true);
	});

	// ────────────────────────────────────────────────────────────────────
	// livelink_remove_subject(subject_name, source_guid)
	// ────────────────────────────────────────────────────────────────────
	Lua.set_function("livelink_remove_subject", [&Session](
		const std::string& subject_name,
		const std::string& source_guid_str,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		ILiveLinkClient* Client = GetLiveLinkClient();
		if (!Client)
		{
			Session.Log(TEXT("[FAIL] LiveLink client not available"));
			return sol::make_object(LuaView, false);
		}

		FGuid SourceGuid;
		if (!FGuid::Parse(UTF8_TO_TCHAR(source_guid_str.c_str()), SourceGuid))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] livelink_remove_subject -> invalid source GUID: %s"), UTF8_TO_TCHAR(source_guid_str.c_str())));
			return sol::make_object(LuaView, false);
		}

		FLiveLinkSubjectKey SubjectKey;
		SubjectKey.SubjectName.Name = FName(UTF8_TO_TCHAR(subject_name.c_str()));
		SubjectKey.Source = SourceGuid;

		Client->RemoveSubject_AnyThread(SubjectKey);

		Session.Log(FString::Printf(TEXT("[OK] livelink_remove_subject(%s, %s)"), UTF8_TO_TCHAR(subject_name.c_str()), UTF8_TO_TCHAR(source_guid_str.c_str())));
		return sol::make_object(LuaView, true);
	});

	// ────────────────────────────────────────────────────────────────────
	// livelink_does_subject_support_role(subject_name, role_name)
	// ────────────────────────────────────────────────────────────────────
	Lua.set_function("livelink_does_subject_support_role", [&Session](
		const std::string& subject_name,
		const std::string& role_name,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		ILiveLinkClient* Client = GetLiveLinkClient();
		if (!Client)
		{
			Session.Log(TEXT("[FAIL] LiveLink client not available"));
			return sol::make_object(LuaView, false);
		}

		TSubclassOf<ULiveLinkRole> RoleClass = FindRoleClassByName(UTF8_TO_TCHAR(role_name.c_str()));
		if (!RoleClass)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] livelink_does_subject_support_role -> role not found: %s"), UTF8_TO_TCHAR(role_name.c_str())));
			return sol::make_object(LuaView, false);
		}

		FLiveLinkSubjectName SubjectName;
		SubjectName.Name = FName(UTF8_TO_TCHAR(subject_name.c_str()));

		bool bSupports = Client->DoesSubjectSupportsRole_AnyThread(SubjectName, RoleClass);

		Session.Log(FString::Printf(TEXT("[OK] livelink_does_subject_support_role(%s, %s) -> %s"),
			UTF8_TO_TCHAR(subject_name.c_str()),
			UTF8_TO_TCHAR(role_name.c_str()),
			bSupports ? TEXT("true") : TEXT("false")));
		return sol::make_object(LuaView, bSupports);
	});

	// ────────────────────────────────────────────────────────────────────
	// livelink_get_virtual_sources()
	// ────────────────────────────────────────────────────────────────────
	Lua.set_function("livelink_get_virtual_sources", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		ILiveLinkClient* Client = GetLiveLinkClient();
		if (!Client)
		{
			Session.Log(TEXT("[FAIL] LiveLink client not available"));
			return sol::lua_nil;
		}

		TArray<FGuid> VirtualSources = Client->GetVirtualSources();

		sol::table Result = LuaView.create_table();
		int32 Idx = 1;
		for (const FGuid& SourceGuid : VirtualSources)
		{
			Result[Idx++] = std::string(TCHAR_TO_UTF8(*SourceGuid.ToString()));
		}

		Session.Log(FString::Printf(TEXT("[OK] livelink_get_virtual_sources() -> %d sources"), VirtualSources.Num()));
		return Result;
	});

	// ────────────────────────────────────────────────────────────────────
	// livelink_force_tick()
	// ────────────────────────────────────────────────────────────────────
	Lua.set_function("livelink_force_tick", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		ILiveLinkClient* Client = GetLiveLinkClient();
		if (!Client)
		{
			Session.Log(TEXT("[FAIL] LiveLink client not available"));
			return sol::make_object(LuaView, false);
		}

		Client->ForceTick();

		Session.Log(TEXT("[OK] livelink_force_tick()"));
		return sol::make_object(LuaView, true);
	});
});

