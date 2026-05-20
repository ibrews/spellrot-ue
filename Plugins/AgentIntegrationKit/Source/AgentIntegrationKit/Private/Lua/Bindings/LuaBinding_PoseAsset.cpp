// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Tools/NeoStackToolUtils.h"
#include "ScopedTransaction.h"

#include "Animation/PoseAsset.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// DOCS
// ============================================================================

static TArray<FLuaFunctionDoc> PoseAssetDocs = {};

// ============================================================================
// BINDING
// ============================================================================

static void BindPoseAsset(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_pose_asset", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		UPoseAsset* PA = LoadObject<UPoseAsset>(nullptr, *FPath);
		if (!PA) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"Element types for add/remove/list/configure:\n"
			"  pose           — named pose (facial shape key / body pose)\n"
			"\n"
			"add(type, params):\n"
			"  add(\"pose\", {name=\"Smile\", source_animation=\"/Game/Anim\"})\n"
			"    — Creates poses from a source animation. If name is given, uses that as\n"
			"      the pose name. Updates the source animation reference on the asset.\n"
			"\n"
			"remove(type, id):\n"
			"  remove(\"pose\", \"PoseName\")           — remove pose by name\n"
			"  remove(\"pose\", {\"Pose1\", \"Pose2\"})   — remove multiple poses\n"
			"  remove(\"curve\", \"CurveName\")          — remove curve by name\n"
			"  remove(\"curve\", {\"C1\", \"C2\"})         — remove multiple curves\n"
			"\n"
			"list(type):\n"
			"  list(\"poses\")      — array of {name, index, is_base_pose, curve_values}\n"
			"  list(\"curves\")     — array of {name, index}\n"
			"  list(\"tracks\")     — array of bone track names\n"
			"  list(\"transforms\") — array of {pose_name, pose_index, bones=[{name, transform}]}\n"
			"    Returns full local-space bone transforms for every pose.\n"
			"\n"
			"configure(type, name_or_opts, opts?):\n"
			"  configure(\"pose\", \"OldName\", {new_name=\"NewName\"})  — rename pose\n"
			"  configure(\"base_pose\", \"PoseName\")       — set base pose for additive\n"
			"  configure(\"base_pose\", \"\")               — use reference pose as base\n"
			"  configure(\"additive\", true/false)         — convert to/from additive\n"
			"  configure(\"source_animation\", \"/Game/Anim\") — set source anim + regenerate\n"
			"  configure(\"retarget_source\", \"SourceName\") — set retarget source name\n"
			"  configure(\"retarget_source\", \"\")           — clear retarget source name\n"
			"  configure(\"retarget_source_asset\", \"/Game/Mesh\") — set retarget source mesh\n"
			"  configure(\"retarget_source_asset\", \"\")    — clear retarget source mesh\n"
			"\n"
			"Action methods:\n"
			"  info() — summary of pose count, skeleton, source animation, additive mode\n";

		// ================================================================
		// info()
		// ================================================================
		AssetObj.set_function("info", [PA, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			sol::table Result = Lua.create_table();

			Result["type"] = "PoseAsset";
			Result["name"] = TCHAR_TO_UTF8(*PA->GetName());
			Result["path"] = TCHAR_TO_UTF8(*PA->GetPathName());
			Result["pose_count"] = PA->GetNumPoses();
			Result["curve_count"] = PA->GetNumCurves();
			Result["track_count"] = PA->GetNumTracks();
			Result["is_additive"] = PA->IsValidAdditive();
			Result["base_pose_index"] = PA->GetBasePoseIndex();

#if WITH_EDITOR
			// GetBasePoseName() is not exported; replicate via GetPoseNameByIndex(GetBasePoseIndex())
			FName BasePoseName = PA->GetPoseNameByIndex(PA->GetBasePoseIndex());
			Result["base_pose_name"] = BasePoseName.IsNone()
				? "reference_pose"
				: TCHAR_TO_UTF8(*BasePoseName.ToString());
#endif

			// Skeleton
			if (USkeleton* Skel = PA->GetSkeleton())
			{
				Result["skeleton"] = TCHAR_TO_UTF8(*Skel->GetPathName());
				Result["skeleton_name"] = TCHAR_TO_UTF8(*Skel->GetName());
			}

			// Source animation
#if WITH_EDITORONLY_DATA
			if (PA->SourceAnimation)
			{
				Result["source_animation"] = TCHAR_TO_UTF8(*PA->SourceAnimation->GetPathName());
				Result["source_animation_name"] = TCHAR_TO_UTF8(*PA->SourceAnimation->GetName());
			}
#endif

			// Retarget source
			if (!PA->RetargetSource.IsNone())
			{
				Result["retarget_source"] = TCHAR_TO_UTF8(*PA->RetargetSource.ToString());
			}

			// Retarget source asset
#if WITH_EDITOR && ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
			{
				const TSoftObjectPtr<USkeletalMesh>& RetargetAsset = PA->GetRetargetSourceAsset();
				if (!RetargetAsset.IsNull())
				{
					Result["retarget_source_asset"] = TCHAR_TO_UTF8(*RetargetAsset.ToString());
				}
			}
#endif

			// Pose names summary
			const TArray<FName>& PoseNames = PA->GetPoseFNames();
			sol::table NamesArr = Lua.create_table();
			for (int32 i = 0; i < PoseNames.Num(); ++i)
			{
				NamesArr[i + 1] = TCHAR_TO_UTF8(*PoseNames[i].ToString());
			}
			Result["pose_names"] = NamesArr;

			Session.Log(FString::Printf(TEXT("[OK] info() -> PoseAsset '%s', %d poses, additive=%s"),
				*PA->GetName(), PA->GetNumPoses(),
				PA->IsValidAdditive() ? TEXT("true") : TEXT("false")));
			return Result;
		});

		// ================================================================
		// list(type)
		// ================================================================
		AssetObj.set_function("list", [PA, &Session](sol::table /*self*/,
			const std::string& Type, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (FType.Equals(TEXT("poses"), ESearchCase::IgnoreCase))
			{
				const TArray<FName>& PoseNames = PA->GetPoseFNames();
				sol::table Arr = Lua.create_table();
				int32 BasePoseIdx = PA->GetBasePoseIndex();

				for (int32 i = 0; i < PoseNames.Num(); ++i)
				{
					sol::table Entry = Lua.create_table();
					Entry["name"] = TCHAR_TO_UTF8(*PoseNames[i].ToString());
					Entry["index"] = i + 1; // 1-based for Lua
					Entry["is_base_pose"] = (i == BasePoseIdx);

					// Curve values for this pose
					TArray<float> CurveVals = PA->GetCurveValues(i);
					if (CurveVals.Num() > 0)
					{
						sol::table Curves = Lua.create_table();
						const TArray<FName> CurveNames = PA->GetCurveFNames();
						for (int32 c = 0; c < FMath::Min(CurveVals.Num(), CurveNames.Num()); ++c)
						{
							Curves[TCHAR_TO_UTF8(*CurveNames[c].ToString())] = CurveVals[c];
						}
						Entry["curve_values"] = Curves;
					}

					Arr[i + 1] = Entry;
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"poses\") -> %d poses"), PoseNames.Num()));
				return Arr;
			}
			else if (FType.Equals(TEXT("curves"), ESearchCase::IgnoreCase))
			{
				const TArray<FName> CurveNames = PA->GetCurveFNames();
				sol::table Arr = Lua.create_table();
				for (int32 i = 0; i < CurveNames.Num(); ++i)
				{
					sol::table Entry = Lua.create_table();
					Entry["name"] = TCHAR_TO_UTF8(*CurveNames[i].ToString());
					Entry["index"] = i + 1;
					Arr[i + 1] = Entry;
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"curves\") -> %d curves"), CurveNames.Num()));
				return Arr;
			}
			else if (FType.Equals(TEXT("tracks"), ESearchCase::IgnoreCase))
			{
				const TArray<FName>& TrackNames = PA->GetTrackNames();
				sol::table Arr = Lua.create_table();
				for (int32 i = 0; i < TrackNames.Num(); ++i)
				{
					sol::table Entry = Lua.create_table();
					Entry["name"] = TCHAR_TO_UTF8(*TrackNames[i].ToString());
					Entry["index"] = i + 1;
					Arr[i + 1] = Entry;
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"tracks\") -> %d tracks"), TrackNames.Num()));
				return Arr;
			}
