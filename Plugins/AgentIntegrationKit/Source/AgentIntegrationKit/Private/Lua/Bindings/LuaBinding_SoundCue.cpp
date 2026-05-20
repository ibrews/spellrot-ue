// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Tools/NeoStackToolUtils.h"
#include "ScopedTransaction.h"

#include "Sound/SoundCue.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundNode.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundConcurrency.h"
#include "Sound/SoundSubmix.h"
#include "Sound/SoundNodeWavePlayer.h"
#include "Sound/SoundEffectSource.h"
#include "Sound/SoundSourceBusSend.h"
#include "Sound/SoundSubmixSend.h"
#include "Sound/SoundSourceBus.h"
#include "Sound/AudioBus.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// HELPERS
// ============================================================================

static int32 GetSoundCueNodeCount(const USoundCue* Cue)
{
#if WITH_EDITORONLY_DATA
	return Cue ? Cue->AllNodes.Num() : 0;
#else
	(void)Cue;
	return 0;
#endif
}

static bool HasSoundCueGraph(const USoundCue* Cue)
{
#if WITH_EDITORONLY_DATA
	return Cue && Cue->SoundCueGraph != nullptr;
#else
	(void)Cue;
	return false;
#endif
}

static const char* VirtualizationModeToString(EVirtualizationMode Mode)
{
	switch (Mode)
	{
	case EVirtualizationMode::Disabled:        return "disabled";
	case EVirtualizationMode::PlayWhenSilent:  return "play_when_silent";
	case EVirtualizationMode::Restart:         return "restart";
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
PRAGMA_DISABLE_DEPRECATION_WARNINGS
	case EVirtualizationMode::SeekRestart:     return "seek_restart";
PRAGMA_ENABLE_DEPRECATION_WARNINGS
#endif
	default:                                   return "disabled";
	}
}

namespace SoundCueHelpers {
static const char* AttenuationShapeToString(EAttenuationShape::Type Shape)
{
	switch (Shape)
	{
	case EAttenuationShape::Sphere:  return "sphere";
	case EAttenuationShape::Capsule: return "capsule";
	case EAttenuationShape::Box:     return "box";
	case EAttenuationShape::Cone:    return "cone";
	default:                         return "sphere";
	}
}
} // namespace SoundCueHelpers

static bool TrySetSeekRestartMode(USoundCue* Cue, const FString& ModeStr)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	if (ModeStr.Equals(TEXT("seek_restart"), ESearchCase::IgnoreCase) || ModeStr.Equals(TEXT("SeekRestart"), ESearchCase::IgnoreCase))
	{
PRAGMA_DISABLE_DEPRECATION_WARNINGS
		Cue->VirtualizationMode = EVirtualizationMode::SeekRestart;
PRAGMA_ENABLE_DEPRECATION_WARNINGS
		return true;
	}
#endif
	return false;
}

static const char* DistanceAlgorithmToString(EAttenuationDistanceModel Algo)
{
	switch (Algo)
	{
	case EAttenuationDistanceModel::Linear:       return "linear";
	case EAttenuationDistanceModel::Logarithmic:  return "logarithmic";
	case EAttenuationDistanceModel::Inverse:      return "inverse";
	case EAttenuationDistanceModel::LogReverse:   return "log_reverse";
	case EAttenuationDistanceModel::NaturalSound: return "natural_sound";
	case EAttenuationDistanceModel::Custom:       return "custom";
	default:                                      return "linear";
	}
}

static const char* SpatializationToString(ESoundSpatializationAlgorithm Algo)
{
	switch (Algo)
	{
	case SPATIALIZATION_Default: return "default";
	case SPATIALIZATION_HRTF:    return "hrtf";
	default:                     return "default";
	}
}

static sol::table BuildAttenuationTable(sol::state_view& Lua, const FSoundAttenuationSettings& Atten)
{
	sol::table AttenT = Lua.create_table();

	AttenT["attenuate"] = static_cast<bool>(Atten.bAttenuate);
	AttenT["spatialize"] = static_cast<bool>(Atten.bSpatialize);
	AttenT["shape"] = SoundCueHelpers::AttenuationShapeToString(Atten.AttenuationShape);

	sol::table Extents = Lua.create_table();
	Extents["x"] = Atten.AttenuationShapeExtents.X;
	Extents["y"] = Atten.AttenuationShapeExtents.Y;
	Extents["z"] = Atten.AttenuationShapeExtents.Z;
	AttenT["shape_extents"] = Extents;

	AttenT["falloff_distance"] = Atten.FalloffDistance;
	AttenT["db_attenuation_at_max"] = Atten.dBAttenuationAtMax;
	AttenT["distance_algorithm"] = DistanceAlgorithmToString(Atten.DistanceAlgorithm);

	AttenT["lpf_enabled"] = static_cast<bool>(Atten.bAttenuateWithLPF);
	AttenT["lpf_radius_min"] = Atten.LPFRadiusMin;
	AttenT["lpf_radius_max"] = Atten.LPFRadiusMax;
	AttenT["lpf_freq_min"] = Atten.LPFFrequencyAtMin;
	AttenT["lpf_freq_max"] = Atten.LPFFrequencyAtMax;
	AttenT["hpf_freq_min"] = Atten.HPFFrequencyAtMin;
	AttenT["hpf_freq_max"] = Atten.HPFFrequencyAtMax;

	AttenT["occlusion"] = static_cast<bool>(Atten.bEnableOcclusion);
	AttenT["reverb_send"] = static_cast<bool>(Atten.bEnableReverbSend);
	AttenT["spatialization_algorithm"] = SpatializationToString(Atten.SpatializationAlgorithm);

	return AttenT;
}

