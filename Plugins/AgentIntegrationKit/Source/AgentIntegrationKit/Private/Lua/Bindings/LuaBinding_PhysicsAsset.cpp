#include "Lua/LuaBindingRegistry.h"
#include "PhysicsAssetUtils.h"
#include "PhysicsEngine/PhysicsAsset.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
#include "PhysicsEngine/SkeletalBodySetup.h"
#endif
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/SphereElem.h"
#include "PhysicsEngine/BoxElem.h"
#include "PhysicsEngine/SphylElem.h"
#include "PhysicsEngine/TaperedCapsuleElem.h"
#include "Engine/SkeletalMesh.h"
#include "Components/SkeletalMeshComponent.h"
#include "ScopedTransaction.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// In UE 5.4, USkeletalBodySetup doesn't exist as a separate class — use UBodySetup instead
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
using FPhysAssetBodySetup = USkeletalBodySetup;
#else
using FPhysAssetBodySetup = UBodySetup;
#endif // ENGINE_MINOR_VERSION >= 5

static EPhysAssetFitGeomType ParseGeomTypePhysAsset(const FString& Str)
{
	if (Str.Equals(TEXT("box"), ESearchCase::IgnoreCase)) return EFG_Box;
	if (Str.Equals(TEXT("sphere"), ESearchCase::IgnoreCase)) return EFG_Sphere;
	if (Str.Equals(TEXT("tapered_capsule"), ESearchCase::IgnoreCase)) return EFG_TaperedCapsule;
	if (Str.Equals(TEXT("convex"), ESearchCase::IgnoreCase)) return EFG_SingleConvexHull;
	return EFG_Sphyl; // capsule default
}

static EAngularConstraintMotion ParseAngularMotionPhysAsset(const FString& Str)
{
	if (Str.Equals(TEXT("free"), ESearchCase::IgnoreCase)) return ACM_Free;
	if (Str.Equals(TEXT("locked"), ESearchCase::IgnoreCase)) return ACM_Locked;
	return ACM_Limited;
}

static ELinearConstraintMotion ParseLinearMotionPhysAsset(const FString& Str)
{
	if (Str.Equals(TEXT("free"), ESearchCase::IgnoreCase)) return LCM_Free;
	if (Str.Equals(TEXT("locked"), ESearchCase::IgnoreCase)) return LCM_Locked;
	return LCM_Limited;
}

// Finds a constraint by bone pair, trying both orderings
static int32 FindConstraintBiDirectional(UPhysicsAsset* PhysAsset, FName Bone1, FName Bone2)
{
	int32 Idx = PhysAsset->FindConstraintIndex(Bone1, Bone2);
	if (Idx == INDEX_NONE)
		Idx = PhysAsset->FindConstraintIndex(Bone2, Bone1);
	return Idx;
}

static TArray<FLuaFunctionDoc> PhysicsAssetDocs = {};

