// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Tools/NeoStackToolUtils.h"
#include "ScopedTransaction.h"

#include "Sound/SoundConcurrency.h"
#include "UObject/UnrealType.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// HELPERS
// ============================================================================

static const char* ResolutionRuleToString(EMaxConcurrentResolutionRule::Type Rule)
{
	switch (Rule)
	{
	case EMaxConcurrentResolutionRule::PreventNew:                     return "prevent_new";
	case EMaxConcurrentResolutionRule::StopOldest:                     return "stop_oldest";
	case EMaxConcurrentResolutionRule::StopFarthestThenPreventNew:     return "stop_farthest_then_prevent_new";
	case EMaxConcurrentResolutionRule::StopFarthestThenOldest:         return "stop_farthest_then_oldest";
	case EMaxConcurrentResolutionRule::StopLowestPriority:             return "stop_lowest_priority";
	case EMaxConcurrentResolutionRule::StopQuietest:                   return "stop_quietest";
	case EMaxConcurrentResolutionRule::StopLowestPriorityThenPreventNew: return "stop_lowest_priority_then_prevent_new";
	default:                                                           return "unknown";
	}
}

static EMaxConcurrentResolutionRule::Type StringToResolutionRule(const std::string& S)
{
	if (S == "prevent_new")                          return EMaxConcurrentResolutionRule::PreventNew;
	if (S == "stop_oldest")                          return EMaxConcurrentResolutionRule::StopOldest;
	if (S == "stop_farthest_then_prevent_new")       return EMaxConcurrentResolutionRule::StopFarthestThenPreventNew;
	if (S == "stop_farthest_then_oldest")            return EMaxConcurrentResolutionRule::StopFarthestThenOldest;
	if (S == "stop_lowest_priority")                 return EMaxConcurrentResolutionRule::StopLowestPriority;
	if (S == "stop_quietest")                        return EMaxConcurrentResolutionRule::StopQuietest;
	if (S == "stop_lowest_priority_then_prevent_new") return EMaxConcurrentResolutionRule::StopLowestPriorityThenPreventNew;
	return EMaxConcurrentResolutionRule::StopFarthestThenOldest; // default
}

static const char* VolumeScaleModeToString(EConcurrencyVolumeScaleMode Mode)
{
	switch (Mode)
	{
	case EConcurrencyVolumeScaleMode::Default:  return "default";
	case EConcurrencyVolumeScaleMode::Distance: return "distance";
	case EConcurrencyVolumeScaleMode::Priority: return "priority";
	default:                                    return "default";
	}
}

static EConcurrencyVolumeScaleMode StringToVolumeScaleMode(const std::string& S)
{
	if (S == "distance") return EConcurrencyVolumeScaleMode::Distance;
	if (S == "priority") return EConcurrencyVolumeScaleMode::Priority;
	return EConcurrencyVolumeScaleMode::Default;
}

// ============================================================================
// BINDING
// ============================================================================

static TArray<FLuaFunctionDoc> SoundConcurrencyDocs = {};

