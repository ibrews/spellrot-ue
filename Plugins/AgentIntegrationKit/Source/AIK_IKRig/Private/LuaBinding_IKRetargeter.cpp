// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Tools/NeoStackToolUtils.h"
#include "ScopedTransaction.h"

#include "Retargeter/IKRetargeter.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "Rig/IKRigDefinition.h"
#include "Engine/SkeletalMesh.h"

// Per-op configuration (5.7+ headers)
#include "RetargetEditor/IKRetargeterPoseGenerator.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
#include "Retargeter/RetargetOps/FKChainsOp.h"
#include "Retargeter/RetargetOps/IKChainsOp.h"
#include "Retargeter/RetargetOps/PelvisMotionOp.h"
#include "Retargeter/RetargetOps/RootMotionGeneratorOp.h"
#endif
#include "Lua/LuaDynamicTypeHelper.h"
#include "Modules/ModuleManager.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// UE 5.7 linker workaround: PostLoad() on these op settings structs is declared
// virtual but not exported with IKRIG_API. Provide empty stubs to satisfy the
// linker when GetSettings() returns by-value copies that need a vtable entry.
// The engine has real PostLoad() implementations that handle version migration,
// but those only matter during asset deserialization — not for our use case of
// reading/writing settings through the op controller API (GetSettings/SetSettings).
// This is an ODR violation across DLL boundaries, but benign: PostLoad is never
// called on the local copies this plugin creates.
// ============================================================================
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
void FIKRetargetIKChainsOpSettings::PostLoad(const FIKRigObjectVersion::Type) {}
void FIKRetargetPelvisMotionOpSettings::PostLoad(const FIKRigObjectVersion::Type) {}
void FIKRetargetRootMotionOpSettings::PostLoad(const FIKRigObjectVersion::Type) {}
#endif

// ============================================================================
// Helpers
// ============================================================================

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
// Backward-compatible short aliases for retarget op types (5.6+)
static const TMap<FString, FString> OpAliases = {
	{ TEXT("Pelvis"), TEXT("PelvisMotion") },
	{ TEXT("FK"), TEXT("FKChains") },
	{ TEXT("IK"), TEXT("IKChains") },
	{ TEXT("RunIK"), TEXT("RunIKRig") },
	{ TEXT("RootMotionGenerator"), TEXT("RootMotion") },
	{ TEXT("Curve"), TEXT("CurveRemap") },
	{ TEXT("Stride"), TEXT("StrideWarping") },
	{ TEXT("Floor"), TEXT("FloorConstraint") },
	{ TEXT("Pin"), TEXT("PinBone") },
	{ TEXT("Filter"), TEXT("FilterBone") },
	{ TEXT("RetargetPose"), TEXT("AdditivePose") },
};

static const TArray<FString> OpPrefixes = { TEXT("FIKRetarget"), TEXT("IKRetarget") };
static const TArray<FString> OpSuffixes = { TEXT("Op") };

/** Resolve user-friendly op type aliases to full script struct paths */
static FString ResolveOpTypePath(const FString& TypeName)
{
	// Try alias first
	FString Resolved = TypeName;
	if (const FString* Alias = OpAliases.Find(TypeName))
		Resolved = *Alias;

	// Dynamic struct discovery
	UScriptStruct* OpStruct = LuaDynamicType::FindDerivedStruct(
		FIKRetargetOpBase::StaticStruct(), Resolved, OpPrefixes, OpSuffixes);
	if (OpStruct)
		return OpStruct->GetPathName();

	return FString();
}
#endif // ENGINE_MINOR_VERSION >= 6

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
/** Find op index by name (5.6+ retarget ops system) */
static int32 FindOpIndexByName(UIKRetargeterController* Ctrl, const FString& OpName)
{
	return Ctrl->GetIndexOfOpByName(FName(*OpName));
}
#endif

/** Parse "source" / "target" string to enum */
static bool ParseSourceOrTarget(const std::string& Str, ERetargetSourceOrTarget& OutVal)
{
	FString FStr = UTF8_TO_TCHAR(Str.c_str());
	if (FStr.Equals(TEXT("source"), ESearchCase::IgnoreCase))
	{
		OutVal = ERetargetSourceOrTarget::Source;
		return true;
	}
	if (FStr.Equals(TEXT("target"), ESearchCase::IgnoreCase))
	{
		OutVal = ERetargetSourceOrTarget::Target;
		return true;
	}
	return false;
}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
/** Parse FK rotation mode string to enum */
static bool ParseFKRotationMode(const std::string& Str, EFKChainRotationMode& OutVal)
{
	FString FStr = UTF8_TO_TCHAR(Str.c_str());
	if (FStr.Equals(TEXT("None"), ESearchCase::IgnoreCase))             { OutVal = EFKChainRotationMode::None; return true; }
	if (FStr.Equals(TEXT("Interpolated"), ESearchCase::IgnoreCase))     { OutVal = EFKChainRotationMode::Interpolated; return true; }
	if (FStr.Equals(TEXT("OneToOne"), ESearchCase::IgnoreCase))         { OutVal = EFKChainRotationMode::OneToOne; return true; }
	if (FStr.Equals(TEXT("OneToOneReversed"), ESearchCase::IgnoreCase)) { OutVal = EFKChainRotationMode::OneToOneReversed; return true; }
	if (FStr.Equals(TEXT("MatchChain"), ESearchCase::IgnoreCase))       { OutVal = EFKChainRotationMode::MatchChain; return true; }
	if (FStr.Equals(TEXT("MatchScaledChain"), ESearchCase::IgnoreCase)) { OutVal = EFKChainRotationMode::MatchScaledChain; return true; }
	if (FStr.Equals(TEXT("CopyLocal"), ESearchCase::IgnoreCase))        { OutVal = EFKChainRotationMode::CopyLocal; return true; }
	return false;
}

/** Parse FK translation mode string to enum */
static bool ParseFKTranslationMode(const std::string& Str, EFKChainTranslationMode& OutVal)
{
	FString FStr = UTF8_TO_TCHAR(Str.c_str());
	if (FStr.Equals(TEXT("None"), ESearchCase::IgnoreCase))                           { OutVal = EFKChainTranslationMode::None; return true; }
	if (FStr.Equals(TEXT("GloballyScaled"), ESearchCase::IgnoreCase))                 { OutVal = EFKChainTranslationMode::GloballyScaled; return true; }
	if (FStr.Equals(TEXT("Absolute"), ESearchCase::IgnoreCase))                       { OutVal = EFKChainTranslationMode::Absolute; return true; }
	if (FStr.Equals(TEXT("StretchBoneLengthUniformly"), ESearchCase::IgnoreCase))     { OutVal = EFKChainTranslationMode::StretchBoneLengthUniformly; return true; }
	if (FStr.Equals(TEXT("StretchBoneLengthNonUniformly"), ESearchCase::IgnoreCase))  { OutVal = EFKChainTranslationMode::StretchBoneLengthNonUniformly; return true; }
	if (FStr.Equals(TEXT("OrientAndScale"), ESearchCase::IgnoreCase))                 { OutVal = EFKChainTranslationMode::OrientAndScale; return true; }
	return false;
}
/** Convert FK rotation mode enum to string */
static const char* FKRotationModeToString(EFKChainRotationMode Mode)
{
	switch (Mode)
	{
	case EFKChainRotationMode::None:              return "None";
	case EFKChainRotationMode::Interpolated:      return "Interpolated";
	case EFKChainRotationMode::OneToOne:          return "OneToOne";
	case EFKChainRotationMode::OneToOneReversed:  return "OneToOneReversed";
	case EFKChainRotationMode::MatchChain:        return "MatchChain";
	case EFKChainRotationMode::MatchScaledChain:  return "MatchScaledChain";
	case EFKChainRotationMode::CopyLocal:         return "CopyLocal";
	default:                                      return "Unknown";
	}
}

/** Convert FK translation mode enum to string */
static const char* FKTranslationModeToString(EFKChainTranslationMode Mode)
{
	switch (Mode)
	{
	case EFKChainTranslationMode::None:                          return "None";
	case EFKChainTranslationMode::GloballyScaled:                return "GloballyScaled";
	case EFKChainTranslationMode::Absolute:                      return "Absolute";
	case EFKChainTranslationMode::StretchBoneLengthUniformly:    return "StretchBoneLengthUniformly";
	case EFKChainTranslationMode::StretchBoneLengthNonUniformly: return "StretchBoneLengthNonUniformly";
	case EFKChainTranslationMode::OrientAndScale:                return "OrientAndScale";
	default:                                                     return "Unknown";
	}
}
#endif // ENGINE_MINOR_VERSION >= 7

/** Parse auto map chain type string to enum */
static bool ParseAutoMapType(const std::string& Str, EAutoMapChainType& OutVal)
{
	FString FStr = UTF8_TO_TCHAR(Str.c_str());
	if (FStr.Equals(TEXT("exact"), ESearchCase::IgnoreCase))  { OutVal = EAutoMapChainType::Exact; return true; }
	if (FStr.Equals(TEXT("fuzzy"), ESearchCase::IgnoreCase))  { OutVal = EAutoMapChainType::Fuzzy; return true; }
	if (FStr.Equals(TEXT("clear"), ESearchCase::IgnoreCase))  { OutVal = EAutoMapChainType::Clear; return true; }
	return false;
}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
/** Get op type display name from its UScriptStruct */
static FString GetOpTypeDisplayName(const FIKRetargetOpBase* Op)
{
	if (!Op) return TEXT("Unknown");
	const UScriptStruct* OpType = Op->GetType();
	if (!OpType) return TEXT("Unknown");
	// Strip "FIKRetarget" prefix and "Op" suffix for display
	FString Name = OpType->GetName();
	Name.RemoveFromStart(TEXT("FIKRetarget"));
	Name.RemoveFromEnd(TEXT("Op"));
	return Name;
}
#endif

// ============================================================================
// Binding registration
// ============================================================================

static TArray<FLuaFunctionDoc> IKRetargeterDocs = {};

