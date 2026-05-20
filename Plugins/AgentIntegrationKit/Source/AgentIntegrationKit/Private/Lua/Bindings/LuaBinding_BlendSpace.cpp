// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Tools/NeoStackToolUtils.h"
#include "ScopedTransaction.h"

#include "Animation/BlendSpace.h"
#include "Animation/BlendSpace1D.h"
#include "Animation/AimOffsetBlendSpace.h"
#include "Animation/AimOffsetBlendSpace1D.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Animation/BlendProfile.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// PROTECTED MEMBER ACCESS HELPERS
// ============================================================================
// SampleData, BlendParameters, PerBoneBlendMode, ManualPerBoneOverrides,
// PerBoneBlendProfile, AxisToScaleAnimation are protected UPROPERTYs.
// We use property reflection for mutable access — standard UE editor pattern.

static TArray<FBlendSample>* GetMutableSamples(UBlendSpace* BS)
{
	static FProperty* Prop = UBlendSpace::StaticClass()->FindPropertyByName(TEXT("SampleData"));
	return Prop ? Prop->ContainerPtrToValuePtr<TArray<FBlendSample>>(BS) : nullptr;
}

static FBlendParameter* GetMutableBlendParameters(UBlendSpace* BS)
{
	static FProperty* Prop = UBlendSpace::StaticClass()->FindPropertyByName(TEXT("BlendParameters"));
	return Prop ? Prop->ContainerPtrToValuePtr<FBlendParameter>(BS) : nullptr;
}

static EBlendSpacePerBoneBlendMode* GetMutablePerBoneBlendMode(UBlendSpace* BS)
{
	static FProperty* Prop = UBlendSpace::StaticClass()->FindPropertyByName(TEXT("PerBoneBlendMode"));
	return Prop ? Prop->ContainerPtrToValuePtr<EBlendSpacePerBoneBlendMode>(BS) : nullptr;
}

static TArray<FPerBoneInterpolation>* GetMutablePerBoneOverrides(UBlendSpace* BS)
{
	static FProperty* Prop = UBlendSpace::StaticClass()->FindPropertyByName(TEXT("ManualPerBoneOverrides"));
	return Prop ? Prop->ContainerPtrToValuePtr<TArray<FPerBoneInterpolation>>(BS) : nullptr;
}

static FBlendSpaceBlendProfile* GetMutableBlendProfile(UBlendSpace* BS)
{
	static FProperty* Prop = UBlendSpace::StaticClass()->FindPropertyByName(TEXT("PerBoneBlendProfile"));
	return Prop ? Prop->ContainerPtrToValuePtr<FBlendSpaceBlendProfile>(BS) : nullptr;
}

static TEnumAsByte<EBlendSpaceAxis>* GetMutableAxisToScale(UBlendSpace* BS)
{
	static FProperty* Prop = UBlendSpace::StaticClass()->FindPropertyByName(TEXT("AxisToScaleAnimation"));
	return Prop ? Prop->ContainerPtrToValuePtr<TEnumAsByte<EBlendSpaceAxis>>(BS) : nullptr;
}

// ============================================================================
// HELPERS
// ============================================================================

static bool IsBlendSpace1D(const UBlendSpace* BS)
{
	return BS->IsA<UBlendSpace1D>();
}

static FString GetBlendSpaceTypeName(const UBlendSpace* BS)
{
	if (BS->IsA<UAimOffsetBlendSpace>()) return TEXT("AimOffset");
	if (BS->IsA<UAimOffsetBlendSpace1D>()) return TEXT("AimOffset1D");
	if (BS->IsA<UBlendSpace1D>()) return TEXT("BlendSpace1D");
	return TEXT("BlendSpace");
}

static FVector ParseSamplePosition(sol::table P, bool bIs1D, bool& bValid)
{
	bValid = false;

	// Try "position" as a number (1D) or table (2D)
	sol::object PosObj = P.get<sol::object>("position");

	if (PosObj.is<double>())
	{
		bValid = true;
		return FVector(PosObj.as<double>(), 0.0, 0.0);
	}

	if (PosObj.is<sol::table>())
	{
		sol::table PosArr = PosObj.as<sol::table>();
		if (bIs1D)
		{
			sol::optional<double> X = PosArr.get<sol::optional<double>>(1);
			if (X.has_value())
			{
				bValid = true;
				return FVector(X.value(), 0.0, 0.0);
			}
		}
		else
		{
			sol::optional<double> X = PosArr.get<sol::optional<double>>(1);
			sol::optional<double> Y = PosArr.get<sol::optional<double>>(2);
			if (X.has_value() && Y.has_value())
			{
				bValid = true;
				return FVector(X.value(), Y.value(), 0.0);
			}
			// Single element for 2D: use as X
			if (X.has_value())
			{
				bValid = true;
				return FVector(X.value(), 0.0, 0.0);
			}
		}
	}

	return FVector::ZeroVector;
}

static int32 ParseAxisIndex(sol::object AxisObj)
{
	if (AxisObj.is<int>()) return AxisObj.as<int>();
	if (AxisObj.is<double>()) return static_cast<int32>(AxisObj.as<double>());
	if (AxisObj.is<std::string>())
	{
		FString S = UTF8_TO_TCHAR(AxisObj.as<std::string>().c_str());
		S = S.ToLower().TrimStartAndEnd();
		if (S == TEXT("x") || S == TEXT("0")) return 0;
		if (S == TEXT("y") || S == TEXT("1")) return 1;
	}
	return -1;
}

static EFilterInterpolationType ParseInterpType(const std::string& TypeStr)
{
	FString S = UTF8_TO_TCHAR(TypeStr.c_str());
	if (S.Equals(TEXT("Average"), ESearchCase::IgnoreCase)) return BSIT_Average;
	if (S.Equals(TEXT("Linear"), ESearchCase::IgnoreCase)) return BSIT_Linear;
	if (S.Equals(TEXT("Cubic"), ESearchCase::IgnoreCase)) return BSIT_Cubic;
	if (S.Equals(TEXT("EaseInOut"), ESearchCase::IgnoreCase)) return BSIT_EaseInOut;
	if (S.Equals(TEXT("Exponential"), ESearchCase::IgnoreCase)) return BSIT_ExponentialDecay;
	if (S.Equals(TEXT("SpringDamper"), ESearchCase::IgnoreCase)) return BSIT_SpringDamper;
	return BSIT_SpringDamper; // default
}

static const char* InterpTypeToString(EFilterInterpolationType Type)
{
	switch (Type)
	{
	case BSIT_Average: return "Average";
	case BSIT_Linear: return "Linear";
	case BSIT_Cubic: return "Cubic";
	case BSIT_EaseInOut: return "EaseInOut";
	case BSIT_ExponentialDecay: return "Exponential";
	case BSIT_SpringDamper: return "SpringDamper";
	default: return "Unknown";
	}
}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
static void ApplySingleFrameBlendOptions(FBlendSample& Sample, const sol::table& Params)
{
	sol::optional<bool> UseSingleFrame = Params.get<sol::optional<bool>>("use_single_frame");
	if (UseSingleFrame.has_value())
	{
		Sample.bUseSingleFrameForBlending = UseSingleFrame.value();
	}

	sol::optional<double> FrameIndex = Params.get<sol::optional<double>>("frame_index");
	if (FrameIndex.has_value())
	{
		Sample.FrameIndexToSample = static_cast<uint32>(FMath::Max(0.0, FrameIndex.value()));
	}
}

