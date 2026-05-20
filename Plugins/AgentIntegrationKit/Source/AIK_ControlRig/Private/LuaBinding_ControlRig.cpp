#include "Lua/LuaBindingRegistry.h"

// Control Rig includes
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
#include "ControlRigBlueprintLegacy.h"
#else
#include "ControlRigBlueprint.h"
#endif
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyController.h"
#include "Rigs/RigHierarchyDefines.h"
#include "Rigs/RigHierarchyElements.h"
#include "Rigs/RigHierarchyMetadata.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
#include "Rigs/RigHierarchyComponents.h"
#endif
#include "Units/RigUnit.h"
#include "RigVMTypeUtils.h"
#include "RigVMCore/RigVMRegistry.h"
#include "ScopedTransaction.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/SavePackage.h"
#include "Modules/ModuleManager.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// Helpers
// ============================================================================

namespace
{

ERigElementType ParseElemType(const FString& T)
{
	if (T.Equals(TEXT("bone"), ESearchCase::IgnoreCase)) return ERigElementType::Bone;
	if (T.Equals(TEXT("control"), ESearchCase::IgnoreCase)) return ERigElementType::Control;
	if (T.Equals(TEXT("null"), ESearchCase::IgnoreCase) || T.Equals(TEXT("space"), ESearchCase::IgnoreCase))
		return ERigElementType::Null;
	if (T.Equals(TEXT("curve"), ESearchCase::IgnoreCase)) return ERigElementType::Curve;
	if (T.Equals(TEXT("connector"), ESearchCase::IgnoreCase)) return ERigElementType::Connector;
	if (T.Equals(TEXT("socket"), ESearchCase::IgnoreCase)) return ERigElementType::Socket;
	return ERigElementType::None;
}

ERigControlType ParseCtrlType(const FString& T)
{
	if (T.Equals(TEXT("Float"), ESearchCase::IgnoreCase)) return ERigControlType::Float;
	if (T.Equals(TEXT("Integer"), ESearchCase::IgnoreCase) || T.Equals(TEXT("Int"), ESearchCase::IgnoreCase))
		return ERigControlType::Integer;
	if (T.Equals(TEXT("Vector2D"), ESearchCase::IgnoreCase)) return ERigControlType::Vector2D;
	if (T.Equals(TEXT("Position"), ESearchCase::IgnoreCase)) return ERigControlType::Position;
	if (T.Equals(TEXT("Scale"), ESearchCase::IgnoreCase)) return ERigControlType::Scale;
	if (T.Equals(TEXT("Rotator"), ESearchCase::IgnoreCase)) return ERigControlType::Rotator;
	if (T.Equals(TEXT("Bool"), ESearchCase::IgnoreCase)) return ERigControlType::Bool;
	if (T.Equals(TEXT("ScaleFloat"), ESearchCase::IgnoreCase)) return ERigControlType::ScaleFloat;
	return ERigControlType::EulerTransform;
}

ERigControlAnimationType ParseAnimType(const FString& T)
{
	if (T.Equals(TEXT("AnimationChannel"), ESearchCase::IgnoreCase) || T.Equals(TEXT("Channel"), ESearchCase::IgnoreCase))
		return ERigControlAnimationType::AnimationChannel;
	if (T.Equals(TEXT("ProxyControl"), ESearchCase::IgnoreCase) || T.Equals(TEXT("Proxy"), ESearchCase::IgnoreCase))
		return ERigControlAnimationType::ProxyControl;
	if (T.Equals(TEXT("VisualCue"), ESearchCase::IgnoreCase))
		return ERigControlAnimationType::VisualCue;
	return ERigControlAnimationType::AnimationControl;
}

EAxis::Type ParseAxis(const FString& AxisStr)
{
	if (AxisStr.Equals(TEXT("X"), ESearchCase::IgnoreCase)) return EAxis::X;
	if (AxisStr.Equals(TEXT("Y"), ESearchCase::IgnoreCase)) return EAxis::Y;
	if (AxisStr.Equals(TEXT("Z"), ESearchCase::IgnoreCase)) return EAxis::Z;
	return EAxis::X;
}

FString ElemTypeStr(ERigElementType T)
{
	switch (T)
	{
	case ERigElementType::Bone: return TEXT("Bone");
	case ERigElementType::Control: return TEXT("Control");
	case ERigElementType::Null: return TEXT("Null");
	case ERigElementType::Curve: return TEXT("Curve");
	case ERigElementType::Connector: return TEXT("Connector");
	case ERigElementType::Socket: return TEXT("Socket");
	default: return TEXT("Unknown");
	}
}

FString CtrlTypeStr(ERigControlType T)
{
	switch (T)
	{
	case ERigControlType::Bool: return TEXT("Bool");
	case ERigControlType::Float: return TEXT("Float");
	case ERigControlType::Integer: return TEXT("Integer");
	case ERigControlType::Vector2D: return TEXT("Vector2D");
	case ERigControlType::Position: return TEXT("Position");
	case ERigControlType::Scale: return TEXT("Scale");
	case ERigControlType::Rotator: return TEXT("Rotator");
	case ERigControlType::EulerTransform: return TEXT("EulerTransform");
	case ERigControlType::ScaleFloat: return TEXT("ScaleFloat");
	default: return TEXT("Unknown");
	}
}

FString MetadataTypeStr(ERigMetadataType T)
{
	switch (T)
	{
	case ERigMetadataType::Bool: return TEXT("bool");
	case ERigMetadataType::Float: return TEXT("float");
	case ERigMetadataType::Int32: return TEXT("int32");
	case ERigMetadataType::Name: return TEXT("name");
	case ERigMetadataType::Vector: return TEXT("vector");
	case ERigMetadataType::Rotator: return TEXT("rotator");
	case ERigMetadataType::Quat: return TEXT("quat");
	case ERigMetadataType::Transform: return TEXT("transform");
	case ERigMetadataType::LinearColor: return TEXT("linearcolor");
	case ERigMetadataType::RigElementKey: return TEXT("rig_element_key");
	default: return TEXT("unknown");
	}
}

FRigElementKey FindElemKey(URigHierarchy* H, const FString& Name)
{
	if (!H || Name.IsEmpty()) return FRigElementKey();
	static const ERigElementType Types[] = {
		ERigElementType::Bone, ERigElementType::Control, ERigElementType::Null,
		ERigElementType::Curve, ERigElementType::Connector, ERigElementType::Socket
	};
	for (ERigElementType T : Types)
	{
		FRigElementKey Key(FName(*Name), T);
		if (H->Contains(Key)) return Key;
	}
	return FRigElementKey();
}

FLinearColor ParseColorStr(const FString& ColorStr)
{
	FLinearColor Color = FLinearColor::White;
	FString Trimmed = ColorStr;
	Trimmed.ReplaceInline(TEXT("("), TEXT(""), ESearchCase::CaseSensitive);
	Trimmed.ReplaceInline(TEXT(")"), TEXT(""), ESearchCase::CaseSensitive);
	TArray<FString> Parts;
	Trimmed.ParseIntoArray(Parts, TEXT(","));
	for (const FString& Part : Parts)
	{
		FString Key, Value;
		if (Part.Split(TEXT("="), &Key, &Value))
		{
			Key.TrimStartAndEndInline();
			float Val = FCString::Atof(*Value);
			if (Key.Equals(TEXT("R"), ESearchCase::IgnoreCase)) Color.R = Val;
			else if (Key.Equals(TEXT("G"), ESearchCase::IgnoreCase)) Color.G = Val;
			else if (Key.Equals(TEXT("B"), ESearchCase::IgnoreCase)) Color.B = Val;
			else if (Key.Equals(TEXT("A"), ESearchCase::IgnoreCase)) Color.A = Val;
		}
	}
	return Color;
}

// Resolve a CPP type name for variables (mirrors MCP tool logic)
FString ResolveCPPType(const FString& TypeName)
{
	if (TypeName.IsEmpty()) return FString();

	if (TypeName.Equals(TEXT("bool"), ESearchCase::IgnoreCase)) return TEXT("bool");
	if (TypeName.Equals(TEXT("float"), ESearchCase::IgnoreCase)) return TEXT("float");
	if (TypeName.Equals(TEXT("double"), ESearchCase::IgnoreCase)) return TEXT("double");
	if (TypeName.Equals(TEXT("int"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("int32"), ESearchCase::IgnoreCase)
		|| TypeName.Equals(TEXT("integer"), ESearchCase::IgnoreCase)) return TEXT("int32");
	if (TypeName.Equals(TEXT("FString"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("string"), ESearchCase::IgnoreCase)) return TEXT("FString");
	if (TypeName.Equals(TEXT("FName"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("name"), ESearchCase::IgnoreCase)) return TEXT("FName");
	if (TypeName.Equals(TEXT("FText"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("text"), ESearchCase::IgnoreCase)) return TEXT("FText");

	if (TypeName.Contains(TEXT(".")))
	{
		UScriptStruct* Struct = LoadObject<UScriptStruct>(nullptr, *TypeName);
		if (Struct) return TypeName;
		UEnum* Enum = LoadObject<UEnum>(nullptr, *TypeName);
		if (Enum) return TypeName;
		return FString();
	}

	FString StructName = TypeName;
	if (StructName.Len() > 1 && StructName[0] == TEXT('F') && FChar::IsUpper(StructName[1]))
	{
		StructName = StructName.Mid(1);
	}

	static const TMap<FString, FString> StructPaths = {
		{ TEXT("Vector"), TEXT("/Script/CoreUObject.Vector") },
		{ TEXT("Rotator"), TEXT("/Script/CoreUObject.Rotator") },
		{ TEXT("Transform"), TEXT("/Script/CoreUObject.Transform") },
		{ TEXT("Quat"), TEXT("/Script/CoreUObject.Quat") },
		{ TEXT("Vector2D"), TEXT("/Script/CoreUObject.Vector2D") },
		{ TEXT("Vector4"), TEXT("/Script/CoreUObject.Vector4") },
		{ TEXT("LinearColor"), TEXT("/Script/CoreUObject.LinearColor") },
		{ TEXT("Color"), TEXT("/Script/CoreUObject.Color") },
		{ TEXT("EulerTransform"), TEXT("/Script/AnimationCore.EulerTransform") },
		{ TEXT("RigElementKey"), TEXT("/Script/ControlRig.RigElementKey") },
	};

	if (const FString* Found = StructPaths.Find(StructName))
	{
		return *Found;
	}

	UScriptStruct* FoundStruct = FindFirstObject<UScriptStruct>(*StructName, EFindFirstObjectOptions::NativeFirst);
	if (FoundStruct)
	{
		return FoundStruct->GetPathName();
	}
	return FString();
}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
UScriptStruct* ResolveRigComponentStruct(const FString& ComponentType)
{
	if (ComponentType.IsEmpty()) return nullptr;

	if (ComponentType.StartsWith(TEXT("/Script/")) || ComponentType.Contains(TEXT(".")))
	{
		UScriptStruct* StructByPath = LoadObject<UScriptStruct>(nullptr, *ComponentType);
		if (StructByPath && StructByPath->IsChildOf(FRigBaseComponent::StaticStruct()))
		{
			return StructByPath;
		}
	}

	const FString QueryLower = ComponentType.ToLower();
	const FString QueryNoPrefix = QueryLower.StartsWith(TEXT("f")) ? QueryLower.Mid(1) : QueryLower;
	const TArray<UScriptStruct*> ComponentStructs = FRigBaseComponent::GetAllComponentScriptStructs(false);
	for (UScriptStruct* Struct : ComponentStructs)
	{
		if (!Struct) continue;
		const FString StructNameLower = Struct->GetName().ToLower();
		const FString StructNameNoPrefix = StructNameLower.StartsWith(TEXT("f")) ? StructNameLower.Mid(1) : StructNameLower;
		if (StructNameLower == QueryLower || StructNameNoPrefix == QueryNoPrefix || Struct->GetPathName().Equals(ComponentType, ESearchCase::IgnoreCase))
		{
			return Struct;
		}
	}
	return nullptr;
}
#endif

static FString GetRigUnitTooltip(const UScriptStruct* Struct)
{
#if WITH_EDITOR
	FString Tooltip = Struct ? Struct->GetMetaData(TEXT("ToolTip")) : FString();
	if (Tooltip.Len() > 120)
	{
		Tooltip = Tooltip.Left(117) + TEXT("...");
	}
	return Tooltip;
#else
	(void)Struct;
	return FString();
#endif
}

static TArray<FRigElementKey> GetAvailableSpaceKeys(const FRigControlElement* ControlElement)
{
	TArray<FRigElementKey> Result;
	if (!ControlElement)
	{
		return Result;
	}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	for (const FRigElementKeyWithLabel& Existing : ControlElement->Settings.Customization.AvailableSpaces)
	{
		Result.Add(Existing.Key);
	}
#else
	Result = ControlElement->Settings.Customization.AvailableSpaces;
#endif
	return Result;
}

static bool AddAvailableSpaceCompat(URigHierarchyController* Ctrl, FRigElementKey ControlKey, FRigElementKey SpaceKey)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	return Ctrl->AddAvailableSpace(ControlKey, SpaceKey, NAME_None, false);
#elif ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	return Ctrl->AddAvailableSpace(ControlKey, SpaceKey, false, false);
#else
	return false; // AddAvailableSpace not available in UE 5.4
#endif
}

static bool SelectElementCompat(URigHierarchyController* Ctrl, FRigElementKey Key, bool bSelect)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	return Ctrl->SelectElement(Key, bSelect, false, false);
#else
	return Ctrl->SelectElement(Key, bSelect, false);
#endif
}

static bool SetSelectionCompat(URigHierarchyController* Ctrl, const TArray<FRigElementKey>& Keys)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	return Ctrl->SetSelection(Keys, false, false);
#else
	return Ctrl->SetSelection(Keys, false);
#endif
}

static bool ClearSelectionCompat(URigHierarchyController* Ctrl)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	return Ctrl->ClearSelection(false);
#else
	return Ctrl->ClearSelection();
#endif
}

static bool AddParentCompat(URigHierarchyController* Ctrl, FRigElementKey ChildKey, FRigElementKey ParentKey, float Weight, bool bMaintainGlobal, const FString& Label)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	return Ctrl->AddParent(ChildKey, ParentKey, Weight, bMaintainGlobal, Label.IsEmpty() ? NAME_None : FName(*Label), false);
#else
	(void)Label;
	return Ctrl->AddParent(ChildKey, ParentKey, Weight, bMaintainGlobal, false);
#endif
}

