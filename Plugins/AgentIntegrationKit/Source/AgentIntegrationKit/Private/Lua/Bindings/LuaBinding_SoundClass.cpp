// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Tools/NeoStackToolUtils.h"
#include "ScopedTransaction.h"

#include "Sound/SoundClass.h"
#include "Sound/SoundMix.h"
#include "Sound/SoundSubmix.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// HELPERS
// ============================================================================

static const TCHAR* LoadingBehaviorToString(ESoundWaveLoadingBehavior B)
{
	switch (B)
	{
	case ESoundWaveLoadingBehavior::Inherited:    return TEXT("inherited");
	case ESoundWaveLoadingBehavior::RetainOnLoad: return TEXT("retain_on_load");
	case ESoundWaveLoadingBehavior::PrimeOnLoad:  return TEXT("prime_on_load");
	case ESoundWaveLoadingBehavior::LoadOnDemand: return TEXT("load_on_demand");
	case ESoundWaveLoadingBehavior::ForceInline:  return TEXT("force_inline");
	case ESoundWaveLoadingBehavior::Uninitialized:return TEXT("uninitialized");
	default:                                      return TEXT("inherited");
	}
}

static bool StringToLoadingBehavior(const std::string& Str, ESoundWaveLoadingBehavior& OutVal)
{
	if (Str == "inherited")       { OutVal = ESoundWaveLoadingBehavior::Inherited; return true; }
	if (Str == "retain_on_load")  { OutVal = ESoundWaveLoadingBehavior::RetainOnLoad; return true; }
	if (Str == "prime_on_load")   { OutVal = ESoundWaveLoadingBehavior::PrimeOnLoad; return true; }
	if (Str == "load_on_demand")  { OutVal = ESoundWaveLoadingBehavior::LoadOnDemand; return true; }
	if (Str == "force_inline")    { OutVal = ESoundWaveLoadingBehavior::ForceInline; return true; }
	return false;
}

static const TCHAR* OutputTargetToString(EAudioOutputTarget::Type T)
{
	switch (T)
	{
	case EAudioOutputTarget::Speaker:                      return TEXT("speaker");
	case EAudioOutputTarget::Controller:                   return TEXT("controller");
	case EAudioOutputTarget::ControllerFallbackToSpeaker:  return TEXT("controller_fallback_to_speaker");
	default:                                               return TEXT("speaker");
	}
}

static bool StringToOutputTarget(const std::string& Str, EAudioOutputTarget::Type& OutVal)
{
	if (Str == "speaker")                       { OutVal = EAudioOutputTarget::Speaker; return true; }
	if (Str == "controller")                    { OutVal = EAudioOutputTarget::Controller; return true; }
	if (Str == "controller_fallback_to_speaker"){ OutVal = EAudioOutputTarget::ControllerFallbackToSpeaker; return true; }
	return false;
}

// ============================================================================
// BINDING
// ============================================================================

static TArray<FLuaFunctionDoc> SoundClassDocs = {};