static void BindIKRetargeter(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_ik_retargeter", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		UIKRetargeter* Retargeter = LoadObject<UIKRetargeter>(nullptr, *FPath);
		if (!Retargeter) return;

		UIKRetargeterController* Ctrl = UIKRetargeterController::GetController(Retargeter);
		if (!Ctrl) return;

		// ====================================================================
		// help text
		// ====================================================================
		AssetObj["_help_text"] =
			"IK Retargeter — element types for add/remove/list/configure:\n"
			"  op            — retarget op in the stack\n"
			"  default_ops   — add standard op set (PelvisMotion, FK, IK, RunIK, RootMotion)\n"
			"  pose          — retarget pose (source or target)\n"
			"  chain_mapping — source-to-target chain mapping\n"
			"\n"
			"add(type, params):\n"
			"  add(\"op\", {type=\"FKChains\", name=\"My FK\", enabled=true})\n"
			"    Op types: dynamically discovered — common: Pelvis, FK, IK, RunIK, RootMotion, CurveRemap, Pin, Filter\n"
			"  add(\"default_ops\")  — add standard op set\n"
			"  add(\"pose\", {name=\"Custom\", for=\"target\"})\n"
			"\n"
			"remove(type, id):\n"
			"  remove(\"op\", \"FK Chains\")  — by name\n"
			"  remove(\"all_ops\")           — remove all ops\n"
			"  remove(\"pose\", {name=\"Custom\", for=\"target\"})\n"
			"\n"
			"list(type, opt_arg?):\n"
			"  list(\"ops\")            — op stack with index/name/type/enabled\n"
			"  list(\"chain_mappings\") — target->source chain pairs\n"
			"  list(\"poses\")          — source and target pose names\n"
			"  list(\"fk_chains\")      — FK chain settings (optional op_name arg)\n"
			"  list(\"fk_chains\", \"My FK Op\")  — FK settings for specific op\n"
			"  list(\"ik_chains\")      — IK chain settings (optional op_name arg)\n"
			"\n"
			"configure(type, params):\n"
			"  configure(\"source_ikrig\", \"/Game/Rigs/Source_IKRig\")\n"
			"  configure(\"target_ikrig\", \"/Game/Rigs/Target_IKRig\")\n"
			"  configure(\"preview_mesh\", {mesh=\"/Game/Mesh\", for=\"source\"})\n"
			"  configure(\"op\", {name=\"FK Chains\", enabled=false})\n"
			"  configure(\"chain_mapping\", {target=\"LeftArm\", source=\"LeftArm\"})\n"
			"  configure(\"fk_chain\", {chain=\"Spine\", rotation_mode=\"Interpolated\", ...})\n"
			"  configure(\"ik_chain\", {chain=\"LeftFoot\", enable_ik=true, ...})\n"
			"  configure(\"pelvis\", {op_name=.., rotation_alpha=1.0, ...})\n"
			"  configure(\"root_motion\", {op_name=.., root_motion_source=\"CopyFromSourceRoot\", ...})\n"
			"\n"
			"Action methods:\n"
			"  auto_map(mode)           — \"exact\", \"fuzzy\", \"clear\"\n"
			"  auto_align({for=\"target\", method=\"chain_to_chain\"})  — auto-align bones (methods: chain_to_chain, mesh_to_mesh, local_rotation, global_rotation)\n"
			"  move_op(name, to_index)  — reorder op in stack\n"
			"  set_parent_op(\"ChildOp\", \"ParentOp\") — set parent/child op relationship\n"
			"  get_parent_op(\"OpName\") — get parent op name (nil if none)\n"
			"  run_op_setup(name_or_index) — trigger initial setup on an op\n"
			"  reset_chain_settings({chain=.., op_name=..}) — reset chain to defaults\n"
			"  get_target_ikrig_for_op(\"OpName\") — get target IK Rig path for op\n"
			"  duplicate_pose({name=.., new_name=.., for=..})\n"
			"  rename_pose({old_name=.., new_name=.., for=..})\n"
			"  set_current_pose({name=.., for=..})\n"
			"  edit_pose_bone({bone=.., rotation={p=0,y=0,r=0}, for=..})\n"
			"  get_rotation_offset({bone=.., for=..}) — read bone rotation offset\n"
			"  set_root_offset({offset={x=0,y=0,z=0}, for=..})\n"
			"  get_root_offset({for=..}) — read root translation offset\n"
			"  get_source_chain({target=.., op_name=..}) — get mapped source chain\n"
			"  get_pelvis_bones({op_name=..}) — read pelvis bone assignments\n"
			"  get_root_bones({op_name=..}) — read root motion bone assignments\n"
			"  reset_pose({pose_name=.., bones={..}, for=..})\n"
			"  snap_to_ground({bone=.., for=..})\n"
			"  info()                   — asset summary\n";

		// ====================================================================
		// add(type, params)
		// ====================================================================
		AssetObj.set_function("add", [Ctrl, Retargeter, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> Params, sol::this_state St) -> sol::object
		{
			sol::state_view Lv(St);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			// ---- add("op", {type=.., name=.., enabled=..}) — 5.6+ retarget ops system ----
			if (FType.Equals(TEXT("op"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value())
				{
					Session.Log(TEXT("[FAIL] add(\"op\") -> params required: {type=\"FKChains\", name=.., enabled=true}"));
					return sol::lua_nil;
				}
				sol::table P = Params.value();
				std::string OpType = P.get_or<std::string>("type", "");
				if (OpType.empty())
				{
					Session.Log(TEXT("[FAIL] add(\"op\") -> 'type' required"));
					return sol::lua_nil;
				}

				FString OpPath = ResolveOpTypePath(UTF8_TO_TCHAR(OpType.c_str()));
				if (OpPath.IsEmpty())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"op\") -> unknown op type '%s'. Valid: %s"),
					UTF8_TO_TCHAR(OpType.c_str()),
					*LuaDynamicType::FormatAvailableStructTypes(FIKRetargetOpBase::StaticStruct(), OpPrefixes, OpSuffixes)));
					return sol::lua_nil;
				}

				FScopedTransaction Txn(FText::FromString(TEXT("Add Retarget Op")));
				int32 Idx = Ctrl->AddRetargetOp(OpPath);
				if (Idx < 0)
				{
					Session.Log(TEXT("[FAIL] add(\"op\") -> AddRetargetOp failed"));
					return sol::lua_nil;
				}

				// Set name if provided
				std::string NameStr = P.get_or<std::string>("name", "");
				if (!NameStr.empty())
				{
					Ctrl->SetOpName(FName(UTF8_TO_TCHAR(NameStr.c_str())), Idx);
				}

				// Set enabled state
				sol::optional<bool> Enabled = P.get<sol::optional<bool>>("enabled");
				if (Enabled.has_value())
				{
					Ctrl->SetRetargetOpEnabled(Idx, Enabled.value());
				}

				// Assign IK rigs to the new op
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
				const UIKRigDefinition* SourceRig = Ctrl->GetIKRig(ERetargetSourceOrTarget::Source);
				if (SourceRig)
				{
					Ctrl->AssignIKRigToAllOps(ERetargetSourceOrTarget::Source, SourceRig);
				}
				const UIKRigDefinition* TargetRig = Ctrl->GetIKRig(ERetargetSourceOrTarget::Target);
				if (TargetRig)
				{
					Ctrl->AssignIKRigToAllOps(ERetargetSourceOrTarget::Target, TargetRig);
				}
#endif

				FName FinalName = Ctrl->GetOpName(Idx);
				Session.Log(FString::Printf(TEXT("[OK] add(\"op\", type=\"%s\") -> index %d, name \"%s\""),
					UTF8_TO_TCHAR(OpType.c_str()), Idx, *FinalName.ToString()));
				return sol::make_object(Lv, Idx);
			}

			// ---- add("default_ops") ----
			if (FType.Equals(TEXT("default_ops"), ESearchCase::IgnoreCase))
			{
				int32 ExistingCount = Ctrl->GetNumRetargetOps();
				if (ExistingCount > 0)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"default_ops\") -> stack already has %d ops, skipping"), ExistingCount));
					return sol::lua_nil;
				}
				FScopedTransaction Txn(FText::FromString(TEXT("Add Default Retarget Ops")));
				Ctrl->AddDefaultOps();

				// Assign IK rigs to all ops
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
				const UIKRigDefinition* SourceRig = Ctrl->GetIKRig(ERetargetSourceOrTarget::Source);
				if (SourceRig) Ctrl->AssignIKRigToAllOps(ERetargetSourceOrTarget::Source, SourceRig);
				const UIKRigDefinition* TargetRig = Ctrl->GetIKRig(ERetargetSourceOrTarget::Target);
				if (TargetRig) Ctrl->AssignIKRigToAllOps(ERetargetSourceOrTarget::Target, TargetRig);
#endif

				int32 NewCount = Ctrl->GetNumRetargetOps();
				Session.Log(FString::Printf(TEXT("[OK] add(\"default_ops\") -> %d ops added"), NewCount));
				return sol::make_object(Lv, NewCount);
			}
#endif // ENGINE_MINOR_VERSION >= 6 (retarget ops)

			// ---- add("pose", {name=.., for="target"}) ----
			if (FType.Equals(TEXT("pose"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value())
				{
					Session.Log(TEXT("[FAIL] add(\"pose\") -> params required: {name=.., for=\"target\"}"));
					return sol::lua_nil;
				}
				sol::table P = Params.value();
				std::string PoseName = P.get_or<std::string>("name", "");
				std::string ForStr = P.get_or<std::string>("for", "target");
				if (PoseName.empty())
				{
					Session.Log(TEXT("[FAIL] add(\"pose\") -> 'name' required"));
					return sol::lua_nil;
				}

				ERetargetSourceOrTarget Side;
				if (!ParseSourceOrTarget(ForStr, Side))
				{
					Session.Log(TEXT("[FAIL] add(\"pose\") -> 'for' must be \"source\" or \"target\""));
					return sol::lua_nil;
				}

				FScopedTransaction Txn(FText::FromString(TEXT("Create Retarget Pose")));
				FName Result = Ctrl->CreateRetargetPose(FName(UTF8_TO_TCHAR(PoseName.c_str())), Side);
				if (Result.IsNone())
				{
					Session.Log(TEXT("[FAIL] add(\"pose\") -> CreateRetargetPose failed"));
					return sol::lua_nil;
				}
				Session.Log(FString::Printf(TEXT("[OK] add(\"pose\", \"%s\", for=%s)"), *Result.ToString(), UTF8_TO_TCHAR(ForStr.c_str())));
				return sol::make_object(Lv, std::string(TCHAR_TO_UTF8(*Result.ToString())));
			}

			Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Valid: op, default_ops, pose"), *FType));
			return sol::lua_nil;
		});

		// ====================================================================
		// remove(type, id)
		// ====================================================================
		AssetObj.set_function("remove", [Ctrl, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::object> IdOpt, sol::this_state St) -> sol::object
		{
			sol::state_view Lv(St);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			// ---- remove("op", "OpName") — 5.6+ ----
			if (FType.Equals(TEXT("op"), ESearchCase::IgnoreCase))
			{
				if (!IdOpt.has_value() || !IdOpt.value().is<std::string>())
				{
					Session.Log(TEXT("[FAIL] remove(\"op\") -> op name (string) required"));
					return sol::lua_nil;
				}
				FString OpName = UTF8_TO_TCHAR(IdOpt.value().as<std::string>().c_str());
				int32 Idx = FindOpIndexByName(Ctrl, OpName);
				if (Idx < 0)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"op\", \"%s\") -> not found"), *OpName));
					return sol::lua_nil;
				}
				FScopedTransaction Txn(FText::FromString(TEXT("Remove Retarget Op")));
				bool bOk = Ctrl->RemoveRetargetOp(Idx);
				Session.Log(FString::Printf(TEXT("[%s] remove(\"op\", \"%s\")"), bOk ? TEXT("OK") : TEXT("FAIL"), *OpName));
				return bOk ? sol::make_object(Lv, true) : sol::lua_nil;
			}

			// ---- remove("all_ops") — 5.6+ ----
			if (FType.Equals(TEXT("all_ops"), ESearchCase::IgnoreCase))
			{
				FScopedTransaction Txn(FText::FromString(TEXT("Remove All Retarget Ops")));
				bool bOk = Ctrl->RemoveAllOps();
				Session.Log(FString::Printf(TEXT("[%s] remove(\"all_ops\")"), bOk ? TEXT("OK") : TEXT("FAIL")));
				return bOk ? sol::make_object(Lv, true) : sol::lua_nil;
			}