static bool ApplySingleFrameBlendOptionsAndTrackChanges(FBlendSample& Sample, const sol::table& Params)
{
	const bool bHadSingleFrame = Sample.bUseSingleFrameForBlending;
	const uint32 PreviousFrameIndex = Sample.FrameIndexToSample;
	ApplySingleFrameBlendOptions(Sample, Params);
	return bHadSingleFrame != Sample.bUseSingleFrameForBlending || PreviousFrameIndex != Sample.FrameIndexToSample;
}

static void AddSingleFrameBlendInfo(sol::table& Entry, const FBlendSample& Sample)
{
	Entry["use_single_frame"] = Sample.bUseSingleFrameForBlending;
	Entry["frame_index"] = static_cast<int>(Sample.FrameIndexToSample);
}

static void CopySingleFrameBlendOptions(FBlendSample& Target, const FBlendSample& Source)
{
	Target.bUseSingleFrameForBlending = Source.bUseSingleFrameForBlending;
	Target.FrameIndexToSample = Source.FrameIndexToSample;
}
#else
static void ApplySingleFrameBlendOptions(FBlendSample&, const sol::table&) {}
static bool ApplySingleFrameBlendOptionsAndTrackChanges(FBlendSample&, const sol::table&) { return false; }
static void AddSingleFrameBlendInfo(sol::table&, const FBlendSample&) {}
static void CopySingleFrameBlendOptions(FBlendSample&, const FBlendSample&) {}
#endif

// Trigger InitializePerBoneBlend via PostEditChangeProperty (not exported)
static void TriggerPerBoneBlendUpdate(UBlendSpace* BS, const TCHAR* PropertyName)
{
	FProperty* Prop = UBlendSpace::StaticClass()->FindPropertyByName(PropertyName);
	if (Prop)
	{
		FPropertyChangedEvent Event(Prop, EPropertyChangeType::ValueSet);
		BS->PostEditChangeProperty(Event);
	}
}

// Finalization sequence after modifications
// Note: ResampleData() internally calls ValidateSampleData(), so no need to call it separately
static void FinalizeBlendSpace(UBlendSpace* BS)
{
	BS->ResampleData();
	BS->MarkPackageDirty();
}

// ============================================================================
// BINDING
// ============================================================================

static TArray<FLuaFunctionDoc> BlendSpaceDocs = {};

