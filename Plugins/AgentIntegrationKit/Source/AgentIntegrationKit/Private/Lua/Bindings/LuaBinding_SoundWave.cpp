// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Tools/NeoStackToolUtils.h"
#include "ScopedTransaction.h"

#include "Sound/SoundWave.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundGroups.h"
#include "Engine/EngineTypes.h"
#include "EditorFramework/AssetImportData.h"
#include "IWaveformTransformation.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// VERSION COMPAT HELPERS
// ============================================================================

static TArray<FSoundWaveCuePoint> AIK_GetSoundWaveCuePoints(const USoundWave* Wave)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	return Wave->GetSoundWaveCuePoints();
#else
	return Wave->GetCuePoints();
#endif
}

static void AIK_SetSoundWaveCuePoints(USoundWave* Wave, const TArray<FSoundWaveCuePoint>& InCuePoints)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	Wave->SetSoundWaveCuePoints(InCuePoints);
#else
	FProperty* Prop = USoundWave::StaticClass()->FindPropertyByName(TEXT("CuePoints"));
	if (Prop)
	{
		TArray<FSoundWaveCuePoint>* CuePointsPtr = Prop->ContainerPtrToValuePtr<TArray<FSoundWaveCuePoint>>(Wave);
		if (CuePointsPtr)
		{
			*CuePointsPtr = InCuePoints;
		}
	}
#endif
}

// ============================================================================
// HELPERS
// ============================================================================

static const char* SoundGroupToString(ESoundGroup Group)
{
	switch (Group)
	{
	case SOUNDGROUP_Default: return "default";
	case SOUNDGROUP_Effects: return "effects";
	case SOUNDGROUP_UI:      return "ui";
	case SOUNDGROUP_Music:   return "music";
	case SOUNDGROUP_Voice:   return "voice";
	default:
		// GameSoundGroup1-20 — return numeric identifier
		{
			static thread_local char Buf[32];
			FCStringAnsi::Snprintf(Buf, sizeof(Buf), "game_sound_group_%d", static_cast<int>(Group) - static_cast<int>(SOUNDGROUP_GameSoundGroup1) + 1);
			return Buf;
		}
	}
}

static ESoundGroup StringToSoundGroup(const FString& Str)
{
	if (Str.Equals(TEXT("effects"), ESearchCase::IgnoreCase)) return SOUNDGROUP_Effects;
	if (Str.Equals(TEXT("ui"), ESearchCase::IgnoreCase))      return SOUNDGROUP_UI;
	if (Str.Equals(TEXT("music"), ESearchCase::IgnoreCase))   return SOUNDGROUP_Music;
	if (Str.Equals(TEXT("voice"), ESearchCase::IgnoreCase))   return SOUNDGROUP_Voice;

	// game_sound_group_N
	if (Str.StartsWith(TEXT("game_sound_group_"), ESearchCase::IgnoreCase))
	{
		int32 Num = FCString::Atoi(*Str.Mid(17));
		if (Num >= 1 && Num <= 20)
		{
			return static_cast<ESoundGroup>(static_cast<int>(SOUNDGROUP_GameSoundGroup1) + Num - 1);
		}
	}

	return SOUNDGROUP_Default;
}

namespace SoundWaveHelpers {
static const char* LoadingBehaviorToString(ESoundWaveLoadingBehavior Behavior)
{
	switch (Behavior)
	{
	case ESoundWaveLoadingBehavior::Inherited:    return "inherited";
	case ESoundWaveLoadingBehavior::RetainOnLoad: return "retain_on_load";
	case ESoundWaveLoadingBehavior::PrimeOnLoad:  return "prime_on_load";
	case ESoundWaveLoadingBehavior::LoadOnDemand: return "load_on_demand";
	case ESoundWaveLoadingBehavior::ForceInline:  return "force_inline";
	default:                                      return "inherited";
	}
}
} // namespace SoundWaveHelpers

