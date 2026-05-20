#include "Lua/LuaBindingRegistry.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Animation/BlendProfile.h"
#include "ScopedTransaction.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

static TArray<FLuaFunctionDoc> SkeletonDocs = {};

static bool SupportsSkeletonNotifyNames()
{
#if WITH_EDITORONLY_DATA
	return true;
#else
	return false;
#endif
}

static bool SupportsSkeletonMarkerNames()
{
#if WITH_EDITOR
	return true;
#else
	return false;
#endif
}

static void CollectSkeletonNotifyNames(USkeleton* Skel, TArray<FName>& OutNames)
{
#if WITH_EDITORONLY_DATA
	Skel->CollectAnimationNotifies(OutNames);
#else
	(void)Skel;
	OutNames.Reset();
#endif
}

static void CollectSkeletonMarkerNames(USkeleton* Skel, TArray<FName>& OutNames)
{
#if WITH_EDITOR
	OutNames = Skel->GetExistingMarkerNames();
#else
	(void)Skel;
	OutNames.Reset();
#endif
}

static void AddSkeletonNotifyName(USkeleton* Skel, FName NotifyName)
{
#if WITH_EDITORONLY_DATA
	Skel->AddNewAnimationNotify(NotifyName);
#else
	(void)Skel;
	(void)NotifyName;
#endif
}

static void RemoveSkeletonNotifyName(USkeleton* Skel, FName NotifyName)
{
#if WITH_EDITORONLY_DATA
	Skel->RemoveAnimationNotify(NotifyName);
#else
	(void)Skel;
	(void)NotifyName;
#endif
}

static void RenameSkeletonNotifyName(USkeleton* Skel, FName OldName, FName NewName)
{
#if WITH_EDITORONLY_DATA
	Skel->RenameAnimationNotify(OldName, NewName);
#else
	(void)Skel;
	(void)OldName;
	(void)NewName;
#endif
}

static void AddSkeletonMarkerName(USkeleton* Skel, FName MarkerName)
{
#if WITH_EDITOR
	Skel->RegisterMarkerName(MarkerName);
#else
	(void)Skel;
	(void)MarkerName;
#endif
}

static bool RemoveSkeletonMarkerName(USkeleton* Skel, FName MarkerName)
{
#if WITH_EDITOR
	return Skel->RemoveMarkerName(MarkerName);
#else
	(void)Skel;
	(void)MarkerName;
	return false;
#endif
}

static bool RenameSkeletonMarkerName(USkeleton* Skel, FName OldName, FName NewName)
{
#if WITH_EDITOR
	return Skel->RenameMarkerName(OldName, NewName);
#else
	(void)Skel;
	(void)OldName;
	(void)NewName;
	return false;
#endif
}

static void BindSkeleton(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_skeleton", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		USkeleton* Skel = LoadObject<USkeleton>(nullptr, *FPath);
		if (!Skel) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"Element types for add/remove/list/configure:\n"
			"  socket, virtual_bone, blend_profile, slot, slot_group,\n"
			"  curve, notify_name, marker_name, compatible_skeleton\n"
			"\n"
			"add(type, params):\n"
			"  add(\"socket\", {name=\"Weapon\", bone=\"hand_r\", location={x,y,z}, rotation={p,y,r}, scale={x,y,z}, force_always_animated=true})\n"
			"  add(\"virtual_bone\", {source=\"hand_r\", target=\"hand_l\", name=\"Hands\"})  -- 'VB ' prefix auto-added\n"
			"  add(\"blend_profile\", {name=\"UpperBody\", mode=\"time_factor|weight_factor|blend_mask\"})\n"
			"  add(\"slot\", {name=\"DefaultSlot\"})\n"
			"  add(\"slot_group\", {name=\"MyGroup\"})\n"
			"  add(\"curve\", {name=\"MyCurve\", material=false, morph_target=true})\n"
			"  add(\"notify_name\", {name=\"FootstepL\"})\n"
			"  add(\"marker_name\", {name=\"FootSync\"})\n"
			"  add(\"compatible_skeleton\", {path=\"/Game/Skel\"})\n"
			"\n"
			"remove(type, id):\n"
			"  remove(\"socket\"), remove(\"virtual_bone\"), remove(\"blend_profile\")\n"
			"  remove(\"slot\"), remove(\"slot_group\"), remove(\"curve\")\n"
			"  remove(\"notify_name\"), remove(\"marker_name\")\n"
			"  remove(\"compatible_skeleton\", \"/Game/Skel\")\n"
			"  remove(\"bone\", {names={\"bone1\",\"bone2\"}, children=true})\n"
			"\n"
			"configure(type, id, params):\n"
			"  configure(\"socket\", \"Weapon\", {bone=\"hand_l\", location={0,5,0}, force_always_animated=true})\n"
			"  configure(\"blend_profile\", \"UpperBody\", {bone=\"spine_01\", scale=0.5, recurse=true})\n"
			"  configure(\"blend_profile\", \"Prof\", {mode=\"blend_mask\", rename_to=\"New\", remove_bone=\"spine_01\"})\n"
			"  configure(\"slot\", \"OldSlot\", {rename_to=\"NewSlot\"})\n"
			"  configure(\"slot_group\", \"GroupName\", {slots={\"slot1\",\"slot2\"}})\n"
			"  configure(\"virtual_bone\", \"VB Name\", {rename_to=\"VB New\", source=\"bone_a\", target=\"bone_b\"})\n"
			"  configure(\"curve\", \"CurveName\", {material=true, morph_target=false, rename_to=\"NewName\"})\n"
			"  configure(\"notify_name\", \"Old\", {rename_to=\"New\"})\n"
			"  configure(\"marker_name\", \"Old\", {rename_to=\"New\"})\n"
			"\n"
			"list(type):\n"
			"  list(\"sockets\"), list(\"bones\"), list(\"virtual_bones\"), list(\"blend_profiles\")\n"
			"  list(\"slot_groups\"), list(\"curves\"), list(\"notify_names\"), list(\"marker_names\")\n"
			"  list(\"compatible_skeletons\"), list(\"retarget_sources\")\n"
			"\n"
			"Action methods:\n"
			"  set_retargeting(bone, mode, children_too?) — mode: animation/skeleton/animation_scaled/animation_relative/orient_and_scale\n"
			"  get_bone_transform(bone) — ref pose transform {location, rotation, scale}\n"
			"  get_child_bones(bone) — direct children bone names\n"
			"  get_preview_mesh() — preview skeletal mesh path\n"
			"  set_preview_mesh(path) — set preview skeletal mesh\n"
			"  info() — summary of bone count, sockets, virtual bones, curves, notifies, markers, compatible skeletons, slot groups\n";

		// ---- add(type, params) ----
		AssetObj.set_function("add", [Skel, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (FType.Equals(TEXT("socket"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"socket\") -> {name=.., bone=..} required")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string Name = P.get_or<std::string>("name", "");
				std::string Bone = P.get_or<std::string>("bone", "");
				if (Name.empty() || Bone.empty()) { Session.Log(TEXT("[FAIL] add(\"socket\") -> name and bone required")); return sol::lua_nil; }

				FString SocketStr = UTF8_TO_TCHAR(Name.c_str());
				FName SocketName(SocketStr);
				FString BoneStr = UTF8_TO_TCHAR(Bone.c_str());
				FName BoneName(BoneStr);

				if (Skel->FindSocket(SocketName)) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"socket\") -> '%s' already exists"), *SocketStr)); return sol::lua_nil; }

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "AddSocket", "Add Socket"));
				Skel->Modify();
				USkeletalMeshSocket* Socket = NewObject<USkeletalMeshSocket>(Skel);
				Socket->SocketName = SocketName;
				Socket->BoneName = BoneName;

				sol::optional<sol::table> Loc = P.get<sol::optional<sol::table>>("location");
				if (Loc.has_value()) { sol::table L = Loc.value(); Socket->RelativeLocation = FVector(L.get_or(1, 0.0), L.get_or(2, 0.0), L.get_or(3, 0.0)); }
				sol::optional<sol::table> Rot = P.get<sol::optional<sol::table>>("rotation");
				if (Rot.has_value()) { sol::table R = Rot.value(); Socket->RelativeRotation = FRotator(R.get_or(1, 0.0), R.get_or(2, 0.0), R.get_or(3, 0.0)); }
				sol::optional<sol::table> Sc = P.get<sol::optional<sol::table>>("scale");
				if (Sc.has_value()) { sol::table V = Sc.value(); Socket->RelativeScale = FVector(V.get_or(1, 1.0), V.get_or(2, 1.0), V.get_or(3, 1.0)); }

				sol::optional<bool> ForceAnim = P.get<sol::optional<bool>>("force_always_animated");
				if (ForceAnim.has_value()) Socket->bForceAlwaysAnimated = ForceAnim.value();

				Skel->Sockets.Add(Socket);
				Skel->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add(\"socket\", name=\"%s\", bone=\"%s\")"), *SocketStr, *BoneStr));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("virtual_bone"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"virtual_bone\") -> {source=.., target=..} required")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string Source = P.get_or<std::string>("source", "");
				std::string Target = P.get_or<std::string>("target", "");
				if (Source.empty() || Target.empty()) { Session.Log(TEXT("[FAIL] add(\"virtual_bone\") -> source and target required")); return sol::lua_nil; }

				FString SrcStr = UTF8_TO_TCHAR(Source.c_str());
				FString TgtStr = UTF8_TO_TCHAR(Target.c_str());

				std::string VBName = P.get_or<std::string>("name", "");
				bool bOk;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
				if (!VBName.empty())
				{
					FString VBStr = UTF8_TO_TCHAR(VBName.c_str());
					// Auto-add required "VB " prefix if missing
					if (!VirtualBoneNameHelpers::CheckVirtualBonePrefix(VBStr))
					{
						VBStr = VirtualBoneNameHelpers::AddVirtualBonePrefix(VBStr);
					}
					bOk = Skel->AddNewNamedVirtualBone(FName(SrcStr), FName(TgtStr), FName(VBStr));
				}
				else
				{
					FName OutName;
					bOk = Skel->AddNewVirtualBone(FName(SrcStr), FName(TgtStr), OutName);
				}