#endif

			// ---- remove("pose", {name=.., for=..}) ----
			if (FType.Equals(TEXT("pose"), ESearchCase::IgnoreCase))
			{
				if (!IdOpt.has_value() || !IdOpt.value().is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] remove(\"pose\") -> params table required: {name=.., for=\"target\"}"));
					return sol::lua_nil;
				}
				sol::table P = IdOpt.value().as<sol::table>();
				std::string PoseName = P.get_or<std::string>("name", "");
				std::string ForStr = P.get_or<std::string>("for", "target");
				if (PoseName.empty())
				{
					Session.Log(TEXT("[FAIL] remove(\"pose\") -> 'name' required"));
					return sol::lua_nil;
				}
				ERetargetSourceOrTarget Side;
				if (!ParseSourceOrTarget(ForStr, Side))
				{
					Session.Log(TEXT("[FAIL] remove(\"pose\") -> 'for' must be \"source\" or \"target\""));
					return sol::lua_nil;
				}
				FScopedTransaction Txn(FText::FromString(TEXT("Remove Retarget Pose")));
				bool bOk = Ctrl->RemoveRetargetPose(FName(UTF8_TO_TCHAR(PoseName.c_str())), Side);
				Session.Log(FString::Printf(TEXT("[%s] remove(\"pose\", \"%s\", for=%s)"),
					bOk ? TEXT("OK") : TEXT("FAIL"), UTF8_TO_TCHAR(PoseName.c_str()), UTF8_TO_TCHAR(ForStr.c_str())));
				return bOk ? sol::make_object(Lv, true) : sol::lua_nil;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type. Valid: op, all_ops, pose"), *FType));
			return sol::lua_nil;
		});

		// ====================================================================
		// list(type, opt_arg?)
		// ====================================================================
		AssetObj.set_function("list", [Ctrl, Retargeter, &Session](sol::table self,
			sol::optional<std::string> TypeOpt, sol::optional<std::string> ExtraArg, sol::this_state St) -> sol::object
		{
			sol::state_view Lv(St);
			FString FType = TypeOpt.has_value() ? UTF8_TO_TCHAR(TypeOpt.value().c_str()) : TEXT("all");

			// ---- list("all") / list() -> info() ----
			if (FType.Equals(TEXT("all"), ESearchCase::IgnoreCase))
			{
				sol::protected_function InfoFn = self["info"];
				if (InfoFn.valid()) return InfoFn(self);
				return sol::lua_nil;
			}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			// ---- list("ops") — 5.6+ retarget ops system ----
			if (FType.Equals(TEXT("ops"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("op"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lv.create_table();
				int32 Count = Ctrl->GetNumRetargetOps();
				for (int32 Idx = 0; Idx < Count; Idx++)
				{
					sol::table E = Lv.create_table();
					E["index"] = Idx;
					FName OpName = Ctrl->GetOpName(Idx);
					E["name"] = TCHAR_TO_UTF8(*OpName.ToString());
					E["enabled"] = Ctrl->GetRetargetOpEnabled(Idx);

					FIKRetargetOpBase* Op = Ctrl->GetRetargetOpByIndex(Idx);
					E["type"] = TCHAR_TO_UTF8(*GetOpTypeDisplayName(Op));

					int32 ParentIdx = Ctrl->GetParentOpIndex(Idx);
					if (ParentIdx != INDEX_NONE)
					{
						FName ParentName = Ctrl->GetOpName(ParentIdx);
						E["parent"] = TCHAR_TO_UTF8(*ParentName.ToString());
					}

					Result[Idx + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"ops\") -> %d ops"), Count));
				return Result;
			}
#endif

			// ---- list("chain_mappings") ----
			if (FType.Equals(TEXT("chain_mappings"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("chain_mapping"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lv.create_table();
				int32 PairCount = 0;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
				// Gather chain mappings from all ops (5.6+ retarget ops system)
				int32 NumOps = Ctrl->GetNumRetargetOps();
				for (int32 OpIdx = 0; OpIdx < NumOps; OpIdx++)
				{
					FName OpName = Ctrl->GetOpName(OpIdx);
					const FRetargetChainMapping* Mapping = Ctrl->GetChainMapping(OpName);
					if (!Mapping) continue;

					const TArray<FRetargetChainPair>& Pairs = Mapping->GetChainPairs();
					for (const FRetargetChainPair& Pair : Pairs)
					{
						sol::table E = Lv.create_table();
						E["target_chain"] = TCHAR_TO_UTF8(*Pair.TargetChainName.ToString());
						E["source_chain"] = TCHAR_TO_UTF8(*Pair.SourceChainName.ToString());
						E["op_name"] = TCHAR_TO_UTF8(*OpName.ToString());
						PairCount++;
						Result[PairCount] = E;
					}
				}
				// Fallback: try getting chain mapping without op name
				if (PairCount == 0)
				{
					const FRetargetChainMapping* Mapping = Ctrl->GetChainMapping(NAME_None);
					if (Mapping)
					{
						const TArray<FRetargetChainPair>& Pairs = Mapping->GetChainPairs();
						for (const FRetargetChainPair& Pair : Pairs)
						{
							sol::table E = Lv.create_table();
							E["target_chain"] = TCHAR_TO_UTF8(*Pair.TargetChainName.ToString());
							E["source_chain"] = TCHAR_TO_UTF8(*Pair.SourceChainName.ToString());
							PairCount++;
							Result[PairCount] = E;
						}
					}
				}
#endif // ENGINE_MINOR_VERSION >= 6
				Session.Log(FString::Printf(TEXT("[OK] list(\"chain_mappings\") -> %d pairs"), PairCount));
				return Result;
			}

			// ---- list("poses") ----
			if (FType.Equals(TEXT("poses"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("pose"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lv.create_table();

				sol::table SourcePoses = Lv.create_table();
				TMap<FName, FIKRetargetPose>& SrcMap = Ctrl->GetRetargetPoses(ERetargetSourceOrTarget::Source);
				int32 Si = 1;
				for (auto& KV : SrcMap)
				{
					SourcePoses[Si++] = TCHAR_TO_UTF8(*KV.Key.ToString());
				}
				Result["source"] = SourcePoses;
				Result["source_current"] = TCHAR_TO_UTF8(*Ctrl->GetCurrentRetargetPoseName(ERetargetSourceOrTarget::Source).ToString());

				sol::table TargetPoses = Lv.create_table();
				TMap<FName, FIKRetargetPose>& TgtMap = Ctrl->GetRetargetPoses(ERetargetSourceOrTarget::Target);
				int32 Ti = 1;
				for (auto& KV : TgtMap)
				{
					TargetPoses[Ti++] = TCHAR_TO_UTF8(*KV.Key.ToString());
				}
				Result["target"] = TargetPoses;
				Result["target_current"] = TCHAR_TO_UTF8(*Ctrl->GetCurrentRetargetPoseName(ERetargetSourceOrTarget::Target).ToString());

				Session.Log(FString::Printf(TEXT("[OK] list(\"poses\") -> %d source, %d target"), SrcMap.Num(), TgtMap.Num()));
				return Result;
			}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
			// ---- list("fk_chains", op_name?) ----
			if (FType.Equals(TEXT("fk_chains"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("fk_chain"), ESearchCase::IgnoreCase))
			{
				FName TargetOpName = NAME_None;
				if (ExtraArg.has_value() && !ExtraArg.value().empty())
				{
					TargetOpName = FName(UTF8_TO_TCHAR(ExtraArg.value().c_str()));
				}

				sol::table Result = Lv.create_table();
				int32 EntryIdx = 1;
				int32 NumOps = Ctrl->GetNumRetargetOps();
				for (int32 OpIdx = 0; OpIdx < NumOps; OpIdx++)
				{
					FIKRetargetOpBase* Op = Ctrl->GetRetargetOpByIndex(OpIdx);
					if (!Op || !Op->GetType()->IsChildOf(FIKRetargetFKChainsOp::StaticStruct())) continue;

					FName OpName = Ctrl->GetOpName(OpIdx);
					if (!TargetOpName.IsNone() && OpName != TargetOpName) continue;

					UIKRetargetOpControllerBase* BaseCtrl = Ctrl->GetOpController(OpIdx);
					UIKRetargetFKChainsController* FKCtrl = Cast<UIKRetargetFKChainsController>(BaseCtrl);
					if (!FKCtrl) continue;

					FIKRetargetFKChainsOpSettings Settings = FKCtrl->GetSettings();
					for (const FRetargetFKChainSettings& Chain : Settings.ChainsToRetarget)
					{
						sol::table E = Lv.create_table();
						E["op_name"] = TCHAR_TO_UTF8(*OpName.ToString());
						E["chain"] = TCHAR_TO_UTF8(*Chain.TargetChainName.ToString());
						E["enable_fk"] = Chain.EnableFK;
						E["rotation_mode"] = FKRotationModeToString(Chain.RotationMode);
						E["rotation_alpha"] = Chain.RotationAlpha;
						E["translation_mode"] = FKTranslationModeToString(Chain.TranslationMode);
						E["translation_alpha"] = Chain.TranslationAlpha;
						Result[EntryIdx++] = E;
					}
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"fk_chains\") -> %d chains"), EntryIdx - 1));
				return Result;
			}

			// ---- list("ik_chains", op_name?) ----
			if (FType.Equals(TEXT("ik_chains"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("ik_chain"), ESearchCase::IgnoreCase))
			{
				FName TargetOpName = NAME_None;
				if (ExtraArg.has_value() && !ExtraArg.value().empty())
				{
					TargetOpName = FName(UTF8_TO_TCHAR(ExtraArg.value().c_str()));
				}

				sol::table Result = Lv.create_table();
				int32 EntryIdx = 1;
				int32 NumOps = Ctrl->GetNumRetargetOps();
				for (int32 OpIdx = 0; OpIdx < NumOps; OpIdx++)
				{
					FIKRetargetOpBase* Op = Ctrl->GetRetargetOpByIndex(OpIdx);
					if (!Op || !Op->GetType()->IsChildOf(FIKRetargetIKChainsOp::StaticStruct())) continue;

					FName OpName = Ctrl->GetOpName(OpIdx);
					if (!TargetOpName.IsNone() && OpName != TargetOpName) continue;

					UIKRetargetOpControllerBase* BaseCtrl = Ctrl->GetOpController(OpIdx);
					UIKRetargetIKChainsController* IKCtrl = Cast<UIKRetargetIKChainsController>(BaseCtrl);
					if (!IKCtrl) continue;

					FIKRetargetIKChainsOpSettings Settings = IKCtrl->GetSettings();
					for (const FRetargetIKChainSettings& Chain : Settings.ChainsToRetarget)
					{
						sol::table E = Lv.create_table();
						E["op_name"] = TCHAR_TO_UTF8(*OpName.ToString());
						E["chain"] = TCHAR_TO_UTF8(*Chain.TargetChainName.ToString());
						E["enable_ik"] = Chain.EnableIK;
						E["blend_to_source"] = Chain.BlendToSource;
						E["blend_to_source_translation"] = Chain.BlendToSourceTranslation;
						E["blend_to_source_rotation"] = Chain.BlendToSourceRotation;
						E["extension"] = Chain.Extension;
						E["scale_vertical"] = Chain.ScaleVertical;
						E["apply_pelvis_offset_to_source_goals"] = Chain.ApplyPelvisOffsetToSourceGoals;

						sol::table Offset = Lv.create_table();
						Offset["x"] = Chain.StaticOffset.X;
						Offset["y"] = Chain.StaticOffset.Y;
						Offset["z"] = Chain.StaticOffset.Z;
						E["static_offset"] = Offset;

						sol::table LocalOffset = Lv.create_table();
						LocalOffset["x"] = Chain.StaticLocalOffset.X;
						LocalOffset["y"] = Chain.StaticLocalOffset.Y;
						LocalOffset["z"] = Chain.StaticLocalOffset.Z;
						E["static_local_offset"] = LocalOffset;

						sol::table RotOffset = Lv.create_table();
						RotOffset["p"] = Chain.StaticRotationOffset.Pitch;
						RotOffset["y"] = Chain.StaticRotationOffset.Yaw;
						RotOffset["r"] = Chain.StaticRotationOffset.Roll;
						E["static_rotation_offset"] = RotOffset;

						sol::table BlendWeights = Lv.create_table();
						BlendWeights["x"] = Chain.BlendToSourceWeights.X;
						BlendWeights["y"] = Chain.BlendToSourceWeights.Y;
						BlendWeights["z"] = Chain.BlendToSourceWeights.Z;
						E["blend_to_source_weights"] = BlendWeights;

						Result[EntryIdx++] = E;
					}
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"ik_chains\") -> %d chains"), EntryIdx - 1));
				return Result;
			}
#endif // ENGINE_MINOR_VERSION >= 7

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: ops, chain_mappings, poses, fk_chains, ik_chains"), *FType));
			return sol::lua_nil;
		});

		// ====================================================================
		// configure(type, params)
		// ====================================================================
		AssetObj.set_function("configure", [Ctrl, Retargeter, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Param, sol::this_state St) -> sol::object
		{
			sol::state_view Lv(St);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			// ---- configure("source_ikrig", path) ----
			if (FType.Equals(TEXT("source_ikrig"), ESearchCase::IgnoreCase))
			{
				if (!Param.is<std::string>())
				{
					Session.Log(TEXT("[FAIL] configure(\"source_ikrig\") -> path string required"));
					return sol::lua_nil;
				}
				FString RigPath = UTF8_TO_TCHAR(Param.as<std::string>().c_str());
				UIKRigDefinition* IKRig = LoadObject<UIKRigDefinition>(nullptr, *RigPath);
				if (!IKRig)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"source_ikrig\") -> '%s' not found"), *RigPath));
					return sol::lua_nil;
				}
				FScopedTransaction Txn(FText::FromString(TEXT("Set Source IK Rig")));
				Ctrl->SetIKRig(ERetargetSourceOrTarget::Source, IKRig);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
				Ctrl->AssignIKRigToAllOps(ERetargetSourceOrTarget::Source, IKRig);
#endif
				Session.Log(FString::Printf(TEXT("[OK] configure(\"source_ikrig\", \"%s\")"), *IKRig->GetName()));
				return sol::make_object(Lv, true);
			}

			// ---- configure("target_ikrig", path) ----
			if (FType.Equals(TEXT("target_ikrig"), ESearchCase::IgnoreCase))
			{
				if (!Param.is<std::string>())
				{
					Session.Log(TEXT("[FAIL] configure(\"target_ikrig\") -> path string required"));
					return sol::lua_nil;
				}
				FString RigPath = UTF8_TO_TCHAR(Param.as<std::string>().c_str());
				UIKRigDefinition* IKRig = LoadObject<UIKRigDefinition>(nullptr, *RigPath);
				if (!IKRig)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"target_ikrig\") -> '%s' not found"), *RigPath));
					return sol::lua_nil;
				}
				FScopedTransaction Txn(FText::FromString(TEXT("Set Target IK Rig")));
				Ctrl->SetIKRig(ERetargetSourceOrTarget::Target, IKRig);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
				Ctrl->AssignIKRigToAllOps(ERetargetSourceOrTarget::Target, IKRig);
#endif
				Session.Log(FString::Printf(TEXT("[OK] configure(\"target_ikrig\", \"%s\")"), *IKRig->GetName()));
				return sol::make_object(Lv, true);
			}

			// ---- configure("preview_mesh", {mesh=.., for=..}) ----
			if (FType.Equals(TEXT("preview_mesh"), ESearchCase::IgnoreCase))
			{
				if (!Param.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"preview_mesh\") -> table required: {mesh=.., for=\"source\"}"));
					return sol::lua_nil;
				}
				sol::table P = Param.as<sol::table>();
				std::string MeshPath = P.get_or<std::string>("mesh", "");
				std::string ForStr = P.get_or<std::string>("for", "target");
				if (MeshPath.empty())
				{
					Session.Log(TEXT("[FAIL] configure(\"preview_mesh\") -> 'mesh' path required"));
					return sol::lua_nil;
				}
				ERetargetSourceOrTarget Side;
				if (!ParseSourceOrTarget(ForStr, Side))
				{
					Session.Log(TEXT("[FAIL] configure(\"preview_mesh\") -> 'for' must be \"source\" or \"target\""));
					return sol::lua_nil;
				}
				FString FMeshPath = UTF8_TO_TCHAR(MeshPath.c_str());
				USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *FMeshPath);
				if (!Mesh)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"preview_mesh\") -> '%s' not found"), *FMeshPath));
					return sol::lua_nil;
				}
				FScopedTransaction Txn(FText::FromString(TEXT("Set Preview Mesh")));
				Ctrl->SetPreviewMesh(Side, Mesh);
				Session.Log(FString::Printf(TEXT("[OK] configure(\"preview_mesh\", \"%s\", for=%s)"),
					*Mesh->GetName(), UTF8_TO_TCHAR(ForStr.c_str())));
				return sol::make_object(Lv, true);
			}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			// ---- configure("op", {name=.., enabled=..}) — 5.6+ ----
			if (FType.Equals(TEXT("op"), ESearchCase::IgnoreCase))
			{
				if (!Param.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"op\") -> table required: {name=.., enabled=..}"));
					return sol::lua_nil;
				}
				sol::table P = Param.as<sol::table>();
				std::string OpNameStr = P.get_or<std::string>("name", "");
				if (OpNameStr.empty())
				{
					Session.Log(TEXT("[FAIL] configure(\"op\") -> 'name' required"));
					return sol::lua_nil;
				}
				FString OpName = UTF8_TO_TCHAR(OpNameStr.c_str());
				int32 Idx = FindOpIndexByName(Ctrl, OpName);
				if (Idx < 0)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"op\") -> op '%s' not found"), *OpName));
					return sol::lua_nil;
				}
				sol::optional<bool> Enabled = P.get<sol::optional<bool>>("enabled");
				if (Enabled.has_value())
				{
					Ctrl->SetRetargetOpEnabled(Idx, Enabled.value());
				}
				std::string NewName = P.get_or<std::string>("new_name", "");
				if (!NewName.empty())
				{
					Ctrl->SetOpName(FName(UTF8_TO_TCHAR(NewName.c_str())), Idx);
				}
				Session.Log(FString::Printf(TEXT("[OK] configure(\"op\", \"%s\")"), *OpName));
				return sol::make_object(Lv, true);
			}

