// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Tools/NeoStackToolUtils.h"
#include "ScopedTransaction.h"

#include "Sound/SoundAttenuation.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// HELPERS
// ============================================================================

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

static EAttenuationShape::Type StringToAttenuationShape(const FString& Str)
{
	if (Str.Equals(TEXT("capsule"), ESearchCase::IgnoreCase)) return EAttenuationShape::Capsule;
	if (Str.Equals(TEXT("box"), ESearchCase::IgnoreCase))     return EAttenuationShape::Box;
	if (Str.Equals(TEXT("cone"), ESearchCase::IgnoreCase))    return EAttenuationShape::Cone;
	return EAttenuationShape::Sphere;
}

static const char* DistanceModelToString(EAttenuationDistanceModel Model)
{
	switch (Model)
	{
	case EAttenuationDistanceModel::Linear:       return "linear";
	case EAttenuationDistanceModel::Logarithmic:  return "logarithmic";
	case EAttenuationDistanceModel::Inverse:      return "inverse";
	case EAttenuationDistanceModel::LogReverse:    return "log_reverse";
	case EAttenuationDistanceModel::NaturalSound:  return "natural_sound";
	case EAttenuationDistanceModel::Custom:        return "custom";
	default:                                       return "linear";
	}
}

static EAttenuationDistanceModel StringToDistanceModel(const FString& Str)
{
	if (Str.Equals(TEXT("logarithmic"), ESearchCase::IgnoreCase))  return EAttenuationDistanceModel::Logarithmic;
	if (Str.Equals(TEXT("inverse"), ESearchCase::IgnoreCase))      return EAttenuationDistanceModel::Inverse;
	if (Str.Equals(TEXT("log_reverse"), ESearchCase::IgnoreCase))  return EAttenuationDistanceModel::LogReverse;
	if (Str.Equals(TEXT("natural_sound"), ESearchCase::IgnoreCase)) return EAttenuationDistanceModel::NaturalSound;
	if (Str.Equals(TEXT("custom"), ESearchCase::IgnoreCase))       return EAttenuationDistanceModel::Custom;
	return EAttenuationDistanceModel::Linear;
}

static const char* FalloffModeToString(ENaturalSoundFalloffMode Mode)
{
	switch (Mode)
	{
	case ENaturalSoundFalloffMode::Continues: return "continues";
	case ENaturalSoundFalloffMode::Silent:    return "silent";
	case ENaturalSoundFalloffMode::Hold:      return "hold";
	default:                                  return "continues";
	}
}

static ENaturalSoundFalloffMode StringToFalloffMode(const FString& Str)
{
	if (Str.Equals(TEXT("silent"), ESearchCase::IgnoreCase)) return ENaturalSoundFalloffMode::Silent;
	if (Str.Equals(TEXT("hold"), ESearchCase::IgnoreCase))   return ENaturalSoundFalloffMode::Hold;
	return ENaturalSoundFalloffMode::Continues;
}

static const char* SpatializationAlgorithmToString(ESoundSpatializationAlgorithm Algo)
{
	switch (Algo)
	{
	case SPATIALIZATION_Default: return "panning";
	case SPATIALIZATION_HRTF:    return "hrtf";
	default:                     return "panning";
	}
}

static ESoundSpatializationAlgorithm StringToSpatializationAlgorithm(const FString& Str)
{
	if (Str.Equals(TEXT("hrtf"), ESearchCase::IgnoreCase) || Str.Equals(TEXT("plugin"), ESearchCase::IgnoreCase))
		return SPATIALIZATION_HRTF;
	return SPATIALIZATION_Default;
}

static const char* AbsorptionMethodToString(EAirAbsorptionMethod Method)
{
	switch (Method)
	{
	case EAirAbsorptionMethod::Linear:      return "linear";
	case EAirAbsorptionMethod::CustomCurve: return "custom_curve";
	default:                                return "linear";
	}
}

static EAirAbsorptionMethod StringToAbsorptionMethod(const FString& Str)
{
	if (Str.Equals(TEXT("custom_curve"), ESearchCase::IgnoreCase)) return EAirAbsorptionMethod::CustomCurve;
	return EAirAbsorptionMethod::Linear;
}

static const char* ReverbSendMethodToString(EReverbSendMethod Method)
{
	switch (Method)
	{
	case EReverbSendMethod::Linear:      return "linear";
	case EReverbSendMethod::CustomCurve: return "custom_curve";
	case EReverbSendMethod::Manual:      return "manual";
	default:                             return "linear";
	}
}

static EReverbSendMethod StringToReverbSendMethod(const FString& Str)
{
	if (Str.Equals(TEXT("custom_curve"), ESearchCase::IgnoreCase)) return EReverbSendMethod::CustomCurve;
	if (Str.Equals(TEXT("manual"), ESearchCase::IgnoreCase))       return EReverbSendMethod::Manual;
	return EReverbSendMethod::Linear;
}

static const char* PriorityAttenuationMethodToString(EPriorityAttenuationMethod Method)
{
	switch (Method)
	{
	case EPriorityAttenuationMethod::Linear:      return "linear";
	case EPriorityAttenuationMethod::CustomCurve: return "custom_curve";
	case EPriorityAttenuationMethod::Manual:      return "manual";
	default:                                      return "linear";
	}
}

