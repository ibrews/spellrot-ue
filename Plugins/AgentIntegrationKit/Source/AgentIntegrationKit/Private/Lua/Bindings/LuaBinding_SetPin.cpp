#include "Lua/LuaBindingRegistry.h"
#include "Modules/ModuleManager.h"
#if WITH_CONTROLRIG
#include "Lua/LuaControlRigHelper.h"
#endif
#include "Lua/LuaPinHelper.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

static UEdGraphPin* FindPinByName(UEdGraphNode* Node, const FString& PinName)
{
	if (!Node) return nullptr;

	// Exact PinName match
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin) continue;
		if (Pin->PinName.ToString() == PinName)
			return Pin;
	}

	// Display name
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin) continue;
		if (Pin->GetDisplayName().ToString() == PinName)
			return Pin;
	}

	// Generated usable name (handles "in_0", "out_0" for blank pins)
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin) continue;
		if (NeoLuaPin::GetUsablePinName(Pin) == PinName)
			return Pin;
	}

	// Case-insensitive (all name sources)
	FString PinLower = PinName.ToLower();
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin) continue;
		if (Pin->PinName.ToString().ToLower() == PinLower
			|| Pin->GetDisplayName().ToString().ToLower() == PinLower
			|| NeoLuaPin::GetUsablePinName(Pin).ToLower() == PinLower)
			return Pin;
	}

	return nullptr;
}

static FString ListAllPins(UEdGraphNode* Node)
{
	if (!Node) return FString();
	FString List;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->bHidden) continue;
		if (List.Len() > 0) List += TEXT(", ");
		FString Name = NeoLuaPin::GetUsablePinName(Pin);
		FString Dir = Pin->Direction == EGPD_Input ? TEXT("in") : TEXT("out");
		FString Type = Pin->PinType.PinCategory.ToString();
		if (Pin->PinType.PinSubCategoryObject.IsValid())
		{
			Type += FString::Printf(TEXT("<%s>"), *Pin->PinType.PinSubCategoryObject->GetName());
		}
		List += FString::Printf(TEXT("%s (%s, %s)"), *Name, *Dir, *Type);
	}
	return List;
}

static FString SolToString(const sol::object& Obj)
{
	if (Obj.is<std::string>()) return UTF8_TO_TCHAR(Obj.as<std::string>().c_str());
	if (Obj.is<double>())
	{
		double Val = Obj.as<double>();
		if (FMath::IsNearlyEqual(Val, FMath::RoundToDouble(Val)))
			return FString::Printf(TEXT("%lld"), (long long)Val);
		return FString::Printf(TEXT("%g"), Val);
	}
	if (Obj.is<bool>()) return Obj.as<bool>() ? TEXT("true") : TEXT("false");
	return TEXT("");
}

static double TableGetNum(const sol::table& T, const char* Key, double Default = 0.0)
{
	auto V = T.get<sol::optional<double>>(Key);
	return V.has_value() ? V.value() : Default;
}

// Find the "inner instanced object" on a graph node — the UObject that holds the node's
// configurable parameters (e.g., USoundNode on SoundCueGraphNode, UBTNode on BehaviorTreeGraphNode,
// UMaterialExpression on MaterialGraphNode). Uses UE's Instanced UPROPERTY pattern.
// Returns nullptr if the node stores properties directly on itself (Blueprint K2, Niagara, AnimGraph).
static UObject* FindNodeInnerObject(UEdGraphNode* Node)
{
	if (!Node) return nullptr;

	// Well-known property names (fast path, avoids full class iteration)
	static const FName KnownNames[] = {
		TEXT("SoundNode"),          // SoundCueGraphNode
		TEXT("NodeInstance"),       // UAIGraphNode (BehaviorTree, EQS)
		TEXT("MaterialExpression"), // MaterialGraphNode
	};

	for (const FName& PropName : KnownNames)
	{
		FObjectProperty* ObjProp = CastField<FObjectProperty>(Node->GetClass()->FindPropertyByName(PropName));
		if (ObjProp)
		{
			UObject* Inner = ObjProp->GetObjectPropertyValue_InContainer(Node);
			if (Inner) return Inner;
		}
	}

	// Generic fallback: scan for any Instanced UObject* property
	for (TFieldIterator<FObjectProperty> It(Node->GetClass()); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_InstancedReference) && !It->HasAnyPropertyFlags(CPF_Transient))
		{
			UObject* Inner = It->GetObjectPropertyValue_InContainer(Node);
			if (Inner && !Inner->IsA<UEdGraph>())
			{
				return Inner;
			}
		}
	}

	return nullptr;
}