static bool SupportsRigComponents()
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	return true;
#else
	return false;
#endif
}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
static FRigComponentKey AddControlRigComponentCompat(URigHierarchyController* Ctrl, UScriptStruct* ComponentStruct, const FString& Name, FRigElementKey ElementKey, const FString& Content)
{
	return Ctrl->AddComponent(ComponentStruct, FName(*Name), ElementKey, Content, false, false);
}

static bool RemoveControlRigComponentCompat(URigHierarchyController* Ctrl, FRigComponentKey ComponentKey)
{
	return Ctrl->RemoveComponent(ComponentKey, false, false);
}

static FRigComponentKey RenameControlRigComponentCompat(URigHierarchyController* Ctrl, FRigComponentKey ComponentKey, const FString& NewName)
{
	return Ctrl->RenameComponent(ComponentKey, FName(*NewName), false, false, true);
}

static FRigComponentKey ReparentControlRigComponentCompat(URigHierarchyController* Ctrl, FRigComponentKey ComponentKey, FRigElementKey NewElementKey)
{
	return Ctrl->ReparentComponent(ComponentKey, NewElementKey, false, false, true);
}
#endif

} // namespace

// ============================================================================
// Lua Binding
// ============================================================================

static TArray<FLuaFunctionDoc> ControlRigDocs = {};