static ESoundWaveLoadingBehavior StringToLoadingBehavior(const FString& Str)
{
	if (Str.Equals(TEXT("retain_on_load"), ESearchCase::IgnoreCase)) return ESoundWaveLoadingBehavior::RetainOnLoad;
	if (Str.Equals(TEXT("prime_on_load"), ESearchCase::IgnoreCase))  return ESoundWaveLoadingBehavior::PrimeOnLoad;
	if (Str.Equals(TEXT("load_on_demand"), ESearchCase::IgnoreCase)) return ESoundWaveLoadingBehavior::LoadOnDemand;
	if (Str.Equals(TEXT("force_inline"), ESearchCase::IgnoreCase))   return ESoundWaveLoadingBehavior::ForceInline;
	return ESoundWaveLoadingBehavior::Inherited;
}

static const char* CompressionTypeToString(ESoundAssetCompressionType Type)
{
	switch (Type)
	{
	case ESoundAssetCompressionType::BinkAudio:        return "bink_audio";
	case ESoundAssetCompressionType::ADPCM:            return "adpcm";
	case ESoundAssetCompressionType::PCM:              return "pcm";
	case ESoundAssetCompressionType::Opus:             return "opus";
	case ESoundAssetCompressionType::PlatformSpecific: return "platform_specific";
	case ESoundAssetCompressionType::ProjectDefined:   return "project_defined";
	case ESoundAssetCompressionType::RADAudio:         return "rad_audio";
	default:                                           return "platform_specific";
	}
}

static ESoundAssetCompressionType StringToCompressionType(const FString& Str)
{
	if (Str.Equals(TEXT("bink_audio"), ESearchCase::IgnoreCase))        return ESoundAssetCompressionType::BinkAudio;
	if (Str.Equals(TEXT("adpcm"), ESearchCase::IgnoreCase))            return ESoundAssetCompressionType::ADPCM;
	if (Str.Equals(TEXT("pcm"), ESearchCase::IgnoreCase))              return ESoundAssetCompressionType::PCM;
	if (Str.Equals(TEXT("opus"), ESearchCase::IgnoreCase))             return ESoundAssetCompressionType::Opus;
	if (Str.Equals(TEXT("platform_specific"), ESearchCase::IgnoreCase)) return ESoundAssetCompressionType::PlatformSpecific;
	if (Str.Equals(TEXT("project_defined"), ESearchCase::IgnoreCase))  return ESoundAssetCompressionType::ProjectDefined;
	if (Str.Equals(TEXT("rad_audio"), ESearchCase::IgnoreCase))        return ESoundAssetCompressionType::RADAudio;
	return ESoundAssetCompressionType::PlatformSpecific;
}

// ============================================================================
// BINDING
// ============================================================================

static TArray<FLuaFunctionDoc> SoundWaveDocs = {};