// Try to set a property on a graph node's inner object. Returns true if handled (success or error logged).
static bool SetPin_HandleInnerObject(FLuaSessionData& Session, UEdGraphNode* Node, const FString& FHandle,
	const FString& FPinName, const FString& FValue, sol::this_state S, sol::object& OutResult)
{
	UObject* Inner = FindNodeInnerObject(Node);
	if (!Inner) return false;

	FProperty* Prop = Inner->GetClass()->FindPropertyByName(FName(*FPinName));
	// Case-insensitive fallback
	if (!Prop)
	{
		for (TFieldIterator<FProperty> It(Inner->GetClass()); It; ++It)
		{
			if (It->GetName().Equals(FPinName, ESearchCase::IgnoreCase)
				|| It->GetAuthoredName().Equals(FPinName, ESearchCase::IgnoreCase))
			{
				Prop = *It;
				break;
			}
		}
	}

	if (Prop)
	{
		Inner->Modify();
		void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Inner);
		if (Prop->ImportText_Direct(*FValue, ValuePtr, Inner, PPF_None))
		{
			FPropertyChangedEvent Evt(Prop);
			Inner->PostEditChangeProperty(Evt);
			Session.Log(FString::Printf(TEXT("[OK] set_pin(%s, \"%s\") = \"%s\" (%s property)"),
				*FHandle, *FPinName, *FValue, *Inner->GetClass()->GetName()));
			OutResult = sol::make_object(S, true);
			return true;
		}
	}

	// Property not found on inner object — list available ones
	FString PropList;
	for (TFieldIterator<FProperty> It(Inner->GetClass()); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_Edit))
		{
			if (PropList.Len() > 0) PropList += TEXT(", ");
			PropList += It->GetName();
		}
	}
	Session.Log(FString::Printf(TEXT("[FAIL] set_pin -> property \"%s\" not found on %s. Available: %s"),
		*FPinName, *Inner->GetClass()->GetName(), *PropList));
	OutResult = sol::object(sol::lua_nil);
	return true;
}

// Try to get a property from a graph node's inner object. Returns true if handled.
static bool GetPin_HandleInnerObject(FLuaSessionData& Session, UEdGraphNode* Node,
	const FString& FPinName, sol::this_state S, sol::object& OutResult)
{
	UObject* Inner = FindNodeInnerObject(Node);
	if (!Inner) return false;

	FProperty* Prop = Inner->GetClass()->FindPropertyByName(FName(*FPinName));
	if (!Prop)
	{
		for (TFieldIterator<FProperty> It(Inner->GetClass()); It; ++It)
		{
			if (It->GetName().Equals(FPinName, ESearchCase::IgnoreCase)
				|| It->GetAuthoredName().Equals(FPinName, ESearchCase::IgnoreCase))
			{
				Prop = *It;
				break;
			}
		}
	}

	if (Prop)
	{
		FString Value;
		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Inner);
		Prop->ExportTextItem_Direct(Value, ValuePtr, nullptr, Inner, PPF_None);
		OutResult = sol::make_object(S, std::string(TCHAR_TO_UTF8(*Value)));
		return true;
	}

	return false; // not handled — let caller show normal pin error
}