static void BindControlRig(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_control_rig", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());

		UControlRigBlueprint* BP = LoadObject<UControlRigBlueprint>(nullptr, *FPath);
		if (!BP) return;

		URigHierarchy* Hierarchy = BP->Hierarchy;
		if (!Hierarchy) return;
		URigHierarchyController* Ctrl = Hierarchy->GetController(true);
		if (!Ctrl) return;

		// ---- save() ----
		TWeakObjectPtr<UObject> WeakBP = BP;
		AssetObj.set_function("save", [WeakBP, FPath, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UObject* Asset = WeakBP.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] save -> asset no longer valid"));
				return sol::lua_nil;
			}
			UPackage* Package = Asset->GetOutermost();
			if (!Package)
			{
				Session.Log(TEXT("[FAIL] save -> no package"));
				return sol::lua_nil;
			}
			FString PackageFilename;
			if (!FPackageName::DoesPackageExist(Package->GetName(), &PackageFilename))
			{
				PackageFilename = FPackageName::LongPackageNameToFilename(Package->GetName(),
					FPackageName::GetAssetPackageExtension());
			}
			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Standalone;
			SaveArgs.Error = GWarn;
			FSavePackageResultStruct SaveResult = UPackage::Save(Package, Asset, *PackageFilename, SaveArgs);
			bool bSuccess = (SaveResult.Result == ESavePackageResult::Success);
			if (bSuccess)
				Session.Log(FString::Printf(TEXT("[OK] save(\"%s\")"), *FPath));
			else
				Session.Log(FString::Printf(TEXT("[FAIL] save(\"%s\") -> save failed"), *FPath));
			return sol::make_object(Lua, bSuccess);
		});

		AssetObj["_help_text"] =
			"Element types for add/remove/list:\n"
			"  bone      — skeleton bone\n"
			"  control   — animation control (Float/Int/Position/Rotator/EulerTransform/...)\n"
			"  null      — null/space element\n"
			"  curve     — animation curve\n"
			"  connector — connector element\n"
			"  socket    — socket element\n"
			"  variable  — blueprint member variable\n"
			"\n"
			"add(type, params):\n"
			"  add(\"bone\", {name=\"MyBone\", parent=\"root\", transform={location={x=0,y=0,z=0}}})\n"
			"  add(\"control\", {name=\"CTRL_Hand\", parent=\"root\", control_type=\"EulerTransform\", anim_type=\"AnimationControl\"})\n"
			"  add(\"null\", {name=\"Space_IK\", parent=\"root\"})\n"
			"  add(\"curve\", {name=\"MyCurve\", value=0.5})\n"
			"  add(\"variable\", {name=\"Speed\", type=\"float\", default_value=\"0.0\"})\n"
			"\n"
			"remove(type, id):\n"
			"  remove(\"bone\", \"MyBone\")\n"
			"  remove(\"variable\", \"Speed\")\n"
			"\n"
			"configure(type, id, params):\n"
			"  configure(\"control\", \"CTRL_Hand\", {control_type=\"Position\", shape_visible=true})\n"
			"  configure(\"variable\", \"Speed\", {value=\"1.0\"})\n"
			"\n"
			"list(type):\n"
			"  list(\"bones\"), list(\"controls\"), list(\"nulls\"), list(\"all\"), list(\"variables\")\n"
			"  list(\"selection\") — currently selected elements\n"
			"  list(\"metadata\") or list(\"metadata\", \"ElementName\") — element metadata\n"
			"  list(\"unit_types\") or list(\"unit_types\", \"IK\") — search RigUnit types\n"
			"  list(\"tags\") or list(\"tags\", \"ElementName\") — element tags\n"
			"\n"
			"Action methods:\n"
			"  reparent(name, new_parent) — reparent element\n"
			"  rename(name, new_name) — rename element\n"
			"  set_display_name(control, display_name, {rename_element=false}) — set control display name\n"
			"  import_hierarchy({skeleton=\"/Game/..\"}) or ({skeletal_mesh=\"/Game/..\"})\n"
			"  import_curves({skeleton=\"/Game/..\"}) or ({skeletal_mesh=\"/Game/..\", namespace=\"\"}) — import curves from skeleton/mesh\n"
			"  import_sockets({skeletal_mesh=\"/Game/..\"}) — import sockets from skeletal mesh (replace_existing=true, remove_obsolete=true)\n"
			"  export_text({\"elem1\", \"elem2\"}) — export elements to text\n"
			"  import_text(content, {replace_existing=false}) — import elements from text\n"
			"  duplicate({\"elem1\", \"elem2\"}) — duplicate elements\n"
			"  mirror({elements={\"Arm_L\"}, search=\"_L\", replace=\"_R\", mirror_axis=\"X\", axis_to_flip=\"Z\"})\n"
			"  reorder(name, new_index) — reorder element among siblings\n"
			"  set_spaces(control, {spaces={\"Bone1\",\"Bone2\"}, clear=false, active_space=\"Bone1\"})\n"
			"  set_metadata(element, name, type, value) — set metadata on element\n"
			"  remove_metadata(element, name) — remove metadata from element\n"
			"  set_tag(element, tag) — add a tag to an element\n"
			"  remove_tag(element, tag) — remove a tag from an element\n"
			"  select({\"elem1\", \"elem2\"}) — select hierarchy elements\n"
			"  deselect({\"elem1\"}) — deselect hierarchy elements\n"
			"  set_selection({\"elem1\", \"elem2\"}) — replace full selection\n"
			"  clear_selection() — clear hierarchy selection\n"
			"  add_parent(child, parent, {weight=0, maintain_global=true}) — multi-parent add\n"
			"  remove_parent(child, parent, {maintain_global=true}) — remove specific parent\n"
			"  clear_parents(child, {maintain_global=true}) — remove all parents\n"
			"  add_component(element, {name, component_type, content}) — add hierarchy component (5.6+)\n"
			"  remove_component(element, name) — remove component (5.6+)\n"
			"  rename_component(element, name, new_name) — rename component (5.6+)\n"
			"  reparent_component(element, name, new_element) — reparent component (5.6+)\n"
			"  set_component_content(element, name, content) — update component content (5.6+)\n"
			"  add_animation_channel({name, parent_control, control_type}) — add animation channel under a control\n"
			"  add_channel_host(channel, host) — add a host to an animation channel\n"
			"  remove_channel_host(channel, host) — remove a host from an animation channel\n"
			"  get_control_value(control, {value_type=\"current\"}) — read control value\n"
			"  set_control_value(control, {x=0,y=0,z=0}, {value_type=\"current\"}) — set control value\n"
			"  info() — summary of hierarchy\n";

		// ---- add(type, params) ----
		AssetObj.set_function("add", [BP, Hierarchy, Ctrl, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (!Params.has_value()) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> params required"), *FType)); return sol::lua_nil; }
			sol::table P = Params.value();
			std::string Name = P.get_or<std::string>("name", "");
			if (Name.empty()) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> name required"), *FType)); return sol::lua_nil; }

			FString FElemName = UTF8_TO_TCHAR(Name.c_str());
			std::string ParentStr = P.get_or<std::string>("parent", "");
			FRigElementKey ParentKey;
			if (!ParentStr.empty())
			{
				ParentKey = FindElemKey(Hierarchy, UTF8_TO_TCHAR(ParentStr.c_str()));
				if (!ParentKey.IsValid()) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> parent '%s' not found"), *FType, UTF8_TO_TCHAR(ParentStr.c_str()))); return sol::lua_nil; }
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "AddCRElement", "Add ControlRig Element"));
			static_cast<UObject*>(BP)->Modify();

			// Parse optional transform from params
			FTransform Transform = FTransform::Identity;
			sol::optional<sol::table> TransOpt = P.get<sol::optional<sol::table>>("transform");
			if (TransOpt.has_value())
			{
				sol::table T = TransOpt.value();
				sol::optional<sol::table> Loc = T.get<sol::optional<sol::table>>("location");
				if (Loc.has_value()) Transform.SetLocation(FVector(Loc.value().get_or("x", 0.0), Loc.value().get_or("y", 0.0), Loc.value().get_or("z", 0.0)));
				sol::optional<sol::table> Rot = T.get<sol::optional<sol::table>>("rotation");
				if (Rot.has_value()) Transform.SetRotation(FRotator(Rot.value().get_or("pitch", 0.0), Rot.value().get_or("yaw", 0.0), Rot.value().get_or("roll", 0.0)).Quaternion());
				sol::optional<sol::table> Scl = T.get<sol::optional<sol::table>>("scale");
				if (Scl.has_value()) Transform.SetScale3D(FVector(Scl.value().get_or("x", 1.0), Scl.value().get_or("y", 1.0), Scl.value().get_or("z", 1.0)));
			}

			ERigElementType ElemType = ParseElemType(FType);

			if (ElemType == ERigElementType::Bone)
			{
				FRigElementKey Key = Ctrl->AddBone(FName(*FElemName), ParentKey, Transform, true, ERigBoneType::User, false);
				if (!Key.IsValid()) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"bone\") -> failed to add '%s'"), *FElemName)); return sol::lua_nil; }
				BP->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add(\"bone\", name=\"%s\")"), *FElemName));
				return sol::make_object(Lua, true);
			}
			else if (ElemType == ERigElementType::Control)
			{
				FRigControlSettings Settings;
				std::string CtrlTypeStr2 = P.get_or<std::string>("control_type", "EulerTransform");
				Settings.ControlType = ParseCtrlType(UTF8_TO_TCHAR(CtrlTypeStr2.c_str()));
				std::string AnimTypeStr = P.get_or<std::string>("anim_type", "AnimationControl");
				Settings.AnimationType = ParseAnimType(UTF8_TO_TCHAR(AnimTypeStr.c_str()));
				Settings.bShapeVisible = P.get_or("shape_visible", true);
				Settings.ShapeColor = FLinearColor::Red;

				std::string DisplayName = P.get_or<std::string>("display_name", "");
				if (!DisplayName.empty()) Settings.DisplayName = FName(UTF8_TO_TCHAR(DisplayName.c_str()));

				FRigControlValue Value;
				FRigElementKey Key = Ctrl->AddControl(FName(*FElemName), ParentKey, Settings, Value, FTransform::Identity, FTransform::Identity, false);
				if (!Key.IsValid()) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"control\") -> failed to add '%s'"), *FElemName)); return sol::lua_nil; }
				BP->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add(\"control\", name=\"%s\", type=\"%s\")"), *FElemName, UTF8_TO_TCHAR(CtrlTypeStr2.c_str())));
				return sol::make_object(Lua, true);
			}
			else if (ElemType == ERigElementType::Null)
			{
				FRigElementKey Key = Ctrl->AddNull(FName(*FElemName), ParentKey, Transform, true, false);
				if (!Key.IsValid()) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"null\") -> failed to add '%s'"), *FElemName)); return sol::lua_nil; }
				BP->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add(\"null\", name=\"%s\")"), *FElemName));
				return sol::make_object(Lua, true);
			}
			else if (ElemType == ERigElementType::Curve)
			{
				float Value = static_cast<float>(P.get_or("value", 0.0));
				FRigElementKey Key = Ctrl->AddCurve(FName(*FElemName), Value, false);
				if (!Key.IsValid()) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"curve\") -> failed to add '%s'"), *FElemName)); return sol::lua_nil; }
				BP->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add(\"curve\", name=\"%s\")"), *FElemName));
				return sol::make_object(Lua, true);
			}
			else if (ElemType == ERigElementType::Connector)
			{
				FRigConnectorSettings ConnSettings;
				FRigElementKey Key = Ctrl->AddConnector(FName(*FElemName), ConnSettings, false);
				if (!Key.IsValid()) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"connector\") -> failed to add '%s'"), *FElemName)); return sol::lua_nil; }
				BP->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add(\"connector\", name=\"%s\")"), *FElemName));
				return sol::make_object(Lua, true);
			}
			else if (ElemType == ERigElementType::Socket)
			{
				FRigElementKey Key = Ctrl->AddSocket(FName(*FElemName), ParentKey, Transform, true, FLinearColor::White, TEXT(""), false);
				if (!Key.IsValid()) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"socket\") -> failed to add '%s'"), *FElemName)); return sol::lua_nil; }
				BP->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add(\"socket\", name=\"%s\")"), *FElemName));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("variable"), ESearchCase::IgnoreCase))
			{
				std::string VarType = P.get_or<std::string>("type", "float");
				FString FVarType = UTF8_TO_TCHAR(VarType.c_str());
				std::string DefaultVal = P.get_or<std::string>("default_value", "");
				FString FDefaultVal = UTF8_TO_TCHAR(DefaultVal.c_str());

				FString CPPType = ResolveCPPType(FVarType);
				if (CPPType.IsEmpty())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"variable\") -> unknown type '%s'"), *FVarType));
					return sol::lua_nil;
				}

				// Validate non-primitive types before calling AddMemberVariable
				bool bIsPrimitive = (CPPType == TEXT("bool") || CPPType == TEXT("float") || CPPType == TEXT("double")
					|| CPPType == TEXT("int32") || CPPType == TEXT("FString") || CPPType == TEXT("FName") || CPPType == TEXT("FText"));

				UScriptStruct* ResolvedStruct = nullptr;
				UEnum* ResolvedEnum = nullptr;
				if (!bIsPrimitive)
				{
					ResolvedStruct = LoadObject<UScriptStruct>(nullptr, *CPPType);
					if (!ResolvedStruct)
					{
						ResolvedEnum = LoadObject<UEnum>(nullptr, *CPPType);
					}
					if (!ResolvedStruct && !ResolvedEnum)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] add(\"variable\") -> type '%s' could not be loaded"), *CPPType));
						return sol::lua_nil;
					}
				}

				FName Result = BP->AddMemberVariable(FName(*FElemName), CPPType, false, false, FDefaultVal);
				if (Result.IsNone())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"variable\") -> failed to add '%s'"), *FElemName));
					return sol::lua_nil;
				}

				// Register struct/enum in RigVM registry
				if (ResolvedStruct)
				{
					FString StructCPPName = RigVMTypeUtils::GetUniqueStructTypeName(ResolvedStruct);
					FRigVMRegistry::Get().FindOrAddType(FRigVMTemplateArgumentType(*StructCPPName, ResolvedStruct));
				}
				else if (ResolvedEnum)
				{
					FString EnumCPPName = RigVMTypeUtils::CPPTypeFromEnum(ResolvedEnum);
					FRigVMRegistry::Get().FindOrAddType(FRigVMTemplateArgumentType(*EnumCPPName, ResolvedEnum));
				}

				BP->RequestAutoVMRecompilation();
				BP->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add(\"variable\", name=\"%s\", type=\"%s\")"), *FElemName, *CPPType));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Valid: bone, control, null, curve, connector, socket, variable"), *FType));
			return sol::lua_nil;
		});

		// ---- remove(type, id) ----
		AssetObj.set_function("remove", [BP, Hierarchy, Ctrl, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (!Id.is<std::string>()) { Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> name required"), *FType)); return sol::lua_nil; }
			FString Name = UTF8_TO_TCHAR(Id.as<std::string>().c_str());

			if (FType.Equals(TEXT("variable"), ESearchCase::IgnoreCase))
			{
				const FScopedTransaction Tx(NSLOCTEXT("AIK", "RemCRVar", "Remove ControlRig Variable"));
				static_cast<UObject*>(BP)->Modify();
				if (BP->RemoveMemberVariable(FName(*Name)))
				{
					BP->MarkPackageDirty();
					Session.Log(FString::Printf(TEXT("[OK] remove(\"variable\", \"%s\")"), *Name));
					return sol::make_object(Lua, true);
				}
				Session.Log(FString::Printf(TEXT("[FAIL] remove(\"variable\", \"%s\") -> not found"), *Name));
				return sol::lua_nil;
			}

			FRigElementKey Key = FindElemKey(Hierarchy, Name);
			if (!Key.IsValid()) { Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> '%s' not found"), *FType, *Name)); return sol::lua_nil; }

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "RemCRElement", "Remove ControlRig Element"));
			static_cast<UObject*>(BP)->Modify();
			bool bOK = Ctrl->RemoveElement(Key, false);
			if (!bOK) { Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> failed to remove '%s'"), *FType, *Name)); return sol::lua_nil; }
			BP->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] remove(\"%s\", \"%s\")"), *FType, *Name));
			return sol::make_object(Lua, true);
		});

		// ---- configure(type, id, params) ----
		AssetObj.set_function("configure", [BP, Hierarchy, Ctrl, &Session](sol::table /*self*/,
			const std::string& Type, const std::string& Id, sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());
			FString FId = UTF8_TO_TCHAR(Id.c_str());

			// --- Variable configure ---
			if (FType.Equals(TEXT("variable"), ESearchCase::IgnoreCase))
			{
				std::string ValStr = Params.get_or<std::string>("value", "");
				if (ValStr.empty())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"variable\", \"%s\") -> value required"), *FId));
					return sol::lua_nil;
				}
				FString FVal = UTF8_TO_TCHAR(ValStr.c_str());

				// Find the variable to get its type
				TArray<FRigVMGraphVariableDescription> MemberVars = BP->GetMemberVariables();
				const FRigVMGraphVariableDescription* Found = nullptr;
				for (const FRigVMGraphVariableDescription& Desc : MemberVars)
				{
					if (Desc.Name == FName(*FId))
					{
						Found = &Desc;
						break;
					}
				}
				if (!Found)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"variable\", \"%s\") -> not found"), *FId));
					return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "CfgCRVar", "Configure ControlRig Variable"));
				static_cast<UObject*>(BP)->Modify();
				if (BP->ChangeMemberVariableType(FName(*FId), Found->CPPType, false, false, FVal))
				{
					BP->MarkPackageDirty();
					Session.Log(FString::Printf(TEXT("[OK] configure(\"variable\", \"%s\", value=\"%s\")"), *FId, *FVal.Left(100)));
					return sol::make_object(Lua, true);
				}
				Session.Log(FString::Printf(TEXT("[FAIL] configure(\"variable\", \"%s\") -> failed to set default"), *FId));
				return sol::lua_nil;
			}

			// --- Bone/Null/Socket/Connector configure (transform) ---
			if (FType.Equals(TEXT("bone"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("null"), ESearchCase::IgnoreCase)
				|| FType.Equals(TEXT("socket"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("connector"), ESearchCase::IgnoreCase))
			{
				ERigElementType ElemType = ParseElemType(FType);
				FRigElementKey Key(FName(*FId), ElemType);
				if (!Hierarchy->Contains(Key)) { Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\", \"%s\") -> not found"), *FType, *FId)); return sol::lua_nil; }

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "CfgCRElem", "Configure ControlRig Element"));
				static_cast<UObject*>(BP)->Modify();

				// Set initial transform
				sol::optional<sol::table> TransOpt = Params.get<sol::optional<sol::table>>("transform");
				if (TransOpt.has_value())
				{
					sol::table T = TransOpt.value();
					FTransform Transform = FTransform::Identity;
					sol::optional<sol::table> Loc = T.get<sol::optional<sol::table>>("location");
					if (Loc.has_value()) Transform.SetLocation(FVector(Loc.value().get_or("x", 0.0), Loc.value().get_or("y", 0.0), Loc.value().get_or("z", 0.0)));
					sol::optional<sol::table> Rot = T.get<sol::optional<sol::table>>("rotation");
					if (Rot.has_value()) Transform.SetRotation(FRotator(Rot.value().get_or("pitch", 0.0), Rot.value().get_or("yaw", 0.0), Rot.value().get_or("roll", 0.0)).Quaternion());
					sol::optional<sol::table> Scl = T.get<sol::optional<sol::table>>("scale");
					if (Scl.has_value()) Transform.SetScale3D(FVector(Scl.value().get_or("x", 1.0), Scl.value().get_or("y", 1.0), Scl.value().get_or("z", 1.0)));
					Hierarchy->SetInitialGlobalTransform(Hierarchy->GetIndex(Key), Transform);
				}

				BP->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"%s\", \"%s\")"), *FType, *FId));
				return sol::make_object(Lua, true);
			}

			// --- Control configure ---
			if (!FType.Equals(TEXT("control"), ESearchCase::IgnoreCase))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> unknown type. Valid: control, variable, bone, null, socket, connector"), *FType));
				return sol::lua_nil;
			}

			FRigElementKey Key(FName(*FId), ERigElementType::Control);
			if (!Hierarchy->Contains(Key)) { Session.Log(FString::Printf(TEXT("[FAIL] configure(\"control\") -> '%s' not found"), *FId)); return sol::lua_nil; }

			FRigControlElement* Elem = Cast<FRigControlElement>(Hierarchy->Find(Key));
			if (!Elem) { Session.Log(FString::Printf(TEXT("[FAIL] configure(\"control\") -> element cast failed for '%s'"), *FId)); return sol::lua_nil; }

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "CfgCRCtrl", "Configure ControlRig Control"));
			static_cast<UObject*>(BP)->Modify();

			FRigControlSettings Settings = Elem->Settings;

			sol::optional<std::string> CtrlOpt = Params.get<sol::optional<std::string>>("control_type");
			if (CtrlOpt.has_value()) Settings.ControlType = ParseCtrlType(UTF8_TO_TCHAR(CtrlOpt.value().c_str()));

			sol::optional<std::string> AnimOpt = Params.get<sol::optional<std::string>>("anim_type");
			if (AnimOpt.has_value()) Settings.AnimationType = ParseAnimType(UTF8_TO_TCHAR(AnimOpt.value().c_str()));

			sol::optional<bool> ShapeVis = Params.get<sol::optional<bool>>("shape_visible");
			if (ShapeVis.has_value()) Settings.bShapeVisible = ShapeVis.value();

			sol::optional<std::string> DispName = Params.get<sol::optional<std::string>>("display_name");
			if (DispName.has_value()) Settings.DisplayName = FName(UTF8_TO_TCHAR(DispName.value().c_str()));

			sol::optional<std::string> ShapeName = Params.get<sol::optional<std::string>>("shape_name");
			if (ShapeName.has_value()) Settings.ShapeName = FName(UTF8_TO_TCHAR(ShapeName.value().c_str()));

			sol::optional<std::string> ShapeColor = Params.get<sol::optional<std::string>>("shape_color");
			if (ShapeColor.has_value()) Settings.ShapeColor = ParseColorStr(UTF8_TO_TCHAR(ShapeColor.value().c_str()));

			sol::optional<bool> DrawLimits = Params.get<sol::optional<bool>>("draw_limits");
			if (DrawLimits.has_value()) Settings.bDrawLimits = DrawLimits.value();

			sol::optional<bool> GroupParent = Params.get<sol::optional<bool>>("group_with_parent");
			if (GroupParent.has_value()) Settings.bGroupWithParentControl = GroupParent.value();

			sol::optional<bool> RestrictSpace = Params.get<sol::optional<bool>>("restrict_space_switching");
			if (RestrictSpace.has_value()) Settings.bRestrictSpaceSwitching = RestrictSpace.value();

			// Limits — per-axis enable (array of {min, max} booleans)
			sol::optional<sol::table> LimitEnabled = Params.get<sol::optional<sol::table>>("limit_enabled");
			if (LimitEnabled.has_value())
			{
				Settings.LimitEnabled.Empty();
				for (auto& kv : LimitEnabled.value())
				{
					if (kv.second.is<sol::table>())
					{
						sol::table LE = kv.second.as<sol::table>();
						Settings.LimitEnabled.Add(FRigControlLimitEnabled(
							LE.get_or("min", false), LE.get_or("max", false)));
					}
					else if (kv.second.is<bool>())
					{
						Settings.LimitEnabled.Add(FRigControlLimitEnabled(kv.second.as<bool>()));
					}
				}
			}

			// Minimum/Maximum values
			auto ParseControlValue = [&](sol::table Tbl, FRigControlValue& OutVal, ERigControlType Type)
			{
				switch (Type)
				{
				case ERigControlType::Bool:
					OutVal.Set<bool>(Tbl.get_or("x", false));
					break;
				case ERigControlType::Float:
				case ERigControlType::ScaleFloat:
					OutVal.Set<float>(Tbl.get_or("x", 0.0f));
					break;
				case ERigControlType::Integer:
					OutVal.Set<int32>((int32)Tbl.get_or("x", 0.0));
					break;
				case ERigControlType::Vector2D:
					OutVal.Set<FVector3f>(FVector3f(
						(float)Tbl.get_or("x", 0.0),
						(float)Tbl.get_or("y", 0.0),
						0.f));
					break;
				case ERigControlType::Position:
				case ERigControlType::Scale:
				case ERigControlType::Rotator:
					OutVal.Set<FVector3f>(FVector3f(
						(float)Tbl.get_or("x", 0.0),
						(float)Tbl.get_or("y", 0.0),
						(float)Tbl.get_or("z", 0.0)));
					break;
				case ERigControlType::EulerTransform:
				{
					FEulerTransform ZeroET;
				FRigControlValue::FEulerTransform_Float ET(ZeroET);
					ET.TranslationX = (float)Tbl.get_or("tx", 0.0);
					ET.TranslationY = (float)Tbl.get_or("ty", 0.0);
					ET.TranslationZ = (float)Tbl.get_or("tz", 0.0);
					ET.RotationPitch = (float)Tbl.get_or("pitch", 0.0);
					ET.RotationYaw = (float)Tbl.get_or("yaw", 0.0);
					ET.RotationRoll = (float)Tbl.get_or("roll", 0.0);
					ET.ScaleX = (float)Tbl.get_or("sx", 1.0);
					ET.ScaleY = (float)Tbl.get_or("sy", 1.0);
					ET.ScaleZ = (float)Tbl.get_or("sz", 1.0);
					OutVal.Set<FRigControlValue::FEulerTransform_Float>(ET);
					break;
				}
				default:
					break;
				}
			};
			sol::optional<sol::table> MinTbl = Params.get<sol::optional<sol::table>>("minimum");
			if (MinTbl.has_value())
			{
				ParseControlValue(MinTbl.value(), Settings.MinimumValue, Settings.ControlType);
			}
			sol::optional<sol::table> MaxTbl = Params.get<sol::optional<sol::table>>("maximum");
			if (MaxTbl.has_value())
			{
				ParseControlValue(MaxTbl.value(), Settings.MaximumValue, Settings.ControlType);
			}

			Ctrl->SetControlSettings(Key, Settings, false);

			// Control value setting (current, initial, offset, shape transforms)
			sol::optional<sol::table> OffsetTransform = Params.get<sol::optional<sol::table>>("offset_transform");
			if (OffsetTransform.has_value())
			{
				sol::table OT = OffsetTransform.value();
				FTransform T = FTransform::Identity;
				sol::optional<sol::table> Loc = OT.get<sol::optional<sol::table>>("location");
				if (Loc.has_value()) T.SetLocation(FVector(Loc.value().get_or("x", 0.0), Loc.value().get_or("y", 0.0), Loc.value().get_or("z", 0.0)));
				sol::optional<sol::table> Rot = OT.get<sol::optional<sol::table>>("rotation");
				if (Rot.has_value()) T.SetRotation(FRotator(Rot.value().get_or("pitch", 0.0), Rot.value().get_or("yaw", 0.0), Rot.value().get_or("roll", 0.0)).Quaternion());
				sol::optional<sol::table> Scl = OT.get<sol::optional<sol::table>>("scale");
				if (Scl.has_value()) T.SetScale3D(FVector(Scl.value().get_or("x", 1.0), Scl.value().get_or("y", 1.0), Scl.value().get_or("z", 1.0)));
				Hierarchy->SetControlOffsetTransform(Key, T, true, true, false);
			}

			sol::optional<sol::table> ShapeTransform = Params.get<sol::optional<sol::table>>("shape_transform");
			if (ShapeTransform.has_value())
			{
				sol::table ST = ShapeTransform.value();
				FTransform T = FTransform::Identity;
				sol::optional<sol::table> Loc = ST.get<sol::optional<sol::table>>("location");
				if (Loc.has_value()) T.SetLocation(FVector(Loc.value().get_or("x", 0.0), Loc.value().get_or("y", 0.0), Loc.value().get_or("z", 0.0)));
				sol::optional<sol::table> Rot = ST.get<sol::optional<sol::table>>("rotation");
				if (Rot.has_value()) T.SetRotation(FRotator(Rot.value().get_or("pitch", 0.0), Rot.value().get_or("yaw", 0.0), Rot.value().get_or("roll", 0.0)).Quaternion());
				sol::optional<sol::table> Scl = ST.get<sol::optional<sol::table>>("scale");
				if (Scl.has_value()) T.SetScale3D(FVector(Scl.value().get_or("x", 1.0), Scl.value().get_or("y", 1.0), Scl.value().get_or("z", 1.0)));
				Hierarchy->SetControlShapeTransform(Key, T, true, false);
			}

			BP->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] configure(\"control\", \"%s\")"), *FId));
			return sol::make_object(Lua, true);
		});

		// ---- list(type?, filter?) ----
		AssetObj.set_function("list", [BP, Hierarchy, Ctrl, &Session](sol::table self,
			sol::optional<std::string> TypeOpt, sol::optional<std::string> FilterOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = TypeOpt.has_value() ? UTF8_TO_TCHAR(TypeOpt.value().c_str()) : TEXT("all");

			if (FType.Equals(TEXT("all"), ESearchCase::IgnoreCase))
			{
				// Return every element in the hierarchy
				FType = TEXT("all_elements");
			}

			// --- list("variables") ---
			if (FType.Equals(TEXT("variables"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				TArray<FRigVMGraphVariableDescription> Vars = BP->GetMemberVariables();
				int32 Idx = 1;
				for (const FRigVMGraphVariableDescription& V : Vars)
				{
					sol::table Entry = Lua.create_table();
					Entry["name"] = TCHAR_TO_UTF8(*V.Name.ToString());
					Entry["type"] = TCHAR_TO_UTF8(*V.CPPType);
					Entry["default_value"] = TCHAR_TO_UTF8(*V.DefaultValue);
					Result[Idx++] = Entry;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"variables\") -> %d"), Idx - 1));
				return Result;
			}

			// --- list("selection") ---
			if (FType.Equals(TEXT("selection"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				TArray<FRigElementKey> Selected = Hierarchy->GetSelectedKeys(ERigElementType::All);
				int32 Idx = 1;
				for (const FRigElementKey& Key : Selected)
				{
					sol::table Entry = Lua.create_table();
					Entry["name"] = TCHAR_TO_UTF8(*Key.Name.ToString());
					Entry["type"] = TCHAR_TO_UTF8(*ElemTypeStr(Key.Type));
					Result[Idx++] = Entry;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"selection\") -> %d"), Idx - 1));
				return Result;
			}

			// --- list("metadata", optional element_name) ---
			if (FType.Equals(TEXT("metadata"), ESearchCase::IgnoreCase))
			{
				TArray<FRigElementKey> Keys;
				if (FilterOpt.has_value() && !FilterOpt.value().empty())
				{
					FString FilterName = UTF8_TO_TCHAR(FilterOpt.value().c_str());
					FRigElementKey Key = FindElemKey(Hierarchy, FilterName);
					if (Key.IsValid()) Keys.Add(Key);
				}
				else
				{
					Keys = Hierarchy->GetAllKeys(false, ERigElementType::All);
				}

				sol::table Result = Lua.create_table();
				int32 Idx = 1;
				for (const FRigElementKey& Key : Keys)
				{
					TArray<FName> MetaNames = Hierarchy->GetMetadataNames(Key);
					for (const FName& MetaName : MetaNames)
					{
						ERigMetadataType MetaType = Hierarchy->GetMetadataType(Key, MetaName);
						FString Value;
						switch (MetaType)
						{
						case ERigMetadataType::Bool: Value = Hierarchy->GetBoolMetadata(Key, MetaName, false) ? TEXT("true") : TEXT("false"); break;
						case ERigMetadataType::Float: Value = FString::SanitizeFloat(Hierarchy->GetFloatMetadata(Key, MetaName, 0.0f)); break;
						case ERigMetadataType::Int32: Value = FString::FromInt(Hierarchy->GetInt32Metadata(Key, MetaName, 0)); break;
						case ERigMetadataType::Name: Value = Hierarchy->GetNameMetadata(Key, MetaName, NAME_None).ToString(); break;
						case ERigMetadataType::Vector: Value = Hierarchy->GetVectorMetadata(Key, MetaName, FVector::ZeroVector).ToString(); break;
						case ERigMetadataType::Rotator: Value = Hierarchy->GetRotatorMetadata(Key, MetaName, FRotator::ZeroRotator).ToString(); break;
						case ERigMetadataType::Quat: Value = Hierarchy->GetQuatMetadata(Key, MetaName, FQuat::Identity).ToString(); break;
						case ERigMetadataType::Transform: Value = Hierarchy->GetTransformMetadata(Key, MetaName, FTransform::Identity).ToString(); break;
						case ERigMetadataType::LinearColor: Value = Hierarchy->GetLinearColorMetadata(Key, MetaName, FLinearColor::Transparent).ToString(); break;
						case ERigMetadataType::RigElementKey:
						{
							FRigElementKey RefKey = Hierarchy->GetRigElementKeyMetadata(Key, MetaName, FRigElementKey());
							Value = FString::Printf(TEXT("%s:%s"), *RefKey.Name.ToString(), *ElemTypeStr(RefKey.Type));
							break;
						}
						default: Value = TEXT("<unsupported>"); break;
						}

						sol::table Entry = Lua.create_table();
						Entry["element"] = TCHAR_TO_UTF8(*Key.Name.ToString());
						Entry["element_type"] = TCHAR_TO_UTF8(*ElemTypeStr(Key.Type));
						Entry["name"] = TCHAR_TO_UTF8(*MetaName.ToString());
						Entry["type"] = TCHAR_TO_UTF8(*MetadataTypeStr(MetaType));
						Entry["value"] = TCHAR_TO_UTF8(*Value);
						Result[Idx++] = Entry;
					}
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"metadata\") -> %d"), Idx - 1));
				return Result;
			}

			// --- list("unit_types", optional filter) ---
			if (FType.Equals(TEXT("unit_types"), ESearchCase::IgnoreCase))
			{
				FString Filter = FilterOpt.has_value() ? UTF8_TO_TCHAR(FilterOpt.value().c_str()) : TEXT("");
				sol::table Result = Lua.create_table();
				int32 Idx = 1;
				for (TObjectIterator<UScriptStruct> It; It; ++It)
				{
					UScriptStruct* Struct = *It;
					if (!Struct || !Struct->IsChildOf(FRigUnit::StaticStruct())) continue;
					if (Struct->HasMetaData(TEXT("Abstract")) || Struct->HasMetaData(TEXT("Deprecated"))) continue;
					FString StructName = Struct->GetName();
					if (!Filter.IsEmpty() && !StructName.Contains(Filter, ESearchCase::IgnoreCase)) continue;

					sol::table Entry = Lua.create_table();
					FString ShortName = StructName;
					ShortName.RemoveFromStart(TEXT("RigUnit_"));
					Entry["name"] = TCHAR_TO_UTF8(*ShortName);
					Entry["full_name"] = TCHAR_TO_UTF8(*StructName);
					const FString Tooltip = GetRigUnitTooltip(Struct);
					if (!Tooltip.IsEmpty())
					{
						Entry["tooltip"] = TCHAR_TO_UTF8(*Tooltip);
					}
					Result[Idx++] = Entry;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"unit_types\") -> %d"), Idx - 1));
				return Result;
			}

			// --- list("tags", optional element_name) ---
			if (FType.Equals(TEXT("tags"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				int32 Idx = 1;

				if (FilterOpt.has_value() && !FilterOpt.value().empty())
				{
					FString FilterName = UTF8_TO_TCHAR(FilterOpt.value().c_str());
					FRigElementKey Key = FindElemKey(Hierarchy, FilterName);
					if (Key.IsValid())
					{
						TArray<FName> Tags = Hierarchy->GetTags(Key);
						for (const FName& Tag : Tags)
						{
							sol::table Entry = Lua.create_table();
							Entry["element"] = TCHAR_TO_UTF8(*Key.Name.ToString());
							Entry["element_type"] = TCHAR_TO_UTF8(*ElemTypeStr(Key.Type));
							Entry["tag"] = TCHAR_TO_UTF8(*Tag.ToString());
							Result[Idx++] = Entry;
						}
					}
				}
				else
				{
					TArray<FRigElementKey> AllKeys = Hierarchy->GetAllKeys(false, ERigElementType::All);
					for (const FRigElementKey& Key : AllKeys)
					{
						TArray<FName> Tags = Hierarchy->GetTags(Key);
						for (const FName& Tag : Tags)
						{
							sol::table Entry = Lua.create_table();
							Entry["element"] = TCHAR_TO_UTF8(*Key.Name.ToString());
							Entry["element_type"] = TCHAR_TO_UTF8(*ElemTypeStr(Key.Type));
							Entry["tag"] = TCHAR_TO_UTF8(*Tag.ToString());
							Result[Idx++] = Entry;
						}
					}
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"tags\") -> %d"), Idx - 1));
				return Result;
			}

			// --- Filter by element type ---
			ERigElementType FilterType = ERigElementType::None;
			if (FType.Contains(TEXT("bone"), ESearchCase::IgnoreCase)) FilterType = ERigElementType::Bone;
			else if (FType.Contains(TEXT("control"), ESearchCase::IgnoreCase)) FilterType = ERigElementType::Control;
			else if (FType.Contains(TEXT("null"), ESearchCase::IgnoreCase)) FilterType = ERigElementType::Null;
			else if (FType.Contains(TEXT("curve"), ESearchCase::IgnoreCase)) FilterType = ERigElementType::Curve;
			else if (FType.Contains(TEXT("connector"), ESearchCase::IgnoreCase)) FilterType = ERigElementType::Connector;
			else if (FType.Contains(TEXT("socket"), ESearchCase::IgnoreCase)) FilterType = ERigElementType::Socket;

			sol::table Result = Lua.create_table();
			int32 Idx = 1;

			Hierarchy->ForEach<FRigBaseElement>([&](FRigBaseElement* Elem) -> bool
			{
				if (FilterType != ERigElementType::None && Elem->GetKey().Type != FilterType)
					return true;

				sol::table Entry = Lua.create_table();
				Entry["name"] = TCHAR_TO_UTF8(*Elem->GetKey().Name.ToString());
				Entry["type"] = TCHAR_TO_UTF8(*ElemTypeStr(Elem->GetKey().Type));

				// Parent info
				FRigElementKey ParentKey = Hierarchy->GetFirstParent(Elem->GetKey());
				if (ParentKey.IsValid())
					Entry["parent"] = TCHAR_TO_UTF8(*ParentKey.Name.ToString());

				// Control-specific info
				if (Elem->GetKey().Type == ERigElementType::Control)
				{
					FRigControlElement* CE = Cast<FRigControlElement>(Elem);
					if (CE)
					{
						Entry["control_type"] = TCHAR_TO_UTF8(*CtrlTypeStr(CE->Settings.ControlType));
						if (!CE->Settings.DisplayName.IsNone())
							Entry["display_name"] = TCHAR_TO_UTF8(*CE->Settings.DisplayName.ToString());
						Entry["shape_visible"] = CE->Settings.bShapeVisible;
						if (!CE->Settings.ShapeName.IsNone())
							Entry["shape_name"] = TCHAR_TO_UTF8(*CE->Settings.ShapeName.ToString());

						// Limits
						bool bHasLimits = false;
						for (const FRigControlLimitEnabled& LE : CE->Settings.LimitEnabled)
						{
							if (LE.IsOn()) { bHasLimits = true; break; }
						}
						Entry["has_limits"] = bHasLimits;

						}
				}

				Result[Idx++] = Entry;
				return true;
			});

			Session.Log(FString::Printf(TEXT("[OK] list(\"%s\") -> %d"), *FType, Idx - 1));
			return Result;
		});

		// ---- reparent(name, new_parent) ----
		AssetObj.set_function("reparent", [BP, Hierarchy, Ctrl, &Session](sol::table /*self*/,
			const std::string& Name, const std::string& NewParent, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FElemName = UTF8_TO_TCHAR(Name.c_str());
			FRigElementKey Key = FindElemKey(Hierarchy, FElemName);
			if (!Key.IsValid()) { Session.Log(FString::Printf(TEXT("[FAIL] reparent -> '%s' not found"), *FElemName)); return sol::lua_nil; }

			FRigElementKey NewParentKey;
			if (!NewParent.empty())
			{
				NewParentKey = FindElemKey(Hierarchy, UTF8_TO_TCHAR(NewParent.c_str()));
				if (!NewParentKey.IsValid()) { Session.Log(FString::Printf(TEXT("[FAIL] reparent -> parent '%s' not found"), UTF8_TO_TCHAR(NewParent.c_str()))); return sol::lua_nil; }
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "ReparentCR", "Reparent ControlRig Element"));
			static_cast<UObject*>(BP)->Modify();
			bool bOK = Ctrl->SetParent(Key, NewParentKey, true, false);
			if (!bOK) { Session.Log(TEXT("[FAIL] reparent -> operation failed")); return sol::lua_nil; }
			BP->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] reparent(\"%s\", \"%s\")"), *FElemName, UTF8_TO_TCHAR(NewParent.c_str())));
			return sol::make_object(Lua, true);
		});

		// ---- rename(name, new_name) ----
		AssetObj.set_function("rename", [BP, Hierarchy, Ctrl, &Session](sol::table /*self*/,
			const std::string& Name, const std::string& NewName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FElemName = UTF8_TO_TCHAR(Name.c_str());
			FRigElementKey Key = FindElemKey(Hierarchy, FElemName);
			if (!Key.IsValid()) { Session.Log(FString::Printf(TEXT("[FAIL] rename -> '%s' not found"), *FElemName)); return sol::lua_nil; }

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "RenameCR", "Rename ControlRig Element"));
			static_cast<UObject*>(BP)->Modify();
			FRigElementKey NewKey = Ctrl->RenameElement(Key, FName(*FString(UTF8_TO_TCHAR(NewName.c_str()))), false);
			if (!NewKey.IsValid()) { Session.Log(TEXT("[FAIL] rename -> operation failed")); return sol::lua_nil; }
			BP->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] rename(\"%s\", \"%s\")"), *FElemName, UTF8_TO_TCHAR(NewName.c_str())));
			return sol::make_object(Lua, true);
		});

		// ---- import_hierarchy(params) ----
		AssetObj.set_function("import_hierarchy", [BP, Hierarchy, Ctrl, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			std::string SkelPath = Params.get_or<std::string>("skeleton", "");
			std::string MeshPath = Params.get_or<std::string>("skeletal_mesh", "");

			if (SkelPath.empty() && MeshPath.empty())
			{
				Session.Log(TEXT("[FAIL] import_hierarchy -> skeleton or skeletal_mesh required"));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "ImportCR", "Import ControlRig Hierarchy"));
			static_cast<UObject*>(BP)->Modify();

			int32 Before = Hierarchy->Num();
			bool bOK = false;

			if (!SkelPath.empty())
			{
				FString FSkel = UTF8_TO_TCHAR(SkelPath.c_str());
				if (!FSkel.StartsWith(TEXT("/"))) FSkel = TEXT("/Game/") + FSkel;
				USkeleton* Skel = LoadObject<USkeleton>(nullptr, *FSkel);
				if (Skel)
				{
					TArray<FRigElementKey> Keys = Ctrl->ImportBones(Skel->GetReferenceSkeleton(), FName(), false, false, false, false);
					bOK = Keys.Num() > 0;
				}
			}
			else
			{
				FString FMesh = UTF8_TO_TCHAR(MeshPath.c_str());
				if (!FMesh.StartsWith(TEXT("/"))) FMesh = TEXT("/Game/") + FMesh;
				USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *FMesh);
				if (Mesh)
				{
					TArray<FRigElementKey> Keys = Ctrl->ImportBones(Mesh->GetRefSkeleton(), FName(), false, false, false, false);
					bOK = Keys.Num() > 0;
				}
			}

			if (!bOK) { Session.Log(TEXT("[FAIL] import_hierarchy -> import failed")); return sol::lua_nil; }
			int32 Added = Hierarchy->Num() - Before;
			BP->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] import_hierarchy -> %d elements added"), Added));
			return sol::make_object(Lua, true);
		});

		// ---- import_curves(params) ---- Imports curves from a skeleton or skeletal mesh
		AssetObj.set_function("import_curves", [BP, Hierarchy, Ctrl, &Session](sol::table /*self*/,
			sol::optional<sol::table> OptParams, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			std::string SkelPath;
			std::string MeshPath;
			std::string Namespace;

			if (OptParams.has_value())
			{
				sol::table Params = OptParams.value();
				SkelPath = Params.get_or<std::string>("skeleton", "");
				MeshPath = Params.get_or<std::string>("skeletal_mesh", "");
				Namespace = Params.get_or<std::string>("namespace", "");
			}

			if (SkelPath.empty() && MeshPath.empty())
			{
				Session.Log(TEXT("[FAIL] import_curves -> skeleton or skeletal_mesh required"));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "ImportCRCurves", "Import ControlRig Curves"));
			static_cast<UObject*>(BP)->Modify();

			FName FNamespace = Namespace.empty() ? NAME_None : FName(UTF8_TO_TCHAR(Namespace.c_str()));
			TArray<FRigElementKey> Keys;

			if (!MeshPath.empty())
			{
				FString FMesh = UTF8_TO_TCHAR(MeshPath.c_str());
				if (!FMesh.StartsWith(TEXT("/"))) FMesh = TEXT("/Game/") + FMesh;
				USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *FMesh);
				if (!Mesh) { Session.Log(FString::Printf(TEXT("[FAIL] import_curves -> could not load skeletal mesh '%s'"), *FMesh)); return sol::lua_nil; }
	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
				Keys = Ctrl->ImportCurvesFromSkeletalMesh(Mesh, FNamespace, false, false);
#else
				Session.Log(TEXT("[FAIL] import_curves from skeletal mesh requires UE 5.5+")); return sol::lua_nil;
#endif
			}
			else
			{
				FString FSkel = UTF8_TO_TCHAR(SkelPath.c_str());
				if (!FSkel.StartsWith(TEXT("/"))) FSkel = TEXT("/Game/") + FSkel;
				USkeleton* Skel = LoadObject<USkeleton>(nullptr, *FSkel);
				if (!Skel) { Session.Log(FString::Printf(TEXT("[FAIL] import_curves -> could not load skeleton '%s'"), *FSkel)); return sol::lua_nil; }
				Keys = Ctrl->ImportCurves(Skel, FNamespace, false, false);
			}

			BP->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] import_curves -> %d curves imported"), Keys.Num()));

			sol::table Result = Lua.create_table();
			for (int32 i = 0; i < Keys.Num(); ++i)
			{
				Result[i + 1] = TCHAR_TO_UTF8(*Keys[i].Name.ToString());
			}
			return Result;
		});

		// ---- import_sockets(params) ---- Imports sockets from a skeletal mesh
		AssetObj.set_function("import_sockets", [BP, Hierarchy, Ctrl, &Session](sol::table /*self*/,
			sol::optional<sol::table> OptParams, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			std::string MeshPath;
			std::string Namespace;
			bool bReplaceExisting = true;
			bool bRemoveObsolete = true;

			if (OptParams.has_value())
			{
				sol::table Params = OptParams.value();
				MeshPath = Params.get_or<std::string>("skeletal_mesh", "");
				Namespace = Params.get_or<std::string>("namespace", "");
				bReplaceExisting = Params.get_or("replace_existing", true);
				bRemoveObsolete = Params.get_or("remove_obsolete", true);
			}

			if (MeshPath.empty())
			{
				Session.Log(TEXT("[FAIL] import_sockets -> skeletal_mesh required (sockets only exist on meshes)"));
				return sol::lua_nil;
			}

			FString FMesh = UTF8_TO_TCHAR(MeshPath.c_str());
			if (!FMesh.StartsWith(TEXT("/"))) FMesh = TEXT("/Game/") + FMesh;
			USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *FMesh);
			if (!Mesh)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] import_sockets -> could not load skeletal mesh '%s'"), *FMesh));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "ImportCRSockets", "Import ControlRig Sockets"));
			static_cast<UObject*>(BP)->Modify();

			FName FNamespace = Namespace.empty() ? NAME_None : FName(UTF8_TO_TCHAR(Namespace.c_str()));
	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			TArray<FRigElementKey> Keys = Ctrl->ImportSocketsFromSkeletalMesh(Mesh, FNamespace, bReplaceExisting, bRemoveObsolete, false, false);
