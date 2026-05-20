// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Tools/NeoStackToolUtils.h"
#include "ScopedTransaction.h"

#include "PhysicalMaterials/PhysicalMaterial.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// HELPERS
// ============================================================================

static const char* CombineModeToString(EFrictionCombineMode::Type Mode)
{
	switch (Mode)
	{
	case EFrictionCombineMode::Average:  return "Average";
	case EFrictionCombineMode::Min:      return "Min";
	case EFrictionCombineMode::Multiply: return "Multiply";
	case EFrictionCombineMode::Max:      return "Max";
	default:                             return "Average";
	}
}

static bool ParseCombineMode(const FString& Str, EFrictionCombineMode::Type& OutMode)
{
	if (Str.Equals(TEXT("Average"), ESearchCase::IgnoreCase))       { OutMode = EFrictionCombineMode::Average;  return true; }
	if (Str.Equals(TEXT("Min"), ESearchCase::IgnoreCase))           { OutMode = EFrictionCombineMode::Min;      return true; }
	if (Str.Equals(TEXT("Multiply"), ESearchCase::IgnoreCase))      { OutMode = EFrictionCombineMode::Multiply; return true; }
	if (Str.Equals(TEXT("Max"), ESearchCase::IgnoreCase))           { OutMode = EFrictionCombineMode::Max;      return true; }
	return false;
}

static const char* SoftCollisionModeToString(EPhysicalMaterialSoftCollisionMode Mode)
{
	switch (Mode)
	{
	case EPhysicalMaterialSoftCollisionMode::None:              return "None";
	case EPhysicalMaterialSoftCollisionMode::RelativeThickness: return "RelativeThickness";
	case EPhysicalMaterialSoftCollisionMode::AbsoluteThickess:  return "AbsoluteThickness";
	default:                                                    return "None";
	}
}

static bool ParseSoftCollisionMode(const FString& Str, EPhysicalMaterialSoftCollisionMode& OutMode)
{
	if (Str.Equals(TEXT("None"), ESearchCase::IgnoreCase))              { OutMode = EPhysicalMaterialSoftCollisionMode::None;              return true; }
	if (Str.Equals(TEXT("RelativeThickness"), ESearchCase::IgnoreCase)) { OutMode = EPhysicalMaterialSoftCollisionMode::RelativeThickness; return true; }
	if (Str.Equals(TEXT("AbsoluteThickness"), ESearchCase::IgnoreCase)) { OutMode = EPhysicalMaterialSoftCollisionMode::AbsoluteThickess; return true; }
	return false;
}

// ============================================================================
// BINDING
// ============================================================================

static TArray<FLuaFunctionDoc> PhysicalMaterialDocs = {};