// Check if a node lives in a material graph (uses UMaterialGraphSchema).
// Uses class name check to avoid adding a MaterialGraphSchema.h include.
static bool IsMaterialGraphPin(UEdGraphPin* Pin)
{
	if (!Pin) return false;
	UEdGraphNode* Node = Pin->GetOwningNode();
	if (!Node) return false;
	UEdGraph* Graph = Node->GetGraph();
	if (!Graph) return false;
	const UEdGraphSchema* Schema = Graph->GetSchema();
	return Schema && Schema->GetClass()->GetName() == TEXT("MaterialGraphSchema");
}

// Convert a Lua table to the value string format expected by UMaterialExpression::PinDefaultValueChanged.
// Material pins use custom PinCategories (PC_Mask, PC_Required, etc.) — not PC_Struct —
// so the generic TableToStructPinValue doesn't apply. The engine format depends on PinSubCategory:
//   "rgb"     → 3 comma-separated numbers (HideAlphaChannel FLinearColor, FVector, or 3 scalars)
//   "rgba"    → InitFromString "(R=%g,G=%g,B=%g,A=%g)" (FLinearColor without HideAlphaChannel)
//   "vector4" → 4 comma-separated numbers (FVector4)
//   "rg"      → InitFromString "(X=%g,Y=%g)" (FVector2D or 2 scalars)
static FString TableToMaterialPinValue(const sol::object& Value, UEdGraphPin* Pin)
{
	if (!Value.is<sol::table>() || !Pin) return FString();
	if (!IsMaterialGraphPin(Pin)) return FString();

	sol::table T = Value.as<sol::table>();
	FName SubCat = Pin->PinType.PinSubCategory;

	// Color tables: {r=, g=, b=, [a=]}
	auto HasR = T.get<sol::optional<double>>("r");
	if (HasR.has_value())
	{
		double R = TableGetNum(T, "r");
		double G = TableGetNum(T, "g");
		double B = TableGetNum(T, "b");

		if (SubCat == FName(TEXT("rgb")))
		{
			return FString::Printf(TEXT("%g,%g,%g"), R, G, B);
		}
		if (SubCat == FName(TEXT("rgba")))
		{
			double A = TableGetNum(T, "a", 1.0);
			return FString::Printf(TEXT("(R=%g,G=%g,B=%g,A=%g)"), R, G, B, A);
		}
		if (SubCat == FName(TEXT("vector4")))
		{
			double A = TableGetNum(T, "a", 0.0);
			return FString::Printf(TEXT("%g,%g,%g,%g"), R, G, B, A);
		}
		// Fallback for color tables on unknown subcategory: 3-comma format (safest default)
		return FString::Printf(TEXT("%g,%g,%g"), R, G, B);
	}

	// Vector tables: {x=, y=, z=, [w=]}
	auto HasX = T.get<sol::optional<double>>("x");
	if (HasX.has_value())
	{
		double X = TableGetNum(T, "x");
		double Y = TableGetNum(T, "y");
		double Z = TableGetNum(T, "z");

		if (SubCat == FName(TEXT("rgb")))
		{
			return FString::Printf(TEXT("%g,%g,%g"), X, Y, Z);
		}
		if (SubCat == FName(TEXT("vector4")))
		{
			double W = TableGetNum(T, "w", 0.0);
			return FString::Printf(TEXT("%g,%g,%g,%g"), X, Y, Z, W);
		}
		if (SubCat == FName(TEXT("rg")))
		{
			return FString::Printf(TEXT("(X=%g,Y=%g)"), X, Y);
		}
		// Fallback: 3-comma format
		return FString::Printf(TEXT("%g,%g,%g"), X, Y, Z);
	}

	return FString();
}

