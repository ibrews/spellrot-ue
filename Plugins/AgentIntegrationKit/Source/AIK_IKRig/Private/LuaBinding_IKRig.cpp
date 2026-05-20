#include "Lua/LuaBindingRegistry.h"
#include "ScopedTransaction.h"

// IK Rig API
#include "Rig/IKRigDefinition.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
#include "Rig/Solvers/IKRigSolverBase.h"
#endif
#include "RigEditor/IKRigController.h"
#include "Lua/LuaDynamicTypeHelper.h"
#include "Modules/ModuleManager.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
// Backward-compatible aliases for solver type names
static const TMap<FString, FString> SolverAliases = {
	{ TEXT("FBIK"), TEXT("FullBodyIK") },
	{ TEXT("Limb"), TEXT("Limb") },
	{ TEXT("StretchLimb"), TEXT("StretchLimb") },
	{ TEXT("Pole"), TEXT("Pole") },
	{ TEXT("BodyMover"), TEXT("BodyMover") },
	{ TEXT("SetTransform"), TEXT("SetTransform") },
};

static const TArray<FString> SolverPrefixes = { TEXT("FIKRig"), TEXT("F") };
static const TArray<FString> SolverSuffixes = { TEXT("Solver") };

static FString ResolveIKSolverPath(const FString& TypeName)
{
	// Try alias first
	FString Resolved = TypeName;
	if (const FString* Alias = SolverAliases.Find(TypeName))
		Resolved = *Alias;

	// Dynamic struct discovery
	UScriptStruct* SolverStruct = LuaDynamicType::FindDerivedStruct(
		FIKRigSolverBase::StaticStruct(), Resolved, SolverPrefixes, SolverSuffixes);
	if (SolverStruct)
		return SolverStruct->GetPathName();

	return FString();
}
#endif // ENGINE_MINOR_VERSION >= 6

static TArray<FLuaFunctionDoc> IKRigDocs = {};

static void BindIKRig(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_ikrig", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		UIKRigDefinition* IKRig = LoadObject<UIKRigDefinition>(nullptr, *FPath);
		if (!IKRig) return;

		UIKRigController* Ctrl = UIKRigController::GetController(IKRig);
		if (!Ctrl) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"Element types for add/remove/list/configure:\n"
			"  solver   — IK solver (dynamically discovered — use list(\"solver_types\") or try FBIK, Limb, Pole, etc.)\n"
			"  goal     — IK goal attached to a bone\n"
			"  chain    — retarget chain (start_bone -> end_bone)\n"
			"\n"
			"add(type, params):\n"
			"  add(\"solver\", {type=\"FBIK\", root_bone=.., end_bone=.., enabled=true})\n"
			"  add(\"goal\", {name=\"LeftFoot\", bone=\"foot_l\"})\n"
			"  add(\"chain\", {name=\"Spine\", start_bone=\"spine_01\", end_bone=\"spine_05\", goal=\"SpineGoal\"})\n"
			"\n"
			"remove(type, id):\n"
			"  remove(\"solver\", 0)         — by index\n"
			"  remove(\"goal\", \"LeftFoot\")  — by name\n"
			"  remove(\"chain\", \"Spine\")    — by name\n"
			"\n"
			"configure(type, id, params):\n"
			"  configure(\"solver\", 0, {target_index=2})  — move solver to index 2\n"
			"  configure(\"goal\", \"LeftFoot\", {new_name=\"LFoot\", bone=\"foot_l\", position_alpha=0.5, rotation_alpha=1.0})\n"
			"  configure(\"chain\", \"Spine\", {new_name=\"Back\", start_bone=.., end_bone=.., goal=..})\n"
			"\n"
			"list(type):\n"
			"  list(\"solvers\"), list(\"goals\"), list(\"chains\"), list(\"excluded_bones\"), list(\"solver_types\")\n"
			"\n"
			"Action methods:\n"
			"  connect_goal(goal_name, solver_index) — connect goal to solver\n"
			"  disconnect_goal(goal_name, solver_index) — disconnect\n"
			"  set_mesh(skeletal_mesh_path) — assign skeletal mesh\n"
			"  is_mesh_compatible(skeletal_mesh_path) — check if mesh is compatible\n"
			"  set_retarget_root(bone) — set retarget root bone\n"
			"  auto_retarget() — auto-generate retarget chains\n"
			"  auto_fbik() — auto-generate FBIK setup\n"
			"  get_solver_bones(solver_index) — get start/end bone for a solver\n"
			"  get_goal(goal_name) — get full goal details (bone, alphas, transform)\n"
			"  get_ref_pose(bone_name) — get reference pose transform of a bone\n"
			"  info() — summary of solvers/goals/chains/mesh\n"
			"\n"
			"Solver configuration:\n"
			"  configure_solver(solver_index, {prop=val, ..}) — set solver properties via reflection\n"
			"\n"
			"Bone exclusion:\n"
			"  exclude_bones({\"bone1\", \"bone2\"}) — exclude bones from solvers\n"
			"  include_bones({\"bone1\", \"bone2\"}) — re-include excluded bones\n"
			"\n"
			"Per-bone solver settings:\n"
			"  add_bone_setting(bone_name, solver_index) — add per-bone settings\n"
			"  remove_bone_setting(bone_name, solver_index) — remove per-bone settings\n"
			"  get_bone_settings(bone_name, solver_index) — read settings as table\n"
			"  set_bone_settings(bone_name, solver_index, {prop=val, ..}) — write settings\n";

		// ---- add(type, params) ----
		AssetObj.set_function("add", [Ctrl, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (FType.Equals(TEXT("solver"), ESearchCase::IgnoreCase))
			{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"solver\") -> params required: {type=\"FBIK\"}")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string SolverType = P.get_or<std::string>("type", "FBIK");
				FString SolverPath = ResolveIKSolverPath(UTF8_TO_TCHAR(SolverType.c_str()));
				if (SolverPath.IsEmpty()) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"solver\") -> unknown solver type '%s'. Valid: %s"), UTF8_TO_TCHAR(SolverType.c_str()), *LuaDynamicType::FormatAvailableStructTypes(FIKRigSolverBase::StaticStruct(), SolverPrefixes, SolverSuffixes))); return sol::lua_nil; }

				int32 Idx = Ctrl->AddSolver(SolverPath);
				if (Idx < 0) { Session.Log(TEXT("[FAIL] add(\"solver\")")); return sol::lua_nil; }

				std::string Root = P.get_or<std::string>("root_bone", "");
				std::string End = P.get_or<std::string>("end_bone", "");
				if (!Root.empty()) { FString RootStr = UTF8_TO_TCHAR(Root.c_str()); Ctrl->SetStartBone(FName(RootStr), Idx); }
				if (!End.empty()) { FString EndStr = UTF8_TO_TCHAR(End.c_str()); Ctrl->SetEndBone(FName(EndStr), Idx); }
				sol::optional<bool> Enabled = P.get<sol::optional<bool>>("enabled");
				if (Enabled.has_value()) Ctrl->SetSolverEnabled(Idx, Enabled.value());

				Ctrl->GetAsset()->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add(\"solver\", type=\"%s\") -> index %d"), UTF8_TO_TCHAR(SolverType.c_str()), Idx));
				return sol::make_object(Lua, Idx);
