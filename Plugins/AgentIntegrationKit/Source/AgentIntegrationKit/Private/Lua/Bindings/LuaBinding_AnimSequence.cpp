#include "Lua/LuaBindingRegistry.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimData/AttributeIdentifier.h"
#include "Animation/BuiltInAttributeTypes.h"
#include "Animation/AnimCurveTypes.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/AnimTypes.h"
#include "Animation/AnimBoneCompressionSettings.h"
#include "Animation/AnimCurveCompressionSettings.h"
#include "Curves/RichCurve.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

static UClass* FindNotifyClassForAnimSeq(const FString& TypeName)
{
	if (TypeName.IsEmpty()) return nullptr;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if ((*It)->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated)) continue;
		if (!(*It)->IsChildOf(UAnimNotify::StaticClass()) && !(*It)->IsChildOf(UAnimNotifyState::StaticClass())) continue;
		FString Name = (*It)->GetName();
		if (Name.Equals(TypeName, ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("AnimNotify_") + TypeName, ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("AnimNotifyState_") + TypeName, ESearchCase::IgnoreCase))
			return *It;
	}
	return nullptr;
}

static void InitializeAnimSequenceNotifyForEditor(FAnimNotifyEvent& Event, UAnimNotify* Notify)
{
	if (Notify)
	{
		Notify->OnAnimNotifyCreatedInEditor(Event);
	}
}

static void InitializeAnimSequenceNotifyForEditor(FAnimNotifyEvent& Event, UAnimNotifyState* NotifyState)
{
	if (NotifyState)
	{
		NotifyState->OnAnimNotifyCreatedInEditor(Event);
	}
}

// Set extra properties on a notify object from a Lua params table.
// Reserved keys (name, time, type, duration, track, branching_point, trigger_chance) are skipped.
static void SetNotifyPropertiesFromParams(UObject* NotifyObj, const sol::table& Params, FLuaSessionData& Session)
{
	if (!NotifyObj) return;

	static const TSet<FString> ReservedKeys = {
		TEXT("name"), TEXT("time"), TEXT("type"), TEXT("duration"),
		TEXT("track"), TEXT("branching_point"), TEXT("trigger_chance")
	};

	for (const auto& KV : Params)
	{
		if (!KV.first.is<std::string>()) continue;
		FString Key = UTF8_TO_TCHAR(KV.first.as<std::string>().c_str());
		if (ReservedKeys.Contains(Key.ToLower())) continue;
		if (!KV.second.is<std::string>()) continue;

		FString Value = UTF8_TO_TCHAR(KV.second.as<std::string>().c_str());

		// Find property on the notify object (case-insensitive)
		FProperty* Prop = nullptr;
		for (TFieldIterator<FProperty> PropIt(NotifyObj->GetClass()); PropIt; ++PropIt)
		{
			if (PropIt->GetName().Equals(Key, ESearchCase::IgnoreCase))
			{
				Prop = *PropIt;
				break;
			}
		}

		if (!Prop)
		{
			// Try Instanced sub-objects (e.g. RootMotionModifier on MotionWarping notify)
			for (TFieldIterator<FObjectProperty> ObjIt(NotifyObj->GetClass()); ObjIt; ++ObjIt)
			{
				if (ObjIt->GetName().Equals(Key, ESearchCase::IgnoreCase))
				{
					// For instanced object properties, the value is the class name to instantiate
					UClass* SubClass = nullptr;
					for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
					{
						if ((*ClassIt)->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated)) continue;
						if (!(*ClassIt)->IsChildOf(ObjIt->PropertyClass)) continue;
						FString ClassName = (*ClassIt)->GetName();
						if (ClassName.Equals(Value, ESearchCase::IgnoreCase) ||
							ClassName.Equals(TEXT("U") + Value, ESearchCase::IgnoreCase) ||
							ClassName.Contains(Value, ESearchCase::IgnoreCase))
						{
							SubClass = *ClassIt;
							break;
						}
					}

					if (SubClass)
					{
						UObject* SubObj = NewObject<UObject>(NotifyObj, SubClass, NAME_None, RF_Transactional);
						ObjIt->SetObjectPropertyValue(ObjIt->ContainerPtrToValuePtr<void>(NotifyObj), SubObj);
						Session.Log(FString::Printf(TEXT("[OK] notify property \"%s\" = instantiated %s"), *Key, *SubClass->GetName()));
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[WARN] notify property \"%s\" -> class \"%s\" not found"), *Key, *Value));
					}
					break;
				}
			}
			continue;
		}

		const TCHAR* Result = Prop->ImportText_InContainer(*Value, NotifyObj, NotifyObj, PPF_None);
		if (Result)
		{
			Session.Log(FString::Printf(TEXT("[OK] notify property \"%s\" = \"%s\""), *Key, *Value));
		}
		else
		{
			Session.Log(FString::Printf(TEXT("[WARN] notify property \"%s\" -> failed to set \"%s\""), *Key, *Value));
		}
	}
}

static void SetSyncMarkerTrackIndex(FAnimSyncMarker& Marker, int32 TrackIndex)
{
#if WITH_EDITORONLY_DATA
	Marker.TrackIndex = TrackIndex;
#else
	(void)Marker;
	(void)TrackIndex;
#endif
}

static int32 GetSyncMarkerTrackIndex(const FAnimSyncMarker& Marker)
{
#if WITH_EDITORONLY_DATA
	return Marker.TrackIndex;
#else
	(void)Marker;
	return 0;
#endif
}

static UScriptStruct* ResolveAttributeType(const FString& TypeStr)
{
	if (TypeStr.Equals(TEXT("float"), ESearchCase::IgnoreCase))
		return FFloatAnimationAttribute::StaticStruct();
	if (TypeStr.Equals(TEXT("int"), ESearchCase::IgnoreCase) || TypeStr.Equals(TEXT("integer"), ESearchCase::IgnoreCase))
		return FIntegerAnimationAttribute::StaticStruct();
	if (TypeStr.Equals(TEXT("string"), ESearchCase::IgnoreCase))
		return FStringAnimationAttribute::StaticStruct();
	if (TypeStr.Equals(TEXT("transform"), ESearchCase::IgnoreCase))
		return FTransformAnimationAttribute::StaticStruct();
	if (TypeStr.Equals(TEXT("vector"), ESearchCase::IgnoreCase))
		return FVectorAnimationAttribute::StaticStruct();
	if (TypeStr.Equals(TEXT("quaternion"), ESearchCase::IgnoreCase) || TypeStr.Equals(TEXT("quat"), ESearchCase::IgnoreCase))
		return FQuaternionAnimationAttribute::StaticStruct();
	return nullptr;
}

static FString AttributeTypeToString(const UScriptStruct* Struct)
{
	if (!Struct) return TEXT("unknown");
	if (Struct == FFloatAnimationAttribute::StaticStruct()) return TEXT("float");
	if (Struct == FIntegerAnimationAttribute::StaticStruct()) return TEXT("int");
	if (Struct == FStringAnimationAttribute::StaticStruct()) return TEXT("string");
	if (Struct == FTransformAnimationAttribute::StaticStruct()) return TEXT("transform");
	if (Struct == FVectorAnimationAttribute::StaticStruct()) return TEXT("vector");
	if (Struct == FQuaternionAnimationAttribute::StaticStruct()) return TEXT("quaternion");
	return Struct->GetName();
}

static TArray<FLuaFunctionDoc> AnimSequenceDocs = {};