#else
				{
					FName OutName;
					bOk = Skel->AddNewVirtualBone(FName(SrcStr), FName(TgtStr), OutName);
				}
#endif

				if (bOk) { Skel->MarkPackageDirty(); Session.Log(FString::Printf(TEXT("[OK] add(\"virtual_bone\", %s -> %s)"), *SrcStr, *TgtStr)); }
				else Session.Log(FString::Printf(TEXT("[FAIL] add(\"virtual_bone\") -> duplicate or invalid bones (%s -> %s)"), *SrcStr, *TgtStr));
				return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
			}
			else if (FType.Equals(TEXT("blend_profile"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"blend_profile\") -> {name=..} required")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string Name = P.get_or<std::string>("name", "");
				if (Name.empty()) { Session.Log(TEXT("[FAIL] add(\"blend_profile\") -> name required")); return sol::lua_nil; }

				FString ProfStr = UTF8_TO_TCHAR(Name.c_str());
				FName ProfName(ProfStr);
				if (Skel->GetBlendProfile(ProfName)) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"blend_profile\") -> '%s' already exists"), *ProfStr)); return sol::lua_nil; }

				Skel->Modify();
				UBlendProfile* Profile = Skel->CreateNewBlendProfile(ProfName);
				if (!Profile) { Session.Log(TEXT("[FAIL] add(\"blend_profile\")")); return sol::lua_nil; }

				sol::optional<std::string> ModeOpt = P.get<sol::optional<std::string>>("mode");
				if (ModeOpt.has_value())
				{
					FString ModeStr = UTF8_TO_TCHAR(ModeOpt.value().c_str());
					if (ModeStr.Equals(TEXT("weight_factor"), ESearchCase::IgnoreCase)) Profile->Mode = EBlendProfileMode::WeightFactor;
					else if (ModeStr.Equals(TEXT("blend_mask"), ESearchCase::IgnoreCase)) Profile->Mode = EBlendProfileMode::BlendMask;
					else Profile->Mode = EBlendProfileMode::TimeFactor;
				}

				Skel->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add(\"blend_profile\", name=\"%s\")"), *ProfStr));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("slot"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"slot\") -> {name=..} required")); return sol::lua_nil; }
				std::string Name = Params.value().get_or<std::string>("name", "");
				if (Name.empty()) { Session.Log(TEXT("[FAIL] add(\"slot\") -> name required")); return sol::lua_nil; }

				FString SlotStr = UTF8_TO_TCHAR(Name.c_str());
				Skel->Modify();
				bool bOk = Skel->RegisterSlotNode(FName(SlotStr));
				Skel->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[%s] add(\"slot\", name=\"%s\")"), bOk ? TEXT("OK") : TEXT("FAIL"), *SlotStr));
				return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
			}

			else if (FType.Equals(TEXT("slot_group"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"slot_group\") -> {name=..} required")); return sol::lua_nil; }
				std::string Name = Params.value().get_or<std::string>("name", "");
				if (Name.empty()) { Session.Log(TEXT("[FAIL] add(\"slot_group\") -> name required")); return sol::lua_nil; }

				FString GroupStr = UTF8_TO_TCHAR(Name.c_str());
				Skel->Modify();
				bool bOk = Skel->AddSlotGroupName(FName(GroupStr));
				Skel->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[%s] add(\"slot_group\", name=\"%s\")"), bOk ? TEXT("OK") : TEXT("FAIL"), *GroupStr));
				return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
			}
			else if (FType.Equals(TEXT("curve"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"curve\") -> {name=..} required")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string Name = P.get_or<std::string>("name", "");
				if (Name.empty()) { Session.Log(TEXT("[FAIL] add(\"curve\") -> name required")); return sol::lua_nil; }

				FString CurveStr = UTF8_TO_TCHAR(Name.c_str());
				FName CurveName(CurveStr);
				Skel->Modify();
				bool bOk = Skel->AddCurveMetaData(CurveName);
				if (!bOk) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"curve\", name=\"%s\") -> already exists or failed"), *CurveStr)); return sol::lua_nil; }

				bool bMaterial = P.get<sol::optional<bool>>("material").value_or(false);
				bool bMorphTarget = P.get<sol::optional<bool>>("morph_target").value_or(false);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
				if (bMaterial) Skel->SetCurveMetaDataMaterial(CurveName, true);
				if (bMorphTarget) Skel->SetCurveMetaDataMorphTarget(CurveName, true);