#else
				Session.Log(TEXT("[FAIL] add(\"solver\") -> solver management requires UE 5.6+"));
				return sol::lua_nil;
#endif // ENGINE_MINOR_VERSION >= 6
			}
			else if (FType.Equals(TEXT("goal"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"goal\") -> params required: {name=.., bone=..}")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string GoalName = P.get_or<std::string>("name", "");
				std::string BoneName = P.get_or<std::string>("bone", "");
				if (GoalName.empty() || BoneName.empty()) { Session.Log(TEXT("[FAIL] add(\"goal\") -> name and bone required")); return sol::lua_nil; }

				FString GoalStr = UTF8_TO_TCHAR(GoalName.c_str());
				FString BoneStr = UTF8_TO_TCHAR(BoneName.c_str());
				FName Result = Ctrl->AddNewGoal(FName(GoalStr), FName(BoneStr));
				if (Result.IsNone()) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"goal\", name=\"%s\")"), *GoalStr)); return sol::lua_nil; }
				Ctrl->GetAsset()->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add(\"goal\", name=\"%s\", bone=\"%s\")"), *Result.ToString(), *BoneStr));
				return sol::make_object(Lua, std::string(TCHAR_TO_UTF8(*Result.ToString())));
			}
			else if (FType.Equals(TEXT("chain"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"chain\") -> params required: {name=.., start_bone=.., end_bone=..}")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string ChainName = P.get_or<std::string>("name", "");
				std::string StartBone = P.get_or<std::string>("start_bone", "");
				std::string EndBone = P.get_or<std::string>("end_bone", "");
				if (ChainName.empty() || StartBone.empty() || EndBone.empty()) { Session.Log(TEXT("[FAIL] add(\"chain\") -> name, start_bone, end_bone required")); return sol::lua_nil; }

				FString ChainStr = UTF8_TO_TCHAR(ChainName.c_str());
				FString StartStr = UTF8_TO_TCHAR(StartBone.c_str());
				FString EndStr = UTF8_TO_TCHAR(EndBone.c_str());
				FName Goal = NAME_None;
				std::string GoalStr = P.get_or<std::string>("goal", "");
				if (!GoalStr.empty()) { FString G = UTF8_TO_TCHAR(GoalStr.c_str()); Goal = FName(G); }

				FName Result = Ctrl->AddRetargetChain(FName(ChainStr), FName(StartStr), FName(EndStr), Goal);
				if (Result.IsNone()) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"chain\", name=\"%s\")"), *ChainStr)); return sol::lua_nil; }
				Ctrl->GetAsset()->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add(\"chain\", \"%s\", %s -> %s)"), *Result.ToString(), *StartStr, *EndStr));
				return sol::make_object(Lua, std::string(TCHAR_TO_UTF8(*Result.ToString())));
			}

			Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Valid: solver, goal, chain"), *FType));
			return sol::lua_nil;
		});

		// ---- remove(type, id) ----
		AssetObj.set_function("remove", [Ctrl, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (FType.Equals(TEXT("solver"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<int>()) { Session.Log(TEXT("[FAIL] remove(\"solver\") -> index required")); return sol::lua_nil; }
				int32 Idx = Id.as<int>();
				bool bOk = Ctrl->RemoveSolver(Idx);
				if (bOk) Ctrl->GetAsset()->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[%s] remove(\"solver\", %d)"), bOk ? TEXT("OK") : TEXT("FAIL"), Idx));
				return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
			}
			else if (FType.Equals(TEXT("goal"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<std::string>()) { Session.Log(TEXT("[FAIL] remove(\"goal\") -> name required")); return sol::lua_nil; }
				FString GoalStr = UTF8_TO_TCHAR(Id.as<std::string>().c_str());
				bool bOk = Ctrl->RemoveGoal(FName(GoalStr));
				if (bOk) Ctrl->GetAsset()->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[%s] remove(\"goal\", \"%s\")"), bOk ? TEXT("OK") : TEXT("FAIL"), *GoalStr));
				return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
			}
			else if (FType.Equals(TEXT("chain"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<std::string>()) { Session.Log(TEXT("[FAIL] remove(\"chain\") -> name required")); return sol::lua_nil; }
				FString ChainStr = UTF8_TO_TCHAR(Id.as<std::string>().c_str());
				bool bOk = Ctrl->RemoveRetargetChain(FName(ChainStr));
				if (bOk) Ctrl->GetAsset()->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[%s] remove(\"chain\", \"%s\")"), bOk ? TEXT("OK") : TEXT("FAIL"), *ChainStr));
				return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type"), *FType));
			return sol::lua_nil;
		});

		// ---- configure(type, id, params) ----
		AssetObj.set_function("configure", [Ctrl, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (FType.Equals(TEXT("solver"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<int>()) { Session.Log(TEXT("[FAIL] configure(\"solver\") -> index required")); return sol::lua_nil; }
				int32 Idx = Id.as<int>();
				if (Idx < 0 || Idx >= Ctrl->GetNumSolvers())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"solver\", %d) -> index out of range"), Idx));
					return sol::lua_nil;
				}

				sol::optional<int> TargetIdx = Params.get<sol::optional<int>>("target_index");
				if (TargetIdx.has_value())
				{
					FScopedTransaction Txn(FText::FromString(TEXT("Move Solver In Stack")));
					bool bOk = Ctrl->MoveSolverInStack(Idx, TargetIdx.value());
					if (!bOk) { Session.Log(FString::Printf(TEXT("[FAIL] configure(\"solver\", %d) -> MoveSolverInStack to %d failed"), Idx, TargetIdx.value())); return sol::lua_nil; }
					Ctrl->GetAsset()->MarkPackageDirty();
					Session.Log(FString::Printf(TEXT("[OK] configure(\"solver\", %d, target_index=%d)"), Idx, TargetIdx.value()));
					return sol::make_object(Lua, true);
				}

				Session.Log(FString::Printf(TEXT("[FAIL] configure(\"solver\", %d) -> no valid params (use target_index)"), Idx));
				return sol::lua_nil;
			}
			else if (FType.Equals(TEXT("goal"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<std::string>()) { Session.Log(TEXT("[FAIL] configure(\"goal\") -> name required")); return sol::lua_nil; }
				FString GoalStr = UTF8_TO_TCHAR(Id.as<std::string>().c_str());
				FName GoalName(GoalStr);

				FScopedTransaction Txn(FText::FromString(TEXT("Configure IK Goal")));
				int32 Applied = 0;

				std::string NewBone = Params.get_or<std::string>("bone", "");
				if (!NewBone.empty())
				{
					FString BoneStr = UTF8_TO_TCHAR(NewBone.c_str());
					bool bOk = Ctrl->SetGoalBone(GoalName, FName(BoneStr));
					if (bOk) Applied++;
					else Session.Log(FString::Printf(TEXT("[WARN] configure(\"goal\") -> SetGoalBone(\"%s\") failed"), *BoneStr));
				}

				// Goal alpha values
				UIKRigEffectorGoal* GoalObj = Ctrl->GetGoal(GoalName);
				if (GoalObj)
				{
					sol::optional<double> PosAlpha = Params.get<sol::optional<double>>("position_alpha");
					if (PosAlpha.has_value())
					{
						GoalObj->PositionAlpha = FMath::Clamp((float)PosAlpha.value(), 0.0f, 1.0f);
						Applied++;
					}
					sol::optional<double> RotAlpha = Params.get<sol::optional<double>>("rotation_alpha");
					if (RotAlpha.has_value())
					{
						GoalObj->RotationAlpha = FMath::Clamp((float)RotAlpha.value(), 0.0f, 1.0f);
						Applied++;
					}
				}

				std::string NewName = Params.get_or<std::string>("new_name", "");
				if (!NewName.empty())
				{
					FString NewNameStr = UTF8_TO_TCHAR(NewName.c_str());
					FName Result = Ctrl->RenameGoal(GoalName, FName(NewNameStr));
					if (!Result.IsNone() && Result != GoalName)
					{
						GoalName = Result;
						Applied++;
					}
					else if (Result.IsNone())
					{
						Session.Log(FString::Printf(TEXT("[WARN] configure(\"goal\") -> RenameGoal(\"%s\") failed"), *NewNameStr));
					}
				}

				if (Applied > 0) Ctrl->GetAsset()->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[%s] configure(\"goal\", \"%s\") -> %d changes"), Applied > 0 ? TEXT("OK") : TEXT("FAIL"), *GoalStr, Applied));
				return Applied > 0 ? sol::make_object(Lua, Applied) : sol::lua_nil;
			}
			else if (FType.Equals(TEXT("chain"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<std::string>()) { Session.Log(TEXT("[FAIL] configure(\"chain\") -> name required")); return sol::lua_nil; }
				FString ChainStr = UTF8_TO_TCHAR(Id.as<std::string>().c_str());
				FName ChainName(ChainStr);

				FScopedTransaction Txn(FText::FromString(TEXT("Configure Retarget Chain")));
				int32 Applied = 0;

				std::string StartBone = Params.get_or<std::string>("start_bone", "");
				if (!StartBone.empty())
				{
					FString BoneStr = UTF8_TO_TCHAR(StartBone.c_str());
					bool bOk = Ctrl->SetRetargetChainStartBone(ChainName, FName(BoneStr));
					if (bOk) Applied++;
					else Session.Log(FString::Printf(TEXT("[WARN] configure(\"chain\") -> SetRetargetChainStartBone(\"%s\") failed"), *BoneStr));
				}

				std::string EndBone = Params.get_or<std::string>("end_bone", "");
				if (!EndBone.empty())
				{
					FString BoneStr = UTF8_TO_TCHAR(EndBone.c_str());
					bool bOk = Ctrl->SetRetargetChainEndBone(ChainName, FName(BoneStr));
					if (bOk) Applied++;
					else Session.Log(FString::Printf(TEXT("[WARN] configure(\"chain\") -> SetRetargetChainEndBone(\"%s\") failed"), *BoneStr));
				}

				std::string Goal = Params.get_or<std::string>("goal", "");
				if (!Goal.empty())
				{
					FString GoalStr = UTF8_TO_TCHAR(Goal.c_str());
					bool bOk = Ctrl->SetRetargetChainGoal(ChainName, FName(GoalStr));
					if (bOk) Applied++;
					else Session.Log(FString::Printf(TEXT("[WARN] configure(\"chain\") -> SetRetargetChainGoal(\"%s\") failed"), *GoalStr));
				}

				std::string NewName = Params.get_or<std::string>("new_name", "");
				if (!NewName.empty())
				{
					FString NewNameStr = UTF8_TO_TCHAR(NewName.c_str());
					FName Result = Ctrl->RenameRetargetChain(ChainName, FName(NewNameStr));
					if (Result != ChainName)
					{
						ChainName = Result;
						Applied++;
					}
					else Session.Log(FString::Printf(TEXT("[WARN] configure(\"chain\") -> RenameRetargetChain(\"%s\") failed"), *NewNameStr));
				}

				if (Applied > 0) Ctrl->GetAsset()->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[%s] configure(\"chain\", \"%s\") -> %d changes"), Applied > 0 ? TEXT("OK") : TEXT("FAIL"), *ChainStr, Applied));
				return Applied > 0 ? sol::make_object(Lua, Applied) : sol::lua_nil;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> unknown type. Valid: solver, goal, chain"), *FType));
			return sol::lua_nil;
		});

		// ---- list(type?) ----
		AssetObj.set_function("list", [Ctrl, &Session](sol::table self,
			sol::optional<std::string> TypeOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = TypeOpt.has_value() ? UTF8_TO_TCHAR(TypeOpt.value().c_str()) : TEXT("all");

			if (FType.Equals(TEXT("all"), ESearchCase::IgnoreCase))
			{
				// Return full info
				sol::protected_function InfoFn = self["info"];
				if (InfoFn.valid()) return InfoFn(self);
				return sol::lua_nil;
			}

			if (FType.Equals(TEXT("solvers"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("solver"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				int32 Count = Ctrl->GetNumSolvers();
				const TArray<UIKRigEffectorGoal*>& AllGoals = Ctrl->GetAllGoals();
				for (int32 i = 0; i < Count; i++)
				{
					sol::table E = Lua.create_table();
					E["index"] = i;
					E["type"] = TCHAR_TO_UTF8(*Ctrl->GetSolverUniqueName(i));
					E["enabled"] = Ctrl->GetSolverEnabled(i);
	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
					E["start_bone"] = TCHAR_TO_UTF8(*Ctrl->GetStartBone(i).ToString());
#endif
					E["end_bone"] = TCHAR_TO_UTF8(*Ctrl->GetEndBone(i).ToString());
					// Enrichment: connected goals
					sol::table ConnGoals = Lua.create_table();
					int32 GoalIdx = 1;
					for (const UIKRigEffectorGoal* Goal : AllGoals)
					{
						if (!Goal) continue;
						if (Ctrl->IsGoalConnectedToSolver(Goal->GoalName, i))
						{
							ConnGoals[GoalIdx++] = TCHAR_TO_UTF8(*Goal->GoalName.ToString());
						}
					}
					E["connected_goals"] = ConnGoals;
					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"solvers\") -> %d"), Count));
				return Result;
			}

			if (FType.Equals(TEXT("goals"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("goal"), ESearchCase::IgnoreCase))
			{
				const TArray<UIKRigEffectorGoal*>& Goals = Ctrl->GetAllGoals();
				int32 NumSolvers = Ctrl->GetNumSolvers();
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Goals.Num(); i++)
				{
					if (!Goals[i]) continue;
					sol::table E = Lua.create_table();
					E["name"] = TCHAR_TO_UTF8(*Goals[i]->GoalName.ToString());
					FName BoneForGoal = Ctrl->GetBoneForGoal(Goals[i]->GoalName);
					E["bone"] = TCHAR_TO_UTF8(*BoneForGoal.ToString());
					E["position_alpha"] = Goals[i]->PositionAlpha;
					E["rotation_alpha"] = Goals[i]->RotationAlpha;
					// Enrichment: connected solver indices
					sol::table ConnSolvers = Lua.create_table();
					int32 SolverIdx = 1;
					for (int32 s = 0; s < NumSolvers; s++)
					{
						if (Ctrl->IsGoalConnectedToSolver(Goals[i]->GoalName, s))
						{
							ConnSolvers[SolverIdx++] = s;
						}
					}
					E["connected_solvers"] = ConnSolvers;
					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"goals\") -> %d"), Goals.Num()));
				return Result;
			}

			if (FType.Equals(TEXT("chains"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("chain"), ESearchCase::IgnoreCase))
			{
				const TArray<FBoneChain>& Chains = Ctrl->GetRetargetChains();
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Chains.Num(); i++)
				{
					sol::table E = Lua.create_table();
					E["name"] = TCHAR_TO_UTF8(*Chains[i].ChainName.ToString());
					E["start_bone"] = TCHAR_TO_UTF8(*Chains[i].StartBone.BoneName.ToString());
					E["end_bone"] = TCHAR_TO_UTF8(*Chains[i].EndBone.BoneName.ToString());
					// Enrichment: goal for this chain
					FName ChainGoal = Ctrl->GetRetargetChainGoal(Chains[i].ChainName);
					E["goal"] = TCHAR_TO_UTF8(*ChainGoal.ToString());
					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"chains\") -> %d"), Chains.Num()));
				return Result;
			}

			if (FType.Equals(TEXT("solver_types"), ESearchCase::IgnoreCase))
			{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
				const TArray<FString>& Names = LuaDynamicType::ListDerivedStructNames(
					FIKRigSolverBase::StaticStruct(), SolverPrefixes, SolverSuffixes);
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Names.Num(); i++)
				{
					Result[i + 1] = TCHAR_TO_UTF8(*Names[i]);
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"solver_types\") -> %d types"), Names.Num()));
				return Result;