#if WITH_EDITOR
			else if (FType.Equals(TEXT("transforms"), ESearchCase::IgnoreCase))
			{
				const TArray<FName>& PoseNames = PA->GetPoseFNames();
				const TArray<FName>& TrackNames = PA->GetTrackNames();
				sol::table Arr = Lua.create_table();

				for (int32 i = 0; i < PoseNames.Num(); ++i)
				{
					TArray<FTransform> Transforms;
					if (!PA->GetFullPose(i, Transforms))
					{
						continue;
					}

					sol::table PoseEntry = Lua.create_table();
					PoseEntry["pose_name"] = TCHAR_TO_UTF8(*PoseNames[i].ToString());
					PoseEntry["pose_index"] = i + 1;

					sol::table Bones = Lua.create_table();
					for (int32 t = 0; t < FMath::Min(Transforms.Num(), TrackNames.Num()); ++t)
					{
						sol::table Bone = Lua.create_table();
						Bone["name"] = TCHAR_TO_UTF8(*TrackNames[t].ToString());

						const FTransform& Xform = Transforms[t];
						sol::table XformTbl = Lua.create_table();

						FVector Loc = Xform.GetLocation();
						XformTbl["location_x"] = Loc.X;
						XformTbl["location_y"] = Loc.Y;
						XformTbl["location_z"] = Loc.Z;

						FRotator Rot = Xform.Rotator();
						XformTbl["rotation_pitch"] = Rot.Pitch;
						XformTbl["rotation_yaw"] = Rot.Yaw;
						XformTbl["rotation_roll"] = Rot.Roll;

						FVector Scale = Xform.GetScale3D();
						XformTbl["scale_x"] = Scale.X;
						XformTbl["scale_y"] = Scale.Y;
						XformTbl["scale_z"] = Scale.Z;

						Bone["transform"] = XformTbl;
						Bones[t + 1] = Bone;
					}

					PoseEntry["bones"] = Bones;
					Arr[i + 1] = PoseEntry;
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"transforms\") -> %d poses, %d tracks"),
					PoseNames.Num(), TrackNames.Num()));
				return Arr;
			}