#endif // ENGINE_MINOR_VERSION >= 6 (configure op)

			// ---- configure("chain_mapping", {target=.., source=.., op_name=..}) ----
			if (FType.Equals(TEXT("chain_mapping"), ESearchCase::IgnoreCase))
			{
				if (!Param.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"chain_mapping\") -> table required: {target=.., source=..}"));
					return sol::lua_nil;
				}
				sol::table P = Param.as<sol::table>();
				std::string TargetChain = P.get_or<std::string>("target", "");
				std::string SourceChain = P.get_or<std::string>("source", "");
				std::string OpNameStr = P.get_or<std::string>("op_name", "");
				if (TargetChain.empty())
				{
					Session.Log(TEXT("[FAIL] configure(\"chain_mapping\") -> 'target' chain name required"));
					return sol::lua_nil;
				}
				FName OpName = OpNameStr.empty() ? NAME_None : FName(UTF8_TO_TCHAR(OpNameStr.c_str()));
				FScopedTransaction Txn(FText::FromString(TEXT("Set Chain Mapping")));
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
				bool bOk = Ctrl->SetSourceChain(
					FName(UTF8_TO_TCHAR(SourceChain.c_str())),
					FName(UTF8_TO_TCHAR(TargetChain.c_str())),
					OpName);
#else
				bool bOk = Ctrl->SetSourceChain(
					FName(UTF8_TO_TCHAR(SourceChain.c_str())),
					FName(UTF8_TO_TCHAR(TargetChain.c_str())));
#endif
				Session.Log(FString::Printf(TEXT("[%s] configure(\"chain_mapping\", target=\"%s\", source=\"%s\")"),
					bOk ? TEXT("OK") : TEXT("FAIL"),
					UTF8_TO_TCHAR(TargetChain.c_str()), UTF8_TO_TCHAR(SourceChain.c_str())));
				return bOk ? sol::make_object(Lv, true) : sol::lua_nil;
			}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
			// ---- configure("fk_chain", {chain=.., op_name=.., ...}) ----
			if (FType.Equals(TEXT("fk_chain"), ESearchCase::IgnoreCase))
			{
				if (!Param.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"fk_chain\") -> table required"));
					return sol::lua_nil;
				}
				sol::table P = Param.as<sol::table>();
				std::string ChainName = P.get_or<std::string>("chain", "");
				if (ChainName.empty())
				{
					Session.Log(TEXT("[FAIL] configure(\"fk_chain\") -> 'chain' name required"));
					return sol::lua_nil;
				}
				FName FChainName = FName(UTF8_TO_TCHAR(ChainName.c_str()));

				// Find the FK op (by op_name or first FK op)
				std::string OpNameStr = P.get_or<std::string>("op_name", "");
				int32 NumOps = Ctrl->GetNumRetargetOps();
				int32 FoundOpIdx = -1;
				for (int32 OpIdx = 0; OpIdx < NumOps; OpIdx++)
				{
					FIKRetargetOpBase* Op = Ctrl->GetRetargetOpByIndex(OpIdx);
					if (!Op || !Op->GetType()->IsChildOf(FIKRetargetFKChainsOp::StaticStruct())) continue;
					if (!OpNameStr.empty())
					{
						FName OpNm = Ctrl->GetOpName(OpIdx);
						if (!OpNm.ToString().Equals(UTF8_TO_TCHAR(OpNameStr.c_str()), ESearchCase::IgnoreCase)) continue;
					}
					FoundOpIdx = OpIdx;
					break;
				}
				if (FoundOpIdx < 0)
				{
					Session.Log(TEXT("[FAIL] configure(\"fk_chain\") -> no FK Chains op found"));
					return sol::lua_nil;
				}

				UIKRetargetOpControllerBase* BaseCtrl = Ctrl->GetOpController(FoundOpIdx);
				UIKRetargetFKChainsController* FKCtrl = Cast<UIKRetargetFKChainsController>(BaseCtrl);
				if (!FKCtrl)
				{
					Session.Log(TEXT("[FAIL] configure(\"fk_chain\") -> failed to get FK controller"));
					return sol::lua_nil;
				}

				FIKRetargetFKChainsOpSettings Settings = FKCtrl->GetSettings();
				bool bFound = false;
				for (FRetargetFKChainSettings& Chain : Settings.ChainsToRetarget)
				{
					if (Chain.TargetChainName != FChainName) continue;
					bFound = true;

					sol::optional<bool> EnableFK = P.get<sol::optional<bool>>("enable_fk");
					if (EnableFK.has_value()) Chain.EnableFK = EnableFK.value();

					std::string RotMode = P.get_or<std::string>("rotation_mode", "");
					if (!RotMode.empty())
					{
						EFKChainRotationMode Mode;
						if (ParseFKRotationMode(RotMode, Mode)) Chain.RotationMode = Mode;
					}

					sol::optional<double> RotAlpha = P.get<sol::optional<double>>("rotation_alpha");
					if (RotAlpha.has_value()) Chain.RotationAlpha = RotAlpha.value();

					std::string TransMode = P.get_or<std::string>("translation_mode", "");
					if (!TransMode.empty())
					{
						EFKChainTranslationMode Mode;
						if (ParseFKTranslationMode(TransMode, Mode)) Chain.TranslationMode = Mode;
					}

					sol::optional<double> TransAlpha = P.get<sol::optional<double>>("translation_alpha");
					if (TransAlpha.has_value()) Chain.TranslationAlpha = TransAlpha.value();

					break;
				}
				if (!bFound)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"fk_chain\") -> chain '%s' not found in FK op"), UTF8_TO_TCHAR(ChainName.c_str())));
					return sol::lua_nil;
				}

				FScopedTransaction Txn(FText::FromString(TEXT("Configure FK Chain")));
				FKCtrl->SetSettings(Settings);
				Session.Log(FString::Printf(TEXT("[OK] configure(\"fk_chain\", \"%s\")"), UTF8_TO_TCHAR(ChainName.c_str())));
				return sol::make_object(Lv, true);
			}

			// ---- configure("ik_chain", {chain=.., op_name=.., ...}) ----
			if (FType.Equals(TEXT("ik_chain"), ESearchCase::IgnoreCase))
			{
				if (!Param.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"ik_chain\") -> table required"));
					return sol::lua_nil;
				}
				sol::table P = Param.as<sol::table>();
				std::string ChainName = P.get_or<std::string>("chain", "");
				if (ChainName.empty())
				{
					Session.Log(TEXT("[FAIL] configure(\"ik_chain\") -> 'chain' name required"));
					return sol::lua_nil;
				}
				FName FChainName = FName(UTF8_TO_TCHAR(ChainName.c_str()));

				// Find the IK op
				std::string OpNameStr = P.get_or<std::string>("op_name", "");
				int32 NumOps = Ctrl->GetNumRetargetOps();
				int32 FoundOpIdx = -1;
				for (int32 OpIdx = 0; OpIdx < NumOps; OpIdx++)
				{
					FIKRetargetOpBase* Op = Ctrl->GetRetargetOpByIndex(OpIdx);
					if (!Op || !Op->GetType()->IsChildOf(FIKRetargetIKChainsOp::StaticStruct())) continue;
					if (!OpNameStr.empty())
					{
						FName OpNm = Ctrl->GetOpName(OpIdx);
						if (!OpNm.ToString().Equals(UTF8_TO_TCHAR(OpNameStr.c_str()), ESearchCase::IgnoreCase)) continue;
					}
					FoundOpIdx = OpIdx;
					break;
				}
				if (FoundOpIdx < 0)
				{
					Session.Log(TEXT("[FAIL] configure(\"ik_chain\") -> no IK Chains op found"));
					return sol::lua_nil;
				}

				UIKRetargetOpControllerBase* BaseCtrl = Ctrl->GetOpController(FoundOpIdx);
				UIKRetargetIKChainsController* IKOpCtrl = Cast<UIKRetargetIKChainsController>(BaseCtrl);
				if (!IKOpCtrl)
				{
					Session.Log(TEXT("[FAIL] configure(\"ik_chain\") -> failed to get IK controller"));
					return sol::lua_nil;
				}

				FIKRetargetIKChainsOpSettings Settings = IKOpCtrl->GetSettings();
				bool bFound = false;
				for (FRetargetIKChainSettings& Chain : Settings.ChainsToRetarget)
				{
					if (Chain.TargetChainName != FChainName) continue;
					bFound = true;

					sol::optional<bool> EnableIK = P.get<sol::optional<bool>>("enable_ik");
					if (EnableIK.has_value()) Chain.EnableIK = EnableIK.value();

					sol::optional<double> BlendSrc = P.get<sol::optional<double>>("blend_to_source");
					if (BlendSrc.has_value()) Chain.BlendToSource = BlendSrc.value();

					sol::optional<double> BlendSrcTrans = P.get<sol::optional<double>>("blend_to_source_translation");
					if (BlendSrcTrans.has_value()) Chain.BlendToSourceTranslation = BlendSrcTrans.value();

					sol::optional<double> BlendSrcRot = P.get<sol::optional<double>>("blend_to_source_rotation");
					if (BlendSrcRot.has_value()) Chain.BlendToSourceRotation = BlendSrcRot.value();

					sol::optional<double> Ext = P.get<sol::optional<double>>("extension");
					if (Ext.has_value()) Chain.Extension = Ext.value();

					sol::optional<double> ScaleV = P.get<sol::optional<double>>("scale_vertical");
					if (ScaleV.has_value()) Chain.ScaleVertical = ScaleV.value();

					sol::optional<sol::table> OffsetTbl = P.get<sol::optional<sol::table>>("static_offset");
					if (OffsetTbl.has_value())
					{
						sol::table OT = OffsetTbl.value();
						Chain.StaticOffset.X = OT.get<sol::optional<double>>("x").value_or(Chain.StaticOffset.X);
						Chain.StaticOffset.Y = OT.get<sol::optional<double>>("y").value_or(Chain.StaticOffset.Y);
						Chain.StaticOffset.Z = OT.get<sol::optional<double>>("z").value_or(Chain.StaticOffset.Z);
					}

					sol::optional<sol::table> LocalOffsetTbl = P.get<sol::optional<sol::table>>("static_local_offset");
					if (LocalOffsetTbl.has_value())
					{
						sol::table OT = LocalOffsetTbl.value();
						Chain.StaticLocalOffset.X = OT.get<sol::optional<double>>("x").value_or(Chain.StaticLocalOffset.X);
						Chain.StaticLocalOffset.Y = OT.get<sol::optional<double>>("y").value_or(Chain.StaticLocalOffset.Y);
						Chain.StaticLocalOffset.Z = OT.get<sol::optional<double>>("z").value_or(Chain.StaticLocalOffset.Z);
					}

					sol::optional<sol::table> RotOffsetTbl = P.get<sol::optional<sol::table>>("static_rotation_offset");
					if (RotOffsetTbl.has_value())
					{
						sol::table RT = RotOffsetTbl.value();
						Chain.StaticRotationOffset.Pitch = RT.get<sol::optional<double>>("p").value_or(Chain.StaticRotationOffset.Pitch);
						Chain.StaticRotationOffset.Yaw = RT.get<sol::optional<double>>("y").value_or(Chain.StaticRotationOffset.Yaw);
						Chain.StaticRotationOffset.Roll = RT.get<sol::optional<double>>("r").value_or(Chain.StaticRotationOffset.Roll);
					}

					sol::optional<sol::table> BlendWeightsTbl = P.get<sol::optional<sol::table>>("blend_to_source_weights");
					if (BlendWeightsTbl.has_value())
					{
						sol::table BW = BlendWeightsTbl.value();
						Chain.BlendToSourceWeights.X = BW.get<sol::optional<double>>("x").value_or(Chain.BlendToSourceWeights.X);
						Chain.BlendToSourceWeights.Y = BW.get<sol::optional<double>>("y").value_or(Chain.BlendToSourceWeights.Y);
						Chain.BlendToSourceWeights.Z = BW.get<sol::optional<double>>("z").value_or(Chain.BlendToSourceWeights.Z);
					}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
					sol::optional<bool> ApplyPelvis = P.get<sol::optional<bool>>("apply_pelvis_offset_to_source_goals");
					if (ApplyPelvis.has_value()) Chain.ApplyPelvisOffsetToSourceGoals = ApplyPelvis.value();
#endif

					break;
				}
				if (!bFound)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"ik_chain\") -> chain '%s' not found in IK op"), UTF8_TO_TCHAR(ChainName.c_str())));
					return sol::lua_nil;
				}

				FScopedTransaction Txn(FText::FromString(TEXT("Configure IK Chain")));
				IKOpCtrl->SetSettings(Settings);
				Session.Log(FString::Printf(TEXT("[OK] configure(\"ik_chain\", \"%s\")"), UTF8_TO_TCHAR(ChainName.c_str())));
				return sol::make_object(Lv, true);
			}

			// ---- configure("pelvis", {op_name=.., ...}) ----
			if (FType.Equals(TEXT("pelvis"), ESearchCase::IgnoreCase))
			{
				if (!Param.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"pelvis\") -> table required"));
					return sol::lua_nil;
				}
				sol::table P = Param.as<sol::table>();

				// Find pelvis op
				std::string OpNameStr = P.get_or<std::string>("op_name", "");
				int32 NumOps = Ctrl->GetNumRetargetOps();
				int32 FoundOpIdx = -1;
				for (int32 OpIdx = 0; OpIdx < NumOps; OpIdx++)
				{
					FIKRetargetOpBase* Op = Ctrl->GetRetargetOpByIndex(OpIdx);
					if (!Op || !Op->GetType()->IsChildOf(FIKRetargetPelvisMotionOp::StaticStruct())) continue;
					if (!OpNameStr.empty())
					{
						FName OpNm = Ctrl->GetOpName(OpIdx);
						if (!OpNm.ToString().Equals(UTF8_TO_TCHAR(OpNameStr.c_str()), ESearchCase::IgnoreCase)) continue;
					}
					FoundOpIdx = OpIdx;
					break;
				}
				if (FoundOpIdx < 0)
				{
					Session.Log(TEXT("[FAIL] configure(\"pelvis\") -> no Pelvis Motion op found"));
					return sol::lua_nil;
				}

				UIKRetargetOpControllerBase* BaseCtrl = Ctrl->GetOpController(FoundOpIdx);
				UIKRetargetPelvisMotionController* PelvisCtrl = Cast<UIKRetargetPelvisMotionController>(BaseCtrl);
				if (!PelvisCtrl)
				{
					Session.Log(TEXT("[FAIL] configure(\"pelvis\") -> failed to get Pelvis controller"));
					return sol::lua_nil;
				}

				FScopedTransaction Txn(FText::FromString(TEXT("Configure Pelvis Motion")));

				// Bone references — MUST use dedicated setters, not direct struct modification
				std::string SrcPelvis = P.get_or<std::string>("source_pelvis_bone", "");
				if (!SrcPelvis.empty()) PelvisCtrl->SetSourcePelvisBone(FName(UTF8_TO_TCHAR(SrcPelvis.c_str())));

				std::string TgtPelvis = P.get_or<std::string>("target_pelvis_bone", "");
				if (!TgtPelvis.empty()) PelvisCtrl->SetTargetPelvisBone(FName(UTF8_TO_TCHAR(TgtPelvis.c_str())));

				// Numeric settings via GetSettings/SetSettings
				FIKRetargetPelvisMotionOpSettings Settings = PelvisCtrl->GetSettings();

				sol::optional<double> RotAlpha = P.get<sol::optional<double>>("rotation_alpha");
				if (RotAlpha.has_value()) Settings.RotationAlpha = RotAlpha.value();

				sol::optional<double> TransAlpha = P.get<sol::optional<double>>("translation_alpha");
				if (TransAlpha.has_value()) Settings.TranslationAlpha = TransAlpha.value();

				sol::optional<double> ScaleH = P.get<sol::optional<double>>("scale_horizontal");
				if (ScaleH.has_value()) Settings.ScaleHorizontal = ScaleH.value();

				sol::optional<double> ScaleV = P.get<sol::optional<double>>("scale_vertical");
				if (ScaleV.has_value()) Settings.ScaleVertical = ScaleV.value();

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
				sol::optional<double> FloorW = P.get<sol::optional<double>>("floor_constraint_weight");
				if (FloorW.has_value()) Settings.FloorConstraintWeight = FloorW.value();

				sol::optional<double> SrcCrotch = P.get<sol::optional<double>>("source_crotch_offset");
				if (SrcCrotch.has_value()) Settings.SourceCrotchOffset = SrcCrotch.value();

				sol::optional<double> TgtCrotch = P.get<sol::optional<double>>("target_crotch_offset");
				if (TgtCrotch.has_value()) Settings.TargetCrotchOffset = TgtCrotch.value();