#else
			TArray<FRigElementKey> Keys;
			Session.Log(TEXT("[FAIL] import_sockets -> requires UE 5.6+"));
#endif

			BP->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] import_sockets -> %d sockets imported"), Keys.Num()));

			sol::table Result = Lua.create_table();
			for (int32 i = 0; i < Keys.Num(); ++i)
			{
				Result[i + 1] = TCHAR_TO_UTF8(*Keys[i].Name.ToString());
			}
			return Result;
		});

		// ---- duplicate(names_array) ----
		AssetObj.set_function("duplicate", [BP, Hierarchy, Ctrl, &Session](sol::table /*self*/,
			sol::table Names, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			TArray<FRigElementKey> Keys;
			for (const auto& Pair : Names)
			{
				if (Pair.second.is<std::string>())
				{
					FString Name = UTF8_TO_TCHAR(Pair.second.as<std::string>().c_str());
					FRigElementKey Key = FindElemKey(Hierarchy, Name);
					if (Key.IsValid())
					{
						Keys.Add(Key);
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[FAIL] duplicate -> '%s' not found"), *Name));
					}
				}
			}

			if (Keys.Num() == 0)
			{
				Session.Log(TEXT("[FAIL] duplicate -> no valid elements"));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "DupCR", "Duplicate ControlRig Elements"));
			static_cast<UObject*>(BP)->Modify();
			TArray<FRigElementKey> NewKeys = Ctrl->DuplicateElements(Keys, false, false);
			if (NewKeys.Num() == 0)
			{
				Session.Log(TEXT("[FAIL] duplicate -> operation failed"));
				return sol::lua_nil;
			}

			BP->MarkPackageDirty();
			sol::table Result = Lua.create_table();
			int32 Idx = 1;
			for (const FRigElementKey& K : NewKeys)
			{
				Result[Idx++] = TCHAR_TO_UTF8(*K.Name.ToString());
			}
			Session.Log(FString::Printf(TEXT("[OK] duplicate -> %d elements duplicated"), NewKeys.Num()));
			return Result;
		});

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
		// ---- mirror(params) — 5.6+ ----
		AssetObj.set_function("mirror", [BP, Hierarchy, Ctrl, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			sol::optional<sol::table> ElemsOpt = Params.get<sol::optional<sol::table>>("elements");
			if (!ElemsOpt.has_value())
			{
				Session.Log(TEXT("[FAIL] mirror -> elements array required"));
				return sol::lua_nil;
			}

			TArray<FRigElementKey> Keys;
			sol::table Elems = ElemsOpt.value();
			for (const auto& Pair : Elems)
			{
				if (Pair.second.is<std::string>())
				{
					FString Name = UTF8_TO_TCHAR(Pair.second.as<std::string>().c_str());
					FRigElementKey Key = FindElemKey(Hierarchy, Name);
					if (Key.IsValid()) Keys.Add(Key);
					else Session.Log(FString::Printf(TEXT("[FAIL] mirror -> '%s' not found"), *Name));
				}
			}

			if (Keys.Num() == 0)
			{
				Session.Log(TEXT("[FAIL] mirror -> no valid elements"));
				return sol::lua_nil;
			}

			std::string SearchStr = Params.get_or<std::string>("search", "");
			std::string ReplaceStr = Params.get_or<std::string>("replace", "");
			std::string MirrorAxisStr = Params.get_or<std::string>("mirror_axis", "");
			std::string AxisToFlipStr = Params.get_or<std::string>("axis_to_flip", "");

			FRigVMMirrorSettings MirrorSettings;
			MirrorSettings.SearchString = UTF8_TO_TCHAR(SearchStr.c_str());
			MirrorSettings.ReplaceString = UTF8_TO_TCHAR(ReplaceStr.c_str());
			if (!MirrorAxisStr.empty())
				MirrorSettings.MirrorAxis = ParseAxis(UTF8_TO_TCHAR(MirrorAxisStr.c_str()));
			if (!AxisToFlipStr.empty())
				MirrorSettings.AxisToFlip = ParseAxis(UTF8_TO_TCHAR(AxisToFlipStr.c_str()));

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "MirrorCR", "Mirror ControlRig Elements"));
			static_cast<UObject*>(BP)->Modify();
			TArray<FRigElementKey> NewKeys = Ctrl->MirrorElements(Keys, MirrorSettings, false, false);
			if (NewKeys.Num() == 0)
			{
				Session.Log(TEXT("[FAIL] mirror -> operation failed"));
				return sol::lua_nil;
			}

			BP->MarkPackageDirty();
			sol::table Result = Lua.create_table();
			int32 Idx = 1;
			for (const FRigElementKey& K : NewKeys)
			{
				Result[Idx++] = TCHAR_TO_UTF8(*K.Name.ToString());
			}
			Session.Log(FString::Printf(TEXT("[OK] mirror -> %d elements mirrored"), NewKeys.Num()));
			return Result;
		});