static void BindPhysicsAsset(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_physics_asset", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		UPhysicsAsset* PhysAsset = LoadObject<UPhysicsAsset>(nullptr, *FPath);
		if (!PhysAsset) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"Element types for add/remove/list:\n"
			"  body       — physics body on a bone\n"
			"  constraint — physics constraint between two bones\n"
			"  shape      — collision shape on a body (sphere/box/capsule/tapered_capsule)\n"
			"\n"
			"add(type, params):\n"
			"  add(\"body\", {bone=\"spine_01\", geom_type=\"capsule\"})  — geom: capsule/box/sphere/tapered_capsule/convex\n"
			"  add(\"constraint\", {child=\"spine_02\", parent=\"spine_01\",\n"
			"       swing1={motion=\"limited\", limit=45}, swing2={...}, twist={...}, disable_collision=true})\n"
			"  add(\"shape\", {bone=\"spine_01\", type=\"sphere\", radius=10, center={0,0,0}})\n"
			"  add(\"shape\", {bone=\"spine_01\", type=\"box\", x=10, y=20, z=30, center={0,0,0}, rotation={pitch=0,yaw=0,roll=0}})\n"
			"  add(\"shape\", {bone=\"spine_01\", type=\"capsule\", radius=5, length=20})\n"
			"  add(\"shape\", {bone=\"spine_01\", type=\"tapered_capsule\", radius0=5, radius1=3, length=20})\n"
			"\n"
			"remove(type, id):\n"
			"  remove(\"body\", \"spine_01\")  — by bone name\n"
			"  remove(\"constraint\", {child=\"spine_02\", parent=\"spine_01\"})  — by bone pair\n"
			"  remove(\"shape\", {bone=\"spine_01\", type=\"sphere\", index=0})\n"
			"\n"
			"list(type, filter?):\n"
			"  list(\"bodies\"), list(\"constraints\"), list(\"shapes\"), list(\"shapes\", \"spine_01\")\n"
			"  list(\"constraint_profiles\")  — named constraint profiles on the asset\n"
			"  list(\"body_profiles\")        — named physical animation profiles on the asset\n"
			"\n"
			"configure(type, params):\n"
			"  configure(\"body\", {bone=\"spine_01\", mass_override=80, mass_scale=1.0,\n"
			"       linear_damping=0.01, angular_damping=0.0, sleep_family=\"normal\",\n"
			"       collision_response=\"enabled\", phys_material=\"/Game/PM\",\n"
			"       position_solver_iterations=8, velocity_solver_iterations=2,\n"
			"       max_depenetration_velocity=100.0})\n"
			"  configure(\"shape\", {bone=\"spine_01\", type=\"sphere\", index=0, radius=15, center={1,2,3}})\n"
			"  configure(\"constraint\", {child=\"spine_02\", parent=\"spine_01\",\n"
			"       swing1={motion=\"limited\", limit=45}, swing2={...}, twist={...},\n"
			"       linear={x_motion=\"free\", y_motion=\"locked\", z_motion=\"limited\", limit=100},\n"
			"       disable_collision=true, projection=true,\n"
			"       breakable={linear=true, linear_threshold=1000},\n"
			"       angular_drive={stiffness=100, damping=10, mode=\"slerp\"},\n"
			"       linear_drive={stiffness=100, damping=10, target_position={x=0,y=0,z=0}},\n"
			"       frame1={position={x=0,y=0,z=5}, primary_axis={x=1,y=0,z=0}, secondary_axis={x=0,y=1,z=0}},\n"
			"       frame2={position={x=0,y=0,z=-5}, primary_axis={x=1,y=0,z=0}, secondary_axis={x=0,y=1,z=0}}})\n"
			"  configure(\"constraint_profile\", {constraint=.., profile=\"ProfileName\"})  — apply named profile to constraint\n"
			"    constraint can be {child=, parent=} or {index=N}\n"
			"\n"
			"Action methods:\n"
			"  generate(skeletal_mesh_path, {geom_type, min_bone_size, create_constraints, body_for_all, disable_collisions, angular_mode})\n"
			"  disable_collision(bone_a, bone_b) / enable_collision(bone_a, bone_b)\n"
			"  is_collision_enabled(bone_a, bone_b) — check if collision is enabled between two bodies\n"
			"  set_physics_type(bone, type) — type: default/kinematic/simulated\n"
			"  weld(base_bone, add_bone) — weld add_bone's body into base_bone's body\n"
			"  get_bodies_below(bone, include_parent?) — get all bodies in hierarchy below a bone\n"
			"  find_constraints(bone) — find all constraints connected to a body\n"
			"  info() — summary of bodies and constraints\n";

		// ---- add(type, params) ----
		AssetObj.set_function("add", [PhysAsset, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (FType.Equals(TEXT("body"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"body\") -> {bone=..} required")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string BoneName = P.get_or<std::string>("bone", "");
				if (BoneName.empty()) { Session.Log(TEXT("[FAIL] add(\"body\") -> bone required")); return sol::lua_nil; }

				FString BoneStr = UTF8_TO_TCHAR(BoneName.c_str());

				// Check if body already exists for this bone
				int32 Existing = PhysAsset->FindBodyIndex(FName(BoneStr));
				if (Existing != INDEX_NONE)
				{
					Session.Log(FString::Printf(TEXT("[WARN] add(\"body\") -> body for bone '%s' already exists at index %d"), *BoneStr, Existing));
					return sol::make_object(Lua, Existing);
				}

				FPhysAssetCreateParams CreateParams;
				std::string Geom = P.get_or<std::string>("geom_type", "capsule");
				CreateParams.GeomType = ParseGeomTypePhysAsset(UTF8_TO_TCHAR(Geom.c_str()));

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "AddBody", "Add Physics Body"));
				int32 Idx = FPhysicsAssetUtils::CreateNewBody(PhysAsset, FName(BoneStr), CreateParams);
				if (Idx < 0) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"body\", bone=\"%s\")"), *BoneStr)); return sol::lua_nil; }
				PhysAsset->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add(\"body\", bone=\"%s\") -> index %d"), *BoneStr, Idx));
				return sol::make_object(Lua, Idx);
			}
			else if (FType.Equals(TEXT("constraint"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"constraint\") -> {child=.., parent=..} required")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string ChildBone = P.get_or<std::string>("child", "");
				std::string ParentBone = P.get_or<std::string>("parent", "");
				if (ChildBone.empty() || ParentBone.empty()) { Session.Log(TEXT("[FAIL] add(\"constraint\") -> child and parent required")); return sol::lua_nil; }

				FString ChildStr = UTF8_TO_TCHAR(ChildBone.c_str());
				FString ParentStr = UTF8_TO_TCHAR(ParentBone.c_str());

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "AddConstraint", "Add Physics Constraint"));
				int32 Idx = FPhysicsAssetUtils::CreateNewConstraint(PhysAsset, FName(ChildStr));
				if (Idx < 0) { Session.Log(TEXT("[FAIL] add(\"constraint\")")); return sol::lua_nil; }

				UPhysicsConstraintTemplate* CS = PhysAsset->ConstraintSetup[Idx];
				CS->Modify();
				CS->DefaultInstance.ConstraintBone1 = FName(ChildStr);
				CS->DefaultInstance.ConstraintBone2 = FName(ParentStr);

				FConstraintInstance& CI = CS->DefaultInstance;
				sol::optional<sol::table> Swing1 = P.get<sol::optional<sol::table>>("swing1");
				if (Swing1.has_value())
				{
					std::string Motion = Swing1.value().get_or<std::string>("motion", "limited");
					double Limit = Swing1.value().get_or("limit", 45.0);
					CI.SetAngularSwing1Limit(ParseAngularMotionPhysAsset(UTF8_TO_TCHAR(Motion.c_str())), static_cast<float>(Limit));
				}
				sol::optional<sol::table> Swing2 = P.get<sol::optional<sol::table>>("swing2");
				if (Swing2.has_value())
				{
					std::string Motion = Swing2.value().get_or<std::string>("motion", "limited");
					double Limit = Swing2.value().get_or("limit", 45.0);
					CI.SetAngularSwing2Limit(ParseAngularMotionPhysAsset(UTF8_TO_TCHAR(Motion.c_str())), static_cast<float>(Limit));
				}
				sol::optional<sol::table> Twist = P.get<sol::optional<sol::table>>("twist");
				if (Twist.has_value())
				{
					std::string Motion = Twist.value().get_or<std::string>("motion", "limited");
					double Limit = Twist.value().get_or("limit", 45.0);
					CI.SetAngularTwistLimit(ParseAngularMotionPhysAsset(UTF8_TO_TCHAR(Motion.c_str())), static_cast<float>(Limit));
				}
				CI.ProfileInstance.bDisableCollision = P.get_or("disable_collision", true);

				PhysAsset->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add(\"constraint\", child=\"%s\", parent=\"%s\") -> index %d"), *ChildStr, *ParentStr, Idx));
				return sol::make_object(Lua, Idx);
			}

			else if (FType.Equals(TEXT("shape"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"shape\") -> {bone=.., type=..} required")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string BoneName = P.get_or<std::string>("bone", "");
				std::string ShapeType = P.get_or<std::string>("type", "");
				if (BoneName.empty() || ShapeType.empty()) { Session.Log(TEXT("[FAIL] add(\"shape\") -> bone and type required")); return sol::lua_nil; }

				FString BoneStr = UTF8_TO_TCHAR(BoneName.c_str());
				int32 BodyIdx = PhysAsset->FindBodyIndex(FName(BoneStr));
				if (BodyIdx == INDEX_NONE) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"shape\") -> body for bone '%s' not found"), *BoneStr)); return sol::lua_nil; }
				FPhysAssetBodySetup* Body = PhysAsset->SkeletalBodySetups[BodyIdx];

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "AddShape", "Add Collision Shape"));
				Body->Modify();

				// Parse common center
				FVector Center = FVector::ZeroVector;
				sol::optional<sol::table> CenterTbl = P.get<sol::optional<sol::table>>("center");
				if (CenterTbl.has_value())
				{
					Center.X = CenterTbl.value().get<sol::optional<double>>(1).value_or(0.0);
					Center.Y = CenterTbl.value().get<sol::optional<double>>(2).value_or(0.0);
					Center.Z = CenterTbl.value().get<sol::optional<double>>(3).value_or(0.0);
				}
				// Parse common rotation
				FRotator Rot = FRotator::ZeroRotator;
				sol::optional<sol::table> RotTbl = P.get<sol::optional<sol::table>>("rotation");
				if (RotTbl.has_value())
				{
					Rot.Pitch = static_cast<float>(RotTbl.value().get<sol::optional<double>>("pitch").value_or(0.0));
					Rot.Yaw = static_cast<float>(RotTbl.value().get<sol::optional<double>>("yaw").value_or(0.0));
					Rot.Roll = static_cast<float>(RotTbl.value().get<sol::optional<double>>("roll").value_or(0.0));
				}

				FString FShapeType = UTF8_TO_TCHAR(ShapeType.c_str());
				if (FShapeType.Equals(TEXT("sphere"), ESearchCase::IgnoreCase))
				{
					FKSphereElem Elem;
					Elem.Center = Center;
					Elem.Radius = static_cast<float>(P.get<sol::optional<double>>("radius").value_or(10.0));
					Body->AggGeom.SphereElems.Add(Elem);
				}
				else if (FShapeType.Equals(TEXT("box"), ESearchCase::IgnoreCase))
				{
					FKBoxElem Elem;
					Elem.Center = Center;
					Elem.Rotation = Rot;
					Elem.X = static_cast<float>(P.get<sol::optional<double>>("x").value_or(10.0));
					Elem.Y = static_cast<float>(P.get<sol::optional<double>>("y").value_or(10.0));
					Elem.Z = static_cast<float>(P.get<sol::optional<double>>("z").value_or(10.0));
					Body->AggGeom.BoxElems.Add(Elem);
				}
				else if (FShapeType.Equals(TEXT("capsule"), ESearchCase::IgnoreCase))
				{
					FKSphylElem Elem;
					Elem.Center = Center;
					Elem.Rotation = Rot;
					Elem.Radius = static_cast<float>(P.get<sol::optional<double>>("radius").value_or(5.0));
					Elem.Length = static_cast<float>(P.get<sol::optional<double>>("length").value_or(20.0));
					Body->AggGeom.SphylElems.Add(Elem);
				}
				else if (FShapeType.Equals(TEXT("tapered_capsule"), ESearchCase::IgnoreCase))
				{
					FKTaperedCapsuleElem Elem;
					Elem.Center = Center;
					Elem.Rotation = Rot;
					Elem.Radius0 = static_cast<float>(P.get<sol::optional<double>>("radius0").value_or(5.0));
					Elem.Radius1 = static_cast<float>(P.get<sol::optional<double>>("radius1").value_or(3.0));
					Elem.Length = static_cast<float>(P.get<sol::optional<double>>("length").value_or(20.0));
					Body->AggGeom.TaperedCapsuleElems.Add(Elem);
				}
				else
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"shape\") -> unknown shape type '%s'. Valid: sphere, box, capsule, tapered_capsule"), *FShapeType));
					return sol::lua_nil;
				}

				PhysAsset->InvalidateAllPhysicsMeshes();
				PhysAsset->UpdateBoundsBodiesArray();
				PhysAsset->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add(\"shape\", bone=\"%s\", type=\"%s\")"), *BoneStr, *FShapeType));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Valid: body, constraint, shape"), *FType));
			return sol::lua_nil;
		});

		// ---- remove(type, id) ----
		AssetObj.set_function("remove", [PhysAsset, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (FType.Equals(TEXT("body"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<std::string>()) { Session.Log(TEXT("[FAIL] remove(\"body\") -> bone name required")); return sol::lua_nil; }
				FString BoneStr = UTF8_TO_TCHAR(Id.as<std::string>().c_str());
				int32 Idx = PhysAsset->FindBodyIndex(FName(BoneStr));
				if (Idx == INDEX_NONE) { Session.Log(FString::Printf(TEXT("[FAIL] remove(\"body\") -> '%s' not found"), *BoneStr)); return sol::lua_nil; }
				const FScopedTransaction Tx(NSLOCTEXT("AIK", "RemoveBody", "Remove Physics Body"));
				FPhysicsAssetUtils::DestroyBody(PhysAsset, Idx);
				PhysAsset->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove(\"body\", \"%s\")"), *BoneStr));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("constraint"), ESearchCase::IgnoreCase))
			{
				// Accept table {child=, parent=} or just index
				if (Id.is<sol::table>())
				{
					sol::table T = Id.as<sol::table>();
					std::string Child = T.get_or<std::string>("child", "");
					std::string Parent = T.get_or<std::string>("parent", "");
					FString ChildStr = UTF8_TO_TCHAR(Child.c_str());
					FString ParentStr = UTF8_TO_TCHAR(Parent.c_str());
					int32 Idx = FindConstraintBiDirectional(PhysAsset, FName(ChildStr), FName(ParentStr));
					if (Idx == INDEX_NONE) { Session.Log(TEXT("[FAIL] remove(\"constraint\") -> not found")); return sol::lua_nil; }
					const FScopedTransaction Tx(NSLOCTEXT("AIK", "RemoveConstraint", "Remove Physics Constraint"));
					FPhysicsAssetUtils::DestroyConstraint(PhysAsset, Idx);
					PhysAsset->MarkPackageDirty();
					Session.Log(FString::Printf(TEXT("[OK] remove(\"constraint\", \"%s\" -> \"%s\")"), *ChildStr, *ParentStr));
					return sol::make_object(Lua, true);
				}
				else if (Id.is<int>())
				{
					int32 Idx = Id.as<int>();
					if (Idx < 0 || Idx >= PhysAsset->ConstraintSetup.Num()) { Session.Log(TEXT("[FAIL] remove(\"constraint\") -> index out of range")); return sol::lua_nil; }
					const FScopedTransaction Tx(NSLOCTEXT("AIK", "RemoveConstraint", "Remove Physics Constraint"));
					FPhysicsAssetUtils::DestroyConstraint(PhysAsset, Idx);
					PhysAsset->MarkPackageDirty();
					Session.Log(FString::Printf(TEXT("[OK] remove(\"constraint\", %d)"), Idx));
					return sol::make_object(Lua, true);
				}
				Session.Log(TEXT("[FAIL] remove(\"constraint\") -> provide {child=, parent=} or index"));
				return sol::lua_nil;
			}

			else if (FType.Equals(TEXT("shape"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<sol::table>()) { Session.Log(TEXT("[FAIL] remove(\"shape\") -> {bone=.., type=.., index=..} required")); return sol::lua_nil; }
				sol::table T = Id.as<sol::table>();
				std::string BoneName = T.get_or<std::string>("bone", "");
				std::string ShapeType = T.get_or<std::string>("type", "");
				int32 ShapeIdx = static_cast<int32>(T.get<sol::optional<double>>("index").value_or(0.0));
				if (BoneName.empty() || ShapeType.empty()) { Session.Log(TEXT("[FAIL] remove(\"shape\") -> bone and type required")); return sol::lua_nil; }

				FString BoneStr = UTF8_TO_TCHAR(BoneName.c_str());
				int32 BodyIdx = PhysAsset->FindBodyIndex(FName(BoneStr));
				if (BodyIdx == INDEX_NONE) { Session.Log(FString::Printf(TEXT("[FAIL] remove(\"shape\") -> body for bone '%s' not found"), *BoneStr)); return sol::lua_nil; }
				FPhysAssetBodySetup* Body = PhysAsset->SkeletalBodySetups[BodyIdx];

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "RemoveShape", "Remove Collision Shape"));
				Body->Modify();
				FString FShapeType = UTF8_TO_TCHAR(ShapeType.c_str());
				bool bRemoved = false;
				if (FShapeType.Equals(TEXT("sphere"), ESearchCase::IgnoreCase) && Body->AggGeom.SphereElems.IsValidIndex(ShapeIdx))
				{
					Body->AggGeom.SphereElems.RemoveAt(ShapeIdx);
					bRemoved = true;
				}
				else if (FShapeType.Equals(TEXT("box"), ESearchCase::IgnoreCase) && Body->AggGeom.BoxElems.IsValidIndex(ShapeIdx))
				{
					Body->AggGeom.BoxElems.RemoveAt(ShapeIdx);
					bRemoved = true;
				}
				else if (FShapeType.Equals(TEXT("capsule"), ESearchCase::IgnoreCase) && Body->AggGeom.SphylElems.IsValidIndex(ShapeIdx))
				{
					Body->AggGeom.SphylElems.RemoveAt(ShapeIdx);
					bRemoved = true;
				}
				else if (FShapeType.Equals(TEXT("tapered_capsule"), ESearchCase::IgnoreCase) && Body->AggGeom.TaperedCapsuleElems.IsValidIndex(ShapeIdx))
				{
					Body->AggGeom.TaperedCapsuleElems.RemoveAt(ShapeIdx);
					bRemoved = true;
				}

				if (!bRemoved)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"shape\") -> shape type '%s' index %d not found on bone '%s'"), *FShapeType, ShapeIdx, *BoneStr));
					return sol::lua_nil;
				}

				PhysAsset->InvalidateAllPhysicsMeshes();
				PhysAsset->UpdateBoundsBodiesArray();
				PhysAsset->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove(\"shape\", bone=\"%s\", type=\"%s\", index=%d)"), *BoneStr, *FShapeType, ShapeIdx));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type. Valid: body, constraint, shape"), *FType));
			return sol::lua_nil;
		});

		// ---- list(type?, filter?) ----
		AssetObj.set_function("list", [PhysAsset, &Session](sol::table self,
			sol::optional<std::string> TypeOpt, sol::optional<std::string> FilterOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = TypeOpt.has_value() ? UTF8_TO_TCHAR(TypeOpt.value().c_str()) : TEXT("all");

			if (FType.Equals(TEXT("all"), ESearchCase::IgnoreCase))
			{
				sol::protected_function InfoFn = self["info"];
				if (InfoFn.valid()) return InfoFn(self);
				return sol::lua_nil;
			}

			// Check profile types BEFORE generic "constraint"/"bod" checks (since Contains would match both)
			if (FType.Contains(TEXT("constraint_profile"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				const TArray<FName>& Profiles = PhysAsset->GetConstraintProfileNames();
				for (int32 i = 0; i < Profiles.Num(); i++)
				{
					sol::table E = Lua.create_table();
					E["index"] = i;
					E["name"] = TCHAR_TO_UTF8(*Profiles[i].ToString());
					// List which constraints have this profile
					sol::table Constraints = Lua.create_table();
					int32 Idx = 1;
					for (int32 j = 0; j < PhysAsset->ConstraintSetup.Num(); j++)
					{
						UPhysicsConstraintTemplate* CS = PhysAsset->ConstraintSetup[j];
						if (CS && CS->ContainsConstraintProfile(Profiles[i]))
						{
							sol::table C = Lua.create_table();
							C["index"] = j;
							C["child"] = TCHAR_TO_UTF8(*CS->DefaultInstance.ConstraintBone1.ToString());
							C["parent"] = TCHAR_TO_UTF8(*CS->DefaultInstance.ConstraintBone2.ToString());
							Constraints[Idx++] = C;
						}
					}
					E["constraints"] = Constraints;
					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"constraint_profiles\") -> %d"), Profiles.Num()));
				return Result;
			}

			if (FType.Contains(TEXT("body_profile"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				const TArray<FName>& Profiles = PhysAsset->GetPhysicalAnimationProfileNames();
				for (int32 i = 0; i < Profiles.Num(); i++)
				{
					sol::table E = Lua.create_table();
					E["index"] = i;
					E["name"] = TCHAR_TO_UTF8(*Profiles[i].ToString());
					// List which bodies have this profile
					sol::table Bodies = Lua.create_table();
					int32 Idx = 1;
					for (int32 j = 0; j < PhysAsset->SkeletalBodySetups.Num(); j++)
					{
						FPhysAssetBodySetup* Body = PhysAsset->SkeletalBodySetups[j];
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
						if (Body && Body->FindPhysicalAnimationProfile(Profiles[i]))
#else
						if (Body)
#endif
						{
							sol::table B = Lua.create_table();
							B["index"] = j;
							B["bone"] = TCHAR_TO_UTF8(*Body->BoneName.ToString());
							Bodies[Idx++] = B;
						}
					}
					E["bodies"] = Bodies;
					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"body_profiles\") -> %d"), Profiles.Num()));
				return Result;
			}

			if (FType.Contains(TEXT("bod"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < PhysAsset->SkeletalBodySetups.Num(); i++)
				{
					FPhysAssetBodySetup* Body = PhysAsset->SkeletalBodySetups[i];
					if (!Body) continue;
					sol::table E = Lua.create_table();
					E["index"] = i;
					E["bone"] = TCHAR_TO_UTF8(*Body->BoneName.ToString());
					E["total_shapes"] = Body->AggGeom.GetElementCount();
					E["spheres"] = Body->AggGeom.SphereElems.Num();
					E["boxes"] = Body->AggGeom.BoxElems.Num();
					E["capsules"] = Body->AggGeom.SphylElems.Num();
					E["tapered_capsules"] = Body->AggGeom.TaperedCapsuleElems.Num();
					E["convex"] = Body->AggGeom.ConvexElems.Num();

					// Physics type
					switch (Body->PhysicsType)
					{
						case PhysType_Kinematic: E["physics_type"] = "kinematic"; break;
						case PhysType_Simulated: E["physics_type"] = "simulated"; break;
						default: E["physics_type"] = "default"; break;
					}

					// Body instance details
					const FBodyInstance& BI = Body->DefaultInstance;
					E["linear_damping"] = BI.LinearDamping;
					E["angular_damping"] = BI.AngularDamping;
					E["mass_scale"] = BI.MassScale;
					E["mass_override"] = BI.GetMassOverride();

					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"bodies\") -> %d"), PhysAsset->SkeletalBodySetups.Num()));
				return Result;
			}

			if (FType.Contains(TEXT("constraint"), ESearchCase::IgnoreCase))
			{
				auto MotionToStr = [](EAngularConstraintMotion M) -> const char* {
					switch (M) { case ACM_Free: return "free"; case ACM_Locked: return "locked"; default: return "limited"; }
				};
				auto LinMotionToStr = [](ELinearConstraintMotion M) -> const char* {
					switch (M) { case LCM_Free: return "free"; case LCM_Locked: return "locked"; default: return "limited"; }
				};
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < PhysAsset->ConstraintSetup.Num(); i++)
				{
					UPhysicsConstraintTemplate* CS = PhysAsset->ConstraintSetup[i];
					if (!CS) continue;
					const FConstraintInstance& CI = CS->DefaultInstance;
					sol::table E = Lua.create_table();
					E["index"] = i;
					E["child"] = TCHAR_TO_UTF8(*CI.ConstraintBone1.ToString());
					E["parent"] = TCHAR_TO_UTF8(*CI.ConstraintBone2.ToString());

					// Angular limits
					E["swing1_motion"] = MotionToStr(CI.GetAngularSwing1Motion());
					E["swing1_limit"] = CI.GetAngularSwing1Limit();
					E["swing2_motion"] = MotionToStr(CI.GetAngularSwing2Motion());
					E["swing2_limit"] = CI.GetAngularSwing2Limit();
					E["twist_motion"] = MotionToStr(CI.GetAngularTwistMotion());
					E["twist_limit"] = CI.GetAngularTwistLimit();

					// Linear limits
					E["linear_x"] = LinMotionToStr(CI.GetLinearXMotion());
					E["linear_y"] = LinMotionToStr(CI.GetLinearYMotion());
					E["linear_z"] = LinMotionToStr(CI.GetLinearZMotion());
					E["linear_limit"] = CI.GetLinearLimit();

					E["disable_collision"] = (bool)CI.ProfileInstance.bDisableCollision;
					E["projection"] = (bool)CI.ProfileInstance.bEnableProjection;

					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"constraints\") -> %d"), PhysAsset->ConstraintSetup.Num()));
				return Result;
			}

			if (FType.Contains(TEXT("shape"), ESearchCase::IgnoreCase))
			{
				// Optionally filter by bone name
				FString BoneFilter = FilterOpt.has_value() ? UTF8_TO_TCHAR(FilterOpt.value().c_str()) : TEXT("");
				sol::table Result = Lua.create_table();
				int32 Idx = 1;
				for (int32 i = 0; i < PhysAsset->SkeletalBodySetups.Num(); i++)
				{
					FPhysAssetBodySetup* Body = PhysAsset->SkeletalBodySetups[i];
					if (!Body) continue;
					FString BoneStr = Body->BoneName.ToString();
					if (!BoneFilter.IsEmpty() && !BoneStr.Equals(BoneFilter, ESearchCase::IgnoreCase)) continue;

					for (int32 j = 0; j < Body->AggGeom.SphereElems.Num(); j++)
					{
						const FKSphereElem& E = Body->AggGeom.SphereElems[j];
						sol::table S2 = Lua.create_table();
						S2["bone"] = TCHAR_TO_UTF8(*BoneStr);
						S2["type"] = "sphere";
						S2["index"] = j;
						S2["radius"] = E.Radius;
						S2["center_x"] = E.Center.X; S2["center_y"] = E.Center.Y; S2["center_z"] = E.Center.Z;
						Result[Idx++] = S2;
					}
					for (int32 j = 0; j < Body->AggGeom.BoxElems.Num(); j++)
					{
						const FKBoxElem& E = Body->AggGeom.BoxElems[j];
						sol::table S2 = Lua.create_table();
						S2["bone"] = TCHAR_TO_UTF8(*BoneStr);
						S2["type"] = "box";
						S2["index"] = j;
						S2["x"] = E.X; S2["y"] = E.Y; S2["z"] = E.Z;
						S2["center_x"] = E.Center.X; S2["center_y"] = E.Center.Y; S2["center_z"] = E.Center.Z;
						Result[Idx++] = S2;
					}
					for (int32 j = 0; j < Body->AggGeom.SphylElems.Num(); j++)
					{
						const FKSphylElem& E = Body->AggGeom.SphylElems[j];
						sol::table S2 = Lua.create_table();
						S2["bone"] = TCHAR_TO_UTF8(*BoneStr);
						S2["type"] = "capsule";
						S2["index"] = j;
						S2["radius"] = E.Radius; S2["length"] = E.Length;
						S2["center_x"] = E.Center.X; S2["center_y"] = E.Center.Y; S2["center_z"] = E.Center.Z;
						Result[Idx++] = S2;
					}
					for (int32 j = 0; j < Body->AggGeom.TaperedCapsuleElems.Num(); j++)
					{
						const FKTaperedCapsuleElem& E = Body->AggGeom.TaperedCapsuleElems[j];
						sol::table S2 = Lua.create_table();
						S2["bone"] = TCHAR_TO_UTF8(*BoneStr);
						S2["type"] = "tapered_capsule";
						S2["index"] = j;
						S2["radius0"] = E.Radius0; S2["radius1"] = E.Radius1; S2["length"] = E.Length;
						S2["center_x"] = E.Center.X; S2["center_y"] = E.Center.Y; S2["center_z"] = E.Center.Z;
						Result[Idx++] = S2;
					}
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"shapes\") -> %d"), Idx - 1));
				return Result;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: bodies, constraints, shapes, constraint_profiles, body_profiles"), *FType));
			return sol::lua_nil;
		});

		// ---- Action: generate ----
		AssetObj.set_function("generate", [PhysAsset, &Session](sol::table /*self*/,
			const std::string& MeshPath, sol::optional<sol::table> Opts, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FMesh = UTF8_TO_TCHAR(MeshPath.c_str());
			if (!FMesh.StartsWith(TEXT("/"))) FMesh = TEXT("/Game/") + FMesh;
			USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *FMesh);
			if (!Mesh) { Session.Log(FString::Printf(TEXT("[FAIL] generate -> mesh '%s' not found"), *FMesh)); return sol::lua_nil; }

			FPhysAssetCreateParams Params;
			if (Opts.has_value())
			{
				sol::table O = Opts.value();
				std::string Geom = O.get_or<std::string>("geom_type", "capsule");
				Params.GeomType = ParseGeomTypePhysAsset(UTF8_TO_TCHAR(Geom.c_str()));
				Params.MinBoneSize = static_cast<float>(O.get_or("min_bone_size", 20.0));
				Params.bCreateConstraints = O.get_or("create_constraints", true);
				Params.bBodyForAll = O.get_or("body_for_all", false);
				Params.bDisableCollisionsByDefault = O.get_or("disable_collisions", true);
				std::string AngMode = O.get_or<std::string>("angular_mode", "limited");
				Params.AngularConstraintMode = ParseAngularMotionPhysAsset(UTF8_TO_TCHAR(AngMode.c_str()));
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "GeneratePhysAsset", "Generate Physics Asset"));
			FText ErrorMsg;
			bool bOk = FPhysicsAssetUtils::CreateFromSkeletalMesh(PhysAsset, Mesh, Params, ErrorMsg);
			if (bOk)
			{
				PhysAsset->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] generate -> %d bodies, %d constraints"),
					PhysAsset->SkeletalBodySetups.Num(), PhysAsset->ConstraintSetup.Num()));
			}
			else
				Session.Log(FString::Printf(TEXT("[FAIL] generate -> %s"), *ErrorMsg.ToString()));
			return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
		});

		// ---- Action: disable_collision / enable_collision / is_collision_enabled ----
		AssetObj.set_function("disable_collision", [PhysAsset, &Session](sol::table /*self*/,
			const std::string& BoneA, const std::string& BoneB, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString AStr = UTF8_TO_TCHAR(BoneA.c_str());
			FString BStr = UTF8_TO_TCHAR(BoneB.c_str());
			int32 IdxA = PhysAsset->FindBodyIndex(FName(AStr));
			int32 IdxB = PhysAsset->FindBodyIndex(FName(BStr));
			if (IdxA == INDEX_NONE || IdxB == INDEX_NONE) { Session.Log(TEXT("[FAIL] disable_collision -> body not found")); return sol::lua_nil; }
			const FScopedTransaction Tx(NSLOCTEXT("AIK", "DisableCollision", "Disable Collision"));
			PhysAsset->DisableCollision(IdxA, IdxB);
			PhysAsset->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] disable_collision(\"%s\", \"%s\")"), *AStr, *BStr));
			return sol::make_object(Lua, true);
		});

		AssetObj.set_function("enable_collision", [PhysAsset, &Session](sol::table /*self*/,
			const std::string& BoneA, const std::string& BoneB, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString AStr = UTF8_TO_TCHAR(BoneA.c_str());
			FString BStr = UTF8_TO_TCHAR(BoneB.c_str());
			int32 IdxA = PhysAsset->FindBodyIndex(FName(AStr));
			int32 IdxB = PhysAsset->FindBodyIndex(FName(BStr));
			if (IdxA == INDEX_NONE || IdxB == INDEX_NONE) { Session.Log(TEXT("[FAIL] enable_collision -> body not found")); return sol::lua_nil; }
			const FScopedTransaction Tx(NSLOCTEXT("AIK", "EnableCollision", "Enable Collision"));
			PhysAsset->EnableCollision(IdxA, IdxB);
			PhysAsset->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] enable_collision(\"%s\", \"%s\")"), *AStr, *BStr));
			return sol::make_object(Lua, true);
		});

		AssetObj.set_function("is_collision_enabled", [PhysAsset, &Session](sol::table /*self*/,
			const std::string& BoneA, const std::string& BoneB, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString AStr = UTF8_TO_TCHAR(BoneA.c_str());
			FString BStr = UTF8_TO_TCHAR(BoneB.c_str());
			int32 IdxA = PhysAsset->FindBodyIndex(FName(AStr));
			int32 IdxB = PhysAsset->FindBodyIndex(FName(BStr));
			if (IdxA == INDEX_NONE || IdxB == INDEX_NONE) { Session.Log(TEXT("[FAIL] is_collision_enabled -> body not found")); return sol::lua_nil; }
			bool bEnabled = PhysAsset->IsCollisionEnabled(IdxA, IdxB);
			Session.Log(FString::Printf(TEXT("[OK] is_collision_enabled(\"%s\", \"%s\") -> %s"), *AStr, *BStr, bEnabled ? TEXT("true") : TEXT("false")));
			return sol::make_object(Lua, bEnabled);
		});

		// ---- Action: set_physics_type ----
		AssetObj.set_function("set_physics_type", [PhysAsset, &Session](sol::table /*self*/,
			const std::string& BoneName, const std::string& TypeStr, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString BoneStr = UTF8_TO_TCHAR(BoneName.c_str());
			int32 Idx = PhysAsset->FindBodyIndex(FName(BoneStr));
			if (Idx == INDEX_NONE) { Session.Log(TEXT("[FAIL] set_physics_type -> body not found")); return sol::lua_nil; }

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "SetPhysType", "Set Physics Type"));
			FPhysAssetBodySetup* Body = PhysAsset->SkeletalBodySetups[Idx];
			Body->Modify();

			FString FTypeStr = UTF8_TO_TCHAR(TypeStr.c_str());
			EPhysicsType PhysType = PhysType_Default;
			if (FTypeStr.Equals(TEXT("kinematic"), ESearchCase::IgnoreCase)) PhysType = PhysType_Kinematic;
			else if (FTypeStr.Equals(TEXT("simulated"), ESearchCase::IgnoreCase)) PhysType = PhysType_Simulated;

			Body->PhysicsType = PhysType;
			PhysAsset->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] set_physics_type(\"%s\", %s)"), *BoneStr, *FTypeStr));
			return sol::make_object(Lua, true);
		});

		// ---- configure(type, params) ----
		AssetObj.set_function("configure", [PhysAsset, &Session](sol::table /*self*/,
			const std::string& Type, sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (FType.Equals(TEXT("shape"), ESearchCase::IgnoreCase))
			{
				std::string BoneName = Params.get_or<std::string>("bone", "");
				std::string ShapeType = Params.get_or<std::string>("type", "");
				int32 ShapeIdx = static_cast<int32>(Params.get<sol::optional<double>>("index").value_or(0.0));
				if (BoneName.empty() || ShapeType.empty()) { Session.Log(TEXT("[FAIL] configure(\"shape\") -> bone and type required")); return sol::lua_nil; }

				FString BoneStr = UTF8_TO_TCHAR(BoneName.c_str());
				int32 BodyIdx = PhysAsset->FindBodyIndex(FName(BoneStr));
				if (BodyIdx == INDEX_NONE) { Session.Log(FString::Printf(TEXT("[FAIL] configure(\"shape\") -> body for bone '%s' not found"), *BoneStr)); return sol::lua_nil; }
				FPhysAssetBodySetup* Body = PhysAsset->SkeletalBodySetups[BodyIdx];

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "ConfigShape", "Configure Collision Shape"));
				Body->Modify();

				// Parse optional center
				FVector* CenterPtr = nullptr;
				FVector NewCenter;
				sol::optional<sol::table> CenterTbl = Params.get<sol::optional<sol::table>>("center");
				if (CenterTbl.has_value())
				{
					NewCenter.X = CenterTbl.value().get<sol::optional<double>>(1).value_or(0.0);
					NewCenter.Y = CenterTbl.value().get<sol::optional<double>>(2).value_or(0.0);
					NewCenter.Z = CenterTbl.value().get<sol::optional<double>>(3).value_or(0.0);
					CenterPtr = &NewCenter;
				}
				// Parse optional rotation
				FRotator* RotPtr = nullptr;
				FRotator NewRot;
				sol::optional<sol::table> RotTbl = Params.get<sol::optional<sol::table>>("rotation");
				if (RotTbl.has_value())
				{
					NewRot.Pitch = static_cast<float>(RotTbl.value().get<sol::optional<double>>("pitch").value_or(0.0));
					NewRot.Yaw = static_cast<float>(RotTbl.value().get<sol::optional<double>>("yaw").value_or(0.0));
					NewRot.Roll = static_cast<float>(RotTbl.value().get<sol::optional<double>>("roll").value_or(0.0));
					RotPtr = &NewRot;
				}

				FString FShapeType = UTF8_TO_TCHAR(ShapeType.c_str());
				bool bFound = false;

				if (FShapeType.Equals(TEXT("sphere"), ESearchCase::IgnoreCase) && Body->AggGeom.SphereElems.IsValidIndex(ShapeIdx))
				{
					FKSphereElem& Elem = Body->AggGeom.SphereElems[ShapeIdx];
					if (CenterPtr) Elem.Center = *CenterPtr;
					sol::optional<double> Radius = Params.get<sol::optional<double>>("radius");
					if (Radius.has_value()) Elem.Radius = static_cast<float>(Radius.value());
					bFound = true;
				}
				else if (FShapeType.Equals(TEXT("box"), ESearchCase::IgnoreCase) && Body->AggGeom.BoxElems.IsValidIndex(ShapeIdx))
				{
					FKBoxElem& Elem = Body->AggGeom.BoxElems[ShapeIdx];
					if (CenterPtr) Elem.Center = *CenterPtr;
					if (RotPtr) Elem.Rotation = *RotPtr;
					sol::optional<double> X = Params.get<sol::optional<double>>("x");
					sol::optional<double> Y = Params.get<sol::optional<double>>("y");
					sol::optional<double> Z = Params.get<sol::optional<double>>("z");
					if (X.has_value()) Elem.X = static_cast<float>(X.value());
					if (Y.has_value()) Elem.Y = static_cast<float>(Y.value());
					if (Z.has_value()) Elem.Z = static_cast<float>(Z.value());
					bFound = true;
				}
				else if (FShapeType.Equals(TEXT("capsule"), ESearchCase::IgnoreCase) && Body->AggGeom.SphylElems.IsValidIndex(ShapeIdx))
				{
					FKSphylElem& Elem = Body->AggGeom.SphylElems[ShapeIdx];
					if (CenterPtr) Elem.Center = *CenterPtr;
					if (RotPtr) Elem.Rotation = *RotPtr;
					sol::optional<double> Radius = Params.get<sol::optional<double>>("radius");
					sol::optional<double> Length = Params.get<sol::optional<double>>("length");
					if (Radius.has_value()) Elem.Radius = static_cast<float>(Radius.value());
					if (Length.has_value()) Elem.Length = static_cast<float>(Length.value());
					bFound = true;
				}
				else if (FShapeType.Equals(TEXT("tapered_capsule"), ESearchCase::IgnoreCase) && Body->AggGeom.TaperedCapsuleElems.IsValidIndex(ShapeIdx))
				{
					FKTaperedCapsuleElem& Elem = Body->AggGeom.TaperedCapsuleElems[ShapeIdx];
					if (CenterPtr) Elem.Center = *CenterPtr;
					if (RotPtr) Elem.Rotation = *RotPtr;
					sol::optional<double> R0 = Params.get<sol::optional<double>>("radius0");
					sol::optional<double> R1 = Params.get<sol::optional<double>>("radius1");
					sol::optional<double> Length = Params.get<sol::optional<double>>("length");
					if (R0.has_value()) Elem.Radius0 = static_cast<float>(R0.value());
					if (R1.has_value()) Elem.Radius1 = static_cast<float>(R1.value());
					if (Length.has_value()) Elem.Length = static_cast<float>(Length.value());
					bFound = true;
				}

				if (!bFound)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"shape\") -> shape type '%s' index %d not found on bone '%s'"), *FShapeType, ShapeIdx, *BoneStr));
					return sol::lua_nil;
				}

				PhysAsset->InvalidateAllPhysicsMeshes();
				PhysAsset->UpdateBoundsBodiesArray();
				PhysAsset->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"shape\", bone=\"%s\", type=\"%s\", index=%d)"), *BoneStr, *FShapeType, ShapeIdx));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("body"), ESearchCase::IgnoreCase))
			{
				std::string BoneName = Params.get_or<std::string>("bone", "");
				if (BoneName.empty()) { Session.Log(TEXT("[FAIL] configure(\"body\") -> bone required")); return sol::lua_nil; }

				FString BoneStr = UTF8_TO_TCHAR(BoneName.c_str());
				int32 BodyIdx = PhysAsset->FindBodyIndex(FName(BoneStr));
				if (BodyIdx == INDEX_NONE) { Session.Log(FString::Printf(TEXT("[FAIL] configure(\"body\") -> body for bone '%s' not found"), *BoneStr)); return sol::lua_nil; }

				UBodySetup* BodySetup = PhysAsset->SkeletalBodySetups[BodyIdx];
				FBodyInstance& BI = BodySetup->DefaultInstance;

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "ConfigBody", "Configure Body"));
				BodySetup->Modify();

				// Mass
				sol::optional<double> MassOverride = Params.get<sol::optional<double>>("mass_override");
				if (MassOverride.has_value())
				{
					BI.SetMassOverride(FMath::Max(0.001f, static_cast<float>(MassOverride.value())), true);
				}
				sol::optional<double> MassScale = Params.get<sol::optional<double>>("mass_scale");
				if (MassScale.has_value()) BI.MassScale = static_cast<float>(MassScale.value());

				// Damping
				sol::optional<double> LinDamp = Params.get<sol::optional<double>>("linear_damping");
				if (LinDamp.has_value()) BI.LinearDamping = static_cast<float>(LinDamp.value());
				sol::optional<double> AngDamp = Params.get<sol::optional<double>>("angular_damping");
				if (AngDamp.has_value()) BI.AngularDamping = static_cast<float>(AngDamp.value());

				// Sleep
				sol::optional<std::string> SleepFam = Params.get<sol::optional<std::string>>("sleep_family");
				if (SleepFam.has_value())
				{
					FString SF = UTF8_TO_TCHAR(SleepFam.value().c_str());
					if (SF.Equals(TEXT("normal"), ESearchCase::IgnoreCase)) BI.SleepFamily = ESleepFamily::Normal;
					else if (SF.Equals(TEXT("sensitive"), ESearchCase::IgnoreCase)) BI.SleepFamily = ESleepFamily::Sensitive;
					else if (SF.Equals(TEXT("custom"), ESearchCase::IgnoreCase)) BI.SleepFamily = ESleepFamily::Custom;
				}
				sol::optional<double> SleepThresh = Params.get<sol::optional<double>>("custom_sleep_threshold");
				if (SleepThresh.has_value()) BI.CustomSleepThresholdMultiplier = static_cast<float>(SleepThresh.value());

				// Collision response
				sol::optional<std::string> CollResp = Params.get<sol::optional<std::string>>("collision_response");
				if (CollResp.has_value())
				{
					FString CR = UTF8_TO_TCHAR(CollResp.value().c_str());
					if (CR.Equals(TEXT("enabled"), ESearchCase::IgnoreCase) || CR.Equals(TEXT("block"), ESearchCase::IgnoreCase))
						BI.SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
					else if (CR.Equals(TEXT("query_only"), ESearchCase::IgnoreCase))
						BI.SetCollisionEnabled(ECollisionEnabled::QueryOnly);
					else if (CR.Equals(TEXT("physics_only"), ESearchCase::IgnoreCase))
						BI.SetCollisionEnabled(ECollisionEnabled::PhysicsOnly);
					else if (CR.Equals(TEXT("disabled"), ESearchCase::IgnoreCase) || CR.Equals(TEXT("none"), ESearchCase::IgnoreCase))
						BI.SetCollisionEnabled(ECollisionEnabled::NoCollision);
				}

				// Physical material
				sol::optional<std::string> PhysMatPath = Params.get<sol::optional<std::string>>("phys_material");
				if (PhysMatPath.has_value())
				{
					FString FMat = UTF8_TO_TCHAR(PhysMatPath.value().c_str());
					UPhysicalMaterial* Mat = LoadObject<UPhysicalMaterial>(nullptr, *FMat);
					if (Mat) BI.SetPhysMaterialOverride(Mat);
					else Session.Log(FString::Printf(TEXT("[WARN] configure(\"body\") -> phys_material not found: %s"), *FMat));
				}

				// Solver iteration overrides
				sol::optional<double> PosIter = Params.get<sol::optional<double>>("position_solver_iterations");
				sol::optional<double> VelIter = Params.get<sol::optional<double>>("velocity_solver_iterations");
				if (PosIter.has_value() || VelIter.has_value())
				{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
#if ENGINE_MINOR_VERSION >= 7
					BI.SetOverrideIterationCounts(true);
#endif
					if (PosIter.has_value()) BI.SetPositionSolverIterationCount(static_cast<uint8>(FMath::Clamp(static_cast<int32>(PosIter.value()), 0, 255)));
					if (VelIter.has_value()) BI.SetVelocitySolverIterationCount(static_cast<uint8>(FMath::Clamp(static_cast<int32>(VelIter.value()), 0, 255)));
#else
					Session.Log(TEXT("[WARN] configure(\"body\") -> solver iteration overrides require UE 5.5+"));
#endif
				}

				// Max depenetration velocity (implicitly enables override)
				sol::optional<double> MaxDepen = Params.get<sol::optional<double>>("max_depenetration_velocity");
				if (MaxDepen.has_value()) BI.SetMaxDepenetrationVelocity(static_cast<float>(MaxDepen.value()));

				PhysAsset->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"body\", bone=\"%s\")"), *BoneStr));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("constraint"), ESearchCase::IgnoreCase))
			{
				// Find constraint by {child=, parent=} or by index
				int32 CIdx = INDEX_NONE;
				sol::optional<double> IdxOpt = Params.get<sol::optional<double>>("index");
				if (IdxOpt.has_value())
				{
					CIdx = static_cast<int32>(IdxOpt.value());
				}
				else
				{
					std::string Child = Params.get_or<std::string>("child", "");
					std::string Parent = Params.get_or<std::string>("parent", "");
					if (Child.empty() || Parent.empty()) { Session.Log(TEXT("[FAIL] configure(\"constraint\") -> {child=, parent=} or {index=} required")); return sol::lua_nil; }
					CIdx = FindConstraintBiDirectional(PhysAsset, FName(UTF8_TO_TCHAR(Child.c_str())), FName(UTF8_TO_TCHAR(Parent.c_str())));
				}
				if (CIdx == INDEX_NONE || CIdx < 0 || CIdx >= PhysAsset->ConstraintSetup.Num())
				{
					Session.Log(TEXT("[FAIL] configure(\"constraint\") -> constraint not found"));
					return sol::lua_nil;
				}

				UPhysicsConstraintTemplate* CS = PhysAsset->ConstraintSetup[CIdx];
				const FScopedTransaction Tx(NSLOCTEXT("AIK", "ConfigConstraint", "Configure Constraint"));
				CS->Modify();
				FConstraintInstance& CI = CS->DefaultInstance;

				// Angular limits
				sol::optional<sol::table> Swing1 = Params.get<sol::optional<sol::table>>("swing1");
				if (Swing1.has_value())
				{
					std::string Motion = Swing1.value().get_or<std::string>("motion", "limited");
					double Limit = Swing1.value().get<sol::optional<double>>("limit").value_or(45.0);
					CI.SetAngularSwing1Limit(ParseAngularMotionPhysAsset(UTF8_TO_TCHAR(Motion.c_str())), static_cast<float>(Limit));
				}
				sol::optional<sol::table> Swing2 = Params.get<sol::optional<sol::table>>("swing2");
				if (Swing2.has_value())
				{
					std::string Motion = Swing2.value().get_or<std::string>("motion", "limited");
					double Limit = Swing2.value().get<sol::optional<double>>("limit").value_or(45.0);
					CI.SetAngularSwing2Limit(ParseAngularMotionPhysAsset(UTF8_TO_TCHAR(Motion.c_str())), static_cast<float>(Limit));
				}
				sol::optional<sol::table> Twist = Params.get<sol::optional<sol::table>>("twist");
				if (Twist.has_value())
				{
					std::string Motion = Twist.value().get_or<std::string>("motion", "limited");
					double Limit = Twist.value().get<sol::optional<double>>("limit").value_or(45.0);
					CI.SetAngularTwistLimit(ParseAngularMotionPhysAsset(UTF8_TO_TCHAR(Motion.c_str())), static_cast<float>(Limit));
				}

				// Linear limits
				sol::optional<sol::table> Linear = Params.get<sol::optional<sol::table>>("linear");
				if (Linear.has_value())
				{
					sol::table L = Linear.value();
					sol::optional<std::string> XMotion = L.get<sol::optional<std::string>>("x_motion");
					sol::optional<std::string> YMotion = L.get<sol::optional<std::string>>("y_motion");
					sol::optional<std::string> ZMotion = L.get<sol::optional<std::string>>("z_motion");
					if (XMotion.has_value()) CI.ProfileInstance.LinearLimit.XMotion = ParseLinearMotionPhysAsset(UTF8_TO_TCHAR(XMotion.value().c_str()));
					if (YMotion.has_value()) CI.ProfileInstance.LinearLimit.YMotion = ParseLinearMotionPhysAsset(UTF8_TO_TCHAR(YMotion.value().c_str()));
					if (ZMotion.has_value()) CI.ProfileInstance.LinearLimit.ZMotion = ParseLinearMotionPhysAsset(UTF8_TO_TCHAR(ZMotion.value().c_str()));
					sol::optional<double> LimitVal = L.get<sol::optional<double>>("limit");
					if (LimitVal.has_value()) CI.ProfileInstance.LinearLimit.Limit = static_cast<float>(LimitVal.value());
				}

				// Disable collision
				sol::optional<bool> DisableCol = Params.get<sol::optional<bool>>("disable_collision");
				if (DisableCol.has_value()) CI.ProfileInstance.bDisableCollision = DisableCol.value();

				// Projection
				sol::optional<bool> Projection = Params.get<sol::optional<bool>>("projection");
				if (Projection.has_value()) CI.ProfileInstance.bEnableProjection = Projection.value();

				// Breakable
				sol::optional<sol::table> Breakable = Params.get<sol::optional<sol::table>>("breakable");
				if (Breakable.has_value())
				{
					sol::table B = Breakable.value();
					sol::optional<bool> LinBreak = B.get<sol::optional<bool>>("linear");
					if (LinBreak.has_value() && LinBreak.value())
					{
						CI.ProfileInstance.bLinearBreakable = true;
						CI.ProfileInstance.LinearBreakThreshold = static_cast<float>(B.get<sol::optional<double>>("linear_threshold").value_or(1000.0));
					}
					else if (LinBreak.has_value())
					{
						CI.ProfileInstance.bLinearBreakable = false;
					}
					sol::optional<bool> AngBreak = B.get<sol::optional<bool>>("angular");
					if (AngBreak.has_value() && AngBreak.value())
					{
						CI.ProfileInstance.bAngularBreakable = true;
						CI.ProfileInstance.AngularBreakThreshold = static_cast<float>(B.get<sol::optional<double>>("angular_threshold").value_or(5000.0));
					}
					else if (AngBreak.has_value())
					{
						CI.ProfileInstance.bAngularBreakable = false;
					}
				}

				// Angular drive
				sol::optional<sol::table> AngDrive = Params.get<sol::optional<sol::table>>("angular_drive");
				if (AngDrive.has_value())
				{
					sol::table D = AngDrive.value();
					sol::optional<double> Stiffness = D.get<sol::optional<double>>("stiffness");
					sol::optional<double> Damping = D.get<sol::optional<double>>("damping");
					sol::optional<double> MaxForce = D.get<sol::optional<double>>("max_force");

					if (Stiffness.has_value()) CI.ProfileInstance.AngularDrive.SlerpDrive.Stiffness = static_cast<float>(Stiffness.value());
					if (Damping.has_value()) CI.ProfileInstance.AngularDrive.SlerpDrive.Damping = static_cast<float>(Damping.value());
					if (MaxForce.has_value()) CI.ProfileInstance.AngularDrive.SlerpDrive.MaxForce = static_cast<float>(MaxForce.value());

					sol::optional<std::string> Mode = D.get<sol::optional<std::string>>("mode");
					if (Mode.has_value())
					{
						FString ModeStr = UTF8_TO_TCHAR(Mode.value().c_str());
						if (ModeStr.Equals(TEXT("slerp"), ESearchCase::IgnoreCase))
							CI.ProfileInstance.AngularDrive.AngularDriveMode = EAngularDriveMode::SLERP;
						else
							CI.ProfileInstance.AngularDrive.AngularDriveMode = EAngularDriveMode::TwistAndSwing;
					}

					sol::optional<sol::table> TargetOri = D.get<sol::optional<sol::table>>("target_orientation");
					if (TargetOri.has_value())
					{
						sol::table TO = TargetOri.value();
						CI.SetAngularOrientationTarget(FRotator(
							static_cast<float>(TO.get_or("pitch", 0.0)),
							static_cast<float>(TO.get_or("yaw", 0.0)),
							static_cast<float>(TO.get_or("roll", 0.0))).Quaternion());
					}
					sol::optional<sol::table> TargetVel = D.get<sol::optional<sol::table>>("target_velocity");
					if (TargetVel.has_value())
					{
						sol::table TV = TargetVel.value();
						CI.SetAngularVelocityTarget(FVector(
							TV.get_or("x", 0.0),
							TV.get_or("y", 0.0),
							TV.get_or("z", 0.0)));
					}
				}

				// Linear drive
				sol::optional<sol::table> LinDrive = Params.get<sol::optional<sol::table>>("linear_drive");
				if (LinDrive.has_value())
				{
					sol::table D = LinDrive.value();
					sol::optional<double> Stiffness = D.get<sol::optional<double>>("stiffness");
					sol::optional<double> Damping = D.get<sol::optional<double>>("damping");
					sol::optional<double> MaxForce = D.get<sol::optional<double>>("max_force");

					if (Stiffness.has_value())
					{
						CI.ProfileInstance.LinearDrive.XDrive.Stiffness = static_cast<float>(Stiffness.value());
						CI.ProfileInstance.LinearDrive.YDrive.Stiffness = static_cast<float>(Stiffness.value());
						CI.ProfileInstance.LinearDrive.ZDrive.Stiffness = static_cast<float>(Stiffness.value());
					}
					if (Damping.has_value())
					{
						CI.ProfileInstance.LinearDrive.XDrive.Damping = static_cast<float>(Damping.value());
						CI.ProfileInstance.LinearDrive.YDrive.Damping = static_cast<float>(Damping.value());
						CI.ProfileInstance.LinearDrive.ZDrive.Damping = static_cast<float>(Damping.value());
					}
					if (MaxForce.has_value())
					{
						CI.ProfileInstance.LinearDrive.XDrive.MaxForce = static_cast<float>(MaxForce.value());
						CI.ProfileInstance.LinearDrive.YDrive.MaxForce = static_cast<float>(MaxForce.value());
						CI.ProfileInstance.LinearDrive.ZDrive.MaxForce = static_cast<float>(MaxForce.value());
					}

					sol::optional<sol::table> TargetPos = D.get<sol::optional<sol::table>>("target_position");
					if (TargetPos.has_value())
					{
						sol::table TP = TargetPos.value();
						CI.SetLinearPositionTarget(FVector(
							TP.get_or("x", 0.0),
							TP.get_or("y", 0.0),
							TP.get_or("z", 0.0)));
					}
					sol::optional<sol::table> TargetVel = D.get<sol::optional<sol::table>>("target_velocity");
					if (TargetVel.has_value())
					{
						sol::table TV = TargetVel.value();
						CI.SetLinearVelocityTarget(FVector(
							TV.get_or("x", 0.0),
							TV.get_or("y", 0.0),
							TV.get_or("z", 0.0)));
					}
				}

				// Constraint frames (relative transform between bodies)
				sol::optional<sol::table> Frame1 = Params.get<sol::optional<sol::table>>("frame1");
				if (Frame1.has_value())
				{
					sol::table F = Frame1.value();
					sol::optional<sol::table> Pos = F.get<sol::optional<sol::table>>("position");
					if (Pos.has_value())
					{
						sol::table P = Pos.value();
						CI.Pos1 = FVector(P.get_or("x", 0.0), P.get_or("y", 0.0), P.get_or("z", 0.0));
					}
					sol::optional<sol::table> Pri = F.get<sol::optional<sol::table>>("primary_axis");
					if (Pri.has_value())
					{
						sol::table A = Pri.value();
						CI.PriAxis1 = FVector(A.get_or("x", 0.0), A.get_or("y", 0.0), A.get_or("z", 0.0)).GetSafeNormal();
					}
					sol::optional<sol::table> Sec = F.get<sol::optional<sol::table>>("secondary_axis");
					if (Sec.has_value())
					{
						sol::table A = Sec.value();
						CI.SecAxis1 = FVector(A.get_or("x", 0.0), A.get_or("y", 0.0), A.get_or("z", 0.0)).GetSafeNormal();
					}
				}
				sol::optional<sol::table> Frame2 = Params.get<sol::optional<sol::table>>("frame2");
				if (Frame2.has_value())
				{
					sol::table F = Frame2.value();
					sol::optional<sol::table> Pos = F.get<sol::optional<sol::table>>("position");
					if (Pos.has_value())
					{
						sol::table P = Pos.value();
						CI.Pos2 = FVector(P.get_or("x", 0.0), P.get_or("y", 0.0), P.get_or("z", 0.0));
					}
					sol::optional<sol::table> Pri = F.get<sol::optional<sol::table>>("primary_axis");
					if (Pri.has_value())
					{
						sol::table A = Pri.value();
						CI.PriAxis2 = FVector(A.get_or("x", 0.0), A.get_or("y", 0.0), A.get_or("z", 0.0)).GetSafeNormal();
					}
					sol::optional<sol::table> Sec = F.get<sol::optional<sol::table>>("secondary_axis");
					if (Sec.has_value())
					{
						sol::table A = Sec.value();
						CI.SecAxis2 = FVector(A.get_or("x", 0.0), A.get_or("y", 0.0), A.get_or("z", 0.0)).GetSafeNormal();
					}
				}

				CS->SetDefaultProfile(CI);
				PhysAsset->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"constraint\", index=%d)"), CIdx));
				return sol::make_object(Lua, true);
			}

			else if (FType.Equals(TEXT("constraint_profile"), ESearchCase::IgnoreCase))
			{
				// Apply a named constraint profile to a constraint
				std::string ProfileNameStr = Params.get_or<std::string>("profile", "");
				if (ProfileNameStr.empty()) { Session.Log(TEXT("[FAIL] configure(\"constraint_profile\") -> profile name required")); return sol::lua_nil; }
				FName ProfileName = FName(UTF8_TO_TCHAR(ProfileNameStr.c_str()));

				// Find constraint by {child=, parent=} or {index=}
				int32 CIdx = INDEX_NONE;
				sol::optional<double> IdxOpt = Params.get<sol::optional<double>>("index");
				if (IdxOpt.has_value())
				{
					CIdx = static_cast<int32>(IdxOpt.value());
				}
				else
				{
					std::string Child = Params.get_or<std::string>("child", "");
					std::string Parent = Params.get_or<std::string>("parent", "");
					if (Child.empty() || Parent.empty()) { Session.Log(TEXT("[FAIL] configure(\"constraint_profile\") -> {child=, parent=} or {index=} required")); return sol::lua_nil; }
					CIdx = FindConstraintBiDirectional(PhysAsset, FName(UTF8_TO_TCHAR(Child.c_str())), FName(UTF8_TO_TCHAR(Parent.c_str())));
				}
				if (CIdx == INDEX_NONE || CIdx < 0 || CIdx >= PhysAsset->ConstraintSetup.Num())
				{
					Session.Log(TEXT("[FAIL] configure(\"constraint_profile\") -> constraint not found"));
					return sol::lua_nil;
				}

				UPhysicsConstraintTemplate* CS = PhysAsset->ConstraintSetup[CIdx];

				// Verify profile exists on this constraint template
				if (!CS->ContainsConstraintProfile(ProfileName))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"constraint_profile\") -> profile '%s' not found on constraint %d"), *ProfileName.ToString(), CIdx));
					return sol::lua_nil;
				}

				// Apply profile to the default instance
				const FScopedTransaction Tx(NSLOCTEXT("AIK", "ApplyConstraintProfile", "Apply Constraint Profile"));
				CS->Modify();
				CS->ApplyConstraintProfile(ProfileName, CS->DefaultInstance, false);
				CS->UpdateProfileInstance();
				PhysAsset->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"constraint_profile\", index=%d, profile=\"%s\")"), CIdx, *ProfileName.ToString()));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> unknown type. Valid: shape, body, constraint, constraint_profile"), *FType));
			return sol::lua_nil;
		});

		// ---- weld(base_bone, add_bone) ----
		AssetObj.set_function("weld", [PhysAsset, &Session](sol::table /*self*/,
			const std::string& BaseBone, const std::string& AddBone, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			USkeletalMesh* SkelMesh = PhysAsset->GetPreviewMesh();
			if (!SkelMesh) { Session.Log(TEXT("[FAIL] weld -> no preview mesh set on physics asset")); return sol::lua_nil; }

			FString BaseStr = UTF8_TO_TCHAR(BaseBone.c_str());
			FString AddStr = UTF8_TO_TCHAR(AddBone.c_str());
			int32 BaseIdx = PhysAsset->FindBodyIndex(FName(BaseStr));
			int32 AddIdx = PhysAsset->FindBodyIndex(FName(AddStr));
			if (BaseIdx == INDEX_NONE) { Session.Log(FString::Printf(TEXT("[FAIL] weld -> base body '%s' not found"), *BaseStr)); return sol::lua_nil; }
			if (AddIdx == INDEX_NONE) { Session.Log(FString::Printf(TEXT("[FAIL] weld -> add body '%s' not found"), *AddStr)); return sol::lua_nil; }
			if (BaseIdx == AddIdx) { Session.Log(TEXT("[FAIL] weld -> cannot weld a body to itself")); return sol::lua_nil; }

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "WeldBodies", "Weld Bodies"));
			USkeletalMeshComponent* TempComp = NewObject<USkeletalMeshComponent>(GetTransientPackage());
			TempComp->SetSkeletalMesh(SkelMesh);
			TempComp->SetPhysicsAsset(PhysAsset);

			FPhysicsAssetUtils::WeldBodies(PhysAsset, BaseIdx, AddIdx, TempComp);

			TempComp->MarkAsGarbage();

			PhysAsset->UpdateBodySetupIndexMap();
			PhysAsset->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] weld(\"%s\", \"%s\") -> welded into base body"), *BaseStr, *AddStr));
			return sol::make_object(Lua, true);
		});

		// ---- get_bodies_below(bone_name, include_parent?) ----
		AssetObj.set_function("get_bodies_below", [PhysAsset, &Session](sol::table /*self*/,
			const std::string& BoneName, sol::optional<bool> IncludeParent, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			USkeletalMesh* SkelMesh = PhysAsset->GetPreviewMesh();
			if (!SkelMesh) { Session.Log(TEXT("[FAIL] get_bodies_below -> no preview mesh set on physics asset")); return sol::lua_nil; }

			FString BoneStr = UTF8_TO_TCHAR(BoneName.c_str());
			TArray<int32> BodyIndices;
			PhysAsset->GetBodyIndicesBelow(BodyIndices, FName(BoneStr), SkelMesh, IncludeParent.value_or(true));

			sol::table Result = Lua.create_table();
			for (int32 i = 0; i < BodyIndices.Num(); i++)
			{
				int32 Idx = BodyIndices[i];
				if (Idx >= 0 && Idx < PhysAsset->SkeletalBodySetups.Num() && PhysAsset->SkeletalBodySetups[Idx])
				{
					sol::table E = Lua.create_table();
					E["index"] = Idx;
					E["bone"] = TCHAR_TO_UTF8(*PhysAsset->SkeletalBodySetups[Idx]->BoneName.ToString());
					Result[i + 1] = E;
				}
			}
			Session.Log(FString::Printf(TEXT("[OK] get_bodies_below(\"%s\") -> %d bodies"), *BoneStr, BodyIndices.Num()));
			return Result;
		});

		// ---- find_constraints(bone_name) ----
		AssetObj.set_function("find_constraints", [PhysAsset, &Session](sol::table /*self*/,
			const std::string& BoneName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString BoneStr = UTF8_TO_TCHAR(BoneName.c_str());
			int32 BodyIdx = PhysAsset->FindBodyIndex(FName(BoneStr));
			if (BodyIdx == INDEX_NONE) { Session.Log(FString::Printf(TEXT("[FAIL] find_constraints -> body for bone '%s' not found"), *BoneStr)); return sol::lua_nil; }

			TArray<int32> ConstraintIndices;
			PhysAsset->BodyFindConstraints(BodyIdx, ConstraintIndices);

			sol::table Result = Lua.create_table();
			for (int32 i = 0; i < ConstraintIndices.Num(); i++)
			{
				int32 Idx = ConstraintIndices[i];
				if (Idx >= 0 && Idx < PhysAsset->ConstraintSetup.Num() && PhysAsset->ConstraintSetup[Idx])
				{
					const FConstraintInstance& CI = PhysAsset->ConstraintSetup[Idx]->DefaultInstance;
					sol::table E = Lua.create_table();
					E["index"] = Idx;
					E["child"] = TCHAR_TO_UTF8(*CI.ConstraintBone1.ToString());
					E["parent"] = TCHAR_TO_UTF8(*CI.ConstraintBone2.ToString());
					Result[i + 1] = E;
				}
			}
			Session.Log(FString::Printf(TEXT("[OK] find_constraints(\"%s\") -> %d constraints"), *BoneStr, ConstraintIndices.Num()));
			return Result;
		});

		// ---- info() — override default ----
		AssetObj.set_function("info", [PhysAsset, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			sol::table Result = Lua.create_table();
			Result["bodies"] = PhysAsset->SkeletalBodySetups.Num();
			Result["constraints"] = PhysAsset->ConstraintSetup.Num();
			Session.Log(FString::Printf(TEXT("[OK] info() -> %d bodies, %d constraints"),
				PhysAsset->SkeletalBodySetups.Num(), PhysAsset->ConstraintSetup.Num()));
			return Result;
		});
	});
}

REGISTER_LUA_BINDING(PhysicsAsset, PhysicsAssetDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindPhysicsAsset(Lua, Session);
});