#endif // WITH_EDITOR

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Use: poses, curves, tracks, transforms"),
				UTF8_TO_TCHAR(Type.c_str())));
			return sol::lua_nil;
		});

#if WITH_EDITOR
		// ================================================================
		// add(type, params)
		// ================================================================
		AssetObj.set_function("add", [PA, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (FType.Equals(TEXT("pose"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value())
				{
					Session.Log(TEXT("[FAIL] add(\"pose\") -> params required: {source_animation=\"/Game/Anim\"}"));
					return sol::lua_nil;
				}
				sol::table P = Params.value();

				// Source animation is required to create poses from
				std::string AnimPath = P.get_or<std::string>("source_animation", "");
				if (AnimPath.empty())
				{
					Session.Log(TEXT("[FAIL] add(\"pose\") -> 'source_animation' required"));
					return sol::lua_nil;
				}

				FString FAnimPath = UTF8_TO_TCHAR(AnimPath.c_str());
				UAnimSequence* SourceAnim = NeoStackToolUtils::LoadAssetWithFallback<UAnimSequence>(FAnimPath);
				if (!SourceAnim && !FAnimPath.StartsWith(TEXT("/")))
				{
					SourceAnim = NeoStackToolUtils::LoadAssetWithFallback<UAnimSequence>(TEXT("/Game/") + FAnimPath);
				}
				if (!SourceAnim)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"pose\") -> source animation not found: %s"), *FAnimPath));
					return sol::lua_nil;
				}

				// Optional: specific pose names to create
				TArray<FName> PoseNames;
				std::string NameStr = P.get_or<std::string>("name", "");
				if (!NameStr.empty())
				{
					PoseNames.Add(FName(UTF8_TO_TCHAR(NameStr.c_str())));
				}

				// Check for names array
				sol::optional<sol::table> NamesOpt = P.get<sol::optional<sol::table>>("names");
				if (NamesOpt.has_value())
				{
					sol::table NamesTable = NamesOpt.value();
					for (auto& kv : NamesTable)
					{
						if (kv.second.is<std::string>())
						{
							PoseNames.Add(FName(UTF8_TO_TCHAR(kv.second.as<std::string>().c_str())));
						}
					}
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("PoseAsset: Add Poses from Animation")));
				PA->Modify();

				// CreatePoseFromAnimation sets SourceAnimation internally
				if (PoseNames.Num() > 0)
				{
					PA->CreatePoseFromAnimation(SourceAnim, &PoseNames);
				}
				else
				{
					PA->CreatePoseFromAnimation(SourceAnim, static_cast<const TArray<FName>*>(nullptr));
				}

				PA->PostEditChange();
				PA->MarkPackageDirty();

				int32 NewCount = PA->GetNumPoses();
				Session.Log(FString::Printf(TEXT("[OK] add(\"pose\") -> created poses from '%s', total poses: %d"),
					*SourceAnim->GetName(), NewCount));

				sol::table Result = Lua.create_table();
				Result["pose_count"] = NewCount;
				Result["source_animation"] = TCHAR_TO_UTF8(*SourceAnim->GetPathName());
				return Result;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Use: pose"),
				UTF8_TO_TCHAR(Type.c_str())));
			return sol::lua_nil;
		});

		// ================================================================
		// remove(type, id)
		// ================================================================
		AssetObj.set_function("remove", [PA, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (FType.Equals(TEXT("pose"), ESearchCase::IgnoreCase))
			{
				TArray<FName> NamesToRemove;

				if (Id.is<std::string>())
				{
					NamesToRemove.Add(FName(UTF8_TO_TCHAR(Id.as<std::string>().c_str())));
				}
				else if (Id.is<sol::table>())
				{
					sol::table Tbl = Id.as<sol::table>();
					for (auto& kv : Tbl)
					{
						if (kv.second.is<std::string>())
						{
							NamesToRemove.Add(FName(UTF8_TO_TCHAR(kv.second.as<std::string>().c_str())));
						}
					}
				}
				else
				{
					Session.Log(TEXT("[FAIL] remove(\"pose\") -> provide pose name (string) or array of names"));
					return sol::lua_nil;
				}

				if (NamesToRemove.Num() == 0)
				{
					Session.Log(TEXT("[FAIL] remove(\"pose\") -> no valid names provided"));
					return sol::lua_nil;
				}

				// Verify poses exist
				for (const FName& Name : NamesToRemove)
				{
					if (!PA->ContainsPose(Name))
					{
						Session.Log(FString::Printf(TEXT("[FAIL] remove(\"pose\") -> pose '%s' not found"),
							*Name.ToString()));
						return sol::lua_nil;
					}
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("PoseAsset: Delete Poses")));
				PA->Modify();

				int32 Removed = PA->DeletePoses(NamesToRemove);

				PA->PostEditChange();
				PA->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] remove(\"pose\") -> removed %d pose(s), %d remaining"),
					Removed, PA->GetNumPoses()));

				sol::table Result = Lua.create_table();
				Result["removed"] = Removed;
				Result["remaining"] = PA->GetNumPoses();
				return Result;
			}
			else if (FType.Equals(TEXT("curve"), ESearchCase::IgnoreCase))
			{
				TArray<FName> NamesToRemove;

				if (Id.is<std::string>())
				{
					NamesToRemove.Add(FName(UTF8_TO_TCHAR(Id.as<std::string>().c_str())));
				}
				else if (Id.is<sol::table>())
				{
					sol::table Tbl = Id.as<sol::table>();
					for (auto& kv : Tbl)
					{
						if (kv.second.is<std::string>())
						{
							NamesToRemove.Add(FName(UTF8_TO_TCHAR(kv.second.as<std::string>().c_str())));
						}
					}
				}
				else
				{
					Session.Log(TEXT("[FAIL] remove(\"curve\") -> provide curve name (string) or array of names"));
					return sol::lua_nil;
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("PoseAsset: Delete Curves")));
				PA->Modify();

				int32 Removed = PA->DeleteCurves(NamesToRemove);

				PA->PostEditChange();
				PA->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] remove(\"curve\") -> removed %d curve(s)"), Removed));

				sol::table Result = Lua.create_table();
				Result["removed"] = Removed;
				return Result;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type. Use: pose, curve"),
				UTF8_TO_TCHAR(Type.c_str())));
			return sol::lua_nil;
		});

		// ================================================================
		// configure(type, name_or_val, opts?)
		// ================================================================
		AssetObj.set_function("configure", [PA, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Arg2, sol::optional<sol::table> Arg3, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			// configure("pose", "OldName", {new_name="NewName"})
			if (FType.Equals(TEXT("pose"), ESearchCase::IgnoreCase))
			{
				if (!Arg2.is<std::string>())
				{
					Session.Log(TEXT("[FAIL] configure(\"pose\") -> second arg must be pose name (string)"));
					return sol::lua_nil;
				}
				FName OldName = FName(UTF8_TO_TCHAR(Arg2.as<std::string>().c_str()));

				if (!PA->ContainsPose(OldName))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"pose\", \"%s\") -> pose not found"),
						*OldName.ToString()));
					return sol::lua_nil;
				}

				if (!Arg3.has_value())
				{
					Session.Log(TEXT("[FAIL] configure(\"pose\") -> third arg required: {new_name=\"...\"}"));
					return sol::lua_nil;
				}
				sol::table Opts = Arg3.value();

				std::string NewNameStr = Opts.get_or<std::string>("new_name", "");
				if (NewNameStr.empty())
				{
					Session.Log(TEXT("[FAIL] configure(\"pose\") -> 'new_name' required in options"));
					return sol::lua_nil;
				}

				FName NewName = FName(UTF8_TO_TCHAR(NewNameStr.c_str()));

				// Check for collision
				if (PA->ContainsPose(NewName))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"pose\") -> pose '%s' already exists"),
						*NewName.ToString()));
					return sol::lua_nil;
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("PoseAsset: Rename Pose")));
				PA->Modify();

				// RenamePose() is not exported; use ModifyPoseName() which is ENGINE_API
				if (!PA->ModifyPoseName(OldName, NewName))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"pose\") -> failed to rename '%s'"),
						*OldName.ToString()));
					return sol::lua_nil;
				}

				PA->PostEditChange();
				PA->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"pose\", \"%s\") -> renamed to '%s'"),
					*OldName.ToString(), *NewName.ToString()));

				sol::table Result = Lua.create_table();
				Result["old_name"] = TCHAR_TO_UTF8(*OldName.ToString());
				Result["new_name"] = TCHAR_TO_UTF8(*NewName.ToString());
				return Result;
			}
			// configure("base_pose", "PoseName") or configure("base_pose", "")
			else if (FType.Equals(TEXT("base_pose"), ESearchCase::IgnoreCase))
			{
				if (!Arg2.is<std::string>())
				{
					Session.Log(TEXT("[FAIL] configure(\"base_pose\") -> second arg must be pose name (string) or empty string for reference pose"));
					return sol::lua_nil;
				}

				FName BasePoseName = FName(UTF8_TO_TCHAR(Arg2.as<std::string>().c_str()));

				int32 NewBasePoseIndex = INDEX_NONE;
				if (!BasePoseName.IsNone())
				{
					NewBasePoseIndex = PA->GetPoseIndexByName(BasePoseName);
					if (NewBasePoseIndex == INDEX_NONE)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] configure(\"base_pose\", \"%s\") -> pose not found"),
							*BasePoseName.ToString()));
						return sol::lua_nil;
					}
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("PoseAsset: Set Base Pose")));
				PA->Modify();

				// ConvertSpace sets BasePoseIndex and calls PostProcessData() to recompute
				// additive deltas — essential when the asset is additive
				PA->ConvertSpace(PA->IsValidAdditive(), NewBasePoseIndex);

				PA->PostEditChange();
				PA->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"base_pose\") -> set to '%s'"),
					BasePoseName.IsNone() ? TEXT("reference_pose") : *BasePoseName.ToString()));

				sol::table Result = Lua.create_table();
				Result["base_pose"] = BasePoseName.IsNone()
					? "reference_pose"
					: TCHAR_TO_UTF8(*BasePoseName.ToString());
				return Result;
			}
			// configure("additive", true/false)
			else if (FType.Equals(TEXT("additive"), ESearchCase::IgnoreCase))
			{
				if (!Arg2.is<bool>())
				{
					Session.Log(TEXT("[FAIL] configure(\"additive\") -> second arg must be true or false"));
					return sol::lua_nil;
				}

				bool bNewAdditive = Arg2.as<bool>();
				bool bCurrentAdditive = PA->IsValidAdditive();

				if (bNewAdditive == bCurrentAdditive)
				{
					Session.Log(FString::Printf(TEXT("[OK] configure(\"additive\") -> already %s"),
						bNewAdditive ? TEXT("additive") : TEXT("full pose")));
					sol::table Result = Lua.create_table();
					Result["additive"] = bNewAdditive;
					return Result;
				}

				int32 BasePoseIdx = PA->GetBasePoseIndex();

				const FScopedTransaction Transaction(FText::FromString(TEXT("PoseAsset: Convert Space")));
				PA->Modify();

				bool bSuccess = PA->ConvertSpace(bNewAdditive, BasePoseIdx);
				if (!bSuccess)
				{
					Session.Log(TEXT("[FAIL] configure(\"additive\") -> conversion failed"));
					return sol::lua_nil;
				}

				PA->PostEditChange();
				PA->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"additive\") -> converted to %s"),
					bNewAdditive ? TEXT("additive") : TEXT("full pose")));

				sol::table Result = Lua.create_table();
				Result["additive"] = bNewAdditive;
				return Result;
			}
			// configure("source_animation", "/Game/Anim")
			else if (FType.Equals(TEXT("source_animation"), ESearchCase::IgnoreCase))
			{
				if (!Arg2.is<std::string>())
				{
					Session.Log(TEXT("[FAIL] configure(\"source_animation\") -> second arg must be animation path (string)"));
					return sol::lua_nil;
				}

				FString FAnimPath = UTF8_TO_TCHAR(Arg2.as<std::string>().c_str());
				UAnimSequence* SourceAnim = NeoStackToolUtils::LoadAssetWithFallback<UAnimSequence>(FAnimPath);
				if (!SourceAnim && !FAnimPath.StartsWith(TEXT("/")))
				{
					SourceAnim = NeoStackToolUtils::LoadAssetWithFallback<UAnimSequence>(TEXT("/Game/") + FAnimPath);
				}
				if (!SourceAnim)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"source_animation\") -> animation not found: %s"), *FAnimPath));
					return sol::lua_nil;
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("PoseAsset: Update Source Animation")));
				PA->Modify();

				PA->UpdatePoseFromAnimation(SourceAnim);

				PA->PostEditChange();
				PA->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"source_animation\") -> updated from '%s', %d poses"),
					*SourceAnim->GetName(), PA->GetNumPoses()));

				sol::table Result = Lua.create_table();
				Result["source_animation"] = TCHAR_TO_UTF8(*SourceAnim->GetPathName());
				Result["pose_count"] = PA->GetNumPoses();
				return Result;
			}

			// configure("retarget_source", "SourceName") or configure("retarget_source", "")
			else if (FType.Equals(TEXT("retarget_source"), ESearchCase::IgnoreCase))
			{
				if (!Arg2.is<std::string>())
				{
					Session.Log(TEXT("[FAIL] configure(\"retarget_source\") -> second arg must be source name (string) or empty to clear"));
					return sol::lua_nil;
				}

				FName NewSource = FName(UTF8_TO_TCHAR(Arg2.as<std::string>().c_str()));

				const FScopedTransaction Transaction(FText::FromString(TEXT("PoseAsset: Set Retarget Source")));
				PA->Modify();

				PA->RetargetSource = NewSource;

				PA->PostEditChange();
				PA->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"retarget_source\") -> set to '%s'"),
					NewSource.IsNone() ? TEXT("(none)") : *NewSource.ToString()));

				sol::table Result = Lua.create_table();
				Result["retarget_source"] = NewSource.IsNone()
					? ""
					: TCHAR_TO_UTF8(*NewSource.ToString());
				return Result;
			}
			// configure("retarget_source_asset", "/Game/Mesh") or configure("retarget_source_asset", "")
			else if (FType.Equals(TEXT("retarget_source_asset"), ESearchCase::IgnoreCase))
			{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
				if (!Arg2.is<std::string>())
				{
					Session.Log(TEXT("[FAIL] configure(\"retarget_source_asset\") -> second arg must be skeletal mesh path (string) or empty to clear"));
					return sol::lua_nil;
				}

				std::string MeshPath = Arg2.as<std::string>();

				const FScopedTransaction Transaction(FText::FromString(TEXT("PoseAsset: Set Retarget Source Asset")));
				PA->Modify();

				if (MeshPath.empty())
				{
					PA->ClearRetargetSourceAsset();

					Session.Log(TEXT("[OK] configure(\"retarget_source_asset\") -> cleared"));

					sol::table Result = Lua.create_table();
					Result["retarget_source_asset"] = "";
					return Result;
				}

				FString FMeshPath = UTF8_TO_TCHAR(MeshPath.c_str());
				USkeletalMesh* Mesh = NeoStackToolUtils::LoadAssetWithFallback<USkeletalMesh>(FMeshPath);
				if (!Mesh && !FMeshPath.StartsWith(TEXT("/")))
				{
					Mesh = NeoStackToolUtils::LoadAssetWithFallback<USkeletalMesh>(TEXT("/Game/") + FMeshPath);
				}
				if (!Mesh)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"retarget_source_asset\") -> skeletal mesh not found: %s"), *FMeshPath));
					return sol::lua_nil;
				}

				PA->SetRetargetSourceAsset(Mesh);
				PA->UpdateRetargetSourceAssetData();

				PA->PostEditChange();
				PA->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"retarget_source_asset\") -> set to '%s'"),
					*Mesh->GetName()));

				sol::table Result = Lua.create_table();
				Result["retarget_source_asset"] = TCHAR_TO_UTF8(*Mesh->GetPathName());
				return Result;
#else
				Session.Log(TEXT("[FAIL] configure(\"retarget_source_asset\") requires UE 5.5+"));
				return sol::lua_nil;
#endif
			}

			Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> unknown type. Use: pose, base_pose, additive, source_animation, retarget_source, retarget_source_asset"),
				UTF8_TO_TCHAR(Type.c_str())));
			return sol::lua_nil;
		});
#endif // WITH_EDITOR
	});
}

REGISTER_LUA_BINDING(PoseAsset, PoseAssetDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindPoseAsset(Lua, Session);
});