static bool BuildSoundCueNodeList(const USoundCue* Cue, sol::state_view& Lua, sol::table& Result, int32& OutCount)
{
#if WITH_EDITORONLY_DATA
	OutCount = Cue ? Cue->AllNodes.Num() : 0;
	if (!Cue)
	{
		return false;
	}

	for (int32 i = 0; i < Cue->AllNodes.Num(); i++)
	{
		USoundNode* Node = Cue->AllNodes[i];
		if (!Node) continue;

		sol::table E = Lua.create_table();
		E["index"] = i + 1;
		E["class"] = TCHAR_TO_UTF8(*Node->GetClass()->GetName());
		E["name"] = TCHAR_TO_UTF8(*Node->GetName());
		E["child_count"] = static_cast<int>(Node->ChildNodes.Num());

		if (USoundNodeWavePlayer* WavePlayer = Cast<USoundNodeWavePlayer>(Node))
		{
			USoundWave* Wave = WavePlayer->GetSoundWave();
			if (Wave)
			{
				E["sound_wave"] = TCHAR_TO_UTF8(*Wave->GetPathName());
				E["sound_wave_name"] = TCHAR_TO_UTF8(*Wave->GetName());
			}
			E["is_looping"] = static_cast<bool>(WavePlayer->bLooping);
		}

		if (Node->ChildNodes.Num() > 0)
		{
			sol::table ChildIndices = Lua.create_table();
			int32 ChildIdx = 1;
			for (USoundNode* ChildNode : Node->ChildNodes)
			{
				if (!ChildNode) continue;
				const int32 ChildAllNodesIndex = Cue->AllNodes.IndexOfByKey(ChildNode);
				if (ChildAllNodesIndex != INDEX_NONE)
				{
					ChildIndices[ChildIdx++] = ChildAllNodesIndex + 1;
				}
			}
			E["children"] = ChildIndices;
		}

		Result[i + 1] = E;
	}

	return true;
#else
	(void)Cue;
	(void)Lua;
	(void)Result;
	OutCount = 0;
	return false;
#endif
}

static const char* SubmixSendStageToString(ESubmixSendStage Stage)
{
	switch (Stage)
	{
	case ESubmixSendStage::PostDistanceAttenuation: return "post_distance_attenuation";
	case ESubmixSendStage::PreDistanceAttenuation:  return "pre_distance_attenuation";
	default:                                         return "post_distance_attenuation";
	}
}

static const char* SendLevelControlToString(ESendLevelControlMethod Method)
{
	switch (Method)
	{
	case ESendLevelControlMethod::Linear:      return "linear";
	case ESendLevelControlMethod::CustomCurve: return "custom_curve";
	case ESendLevelControlMethod::Manual:      return "manual";
	default:                                    return "manual";
	}
}

static const char* BusSendControlToString(ESourceBusSendLevelControlMethod Method)
{
	switch (Method)
	{
	case ESourceBusSendLevelControlMethod::Linear:      return "linear";
	case ESourceBusSendLevelControlMethod::CustomCurve: return "custom_curve";
	case ESourceBusSendLevelControlMethod::Manual:      return "manual";
	default:                                             return "manual";
	}
}

// ============================================================================
// BINDING
// ============================================================================

static TArray<FLuaFunctionDoc> SoundCueDocs = {};