#else
				Session.Log(TEXT("[FAIL] list(\"solver_types\") -> solver type discovery requires UE 5.6+"));
				return sol::lua_nil;
#endif
			}

			if (FType.Equals(TEXT("excluded_bones"), ESearchCase::IgnoreCase))
			{
				const FIKRigSkeleton& Skel = Ctrl->GetIKRigSkeleton();
				sol::table Result = Lua.create_table();
				int32 Idx = 1;
				for (const FName& BoneName : Skel.BoneNames)
				{
					if (Ctrl->GetBoneExcluded(BoneName))
					{
						Result[Idx++] = TCHAR_TO_UTF8(*BoneName.ToString());
					}
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"excluded_bones\") -> %d"), Idx - 1));
				return Result;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: solvers, goals, chains, excluded_bones, solver_types"), *FType));
			return sol::lua_nil;
		});

		// ---- Action methods ----

		AssetObj.set_function("connect_goal", [Ctrl, &Session](sol::table /*self*/,
			const std::string& GoalName, int SolverIndex, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString GoalStr = UTF8_TO_TCHAR(GoalName.c_str());
			if (SolverIndex < 0 || SolverIndex >= Ctrl->GetNumSolvers())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] connect_goal -> solver index %d out of range"), SolverIndex));
				return sol::lua_nil;
			}
			bool bOk = Ctrl->ConnectGoalToSolver(FName(GoalStr), SolverIndex);
			Session.Log(FString::Printf(TEXT("[%s] connect_goal(\"%s\", %d)"), bOk ? TEXT("OK") : TEXT("FAIL"), *GoalStr, SolverIndex));
			return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
		});

		AssetObj.set_function("disconnect_goal", [Ctrl, &Session](sol::table /*self*/,
			const std::string& GoalName, int SolverIndex, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString GoalStr = UTF8_TO_TCHAR(GoalName.c_str());
			if (SolverIndex < 0 || SolverIndex >= Ctrl->GetNumSolvers())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] disconnect_goal -> solver index %d out of range"), SolverIndex));
				return sol::lua_nil;
			}
			bool bOk = Ctrl->DisconnectGoalFromSolver(FName(GoalStr), SolverIndex);
			Session.Log(FString::Printf(TEXT("[%s] disconnect_goal(\"%s\", %d)"), bOk ? TEXT("OK") : TEXT("FAIL"), *GoalStr, SolverIndex));
			return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
		});

		AssetObj.set_function("set_mesh", [Ctrl, &Session](sol::table /*self*/,
			const std::string& MeshPath, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FMesh = UTF8_TO_TCHAR(MeshPath.c_str());
			if (!FMesh.StartsWith(TEXT("/"))) FMesh = TEXT("/Game/") + FMesh;
			USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *FMesh);
			if (!Mesh) { Session.Log(FString::Printf(TEXT("[FAIL] set_mesh -> '%s' not found"), *FMesh)); return sol::lua_nil; }
			bool bOk = Ctrl->SetSkeletalMesh(Mesh);
			Session.Log(FString::Printf(TEXT("[%s] set_mesh(\"%s\")"), bOk ? TEXT("OK") : TEXT("FAIL"), *Mesh->GetName()));
			return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
		});

		AssetObj.set_function("set_retarget_root", [Ctrl, &Session](sol::table /*self*/,
			const std::string& BoneName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString BoneStr = UTF8_TO_TCHAR(BoneName.c_str());
			bool bOk = Ctrl->SetRetargetRoot(FName(BoneStr));
			Session.Log(FString::Printf(TEXT("[%s] set_retarget_root(\"%s\")"), bOk ? TEXT("OK") : TEXT("FAIL"), *BoneStr));
			return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
		});

		AssetObj.set_function("auto_retarget", [Ctrl, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			bool bOk = Ctrl->ApplyAutoGeneratedRetargetDefinition();
			Session.Log(FString::Printf(TEXT("[%s] auto_retarget()"), bOk ? TEXT("OK") : TEXT("FAIL")));
			return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
		});

		AssetObj.set_function("auto_fbik", [Ctrl, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			bool bOk = Ctrl->ApplyAutoFBIK();
			Session.Log(FString::Printf(TEXT("[%s] auto_fbik()"), bOk ? TEXT("OK") : TEXT("FAIL")));
			return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
		});

		AssetObj.set_function("is_mesh_compatible", [Ctrl, &Session](sol::table /*self*/,
			const std::string& MeshPath, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FMesh = UTF8_TO_TCHAR(MeshPath.c_str());
			if (!FMesh.StartsWith(TEXT("/"))) FMesh = TEXT("/Game/") + FMesh;
			USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *FMesh);
			if (!Mesh) { Session.Log(FString::Printf(TEXT("[FAIL] is_mesh_compatible -> '%s' not found"), *FMesh)); return sol::lua_nil; }
			bool bOk = Ctrl->IsSkeletalMeshCompatible(Mesh);
			Session.Log(FString::Printf(TEXT("[OK] is_mesh_compatible(\"%s\") -> %s"), *Mesh->GetName(), bOk ? TEXT("true") : TEXT("false")));
			return sol::make_object(Lua, bOk);
		});

		AssetObj.set_function("get_solver_bones", [Ctrl, &Session](sol::table /*self*/,
			int SolverIndex, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (SolverIndex < 0 || SolverIndex >= Ctrl->GetNumSolvers())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] get_solver_bones -> solver index %d out of range"), SolverIndex));
				return sol::lua_nil;
			}
			sol::table Result = Lua.create_table();
	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			Result["start_bone"] = TCHAR_TO_UTF8(*Ctrl->GetStartBone(SolverIndex).ToString());