static void BindSoundWave(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_sound_wave", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		USoundWave* Wave = LoadObject<USoundWave>(nullptr, *FPath);
		if (!Wave) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"SoundWave enrichment methods:\n"
			"\n"
			"info() — structured summary:\n"
			"  duration, sample_rate, num_channels, volume, pitch, is_looping, priority,\n"
			"  sound_group, compression_quality, compression_type, loading_behavior,\n"
			"  is_ambisonics, is_mature, subtitle_priority, import_path, sound_class,\n"
			"  lufs, sample_peak_db, manual_word_wrap, single_line, cue_point_count, loop_region_count\n"
			"\n"
			"configure(params) — set properties:\n"
			"  volume (float), pitch (float 0.125-4.0), is_looping (bool), priority (float 0-100),\n"
			"  sound_group (default/effects/ui/music/voice/game_sound_group_N),\n"
			"  compression_quality (int 1-100, ignored for pcm/adpcm),\n"
			"  compression_type (bink_audio/adpcm/pcm/opus/platform_specific/project_defined/rad_audio),\n"
			"  loading_behavior (inherited/retain_on_load/prime_on_load/load_on_demand/force_inline),\n"
			"  is_mature (bool), subtitle_priority (float), is_ambisonics (bool),\n"
			"  sound_class (asset path or '' to clear),\n"
			"  manual_word_wrap (bool), single_line (bool)\n"
			"\n"
			"list(type) — list sub-items:\n"
			"  'subtitles' — subtitle cues (text, time)\n"
			"  'cue_points' — cue points (label, frame_position, frame_length, is_loop_region)\n"
			"  'loop_regions' — loop region cue points only\n"
			"\n"
			"add(type, params) — add sub-items:\n"
			"  'subtitle', {text='...', time=0.0}\n"
			"  'cue_point', {label='...', frame_position=0, frame_length=0}\n"
			"\n"
			"remove(type, params) — remove sub-items:\n"
			"  'subtitle', {index=N} — remove subtitle at 0-based index\n"
			"  'cue_point', {index=N} — remove cue point at 0-based index\n"
			"  'all_subtitles' — clear all subtitles\n"
			"  'all_cue_points' — clear all cue points\n";

		// ================================================================
		// info()
		// ================================================================
		AssetObj.set_function("info", [Wave, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(Wave))
			{
				Session.Log(TEXT("[FAIL] info -> asset no longer valid"));
				return sol::lua_nil;
			}

			sol::table R = Lua.create_table();

			R["duration"] = Wave->Duration;
			R["sample_rate"] = Wave->GetSampleRateForCurrentPlatform();
			R["num_channels"] = Wave->NumChannels;
			R["volume"] = Wave->Volume;
			R["pitch"] = Wave->Pitch;
			R["is_looping"] = static_cast<bool>(Wave->bLooping);
			R["priority"] = Wave->Priority;
			R["sound_group"] = SoundGroupToString(Wave->SoundGroup.GetValue());
			R["compression_quality"] = Wave->GetCompressionQuality();
			R["compression_type"] = CompressionTypeToString(Wave->GetSoundAssetCompressionTypeEnum());
			R["loading_behavior"] = SoundWaveHelpers::LoadingBehaviorToString(Wave->LoadingBehavior);
			R["is_ambisonics"] = static_cast<bool>(Wave->bIsAmbisonics);
			R["is_mature"] = static_cast<bool>(Wave->bMature);
			R["subtitle_priority"] = Wave->SubtitlePriority;
			R["subtitle_count"] = Wave->Subtitles.Num();
			R["total_samples"] = Wave->TotalSamples;
			R["manual_word_wrap"] = static_cast<bool>(Wave->bManualWordWrap);
			R["single_line"] = static_cast<bool>(Wave->bSingleLine);

			// Cue point / loop region counts
			TArray<FSoundWaveCuePoint> AllCuePoints = Wave->GetCuePoints();
			R["cue_point_count"] = AllCuePoints.Num();
			TArray<FSoundWaveCuePoint> LoopRegions = Wave->GetLoopRegions();
			R["loop_region_count"] = LoopRegions.Num();

			// Sound class reference
			if (Wave->SoundClassObject)
			{
				R["sound_class"] = std::string(TCHAR_TO_UTF8(*Wave->SoundClassObject->GetPathName()));
			}

			// Import path & loudness (editor-only)
#if WITH_EDITORONLY_DATA
			if (Wave->AssetImportData)
			{
				FString ImportPath = Wave->AssetImportData->GetFirstFilename();
				if (!ImportPath.IsEmpty())
				{
					R["import_path"] = std::string(TCHAR_TO_UTF8(*ImportPath));
				}
			}
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
			R["lufs"] = Wave->LUFS;
			R["sample_peak_db"] = Wave->SamplePeakDB;
#endif
#endif

			Session.Log(FString::Printf(TEXT("[OK] info() -> SoundWave, duration=%.2fs, channels=%d, sample_rate=%d"),
				Wave->Duration, Wave->NumChannels, (int32)Wave->GetSampleRateForCurrentPlatform()));
			return R;
		});

		// ================================================================
		// configure(params)
		// ================================================================
		AssetObj.set_function("configure", [Wave, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(Wave))
			{
				Session.Log(TEXT("[FAIL] configure -> asset no longer valid"));
				return sol::lua_nil;
			}

			const FScopedTransaction Transaction(FText::FromString(TEXT("SoundWave: Configure")));
			Wave->Modify();
			bool bModified = false;
			bool bCompressionTypeChanged = false;
			FString Changes;

			// Volume
			sol::optional<double> Vol = Params.get<sol::optional<double>>("volume");
			if (Vol.has_value())
			{
				Wave->Volume = FMath::Max(0.0f, static_cast<float>(Vol.value()));
				Changes += FString::Printf(TEXT(" volume=%.2f"), (double)Wave->Volume);
				bModified = true;
			}

			// Pitch
			sol::optional<double> PitchVal = Params.get<sol::optional<double>>("pitch");
			if (PitchVal.has_value())
			{
				Wave->Pitch = FMath::Clamp(static_cast<float>(PitchVal.value()), 0.125f, 4.0f);
				Changes += FString::Printf(TEXT(" pitch=%.3f"), (double)Wave->Pitch);
				bModified = true;
			}

			// Looping
			sol::optional<bool> Looping = Params.get<sol::optional<bool>>("is_looping");
			if (Looping.has_value())
			{
				Wave->bLooping = Looping.value();
				Changes += FString::Printf(TEXT(" is_looping=%s"), Looping.value() ? TEXT("true") : TEXT("false"));
				bModified = true;
			}

			// Priority
			sol::optional<double> Pri = Params.get<sol::optional<double>>("priority");
			if (Pri.has_value())
			{
				Wave->Priority = FMath::Clamp(static_cast<float>(Pri.value()), 0.0f, 100.0f);
				Changes += FString::Printf(TEXT(" priority=%.1f"), (double)Wave->Priority);
				bModified = true;
			}

			// Sound group
			sol::optional<std::string> Group = Params.get<sol::optional<std::string>>("sound_group");
			if (Group.has_value())
			{
				FString GroupStr = UTF8_TO_TCHAR(Group.value().c_str());
				Wave->SoundGroup = StringToSoundGroup(GroupStr);
				Changes += FString::Printf(TEXT(" sound_group=%s"), *GroupStr.ToLower());
				bModified = true;
			}

			// Compression quality (private field, use reflection)
			sol::optional<int> CompQuality = Params.get<sol::optional<int>>("compression_quality");
			if (CompQuality.has_value())
			{
				// Warn if compression type ignores quality
				ESoundAssetCompressionType CurrentType = Wave->GetSoundAssetCompressionTypeEnum();
				if (CurrentType == ESoundAssetCompressionType::PCM || CurrentType == ESoundAssetCompressionType::ADPCM)
				{
					Session.Log(FString::Printf(TEXT("[WARN] compression_quality has no effect on %hs compression type"),
						CompressionTypeToString(CurrentType)));
				}

				static FProperty* CompProp = USoundWave::StaticClass()->FindPropertyByName(TEXT("CompressionQuality"));
				if (CompProp)
				{
					int32 Val = FMath::Clamp(CompQuality.value(), 1, 100);
					int32* Ptr = CompProp->ContainerPtrToValuePtr<int32>(Wave);
					if (Ptr)
					{
						*Ptr = Val;
						Changes += FString::Printf(TEXT(" compression_quality=%d"), Val);
						bModified = true;
					}
				}
			}

			// Compression type — use bMarkDirty=true so ProjectDefined is not silently skipped.
			// SetSoundAssetCompressionType internally calls UpdateAsset which handles recaching,
			// so we skip the generic PostEditChangeProperty for this property.
			sol::optional<std::string> CompType = Params.get<sol::optional<std::string>>("compression_type");
			if (CompType.has_value())
			{
				FString TypeStr = UTF8_TO_TCHAR(CompType.value().c_str());
				Wave->SetSoundAssetCompressionType(StringToCompressionType(TypeStr));
				Changes += FString::Printf(TEXT(" compression_type=%s"), *TypeStr.ToLower());
				bModified = true;
				bCompressionTypeChanged = true;
			}

			// Loading behavior
			sol::optional<std::string> LoadBehavior = Params.get<sol::optional<std::string>>("loading_behavior");
			if (LoadBehavior.has_value())
			{
				FString BehaviorStr = UTF8_TO_TCHAR(LoadBehavior.value().c_str());
				Wave->LoadingBehavior = StringToLoadingBehavior(BehaviorStr);
				Changes += FString::Printf(TEXT(" loading_behavior=%s"), *BehaviorStr.ToLower());
				bModified = true;
			}

			// Mature content flag
			sol::optional<bool> Mature = Params.get<sol::optional<bool>>("is_mature");
			if (Mature.has_value())
			{
				Wave->bMature = Mature.value();
				Changes += FString::Printf(TEXT(" is_mature=%s"), Mature.value() ? TEXT("true") : TEXT("false"));
				bModified = true;
			}

			// Subtitle priority
			sol::optional<double> SubPri = Params.get<sol::optional<double>>("subtitle_priority");
			if (SubPri.has_value())
			{
				Wave->SubtitlePriority = static_cast<float>(SubPri.value());
				Changes += FString::Printf(TEXT(" subtitle_priority=%.1f"), (double)Wave->SubtitlePriority);
				bModified = true;
			}

			// Ambisonics
			sol::optional<bool> Ambi = Params.get<sol::optional<bool>>("is_ambisonics");
			if (Ambi.has_value())
			{
				Wave->bIsAmbisonics = Ambi.value();
				Changes += FString::Printf(TEXT(" is_ambisonics=%s"), Ambi.value() ? TEXT("true") : TEXT("false"));
				bModified = true;
			}

			// Sound class (asset path or empty string to clear)
			sol::optional<std::string> SoundClass = Params.get<sol::optional<std::string>>("sound_class");
			if (SoundClass.has_value())
			{
				FString ClassPath = UTF8_TO_TCHAR(SoundClass.value().c_str());
				if (ClassPath.IsEmpty())
				{
					Wave->SoundClassObject = nullptr;
					Changes += TEXT(" sound_class=<cleared>");
					bModified = true;
				}
				else
				{
					USoundClass* SC = LoadObject<USoundClass>(nullptr, *ClassPath);
					if (SC)
					{
						Wave->SoundClassObject = SC;
						Changes += FString::Printf(TEXT(" sound_class=%s"), *SC->GetName());
						bModified = true;
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[WARN] sound_class '%s' not found"), *ClassPath));
					}
				}
			}

			// Manual word wrap
			sol::optional<bool> WordWrap = Params.get<sol::optional<bool>>("manual_word_wrap");
			if (WordWrap.has_value())
			{
				Wave->bManualWordWrap = WordWrap.value();
				Changes += FString::Printf(TEXT(" manual_word_wrap=%s"), WordWrap.value() ? TEXT("true") : TEXT("false"));
				bModified = true;
			}

			// Single line subtitles
			sol::optional<bool> SingleLine = Params.get<sol::optional<bool>>("single_line");
			if (SingleLine.has_value())
			{
				Wave->bSingleLine = SingleLine.value();
				Changes += FString::Printf(TEXT(" single_line=%s"), SingleLine.value() ? TEXT("true") : TEXT("false"));
				bModified = true;
			}

			if (bModified)
			{
				// SetSoundAssetCompressionType already calls UpdateAsset internally.
				// Only fire PostEditChangeProperty for other property changes.
				if (!bCompressionTypeChanged)
				{
					FPropertyChangedEvent Event(nullptr, EPropertyChangeType::ValueSet);
					Wave->PostEditChangeProperty(Event);
				}
				Wave->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(%s)"), *Changes.TrimStart()));
				return sol::make_object(Lua, true);
			}

			Session.Log(TEXT("[OK] configure() -> nothing changed. Use help() to see valid keys."));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// list(type)
		// ================================================================
		AssetObj.set_function("list", [Wave, &Session](sol::table /*self*/,
			const std::string& ListType, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(Wave))
			{
				Session.Log(TEXT("[FAIL] list -> asset no longer valid"));
				return sol::lua_nil;
			}

			FString TypeStr = UTF8_TO_TCHAR(ListType.c_str());

			// ----- subtitles -----
			if (TypeStr.Equals(TEXT("subtitles"), ESearchCase::IgnoreCase))
			{
				sol::table Arr = Lua.create_table();
				for (int32 i = 0; i < Wave->Subtitles.Num(); i++)
				{
					const FSubtitleCue& Cue = Wave->Subtitles[i];
					sol::table Entry = Lua.create_table();
					Entry["index"] = i;
					Entry["text"] = std::string(TCHAR_TO_UTF8(*Cue.Text.ToString()));
					Entry["time"] = Cue.Time;
					Arr[i + 1] = Entry;
				}
				Session.Log(FString::Printf(TEXT("[OK] list('subtitles') -> %d entries"), Wave->Subtitles.Num()));
				return Arr;
			}

			// ----- cue_points -----
			if (TypeStr.Equals(TEXT("cue_points"), ESearchCase::IgnoreCase))
			{
				TArray<FSoundWaveCuePoint> CuePoints = Wave->GetCuePoints();
				sol::table Arr = Lua.create_table();
				for (int32 i = 0; i < CuePoints.Num(); i++)
				{
					const FSoundWaveCuePoint& CP = CuePoints[i];
					sol::table Entry = Lua.create_table();
					Entry["index"] = i;
					Entry["cue_point_id"] = CP.CuePointID;
					Entry["label"] = std::string(TCHAR_TO_UTF8(*CP.Label));
					Entry["frame_position"] = static_cast<double>(CP.FramePosition);
					Entry["frame_length"] = static_cast<double>(CP.FrameLength);
					Entry["is_loop_region"] = CP.IsLoopRegion();
					Arr[i + 1] = Entry;
				}
				Session.Log(FString::Printf(TEXT("[OK] list('cue_points') -> %d entries"), CuePoints.Num()));
				return Arr;
			}

			// ----- loop_regions -----
			if (TypeStr.Equals(TEXT("loop_regions"), ESearchCase::IgnoreCase))
			{
				TArray<FSoundWaveCuePoint> Regions = Wave->GetLoopRegions();
				sol::table Arr = Lua.create_table();
				for (int32 i = 0; i < Regions.Num(); i++)
				{
					const FSoundWaveCuePoint& CP = Regions[i];
					sol::table Entry = Lua.create_table();
					Entry["index"] = i;
					Entry["cue_point_id"] = CP.CuePointID;
					Entry["label"] = std::string(TCHAR_TO_UTF8(*CP.Label));
					Entry["frame_position"] = static_cast<double>(CP.FramePosition);
					Entry["frame_length"] = static_cast<double>(CP.FrameLength);
					Arr[i + 1] = Entry;
				}
				Session.Log(FString::Printf(TEXT("[OK] list('loop_regions') -> %d entries"), Regions.Num()));
				return Arr;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list('%s') -> unknown type. Valid: subtitles, cue_points, loop_regions"), *TypeStr));
			return sol::lua_nil;
		});

		// ================================================================
		// add(type, params)
		// ================================================================
		AssetObj.set_function("add", [Wave, &Session](sol::table /*self*/,
			const std::string& AddType, sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(Wave))
			{
				Session.Log(TEXT("[FAIL] add -> asset no longer valid"));
				return sol::lua_nil;
			}

			FString TypeStr = UTF8_TO_TCHAR(AddType.c_str());

			// ----- subtitle -----
			if (TypeStr.Equals(TEXT("subtitle"), ESearchCase::IgnoreCase))
			{
				const FScopedTransaction Transaction(FText::FromString(TEXT("SoundWave: Add Subtitle")));
				Wave->Modify();

				FSubtitleCue NewCue;
				sol::optional<std::string> Text = Params.get<sol::optional<std::string>>("text");
				if (Text.has_value())
				{
					NewCue.Text = FText::FromString(UTF8_TO_TCHAR(Text.value().c_str()));
				}
				NewCue.Time = static_cast<float>(Params.get_or("time", 0.0));

				Wave->Subtitles.Add(NewCue);

				FPropertyChangedEvent Event(nullptr, EPropertyChangeType::ValueSet);
				Wave->PostEditChangeProperty(Event);
				Wave->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] add('subtitle') -> index %d, text='%s', time=%.3f"),
					Wave->Subtitles.Num() - 1, *NewCue.Text.ToString(), NewCue.Time));
				return sol::make_object(Lua, Wave->Subtitles.Num() - 1);
			}

			// ----- cue_point -----
			if (TypeStr.Equals(TEXT("cue_point"), ESearchCase::IgnoreCase))
			{
#if WITH_EDITOR
				const FScopedTransaction Transaction(FText::FromString(TEXT("SoundWave: Add Cue Point")));
				Wave->Modify();

				// Get current cue points, add new one, set back
				TArray<FSoundWaveCuePoint> CuePoints = AIK_GetSoundWaveCuePoints(Wave);

				FSoundWaveCuePoint NewCP;
				sol::optional<std::string> Label = Params.get<sol::optional<std::string>>("label");
				if (Label.has_value())
				{
					NewCP.Label = UTF8_TO_TCHAR(Label.value().c_str());
				}
				NewCP.FramePosition = static_cast<int64>(Params.get_or("frame_position", 0.0));
				NewCP.FrameLength = static_cast<int64>(Params.get_or("frame_length", 0.0));
				// CuePointID: assign next available
				int32 MaxID = -1;
				for (const FSoundWaveCuePoint& CP : CuePoints) { MaxID = FMath::Max(MaxID, CP.CuePointID); }
				NewCP.CuePointID = MaxID + 1;

				CuePoints.Add(NewCP);
				AIK_SetSoundWaveCuePoints(Wave,CuePoints);

				Wave->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] add('cue_point') -> id=%d, label='%s', frame_pos=%lld"),
					NewCP.CuePointID, *NewCP.Label, NewCP.FramePosition));
				return sol::make_object(Lua, NewCP.CuePointID);