#else
				if (bMaterial || bMorphTarget)
				{
					Session.Log(TEXT("[WARN] add(\"curve\") -> material/morph_target flags require UE 5.7+, ignored"));
				}
#endif

				Skel->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add(\"curve\", name=\"%s\")"), *CurveStr));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("notify_name"), ESearchCase::IgnoreCase))
			{
				if (!SupportsSkeletonNotifyNames()) { Session.Log(TEXT("[FAIL] add(\"notify_name\") -> not available in this build")); return sol::lua_nil; }
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"notify_name\") -> {name=..} required")); return sol::lua_nil; }
				std::string Name = Params.value().get_or<std::string>("name", "");
				if (Name.empty()) { Session.Log(TEXT("[FAIL] add(\"notify_name\") -> name required")); return sol::lua_nil; }

				FString NotifyStr = UTF8_TO_TCHAR(Name.c_str());
				Skel->Modify();
				AddSkeletonNotifyName(Skel, FName(NotifyStr));
				Skel->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add(\"notify_name\", name=\"%s\")"), *NotifyStr));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("marker_name"), ESearchCase::IgnoreCase))
			{
				if (!SupportsSkeletonMarkerNames()) { Session.Log(TEXT("[FAIL] add(\"marker_name\") -> not available in this build")); return sol::lua_nil; }
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"marker_name\") -> {name=..} required")); return sol::lua_nil; }
				std::string Name = Params.value().get_or<std::string>("name", "");
				if (Name.empty()) { Session.Log(TEXT("[FAIL] add(\"marker_name\") -> name required")); return sol::lua_nil; }

				FString MarkerStr = UTF8_TO_TCHAR(Name.c_str());
				Skel->Modify();
				AddSkeletonMarkerName(Skel, FName(MarkerStr));
				Skel->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add(\"marker_name\", name=\"%s\")"), *MarkerStr));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("compatible_skeleton"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"compatible_skeleton\") -> {path=..} required")); return sol::lua_nil; }
				std::string Path = Params.value().get_or<std::string>("path", "");
				if (Path.empty()) { Session.Log(TEXT("[FAIL] add(\"compatible_skeleton\") -> path required")); return sol::lua_nil; }

				FString SkelPath = UTF8_TO_TCHAR(Path.c_str());
				USkeleton* OtherSkel = LoadObject<USkeleton>(nullptr, *SkelPath);
				if (!OtherSkel) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"compatible_skeleton\") -> could not load '%s'"), *SkelPath)); return sol::lua_nil; }

				Skel->Modify();
				Skel->AddCompatibleSkeleton(OtherSkel);
				Skel->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add(\"compatible_skeleton\", path=\"%s\")"), *SkelPath));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Valid: socket, virtual_bone, blend_profile, slot, slot_group, curve, notify_name, marker_name, compatible_skeleton"), *FType));
			return sol::lua_nil;
		});

		// ---- remove(type, id) ----
		AssetObj.set_function("remove", [Skel, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (FType.Equals(TEXT("socket"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<std::string>()) { Session.Log(TEXT("[FAIL] remove(\"socket\") -> name required")); return sol::lua_nil; }
				FString SocketStr = UTF8_TO_TCHAR(Id.as<std::string>().c_str());
				FName SocketName(SocketStr);
				int32 Idx;
				USkeletalMeshSocket* Socket = Skel->FindSocketAndIndex(SocketName, Idx);
				if (!Socket) { Session.Log(FString::Printf(TEXT("[FAIL] remove(\"socket\") -> '%s' not found"), *SocketStr)); return sol::lua_nil; }
				Skel->Modify();
				Skel->Sockets.RemoveAt(Idx);
				Skel->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove(\"socket\", \"%s\")"), *SocketStr));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("virtual_bone"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<std::string>()) { Session.Log(TEXT("[FAIL] remove(\"virtual_bone\") -> name required")); return sol::lua_nil; }
				FString VBStr = UTF8_TO_TCHAR(Id.as<std::string>().c_str());
				FName VBName(VBStr);

				// Validate virtual bone exists
				bool bFound = false;
				for (const FVirtualBone& VB : Skel->GetVirtualBones())
				{
					if (VB.VirtualBoneName == VBName) { bFound = true; break; }
				}
				if (!bFound) { Session.Log(FString::Printf(TEXT("[FAIL] remove(\"virtual_bone\") -> '%s' not found"), *VBStr)); return sol::lua_nil; }

				TArray<FName> ToRemove = { VBName };
				Skel->RemoveVirtualBones(ToRemove);
				Skel->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove(\"virtual_bone\", \"%s\")"), *VBStr));
				return sol::make_object(Lua, true);
			}

			else if (FType.Equals(TEXT("slot"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<std::string>()) { Session.Log(TEXT("[FAIL] remove(\"slot\") -> name required")); return sol::lua_nil; }
				FString SlotStr = UTF8_TO_TCHAR(Id.as<std::string>().c_str());
				if (!Skel->ContainsSlotName(FName(SlotStr))) { Session.Log(FString::Printf(TEXT("[FAIL] remove(\"slot\") -> '%s' not found"), *SlotStr)); return sol::lua_nil; }
				Skel->Modify();
				Skel->RemoveSlotName(FName(SlotStr));
				Skel->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove(\"slot\", \"%s\")"), *SlotStr));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("slot_group"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<std::string>()) { Session.Log(TEXT("[FAIL] remove(\"slot_group\") -> name required")); return sol::lua_nil; }
				FString GroupStr = UTF8_TO_TCHAR(Id.as<std::string>().c_str());
				if (!Skel->FindAnimSlotGroup(FName(GroupStr))) { Session.Log(FString::Printf(TEXT("[FAIL] remove(\"slot_group\") -> '%s' not found"), *GroupStr)); return sol::lua_nil; }
				Skel->Modify();
				Skel->RemoveSlotGroup(FName(GroupStr));
				Skel->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove(\"slot_group\", \"%s\")"), *GroupStr));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("blend_profile"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<std::string>()) { Session.Log(TEXT("[FAIL] remove(\"blend_profile\") -> name required")); return sol::lua_nil; }
				FString ProfStr = UTF8_TO_TCHAR(Id.as<std::string>().c_str());
				FName ProfName(ProfStr);
				int32 RemovedIdx = INDEX_NONE;
				for (int32 i = 0; i < Skel->BlendProfiles.Num(); ++i)
				{
					if (Skel->BlendProfiles[i] && Skel->BlendProfiles[i]->GetFName() == ProfName)
					{
						RemovedIdx = i;
						break;
					}
				}
				if (RemovedIdx == INDEX_NONE) { Session.Log(FString::Printf(TEXT("[FAIL] remove(\"blend_profile\") -> '%s' not found"), *ProfStr)); return sol::lua_nil; }
				Skel->Modify();
				Skel->BlendProfiles.RemoveAt(RemovedIdx);
				Skel->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove(\"blend_profile\", \"%s\")"), *ProfStr));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("curve"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<std::string>()) { Session.Log(TEXT("[FAIL] remove(\"curve\") -> name required")); return sol::lua_nil; }
				FString CurveStr = UTF8_TO_TCHAR(Id.as<std::string>().c_str());
				Skel->Modify();
				bool bOk = Skel->RemoveCurveMetaData(FName(CurveStr));
				Skel->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[%s] remove(\"curve\", \"%s\")"), bOk ? TEXT("OK") : TEXT("FAIL"), *CurveStr));
				return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
			}
			else if (FType.Equals(TEXT("notify_name"), ESearchCase::IgnoreCase))
			{
				if (!SupportsSkeletonNotifyNames()) { Session.Log(TEXT("[FAIL] remove(\"notify_name\") -> not available in this build")); return sol::lua_nil; }
				if (!Id.is<std::string>()) { Session.Log(TEXT("[FAIL] remove(\"notify_name\") -> name required")); return sol::lua_nil; }
				FString NotifyStr = UTF8_TO_TCHAR(Id.as<std::string>().c_str());
				Skel->Modify();
				RemoveSkeletonNotifyName(Skel, FName(NotifyStr));
				Skel->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove(\"notify_name\", \"%s\")"), *NotifyStr));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("marker_name"), ESearchCase::IgnoreCase))
			{
				if (!SupportsSkeletonMarkerNames()) { Session.Log(TEXT("[FAIL] remove(\"marker_name\") -> not available in this build")); return sol::lua_nil; }
				if (!Id.is<std::string>()) { Session.Log(TEXT("[FAIL] remove(\"marker_name\") -> name required")); return sol::lua_nil; }
				FString MarkerStr = UTF8_TO_TCHAR(Id.as<std::string>().c_str());
				Skel->Modify();
				bool bOk = RemoveSkeletonMarkerName(Skel, FName(MarkerStr));
				Skel->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[%s] remove(\"marker_name\", \"%s\")"), bOk ? TEXT("OK") : TEXT("FAIL"), *MarkerStr));
				return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
			}
			else if (FType.Equals(TEXT("compatible_skeleton"), ESearchCase::IgnoreCase))
			{
				FString SkelPath;
				if (Id.is<std::string>())
				{
					SkelPath = UTF8_TO_TCHAR(Id.as<std::string>().c_str());
				}
				else if (Id.is<sol::table>())
				{
					sol::table T = Id.as<sol::table>();
					std::string Path = T.get_or<std::string>("path", "");
					SkelPath = UTF8_TO_TCHAR(Path.c_str());
				}
				if (SkelPath.IsEmpty()) { Session.Log(TEXT("[FAIL] remove(\"compatible_skeleton\") -> path required")); return sol::lua_nil; }

				USkeleton* OtherSkel = LoadObject<USkeleton>(nullptr, *SkelPath);
				if (!OtherSkel) { Session.Log(FString::Printf(TEXT("[FAIL] remove(\"compatible_skeleton\") -> could not load '%s'"), *SkelPath)); return sol::lua_nil; }

				Skel->Modify();
				Skel->RemoveCompatibleSkeleton(OtherSkel);
				Skel->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove(\"compatible_skeleton\", \"%s\")"), *SkelPath));
				return sol::make_object(Lua, true);
			}

			else if (FType.Equals(TEXT("bone"), ESearchCase::IgnoreCase))
			{
#if WITH_EDITOR
				TArray<FName> BoneNames;
				bool bRemoveChildren = false;
				if (Id.is<sol::table>())
				{
					sol::table T = Id.as<sol::table>();
					sol::optional<sol::table> NamesOpt = T.get<sol::optional<sol::table>>("names");
					if (NamesOpt.has_value())
					{
						for (auto& kv : NamesOpt.value())
						{
							if (kv.second.is<std::string>())
								BoneNames.Add(FName(UTF8_TO_TCHAR(kv.second.as<std::string>().c_str())));
						}
					}
					bRemoveChildren = T.get_or("children", false);
				}
				else if (Id.is<std::string>())
				{
					BoneNames.Add(FName(UTF8_TO_TCHAR(Id.as<std::string>().c_str())));
				}
				if (BoneNames.IsEmpty()) { Session.Log(TEXT("[FAIL] remove(\"bone\") -> {names={..}} or bone name required")); return sol::lua_nil; }

				// Validate all bones exist
				const FReferenceSkeleton& RefSkel = Skel->GetReferenceSkeleton();
				for (const FName& BN : BoneNames)
				{
					if (RefSkel.FindBoneIndex(BN) == INDEX_NONE)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] remove(\"bone\") -> bone '%s' not found"), *BN.ToString()));
						return sol::lua_nil;
					}
				}

				Skel->Modify();
				Skel->RemoveBonesFromSkeleton(BoneNames, bRemoveChildren);
				Session.Log(FString::Printf(TEXT("[OK] remove(\"bone\") -> removed %d bone(s), children=%s"), BoneNames.Num(), bRemoveChildren ? TEXT("true") : TEXT("false")));
				return sol::make_object(Lua, true);