static void BindSoundClass(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_sound_class", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		USoundClass* SoundClass = LoadObject<USoundClass>(nullptr, *FPath);
		if (!SoundClass) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"SoundClass enrichment methods:\n"
			"\n"
			"info() — structured summary:\n"
			"  volume, pitch, low_pass_filter_frequency, attenuation_distance_scale,\n"
			"  lfe_bleed, voice_center_channel_volume, always_play, is_ui_sound,\n"
			"  is_music, center_channel_only, apply_ambient_volumes, apply_reverb,\n"
			"  default_2d_reverb_send_amount, apply_effects, loading_behavior,\n"
			"  output_target, default_submix, modulation,\n"
			"  radio_filter_volume, radio_filter_volume_threshold,\n"
			"  parent, child_count, children, passive_sound_mix_count,\n"
			"  passive_sound_mixes\n"
			"\n"
			"configure(params) — set FSoundClassProperties fields:\n"
			"  volume (float>=0), pitch (float>0), low_pass_filter_frequency (float>=0),\n"
			"  attenuation_distance_scale (float), lfe_bleed (float 0-1),\n"
			"  voice_center_channel_volume (float 0-1), always_play (bool),\n"
			"  is_ui_sound (bool), is_music (bool), center_channel_only (bool),\n"
			"  apply_ambient_volumes (bool), apply_reverb (bool),\n"
			"  default_2d_reverb_send_amount (float 0-1), apply_effects (bool),\n"
			"  radio_filter_volume (float), radio_filter_volume_threshold (float),\n"
			"  loading_behavior (string: inherited|retain_on_load|prime_on_load|load_on_demand|force_inline),\n"
			"  output_target (string: speaker|controller|controller_fallback_to_speaker),\n"
			"  default_submix (string path or 'none')\n"
			"\n"
			"list(type) — type = 'passive_sound_mixes' | 'children'\n"
			"\n"
			"add(type, params) — add items:\n"
			"  type='passive_sound_mix': {sound_mix='/path', min_volume=0, max_volume=10}\n"
			"  type='child': {path='/Game/MyChildClass'}\n"
			"\n"
			"remove(type, params) — remove items:\n"
			"  type='passive_sound_mix': {index=N} or {sound_mix='/path'}\n"
			"  type='child': {index=N} or {path='/path'}\n";

		// ================================================================
		// info()
		// ================================================================
		AssetObj.set_function("info", [SoundClass, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(SoundClass))
			{
				Session.Log(TEXT("[FAIL] info -> asset no longer valid"));
				return sol::lua_nil;
			}

			const FSoundClassProperties& P = SoundClass->Properties;
			sol::table R = Lua.create_table();

			// Core properties
			R["volume"] = P.Volume;
			R["pitch"] = P.Pitch;
			R["low_pass_filter_frequency"] = P.LowPassFilterFrequency;
			R["attenuation_distance_scale"] = P.AttenuationDistanceScale;
			R["lfe_bleed"] = P.LFEBleed;
			R["voice_center_channel_volume"] = P.VoiceCenterChannelVolume;

			// Flags
			R["always_play"] = static_cast<bool>(P.bAlwaysPlay);
			R["is_ui_sound"] = static_cast<bool>(P.bIsUISound);
			R["is_music"] = static_cast<bool>(P.bIsMusic);
			R["center_channel_only"] = static_cast<bool>(P.bCenterChannelOnly);
			R["apply_ambient_volumes"] = static_cast<bool>(P.bApplyAmbientVolumes);
			R["apply_reverb"] = static_cast<bool>(P.bReverb);
			R["apply_effects"] = static_cast<bool>(P.bApplyEffects);
			R["default_2d_reverb_send_amount"] = P.Default2DReverbSendAmount;

			// Loading behavior
			R["loading_behavior"] = std::string(TCHAR_TO_UTF8(LoadingBehaviorToString(P.LoadingBehavior)));

			// Output target
			R["output_target"] = std::string(TCHAR_TO_UTF8(OutputTargetToString(P.OutputTarget)));

			// Default submix
			if (P.DefaultSubmix)
			{
				R["default_submix"] = std::string(TCHAR_TO_UTF8(*P.DefaultSubmix->GetPathName()));
			}

			// Modulation settings
			{
				sol::table Mod = Lua.create_table();
				sol::table Vol = Lua.create_table();
				Vol["value"] = P.ModulationSettings.VolumeModulationDestination.Value;
				Mod["volume"] = Vol;

				sol::table Pit = Lua.create_table();
				Pit["value"] = P.ModulationSettings.PitchModulationDestination.Value;
				Mod["pitch"] = Pit;

				sol::table HP = Lua.create_table();
				HP["value"] = P.ModulationSettings.HighpassModulationDestination.Value;
				Mod["highpass"] = HP;

				sol::table LP = Lua.create_table();
				LP["value"] = P.ModulationSettings.LowpassModulationDestination.Value;
				Mod["lowpass"] = LP;

				R["modulation"] = Mod;
			}

			// Legacy
			R["radio_filter_volume"] = P.RadioFilterVolume;
			R["radio_filter_volume_threshold"] = P.RadioFilterVolumeThreshold;

			// Hierarchy
			if (SoundClass->ParentClass)
			{
				R["parent"] = std::string(TCHAR_TO_UTF8(*SoundClass->ParentClass->GetPathName()));
			}

			R["child_count"] = SoundClass->ChildClasses.Num();
			if (SoundClass->ChildClasses.Num() > 0)
			{
				sol::table Children = Lua.create_table();
				for (int32 i = 0; i < SoundClass->ChildClasses.Num(); i++)
				{
					if (SoundClass->ChildClasses[i])
					{
						Children[i + 1] = std::string(TCHAR_TO_UTF8(*SoundClass->ChildClasses[i]->GetPathName()));
					}
				}
				R["children"] = Children;
			}

			// Passive sound mix modifiers
			R["passive_sound_mix_count"] = SoundClass->PassiveSoundMixModifiers.Num();
			if (SoundClass->PassiveSoundMixModifiers.Num() > 0)
			{
				sol::table Mixes = Lua.create_table();
				for (int32 i = 0; i < SoundClass->PassiveSoundMixModifiers.Num(); i++)
				{
					const FPassiveSoundMixModifier& Mod = SoundClass->PassiveSoundMixModifiers[i];
					sol::table Entry = Lua.create_table();
					Entry["index"] = i + 1;
					if (Mod.SoundMix)
					{
						Entry["sound_mix"] = std::string(TCHAR_TO_UTF8(*Mod.SoundMix->GetPathName()));
					}
					Entry["min_volume_threshold"] = Mod.MinVolumeThreshold;
					Entry["max_volume_threshold"] = Mod.MaxVolumeThreshold;
					Mixes[i + 1] = Entry;
				}
				R["passive_sound_mixes"] = Mixes;
			}

			Session.Log(FString::Printf(TEXT("[OK] info() -> SoundClass, volume=%.2f, pitch=%.2f, children=%d"),
				P.Volume, P.Pitch, SoundClass->ChildClasses.Num()));
			return R;
		});

		// ================================================================
		// list(type)
		// ================================================================
		AssetObj.set_function("list", [SoundClass, &Session](sol::table /*self*/,
			std::string Type, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(SoundClass))
			{
				Session.Log(TEXT("[FAIL] list -> asset no longer valid"));
				return sol::lua_nil;
			}

			if (Type == "passive_sound_mixes")
			{
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < SoundClass->PassiveSoundMixModifiers.Num(); i++)
				{
					const FPassiveSoundMixModifier& Mod = SoundClass->PassiveSoundMixModifiers[i];
					sol::table Entry = Lua.create_table();
					Entry["index"] = i + 1;
					if (Mod.SoundMix)
					{
						Entry["sound_mix"] = std::string(TCHAR_TO_UTF8(*Mod.SoundMix->GetPathName()));
						Entry["name"] = std::string(TCHAR_TO_UTF8(*Mod.SoundMix->GetName()));
					}
					Entry["min_volume_threshold"] = Mod.MinVolumeThreshold;
					Entry["max_volume_threshold"] = Mod.MaxVolumeThreshold;
					Result[i + 1] = Entry;
				}
				Session.Log(FString::Printf(TEXT("[OK] list('passive_sound_mixes') -> %d entries"),
					SoundClass->PassiveSoundMixModifiers.Num()));
				return Result;
			}

			if (Type == "children")
			{
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < SoundClass->ChildClasses.Num(); i++)
				{
					if (SoundClass->ChildClasses[i])
					{
						sol::table Entry = Lua.create_table();
						Entry["index"] = i + 1;
						Entry["path"] = std::string(TCHAR_TO_UTF8(*SoundClass->ChildClasses[i]->GetPathName()));
						Entry["name"] = std::string(TCHAR_TO_UTF8(*SoundClass->ChildClasses[i]->GetName()));
						Result[i + 1] = Entry;
					}
				}
				Session.Log(FString::Printf(TEXT("[OK] list('children') -> %d entries"),
					SoundClass->ChildClasses.Num()));
				return Result;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list('%s') -> unknown type. Use: passive_sound_mixes, children"),
				UTF8_TO_TCHAR(Type.c_str())));
			return sol::lua_nil;
		});

		// ================================================================
		// configure(params)
		// ================================================================
		AssetObj.set_function("configure", [SoundClass, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(SoundClass))
			{
				Session.Log(TEXT("[FAIL] configure -> asset no longer valid"));
				return sol::lua_nil;
			}

			const FScopedTransaction Transaction(FText::FromString(TEXT("SoundClass: Configure")));
			SoundClass->Modify();
			FSoundClassProperties& P = SoundClass->Properties;
			bool bModified = false;
			FString Changes;

			// Volume (clamped >= 0, matches engine ClampMin="0.0")
			sol::optional<double> Vol = Params.get<sol::optional<double>>("volume");
			if (Vol.has_value())
			{
				P.Volume = FMath::Max(0.0f, static_cast<float>(Vol.value()));
				Changes += FString::Printf(TEXT(" volume=%.2f"), (double)P.Volume);
				bModified = true;
			}

			// Pitch (clamped > 0 to avoid zero/negative pitch)
			sol::optional<double> PitchVal = Params.get<sol::optional<double>>("pitch");
			if (PitchVal.has_value())
			{
				P.Pitch = FMath::Max(0.001f, static_cast<float>(PitchVal.value()));
				Changes += FString::Printf(TEXT(" pitch=%.2f"), (double)P.Pitch);
				bModified = true;
			}

			// Low pass filter frequency (clamped >= 0)
			sol::optional<double> LPF = Params.get<sol::optional<double>>("low_pass_filter_frequency");
			if (LPF.has_value())
			{
				P.LowPassFilterFrequency = FMath::Max(0.0f, static_cast<float>(LPF.value()));
				Changes += FString::Printf(TEXT(" low_pass_filter_frequency=%.0f"), (double)P.LowPassFilterFrequency);
				bModified = true;
			}

			// Attenuation distance scale
			sol::optional<double> AttDistScale = Params.get<sol::optional<double>>("attenuation_distance_scale");
			if (AttDistScale.has_value())
			{
				P.AttenuationDistanceScale = static_cast<float>(AttDistScale.value());
				Changes += FString::Printf(TEXT(" attenuation_distance_scale=%.2f"), (double)P.AttenuationDistanceScale);
				bModified = true;
			}

			// LFE bleed (clamped 0-1)
			sol::optional<double> LFE = Params.get<sol::optional<double>>("lfe_bleed");
			if (LFE.has_value())
			{
				P.LFEBleed = FMath::Clamp(static_cast<float>(LFE.value()), 0.0f, 1.0f);
				Changes += FString::Printf(TEXT(" lfe_bleed=%.2f"), (double)P.LFEBleed);
				bModified = true;
			}

			// Voice center channel volume (clamped 0-1)
			sol::optional<double> VoiceCenter = Params.get<sol::optional<double>>("voice_center_channel_volume");
			if (VoiceCenter.has_value())
			{
				P.VoiceCenterChannelVolume = FMath::Clamp(static_cast<float>(VoiceCenter.value()), 0.0f, 1.0f);
				Changes += FString::Printf(TEXT(" voice_center_channel_volume=%.2f"), (double)P.VoiceCenterChannelVolume);
				bModified = true;
			}

			// Default 2D reverb send amount (clamped 0-1)
			sol::optional<double> RevSend = Params.get<sol::optional<double>>("default_2d_reverb_send_amount");
			if (RevSend.has_value())
			{
				P.Default2DReverbSendAmount = FMath::Clamp(static_cast<float>(RevSend.value()), 0.0f, 1.0f);
				Changes += FString::Printf(TEXT(" default_2d_reverb_send_amount=%.2f"), (double)P.Default2DReverbSendAmount);
				bModified = true;
			}

			// Radio filter volume (legacy)
			sol::optional<double> RadioVol = Params.get<sol::optional<double>>("radio_filter_volume");
			if (RadioVol.has_value())
			{
				P.RadioFilterVolume = static_cast<float>(RadioVol.value());
				Changes += FString::Printf(TEXT(" radio_filter_volume=%.2f"), (double)P.RadioFilterVolume);
				bModified = true;
			}

			// Radio filter volume threshold (legacy)
			sol::optional<double> RadioThresh = Params.get<sol::optional<double>>("radio_filter_volume_threshold");
			if (RadioThresh.has_value())
			{
				P.RadioFilterVolumeThreshold = static_cast<float>(RadioThresh.value());
				Changes += FString::Printf(TEXT(" radio_filter_volume_threshold=%.2f"), (double)P.RadioFilterVolumeThreshold);
				bModified = true;
			}

			// Boolean flags (bitfields cannot be passed by reference, so set individually)
#define SET_BOOL_FLAG(Key, Field, Name) \
			{ \
				sol::optional<bool> Val = Params.get<sol::optional<bool>>(Key); \
				if (Val.has_value()) \
				{ \
					Field = Val.value(); \
					Changes += FString::Printf(TEXT(" %s=%s"), TEXT(Name), Val.value() ? TEXT("true") : TEXT("false")); \
					bModified = true; \
				} \
			}
			SET_BOOL_FLAG("always_play", P.bAlwaysPlay, "always_play")
			SET_BOOL_FLAG("is_ui_sound", P.bIsUISound, "is_ui_sound")
			SET_BOOL_FLAG("is_music", P.bIsMusic, "is_music")
			SET_BOOL_FLAG("center_channel_only", P.bCenterChannelOnly, "center_channel_only")
			SET_BOOL_FLAG("apply_ambient_volumes", P.bApplyAmbientVolumes, "apply_ambient_volumes")
			SET_BOOL_FLAG("apply_reverb", P.bReverb, "apply_reverb")
			SET_BOOL_FLAG("apply_effects", P.bApplyEffects, "apply_effects")
#undef SET_BOOL_FLAG

			// Loading behavior
			sol::optional<std::string> LoadBehavior = Params.get<sol::optional<std::string>>("loading_behavior");
			if (LoadBehavior.has_value())
			{
				ESoundWaveLoadingBehavior NewBehavior;
				if (StringToLoadingBehavior(LoadBehavior.value(), NewBehavior))
				{
					P.LoadingBehavior = NewBehavior;
					Changes += FString::Printf(TEXT(" loading_behavior=%s"),
						LoadingBehaviorToString(NewBehavior));
					bModified = true;
				}
				else
				{
					Session.Log(FString::Printf(TEXT("[WARN] configure -> unknown loading_behavior '%s'. "
						"Valid: inherited, retain_on_load, prime_on_load, load_on_demand, force_inline"),
						UTF8_TO_TCHAR(LoadBehavior.value().c_str())));
				}
			}

			// Output target
			sol::optional<std::string> OutTarget = Params.get<sol::optional<std::string>>("output_target");
			if (OutTarget.has_value())
			{
				EAudioOutputTarget::Type NewTarget;
				if (StringToOutputTarget(OutTarget.value(), NewTarget))
				{
					P.OutputTarget = NewTarget;
					Changes += FString::Printf(TEXT(" output_target=%s"),
						OutputTargetToString(NewTarget));
					bModified = true;
				}
				else
				{
					Session.Log(FString::Printf(TEXT("[WARN] configure -> unknown output_target '%s'. "
						"Valid: speaker, controller, controller_fallback_to_speaker"),
						UTF8_TO_TCHAR(OutTarget.value().c_str())));
				}
			}

			// Default submix
			sol::optional<std::string> Submix = Params.get<sol::optional<std::string>>("default_submix");
			if (Submix.has_value())
			{
				if (Submix.value() == "none" || Submix.value().empty())
				{
					P.DefaultSubmix = nullptr;
					Changes += TEXT(" default_submix=none");
					bModified = true;
				}
				else
				{
					FString SubmixPath = UTF8_TO_TCHAR(Submix.value().c_str());
					USoundSubmix* SubmixObj = LoadObject<USoundSubmix>(nullptr, *SubmixPath);
					if (SubmixObj)
					{
						P.DefaultSubmix = SubmixObj;
						Changes += FString::Printf(TEXT(" default_submix=%s"), *SubmixObj->GetName());
						bModified = true;
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[WARN] configure -> could not load submix '%s'"), *SubmixPath));
					}
				}
			}

			if (bModified)
			{
				SoundClass->PostEditChange();
				SoundClass->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(%s)"), *Changes.TrimStart()));
				return sol::make_object(Lua, true);
			}

			Session.Log(TEXT("[OK] configure() -> nothing changed. Use help() to see valid keys."));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// add(type, params)
		// ================================================================
		AssetObj.set_function("add", [SoundClass, &Session](sol::table /*self*/,
			std::string Type, sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(SoundClass))
			{
				Session.Log(TEXT("[FAIL] add -> asset no longer valid"));
				return sol::lua_nil;
			}

			if (Type == "passive_sound_mix")
			{
				sol::optional<std::string> MixPath = Params.get<sol::optional<std::string>>("sound_mix");
				if (!MixPath.has_value() || MixPath.value().empty())
				{
					Session.Log(TEXT("[FAIL] add('passive_sound_mix') -> 'sound_mix' path required"));
					return sol::lua_nil;
				}

				FString Path = UTF8_TO_TCHAR(MixPath.value().c_str());
				USoundMix* Mix = LoadObject<USoundMix>(nullptr, *Path);
				if (!Mix)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add('passive_sound_mix') -> could not load SoundMix '%s'"), *Path));
					return sol::lua_nil;
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("SoundClass: Add Passive Sound Mix")));
				SoundClass->Modify();

				FPassiveSoundMixModifier NewMod;
				NewMod.SoundMix = Mix;
				NewMod.MinVolumeThreshold = static_cast<float>(Params.get_or("min_volume", 0.0));
				NewMod.MaxVolumeThreshold = static_cast<float>(Params.get_or("max_volume", 10.0));
				SoundClass->PassiveSoundMixModifiers.Add(NewMod);

				SoundClass->PostEditChange();
				SoundClass->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add('passive_sound_mix') -> added '%s' (min=%.1f, max=%.1f)"),
					*Mix->GetName(), NewMod.MinVolumeThreshold, NewMod.MaxVolumeThreshold));
				return sol::make_object(Lua, SoundClass->PassiveSoundMixModifiers.Num());
			}

			if (Type == "child")
			{
				sol::optional<std::string> ChildPath = Params.get<sol::optional<std::string>>("path");
				if (!ChildPath.has_value() || ChildPath.value().empty())
				{
					Session.Log(TEXT("[FAIL] add('child') -> 'path' required"));
					return sol::lua_nil;
				}

				FString Path = UTF8_TO_TCHAR(ChildPath.value().c_str());
				USoundClass* Child = LoadObject<USoundClass>(nullptr, *Path);
				if (!Child)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add('child') -> could not load SoundClass '%s'"), *Path));
					return sol::lua_nil;
				}

				if (Child == SoundClass)
				{
					Session.Log(TEXT("[FAIL] add('child') -> cannot add self as child"));
					return sol::lua_nil;
				}

				// Cycle detection: check if Child is an ancestor of SoundClass
				if (Child->RecurseCheckChild(SoundClass))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add('child') -> '%s' is an ancestor, would create cycle"), *Child->GetName()));
					return sol::lua_nil;
				}

				// Check for duplicates
				if (SoundClass->ChildClasses.Contains(Child))
				{
					Session.Log(FString::Printf(TEXT("[WARN] add('child') -> '%s' is already a child"), *Child->GetName()));
					return sol::make_object(Lua, SoundClass->ChildClasses.Num());
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("SoundClass: Add Child")));
				SoundClass->Modify();

				// SetParentClass removes from old parent and sets ParentClass,
				// but does NOT add to new parent's ChildClasses array
				Child->SetParentClass(SoundClass);
				SoundClass->ChildClasses.Add(Child);

				SoundClass->PostEditChange();
				SoundClass->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add('child') -> added '%s'"), *Child->GetName()));
				return sol::make_object(Lua, SoundClass->ChildClasses.Num());
			}

			Session.Log(FString::Printf(TEXT("[FAIL] add('%s') -> unknown type. Use: passive_sound_mix, child"),
				UTF8_TO_TCHAR(Type.c_str())));
			return sol::lua_nil;
		});

		// ================================================================
		// remove(type, params)
		// ================================================================
		AssetObj.set_function("remove", [SoundClass, &Session](sol::table /*self*/,
			std::string Type, sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(SoundClass))
			{
				Session.Log(TEXT("[FAIL] remove -> asset no longer valid"));
				return sol::lua_nil;
			}

			if (Type == "passive_sound_mix")
			{
				const FScopedTransaction Transaction(FText::FromString(TEXT("SoundClass: Remove Passive Sound Mix")));
				SoundClass->Modify();

				// By index (1-based)
				sol::optional<int> Idx = Params.get<sol::optional<int>>("index");
				if (Idx.has_value())
				{
					int32 ZeroIdx = Idx.value() - 1;
					if (ZeroIdx < 0 || ZeroIdx >= SoundClass->PassiveSoundMixModifiers.Num())
					{
						Session.Log(FString::Printf(TEXT("[FAIL] remove('passive_sound_mix') -> index %d out of range (1-%d)"),
							Idx.value(), SoundClass->PassiveSoundMixModifiers.Num()));
						return sol::lua_nil;
					}
					SoundClass->PassiveSoundMixModifiers.RemoveAt(ZeroIdx);
					SoundClass->PostEditChange();
					SoundClass->MarkPackageDirty();
					Session.Log(FString::Printf(TEXT("[OK] remove('passive_sound_mix') -> removed index %d"), Idx.value()));
					return sol::make_object(Lua, true);
				}

				// By sound mix path
				sol::optional<std::string> MixPath = Params.get<sol::optional<std::string>>("sound_mix");
				if (MixPath.has_value())
				{
					FString Path = UTF8_TO_TCHAR(MixPath.value().c_str());
					for (int32 i = SoundClass->PassiveSoundMixModifiers.Num() - 1; i >= 0; i--)
					{
						if (SoundClass->PassiveSoundMixModifiers[i].SoundMix &&
							SoundClass->PassiveSoundMixModifiers[i].SoundMix->GetPathName() == Path)
						{
							SoundClass->PassiveSoundMixModifiers.RemoveAt(i);
							SoundClass->PostEditChange();
							SoundClass->MarkPackageDirty();
							Session.Log(FString::Printf(TEXT("[OK] remove('passive_sound_mix') -> removed '%s'"), *Path));
							return sol::make_object(Lua, true);
						}
					}
					Session.Log(FString::Printf(TEXT("[FAIL] remove('passive_sound_mix') -> '%s' not found"), *Path));
					return sol::lua_nil;
				}

				Session.Log(TEXT("[FAIL] remove('passive_sound_mix') -> specify 'index' or 'sound_mix'"));
				return sol::lua_nil;
			}

			if (Type == "child")
			{
				const FScopedTransaction Transaction(FText::FromString(TEXT("SoundClass: Remove Child")));
				SoundClass->Modify();

				// By index (1-based)
				sol::optional<int> Idx = Params.get<sol::optional<int>>("index");
				if (Idx.has_value())
				{
					int32 ZeroIdx = Idx.value() - 1;
					if (ZeroIdx < 0 || ZeroIdx >= SoundClass->ChildClasses.Num())
					{
						Session.Log(FString::Printf(TEXT("[FAIL] remove('child') -> index %d out of range (1-%d)"),
							Idx.value(), SoundClass->ChildClasses.Num()));
						return sol::lua_nil;
					}
					USoundClass* Child = SoundClass->ChildClasses[ZeroIdx];
					if (Child)
					{
						Child->Modify();
						Child->ParentClass = nullptr;
					}
					SoundClass->ChildClasses.RemoveAt(ZeroIdx);
					SoundClass->PostEditChange();
					SoundClass->MarkPackageDirty();
					Session.Log(FString::Printf(TEXT("[OK] remove('child') -> removed index %d"), Idx.value()));
					return sol::make_object(Lua, true);
				}

				// By path
				sol::optional<std::string> ChildPath = Params.get<sol::optional<std::string>>("path");
				if (ChildPath.has_value())
				{
					FString Path = UTF8_TO_TCHAR(ChildPath.value().c_str());
					for (int32 i = SoundClass->ChildClasses.Num() - 1; i >= 0; i--)
					{
						if (SoundClass->ChildClasses[i] && SoundClass->ChildClasses[i]->GetPathName() == Path)
						{
							SoundClass->ChildClasses[i]->Modify();
							SoundClass->ChildClasses[i]->ParentClass = nullptr;
							SoundClass->ChildClasses.RemoveAt(i);
							SoundClass->PostEditChange();
							SoundClass->MarkPackageDirty();
							Session.Log(FString::Printf(TEXT("[OK] remove('child') -> removed '%s'"), *Path));
							return sol::make_object(Lua, true);
						}
					}
					Session.Log(FString::Printf(TEXT("[FAIL] remove('child') -> '%s' not found"), *Path));
					return sol::lua_nil;
				}

				Session.Log(TEXT("[FAIL] remove('child') -> specify 'index' or 'path'"));
				return sol::lua_nil;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] remove('%s') -> unknown type. Use: passive_sound_mix, child"),
				UTF8_TO_TCHAR(Type.c_str())));
			return sol::lua_nil;
		});
	});
}

REGISTER_LUA_BINDING(SoundClass, SoundClassDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindSoundClass(Lua, Session);
});