#endif // ENGINE_MINOR_VERSION >= 6 (mirror)

		// ---- reorder(name, new_index) ----
		AssetObj.set_function("reorder", [BP, Hierarchy, Ctrl, &Session](sol::table /*self*/,
			const std::string& Name, int NewIndex, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FElemName = UTF8_TO_TCHAR(Name.c_str());
			FRigElementKey Key = FindElemKey(Hierarchy, FElemName);
			if (!Key.IsValid())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] reorder -> '%s' not found"), *FElemName));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "ReorderCR", "Reorder ControlRig Element"));
			static_cast<UObject*>(BP)->Modify();
			if (Ctrl->ReorderElement(Key, static_cast<int32>(NewIndex), false))
			{
				BP->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] reorder(\"%s\", %d)"), *FElemName, NewIndex));
				return sol::make_object(Lua, true);
			}
			// Engine returns false for no-op (element already at target index).
			// Since we verified the element exists, treat this as success.
			Session.Log(FString::Printf(TEXT("[OK] reorder(\"%s\", %d) — already at target index"), *FElemName, NewIndex));
			return sol::make_object(Lua, true);
			return sol::lua_nil;
		});

		// ---- set_spaces(control_name, params) ----
		AssetObj.set_function("set_spaces", [BP, Hierarchy, Ctrl, &Session](sol::table /*self*/,
			const std::string& ControlName, sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FCtrlName = UTF8_TO_TCHAR(ControlName.c_str());
			FRigElementKey ControlKey = FindElemKey(Hierarchy, FCtrlName);
			if (!ControlKey.IsValid() || ControlKey.Type != ERigElementType::Control)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_spaces -> control '%s' not found"), *FCtrlName));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "SetSpacesCR", "Set ControlRig Spaces"));
			static_cast<UObject*>(BP)->Modify();

			int32 Count = 0;

			// Clear existing spaces if requested
			bool bClear = Params.get_or("clear", false);
			if (bClear)
			{
				FRigControlElement* ControlElement = Cast<FRigControlElement>(Hierarchy->Find(ControlKey));
				if (ControlElement)
				{
					const TArray<FRigElementKey> ExistingSpaces = GetAvailableSpaceKeys(ControlElement);
					for (const FRigElementKey& Existing : ExistingSpaces)
					{
	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
						if (Ctrl->RemoveAvailableSpace(ControlKey, Existing, false, false)) Count++;
#endif
					}
				}
			}

			// Add spaces
			sol::optional<sol::table> SpacesOpt = Params.get<sol::optional<sol::table>>("spaces");
			if (SpacesOpt.has_value())
			{
				sol::table Spaces = SpacesOpt.value();
				for (const auto& Pair : Spaces)
				{
					FString SpaceName;
					if (Pair.second.is<std::string>())
					{
						SpaceName = UTF8_TO_TCHAR(Pair.second.as<std::string>().c_str());
					}
					else if (Pair.second.is<sol::table>())
					{
						sol::table SpaceEntry = Pair.second.as<sol::table>();
						std::string ElemStr = SpaceEntry.get_or<std::string>("element", "");
						SpaceName = UTF8_TO_TCHAR(ElemStr.c_str());
					}

					if (SpaceName.IsEmpty()) continue;
					FRigElementKey SpaceKey = FindElemKey(Hierarchy, SpaceName);
					if (!SpaceKey.IsValid())
					{
						Session.Log(FString::Printf(TEXT("[FAIL] set_spaces -> space '%s' not found"), *SpaceName));
						continue;
					}

					if (AddAvailableSpaceCompat(Ctrl, ControlKey, SpaceKey)) Count++;
				}
			}

			// Activate specific space
			std::string ActiveStr = Params.get_or<std::string>("active_space", "");
			if (!ActiveStr.empty())
			{
				FString FActive = UTF8_TO_TCHAR(ActiveStr.c_str());
				FRigElementKey ActiveKey = FindElemKey(Hierarchy, FActive);
				if (ActiveKey.IsValid())
				{
					if (Hierarchy->SwitchToParent(ControlKey, ActiveKey, false, true)) Count++;
				}
				else
				{
					Session.Log(FString::Printf(TEXT("[FAIL] set_spaces -> active_space '%s' not found"), *FActive));
				}
			}

			BP->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] set_spaces(\"%s\") -> %d changes"), *FCtrlName, Count));
			return sol::make_object(Lua, true);
		});

		// ---- set_metadata(element, name, type, value) ----
		AssetObj.set_function("set_metadata", [BP, Hierarchy, &Session](sol::table /*self*/,
			const std::string& Element, const std::string& MetaNameStr,
			const std::string& MetaType, const std::string& MetaValue, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FElem = UTF8_TO_TCHAR(Element.c_str());
			FString FMetaName = UTF8_TO_TCHAR(MetaNameStr.c_str());
			FString FMetaType = FString(UTF8_TO_TCHAR(MetaType.c_str())).ToLower();
			FString FMetaValue = UTF8_TO_TCHAR(MetaValue.c_str());

			FRigElementKey Key = FindElemKey(Hierarchy, FElem);
			if (!Key.IsValid())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_metadata -> element '%s' not found"), *FElem));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "SetMetaCR", "Set ControlRig Metadata"));
			static_cast<UObject*>(BP)->Modify();

			const FName MetaFName(*FMetaName);
			bool bSuccess = false;

			if (FMetaType == TEXT("bool"))
			{
				bool bVal = FMetaValue.Equals(TEXT("true"), ESearchCase::IgnoreCase) || FMetaValue == TEXT("1");
				bSuccess = Hierarchy->SetBoolMetadata(Key, MetaFName, bVal);
			}
			else if (FMetaType == TEXT("int32") || FMetaType == TEXT("int"))
			{
				bSuccess = Hierarchy->SetInt32Metadata(Key, MetaFName, FCString::Atoi(*FMetaValue));
			}
			else if (FMetaType == TEXT("float"))
			{
				bSuccess = Hierarchy->SetFloatMetadata(Key, MetaFName, FCString::Atof(*FMetaValue));
			}
			else if (FMetaType == TEXT("name") || FMetaType == TEXT("string") || FMetaType == TEXT("fname"))
			{
				bSuccess = Hierarchy->SetNameMetadata(Key, MetaFName, FName(*FMetaValue));
			}
			else if (FMetaType == TEXT("vector"))
			{
				FVector V = FVector::ZeroVector;
				V.InitFromString(FMetaValue);
				bSuccess = Hierarchy->SetVectorMetadata(Key, MetaFName, V);
			}
			else if (FMetaType == TEXT("rotator"))
			{
				FRotator R = FRotator::ZeroRotator;
				R.InitFromString(FMetaValue);
				bSuccess = Hierarchy->SetRotatorMetadata(Key, MetaFName, R);
			}
			else if (FMetaType == TEXT("quat"))
			{
				FQuat Q = FQuat::Identity;
				Q.InitFromString(FMetaValue);
				bSuccess = Hierarchy->SetQuatMetadata(Key, MetaFName, Q);
			}
			else if (FMetaType == TEXT("transform"))
			{
				FTransform T = FTransform::Identity;
				T.InitFromString(FMetaValue);
				bSuccess = Hierarchy->SetTransformMetadata(Key, MetaFName, T);
			}
			else if (FMetaType == TEXT("color") || FMetaType == TEXT("linearcolor"))
			{
				bSuccess = Hierarchy->SetLinearColorMetadata(Key, MetaFName, ParseColorStr(FMetaValue));
			}
			else if (FMetaType == TEXT("rig_element_key"))
			{
				FString ElementName = FMetaValue;
				FString ElementType;
				if (FMetaValue.Split(TEXT(":"), &ElementName, &ElementType))
				{
					FRigElementKey RefKey(FName(*ElementName), ParseElemType(ElementType));
					bSuccess = Hierarchy->SetRigElementKeyMetadata(Key, MetaFName, RefKey);
				}
				else
				{
					FRigElementKey RefKey = FindElemKey(Hierarchy, ElementName);
					if (RefKey.IsValid())
					{
						bSuccess = Hierarchy->SetRigElementKeyMetadata(Key, MetaFName, RefKey);
					}
				}
			}
			else
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_metadata -> unsupported type '%s'"), *FMetaType));
				return sol::lua_nil;
			}

			if (bSuccess)
			{
				BP->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] set_metadata(\"%s\", \"%s\", \"%s\")"), *FElem, *FMetaName, *FMetaType));
				return sol::make_object(Lua, true);
			}
			Session.Log(FString::Printf(TEXT("[FAIL] set_metadata -> failed for '%s.%s'"), *FElem, *FMetaName));
			return sol::lua_nil;
		});

		// ---- remove_metadata(element, name) ----
		AssetObj.set_function("remove_metadata", [BP, Hierarchy, &Session](sol::table /*self*/,
			const std::string& Element, const std::string& MetaNameStr, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FElem = UTF8_TO_TCHAR(Element.c_str());
			FString FMetaName = UTF8_TO_TCHAR(MetaNameStr.c_str());

			FRigElementKey Key = FindElemKey(Hierarchy, FElem);
			if (!Key.IsValid())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove_metadata -> element '%s' not found"), *FElem));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "RemMetaCR", "Remove ControlRig Metadata"));
			static_cast<UObject*>(BP)->Modify();
			if (Hierarchy->RemoveMetadata(Key, FName(*FMetaName)))
			{
				BP->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove_metadata(\"%s\", \"%s\")"), *FElem, *FMetaName));
				return sol::make_object(Lua, true);
			}
			Session.Log(FString::Printf(TEXT("[FAIL] remove_metadata -> '%s.%s' not found"), *FElem, *FMetaName));
			return sol::lua_nil;
		});

		// ---- select(names_array) ----
		AssetObj.set_function("select", [BP, Hierarchy, Ctrl, &Session](sol::table /*self*/,
			sol::table Names, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			int32 Count = 0;
			for (const auto& Pair : Names)
			{
				if (Pair.second.is<std::string>())
				{
					FString Name = UTF8_TO_TCHAR(Pair.second.as<std::string>().c_str());
					FRigElementKey Key = FindElemKey(Hierarchy, Name);
					if (!Key.IsValid()) continue;
					if (SelectElementCompat(Ctrl, Key, true)) Count++;
				}
			}
			Session.Log(FString::Printf(TEXT("[OK] select -> %d elements"), Count));
			return sol::make_object(Lua, true);
		});

		// ---- deselect(names_array) ----
		AssetObj.set_function("deselect", [BP, Hierarchy, Ctrl, &Session](sol::table /*self*/,
			sol::table Names, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			int32 Count = 0;
			for (const auto& Pair : Names)
			{
				if (Pair.second.is<std::string>())
				{
					FString Name = UTF8_TO_TCHAR(Pair.second.as<std::string>().c_str());
					FRigElementKey Key = FindElemKey(Hierarchy, Name);
					if (!Key.IsValid()) continue;
					if (SelectElementCompat(Ctrl, Key, false)) Count++;
				}
			}
			Session.Log(FString::Printf(TEXT("[OK] deselect -> %d elements"), Count));
			return sol::make_object(Lua, true);
		});

		// ---- set_selection(names_array) ----
		AssetObj.set_function("set_selection", [BP, Hierarchy, Ctrl, &Session](sol::table /*self*/,
			sol::table Names, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			TArray<FRigElementKey> Keys;
			for (const auto& Pair : Names)
			{
				if (Pair.second.is<std::string>())
				{
					FString Name = UTF8_TO_TCHAR(Pair.second.as<std::string>().c_str());
					FRigElementKey Key = FindElemKey(Hierarchy, Name);
					if (Key.IsValid()) Keys.Add(Key);
				}
			}

			if (SetSelectionCompat(Ctrl, Keys))
			{
				Session.Log(FString::Printf(TEXT("[OK] set_selection -> %d elements"), Keys.Num()));
				return sol::make_object(Lua, true);
			}
			Session.Log(TEXT("[FAIL] set_selection -> operation failed"));
			return sol::lua_nil;
		});

		// ---- clear_selection() ----
		AssetObj.set_function("clear_selection", [Hierarchy, Ctrl, &Session](sol::table /*self*/,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (ClearSelectionCompat(Ctrl))
			{
				Session.Log(TEXT("[OK] clear_selection"));
				return sol::make_object(Lua, true);
			}
			Session.Log(TEXT("[OK] clear_selection -> no selection to clear"));
			return sol::make_object(Lua, true);
		});

		// ---- add_parent(child, parent, opts?) ----
		AssetObj.set_function("add_parent", [BP, Hierarchy, Ctrl, &Session](sol::table /*self*/,
			const std::string& Child, const std::string& Parent,
			sol::optional<sol::table> Opts, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FChild = UTF8_TO_TCHAR(Child.c_str());
			FString FParent = UTF8_TO_TCHAR(Parent.c_str());

			FRigElementKey ChildKey = FindElemKey(Hierarchy, FChild);
			FRigElementKey ParentKey = FindElemKey(Hierarchy, FParent);
			if (!ChildKey.IsValid() || !ParentKey.IsValid())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_parent -> child '%s' or parent '%s' not found"), *FChild, *FParent));
				return sol::lua_nil;
			}

			float Weight = 0.f;
			bool bMaintainGlobal = true;
			FString Label;
			if (Opts.has_value())
			{
				Weight = static_cast<float>(Opts.value().get_or("weight", 0.0));
				bMaintainGlobal = Opts.value().get_or("maintain_global", true);
				std::string LabelStr = Opts.value().get_or<std::string>("label", "");
				Label = UTF8_TO_TCHAR(LabelStr.c_str());
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "AddParentCR", "Add ControlRig Parent"));
			static_cast<UObject*>(BP)->Modify();

			const bool bOK = AddParentCompat(Ctrl, ChildKey, ParentKey, Weight, bMaintainGlobal, Label);
			if (bOK)
			{
				BP->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add_parent(\"%s\", \"%s\")"), *FChild, *FParent));
				return sol::make_object(Lua, true);
			}
			Session.Log(FString::Printf(TEXT("[FAIL] add_parent(\"%s\", \"%s\") -> failed"), *FChild, *FParent));
			return sol::lua_nil;
		});

		// ---- remove_parent(child, parent, opts?) ----
		AssetObj.set_function("remove_parent", [BP, Hierarchy, Ctrl, &Session](sol::table /*self*/,
			const std::string& Child, const std::string& Parent,
			sol::optional<sol::table> Opts, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FChild = UTF8_TO_TCHAR(Child.c_str());
			FString FParent = UTF8_TO_TCHAR(Parent.c_str());

			FRigElementKey ChildKey = FindElemKey(Hierarchy, FChild);
			FRigElementKey ParentKey = FindElemKey(Hierarchy, FParent);
			if (!ChildKey.IsValid() || !ParentKey.IsValid())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove_parent -> child '%s' or parent '%s' not found"), *FChild, *FParent));
				return sol::lua_nil;
			}

			bool bMaintainGlobal = true;
			if (Opts.has_value())
			{
				bMaintainGlobal = Opts.value().get_or("maintain_global", true);
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "RemParentCR", "Remove ControlRig Parent"));
			static_cast<UObject*>(BP)->Modify();
			if (Ctrl->RemoveParent(ChildKey, ParentKey, bMaintainGlobal, false, false))
			{
				BP->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove_parent(\"%s\", \"%s\")"), *FChild, *FParent));
				return sol::make_object(Lua, true);
			}
			Session.Log(FString::Printf(TEXT("[FAIL] remove_parent(\"%s\", \"%s\") -> failed"), *FChild, *FParent));
			return sol::lua_nil;
		});

		// ---- clear_parents(child, opts?) ----
		AssetObj.set_function("clear_parents", [BP, Hierarchy, Ctrl, &Session](sol::table /*self*/,
			const std::string& Child, sol::optional<sol::table> Opts, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FChild = UTF8_TO_TCHAR(Child.c_str());
			FRigElementKey ChildKey = FindElemKey(Hierarchy, FChild);
			if (!ChildKey.IsValid())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] clear_parents -> child '%s' not found"), *FChild));
				return sol::lua_nil;
			}

			bool bMaintainGlobal = true;
			if (Opts.has_value())
			{
				bMaintainGlobal = Opts.value().get_or("maintain_global", true);
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "ClearParentsCR", "Clear ControlRig Parents"));
			static_cast<UObject*>(BP)->Modify();
			if (Ctrl->RemoveAllParents(ChildKey, bMaintainGlobal, false, false))
			{
				BP->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] clear_parents(\"%s\")"), *FChild));
				return sol::make_object(Lua, true);
			}
			Session.Log(FString::Printf(TEXT("[FAIL] clear_parents(\"%s\") -> failed"), *FChild));
			return sol::lua_nil;
		});

		// ---- add_component(element, params) ----
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
		AssetObj.set_function("add_component", [BP, Hierarchy, Ctrl, &Session](sol::table /*self*/,
			const std::string& Element, sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FElem = UTF8_TO_TCHAR(Element.c_str());
			std::string CompName = Params.get_or<std::string>("name", "");
			std::string CompType = Params.get_or<std::string>("component_type", "");
			std::string Content = Params.get_or<std::string>("content", "");

			if (CompName.empty() || CompType.empty())
			{
				Session.Log(TEXT("[FAIL] add_component -> name and component_type required"));
				return sol::lua_nil;
			}

			FRigElementKey ElementKey = FindElemKey(Hierarchy, FElem);
			if (!ElementKey.IsValid())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_component -> element '%s' not found"), *FElem));
				return sol::lua_nil;
			}

			FString FCompType = UTF8_TO_TCHAR(CompType.c_str());
			UScriptStruct* ComponentStruct = ResolveRigComponentStruct(FCompType);
			if (!ComponentStruct)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_component -> component_type '%s' not found"), *FCompType));
				return sol::lua_nil;
			}

			FString FContent = UTF8_TO_TCHAR(Content.c_str());
			const FScopedTransaction Tx(NSLOCTEXT("AIK", "AddCompCR", "Add ControlRig Component"));
			static_cast<UObject*>(BP)->Modify();
			FRigComponentKey Added = AddControlRigComponentCompat(Ctrl, ComponentStruct, UTF8_TO_TCHAR(CompName.c_str()), ElementKey, FContent);
			if (Added.IsValid())
			{
				BP->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add_component(\"%s\", \"%s\")"), *FElem, UTF8_TO_TCHAR(CompName.c_str())));
				return sol::make_object(Lua, true);
			}
			Session.Log(FString::Printf(TEXT("[FAIL] add_component -> failed for '%s'"), *FElem));
			return sol::lua_nil;
		});

		// ---- remove_component(element, name) ----
		AssetObj.set_function("remove_component", [BP, Hierarchy, Ctrl, &Session](sol::table /*self*/,
			const std::string& Element, const std::string& CompName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FElem = UTF8_TO_TCHAR(Element.c_str());
			FString FCompName = UTF8_TO_TCHAR(CompName.c_str());

			FRigElementKey ElementKey = FindElemKey(Hierarchy, FElem);
			if (!ElementKey.IsValid())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove_component -> element '%s' not found"), *FElem));
				return sol::lua_nil;
			}

			FRigComponentKey ComponentKey(ElementKey, FName(*FCompName));
			if (!Hierarchy->FindComponent(ComponentKey))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove_component -> component '%s' not found on '%s'"), *FCompName, *FElem));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "RemCompCR", "Remove ControlRig Component"));
			static_cast<UObject*>(BP)->Modify();
			if (RemoveControlRigComponentCompat(Ctrl, ComponentKey))
			{
				BP->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove_component(\"%s\", \"%s\")"), *FElem, *FCompName));
				return sol::make_object(Lua, true);
			}
			Session.Log(FString::Printf(TEXT("[FAIL] remove_component -> failed for '%s' on '%s'"), *FCompName, *FElem));
			return sol::lua_nil;
		});

		// ---- rename_component(element, name, new_name) ----
		AssetObj.set_function("rename_component", [BP, Hierarchy, Ctrl, &Session](sol::table /*self*/,
			const std::string& Element, const std::string& CompName, const std::string& NewName,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FElem = UTF8_TO_TCHAR(Element.c_str());
			FString FCompName = UTF8_TO_TCHAR(CompName.c_str());
			FString FNewName = UTF8_TO_TCHAR(NewName.c_str());

			FRigElementKey ElementKey = FindElemKey(Hierarchy, FElem);
			if (!ElementKey.IsValid())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] rename_component -> element '%s' not found"), *FElem));
				return sol::lua_nil;
			}

			FRigComponentKey ComponentKey(ElementKey, FName(*FCompName));
			if (!Hierarchy->FindComponent(ComponentKey))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] rename_component -> component '%s' not found on '%s'"), *FCompName, *FElem));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "RenCompCR", "Rename ControlRig Component"));
			static_cast<UObject*>(BP)->Modify();
			FRigComponentKey Renamed = RenameControlRigComponentCompat(Ctrl, ComponentKey, FNewName);
			if (Renamed.IsValid())
			{
				BP->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] rename_component(\"%s\", \"%s\", \"%s\")"), *FElem, *FCompName, *FNewName));
				return sol::make_object(Lua, true);
			}
			Session.Log(FString::Printf(TEXT("[FAIL] rename_component -> failed for '%s' on '%s'"), *FCompName, *FElem));
			return sol::lua_nil;
		});

		// ---- reparent_component(element, name, new_element) ----
		AssetObj.set_function("reparent_component", [BP, Hierarchy, Ctrl, &Session](sol::table /*self*/,
			const std::string& Element, const std::string& CompName, const std::string& NewElement,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FElem = UTF8_TO_TCHAR(Element.c_str());
			FString FCompName = UTF8_TO_TCHAR(CompName.c_str());
			FString FNewElem = UTF8_TO_TCHAR(NewElement.c_str());

			FRigElementKey ElementKey = FindElemKey(Hierarchy, FElem);
			FRigElementKey NewElementKey = FindElemKey(Hierarchy, FNewElem);
			if (!ElementKey.IsValid() || !NewElementKey.IsValid())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] reparent_component -> element '%s' or new_element '%s' not found"), *FElem, *FNewElem));
				return sol::lua_nil;
			}

			FRigComponentKey ComponentKey(ElementKey, FName(*FCompName));
			if (!Hierarchy->FindComponent(ComponentKey))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] reparent_component -> component '%s' not found on '%s'"), *FCompName, *FElem));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "ReparCompCR", "Reparent ControlRig Component"));
			static_cast<UObject*>(BP)->Modify();
			FRigComponentKey Reparented = ReparentControlRigComponentCompat(Ctrl, ComponentKey, NewElementKey);
			if (Reparented.IsValid())
			{
				BP->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] reparent_component(\"%s\", \"%s\", \"%s\")"), *FElem, *FCompName, *FNewElem));
				return sol::make_object(Lua, true);
			}
			Session.Log(FString::Printf(TEXT("[FAIL] reparent_component -> failed for '%s'"), *FCompName));
			return sol::lua_nil;
		});