// Convert a Lua table to the UE string format for a struct pin, based on the pin's struct type.
// Returns empty string if the pin is not a recognized struct type or value is not a table.
static FString TableToStructPinValue(const sol::object& Value, UEdGraphPin* Pin)
{
	if (!Value.is<sol::table>() || !Pin) return FString();

	// Must be a struct pin
	if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Struct) return FString();

	UScriptStruct* StructType = Cast<UScriptStruct>(Pin->PinType.PinSubCategoryObject.Get());
	if (!StructType) return FString();

	sol::table T = Value.as<sol::table>();
	FName StructName = StructType->GetFName();

	// FVector / FVector3d / FVector3f — comma-separated "x,y,z"
	// ParseVector primary path expects this format; also accepted by FVector::InitFromString fallback
	if (StructName == NAME_Vector || StructName == TEXT("Vector3d") || StructName == TEXT("Vector3f"))
	{
		return FString::Printf(TEXT("%g,%g,%g"),
			TableGetNum(T, "x"), TableGetNum(T, "y"), TableGetNum(T, "z"));
	}

	// FRotator — comma-separated "pitch,yaw,roll"
	// IsStringValidRotator delegates to IsStringValidVector -> ParseVector which expects comma-separated.
	// ParseRotator maps (X,Y,Z) -> (Pitch,Yaw,Roll). Named "P= Y= R=" format does NOT pass
	// IsStringValidVector validation (it falls back to FVector3d::InitFromString which expects X=/Y=/Z=).
	if (StructName == NAME_Rotator)
	{
		return FString::Printf(TEXT("%g,%g,%g"),
			TableGetNum(T, "pitch"), TableGetNum(T, "yaw"), TableGetNum(T, "roll"));
	}

	// FLinearColor — "(R=%g,G=%g,B=%g,A=%g)"
	if (StructName == NAME_LinearColor)
	{
		return FString::Printf(TEXT("(R=%g,G=%g,B=%g,A=%g)"),
			TableGetNum(T, "r"), TableGetNum(T, "g"), TableGetNum(T, "b"), TableGetNum(T, "a", 1.0));
	}

	// FTransform — combine translation, rotation, scale
	if (StructName == NAME_Transform)
	{
		// Translation
		double TX = 0, TY = 0, TZ = 0;
		auto TransSub = T.get<sol::optional<sol::table>>("translation");
		if (TransSub.has_value())
		{
			TX = TableGetNum(TransSub.value(), "x");
			TY = TableGetNum(TransSub.value(), "y");
			TZ = TableGetNum(TransSub.value(), "z");
		}

		// Rotation (as Euler pitch/yaw/roll)
		double RP = 0, RY = 0, RR = 0;
		auto RotSub = T.get<sol::optional<sol::table>>("rotation");
		if (RotSub.has_value())
		{
			RP = TableGetNum(RotSub.value(), "pitch");
			RY = TableGetNum(RotSub.value(), "yaw");
			RR = TableGetNum(RotSub.value(), "roll");
		}

		// Scale (default 1,1,1)
		double SX = 1, SY = 1, SZ = 1;
		auto ScaleSub = T.get<sol::optional<sol::table>>("scale");
		if (ScaleSub.has_value())
		{
			SX = TableGetNum(ScaleSub.value(), "x", 1.0);
			SY = TableGetNum(ScaleSub.value(), "y", 1.0);
			SZ = TableGetNum(ScaleSub.value(), "z", 1.0);
		}

		// FTransform::InitFromString expects "%f,%f,%f|%f,%f,%f|%f,%f,%f" (translation|rotation|scale)
		// Rotation component parsed by ParseRotator: comma-separated maps (X,Y,Z)->(Pitch,Yaw,Roll)
		return FString::Printf(TEXT("%g,%g,%g|%g,%g,%g|%g,%g,%g"),
			TX, TY, TZ, RP, RY, RR, SX, SY, SZ);
	}

	// FVector2D — "X=%g Y=%g"
	if (StructName == TEXT("Vector2D"))
	{
		return FString::Printf(TEXT("(X=%g,Y=%g)"),
			TableGetNum(T, "x"), TableGetNum(T, "y"));
	}

	return FString();
}