#endif
			Result["end_bone"] = TCHAR_TO_UTF8(*Ctrl->GetEndBone(SolverIndex).ToString());
			Session.Log(FString::Printf(TEXT("[OK] get_solver_bones(%d)"), SolverIndex));
			return Result;
		});

		AssetObj.set_function("get_goal", [Ctrl, &Session](sol::table /*self*/,
			const std::string& GoalName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString GoalStr = UTF8_TO_TCHAR(GoalName.c_str());
			FName GoalFName(GoalStr);
			UIKRigEffectorGoal* Goal = Ctrl->GetGoal(GoalFName);
			if (!Goal) { Session.Log(FString::Printf(TEXT("[FAIL] get_goal -> '%s' not found"), *GoalStr)); return sol::lua_nil; }

			sol::table Result = Lua.create_table();
			Result["name"] = TCHAR_TO_UTF8(*Goal->GoalName.ToString());
			Result["bone"] = TCHAR_TO_UTF8(*Ctrl->GetBoneForGoal(GoalFName).ToString());
			Result["position_alpha"] = Goal->PositionAlpha;
			Result["rotation_alpha"] = Goal->RotationAlpha;

			const FTransform& T = Goal->CurrentTransform;
			sol::table Transform = Lua.create_table();
			sol::table Loc = Lua.create_table();
			Loc["x"] = T.GetLocation().X; Loc["y"] = T.GetLocation().Y; Loc["z"] = T.GetLocation().Z;
			Transform["location"] = Loc;
			sol::table Rot = Lua.create_table();
			FRotator R = T.Rotator();
			Rot["pitch"] = R.Pitch; Rot["yaw"] = R.Yaw; Rot["roll"] = R.Roll;
			Transform["rotation"] = Rot;
			sol::table Scl = Lua.create_table();
			Scl["x"] = T.GetScale3D().X; Scl["y"] = T.GetScale3D().Y; Scl["z"] = T.GetScale3D().Z;
			Transform["scale"] = Scl;
			Result["transform"] = Transform;

			// Connected solvers
			int32 NumSolvers = Ctrl->GetNumSolvers();
			sol::table ConnSolvers = Lua.create_table();
			int32 SolverIdx = 1;
			for (int32 s = 0; s < NumSolvers; s++)
			{
				if (Ctrl->IsGoalConnectedToSolver(GoalFName, s))
					ConnSolvers[SolverIdx++] = s;
			}
			Result["connected_solvers"] = ConnSolvers;

			Session.Log(FString::Printf(TEXT("[OK] get_goal(\"%s\")"), *GoalStr));
			return Result;
		});

		AssetObj.set_function("get_ref_pose", [Ctrl, &Session](sol::table /*self*/,
			const std::string& BoneName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString BoneStr = UTF8_TO_TCHAR(BoneName.c_str());
			FTransform T = Ctrl->GetRefPoseTransformOfBone(FName(BoneStr));
			sol::table Result = Lua.create_table();
			sol::table Loc = Lua.create_table();
			Loc["x"] = T.GetLocation().X; Loc["y"] = T.GetLocation().Y; Loc["z"] = T.GetLocation().Z;
			Result["location"] = Loc;
			sol::table Rot = Lua.create_table();
			FRotator R = T.Rotator();
			Rot["pitch"] = R.Pitch; Rot["yaw"] = R.Yaw; Rot["roll"] = R.Roll;
			Result["rotation"] = Rot;
			sol::table Scl = Lua.create_table();
			Scl["x"] = T.GetScale3D().X; Scl["y"] = T.GetScale3D().Y; Scl["z"] = T.GetScale3D().Z;
			Result["scale"] = Scl;
			Session.Log(FString::Printf(TEXT("[OK] get_ref_pose(\"%s\")"), *BoneStr));
			return Result;
		});

		// ---- configure_solver(solver_index, params) ----
		AssetObj.set_function("configure_solver", [Ctrl, &Session](sol::table /*self*/,
			int SolverIndex, sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			if (SolverIndex < 0 || SolverIndex >= Ctrl->GetNumSolvers())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] configure_solver -> solver index %d out of range"), SolverIndex));
				return sol::lua_nil;
			}

			FIKRigSolverBase* Solver = Ctrl->GetSolverAtIndex(SolverIndex);
			if (!Solver)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] configure_solver -> solver at index %d is null"), SolverIndex));
				return sol::lua_nil;
			}

			FIKRigSolverSettingsBase* Settings = Solver->GetSolverSettings();
			const UScriptStruct* SettingsType = Solver->GetSolverSettingsType();
			if (!Settings || !SettingsType)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] configure_solver -> solver %d has no configurable settings"), SolverIndex));
				return sol::lua_nil;
			}

			int32 Applied = 0;
			for (auto& [key, val] : Params)
			{
				if (!key.is<std::string>()) continue;
				FString PropName = UTF8_TO_TCHAR(key.as<std::string>().c_str());
				FProperty* Prop = SettingsType->FindPropertyByName(FName(PropName));
				if (!Prop) continue;
				if (Prop->HasAnyPropertyFlags(CPF_BlueprintReadOnly | CPF_EditConst)) continue;

				void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Settings);

				if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
				{
					if (val.is<double>()) { FloatProp->SetPropertyValue(ValuePtr, (float)val.as<double>()); Applied++; }
				}
				else if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop))
				{
					if (val.is<double>()) { DoubleProp->SetPropertyValue(ValuePtr, val.as<double>()); Applied++; }
				}
				else if (FIntProperty* IntProp = CastField<FIntProperty>(Prop))
				{
					if (val.is<double>()) { IntProp->SetPropertyValue(ValuePtr, (int32)val.as<double>()); Applied++; }
				}
				else if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
				{
					if (val.is<bool>()) { BoolProp->SetPropertyValue(ValuePtr, val.as<bool>()); Applied++; }
				}
				else if (FStrProperty* StrProp = CastField<FStrProperty>(Prop))
				{
					if (val.is<std::string>()) { StrProp->SetPropertyValue(ValuePtr, FString(UTF8_TO_TCHAR(val.as<std::string>().c_str()))); Applied++; }
				}
				else if (FNameProperty* NameProp = CastField<FNameProperty>(Prop))
				{
					if (val.is<std::string>()) { NameProp->SetPropertyValue(ValuePtr, FName(UTF8_TO_TCHAR(val.as<std::string>().c_str()))); Applied++; }
				}
				else if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
				{
					if (val.is<std::string>() && EnumProp->GetEnum())
					{
						FString TextValue = UTF8_TO_TCHAR(val.as<std::string>().c_str());
						UEnum* Enum = EnumProp->GetEnum();
						int64 EnumVal = Enum->GetValueByNameString(TextValue);
						if (EnumVal == INDEX_NONE) EnumVal = Enum->GetValueByNameString(Enum->GetName() + TEXT("::") + TextValue);
						if (EnumVal != INDEX_NONE) { EnumProp->GetUnderlyingProperty()->SetIntPropertyValue(ValuePtr, EnumVal); Applied++; }
					}
				}
				else if (FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
				{
					if (val.is<std::string>() && ByteProp->GetIntPropertyEnum())
					{
						FString TextValue = UTF8_TO_TCHAR(val.as<std::string>().c_str());
						UEnum* Enum = ByteProp->GetIntPropertyEnum();
						int64 EnumVal = Enum->GetValueByNameString(TextValue);
						if (EnumVal == INDEX_NONE) EnumVal = Enum->GetValueByNameString(Enum->GetName() + TEXT("::") + TextValue);
						if (EnumVal != INDEX_NONE) { ByteProp->SetIntPropertyValue(ValuePtr, EnumVal); Applied++; }
					}
					else if (val.is<double>() && !ByteProp->GetIntPropertyEnum())
					{
						ByteProp->SetPropertyValue(ValuePtr, (uint8)val.as<double>());
						Applied++;
					}
				}
			}

			Ctrl->BroadcastNeedsReinitialized();
			Ctrl->GetAsset()->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] configure_solver(%d) -> %d properties set"), SolverIndex, Applied));
			return sol::make_object(Lua, Applied);