#else
		AssetObj.set_function("add_component", [&Session](sol::table /*self*/, const std::string& /*Element*/, sol::table /*Params*/, sol::this_state S) -> sol::object
		{
			Session.Log(TEXT("[FAIL] add_component -> requires UE 5.6+"));
			return sol::lua_nil;
		});
		AssetObj.set_function("remove_component", [&Session](sol::table /*self*/, const std::string& /*Element*/, const std::string& /*CompName*/, sol::this_state S) -> sol::object
		{
			Session.Log(TEXT("[FAIL] remove_component -> requires UE 5.6+"));
			return sol::lua_nil;
		});
		AssetObj.set_function("rename_component", [&Session](sol::table /*self*/, const std::string& /*Element*/, const std::string& /*CompName*/, const std::string& /*NewName*/, sol::this_state S) -> sol::object
		{
			Session.Log(TEXT("[FAIL] rename_component -> requires UE 5.6+"));
			return sol::lua_nil;
		});
		AssetObj.set_function("reparent_component", [&Session](sol::table /*self*/, const std::string& /*Element*/, const std::string& /*CompName*/, const std::string& /*NewElement*/, sol::this_state S) -> sol::object
		{
			Session.Log(TEXT("[FAIL] reparent_component -> requires UE 5.6+"));
			return sol::lua_nil;
		});