// Helper functions extracted outside REGISTER_LUA_BINDING to avoid #if inside macro arguments (MSVC C5101)
#if WITH_CONTROLRIG
static bool SetPin_HandleControlRig(FLuaSessionData& Session, UEdGraphNode* Node, const FString& FHandle, const FString& FPinName,
	const FString& FValue, sol::this_state S, sol::object& OutResult)
{
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("ControlRig"))) return false;
	if (!LuaControlRig::IsRigVMNode(Node)) return false;

	UEdGraph* Graph = Node->GetGraph();
	FString Error;
	bool bSuccess = LuaControlRig::SetPinValue(Graph, Node, FPinName, FValue, Error);
	if (bSuccess)
	{
		Session.Log(FString::Printf(TEXT("[OK] set_pin(%s, \"%s\") = \"%s\" via RigVM controller"), *FHandle, *FPinName, *FValue));
		OutResult = sol::make_object(S, true);
		return true;
	}
	Session.Log(FString::Printf(TEXT("[FAIL] set_pin(%s, \"%s\") -> %s"), *FHandle, *FPinName, *Error));
	OutResult = sol::object(sol::lua_nil);
	return true;
}

static bool GetPin_HandleControlRig(FLuaSessionData& Session, UEdGraphNode* Node, const FString& FPinName, sol::this_state S, sol::object& OutResult)
{
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("ControlRig"))) return false;
	if (!LuaControlRig::IsRigVMNode(Node)) return false;

	UEdGraph* Graph = Node->GetGraph();
	FString Value = LuaControlRig::GetPinValue(Graph, Node, FPinName);
	OutResult = sol::make_object(S, std::string(TCHAR_TO_UTF8(*Value)));
	return true;
}
#else
static bool SetPin_HandleControlRig(FLuaSessionData&, UEdGraphNode*, const FString&, const FString&,
	const FString&, sol::this_state, sol::object&) { return false; }
static bool GetPin_HandleControlRig(FLuaSessionData&, UEdGraphNode*, const FString&, sol::this_state, sol::object&) { return false; }
#endif

static TArray<FLuaFunctionDoc> SetPinDocs = {
	{
		TEXT("set_pin(handle, pin_name, value)"),
		TEXT("Set the default value of a pin on a node. Pin name is case-insensitive. Struct pins accept tables: {x,y,z} for Vector/Rotator, {r,g,b,a} for LinearColor, {translation={},rotation={},scale={}} for Transform."),
		TEXT("true on success, nil on failure")
	},
	{
		TEXT("get_pin(handle, pin_name)"),
		TEXT("Get the default value of a pin on a node. Pin name is case-insensitive."),
		TEXT("string value or nil")
	},
	{
		TEXT("reset_pin(handle, pin_name)"),
		TEXT("Reset a pin's default value back to its autogenerated default. Pin name is case-insensitive."),
		TEXT("true on success, nil on failure")
	}
};