#else
				Session.Log(TEXT("[FAIL] remove(\"bone\") -> not available in this build"));
				return sol::lua_nil;
#endif
			}

			Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type. Valid: socket, virtual_bone, slot, slot_group, blend_profile, curve, notify_name, marker_name, compatible_skeleton, bone"), *FType));
			return sol::lua_nil;
		});

		// ---- configure(type, id, params) ----
		AssetObj.set_function("configure", [Skel, &Session](sol::table /*self*/,
			const std::string& Type, const std::string& Id, sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (FType.Equals(TEXT("socket"), ESearchCase::IgnoreCase))
			{
				FString SocketStr = UTF8_TO_TCHAR(Id.c_str());
				USkeletalMeshSocket* Socket = Skel->FindSocket(FName(SocketStr));
				if (!Socket) { Session.Log(FString::Printf(TEXT("[FAIL] configure(\"socket\", \"%s\") -> not found"), *SocketStr)); return sol::lua_nil; }

				Skel->Modify();
				sol::optional<std::string> BoneOpt = Params.get<sol::optional<std::string>>("bone");
				if (BoneOpt.has_value()) { FString BStr = UTF8_TO_TCHAR(BoneOpt.value().c_str()); Socket->BoneName = FName(BStr); }

				sol::optional<sol::table> Loc = Params.get<sol::optional<sol::table>>("location");
				if (Loc.has_value()) { sol::table L = Loc.value(); Socket->RelativeLocation = FVector(L.get_or(1, 0.0), L.get_or(2, 0.0), L.get_or(3, 0.0)); }
				sol::optional<sol::table> Rot = Params.get<sol::optional<sol::table>>("rotation");
				if (Rot.has_value()) { sol::table R = Rot.value(); Socket->RelativeRotation = FRotator(R.get_or(1, 0.0), R.get_or(2, 0.0), R.get_or(3, 0.0)); }
				sol::optional<sol::table> Sc = Params.get<sol::optional<sol::table>>("scale");
				if (Sc.has_value()) { sol::table V = Sc.value(); Socket->RelativeScale = FVector(V.get_or(1, 1.0), V.get_or(2, 1.0), V.get_or(3, 1.0)); }

				sol::optional<bool> ForceAnim = Params.get<sol::optional<bool>>("force_always_animated");
				if (ForceAnim.has_value()) Socket->bForceAlwaysAnimated = ForceAnim.value();

				Skel->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"socket\", \"%s\")"), *SocketStr));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("blend_profile"), ESearchCase::IgnoreCase))
			{
				FString ProfStr = UTF8_TO_TCHAR(Id.c_str());
				UBlendProfile* Profile = Skel->GetBlendProfile(FName(ProfStr));
				if (!Profile) { Session.Log(FString::Printf(TEXT("[FAIL] configure(\"blend_profile\", \"%s\") -> not found"), *ProfStr)); return sol::lua_nil; }

				Skel->Modify();

				// Mode setting
				sol::optional<std::string> ModeOpt = Params.get<sol::optional<std::string>>("mode");
				if (ModeOpt.has_value())
				{
					FString ModeStr = UTF8_TO_TCHAR(ModeOpt.value().c_str());
					if (ModeStr.Equals(TEXT("weight_factor"), ESearchCase::IgnoreCase)) Profile->Mode = EBlendProfileMode::WeightFactor;
					else if (ModeStr.Equals(TEXT("blend_mask"), ESearchCase::IgnoreCase)) Profile->Mode = EBlendProfileMode::BlendMask;
					else Profile->Mode = EBlendProfileMode::TimeFactor;
				}

				// Rename
				sol::optional<std::string> RenameOpt = Params.get<sol::optional<std::string>>("rename_to");
				if (RenameOpt.has_value())
				{
					FString NewStr = UTF8_TO_TCHAR(RenameOpt.value().c_str());
					UBlendProfile* Renamed = Skel->RenameBlendProfile(FName(ProfStr), FName(NewStr));
					if (!Renamed) { Session.Log(FString::Printf(TEXT("[WARN] configure(\"blend_profile\") -> rename '%s' -> '%s' failed (name collision or not found)"), *ProfStr, *NewStr)); }
				}

				// Remove bone entry
				sol::optional<std::string> RemoveBoneOpt = Params.get<sol::optional<std::string>>("remove_bone");
				if (RemoveBoneOpt.has_value())
				{
					FString RBoneStr = UTF8_TO_TCHAR(RemoveBoneOpt.value().c_str());
					int32 BoneIdx = Skel->GetReferenceSkeleton().FindBoneIndex(FName(RBoneStr));
					if (BoneIdx != INDEX_NONE) Profile->RemoveEntry(BoneIdx);
				}

				// Set bone blend scale (optional — only if bone is specified)
				std::string BoneName = Params.get_or<std::string>("bone", "");
				if (!BoneName.empty())
				{
					FString BoneStr = UTF8_TO_TCHAR(BoneName.c_str());
					double Scale = Params.get_or("scale", 1.0);
					bool bRecurse = Params.get_or("recurse", false);
					Profile->SetBoneBlendScale(FName(BoneStr), static_cast<float>(Scale), bRecurse, true);
				}

				Skel->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"blend_profile\", \"%s\")"), *ProfStr));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("virtual_bone"), ESearchCase::IgnoreCase))
			{
				FString VBStr = UTF8_TO_TCHAR(Id.c_str());
				FName VBName(VBStr);

				// Validate virtual bone exists
				const FVirtualBone* FoundVB = nullptr;
				for (const FVirtualBone& VB : Skel->GetVirtualBones())
				{
					if (VB.VirtualBoneName == VBName) { FoundVB = &VB; break; }
				}
				if (!FoundVB) { Session.Log(FString::Printf(TEXT("[FAIL] configure(\"virtual_bone\", \"%s\") -> not found"), *VBStr)); return sol::lua_nil; }

				sol::optional<std::string> RenameOpt = Params.get<sol::optional<std::string>>("rename_to");
				sol::optional<std::string> SourceOpt = Params.get<sol::optional<std::string>>("source");
				sol::optional<std::string> TargetOpt = Params.get<sol::optional<std::string>>("target");

				if (!RenameOpt.has_value() && !SourceOpt.has_value() && !TargetOpt.has_value())
				{
					Session.Log(TEXT("[FAIL] configure(\"virtual_bone\") -> {rename_to=.., source=.., target=..} at least one required"));
					return sol::lua_nil;
				}

				// If changing source or target, we need to remove and re-add
				if (SourceOpt.has_value() || TargetOpt.has_value())
				{
					FName NewSource = SourceOpt.has_value() ? FName(UTF8_TO_TCHAR(SourceOpt.value().c_str())) : FoundVB->SourceBoneName;
					FName NewTarget = TargetOpt.has_value() ? FName(UTF8_TO_TCHAR(TargetOpt.value().c_str())) : FoundVB->TargetBoneName;
					FName NewName = VBName;
					if (RenameOpt.has_value())
					{
						FString RenameStr = UTF8_TO_TCHAR(RenameOpt.value().c_str());
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
						if (!VirtualBoneNameHelpers::CheckVirtualBonePrefix(RenameStr))
							RenameStr = VirtualBoneNameHelpers::AddVirtualBonePrefix(RenameStr);
#endif
						NewName = FName(RenameStr);
					}

					TArray<FName> ToRemove = { VBName };
					Skel->RemoveVirtualBones(ToRemove);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
					bool bOk = Skel->AddNewNamedVirtualBone(NewSource, NewTarget, NewName);
#else
					FName OutName;
					bool bOk = Skel->AddNewVirtualBone(NewSource, NewTarget, OutName);
#endif
					if (!bOk)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] configure(\"virtual_bone\") -> re-add failed for %s -> %s"), *NewSource.ToString(), *NewTarget.ToString()));
						return sol::lua_nil;
					}
				}
				else if (RenameOpt.has_value())
				{
					FString NewStr = UTF8_TO_TCHAR(RenameOpt.value().c_str());
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
					if (!VirtualBoneNameHelpers::CheckVirtualBonePrefix(NewStr))
						NewStr = VirtualBoneNameHelpers::AddVirtualBonePrefix(NewStr);
#endif
					Skel->RenameVirtualBone(VBName, FName(NewStr));
				}

				Skel->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"virtual_bone\", \"%s\")"), *VBStr));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("slot"), ESearchCase::IgnoreCase))
			{
				FString SlotStr = UTF8_TO_TCHAR(Id.c_str());
				sol::optional<std::string> RenameOpt = Params.get<sol::optional<std::string>>("rename_to");
				if (!RenameOpt.has_value()) { Session.Log(TEXT("[FAIL] configure(\"slot\") -> {rename_to=..} required")); return sol::lua_nil; }

				// Validate slot exists before calling RenameSlotName (has check() assertion)
				bool bSlotExists = false;
				for (const FAnimSlotGroup& Group : Skel->GetSlotGroups())
				{
					if (Group.SlotNames.Contains(FName(SlotStr))) { bSlotExists = true; break; }
				}
				if (!bSlotExists) { Session.Log(FString::Printf(TEXT("[FAIL] configure(\"slot\") -> '%s' not found"), *SlotStr)); return sol::lua_nil; }

				FString NewStr = UTF8_TO_TCHAR(RenameOpt.value().c_str());
				Skel->Modify();
				Skel->RenameSlotName(FName(SlotStr), FName(NewStr));
				Skel->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"slot\", \"%s\" -> \"%s\")"), *SlotStr, *NewStr));
				return sol::make_object(Lua, true);
			}

			else if (FType.Equals(TEXT("slot_group"), ESearchCase::IgnoreCase))
			{
				FString GroupStr = UTF8_TO_TCHAR(Id.c_str());
				sol::optional<sol::table> SlotsOpt = Params.get<sol::optional<sol::table>>("slots");
				if (!SlotsOpt.has_value()) { Session.Log(TEXT("[FAIL] configure(\"slot_group\") -> {slots={..}} required")); return sol::lua_nil; }

				Skel->Modify();
				sol::table Slots = SlotsOpt.value();
				int32 Count = 0;
				for (auto& kv : Slots)
				{
					if (kv.second.is<std::string>())
					{
						FString SlotStr = UTF8_TO_TCHAR(kv.second.as<std::string>().c_str());
						Skel->SetSlotGroupName(FName(SlotStr), FName(GroupStr));
						Count++;
					}
				}
				Skel->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"slot_group\", \"%s\") -> assigned %d slots"), *GroupStr, Count));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("curve"), ESearchCase::IgnoreCase))
			{
				FString CurveStr = UTF8_TO_TCHAR(Id.c_str());
				FName CurveName(CurveStr);
				FCurveMetaData* Meta = Skel->GetCurveMetaData(CurveName);
				if (!Meta) { Session.Log(FString::Printf(TEXT("[FAIL] configure(\"curve\", \"%s\") -> not found"), *CurveStr)); return sol::lua_nil; }

				Skel->Modify();
				sol::optional<bool> MatOpt = Params.get<sol::optional<bool>>("material");
				sol::optional<bool> MorphOpt = Params.get<sol::optional<bool>>("morph_target");
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
				if (MatOpt.has_value()) Skel->SetCurveMetaDataMaterial(CurveName, MatOpt.value());
				if (MorphOpt.has_value()) Skel->SetCurveMetaDataMorphTarget(CurveName, MorphOpt.value());
#else
				if (MatOpt.has_value() || MorphOpt.has_value())
				{
					Session.Log(TEXT("[WARN] configure(\"curve\") -> material/morph_target flags require UE 5.7+, ignored"));
				}
#endif

				sol::optional<std::string> RenameOpt = Params.get<sol::optional<std::string>>("rename_to");
				if (RenameOpt.has_value())
				{
					FString NewStr = UTF8_TO_TCHAR(RenameOpt.value().c_str());
					bool bRenamed = Skel->RenameCurveMetaData(CurveName, FName(NewStr));
					if (!bRenamed) { Session.Log(FString::Printf(TEXT("[FAIL] configure(\"curve\") -> rename to '%s' failed"), *NewStr)); }
				}

				Skel->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"curve\", \"%s\")"), *CurveStr));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("notify_name"), ESearchCase::IgnoreCase))
			{
				if (!SupportsSkeletonNotifyNames()) { Session.Log(TEXT("[FAIL] configure(\"notify_name\") -> not available in this build")); return sol::lua_nil; }
				FString OldStr = UTF8_TO_TCHAR(Id.c_str());
				sol::optional<std::string> RenameOpt = Params.get<sol::optional<std::string>>("rename_to");
				if (!RenameOpt.has_value()) { Session.Log(TEXT("[FAIL] configure(\"notify_name\") -> {rename_to=..} required")); return sol::lua_nil; }

				FString NewStr = UTF8_TO_TCHAR(RenameOpt.value().c_str());
				Skel->Modify();
				RenameSkeletonNotifyName(Skel, FName(OldStr), FName(NewStr));
				Skel->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"notify_name\", \"%s\" -> \"%s\")"), *OldStr, *NewStr));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("marker_name"), ESearchCase::IgnoreCase))
			{
				if (!SupportsSkeletonMarkerNames()) { Session.Log(TEXT("[FAIL] configure(\"marker_name\") -> not available in this build")); return sol::lua_nil; }
				FString OldStr = UTF8_TO_TCHAR(Id.c_str());
				sol::optional<std::string> RenameOpt = Params.get<sol::optional<std::string>>("rename_to");
				if (!RenameOpt.has_value()) { Session.Log(TEXT("[FAIL] configure(\"marker_name\") -> {rename_to=..} required")); return sol::lua_nil; }

				FString NewStr = UTF8_TO_TCHAR(RenameOpt.value().c_str());
				Skel->Modify();
				bool bOk = RenameSkeletonMarkerName(Skel, FName(OldStr), FName(NewStr));
				Skel->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[%s] configure(\"marker_name\", \"%s\" -> \"%s\")"), bOk ? TEXT("OK") : TEXT("FAIL"), *OldStr, *NewStr));
				return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> unknown type. Valid: socket, blend_profile, virtual_bone, slot, slot_group, curve, notify_name, marker_name"), *FType));
			return sol::lua_nil;
		});

		// ---- list(type?) ----
		AssetObj.set_function("list", [Skel, &Session](sol::table self,
			sol::optional<std::string> TypeOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = TypeOpt.has_value() ? UTF8_TO_TCHAR(TypeOpt.value().c_str()) : TEXT("all");

			if (FType.Equals(TEXT("all"), ESearchCase::IgnoreCase))
			{
				sol::protected_function InfoFn = self["info"];
				if (InfoFn.valid()) return InfoFn(self);
				return sol::lua_nil;
			}

			if (FType.Contains(TEXT("socket"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Skel->Sockets.Num(); i++)
				{
					USkeletalMeshSocket* Socket = Skel->Sockets[i];
					if (!Socket) continue;
					sol::table E = Lua.create_table();
					E["name"] = TCHAR_TO_UTF8(*Socket->SocketName.ToString());
					E["bone"] = TCHAR_TO_UTF8(*Socket->BoneName.ToString());
					E["location"] = Lua.create_table_with(1, Socket->RelativeLocation.X, 2, Socket->RelativeLocation.Y, 3, Socket->RelativeLocation.Z);
					E["rotation"] = Lua.create_table_with(1, Socket->RelativeRotation.Pitch, 2, Socket->RelativeRotation.Yaw, 3, Socket->RelativeRotation.Roll);
					E["scale"] = Lua.create_table_with(1, Socket->RelativeScale.X, 2, Socket->RelativeScale.Y, 3, Socket->RelativeScale.Z);
					E["force_always_animated"] = Socket->bForceAlwaysAnimated;
					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"sockets\") -> %d"), Skel->Sockets.Num()));
				return Result;
			}

			if (FType.Contains(TEXT("bone"), ESearchCase::IgnoreCase) && !FType.Contains(TEXT("virtual"), ESearchCase::IgnoreCase))
			{
				const FReferenceSkeleton& RefSkel = Skel->GetReferenceSkeleton();
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < RefSkel.GetNum(); i++)
				{
					sol::table E = Lua.create_table();
					E["index"] = i;
					E["name"] = TCHAR_TO_UTF8(*RefSkel.GetBoneName(i).ToString());
					E["parent"] = RefSkel.GetParentIndex(i);
					EBoneTranslationRetargetingMode::Type RetMode = Skel->GetBoneTranslationRetargetingMode(i);
					const char* RetModeStr = "animation";
					if (RetMode == EBoneTranslationRetargetingMode::Skeleton) RetModeStr = "skeleton";
					else if (RetMode == EBoneTranslationRetargetingMode::AnimationScaled) RetModeStr = "animation_scaled";
					else if (RetMode == EBoneTranslationRetargetingMode::AnimationRelative) RetModeStr = "animation_relative";
					else if (RetMode == EBoneTranslationRetargetingMode::OrientAndScale) RetModeStr = "orient_and_scale";
					E["retargeting"] = RetModeStr;
					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"bones\") -> %d"), RefSkel.GetNum()));
				return Result;
			}

			if (FType.Contains(TEXT("virtual"), ESearchCase::IgnoreCase))
			{
				const TArray<FVirtualBone>& VBones = Skel->GetVirtualBones();
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < VBones.Num(); i++)
				{
					sol::table E = Lua.create_table();
					E["name"] = TCHAR_TO_UTF8(*VBones[i].VirtualBoneName.ToString());
					E["source"] = TCHAR_TO_UTF8(*VBones[i].SourceBoneName.ToString());
					E["target"] = TCHAR_TO_UTF8(*VBones[i].TargetBoneName.ToString());
					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"virtual_bones\") -> %d"), VBones.Num()));
				return Result;
			}

			if (FType.Contains(TEXT("blend"), ESearchCase::IgnoreCase) || FType.Contains(TEXT("profile"), ESearchCase::IgnoreCase))
			{
				const TArray<UBlendProfile*>& Profiles = Skel->BlendProfiles;
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Profiles.Num(); i++)
				{
					if (!Profiles[i]) continue;
					sol::table E = Lua.create_table();
					E["name"] = TCHAR_TO_UTF8(*Profiles[i]->GetFName().ToString());
					const char* ModeStr = "time_factor";
					if (Profiles[i]->Mode == EBlendProfileMode::WeightFactor) ModeStr = "weight_factor";
					else if (Profiles[i]->Mode == EBlendProfileMode::BlendMask) ModeStr = "blend_mask";
					E["mode"] = ModeStr;
					E["entries"] = Profiles[i]->GetNumBlendEntries();
					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"blend_profiles\") -> %d"), Profiles.Num()));
				return Result;
			}

			if (FType.Contains(TEXT("slot_group"), ESearchCase::IgnoreCase))
			{
				const TArray<FAnimSlotGroup>& Groups = Skel->GetSlotGroups();
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Groups.Num(); i++)
				{
					sol::table E = Lua.create_table();
					E["name"] = TCHAR_TO_UTF8(*Groups[i].GroupName.ToString());
					sol::table SlotsArr = Lua.create_table();
					for (int32 j = 0; j < Groups[i].SlotNames.Num(); j++)
					{
						SlotsArr[j + 1] = TCHAR_TO_UTF8(*Groups[i].SlotNames[j].ToString());
					}
					E["slots"] = SlotsArr;
					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"slot_groups\") -> %d"), Groups.Num()));
				return Result;
			}

			if (FType.Contains(TEXT("curve"), ESearchCase::IgnoreCase))
			{
				TArray<FName> CurveNames;
				Skel->GetCurveMetaDataNames(CurveNames);
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < CurveNames.Num(); i++)
				{
					sol::table E = Lua.create_table();
					E["name"] = TCHAR_TO_UTF8(*CurveNames[i].ToString());
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
					E["material"] = Skel->GetCurveMetaDataMaterial(CurveNames[i]);
					E["morph_target"] = Skel->GetCurveMetaDataMorphTarget(CurveNames[i]);
#else
					E["material"] = false;
					E["morph_target"] = false;
#endif
					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"curves\") -> %d"), CurveNames.Num()));
				return Result;
			}

			if (FType.Contains(TEXT("notify_name"), ESearchCase::IgnoreCase))
			{
				if (!SupportsSkeletonNotifyNames()) { Session.Log(TEXT("[FAIL] list(\"notify_names\") -> not available in this build")); return sol::lua_nil; }
				TArray<FName> Notifies;
				CollectSkeletonNotifyNames(Skel, Notifies);
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Notifies.Num(); i++)
				{
					Result[i + 1] = TCHAR_TO_UTF8(*Notifies[i].ToString());
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"notify_names\") -> %d"), Notifies.Num()));
				return Result;
			}

			if (FType.Contains(TEXT("marker_name"), ESearchCase::IgnoreCase))
			{
				if (!SupportsSkeletonMarkerNames()) { Session.Log(TEXT("[FAIL] list(\"marker_names\") -> not available in this build")); return sol::lua_nil; }
				TArray<FName> Markers;
				CollectSkeletonMarkerNames(Skel, Markers);
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Markers.Num(); i++)
				{
					Result[i + 1] = TCHAR_TO_UTF8(*Markers[i].ToString());
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"marker_names\") -> %d"), Markers.Num()));
				return Result;
			}

			if (FType.Contains(TEXT("compatible"), ESearchCase::IgnoreCase))
			{
				const TArray<TSoftObjectPtr<USkeleton>>& Compat = Skel->GetCompatibleSkeletons();
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Compat.Num(); i++)
				{
					sol::table E = Lua.create_table();
					E["path"] = TCHAR_TO_UTF8(*Compat[i].ToSoftObjectPath().ToString());
					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"compatible_skeletons\") -> %d"), Compat.Num()));
				return Result;
			}

			if (FType.Contains(TEXT("retarget"), ESearchCase::IgnoreCase))
			{
#if WITH_EDITORONLY_DATA
				TArray<FName> RetargetNames;
				Skel->GetRetargetSources(RetargetNames);
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < RetargetNames.Num(); i++)
				{
					sol::table E = Lua.create_table();
					E["name"] = TCHAR_TO_UTF8(*RetargetNames[i].ToString());
					const FReferencePose* RefPose = Skel->AnimRetargetSources.Find(RetargetNames[i]);
					if (RefPose && RefPose->SourceReferenceMesh.IsValid())
					{
						E["source_mesh"] = TCHAR_TO_UTF8(*RefPose->SourceReferenceMesh.ToSoftObjectPath().ToString());
					}
					E["pose_bones"] = RefPose ? RefPose->ReferencePose.Num() : 0;
					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"retarget_sources\") -> %d"), RetargetNames.Num()));
				return Result;