#else
				Session.Log(TEXT("[FAIL] add('cue_point') -> only available in editor builds"));
				return sol::lua_nil;
#endif
			}

			Session.Log(FString::Printf(TEXT("[FAIL] add('%s') -> unknown type. Valid: subtitle, cue_point"), *TypeStr));
			return sol::lua_nil;
		});

		// ================================================================
		// remove(type, params)
		// ================================================================
		AssetObj.set_function("remove", [Wave, &Session](sol::table /*self*/,
			const std::string& RemoveType, sol::optional<sol::table> OptParams, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(Wave))
			{
				Session.Log(TEXT("[FAIL] remove -> asset no longer valid"));
				return sol::lua_nil;
			}

			FString TypeStr = UTF8_TO_TCHAR(RemoveType.c_str());

			// ----- all_subtitles -----
			if (TypeStr.Equals(TEXT("all_subtitles"), ESearchCase::IgnoreCase))
			{
				const FScopedTransaction Transaction(FText::FromString(TEXT("SoundWave: Clear Subtitles")));
				Wave->Modify();
				int32 Count = Wave->Subtitles.Num();
				Wave->Subtitles.Empty();
				FPropertyChangedEvent Event(nullptr, EPropertyChangeType::ValueSet);
				Wave->PostEditChangeProperty(Event);
				Wave->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove('all_subtitles') -> cleared %d entries"), Count));
				return sol::make_object(Lua, true);
			}

			// ----- all_cue_points -----
			if (TypeStr.Equals(TEXT("all_cue_points"), ESearchCase::IgnoreCase))
			{
#if WITH_EDITOR
				const FScopedTransaction Transaction(FText::FromString(TEXT("SoundWave: Clear Cue Points")));
				Wave->Modify();
				TArray<FSoundWaveCuePoint> Empty;
				AIK_SetSoundWaveCuePoints(Wave,Empty);
				Wave->MarkPackageDirty();
				Session.Log(TEXT("[OK] remove('all_cue_points') -> cleared"));
				return sol::make_object(Lua, true);
#else
				Session.Log(TEXT("[FAIL] remove('all_cue_points') -> only available in editor builds"));
				return sol::lua_nil;
#endif
			}

			// For indexed removals, params table is required
			if (!OptParams.has_value())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove('%s') -> params table required with 'index' key"), *TypeStr));
				return sol::lua_nil;
			}
			sol::table Params = OptParams.value();

			// ----- subtitle -----
			if (TypeStr.Equals(TEXT("subtitle"), ESearchCase::IgnoreCase))
			{
				sol::optional<int> Idx = Params.get<sol::optional<int>>("index");
				if (!Idx.has_value())
				{
					Session.Log(TEXT("[FAIL] remove('subtitle') -> 'index' param required"));
					return sol::lua_nil;
				}
				int32 Index = Idx.value();
				if (Index < 0 || Index >= Wave->Subtitles.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove('subtitle') -> index %d out of range [0..%d)"),
						Index, Wave->Subtitles.Num()));
					return sol::lua_nil;
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("SoundWave: Remove Subtitle")));
				Wave->Modify();
				Wave->Subtitles.RemoveAt(Index);
				FPropertyChangedEvent Event(nullptr, EPropertyChangeType::ValueSet);
				Wave->PostEditChangeProperty(Event);
				Wave->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] remove('subtitle', index=%d)"), Index));
				return sol::make_object(Lua, true);
			}

			// ----- cue_point -----
			if (TypeStr.Equals(TEXT("cue_point"), ESearchCase::IgnoreCase))
			{
#if WITH_EDITOR
				sol::optional<int> Idx = Params.get<sol::optional<int>>("index");
				if (!Idx.has_value())
				{
					Session.Log(TEXT("[FAIL] remove('cue_point') -> 'index' param required"));
					return sol::lua_nil;
				}

				TArray<FSoundWaveCuePoint> CuePoints = AIK_GetSoundWaveCuePoints(Wave);
				int32 Index = Idx.value();
				if (Index < 0 || Index >= CuePoints.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove('cue_point') -> index %d out of range [0..%d)"),
						Index, CuePoints.Num()));
					return sol::lua_nil;
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("SoundWave: Remove Cue Point")));
				Wave->Modify();
				CuePoints.RemoveAt(Index);
				AIK_SetSoundWaveCuePoints(Wave,CuePoints);
				Wave->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] remove('cue_point', index=%d)"), Index));
				return sol::make_object(Lua, true);
#else
				Session.Log(TEXT("[FAIL] remove('cue_point') -> only available in editor builds"));
				return sol::lua_nil;
#endif
			}

			Session.Log(FString::Printf(TEXT("[FAIL] remove('%s') -> unknown type. Valid: subtitle, cue_point, all_subtitles, all_cue_points"), *TypeStr));
			return sol::lua_nil;
		});
	});
}

REGISTER_LUA_BINDING(SoundWave, SoundWaveDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindSoundWave(Lua, Session);
});