static EPriorityAttenuationMethod StringToPriorityAttenuationMethod(const FString& Str)
{
	if (Str.Equals(TEXT("custom_curve"), ESearchCase::IgnoreCase)) return EPriorityAttenuationMethod::CustomCurve;
	if (Str.Equals(TEXT("manual"), ESearchCase::IgnoreCase))       return EPriorityAttenuationMethod::Manual;
	return EPriorityAttenuationMethod::Linear;
}

static const char* NonSpatRadiusModeToString(ENonSpatializedRadiusSpeakerMapMode Mode)
{
	switch (Mode)
	{
	case ENonSpatializedRadiusSpeakerMapMode::OmniDirectional: return "omni_directional";
	case ENonSpatializedRadiusSpeakerMapMode::Direct2D:        return "direct_2d";
	case ENonSpatializedRadiusSpeakerMapMode::Surround2D:      return "surround_2d";
	default:                                                    return "omni_directional";
	}
}

static ENonSpatializedRadiusSpeakerMapMode StringToNonSpatRadiusMode(const FString& Str)
{
	if (Str.Equals(TEXT("direct_2d"), ESearchCase::IgnoreCase))    return ENonSpatializedRadiusSpeakerMapMode::Direct2D;
	if (Str.Equals(TEXT("surround_2d"), ESearchCase::IgnoreCase))  return ENonSpatializedRadiusSpeakerMapMode::Surround2D;
	return ENonSpatializedRadiusSpeakerMapMode::OmniDirectional;
}

static const char* OcclusionTraceChannelToString(ECollisionChannel Channel)
{
	switch (Channel)
	{
	case ECC_Visibility:    return "visibility";
	case ECC_Camera:        return "camera";
	case ECC_WorldStatic:   return "world_static";
	case ECC_WorldDynamic:  return "world_dynamic";
	case ECC_Pawn:          return "pawn";
	case ECC_PhysicsBody:   return "physics_body";
	case ECC_Vehicle:       return "vehicle";
	case ECC_Destructible:  return "destructible";
	default:                return "visibility";
	}
}

static ECollisionChannel StringToOcclusionTraceChannel(const FString& Str)
{
	if (Str.Equals(TEXT("camera"), ESearchCase::IgnoreCase))        return ECC_Camera;
	if (Str.Equals(TEXT("world_static"), ESearchCase::IgnoreCase))  return ECC_WorldStatic;
	if (Str.Equals(TEXT("world_dynamic"), ESearchCase::IgnoreCase)) return ECC_WorldDynamic;
	if (Str.Equals(TEXT("pawn"), ESearchCase::IgnoreCase))          return ECC_Pawn;
	if (Str.Equals(TEXT("physics_body"), ESearchCase::IgnoreCase))  return ECC_PhysicsBody;
	if (Str.Equals(TEXT("vehicle"), ESearchCase::IgnoreCase))       return ECC_Vehicle;
	if (Str.Equals(TEXT("destructible"), ESearchCase::IgnoreCase))  return ECC_Destructible;
	return ECC_Visibility;
}

// ============================================================================
// BINDING
// ============================================================================

static TArray<FLuaFunctionDoc> SoundAttenuationDocs = {};