static void BindBlendSpace(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_blend_space", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		UBlendSpace* BS = LoadObject<UBlendSpace>(nullptr, *FPath);
		if (!BS) return;

		const bool bIs1D = IsBlendSpace1D(BS);

		// ---- help text ----
		AssetObj["_help_text"] =
			"Element types for add/remove/list/configure:\n"
			"  sample         — blend sample (animation at a position)\n"
			"  per_bone_override — per-bone interpolation speed override\n"
			"\n"
			"add(type, params):\n"
			"  add(\"sample\", {animation=\"/Game/Anim\", position=0.5 or {x,y}, rate_scale=1.0})\n"
			"  add(\"per_bone_override\", {bone=\"spine_03\", interpolation_speed=5.0})\n"
			"\n"
			"remove(type, id):\n"
			"  remove(\"sample\", 1)                — 1-based Lua index\n"
			"  remove(\"per_bone_override\", \"spine_03\") — by bone name\n"
			"\n"
			"list(type):\n"
			"  list(\"samples\"), list(\"axes\"), list(\"per_bone_overrides\"), list(\"settings\")\n"
			"\n"
			"configure(type, params):\n"
			"  configure(\"sample\", {index=1, position=.., animation=.., rate_scale=..})\n"
			"  configure(\"axis\", {axis=0, display_name=.., min=.., max=.., grid_divisions=.., snap_to_grid=.., wrap_input=..})\n"
			"  configure(\"interpolation\", {axis=0, time=.., damping_ratio=.., max_speed=.., type=\"SpringDamper\"})\n"
			"  configure(\"per_bone_override\", {bone=\"spine_03\", interpolation_speed=8.0})\n"
			"  configure(\"per_bone_mode\", \"Manual\" or \"BlendProfile\")\n"
			"  configure(\"blend_profile\", {profile=\"ProfileName\", weight_speed=5.0})\n"
			"  configure(\"weight_speed\", 5.0)\n"
			"  configure(\"notify_mode\", \"all\" or \"highest\" or \"none\")\n"
			"  configure(\"axis_scale\", \"None\" or \"X\" or \"Y\")\n"
			"  configure(\"settings\", {loop=.., allow_marker_sync=.., match_sync_phases=..,\n"
			"    weight_ease_in_out=.., allow_mesh_space_blending=.., interpolate_using_grid=..,\n"
			"    triangulation_direction=\"Tangential\", scale_animation=..})\n"
			"\n"
			"Action methods:\n"
			"  duplicate_sample({index=1, position=..}) — copy sample to new position\n"
			"  info() — summary of type, samples, axes, settings\n";

		// ================================================================
		// add(type, params)
		// ================================================================
		AssetObj.set_function("add", [BS, bIs1D, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (FType.Equals(TEXT("sample"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value())
				{
					Session.Log(TEXT("[FAIL] add(\"sample\") -> params required: {animation=.., position=..}"));
					return sol::lua_nil;
				}
				sol::table P = Params.value();

				// Load animation
				std::string AnimPath = P.get_or<std::string>("animation", "");
				if (AnimPath.empty())
				{
					Session.Log(TEXT("[FAIL] add(\"sample\") -> 'animation' required"));
					return sol::lua_nil;
				}

				FString FAnimPath = UTF8_TO_TCHAR(AnimPath.c_str());
				UAnimSequence* Anim = NeoStackToolUtils::LoadAssetWithFallback<UAnimSequence>(FAnimPath);
				if (!Anim && !FAnimPath.StartsWith(TEXT("/")))
				{
					Anim = NeoStackToolUtils::LoadAssetWithFallback<UAnimSequence>(TEXT("/Game/") + FAnimPath);
				}
				if (!Anim)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"sample\") -> animation not found: %s"), *FAnimPath));
					return sol::lua_nil;
				}

				// Compatibility check
				if (!BS->IsAnimationCompatible(Anim))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"sample\") -> animation '%s' not compatible (skeleton/additive mismatch)"), *Anim->GetName()));
					return sol::lua_nil;
				}

				// Parse position
				bool bPosValid = false;
				FVector SamplePos = ParseSamplePosition(P, bIs1D, bPosValid);
				if (!bPosValid)
				{
					Session.Log(TEXT("[FAIL] add(\"sample\") -> invalid 'position' (use number for 1D, {x,y} for 2D)"));
					return sol::lua_nil;
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("BlendSpace: Add Sample")));
				BS->Modify();

				int32 NewIndex = BS->AddSample(Anim, SamplePos);
				if (NewIndex == INDEX_NONE)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"sample\") -> AddSample failed at (%.2f, %.2f)"), SamplePos.X, SamplePos.Y));
					return sol::lua_nil;
				}

				// Set optional properties
				TArray<FBlendSample>* Samples = GetMutableSamples(BS);
				if (Samples && NewIndex >= 0 && NewIndex < Samples->Num())
				{
					sol::optional<double> RateScale = P.get<sol::optional<double>>("rate_scale");
					if (RateScale.has_value())
					{
						(*Samples)[NewIndex].RateScale = FMath::Clamp(static_cast<float>(RateScale.value()), 0.01f, 64.0f);
					}
					ApplySingleFrameBlendOptions((*Samples)[NewIndex], P);
				}

				FinalizeBlendSpace(BS);

				// Return 1-based index
				int32 LuaIdx = NewIndex + 1;
				Session.Log(FString::Printf(TEXT("[OK] add(\"sample\", \"%s\") at (%.2f, %.2f) -> index %d"),
					*Anim->GetName(), SamplePos.X, SamplePos.Y, LuaIdx));
				return sol::make_object(Lua, LuaIdx);
			}
			else if (FType.Equals(TEXT("per_bone_override"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value())
				{
					Session.Log(TEXT("[FAIL] add(\"per_bone_override\") -> params required: {bone=.., interpolation_speed=..}"));
					return sol::lua_nil;
				}
				sol::table P = Params.value();

				std::string BoneName = P.get_or<std::string>("bone", "");
				if (BoneName.empty())
				{
					Session.Log(TEXT("[FAIL] add(\"per_bone_override\") -> 'bone' required"));
					return sol::lua_nil;
				}

				double InterpSpeed = P.get<sol::optional<double>>("interpolation_speed").value_or(5.0);

				TArray<FPerBoneInterpolation>* Overrides = GetMutablePerBoneOverrides(BS);
				if (!Overrides)
				{
					Session.Log(TEXT("[FAIL] add(\"per_bone_override\") -> could not access overrides"));
					return sol::lua_nil;
				}

				FString FBoneName = UTF8_TO_TCHAR(BoneName.c_str());

				// Check for duplicates
				for (const FPerBoneInterpolation& Existing : *Overrides)
				{
					if (Existing.BoneReference.BoneName == FName(*FBoneName))
					{
						Session.Log(FString::Printf(TEXT("[FAIL] add(\"per_bone_override\") -> '%s' already exists, use configure"), *FBoneName));
						return sol::lua_nil;
					}
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("BlendSpace: Add PerBone Override")));
				BS->Modify();

				FPerBoneInterpolation NewOverride;
				NewOverride.BoneReference.BoneName = FName(*FBoneName);
				NewOverride.InterpolationSpeedPerSec = static_cast<float>(InterpSpeed);
				Overrides->Add(NewOverride);

				TriggerPerBoneBlendUpdate(BS, TEXT("ManualPerBoneOverrides"));
				BS->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] add(\"per_bone_override\", bone=\"%s\", speed=%.2f)"), *FBoneName, NewOverride.InterpolationSpeedPerSec));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Valid: sample, per_bone_override"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// remove(type, id)
		// ================================================================
		AssetObj.set_function("remove", [BS, bIs1D, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (FType.Equals(TEXT("sample"), ESearchCase::IgnoreCase))
			{
				TArray<FBlendSample>* Samples = GetMutableSamples(BS);
				if (!Samples)
				{
					Session.Log(TEXT("[FAIL] remove(\"sample\") -> could not access samples"));
					return sol::lua_nil;
				}

				// Accept single index or table of indices (all 1-based)
				TArray<int32> IndicesToRemove;

				if (Id.is<int>() || Id.is<double>())
				{
					int32 LuaIdx = Id.is<int>() ? Id.as<int>() : static_cast<int32>(Id.as<double>());
					IndicesToRemove.Add(LuaIdx - 1); // Convert to 0-based
				}
				else if (Id.is<sol::table>())
				{
					sol::table IdxTable = Id.as<sol::table>();
					for (auto& Pair : IdxTable)
					{
						if (Pair.second.is<int>() || Pair.second.is<double>())
						{
							int32 LuaIdx = Pair.second.is<int>() ? Pair.second.as<int>() : static_cast<int32>(Pair.second.as<double>());
							IndicesToRemove.AddUnique(LuaIdx - 1);
						}
					}
				}
				else
				{
					Session.Log(TEXT("[FAIL] remove(\"sample\") -> index (1-based) or table of indices required"));
					return sol::lua_nil;
				}

				// Validate all indices
				for (int32 Idx : IndicesToRemove)
				{
					if (Idx < 0 || Idx >= Samples->Num())
					{
						Session.Log(FString::Printf(TEXT("[FAIL] remove(\"sample\") -> index %d out of range (1-%d)"), Idx + 1, Samples->Num()));
						return sol::lua_nil;
					}
				}

				// Sort DESCENDING — DeleteSample uses RemoveAtSwap
				IndicesToRemove.Sort([](int32 A, int32 B) { return A > B; });

				const FScopedTransaction Transaction(FText::FromString(TEXT("BlendSpace: Remove Sample")));
				BS->Modify();

				int32 Removed = 0;
				for (int32 Idx : IndicesToRemove)
				{
					// Re-check bounds (RemoveAtSwap may have shifted)
					if (Idx >= 0 && Idx < Samples->Num())
					{
						FString AnimName = (*Samples)[Idx].Animation ? (*Samples)[Idx].Animation->GetName() : TEXT("(none)");
						BS->DeleteSample(Idx);
						Session.Log(FString::Printf(TEXT("[OK] remove(\"sample\", %d) -> '%s'"), Idx + 1, *AnimName));
						Removed++;
					}
				}

				if (Removed > 0) FinalizeBlendSpace(BS);
				return Removed > 0 ? sol::make_object(Lua, Removed) : sol::lua_nil;
			}
			else if (FType.Equals(TEXT("per_bone_override"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<std::string>())
				{
					Session.Log(TEXT("[FAIL] remove(\"per_bone_override\") -> bone name required"));
					return sol::lua_nil;
				}

				TArray<FPerBoneInterpolation>* Overrides = GetMutablePerBoneOverrides(BS);
				if (!Overrides)
				{
					Session.Log(TEXT("[FAIL] remove(\"per_bone_override\") -> could not access overrides"));
					return sol::lua_nil;
				}

				FString BoneName = UTF8_TO_TCHAR(Id.as<std::string>().c_str());
				int32 FoundIndex = INDEX_NONE;
				for (int32 i = 0; i < Overrides->Num(); ++i)
				{
					if ((*Overrides)[i].BoneReference.BoneName == FName(*BoneName))
					{
						FoundIndex = i;
						break;
					}
				}

				if (FoundIndex == INDEX_NONE)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"per_bone_override\") -> '%s' not found"), *BoneName));
					return sol::lua_nil;
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("BlendSpace: Remove PerBone Override")));
				BS->Modify();
				Overrides->RemoveAt(FoundIndex);
				TriggerPerBoneBlendUpdate(BS, TEXT("ManualPerBoneOverrides"));
				BS->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] remove(\"per_bone_override\", \"%s\")"), *BoneName));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type. Valid: sample, per_bone_override"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// list(type?)
		// ================================================================
		AssetObj.set_function("list", [BS, bIs1D, &Session](sol::table Self,
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

			if (FType.Equals(TEXT("samples"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("sample"), ESearchCase::IgnoreCase))
			{
				const TArray<FBlendSample>& Samples = BS->GetBlendSamples();
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Samples.Num(); i++)
				{
					sol::table E = Lua.create_table();
					E["index"] = i + 1; // 1-based
					E["animation"] = Samples[i].Animation ? TCHAR_TO_UTF8(*Samples[i].Animation->GetName()) : "(none)";
					E["animation_path"] = Samples[i].Animation ? TCHAR_TO_UTF8(*Samples[i].Animation->GetPathName()) : "";
					if (bIs1D)
					{
						E["position"] = Samples[i].SampleValue.X;
					}
					else
					{
						sol::table Pos = Lua.create_table();
						Pos[1] = Samples[i].SampleValue.X;
						Pos[2] = Samples[i].SampleValue.Y;
						E["position"] = Pos;
					}
					E["rate_scale"] = Samples[i].RateScale;
					AddSingleFrameBlendInfo(E, Samples[i]);
					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"samples\") -> %d"), Samples.Num()));
				return Result;
			}

			if (FType.Equals(TEXT("axes"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("axis"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				int32 NumAxes = bIs1D ? 1 : 2;
				for (int32 i = 0; i < NumAxes; i++)
				{
					const FBlendParameter& Param = BS->GetBlendParameter(i);
					sol::table E = Lua.create_table();
					E["axis"] = i;
					E["name"] = i == 0 ? "x" : "y";
					E["display_name"] = TCHAR_TO_UTF8(*Param.DisplayName);
					E["min"] = Param.Min;
					E["max"] = Param.Max;
					E["grid_divisions"] = Param.GridNum;
					E["snap_to_grid"] = Param.bSnapToGrid;
					E["wrap_input"] = Param.bWrapInput;

					// Interpolation params (public)
					const FInterpolationParameter& Interp = BS->InterpolationParam[i];
					sol::table InterpT = Lua.create_table();
					InterpT["time"] = Interp.InterpolationTime;
					InterpT["damping_ratio"] = Interp.DampingRatio;
					InterpT["max_speed"] = Interp.MaxSpeed;
					InterpT["type"] = InterpTypeToString(Interp.InterpolationType);
					E["interpolation"] = InterpT;

					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"axes\") -> %d"), NumAxes));
				return Result;
			}

			if (FType.Equals(TEXT("per_bone_overrides"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("per_bone_override"), ESearchCase::IgnoreCase))
			{
				TArray<FPerBoneInterpolation>* Overrides = GetMutablePerBoneOverrides(BS);
				sol::table Result = Lua.create_table();
				if (Overrides)
				{
					for (int32 i = 0; i < Overrides->Num(); i++)
					{
						sol::table E = Lua.create_table();
						E["bone"] = TCHAR_TO_UTF8(*(*Overrides)[i].BoneReference.BoneName.ToString());
						E["interpolation_speed"] = (*Overrides)[i].InterpolationSpeedPerSec;
						Result[i + 1] = E;
					}
					Session.Log(FString::Printf(TEXT("[OK] list(\"per_bone_overrides\") -> %d"), Overrides->Num()));
				}
				return Result;
			}

			if (FType.Equals(TEXT("settings"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				Result["loop"] = BS->bLoop;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
				Result["allow_marker_sync"] = BS->bAllowMarkerBasedSync;
#endif
	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
				Result["match_sync_phases"] = BS->bShouldMatchSyncPhases;
#endif
				Result["weight_speed"] = BS->TargetWeightInterpolationSpeedPerSec;
				Result["weight_ease_in_out"] = BS->bTargetWeightInterpolationEaseInOut;
				Result["allow_mesh_space_blending"] = BS->bAllowMeshSpaceBlending;
				Result["interpolate_using_grid"] = BS->bInterpolateUsingGrid;

				switch (BS->PreferredTriangulationDirection)
				{
				case EPreferredTriangulationDirection::None: Result["triangulation_direction"] = "None"; break;
				case EPreferredTriangulationDirection::Tangential: Result["triangulation_direction"] = "Tangential"; break;
				case EPreferredTriangulationDirection::Radial: Result["triangulation_direction"] = "Radial"; break;
				default: Result["triangulation_direction"] = "Unknown"; break;
				}

				// Axis scale (protected — use reflection)
				TEnumAsByte<EBlendSpaceAxis>* AxisPtr = GetMutableAxisToScale(BS);
				if (AxisPtr)
				{
					switch (AxisPtr->GetValue())
					{
					case BSA_None: Result["axis_scale"] = "None"; break;
					case BSA_X: Result["axis_scale"] = "X"; break;
					case BSA_Y: Result["axis_scale"] = "Y"; break;
					default: Result["axis_scale"] = "Unknown"; break;
					}
				}

				// 1D-specific
				if (bIs1D)
				{
					UBlendSpace1D* BS1D = Cast<UBlendSpace1D>(BS);
					if (BS1D) Result["scale_animation"] = BS1D->bScaleAnimation;
				}

				Session.Log(TEXT("[OK] list(\"settings\")"));
				return Result;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: samples, axes, per_bone_overrides, settings"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// configure(type, params)
		// ================================================================
		AssetObj.set_function("configure", [BS, bIs1D, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			// ---- configure("sample", {index=.., position=.., animation=.., rate_scale=..}) ----
			if (FType.Equals(TEXT("sample"), ESearchCase::IgnoreCase))
			{
				if (!Params.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"sample\") -> table params required: {index=.., ...}"));
					return sol::lua_nil;
				}
				sol::table P = Params.as<sol::table>();

				sol::optional<int> LuaIdx = P.get<sol::optional<int>>("index");
				if (!LuaIdx.has_value())
				{
					Session.Log(TEXT("[FAIL] configure(\"sample\") -> 'index' required (1-based)"));
					return sol::lua_nil;
				}
				int32 Index = LuaIdx.value() - 1; // Convert to 0-based

				TArray<FBlendSample>* Samples = GetMutableSamples(BS);
				if (!Samples || Index < 0 || Index >= Samples->Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"sample\") -> index %d out of range"), LuaIdx.value()));
					return sol::lua_nil;
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("BlendSpace: Configure Sample")));
				BS->Modify();
				bool bModified = false;
				bool bSamplesChanged = false;

				// Position
				sol::optional<sol::object> PosOpt = P.get<sol::optional<sol::object>>("position");
				if (PosOpt.has_value())
				{
					bool bPosValid = false;
					FVector NewPos = ParseSamplePosition(P, bIs1D, bPosValid);
					if (bPosValid)
					{
						if (BS->EditSampleValue(Index, NewPos))
						{
							bModified = true;
							bSamplesChanged = true;
						}
						else
						{
							Session.Log(FString::Printf(TEXT("[WARN] configure(\"sample\") -> position rejected for index %d"), LuaIdx.value()));
						}
					}
				}

				// Animation
				std::string AnimPath = P.get_or<std::string>("animation", "");
				if (!AnimPath.empty())
				{
					FString FAnimPath = UTF8_TO_TCHAR(AnimPath.c_str());
					UAnimSequence* NewAnim = NeoStackToolUtils::LoadAssetWithFallback<UAnimSequence>(FAnimPath);
					if (!NewAnim && !FAnimPath.StartsWith(TEXT("/")))
					{
						NewAnim = NeoStackToolUtils::LoadAssetWithFallback<UAnimSequence>(TEXT("/Game/") + FAnimPath);
					}
					if (NewAnim)
					{
						BS->ReplaceSampleAnimation(Index, NewAnim);
						bModified = true;
						bSamplesChanged = true;
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[WARN] configure(\"sample\") -> animation not found: %s"), *FAnimPath));
					}
				}

				// Rate scale
				sol::optional<double> RateScale = P.get<sol::optional<double>>("rate_scale");
				if (RateScale.has_value())
				{
					(*Samples)[Index].RateScale = FMath::Clamp(static_cast<float>(RateScale.value()), 0.01f, 64.0f);
					bModified = true;
				}
				bModified = bModified || ApplySingleFrameBlendOptionsAndTrackChanges((*Samples)[Index], P);

				if (bModified)
				{
					FinalizeBlendSpace(BS);
					Session.Log(FString::Printf(TEXT("[OK] configure(\"sample\", index=%d)"), LuaIdx.value()));
					return sol::make_object(Lua, true);
				}

				Session.Log(FString::Printf(TEXT("[OK] configure(\"sample\", index=%d) -> nothing changed"), LuaIdx.value()));
				return sol::make_object(Lua, true);
			}

			// ---- configure("axis", {axis=0, display_name=.., min=.., max=.., ...}) ----
			if (FType.Equals(TEXT("axis"), ESearchCase::IgnoreCase))
			{
				if (!Params.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"axis\") -> table params required: {axis=0, ...}"));
					return sol::lua_nil;
				}
				sol::table P = Params.as<sol::table>();

				sol::object AxisObj = P.get<sol::object>("axis");
				int32 AxisIndex = ParseAxisIndex(AxisObj);
				if (AxisIndex < 0 || AxisIndex > 1)
				{
					Session.Log(TEXT("[FAIL] configure(\"axis\") -> 'axis' must be 0/\"x\" or 1/\"y\""));
					return sol::lua_nil;
				}
				if (bIs1D && AxisIndex > 0)
				{
					Session.Log(TEXT("[FAIL] configure(\"axis\") -> 1D blend spaces only support axis 0 (X)"));
					return sol::lua_nil;
				}

				FBlendParameter* Params_Ptr = GetMutableBlendParameters(BS);
				if (!Params_Ptr)
				{
					Session.Log(TEXT("[FAIL] configure(\"axis\") -> could not access blend parameters"));
					return sol::lua_nil;
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("BlendSpace: Configure Axis")));
				BS->Modify();

				FBlendParameter& Param = Params_Ptr[AxisIndex];

				std::string DisplayName = P.get_or<std::string>("display_name", "");

				// Validate min/max before modifying
				sol::optional<double> MinVal = P.get<sol::optional<double>>("min");
				sol::optional<double> MaxVal = P.get<sol::optional<double>>("max");
				float NewMin = MinVal.has_value() ? static_cast<float>(MinVal.value()) : Param.Min;
				float NewMax = MaxVal.has_value() ? static_cast<float>(MaxVal.value()) : Param.Max;
				if (NewMin >= NewMax)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"axis\") -> min (%.1f) must be < max (%.1f)"), NewMin, NewMax));
					return sol::lua_nil;
				}

				if (!DisplayName.empty())
				{
					Param.DisplayName = UTF8_TO_TCHAR(DisplayName.c_str());
				}
				Param.Min = NewMin;
				Param.Max = NewMax;

				sol::optional<double> GridDiv = P.get<sol::optional<double>>("grid_divisions");
				if (GridDiv.has_value()) Param.GridNum = FMath::Max(1, static_cast<int32>(GridDiv.value()));

				sol::optional<bool> SnapToGrid = P.get<sol::optional<bool>>("snap_to_grid");
				if (SnapToGrid.has_value()) Param.bSnapToGrid = SnapToGrid.value();

				sol::optional<bool> WrapInput = P.get<sol::optional<bool>>("wrap_input");
				if (WrapInput.has_value()) Param.bWrapInput = WrapInput.value();

				FinalizeBlendSpace(BS);

				FString AxisName = AxisIndex == 0 ? TEXT("X") : TEXT("Y");
				Session.Log(FString::Printf(TEXT("[OK] configure(\"axis\", %s) -> '%s' [%.1f, %.1f] Grid=%d"),
					*AxisName, *Param.DisplayName, Param.Min, Param.Max, Param.GridNum));
				return sol::make_object(Lua, true);
			}

			// ---- configure("interpolation", {axis=0, time=.., damping_ratio=.., max_speed=.., type=..}) ----
			if (FType.Equals(TEXT("interpolation"), ESearchCase::IgnoreCase))
			{
				if (!Params.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"interpolation\") -> table params required: {axis=0, ...}"));
					return sol::lua_nil;
				}
				sol::table P = Params.as<sol::table>();

				sol::object AxisObj = P.get<sol::object>("axis");
				int32 AxisIndex = ParseAxisIndex(AxisObj);
				if (AxisIndex < 0 || AxisIndex > 1)
				{
					Session.Log(TEXT("[FAIL] configure(\"interpolation\") -> 'axis' must be 0/\"x\" or 1/\"y\""));
					return sol::lua_nil;
				}
				if (bIs1D && AxisIndex > 0)
				{
					Session.Log(TEXT("[FAIL] configure(\"interpolation\") -> 1D blend spaces only support axis 0"));
					return sol::lua_nil;
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("BlendSpace: Configure Interpolation")));
				BS->Modify();

				FInterpolationParameter& Interp = BS->InterpolationParam[AxisIndex];

				sol::optional<double> Time = P.get<sol::optional<double>>("time");
				if (Time.has_value()) Interp.InterpolationTime = FMath::Max(0.0f, static_cast<float>(Time.value()));

				sol::optional<double> Damping = P.get<sol::optional<double>>("damping_ratio");
				if (Damping.has_value()) Interp.DampingRatio = FMath::Max(0.0f, static_cast<float>(Damping.value()));

				sol::optional<double> MaxSpeed = P.get<sol::optional<double>>("max_speed");
				if (MaxSpeed.has_value()) Interp.MaxSpeed = FMath::Max(0.0f, static_cast<float>(MaxSpeed.value()));

				std::string InterpType = P.get_or<std::string>("type", "");
				if (!InterpType.empty())
				{
					Interp.InterpolationType = ParseInterpType(InterpType);
				}

				BS->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"interpolation\", axis=%d) -> time=%.2f type=%s"),
					AxisIndex, Interp.InterpolationTime, UTF8_TO_TCHAR(InterpTypeToString(Interp.InterpolationType))));
				return sol::make_object(Lua, true);
			}

			// ---- configure("per_bone_override", {bone=.., interpolation_speed=..}) ----
			if (FType.Equals(TEXT("per_bone_override"), ESearchCase::IgnoreCase))
			{
				if (!Params.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"per_bone_override\") -> table required: {bone=.., interpolation_speed=..}"));
					return sol::lua_nil;
				}
				sol::table P = Params.as<sol::table>();

				std::string BoneName = P.get_or<std::string>("bone", "");
				if (BoneName.empty())
				{
					Session.Log(TEXT("[FAIL] configure(\"per_bone_override\") -> 'bone' required"));
					return sol::lua_nil;
				}

				TArray<FPerBoneInterpolation>* Overrides = GetMutablePerBoneOverrides(BS);
				if (!Overrides)
				{
					Session.Log(TEXT("[FAIL] configure(\"per_bone_override\") -> could not access overrides"));
					return sol::lua_nil;
				}

				FString FBoneName = UTF8_TO_TCHAR(BoneName.c_str());
				int32 FoundIndex = INDEX_NONE;
				for (int32 i = 0; i < Overrides->Num(); ++i)
				{
					if ((*Overrides)[i].BoneReference.BoneName == FName(*FBoneName))
					{
						FoundIndex = i;
						break;
					}
				}

				if (FoundIndex == INDEX_NONE)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"per_bone_override\") -> '%s' not found, use add first"), *FBoneName));
					return sol::lua_nil;
				}

				sol::optional<double> InterpSpeed = P.get<sol::optional<double>>("interpolation_speed");
				if (!InterpSpeed.has_value())
				{
					Session.Log(TEXT("[FAIL] configure(\"per_bone_override\") -> 'interpolation_speed' required"));
					return sol::lua_nil;
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("BlendSpace: Configure PerBone Override")));
				BS->Modify();

				(*Overrides)[FoundIndex].InterpolationSpeedPerSec = static_cast<float>(InterpSpeed.value());
				TriggerPerBoneBlendUpdate(BS, TEXT("ManualPerBoneOverrides"));
				BS->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"per_bone_override\", bone=\"%s\", speed=%.2f)"),
					*FBoneName, (*Overrides)[FoundIndex].InterpolationSpeedPerSec));
				return sol::make_object(Lua, true);
			}

			// ---- configure("per_bone_mode", "Manual"/"BlendProfile") ----
			if (FType.Equals(TEXT("per_bone_mode"), ESearchCase::IgnoreCase))
			{
				if (!Params.is<std::string>())
				{
					Session.Log(TEXT("[FAIL] configure(\"per_bone_mode\") -> string required: \"Manual\" or \"BlendProfile\""));
					return sol::lua_nil;
				}

				EBlendSpacePerBoneBlendMode* ModePtr = GetMutablePerBoneBlendMode(BS);
				if (!ModePtr)
				{
					Session.Log(TEXT("[FAIL] configure(\"per_bone_mode\") -> could not access mode"));
					return sol::lua_nil;
				}

				FString ModeStr = UTF8_TO_TCHAR(Params.as<std::string>().c_str());
				const FScopedTransaction Transaction(FText::FromString(TEXT("BlendSpace: Configure PerBone Mode")));
				BS->Modify();

				if (ModeStr.Equals(TEXT("Manual"), ESearchCase::IgnoreCase))
				{
					*ModePtr = EBlendSpacePerBoneBlendMode::ManualPerBoneOverride;
				}
				else if (ModeStr.Equals(TEXT("BlendProfile"), ESearchCase::IgnoreCase))
				{
					*ModePtr = EBlendSpacePerBoneBlendMode::BlendProfile;
				}
				else
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"per_bone_mode\") -> unknown mode '%s'"), *ModeStr));
					return sol::lua_nil;
				}

				// Trigger InitializePerBoneBlend
				TriggerPerBoneBlendUpdate(BS, TEXT("PerBoneBlendMode"));
				BS->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"per_bone_mode\", \"%s\")"), *ModeStr));
				return sol::make_object(Lua, true);
			}

			// ---- configure("blend_profile", {profile=.., weight_speed=..}) ----
			if (FType.Equals(TEXT("blend_profile"), ESearchCase::IgnoreCase))
			{
				if (!Params.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"blend_profile\") -> table params required: {profile=.., weight_speed=..}"));
					return sol::lua_nil;
				}
				sol::table P = Params.as<sol::table>();

				FBlendSpaceBlendProfile* BPPtr = GetMutableBlendProfile(BS);
				if (!BPPtr)
				{
					Session.Log(TEXT("[FAIL] configure(\"blend_profile\") -> could not access blend profile data"));
					return sol::lua_nil;
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("BlendSpace: Configure Blend Profile")));
				BS->Modify();

				// Profile name — UBlendProfile is inner object of USkeleton
				std::string ProfileName = P.get_or<std::string>("profile", "");
				if (!ProfileName.empty())
				{
					FString FProfileName = UTF8_TO_TCHAR(ProfileName.c_str());
					USkeleton* Skeleton = BS->GetSkeleton();
					if (Skeleton)
					{
						UBlendProfile* Profile = Skeleton->GetBlendProfile(FName(*FProfileName));
						if (Profile)
						{
							BPPtr->BlendProfile = Profile;
							Session.Log(FString::Printf(TEXT("[OK] configure(\"blend_profile\") -> profile='%s'"), *FProfileName));
						}
						else
						{
							Session.Log(FString::Printf(TEXT("[FAIL] configure(\"blend_profile\") -> profile '%s' not found on skeleton"), *FProfileName));
							return sol::lua_nil;
						}
					}
					else
					{
						Session.Log(TEXT("[FAIL] configure(\"blend_profile\") -> no skeleton on blend space"));
						return sol::lua_nil;
					}
				}

				sol::optional<double> WeightSpeed = P.get<sol::optional<double>>("weight_speed");
				if (WeightSpeed.has_value())
				{
					BPPtr->TargetWeightInterpolationSpeedPerSec = FMath::Max(0.0f, static_cast<float>(WeightSpeed.value()));
				}

				BS->MarkPackageDirty();
				return sol::make_object(Lua, true);
			}

			// ---- configure("weight_speed", number) ----
			if (FType.Equals(TEXT("weight_speed"), ESearchCase::IgnoreCase))
			{
				if (!Params.is<double>() && !Params.is<int>())
				{
					Session.Log(TEXT("[FAIL] configure(\"weight_speed\") -> number required"));
					return sol::lua_nil;
				}
				double Speed = Params.is<double>() ? Params.as<double>() : static_cast<double>(Params.as<int>());

				const FScopedTransaction Transaction(FText::FromString(TEXT("BlendSpace: Configure Weight Speed")));
				BS->Modify();
				BS->TargetWeightInterpolationSpeedPerSec = FMath::Max(0.0f, static_cast<float>(Speed));
				BS->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"weight_speed\", %.2f)"), BS->TargetWeightInterpolationSpeedPerSec));
				return sol::make_object(Lua, true);
			}

			// ---- configure("notify_mode", "all"/"highest"/"none") ----
			if (FType.Equals(TEXT("notify_mode"), ESearchCase::IgnoreCase))
			{
				if (!Params.is<std::string>())
				{
					Session.Log(TEXT("[FAIL] configure(\"notify_mode\") -> string required: \"all\", \"highest\", or \"none\""));
					return sol::lua_nil;
				}

				FString ModeStr = UTF8_TO_TCHAR(Params.as<std::string>().c_str());
				const FScopedTransaction Transaction(FText::FromString(TEXT("BlendSpace: Configure Notify Mode")));
				BS->Modify();

				if (ModeStr.Equals(TEXT("all"), ESearchCase::IgnoreCase))
				{
					BS->NotifyTriggerMode = ENotifyTriggerMode::AllAnimations;
				}
				else if (ModeStr.Equals(TEXT("highest"), ESearchCase::IgnoreCase))
				{
					BS->NotifyTriggerMode = ENotifyTriggerMode::HighestWeightedAnimation;
				}
				else if (ModeStr.Equals(TEXT("none"), ESearchCase::IgnoreCase))
				{
					BS->NotifyTriggerMode = ENotifyTriggerMode::None;
				}
				else
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"notify_mode\") -> unknown mode '%s'"), *ModeStr));
					return sol::lua_nil;
				}

				BS->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"notify_mode\", \"%s\")"), *ModeStr));
				return sol::make_object(Lua, true);
			}

			// ---- configure("axis_scale", "None"/"X"/"Y") ----
			if (FType.Equals(TEXT("axis_scale"), ESearchCase::IgnoreCase))
			{
				if (!Params.is<std::string>())
				{
					Session.Log(TEXT("[FAIL] configure(\"axis_scale\") -> string required: \"None\", \"X\", or \"Y\""));
					return sol::lua_nil;
				}

				TEnumAsByte<EBlendSpaceAxis>* AxisPtr = GetMutableAxisToScale(BS);
				if (!AxisPtr)
				{
					Session.Log(TEXT("[FAIL] configure(\"axis_scale\") -> could not access AxisToScaleAnimation"));
					return sol::lua_nil;
				}

				FString AxisStr = UTF8_TO_TCHAR(Params.as<std::string>().c_str());
				const FScopedTransaction Transaction(FText::FromString(TEXT("BlendSpace: Configure Axis Scale")));
				BS->Modify();

				if (AxisStr.Equals(TEXT("None"), ESearchCase::IgnoreCase))
				{
					*AxisPtr = BSA_None;
				}
				else if (AxisStr.Equals(TEXT("X"), ESearchCase::IgnoreCase))
				{
					*AxisPtr = BSA_X;
				}
				else if (AxisStr.Equals(TEXT("Y"), ESearchCase::IgnoreCase))
				{
					if (bIs1D)
					{
						Session.Log(TEXT("[FAIL] configure(\"axis_scale\") -> 1D blend spaces only support None or X"));
						return sol::lua_nil;
					}
					*AxisPtr = BSA_Y;
				}
				else
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"axis_scale\") -> unknown axis '%s'"), *AxisStr));
					return sol::lua_nil;
				}

				BS->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"axis_scale\", \"%s\")"), *AxisStr));
				return sol::make_object(Lua, true);
			}

			// ---- configure("settings", {loop=.., allow_marker_sync=.., ...}) ----
			if (FType.Equals(TEXT("settings"), ESearchCase::IgnoreCase))
			{
				if (!Params.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"settings\") -> table required: {loop=.., allow_marker_sync=.., ...}"));
					return sol::lua_nil;
				}
				sol::table P = Params.as<sol::table>();

				const FScopedTransaction Transaction(FText::FromString(TEXT("BlendSpace: Configure Settings")));
				BS->Modify();

				int32 SetCount = 0;

				sol::optional<bool> Loop = P.get<sol::optional<bool>>("loop");
				if (Loop.has_value()) { BS->bLoop = Loop.value(); SetCount++; }

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
				sol::optional<bool> MarkerSync = P.get<sol::optional<bool>>("allow_marker_sync");
				if (MarkerSync.has_value()) { BS->bAllowMarkerBasedSync = MarkerSync.value(); SetCount++; }