#else
			Session.Log(TEXT("[FAIL] configure_solver -> solver configuration requires UE 5.6+"));
			return sol::lua_nil;
#endif // ENGINE_MINOR_VERSION >= 6
		});

		// ---- exclude_bones / include_bones ----
		AssetObj.set_function("exclude_bones", [Ctrl, &Session](sol::table /*self*/,
			sol::table Bones, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			int32 Count = 0;
			for (auto& [_, val] : Bones)
			{
				if (!val.is<std::string>()) continue;
				FString BoneStr = UTF8_TO_TCHAR(val.as<std::string>().c_str());
				if (Ctrl->SetBoneExcluded(FName(BoneStr), true)) Count++;
			}
			if (Count > 0) Ctrl->GetAsset()->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] exclude_bones -> %d excluded"), Count));
			return sol::make_object(Lua, Count);
		});

		AssetObj.set_function("include_bones", [Ctrl, &Session](sol::table /*self*/,
			sol::table Bones, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			int32 Count = 0;
			for (auto& [_, val] : Bones)
			{
				if (!val.is<std::string>()) continue;
				FString BoneStr = UTF8_TO_TCHAR(val.as<std::string>().c_str());
				if (Ctrl->SetBoneExcluded(FName(BoneStr), false)) Count++;
			}
			if (Count > 0) Ctrl->GetAsset()->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] include_bones -> %d included"), Count));
			return sol::make_object(Lua, Count);
		});

		// ---- Bone settings ----
		AssetObj.set_function("add_bone_setting", [Ctrl, &Session](sol::table /*self*/,
			const std::string& BoneName, int SolverIndex, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString BoneStr = UTF8_TO_TCHAR(BoneName.c_str());
			if (SolverIndex < 0 || SolverIndex >= Ctrl->GetNumSolvers())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_bone_setting -> solver index %d out of range"), SolverIndex));
				return sol::lua_nil;
			}
			bool bOk = Ctrl->AddBoneSetting(FName(BoneStr), SolverIndex);
			Session.Log(FString::Printf(TEXT("[%s] add_bone_setting(\"%s\", %d)"), bOk ? TEXT("OK") : TEXT("FAIL"), *BoneStr, SolverIndex));
			return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
		});

		AssetObj.set_function("remove_bone_setting", [Ctrl, &Session](sol::table /*self*/,
			const std::string& BoneName, int SolverIndex, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString BoneStr = UTF8_TO_TCHAR(BoneName.c_str());
			if (SolverIndex < 0 || SolverIndex >= Ctrl->GetNumSolvers())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove_bone_setting -> solver index %d out of range"), SolverIndex));
				return sol::lua_nil;
			}
			bool bOk = Ctrl->RemoveBoneSetting(FName(BoneStr), SolverIndex);
			Session.Log(FString::Printf(TEXT("[%s] remove_bone_setting(\"%s\", %d)"), bOk ? TEXT("OK") : TEXT("FAIL"), *BoneStr, SolverIndex));
			return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
		});

		AssetObj.set_function("get_bone_settings", [Ctrl, &Session](sol::table /*self*/,
			const std::string& BoneName, int SolverIndex, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			FString BoneStr = UTF8_TO_TCHAR(BoneName.c_str());
			if (SolverIndex < 0 || SolverIndex >= Ctrl->GetNumSolvers())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] get_bone_settings -> solver index %d out of range"), SolverIndex));
				return sol::lua_nil;
			}

			FIKRigSolverBase* Solver = Ctrl->GetSolverAtIndex(SolverIndex);
			if (!Solver || !Solver->UsesCustomBoneSettings())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] get_bone_settings -> solver %d does not support bone settings"), SolverIndex));
				return sol::lua_nil;
			}

			FIKRigBoneSettingsBase* BoneSettings = Solver->GetBoneSettings(FName(BoneStr));
			if (!BoneSettings)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] get_bone_settings -> no settings on bone '%s' for solver %d"), *BoneStr, SolverIndex));
				return sol::lua_nil;
			}

			const UScriptStruct* SettingsType = Solver->GetBoneSettingsType();
			if (!SettingsType)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] get_bone_settings -> solver %d has no bone settings type"), SolverIndex));
				return sol::lua_nil;
			}

			sol::table Result = Lua.create_table();
			for (TFieldIterator<FProperty> It(SettingsType); It; ++It)
			{
				FProperty* Prop = *It;
				void* ValPtr = Prop->ContainerPtrToValuePtr<void>(BoneSettings);
				FString Key = Prop->GetName();
				std::string KeyStr = TCHAR_TO_UTF8(*Key);

				if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
				{
					Result[KeyStr] = FloatProp->GetPropertyValue(ValPtr);
				}
				else if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop))
				{
					Result[KeyStr] = DoubleProp->GetPropertyValue(ValPtr);
				}
				else if (FIntProperty* IntProp = CastField<FIntProperty>(Prop))
				{
					Result[KeyStr] = IntProp->GetPropertyValue(ValPtr);
				}
				else if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
				{
					Result[KeyStr] = BoolProp->GetPropertyValue(ValPtr);
				}
				else if (FStrProperty* StrProp = CastField<FStrProperty>(Prop))
				{
					Result[KeyStr] = TCHAR_TO_UTF8(*StrProp->GetPropertyValue(ValPtr));
				}
				else if (FNameProperty* NameProp = CastField<FNameProperty>(Prop))
				{
					Result[KeyStr] = TCHAR_TO_UTF8(*NameProp->GetPropertyValue(ValPtr).ToString());
				}
				else if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
				{
					if (UEnum* Enum = EnumProp->GetEnum())
					{
						FNumericProperty* UnderlyingProp = EnumProp->GetUnderlyingProperty();
						int64 EnumValue = UnderlyingProp->GetSignedIntPropertyValue(ValPtr);
						Result[KeyStr] = TCHAR_TO_UTF8(*Enum->GetNameStringByValue(EnumValue));
					}
				}
				else if (FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
				{
					if (UEnum* Enum = ByteProp->GetIntPropertyEnum())
					{
						uint8 ByteValue = ByteProp->GetPropertyValue(ValPtr);
						Result[KeyStr] = TCHAR_TO_UTF8(*Enum->GetNameStringByValue(ByteValue));
					}
					else
					{
						Result[KeyStr] = (int)ByteProp->GetPropertyValue(ValPtr);
					}
				}
			}

			Session.Log(FString::Printf(TEXT("[OK] get_bone_settings(\"%s\", %d)"), *BoneStr, SolverIndex));
			return Result;