#endif

		// ---- add_animation_channel({name, parent_control, control_type}) ----
		AssetObj.set_function("add_animation_channel", [BP, Hierarchy, Ctrl, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			std::string Name = Params.get_or<std::string>("name", "");
			if (Name.empty()) { Session.Log(TEXT("[FAIL] add_animation_channel -> name required")); return sol::lua_nil; }

			std::string ParentControl = Params.get_or<std::string>("parent_control", "");
			if (ParentControl.empty()) { Session.Log(TEXT("[FAIL] add_animation_channel -> parent_control required")); return sol::lua_nil; }

			FString FElemName = UTF8_TO_TCHAR(Name.c_str());
			FString FParentControl = UTF8_TO_TCHAR(ParentControl.c_str());

			FRigElementKey ParentKey(FName(*FParentControl), ERigElementType::Control);
			if (!Hierarchy->Contains(ParentKey))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_animation_channel -> parent control '%s' not found"), *FParentControl));
				return sol::lua_nil;
			}

			FRigControlSettings Settings;
			Settings.AnimationType = ERigControlAnimationType::AnimationChannel;
			std::string CtrlTypeStr2 = Params.get_or<std::string>("control_type", "Float");
			Settings.ControlType = ParseCtrlType(UTF8_TO_TCHAR(CtrlTypeStr2.c_str()));

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "AddCRAnimChannel", "Add Animation Channel"));
			static_cast<UObject*>(BP)->Modify();

			FRigElementKey ResultKey = Ctrl->AddAnimationChannel(FName(*FElemName), ParentKey, Settings, false, false);
			if (!ResultKey.IsValid())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_animation_channel -> failed to add '%s' under '%s'"), *FElemName, *FParentControl));
				return sol::lua_nil;
			}

			BP->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] add_animation_channel(name=\"%s\", parent_control=\"%s\", type=\"%s\")"),
				*FElemName, *FParentControl, UTF8_TO_TCHAR(CtrlTypeStr2.c_str())));
			return sol::make_object(Lua, true);
		});

		// ---- set_display_name(control, display_name, opts?) ----
		AssetObj.set_function("set_display_name", [BP, Hierarchy, Ctrl, &Session](sol::table /*self*/,
			const std::string& ControlName, const std::string& DisplayName,
			sol::optional<sol::table> Opts, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FCtrlName = UTF8_TO_TCHAR(ControlName.c_str());
			FString FDisplayName = UTF8_TO_TCHAR(DisplayName.c_str());

			FRigElementKey ControlKey(FName(*FCtrlName), ERigElementType::Control);
			if (!Hierarchy->Contains(ControlKey))
			{
				ControlKey = FindElemKey(Hierarchy, FCtrlName);
			}
			if (!ControlKey.IsValid())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_display_name -> '%s' not found"), *FCtrlName));
				return sol::lua_nil;
			}

			bool bRenameElement = false;
			if (Opts.has_value())
			{
				bRenameElement = Opts.value().get_or("rename_element", false);
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "SetDispNameCR", "Set ControlRig Display Name"));
			static_cast<UObject*>(BP)->Modify();
			FName Result = Ctrl->SetDisplayName(ControlKey, FName(*FDisplayName), bRenameElement, false);
			if (Result.IsNone())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_display_name -> failed for '%s'"), *FCtrlName));
				return sol::lua_nil;
			}
			BP->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] set_display_name(\"%s\", \"%s\")"), *FCtrlName, *FDisplayName));
			return sol::make_object(Lua, std::string(TCHAR_TO_UTF8(*Result.ToString())));
		});

		// ---- export_text(names_array) ----
		AssetObj.set_function("export_text", [Hierarchy, Ctrl, &Session](sol::table /*self*/,
			sol::table Names, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			TArray<FRigElementKey> Keys;
			for (const auto& Pair : Names)
			{
				if (Pair.second.is<std::string>())
				{
					FString Name = UTF8_TO_TCHAR(Pair.second.as<std::string>().c_str());
					FRigElementKey Key = FindElemKey(Hierarchy, Name);
					if (Key.IsValid()) Keys.Add(Key);
				}
			}
			if (Keys.Num() == 0)
			{
				Session.Log(TEXT("[FAIL] export_text -> no valid elements"));
				return sol::lua_nil;
			}
			FString Text = Ctrl->ExportToText(Keys);
			Session.Log(FString::Printf(TEXT("[OK] export_text -> %d elements exported (%d chars)"), Keys.Num(), Text.Len()));
			return sol::make_object(Lua, std::string(TCHAR_TO_UTF8(*Text)));
		});

		// ---- import_text(content, opts?) ----
		AssetObj.set_function("import_text", [BP, Ctrl, &Session](sol::table /*self*/,
			const std::string& Content, sol::optional<sol::table> Opts, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FContent = UTF8_TO_TCHAR(Content.c_str());
			if (FContent.IsEmpty())
			{
				Session.Log(TEXT("[FAIL] import_text -> content required"));
				return sol::lua_nil;
			}

			bool bReplaceExisting = false;
			if (Opts.has_value())
			{
				bReplaceExisting = Opts.value().get_or("replace_existing", false);
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "ImportTextCR", "Import ControlRig Text"));
			static_cast<UObject*>(BP)->Modify();
			TArray<FRigElementKey> NewKeys = Ctrl->ImportFromText(FContent, bReplaceExisting, true, false, false);
			if (NewKeys.Num() == 0)
			{
				Session.Log(TEXT("[FAIL] import_text -> no elements imported"));
				return sol::lua_nil;
			}
			BP->MarkPackageDirty();

			sol::table Result = Lua.create_table();
			for (int32 i = 0; i < NewKeys.Num(); ++i)
			{
				Result[i + 1] = TCHAR_TO_UTF8(*NewKeys[i].Name.ToString());
			}
			Session.Log(FString::Printf(TEXT("[OK] import_text -> %d elements imported"), NewKeys.Num()));
			return Result;
		});

		// ---- set_tag(element, tag) ----
		AssetObj.set_function("set_tag", [BP, Hierarchy, &Session](sol::table /*self*/,
			const std::string& Element, const std::string& Tag, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FElem = UTF8_TO_TCHAR(Element.c_str());
			FString FTag = UTF8_TO_TCHAR(Tag.c_str());
			FRigElementKey Key = FindElemKey(Hierarchy, FElem);
			if (!Key.IsValid())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_tag -> element '%s' not found"), *FElem));
				return sol::lua_nil;
			}
			const FScopedTransaction Tx(NSLOCTEXT("AIK", "SetTagCR", "Set ControlRig Tag"));
			static_cast<UObject*>(BP)->Modify();
			if (Hierarchy->SetTag(Key, FName(*FTag)))
			{
				BP->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] set_tag(\"%s\", \"%s\")"), *FElem, *FTag));
				return sol::make_object(Lua, true);
			}
			Session.Log(FString::Printf(TEXT("[FAIL] set_tag -> failed for '%s'"), *FElem));
			return sol::lua_nil;
		});

		// ---- remove_tag(element, tag) ----
		AssetObj.set_function("remove_tag", [BP, Hierarchy, &Session](sol::table /*self*/,
			const std::string& Element, const std::string& Tag, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FElem = UTF8_TO_TCHAR(Element.c_str());
			FString FTag = UTF8_TO_TCHAR(Tag.c_str());
			FRigElementKey Key = FindElemKey(Hierarchy, FElem);
			if (!Key.IsValid())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove_tag -> element '%s' not found"), *FElem));
				return sol::lua_nil;
			}
			const FScopedTransaction Tx(NSLOCTEXT("AIK", "RemTagCR", "Remove ControlRig Tag"));
			static_cast<UObject*>(BP)->Modify();
			TArray<FName> Tags = Hierarchy->GetTags(Key);
			if (Tags.Remove(FName(*FTag)) > 0)
			{
				Hierarchy->SetNameArrayMetadata(Key, URigHierarchy::TagMetadataName, Tags);
				BP->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove_tag(\"%s\", \"%s\")"), *FElem, *FTag));
				return sol::make_object(Lua, true);
			}
			Session.Log(FString::Printf(TEXT("[FAIL] remove_tag -> tag '%s' not found on '%s'"), *FTag, *FElem));
			return sol::lua_nil;
		});

		// ---- add_channel_host(channel, host) ----
		AssetObj.set_function("add_channel_host", [BP, Hierarchy, Ctrl, &Session](sol::table /*self*/,
			const std::string& Channel, const std::string& Host, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FChannel = UTF8_TO_TCHAR(Channel.c_str());
			FString FHost = UTF8_TO_TCHAR(Host.c_str());
			FRigElementKey ChannelKey = FindElemKey(Hierarchy, FChannel);
			FRigElementKey HostKey = FindElemKey(Hierarchy, FHost);
			if (!ChannelKey.IsValid() || !HostKey.IsValid())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_channel_host -> channel '%s' or host '%s' not found"), *FChannel, *FHost));
				return sol::lua_nil;
			}
			const FScopedTransaction Tx(NSLOCTEXT("AIK", "AddChanHostCR", "Add Channel Host"));
			static_cast<UObject*>(BP)->Modify();
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
			if (Ctrl->AddChannelHost(ChannelKey, HostKey, false))
			{
				BP->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add_channel_host(\"%s\", \"%s\")"), *FChannel, *FHost));
				return sol::make_object(Lua, true);
			}
			Session.Log(FString::Printf(TEXT("[FAIL] add_channel_host -> failed for '%s'"), *FChannel));
			return sol::lua_nil;