static void BindAnimSequence(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_anim_sequence", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		UAnimSequence* Seq = LoadObject<UAnimSequence>(nullptr, *FPath);
		if (!Seq) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"Element types for add/remove/list/configure:\n"
			"  notify      — anim notify event at a time\n"
			"  sync_marker — authored sync marker\n"
			"  bone_track  — bone transform track\n"
			"  curve       — float or transform curve\n"
			"  attribute   — custom bone attribute\n"
			"\n"
			"add(type, params):\n"
			"  add(\"notify\", {name=\"FootStep\", time=0.5, type=\"AnimNotify_PlaySound\", track=0})\n"
			"  add(\"notify\", {name=\"Trail\", time=0.2, type=\"AnimNotifyState_Trail\", duration=0.8})\n"
			"  add(\"sync_marker\", {name=\"Foot\", time=0.3, track=0})\n"
			"  add(\"bone_track\", {bone=\"hand_r\"})\n"
			"  add(\"curve\", {name=\"Speed\", type=\"float\"})  — type: float or transform\n"
			"  add(\"curve\", {name=\"SpeedCopy\", source=\"Speed\"})  — duplicate existing curve\n"
			"  add(\"curve_key\", {curve=\"Speed\", time=0.5, value=1.0, interp=\"cubic\", tangent_mode=\"auto\"})\n"
			"  add(\"attribute\", {bone=\"hand_r\", name=\"ImpactWeight\", type=\"float\"})\n"
			"    — type: float, int, string, transform, vector, quaternion\n"
			"\n"
			"remove(type, id):\n"
			"  remove(\"notify\", \"FootStep\") or remove(\"notify\", 1)  — by name or 1-based index\n"
			"  remove(\"sync_marker\", \"Foot\") or remove(\"sync_marker\", 1)\n"
			"  remove(\"bone_track\", \"hand_r\") — by bone name\n"
			"  remove(\"curve\", \"Speed\") or remove(\"curve\", {name=\"Speed\", type=\"transform\"})\n"
			"  remove(\"curve_key\", {curve=\"Speed\", time=0.5})\n"
			"  remove(\"attribute\", {bone=\"hand_r\", name=\"ImpactWeight\"})\n"
			"  remove(\"all_attributes\") — remove all bone attributes\n"
			"  remove(\"bone_attributes\", \"hand_r\") — remove all attributes for bone\n"
			"\n"
			"list(type):\n"
			"  list(\"notifies\"), list(\"sync_markers\"), list(\"bone_tracks\"), list(\"curves\")\n"
			"  list(\"attributes\") — all custom bone attributes\n"
			"\n"
			"configure(type, id, params):\n"
			"  configure(\"notify\", 1, {time=0.3, duration=0.5, track=1, trigger_chance=0.8})\n"
			"  configure(\"notify\", 1, {filter_type=\"LOD\", filter_lod=2, dedicated_server=false})\n"
			"  configure(\"sync_marker\", 1, {name=\"NewName\", time=0.5, track=0})\n"
			"  configure(\"sequence\", nil, {rate_scale=1.5, loop=true, interpolation=\"step\"})\n"
			"  configure(\"sequence\", nil, {additive_type=\"local_space\", ref_pose_type=\"ref_pose\"})\n"
			"  configure(\"sequence\", nil, {root_motion=true, root_motion_lock=\"ref_pose\"})\n"
			"  configure(\"sequence\", nil, {compression_error_threshold_scale=10.0, allow_frame_stripping=false})\n"
			"  configure(\"sequence\", nil, {bone_compression=\"/Path/To/Settings\", curve_compression=\"/Path/To/Settings\"})\n"
			"  configure(\"curve\", \"Speed\", {editable=true, disabled=false, metadata=false})\n"
			"\n"
			"Action methods:\n"
			"  set_bone_keys(bone, {positions={{x,y,z},...}, rotations={{p,y,r},...}, scales=...})\n"
			"  update_bone_keys(bone, start_frame, {positions=..., rotations=..., scales=...})\n"
			"  set_curve_keys(name, {{time=0, value=1, interp=\"cubic\"},...}, type?)\n"
			"  set_transform_curve_keys(name, {{time=0, location={x,y,z}, rotation={p,y,r}, scale={x,y,z}},...})\n"
			"  duplicate_curve(source_name, new_name, type?) — duplicate a curve\n"
			"  rename_curve(old_name, new_name, type?) — rename a curve\n"
			"  scale_curve(name, factor, origin?, type?) — scale values by factor around origin\n"
			"  set_curve_color(name, {r,g,b,a?}, type?) — set editor display color\n"
			"  set_frame_rate(fps) — set sampling frame rate\n"
			"  set_num_frames(count) — set total frame count\n"
			"  resize(new_frame_count, t0, t1) — resize with frame remapping\n"
			"  cleanup_bone_tracks() — remove bone tracks not in skeleton\n"
			"  info() — summary of length, frame rate, notifies, sync markers, curves, bone tracks\n";

		// ---- add(type, params) ----
		AssetObj.set_function("add", [Seq, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (FType.Equals(TEXT("notify"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"notify\") -> params required: {name=.., time=..}")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string Name = P.get_or<std::string>("name", "");
				double Time = P.get_or("time", 0.0);
				if (Name.empty()) { Session.Log(TEXT("[FAIL] add(\"notify\") -> name required")); return sol::lua_nil; }

				FString FName = UTF8_TO_TCHAR(Name.c_str());
				float PlayLength = Seq->GetPlayLength();
				if (Time < 0.0 || Time > PlayLength)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"notify\") -> time %.2f out of range (0-%.2f)"), Time, PlayLength));
					return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "AddNotify", "Add Notify"));
				Seq->Modify();

				FAnimNotifyEvent NewEvent;
				NewEvent.NotifyName = ::FName(*FName);
				NewEvent.Link(Seq, static_cast<float>(Time));
				NewEvent.TriggerTimeOffset = GetTriggerTimeOffsetForType(Seq->CalculateOffsetForNotify(static_cast<float>(Time)));
				NewEvent.TrackIndex = static_cast<int32>(P.get_or("track", 0.0));

				std::string TypeStr = P.get_or<std::string>("type", "");
				if (!TypeStr.empty())
				{
					FString FTypeStr = UTF8_TO_TCHAR(TypeStr.c_str());
					UClass* NotifyClass = FindNotifyClassForAnimSeq(FTypeStr);
					if (NotifyClass)
					{
						if (NotifyClass->IsChildOf(UAnimNotifyState::StaticClass()))
						{
							UAnimNotifyState* NS = NewObject<UAnimNotifyState>(Seq, NotifyClass, NAME_None, RF_Transactional);
							InitializeAnimSequenceNotifyForEditor(NewEvent, NS);
							NewEvent.NotifyStateClass = NS;
							double Dur = P.get_or("duration", 0.5);
							NewEvent.Duration = static_cast<float>(Dur);
							NewEvent.EndLink.Link(Seq, static_cast<float>(Time + Dur));
						}
						else
						{
							NewEvent.Notify = NewObject<UAnimNotify>(Seq, NotifyClass, NAME_None, RF_Transactional);
							InitializeAnimSequenceNotifyForEditor(NewEvent, NewEvent.Notify);
						}
					}
				}

				// Set extra properties on the notify object from params table
				// Any key that isn't a reserved param (name, time, type, duration, track, etc.)
				// gets set as a UPROPERTY on the created notify via ImportText.
				// For instanced sub-objects (e.g. RootMotionModifier), pass the class name as value.
				UObject* CreatedNotify = NewEvent.NotifyStateClass
					? static_cast<UObject*>(NewEvent.NotifyStateClass)
					: static_cast<UObject*>(NewEvent.Notify);
				if (CreatedNotify)
				{
					SetNotifyPropertiesFromParams(CreatedNotify, P, Session);
				}

#if WITH_EDITORONLY_DATA
				NewEvent.Guid = FGuid::NewGuid();
#endif
				NewEvent.TriggerWeightThreshold = ZERO_ANIMWEIGHT_THRESH;

				if (P.get_or("branching_point", false))
					NewEvent.MontageTickType = EMontageNotifyTickType::BranchingPoint;

				Seq->Notifies.Add(NewEvent);
				Seq->RefreshCacheData();
				Seq->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add(\"notify\", name=\"%s\", time=%.2f)"), *FName, Time));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("bone_track"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"bone_track\") -> {bone=..} required")); return sol::lua_nil; }
				std::string BoneName = Params.value().get_or<std::string>("bone", "");
				if (BoneName.empty()) { Session.Log(TEXT("[FAIL] add(\"bone_track\") -> bone name required")); return sol::lua_nil; }

				FString BoneStr = UTF8_TO_TCHAR(BoneName.c_str());
				IAnimationDataController& Ctrl = Seq->GetController();
				IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "AddBoneTrack", "Add Bone Track"));
				bool bOk = Ctrl.AddBoneCurve(::FName(BoneStr));
				Session.Log(FString::Printf(TEXT("[%s] add(\"bone_track\", bone=\"%s\")"), bOk ? TEXT("OK") : TEXT("FAIL"), *BoneStr));
				return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
			}
			else if (FType.Equals(TEXT("curve"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"curve\") -> {name=..} required")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string CurveName = P.get_or<std::string>("name", "");
				if (CurveName.empty()) { Session.Log(TEXT("[FAIL] add(\"curve\") -> name required")); return sol::lua_nil; }

				FString FCurveName = UTF8_TO_TCHAR(CurveName.c_str());
				std::string TypeStr = P.get_or<std::string>("type", "float");
				ERawCurveTrackTypes CurveType = FString(UTF8_TO_TCHAR(TypeStr.c_str())).Equals(TEXT("transform"), ESearchCase::IgnoreCase)
					? ERawCurveTrackTypes::RCT_Transform : ERawCurveTrackTypes::RCT_Float;

				// If source is specified, duplicate an existing curve
				std::string SourceName = P.get_or<std::string>("source", "");
				if (!SourceName.empty())
				{
					FString FSourceName = UTF8_TO_TCHAR(SourceName.c_str());
					FAnimationCurveIdentifier SourceId(::FName(*FSourceName), CurveType);
					FAnimationCurveIdentifier NewId(::FName(*FCurveName), CurveType);
					IAnimationDataController& Ctrl = Seq->GetController();
					IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "DupCurve", "Duplicate Curve"));
					bool bOk = Ctrl.DuplicateCurve(SourceId, NewId);
					Session.Log(FString::Printf(TEXT("[%s] add(\"curve\", name=\"%s\", source=\"%s\")"), bOk ? TEXT("OK") : TEXT("FAIL"), *FCurveName, *FSourceName));
					return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
				}

				FAnimationCurveIdentifier CurveId(::FName(*FCurveName), CurveType);
				IAnimationDataController& Ctrl = Seq->GetController();
				IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "AddCurve", "Add Curve"));
				bool bOk = Ctrl.AddCurve(CurveId);
				Session.Log(FString::Printf(TEXT("[%s] add(\"curve\", name=\"%s\")"), bOk ? TEXT("OK") : TEXT("FAIL"), *FCurveName));
				return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
			}
			else if (FType.Equals(TEXT("curve_key"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"curve_key\") -> {curve=.., time=.., value=..} required")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string CrvName = P.get_or<std::string>("curve", "");
				if (CrvName.empty()) { Session.Log(TEXT("[FAIL] add(\"curve_key\") -> curve name required")); return sol::lua_nil; }

				FString FCrvName = UTF8_TO_TCHAR(CrvName.c_str());
				std::string CrvTypeStr = P.get_or<std::string>("type", "float");
				ERawCurveTrackTypes CrvType = FString(UTF8_TO_TCHAR(CrvTypeStr.c_str())).Equals(TEXT("transform"), ESearchCase::IgnoreCase)
					? ERawCurveTrackTypes::RCT_Transform : ERawCurveTrackTypes::RCT_Float;

				float Time = static_cast<float>(P.get_or("time", 0.0));
				float Value = static_cast<float>(P.get_or("value", 0.0));
				FRichCurveKey Key(Time, Value);

				std::string InterpStr = P.get_or<std::string>("interp", "cubic");
				if (InterpStr == "linear") Key.InterpMode = RCIM_Linear;
				else if (InterpStr == "constant") Key.InterpMode = RCIM_Constant;
				else Key.InterpMode = RCIM_Cubic;

				std::string TangentStr = P.get_or<std::string>("tangent_mode", "auto");
				if (TangentStr == "user") Key.TangentMode = RCTM_User;
				else if (TangentStr == "break") Key.TangentMode = RCTM_Break;
				else if (TangentStr == "smart_auto") Key.TangentMode = RCTM_SmartAuto;
				else Key.TangentMode = RCTM_Auto;

				Key.ArriveTangent = static_cast<float>(P.get_or("arrive_tangent", 0.0));
				Key.LeaveTangent = static_cast<float>(P.get_or("leave_tangent", 0.0));

				FAnimationCurveIdentifier CurveId(::FName(*FCrvName), CrvType);
				IAnimationDataController& Ctrl = Seq->GetController();
				IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "SetCurveKey", "Set Curve Key"));
				bool bOk = Ctrl.SetCurveKey(CurveId, Key);
				Session.Log(FString::Printf(TEXT("[%s] add(\"curve_key\", curve=\"%s\", time=%.3f, value=%.3f)"), bOk ? TEXT("OK") : TEXT("FAIL"), *FCrvName, Time, Value));
				return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
			}

			else if (FType.Equals(TEXT("sync_marker"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"sync_marker\") -> params required: {name=.., time=..}")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string Name = P.get_or<std::string>("name", "");
				if (Name.empty()) { Session.Log(TEXT("[FAIL] add(\"sync_marker\") -> name required")); return sol::lua_nil; }

				double Time = P.get<sol::optional<double>>("time").value_or(0.0);
				int32 Track = static_cast<int32>(P.get<sol::optional<double>>("track").value_or(0.0));

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "AddSyncMarker", "Add Sync Marker"));
				Seq->Modify();

				FAnimSyncMarker NewMarker;
				NewMarker.MarkerName = ::FName(UTF8_TO_TCHAR(Name.c_str()));
				NewMarker.Time = static_cast<float>(Time);