#endif

	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
				sol::optional<bool> MatchSync = P.get<sol::optional<bool>>("match_sync_phases");
				if (MatchSync.has_value()) { BS->bShouldMatchSyncPhases = MatchSync.value(); SetCount++; }
#endif

				sol::optional<bool> WeightEaseInOut = P.get<sol::optional<bool>>("weight_ease_in_out");
				if (WeightEaseInOut.has_value()) { BS->bTargetWeightInterpolationEaseInOut = WeightEaseInOut.value(); SetCount++; }

				sol::optional<bool> MeshSpaceBlend = P.get<sol::optional<bool>>("allow_mesh_space_blending");
				if (MeshSpaceBlend.has_value()) { BS->bAllowMeshSpaceBlending = MeshSpaceBlend.value(); SetCount++; }

				sol::optional<bool> UseGrid = P.get<sol::optional<bool>>("interpolate_using_grid");
				if (UseGrid.has_value()) { BS->bInterpolateUsingGrid = UseGrid.value(); SetCount++; }

				std::string TriDir = P.get_or<std::string>("triangulation_direction", "");
				if (!TriDir.empty())
				{
					FString FTriDir = UTF8_TO_TCHAR(TriDir.c_str());
					if (FTriDir.Equals(TEXT("None"), ESearchCase::IgnoreCase))
					{
						BS->PreferredTriangulationDirection = EPreferredTriangulationDirection::None;
						SetCount++;
					}
					else if (FTriDir.Equals(TEXT("Tangential"), ESearchCase::IgnoreCase))
					{
						BS->PreferredTriangulationDirection = EPreferredTriangulationDirection::Tangential;
						SetCount++;
					}
					else if (FTriDir.Equals(TEXT("Radial"), ESearchCase::IgnoreCase))
					{
						BS->PreferredTriangulationDirection = EPreferredTriangulationDirection::Radial;
						SetCount++;
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[WARN] configure(\"settings\") -> unknown triangulation_direction '%s'"), *FTriDir));
					}
				}

				// 1D-specific: bScaleAnimation
				if (bIs1D)
				{
					sol::optional<bool> ScaleAnim = P.get<sol::optional<bool>>("scale_animation");
					if (ScaleAnim.has_value())
					{
						UBlendSpace1D* BS1D = Cast<UBlendSpace1D>(BS);
						if (BS1D)
						{
							BS1D->bScaleAnimation = ScaleAnim.value();
							SetCount++;
						}
					}
				}

				BS->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"settings\") -> %d settings changed"), SetCount));
				return sol::make_object(Lua, true);
			}

			// ---- configure("properties", {key=val, ...}) ----
			if (FType.Equals(TEXT("properties"), ESearchCase::IgnoreCase))
			{
				if (!Params.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"properties\") -> table of {property=value, ...} required"));
					return sol::lua_nil;
				}
				sol::table Props = Params.as<sol::table>();

				const FScopedTransaction Transaction(FText::FromString(TEXT("BlendSpace: Configure Properties")));
				BS->Modify();

				int32 SetCount = 0;
				for (auto& kv : Props)
				{
					if (!kv.first.is<std::string>()) continue;
					FString PropName = UTF8_TO_TCHAR(kv.first.as<std::string>().c_str());

					FProperty* Prop = BS->GetClass()->FindPropertyByName(FName(*PropName));
					if (!Prop)
					{
						Session.Log(FString::Printf(TEXT("[WARN] configure(\"properties\") -> '%s' not found"), *PropName));
						continue;
					}

					void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(BS);

					if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
					{
						bool bVal = kv.second.is<bool>() ? kv.second.as<bool>() : (kv.second.is<double>() && kv.second.as<double>() != 0.0);
						BoolProp->SetPropertyValue(ValuePtr, bVal);
						SetCount++;
					}
					else if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
					{
						FString StrVal = kv.second.is<std::string>() ? UTF8_TO_TCHAR(kv.second.as<std::string>().c_str()) : TEXT("");
						if (!StrVal.IsEmpty())
						{
							if (Prop->ImportText_Direct(*StrVal, ValuePtr, BS, PPF_None))
								SetCount++;
							else
								Session.Log(FString::Printf(TEXT("[WARN] configure(\"properties\") -> failed to parse '%s' for '%s'"), *StrVal, *PropName));
						}
						else if (kv.second.is<double>())
						{
							FNumericProperty* Under = EnumProp->GetUnderlyingProperty();
							if (Under) { Under->SetIntPropertyValue(ValuePtr, static_cast<int64>(kv.second.as<double>())); SetCount++; }
						}
					}
					else if (FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
					{
						if (kv.second.is<double>())
						{
							ByteProp->SetPropertyValue(ValuePtr, static_cast<uint8>(kv.second.as<double>()));
							SetCount++;
						}
						else if (kv.second.is<std::string>())
						{
							if (Prop->ImportText_Direct(*FString(UTF8_TO_TCHAR(kv.second.as<std::string>().c_str())), ValuePtr, BS, PPF_None))
								SetCount++;
						}
					}
					else if (FNumericProperty* NumProp = CastField<FNumericProperty>(Prop))
					{
						if (kv.second.is<double>())
						{
							if (NumProp->IsFloatingPoint())
							{
								NumProp->SetFloatingPointPropertyValue(ValuePtr, kv.second.as<double>());
								SetCount++;
							}
							else if (NumProp->IsInteger())
							{
								NumProp->SetIntPropertyValue(ValuePtr, static_cast<int64>(kv.second.as<double>()));
								SetCount++;
							}
						}
						else if (kv.second.is<std::string>())
						{
							if (Prop->ImportText_Direct(*FString(UTF8_TO_TCHAR(kv.second.as<std::string>().c_str())), ValuePtr, BS, PPF_None))
								SetCount++;
						}
					}
					else if (kv.second.is<std::string>())
					{
						FString StrVal = UTF8_TO_TCHAR(kv.second.as<std::string>().c_str());
						if (Prop->ImportText_Direct(*StrVal, ValuePtr, BS, PPF_None))
							SetCount++;
						else
							Session.Log(FString::Printf(TEXT("[WARN] configure(\"properties\") -> ImportText failed for '%s'"), *PropName));
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[WARN] configure(\"properties\") -> unsupported value for '%s'"), *PropName));
					}
				}

				FPropertyChangedEvent ChangeEvent(nullptr);
				BS->PostEditChangeProperty(ChangeEvent);
				BS->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"properties\") -> %d properties set"), SetCount));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> unknown type. Valid: sample, axis, interpolation, per_bone_override, per_bone_mode, blend_profile, weight_speed, notify_mode, axis_scale, settings, properties"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// duplicate_sample({index=.., position=..})
		// ================================================================
		AssetObj.set_function("duplicate_sample", [BS, bIs1D, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			sol::optional<int> LuaIdx = Params.get<sol::optional<int>>("index");
			if (!LuaIdx.has_value())
			{
				Session.Log(TEXT("[FAIL] duplicate_sample -> 'index' required (1-based)"));
				return sol::lua_nil;
			}
			int32 SourceIndex = LuaIdx.value() - 1; // Convert to 0-based

			TArray<FBlendSample>* Samples = GetMutableSamples(BS);
			if (!Samples || SourceIndex < 0 || SourceIndex >= Samples->Num())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] duplicate_sample -> index %d out of range"), LuaIdx.value()));
				return sol::lua_nil;
			}

			bool bPosValid = false;
			FVector NewPos = ParseSamplePosition(Params, bIs1D, bPosValid);
			if (!bPosValid)
			{
				Session.Log(TEXT("[FAIL] duplicate_sample -> 'position' required"));
				return sol::lua_nil;
			}

			// CRITICAL: Copy by VALUE before AddSample — array may reallocate
			const FBlendSample SourceCopy = (*Samples)[SourceIndex];
			UAnimSequence* Anim = Cast<UAnimSequence>(SourceCopy.Animation);
			if (!Anim)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] duplicate_sample -> source sample[%d] has no valid animation"), LuaIdx.value()));
				return sol::lua_nil;
			}

			const FScopedTransaction Transaction(FText::FromString(TEXT("BlendSpace: Duplicate Sample")));
			BS->Modify();

			int32 NewIndex = BS->AddSample(Anim, NewPos);
			if (NewIndex == INDEX_NONE)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] duplicate_sample -> AddSample failed at (%.2f, %.2f)"), NewPos.X, NewPos.Y));
				return sol::lua_nil;
			}

			// Copy properties from the VALUE copy (safe after potential reallocation)
			Samples = GetMutableSamples(BS); // Re-fetch after potential realloc
			if (Samples && NewIndex >= 0 && NewIndex < Samples->Num())
			{
				(*Samples)[NewIndex].RateScale = SourceCopy.RateScale;
				CopySingleFrameBlendOptions((*Samples)[NewIndex], SourceCopy);
			}

			FinalizeBlendSpace(BS);

			int32 NewLuaIdx = NewIndex + 1;
			Session.Log(FString::Printf(TEXT("[OK] duplicate_sample(%d -> %d) -> '%s' at (%.2f, %.2f)"),
				LuaIdx.value(), NewLuaIdx, *Anim->GetName(), NewPos.X, NewPos.Y));
			return sol::make_object(Lua, NewLuaIdx);
		});

		// ================================================================
		// info() — override default
		// ================================================================
		AssetObj.set_function("info", [BS, bIs1D, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			sol::table Result = Lua.create_table();

			Result["type"] = TCHAR_TO_UTF8(*GetBlendSpaceTypeName(BS));
			Result["path"] = TCHAR_TO_UTF8(*BS->GetPathName());
			Result["num_samples"] = static_cast<int>(BS->GetBlendSamples().Num());
			Result["is_1d"] = bIs1D;

			// Skeleton
			USkeleton* Skel = BS->GetSkeleton();
			Result["skeleton"] = Skel ? TCHAR_TO_UTF8(*Skel->GetName()) : "none";

			// Axes
			sol::table Axes = Lua.create_table();
			int32 NumAxes = bIs1D ? 1 : 2;
			for (int32 i = 0; i < NumAxes; i++)
			{
				const FBlendParameter& Param = BS->GetBlendParameter(i);
				sol::table A = Lua.create_table();
				A["display_name"] = TCHAR_TO_UTF8(*Param.DisplayName);
				A["min"] = Param.Min;
				A["max"] = Param.Max;
				A["grid_divisions"] = Param.GridNum;
				Axes[i + 1] = A;
			}
			Result["axes"] = Axes;

			// Per-bone blend mode
			EBlendSpacePerBoneBlendMode* ModePtr = GetMutablePerBoneBlendMode(BS);
			if (ModePtr)
			{
				Result["per_bone_mode"] = (*ModePtr == EBlendSpacePerBoneBlendMode::BlendProfile) ? "BlendProfile" : "Manual";
			}

			// Per-bone overrides count
			TArray<FPerBoneInterpolation>* Overrides = GetMutablePerBoneOverrides(BS);
			Result["per_bone_overrides_count"] = Overrides ? static_cast<int>(Overrides->Num()) : 0;

			// Notify mode
			switch (BS->NotifyTriggerMode.GetValue())
			{
			case ENotifyTriggerMode::AllAnimations: Result["notify_mode"] = "all"; break;
			case ENotifyTriggerMode::HighestWeightedAnimation: Result["notify_mode"] = "highest"; break;
			case ENotifyTriggerMode::None: Result["notify_mode"] = "none"; break;
			default: Result["notify_mode"] = "unknown"; break;
			}

			// Weight speed
			Result["weight_speed"] = BS->TargetWeightInterpolationSpeedPerSec;
			Result["weight_ease_in_out"] = BS->bTargetWeightInterpolationEaseInOut;

			// Animation settings
			Result["loop"] = BS->bLoop;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
			Result["allow_marker_sync"] = BS->bAllowMarkerBasedSync;
#endif
			Result["allow_mesh_space_blending"] = BS->bAllowMeshSpaceBlending;
			Result["interpolate_using_grid"] = BS->bInterpolateUsingGrid;
			Result["anim_length"] = BS->AnimLength;

			Session.Log(FString::Printf(TEXT("[OK] info() -> %s, %d samples"),
				*GetBlendSpaceTypeName(BS), BS->GetBlendSamples().Num()));
			return Result;
		});
	});
}

REGISTER_LUA_BINDING(BlendSpace, BlendSpaceDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindBlendSpace(Lua, Session);
});