static void BindPhysicalMaterial(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_physical_material", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		UPhysicalMaterial* PhysMat = LoadObject<UPhysicalMaterial>(nullptr, *FPath);
		if (!PhysMat) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"PhysicalMaterial enrichment methods:\n"
			"\n"
			"info() — structured summary:\n"
			"  friction, static_friction, friction_combine_mode, override_friction_combine_mode,\n"
			"  restitution, restitution_combine_mode, override_restitution_combine_mode,\n"
			"  density, surface_type, raise_mass_to_power,\n"
			"  sleep_linear_velocity_threshold, sleep_angular_velocity_threshold, sleep_counter_threshold,\n"
			"  strength (tensile, compression, shear), damage_threshold_multiplier, debug_color,\n"
			"  soft_collision_mode, soft_collision_thickness, base_friction_impulse (experimental)\n"
			"\n"
			"configure(params):\n"
			"  configure({friction=0.7, static_friction=0.8, friction_combine_mode=\"Average\",\n"
			"             override_friction_combine_mode=true,\n"
			"             restitution=0.3, restitution_combine_mode=\"Min\",\n"
			"             override_restitution_combine_mode=true,\n"
			"             density=1.1, surface_type=\"SurfaceType1\",\n"
			"             raise_mass_to_power=0.75,\n"
			"             sleep_linear_velocity_threshold=1.0, sleep_angular_velocity_threshold=1.0,\n"
			"             sleep_counter_threshold=3,\n"
			"             tensile_strength=100.0, compression_strength=200.0, shear_strength=50.0,\n"
			"             damage_threshold_multiplier=1.0,\n"
			"             debug_color={r=1.0, g=0.0, b=0.0, a=1.0},\n"
			"             soft_collision_mode=\"None\", soft_collision_thickness=0.1,\n"
			"             base_friction_impulse=0.0})\n"
			"  Combine modes: Average, Min, Multiply, Max\n"
			"  Soft collision modes: None, RelativeThickness, AbsoluteThickness\n"
			"  Surface types: SurfaceType_Default, SurfaceType1..SurfaceType62 (project-defined names)\n";

		// ================================================================
		// info()
		// ================================================================
		AssetObj.set_function("info", [PhysMat, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(PhysMat))
			{
				Session.Log(TEXT("[FAIL] info -> asset no longer valid"));
				return sol::lua_nil;
			}

			sol::table Result = Lua.create_table();

			// Surface properties
			Result["friction"] = PhysMat->Friction;
			Result["static_friction"] = PhysMat->StaticFriction;
			Result["friction_combine_mode"] = CombineModeToString(PhysMat->FrictionCombineMode.GetValue());
			Result["override_friction_combine_mode"] = static_cast<bool>(PhysMat->bOverrideFrictionCombineMode);

			Result["restitution"] = PhysMat->Restitution;
			Result["restitution_combine_mode"] = CombineModeToString(PhysMat->RestitutionCombineMode.GetValue());
			Result["override_restitution_combine_mode"] = static_cast<bool>(PhysMat->bOverrideRestitutionCombineMode);

			// Object properties
			Result["density"] = PhysMat->Density;
			Result["raise_mass_to_power"] = PhysMat->RaiseMassToPower;
			Result["sleep_linear_velocity_threshold"] = PhysMat->SleepLinearVelocityThreshold;
			Result["sleep_angular_velocity_threshold"] = PhysMat->SleepAngularVelocityThreshold;
			Result["sleep_counter_threshold"] = PhysMat->SleepCounterThreshold;

			// Surface type
			const UEnum* SurfaceEnum = StaticEnum<EPhysicalSurface>();
			if (SurfaceEnum)
			{
				FString SurfaceName = SurfaceEnum->GetNameStringByValue(static_cast<int64>(PhysMat->SurfaceType.GetValue()));
				Result["surface_type"] = TCHAR_TO_UTF8(*SurfaceName);
			}
			else
			{
				Result["surface_type"] = static_cast<int>(PhysMat->SurfaceType.GetValue());
			}

			// Strength
			sol::table StrengthTable = Lua.create_table();
			StrengthTable["tensile"] = PhysMat->Strength.TensileStrength;
			StrengthTable["compression"] = PhysMat->Strength.CompressionStrength;
			StrengthTable["shear"] = PhysMat->Strength.ShearStrength;
			Result["strength"] = StrengthTable;

			// Damage modifier
			Result["damage_threshold_multiplier"] = PhysMat->DamageModifier.DamageThresholdMultiplier;

			// Debug color
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			sol::table ColorTable = Lua.create_table();
			ColorTable["r"] = PhysMat->DebugColor.R;
			ColorTable["g"] = PhysMat->DebugColor.G;
			ColorTable["b"] = PhysMat->DebugColor.B;
			ColorTable["a"] = PhysMat->DebugColor.A;
			Result["debug_color"] = ColorTable;
#endif

			// Experimental properties
			Result["soft_collision_mode"] = SoftCollisionModeToString(PhysMat->SoftCollisionMode);
			Result["soft_collision_thickness"] = PhysMat->SoftCollisionThickness;
			Result["base_friction_impulse"] = PhysMat->BaseFrictionImpulse;

			Session.Log(FString::Printf(TEXT("[OK] info() -> PhysicalMaterial, friction=%.2f, restitution=%.2f, density=%.2f"),
				(double)PhysMat->Friction, (double)PhysMat->Restitution, (double)PhysMat->Density));
			return Result;
		});

		// ================================================================
		// configure(params)
		// ================================================================
		AssetObj.set_function("configure", [PhysMat, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(PhysMat))
			{
				Session.Log(TEXT("[FAIL] configure -> asset no longer valid"));
				return sol::lua_nil;
			}

			const FScopedTransaction Transaction(FText::FromString(TEXT("PhysicalMaterial: Configure")));
			PhysMat->Modify();
			bool bModified = false;
			FString Changes;

			// Friction
			sol::optional<double> Friction = Params.get<sol::optional<double>>("friction");
			if (Friction.has_value())
			{
				PhysMat->Friction = FMath::Max(0.0f, static_cast<float>(Friction.value()));
				Changes += FString::Printf(TEXT(" friction=%.2f"), (double)PhysMat->Friction);
				bModified = true;
			}

			// Static Friction
			sol::optional<double> StaticFriction = Params.get<sol::optional<double>>("static_friction");
			if (StaticFriction.has_value())
			{
				PhysMat->StaticFriction = FMath::Max(0.0f, static_cast<float>(StaticFriction.value()));
				Changes += FString::Printf(TEXT(" static_friction=%.2f"), (double)PhysMat->StaticFriction);
				bModified = true;
			}

			// Friction combine mode
			sol::optional<std::string> FrictionCombine = Params.get<sol::optional<std::string>>("friction_combine_mode");
			if (FrictionCombine.has_value())
			{
				FString ModeStr = UTF8_TO_TCHAR(FrictionCombine.value().c_str());
				EFrictionCombineMode::Type Mode;
				if (ParseCombineMode(ModeStr, Mode))
				{
					PhysMat->FrictionCombineMode = Mode;
					Changes += FString::Printf(TEXT(" friction_combine_mode=%s"), *ModeStr);
					bModified = true;
				}
				else
				{
					Session.Log(FString::Printf(TEXT("[WARN] configure -> invalid friction_combine_mode '%s'. Valid: Average, Min, Multiply, Max"), *ModeStr));
				}
			}

			// Override friction combine mode
			sol::optional<bool> OverrideFrictionCombine = Params.get<sol::optional<bool>>("override_friction_combine_mode");
			if (OverrideFrictionCombine.has_value())
			{
				PhysMat->bOverrideFrictionCombineMode = OverrideFrictionCombine.value();
				Changes += FString::Printf(TEXT(" override_friction_combine_mode=%s"), OverrideFrictionCombine.value() ? TEXT("true") : TEXT("false"));
				bModified = true;
			}

			// Restitution
			sol::optional<double> Restitution = Params.get<sol::optional<double>>("restitution");
			if (Restitution.has_value())
			{
				PhysMat->Restitution = FMath::Clamp(static_cast<float>(Restitution.value()), 0.0f, 1.0f);
				Changes += FString::Printf(TEXT(" restitution=%.2f"), (double)PhysMat->Restitution);
				bModified = true;
			}

			// Restitution combine mode
			sol::optional<std::string> RestitutionCombine = Params.get<sol::optional<std::string>>("restitution_combine_mode");
			if (RestitutionCombine.has_value())
			{
				FString ModeStr = UTF8_TO_TCHAR(RestitutionCombine.value().c_str());
				EFrictionCombineMode::Type Mode;
				if (ParseCombineMode(ModeStr, Mode))
				{
					PhysMat->RestitutionCombineMode = Mode;
					Changes += FString::Printf(TEXT(" restitution_combine_mode=%s"), *ModeStr);
					bModified = true;
				}
				else
				{
					Session.Log(FString::Printf(TEXT("[WARN] configure -> invalid restitution_combine_mode '%s'. Valid: Average, Min, Multiply, Max"), *ModeStr));
				}
			}

			// Override restitution combine mode
			sol::optional<bool> OverrideRestitutionCombine = Params.get<sol::optional<bool>>("override_restitution_combine_mode");
			if (OverrideRestitutionCombine.has_value())
			{
				PhysMat->bOverrideRestitutionCombineMode = OverrideRestitutionCombine.value();
				Changes += FString::Printf(TEXT(" override_restitution_combine_mode=%s"), OverrideRestitutionCombine.value() ? TEXT("true") : TEXT("false"));
				bModified = true;
			}

			// Density
			sol::optional<double> Density = Params.get<sol::optional<double>>("density");
			if (Density.has_value())
			{
				PhysMat->Density = FMath::Max(0.0f, static_cast<float>(Density.value()));
				Changes += FString::Printf(TEXT(" density=%.2f"), (double)PhysMat->Density);
				bModified = true;
			}

			// Raise mass to power
			sol::optional<double> RaiseMass = Params.get<sol::optional<double>>("raise_mass_to_power");
			if (RaiseMass.has_value())
			{
				PhysMat->RaiseMassToPower = FMath::Clamp(static_cast<float>(RaiseMass.value()), 0.1f, 1.0f);
				Changes += FString::Printf(TEXT(" raise_mass_to_power=%.2f"), (double)PhysMat->RaiseMassToPower);
				bModified = true;
			}

			// Sleep thresholds
			sol::optional<double> SleepLinear = Params.get<sol::optional<double>>("sleep_linear_velocity_threshold");
			if (SleepLinear.has_value())
			{
				PhysMat->SleepLinearVelocityThreshold = FMath::Max(0.0f, static_cast<float>(SleepLinear.value()));
				Changes += FString::Printf(TEXT(" sleep_linear=%.2f"), (double)PhysMat->SleepLinearVelocityThreshold);
				bModified = true;
			}

			sol::optional<double> SleepAngular = Params.get<sol::optional<double>>("sleep_angular_velocity_threshold");
			if (SleepAngular.has_value())
			{
				PhysMat->SleepAngularVelocityThreshold = FMath::Max(0.0f, static_cast<float>(SleepAngular.value()));
				Changes += FString::Printf(TEXT(" sleep_angular=%.2f"), (double)PhysMat->SleepAngularVelocityThreshold);
				bModified = true;
			}

			sol::optional<int> SleepCounter = Params.get<sol::optional<int>>("sleep_counter_threshold");
			if (SleepCounter.has_value())
			{
				PhysMat->SleepCounterThreshold = FMath::Max(0, SleepCounter.value());
				Changes += FString::Printf(TEXT(" sleep_counter=%d"), PhysMat->SleepCounterThreshold);
				bModified = true;
			}

			// Surface type
			sol::optional<std::string> SurfaceType = Params.get<sol::optional<std::string>>("surface_type");
			if (SurfaceType.has_value())
			{
				FString SurfStr = UTF8_TO_TCHAR(SurfaceType.value().c_str());
				const UEnum* SurfaceEnum = StaticEnum<EPhysicalSurface>();
				if (SurfaceEnum)
				{
					int64 EnumValue = SurfaceEnum->GetValueByNameString(SurfStr);
					if (EnumValue == INDEX_NONE)
					{
						// Try with prefix
						EnumValue = SurfaceEnum->GetValueByNameString(FString::Printf(TEXT("EPhysicalSurface::%s"), *SurfStr));
					}
					if (EnumValue != INDEX_NONE)
					{
						PhysMat->SurfaceType = static_cast<EPhysicalSurface>(EnumValue);
						Changes += FString::Printf(TEXT(" surface_type=%s"), *SurfStr);
						bModified = true;
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[WARN] configure -> invalid surface_type '%s'"), *SurfStr));
					}
				}
			}

			// Strength properties
			sol::optional<double> Tensile = Params.get<sol::optional<double>>("tensile_strength");
			if (Tensile.has_value())
			{
				PhysMat->Strength.TensileStrength = FMath::Max(0.0f, static_cast<float>(Tensile.value()));
				Changes += FString::Printf(TEXT(" tensile_strength=%.1f"), (double)PhysMat->Strength.TensileStrength);
				bModified = true;
			}

			sol::optional<double> Compression = Params.get<sol::optional<double>>("compression_strength");
			if (Compression.has_value())
			{
				PhysMat->Strength.CompressionStrength = FMath::Max(0.0f, static_cast<float>(Compression.value()));
				Changes += FString::Printf(TEXT(" compression_strength=%.1f"), (double)PhysMat->Strength.CompressionStrength);
				bModified = true;
			}

			sol::optional<double> Shear = Params.get<sol::optional<double>>("shear_strength");
			if (Shear.has_value())
			{
				PhysMat->Strength.ShearStrength = FMath::Max(0.0f, static_cast<float>(Shear.value()));
				Changes += FString::Printf(TEXT(" shear_strength=%.1f"), (double)PhysMat->Strength.ShearStrength);
				bModified = true;
			}

			// Damage modifier
			sol::optional<double> DamageMultiplier = Params.get<sol::optional<double>>("damage_threshold_multiplier");
			if (DamageMultiplier.has_value())
			{
				PhysMat->DamageModifier.DamageThresholdMultiplier = FMath::Max(0.0f, static_cast<float>(DamageMultiplier.value()));
				Changes += FString::Printf(TEXT(" damage_threshold_multiplier=%.2f"), (double)PhysMat->DamageModifier.DamageThresholdMultiplier);
				bModified = true;
			}

			// Debug color (5.6+ only)
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			sol::optional<sol::table> DebugColorOpt = Params.get<sol::optional<sol::table>>("debug_color");
			if (DebugColorOpt.has_value())
			{
				sol::table ColorTbl = DebugColorOpt.value();
				sol::optional<double> R = ColorTbl.get<sol::optional<double>>("r");
				sol::optional<double> G = ColorTbl.get<sol::optional<double>>("g");
				sol::optional<double> B = ColorTbl.get<sol::optional<double>>("b");
				sol::optional<double> A = ColorTbl.get<sol::optional<double>>("a");
				if (R.has_value()) PhysMat->DebugColor.R = static_cast<float>(R.value());
				if (G.has_value()) PhysMat->DebugColor.G = static_cast<float>(G.value());
				if (B.has_value()) PhysMat->DebugColor.B = static_cast<float>(B.value());
				if (A.has_value()) PhysMat->DebugColor.A = static_cast<float>(A.value());
				Changes += FString::Printf(TEXT(" debug_color=(%.2f,%.2f,%.2f,%.2f)"),
					(double)PhysMat->DebugColor.R, (double)PhysMat->DebugColor.G,
					(double)PhysMat->DebugColor.B, (double)PhysMat->DebugColor.A);
				bModified = true;
			}
#endif

			// Experimental: Soft collision mode
			sol::optional<std::string> SoftMode = Params.get<sol::optional<std::string>>("soft_collision_mode");
			if (SoftMode.has_value())
			{
				FString ModeStr = UTF8_TO_TCHAR(SoftMode.value().c_str());
				EPhysicalMaterialSoftCollisionMode Mode;
				if (ParseSoftCollisionMode(ModeStr, Mode))
				{
					PhysMat->SoftCollisionMode = Mode;
					Changes += FString::Printf(TEXT(" soft_collision_mode=%s"), *ModeStr);
					bModified = true;
				}
				else
				{
					Session.Log(FString::Printf(TEXT("[WARN] configure -> invalid soft_collision_mode '%s'. Valid: None, RelativeThickness, AbsoluteThickness"), *ModeStr));
				}
			}

			// Experimental: Soft collision thickness
			sol::optional<double> SoftThickness = Params.get<sol::optional<double>>("soft_collision_thickness");
			if (SoftThickness.has_value())
			{
				PhysMat->SoftCollisionThickness = FMath::Max(0.0f, static_cast<float>(SoftThickness.value()));
				Changes += FString::Printf(TEXT(" soft_collision_thickness=%.3f"), (double)PhysMat->SoftCollisionThickness);
				bModified = true;
			}

			// Experimental: Base friction impulse
			sol::optional<double> BaseFriction = Params.get<sol::optional<double>>("base_friction_impulse");
			if (BaseFriction.has_value())
			{
				PhysMat->BaseFrictionImpulse = FMath::Max(0.0f, static_cast<float>(BaseFriction.value()));
				Changes += FString::Printf(TEXT(" base_friction_impulse=%.2f"), (double)PhysMat->BaseFrictionImpulse);
				bModified = true;
			}

			if (bModified)
			{
				FPropertyChangedEvent Event(nullptr, EPropertyChangeType::ValueSet);
				PhysMat->PostEditChangeProperty(Event);
				PhysMat->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(%s)"), *Changes.TrimStart()));
				return sol::make_object(Lua, true);
			}

			Session.Log(TEXT("[OK] configure() -> nothing changed. Valid keys: friction, static_friction, friction_combine_mode, override_friction_combine_mode, restitution, restitution_combine_mode, override_restitution_combine_mode, density, raise_mass_to_power, sleep_linear_velocity_threshold, sleep_angular_velocity_threshold, sleep_counter_threshold, surface_type, tensile_strength, compression_strength, shear_strength, damage_threshold_multiplier, debug_color, soft_collision_mode, soft_collision_thickness, base_friction_impulse"));
			return sol::make_object(Lua, true);
		});
	});
}

REGISTER_LUA_BINDING(PhysicalMaterial, PhysicalMaterialDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindPhysicalMaterial(Lua, Session);
});