#if WITH_EDITORONLY_DATA
				NewMarker.Guid = FGuid::NewGuid();
#endif
				SetSyncMarkerTrackIndex(NewMarker, Track);
				Seq->AuthoredSyncMarkers.Add(NewMarker);
				Seq->SortSyncMarkers();
				Seq->RefreshSyncMarkerDataFromAuthored();
				Seq->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add(\"sync_marker\", name=\"%s\", time=%.2f)"), UTF8_TO_TCHAR(Name.c_str()), Time));
				return sol::make_object(Lua, true);
			}

			else if (FType.Equals(TEXT("attribute"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"attribute\") -> {bone=.., name=.., type=..} required")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string BoneName = P.get_or<std::string>("bone", "");
				std::string AttrName = P.get_or<std::string>("name", "");
				std::string TypeStr = P.get_or<std::string>("type", "float");
				if (BoneName.empty() || AttrName.empty()) { Session.Log(TEXT("[FAIL] add(\"attribute\") -> bone and name required")); return sol::lua_nil; }

				FString FBoneName = UTF8_TO_TCHAR(BoneName.c_str());
				FString FAttrName = UTF8_TO_TCHAR(AttrName.c_str());
				FString FTypeStr = UTF8_TO_TCHAR(TypeStr.c_str());
				UScriptStruct* AttrType = ResolveAttributeType(FTypeStr);
				if (!AttrType) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"attribute\") -> unknown type \"%s\". Valid: float, int, string, transform, vector, quaternion"), *FTypeStr)); return sol::lua_nil; }

				FAnimationAttributeIdentifier AttrId = UAnimationAttributeIdentifierExtensions::CreateAttributeIdentifier(Seq, ::FName(*FAttrName), ::FName(*FBoneName), AttrType);
				if (!AttrId.IsValid()) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"attribute\") -> invalid identifier (bone \"%s\" may not exist in skeleton)"), *FBoneName)); return sol::lua_nil; }

				IAnimationDataController& Ctrl = Seq->GetController();
				IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "AddAttr", "Add Attribute"));
				bool bOk = Ctrl.AddAttribute(AttrId);
				Session.Log(FString::Printf(TEXT("[%s] add(\"attribute\", bone=\"%s\", name=\"%s\", type=\"%s\")"), bOk ? TEXT("OK") : TEXT("FAIL"), *FBoneName, *FAttrName, *FTypeStr));
				return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Valid: notify, sync_marker, bone_track, curve, curve_key, attribute"), *FType));
			return sol::lua_nil;
		});

		// ---- remove(type, id) ----
		AssetObj.set_function("remove", [Seq, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (FType.Equals(TEXT("notify"), ESearchCase::IgnoreCase))
			{
				if (!IsValid(Seq)) return sol::lua_nil;
				int32 IdxToRemove = INDEX_NONE;
				if (Id.is<int>())
				{
					IdxToRemove = Id.as<int>() - 1;
				}
				else if (Id.is<std::string>())
				{
					FString Target = UTF8_TO_TCHAR(Id.as<std::string>().c_str());
					for (int32 i = 0; i < Seq->Notifies.Num(); i++)
						if (Seq->Notifies[i].NotifyName.ToString().Equals(Target, ESearchCase::IgnoreCase)) { IdxToRemove = i; break; }
				}
				if (IdxToRemove < 0 || IdxToRemove >= Seq->Notifies.Num())
				{
					Session.Log(TEXT("[FAIL] remove(\"notify\") -> not found")); return sol::lua_nil;
				}
				FString Removed = Seq->Notifies[IdxToRemove].NotifyName.ToString();
				const FScopedTransaction Tx(NSLOCTEXT("AIK", "RemNotify", "Remove Notify"));
				Seq->Modify();
				Seq->Notifies.RemoveAt(IdxToRemove);
				Seq->RefreshCacheData();
				Seq->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove(\"notify\", \"%s\")"), *Removed));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("bone_track"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<std::string>()) { Session.Log(TEXT("[FAIL] remove(\"bone_track\") -> bone name required")); return sol::lua_nil; }
				FString BoneStr = UTF8_TO_TCHAR(Id.as<std::string>().c_str());
				IAnimationDataController& Ctrl = Seq->GetController();
				IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "RemBoneTrack", "Remove Bone Track"));
				bool bOk = Ctrl.RemoveBoneTrack(::FName(BoneStr));
				Session.Log(FString::Printf(TEXT("[%s] remove(\"bone_track\", \"%s\")"), bOk ? TEXT("OK") : TEXT("FAIL"), *BoneStr));
				return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
			}
			else if (FType.Equals(TEXT("curve"), ESearchCase::IgnoreCase))
			{
				FString FCurveName;
				ERawCurveTrackTypes CurveType = ERawCurveTrackTypes::RCT_Float;
				if (Id.is<std::string>())
				{
					FCurveName = UTF8_TO_TCHAR(Id.as<std::string>().c_str());
				}
				else if (Id.is<sol::table>())
				{
					sol::table T = Id.as<sol::table>();
					FCurveName = UTF8_TO_TCHAR(T.get_or<std::string>("name", "").c_str());
					std::string TypeStr = T.get_or<std::string>("type", "float");
					if (FString(UTF8_TO_TCHAR(TypeStr.c_str())).Equals(TEXT("transform"), ESearchCase::IgnoreCase))
						CurveType = ERawCurveTrackTypes::RCT_Transform;
				}
				if (FCurveName.IsEmpty()) { Session.Log(TEXT("[FAIL] remove(\"curve\") -> curve name required")); return sol::lua_nil; }
				FAnimationCurveIdentifier CurveId(::FName(*FCurveName), CurveType);
				IAnimationDataController& Ctrl = Seq->GetController();
				IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "RemCurve", "Remove Curve"));
				bool bOk = Ctrl.RemoveCurve(CurveId);
				Session.Log(FString::Printf(TEXT("[%s] remove(\"curve\", \"%s\")"), bOk ? TEXT("OK") : TEXT("FAIL"), *FCurveName));
				return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
			}
			else if (FType.Equals(TEXT("curve_key"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<sol::table>()) { Session.Log(TEXT("[FAIL] remove(\"curve_key\") -> {curve=.., time=..} required")); return sol::lua_nil; }
				sol::table T = Id.as<sol::table>();
				std::string CrvName = T.get_or<std::string>("curve", "");
				if (CrvName.empty()) { Session.Log(TEXT("[FAIL] remove(\"curve_key\") -> curve name required")); return sol::lua_nil; }

				FString FCrvName = UTF8_TO_TCHAR(CrvName.c_str());
				std::string CrvTypeStr = T.get_or<std::string>("type", "float");
				ERawCurveTrackTypes CrvType = FString(UTF8_TO_TCHAR(CrvTypeStr.c_str())).Equals(TEXT("transform"), ESearchCase::IgnoreCase)
					? ERawCurveTrackTypes::RCT_Transform : ERawCurveTrackTypes::RCT_Float;

				float Time = static_cast<float>(T.get_or("time", 0.0));
				FAnimationCurveIdentifier CurveId(::FName(*FCrvName), CrvType);
				IAnimationDataController& Ctrl = Seq->GetController();
				IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "RemCurveKey", "Remove Curve Key"));
				bool bOk = Ctrl.RemoveCurveKey(CurveId, Time);
				Session.Log(FString::Printf(TEXT("[%s] remove(\"curve_key\", curve=\"%s\", time=%.3f)"), bOk ? TEXT("OK") : TEXT("FAIL"), *FCrvName, Time));
				return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
			}

			else if (FType.Equals(TEXT("sync_marker"), ESearchCase::IgnoreCase))
			{
				if (!IsValid(Seq)) return sol::lua_nil;
				const FScopedTransaction Tx(NSLOCTEXT("AIK", "RemSyncMarker", "Remove Sync Marker"));
				Seq->Modify();

				if (Id.is<int>())
				{
					int32 Idx = Id.as<int>() - 1;
					if (Idx < 0 || Idx >= Seq->AuthoredSyncMarkers.Num())
					{
						Session.Log(TEXT("[FAIL] remove(\"sync_marker\") -> index out of range")); return sol::lua_nil;
					}
					FString Removed = Seq->AuthoredSyncMarkers[Idx].MarkerName.ToString();
					Seq->AuthoredSyncMarkers.RemoveAt(Idx);
					Seq->SortSyncMarkers();
					Seq->RefreshSyncMarkerDataFromAuthored();
					Seq->MarkPackageDirty();
					Session.Log(FString::Printf(TEXT("[OK] remove(\"sync_marker\", %d) -> \"%s\""), Idx + 1, *Removed));
					return sol::make_object(Lua, true);
				}
				else if (Id.is<std::string>())
				{
					FName Target(UTF8_TO_TCHAR(Id.as<std::string>().c_str()));
					TArray<FName> Names = { Target };
					Seq->RemoveSyncMarkers(Names);
					Seq->SortSyncMarkers();
					Seq->RefreshSyncMarkerDataFromAuthored();
					Seq->MarkPackageDirty();
					Session.Log(FString::Printf(TEXT("[OK] remove(\"sync_marker\", \"%s\")"), *Target.ToString()));
					return sol::make_object(Lua, true);
				}
				Session.Log(TEXT("[FAIL] remove(\"sync_marker\") -> provide name or 1-based index")); return sol::lua_nil;
			}

			else if (FType.Equals(TEXT("attribute"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<sol::table>()) { Session.Log(TEXT("[FAIL] remove(\"attribute\") -> {bone=.., name=..} required")); return sol::lua_nil; }
				sol::table T = Id.as<sol::table>();
				std::string BoneName = T.get_or<std::string>("bone", "");
				std::string AttrName = T.get_or<std::string>("name", "");
				if (BoneName.empty() || AttrName.empty()) { Session.Log(TEXT("[FAIL] remove(\"attribute\") -> bone and name required")); return sol::lua_nil; }

				FString FBoneName = UTF8_TO_TCHAR(BoneName.c_str());
				FString FAttrName = UTF8_TO_TCHAR(AttrName.c_str());

				// Find the attribute to get its full identifier (with type info)
				IAnimationDataController& AttrCtrl = Seq->GetController();
				FAnimationAttributeIdentifier FoundId;
				bool bFound = false;
				for (const FAnimatedBoneAttribute& Attr : AttrCtrl.GetModel()->GetAttributes())
				{
					if (Attr.Identifier.GetBoneName() == ::FName(*FBoneName) && Attr.Identifier.GetName() == ::FName(*FAttrName))
					{
						FoundId = Attr.Identifier;
						bFound = true;
						break;
					}
				}
				if (!bFound) { Session.Log(FString::Printf(TEXT("[FAIL] remove(\"attribute\") -> attribute \"%s\" on bone \"%s\" not found"), *FAttrName, *FBoneName)); return sol::lua_nil; }

				IAnimationDataController::FScopedBracket Bracket(&AttrCtrl, NSLOCTEXT("AIK", "RemAttr", "Remove Attribute"));
				bool bOk = AttrCtrl.RemoveAttribute(FoundId);
				Session.Log(FString::Printf(TEXT("[%s] remove(\"attribute\", bone=\"%s\", name=\"%s\")"), bOk ? TEXT("OK") : TEXT("FAIL"), *FBoneName, *FAttrName));
				return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
			}

			else if (FType.Equals(TEXT("all_attributes"), ESearchCase::IgnoreCase))
			{
				IAnimationDataController& Ctrl = Seq->GetController();
				IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "RemAllAttr", "Remove All Attributes"));
				int32 Count = Ctrl.RemoveAllAttributes();
				Session.Log(FString::Printf(TEXT("[OK] remove(\"all_attributes\") -> %d removed"), Count));
				return sol::make_object(Lua, Count);
			}

			else if (FType.Equals(TEXT("bone_attributes"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<std::string>()) { Session.Log(TEXT("[FAIL] remove(\"bone_attributes\") -> bone name required")); return sol::lua_nil; }
				FString FBoneName = UTF8_TO_TCHAR(Id.as<std::string>().c_str());
				IAnimationDataController& Ctrl = Seq->GetController();
				IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "RemBoneAttr", "Remove Bone Attributes"));
				int32 Count = Ctrl.RemoveAllAttributesForBone(::FName(*FBoneName));
				Session.Log(FString::Printf(TEXT("[OK] remove(\"bone_attributes\", \"%s\") -> %d removed"), *FBoneName, Count));
				return sol::make_object(Lua, Count);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type. Valid: notify, sync_marker, bone_track, curve, curve_key, attribute, all_attributes, bone_attributes"), *FType));
			return sol::lua_nil;
		});

		// ---- list(type?) ----
		AssetObj.set_function("list", [Seq, &Session](sol::table self,
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

			if (FType.Contains(TEXT("notif"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Seq->Notifies.Num(); i++)
				{
					const FAnimNotifyEvent& E = Seq->Notifies[i];
					sol::table Entry = Lua.create_table();
					Entry["index"] = i + 1;
					Entry["name"] = TCHAR_TO_UTF8(*E.NotifyName.ToString());
					Entry["time"] = E.GetTriggerTime();
					Entry["duration"] = E.GetDuration();
					Entry["track"] = E.TrackIndex;
					Result[i + 1] = Entry;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"notifies\") -> %d"), Seq->Notifies.Num()));
				return Result;
			}

			// For bone_tracks and curves, return summary
			if (FType.Contains(TEXT("bone"), ESearchCase::IgnoreCase) || FType.Contains(TEXT("track"), ESearchCase::IgnoreCase))
			{
				IAnimationDataController& Ctrl = Seq->GetController();
				TArray<FName> TrackNames;
				Ctrl.GetModel()->GetBoneTrackNames(TrackNames);
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < TrackNames.Num(); i++)
				{
					sol::table E = Lua.create_table();
					E["index"] = i + 1;
					E["bone"] = TCHAR_TO_UTF8(*TrackNames[i].ToString());
					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"bone_tracks\") -> %d"), TrackNames.Num()));
				return Result;
			}

			if (FType.Contains(TEXT("curve"), ESearchCase::IgnoreCase))
			{
				IAnimationDataController& Ctrl = Seq->GetController();
				const TArray<FFloatCurve>& FloatCurves = Ctrl.GetModel()->GetFloatCurves();
				const TArray<FTransformCurve>& TransformCurves = Ctrl.GetModel()->GetTransformCurves();
				sol::table Result = Lua.create_table();
				int32 Idx = 1;
				for (int32 i = 0; i < FloatCurves.Num(); i++)
				{
					sol::table E = Lua.create_table();
					E["index"] = Idx;
					E["name"] = TCHAR_TO_UTF8(*FloatCurves[i].GetName().ToString());
					E["type"] = "float";
					E["keys"] = FloatCurves[i].FloatCurve.GetNumKeys();
					E["editable"] = FloatCurves[i].GetCurveTypeFlag(AACF_Editable);
					E["disabled"] = FloatCurves[i].GetCurveTypeFlag(AACF_Disabled);
					E["metadata"] = FloatCurves[i].GetCurveTypeFlag(AACF_Metadata);
					Result[Idx++] = E;
				}
				for (int32 i = 0; i < TransformCurves.Num(); i++)
				{
					sol::table E = Lua.create_table();
					E["index"] = Idx;
					E["name"] = TCHAR_TO_UTF8(*TransformCurves[i].GetName().ToString());
					E["type"] = "transform";
					E["keys"] = TransformCurves[i].TranslationCurve.GetNumKeys();
					E["editable"] = TransformCurves[i].GetCurveTypeFlag(AACF_Editable);
					E["disabled"] = TransformCurves[i].GetCurveTypeFlag(AACF_Disabled);
					E["metadata"] = TransformCurves[i].GetCurveTypeFlag(AACF_Metadata);
					Result[Idx++] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"curves\") -> %d float, %d transform"), FloatCurves.Num(), TransformCurves.Num()));
				return Result;
			}

			if (FType.Contains(TEXT("sync"), ESearchCase::IgnoreCase) || FType.Contains(TEXT("marker"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Seq->AuthoredSyncMarkers.Num(); i++)
				{
					const FAnimSyncMarker& M = Seq->AuthoredSyncMarkers[i];
					sol::table Entry = Lua.create_table();
					Entry["index"] = i + 1;
					Entry["name"] = TCHAR_TO_UTF8(*M.MarkerName.ToString());
					Entry["time"] = M.Time;
					Entry["track_index"] = GetSyncMarkerTrackIndex(M);
					Result[i + 1] = Entry;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"sync_markers\") -> %d"), Seq->AuthoredSyncMarkers.Num()));
				return Result;
			}

			if (FType.Contains(TEXT("attrib"), ESearchCase::IgnoreCase))
			{
				IAnimationDataController& AttrCtrl = Seq->GetController();
				TArrayView<const FAnimatedBoneAttribute> Attributes = AttrCtrl.GetModel()->GetAttributes();
				sol::table Result = Lua.create_table();
				int32 Idx = 1;
				for (const FAnimatedBoneAttribute& Attr : Attributes)
				{
					sol::table Entry = Lua.create_table();
					Entry["index"] = Idx;
					Entry["bone"] = TCHAR_TO_UTF8(*Attr.Identifier.GetBoneName().ToString());
					Entry["name"] = TCHAR_TO_UTF8(*Attr.Identifier.GetName().ToString());
					Entry["type"] = TCHAR_TO_UTF8(*AttributeTypeToString(Attr.Identifier.GetType()));
					Entry["keys"] = Attr.Curve.GetNumKeys();
					Result[Idx++] = Entry;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"attributes\") -> %d"), Attributes.Num()));
				return Result;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: notifies, sync_markers, bone_tracks, curves, attributes"), *FType));
			return sol::lua_nil;
		});

		// ---- Action: set_bone_keys(bone, data) ----
		AssetObj.set_function("set_bone_keys", [Seq, &Session](sol::table /*self*/,
			const std::string& BoneName, sol::table Data, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Seq)) return sol::lua_nil;

			FString BoneStr = UTF8_TO_TCHAR(BoneName.c_str());
			::FName Bone(BoneStr);

			sol::optional<sol::table> PosOpt = Data.get<sol::optional<sol::table>>("positions");
			sol::optional<sol::table> RotOpt = Data.get<sol::optional<sol::table>>("rotations");
			if (!PosOpt.has_value() || !RotOpt.has_value())
			{
				Session.Log(TEXT("[FAIL] set_bone_keys -> \"positions\" and \"rotations\" tables required"));
				return sol::lua_nil;
			}
			sol::table Positions = PosOpt.value();
			sol::table Rotations = RotOpt.value();
			sol::optional<sol::table> ScalesOpt = Data.get<sol::optional<sol::table>>("scales");

			int32 NumKeys = static_cast<int32>(Positions.size());
			TArray<FVector3f> PosKeys;
			TArray<FQuat4f> RotKeys;
			TArray<FVector3f> ScaleKeys;
			PosKeys.Reserve(NumKeys);
			RotKeys.Reserve(NumKeys);
			ScaleKeys.Reserve(NumKeys);

			for (int32 i = 1; i <= NumKeys; i++)
			{
				sol::table P = Positions[i];
				PosKeys.Add(FVector3f(
					static_cast<float>(P.get_or(1, 0.0)),
					static_cast<float>(P.get_or(2, 0.0)),
					static_cast<float>(P.get_or(3, 0.0))));

				sol::table R = Rotations[i];
				if (R.size() >= 4)
					RotKeys.Add(FQuat4f(
						static_cast<float>(R.get_or(1, 0.0)),
						static_cast<float>(R.get_or(2, 0.0)),
						static_cast<float>(R.get_or(3, 0.0)),
						static_cast<float>(R.get_or(4, 1.0))));
				else
				{
					FRotator3f Rot(
						static_cast<float>(R.get_or(1, 0.0)),
						static_cast<float>(R.get_or(2, 0.0)),
						static_cast<float>(R.get_or(3, 0.0)));
					RotKeys.Add(Rot.Quaternion());
				}

				if (ScalesOpt.has_value())
				{
					sol::table Sc = ScalesOpt.value()[i];
					ScaleKeys.Add(FVector3f(
						static_cast<float>(Sc.get_or(1, 1.0)),
						static_cast<float>(Sc.get_or(2, 1.0)),
						static_cast<float>(Sc.get_or(3, 1.0))));
				}
				else
					ScaleKeys.Add(FVector3f(1, 1, 1));
			}

			IAnimationDataController& Ctrl = Seq->GetController();
			IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "SetBoneKeys", "Set Bone Keys"));
			bool bOk = Ctrl.SetBoneTrackKeys(Bone, PosKeys, RotKeys, ScaleKeys);
			Session.Log(FString::Printf(TEXT("[%s] set_bone_keys(\"%s\", %d keys)"), bOk ? TEXT("OK") : TEXT("FAIL"), *Bone.ToString(), NumKeys));
			return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
		});

		// ---- Action: set_curve_keys(name, keys, type?) ----
		AssetObj.set_function("set_curve_keys", [Seq, &Session](sol::table /*self*/,
			const std::string& CurveName, sol::table Keys, sol::optional<std::string> TypeOpt,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Seq)) return sol::lua_nil;

			FString FCurveName = UTF8_TO_TCHAR(CurveName.c_str());
			ERawCurveTrackTypes CurveType = (TypeOpt.has_value() && FString(UTF8_TO_TCHAR(TypeOpt.value().c_str())).Equals(TEXT("transform"), ESearchCase::IgnoreCase))
				? ERawCurveTrackTypes::RCT_Transform : ERawCurveTrackTypes::RCT_Float;

			FAnimationCurveIdentifier CurveId(::FName(*FCurveName), CurveType);
			TArray<FRichCurveKey> CurveKeys;
			for (auto& [_, val] : Keys)
			{
				sol::table K = val.as<sol::table>();
				float T = static_cast<float>(K.get_or("time", 0.0));
				float V = static_cast<float>(K.get_or("value", 0.0));
				FRichCurveKey Key(T, V);
				std::string Interp = K.get_or<std::string>("interp", "cubic");
				if (Interp == "linear") Key.InterpMode = RCIM_Linear;
				else if (Interp == "constant") Key.InterpMode = RCIM_Constant;
				else Key.InterpMode = RCIM_Cubic;
				Key.ArriveTangent = static_cast<float>(K.get_or("arrive_tangent", 0.0));
				Key.LeaveTangent = static_cast<float>(K.get_or("leave_tangent", 0.0));
				CurveKeys.Add(Key);
			}

			IAnimationDataController& Ctrl = Seq->GetController();
			IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "SetCurveKeys", "Set Curve Keys"));
			bool bOk = Ctrl.SetCurveKeys(CurveId, CurveKeys);
			Session.Log(FString::Printf(TEXT("[%s] set_curve_keys(\"%s\", %d keys)"), bOk ? TEXT("OK") : TEXT("FAIL"), *FCurveName, CurveKeys.Num()));
			return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
		});

		// ---- Action: set_frame_rate / set_num_frames ----
		AssetObj.set_function("set_frame_rate", [Seq, &Session](sol::table /*self*/, int Fps, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Seq)) return sol::lua_nil;
			IAnimationDataController& Ctrl = Seq->GetController();
			IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "SetFR", "Set Frame Rate"));
			Ctrl.SetFrameRate(FFrameRate(Fps, 1));
			Session.Log(FString::Printf(TEXT("[OK] set_frame_rate(%d)"), Fps));
			return sol::make_object(Lua, true);
		});

		AssetObj.set_function("set_num_frames", [Seq, &Session](sol::table /*self*/, int Count, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Seq)) return sol::lua_nil;
			IAnimationDataController& Ctrl = Seq->GetController();
			IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "SetFrames", "Set Num Frames"));
			Ctrl.SetNumberOfFrames(FFrameNumber(Count));
			Session.Log(FString::Printf(TEXT("[OK] set_num_frames(%d)"), Count));
			return sol::make_object(Lua, true);
		});

		// ---- Action: update_bone_keys(bone, start_frame, data) ----
		AssetObj.set_function("update_bone_keys", [Seq, &Session](sol::table /*self*/,
			const std::string& BoneName, int StartFrame, sol::table Data, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Seq)) return sol::lua_nil;

			FString BoneStr = UTF8_TO_TCHAR(BoneName.c_str());
			::FName Bone(BoneStr);

			sol::optional<sol::table> PosOpt = Data.get<sol::optional<sol::table>>("positions");
			sol::optional<sol::table> RotOpt = Data.get<sol::optional<sol::table>>("rotations");
			if (!PosOpt.has_value() || !RotOpt.has_value())
			{
				Session.Log(TEXT("[FAIL] update_bone_keys -> \"positions\" and \"rotations\" tables required"));
				return sol::lua_nil;
			}
			sol::table Positions = PosOpt.value();
			sol::table Rotations = RotOpt.value();
			sol::optional<sol::table> ScalesOpt = Data.get<sol::optional<sol::table>>("scales");

			int32 NumKeys = static_cast<int32>(Positions.size());
			TArray<FVector3f> PosKeys;
			TArray<FQuat4f> RotKeys;
			TArray<FVector3f> ScaleKeys;
			PosKeys.Reserve(NumKeys);
			RotKeys.Reserve(NumKeys);
			ScaleKeys.Reserve(NumKeys);

			for (int32 i = 1; i <= NumKeys; i++)
			{
				sol::table P = Positions[i];
				PosKeys.Add(FVector3f(
					static_cast<float>(P.get_or(1, 0.0)),
					static_cast<float>(P.get_or(2, 0.0)),
					static_cast<float>(P.get_or(3, 0.0))));

				sol::table R = Rotations[i];
				if (R.size() >= 4)
					RotKeys.Add(FQuat4f(
						static_cast<float>(R.get_or(1, 0.0)),
						static_cast<float>(R.get_or(2, 0.0)),
						static_cast<float>(R.get_or(3, 0.0)),
						static_cast<float>(R.get_or(4, 1.0))));
				else
				{
					FRotator3f Rot(
						static_cast<float>(R.get_or(1, 0.0)),
						static_cast<float>(R.get_or(2, 0.0)),
						static_cast<float>(R.get_or(3, 0.0)));
					RotKeys.Add(Rot.Quaternion());
				}

				if (ScalesOpt.has_value())
				{
					sol::table Sc = ScalesOpt.value()[i];
					ScaleKeys.Add(FVector3f(
						static_cast<float>(Sc.get_or(1, 1.0)),
						static_cast<float>(Sc.get_or(2, 1.0)),
						static_cast<float>(Sc.get_or(3, 1.0))));
				}
				else
					ScaleKeys.Add(FVector3f(1, 1, 1));
			}

			FInt32Range KeyRange(StartFrame, StartFrame + NumKeys);
			IAnimationDataController& Ctrl = Seq->GetController();
			IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "UpdateBoneKeys", "Update Bone Keys"));
			bool bOk = Ctrl.UpdateBoneTrackKeys(Bone, KeyRange, PosKeys, RotKeys, ScaleKeys);
			Session.Log(FString::Printf(TEXT("[%s] update_bone_keys(\"%s\", start=%d, %d keys)"), bOk ? TEXT("OK") : TEXT("FAIL"), *Bone.ToString(), StartFrame, NumKeys));
			return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
		});

		// ---- Action: set_transform_curve_keys(name, keys) ----
		AssetObj.set_function("set_transform_curve_keys", [Seq, &Session](sol::table /*self*/,
			const std::string& CurveName, sol::table Keys, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Seq)) return sol::lua_nil;

			FString FCurveName = UTF8_TO_TCHAR(CurveName.c_str());
			FAnimationCurveIdentifier CurveId(::FName(*FCurveName), ERawCurveTrackTypes::RCT_Transform);

			TArray<FTransform> Transforms;
			TArray<float> Times;
			int32 NumKeys = static_cast<int32>(Keys.size());
			Transforms.Reserve(NumKeys);
			Times.Reserve(NumKeys);

			for (int32 i = 1; i <= NumKeys; i++)
			{
				sol::table K = Keys[i];
				float T = static_cast<float>(K.get_or("time", 0.0));
				Times.Add(T);

				FVector Loc(0, 0, 0);
				sol::optional<sol::table> LocOpt = K.get<sol::optional<sol::table>>("location");
				if (LocOpt.has_value())
				{
					sol::table L = LocOpt.value();
					Loc = FVector(L.get_or(1, 0.0), L.get_or(2, 0.0), L.get_or(3, 0.0));
				}

				FQuat Rot = FQuat::Identity;
				sol::optional<sol::table> RotOpt = K.get<sol::optional<sol::table>>("rotation");
				if (RotOpt.has_value())
				{
					sol::table R = RotOpt.value();
					FRotator Rotator(R.get_or(1, 0.0), R.get_or(2, 0.0), R.get_or(3, 0.0));
					Rot = Rotator.Quaternion();
				}

				FVector Sc(1, 1, 1);
				sol::optional<sol::table> ScOpt = K.get<sol::optional<sol::table>>("scale");
				if (ScOpt.has_value())
				{
					sol::table ScT = ScOpt.value();
					Sc = FVector(ScT.get_or(1, 1.0), ScT.get_or(2, 1.0), ScT.get_or(3, 1.0));
				}

				Transforms.Add(FTransform(Rot, Loc, Sc));
			}

			IAnimationDataController& Ctrl = Seq->GetController();
			IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "SetTCKeys", "Set Transform Curve Keys"));
			bool bOk = Ctrl.SetTransformCurveKeys(CurveId, Transforms, Times);
			Session.Log(FString::Printf(TEXT("[%s] set_transform_curve_keys(\"%s\", %d keys)"), bOk ? TEXT("OK") : TEXT("FAIL"), *FCurveName, NumKeys));
			return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
		});

		// ---- Action: duplicate_curve(source_name, new_name, type?) ----
		AssetObj.set_function("duplicate_curve", [Seq, &Session](sol::table /*self*/,
			const std::string& SourceName, const std::string& NewName,
			sol::optional<std::string> TypeOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Seq)) return sol::lua_nil;

			ERawCurveTrackTypes CurveType = (TypeOpt.has_value() && FString(UTF8_TO_TCHAR(TypeOpt.value().c_str())).Equals(TEXT("transform"), ESearchCase::IgnoreCase))
				? ERawCurveTrackTypes::RCT_Transform : ERawCurveTrackTypes::RCT_Float;

			FString FSourceName = UTF8_TO_TCHAR(SourceName.c_str());
			FString FNewName = UTF8_TO_TCHAR(NewName.c_str());
			FAnimationCurveIdentifier SourceId(::FName(*FSourceName), CurveType);
			FAnimationCurveIdentifier NewId(::FName(*FNewName), CurveType);

			IAnimationDataController& Ctrl = Seq->GetController();
			IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "DuplicateCurve", "Duplicate Curve"));
			bool bOk = Ctrl.DuplicateCurve(SourceId, NewId);
			Session.Log(FString::Printf(TEXT("[%s] duplicate_curve(\"%s\" -> \"%s\")"), bOk ? TEXT("OK") : TEXT("FAIL"), *FSourceName, *FNewName));
			return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
		});

		// ---- Action: rename_curve(old_name, new_name, type?) ----
		AssetObj.set_function("rename_curve", [Seq, &Session](sol::table /*self*/,
			const std::string& OldName, const std::string& NewName,
			sol::optional<std::string> TypeOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Seq)) return sol::lua_nil;

			ERawCurveTrackTypes CurveType = (TypeOpt.has_value() && FString(UTF8_TO_TCHAR(TypeOpt.value().c_str())).Equals(TEXT("transform"), ESearchCase::IgnoreCase))
				? ERawCurveTrackTypes::RCT_Transform : ERawCurveTrackTypes::RCT_Float;

			FString FOldName = UTF8_TO_TCHAR(OldName.c_str());
			FString FNewName = UTF8_TO_TCHAR(NewName.c_str());
			FAnimationCurveIdentifier OldId(::FName(*FOldName), CurveType);
			FAnimationCurveIdentifier NewId(::FName(*FNewName), CurveType);

			IAnimationDataController& Ctrl = Seq->GetController();
			IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "RenameCurve", "Rename Curve"));
			bool bOk = Ctrl.RenameCurve(OldId, NewId);
			Session.Log(FString::Printf(TEXT("[%s] rename_curve(\"%s\" -> \"%s\")"), bOk ? TEXT("OK") : TEXT("FAIL"), *FOldName, *FNewName));
			return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
		});

		// ---- Action: scale_curve(name, factor, origin?, type?) ----
		AssetObj.set_function("scale_curve", [Seq, &Session](sol::table /*self*/,
			const std::string& CurveName, double Factor,
			sol::optional<double> OriginOpt, sol::optional<std::string> TypeOpt,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Seq)) return sol::lua_nil;

			ERawCurveTrackTypes CurveType = (TypeOpt.has_value() && FString(UTF8_TO_TCHAR(TypeOpt.value().c_str())).Equals(TEXT("transform"), ESearchCase::IgnoreCase))
				? ERawCurveTrackTypes::RCT_Transform : ERawCurveTrackTypes::RCT_Float;

			FString FCurveName = UTF8_TO_TCHAR(CurveName.c_str());
			FAnimationCurveIdentifier CurveId(::FName(*FCurveName), CurveType);
			float Origin = static_cast<float>(OriginOpt.value_or(0.0));

			IAnimationDataController& Ctrl = Seq->GetController();
			IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "ScaleCurve", "Scale Curve"));
			bool bOk = Ctrl.ScaleCurve(CurveId, Origin, static_cast<float>(Factor));
			Session.Log(FString::Printf(TEXT("[%s] scale_curve(\"%s\", factor=%.2f, origin=%.2f)"), bOk ? TEXT("OK") : TEXT("FAIL"), *FCurveName, Factor, Origin));
			return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
		});

		// ---- Action: set_curve_color(name, color, type?) ----
		AssetObj.set_function("set_curve_color", [Seq, &Session](sol::table /*self*/,
			const std::string& CurveName, sol::table Color,
			sol::optional<std::string> TypeOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Seq)) return sol::lua_nil;

			if (Color.size() < 3) { Session.Log(TEXT("[FAIL] set_curve_color -> color needs at least {r, g, b}")); return sol::lua_nil; }

			ERawCurveTrackTypes CurveType = (TypeOpt.has_value() && FString(UTF8_TO_TCHAR(TypeOpt.value().c_str())).Equals(TEXT("transform"), ESearchCase::IgnoreCase))
				? ERawCurveTrackTypes::RCT_Transform : ERawCurveTrackTypes::RCT_Float;

			FString FCurveName = UTF8_TO_TCHAR(CurveName.c_str());
			FAnimationCurveIdentifier CurveId(::FName(*FCurveName), CurveType);
			FLinearColor C(
				static_cast<float>(Color.get_or(1, 0.0)),
				static_cast<float>(Color.get_or(2, 0.0)),
				static_cast<float>(Color.get_or(3, 0.0)),
				static_cast<float>(Color.get_or(4, 1.0)));

			IAnimationDataController& Ctrl = Seq->GetController();
			IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "SetCurveColor", "Set Curve Color"));
			bool bOk = Ctrl.SetCurveColor(CurveId, C);
			Session.Log(FString::Printf(TEXT("[%s] set_curve_color(\"%s\", {%.2f,%.2f,%.2f,%.2f})"), bOk ? TEXT("OK") : TEXT("FAIL"), *FCurveName, C.R, C.G, C.B, C.A));
			return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
		});

		// ---- configure(type, id, params) ----
		AssetObj.set_function("configure", [Seq, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Seq)) return sol::lua_nil;
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (FType.Equals(TEXT("notify"), ESearchCase::IgnoreCase))
			{
				int32 Idx = INDEX_NONE;
				if (Id.is<int>())
				{
					Idx = Id.as<int>() - 1;
				}
				else if (Id.is<std::string>())
				{
					FString Target = UTF8_TO_TCHAR(Id.as<std::string>().c_str());
					for (int32 i = 0; i < Seq->Notifies.Num(); i++)
						if (Seq->Notifies[i].NotifyName.ToString().Equals(Target, ESearchCase::IgnoreCase)) { Idx = i; break; }
				}
				if (Idx < 0 || Idx >= Seq->Notifies.Num())
				{
					Session.Log(TEXT("[FAIL] configure(\"notify\") -> not found")); return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "ConfigNotify", "Configure Notify"));
				Seq->Modify();
				FAnimNotifyEvent& Evt = Seq->Notifies[Idx];

				sol::optional<double> NewTime = Params.get<sol::optional<double>>("time");
				if (NewTime.has_value())
				{
					float T = static_cast<float>(NewTime.value());
					Evt.Link(Seq, T);
					Evt.TriggerTimeOffset = GetTriggerTimeOffsetForType(Seq->CalculateOffsetForNotify(T));
				}

				sol::optional<double> NewDuration = Params.get<sol::optional<double>>("duration");
				if (NewDuration.has_value() && Evt.NotifyStateClass)
				{
					Evt.Duration = static_cast<float>(NewDuration.value());
					Evt.EndLink.Link(Seq, Evt.GetTriggerTime() + Evt.Duration);
				}

				sol::optional<double> NewTrack = Params.get<sol::optional<double>>("track");
				if (NewTrack.has_value())
				{
					Evt.TrackIndex = static_cast<int32>(NewTrack.value());
				}

				sol::optional<double> TriggerChance = Params.get<sol::optional<double>>("trigger_chance");
				if (TriggerChance.has_value())
				{
					Evt.NotifyTriggerChance = FMath::Clamp(static_cast<float>(TriggerChance.value()), 0.0f, 1.0f);
				}

				sol::optional<bool> BranchingPoint = Params.get<sol::optional<bool>>("branching_point");
				if (BranchingPoint.has_value())
				{
					Evt.MontageTickType = BranchingPoint.value() ? EMontageNotifyTickType::BranchingPoint : EMontageNotifyTickType::Queued;
				}

				sol::optional<double> WeightThreshold = Params.get<sol::optional<double>>("trigger_weight_threshold");
				if (WeightThreshold.has_value())
				{
					Evt.TriggerWeightThreshold = static_cast<float>(WeightThreshold.value());
				}

				sol::optional<std::string> FilterType = Params.get<sol::optional<std::string>>("filter_type");
				if (FilterType.has_value())
				{
					FString FFilter = UTF8_TO_TCHAR(FilterType.value().c_str());
					if (FFilter.Equals(TEXT("LOD"), ESearchCase::IgnoreCase))
						Evt.NotifyFilterType = ENotifyFilterType::LOD;
					else
						Evt.NotifyFilterType = ENotifyFilterType::NoFiltering;
				}

				sol::optional<double> FilterLOD = Params.get<sol::optional<double>>("filter_lod");
				if (FilterLOD.has_value())
				{
					Evt.NotifyFilterLOD = static_cast<int32>(FilterLOD.value());
				}

				sol::optional<bool> DedicatedServer = Params.get<sol::optional<bool>>("dedicated_server");
				if (DedicatedServer.has_value())
				{
					Evt.bTriggerOnDedicatedServer = DedicatedServer.value();
				}

				sol::optional<bool> TriggerFollower = Params.get<sol::optional<bool>>("trigger_on_follower");
				if (TriggerFollower.has_value())
				{
					Evt.bTriggerOnFollower = TriggerFollower.value();
				}

				Seq->SortNotifies();
				Seq->RefreshCacheData();
				Seq->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"notify\", %d)"), Idx + 1));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("sync_marker"), ESearchCase::IgnoreCase))
			{
				int32 Idx = INDEX_NONE;
				if (Id.is<int>())
				{
					Idx = Id.as<int>() - 1;
				}
				else if (Id.is<std::string>())
				{
					FName Target(UTF8_TO_TCHAR(Id.as<std::string>().c_str()));
					for (int32 i = 0; i < Seq->AuthoredSyncMarkers.Num(); i++)
						if (Seq->AuthoredSyncMarkers[i].MarkerName == Target) { Idx = i; break; }
				}
				if (Idx < 0 || Idx >= Seq->AuthoredSyncMarkers.Num())
				{
					Session.Log(TEXT("[FAIL] configure(\"sync_marker\") -> not found")); return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "ConfigSyncMarker", "Configure Sync Marker"));
				Seq->Modify();
				FAnimSyncMarker& Marker = Seq->AuthoredSyncMarkers[Idx];

				sol::optional<std::string> NewName = Params.get<sol::optional<std::string>>("name");
				if (NewName.has_value())
				{
					FName OldName = Marker.MarkerName;
					FName NewFName(UTF8_TO_TCHAR(NewName.value().c_str()));
					if (OldName != NewFName)
					{
						Seq->RenameSyncMarkers(OldName, NewFName);
					}
				}

				sol::optional<double> NewTime = Params.get<sol::optional<double>>("time");
				if (NewTime.has_value())
				{
					Marker.Time = static_cast<float>(NewTime.value());
				}

				sol::optional<double> NewTrack = Params.get<sol::optional<double>>("track");
				if (NewTrack.has_value())
				{
					SetSyncMarkerTrackIndex(Marker, static_cast<int32>(NewTrack.value()));
				}

				Seq->SortSyncMarkers();
				Seq->RefreshSyncMarkerDataFromAuthored();
				Seq->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"sync_marker\", %d)"), Idx + 1));
				return sol::make_object(Lua, true);
			}

			else if (FType.Equals(TEXT("sequence"), ESearchCase::IgnoreCase))
			{
				const FScopedTransaction Tx(NSLOCTEXT("AIK", "ConfigSeq", "Configure Sequence"));
				Seq->Modify();

				sol::optional<double> RateScale = Params.get<sol::optional<double>>("rate_scale");
				if (RateScale.has_value())
				{
					Seq->RateScale = static_cast<float>(RateScale.value());
				}

				sol::optional<bool> Loop = Params.get<sol::optional<bool>>("loop");
				if (Loop.has_value())
				{
					Seq->bLoop = Loop.value();
				}

				sol::optional<std::string> Interpolation = Params.get<sol::optional<std::string>>("interpolation");
				if (Interpolation.has_value())
				{
					FString FInterp = UTF8_TO_TCHAR(Interpolation.value().c_str());
					if (FInterp.Equals(TEXT("step"), ESearchCase::IgnoreCase))
						Seq->Interpolation = EAnimInterpolationType::Step;
					else
						Seq->Interpolation = EAnimInterpolationType::Linear;
				}

				sol::optional<std::string> AdditiveType = Params.get<sol::optional<std::string>>("additive_type");
				if (AdditiveType.has_value())
				{
					FString FAdditive = UTF8_TO_TCHAR(AdditiveType.value().c_str());
					if (FAdditive.Equals(TEXT("local_space"), ESearchCase::IgnoreCase) || FAdditive.Equals(TEXT("local"), ESearchCase::IgnoreCase))
						Seq->AdditiveAnimType = AAT_LocalSpaceBase;
					else if (FAdditive.Equals(TEXT("mesh_space"), ESearchCase::IgnoreCase) || FAdditive.Equals(TEXT("mesh"), ESearchCase::IgnoreCase))
						Seq->AdditiveAnimType = AAT_RotationOffsetMeshSpace;
					else
						Seq->AdditiveAnimType = AAT_None;
				}

				sol::optional<std::string> RefPoseType = Params.get<sol::optional<std::string>>("ref_pose_type");
				if (RefPoseType.has_value())
				{
					FString FRefPose = UTF8_TO_TCHAR(RefPoseType.value().c_str());
					if (FRefPose.Equals(TEXT("anim_frame"), ESearchCase::IgnoreCase) || FRefPose.Equals(TEXT("frame"), ESearchCase::IgnoreCase))
						Seq->RefPoseType = ABPT_AnimFrame;
					else if (FRefPose.Equals(TEXT("anim_scale"), ESearchCase::IgnoreCase) || FRefPose.Equals(TEXT("scale"), ESearchCase::IgnoreCase))
						Seq->RefPoseType = ABPT_AnimScaled;
					else
						Seq->RefPoseType = ABPT_RefPose;
				}

				sol::optional<std::string> RefPoseSeqPath = Params.get<sol::optional<std::string>>("ref_pose_seq");
				if (RefPoseSeqPath.has_value())
				{
					FString FRefPath = UTF8_TO_TCHAR(RefPoseSeqPath.value().c_str());
					if (FRefPath.IsEmpty() || FRefPath.Equals(TEXT("none"), ESearchCase::IgnoreCase))
					{
						Seq->RefPoseSeq = nullptr;
					}
					else
					{
						UAnimSequence* RefSeq = LoadObject<UAnimSequence>(nullptr, *FRefPath);
						if (RefSeq)
							Seq->RefPoseSeq = RefSeq;
						else
							Session.Log(FString::Printf(TEXT("[WARN] configure -> ref_pose_seq not found: %s"), *FRefPath));
					}
				}

				sol::optional<double> RefFrameIndex = Params.get<sol::optional<double>>("ref_frame_index");
				if (RefFrameIndex.has_value())
				{
					Seq->RefFrameIndex = static_cast<int32>(RefFrameIndex.value());
				}

				sol::optional<bool> RootMotion = Params.get<sol::optional<bool>>("root_motion");
				if (RootMotion.has_value())
				{
					Seq->bEnableRootMotion = RootMotion.value();
				}

				sol::optional<bool> ForceRootLock = Params.get<sol::optional<bool>>("force_root_lock");
				if (ForceRootLock.has_value())
				{
					Seq->bForceRootLock = ForceRootLock.value();
				}

				sol::optional<std::string> RootMotionLock = Params.get<sol::optional<std::string>>("root_motion_lock");
				if (RootMotionLock.has_value())
				{
					FString FLock = UTF8_TO_TCHAR(RootMotionLock.value().c_str());
					if (FLock.Equals(TEXT("anim_first_frame"), ESearchCase::IgnoreCase) || FLock.Equals(TEXT("first_frame"), ESearchCase::IgnoreCase))
						Seq->RootMotionRootLock = ERootMotionRootLock::AnimFirstFrame;
					else if (FLock.Equals(TEXT("zero"), ESearchCase::IgnoreCase))
						Seq->RootMotionRootLock = ERootMotionRootLock::Zero;
					else
						Seq->RootMotionRootLock = ERootMotionRootLock::RefPose;
				}

				sol::optional<bool> NormalizedRootMotion = Params.get<sol::optional<bool>>("normalized_root_motion_scale");
				if (NormalizedRootMotion.has_value())
				{
					Seq->bUseNormalizedRootMotionScale = NormalizedRootMotion.value();
				}

				sol::optional<std::string> RetargetSource = Params.get<sol::optional<std::string>>("retarget_source");
				if (RetargetSource.has_value())
				{
					Seq->RetargetSource = ::FName(UTF8_TO_TCHAR(RetargetSource.value().c_str()));
				}

#if WITH_EDITORONLY_DATA
				sol::optional<double> CompressionErrorScale = Params.get<sol::optional<double>>("compression_error_threshold_scale");
				if (CompressionErrorScale.has_value())
				{
					Seq->CompressionErrorThresholdScale = static_cast<float>(CompressionErrorScale.value());
				}

				sol::optional<bool> AllowFrameStripping = Params.get<sol::optional<bool>>("allow_frame_stripping");
				if (AllowFrameStripping.has_value())
				{
					Seq->bAllowFrameStripping = AllowFrameStripping.value();
				}
#endif

				sol::optional<std::string> BoneCompression = Params.get<sol::optional<std::string>>("bone_compression");
				if (BoneCompression.has_value())
				{
					FString BoneCompPath = UTF8_TO_TCHAR(BoneCompression.value().c_str());
					if (BoneCompPath.IsEmpty() || BoneCompPath.Equals(TEXT("none"), ESearchCase::IgnoreCase))
					{
						Seq->BoneCompressionSettings = nullptr;
					}
					else
					{
						UAnimBoneCompressionSettings* Settings = LoadObject<UAnimBoneCompressionSettings>(nullptr, *BoneCompPath);
						if (Settings)
							Seq->BoneCompressionSettings = Settings;
						else
							Session.Log(FString::Printf(TEXT("[WARN] configure -> bone_compression not found: %s"), *BoneCompPath));
					}
				}

				sol::optional<std::string> CurveCompression = Params.get<sol::optional<std::string>>("curve_compression");
				if (CurveCompression.has_value())
				{
					FString CurveCompPath = UTF8_TO_TCHAR(CurveCompression.value().c_str());
					if (CurveCompPath.IsEmpty() || CurveCompPath.Equals(TEXT("none"), ESearchCase::IgnoreCase))
					{
						Seq->CurveCompressionSettings = nullptr;
					}
					else
					{
						UAnimCurveCompressionSettings* Settings = LoadObject<UAnimCurveCompressionSettings>(nullptr, *CurveCompPath);
						if (Settings)
							Seq->CurveCompressionSettings = Settings;
						else
							Session.Log(FString::Printf(TEXT("[WARN] configure -> curve_compression not found: %s"), *CurveCompPath));
					}
				}

				Seq->PostEditChange();
				Seq->MarkPackageDirty();
				Session.Log(TEXT("[OK] configure(\"sequence\")"));
				return sol::make_object(Lua, true);
			}

			else if (FType.Equals(TEXT("curve"), ESearchCase::IgnoreCase))
			{
				FString FCurveName;
				ERawCurveTrackTypes CurveType = ERawCurveTrackTypes::RCT_Float;
				if (Id.is<std::string>())
				{
					FCurveName = UTF8_TO_TCHAR(Id.as<std::string>().c_str());
				}
				else if (Id.is<sol::table>())
				{
					sol::table T = Id.as<sol::table>();
					FCurveName = UTF8_TO_TCHAR(T.get_or<std::string>("name", "").c_str());
					std::string TypeStr = T.get_or<std::string>("type", "float");
					if (FString(UTF8_TO_TCHAR(TypeStr.c_str())).Equals(TEXT("transform"), ESearchCase::IgnoreCase))
						CurveType = ERawCurveTrackTypes::RCT_Transform;
				}
				if (FCurveName.IsEmpty()) { Session.Log(TEXT("[FAIL] configure(\"curve\") -> curve name required")); return sol::lua_nil; }

				FAnimationCurveIdentifier CurveId(::FName(*FCurveName), CurveType);
				IAnimationDataController& Ctrl = Seq->GetController();
				IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "CfgCurveFlags", "Configure Curve Flags"));

				bool bAnySet = false;
				sol::optional<bool> Editable = Params.get<sol::optional<bool>>("editable");
				if (Editable.has_value())
				{
					Ctrl.SetCurveFlag(CurveId, AACF_Editable, Editable.value());
					bAnySet = true;
				}
				sol::optional<bool> Disabled = Params.get<sol::optional<bool>>("disabled");
				if (Disabled.has_value())
				{
					Ctrl.SetCurveFlag(CurveId, AACF_Disabled, Disabled.value());
					bAnySet = true;
				}
				sol::optional<bool> Metadata = Params.get<sol::optional<bool>>("metadata");
				if (Metadata.has_value())
				{
					Ctrl.SetCurveFlag(CurveId, AACF_Metadata, Metadata.value());
					bAnySet = true;
				}

				if (!bAnySet) { Session.Log(TEXT("[FAIL] configure(\"curve\") -> specify editable, disabled, or metadata")); return sol::lua_nil; }
				Session.Log(FString::Printf(TEXT("[OK] configure(\"curve\", \"%s\")"), *FCurveName));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> unknown type. Valid: notify, sync_marker, sequence, curve"), *FType));
			return sol::lua_nil;
		});

		// ---- resize(new_frame_count, t0, t1) ----
		AssetObj.set_function("resize", [Seq, &Session](sol::table /*self*/, int NewFrameCount, int T0, int T1, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Seq)) return sol::lua_nil;
			IAnimationDataController& Ctrl = Seq->GetController();
			IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "Resize", "Resize Animation"));
			Ctrl.ResizeInFrames(FFrameNumber(NewFrameCount), FFrameNumber(T0), FFrameNumber(T1));
			Session.Log(FString::Printf(TEXT("[OK] resize(%d, t0=%d, t1=%d)"), NewFrameCount, T0, T1));
			return sol::make_object(Lua, true);
		});

		// ---- cleanup_bone_tracks() ----
		AssetObj.set_function("cleanup_bone_tracks", [Seq, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Seq)) return sol::lua_nil;
			USkeleton* Skeleton = Seq->GetSkeleton();
			if (!Skeleton) { Session.Log(TEXT("[FAIL] cleanup_bone_tracks() -> no skeleton assigned")); return sol::lua_nil; }

			IAnimationDataController& Ctrl = Seq->GetController();
			int32 TracksBefore = Ctrl.GetModel()->GetNumBoneTracks();
			IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "CleanupBoneTracks", "Cleanup Bone Tracks"));
			bool bOk = Ctrl.RemoveBoneTracksMissingFromSkeleton(Skeleton);
			int32 TracksAfter = Ctrl.GetModel()->GetNumBoneTracks();
			int32 Removed = TracksBefore - TracksAfter;
			Session.Log(FString::Printf(TEXT("[%s] cleanup_bone_tracks() -> %d tracks removed (%d remaining)"), bOk ? TEXT("OK") : TEXT("FAIL"), Removed, TracksAfter));
			return sol::make_object(Lua, Removed);
		});

		// ---- info() — override default ----
		AssetObj.set_function("info", [Seq, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Seq)) return sol::lua_nil;

			sol::table Result = Lua.create_table();
			Result["length"] = Seq->GetPlayLength();
			Result["frame_rate"] = Seq->GetSamplingFrameRate().AsDecimal();
			Result["num_frames"] = Seq->GetNumberOfSampledKeys();
			Result["notifies"] = Seq->Notifies.Num();
			Result["sync_markers"] = Seq->AuthoredSyncMarkers.Num();

			IAnimationDataController& Ctrl = Seq->GetController();
			const IAnimationDataModel* Model = Ctrl.GetModel();
			Result["bone_tracks"] = Model->GetNumBoneTracks();
			Result["float_curves"] = static_cast<int>(Model->GetFloatCurves().Num());
			Result["transform_curves"] = static_cast<int>(Model->GetTransformCurves().Num());
			Result["attributes"] = static_cast<int>(Model->GetAttributes().Num());

			Result["rate_scale"] = Seq->RateScale;
			Result["loop"] = Seq->bLoop;
			Result["root_motion"] = Seq->bEnableRootMotion;
			Result["force_root_lock"] = Seq->bForceRootLock;

			switch (Seq->Interpolation)
			{
			case EAnimInterpolationType::Step: Result["interpolation"] = "step"; break;
			default: Result["interpolation"] = "linear"; break;
			}

			switch (Seq->AdditiveAnimType)
			{
			case AAT_LocalSpaceBase: Result["additive_type"] = "local_space"; break;
			case AAT_RotationOffsetMeshSpace: Result["additive_type"] = "mesh_space"; break;
			default: Result["additive_type"] = "none"; break;
			}

			Session.Log(FString::Printf(TEXT("[OK] info() -> %.2fs, %d notifies, %d sync markers, %d bone tracks, %d curves, %d attributes"),
				Seq->GetPlayLength(), Seq->Notifies.Num(), Seq->AuthoredSyncMarkers.Num(),
				Model->GetNumBoneTracks(),
				Model->GetFloatCurves().Num(),
				Model->GetAttributes().Num()));
			return Result;
		});
	});
}

REGISTER_LUA_BINDING(AnimSequence, AnimSequenceDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindAnimSequence(Lua, Session);
});