REGISTER_LUA_BINDING(SetPin, SetPinDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("set_pin", [&Session](const std::string& Handle, const std::string& PinName,
		sol::object Value, sol::this_state S) -> sol::object
	{
		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());
		FString FPinName = UTF8_TO_TCHAR(PinName.c_str());

		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] set_pin -> node \"%s\" not found. Call read_graph() or add_node() first."), *FHandle));
			return sol::lua_nil;
		}

		Session.MarkGraphDirty(Node->GetGraph());

		// ControlRig/RigVM: resolve value as string early (no struct table support yet)
		{
			FString FValueCR = SolToString(Value);
			sol::object CRResult;
			if (SetPin_HandleControlRig(Session, Node, FHandle, FPinName, FValueCR, S, CRResult))
				return CRResult;
		}

		UEdGraphPin* Pin = FindPinByName(Node, FPinName);
		if (!Pin)
		{
			// Generic inner object fallback: many graph node types (SoundCue, BehaviorTree, Material, etc.)
			// store parameters as UPROPERTYs on an inner instanced object, not as pin defaults.
			{
				FString FValue = SolToString(Value);
				sol::object InnerResult;
				if (SetPin_HandleInnerObject(Session, Node, FHandle, FPinName, FValue, S, InnerResult))
					return InnerResult;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] set_pin -> pin \"%s\" not found on node. Available: %s"),
				*FPinName, *ListAllPins(Node)));
			return sol::lua_nil;
		}

		// Try material pin table conversion first (material pins don't use PC_Struct)
		FString FValue = TableToMaterialPinValue(Value, Pin);
		// Try struct table conversion (Vector, Rotator, Transform, LinearColor, Vector2D)
		if (FValue.IsEmpty())
		{
			FValue = TableToStructPinValue(Value, Pin);
		}
		if (FValue.IsEmpty())
		{
			FValue = SolToString(Value);
		}

		if (Pin->Direction != EGPD_Input)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] set_pin -> pin \"%s\" is an output pin, can only set default values on input pins"), *FPinName));
			return sol::lua_nil;
		}

		if (Pin->PinType.IsContainer())
		{
			FString ContainerKind = Pin->PinType.IsArray() ? TEXT("array") : (Pin->PinType.IsSet() ? TEXT("set") : TEXT("map"));
			Session.Log(FString::Printf(TEXT("[FAIL] set_pin -> pin \"%s\" is a %s pin. Container pins cannot have string default values — wire a Make%s node instead."),
				*FPinName, *ContainerKind, *ContainerKind));
			return sol::lua_nil;
		}

		UEdGraph* Graph = Node->GetGraph();
		if (!Graph)
		{
			Session.Log(TEXT("[FAIL] set_pin -> node has no owning graph"));
			return sol::lua_nil;
		}
		const UEdGraphSchema* Schema = Graph->GetSchema();
		if (!Schema)
		{
			Session.Log(TEXT("[FAIL] set_pin -> could not get graph schema"));
			return sol::lua_nil;
		}

		// Pre-validate using schema to get specific error messages.
		// Skip for object/class/interface/text pins — TrySetDefaultValue internally resolves
		// string paths to DefaultObject via GetPinDefaultValuesFromString, but IsPinDefaultValid
		// rejects raw path strings for those categories (it expects resolved values).
		{
			const FName& Cat = Pin->PinType.PinCategory;
			bool bSkipPreValidation = (Cat == UEdGraphSchema_K2::PC_Object
				|| Cat == UEdGraphSchema_K2::PC_Class
				|| Cat == UEdGraphSchema_K2::PC_Interface
				|| Cat == UEdGraphSchema_K2::PC_SoftObject
				|| Cat == UEdGraphSchema_K2::PC_SoftClass
				|| Cat == UEdGraphSchema_K2::PC_Text);

			if (!bSkipPreValidation)
			{
				FString ValidationError = Schema->IsPinDefaultValid(Pin, FValue, nullptr, FText::GetEmpty());
				if (!ValidationError.IsEmpty() && ValidationError != TEXT("Not implemented by this schema"))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] set_pin(%s, \"%s\") -> invalid value \"%s\": %s"),
						*FHandle, *FPinName, *FValue, *ValidationError));
					return sol::lua_nil;
				}
			}
		}

		FString ValueBefore = Pin->GetDefaultAsString();
		UObject* ObjBefore = Pin->DefaultObject;
		Schema->TrySetDefaultValue(*Pin, FValue);

		// For class/object/interface pins, TrySetDefaultValue sets Pin->DefaultObject
		// (not Pin->DefaultValue). Use GetDefaultAsString() which handles both.
		FString ValueAfter = Pin->GetDefaultAsString();

		if (ValueAfter == FValue)
		{
			// Exact match — value set successfully
			Session.Log(FString::Printf(TEXT("[OK] set_pin(%s, \"%s\") = \"%s\""), *FHandle, *FPinName, *FValue));
			return sol::make_object(S, true);
		}

		// Check if DefaultObject changed (class/object/soft-ref pins)
		if (Pin->DefaultObject != ObjBefore && Pin->DefaultObject != nullptr)
		{
			Session.Log(FString::Printf(TEXT("[OK] set_pin(%s, \"%s\") = \"%s\""), *FHandle, *FPinName, *ValueAfter));
			return sol::make_object(S, true);
		}

		if (ValueAfter != ValueBefore)
		{
			// Value changed but format differs (e.g. "0.3" stored as "0.300000")
			Session.Log(FString::Printf(TEXT("[OK] set_pin(%s, \"%s\") = \"%s\""), *FHandle, *FPinName, *ValueAfter));
			return sol::make_object(S, true);
		}

		Session.Log(FString::Printf(TEXT("[FAIL] set_pin(%s, \"%s\") -> failed to set value \"%s\" (current: \"%s\")"),
			*FHandle, *FPinName, *FValue, *ValueAfter));
		return sol::lua_nil;
	});

	Lua.set_function("get_pin", [&Session](const std::string& Handle, const std::string& PinName,
		sol::this_state S) -> sol::object
	{
		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());
		FString FPinName = UTF8_TO_TCHAR(PinName.c_str());

		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] get_pin -> node \"%s\" not found. Call read_graph() or add_node() first."), *FHandle));
			return sol::lua_nil;
		}

		// ControlRig/RigVM: use URigVMController::GetPinDefaultValue
		{
			sol::object CRResult;
			if (GetPin_HandleControlRig(Session, Node, FPinName, S, CRResult))
				return CRResult;
		}

		UEdGraphPin* Pin = FindPinByName(Node, FPinName);
		if (!Pin)
		{
			// Generic inner object fallback for graph nodes with instanced backing objects
			{
				sol::object InnerResult;
				if (GetPin_HandleInnerObject(Session, Node, FPinName, S, InnerResult))
					return InnerResult;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] get_pin -> pin \"%s\" not found on node. Available: %s"),
				*FPinName, *ListAllPins(Node)));
			return sol::lua_nil;
		}

		// Use GetDefaultAsString() to handle both string values and object references
		// (class/object pins store their value in DefaultObject, not DefaultValue)
		return sol::make_object(S, std::string(TCHAR_TO_UTF8(*Pin->GetDefaultAsString())));
	});

	Lua.set_function("reset_pin", [&Session](const std::string& Handle, const std::string& PinName,
		sol::this_state S) -> sol::object
	{
		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());
		FString FPinName = UTF8_TO_TCHAR(PinName.c_str());

		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] reset_pin -> node \"%s\" not found. Call read_graph() or add_node() first."), *FHandle));
			return sol::lua_nil;
		}

		UEdGraphPin* Pin = FindPinByName(Node, FPinName);
		if (!Pin)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] reset_pin -> pin \"%s\" not found on node. Available: %s"),
				*FPinName, *ListAllPins(Node)));
			return sol::lua_nil;
		}

		UEdGraph* Graph = Node->GetGraph();
		if (!Graph)
		{
			Session.Log(TEXT("[FAIL] reset_pin -> node has no owning graph"));
			return sol::lua_nil;
		}
		const UEdGraphSchema* Schema = Graph->GetSchema();
		if (!Schema)
		{
			Session.Log(TEXT("[FAIL] reset_pin -> could not get graph schema"));
			return sol::lua_nil;
		}

		Session.MarkGraphDirty(Graph);
		Schema->ResetPinToAutogeneratedDefaultValue(Pin);

		FString ValueAfter = Pin->GetDefaultAsString();
		Session.Log(FString::Printf(TEXT("[OK] reset_pin(%s, \"%s\") -> default restored to \"%s\""), *FHandle, *FPinName, *ValueAfter));
		return sol::make_object(S, true);
	});
});