#else
				Session.Log(TEXT("[FAIL] list(\"retarget_sources\") -> not available in this build"));
				return sol::lua_nil;
#endif
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: sockets, bones, virtual_bones, blend_profiles, slot_groups, curves, notify_names, marker_names, compatible_skeletons, retarget_sources"), *FType));
			return sol::lua_nil;
		});

		// ---- Action: set_retargeting ----
		AssetObj.set_function("set_retargeting", [Skel, &Session](sol::table /*self*/,
			const std::string& BoneName, const std::string& Mode, sol::optional<bool> ChildrenToo,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Skel)) return sol::lua_nil;
			FString BoneStr = UTF8_TO_TCHAR(BoneName.c_str());
			FString FMode = UTF8_TO_TCHAR(Mode.c_str());

			int32 BoneIdx = Skel->GetReferenceSkeleton().FindBoneIndex(FName(BoneStr));
			if (BoneIdx == INDEX_NONE) { Session.Log(FString::Printf(TEXT("[FAIL] set_retargeting -> bone '%s' not found"), *BoneStr)); return sol::lua_nil; }

			EBoneTranslationRetargetingMode::Type RetMode = EBoneTranslationRetargetingMode::Animation;
			if (FMode.Equals(TEXT("skeleton"), ESearchCase::IgnoreCase)) RetMode = EBoneTranslationRetargetingMode::Skeleton;
			else if (FMode.Equals(TEXT("animation_scaled"), ESearchCase::IgnoreCase)) RetMode = EBoneTranslationRetargetingMode::AnimationScaled;
			else if (FMode.Equals(TEXT("animation_relative"), ESearchCase::IgnoreCase)) RetMode = EBoneTranslationRetargetingMode::AnimationRelative;
			else if (FMode.Equals(TEXT("orient_and_scale"), ESearchCase::IgnoreCase)) RetMode = EBoneTranslationRetargetingMode::OrientAndScale;

			Skel->Modify();
			Skel->SetBoneTranslationRetargetingMode(BoneIdx, RetMode, ChildrenToo.value_or(false));
			Skel->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] set_retargeting(\"%s\", %s)"), *BoneStr, *FMode));
			return sol::make_object(Lua, true);
		});

		// ---- get_bone_transform(bone) ----
		AssetObj.set_function("get_bone_transform", [Skel, &Session](sol::table /*self*/,
			const std::string& BoneName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Skel)) return sol::lua_nil;
			FString BoneStr = UTF8_TO_TCHAR(BoneName.c_str());
			const FReferenceSkeleton& RefSkel = Skel->GetReferenceSkeleton();
			int32 BoneIdx = RefSkel.FindBoneIndex(FName(BoneStr));
			if (BoneIdx == INDEX_NONE) { Session.Log(FString::Printf(TEXT("[FAIL] get_bone_transform -> bone '%s' not found"), *BoneStr)); return sol::lua_nil; }

			const FTransform& BoneTransform = RefSkel.GetRefBonePose()[BoneIdx];
			sol::table Result = Lua.create_table();
			FVector Loc = BoneTransform.GetLocation();
			FRotator Rot = BoneTransform.Rotator();
			FVector Sc = BoneTransform.GetScale3D();
			Result["location"] = Lua.create_table_with(1, Loc.X, 2, Loc.Y, 3, Loc.Z);
			Result["rotation"] = Lua.create_table_with(1, Rot.Pitch, 2, Rot.Yaw, 3, Rot.Roll);
			Result["scale"] = Lua.create_table_with(1, Sc.X, 2, Sc.Y, 3, Sc.Z);
			Session.Log(FString::Printf(TEXT("[OK] get_bone_transform(\"%s\")"), *BoneStr));
			return Result;
		});

		// ---- get_child_bones(bone) ----
		AssetObj.set_function("get_child_bones", [Skel, &Session](sol::table /*self*/,
			const std::string& BoneName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Skel)) return sol::lua_nil;
			FString BoneStr = UTF8_TO_TCHAR(BoneName.c_str());
			const FReferenceSkeleton& RefSkel = Skel->GetReferenceSkeleton();
			int32 BoneIdx = RefSkel.FindBoneIndex(FName(BoneStr));
			if (BoneIdx == INDEX_NONE) { Session.Log(FString::Printf(TEXT("[FAIL] get_child_bones -> bone '%s' not found"), *BoneStr)); return sol::lua_nil; }