static void BindSoundCue(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_sound_cue", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		USoundCue* Cue = LoadObject<USoundCue>(nullptr, *FPath);
		if (!Cue) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"SoundCue enrichment methods:\n"
			"\n"
			"info() — structured summary:\n"
			"  volume, pitch, priority, virtualization_mode, sound_submix,\n"
			"  is_looping, duration, max_distance, sound_class,\n"
			"  node_count, first_node_class, override_attenuation, subtitle_priority, has_graph,\n"
			"  attenuation_settings, source_effect_chain, enable_bus_sends, enable_base_submix,\n"
			"  enable_submix_sends, bypass_volume_scale_for_priority, concurrency_count,\n"
			"  override_concurrency, max_concurrent_count\n"
			"  When override_attenuation=true, includes attenuation_overrides table:\n"
			"    attenuate, spatialize, shape, shape_extents, falloff_distance, db_attenuation_at_max,\n"
			"    distance_algorithm, lpf_enabled, lpf_radius_min/max, lpf_freq_min/max,\n"
			"    hpf_freq_min/max, occlusion, reverb_send, spatialization_algorithm\n"
			"\n"
			"list(type):\n"
			"  list(\"nodes\") — all sound nodes: {index, class, name, child_count, children, sound_wave}\n"
			"  list(\"concurrency\") — concurrency set: {name, path}\n"
			"  list(\"submix_sends\") — submix sends: {submix, send_level, control_method, stage, ...}\n"
			"  list(\"bus_sends\") — bus sends (post-effect): {bus, send_level, control_method, ...}\n"
			"  list(\"pre_effect_bus_sends\") — bus sends (pre-effect)\n"
			"\n"
			"configure(params):\n"
			"  configure({volume=1.5, pitch=0.8, override_attenuation=true, prime_on_load=true,\n"
			"             exclude_random_culling=true, sound_class=\"/Path/To/SoundClass\",\n"
			"             override_concurrency=true, max_concurrent_count=4,\n"
			"             virtualization_mode=\"disabled|play_when_silent|restart|seek_restart\",\n"
			"             attenuation_settings=\"/Path/To/SoundAttenuation\",\n"
			"             source_effect_chain=\"/Path/To/Chain\",\n"
			"             enable_bus_sends=true, enable_base_submix=true, enable_submix_sends=true,\n"
			"             bypass_volume_scale_for_priority=false,\n"
			"             concurrency_set={\"/Path/To/ConcurrencyAsset\"},\n"
			"             attenuation={spatialize=true, attenuate=true, falloff_distance=2000,\n"
			"               shape=\"sphere\", shape_extents={x=400}, db_attenuation_at_max=-60,\n"
			"               distance_algorithm=\"linear\", lpf_enabled=true,\n"
			"               lpf_radius_min=500, lpf_radius_max=3000,\n"
			"               lpf_freq_min=20000, lpf_freq_max=20000,\n"
			"               hpf_freq_min=0, hpf_freq_max=0,\n"
			"               occlusion=false, reverb_send=false,\n"
			"               spatialization=\"default\"}})\n"
			"\n"
			"Graph editing:\n"
			"  Use read_graph() + graph editing tools (add_node, connect, etc.)\n"
			"  SoundCue graphs are now supported by the graph resolver.\n";

		// ================================================================
		// info()
		// ================================================================
		AssetObj.set_function("info", [Cue, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(Cue))
			{
				Session.Log(TEXT("[FAIL] info -> asset no longer valid"));
				return sol::lua_nil;
			}

			sol::table Result = Lua.create_table();

			Result["volume"] = Cue->VolumeMultiplier;
			Result["pitch"] = Cue->PitchMultiplier;
			Result["is_looping"] = Cue->IsLooping();
			Result["duration"] = Cue->GetDuration();
			Result["max_distance"] = Cue->GetMaxDistance();

			// Sound class (on USoundBase)
			USoundClass* SoundClass = Cue->SoundClassObject;
			Result["sound_class"] = SoundClass ? TCHAR_TO_UTF8(*SoundClass->GetPathName()) : "None";

			// Node info (editor-only)
			Result["node_count"] = GetSoundCueNodeCount(Cue);
			Result["has_graph"] = HasSoundCueGraph(Cue);

			// FirstNode (not editor-only)
			USoundNode* First = Cue->FirstNode;
			Result["first_node_class"] = First ? TCHAR_TO_UTF8(*First->GetClass()->GetName()) : "None";

			Result["override_attenuation"] = static_cast<bool>(Cue->bOverrideAttenuation);

			// Attenuation overrides detail (only when override is enabled)
			if (Cue->bOverrideAttenuation)
			{
				Result["attenuation_overrides"] = BuildAttenuationTable(Lua, Cue->AttenuationOverrides);
			}

			// Attenuation settings asset (when NOT overriding)
			if (Cue->AttenuationSettings)
			{
				Result["attenuation_settings"] = TCHAR_TO_UTF8(*Cue->AttenuationSettings->GetPathName());
			}

			// SubtitlePriority is protected — use the public getter
			Result["subtitle_priority"] = Cue->GetSubtitlePriority();

			Result["prime_on_load"] = static_cast<bool>(Cue->bPrimeOnLoad);

			// Priority (from USoundBase, 0-100)
			Result["priority"] = Cue->Priority;

			// VirtualizationMode (includes SeekRestart for 5.7)
			Result["virtualization_mode"] = VirtualizationModeToString(Cue->VirtualizationMode);

			// Sound submix
			if (Cue->SoundSubmixObject)
				Result["sound_submix"] = TCHAR_TO_UTF8(*Cue->SoundSubmixObject->GetPathName());

			// Source effect chain
			if (Cue->SourceEffectChain)
				Result["source_effect_chain"] = TCHAR_TO_UTF8(*Cue->SourceEffectChain->GetPathName());

			// Enable flags (from USoundBase)
			Result["enable_bus_sends"] = static_cast<bool>(Cue->bEnableBusSends);
			Result["enable_base_submix"] = static_cast<bool>(Cue->bEnableBaseSubmix);
			Result["enable_submix_sends"] = static_cast<bool>(Cue->bEnableSubmixSends);
			Result["bypass_volume_scale_for_priority"] = static_cast<bool>(Cue->bBypassVolumeScaleForPriority);

			// Concurrency
			Result["override_concurrency"] = static_cast<bool>(Cue->bOverrideConcurrency);
			if (Cue->bOverrideConcurrency)
			{
				Result["max_concurrent_count"] = Cue->ConcurrencyOverrides.MaxCount;
			}
			Result["concurrency_count"] = Cue->ConcurrencySet.Num();

			// Send counts
			Result["submix_send_count"] = Cue->SoundSubmixSends.Num();
			Result["bus_send_count"] = Cue->BusSends.Num();
			Result["pre_effect_bus_send_count"] = Cue->PreEffectBusSends.Num();

			const int32 NodeCount = GetSoundCueNodeCount(Cue);
			Session.Log(FString::Printf(TEXT("[OK] info() -> SoundCue, %d nodes, vol=%.2f, pitch=%.2f"),
				NodeCount, (double)Cue->VolumeMultiplier, (double)Cue->PitchMultiplier));
			return Result;
		});

		// ================================================================
		// list(type?)
		// ================================================================
		AssetObj.set_function("list", [Cue, &Session](sol::table Self,
			sol::optional<std::string> TypeOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = TypeOpt.has_value() ? UTF8_TO_TCHAR(TypeOpt.value().c_str()) : TEXT("all");

			if (FType.Equals(TEXT("all"), ESearchCase::IgnoreCase))
			{
				sol::protected_function InfoFn = Self["info"];
				if (InfoFn.valid()) return InfoFn(Self);
				return sol::lua_nil;
			}

			if (!IsValid(Cue))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> asset no longer valid"), *FType));
				return sol::lua_nil;
			}

			// ---- nodes ----
			if (FType.Equals(TEXT("nodes"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("node"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				int32 NodeCount = 0;
				if (!BuildSoundCueNodeList(Cue, Lua, Result, NodeCount))
				{
					Session.Log(TEXT("[FAIL] list(\"nodes\") -> not available in non-editor builds"));
					return sol::lua_nil;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"nodes\") -> %d"), NodeCount));
				return Result;
			}

			// ---- concurrency ----
			if (FType.Equals(TEXT("concurrency"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				int32 Idx = 1;
				for (const TObjectPtr<USoundConcurrency>& Conc : Cue->ConcurrencySet)
				{
					if (!Conc) continue;
					sol::table E = Lua.create_table();
					E["name"] = TCHAR_TO_UTF8(*Conc->GetName());
					E["path"] = TCHAR_TO_UTF8(*Conc->GetPathName());
	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
				E["max_count"] = Conc->Concurrency.GetMaxCount();
#else
				E["max_count"] = Conc->Concurrency.MaxCount;
#endif
					Result[Idx++] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"concurrency\") -> %d"), Idx - 1));
				return Result;
			}

			// ---- submix_sends ----
			if (FType.Equals(TEXT("submix_sends"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Cue->SoundSubmixSends.Num(); i++)
				{
					const FSoundSubmixSendInfo& Send = Cue->SoundSubmixSends[i];
					sol::table E = Lua.create_table();
					E["index"] = i + 1;
					E["submix"] = Send.SoundSubmix ? TCHAR_TO_UTF8(*Send.SoundSubmix->GetPathName()) : "None";
					E["send_level"] = Send.SendLevel;
					E["control_method"] = SendLevelControlToString(Send.SendLevelControlMethod);
					E["stage"] = SubmixSendStageToString(Send.SendStage);
					E["min_send_level"] = Send.MinSendLevel;
					E["max_send_level"] = Send.MaxSendLevel;
					E["min_send_distance"] = Send.MinSendDistance;
					E["max_send_distance"] = Send.MaxSendDistance;
					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"submix_sends\") -> %d"), Cue->SoundSubmixSends.Num()));
				return Result;
			}

			// ---- bus_sends / pre_effect_bus_sends ----
			if (FType.Equals(TEXT("bus_sends"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("pre_effect_bus_sends"), ESearchCase::IgnoreCase))
			{
				const bool bPreEffect = FType.Equals(TEXT("pre_effect_bus_sends"), ESearchCase::IgnoreCase);
				const TArray<FSoundSourceBusSendInfo>& Sends = bPreEffect ? Cue->PreEffectBusSends : Cue->BusSends;

				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Sends.Num(); i++)
				{
					const FSoundSourceBusSendInfo& Send = Sends[i];
					sol::table E = Lua.create_table();
					E["index"] = i + 1;
					if (Send.SoundSourceBus)
						E["source_bus"] = TCHAR_TO_UTF8(*Send.SoundSourceBus->GetPathName());
					if (Send.AudioBus)
						E["audio_bus"] = TCHAR_TO_UTF8(*Send.AudioBus->GetPathName());
					E["send_level"] = Send.SendLevel;
					E["control_method"] = BusSendControlToString(Send.SourceBusSendLevelControlMethod);
					E["min_send_level"] = Send.MinSendLevel;
					E["max_send_level"] = Send.MaxSendLevel;
					E["min_send_distance"] = Send.MinSendDistance;
					E["max_send_distance"] = Send.MaxSendDistance;
					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"%s\") -> %d"), *FType, Sends.Num()));
				return Result;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: nodes, concurrency, submix_sends, bus_sends, pre_effect_bus_sends"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// configure(params)
		// ================================================================
		AssetObj.set_function("configure", [Cue, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(Cue))
			{
				Session.Log(TEXT("[FAIL] configure -> asset no longer valid"));
				return sol::lua_nil;
			}

			const FScopedTransaction Transaction(FText::FromString(TEXT("SoundCue: Configure")));
			Cue->Modify();
			bool bModified = false;
			FString Changes;

			sol::optional<double> Volume = Params.get<sol::optional<double>>("volume");
			if (Volume.has_value())
			{
				Cue->VolumeMultiplier = static_cast<float>(Volume.value());
				Changes += FString::Printf(TEXT(" volume=%.2f"), (double)Cue->VolumeMultiplier);
				bModified = true;
			}

			sol::optional<double> Pitch = Params.get<sol::optional<double>>("pitch");
			if (Pitch.has_value())
			{
				Cue->PitchMultiplier = static_cast<float>(Pitch.value());
				Changes += FString::Printf(TEXT(" pitch=%.2f"), (double)Cue->PitchMultiplier);
				bModified = true;
			}

			sol::optional<bool> OverrideAtten = Params.get<sol::optional<bool>>("override_attenuation");
			if (OverrideAtten.has_value())
			{
				Cue->bOverrideAttenuation = OverrideAtten.value();
				Changes += FString::Printf(TEXT(" override_attenuation=%s"), OverrideAtten.value() ? TEXT("true") : TEXT("false"));
				bModified = true;
			}

			sol::optional<bool> PrimeOnLoad = Params.get<sol::optional<bool>>("prime_on_load");
			if (PrimeOnLoad.has_value())
			{
				Cue->bPrimeOnLoad = PrimeOnLoad.value();
				Changes += FString::Printf(TEXT(" prime_on_load=%s"), PrimeOnLoad.value() ? TEXT("true") : TEXT("false"));
				bModified = true;
			}

			sol::optional<bool> ExcludeRandomCulling = Params.get<sol::optional<bool>>("exclude_random_culling");
			if (ExcludeRandomCulling.has_value())
			{
				Cue->bExcludeFromRandomNodeBranchCulling = ExcludeRandomCulling.value();
				Changes += FString::Printf(TEXT(" exclude_random_culling=%s"), ExcludeRandomCulling.value() ? TEXT("true") : TEXT("false"));
				bModified = true;
			}

			// Sound class assignment
			std::string SoundClassStr = Params.get_or<std::string>("sound_class", "");
			if (!SoundClassStr.empty())
			{
				FString SoundClassPath = UTF8_TO_TCHAR(SoundClassStr.c_str());
				if (SoundClassPath.Equals(TEXT("none"), ESearchCase::IgnoreCase))
				{
					Cue->SoundClassObject = nullptr;
					Changes += TEXT(" sound_class=none");
					bModified = true;
				}
				else
				{
					USoundClass* NewSoundClass = LoadObject<USoundClass>(nullptr, *SoundClassPath);
					if (NewSoundClass)
					{
						Cue->SoundClassObject = NewSoundClass;
						Changes += FString::Printf(TEXT(" sound_class=%s"), *NewSoundClass->GetName());
						bModified = true;
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[WARN] configure -> sound_class '%s' not found"), *SoundClassPath));
					}
				}
			}

			// Priority (0-100)
			sol::optional<double> Priority = Params.get<sol::optional<double>>("priority");
			if (Priority.has_value())
			{
				Cue->Priority = FMath::Clamp(static_cast<float>(Priority.value()), 0.0f, 100.0f);
				Changes += FString::Printf(TEXT(" priority=%.1f"), (double)Cue->Priority);
				bModified = true;
			}

			// VirtualizationMode
			sol::optional<std::string> VirtMode = Params.get<sol::optional<std::string>>("virtualization_mode");
			if (VirtMode.has_value())
			{
				FString ModeStr = UTF8_TO_TCHAR(VirtMode.value().c_str());
				bool bRecognized = true;
				if (ModeStr.Equals(TEXT("disabled"), ESearchCase::IgnoreCase))
					Cue->VirtualizationMode = EVirtualizationMode::Disabled;
				else if (ModeStr.Equals(TEXT("play_when_silent"), ESearchCase::IgnoreCase) || ModeStr.Equals(TEXT("PlayWhenSilent"), ESearchCase::IgnoreCase))
					Cue->VirtualizationMode = EVirtualizationMode::PlayWhenSilent;
				else if (ModeStr.Equals(TEXT("restart"), ESearchCase::IgnoreCase))
					Cue->VirtualizationMode = EVirtualizationMode::Restart;
				else if (TrySetSeekRestartMode(Cue, ModeStr))
				{
				}
				else
				{
					bRecognized = false;
					Session.Log(FString::Printf(TEXT("[WARN] configure -> virtualization_mode '%s' not recognized. Valid: disabled, play_when_silent, restart, seek_restart"), *ModeStr));
				}
				if (bRecognized)
				{
					Changes += FString::Printf(TEXT(" virtualization_mode=%s"), *ModeStr);
					bModified = true;
				}
			}

			// Sound submix
			std::string SubmixStr = Params.get_or<std::string>("sound_submix", "");
			if (!SubmixStr.empty())
			{
				FString SubmixPath = UTF8_TO_TCHAR(SubmixStr.c_str());
				if (SubmixPath.Equals(TEXT("none"), ESearchCase::IgnoreCase))
				{
					Cue->SoundSubmixObject = nullptr;
					Changes += TEXT(" sound_submix=none");
					bModified = true;
				}
				else
				{
					USoundSubmixBase* Submix = LoadObject<USoundSubmixBase>(nullptr, *SubmixPath);
					if (Submix)
					{
						Cue->SoundSubmixObject = Submix;
						Changes += FString::Printf(TEXT(" sound_submix=%s"), *Submix->GetName());
						bModified = true;
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[WARN] configure -> sound_submix '%s' not found"), *SubmixPath));
					}
				}
			}

			// Override concurrency
			sol::optional<bool> OverrideConcurrency = Params.get<sol::optional<bool>>("override_concurrency");
			if (OverrideConcurrency.has_value())
			{
				Cue->bOverrideConcurrency = OverrideConcurrency.value();
				Changes += FString::Printf(TEXT(" override_concurrency=%s"), OverrideConcurrency.value() ? TEXT("true") : TEXT("false"));
				bModified = true;
			}

			// Max concurrent count (sets ConcurrencyOverrides.MaxCount)
			sol::optional<int> MaxConcurrentCount = Params.get<sol::optional<int>>("max_concurrent_count");
			if (MaxConcurrentCount.has_value())
			{
				Cue->ConcurrencyOverrides.MaxCount = FMath::Max(1, MaxConcurrentCount.value());
				Changes += FString::Printf(TEXT(" max_concurrent_count=%d"), Cue->ConcurrencyOverrides.MaxCount);
				bModified = true;
			}

			// Attenuation settings asset (USoundAttenuation reference)
			std::string AttenSettingsStr = Params.get_or<std::string>("attenuation_settings", "");
			if (!AttenSettingsStr.empty())
			{
				FString AttenPath = UTF8_TO_TCHAR(AttenSettingsStr.c_str());
				if (AttenPath.Equals(TEXT("none"), ESearchCase::IgnoreCase))
				{
					Cue->AttenuationSettings = nullptr;
					Changes += TEXT(" attenuation_settings=none");
					bModified = true;
				}
				else
				{
					USoundAttenuation* AttenAsset = LoadObject<USoundAttenuation>(nullptr, *AttenPath);
					if (AttenAsset)
					{
						Cue->AttenuationSettings = AttenAsset;
						Changes += FString::Printf(TEXT(" attenuation_settings=%s"), *AttenAsset->GetName());
						bModified = true;
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[WARN] configure -> attenuation_settings '%s' not found"), *AttenPath));
					}
				}
			}

			// Source effect chain
			std::string EffectChainStr = Params.get_or<std::string>("source_effect_chain", "");
			if (!EffectChainStr.empty())
			{
				FString ChainPath = UTF8_TO_TCHAR(EffectChainStr.c_str());
				if (ChainPath.Equals(TEXT("none"), ESearchCase::IgnoreCase))
				{
					Cue->SourceEffectChain = nullptr;
					Changes += TEXT(" source_effect_chain=none");
					bModified = true;
				}
				else
				{
					USoundEffectSourcePresetChain* Chain = LoadObject<USoundEffectSourcePresetChain>(nullptr, *ChainPath);
					if (Chain)
					{
						Cue->SourceEffectChain = Chain;
						Changes += FString::Printf(TEXT(" source_effect_chain=%s"), *Chain->GetName());
						bModified = true;
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[WARN] configure -> source_effect_chain '%s' not found"), *ChainPath));
					}
				}
			}

			// Enable flags (from USoundBase)
			sol::optional<bool> EnableBusSends = Params.get<sol::optional<bool>>("enable_bus_sends");
			if (EnableBusSends.has_value())
			{
				Cue->bEnableBusSends = EnableBusSends.value();
				Changes += FString::Printf(TEXT(" enable_bus_sends=%s"), EnableBusSends.value() ? TEXT("true") : TEXT("false"));
				bModified = true;
			}

			sol::optional<bool> EnableBaseSubmix = Params.get<sol::optional<bool>>("enable_base_submix");
			if (EnableBaseSubmix.has_value())
			{
				Cue->bEnableBaseSubmix = EnableBaseSubmix.value();
				Changes += FString::Printf(TEXT(" enable_base_submix=%s"), EnableBaseSubmix.value() ? TEXT("true") : TEXT("false"));
				bModified = true;
			}

			sol::optional<bool> EnableSubmixSends = Params.get<sol::optional<bool>>("enable_submix_sends");
			if (EnableSubmixSends.has_value())
			{
				Cue->bEnableSubmixSends = EnableSubmixSends.value();
				Changes += FString::Printf(TEXT(" enable_submix_sends=%s"), EnableSubmixSends.value() ? TEXT("true") : TEXT("false"));
				bModified = true;
			}

			sol::optional<bool> BypassVolPriority = Params.get<sol::optional<bool>>("bypass_volume_scale_for_priority");
			if (BypassVolPriority.has_value())
			{
				Cue->bBypassVolumeScaleForPriority = BypassVolPriority.value();
				Changes += FString::Printf(TEXT(" bypass_volume_scale_for_priority=%s"), BypassVolPriority.value() ? TEXT("true") : TEXT("false"));
				bModified = true;
			}

			// Concurrency set (array of paths — replaces entire set)
			sol::optional<sol::table> ConcSetOpt = Params.get<sol::optional<sol::table>>("concurrency_set");
			if (ConcSetOpt.has_value())
			{
				sol::table ConcArr = ConcSetOpt.value();
				TSet<TObjectPtr<USoundConcurrency>> NewSet;
				int32 LoadedCount = 0;

				for (const auto& Pair : ConcArr)
				{
					if (Pair.second.is<std::string>())
					{
						FString ConcPath = UTF8_TO_TCHAR(Pair.second.as<std::string>().c_str());
						USoundConcurrency* Conc = LoadObject<USoundConcurrency>(nullptr, *ConcPath);
						if (Conc)
						{
							NewSet.Add(Conc);
							LoadedCount++;
						}
						else
						{
							Session.Log(FString::Printf(TEXT("[WARN] configure -> concurrency_set: '%s' not found"), *ConcPath));
						}
					}
				}

				Cue->ConcurrencySet = MoveTemp(NewSet);
				Changes += FString::Printf(TEXT(" concurrency_set=%d"), LoadedCount);
				bModified = true;
			}

			// Attenuation overrides sub-table (accept both "attenuation" and "attenuation_overrides")
			sol::optional<sol::table> AttenOpt = Params.get<sol::optional<sol::table>>("attenuation");
			if (!AttenOpt.has_value())
				AttenOpt = Params.get<sol::optional<sol::table>>("attenuation_overrides");
			if (AttenOpt.has_value())
			{
				sol::table A = AttenOpt.value();
				FSoundAttenuationSettings& Atten = Cue->AttenuationOverrides;
				FString AttenChanges;

				sol::optional<bool> Attenuate = A.get<sol::optional<bool>>("attenuate");
				if (Attenuate.has_value())
				{
					Atten.bAttenuate = Attenuate.value();
					AttenChanges += FString::Printf(TEXT(" attenuate=%s"), Attenuate.value() ? TEXT("true") : TEXT("false"));
				}

				sol::optional<bool> Spatialize = A.get<sol::optional<bool>>("spatialize");
				if (Spatialize.has_value())
				{
					Atten.bSpatialize = Spatialize.value();
					AttenChanges += FString::Printf(TEXT(" spatialize=%s"), Spatialize.value() ? TEXT("true") : TEXT("false"));
				}

				sol::optional<std::string> ShapeStr = A.get<sol::optional<std::string>>("shape");
				if (ShapeStr.has_value())
				{
					FString ShapeVal = UTF8_TO_TCHAR(ShapeStr.value().c_str());
					if (ShapeVal.Equals(TEXT("sphere"), ESearchCase::IgnoreCase))
						Atten.AttenuationShape = EAttenuationShape::Sphere;
					else if (ShapeVal.Equals(TEXT("capsule"), ESearchCase::IgnoreCase))
						Atten.AttenuationShape = EAttenuationShape::Capsule;
					else if (ShapeVal.Equals(TEXT("box"), ESearchCase::IgnoreCase))
						Atten.AttenuationShape = EAttenuationShape::Box;
					else if (ShapeVal.Equals(TEXT("cone"), ESearchCase::IgnoreCase))
						Atten.AttenuationShape = EAttenuationShape::Cone;
					else
						Session.Log(FString::Printf(TEXT("[WARN] configure -> attenuation shape '%s' not recognized. Valid: sphere, capsule, box, cone"), *ShapeVal));
					AttenChanges += FString::Printf(TEXT(" shape=%s"), *ShapeVal);
				}

				sol::optional<sol::table> ExtentsOpt = A.get<sol::optional<sol::table>>("shape_extents");
				if (ExtentsOpt.has_value())
				{
					sol::table E = ExtentsOpt.value();
					sol::optional<double> Ex = E.get<sol::optional<double>>("x");
					sol::optional<double> Ey = E.get<sol::optional<double>>("y");
					sol::optional<double> Ez = E.get<sol::optional<double>>("z");
					if (Ex.has_value()) Atten.AttenuationShapeExtents.X = Ex.value();
					if (Ey.has_value()) Atten.AttenuationShapeExtents.Y = Ey.value();
					if (Ez.has_value()) Atten.AttenuationShapeExtents.Z = Ez.value();
					AttenChanges += FString::Printf(TEXT(" shape_extents=(%.0f,%.0f,%.0f)"),
						Atten.AttenuationShapeExtents.X, Atten.AttenuationShapeExtents.Y, Atten.AttenuationShapeExtents.Z);
				}

				sol::optional<double> FalloffDist = A.get<sol::optional<double>>("falloff_distance");
				if (FalloffDist.has_value())
				{
					Atten.FalloffDistance = FMath::Max(0.0f, static_cast<float>(FalloffDist.value()));
					AttenChanges += FString::Printf(TEXT(" falloff_distance=%.0f"), (double)Atten.FalloffDistance);
				}

				sol::optional<double> DbAtMax = A.get<sol::optional<double>>("db_attenuation_at_max");
				if (DbAtMax.has_value())
				{
					Atten.dBAttenuationAtMax = FMath::Clamp(static_cast<float>(DbAtMax.value()), -60.0f, 0.0f);
					AttenChanges += FString::Printf(TEXT(" db_attenuation_at_max=%.1f"), (double)Atten.dBAttenuationAtMax);
				}

				sol::optional<std::string> DistAlgo = A.get<sol::optional<std::string>>("distance_algorithm");
				if (DistAlgo.has_value())
				{
					FString DA = UTF8_TO_TCHAR(DistAlgo.value().c_str());
					if (DA.Equals(TEXT("linear"), ESearchCase::IgnoreCase))
						Atten.DistanceAlgorithm = EAttenuationDistanceModel::Linear;
					else if (DA.Equals(TEXT("logarithmic"), ESearchCase::IgnoreCase))
						Atten.DistanceAlgorithm = EAttenuationDistanceModel::Logarithmic;
					else if (DA.Equals(TEXT("inverse"), ESearchCase::IgnoreCase))
						Atten.DistanceAlgorithm = EAttenuationDistanceModel::Inverse;
					else if (DA.Equals(TEXT("log_reverse"), ESearchCase::IgnoreCase))
						Atten.DistanceAlgorithm = EAttenuationDistanceModel::LogReverse;
					else if (DA.Equals(TEXT("natural_sound"), ESearchCase::IgnoreCase))
						Atten.DistanceAlgorithm = EAttenuationDistanceModel::NaturalSound;
					else if (DA.Equals(TEXT("custom"), ESearchCase::IgnoreCase))
						Atten.DistanceAlgorithm = EAttenuationDistanceModel::Custom;
					else
						Session.Log(FString::Printf(TEXT("[WARN] configure -> distance_algorithm '%s' not recognized"), *DA));
					AttenChanges += FString::Printf(TEXT(" distance_algorithm=%s"), *DA);
				}

				sol::optional<bool> LpfEnabled = A.get<sol::optional<bool>>("lpf_enabled");
				if (LpfEnabled.has_value())
				{
					Atten.bAttenuateWithLPF = LpfEnabled.value();
					AttenChanges += FString::Printf(TEXT(" lpf_enabled=%s"), LpfEnabled.value() ? TEXT("true") : TEXT("false"));
				}

				sol::optional<double> LpfRadMin = A.get<sol::optional<double>>("lpf_radius_min");
				if (LpfRadMin.has_value())
				{
					Atten.LPFRadiusMin = static_cast<float>(LpfRadMin.value());
					AttenChanges += FString::Printf(TEXT(" lpf_radius_min=%.0f"), (double)Atten.LPFRadiusMin);
				}

				sol::optional<double> LpfRadMax = A.get<sol::optional<double>>("lpf_radius_max");
				if (LpfRadMax.has_value())
				{
					Atten.LPFRadiusMax = static_cast<float>(LpfRadMax.value());
					AttenChanges += FString::Printf(TEXT(" lpf_radius_max=%.0f"), (double)Atten.LPFRadiusMax);
				}

				sol::optional<double> LpfFreqMin = A.get<sol::optional<double>>("lpf_freq_min");
				if (LpfFreqMin.has_value())
				{
					Atten.LPFFrequencyAtMin = static_cast<float>(LpfFreqMin.value());
					AttenChanges += FString::Printf(TEXT(" lpf_freq_min=%.0f"), (double)Atten.LPFFrequencyAtMin);
				}

				sol::optional<double> LpfFreqMax = A.get<sol::optional<double>>("lpf_freq_max");
				if (LpfFreqMax.has_value())
				{
					Atten.LPFFrequencyAtMax = static_cast<float>(LpfFreqMax.value());
					AttenChanges += FString::Printf(TEXT(" lpf_freq_max=%.0f"), (double)Atten.LPFFrequencyAtMax);
				}

				sol::optional<double> HpfFreqMin = A.get<sol::optional<double>>("hpf_freq_min");
				if (HpfFreqMin.has_value())
				{
					Atten.HPFFrequencyAtMin = static_cast<float>(HpfFreqMin.value());
					AttenChanges += FString::Printf(TEXT(" hpf_freq_min=%.0f"), (double)Atten.HPFFrequencyAtMin);
				}

				sol::optional<double> HpfFreqMax = A.get<sol::optional<double>>("hpf_freq_max");
				if (HpfFreqMax.has_value())
				{
					Atten.HPFFrequencyAtMax = static_cast<float>(HpfFreqMax.value());
					AttenChanges += FString::Printf(TEXT(" hpf_freq_max=%.0f"), (double)Atten.HPFFrequencyAtMax);
				}

				sol::optional<bool> Occlusion = A.get<sol::optional<bool>>("occlusion");
				if (Occlusion.has_value())
				{
					Atten.bEnableOcclusion = Occlusion.value();
					AttenChanges += FString::Printf(TEXT(" occlusion=%s"), Occlusion.value() ? TEXT("true") : TEXT("false"));
				}

				sol::optional<bool> ReverbSend = A.get<sol::optional<bool>>("reverb_send");
				if (ReverbSend.has_value())
				{
					Atten.bEnableReverbSend = ReverbSend.value();
					AttenChanges += FString::Printf(TEXT(" reverb_send=%s"), ReverbSend.value() ? TEXT("true") : TEXT("false"));
				}

				sol::optional<std::string> SpatAlgo = A.get<sol::optional<std::string>>("spatialization");
				if (SpatAlgo.has_value())
				{
					FString SA = UTF8_TO_TCHAR(SpatAlgo.value().c_str());
					if (SA.Equals(TEXT("default"), ESearchCase::IgnoreCase) || SA.Equals(TEXT("panning"), ESearchCase::IgnoreCase))
						Atten.SpatializationAlgorithm = SPATIALIZATION_Default;
					else if (SA.Equals(TEXT("hrtf"), ESearchCase::IgnoreCase))
						Atten.SpatializationAlgorithm = SPATIALIZATION_HRTF;
					else
						Session.Log(FString::Printf(TEXT("[WARN] configure -> spatialization '%s' not recognized. Valid: default, panning, hrtf"), *SA));
					AttenChanges += FString::Printf(TEXT(" spatialization=%s"), *SA);
				}

				if (!AttenChanges.IsEmpty())
				{
					Changes += FString::Printf(TEXT(" attenuation={%s}"), *AttenChanges.TrimStart());
					bModified = true;
				}
			}

			if (bModified)
			{
				FPropertyChangedEvent Event(nullptr, EPropertyChangeType::ValueSet);
				Cue->PostEditChangeProperty(Event);
				Cue->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(%s)"), *Changes.TrimStart()));
				return sol::make_object(Lua, true);
			}

			Session.Log(TEXT("[OK] configure() -> nothing changed"));
			return sol::make_object(Lua, true);
		});
	});
}

REGISTER_LUA_BINDING(SoundCue, SoundCueDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindSoundCue(Lua, Session);
});