#else
			Session.Log(TEXT("[FAIL] get_bone_settings -> solver bone settings require UE 5.6+"));
			return sol::lua_nil;
#endif // ENGINE_MINOR_VERSION >= 6
		});

		AssetObj.set_function("set_bone_settings", [Ctrl, &Session](sol::table /*self*/,
			const std::string& BoneName, int SolverIndex, sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			FString BoneStr = UTF8_TO_TCHAR(BoneName.c_str());
			if (SolverIndex < 0 || SolverIndex >= Ctrl->GetNumSolvers())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_bone_settings -> solver index %d out of range"), SolverIndex));
				return sol::lua_nil;
			}

			FIKRigSolverBase* Solver = Ctrl->GetSolverAtIndex(SolverIndex);
			if (!Solver || !Solver->UsesCustomBoneSettings())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_bone_settings -> solver %d does not support bone settings"), SolverIndex));
				return sol::lua_nil;
			}

			FIKRigBoneSettingsBase* BoneSettings = Solver->GetBoneSettings(FName(BoneStr));
			if (!BoneSettings)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_bone_settings -> no settings on bone '%s' for solver %d (add_bone_setting first)"), *BoneStr, SolverIndex));
				return sol::lua_nil;
			}

			const UScriptStruct* SettingsType = Solver->GetBoneSettingsType();
			if (!SettingsType)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_bone_settings -> solver %d has no bone settings type"), SolverIndex));
				return sol::lua_nil;
			}

			int32 Applied = 0;
			for (auto& [key, val] : Params)
			{
				if (!key.is<std::string>()) continue;
				FString PropName = UTF8_TO_TCHAR(key.as<std::string>().c_str());
				FProperty* Prop = SettingsType->FindPropertyByName(FName(PropName));
				if (!Prop) continue;
				if (Prop->HasAnyPropertyFlags(CPF_BlueprintReadOnly | CPF_EditConst)) continue;

				void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(BoneSettings);

				if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
				{
					if (val.is<double>()) { FloatProp->SetPropertyValue(ValuePtr, (float)val.as<double>()); Applied++; }
				}
				else if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop))
				{
					if (val.is<double>()) { DoubleProp->SetPropertyValue(ValuePtr, val.as<double>()); Applied++; }
				}
				else if (FIntProperty* IntProp = CastField<FIntProperty>(Prop))
				{
					if (val.is<double>()) { IntProp->SetPropertyValue(ValuePtr, (int32)val.as<double>()); Applied++; }
				}
				else if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
				{
					if (val.is<bool>()) { BoolProp->SetPropertyValue(ValuePtr, val.as<bool>()); Applied++; }
				}
				else if (FStrProperty* StrProp = CastField<FStrProperty>(Prop))
				{
					if (val.is<std::string>()) { StrProp->SetPropertyValue(ValuePtr, FString(UTF8_TO_TCHAR(val.as<std::string>().c_str()))); Applied++; }
				}
				else if (FNameProperty* NameProp = CastField<FNameProperty>(Prop))
				{
					if (val.is<std::string>()) { NameProp->SetPropertyValue(ValuePtr, FName(UTF8_TO_TCHAR(val.as<std::string>().c_str()))); Applied++; }
				}
				else if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
				{
					if (val.is<std::string>() && EnumProp->GetEnum())
					{
						FString TextValue = UTF8_TO_TCHAR(val.as<std::string>().c_str());
						UEnum* Enum = EnumProp->GetEnum();
						int64 EnumVal = Enum->GetValueByNameString(TextValue);
						if (EnumVal == INDEX_NONE) EnumVal = Enum->GetValueByNameString(Enum->GetName() + TEXT("::") + TextValue);
						if (EnumVal != INDEX_NONE) { EnumProp->GetUnderlyingProperty()->SetIntPropertyValue(ValuePtr, EnumVal); Applied++; }
					}
				}
				else if (FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
				{
					if (val.is<std::string>() && ByteProp->GetIntPropertyEnum())
					{
						FString TextValue = UTF8_TO_TCHAR(val.as<std::string>().c_str());
						UEnum* Enum = ByteProp->GetIntPropertyEnum();
						int64 EnumVal = Enum->GetValueByNameString(TextValue);
						if (EnumVal == INDEX_NONE) EnumVal = Enum->GetValueByNameString(Enum->GetName() + TEXT("::") + TextValue);
						if (EnumVal != INDEX_NONE) { ByteProp->SetIntPropertyValue(ValuePtr, EnumVal); Applied++; }
					}
					else if (val.is<double>() && !ByteProp->GetIntPropertyEnum())
					{
						ByteProp->SetPropertyValue(ValuePtr, (uint8)val.as<double>());
						Applied++;
					}
				}
			}

			Ctrl->BroadcastNeedsReinitialized();
			Ctrl->GetAsset()->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] set_bone_settings(\"%s\", %d) -> %d properties set"), *BoneStr, SolverIndex, Applied));
			return sol::make_object(Lua, Applied);