#endif

				sol::optional<double> BlendSrcTrans = P.get<sol::optional<double>>("blend_to_source_translation");
				if (BlendSrcTrans.has_value()) Settings.BlendToSourceTranslation = BlendSrcTrans.value();

				sol::optional<sol::table> BlendSrcTransWeights = P.get<sol::optional<sol::table>>("blend_to_source_translation_weights");
				if (BlendSrcTransWeights.has_value())
				{
					sol::table BW = BlendSrcTransWeights.value();
					Settings.BlendToSourceTranslationWeights.X = BW.get<sol::optional<double>>("x").value_or(Settings.BlendToSourceTranslationWeights.X);
					Settings.BlendToSourceTranslationWeights.Y = BW.get<sol::optional<double>>("y").value_or(Settings.BlendToSourceTranslationWeights.Y);
					Settings.BlendToSourceTranslationWeights.Z = BW.get<sol::optional<double>>("z").value_or(Settings.BlendToSourceTranslationWeights.Z);
				}

				sol::optional<double> AffectIKH = P.get<sol::optional<double>>("affect_ik_horizontal");
				if (AffectIKH.has_value()) Settings.AffectIKHorizontal = AffectIKH.value();

				sol::optional<double> AffectIKV = P.get<sol::optional<double>>("affect_ik_vertical");
				if (AffectIKV.has_value()) Settings.AffectIKVertical = AffectIKV.value();

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
				sol::optional<sol::table> RotOffLocal = P.get<sol::optional<sol::table>>("rotation_offset_local");
				if (RotOffLocal.has_value())
				{
					sol::table RT = RotOffLocal.value();
					Settings.RotationOffsetLocal.Pitch = RT.get<sol::optional<double>>("p").value_or(Settings.RotationOffsetLocal.Pitch);
					Settings.RotationOffsetLocal.Yaw = RT.get<sol::optional<double>>("y").value_or(Settings.RotationOffsetLocal.Yaw);
					Settings.RotationOffsetLocal.Roll = RT.get<sol::optional<double>>("r").value_or(Settings.RotationOffsetLocal.Roll);
				}

				sol::optional<sol::table> RotOffGlobal = P.get<sol::optional<sol::table>>("rotation_offset_global");
				if (RotOffGlobal.has_value())
				{
					sol::table RT = RotOffGlobal.value();
					Settings.RotationOffsetGlobal.Pitch = RT.get<sol::optional<double>>("p").value_or(Settings.RotationOffsetGlobal.Pitch);
					Settings.RotationOffsetGlobal.Yaw = RT.get<sol::optional<double>>("y").value_or(Settings.RotationOffsetGlobal.Yaw);
					Settings.RotationOffsetGlobal.Roll = RT.get<sol::optional<double>>("r").value_or(Settings.RotationOffsetGlobal.Roll);
				}

				sol::optional<sol::table> TransOffLocal = P.get<sol::optional<sol::table>>("translation_offset_local");
				if (TransOffLocal.has_value())
				{
					sol::table VT = TransOffLocal.value();
					Settings.TranslationOffsetLocal.X = VT.get<sol::optional<double>>("x").value_or(Settings.TranslationOffsetLocal.X);
					Settings.TranslationOffsetLocal.Y = VT.get<sol::optional<double>>("y").value_or(Settings.TranslationOffsetLocal.Y);
					Settings.TranslationOffsetLocal.Z = VT.get<sol::optional<double>>("z").value_or(Settings.TranslationOffsetLocal.Z);
				}

				sol::optional<sol::table> TransOffGlobal = P.get<sol::optional<sol::table>>("translation_offset_global");
				if (TransOffGlobal.has_value())
				{
					sol::table VT = TransOffGlobal.value();
					Settings.TranslationOffsetGlobal.X = VT.get<sol::optional<double>>("x").value_or(Settings.TranslationOffsetGlobal.X);
					Settings.TranslationOffsetGlobal.Y = VT.get<sol::optional<double>>("y").value_or(Settings.TranslationOffsetGlobal.Y);
					Settings.TranslationOffsetGlobal.Z = VT.get<sol::optional<double>>("z").value_or(Settings.TranslationOffsetGlobal.Z);
				}
#endif

				PelvisCtrl->SetSettings(Settings);
				Session.Log(TEXT("[OK] configure(\"pelvis\")"));
				return sol::make_object(Lv, true);
			}

			// ---- configure("root_motion", {op_name=.., ...}) ----
			if (FType.Equals(TEXT("root_motion"), ESearchCase::IgnoreCase))
			{
				if (!Param.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"root_motion\") -> table required"));
					return sol::lua_nil;
				}
				sol::table P = Param.as<sol::table>();

				// Find root motion op
				std::string OpNameStr = P.get_or<std::string>("op_name", "");
				int32 NumOps = Ctrl->GetNumRetargetOps();
				int32 FoundOpIdx = -1;
				for (int32 OpIdx = 0; OpIdx < NumOps; OpIdx++)
				{
					FIKRetargetOpBase* Op = Ctrl->GetRetargetOpByIndex(OpIdx);
					if (!Op || !Op->GetType()->IsChildOf(FIKRetargetRootMotionOp::StaticStruct())) continue;
					if (!OpNameStr.empty())
					{
						FName OpNm = Ctrl->GetOpName(OpIdx);
						if (!OpNm.ToString().Equals(UTF8_TO_TCHAR(OpNameStr.c_str()), ESearchCase::IgnoreCase)) continue;
					}
					FoundOpIdx = OpIdx;
					break;
				}
				if (FoundOpIdx < 0)
				{
					Session.Log(TEXT("[FAIL] configure(\"root_motion\") -> no Root Motion op found"));
					return sol::lua_nil;
				}

				UIKRetargetOpControllerBase* BaseCtrl = Ctrl->GetOpController(FoundOpIdx);
				UIKRetargetRootMotionController* RootCtrl = Cast<UIKRetargetRootMotionController>(BaseCtrl);
				if (!RootCtrl)
				{
					Session.Log(TEXT("[FAIL] configure(\"root_motion\") -> failed to get Root Motion controller"));
					return sol::lua_nil;
				}

				FScopedTransaction Txn(FText::FromString(TEXT("Configure Root Motion")));

				// Bone references — MUST use dedicated setters
				std::string SrcRoot = P.get_or<std::string>("source_root_bone", "");
				if (!SrcRoot.empty()) RootCtrl->SetSourceRootBone(FName(UTF8_TO_TCHAR(SrcRoot.c_str())));

				std::string TgtRoot = P.get_or<std::string>("target_root_bone", "");
				if (!TgtRoot.empty()) RootCtrl->SetTargetRootBone(FName(UTF8_TO_TCHAR(TgtRoot.c_str())));

				std::string TgtPelvis = P.get_or<std::string>("target_pelvis_bone", "");
				if (!TgtPelvis.empty()) RootCtrl->SetTargetPelvisBone(FName(UTF8_TO_TCHAR(TgtPelvis.c_str())));

				// Enum and numeric settings via GetSettings/SetSettings
				FIKRetargetRootMotionOpSettings Settings = RootCtrl->GetSettings();

				std::string RootSrc = P.get_or<std::string>("root_motion_source", "");
				if (!RootSrc.empty())
				{
					FString FSrc = UTF8_TO_TCHAR(RootSrc.c_str());
					if (FSrc.Equals(TEXT("CopyFromSourceRoot"), ESearchCase::IgnoreCase))
						Settings.RootMotionSource = ERootMotionSource::CopyFromSourceRoot;
					else if (FSrc.Equals(TEXT("GenerateFromTargetPelvis"), ESearchCase::IgnoreCase))
						Settings.RootMotionSource = ERootMotionSource::GenerateFromTargetPelvis;
				}

				std::string HeightSrc = P.get_or<std::string>("root_height_source", "");
				if (!HeightSrc.empty())
				{
					FString FH = UTF8_TO_TCHAR(HeightSrc.c_str());
					if (FH.Equals(TEXT("CopyHeightFromSource"), ESearchCase::IgnoreCase))
						Settings.RootHeightSource = ERootMotionHeightSource::CopyHeightFromSource;
					else if (FH.Equals(TEXT("SnapToGround"), ESearchCase::IgnoreCase))
						Settings.RootHeightSource = ERootMotionHeightSource::SnapToGround;
				}

				sol::optional<bool> RotWithPelvis = P.get<sol::optional<bool>>("rotate_with_pelvis");
				if (RotWithPelvis.has_value()) Settings.bRotateWithPelvis = RotWithPelvis.value();

				sol::optional<bool> MaintainOffset = P.get<sol::optional<bool>>("maintain_offset_from_pelvis");
				if (MaintainOffset.has_value()) Settings.bMaintainOffsetFromPelvis = MaintainOffset.value();

				sol::optional<bool> Propagate = P.get<sol::optional<bool>>("propagate_to_non_retargeted_children");
				if (Propagate.has_value()) Settings.bPropagateToNonRetargetedChildren = Propagate.value();

				sol::optional<sol::table> GlobalOff = P.get<sol::optional<sol::table>>("global_offset");
				if (GlobalOff.has_value())
				{
					sol::table GO = GlobalOff.value();
					FVector Loc = Settings.GlobalOffset.GetLocation();
					Loc.X = GO.get<sol::optional<double>>("x").value_or(Loc.X);
					Loc.Y = GO.get<sol::optional<double>>("y").value_or(Loc.Y);
					Loc.Z = GO.get<sol::optional<double>>("z").value_or(Loc.Z);
					Settings.GlobalOffset.SetLocation(Loc);
				}

				RootCtrl->SetSettings(Settings);
				Session.Log(TEXT("[OK] configure(\"root_motion\")"));
				return sol::make_object(Lv, true);
			}