static void BindSoundConcurrency(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_sound_concurrency", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		USoundConcurrency* Asset = LoadObject<USoundConcurrency>(nullptr, *FPath);
		if (!Asset) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"SoundConcurrency enrichment methods:\n"
			"\n"
			"info() — structured summary:\n"
			"  max_count, enable_platform_scaling, resolution_rule, retrigger_time,\n"
			"  limit_to_owner, volume_scale, volume_scale_mode, volume_scale_attack_time,\n"
			"  volume_scale_can_release, volume_scale_release_time,\n"
			"  voice_steal_release_time, is_eviction_supported\n"
			"\n"
			"configure(params) — set concurrency settings:\n"
			"  max_count (int>=1), enable_platform_scaling (bool),\n"
			"  resolution_rule (string: prevent_new/stop_oldest/\n"
			"    stop_farthest_then_prevent_new/stop_farthest_then_oldest/\n"
			"    stop_lowest_priority/stop_quietest/stop_lowest_priority_then_prevent_new),\n"
			"  retrigger_time (float>=0), limit_to_owner (bool),\n"
			"  volume_scale (float 0.0-1.0, ducking factor per voice generation),\n"
			"  volume_scale_mode (string: default/distance/priority),\n"
			"  volume_scale_attack_time (float>=0),\n"
			"  volume_scale_can_release (bool),\n"
			"  volume_scale_release_time (float>=0),\n"
			"  voice_steal_release_time (float>=0)\n";

		// ================================================================
		// info()
		// ================================================================
		AssetObj.set_function("info", [Asset, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(Asset))
			{
				Session.Log(TEXT("[FAIL] info -> asset no longer valid"));
				return sol::lua_nil;
			}

			const FSoundConcurrencySettings& C = Asset->Concurrency;
			sol::table R = Lua.create_table();

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
			R["max_count"] = C.GetMaxCount();
			R["enable_platform_scaling"] = C.IsMaxCountPlatformScalingEnabled();
#else
			R["max_count"] = C.MaxCount;
			R["enable_platform_scaling"] = false;
#endif
			R["resolution_rule"] = ResolutionRuleToString(C.ResolutionRule);
			R["retrigger_time"] = C.RetriggerTime;
			R["limit_to_owner"] = static_cast<bool>(C.bLimitToOwner);
			R["volume_scale"] = C.GetVolumeScale();
			R["volume_scale_mode"] = VolumeScaleModeToString(C.VolumeScaleMode);
			R["volume_scale_attack_time"] = C.VolumeScaleAttackTime;
			R["volume_scale_can_release"] = static_cast<bool>(C.bVolumeScaleCanRelease);
			R["volume_scale_release_time"] = C.VolumeScaleReleaseTime;
			R["voice_steal_release_time"] = C.VoiceStealReleaseTime;
			R["is_eviction_supported"] = C.IsEvictionSupported();

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
			Session.Log(FString::Printf(TEXT("[OK] info() -> SoundConcurrency, max_count=%d, rule=%s"),
				C.GetMaxCount(),
				UTF8_TO_TCHAR(ResolutionRuleToString(C.ResolutionRule))));
#else
			Session.Log(FString::Printf(TEXT("[OK] info() -> SoundConcurrency, max_count=%d, rule=%s"),
				C.MaxCount,
				UTF8_TO_TCHAR(ResolutionRuleToString(C.ResolutionRule))));
#endif
			return R;
		});

		// ================================================================
		// configure(params)
		// ================================================================
		AssetObj.set_function("configure", [Asset, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(Asset))
			{
				Session.Log(TEXT("[FAIL] configure -> asset no longer valid"));
				return sol::lua_nil;
			}

			const FScopedTransaction Transaction(FText::FromString(TEXT("SoundConcurrency: Configure")));
			Asset->Modify();
			FSoundConcurrencySettings& C = Asset->Concurrency;
			bool bModified = false;
			FString Changes;

			// enable_platform_scaling (must be processed before max_count)
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
			sol::optional<bool> PlatformScaling = Params.get<sol::optional<bool>>("enable_platform_scaling");
			if (PlatformScaling.has_value())
			{
				C.SetEnableMaxCountPlatformScaling(PlatformScaling.value());
				Changes += FString::Printf(TEXT(" enable_platform_scaling=%s"), PlatformScaling.value() ? TEXT("true") : TEXT("false"));
				bModified = true;
			}
#endif

			// max_count
			sol::optional<int> MaxCount = Params.get<sol::optional<int>>("max_count");
			if (MaxCount.has_value())
			{
				const int32 ClampedCount = FMath::Max(1, MaxCount.value());
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
				if (C.IsMaxCountPlatformScalingEnabled())
				{
					// Platform scaling is active — disable it so dynamic MaxCount can be set
					C.SetEnableMaxCountPlatformScaling(false);
					Changes += TEXT(" enable_platform_scaling=false(auto)");
				}
				C.SetMaxCount(ClampedCount);
				Changes += FString::Printf(TEXT(" max_count=%d"), C.GetMaxCount());
#else
				C.MaxCount = ClampedCount;
				Changes += FString::Printf(TEXT(" max_count=%d"), C.MaxCount);
#endif
				bModified = true;
			}

			// resolution_rule
			sol::optional<std::string> Rule = Params.get<sol::optional<std::string>>("resolution_rule");
			if (Rule.has_value())
			{
				C.ResolutionRule = StringToResolutionRule(Rule.value());
				Changes += FString::Printf(TEXT(" resolution_rule=%s"), UTF8_TO_TCHAR(Rule.value().c_str()));
				bModified = true;
			}

			// retrigger_time
			sol::optional<double> Retrigger = Params.get<sol::optional<double>>("retrigger_time");
			if (Retrigger.has_value())
			{
				C.RetriggerTime = FMath::Max(0.0f, static_cast<float>(Retrigger.value()));
				Changes += FString::Printf(TEXT(" retrigger_time=%.2f"), (double)C.RetriggerTime);
				bModified = true;
			}

			// limit_to_owner
			sol::optional<bool> LimitOwner = Params.get<sol::optional<bool>>("limit_to_owner");
			if (LimitOwner.has_value())
			{
				C.bLimitToOwner = LimitOwner.value();
				Changes += FString::Printf(TEXT(" limit_to_owner=%s"), LimitOwner.value() ? TEXT("true") : TEXT("false"));
				bModified = true;
			}

			// volume_scale (private UPROPERTY — set via reflection)
			sol::optional<double> VolScale = Params.get<sol::optional<double>>("volume_scale");
			if (VolScale.has_value())
			{
				const float Clamped = FMath::Clamp(static_cast<float>(VolScale.value()), 0.0f, 1.0f);
				FProperty* VolScaleProp = FSoundConcurrencySettings::StaticStruct()->FindPropertyByName(TEXT("VolumeScale"));
				if (VolScaleProp)
				{
					float* ValPtr = VolScaleProp->ContainerPtrToValuePtr<float>(&C);
					*ValPtr = Clamped;
					Changes += FString::Printf(TEXT(" volume_scale=%.3f"), Clamped);
					bModified = true;
				}
			}

			// volume_scale_mode
			sol::optional<std::string> VSMode = Params.get<sol::optional<std::string>>("volume_scale_mode");
			if (VSMode.has_value())
			{
				C.VolumeScaleMode = StringToVolumeScaleMode(VSMode.value());
				Changes += FString::Printf(TEXT(" volume_scale_mode=%s"), UTF8_TO_TCHAR(VSMode.value().c_str()));
				bModified = true;
			}

			// volume_scale_attack_time
			sol::optional<double> AttackTime = Params.get<sol::optional<double>>("volume_scale_attack_time");
			if (AttackTime.has_value())
			{
				C.VolumeScaleAttackTime = FMath::Max(0.0f, static_cast<float>(AttackTime.value()));
				Changes += FString::Printf(TEXT(" volume_scale_attack_time=%.3f"), (double)C.VolumeScaleAttackTime);
				bModified = true;
			}

			// volume_scale_can_release
			sol::optional<bool> CanRelease = Params.get<sol::optional<bool>>("volume_scale_can_release");
			if (CanRelease.has_value())
			{
				C.bVolumeScaleCanRelease = CanRelease.value();
				Changes += FString::Printf(TEXT(" volume_scale_can_release=%s"), CanRelease.value() ? TEXT("true") : TEXT("false"));
				bModified = true;
			}

			// volume_scale_release_time
			sol::optional<double> ReleaseTime = Params.get<sol::optional<double>>("volume_scale_release_time");
			if (ReleaseTime.has_value())
			{
				C.VolumeScaleReleaseTime = FMath::Max(0.0f, static_cast<float>(ReleaseTime.value()));
				Changes += FString::Printf(TEXT(" volume_scale_release_time=%.3f"), (double)C.VolumeScaleReleaseTime);
				bModified = true;
			}

			// voice_steal_release_time
			sol::optional<double> StealRelease = Params.get<sol::optional<double>>("voice_steal_release_time");
			if (StealRelease.has_value())
			{
				C.VoiceStealReleaseTime = FMath::Max(0.0f, static_cast<float>(StealRelease.value()));
				Changes += FString::Printf(TEXT(" voice_steal_release_time=%.3f"), (double)C.VoiceStealReleaseTime);
				bModified = true;
			}

			if (bModified)
			{
				FProperty* ConcurrencyProp = USoundConcurrency::StaticClass()->FindPropertyByName(TEXT("Concurrency"));
				FPropertyChangedEvent Event(ConcurrencyProp, EPropertyChangeType::ValueSet);
				Asset->PostEditChangeProperty(Event);
				Asset->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(%s)"), *Changes.TrimStart()));
				return sol::make_object(Lua, true);
			}

			Session.Log(TEXT("[OK] configure() -> nothing changed. Check _help_text for valid keys."));
			return sol::make_object(Lua, true);
		});
	});
}

REGISTER_LUA_BINDING(SoundConcurrency, SoundConcurrencyDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindSoundConcurrency(Lua, Session);
});