#else
			Session.Log(TEXT("[FAIL] add_channel_host requires UE 5.5+"));
			return sol::lua_nil;
#endif
		});

		// ---- remove_channel_host(channel, host) ----
		AssetObj.set_function("remove_channel_host", [BP, Hierarchy, Ctrl, &Session](sol::table /*self*/,
			const std::string& Channel, const std::string& Host, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FChannel = UTF8_TO_TCHAR(Channel.c_str());
			FString FHost = UTF8_TO_TCHAR(Host.c_str());
			FRigElementKey ChannelKey = FindElemKey(Hierarchy, FChannel);
			FRigElementKey HostKey = FindElemKey(Hierarchy, FHost);
			if (!ChannelKey.IsValid() || !HostKey.IsValid())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove_channel_host -> channel '%s' or host '%s' not found"), *FChannel, *FHost));
				return sol::lua_nil;
			}
			const FScopedTransaction Tx(NSLOCTEXT("AIK", "RemChanHostCR", "Remove Channel Host"));
			static_cast<UObject*>(BP)->Modify();
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
			if (Ctrl->RemoveChannelHost(ChannelKey, HostKey, false))
			{
				BP->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove_channel_host(\"%s\", \"%s\")"), *FChannel, *FHost));
				return sol::make_object(Lua, true);
			}
			Session.Log(FString::Printf(TEXT("[FAIL] remove_channel_host -> failed for '%s'"), *FChannel));
			return sol::lua_nil;
#else
			Session.Log(TEXT("[FAIL] remove_channel_host requires UE 5.5+"));
			return sol::lua_nil;
#endif
		});

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
		// ---- set_component_content(element, name, content) ----
		AssetObj.set_function("set_component_content", [BP, Hierarchy, Ctrl, &Session](sol::table /*self*/,
			const std::string& Element, const std::string& CompName, const std::string& Content,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FElem = UTF8_TO_TCHAR(Element.c_str());
			FString FCompName = UTF8_TO_TCHAR(CompName.c_str());
			FString FContent = UTF8_TO_TCHAR(Content.c_str());

			FRigElementKey ElementKey = FindElemKey(Hierarchy, FElem);
			if (!ElementKey.IsValid())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_component_content -> element '%s' not found"), *FElem));
				return sol::lua_nil;
			}
			FRigComponentKey ComponentKey(ElementKey, FName(*FCompName));
			if (!Hierarchy->FindComponent(ComponentKey))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_component_content -> component '%s' not found on '%s'"), *FCompName, *FElem));
				return sol::lua_nil;
			}
			const FScopedTransaction Tx(NSLOCTEXT("AIK", "SetCompContentCR", "Set ControlRig Component Content"));
			static_cast<UObject*>(BP)->Modify();
			if (Ctrl->SetComponentContent(ComponentKey, FContent, false))
			{
				BP->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] set_component_content(\"%s\", \"%s\")"), *FElem, *FCompName));
				return sol::make_object(Lua, true);
			}
			Session.Log(FString::Printf(TEXT("[FAIL] set_component_content -> failed for '%s' on '%s'"), *FCompName, *FElem));
			return sol::lua_nil;
		});
#else
		AssetObj.set_function("set_component_content", [&Session](sol::table /*self*/, const std::string& /*Element*/, const std::string& /*CompName*/, const std::string& /*Content*/, sol::this_state S) -> sol::object
		{
			Session.Log(TEXT("[FAIL] set_component_content -> requires UE 5.6+"));
			return sol::lua_nil;
		});
#endif

		// ---- get_control_value(control, opts?) ----
		AssetObj.set_function("get_control_value", [Hierarchy, &Session](sol::table /*self*/,
			const std::string& ControlName, sol::optional<sol::table> Opts, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FCtrlName = UTF8_TO_TCHAR(ControlName.c_str());
			FRigElementKey ControlKey(FName(*FCtrlName), ERigElementType::Control);
			if (!Hierarchy->Contains(ControlKey))
			{
				ControlKey = FindElemKey(Hierarchy, FCtrlName);
				if (!ControlKey.IsValid() || ControlKey.Type != ERigElementType::Control)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] get_control_value -> control '%s' not found"), *FCtrlName));
					return sol::lua_nil;
				}
			}

			ERigControlValueType ValueType = ERigControlValueType::Current;
			if (Opts.has_value())
			{
				std::string VTStr = Opts.value().get_or<std::string>("value_type", "current");
				FString FVT = UTF8_TO_TCHAR(VTStr.c_str());
				if (FVT.Equals(TEXT("initial"), ESearchCase::IgnoreCase)) ValueType = ERigControlValueType::Initial;
				else if (FVT.Equals(TEXT("minimum"), ESearchCase::IgnoreCase)) ValueType = ERigControlValueType::Minimum;
				else if (FVT.Equals(TEXT("maximum"), ESearchCase::IgnoreCase)) ValueType = ERigControlValueType::Maximum;
			}

			FRigControlElement* Elem = Cast<FRigControlElement>(Hierarchy->Find(ControlKey));
			if (!Elem)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] get_control_value -> element cast failed for '%s'"), *FCtrlName));
				return sol::lua_nil;
			}

			FRigControlValue Val = Hierarchy->GetControlValue(ControlKey, ValueType);
			sol::table Result = Lua.create_table();
			Result["control_type"] = TCHAR_TO_UTF8(*CtrlTypeStr(Elem->Settings.ControlType));

			switch (Elem->Settings.ControlType)
			{
			case ERigControlType::Bool:
				Result["value"] = Val.Get<bool>();
				break;
			case ERigControlType::Float:
			case ERigControlType::ScaleFloat:
				Result["x"] = Val.Get<float>();
				break;
			case ERigControlType::Integer:
				Result["x"] = Val.Get<int32>();
				break;
			case ERigControlType::Vector2D:
			case ERigControlType::Position:
			case ERigControlType::Scale:
			case ERigControlType::Rotator:
			{
				FVector3f V = Val.Get<FVector3f>();
				Result["x"] = V.X;
				Result["y"] = V.Y;
				Result["z"] = V.Z;
				break;
			}
			case ERigControlType::EulerTransform:
			{
				const auto& ET = Val.GetRef<FRigControlValue::FEulerTransform_Float>();
				Result["tx"] = ET.TranslationX;
				Result["ty"] = ET.TranslationY;
				Result["tz"] = ET.TranslationZ;
				Result["pitch"] = ET.RotationPitch;
				Result["yaw"] = ET.RotationYaw;
				Result["roll"] = ET.RotationRoll;
				Result["sx"] = ET.ScaleX;
				Result["sy"] = ET.ScaleY;
				Result["sz"] = ET.ScaleZ;
				break;
			}
			default:
				break;
			}
			Session.Log(FString::Printf(TEXT("[OK] get_control_value(\"%s\")"), *FCtrlName));
			return Result;
		});

		// ---- set_control_value(control, value_table, opts?) ----
		AssetObj.set_function("set_control_value", [BP, Hierarchy, &Session](sol::table /*self*/,
			const std::string& ControlName, sol::table ValueTbl, sol::optional<sol::table> Opts, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FCtrlName = UTF8_TO_TCHAR(ControlName.c_str());
			FRigElementKey ControlKey(FName(*FCtrlName), ERigElementType::Control);
			if (!Hierarchy->Contains(ControlKey))
			{
				ControlKey = FindElemKey(Hierarchy, FCtrlName);
				if (!ControlKey.IsValid() || ControlKey.Type != ERigElementType::Control)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] set_control_value -> control '%s' not found"), *FCtrlName));
					return sol::lua_nil;
				}
			}

			ERigControlValueType ValueType = ERigControlValueType::Current;
			if (Opts.has_value())
			{
				std::string VTStr = Opts.value().get_or<std::string>("value_type", "current");
				FString FVT = UTF8_TO_TCHAR(VTStr.c_str());
				if (FVT.Equals(TEXT("initial"), ESearchCase::IgnoreCase)) ValueType = ERigControlValueType::Initial;
				else if (FVT.Equals(TEXT("minimum"), ESearchCase::IgnoreCase)) ValueType = ERigControlValueType::Minimum;
				else if (FVT.Equals(TEXT("maximum"), ESearchCase::IgnoreCase)) ValueType = ERigControlValueType::Maximum;
			}

			FRigControlElement* Elem = Cast<FRigControlElement>(Hierarchy->Find(ControlKey));
			if (!Elem)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_control_value -> element cast failed for '%s'"), *FCtrlName));
				return sol::lua_nil;
			}

			FRigControlValue Val;
			switch (Elem->Settings.ControlType)
			{
			case ERigControlType::Bool:
				Val.Set<bool>(ValueTbl.get_or("value", false));
				break;
			case ERigControlType::Float:
			case ERigControlType::ScaleFloat:
				Val.Set<float>((float)ValueTbl.get_or("x", 0.0));
				break;
			case ERigControlType::Integer:
				Val.Set<int32>((int32)ValueTbl.get_or("x", 0.0));
				break;
			case ERigControlType::Vector2D:
				Val.Set<FVector3f>(FVector3f(
					(float)ValueTbl.get_or("x", 0.0),
					(float)ValueTbl.get_or("y", 0.0),
					0.f));
				break;
			case ERigControlType::Position:
			case ERigControlType::Scale:
			case ERigControlType::Rotator:
				Val.Set<FVector3f>(FVector3f(
					(float)ValueTbl.get_or("x", 0.0),
					(float)ValueTbl.get_or("y", 0.0),
					(float)ValueTbl.get_or("z", 0.0)));
				break;
			case ERigControlType::EulerTransform:
			{
				FEulerTransform ZeroET;
				FRigControlValue::FEulerTransform_Float ET(ZeroET);
				// Accept both tx/ty/tz and x/y/z (fallback) for translation
				ET.TranslationX = (float)ValueTbl.get_or("tx", ValueTbl.get_or("x", 0.0));
				ET.TranslationY = (float)ValueTbl.get_or("ty", ValueTbl.get_or("y", 0.0));
				ET.TranslationZ = (float)ValueTbl.get_or("tz", ValueTbl.get_or("z", 0.0));
				ET.RotationPitch = (float)ValueTbl.get_or("pitch", 0.0);
				ET.RotationYaw = (float)ValueTbl.get_or("yaw", 0.0);
				ET.RotationRoll = (float)ValueTbl.get_or("roll", 0.0);
				ET.ScaleX = (float)ValueTbl.get_or("sx", 1.0);
				ET.ScaleY = (float)ValueTbl.get_or("sy", 1.0);
				ET.ScaleZ = (float)ValueTbl.get_or("sz", 1.0);
				Val.Set<FRigControlValue::FEulerTransform_Float>(ET);
				break;
			}
			default:
				Session.Log(FString::Printf(TEXT("[FAIL] set_control_value -> unsupported control type for '%s'"), *FCtrlName));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "SetCtrlValCR", "Set ControlRig Control Value"));
			static_cast<UObject*>(BP)->Modify();
			Hierarchy->SetControlValue(ControlKey, Val, ValueType, false);
			BP->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] set_control_value(\"%s\")"), *FCtrlName));
			return sol::make_object(Lua, true);
		});

		// ---- info() ----
		AssetObj.set_function("info", [BP, Hierarchy, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			sol::table Result = Lua.create_table();

			int32 Bones = 0, Controls = 0, Nulls = 0, Curves = 0, Other = 0;
			Hierarchy->ForEach<FRigBaseElement>([&](FRigBaseElement* Elem) -> bool
			{
				switch (Elem->GetKey().Type)
				{
				case ERigElementType::Bone: Bones++; break;
				case ERigElementType::Control: Controls++; break;
				case ERigElementType::Null: Nulls++; break;
				case ERigElementType::Curve: Curves++; break;
				default: Other++; break;
				}
				return true;
			});

			Result["bones"] = Bones;
			Result["controls"] = Controls;
			Result["nulls"] = Nulls;
			Result["curves"] = Curves;
			Result["other"] = Other;
			Result["total"] = Hierarchy->Num();

			TArray<FRigVMGraphVariableDescription> Vars = BP->GetMemberVariables();
			Result["variables"] = Vars.Num();

			Session.Log(FString::Printf(TEXT("[OK] info() -> %d bones, %d controls, %d nulls, %d curves, %d vars"),
				Bones, Controls, Nulls, Curves, Vars.Num()));
			return Result;
		});
	});
}

REGISTER_LUA_BINDING(ControlRig, ControlRigDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("ControlRig")))
	{
		Session.Log(TEXT("[WARN] ControlRig plugin is not loaded. Enable it in Edit > Plugins to use this feature."));
		return;
	}
	BindControlRig(Lua, Session);
});