#else
			Session.Log(TEXT("[FAIL] set_bone_settings -> solver bone settings require UE 5.6+"));
			return sol::lua_nil;
#endif // ENGINE_MINOR_VERSION >= 6
		});

		// ---- info() — override default ----
		AssetObj.set_function("info", [Ctrl, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			sol::table Result = Lua.create_table();
			Result["solvers"] = Ctrl->GetNumSolvers();
			Result["goals"] = static_cast<int>(Ctrl->GetAllGoals().Num());
			Result["chains"] = static_cast<int>(Ctrl->GetRetargetChains().Num());
			Result["retarget_root"] = TCHAR_TO_UTF8(*Ctrl->GetRetargetRoot().ToString());
			USkeletalMesh* Mesh = Ctrl->GetSkeletalMesh();
			Result["mesh"] = Mesh ? TCHAR_TO_UTF8(*Mesh->GetName()) : "none";
			Session.Log(FString::Printf(TEXT("[OK] info() -> %d solvers, %d goals, %d chains"),
				Ctrl->GetNumSolvers(), Ctrl->GetAllGoals().Num(), Ctrl->GetRetargetChains().Num()));
			return Result;
		});
	});
}

REGISTER_LUA_BINDING(IKRig, IKRigDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("IKRig")))
	{
		Session.Log(TEXT("[WARN] IKRig plugin is not loaded. Enable it in Edit > Plugins to use this feature."));
		return;
	}
	BindIKRig(Lua, Session);
});