#endif // ENGINE_MINOR_VERSION >= 7

			Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> unknown type. Valid: source_ikrig, target_ikrig, preview_mesh, op, chain_mapping, fk_chain, ik_chain, pelvis, root_motion"), *FType));
			return sol::lua_nil;
		});

		// ====================================================================
		// auto_map(mode, force_remap?, op_name?)
		// ====================================================================
		AssetObj.set_function("auto_map", [Ctrl, &Session](sol::table /*self*/,
			const std::string& Mode, sol::optional<bool> ForceRemap, sol::optional<std::string> OpName, sol::this_state St) -> sol::object
		{
			sol::state_view Lv(St);
			EAutoMapChainType MapType;
			if (!ParseAutoMapType(Mode, MapType))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] auto_map(\"%s\") -> unknown mode. Valid: exact, fuzzy, clear"), UTF8_TO_TCHAR(Mode.c_str())));
				return sol::lua_nil;
			}
			bool bForce = ForceRemap.value_or(true);
			FName FOpName = NAME_None;
			if (OpName.has_value() && !OpName.value().empty())
			{
				FOpName = FName(UTF8_TO_TCHAR(OpName.value().c_str()));
			}

			FScopedTransaction Txn(FText::FromString(TEXT("Auto Map Chains")));
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			Ctrl->AutoMapChains(MapType, bForce, FOpName);
#else
			Ctrl->AutoMapChains(MapType, bForce);
#endif
			Session.Log(FString::Printf(TEXT("[OK] auto_map(\"%s\", force=%s)"), UTF8_TO_TCHAR(Mode.c_str()), bForce ? TEXT("true") : TEXT("false")));
			return sol::make_object(Lv, true);
		});

		// ====================================================================
		// auto_align({for=.., bones=..?})
		// ====================================================================
		AssetObj.set_function("auto_align", [Ctrl, &Session](sol::table /*self*/,
			sol::optional<sol::table> Params, sol::this_state St) -> sol::object
		{
			sol::state_view Lv(St);
			std::string ForStr = "target";
			std::string MethodStr = "chain_to_chain";
			if (Params.has_value())
			{
				ForStr = Params.value().get_or<std::string>("for", "target");
				MethodStr = Params.value().get_or<std::string>("method", "chain_to_chain");
			}

			ERetargetSourceOrTarget Side;
			if (!ParseSourceOrTarget(ForStr, Side))
			{
				Session.Log(TEXT("[FAIL] auto_align() -> 'for' must be \"source\" or \"target\""));
				return sol::lua_nil;
			}

			// Parse alignment method
			ERetargetAutoAlignMethod Method = ERetargetAutoAlignMethod::ChainToChain;
			FString FMethod = UTF8_TO_TCHAR(MethodStr.c_str());
			if (FMethod.Equals(TEXT("mesh_to_mesh"), ESearchCase::IgnoreCase) || FMethod.Equals(TEXT("MeshToMesh"), ESearchCase::IgnoreCase))
				Method = ERetargetAutoAlignMethod::MeshToMesh;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			else if (FMethod.Equals(TEXT("local_rotation"), ESearchCase::IgnoreCase) || FMethod.Equals(TEXT("LocalRotationAxes"), ESearchCase::IgnoreCase))
				Method = ERetargetAutoAlignMethod::LocalRotationAxes;
			else if (FMethod.Equals(TEXT("global_rotation"), ESearchCase::IgnoreCase) || FMethod.Equals(TEXT("GlobalRotationAxes"), ESearchCase::IgnoreCase))
				Method = ERetargetAutoAlignMethod::GlobalRotationAxes;
#endif

			FScopedTransaction Txn(FText::FromString(TEXT("Auto Align Bones")));

			// Check if specific bones are requested
			if (Params.has_value())
			{
				sol::optional<sol::table> BonesTbl = Params.value().get<sol::optional<sol::table>>("bones");
				if (BonesTbl.has_value())
				{
					TArray<FName> BoneNames;
					sol::table BT = BonesTbl.value();
					for (auto& KV : BT)
					{
						if (KV.second.is<std::string>())
						{
							BoneNames.Add(FName(UTF8_TO_TCHAR(KV.second.as<std::string>().c_str())));
						}
					}
					Ctrl->AutoAlignBones(BoneNames, Method, Side);
					Session.Log(FString::Printf(TEXT("[OK] auto_align(%d bones, for=%s, method=%s)"), BoneNames.Num(), UTF8_TO_TCHAR(ForStr.c_str()), UTF8_TO_TCHAR(MethodStr.c_str())));
					return sol::make_object(Lv, true);
				}
			}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			Ctrl->AutoAlignAllBones(Side, Method);
#else
			Ctrl->AutoAlignAllBones(Side);
#endif
			Session.Log(FString::Printf(TEXT("[OK] auto_align(all, for=%s, method=%s)"), UTF8_TO_TCHAR(ForStr.c_str()), UTF8_TO_TCHAR(MethodStr.c_str())));
			return sol::make_object(Lv, true);
		});

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
		// ====================================================================
		// move_op(name, to_index) — 5.6+
		// ====================================================================
		AssetObj.set_function("move_op", [Ctrl, &Session](sol::table /*self*/,
			const std::string& OpName, int TargetIndex, sol::this_state St) -> sol::object
		{
			sol::state_view Lv(St);
			FString FOpName = UTF8_TO_TCHAR(OpName.c_str());
			int32 Idx = FindOpIndexByName(Ctrl, FOpName);
			if (Idx < 0)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] move_op(\"%s\") -> not found"), *FOpName));
				return sol::lua_nil;
			}
			FScopedTransaction Txn(FText::FromString(TEXT("Move Retarget Op")));
			bool bOk = Ctrl->MoveRetargetOpInStack(Idx, TargetIndex);
			Session.Log(FString::Printf(TEXT("[%s] move_op(\"%s\", %d)"), bOk ? TEXT("OK") : TEXT("FAIL"), *FOpName, TargetIndex));
			return bOk ? sol::make_object(Lv, true) : sol::lua_nil;
		});

		// ====================================================================
		// set_parent_op("ChildOp", "ParentOp") — 5.6+
		// ====================================================================
		AssetObj.set_function("set_parent_op", [Ctrl, &Session](sol::table /*self*/,
			const std::string& ChildOp, const std::string& ParentOp, sol::this_state St) -> sol::object
		{
			sol::state_view Lv(St);
			FString ChildName = UTF8_TO_TCHAR(ChildOp.c_str());
			FString ParentName = UTF8_TO_TCHAR(ParentOp.c_str());
			FScopedTransaction Txn(FText::FromString(TEXT("Set Parent Op")));
			bool bSuccess = Ctrl->SetParentOpByName(FName(ChildName), FName(ParentName));
			if (bSuccess)
				Session.Log(FString::Printf(TEXT("[OK] set_parent_op(\"%s\" -> parent \"%s\")"), *ChildName, *ParentName));
			else
				Session.Log(FString::Printf(TEXT("[FAIL] set_parent_op -> op not found or invalid")));
			return sol::make_object(Lv, bSuccess);
		});

		// ====================================================================
		// get_parent_op("OpName") — 5.6+
		// ====================================================================
		AssetObj.set_function("get_parent_op", [Ctrl, &Session](sol::table /*self*/,
			const std::string& OpName, sol::this_state St) -> sol::object
		{
			sol::state_view Lv(St);
			FString FOpName = UTF8_TO_TCHAR(OpName.c_str());
			FName ParentName = Ctrl->GetParentOpByName(FName(FOpName));
			if (ParentName != NAME_None)
				return sol::make_object(Lv, std::string(TCHAR_TO_UTF8(*ParentName.ToString())));
			return sol::lua_nil;
		});