static void BindSoundAttenuation(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_sound_attenuation", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		USoundAttenuation* Asset = LoadObject<USoundAttenuation>(nullptr, *FPath);
		if (!Asset) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"SoundAttenuation enrichment methods:\n"
			"\n"
			"info() — structured summary of all attenuation settings\n"
			"\n"
			"configure(params) — set attenuation properties:\n"
			"  Shape & distance: shape (sphere/capsule/box/cone), shape_extents ({x,y,z}),\n"
			"    falloff_distance, distance_algorithm (linear/logarithmic/inverse/log_reverse/natural_sound/custom),\n"
			"    falloff_mode (continues/silent/hold), db_attenuation_at_max, cone_offset,\n"
			"    cone_sphere_radius, cone_sphere_falloff_distance\n"
			"  Spatialization: attenuate (bool), spatialize (bool),\n"
			"    spatialization_algorithm (panning/hrtf), binaural_radius, stereo_spread,\n"
			"    non_spatialized_radius_start, non_spatialized_radius_end,\n"
			"    non_spatialized_radius_mode (omni_directional/direct_2d/surround_2d),\n"
			"    normalize_stereo (bool)\n"
			"  Listener focus: enable_listener_focus (bool), focus_azimuth, non_focus_azimuth,\n"
			"    focus_distance_scale, non_focus_distance_scale,\n"
			"    focus_volume_attenuation, non_focus_volume_attenuation,\n"
			"    focus_priority_scale, non_focus_priority_scale,\n"
			"    enable_focus_interpolation (bool), focus_attack_interp_speed, focus_release_interp_speed\n"
			"  Air absorption: air_absorption (bool), absorption_method (linear/custom_curve),\n"
			"    lpf_radius_min, lpf_radius_max, lpf_frequency_at_min, lpf_frequency_at_max,\n"
			"    hpf_frequency_at_min, hpf_frequency_at_max, enable_log_frequency_scaling (bool)\n"
			"  Occlusion: enable_occlusion (bool), occlusion_lpf_frequency,\n"
			"    occlusion_volume_attenuation, occlusion_interpolation_time,\n"
			"    use_complex_collision_for_occlusion (bool),\n"
			"    occlusion_trace_channel (visibility/camera/world_static/world_dynamic/pawn/physics_body)\n"
			"  Reverb: enable_reverb_send (bool), reverb_send_method (linear/custom_curve/manual),\n"
			"    reverb_wet_level_min/max, reverb_distance_min/max, manual_reverb_send_level\n"
			"  Priority: enable_priority_attenuation (bool),\n"
			"    priority_attenuation_method (linear/custom_curve/manual),\n"
			"    priority_attenuation_min, priority_attenuation_max,\n"
			"    priority_attenuation_distance_min, priority_attenuation_distance_max,\n"
			"    manual_priority_attenuation\n";

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

			const FSoundAttenuationSettings& A = Asset->Attenuation;
			sol::table R = Lua.create_table();

			// Shape & distance
			R["shape"] = AttenuationShapeToString(A.AttenuationShape);
			sol::table Extents = Lua.create_table();
			Extents["x"] = A.AttenuationShapeExtents.X;
			Extents["y"] = A.AttenuationShapeExtents.Y;
			Extents["z"] = A.AttenuationShapeExtents.Z;
			R["shape_extents"] = Extents;
			R["falloff_distance"] = A.FalloffDistance;
			R["distance_algorithm"] = DistanceModelToString(A.DistanceAlgorithm);
			R["falloff_mode"] = FalloffModeToString(A.FalloffMode);
			R["db_attenuation_at_max"] = A.dBAttenuationAtMax;
			R["cone_offset"] = A.ConeOffset;
			R["cone_sphere_radius"] = A.ConeSphereRadius;
			R["cone_sphere_falloff_distance"] = A.ConeSphereFalloffDistance;

			// Attenuation & spatialization
			R["attenuate"] = static_cast<bool>(A.bAttenuate);
			R["spatialize"] = static_cast<bool>(A.bSpatialize);
			R["spatialization_algorithm"] = SpatializationAlgorithmToString(A.SpatializationAlgorithm);
			R["binaural_radius"] = A.BinauralRadius;
			R["stereo_spread"] = A.StereoSpread;
			R["non_spatialized_radius_start"] = A.NonSpatializedRadiusStart;
			R["non_spatialized_radius_end"] = A.NonSpatializedRadiusEnd;
			R["normalize_stereo"] = static_cast<bool>(A.bApplyNormalizationToStereoSounds);
			R["non_spatialized_radius_mode"] = NonSpatRadiusModeToString(A.NonSpatializedRadiusMode);

			// Listener focus
			R["enable_listener_focus"] = static_cast<bool>(A.bEnableListenerFocus);
			R["focus_azimuth"] = A.FocusAzimuth;
			R["non_focus_azimuth"] = A.NonFocusAzimuth;
			R["focus_distance_scale"] = A.FocusDistanceScale;
			R["non_focus_distance_scale"] = A.NonFocusDistanceScale;
			R["focus_priority_scale"] = A.FocusPriorityScale;
			R["non_focus_priority_scale"] = A.NonFocusPriorityScale;
			R["focus_volume_attenuation"] = A.FocusVolumeAttenuation;
			R["non_focus_volume_attenuation"] = A.NonFocusVolumeAttenuation;
			R["enable_focus_interpolation"] = static_cast<bool>(A.bEnableFocusInterpolation);
			R["focus_attack_interp_speed"] = A.FocusAttackInterpSpeed;
			R["focus_release_interp_speed"] = A.FocusReleaseInterpSpeed;

			// Air absorption / LPF / HPF
			R["air_absorption"] = static_cast<bool>(A.bAttenuateWithLPF);
			R["absorption_method"] = AbsorptionMethodToString(A.AbsorptionMethod);
			R["lpf_radius_min"] = A.LPFRadiusMin;
			R["lpf_radius_max"] = A.LPFRadiusMax;
			R["lpf_frequency_at_min"] = A.LPFFrequencyAtMin;
			R["lpf_frequency_at_max"] = A.LPFFrequencyAtMax;
			R["hpf_frequency_at_min"] = A.HPFFrequencyAtMin;
			R["hpf_frequency_at_max"] = A.HPFFrequencyAtMax;
			R["enable_log_frequency_scaling"] = static_cast<bool>(A.bEnableLogFrequencyScaling);

			// Occlusion
			R["enable_occlusion"] = static_cast<bool>(A.bEnableOcclusion);
			R["use_complex_collision_for_occlusion"] = static_cast<bool>(A.bUseComplexCollisionForOcclusion);
			R["occlusion_trace_channel"] = OcclusionTraceChannelToString(static_cast<ECollisionChannel>(A.OcclusionTraceChannel.GetValue()));
			R["occlusion_lpf_frequency"] = A.OcclusionLowPassFilterFrequency;
			R["occlusion_volume_attenuation"] = A.OcclusionVolumeAttenuation;
			R["occlusion_interpolation_time"] = A.OcclusionInterpolationTime;

			// Reverb send
			R["enable_reverb_send"] = static_cast<bool>(A.bEnableReverbSend);
			R["reverb_send_method"] = ReverbSendMethodToString(A.ReverbSendMethod);
			R["reverb_wet_level_min"] = A.ReverbWetLevelMin;
			R["reverb_wet_level_max"] = A.ReverbWetLevelMax;
			R["reverb_distance_min"] = A.ReverbDistanceMin;
			R["reverb_distance_max"] = A.ReverbDistanceMax;
			R["manual_reverb_send_level"] = A.ManualReverbSendLevel;

			// Priority attenuation
			R["enable_priority_attenuation"] = static_cast<bool>(A.bEnablePriorityAttenuation);
			R["priority_attenuation_method"] = PriorityAttenuationMethodToString(A.PriorityAttenuationMethod);
			R["priority_attenuation_min"] = A.PriorityAttenuationMin;
			R["priority_attenuation_max"] = A.PriorityAttenuationMax;
			R["priority_attenuation_distance_min"] = A.PriorityAttenuationDistanceMin;
			R["priority_attenuation_distance_max"] = A.PriorityAttenuationDistanceMax;
			R["manual_priority_attenuation"] = A.ManualPriorityAttenuation;

			Session.Log(FString::Printf(TEXT("[OK] info() -> SoundAttenuation, shape=%hs, falloff=%.0f, spatialize=%s"),
				AttenuationShapeToString(A.AttenuationShape),
				(double)A.FalloffDistance,
				A.bSpatialize ? TEXT("true") : TEXT("false")));
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

			const FScopedTransaction Transaction(FText::FromString(TEXT("SoundAttenuation: Configure")));
			Asset->Modify();
			FSoundAttenuationSettings& A = Asset->Attenuation;
			bool bModified = false;
			FString Changes;

			// --- Shape & distance ---

			sol::optional<std::string> Shape = Params.get<sol::optional<std::string>>("shape");
			if (Shape.has_value())
			{
				FString ShapeStr = UTF8_TO_TCHAR(Shape.value().c_str());
				A.AttenuationShape = StringToAttenuationShape(ShapeStr);
				Changes += FString::Printf(TEXT(" shape=%s"), *ShapeStr.ToLower());
				bModified = true;
			}

			sol::optional<sol::table> ShapeExtents = Params.get<sol::optional<sol::table>>("shape_extents");
			if (ShapeExtents.has_value())
			{
				sol::table E = ShapeExtents.value();
				A.AttenuationShapeExtents.X = FMath::Max(0.0, E.get_or("x", A.AttenuationShapeExtents.X));
				A.AttenuationShapeExtents.Y = FMath::Max(0.0, E.get_or("y", A.AttenuationShapeExtents.Y));
				A.AttenuationShapeExtents.Z = FMath::Max(0.0, E.get_or("z", A.AttenuationShapeExtents.Z));
				Changes += FString::Printf(TEXT(" shape_extents=(%.0f,%.0f,%.0f)"),
					A.AttenuationShapeExtents.X, A.AttenuationShapeExtents.Y, A.AttenuationShapeExtents.Z);
				bModified = true;
			}

			sol::optional<double> FalloffDist = Params.get<sol::optional<double>>("falloff_distance");
			if (FalloffDist.has_value())
			{
				A.FalloffDistance = FMath::Max(0.0f, static_cast<float>(FalloffDist.value()));
				Changes += FString::Printf(TEXT(" falloff_distance=%.0f"), (double)A.FalloffDistance);
				bModified = true;
			}

			sol::optional<std::string> DistAlgo = Params.get<sol::optional<std::string>>("distance_algorithm");
			if (DistAlgo.has_value())
			{
				FString AlgoStr = UTF8_TO_TCHAR(DistAlgo.value().c_str());
				A.DistanceAlgorithm = StringToDistanceModel(AlgoStr);
				Changes += FString::Printf(TEXT(" distance_algorithm=%s"), *AlgoStr.ToLower());
				bModified = true;
			}

			sol::optional<std::string> FalloffModeOpt = Params.get<sol::optional<std::string>>("falloff_mode");
			if (FalloffModeOpt.has_value())
			{
				FString ModeStr = UTF8_TO_TCHAR(FalloffModeOpt.value().c_str());
				A.FalloffMode = StringToFalloffMode(ModeStr);
				Changes += FString::Printf(TEXT(" falloff_mode=%s"), *ModeStr.ToLower());
				bModified = true;
			}

			sol::optional<double> DbAtMax = Params.get<sol::optional<double>>("db_attenuation_at_max");
			if (DbAtMax.has_value())
			{
				A.dBAttenuationAtMax = FMath::Clamp(static_cast<float>(DbAtMax.value()), -60.0f, 0.0f);
				Changes += FString::Printf(TEXT(" db_attenuation_at_max=%.1f"), (double)A.dBAttenuationAtMax);
				bModified = true;
			}

			sol::optional<double> ConeOff = Params.get<sol::optional<double>>("cone_offset");
			if (ConeOff.has_value())
			{
				A.ConeOffset = FMath::Max(0.0f, static_cast<float>(ConeOff.value()));
				Changes += FString::Printf(TEXT(" cone_offset=%.0f"), (double)A.ConeOffset);
				bModified = true;
			}

			sol::optional<double> ConeSphRad = Params.get<sol::optional<double>>("cone_sphere_radius");
			if (ConeSphRad.has_value())
			{
				A.ConeSphereRadius = FMath::Max(0.0f, static_cast<float>(ConeSphRad.value()));
				Changes += FString::Printf(TEXT(" cone_sphere_radius=%.0f"), (double)A.ConeSphereRadius);
				bModified = true;
			}

			sol::optional<double> ConeSphFalloff = Params.get<sol::optional<double>>("cone_sphere_falloff_distance");
			if (ConeSphFalloff.has_value())
			{
				A.ConeSphereFalloffDistance = FMath::Max(0.0f, static_cast<float>(ConeSphFalloff.value()));
				Changes += FString::Printf(TEXT(" cone_sphere_falloff_distance=%.0f"), (double)A.ConeSphereFalloffDistance);
				bModified = true;
			}

			// --- Attenuation & spatialization ---

			sol::optional<bool> Attenuate = Params.get<sol::optional<bool>>("attenuate");
			if (Attenuate.has_value())
			{
				A.bAttenuate = Attenuate.value();
				Changes += FString::Printf(TEXT(" attenuate=%s"), Attenuate.value() ? TEXT("true") : TEXT("false"));
				bModified = true;
			}

			sol::optional<bool> Spatialize = Params.get<sol::optional<bool>>("spatialize");
			if (Spatialize.has_value())
			{
				A.bSpatialize = Spatialize.value();
				Changes += FString::Printf(TEXT(" spatialize=%s"), Spatialize.value() ? TEXT("true") : TEXT("false"));
				bModified = true;
			}

			sol::optional<std::string> SpatAlgo = Params.get<sol::optional<std::string>>("spatialization_algorithm");
			if (SpatAlgo.has_value())
			{
				FString AlgoStr = UTF8_TO_TCHAR(SpatAlgo.value().c_str());
				A.SpatializationAlgorithm = StringToSpatializationAlgorithm(AlgoStr);
				Changes += FString::Printf(TEXT(" spatialization_algorithm=%s"), *AlgoStr.ToLower());
				bModified = true;
			}

			sol::optional<double> BinauralRad = Params.get<sol::optional<double>>("binaural_radius");
			if (BinauralRad.has_value())
			{
				A.BinauralRadius = FMath::Max(0.0f, static_cast<float>(BinauralRad.value()));
				Changes += FString::Printf(TEXT(" binaural_radius=%.0f"), (double)A.BinauralRadius);
				bModified = true;
			}

			sol::optional<double> StereoSpr = Params.get<sol::optional<double>>("stereo_spread");
			if (StereoSpr.has_value())
			{
				A.StereoSpread = FMath::Max(0.0f, static_cast<float>(StereoSpr.value()));
				Changes += FString::Printf(TEXT(" stereo_spread=%.0f"), (double)A.StereoSpread);
				bModified = true;
			}

			sol::optional<double> NonSpatStart = Params.get<sol::optional<double>>("non_spatialized_radius_start");
			if (NonSpatStart.has_value())
			{
				A.NonSpatializedRadiusStart = FMath::Max(0.0f, static_cast<float>(NonSpatStart.value()));
				Changes += FString::Printf(TEXT(" non_spatialized_radius_start=%.0f"), (double)A.NonSpatializedRadiusStart);
				bModified = true;
			}

			sol::optional<double> NonSpatEnd = Params.get<sol::optional<double>>("non_spatialized_radius_end");
			if (NonSpatEnd.has_value())
			{
				A.NonSpatializedRadiusEnd = FMath::Max(0.0f, static_cast<float>(NonSpatEnd.value()));
				Changes += FString::Printf(TEXT(" non_spatialized_radius_end=%.0f"), (double)A.NonSpatializedRadiusEnd);
				bModified = true;
			}

			sol::optional<bool> NormStereo = Params.get<sol::optional<bool>>("normalize_stereo");
			if (NormStereo.has_value())
			{
				A.bApplyNormalizationToStereoSounds = NormStereo.value();
				Changes += FString::Printf(TEXT(" normalize_stereo=%s"), NormStereo.value() ? TEXT("true") : TEXT("false"));
				bModified = true;
			}

			sol::optional<std::string> NonSpatMode = Params.get<sol::optional<std::string>>("non_spatialized_radius_mode");
			if (NonSpatMode.has_value())
			{
				FString ModeStr = UTF8_TO_TCHAR(NonSpatMode.value().c_str());
				A.NonSpatializedRadiusMode = StringToNonSpatRadiusMode(ModeStr);
				Changes += FString::Printf(TEXT(" non_spatialized_radius_mode=%s"), *ModeStr.ToLower());
				bModified = true;
			}

			// --- Listener focus ---

			sol::optional<bool> EnableFocus = Params.get<sol::optional<bool>>("enable_listener_focus");
			if (EnableFocus.has_value())
			{
				A.bEnableListenerFocus = EnableFocus.value();
				Changes += FString::Printf(TEXT(" enable_listener_focus=%s"), EnableFocus.value() ? TEXT("true") : TEXT("false"));
				bModified = true;
			}

			sol::optional<double> FocusAz = Params.get<sol::optional<double>>("focus_azimuth");
			if (FocusAz.has_value())
			{
				A.FocusAzimuth = static_cast<float>(FocusAz.value());
				Changes += FString::Printf(TEXT(" focus_azimuth=%.1f"), (double)A.FocusAzimuth);
				bModified = true;
			}

			sol::optional<double> NonFocusAz = Params.get<sol::optional<double>>("non_focus_azimuth");
			if (NonFocusAz.has_value())
			{
				A.NonFocusAzimuth = static_cast<float>(NonFocusAz.value());
				Changes += FString::Printf(TEXT(" non_focus_azimuth=%.1f"), (double)A.NonFocusAzimuth);
				bModified = true;
			}

			sol::optional<double> FocusDistScale = Params.get<sol::optional<double>>("focus_distance_scale");
			if (FocusDistScale.has_value())
			{
				A.FocusDistanceScale = FMath::Max(0.0f, static_cast<float>(FocusDistScale.value()));
				Changes += FString::Printf(TEXT(" focus_distance_scale=%.2f"), (double)A.FocusDistanceScale);
				bModified = true;
			}

			sol::optional<double> NonFocusDistScale = Params.get<sol::optional<double>>("non_focus_distance_scale");
			if (NonFocusDistScale.has_value())
			{
				A.NonFocusDistanceScale = FMath::Max(0.0f, static_cast<float>(NonFocusDistScale.value()));
				Changes += FString::Printf(TEXT(" non_focus_distance_scale=%.2f"), (double)A.NonFocusDistanceScale);
				bModified = true;
			}

			sol::optional<double> FocusVolAtten = Params.get<sol::optional<double>>("focus_volume_attenuation");
			if (FocusVolAtten.has_value())
			{
				A.FocusVolumeAttenuation = FMath::Max(0.0f, static_cast<float>(FocusVolAtten.value()));
				Changes += FString::Printf(TEXT(" focus_volume_attenuation=%.2f"), (double)A.FocusVolumeAttenuation);
				bModified = true;
			}

			sol::optional<double> NonFocusVolAtten = Params.get<sol::optional<double>>("non_focus_volume_attenuation");
			if (NonFocusVolAtten.has_value())
			{
				A.NonFocusVolumeAttenuation = FMath::Max(0.0f, static_cast<float>(NonFocusVolAtten.value()));
				Changes += FString::Printf(TEXT(" non_focus_volume_attenuation=%.2f"), (double)A.NonFocusVolumeAttenuation);
				bModified = true;
			}

			sol::optional<double> FocusPriScale = Params.get<sol::optional<double>>("focus_priority_scale");
			if (FocusPriScale.has_value())
			{
				A.FocusPriorityScale = FMath::Max(0.0f, static_cast<float>(FocusPriScale.value()));
				Changes += FString::Printf(TEXT(" focus_priority_scale=%.2f"), (double)A.FocusPriorityScale);
				bModified = true;
			}

			sol::optional<double> NonFocusPriScale = Params.get<sol::optional<double>>("non_focus_priority_scale");
			if (NonFocusPriScale.has_value())
			{
				A.NonFocusPriorityScale = FMath::Max(0.0f, static_cast<float>(NonFocusPriScale.value()));
				Changes += FString::Printf(TEXT(" non_focus_priority_scale=%.2f"), (double)A.NonFocusPriorityScale);
				bModified = true;
			}

			sol::optional<bool> EnableFocusInterp = Params.get<sol::optional<bool>>("enable_focus_interpolation");
			if (EnableFocusInterp.has_value())
			{
				A.bEnableFocusInterpolation = EnableFocusInterp.value();
				Changes += FString::Printf(TEXT(" enable_focus_interpolation=%s"), EnableFocusInterp.value() ? TEXT("true") : TEXT("false"));
				bModified = true;
			}

			sol::optional<double> FocusAttackSpeed = Params.get<sol::optional<double>>("focus_attack_interp_speed");
			if (FocusAttackSpeed.has_value())
			{
				A.FocusAttackInterpSpeed = FMath::Max(0.0f, static_cast<float>(FocusAttackSpeed.value()));
				Changes += FString::Printf(TEXT(" focus_attack_interp_speed=%.2f"), (double)A.FocusAttackInterpSpeed);
				bModified = true;
			}

			sol::optional<double> FocusReleaseSpeed = Params.get<sol::optional<double>>("focus_release_interp_speed");
			if (FocusReleaseSpeed.has_value())
			{
				A.FocusReleaseInterpSpeed = FMath::Max(0.0f, static_cast<float>(FocusReleaseSpeed.value()));
				Changes += FString::Printf(TEXT(" focus_release_interp_speed=%.2f"), (double)A.FocusReleaseInterpSpeed);
				bModified = true;
			}

			// --- Air absorption ---

			sol::optional<bool> AirAbsorption = Params.get<sol::optional<bool>>("air_absorption");
			if (AirAbsorption.has_value())
			{
				A.bAttenuateWithLPF = AirAbsorption.value();
				Changes += FString::Printf(TEXT(" air_absorption=%s"), AirAbsorption.value() ? TEXT("true") : TEXT("false"));
				bModified = true;
			}

			sol::optional<std::string> AbsMethod = Params.get<sol::optional<std::string>>("absorption_method");
			if (AbsMethod.has_value())
			{
				FString MethodStr = UTF8_TO_TCHAR(AbsMethod.value().c_str());
				A.AbsorptionMethod = StringToAbsorptionMethod(MethodStr);
				Changes += FString::Printf(TEXT(" absorption_method=%s"), *MethodStr.ToLower());
				bModified = true;
			}

			sol::optional<double> LpfRadMin = Params.get<sol::optional<double>>("lpf_radius_min");
			if (LpfRadMin.has_value())
			{
				A.LPFRadiusMin = static_cast<float>(LpfRadMin.value());
				Changes += FString::Printf(TEXT(" lpf_radius_min=%.0f"), (double)A.LPFRadiusMin);
				bModified = true;
			}

			sol::optional<double> LpfRadMax = Params.get<sol::optional<double>>("lpf_radius_max");
			if (LpfRadMax.has_value())
			{
				A.LPFRadiusMax = static_cast<float>(LpfRadMax.value());
				Changes += FString::Printf(TEXT(" lpf_radius_max=%.0f"), (double)A.LPFRadiusMax);
				bModified = true;
			}

			sol::optional<double> LpfFreqMin = Params.get<sol::optional<double>>("lpf_frequency_at_min");
			if (LpfFreqMin.has_value())
			{
				A.LPFFrequencyAtMin = static_cast<float>(LpfFreqMin.value());
				Changes += FString::Printf(TEXT(" lpf_frequency_at_min=%.0f"), (double)A.LPFFrequencyAtMin);
				bModified = true;
			}

			sol::optional<double> LpfFreqMax = Params.get<sol::optional<double>>("lpf_frequency_at_max");
			if (LpfFreqMax.has_value())
			{
				A.LPFFrequencyAtMax = static_cast<float>(LpfFreqMax.value());
				Changes += FString::Printf(TEXT(" lpf_frequency_at_max=%.0f"), (double)A.LPFFrequencyAtMax);
				bModified = true;
			}

			sol::optional<double> HpfFreqMin = Params.get<sol::optional<double>>("hpf_frequency_at_min");
			if (HpfFreqMin.has_value())
			{
				A.HPFFrequencyAtMin = static_cast<float>(HpfFreqMin.value());
				Changes += FString::Printf(TEXT(" hpf_frequency_at_min=%.0f"), (double)A.HPFFrequencyAtMin);
				bModified = true;
			}

			sol::optional<double> HpfFreqMax = Params.get<sol::optional<double>>("hpf_frequency_at_max");
			if (HpfFreqMax.has_value())
			{
				A.HPFFrequencyAtMax = static_cast<float>(HpfFreqMax.value());
				Changes += FString::Printf(TEXT(" hpf_frequency_at_max=%.0f"), (double)A.HPFFrequencyAtMax);
				bModified = true;
			}

			sol::optional<bool> LogFreqScaling = Params.get<sol::optional<bool>>("enable_log_frequency_scaling");
			if (LogFreqScaling.has_value())
			{
				A.bEnableLogFrequencyScaling = LogFreqScaling.value();
				Changes += FString::Printf(TEXT(" enable_log_frequency_scaling=%s"), LogFreqScaling.value() ? TEXT("true") : TEXT("false"));
				bModified = true;
			}

			// --- Occlusion ---

			sol::optional<bool> EnableOcclusion = Params.get<sol::optional<bool>>("enable_occlusion");
			if (EnableOcclusion.has_value())
			{
				A.bEnableOcclusion = EnableOcclusion.value();
				Changes += FString::Printf(TEXT(" enable_occlusion=%s"), EnableOcclusion.value() ? TEXT("true") : TEXT("false"));
				bModified = true;
			}

			sol::optional<bool> ComplexOcclusion = Params.get<sol::optional<bool>>("use_complex_collision_for_occlusion");
			if (ComplexOcclusion.has_value())
			{
				A.bUseComplexCollisionForOcclusion = ComplexOcclusion.value();
				Changes += FString::Printf(TEXT(" use_complex_collision_for_occlusion=%s"), ComplexOcclusion.value() ? TEXT("true") : TEXT("false"));
				bModified = true;
			}

			sol::optional<std::string> OccTraceChannel = Params.get<sol::optional<std::string>>("occlusion_trace_channel");
			if (OccTraceChannel.has_value())
			{
				FString ChannelStr = UTF8_TO_TCHAR(OccTraceChannel.value().c_str());
				A.OcclusionTraceChannel = StringToOcclusionTraceChannel(ChannelStr);
				Changes += FString::Printf(TEXT(" occlusion_trace_channel=%s"), *ChannelStr.ToLower());
				bModified = true;
			}

			sol::optional<double> OccLpf = Params.get<sol::optional<double>>("occlusion_lpf_frequency");
			if (OccLpf.has_value())
			{
				A.OcclusionLowPassFilterFrequency = FMath::Max(0.0f, static_cast<float>(OccLpf.value()));
				Changes += FString::Printf(TEXT(" occlusion_lpf_frequency=%.0f"), (double)A.OcclusionLowPassFilterFrequency);
				bModified = true;
			}

			sol::optional<double> OccVolAtten = Params.get<sol::optional<double>>("occlusion_volume_attenuation");
			if (OccVolAtten.has_value())
			{
				A.OcclusionVolumeAttenuation = FMath::Max(0.0f, static_cast<float>(OccVolAtten.value()));
				Changes += FString::Printf(TEXT(" occlusion_volume_attenuation=%.2f"), (double)A.OcclusionVolumeAttenuation);
				bModified = true;
			}

			sol::optional<double> OccInterp = Params.get<sol::optional<double>>("occlusion_interpolation_time");
			if (OccInterp.has_value())
			{
				A.OcclusionInterpolationTime = FMath::Max(0.0f, static_cast<float>(OccInterp.value()));
				Changes += FString::Printf(TEXT(" occlusion_interpolation_time=%.2f"), (double)A.OcclusionInterpolationTime);
				bModified = true;
			}

			// --- Reverb send ---

			sol::optional<bool> EnableReverb = Params.get<sol::optional<bool>>("enable_reverb_send");
			if (EnableReverb.has_value())
			{
				A.bEnableReverbSend = EnableReverb.value();
				Changes += FString::Printf(TEXT(" enable_reverb_send=%s"), EnableReverb.value() ? TEXT("true") : TEXT("false"));
				bModified = true;
			}

			sol::optional<std::string> RevMethod = Params.get<sol::optional<std::string>>("reverb_send_method");
			if (RevMethod.has_value())
			{
				FString MethodStr = UTF8_TO_TCHAR(RevMethod.value().c_str());
				A.ReverbSendMethod = StringToReverbSendMethod(MethodStr);
				Changes += FString::Printf(TEXT(" reverb_send_method=%s"), *MethodStr.ToLower());
				bModified = true;
			}

			sol::optional<double> RevWetMin = Params.get<sol::optional<double>>("reverb_wet_level_min");
			if (RevWetMin.has_value())
			{
				A.ReverbWetLevelMin = static_cast<float>(RevWetMin.value());
				Changes += FString::Printf(TEXT(" reverb_wet_level_min=%.2f"), (double)A.ReverbWetLevelMin);
				bModified = true;
			}

			sol::optional<double> RevWetMax = Params.get<sol::optional<double>>("reverb_wet_level_max");
			if (RevWetMax.has_value())
			{
				A.ReverbWetLevelMax = static_cast<float>(RevWetMax.value());
				Changes += FString::Printf(TEXT(" reverb_wet_level_max=%.2f"), (double)A.ReverbWetLevelMax);
				bModified = true;
			}

			sol::optional<double> RevDistMin = Params.get<sol::optional<double>>("reverb_distance_min");
			if (RevDistMin.has_value())
			{
				A.ReverbDistanceMin = static_cast<float>(RevDistMin.value());
				Changes += FString::Printf(TEXT(" reverb_distance_min=%.0f"), (double)A.ReverbDistanceMin);
				bModified = true;
			}

			sol::optional<double> RevDistMax = Params.get<sol::optional<double>>("reverb_distance_max");
			if (RevDistMax.has_value())
			{
				A.ReverbDistanceMax = static_cast<float>(RevDistMax.value());
				Changes += FString::Printf(TEXT(" reverb_distance_max=%.0f"), (double)A.ReverbDistanceMax);
				bModified = true;
			}

			sol::optional<double> ManRevSend = Params.get<sol::optional<double>>("manual_reverb_send_level");
			if (ManRevSend.has_value())
			{
				A.ManualReverbSendLevel = static_cast<float>(ManRevSend.value());
				Changes += FString::Printf(TEXT(" manual_reverb_send_level=%.2f"), (double)A.ManualReverbSendLevel);
				bModified = true;
			}

			// --- Priority attenuation ---

			sol::optional<bool> EnablePriAtten = Params.get<sol::optional<bool>>("enable_priority_attenuation");
			if (EnablePriAtten.has_value())
			{
				A.bEnablePriorityAttenuation = EnablePriAtten.value();
				Changes += FString::Printf(TEXT(" enable_priority_attenuation=%s"), EnablePriAtten.value() ? TEXT("true") : TEXT("false"));
				bModified = true;
			}

			sol::optional<std::string> PriMethod = Params.get<sol::optional<std::string>>("priority_attenuation_method");
			if (PriMethod.has_value())
			{
				FString MethodStr = UTF8_TO_TCHAR(PriMethod.value().c_str());
				A.PriorityAttenuationMethod = StringToPriorityAttenuationMethod(MethodStr);
				Changes += FString::Printf(TEXT(" priority_attenuation_method=%s"), *MethodStr.ToLower());
				bModified = true;
			}

			sol::optional<double> PriAttenMin = Params.get<sol::optional<double>>("priority_attenuation_min");
			if (PriAttenMin.has_value())
			{
				A.PriorityAttenuationMin = FMath::Clamp(static_cast<float>(PriAttenMin.value()), 0.0f, 1.0f);
				Changes += FString::Printf(TEXT(" priority_attenuation_min=%.2f"), (double)A.PriorityAttenuationMin);
				bModified = true;
			}

			sol::optional<double> PriAttenMax = Params.get<sol::optional<double>>("priority_attenuation_max");
			if (PriAttenMax.has_value())
			{
				A.PriorityAttenuationMax = FMath::Clamp(static_cast<float>(PriAttenMax.value()), 0.0f, 1.0f);
				Changes += FString::Printf(TEXT(" priority_attenuation_max=%.2f"), (double)A.PriorityAttenuationMax);
				bModified = true;
			}

			sol::optional<double> PriDistMin = Params.get<sol::optional<double>>("priority_attenuation_distance_min");
			if (PriDistMin.has_value())
			{
				A.PriorityAttenuationDistanceMin = FMath::Max(0.0f, static_cast<float>(PriDistMin.value()));
				Changes += FString::Printf(TEXT(" priority_attenuation_distance_min=%.0f"), (double)A.PriorityAttenuationDistanceMin);
				bModified = true;
			}

			sol::optional<double> PriDistMax = Params.get<sol::optional<double>>("priority_attenuation_distance_max");
			if (PriDistMax.has_value())
			{
				A.PriorityAttenuationDistanceMax = FMath::Max(0.0f, static_cast<float>(PriDistMax.value()));
				Changes += FString::Printf(TEXT(" priority_attenuation_distance_max=%.0f"), (double)A.PriorityAttenuationDistanceMax);
				bModified = true;
			}

			sol::optional<double> ManPriAtten = Params.get<sol::optional<double>>("manual_priority_attenuation");
			if (ManPriAtten.has_value())
			{
				A.ManualPriorityAttenuation = FMath::Clamp(static_cast<float>(ManPriAtten.value()), 0.0f, 1.0f);
				Changes += FString::Printf(TEXT(" manual_priority_attenuation=%.2f"), (double)A.ManualPriorityAttenuation);
				bModified = true;
			}

			if (bModified)
			{
				FPropertyChangedEvent Event(nullptr, EPropertyChangeType::ValueSet);
				Asset->PostEditChangeProperty(Event);
				Asset->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(%s)"), *Changes.TrimStart()));
				return sol::make_object(Lua, true);
			}

			Session.Log(TEXT("[OK] configure() -> nothing changed. Use help() to see valid keys."));
			return sol::make_object(Lua, true);
		});
	});
}

REGISTER_LUA_BINDING(SoundAttenuation, SoundAttenuationDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindSoundAttenuation(Lua, Session);
});