#if WITH_EDITORONLY_DATA
			TArray<int32> Children;
			Skel->GetChildBones(BoneIdx, Children);
			sol::table Result = Lua.create_table();
			for (int32 i = 0; i < Children.Num(); i++)
			{
				Result[i + 1] = TCHAR_TO_UTF8(*RefSkel.GetBoneName(Children[i]).ToString());
			}
			Session.Log(FString::Printf(TEXT("[OK] get_child_bones(\"%s\") -> %d children"), *BoneStr, Children.Num()));
			return Result;
#else
			// Fallback: iterate reference skeleton manually
			sol::table Result = Lua.create_table();
			int32 Count = 0;
			for (int32 i = 0; i < RefSkel.GetNum(); i++)
			{
				if (RefSkel.GetParentIndex(i) == BoneIdx)
				{
					Result[++Count] = TCHAR_TO_UTF8(*RefSkel.GetBoneName(i).ToString());
				}
			}
			Session.Log(FString::Printf(TEXT("[OK] get_child_bones(\"%s\") -> %d children"), *BoneStr, Count));
			return Result;
#endif
		});

		// ---- get_preview_mesh() ----
		AssetObj.set_function("get_preview_mesh", [Skel, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Skel)) return sol::lua_nil;

			USkeletalMesh* PreviewMesh = Skel->GetPreviewMesh();
			if (!PreviewMesh)
			{
				Session.Log(TEXT("[OK] get_preview_mesh() -> none set"));
				return sol::lua_nil;
			}
			FString MeshPath = PreviewMesh->GetPathName();
			Session.Log(FString::Printf(TEXT("[OK] get_preview_mesh() -> %s"), *MeshPath));
			return sol::make_object(Lua, TCHAR_TO_UTF8(*MeshPath));
		});

		// ---- set_preview_mesh(path) ----
		AssetObj.set_function("set_preview_mesh", [Skel, &Session](sol::table /*self*/,
			const std::string& MeshPath, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Skel)) return sol::lua_nil;
			FString FMeshPath = UTF8_TO_TCHAR(MeshPath.c_str());

			if (FMeshPath.IsEmpty())
			{
				Skel->SetPreviewMesh(nullptr);
				Session.Log(TEXT("[OK] set_preview_mesh(nil) -> cleared"));
				return sol::make_object(Lua, true);
			}

			USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *FMeshPath);
			if (!Mesh) { Session.Log(FString::Printf(TEXT("[FAIL] set_preview_mesh -> could not load '%s'"), *FMeshPath)); return sol::lua_nil; }

			Skel->SetPreviewMesh(Mesh);
			Session.Log(FString::Printf(TEXT("[OK] set_preview_mesh(\"%s\")"), *FMeshPath));
			return sol::make_object(Lua, true);
		});

		// ---- info() — override default ----
		AssetObj.set_function("info", [Skel, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Skel)) return sol::lua_nil;

			sol::table Result = Lua.create_table();
			Result["bones"] = Skel->GetReferenceSkeleton().GetNum();
			Result["sockets"] = Skel->Sockets.Num();
			Result["virtual_bones"] = static_cast<int>(Skel->GetVirtualBones().Num());
			Result["blend_profiles"] = static_cast<int>(Skel->BlendProfiles.Num());
			Result["slot_groups"] = static_cast<int>(Skel->GetSlotGroups().Num());
			Result["compatible_skeletons"] = static_cast<int>(Skel->GetCompatibleSkeletons().Num());

			TArray<FName> CurveNames;
			Skel->GetCurveMetaDataNames(CurveNames);
			Result["curves"] = CurveNames.Num();

			TArray<FName> NotifyNames;
			CollectSkeletonNotifyNames(Skel, NotifyNames);
			Result["notify_names"] = NotifyNames.Num();

			TArray<FName> MarkerNames;
			CollectSkeletonMarkerNames(Skel, MarkerNames);
			Result["marker_names"] = MarkerNames.Num();

			Session.Log(FString::Printf(TEXT("[OK] info() -> %d bones, %d sockets, %d virtual bones, %d blend profiles, %d slot groups, %d curves, %d compatible skeletons"),
				Skel->GetReferenceSkeleton().GetNum(), Skel->Sockets.Num(),
				Skel->GetVirtualBones().Num(), Skel->BlendProfiles.Num(),
				Skel->GetSlotGroups().Num(), CurveNames.Num(),
				Skel->GetCompatibleSkeletons().Num()));
			return Result;
		});
	});
}

REGISTER_LUA_BINDING(Skeleton, SkeletonDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindSkeleton(Lua, Session);
});