#endif // ENGINE_MINOR_VERSION >= 6 (ops management)

		// ====================================================================
		// duplicate_pose({name=.., new_name=.., for=..})
		// ====================================================================
		AssetObj.set_function("duplicate_pose", [Ctrl, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state St) -> sol::object
		{
			sol::state_view Lv(St);
			std::string SrcName = Params.get_or<std::string>("name", "");
			std::string NewName = Params.get_or<std::string>("new_name", "");
			std::string ForStr = Params.get_or<std::string>("for", "target");
			if (SrcName.empty() || NewName.empty())
			{
				Session.Log(TEXT("[FAIL] duplicate_pose() -> 'name' and 'new_name' required"));
				return sol::lua_nil;
			}
			ERetargetSourceOrTarget Side;
			if (!ParseSourceOrTarget(ForStr, Side))
			{
				Session.Log(TEXT("[FAIL] duplicate_pose() -> 'for' must be \"source\" or \"target\""));
				return sol::lua_nil;
			}
			FScopedTransaction Txn(FText::FromString(TEXT("Duplicate Retarget Pose")));
			FName Result = Ctrl->DuplicateRetargetPose(
				FName(UTF8_TO_TCHAR(SrcName.c_str())),
				FName(UTF8_TO_TCHAR(NewName.c_str())),
				Side);
			if (Result.IsNone())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] duplicate_pose(\"%s\")"), UTF8_TO_TCHAR(SrcName.c_str())));
				return sol::lua_nil;
			}
			Session.Log(FString::Printf(TEXT("[OK] duplicate_pose(\"%s\" -> \"%s\")"), UTF8_TO_TCHAR(SrcName.c_str()), *Result.ToString()));
			return sol::make_object(Lv, std::string(TCHAR_TO_UTF8(*Result.ToString())));
		});

		// ====================================================================
		// rename_pose({old_name=.., new_name=.., for=..})
		// ====================================================================
		AssetObj.set_function("rename_pose", [Ctrl, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state St) -> sol::object
		{
			sol::state_view Lv(St);
			std::string OldName = Params.get_or<std::string>("old_name", "");
			std::string NewName = Params.get_or<std::string>("new_name", "");
			std::string ForStr = Params.get_or<std::string>("for", "target");
			if (OldName.empty() || NewName.empty())
			{
				Session.Log(TEXT("[FAIL] rename_pose() -> 'old_name' and 'new_name' required"));
				return sol::lua_nil;
			}
			ERetargetSourceOrTarget Side;
			if (!ParseSourceOrTarget(ForStr, Side))
			{
				Session.Log(TEXT("[FAIL] rename_pose() -> 'for' must be \"source\" or \"target\""));
				return sol::lua_nil;
			}
			FScopedTransaction Txn(FText::FromString(TEXT("Rename Retarget Pose")));
			bool bOk = Ctrl->RenameRetargetPose(
				FName(UTF8_TO_TCHAR(OldName.c_str())),
				FName(UTF8_TO_TCHAR(NewName.c_str())),
				Side);
			Session.Log(FString::Printf(TEXT("[%s] rename_pose(\"%s\" -> \"%s\")"),
				bOk ? TEXT("OK") : TEXT("FAIL"), UTF8_TO_TCHAR(OldName.c_str()), UTF8_TO_TCHAR(NewName.c_str())));
			return bOk ? sol::make_object(Lv, true) : sol::lua_nil;
		});

		// ====================================================================
		// set_current_pose({name=.., for=..})
		// ====================================================================
		AssetObj.set_function("set_current_pose", [Ctrl, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state St) -> sol::object
		{
			sol::state_view Lv(St);
			std::string PoseName = Params.get_or<std::string>("name", "");
			std::string ForStr = Params.get_or<std::string>("for", "target");
			if (PoseName.empty())
			{
				Session.Log(TEXT("[FAIL] set_current_pose() -> 'name' required"));
				return sol::lua_nil;
			}
			ERetargetSourceOrTarget Side;
			if (!ParseSourceOrTarget(ForStr, Side))
			{
				Session.Log(TEXT("[FAIL] set_current_pose() -> 'for' must be \"source\" or \"target\""));
				return sol::lua_nil;
			}
			FScopedTransaction Txn(FText::FromString(TEXT("Set Current Retarget Pose")));
			bool bOk = Ctrl->SetCurrentRetargetPose(FName(UTF8_TO_TCHAR(PoseName.c_str())), Side);
			Session.Log(FString::Printf(TEXT("[%s] set_current_pose(\"%s\", for=%s)"),
				bOk ? TEXT("OK") : TEXT("FAIL"), UTF8_TO_TCHAR(PoseName.c_str()), UTF8_TO_TCHAR(ForStr.c_str())));
			return bOk ? sol::make_object(Lv, true) : sol::lua_nil;
		});

		// ====================================================================
		// edit_pose_bone({bone=.., rotation={p,y,r}, for=..})
		// ====================================================================
		AssetObj.set_function("edit_pose_bone", [Ctrl, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state St) -> sol::object
		{
			sol::state_view Lv(St);
			std::string BoneName = Params.get_or<std::string>("bone", "");
			std::string ForStr = Params.get_or<std::string>("for", "target");
			if (BoneName.empty())
			{
				Session.Log(TEXT("[FAIL] edit_pose_bone() -> 'bone' required"));
				return sol::lua_nil;
			}
			ERetargetSourceOrTarget Side;
			if (!ParseSourceOrTarget(ForStr, Side))
			{
				Session.Log(TEXT("[FAIL] edit_pose_bone() -> 'for' must be \"source\" or \"target\""));
				return sol::lua_nil;
			}

			sol::optional<sol::table> RotTbl = Params.get<sol::optional<sol::table>>("rotation");
			if (!RotTbl.has_value())
			{
				Session.Log(TEXT("[FAIL] edit_pose_bone() -> 'rotation' table {p=.., y=.., r=..} required"));
				return sol::lua_nil;
			}
			sol::table RT = RotTbl.value();
			double Pitch = RT.get<sol::optional<double>>("p").value_or(0.0);
			double Yaw = RT.get<sol::optional<double>>("y").value_or(0.0);
			double Roll = RT.get<sol::optional<double>>("r").value_or(0.0);

			// Convert Euler (degrees) to Quaternion
			FRotator Rot(Pitch, Yaw, Roll);
			FQuat Quat = Rot.Quaternion();

			FScopedTransaction Txn(FText::FromString(TEXT("Edit Pose Bone")));
			Ctrl->SetRotationOffsetForRetargetPoseBone(FName(UTF8_TO_TCHAR(BoneName.c_str())), Quat, Side);
			Session.Log(FString::Printf(TEXT("[OK] edit_pose_bone(\"%s\", p=%.1f y=%.1f r=%.1f, for=%s)"),
				UTF8_TO_TCHAR(BoneName.c_str()), Pitch, Yaw, Roll, UTF8_TO_TCHAR(ForStr.c_str())));
			return sol::make_object(Lv, true);
		});

		// ====================================================================
		// set_root_offset({offset={x,y,z}, for=..})
		// ====================================================================
		AssetObj.set_function("set_root_offset", [Ctrl, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state St) -> sol::object
		{
			sol::state_view Lv(St);
			std::string ForStr = Params.get_or<std::string>("for", "target");
			ERetargetSourceOrTarget Side;
			if (!ParseSourceOrTarget(ForStr, Side))
			{
				Session.Log(TEXT("[FAIL] set_root_offset() -> 'for' must be \"source\" or \"target\""));
				return sol::lua_nil;
			}

			sol::optional<sol::table> OffTbl = Params.get<sol::optional<sol::table>>("offset");
			if (!OffTbl.has_value())
			{
				Session.Log(TEXT("[FAIL] set_root_offset() -> 'offset' table {x=.., y=.., z=..} required"));
				return sol::lua_nil;
			}
			sol::table OT = OffTbl.value();
			FVector Offset;
			Offset.X = OT.get<sol::optional<double>>("x").value_or(0.0);
			Offset.Y = OT.get<sol::optional<double>>("y").value_or(0.0);
			Offset.Z = OT.get<sol::optional<double>>("z").value_or(0.0);

			FScopedTransaction Txn(FText::FromString(TEXT("Set Root Offset")));
			Ctrl->SetRootOffsetInRetargetPose(Offset, Side);
			Session.Log(FString::Printf(TEXT("[OK] set_root_offset(%.1f, %.1f, %.1f, for=%s)"),
				Offset.X, Offset.Y, Offset.Z, UTF8_TO_TCHAR(ForStr.c_str())));
			return sol::make_object(Lv, true);
		});

		// ====================================================================
		// reset_pose({pose_name=.., bones={..}, for=..})
		// ====================================================================
		AssetObj.set_function("reset_pose", [Ctrl, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state St) -> sol::object
		{
			sol::state_view Lv(St);
			std::string PoseName = Params.get_or<std::string>("pose_name", "Default");
			std::string ForStr = Params.get_or<std::string>("for", "target");
			ERetargetSourceOrTarget Side;
			if (!ParseSourceOrTarget(ForStr, Side))
			{
				Session.Log(TEXT("[FAIL] reset_pose() -> 'for' must be \"source\" or \"target\""));
				return sol::lua_nil;
			}

			TArray<FName> BonesToReset;
			sol::optional<sol::table> BonesTbl = Params.get<sol::optional<sol::table>>("bones");
			if (BonesTbl.has_value())
			{
				sol::table BT = BonesTbl.value();
				for (auto& KV : BT)
				{
					if (KV.second.is<std::string>())
					{
						BonesToReset.Add(FName(UTF8_TO_TCHAR(KV.second.as<std::string>().c_str())));
					}
				}
			}

			FScopedTransaction Txn(FText::FromString(TEXT("Reset Retarget Pose")));
			Ctrl->ResetRetargetPose(FName(UTF8_TO_TCHAR(PoseName.c_str())), BonesToReset, Side);
			Session.Log(FString::Printf(TEXT("[OK] reset_pose(\"%s\", %d bones, for=%s)"),
				UTF8_TO_TCHAR(PoseName.c_str()), BonesToReset.Num(), UTF8_TO_TCHAR(ForStr.c_str())));
			return sol::make_object(Lv, true);
		});

		// ====================================================================
		// snap_to_ground({bone=.., for=..})
		// ====================================================================
		AssetObj.set_function("snap_to_ground", [Ctrl, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state St) -> sol::object
		{
			sol::state_view Lv(St);
			std::string BoneName = Params.get_or<std::string>("bone", "");
			std::string ForStr = Params.get_or<std::string>("for", "target");
			if (BoneName.empty())
			{
				Session.Log(TEXT("[FAIL] snap_to_ground() -> 'bone' required"));
				return sol::lua_nil;
			}
			ERetargetSourceOrTarget Side;
			if (!ParseSourceOrTarget(ForStr, Side))
			{
				Session.Log(TEXT("[FAIL] snap_to_ground() -> 'for' must be \"source\" or \"target\""));
				return sol::lua_nil;
			}

			FScopedTransaction Txn(FText::FromString(TEXT("Snap Bone to Ground")));
			Ctrl->SnapBoneToGround(FName(UTF8_TO_TCHAR(BoneName.c_str())), Side);
			Session.Log(FString::Printf(TEXT("[OK] snap_to_ground(\"%s\", for=%s)"),
				UTF8_TO_TCHAR(BoneName.c_str()), UTF8_TO_TCHAR(ForStr.c_str())));
			return sol::make_object(Lv, true);
		});

		// ====================================================================
		// get_rotation_offset({bone=.., for=..})
		// ====================================================================
		AssetObj.set_function("get_rotation_offset", [Ctrl, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state St) -> sol::object
		{
			sol::state_view Lv(St);
			std::string BoneName = Params.get_or<std::string>("bone", "");
			std::string ForStr = Params.get_or<std::string>("for", "target");
			if (BoneName.empty())
			{
				Session.Log(TEXT("[FAIL] get_rotation_offset() -> 'bone' required"));
				return sol::lua_nil;
			}
			ERetargetSourceOrTarget Side;
			if (!ParseSourceOrTarget(ForStr, Side))
			{
				Session.Log(TEXT("[FAIL] get_rotation_offset() -> 'for' must be \"source\" or \"target\""));
				return sol::lua_nil;
			}

			FQuat Quat = Ctrl->GetRotationOffsetForRetargetPoseBone(FName(UTF8_TO_TCHAR(BoneName.c_str())), Side);
			FRotator Rot = Quat.Rotator();
			sol::table Result = Lv.create_table();
			Result["p"] = Rot.Pitch;
			Result["y"] = Rot.Yaw;
			Result["r"] = Rot.Roll;
			Session.Log(FString::Printf(TEXT("[OK] get_rotation_offset(\"%s\", for=%s) -> p=%.2f y=%.2f r=%.2f"),
				UTF8_TO_TCHAR(BoneName.c_str()), UTF8_TO_TCHAR(ForStr.c_str()), Rot.Pitch, Rot.Yaw, Rot.Roll));
			return Result;
		});

		// ====================================================================
		// get_root_offset({for=..})
		// ====================================================================
		AssetObj.set_function("get_root_offset", [Ctrl, &Session](sol::table /*self*/,
			sol::optional<sol::table> Params, sol::this_state St) -> sol::object
		{
			sol::state_view Lv(St);
			std::string ForStr = "target";
			if (Params.has_value()) ForStr = Params.value().get_or<std::string>("for", "target");

			ERetargetSourceOrTarget Side;
			if (!ParseSourceOrTarget(ForStr, Side))
			{
				Session.Log(TEXT("[FAIL] get_root_offset() -> 'for' must be \"source\" or \"target\""));
				return sol::lua_nil;
			}

			FVector Offset = Ctrl->GetRootOffsetInRetargetPose(Side);
			sol::table Result = Lv.create_table();
			Result["x"] = Offset.X;
			Result["y"] = Offset.Y;
			Result["z"] = Offset.Z;
			Session.Log(FString::Printf(TEXT("[OK] get_root_offset(for=%s) -> %.2f, %.2f, %.2f"),
				UTF8_TO_TCHAR(ForStr.c_str()), Offset.X, Offset.Y, Offset.Z));
			return Result;
		});

		// ====================================================================
		// get_source_chain({target=.., op_name=..})
		// ====================================================================
		AssetObj.set_function("get_source_chain", [Ctrl, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state St) -> sol::object
		{
			sol::state_view Lv(St);
			std::string TargetChain = Params.get_or<std::string>("target", "");
			if (TargetChain.empty())
			{
				Session.Log(TEXT("[FAIL] get_source_chain() -> 'target' chain name required"));
				return sol::lua_nil;
			}
			std::string OpNameStr = Params.get_or<std::string>("op_name", "");
			FName OpName = OpNameStr.empty() ? NAME_None : FName(UTF8_TO_TCHAR(OpNameStr.c_str()));

	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			FName SourceChain = Ctrl->GetSourceChain(FName(UTF8_TO_TCHAR(TargetChain.c_str())), OpName);
#else
			FName SourceChain = Ctrl->GetSourceChain(FName(UTF8_TO_TCHAR(TargetChain.c_str())));
#endif
			if (SourceChain.IsNone())
			{
				Session.Log(FString::Printf(TEXT("[OK] get_source_chain(\"%s\") -> None (unmapped)"), UTF8_TO_TCHAR(TargetChain.c_str())));
				return sol::lua_nil;
			}
			Session.Log(FString::Printf(TEXT("[OK] get_source_chain(\"%s\") -> \"%s\""),
				UTF8_TO_TCHAR(TargetChain.c_str()), *SourceChain.ToString()));
			return sol::make_object(Lv, std::string(TCHAR_TO_UTF8(*SourceChain.ToString())));
		});

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
		// ====================================================================
		// run_op_setup(name_or_index) — 5.6+
		// ====================================================================
		AssetObj.set_function("run_op_setup", [Ctrl, &Session](sol::table /*self*/,
			sol::object IdArg, sol::this_state St) -> sol::object
		{
			sol::state_view Lv(St);
			int32 Idx = -1;
			if (IdArg.is<int>())
			{
				Idx = IdArg.as<int>();
			}
			else if (IdArg.is<std::string>())
			{
				FString OpName = UTF8_TO_TCHAR(IdArg.as<std::string>().c_str());
				Idx = FindOpIndexByName(Ctrl, OpName);
			}
			if (Idx < 0 || Idx >= Ctrl->GetNumRetargetOps())
			{
				Session.Log(TEXT("[FAIL] run_op_setup() -> op not found or invalid index"));
				return sol::lua_nil;
			}
			FScopedTransaction Txn(FText::FromString(TEXT("Run Op Initial Setup")));
			Ctrl->RunOpInitialSetup(Idx);
			FName OpName = Ctrl->GetOpName(Idx);
			Session.Log(FString::Printf(TEXT("[OK] run_op_setup(\"%s\")"), *OpName.ToString()));
			return sol::make_object(Lv, true);
		});

		// ====================================================================
		// reset_chain_settings({chain=.., op_name=..}) — 5.6+
		// ====================================================================
		AssetObj.set_function("reset_chain_settings", [Ctrl, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state St) -> sol::object
		{
			sol::state_view Lv(St);
			std::string ChainName = Params.get_or<std::string>("chain", "");
			std::string OpNameStr = Params.get_or<std::string>("op_name", "");
			if (ChainName.empty() || OpNameStr.empty())
			{
				Session.Log(TEXT("[FAIL] reset_chain_settings() -> 'chain' and 'op_name' required"));
				return sol::lua_nil;
			}
			FScopedTransaction Txn(FText::FromString(TEXT("Reset Chain Settings")));
			Ctrl->ResetChainSettingsToDefault(FName(UTF8_TO_TCHAR(ChainName.c_str())), FName(UTF8_TO_TCHAR(OpNameStr.c_str())));
			Session.Log(FString::Printf(TEXT("[OK] reset_chain_settings(\"%s\", op=\"%s\")"),
				UTF8_TO_TCHAR(ChainName.c_str()), UTF8_TO_TCHAR(OpNameStr.c_str())));
			return sol::make_object(Lv, true);
		});

		// ====================================================================
		// get_target_ikrig_for_op(op_name) — 5.6+
		// ====================================================================
		AssetObj.set_function("get_target_ikrig_for_op", [Ctrl, &Session](sol::table /*self*/,
			const std::string& OpName, sol::this_state St) -> sol::object
		{
			sol::state_view Lv(St);
			FName FOpName = FName(UTF8_TO_TCHAR(OpName.c_str()));
			const UIKRigDefinition* IKRig = Ctrl->GetTargetIKRigForOp(FOpName);
			if (!IKRig)
			{
				Session.Log(FString::Printf(TEXT("[OK] get_target_ikrig_for_op(\"%s\") -> none"), UTF8_TO_TCHAR(OpName.c_str())));
				return sol::lua_nil;
			}
			std::string Path = TCHAR_TO_UTF8(*IKRig->GetPathName());
			Session.Log(FString::Printf(TEXT("[OK] get_target_ikrig_for_op(\"%s\") -> \"%s\""),
				UTF8_TO_TCHAR(OpName.c_str()), *IKRig->GetPathName()));
			return sol::make_object(Lv, Path);
		});
#endif // ENGINE_MINOR_VERSION >= 6

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
		// ====================================================================
		// get_pelvis_bones({op_name=..}) — read pelvis bone assignments
		// ====================================================================
		AssetObj.set_function("get_pelvis_bones", [Ctrl, &Session](sol::table /*self*/,
			sol::optional<sol::table> Params, sol::this_state St) -> sol::object
		{
			sol::state_view Lv(St);
			// Find pelvis op
			std::string OpNameStr = "";
			if (Params.has_value()) OpNameStr = Params.value().get_or<std::string>("op_name", "");

			int32 NumOps = Ctrl->GetNumRetargetOps();
			for (int32 OpIdx = 0; OpIdx < NumOps; OpIdx++)
			{
				FIKRetargetOpBase* Op = Ctrl->GetRetargetOpByIndex(OpIdx);
				if (!Op || !Op->GetType()->IsChildOf(FIKRetargetPelvisMotionOp::StaticStruct())) continue;
				if (!OpNameStr.empty())
				{
					FName OpNm = Ctrl->GetOpName(OpIdx);
					if (!OpNm.ToString().Equals(UTF8_TO_TCHAR(OpNameStr.c_str()), ESearchCase::IgnoreCase)) continue;
				}

				UIKRetargetOpControllerBase* BaseCtrl = Ctrl->GetOpController(OpIdx);
				UIKRetargetPelvisMotionController* PelvisCtrl = Cast<UIKRetargetPelvisMotionController>(BaseCtrl);
				if (!PelvisCtrl) continue;

				sol::table Result = Lv.create_table();
				FName SrcBone = PelvisCtrl->GetSourcePelvisBone();
				FName TgtBone = PelvisCtrl->GetTargetPelvisBone();
				Result["source_pelvis_bone"] = SrcBone.IsNone() ? "" : TCHAR_TO_UTF8(*SrcBone.ToString());
				Result["target_pelvis_bone"] = TgtBone.IsNone() ? "" : TCHAR_TO_UTF8(*TgtBone.ToString());
				Session.Log(FString::Printf(TEXT("[OK] get_pelvis_bones() -> src=\"%s\", tgt=\"%s\""),
					*SrcBone.ToString(), *TgtBone.ToString()));
				return Result;
			}
			Session.Log(TEXT("[FAIL] get_pelvis_bones() -> no Pelvis Motion op found"));
			return sol::lua_nil;
		});

		// ====================================================================
		// get_root_bones({op_name=..}) — read root motion bone assignments
		// ====================================================================
		AssetObj.set_function("get_root_bones", [Ctrl, &Session](sol::table /*self*/,
			sol::optional<sol::table> Params, sol::this_state St) -> sol::object
		{
			sol::state_view Lv(St);
			std::string OpNameStr = "";
			if (Params.has_value()) OpNameStr = Params.value().get_or<std::string>("op_name", "");

			int32 NumOps = Ctrl->GetNumRetargetOps();
			for (int32 OpIdx = 0; OpIdx < NumOps; OpIdx++)
			{
				FIKRetargetOpBase* Op = Ctrl->GetRetargetOpByIndex(OpIdx);
				if (!Op || !Op->GetType()->IsChildOf(FIKRetargetRootMotionOp::StaticStruct())) continue;
				if (!OpNameStr.empty())
				{
					FName OpNm = Ctrl->GetOpName(OpIdx);
					if (!OpNm.ToString().Equals(UTF8_TO_TCHAR(OpNameStr.c_str()), ESearchCase::IgnoreCase)) continue;
				}

				UIKRetargetOpControllerBase* BaseCtrl = Ctrl->GetOpController(OpIdx);
				UIKRetargetRootMotionController* RootCtrl = Cast<UIKRetargetRootMotionController>(BaseCtrl);
				if (!RootCtrl) continue;

				sol::table Result = Lv.create_table();
				FName SrcRoot = RootCtrl->GetSourceRootBone();
				FName TgtRoot = RootCtrl->GetTargetRootBone();
				FName TgtPelvis = RootCtrl->GetTargetPelvisBone();
				Result["source_root_bone"] = SrcRoot.IsNone() ? "" : TCHAR_TO_UTF8(*SrcRoot.ToString());
				Result["target_root_bone"] = TgtRoot.IsNone() ? "" : TCHAR_TO_UTF8(*TgtRoot.ToString());
				Result["target_pelvis_bone"] = TgtPelvis.IsNone() ? "" : TCHAR_TO_UTF8(*TgtPelvis.ToString());
				Session.Log(FString::Printf(TEXT("[OK] get_root_bones() -> src_root=\"%s\", tgt_root=\"%s\", tgt_pelvis=\"%s\""),
					*SrcRoot.ToString(), *TgtRoot.ToString(), *TgtPelvis.ToString()));
				return Result;
			}
			Session.Log(TEXT("[FAIL] get_root_bones() -> no Root Motion op found"));
			return sol::lua_nil;
		});
#endif // ENGINE_MINOR_VERSION >= 7

		// ====================================================================
		// info() — override default
		// ====================================================================
		AssetObj.set_function("info", [Ctrl, Retargeter, &Session](sol::table /*self*/, sol::this_state St) -> sol::object
		{
			sol::state_view Lv(St);
			sol::table Result = Lv.create_table();

			// Source IK Rig
			const UIKRigDefinition* SourceRig = Ctrl->GetIKRig(ERetargetSourceOrTarget::Source);
			Result["source_ikrig"] = SourceRig ? TCHAR_TO_UTF8(*SourceRig->GetPathName()) : "none";

			// Target IK Rig
			const UIKRigDefinition* TargetRig = Ctrl->GetIKRig(ERetargetSourceOrTarget::Target);
			Result["target_ikrig"] = TargetRig ? TCHAR_TO_UTF8(*TargetRig->GetPathName()) : "none";

			// Preview meshes
			USkeletalMesh* SrcMesh = Ctrl->GetPreviewMesh(ERetargetSourceOrTarget::Source);
			Result["source_mesh"] = SrcMesh ? TCHAR_TO_UTF8(*SrcMesh->GetName()) : "none";
			USkeletalMesh* TgtMesh = Ctrl->GetPreviewMesh(ERetargetSourceOrTarget::Target);
			Result["target_mesh"] = TgtMesh ? TCHAR_TO_UTF8(*TgtMesh->GetName()) : "none";

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			// Op stack summary (5.6+)
			int32 NumOps = Ctrl->GetNumRetargetOps();
			Result["num_ops"] = NumOps;
			sol::table OpList = Lv.create_table();
			for (int32 Idx = 0; Idx < NumOps; Idx++)
			{
				sol::table E = Lv.create_table();
				E["index"] = Idx;
				FName OpName = Ctrl->GetOpName(Idx);
				E["name"] = TCHAR_TO_UTF8(*OpName.ToString());
				E["enabled"] = Ctrl->GetRetargetOpEnabled(Idx);
				FIKRetargetOpBase* Op = Ctrl->GetRetargetOpByIndex(Idx);
				E["type"] = TCHAR_TO_UTF8(*GetOpTypeDisplayName(Op));
				OpList[Idx + 1] = E;
			}
			Result["ops"] = OpList;

			// Chain mapping count
			int32 MappingCount = 0;
			for (int32 OpIdx = 0; OpIdx < NumOps; OpIdx++)
			{
				FName OpName = Ctrl->GetOpName(OpIdx);
				const FRetargetChainMapping* Mapping = Ctrl->GetChainMapping(OpName);
				if (Mapping)
				{
					MappingCount += Mapping->GetChainPairs().Num();
				}
			}
			Result["num_chain_mappings"] = MappingCount;
#else
			Result["num_ops"] = 0;
			Result["num_chain_mappings"] = 0;
#endif

			// Pose names
			sol::table SourcePoseNames = Lv.create_table();
			TMap<FName, FIKRetargetPose>& SrcPoses = Ctrl->GetRetargetPoses(ERetargetSourceOrTarget::Source);
			int32 SPi = 1;
			for (auto& KV : SrcPoses)
			{
				SourcePoseNames[SPi++] = TCHAR_TO_UTF8(*KV.Key.ToString());
			}
			Result["source_poses"] = SourcePoseNames;
			Result["source_current_pose"] = TCHAR_TO_UTF8(*Ctrl->GetCurrentRetargetPoseName(ERetargetSourceOrTarget::Source).ToString());

			sol::table TargetPoseNames = Lv.create_table();
			TMap<FName, FIKRetargetPose>& TgtPoses = Ctrl->GetRetargetPoses(ERetargetSourceOrTarget::Target);
			int32 TPi = 1;
			for (auto& KV : TgtPoses)
			{
				TargetPoseNames[TPi++] = TCHAR_TO_UTF8(*KV.Key.ToString());
			}
			Result["target_poses"] = TargetPoseNames;
			Result["target_current_pose"] = TCHAR_TO_UTF8(*Ctrl->GetCurrentRetargetPoseName(ERetargetSourceOrTarget::Target).ToString());

			Session.Log(FString::Printf(TEXT("[OK] info() -> %d ops, %d chain mappings, %d src poses, %d tgt poses"),
				Result.get_or("num_ops", 0), Result.get_or("num_chain_mappings", 0), SrcPoses.Num(), TgtPoses.Num()));
			return Result;
		});
	});
}

REGISTER_LUA_BINDING(IKRetargeter, IKRetargeterDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("IKRig")))
	{
		Session.Log(TEXT("[WARN] IKRig plugin is not loaded. Enable it in Edit > Plugins to use this feature."));
		return;
	}
	BindIKRetargeter(Lua, Session);
});
