#include "Lua/LuaBindingRegistry.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
#include "NiagaraEmitterBase.h"
#endif
#include "NiagaraEmitterHandle.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraGraph.h"
#include "NiagaraNode.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeInput.h"
#include "NiagaraSimulationStageBase.h"
#include "NiagaraCommon.h"
#include "NiagaraTypes.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
#include "NiagaraTypeRegistry.h"
#endif
#include "NiagaraEditorUtilities.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraRibbonRendererProperties.h"
#include "NiagaraLightRendererProperties.h"
#include "NiagaraDecalRendererProperties.h"
#include "NiagaraVolumeRendererProperties.h"
#include "NiagaraComponentRendererProperties.h"
#include "Lua/LuaDynamicTypeHelper.h"
#include "NiagaraParameterStore.h"
#include "NiagaraUserRedirectionParameterStore.h"
#include "NiagaraNodeCustomHlsl.h"
#include "NiagaraScriptVariable.h"
#include "NiagaraParameterDefinitionsBase.h"
#include "NiagaraParameterDefinitionsSubscriber.h"
#include "NiagaraValidationRule.h"
#include "NiagaraValidationRules.h"
#include "NiagaraValidationRuleSet.h"
#include "ViewModels/NiagaraSystemViewModel.h"
#include "NiagaraDataInterface.h"
#include "NiagaraDataInterfaceCurve.h"
#include "NiagaraDataInterfaceColorCurve.h"
#include "NiagaraDataInterfaceVectorCurve.h"
#include "NiagaraDataInterfaceVector2DCurve.h"
#include "NiagaraDataInterfaceVector4Curve.h"
// UNiagaraStatelessEmitter is in Internal/ headers - forward declared in NiagaraEmitterHandle.h.
// We access its properties via UE reflection to avoid internal include path dependency.
#include "EdGraphSchema_Niagara.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#include "ViewModels/Stack/NiagaraParameterHandle.h"
#include "NiagaraParameterMapHistory.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"
#include "Modules/ModuleManager.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// Helpers
// ============================================================================

namespace
{

ENiagaraScriptUsage ParseUsage(const FString& S)
{
	// Lowercase for case-insensitive matching of all variants
	FString L = S.ToLower();
	// System-level (check first — "systemspawn" contains "spawn")
	if (L.Contains(TEXT("system_spawn")) || L.Contains(TEXT("systemspawn"))) return ENiagaraScriptUsage::SystemSpawnScript;
	if (L.Contains(TEXT("system_update")) || L.Contains(TEXT("systemupdate"))) return ENiagaraScriptUsage::SystemUpdateScript;
	// Emitter-level (check before generic "spawn"/"update")
	if (L.Contains(TEXT("emitter_spawn")) || L.Contains(TEXT("emitterspawn"))) return ENiagaraScriptUsage::EmitterSpawnScript;
	if (L.Contains(TEXT("emitter_update")) || L.Contains(TEXT("emitterupdate"))) return ENiagaraScriptUsage::EmitterUpdateScript;
	// Event
	if (L.Contains(TEXT("particle_event")) || L.Contains(TEXT("particleevent")) || L.Contains(TEXT("event"))) return ENiagaraScriptUsage::ParticleEventScript;
	// Simulation stage
	if (L.Contains(TEXT("simstage")) || L.Contains(TEXT("simulation_stage")) || L.Contains(TEXT("simulationstage"))) return ENiagaraScriptUsage::ParticleSimulationStageScript;
	// Particle-level (generic "spawn"/"update" as fallback)
	if (L.Contains(TEXT("particle_spawn")) || L.Contains(TEXT("particlespawn")) || L.Contains(TEXT("spawn"))) return ENiagaraScriptUsage::ParticleSpawnScript;
	if (L.Contains(TEXT("particle_update")) || L.Contains(TEXT("particleupdate")) || L.Contains(TEXT("update"))) return ENiagaraScriptUsage::ParticleUpdateScript;
	return ENiagaraScriptUsage::ParticleUpdateScript;
}

FString UsageToStr(ENiagaraScriptUsage U)
{
	switch (U)
	{
	case ENiagaraScriptUsage::ParticleSpawnScript: return TEXT("particle_spawn");
	case ENiagaraScriptUsage::ParticleUpdateScript: return TEXT("particle_update");
	case ENiagaraScriptUsage::EmitterSpawnScript: return TEXT("emitter_spawn");
	case ENiagaraScriptUsage::EmitterUpdateScript: return TEXT("emitter_update");
	case ENiagaraScriptUsage::SystemSpawnScript: return TEXT("system_spawn");
	case ENiagaraScriptUsage::SystemUpdateScript: return TEXT("system_update");
	case ENiagaraScriptUsage::ParticleEventScript: return TEXT("particle_event");
	case ENiagaraScriptUsage::ParticleSimulationStageScript: return TEXT("simulation_stage");
	default: return TEXT("unknown");
	}
}

static void ConfigureValidationViewModelOptions(FNiagaraSystemViewModelOptions& Options)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	Options.bCompileForEdit = false;
#else
	(void)Options;
#endif
}

int32 FindEmitterIndex(UNiagaraSystem* System, const FString& Name)
{
	if (!System || Name.IsEmpty()) return INDEX_NONE;
	for (int32 i = 0; i < System->GetNumEmitters(); i++)
	{
		const FNiagaraEmitterHandle& Handle = System->GetEmitterHandle(i);
		if (Handle.GetName().ToString().Equals(Name, ESearchCase::IgnoreCase))
			return i;
	}
	return INDEX_NONE;
}

bool IsParameterMapPin(const UEdGraphPin* Pin)
{
	if (!Pin) return false;
	FNiagaraTypeDefinition PinDef = UEdGraphSchema_Niagara::PinToTypeDefinition(Pin);
	return PinDef == FNiagaraTypeDefinition::GetParameterMapDef();
}

UEdGraphPin* GetParamMapInputPin(UEdGraphNode* Node)
{
	if (!Node) return nullptr;
	for (UEdGraphPin* Pin : Node->GetAllPins())
	{
		if (Pin->Direction == EGPD_Input && IsParameterMapPin(Pin))
			return Pin;
	}
	return nullptr;
}

UEdGraphPin* GetParamMapOutputPin(UEdGraphNode* Node)
{
	if (!Node) return nullptr;
	for (UEdGraphPin* Pin : Node->GetAllPins())
	{
		if (Pin->Direction == EGPD_Output && IsParameterMapPin(Pin))
			return Pin;
	}
	return nullptr;
}

void CollectSubGraphNodes(UEdGraphNode* Node, TArray<UEdGraphNode*>& Out, TSet<UEdGraphNode*>& Visited)
{
	if (!Node || Visited.Contains(Node)) return;
	Visited.Add(Node);
	Out.Add(Node);
	for (UEdGraphPin* Pin : Node->GetAllPins())
	{
		if (Pin->Direction == EGPD_Input)
		{
			for (UEdGraphPin* Linked : Pin->LinkedTo)
			{
				if (UEdGraphNode* LN = Linked->GetOwningNode())
					CollectSubGraphNodes(LN, Out, Visited);
			}
		}
	}
}

// -- CustomHlsl reflection helpers (MinimalAPI class, methods not exported) --------
// The CustomHlsl UPROPERTY is private; access via UE reflection.
FString GetCustomHlslText(const UNiagaraNodeCustomHlsl* Node)
{
	if (!Node) return FString();
	const FStrProperty* Prop = FindFProperty<FStrProperty>(UNiagaraNodeCustomHlsl::StaticClass(), TEXT("CustomHlsl"));
	if (!Prop) return FString();
	return Prop->GetPropertyValue_InContainer(Node);
}

void SetCustomHlslText(UNiagaraNodeCustomHlsl* Node, const FString& InHlsl)
{
	if (!Node) return;
	FStrProperty* Prop = FindFProperty<FStrProperty>(UNiagaraNodeCustomHlsl::StaticClass(), TEXT("CustomHlsl"));
	if (!Prop) return;
	Node->Modify();
	Prop->SetPropertyValue_InContainer(Node, InHlsl);
	// Mirror what SetCustomHlsl() does after setting the value:
	Node->RefreshFromExternalChanges(); // virtual — dispatches through vtable (MinimalAPI exports vtable)
	if (Node->GetOuter() && Node->GetOuter()->IsA<UNiagaraGraph>())
	{
		Node->MarkNodeRequiresSynchronization(TEXT("SetCustomHLSLInput"), true); // NIAGARAEDITOR_API
	}
}

// Replicate InitAsCustomHlslDynamicInput() without calling unexported methods.
// Sets up Signature with a ParameterMap input + typed output, sets ScriptUsage, then ReallocatePins().
void InitCustomHlslAsDynamicInput(UNiagaraNodeCustomHlsl* Node, const FNiagaraTypeDefinition& OutputType)
{
	if (!Node) return;
	Node->Modify();

	// Clear and rebuild the function signature to match what InitAsCustomHlslDynamicInput does:
	// Input 0: ParameterMap ("Map"), Output 0: OutputType ("CustomHLSLOutput")
	Node->Signature.Inputs.Reset();
	Node->Signature.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("Map")));
	Node->Signature.Outputs.Reset();
	Node->Signature.Outputs.Add(FNiagaraVariableBase(OutputType, TEXT("CustomHLSLOutput")));

	// ScriptUsage is a public UPROPERTY
	Node->ScriptUsage = ENiagaraScriptUsage::DynamicInput;

	// ReallocatePins() is protected — use public UEdGraphNode::ReconstructNode() instead
	Node->ReconstructNode();
}

// -- Graph parameter helpers (AddParameter/HasVariable not exported) ---------------
bool GraphHasVariable(UNiagaraGraph* Graph, const FNiagaraVariable& Var)
{
	// GetScriptVariable(FName) is NIAGARAEDITOR_API
	return Graph && Graph->GetScriptVariable(Var.GetName()) != nullptr;
}

void GraphAddParameter(UNiagaraGraph* Graph, const FNiagaraVariable& Var)
{
	if (!Graph) return;
	// Create a UNiagaraScriptVariable and insert into the exported mutable metadata map
	UNiagaraScriptVariable* ScriptVar = NewObject<UNiagaraScriptVariable>(Graph, NAME_None, RF_Transactional);
	FNiagaraVariableMetaData MetaData;
	MetaData.SetVariableGuid(FGuid::NewGuid());
	ScriptVar->Init(Var, MetaData); // NIAGARAEDITOR_API
	Graph->GetAllMetaData().Add(Var, ScriptVar); // GetAllMetaData() mutable ref is NIAGARAEDITOR_API
	Graph->NotifyGraphChanged(); // virtual — dispatches through vtable, triggers recompile
}

UNiagaraNodeOutput* FindOutputNode(UNiagaraSystem* System, int32 EmitterIdx, ENiagaraScriptUsage Usage, const FGuid& UsageId = FGuid(), bool bCreateIfMissing = true)
{
	UNiagaraScript* Script = nullptr;
	if (EmitterIdx == INDEX_NONE)
	{
		if (Usage == ENiagaraScriptUsage::SystemSpawnScript)
			Script = System->GetSystemSpawnScript();
		else if (Usage == ENiagaraScriptUsage::SystemUpdateScript)
			Script = System->GetSystemUpdateScript();
	}
	else
	{
		FVersionedNiagaraEmitterData* ED = System->GetEmitterHandle(EmitterIdx).GetEmitterData();
		if (ED)
		{
			TArray<UNiagaraScript*> Scripts;
			ED->GetScripts(Scripts, false);
			for (UNiagaraScript* S : Scripts)
			{
				if (S && S->GetUsage() == Usage)
				{
					if (!UsageId.IsValid() || S->GetUsageId() == UsageId)
					{
						Script = S;
						break;
					}
				}
			}
		}
	}
	// Try the script's own source first, then fall back to the emitter's shared GraphSource.
	// Emitter-embedded scripts (ParticleSpawn, ParticleUpdate, EmitterSpawn, EmitterUpdate)
	// share one graph source stored at EmitterData->GraphSource, not on the individual script.
	// For versioned template emitters, the ParticleSpawn script may not exist as a separate object
	// but the shared GraphSource still has the graph where we can create an output node.
	UNiagaraScriptSource* Src = Script ? Cast<UNiagaraScriptSource>(Script->GetLatestSource()) : nullptr;
	if (!Src && EmitterIdx != INDEX_NONE)
	{
		FVersionedNiagaraEmitterData* ED = System->GetEmitterHandle(EmitterIdx).GetEmitterData();
		if (ED) Src = Cast<UNiagaraScriptSource>(ED->GraphSource);
	}
	if (!Src) return nullptr;
	UNiagaraGraph* Graph = Src->NodeGraph;
	if (!Graph) return nullptr;

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		UNiagaraNodeOutput* Out = Cast<UNiagaraNodeOutput>(Node);
		if (Out && Out->GetUsage() == Usage)
			return Out;
	}

	if (!bCreateIfMissing) return nullptr;

	// Auto-create missing output node (some templates ship without ParticleSpawnScript output nodes)
	// ResetGraphForOutput is not exported from NiagaraEditor, so we replicate it inline
	Graph->Modify();

	FGraphNodeCreator<UNiagaraNodeOutput> OutputNodeCreator(*Graph);
	UNiagaraNodeOutput* NewOutput = OutputNodeCreator.CreateNode();
	NewOutput->SetUsage(Usage);
	NewOutput->SetUsageId(Script ? Script->GetUsageId() : UsageId);
	NewOutput->Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("Out")));
	OutputNodeCreator.Finalize();

	FGraphNodeCreator<UNiagaraNodeInput> InputNodeCreator(*Graph);
	UNiagaraNodeInput* InputNode = InputNodeCreator.CreateNode();
	InputNode->Input = FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("InputMap"));
	InputNode->Usage = ENiagaraInputNodeUsage::Parameter;
	InputNodeCreator.Finalize();

	// Connect InputNode output → NewOutput input (parameter map pins)
	UEdGraphPin* OutInputPin = nullptr;
	for (UEdGraphPin* Pin : NewOutput->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Input) { OutInputPin = Pin; break; }
	}
	UEdGraphPin* InOutputPin = nullptr;
	for (UEdGraphPin* Pin : InputNode->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output) { InOutputPin = Pin; break; }
	}
	if (OutInputPin && InOutputPin)
	{
		OutInputPin->MakeLinkTo(InOutputPin);
	}

	Graph->NotifyGraphChanged();
	UE_LOG(LogTemp, Log, TEXT("AIK: Auto-created missing output node for usage %d in Niagara graph"), static_cast<int32>(Usage));
	return NewOutput;
}

UNiagaraNodeFunctionCall* FindModuleByName(UNiagaraNodeOutput* OutputNode, const FString& ModuleName)
{
	if (!OutputNode || ModuleName.IsEmpty()) return nullptr;
	UEdGraphPin* InputPin = GetParamMapInputPin(OutputNode);
	if (!InputPin) return nullptr;

	TSet<UEdGraphNode*> Visited;
	TQueue<UEdGraphNode*> Queue;
	for (UEdGraphPin* Link : InputPin->LinkedTo)
	{
		if (Link && Link->GetOwningNode()) Queue.Enqueue(Link->GetOwningNode());
	}

	while (!Queue.IsEmpty())
	{
		UEdGraphNode* Node = nullptr;
		Queue.Dequeue(Node);
		if (!Node || Visited.Contains(Node)) continue;
		Visited.Add(Node);

		if (UNiagaraNodeFunctionCall* FC = Cast<UNiagaraNodeFunctionCall>(Node))
		{
			FString NodeName = FC->GetFunctionName();
			if (NodeName.Equals(ModuleName, ESearchCase::IgnoreCase) ||
				NodeName.Contains(ModuleName))
				return FC;
		}

		UEdGraphPin* PrevPin = GetParamMapInputPin(Node);
		if (PrevPin)
		{
			for (UEdGraphPin* Link : PrevPin->LinkedTo)
			{
				if (Link && Link->GetOwningNode())
					Queue.Enqueue(Link->GetOwningNode());
			}
		}
	}
	return nullptr;
}

TArray<FString> ListModulesInStack(UNiagaraNodeOutput* OutputNode)
{
	TArray<FString> Result;
	if (!OutputNode) return Result;
	UEdGraphPin* InputPin = GetParamMapInputPin(OutputNode);
	if (!InputPin) return Result;

	UEdGraphNode* Current = nullptr;
	if (InputPin->LinkedTo.Num() > 0 && InputPin->LinkedTo[0])
		Current = InputPin->LinkedTo[0]->GetOwningNode();

	TSet<UEdGraphNode*> Visited;
	while (Current && !Visited.Contains(Current))
	{
		Visited.Add(Current);
		if (UNiagaraNodeFunctionCall* FC = Cast<UNiagaraNodeFunctionCall>(Current))
		{
			Result.Insert(FC->GetFunctionName(), 0);
		}
		UEdGraphPin* PrevPin = GetParamMapInputPin(Current);
		Current = nullptr;
		if (PrevPin && PrevPin->LinkedTo.Num() > 0 && PrevPin->LinkedTo[0])
			Current = PrevPin->LinkedTo[0]->GetOwningNode();
	}
	return Result;
}

static const TArray<FString> RendererPrefixes = { TEXT("Niagara") };
static const TArray<FString> RendererSuffixes = { TEXT("RendererProperties") };

UNiagaraRendererProperties* CreateRendererByType(UNiagaraEmitter* Emitter, const FString& Type)
{
	if (!Emitter) return nullptr;
	UClass* Cls = LuaDynamicType::FindDerivedClass(UNiagaraRendererProperties::StaticClass(), Type, RendererPrefixes, RendererSuffixes);
	if (!Cls) return nullptr;
	return NewObject<UNiagaraRendererProperties>(Emitter, Cls);
}

FString GetRendererTypeName(const UNiagaraRendererProperties* R)
{
	if (!R) return TEXT("Unknown");
	return LuaDynamicType::GetFriendlyTypeName(R, RendererPrefixes, RendererSuffixes);
}

bool SetReflectedProperty(UObject* Obj, const FString& PropName, double NumVal, const FString& StrVal, bool BoolVal, bool bIsString, bool bIsBool, FString& OutError)
{
	if (!Obj) { OutError = TEXT("null object"); return false; }
	FProperty* Prop = Obj->GetClass()->FindPropertyByName(FName(*PropName));
	if (!Prop) { OutError = FString::Printf(TEXT("property '%s' not found"), *PropName); return false; }

	void* Container = Prop->ContainerPtrToValuePtr<void>(Obj);
	if (FFloatProperty* FP = CastField<FFloatProperty>(Prop)) { FP->SetPropertyValue(Container, static_cast<float>(NumVal)); return true; }
	if (FDoubleProperty* DP = CastField<FDoubleProperty>(Prop)) { DP->SetPropertyValue(Container, NumVal); return true; }
	if (FIntProperty* IP = CastField<FIntProperty>(Prop)) { IP->SetPropertyValue(Container, static_cast<int32>(NumVal)); return true; }
	if (FBoolProperty* BP = CastField<FBoolProperty>(Prop)) { BP->SetPropertyValue(Container, bIsBool ? BoolVal : (NumVal != 0.0)); return true; }
	if (FStrProperty* SP = CastField<FStrProperty>(Prop)) { SP->SetPropertyValue(Container, StrVal); return true; }
	if (FNameProperty* NP = CastField<FNameProperty>(Prop)) { NP->SetPropertyValue(Container, FName(*StrVal)); return true; }
	if (FByteProperty* ByteP = CastField<FByteProperty>(Prop)) { ByteP->SetPropertyValue(Container, static_cast<uint8>(NumVal)); return true; }
	if (FEnumProperty* EP = CastField<FEnumProperty>(Prop))
	{
		FNumericProperty* Under = EP->GetUnderlyingProperty();
		if (Under) Under->SetIntPropertyValue(Container, static_cast<int64>(NumVal));
		return true;
	}

	// Object property (UMaterial, UTexture, etc.) — load by path string
	if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
	{
		if (bIsString && !StrVal.IsEmpty())
		{
			UObject* Loaded = LoadObject<UObject>(nullptr, *StrVal);
			if (!Loaded)
			{
				// Try with class prefix: Type'/Path'
				FString WithPrefix = FString::Printf(TEXT("%s'%s'"), *ObjProp->PropertyClass->GetName(), *StrVal);
				Loaded = LoadObject<UObject>(nullptr, *WithPrefix);
			}
			if (Loaded && Loaded->IsA(ObjProp->PropertyClass))
			{
				ObjProp->SetObjectPropertyValue(Container, Loaded);
				return true;
			}
			OutError = FString::Printf(TEXT("could not load '%s' as %s"), *StrVal, *ObjProp->PropertyClass->GetName());
			return false;
		}
		OutError = FString::Printf(TEXT("object property '%s' requires a string path value"), *PropName);
		return false;
	}

	// Fallback: try ImportText for struct/array/other complex types
	if (bIsString && !StrVal.IsEmpty())
	{
		const TCHAR* Result = Prop->ImportText_Direct(*StrVal, Container, Obj, PPF_None);
		if (Result)
		{
			return true;
		}
		OutError = FString::Printf(TEXT("ImportText failed for '%s' (type %s) with value '%s'"), *PropName, *Prop->GetCPPType(), *StrVal);
		return false;
	}

	OutError = FString::Printf(TEXT("unsupported property type '%s' for '%s' — pass as string for struct/object types"), *Prop->GetCPPType(), *PropName);
	return false;
}

TArray<UNiagaraScript*> FindAffectedScripts(UNiagaraSystem* System, const FNiagaraEmitterHandle& EmitterHandle, ENiagaraScriptUsage Usage, const FGuid& UsageId = FGuid())
{
	TArray<UNiagaraScript*> Result;
	if (UNiagaraScript* S = System->GetSystemSpawnScript()) Result.AddUnique(S);
	if (UNiagaraScript* S = System->GetSystemUpdateScript()) Result.AddUnique(S);

	FVersionedNiagaraEmitterData* ED = EmitterHandle.GetEmitterData();
	if (ED)
	{
		TArray<UNiagaraScript*> EmitterScripts;
		ED->GetScripts(EmitterScripts, false);
		for (UNiagaraScript* S : EmitterScripts)
		{
			if (!S || !S->ContainsUsage(Usage)) continue;
			// For event handlers and simulation stages, also match by UsageId to target the specific script
			if (UsageId.IsValid() &&
				(Usage == ENiagaraScriptUsage::ParticleEventScript || Usage == ENiagaraScriptUsage::ParticleSimulationStageScript) &&
				S->GetUsageId() != UsageId)
			{
				continue;
			}
			Result.AddUnique(S);
		}
	}
	return Result;
}

void CleanupOverridePinNodes(UEdGraphPin& OverridePin)
{
	if (OverridePin.LinkedTo.Num() == 0) return;
	UEdGraphPin* LinkedPin = OverridePin.LinkedTo[0];
	if (!LinkedPin) return;
	UEdGraphNode* LinkedNode = LinkedPin->GetOwningNode();
	if (!LinkedNode) { OverridePin.BreakAllPinLinks(true); return; }
	UEdGraph* Graph = LinkedNode->GetGraph();
	if (!Graph) { OverridePin.BreakAllPinLinks(true); return; }

	// Engine parity: use IsA<> for types with public headers, string match only for private types
	if (LinkedNode->IsA<UNiagaraNodeInput>() || LinkedNode->GetClass()->GetName() == TEXT("NiagaraNodeParameterMapGet"))
	{
		OverridePin.BreakAllPinLinks(true);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
		LinkedNode->BreakAllNodeLinks(true);
#else
		LinkedNode->BreakAllNodeLinks();
#endif
		Graph->RemoveNode(LinkedNode);
	}
	else if (LinkedNode->IsA<UNiagaraNodeFunctionCall>() || LinkedNode->IsA<UNiagaraNodeCustomHlsl>())
	{
		OverridePin.BreakAllPinLinks(true);
		UEdGraphPin* DIInputMap = GetParamMapInputPin(LinkedNode);
		UEdGraphPin* DIOutputMap = GetParamMapOutputPin(LinkedNode);
		if (DIInputMap) DIInputMap->BreakAllPinLinks(true);
		if (DIOutputMap) DIOutputMap->BreakAllPinLinks(true);

		TArray<UEdGraphNode*> NodesToRemove;
		TSet<UEdGraphNode*> Visited;
		CollectSubGraphNodes(LinkedNode, NodesToRemove, Visited);
		for (UEdGraphNode* N : NodesToRemove)
		{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
			if (N) { N->BreakAllNodeLinks(true); Graph->RemoveNode(N); }
#else
			if (N) { N->BreakAllNodeLinks(); Graph->RemoveNode(N); }
#endif
		}
	}
	else
	{
		OverridePin.BreakAllPinLinks(true);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
		LinkedNode->BreakAllNodeLinks(true);
#else
		LinkedNode->BreakAllNodeLinks();
#endif
		Graph->RemoveNode(LinkedNode);
	}
}

// Engine's SetDataInterfaceValueForFunctionInput / SetDynamicInputForFunctionInput / SetLinkedParameterValueForFunctionInput
// all do CastChecked<UNiagaraNodeParameterMapSet>(OverridePin.GetOwningNode()).
// Static switch pins live on UNiagaraNodeFunctionCall instead — passing those crashes.
// The engine's stack UI prevents this by never offering DI/dynamic/linked modes for static switches.
// We replicate that check here. Private header → class name check.
bool IsOverridePinOnParameterMapSet(UEdGraphPin& Pin)
{
	UEdGraphNode* Owner = Pin.GetOwningNode();
	return Owner && Owner->GetClass()->GetName().Contains(TEXT("ParameterMapSet"));
}

void RemoveRapidIterationParam(UNiagaraSystem* System, const FNiagaraEmitterHandle& EmitterHandle, UNiagaraScript* Script, const FNiagaraVariable& AliasedVar)
{
	if (!System || !Script) return;

	const TCHAR* EmitterNamePtr = nullptr;
	FString UniqueEmitterName;
	if (EmitterHandle.IsValid() && EmitterHandle.GetInstance().Emitter)
	{
		UniqueEmitterName = EmitterHandle.GetInstance().Emitter->GetUniqueEmitterName();
		if (!UniqueEmitterName.IsEmpty())
			EmitterNamePtr = *UniqueEmitterName;
	}

	FNiagaraVariable RIParam = FNiagaraUtilities::ConvertVariableToRapidIterationConstantName(
		AliasedVar,
		EmitterNamePtr,
		Script->GetUsage());

	TArray<UNiagaraScript*> Affected = FindAffectedScripts(System, EmitterHandle, Script->GetUsage());
	for (UNiagaraScript* S : Affected)
	{
		if (S) { S->Modify(); S->RapidIterationParameters.RemoveParameter(RIParam); }
	}
}

TArray<FNiagaraVariable> DiscoverModuleInputs(UNiagaraNodeFunctionCall* ModuleNode, UNiagaraScript* RIScript = nullptr,
	const FNiagaraEmitterHandle* EmitterHandle = nullptr, ENiagaraScriptUsage Usage = ENiagaraScriptUsage::ParticleUpdateScript)
{
	TArray<FNiagaraVariable> Result;
	if (!ModuleNode) return Result;

	// Use the engine's GetStackFunctionInputs API which properly traverses the module's internal
	// graph and resolves static switches to discover ALL inputs including gated ones
	// (e.g. Lifetime, Color, Sprite Size behind mode selectors on V2 InitializeParticle)
	if (EmitterHandle && EmitterHandle->IsValid())
	{
		FVersionedNiagaraEmitter VersionedEmitter = EmitterHandle->GetInstance();
		FCompileConstantResolver Resolver(VersionedEmitter, Usage);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
		// 5.6+: Use the overload with OutHiddenVariables so we also discover static-switch-gated inputs
		TSet<FNiagaraVariable> HiddenVars;
		FNiagaraStackGraphUtilities::GetStackFunctionInputs(*ModuleNode, Result, HiddenVars, Resolver,
			FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly);
		for (const FNiagaraVariable& HV : HiddenVars) Result.AddUnique(HV);
#else
		// 5.4/5.5: GetStackFunctionInputs with resolver not available — skip, rely on pin+RI fallback below
		(void)Resolver;
#endif
	}

	// Also discover via pins — catches data interface pins and other inputs
	// that GetStackFunctionInputs might not return
	for (UEdGraphPin* Pin : ModuleNode->GetAllPins())
	{
		if (Pin->Direction == EGPD_Input && !IsParameterMapPin(Pin))
		{
			FNiagaraTypeDefinition PinType = UEdGraphSchema_Niagara::PinToTypeDefinition(Pin);
			Result.AddUnique(FNiagaraVariable(PinType, Pin->GetFName()));
		}
	}

	if (UNiagaraScript* FuncScript = ModuleNode->FunctionScript)
	{
		for (const FNiagaraVariable& ScriptVar : FuncScript->GetVMExecutableData().Parameters.Parameters)
		{
			FString VarName = ScriptVar.GetName().ToString();
			if (!VarName.StartsWith(TEXT("Module."))) continue;

			FString ShortName = VarName.RightChop(7);
			bool bAlreadyFound = false;
			for (const FNiagaraVariable& Existing : Result)
			{
				if (Existing.GetName().ToString().Equals(VarName, ESearchCase::IgnoreCase) ||
					Existing.GetName().ToString().EndsWith(ShortName))
				{
					bAlreadyFound = true; break;
				}
			}
			if (!bAlreadyFound)
				Result.Add(ScriptVar);
		}
	}

	if (RIScript)
	{
		FString ModFuncName = ModuleNode->GetFunctionName();
		for (const FNiagaraVariableWithOffset& RIVar : RIScript->RapidIterationParameters.ReadParameterVariables())
		{
			FString RIName = RIVar.GetName().ToString();
			if (!RIName.Contains(ModFuncName)) continue;

			FString ShortName = RIName;
			int32 LastDot = INDEX_NONE;
			if (ShortName.FindLastChar(TEXT('.'), LastDot))
				ShortName = ShortName.RightChop(LastDot + 1);

			bool bAlreadyFound = false;
			for (const FNiagaraVariable& Existing : Result)
			{
				FString ExName = Existing.GetName().ToString();
				if (ExName.StartsWith(TEXT("Module.")))
					ExName = ExName.RightChop(7);
				if (ExName.Equals(ShortName, ESearchCase::IgnoreCase)) { bAlreadyFound = true; break; }
			}
			if (bAlreadyFound) continue;

			FNiagaraVariable ModVar(RIVar.GetType(), FName(*FString::Printf(TEXT("Module.%s"), *ShortName)));
			Result.Add(ModVar);
		}
	}

	return Result;
}

UNiagaraScript* FindScratchPadScript(UNiagaraSystem* System, const FString& Name)
{
	if (!System) return nullptr;
	for (UNiagaraScript* S : System->ScratchPadScripts)
	{
		if (S && S->GetName().Equals(Name, ESearchCase::IgnoreCase))
			return S;
	}
	return nullptr;
}

// Warn about any keys in a sol::table that weren't consumed.
// ConsumedKeys: keys we actually read; Context: e.g. "list(\"module_inputs\")"
void WarnUnconsumedKeys(FLuaSessionData& Session, const sol::table& T, const TSet<FString>& ConsumedKeys, const FString& Context)
{
	for (auto& Pair : T)
	{
		if (!Pair.first.is<std::string>()) continue;
		FString Key = UTF8_TO_TCHAR(Pair.first.as<std::string>().c_str());
		if (!ConsumedKeys.Contains(Key))
		{
			Session.Log(FString::Printf(TEXT("[WARN] %s -> key '%s' was not consumed. Keys used: %s"),
				*Context, *Key, ConsumedKeys.Num() > 0 ? *FString::Join(ConsumedKeys.Array(), TEXT(", ")) : TEXT("(none)")));
		}
	}
}

} // namespace

// ============================================================================
// Lua Binding
// ============================================================================

static TArray<FLuaFunctionDoc> NiagaraDocs = {};

static void BindNiagara(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_niagara", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *FPath);
		if (!System) return;

		AssetObj["_help_text"] =
			"Element types for add/remove/list/configure/get:\n"
			"  module, emitter, stateless_emitter, renderer, user_parameter, event_handler, simulation_stage\n"
			"\n"
			"add/remove/list/configure/get(type, id, params)\n"
			"\n"
			"Renderer types: dynamically discovered (use list(\"renderer_types\") to see available)\n"
			"\n"
			"configure(\"module\", \"ModName\", {emitter, stage, parameters={\n"
			"  Speed=1.0, Vel={mode=\"dynamic_input\", script=\"...\"},\n"
			"  Pos={mode=\"linked\", parameter=\"Particles.Position\"},\n"
			"  X={mode=\"hlsl\", code=\"...\"}, M={mode=\"data_interface\", type=\"...\"},\n"
			"  A={mode=\"curve\", keys={{time=0, value=1}, {time=1, value=0}}},\n"
			"  C={mode=\"color_curve\", keys={{time=0, color={r=1,g=0,b=0,a=1}}}},\n"
			"  V={mode=\"vector_curve\", keys={{time=0, x=1, y=0, z=0}}},\n"
			"  R={mode=\"reset\"}}})\n"
			"configure(\"emitter\", name, {sim_target, local_space, determinism, random_seed,\n"
			"  interpolated_spawn_mode, requires_persistent_ids, enabled,\n"
			"  bounds_mode, fixed_bounds, allocation_mode, pre_allocation_count,\n"
			"  max_gpu_particles_spawn_per_frame})\n"
			"configure(\"system\", _, {warmup_time, determinism, random_seed,\n"
			"  bake_out_rapid_iteration, trim_attributes, compress_attributes,\n"
			"  properties={bFixedBounds, bFixedTickDelta, FixedTickDeltaTime, ...}})\n"
			"\n"
			"get(\"system\", _, \"property\") / get(\"emitter\", name) / get(\"emitter\", name, \"prop\")\n"
			"get(\"renderer\", {emitter, index}, \"prop\") / get(\"user_parameter\", name)\n"
			"get(\"module\", {emitter, stage, module_name}, \"input_name\")\n"
			"\n"
			"set_user_parameter({name, value}) — value: number, bool, or table {x,y,z}/{r,g,b,a}\n"
			"\n"
			"list: emitters, modules, renderers, user_parameters, event_handlers,\n"
			"  simulation_stages, module_inputs, reflected_properties, dynamic_inputs,\n"
			"  scratch_pad_scripts, available_modules, emitter_templates, stateless_modules\n"
			"\n"
			"Action methods:\n"
			"  enable_module / move_module / rename_emitter / duplicate_emitter\n"
			"  reorder_renderers / reorder_simulation_stages / set_user_parameter\n"
			"  configure_event_handler / configure_simulation_stage\n"
			"  subscribe/unsubscribe/synchronize_parameter_definitions\n"
			"  list_parameter_definitions / compile / run_validation\n"
			"  create/delete/rename_scratch_pad_script\n"
			"  validate() / info()\n"
			"\n"
			"configure(\"emitter_mode\", name, {mode=\"stateless\"|\"standard\"})\n"
			"version(\"list\"|\"add\"|\"expose\"|\"delete\", {emitter, major?, minor?, version_guid?})\n";

		// ==================================================================
		// add(type, params)
		// ==================================================================
		AssetObj.set_function("add", [System, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (!Params.has_value()) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> params required"), *FType)); return sol::lua_nil; }
			sol::table P = Params.value();

			// ---- add("module") ----
			// Engine parity: uses AddScriptModuleToStack which handles node creation,
			// script versioning, pin creation, and ConnectStackNodeGroup internally
			if (FType.Equals(TEXT("module"), ESearchCase::IgnoreCase))
			{
				std::string EmitterName = P.get_or<std::string>("emitter", "");
				std::string StageStr = P.get_or<std::string>("stage", "particle_update");
				std::string ModulePath = P.get_or<std::string>("module_path", "");
				if (ModulePath.empty()) ModulePath = P.get_or<std::string>("script", "");
				if (ModulePath.empty()) ModulePath = P.get_or<std::string>("path", "");
				int32 TargetIndex = static_cast<int32>(P.get_or("index", -1));
				if (ModulePath.empty()) { Session.Log(TEXT("[FAIL] add(\"module\") -> module_path (or script/path) required")); return sol::lua_nil; }

				FString FEmitter = UTF8_TO_TCHAR(EmitterName.c_str());
				FString FStage = UTF8_TO_TCHAR(StageStr.c_str());
				ENiagaraScriptUsage Usage = ParseUsage(FStage);

				int32 EmIdx = INDEX_NONE;
				bool bSystemLevel = (Usage == ENiagaraScriptUsage::SystemSpawnScript || Usage == ENiagaraScriptUsage::SystemUpdateScript);
				if (!bSystemLevel)
				{
					if (FEmitter.IsEmpty()) { Session.Log(TEXT("[FAIL] add(\"module\") -> emitter required for non-system stages")); return sol::lua_nil; }
					EmIdx = FindEmitterIndex(System, FEmitter);
					if (EmIdx == INDEX_NONE) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"module\") -> emitter '%s' not found"), *FEmitter)); return sol::lua_nil; }
				}

				UNiagaraNodeOutput* OutputNode = FindOutputNode(System, EmIdx, Usage);
				if (!OutputNode) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"module\") -> no output node for stage '%s'"), *FStage)); return sol::lua_nil; }

				FString FModPath = UTF8_TO_TCHAR(ModulePath.c_str());
				UNiagaraScript* ModScript = LoadObject<UNiagaraScript>(nullptr, *FModPath);
				if (!ModScript) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"module\") -> script '%s' not found"), *FModPath)); return sol::lua_nil; }

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "AddNModule", "Add Niagara Module"));

				// Engine API: handles node creation, versioning, pin fallback, and stack node group wiring
				UNiagaraNodeFunctionCall* NewNode = FNiagaraStackGraphUtilities::AddScriptModuleToStack(
					ModScript, *OutputNode, TargetIndex);

				if (!NewNode)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"module\") -> AddScriptModuleToStack failed for '%s'"), *FModPath));
					return sol::lua_nil;
				}

				UNiagaraGraph* Graph = Cast<UNiagaraGraph>(OutputNode->GetGraph());
				if (Graph) Graph->NotifyGraphChanged();
				System->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add(\"module\", path=\"%s\", stage=\"%s\")"), *FModPath, *FStage));
				return sol::make_object(Lua, true);
			}
			// ---- add("emitter") ----
			else if (FType.Equals(TEXT("emitter"), ESearchCase::IgnoreCase))
			{
				std::string Name = P.get_or<std::string>("name", "");
				if (Name.empty()) { Session.Log(TEXT("[FAIL] add(\"emitter\") -> name required")); return sol::lua_nil; }
				std::string TemplateStr = P.get_or<std::string>("template_asset", "");

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "AddNEmitter", "Add Niagara Emitter"));
				System->Modify();

				FString FName = UTF8_TO_TCHAR(Name.c_str());
				if (!TemplateStr.empty())
				{
					FString FTemplate = UTF8_TO_TCHAR(TemplateStr.c_str());
					UNiagaraEmitter* TemplateEmitter = LoadObject<UNiagaraEmitter>(nullptr, *FTemplate);
					if (!TemplateEmitter) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"emitter\") -> template '%s' not found"), *FTemplate)); return sol::lua_nil; }
					FGuid NewGuid = FNiagaraEditorUtilities::AddEmitterToSystem(*System, *TemplateEmitter, TemplateEmitter->GetExposedVersion().VersionGuid, true);
					if (NewGuid.IsValid())
					{
						// Apply the user-specified name to the newly added emitter
						for (int32 i = 0; i < System->GetNumEmitters(); ++i)
						{
							if (System->GetEmitterHandle(i).GetId() == NewGuid)
							{
								System->GetEmitterHandle(i).SetName(::FName(*FName), *System);
								break;
							}
						}
						System->MarkPackageDirty();
						Session.Log(FString::Printf(TEXT("[OK] add(\"emitter\", name=\"%s\", template=\"%s\")"), *FName, *FTemplate));
						return sol::make_object(Lua, true);
					}
				}
				else
				{
					Session.Log(TEXT("[FAIL] add(\"emitter\") -> template_asset required (cannot create empty emitter)"));
					return sol::lua_nil;
				}
				Session.Log(FString::Printf(TEXT("[FAIL] add(\"emitter\") -> failed to add '%s'"), *FName));
				return sol::lua_nil;
			}
			// ---- add("renderer") ----
			else if (FType.Equals(TEXT("renderer"), ESearchCase::IgnoreCase))
			{
				std::string EmitterName = P.get_or<std::string>("emitter", "");
				std::string RType = P.get_or<std::string>("type", "sprite");
				if (EmitterName.empty()) { Session.Log(TEXT("[FAIL] add(\"renderer\") -> emitter required")); return sol::lua_nil; }

				FString FEmitter = UTF8_TO_TCHAR(EmitterName.c_str());
				int32 EmIdx = FindEmitterIndex(System, FEmitter);
				if (EmIdx == INDEX_NONE) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"renderer\") -> emitter '%s' not found"), *FEmitter)); return sol::lua_nil; }

				FNiagaraEmitterHandle& Handle = System->GetEmitterHandle(EmIdx);
				FVersionedNiagaraEmitterData* ED = Handle.GetEmitterData();
				if (!ED) { Session.Log(TEXT("[FAIL] add(\"renderer\") -> no emitter data")); return sol::lua_nil; }

				FVersionedNiagaraEmitter Instance = Handle.GetInstance();
				UNiagaraEmitter* Emitter = Instance.Emitter;
				if (!Emitter) { Session.Log(TEXT("[FAIL] add(\"renderer\") -> no emitter object")); return sol::lua_nil; }

				FString FRType = UTF8_TO_TCHAR(RType.c_str());
				UNiagaraRendererProperties* Renderer = CreateRendererByType(Emitter, FRType);
				if (!Renderer) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"renderer\") -> unknown type '%s'. Valid: %s"), *FRType, *LuaDynamicType::FormatAvailableTypes(UNiagaraRendererProperties::StaticClass(), RendererPrefixes, RendererSuffixes))); return sol::lua_nil; }

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "AddNRenderer", "Add Niagara Renderer"));
				System->Modify();
				Emitter->AddRenderer(Renderer, Instance.Version);

				// Apply properties via reflection
				sol::optional<sol::table> PropsOpt = P.get<sol::optional<sol::table>>("properties");
				if (PropsOpt.has_value())
				{
					Renderer->Modify();
					for (auto& kv : PropsOpt.value())
					{
						if (!kv.first.is<std::string>()) continue;
						FString PropName = UTF8_TO_TCHAR(kv.first.as<std::string>().c_str());
						FString Err;
						double NumVal = kv.second.is<double>() ? kv.second.as<double>() : 0.0;
						FString StrVal = kv.second.is<std::string>() ? UTF8_TO_TCHAR(kv.second.as<std::string>().c_str()) : TEXT("");
						bool BoolVal = kv.second.is<bool>() ? kv.second.as<bool>() : false;
						if (!SetReflectedProperty(Renderer, PropName, NumVal, StrVal, BoolVal, kv.second.is<std::string>(), kv.second.is<bool>(), Err))
							Session.Log(FString::Printf(TEXT("[WARN] SetReflectedProperty('%s'): %s"), *PropName, *Err));
					}
					Renderer->PostEditChange();
				}

				System->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add(\"renderer\", emitter=\"%s\", type=\"%s\")"), *FEmitter, *FRType));
				return sol::make_object(Lua, true);
			}
			// ---- add("user_parameter") ----
			else if (FType.Equals(TEXT("user_parameter"), ESearchCase::IgnoreCase))
			{
				std::string ParamName = P.get_or<std::string>("name", "");
				std::string ParamType = P.get_or<std::string>("type", "Float");
				if (ParamName.empty()) { Session.Log(TEXT("[FAIL] add(\"user_parameter\") -> name required")); return sol::lua_nil; }

				FString FParamName = UTF8_TO_TCHAR(ParamName.c_str());
				FString FParamType = UTF8_TO_TCHAR(ParamType.c_str());

				// Resolve type
				FNiagaraTypeDefinition TypeDef;
				if (FParamType.Equals(TEXT("Float"), ESearchCase::IgnoreCase)) TypeDef = FNiagaraTypeDefinition::GetFloatDef();
				else if (FParamType.Equals(TEXT("Int32"), ESearchCase::IgnoreCase) || FParamType.Equals(TEXT("Int"), ESearchCase::IgnoreCase)) TypeDef = FNiagaraTypeDefinition::GetIntDef();
				else if (FParamType.Equals(TEXT("Bool"), ESearchCase::IgnoreCase)) TypeDef = FNiagaraTypeDefinition::GetBoolDef();
				else if (FParamType.Equals(TEXT("Vector2"), ESearchCase::IgnoreCase) || FParamType.Equals(TEXT("Vector2D"), ESearchCase::IgnoreCase)) TypeDef = FNiagaraTypeDefinition::GetVec2Def();
				else if (FParamType.Equals(TEXT("Vector"), ESearchCase::IgnoreCase) || FParamType.Equals(TEXT("Vector3"), ESearchCase::IgnoreCase)) TypeDef = FNiagaraTypeDefinition::GetVec3Def();
				else if (FParamType.Equals(TEXT("Vector4"), ESearchCase::IgnoreCase)) TypeDef = FNiagaraTypeDefinition::GetVec4Def();
				else if (FParamType.Contains(TEXT("Color"), ESearchCase::IgnoreCase)) TypeDef = FNiagaraTypeDefinition::GetColorDef();
				else if (FParamType.Equals(TEXT("Quat"), ESearchCase::IgnoreCase) || FParamType.Contains(TEXT("Quaternion"))) TypeDef = FNiagaraTypeDefinition::GetQuatDef();
				else TypeDef = FNiagaraTypeDefinition::GetFloatDef();

				if (!TypeDef.IsValid()) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"user_parameter\") -> could not resolve type '%s'"), *FParamType)); return sol::lua_nil; }

				FNiagaraVariable NewVar(TypeDef, FName(*(TEXT("User.") + FParamName)));

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "AddNUserParam", "Add Niagara User Parameter"));
				System->Modify();

				FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();
				if (Store.FindParameterOffset(NewVar) != nullptr)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"user_parameter\") -> '%s' already exists"), *FParamName));
					return sol::lua_nil;
				}

				Store.AddParameter(NewVar, true);

				// Set default value if provided (accept both "default" and "default_value" keys)
				sol::object DefObj = P["default_value"];
				if (!DefObj.valid() || DefObj.is<sol::lua_nil_t>()) DefObj = P["default"];
				sol::optional<double> DefaultNum = DefObj.is<double>() ? sol::optional<double>(DefObj.as<double>()) : sol::nullopt;
				sol::optional<sol::table> DefaultTable = DefObj.is<sol::table>() ? sol::optional<sol::table>(DefObj.as<sol::table>()) : sol::nullopt;
				sol::optional<bool> DefaultBool = DefObj.is<bool>() ? sol::optional<bool>(DefObj.as<bool>()) : sol::nullopt;

				if (DefaultNum.has_value() && TypeDef == FNiagaraTypeDefinition::GetFloatDef())
				{
					float Val = static_cast<float>(DefaultNum.value());
					Store.SetParameterValue(Val, NewVar);
				}
				else if (DefaultNum.has_value() && TypeDef == FNiagaraTypeDefinition::GetIntDef())
				{
					int32 Val = static_cast<int32>(DefaultNum.value());
					Store.SetParameterValue(Val, NewVar);
				}
				else if (DefaultBool.has_value() && TypeDef == FNiagaraTypeDefinition::GetBoolDef())
				{
					FNiagaraBool BVal;
					BVal.SetValue(DefaultBool.value());
					Store.SetParameterValue(BVal, NewVar);
				}
				else if (DefaultTable.has_value())
				{
					sol::table DT = DefaultTable.value();
					if (TypeDef == FNiagaraTypeDefinition::GetVec2Def())
					{
						FVector2f V2(static_cast<float>(DT.get_or("x", 0.0)), static_cast<float>(DT.get_or("y", 0.0)));
						Store.SetParameterValue(V2, NewVar);
					}
					else if (TypeDef == FNiagaraTypeDefinition::GetVec3Def())
					{
						FVector3f V3(static_cast<float>(DT.get_or("x", 0.0)), static_cast<float>(DT.get_or("y", 0.0)), static_cast<float>(DT.get_or("z", 0.0)));
						Store.SetParameterValue(V3, NewVar);
					}
					else if (TypeDef == FNiagaraTypeDefinition::GetVec4Def())
					{
						FVector4f V4(static_cast<float>(DT.get_or("x", 0.0)), static_cast<float>(DT.get_or("y", 0.0)), static_cast<float>(DT.get_or("z", 0.0)), static_cast<float>(DT.get_or("w", 0.0)));
						Store.SetParameterValue(V4, NewVar);
					}
					else if (TypeDef == FNiagaraTypeDefinition::GetColorDef())
					{
						FLinearColor Color(static_cast<float>(DT.get_or("r", 0.0)), static_cast<float>(DT.get_or("g", 0.0)), static_cast<float>(DT.get_or("b", 0.0)), static_cast<float>(DT.get_or("a", 1.0)));
						Store.SetParameterValue(Color, NewVar);
					}
					else if (TypeDef == FNiagaraTypeDefinition::GetQuatDef())
					{
						FQuat4f Quat(static_cast<float>(DT.get_or("x", 0.0)), static_cast<float>(DT.get_or("y", 0.0)), static_cast<float>(DT.get_or("z", 0.0)), static_cast<float>(DT.get_or("w", 1.0)));
						Store.SetParameterValue(Quat, NewVar);
					}
				}

				System->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add(\"user_parameter\", name=\"%s\", type=\"%s\")"), *FParamName, *FParamType));
				return sol::make_object(Lua, true);
			}
			// ---- add("event_handler") ----
			else if (FType.Equals(TEXT("event_handler"), ESearchCase::IgnoreCase))
			{
				std::string EmitterName = P.get_or<std::string>("emitter", "");
				std::string SourceEvent = P.get_or<std::string>("source_event_name", "");
				if (EmitterName.empty() || SourceEvent.empty()) { Session.Log(TEXT("[FAIL] add(\"event_handler\") -> emitter and source_event_name required")); return sol::lua_nil; }

				FString FEmitter = UTF8_TO_TCHAR(EmitterName.c_str());
				int32 EmIdx = FindEmitterIndex(System, FEmitter);
				if (EmIdx == INDEX_NONE) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"event_handler\") -> emitter '%s' not found"), *FEmitter)); return sol::lua_nil; }

				FNiagaraEmitterHandle& Handle = System->GetEmitterHandle(EmIdx);
				FVersionedNiagaraEmitterData* ED = Handle.GetEmitterData();
				UNiagaraEmitter* Emitter = Handle.GetInstance().Emitter;
				if (!ED || !Emitter) { Session.Log(TEXT("[FAIL] add(\"event_handler\") -> no emitter data")); return sol::lua_nil; }

				UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(ED->GraphSource);
				if (!Source) { Session.Log(TEXT("[FAIL] add(\"event_handler\") -> no graph source")); return sol::lua_nil; }

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "AddNEventHandler", "Add Niagara Event Handler"));
				System->Modify();
				Emitter->Modify();

				FNiagaraEventScriptProperties EventProps;
				EventProps.SourceEventName = FName(UTF8_TO_TCHAR(SourceEvent.c_str()));
				std::string SourceEmitter = P.get_or<std::string>("source_emitter", "");
				if (!SourceEmitter.empty())
				{
					int32 SrcIdx = FindEmitterIndex(System, UTF8_TO_TCHAR(SourceEmitter.c_str()));
					if (SrcIdx != INDEX_NONE)
						EventProps.SourceEmitterID = System->GetEmitterHandle(SrcIdx).GetId();
				}

				EventProps.Script = NewObject<UNiagaraScript>(Emitter, NAME_None, RF_Transactional);
				EventProps.Script->SetUsage(ENiagaraScriptUsage::ParticleEventScript);
				FGuid EventUsageId = FGuid::NewGuid();
				EventProps.Script->SetUsageId(EventUsageId);
				EventProps.Script->SetLatestSource(Source);

				// Create output node for the event handler so it has a module stack
				UNiagaraGraph* EvtGraph = Source->NodeGraph;
				if (EvtGraph)
				{
					FGraphNodeCreator<UNiagaraNodeOutput> EvtOutputCreator(*EvtGraph);
					UNiagaraNodeOutput* EvtOutput = EvtOutputCreator.CreateNode();
					EvtOutput->SetUsage(ENiagaraScriptUsage::ParticleEventScript);
					EvtOutput->SetUsageId(EventUsageId);
					EvtOutput->Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("Output")));
					EvtOutputCreator.Finalize();

					// Create and connect an input node
					FGraphNodeCreator<UNiagaraNodeInput> EvtInputCreator(*EvtGraph);
					UNiagaraNodeInput* EvtInput = EvtInputCreator.CreateNode();
					EvtInput->Usage = ENiagaraInputNodeUsage::Parameter;
					EvtInput->Input = FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("MapIn"));
					EvtInputCreator.Finalize();

					const UEdGraphSchema_Niagara* EvtSchema = Cast<UEdGraphSchema_Niagara>(EvtGraph->GetSchema());
					if (EvtSchema) EvtSchema->TryCreateConnection(EvtInput->GetOutputPin(0), EvtOutput->GetInputPin(0));
				}

				Emitter->AddEventHandler(EventProps, Handle.GetInstance().Version);
				System->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add(\"event_handler\", emitter=\"%s\", event=\"%s\")"), *FEmitter, UTF8_TO_TCHAR(SourceEvent.c_str())));
				return sol::make_object(Lua, true);
			}
			// ---- add("simulation_stage") ----
			else if (FType.Equals(TEXT("simulation_stage"), ESearchCase::IgnoreCase))
			{
				std::string EmitterName = P.get_or<std::string>("emitter", "");
				if (EmitterName.empty()) { Session.Log(TEXT("[FAIL] add(\"simulation_stage\") -> emitter required")); return sol::lua_nil; }

				FString FEmitter = UTF8_TO_TCHAR(EmitterName.c_str());
				int32 EmIdx = FindEmitterIndex(System, FEmitter);
				if (EmIdx == INDEX_NONE) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"simulation_stage\") -> emitter '%s' not found"), *FEmitter)); return sol::lua_nil; }

				FNiagaraEmitterHandle& Handle = System->GetEmitterHandle(EmIdx);
				FVersionedNiagaraEmitterData* ED = Handle.GetEmitterData();
				UNiagaraEmitter* Emitter = Handle.GetInstance().Emitter;
				if (!ED || !Emitter) { Session.Log(TEXT("[FAIL] add(\"simulation_stage\") -> no emitter data")); return sol::lua_nil; }

				UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(ED->GraphSource);
				if (!Source) { Session.Log(TEXT("[FAIL] add(\"simulation_stage\") -> no graph source")); return sol::lua_nil; }

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "AddNSimStage", "Add Niagara Simulation Stage"));
				System->Modify();
				Emitter->Modify();

				UNiagaraSimulationStageGeneric* NewStage = NewObject<UNiagaraSimulationStageGeneric>(Emitter, UNiagaraSimulationStageGeneric::StaticClass(), NAME_None, RF_Transactional);
				if (NewStage)
				{
					std::string StageName = P.get_or<std::string>("name", "");
					if (!StageName.empty())
					{
						FName DesiredName = FName(UTF8_TO_TCHAR(StageName.c_str()));
						// Check for duplicate simulation stage names
						const TArray<UNiagaraSimulationStageBase*>& ExistingStages = ED->GetSimulationStages();
						for (const UNiagaraSimulationStageBase* ExStage : ExistingStages)
						{
							if (ExStage && ExStage->SimulationStageName == DesiredName)
							{
								Session.Log(FString::Printf(TEXT("[WARN] add(\"simulation_stage\") -> stage name '%s' already exists on emitter '%s'"), *DesiredName.ToString(), *FEmitter));
								break;
							}
						}
						NewStage->SimulationStageName = DesiredName;
					}

					NewStage->Script = NewObject<UNiagaraScript>(NewStage, NAME_None, RF_Transactional);
					NewStage->Script->SetUsage(ENiagaraScriptUsage::ParticleSimulationStageScript);
					FGuid SimStageUsageId = FGuid::NewGuid();
					NewStage->Script->SetUsageId(SimStageUsageId);
					NewStage->Script->SetLatestSource(Source);

					// Create output node for the simulation stage so it has a module stack
					UNiagaraGraph* SimGraph = Source->NodeGraph;
					if (SimGraph)
					{
						FGraphNodeCreator<UNiagaraNodeOutput> SimOutputCreator(*SimGraph);
						UNiagaraNodeOutput* SimOutput = SimOutputCreator.CreateNode();
						SimOutput->SetUsage(ENiagaraScriptUsage::ParticleSimulationStageScript);
						SimOutput->SetUsageId(SimStageUsageId);
						SimOutput->Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("Output")));
						SimOutputCreator.Finalize();

						FGraphNodeCreator<UNiagaraNodeInput> SimInputCreator(*SimGraph);
						UNiagaraNodeInput* SimInput = SimInputCreator.CreateNode();
						SimInput->Usage = ENiagaraInputNodeUsage::Parameter;
						SimInput->Input = FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("MapIn"));
						SimInputCreator.Finalize();

						const UEdGraphSchema_Niagara* SimSchema = Cast<UEdGraphSchema_Niagara>(SimGraph->GetSchema());
						if (SimSchema) SimSchema->TryCreateConnection(SimInput->GetOutputPin(0), SimOutput->GetInputPin(0));
					}

					Emitter->AddSimulationStage(NewStage, Handle.GetInstance().Version);
					System->MarkPackageDirty();
					Session.Log(FString::Printf(TEXT("[OK] add(\"simulation_stage\", emitter=\"%s\")"), *FEmitter));
					return sol::make_object(Lua, true);
				}
				Session.Log(TEXT("[FAIL] add(\"simulation_stage\") -> creation failed"));
				return sol::lua_nil;
			}

			// ---- add("stateless_emitter") ----
			else if (FType.Equals(TEXT("stateless_emitter"), ESearchCase::IgnoreCase))
			{
				std::string Name = P.get_or<std::string>("name", "");
				if (Name.empty()) { Session.Log(TEXT("[FAIL] add(\"stateless_emitter\") -> name required")); return sol::lua_nil; }
				std::string TemplateStr = P.get_or<std::string>("template_asset", "");

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "AddNStatelessEmitter", "Add Stateless Niagara Emitter"));
				System->Modify();

				FString FName = UTF8_TO_TCHAR(Name.c_str());

				if (!TemplateStr.empty())
				{
					FString FTemplate = UTF8_TO_TCHAR(TemplateStr.c_str());
					UNiagaraEmitter* TemplateEmitter = LoadObject<UNiagaraEmitter>(nullptr, *FTemplate);
					if (!TemplateEmitter) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"stateless_emitter\") -> template '%s' not found"), *FTemplate)); return sol::lua_nil; }
					FGuid NewGuid = FNiagaraEditorUtilities::AddEmitterToSystem(*System, *TemplateEmitter, TemplateEmitter->GetExposedVersion().VersionGuid, true);
					if (NewGuid.IsValid())
					{
						for (int32 i = 0; i < System->GetNumEmitters(); ++i)
						{
							FNiagaraEmitterHandle& Handle = System->GetEmitterHandle(i);
							if (Handle.GetId() == NewGuid)
							{
								Handle.SetName(::FName(*FName), *System);
								Handle.SetEmitterMode(*System, ENiagaraEmitterMode::Stateless);
								break;
							}
						}
						System->MarkPackageDirty();
						Session.Log(FString::Printf(TEXT("[OK] add(\"stateless_emitter\", name=\"%s\", template=\"%s\")"), *FName, *FTemplate));
						return sol::make_object(Lua, true);
					}
				}
				else
				{
					Session.Log(TEXT("[FAIL] add(\"stateless_emitter\") -> template_asset required"));
					return sol::lua_nil;
				}
				Session.Log(FString::Printf(TEXT("[FAIL] add(\"stateless_emitter\") -> failed to add '%s'"), *FName));
				return sol::lua_nil;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Valid: module, emitter, stateless_emitter, renderer, user_parameter, event_handler, simulation_stage"), *FType));
			return sol::lua_nil;
		});

		// ==================================================================
		// remove(type, id)
		// ==================================================================
		AssetObj.set_function("remove", [System, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			// ---- remove("module") ----
			if (FType.Equals(TEXT("module"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<sol::table>()) { Session.Log(TEXT("[FAIL] remove(\"module\") -> {emitter, stage, module_name} required")); return sol::lua_nil; }
				sol::table T = Id.as<sol::table>();
				std::string EmitterName = T.get_or<std::string>("emitter", "");
				std::string StageStr = T.get_or<std::string>("stage", "particle_update");
				std::string ModName = T.get_or<std::string>("module_name", "");
				WarnUnconsumedKeys(Session, T, { TEXT("emitter"), TEXT("stage"), TEXT("module_name") }, TEXT("remove(\"module\")"));
				if (ModName.empty()) { Session.Log(TEXT("[FAIL] remove(\"module\") -> module_name required")); return sol::lua_nil; }

				FString FEmitter = UTF8_TO_TCHAR(EmitterName.c_str());
				ENiagaraScriptUsage Usage = ParseUsage(UTF8_TO_TCHAR(StageStr.c_str()));
				int32 EmIdx = INDEX_NONE;
				bool bSystemLevel = (Usage == ENiagaraScriptUsage::SystemSpawnScript || Usage == ENiagaraScriptUsage::SystemUpdateScript);
				if (!bSystemLevel)
				{
					EmIdx = FindEmitterIndex(System, FEmitter);
					if (EmIdx == INDEX_NONE) { Session.Log(FString::Printf(TEXT("[FAIL] remove(\"module\") -> emitter '%s' not found"), *FEmitter)); return sol::lua_nil; }
				}

				UNiagaraNodeOutput* OutputNode = FindOutputNode(System, EmIdx, Usage);
				if (!OutputNode) { Session.Log(TEXT("[FAIL] remove(\"module\") -> no output node")); return sol::lua_nil; }

				FString FModName = UTF8_TO_TCHAR(ModName.c_str());
				UNiagaraNodeFunctionCall* ModNode = FindModuleByName(OutputNode, FModName);
				if (!ModNode)
				{
					TArray<FString> Available = ListModulesInStack(OutputNode);
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"module\") -> '%s' not found. Available: %s"), *FModName, *FString::Join(Available, TEXT(", "))));
					return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "RemNModule", "Remove Niagara Module"));
				UEdGraph* Graph = ModNode->GetGraph();
				if (Graph) Graph->Modify();

				// Clean up RI params for this module before removing it
				UNiagaraScript* OwningScript = nullptr;
				const FNiagaraEmitterHandle* EmHandlePtr = (EmIdx != INDEX_NONE) ? &System->GetEmitterHandle(EmIdx) : nullptr;
				if (EmIdx != INDEX_NONE)
				{
					FVersionedNiagaraEmitterData* ED = System->GetEmitterHandle(EmIdx).GetEmitterData();
					if (ED)
					{
						TArray<UNiagaraScript*> Scripts;
						ED->GetScripts(Scripts, false);
						for (UNiagaraScript* Sc : Scripts) { if (Sc && Sc->GetUsage() == Usage) { OwningScript = Sc; break; } }
					}
				}
				else
				{
					OwningScript = (Usage == ENiagaraScriptUsage::SystemSpawnScript) ? System->GetSystemSpawnScript() : System->GetSystemUpdateScript();
				}

				if (OwningScript)
				{
					OwningScript->Modify();
					FNiagaraEmitterHandle DummyHandle;
					const FNiagaraEmitterHandle& RIHandle = EmHandlePtr ? *EmHandlePtr : DummyHandle;
					TArray<FNiagaraVariable> ModInputs = DiscoverModuleInputs(ModNode, OwningScript, EmHandlePtr, Usage);
					for (const FNiagaraVariable& Input : ModInputs)
					{
						FNiagaraParameterHandle InputHandle(Input.GetName());
						FNiagaraParameterHandle AliasedHandle = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(InputHandle, ModNode);
						FNiagaraVariable AliasedVar(Input.GetType(), AliasedHandle.GetParameterHandleString());
						RemoveRapidIterationParam(System, RIHandle, OwningScript, AliasedVar);
					}
				}

				// Break param map links and reconnect chain
				UEdGraphPin* ModMapIn = GetParamMapInputPin(ModNode);
				UEdGraphPin* ModMapOut = GetParamMapOutputPin(ModNode);

				UEdGraphPin* UpstreamOut = nullptr;
				TArray<UEdGraphPin*> DownstreamIns;

				if (ModMapIn && ModMapIn->LinkedTo.Num() > 0)
					UpstreamOut = ModMapIn->LinkedTo[0];
				if (ModMapOut)
					DownstreamIns = ModMapOut->LinkedTo;

				// Break param map links (notify linked nodes)
				if (ModMapIn) ModMapIn->BreakAllPinLinks(true);
				if (ModMapOut) ModMapOut->BreakAllPinLinks(true);

				// Reconnect chain
				if (UpstreamOut)
				{
					for (UEdGraphPin* DownIn : DownstreamIns)
					{
						if (DownIn)
						{
							UpstreamOut->MakeLinkTo(DownIn);
							if (UEdGraphNode* UpOwner2 = UpstreamOut->GetOwningNode())
								UpOwner2->PinConnectionListChanged(UpstreamOut);
							if (UEdGraphNode* DownOwner2 = DownIn->GetOwningNode())
								DownOwner2->PinConnectionListChanged(DownIn);
						}
					}
				}

				// Collect and remove subgraph nodes
				TArray<UEdGraphNode*> NodesToRemove;
				TSet<UEdGraphNode*> Visited;
				CollectSubGraphNodes(ModNode, NodesToRemove, Visited);

				for (UEdGraphNode* Node : NodesToRemove)
				{
					if (Node && Graph)
					{
						Node->Modify();
						Node->BreakAllNodeLinks();
						Graph->RemoveNode(Node);
					}
				}

				if (Graph) Graph->NotifyGraphChanged();
				System->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove(\"module\", \"%s\")"), *FModName));
				return sol::make_object(Lua, true);
			}
			// ---- remove("emitter") ----
			else if (FType.Equals(TEXT("emitter"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<std::string>()) { Session.Log(TEXT("[FAIL] remove(\"emitter\") -> name required")); return sol::lua_nil; }
				FString Name = UTF8_TO_TCHAR(Id.as<std::string>().c_str());
				int32 EmIdx = FindEmitterIndex(System, Name);
				if (EmIdx == INDEX_NONE) { Session.Log(FString::Printf(TEXT("[FAIL] remove(\"emitter\") -> '%s' not found"), *Name)); return sol::lua_nil; }

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "RemNEmitter", "Remove Niagara Emitter"));
				System->Modify();
				TSet<FGuid> IdsToRemove;
				IdsToRemove.Add(System->GetEmitterHandle(EmIdx).GetId());
				System->RemoveEmitterHandlesById(IdsToRemove);
				System->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove(\"emitter\", \"%s\")"), *Name));
				return sol::make_object(Lua, true);
			}
			// ---- remove("renderer") ----
			else if (FType.Equals(TEXT("renderer"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<sol::table>()) { Session.Log(TEXT("[FAIL] remove(\"renderer\") -> {emitter, index} required")); return sol::lua_nil; }
				sol::table T = Id.as<sol::table>();
				std::string EmitterName = T.get_or<std::string>("emitter", "");
				int32 Idx = T.get_or("index", -1);

				FString FEmitter = UTF8_TO_TCHAR(EmitterName.c_str());
				int32 EmIdx = FindEmitterIndex(System, FEmitter);
				if (EmIdx == INDEX_NONE) { Session.Log(FString::Printf(TEXT("[FAIL] remove(\"renderer\") -> emitter '%s' not found"), *FEmitter)); return sol::lua_nil; }

				FNiagaraEmitterHandle& Handle = System->GetEmitterHandle(EmIdx);
				FVersionedNiagaraEmitterData* ED = Handle.GetEmitterData();
				UNiagaraEmitter* Emitter = Handle.GetInstance().Emitter;
				if (!ED || !Emitter) { Session.Log(TEXT("[FAIL] remove(\"renderer\") -> no emitter data")); return sol::lua_nil; }

				const TArray<UNiagaraRendererProperties*>& Renderers = ED->GetRenderers();
				if (Idx < 0) Idx = Renderers.Num() - 1;
				if (Idx < 0 || Idx >= Renderers.Num()) { Session.Log(FString::Printf(TEXT("[FAIL] remove(\"renderer\") -> index %d out of range (%d)"), Idx, Renderers.Num())); return sol::lua_nil; }

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "RemNRenderer", "Remove Niagara Renderer"));
				System->Modify();
				Emitter->RemoveRenderer(Renderers[Idx], Handle.GetInstance().Version);
				System->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove(\"renderer\", emitter=\"%s\", index=%d)"), *FEmitter, Idx));
				return sol::make_object(Lua, true);
			}
			// ---- remove("user_parameter") ----
			else if (FType.Equals(TEXT("user_parameter"), ESearchCase::IgnoreCase))
			{
				// Accept both string and table {name=...}
				FString ParamName;
				if (Id.is<std::string>())
					ParamName = UTF8_TO_TCHAR(Id.as<std::string>().c_str());
				else if (Id.is<sol::table>())
					ParamName = UTF8_TO_TCHAR(Id.as<sol::table>().get_or<std::string>("name", "").c_str());
				if (ParamName.IsEmpty()) { Session.Log(TEXT("[FAIL] remove(\"user_parameter\") -> name required")); return sol::lua_nil; }
				FString FullName = TEXT("User.") + ParamName;

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "RemNUserParam", "Remove Niagara User Parameter"));
				System->Modify();
				FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();

				bool bFound = false;
				TArray<FNiagaraVariable> Params;
				Store.GetUserParameters(Params);
				for (const FNiagaraVariable& V : Params)
				{
					FString VName = V.GetName().ToString();
					if (VName.Equals(ParamName, ESearchCase::IgnoreCase) || VName.Equals(FullName, ESearchCase::IgnoreCase))
					{
						Store.RemoveParameter(V);
						bFound = true;
						break;
					}
				}

				if (!bFound) { Session.Log(FString::Printf(TEXT("[FAIL] remove(\"user_parameter\") -> '%s' not found"), *ParamName)); return sol::lua_nil; }
				System->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove(\"user_parameter\", \"%s\")"), *ParamName));
				return sol::make_object(Lua, true);
			}
			// ---- remove("event_handler") ----
			else if (FType.Equals(TEXT("event_handler"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<sol::table>()) { Session.Log(TEXT("[FAIL] remove(\"event_handler\") -> {emitter, usage_id} required")); return sol::lua_nil; }
				sol::table T = Id.as<sol::table>();
				std::string EmitterName = T.get_or<std::string>("emitter", "");
				std::string UsageIdStr = T.get_or<std::string>("usage_id", "");
				int32 Index = T.get_or("index", -1);

				FString FEmitter = UTF8_TO_TCHAR(EmitterName.c_str());
				int32 EmIdx = FindEmitterIndex(System, FEmitter);
				if (EmIdx == INDEX_NONE) { Session.Log(FString::Printf(TEXT("[FAIL] remove(\"event_handler\") -> emitter '%s' not found"), *FEmitter)); return sol::lua_nil; }

				FNiagaraEmitterHandle& Handle = System->GetEmitterHandle(EmIdx);
				FVersionedNiagaraEmitterData* ED = Handle.GetEmitterData();
				UNiagaraEmitter* Emitter = Handle.GetInstance().Emitter;
				if (!ED || !Emitter) { Session.Log(TEXT("[FAIL] remove(\"event_handler\") -> no emitter data")); return sol::lua_nil; }

				const TArray<FNiagaraEventScriptProperties>& Events = ED->GetEventHandlers();
				if (Index < 0) Index = 0;
				if (Index >= Events.Num()) { Session.Log(FString::Printf(TEXT("[FAIL] remove(\"event_handler\") -> index %d out of range (%d)"), Index, Events.Num())); return sol::lua_nil; }

				FGuid UsageId = Events[Index].Script ? Events[Index].Script->GetUsageId() : FGuid();
				if (!UsageId.IsValid()) { Session.Log(TEXT("[FAIL] remove(\"event_handler\") -> could not resolve usage id")); return sol::lua_nil; }

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "RemNEvent", "Remove Niagara Event Handler"));
				System->Modify();
				Emitter->Modify();

				// Clean up orphaned output node and its module subgraph for this event handler
				UNiagaraScriptSource* EvtSource = Cast<UNiagaraScriptSource>(ED->GraphSource);
				if (EvtSource && EvtSource->NodeGraph)
				{
					UNiagaraGraph* EvtGraph = EvtSource->NodeGraph;
					EvtGraph->Modify();
					TArray<UEdGraphNode*> NodesToRemove;
					for (UEdGraphNode* GNode : EvtGraph->Nodes)
					{
						UNiagaraNodeOutput* OutNode = Cast<UNiagaraNodeOutput>(GNode);
						if (OutNode && OutNode->GetUsage() == ENiagaraScriptUsage::ParticleEventScript && OutNode->GetUsageId() == UsageId)
						{
							// Collect the output node and all nodes feeding into it
							TSet<UEdGraphNode*> Visited;
							CollectSubGraphNodes(OutNode, NodesToRemove, Visited);
							break;
						}
					}
					for (UEdGraphNode* N : NodesToRemove)
					{
						if (N) { N->BreakAllNodeLinks(); EvtGraph->RemoveNode(N); }
					}
					if (NodesToRemove.Num() > 0) EvtGraph->NotifyGraphChanged();
				}

				Emitter->RemoveEventHandlerByUsageId(UsageId, Handle.GetInstance().Version);
				System->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove(\"event_handler\", emitter=\"%s\", index=%d)"), *FEmitter, Index));
				return sol::make_object(Lua, true);
			}
			// ---- remove("simulation_stage") ----
			else if (FType.Equals(TEXT("simulation_stage"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<sol::table>()) { Session.Log(TEXT("[FAIL] remove(\"simulation_stage\") -> {emitter, index} required")); return sol::lua_nil; }
				sol::table T = Id.as<sol::table>();
				std::string EmitterName = T.get_or<std::string>("emitter", "");
				int32 Index = T.get_or("index", 0);

				FString FEmitter = UTF8_TO_TCHAR(EmitterName.c_str());
				int32 EmIdx = FindEmitterIndex(System, FEmitter);
				if (EmIdx == INDEX_NONE) { Session.Log(FString::Printf(TEXT("[FAIL] remove(\"simulation_stage\") -> emitter '%s' not found"), *FEmitter)); return sol::lua_nil; }

				FNiagaraEmitterHandle& Handle = System->GetEmitterHandle(EmIdx);
				FVersionedNiagaraEmitterData* ED = Handle.GetEmitterData();
				UNiagaraEmitter* Emitter = Handle.GetInstance().Emitter;
				if (!ED || !Emitter) { Session.Log(TEXT("[FAIL] remove(\"simulation_stage\") -> no emitter data")); return sol::lua_nil; }

				const TArray<UNiagaraSimulationStageBase*>& Stages = ED->GetSimulationStages();
				if (Index >= Stages.Num()) { Session.Log(FString::Printf(TEXT("[FAIL] remove(\"simulation_stage\") -> index %d out of range (%d)"), Index, Stages.Num())); return sol::lua_nil; }

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "RemNSimStage", "Remove Niagara Simulation Stage"));
				System->Modify();
				Emitter->Modify();
				Emitter->RemoveSimulationStage(Stages[Index], Handle.GetInstance().Version);
				System->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove(\"simulation_stage\", emitter=\"%s\", index=%d)"), *FEmitter, Index));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type"), *FType));
			return sol::lua_nil;
		});

		// ==================================================================
		// list(type, params?)
		// ==================================================================
		AssetObj.set_function("list", [System, &Session](sol::table self,
			sol::optional<std::string> TypeOpt, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = TypeOpt.has_value() ? UTF8_TO_TCHAR(TypeOpt.value().c_str()) : TEXT("all");

			if (FType.Equals(TEXT("all"), ESearchCase::IgnoreCase))
			{
				sol::protected_function InfoFn = self["info"];
				if (InfoFn.valid()) return InfoFn(self);
				return sol::lua_nil;
			}

			// ---- list("emitters") ----
			if (FType.Contains(TEXT("emitter"), ESearchCase::IgnoreCase) && !FType.Contains(TEXT("template"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < System->GetNumEmitters(); i++)
				{
					const FNiagaraEmitterHandle& Handle = System->GetEmitterHandle(i);
					FVersionedNiagaraEmitterData* ED = Handle.GetEmitterData();
					sol::table E = Lua.create_table();
					E["index"] = i;
					E["name"] = TCHAR_TO_UTF8(*Handle.GetName().ToString());
					E["enabled"] = Handle.GetIsEnabled();
					E["emitter_mode"] = (Handle.GetEmitterMode() == ENiagaraEmitterMode::Stateless) ? "stateless" : "standard";
					if (Handle.GetEmitterMode() == ENiagaraEmitterMode::Stateless)
					{
						UNiagaraStatelessEmitter* SLE = Handle.GetStatelessEmitter();
						if (SLE)
						{
							// Access via reflection since headers are in Internal/
							FArrayProperty* ModProp = CastField<FArrayProperty>(((UObject*)SLE)->GetClass()->FindPropertyByName(TEXT("Modules")));
							if (ModProp)
							{
								FScriptArrayHelper ModArr(ModProp, ModProp->ContainerPtrToValuePtr<void>((UObject*)SLE));
								E["stateless_modules"] = ModArr.Num();
							}
							FArrayProperty* RendProp = CastField<FArrayProperty>(((UObject*)SLE)->GetClass()->FindPropertyByName(TEXT("RendererProperties")));
							if (RendProp)
							{
								FScriptArrayHelper RendArr(RendProp, RendProp->ContainerPtrToValuePtr<void>((UObject*)SLE));
								E["renderers"] = RendArr.Num();
							}
						}
					}
					else if (ED)
					{
						E["renderers"] = ED->GetRenderers().Num();
						E["event_handlers"] = ED->GetEventHandlers().Num();
						E["simulation_stages"] = ED->GetSimulationStages().Num();
						E["sim_target"] = (ED->SimTarget == ENiagaraSimTarget::GPUComputeSim) ? "gpu" : "cpu";
						E["local_space"] = ED->bLocalSpace;
						E["determinism"] = ED->bDeterminism;
						E["requires_persistent_ids"] = (bool)ED->bRequiresPersistentIDs;
					}
					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"emitters\") -> %d"), System->GetNumEmitters()));
				return Result;
			}
			// ---- list("modules") ----
			if (FType.Contains(TEXT("module"), ESearchCase::IgnoreCase) && !FType.Contains(TEXT("input")))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] list(\"modules\") -> {emitter, stage?} required")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string EmitterName = P.get_or<std::string>("emitter", "");
				std::string StageStr = P.get_or<std::string>("stage", "");

				// Build list of stages to iterate
				TArray<ENiagaraScriptUsage> UsagesToCheck;
				if (StageStr.empty())
				{
					// No stage specified — iterate all emitter stages
					UsagesToCheck.Add(ENiagaraScriptUsage::EmitterSpawnScript);
					UsagesToCheck.Add(ENiagaraScriptUsage::EmitterUpdateScript);
					UsagesToCheck.Add(ENiagaraScriptUsage::ParticleSpawnScript);
					UsagesToCheck.Add(ENiagaraScriptUsage::ParticleUpdateScript);
				}
				else
				{
					UsagesToCheck.Add(ParseUsage(UTF8_TO_TCHAR(StageStr.c_str())));
				}

				int32 EmIdx = INDEX_NONE;
				bool bSystemLevel = false;
				if (UsagesToCheck.Num() == 1)
				{
					bSystemLevel = (UsagesToCheck[0] == ENiagaraScriptUsage::SystemSpawnScript || UsagesToCheck[0] == ENiagaraScriptUsage::SystemUpdateScript);
				}
				if (!bSystemLevel)
				{
					EmIdx = FindEmitterIndex(System, UTF8_TO_TCHAR(EmitterName.c_str()));
					if (EmIdx == INDEX_NONE) { Session.Log(FString::Printf(TEXT("[FAIL] list(\"modules\") -> emitter '%s' not found"), UTF8_TO_TCHAR(EmitterName.c_str()))); return sol::lua_nil; }
				}

				TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
				TArray<FString> ModuleStages;
				for (ENiagaraScriptUsage Usage : UsagesToCheck)
				{
					UNiagaraNodeOutput* OutputNode = FindOutputNode(System, EmIdx, Usage, FGuid(), false);
					if (!OutputNode) continue;

					FString StageName;
					switch (Usage)
					{
					case ENiagaraScriptUsage::EmitterSpawnScript: StageName = TEXT("EmitterSpawn"); break;
					case ENiagaraScriptUsage::EmitterUpdateScript: StageName = TEXT("EmitterUpdate"); break;
					case ENiagaraScriptUsage::ParticleSpawnScript: StageName = TEXT("ParticleSpawn"); break;
					case ENiagaraScriptUsage::ParticleUpdateScript: StageName = TEXT("ParticleUpdate"); break;
					default: StageName = TEXT("Unknown"); break;
					}

					UEdGraphPin* InputPin = GetParamMapInputPin(OutputNode);
					if (InputPin)
					{
						TArray<UNiagaraNodeFunctionCall*> StageModules;
						UEdGraphNode* Current = (InputPin->LinkedTo.Num() > 0 && InputPin->LinkedTo[0]) ? InputPin->LinkedTo[0]->GetOwningNode() : nullptr;
						TSet<UEdGraphNode*> Visited;
						while (Current && !Visited.Contains(Current))
						{
							Visited.Add(Current);
							if (UNiagaraNodeFunctionCall* FC = Cast<UNiagaraNodeFunctionCall>(Current))
								StageModules.Insert(FC, 0);
							UEdGraphPin* PrevPin = GetParamMapInputPin(Current);
							Current = nullptr;
							if (PrevPin && PrevPin->LinkedTo.Num() > 0 && PrevPin->LinkedTo[0])
								Current = PrevPin->LinkedTo[0]->GetOwningNode();
						}
						for (UNiagaraNodeFunctionCall* FC : StageModules)
						{
							ModuleNodes.Add(FC);
							ModuleStages.Add(StageName);
						}
					}
				}

				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < ModuleNodes.Num(); i++)
				{
					sol::table M = Lua.create_table();
					M["index"] = i;
					M["name"] = TCHAR_TO_UTF8(*ModuleNodes[i]->GetFunctionName());
					M["enabled"] = ModuleNodes[i]->GetDesiredEnabledState() == ENodeEnabledState::Enabled;
					if (i < ModuleStages.Num())
						M["stage"] = TCHAR_TO_UTF8(*ModuleStages[i]);
					if (ModuleNodes[i]->FunctionScript)
						M["script"] = TCHAR_TO_UTF8(*ModuleNodes[i]->FunctionScript->GetPathName());
					Result[i + 1] = M;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"modules\") -> %d"), ModuleNodes.Num()));
				return Result;
			}
			// ---- list("renderers") ----
			if (FType.Contains(TEXT("renderer"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] list(\"renderers\") -> {emitter} required")); return sol::lua_nil; }
				std::string EmitterName = Params.value().get_or<std::string>("emitter", "");
				FString FEmitter = UTF8_TO_TCHAR(EmitterName.c_str());
				int32 EmIdx = FindEmitterIndex(System, FEmitter);
				if (EmIdx == INDEX_NONE) { Session.Log(FString::Printf(TEXT("[FAIL] list(\"renderers\") -> emitter '%s' not found"), *FEmitter)); return sol::lua_nil; }

				FVersionedNiagaraEmitterData* ED = System->GetEmitterHandle(EmIdx).GetEmitterData();
				if (!ED) { Session.Log(TEXT("[FAIL] list(\"renderers\") -> no emitter data")); return sol::lua_nil; }

				const TArray<UNiagaraRendererProperties*>& Renderers = ED->GetRenderers();
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Renderers.Num(); i++)
				{
					sol::table R = Lua.create_table();
					R["index"] = i;
					R["type"] = TCHAR_TO_UTF8(*GetRendererTypeName(Renderers[i]));
					R["enabled"] = Renderers[i] ? Renderers[i]->GetIsEnabled() : false;
					R["class"] = Renderers[i] ? TCHAR_TO_UTF8(*Renderers[i]->GetClass()->GetName()) : "";
					if (Renderers[i])
					{
						// Include sort order and material info where available
						if (UNiagaraSpriteRendererProperties* Sprite = Cast<UNiagaraSpriteRendererProperties>(Renderers[i]))
						{
							if (Sprite->Material) R["material"] = TCHAR_TO_UTF8(*Sprite->Material->GetName());
						}
						else if (UNiagaraMeshRendererProperties* Mesh = Cast<UNiagaraMeshRendererProperties>(Renderers[i]))
						{
							R["mesh_count"] = static_cast<int>(Mesh->Meshes.Num());
						}
					}
					Result[i + 1] = R;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"renderers\") -> %d"), Renderers.Num()));
				return Result;
			}
			// ---- list("user_parameters") ----
			if (FType.Contains(TEXT("user_param"), ESearchCase::IgnoreCase) || FType.Contains(TEXT("parameter"), ESearchCase::IgnoreCase))
			{
				FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();
				TArray<FNiagaraVariable> Params2;
				Store.GetUserParameters(Params2);

				sol::table Result = Lua.create_table();
				int32 Idx = 1;
				for (const FNiagaraVariable& V : Params2)
				{
					sol::table E = Lua.create_table();
					E["name"] = TCHAR_TO_UTF8(*V.GetName().ToString());
					E["type"] = TCHAR_TO_UTF8(*V.GetType().GetName());

					// Include current value for known types
					const int32* Offset = Store.FindParameterOffset(V);
					if (Offset)
					{
						if (V.GetType() == FNiagaraTypeDefinition::GetFloatDef())
							E["value"] = Store.GetParameterValue<float>(V);
						else if (V.GetType() == FNiagaraTypeDefinition::GetIntDef())
							E["value"] = Store.GetParameterValue<int32>(V);
						else if (V.GetType() == FNiagaraTypeDefinition::GetBoolDef())
							E["value"] = Store.GetParameterValue<FNiagaraBool>(V).GetValue();
					}

					Result[Idx++] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"user_parameters\") -> %d"), Idx - 1));
				return Result;
			}
			// ---- list("event_handlers") ----
			if (FType.Contains(TEXT("event"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] list(\"event_handlers\") -> {emitter} required")); return sol::lua_nil; }
				std::string EmitterName = Params.value().get_or<std::string>("emitter", "");
				int32 EmIdx = FindEmitterIndex(System, UTF8_TO_TCHAR(EmitterName.c_str()));
				if (EmIdx == INDEX_NONE) { Session.Log(TEXT("[FAIL] list(\"event_handlers\") -> emitter not found")); return sol::lua_nil; }

				FVersionedNiagaraEmitterData* ED = System->GetEmitterHandle(EmIdx).GetEmitterData();
				if (!ED) { Session.Log(TEXT("[FAIL] list(\"event_handlers\") -> no emitter data")); return sol::lua_nil; }

				const TArray<FNiagaraEventScriptProperties>& Events = ED->GetEventHandlers();
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Events.Num(); i++)
				{
					sol::table E = Lua.create_table();
					E["index"] = i;
					E["source_event"] = TCHAR_TO_UTF8(*Events[i].SourceEventName.ToString());
					if (Events[i].Script)
						E["usage_id"] = TCHAR_TO_UTF8(*Events[i].Script->GetUsageId().ToString());
					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"event_handlers\") -> %d"), Events.Num()));
				return Result;
			}
			// ---- list("simulation_stages") ----
			if (FType.Contains(TEXT("simulation"), ESearchCase::IgnoreCase) || FType.Contains(TEXT("simstage"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] list(\"simulation_stages\") -> {emitter} required")); return sol::lua_nil; }
				std::string EmitterName = Params.value().get_or<std::string>("emitter", "");
				int32 EmIdx = FindEmitterIndex(System, UTF8_TO_TCHAR(EmitterName.c_str()));
				if (EmIdx == INDEX_NONE) { Session.Log(TEXT("[FAIL] list(\"simulation_stages\") -> emitter not found")); return sol::lua_nil; }

				FVersionedNiagaraEmitterData* ED = System->GetEmitterHandle(EmIdx).GetEmitterData();
				if (!ED) { Session.Log(TEXT("[FAIL] list(\"simulation_stages\") -> no emitter data")); return sol::lua_nil; }

				const TArray<UNiagaraSimulationStageBase*>& Stages = ED->GetSimulationStages();
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Stages.Num(); i++)
				{
					sol::table E = Lua.create_table();
					E["index"] = i;
					E["name"] = Stages[i] ? TCHAR_TO_UTF8(*Stages[i]->SimulationStageName.ToString()) : "";
					E["enabled"] = Stages[i] ? Stages[i]->bEnabled : false;
					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"simulation_stages\") -> %d"), Stages.Num()));
				return Result;
			}
			// ---- list("module_inputs") ----
			if (FType.Contains(TEXT("input"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] list(\"module_inputs\") -> {emitter, stage, module_name} required")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string EmitterName = P.get_or<std::string>("emitter", "");
				std::string StageStr = P.get_or<std::string>("stage", "particle_update");
				std::string ModName = P.get_or<std::string>("module_name", "");

				// Track consumed keys and warn about unrecognized ones
				TSet<FString> ConsumedKeys = { TEXT("emitter"), TEXT("stage"), TEXT("module_name") };
				WarnUnconsumedKeys(Session, P, ConsumedKeys, TEXT("list(\"module_inputs\")"));

				if (ModName.empty()) { Session.Log(TEXT("[FAIL] list(\"module_inputs\") -> module_name is required (was empty)")); return sol::lua_nil; }

				ENiagaraScriptUsage Usage = ParseUsage(UTF8_TO_TCHAR(StageStr.c_str()));
				int32 EmIdx = INDEX_NONE;
				bool bSys = (Usage == ENiagaraScriptUsage::SystemSpawnScript || Usage == ENiagaraScriptUsage::SystemUpdateScript);
				if (!bSys) EmIdx = FindEmitterIndex(System, UTF8_TO_TCHAR(EmitterName.c_str()));

				UNiagaraNodeOutput* OutputNode = FindOutputNode(System, EmIdx, Usage, FGuid(), false);
				if (!OutputNode) { Session.Log(TEXT("[FAIL] list(\"module_inputs\") -> no output node")); return sol::lua_nil; }

				UNiagaraNodeFunctionCall* ModNode = FindModuleByName(OutputNode, UTF8_TO_TCHAR(ModName.c_str()));
				if (!ModNode) { Session.Log(FString::Printf(TEXT("[FAIL] list(\"module_inputs\") -> module '%s' not found"), UTF8_TO_TCHAR(ModName.c_str()))); return sol::lua_nil; }

				// List input pins — include hidden pins so static-switch-gated inputs are visible
				sol::table Result = Lua.create_table();
				int32 Idx = 1;
				for (UEdGraphPin* Pin : ModNode->GetAllPins())
				{
					if (Pin->Direction == EGPD_Input && !IsParameterMapPin(Pin))
					{
						sol::table E = Lua.create_table();
						E["name"] = TCHAR_TO_UTF8(*Pin->GetName());
						FNiagaraTypeDefinition PinType = UEdGraphSchema_Niagara::PinToTypeDefinition(Pin);
						E["type"] = TCHAR_TO_UTF8(*PinType.GetName());
						E["value_mode"] = "pin";
						E["has_default"] = !Pin->DefaultValue.IsEmpty();
						if (!Pin->DefaultValue.IsEmpty())
							E["default_value"] = TCHAR_TO_UTF8(*Pin->DefaultValue);
						if (Pin->bHidden)
							E["hidden"] = true;
						Result[Idx++] = E;
					}
				}

				// Also list inputs from the called graph (catches versioned template modules)
				if (UNiagaraScript* FuncScript = ModNode->FunctionScript)
				{
					TSet<FString> ExistingNames;
					for (int32 i = 1; i < Idx; i++)
					{
						sol::table Ex = Result[i];
						ExistingNames.Add(UTF8_TO_TCHAR(Ex.get_or<std::string>("name", "").c_str()));
					}
					for (const FNiagaraVariable& ScriptVar : FuncScript->GetVMExecutableData().Parameters.Parameters)
					{
						FString VarName = ScriptVar.GetName().ToString();
						if (!VarName.StartsWith(TEXT("Module."))) continue;
						FString ShortName = VarName.RightChop(7);
						if (ExistingNames.Contains(ShortName)) continue;
						ExistingNames.Add(ShortName);

						sol::table E = Lua.create_table();
						E["name"] = TCHAR_TO_UTF8(*ShortName);
						E["type"] = TCHAR_TO_UTF8(*ScriptVar.GetType().GetName());
						E["value_mode"] = "script_param";
						Result[Idx++] = E;
					}
				}

				// Also list rapid iteration parameters belonging to this module
				UNiagaraScript* RIScript = nullptr;
				if (EmIdx != INDEX_NONE)
				{
					FVersionedNiagaraEmitterData* ED = System->GetEmitterHandle(EmIdx).GetEmitterData();
					if (ED)
					{
						TArray<UNiagaraScript*> Scripts;
						ED->GetScripts(Scripts, false);
						for (UNiagaraScript* Sc : Scripts) { if (Sc && Sc->GetUsage() == Usage) { RIScript = Sc; break; } }
					}
				}
				else
				{
					RIScript = (Usage == ENiagaraScriptUsage::SystemSpawnScript) ? System->GetSystemSpawnScript() : System->GetSystemUpdateScript();
				}

				// Always list RI params — versioned template modules may ONLY expose inputs as RI params
				if (RIScript)
				{
					FString ModFuncName = ModNode->GetFunctionName();
					for (const FNiagaraVariableWithOffset& RIVar : RIScript->RapidIterationParameters.ReadParameterVariables())
					{
						FString RIName = RIVar.GetName().ToString();
						// RI param names contain the module function name as part of the path
						if (!RIName.Contains(ModFuncName)) continue;

						// Extract the short parameter name (last segment after .)
						FString ShortName = RIName;
						int32 LastDot = INDEX_NONE;
						if (ShortName.FindLastChar(TEXT('.'), LastDot))
							ShortName = ShortName.RightChop(LastDot + 1);

						sol::table E = Lua.create_table();
						E["name"] = TCHAR_TO_UTF8(*ShortName);
						E["full_name"] = TCHAR_TO_UTF8(*RIName);
						E["type"] = TCHAR_TO_UTF8(*RIVar.GetType().GetName());
						E["value_mode"] = "rapid_iteration";

						// Read current value for known types
						const int32* Offset = RIScript->RapidIterationParameters.FindParameterOffset(RIVar);
						if (Offset)
						{
							if (RIVar.GetType() == FNiagaraTypeDefinition::GetFloatDef())
								E["value"] = RIScript->RapidIterationParameters.GetParameterValue<float>(RIVar);
							else if (RIVar.GetType() == FNiagaraTypeDefinition::GetIntDef())
								E["value"] = RIScript->RapidIterationParameters.GetParameterValue<int32>(RIVar);
							else if (RIVar.GetType() == FNiagaraTypeDefinition::GetBoolDef())
								E["value"] = RIScript->RapidIterationParameters.GetParameterValue<FNiagaraBool>(RIVar).GetValue();
						}

						Result[Idx++] = E;
					}
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"module_inputs\") -> %d"), Idx - 1));
				return Result;
			}
			// ---- list("reflected_properties") ----
			if (FType.Contains(TEXT("reflected"), ESearchCase::IgnoreCase) || FType.Contains(TEXT("propert"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] list(\"reflected_properties\") -> {target, emitter?} required")); return sol::lua_nil; }
				sol::table P = Params.value();
				// Auto-detect target from params if not explicitly set
				std::string TargetStr = P.get_or<std::string>("target", "");
				if (TargetStr.empty())
				{
					// If renderer_index is present, target is renderer; if emitter is present, target is emitter; else system
					sol::optional<int> HasRendIdx = P.get<sol::optional<int>>("renderer_index");
					if (HasRendIdx.has_value()) TargetStr = "renderer";
					else if (!P.get_or<std::string>("emitter", "").empty()) TargetStr = "emitter";
					else TargetStr = "system";
				}
				FString FTarget = UTF8_TO_TCHAR(TargetStr.c_str());

				UObject* TargetObj = nullptr;
				if (FTarget.Equals(TEXT("system"), ESearchCase::IgnoreCase))
				{
					TargetObj = System;
				}
				else if (FTarget.Equals(TEXT("emitter"), ESearchCase::IgnoreCase))
				{
					std::string EmName = P.get_or<std::string>("emitter", "");
					int32 EmIdx = FindEmitterIndex(System, UTF8_TO_TCHAR(EmName.c_str()));
					if (EmIdx != INDEX_NONE)
						TargetObj = System->GetEmitterHandle(EmIdx).GetInstance().Emitter;
				}
				else if (FTarget.Equals(TEXT("renderer"), ESearchCase::IgnoreCase))
				{
					std::string EmName = P.get_or<std::string>("emitter", "");
					int32 RIdx = P.get_or("renderer_index", 0);
					int32 EmIdx = FindEmitterIndex(System, UTF8_TO_TCHAR(EmName.c_str()));
					if (EmIdx != INDEX_NONE)
					{
						FVersionedNiagaraEmitterData* ED = System->GetEmitterHandle(EmIdx).GetEmitterData();
						if (ED && RIdx >= 0 && RIdx < ED->GetRenderers().Num())
							TargetObj = ED->GetRenderers()[RIdx];
					}
				}

				if (!TargetObj) { Session.Log(FString::Printf(TEXT("[FAIL] list(\"reflected_properties\") -> target '%s' not resolved"), *FTarget)); return sol::lua_nil; }

				sol::table Result = Lua.create_table();
				int32 Idx = 1;
				for (TFieldIterator<FProperty> It(TargetObj->GetClass()); It; ++It)
				{
					FProperty* Prop = *It;
					if (!Prop || !Prop->HasAnyPropertyFlags(CPF_Edit)) continue;
					sol::table E = Lua.create_table();
					E["name"] = TCHAR_TO_UTF8(*Prop->GetName());
					E["type"] = TCHAR_TO_UTF8(*Prop->GetCPPType());
					Result[Idx++] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"reflected_properties\") -> %d on %s"), Idx - 1, *FTarget));
				return Result;
			}

			// ---- list("dynamic_inputs") ----
			if (FType.Contains(TEXT("dynamic"), ESearchCase::IgnoreCase))
			{
				FString TypeFilter;
				if (Params.has_value())
					TypeFilter = UTF8_TO_TCHAR(Params.value().get_or<std::string>("type_filter", "").c_str());

				FNiagaraEditorUtilities::FGetFilteredScriptAssetsOptions Options;
				Options.ScriptUsageToInclude = ENiagaraScriptUsage::DynamicInput;
				TArray<FAssetData> Assets;
				FNiagaraEditorUtilities::GetFilteredScriptAssets(Options, Assets);

				sol::table Result = Lua.create_table();
				int32 Idx = 1;
				for (const FAssetData& Asset : Assets)
				{
					FString AssetName = Asset.AssetName.ToString();
					if (!TypeFilter.IsEmpty() && !AssetName.Contains(TypeFilter, ESearchCase::IgnoreCase))
						continue;
					sol::table E = Lua.create_table();
					E["name"] = TCHAR_TO_UTF8(*AssetName);
					E["path"] = TCHAR_TO_UTF8(*Asset.GetObjectPathString());
					Result[Idx++] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"dynamic_inputs\") -> %d"), Idx - 1));
				return Result;
			}
			// ---- list("scratch_pad_scripts") ----
			if (FType.Contains(TEXT("scratch"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				int32 Idx = 1;
				for (UNiagaraScript* ScratchScript : System->ScratchPadScripts)
				{
					if (!ScratchScript) continue;
					sol::table E = Lua.create_table();
					E["name"] = TCHAR_TO_UTF8(*ScratchScript->GetName());
					E["usage"] = TCHAR_TO_UTF8(*StaticEnum<ENiagaraScriptUsage>()->GetNameStringByValue((int64)ScratchScript->GetUsage()));
					Result[Idx++] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"scratch_pad_scripts\") -> %d"), Idx - 1));
				return Result;
			}
			// ---- list("available_modules") ----
			if (FType.Contains(TEXT("available_module"), ESearchCase::IgnoreCase))
			{
				FNiagaraEditorUtilities::FGetFilteredScriptAssetsOptions Options;
				Options.ScriptUsageToInclude = ENiagaraScriptUsage::Module;

				if (Params.has_value())
				{
					// "usage" overrides the script usage filter (Module, DynamicInput)
					// "stage" is accepted but does NOT change the usage — modules are always usage=Module
					std::string UsageFilter = Params.value().get_or<std::string>("usage", "");
					if (!UsageFilter.empty())
					{
						FString FUsage = UTF8_TO_TCHAR(UsageFilter.c_str());
						if (FUsage.Contains(TEXT("DynamicInput"), ESearchCase::IgnoreCase) || FUsage.Contains(TEXT("dynamic_input"), ESearchCase::IgnoreCase))
							Options.ScriptUsageToInclude = ENiagaraScriptUsage::DynamicInput;
						else if (FUsage.Contains(TEXT("Module"), ESearchCase::IgnoreCase))
							Options.ScriptUsageToInclude = ENiagaraScriptUsage::Module;
					}
				}

				TArray<FAssetData> Assets;
				FNiagaraEditorUtilities::GetFilteredScriptAssets(Options, Assets);

				sol::table Result = Lua.create_table();
				int32 Idx = 1;
				for (const FAssetData& Asset : Assets)
				{
					sol::table E = Lua.create_table();
					E["name"] = TCHAR_TO_UTF8(*Asset.AssetName.ToString());
					E["path"] = TCHAR_TO_UTF8(*Asset.GetObjectPathString());
					FString Desc;
					if (Asset.GetTagValue(TEXT("Description"), Desc) && !Desc.IsEmpty())
						E["description"] = TCHAR_TO_UTF8(*Desc);
					Result[Idx++] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"available_modules\") -> %d"), Idx - 1));
				return Result;
			}
			// ---- list("emitter_templates") ----
			if (FType.Contains(TEXT("emitter_template"), ESearchCase::IgnoreCase))
			{
				IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
				TArray<FAssetData> EmitterAssets;

				// In UE 5.7, emitters are sub-objects of NiagaraSystem — not standalone assets.
				// Search by class AND by known template directories.
				Registry.GetAssetsByClass(UNiagaraEmitter::StaticClass()->GetClassPathName(), EmitterAssets, true);

				// Also search known Niagara template paths
				static const TCHAR* TemplatePaths[] = {
					TEXT("/Niagara/DefaultAssets/Templates"),
					TEXT("/Niagara/Templates"),
					TEXT("/Game"),
				};
				for (const TCHAR* SearchPath : TemplatePaths)
				{
					TArray<FAssetData> PathAssets;
					Registry.GetAssetsByPath(FName(SearchPath), PathAssets, true);
					for (const FAssetData& A : PathAssets)
					{
						FString ClassName = A.AssetClassPath.GetAssetName().ToString();
						if (ClassName.Contains(TEXT("NiagaraEmitter")))
						{
							EmitterAssets.AddUnique(A);
						}
					}
				}

				sol::table Result = Lua.create_table();
				int32 Idx = 1;
				for (const FAssetData& Asset : EmitterAssets)
				{
					FString AssetPath = Asset.GetObjectPathString();
					bool bIsTemplate = AssetPath.Contains(TEXT("Template")) || AssetPath.Contains(TEXT("DefaultAssets")) || AssetPath.Contains(TEXT("Niagara"));
					if (!bIsTemplate && EmitterAssets.Num() > 50) continue;

					sol::table E = Lua.create_table();
					E["name"] = TCHAR_TO_UTF8(*Asset.AssetName.ToString());
					E["path"] = TCHAR_TO_UTF8(*AssetPath);
					Result[Idx++] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"emitter_templates\") -> %d"), Idx - 1));
				return Result;
			}
			// ---- list("stateless_modules") ----
			if (FType.Contains(TEXT("stateless_module"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] list(\"stateless_modules\") -> {emitter} required")); return sol::lua_nil; }
				std::string EmitterName = Params.value().get_or<std::string>("emitter", "");
				FString FEmitter = UTF8_TO_TCHAR(EmitterName.c_str());
				int32 EmIdx = FindEmitterIndex(System, FEmitter);
				if (EmIdx == INDEX_NONE) { Session.Log(FString::Printf(TEXT("[FAIL] list(\"stateless_modules\") -> emitter '%s' not found"), *FEmitter)); return sol::lua_nil; }

				const FNiagaraEmitterHandle& Handle = System->GetEmitterHandle(EmIdx);
				if (Handle.GetEmitterMode() != ENiagaraEmitterMode::Stateless)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] list(\"stateless_modules\") -> emitter '%s' is not in stateless mode"), *FEmitter));
					return sol::lua_nil;
				}

				UNiagaraStatelessEmitter* SLE = Handle.GetStatelessEmitter();
				if (!SLE) { Session.Log(TEXT("[FAIL] list(\"stateless_modules\") -> no stateless emitter")); return sol::lua_nil; }

				// Access Modules array via reflection (header is in Internal/)
				FArrayProperty* ModProp = CastField<FArrayProperty>(((UObject*)SLE)->GetClass()->FindPropertyByName(TEXT("Modules")));
				if (!ModProp) { Session.Log(TEXT("[FAIL] list(\"stateless_modules\") -> Modules property not found")); return sol::lua_nil; }

				FScriptArrayHelper ModArr(ModProp, ModProp->ContainerPtrToValuePtr<void>((UObject*)SLE));
				FObjectPropertyBase* InnerProp = CastField<FObjectPropertyBase>(ModProp->Inner);

				sol::table Result = Lua.create_table();
				int32 Idx = 1;
				for (int32 i = 0; i < ModArr.Num(); ++i)
				{
					UObject* Mod = InnerProp ? InnerProp->GetObjectPropertyValue(ModArr.GetRawPtr(i)) : nullptr;
					if (!Mod) continue;
					sol::table E = Lua.create_table();
					E["index"] = i;
					E["class"] = TCHAR_TO_UTF8(*Mod->GetClass()->GetName());
					// Check bModuleEnabled via reflection
					FBoolProperty* EnabledProp = CastField<FBoolProperty>(Mod->GetClass()->FindPropertyByName(TEXT("bModuleEnabled")));
					E["enabled"] = EnabledProp ? EnabledProp->GetPropertyValue_InContainer(Mod) : true;
					// Strip "NiagaraStatelessModule_" prefix for friendly name
					FString ClassName = Mod->GetClass()->GetName();
					FString FriendlyName = ClassName;
					FriendlyName.RemoveFromStart(TEXT("NiagaraStatelessModule_"));
					E["name"] = TCHAR_TO_UTF8(*FriendlyName);
					Result[Idx++] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"stateless_modules\") -> %d"), Idx - 1));
				return Result;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type"), *FType));
			return sol::lua_nil;
		});

		// ==================================================================
		// configure(type, id, params)
		// ==================================================================
		AssetObj.set_function("configure", [System, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			// ---- configure("emitter") ----
			if (FType.Equals(TEXT("emitter"), ESearchCase::IgnoreCase))
			{
				FString EmName = Id.is<std::string>() ? UTF8_TO_TCHAR(Id.as<std::string>().c_str()) : TEXT("");
				if (EmName.IsEmpty()) EmName = UTF8_TO_TCHAR(Params.get_or<std::string>("emitter", "").c_str());
				int32 EmIdx = FindEmitterIndex(System, EmName);
				if (EmIdx == INDEX_NONE) { Session.Log(FString::Printf(TEXT("[FAIL] configure(\"emitter\") -> '%s' not found"), *EmName)); return sol::lua_nil; }

				FVersionedNiagaraEmitterData* ED = System->GetEmitterHandle(EmIdx).GetEmitterData();
				if (!ED) { Session.Log(TEXT("[FAIL] configure(\"emitter\") -> no emitter data")); return sol::lua_nil; }

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "CfgNEmitter", "Configure Niagara Emitter"));
				System->Modify();

				sol::optional<std::string> SimTarget = Params.get<sol::optional<std::string>>("sim_target");
				if (SimTarget.has_value())
				{
					FString ST = UTF8_TO_TCHAR(SimTarget.value().c_str());
					if (ST.Contains(TEXT("gpu"), ESearchCase::IgnoreCase))
						ED->SimTarget = ENiagaraSimTarget::GPUComputeSim;
					else
						ED->SimTarget = ENiagaraSimTarget::CPUSim;
				}

				sol::optional<bool> LocalSpace = Params.get<sol::optional<bool>>("local_space");
				if (LocalSpace.has_value()) ED->bLocalSpace = LocalSpace.value();

				sol::optional<bool> Determinism = Params.get<sol::optional<bool>>("determinism");
				if (Determinism.has_value()) ED->bDeterminism = Determinism.value();

				sol::optional<int> RandomSeed = Params.get<sol::optional<int>>("random_seed");
				if (RandomSeed.has_value()) ED->RandomSeed = RandomSeed.value();

	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			sol::optional<std::string> InterpSpawn = Params.get<sol::optional<std::string>>("interpolated_spawn_mode");
				if (InterpSpawn.has_value())
				{
					FString ModeStr = UTF8_TO_TCHAR(InterpSpawn.value().c_str());
					if (ModeStr.Equals(TEXT("no_interpolation"), ESearchCase::IgnoreCase))
						ED->InterpolatedSpawnMode = ENiagaraInterpolatedSpawnMode::NoInterpolation;
					else if (ModeStr.Equals(TEXT("run_update_script"), ESearchCase::IgnoreCase))
						ED->InterpolatedSpawnMode = ENiagaraInterpolatedSpawnMode::RunUpdateScript;
					else if (ModeStr.Equals(TEXT("interpolation"), ESearchCase::IgnoreCase))
						ED->InterpolatedSpawnMode = ENiagaraInterpolatedSpawnMode::Interpolation;
				}
#endif

				sol::optional<bool> PersistentIDs = Params.get<sol::optional<bool>>("requires_persistent_ids");
				if (PersistentIDs.has_value()) ED->bRequiresPersistentIDs = PersistentIDs.value();

				sol::optional<bool> Enabled = Params.get<sol::optional<bool>>("enabled");
				if (Enabled.has_value())
				{
					System->GetEmitterHandle(EmIdx).SetIsEnabled(Enabled.value(), *System, false);
				}

				// Bounds mode
				sol::optional<std::string> BoundsMode = Params.get<sol::optional<std::string>>("bounds_mode");
				if (BoundsMode.has_value())
				{
					FString BM = UTF8_TO_TCHAR(BoundsMode.value().c_str());
					if (BM.Contains(TEXT("dynamic"), ESearchCase::IgnoreCase))
						ED->CalculateBoundsMode = ENiagaraEmitterCalculateBoundMode::Dynamic;
					else if (BM.Contains(TEXT("fixed"), ESearchCase::IgnoreCase))
						ED->CalculateBoundsMode = ENiagaraEmitterCalculateBoundMode::Fixed;
					else if (BM.Contains(TEXT("program"), ESearchCase::IgnoreCase))
						ED->CalculateBoundsMode = ENiagaraEmitterCalculateBoundMode::Programmable;
				}

				// Fixed bounds: table {min={x,y,z}, max={x,y,z}} or flat {min_x, min_y, min_z, max_x, max_y, max_z}
				// Also accept a single number for symmetric bounds (e.g. 100 -> box from -100 to 100)
				sol::optional<sol::table> FixedBoundsTable = Params.get<sol::optional<sol::table>>("fixed_bounds");
				sol::optional<double> FixedBoundsExtent = Params.get<sol::optional<double>>("fixed_bounds");
				if (FixedBoundsTable.has_value())
				{
					sol::table BT = FixedBoundsTable.value();
					sol::optional<sol::table> MinT = BT.get<sol::optional<sol::table>>("min");
					sol::optional<sol::table> MaxT = BT.get<sol::optional<sol::table>>("max");
					if (MinT.has_value() && MaxT.has_value())
					{
						FVector Min(MinT.value().get_or("x", -100.0), MinT.value().get_or("y", -100.0), MinT.value().get_or("z", -100.0));
						FVector Max(MaxT.value().get_or("x", 100.0), MaxT.value().get_or("y", 100.0), MaxT.value().get_or("z", 100.0));
						ED->FixedBounds = FBox(Min, Max);
					}
					else
					{
						FVector Min(BT.get_or("min_x", -100.0), BT.get_or("min_y", -100.0), BT.get_or("min_z", -100.0));
						FVector Max(BT.get_or("max_x", 100.0), BT.get_or("max_y", 100.0), BT.get_or("max_z", 100.0));
						ED->FixedBounds = FBox(Min, Max);
					}
				}
				else if (FixedBoundsExtent.has_value())
				{
					double E = FixedBoundsExtent.value();
					ED->FixedBounds = FBox(FVector(-E), FVector(E));
				}

				// Allocation mode
				sol::optional<std::string> AllocMode = Params.get<sol::optional<std::string>>("allocation_mode");
				if (AllocMode.has_value())
				{
					FString AM = UTF8_TO_TCHAR(AllocMode.value().c_str());
					if (AM.Contains(TEXT("auto"), ESearchCase::IgnoreCase))
						ED->AllocationMode = EParticleAllocationMode::AutomaticEstimate;
					else if (AM.Contains(TEXT("manual"), ESearchCase::IgnoreCase) || AM.Contains(TEXT("fixed"), ESearchCase::IgnoreCase))
						ED->AllocationMode = EParticleAllocationMode::ManualEstimate;
				}

				sol::optional<int> PreAllocCount = Params.get<sol::optional<int>>("pre_allocation_count");
				if (PreAllocCount.has_value()) ED->PreAllocationCount = PreAllocCount.value();

				sol::optional<int> MaxGPUSpawn = Params.get<sol::optional<int>>("max_gpu_particles_spawn_per_frame");
				if (MaxGPUSpawn.has_value()) ED->MaxGPUParticlesSpawnPerFrame = MaxGPUSpawn.value();

				// Notify engine of emitter property changes so recompile/UI update is triggered
				UNiagaraEmitter* EmitterObj = System->GetEmitterHandle(EmIdx).GetInstance().Emitter;
				if (EmitterObj) EmitterObj->PostEditChange();
				System->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"emitter\", \"%s\")"), *EmName));
				return sol::make_object(Lua, true);
			}
			// ---- configure("system") ----
			else if (FType.Equals(TEXT("system"), ESearchCase::IgnoreCase))
			{
				const FScopedTransaction Tx(NSLOCTEXT("AIK", "CfgNSystem", "Configure Niagara System"));
				System->Modify();

				sol::optional<double> WarmupTime = Params.get<sol::optional<double>>("warmup_time");
				if (WarmupTime.has_value()) System->SetWarmupTime(static_cast<float>(WarmupTime.value()));

				sol::optional<bool> Determinism = Params.get<sol::optional<bool>>("determinism");
				if (Determinism.has_value())
				{
					FBoolProperty* Prop = CastField<FBoolProperty>(System->GetClass()->FindPropertyByName(TEXT("bFixedRandomSeed")));
					if (Prop) Prop->SetPropertyValue_InContainer(System, Determinism.value());
				}

				sol::optional<int> RandomSeed = Params.get<sol::optional<int>>("random_seed");
				if (RandomSeed.has_value())
				{
					FIntProperty* Prop = CastField<FIntProperty>(System->GetClass()->FindPropertyByName(TEXT("RandomSeed")));
					if (Prop) Prop->SetPropertyValue_InContainer(System, RandomSeed.value());
				}

				// Performance flags
				sol::optional<bool> BakeOutRI = Params.get<sol::optional<bool>>("bake_out_rapid_iteration");
				if (BakeOutRI.has_value())
				{
					System->SetBakeOutRapidIterationOnCook(BakeOutRI.value());
				}

				sol::optional<bool> TrimAttrs = Params.get<sol::optional<bool>>("trim_attributes");
				if (TrimAttrs.has_value())
				{
					System->SetTrimAttributesOnCook(TrimAttrs.value());
				}

				sol::optional<bool> CompressAttrs = Params.get<sol::optional<bool>>("compress_attributes");
				if (CompressAttrs.has_value())
				{
					FBoolProperty* Prop = CastField<FBoolProperty>(System->GetClass()->FindPropertyByName(TEXT("bCompressAttributes")));
					if (Prop) Prop->SetPropertyValue_InContainer(System, CompressAttrs.value());
				}

					// Generic reflected properties via "properties" sub-table
				sol::optional<sol::table> Props = Params.get<sol::optional<sol::table>>("properties");
				if (Props.has_value())
				{
					for (auto& kv : Props.value())
					{
						if (!kv.first.is<std::string>()) continue;
						FString PropName = UTF8_TO_TCHAR(kv.first.as<std::string>().c_str());
						FString Err;
						double NumVal = kv.second.is<double>() ? kv.second.as<double>() : 0.0;
						FString StrVal = kv.second.is<std::string>() ? UTF8_TO_TCHAR(kv.second.as<std::string>().c_str()) : TEXT("");
						bool BoolVal = kv.second.is<bool>() ? kv.second.as<bool>() : false;
						if (!SetReflectedProperty(System, PropName, NumVal, StrVal, BoolVal, kv.second.is<std::string>(), kv.second.is<bool>(), Err))
							Session.Log(FString::Printf(TEXT("[WARN] SetReflectedProperty('%s'): %s"), *PropName, *Err));
					}
				}

				System->MarkPackageDirty();
				Session.Log(TEXT("[OK] configure(\"system\")"));
				return sol::make_object(Lua, true);
			}
			// ---- configure("renderer") ----
			else if (FType.Equals(TEXT("renderer"), ESearchCase::IgnoreCase))
			{
				// Accept emitter/index from Id table (2nd arg) or Params table (3rd arg)
				std::string EmName;
				int32 RIdx = 0;
				if (Id.is<sol::table>())
				{
					sol::table IdT = Id.as<sol::table>();
					EmName = IdT.get_or<std::string>("emitter", "");
					RIdx = IdT.get_or("index", 0);
				}
				if (EmName.empty()) EmName = Params.get_or<std::string>("emitter", "");
				if (Id.is<int>()) RIdx = Id.as<int>();
				else if (RIdx == 0) RIdx = Params.get_or("index", 0);
				int32 EmIdx = FindEmitterIndex(System, UTF8_TO_TCHAR(EmName.c_str()));
				if (EmIdx == INDEX_NONE) { Session.Log(TEXT("[FAIL] configure(\"renderer\") -> emitter not found")); return sol::lua_nil; }

				FVersionedNiagaraEmitterData* ED = System->GetEmitterHandle(EmIdx).GetEmitterData();
				if (!ED) { Session.Log(TEXT("[FAIL] configure(\"renderer\") -> no emitter data")); return sol::lua_nil; }

				const TArray<UNiagaraRendererProperties*>& Renderers = ED->GetRenderers();
				if (RIdx < 0 || RIdx >= Renderers.Num()) { Session.Log(TEXT("[FAIL] configure(\"renderer\") -> index out of range")); return sol::lua_nil; }

				UNiagaraRendererProperties* Renderer = Renderers[RIdx];
				if (!Renderer) { Session.Log(TEXT("[FAIL] configure(\"renderer\") -> null renderer")); return sol::lua_nil; }

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "CfgNRenderer", "Configure Niagara Renderer"));
				Renderer->Modify();

				sol::optional<sol::table> Props = Params.get<sol::optional<sol::table>>("properties");
				if (Props.has_value())
				{
					for (auto& kv : Props.value())
					{
						if (!kv.first.is<std::string>()) continue;
						FString PropName = UTF8_TO_TCHAR(kv.first.as<std::string>().c_str());
						FString Err;
						double NumVal = kv.second.is<double>() ? kv.second.as<double>() : 0.0;
						FString StrVal = kv.second.is<std::string>() ? UTF8_TO_TCHAR(kv.second.as<std::string>().c_str()) : TEXT("");
						bool BoolVal = kv.second.is<bool>() ? kv.second.as<bool>() : false;
						if (!SetReflectedProperty(Renderer, PropName, NumVal, StrVal, BoolVal, kv.second.is<std::string>(), kv.second.is<bool>(), Err))
							Session.Log(FString::Printf(TEXT("[WARN] SetReflectedProperty('%s'): %s"), *PropName, *Err));
					}
				}

				Renderer->PostEditChange();
				System->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"renderer\", index=%d)"), RIdx));
				return sol::make_object(Lua, true);
			}
			// ---- configure("reflected_properties") ----
			else if (FType.Contains(TEXT("reflected"), ESearchCase::IgnoreCase))
			{
				std::string TargetStr = Params.get_or<std::string>("target", "system");
				FString FTarget = UTF8_TO_TCHAR(TargetStr.c_str());

				UObject* TargetObj = nullptr;
				if (FTarget.Equals(TEXT("system"), ESearchCase::IgnoreCase)) TargetObj = System;
				else if (FTarget.Equals(TEXT("emitter"), ESearchCase::IgnoreCase))
				{
					std::string EmName = Params.get_or<std::string>("emitter", "");
					int32 EmIdx = FindEmitterIndex(System, UTF8_TO_TCHAR(EmName.c_str()));
					if (EmIdx != INDEX_NONE)
						TargetObj = System->GetEmitterHandle(EmIdx).GetInstance().Emitter;
				}
				else if (FTarget.Equals(TEXT("renderer"), ESearchCase::IgnoreCase))
				{
					std::string EmName = Params.get_or<std::string>("emitter", "");
					int32 RIdx = Params.get_or("renderer_index", 0);
					int32 EmIdx = FindEmitterIndex(System, UTF8_TO_TCHAR(EmName.c_str()));
					if (EmIdx != INDEX_NONE)
					{
						FVersionedNiagaraEmitterData* ED = System->GetEmitterHandle(EmIdx).GetEmitterData();
						if (ED && RIdx >= 0 && RIdx < ED->GetRenderers().Num())
							TargetObj = ED->GetRenderers()[RIdx];
					}
				}

				if (!TargetObj) { Session.Log(FString::Printf(TEXT("[FAIL] configure(\"reflected_properties\") -> target '%s' not resolved"), *FTarget)); return sol::lua_nil; }

				sol::optional<sol::table> Props = Params.get<sol::optional<sol::table>>("properties");
				if (!Props.has_value()) { Session.Log(TEXT("[FAIL] configure(\"reflected_properties\") -> properties table required")); return sol::lua_nil; }

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "CfgNReflected", "Configure Niagara Reflected Properties"));
				TargetObj->Modify();

				int32 SetCount = 0;
				for (auto& kv : Props.value())
				{
					if (!kv.first.is<std::string>()) continue;
					FString PropName = UTF8_TO_TCHAR(kv.first.as<std::string>().c_str());
					FString Err;
					double NumVal = kv.second.is<double>() ? kv.second.as<double>() : 0.0;
					FString StrVal = kv.second.is<std::string>() ? UTF8_TO_TCHAR(kv.second.as<std::string>().c_str()) : TEXT("");
					bool BoolVal = kv.second.is<bool>() ? kv.second.as<bool>() : false;
					if (SetReflectedProperty(TargetObj, PropName, NumVal, StrVal, BoolVal, kv.second.is<std::string>(), kv.second.is<bool>(), Err))
						SetCount++;
					else
						Session.Log(FString::Printf(TEXT("[WARN] SetReflectedProperty('%s'): %s"), *PropName, *Err));
				}

				TargetObj->PostEditChange();
				System->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"reflected_properties\", target=\"%s\", %d set)"), *FTarget, SetCount));
				return sol::make_object(Lua, true);
			}

			// ---- configure("module") ----
			// Supports advanced parameter modes: {mode="dynamic_input", script="...", parameters={...}}
			// {mode="linked", parameter="Particles.Position"}, {mode="hlsl", code="..."},
			// {mode="data_interface", type="SkeletalMesh"}, {mode="reset"}, or plain values (static)
			else if (FType.Equals(TEXT("module"), ESearchCase::IgnoreCase))
			{
				FString ModName = Id.is<std::string>() ? UTF8_TO_TCHAR(Id.as<std::string>().c_str()) : TEXT("");
				if (ModName.IsEmpty()) { Session.Log(TEXT("[FAIL] configure(\"module\") -> module name required (second argument)")); return sol::lua_nil; }

				std::string EmitterName = Params.get_or<std::string>("emitter", "");
				std::string StageStr = Params.get_or<std::string>("stage", "particle_update");
				WarnUnconsumedKeys(Session, Params, { TEXT("emitter"), TEXT("stage"), TEXT("parameters") }, TEXT("configure(\"module\")"));
				ENiagaraScriptUsage Usage = ParseUsage(UTF8_TO_TCHAR(StageStr.c_str()));

				int32 EmIdx = INDEX_NONE;
				bool bSys = (Usage == ENiagaraScriptUsage::SystemSpawnScript || Usage == ENiagaraScriptUsage::SystemUpdateScript);
				if (!bSys)
				{
					EmIdx = FindEmitterIndex(System, UTF8_TO_TCHAR(EmitterName.c_str()));
					if (EmIdx == INDEX_NONE) { Session.Log(FString::Printf(TEXT("[FAIL] configure(\"module\") -> emitter '%s' not found"), UTF8_TO_TCHAR(EmitterName.c_str()))); return sol::lua_nil; }
				}

				UNiagaraNodeOutput* OutputNode = FindOutputNode(System, EmIdx, Usage);
				if (!OutputNode) { Session.Log(TEXT("[FAIL] configure(\"module\") -> no output node")); return sol::lua_nil; }

				UNiagaraNodeFunctionCall* ModNode = FindModuleByName(OutputNode, ModName);
				if (!ModNode) { Session.Log(FString::Printf(TEXT("[FAIL] configure(\"module\") -> '%s' not found"), *ModName)); return sol::lua_nil; }

				sol::optional<sol::table> ParamsOpt = Params.get<sol::optional<sol::table>>("parameters");
				if (!ParamsOpt.has_value()) { Session.Log(TEXT("[FAIL] configure(\"module\") -> parameters table required")); return sol::lua_nil; }

				// Resolve script and emitter handle for RI param operations
				UNiagaraScript* Script = nullptr;
				if (EmIdx != INDEX_NONE)
				{
					FVersionedNiagaraEmitterData* ED = System->GetEmitterHandle(EmIdx).GetEmitterData();
					if (ED)
					{
						TArray<UNiagaraScript*> Scripts;
						ED->GetScripts(Scripts, false);
						for (UNiagaraScript* Sc : Scripts)
						{
							if (Sc && Sc->GetUsage() == Usage) { Script = Sc; break; }
						}
					}
				}
				else
				{
					Script = (Usage == ENiagaraScriptUsage::SystemSpawnScript) ? System->GetSystemSpawnScript() : System->GetSystemUpdateScript();
				}

				const FNiagaraEmitterHandle* EmitterHandlePtr = (EmIdx != INDEX_NONE) ? &System->GetEmitterHandle(EmIdx) : nullptr;
				FNiagaraEmitterHandle DummyHandle;
				if (!EmitterHandlePtr) EmitterHandlePtr = &DummyHandle;

				TArray<FNiagaraVariable> AvailableInputs = DiscoverModuleInputs(ModNode, Script, EmitterHandlePtr, Usage);

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "CfgNModule", "Configure Niagara Module"));
				int32 SetCount = 0;
				bool bGraphChanged = false; // Track if graph-structural changes happened (advanced modes)
				TArray<FString> Errors;

				for (auto& kv : ParamsOpt.value())
				{
					if (!kv.first.is<std::string>()) continue;
					FString ParamName = UTF8_TO_TCHAR(kv.first.as<std::string>().c_str());

					// Find matching input
					FNiagaraVariable* MatchedInput = nullptr;
					for (FNiagaraVariable& Input : AvailableInputs)
					{
						FString InputName = Input.GetName().ToString();
						FString ShortName = InputName;
						if (ShortName.Contains(TEXT(".")))
							ShortName = ShortName.RightChop(ShortName.Find(TEXT("."), ESearchCase::IgnoreCase, ESearchDir::FromEnd) + 1);
						if (InputName.Equals(ParamName, ESearchCase::IgnoreCase) ||
							ShortName.Equals(ParamName, ESearchCase::IgnoreCase) ||
							InputName.Equals(TEXT("Module.") + ParamName, ESearchCase::IgnoreCase))
						{
							MatchedInput = &Input;
							break;
						}
					}

					if (!MatchedInput)
					{
						Errors.Add(FString::Printf(TEXT("'%s' not found"), *ParamName));
						continue;
					}

					// Check if value is a table with "mode" key (advanced mode)
					bool bAdvanced = false;
					if (kv.second.is<sol::table>())
					{
						sol::table ValTable = kv.second.as<sol::table>();
						sol::optional<std::string> ModeOpt = ValTable.get<sol::optional<std::string>>("mode");
						if (ModeOpt.has_value())
						{
							bAdvanced = true;
							bGraphChanged = true; // Advanced modes modify graph nodes
							FString Mode = FString(UTF8_TO_TCHAR(ModeOpt.value().c_str())).ToLower();

							FNiagaraTypeDefinition InputType = MatchedInput->GetType();
							FNiagaraParameterHandle InputHandle(MatchedInput->GetName());
							FNiagaraParameterHandle AliasedHandle = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(InputHandle, ModNode);
							FNiagaraVariable AliasedVar(InputType, AliasedHandle.GetParameterHandleString());

							UEdGraphPin& OverridePin = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
								*ModNode, AliasedHandle, InputType, FGuid(), FGuid());

							// Static switch pins live on the FunctionCall node, not the ParameterMapSet.
							// DI/dynamic/linked/curve modes require ParameterMapSet — skip with error if static switch.
							if (!IsOverridePinOnParameterMapSet(OverridePin) && Mode != TEXT("static"))
							{
								// For static switch inputs, set the pin default value directly
								if (!ValTable.get_or<std::string>("value", "").empty())
								{
									OverridePin.DefaultValue = UTF8_TO_TCHAR(ValTable.get<std::string>("value").c_str());
									SetCount++;
								}
								else
								{
									FString DefaultHint = OverridePin.DefaultValue.IsEmpty() ? TEXT("(no default)") : OverridePin.DefaultValue;
								FString TypeHint = OverridePin.PinType.PinCategory.ToString();
								Errors.Add(FString::Printf(TEXT("'%s': parameter is a static switch (type=%s, current=%s) — use {value=\"...\"} instead of mode='%s'"), *ParamName, *TypeHint, *DefaultHint, *Mode));
								}
								continue;
							}

							if (Mode == TEXT("dynamic_input") || Mode == TEXT("dynamic"))
							{
								std::string ScriptPath = ValTable.get_or<std::string>("script", "");
								if (ScriptPath.empty()) { Errors.Add(FString::Printf(TEXT("'%s': dynamic_input needs 'script'"), *ParamName)); continue; }

								UNiagaraScript* DIScript = LoadObject<UNiagaraScript>(nullptr, *FString(UTF8_TO_TCHAR(ScriptPath.c_str())));
								if (!DIScript || DIScript->GetUsage() != ENiagaraScriptUsage::DynamicInput)
								{
									Errors.Add(FString::Printf(TEXT("'%s': DI script not found or wrong usage"), *ParamName));
									continue;
								}

								if (OverridePin.LinkedTo.Num() > 0) CleanupOverridePinNodes(OverridePin);
								if (Script) RemoveRapidIterationParam(System, *EmitterHandlePtr, Script, AliasedVar);
								OverridePin.DefaultValue = FString();

								UNiagaraNodeFunctionCall* DINode = nullptr;
								FNiagaraStackGraphUtilities::SetDynamicInputForFunctionInput(OverridePin, DIScript, DINode);
								if (DINode) SetCount++;
								else Errors.Add(FString::Printf(TEXT("'%s': SetDynamicInput failed"), *ParamName));
							}
							else if (Mode == TEXT("linked") || Mode == TEXT("link"))
							{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
								std::string LinkedParam = ValTable.get_or<std::string>("parameter", "");
								if (LinkedParam.empty()) { Errors.Add(FString::Printf(TEXT("'%s': linked needs 'parameter'"), *ParamName)); continue; }

								if (OverridePin.LinkedTo.Num() > 0) CleanupOverridePinNodes(OverridePin);
								if (Script) RemoveRapidIterationParam(System, *EmitterHandlePtr, Script, AliasedVar);
								OverridePin.DefaultValue = FString();

								FNiagaraVariable LinkedVar(InputType, FName(UTF8_TO_TCHAR(LinkedParam.c_str())));
								TArray<FNiagaraVariable> ExposedVars;
								System->GetExposedParameters().GetUserParameters(ExposedVars);
								TSet<FNiagaraVariableBase> KnownParameters;
								for (const FNiagaraVariable& V : ExposedVars) KnownParameters.Add(V);
								KnownParameters.Add(FNiagaraVariableBase(LinkedVar.GetType(), LinkedVar.GetName()));

								FNiagaraStackGraphUtilities::SetLinkedParameterValueForFunctionInput(
									OverridePin, LinkedVar, KnownParameters, ENiagaraDefaultMode::FailIfPreviouslyNotSet);

								// Engine parity: register parameter in graph's known parameter list
								UEdGraphNode* LinkedOverrideNode = OverridePin.GetOwningNode();
								if (LinkedOverrideNode)
								{
									UNiagaraGraph* LinkedGraph = Cast<UNiagaraGraph>(LinkedOverrideNode->GetGraph());
									if (LinkedGraph && !GraphHasVariable(LinkedGraph, LinkedVar))
									{
										LinkedGraph->Modify();
										GraphAddParameter(LinkedGraph, LinkedVar);
									}
								}
								SetCount++;
#else
								Errors.Add(FString::Printf(TEXT("'%s': linked mode requires UE 5.6+"), *ParamName));
#endif
							}
							else if (Mode == TEXT("hlsl") || Mode == TEXT("custom_hlsl") || Mode == TEXT("expression"))
							{
								std::string Code = ValTable.get_or<std::string>("code", "");
								if (Code.empty()) { Errors.Add(FString::Printf(TEXT("'%s': hlsl needs 'code'"), *ParamName)); continue; }

								if (OverridePin.LinkedTo.Num() > 0) CleanupOverridePinNodes(OverridePin);
								if (Script) RemoveRapidIterationParam(System, *EmitterHandlePtr, Script, AliasedVar);
								OverridePin.DefaultValue = FString();

								UEdGraphNode* OverrideNode = OverridePin.GetOwningNode();
								UEdGraph* Graph = OverrideNode ? OverrideNode->GetGraph() : nullptr;
								if (!Graph) { Errors.Add(FString::Printf(TEXT("'%s': no graph for HLSL node"), *ParamName)); continue; }
								Graph->Modify();

								// Engine parity: create custom HLSL dynamic input node
								FGraphNodeCreator<UNiagaraNodeCustomHlsl> NodeCreator(*Graph);
								UNiagaraNodeCustomHlsl* HlslNode = NodeCreator.CreateNode();
								InitCustomHlslAsDynamicInput(HlslNode, InputType);
								NodeCreator.Finalize();

								// Inherit enabled state from override node (engine: SetCustomExpressionForFunctionInput)
								HlslNode->SetEnabledState(OverrideNode->GetDesiredEnabledState(), OverrideNode->HasUserSetTheEnabledState());

								// Connect param map
								UEdGraphPin* HlslInputMap = GetParamMapInputPin(HlslNode);
								UEdGraphPin* OverrideNodeInputMap = GetParamMapInputPin(OverrideNode);
								if (HlslInputMap && OverrideNodeInputMap && OverrideNodeInputMap->LinkedTo.Num() > 0)
								{
									UEdGraphPin* PrevOut = OverrideNodeInputMap->LinkedTo[0];
									HlslInputMap->MakeLinkTo(PrevOut);
									HlslNode->PinConnectionListChanged(HlslInputMap);
									if (UEdGraphNode* PrevOwner = PrevOut->GetOwningNode()) PrevOwner->PinConnectionListChanged(PrevOut);
								}

								// Connect typed output to override pin
								FPinCollectorArray HlslOutputPins;
								HlslNode->GetOutputPins(HlslOutputPins);
								for (UEdGraphPin* OutPin : HlslOutputPins)
								{
									FNiagaraTypeDefinition PinType = GetDefault<UEdGraphSchema_Niagara>()->PinToTypeDefinition(OutPin);
									if (PinType != FNiagaraTypeDefinition::GetParameterMapDef())
									{
										OutPin->MakeLinkTo(&OverridePin);
										HlslNode->PinConnectionListChanged(OutPin);
										if (UEdGraphNode* OverrideOwner = OverridePin.GetOwningNode()) OverrideOwner->PinConnectionListChanged(&OverridePin);
										break;
									}
								}

								// Set HLSL code via reflection (SetCustomHlsl not exported from MinimalAPI class)
								SetCustomHlslText(HlslNode, UTF8_TO_TCHAR(Code.c_str()));
								SetCount++;
							}
							else if (Mode == TEXT("data_interface") || Mode == TEXT("di"))
							{
								std::string DIType = ValTable.get_or<std::string>("type", "");
								if (DIType.empty()) { Errors.Add(FString::Printf(TEXT("'%s': data_interface needs 'type'"), *ParamName)); continue; }

								if (!InputType.IsDataInterface())
								{
									Errors.Add(FString::Printf(TEXT("'%s': not a data interface type"), *ParamName));
									continue;
								}

								FString FullClassName = FString::Printf(TEXT("NiagaraDataInterface%s"), UTF8_TO_TCHAR(DIType.c_str()));
								UClass* DIClass = FindFirstObject<UClass>(*FullClassName, EFindFirstObjectOptions::None);
								if (!DIClass) DIClass = FindFirstObject<UClass>(*FString(UTF8_TO_TCHAR(DIType.c_str())), EFindFirstObjectOptions::None);
								if (!DIClass || !DIClass->IsChildOf(UNiagaraDataInterface::StaticClass()))
								{
									Errors.Add(FString::Printf(TEXT("'%s': DI class '%s' not found"), *ParamName, UTF8_TO_TCHAR(DIType.c_str())));
									continue;
								}

								if (OverridePin.LinkedTo.Num() > 0) CleanupOverridePinNodes(OverridePin);
								OverridePin.DefaultValue = FString();
								if (Script) RemoveRapidIterationParam(System, *EmitterHandlePtr, Script, AliasedVar);

								UNiagaraDataInterface* NewDI = nullptr;
								FNiagaraStackGraphUtilities::SetDataInterfaceValueForFunctionInput(
									OverridePin, DIClass, AliasedHandle.GetParameterHandleString().ToString(), NewDI);
								if (NewDI)
								{
									// Apply properties via reflection
									sol::optional<sol::table> DIProps = ValTable.get<sol::optional<sol::table>>("properties");
									if (DIProps.has_value())
									{
										for (auto& dp : DIProps.value())
										{
											if (!dp.first.is<std::string>()) continue;
											FString DPropName = UTF8_TO_TCHAR(dp.first.as<std::string>().c_str());
											FString Err;
											double NV = dp.second.is<double>() ? dp.second.as<double>() : 0.0;
											FString SV = dp.second.is<std::string>() ? UTF8_TO_TCHAR(dp.second.as<std::string>().c_str()) : TEXT("");
											bool BV = dp.second.is<bool>() ? dp.second.as<bool>() : false;
											if (!SetReflectedProperty(NewDI, DPropName, NV, SV, BV, dp.second.is<std::string>(), dp.second.is<bool>(), Err))
												Session.Log(FString::Printf(TEXT("[WARN] SetReflectedProperty('%s'): %s"), *DPropName, *Err));
										}
									}
									SetCount++;
								}
								else
								{
									Errors.Add(FString::Printf(TEXT("'%s': SetDataInterface failed"), *ParamName));
								}
							}
							else if (Mode == TEXT("curve"))
							{
								// Create a UNiagaraDataInterfaceCurve and populate with keys
								sol::optional<sol::table> KeysOpt = ValTable.get<sol::optional<sol::table>>("keys");
								if (!KeysOpt.has_value()) { Errors.Add(FString::Printf(TEXT("'%s': curve mode needs 'keys' array"), *ParamName)); continue; }

								if (OverridePin.LinkedTo.Num() > 0) CleanupOverridePinNodes(OverridePin);
								if (Script) RemoveRapidIterationParam(System, *EmitterHandlePtr, Script, AliasedVar);
								OverridePin.DefaultValue = FString();

								UNiagaraDataInterface* NewDI = nullptr;
								FNiagaraStackGraphUtilities::SetDataInterfaceValueForFunctionInput(
									OverridePin, UNiagaraDataInterfaceCurve::StaticClass(), AliasedHandle.GetParameterHandleString().ToString(), NewDI);
								if (NewDI)
								{
									UNiagaraDataInterfaceCurve* CurveDI = Cast<UNiagaraDataInterfaceCurve>(NewDI);
									if (CurveDI)
									{
										CurveDI->Curve.Reset();
										sol::table Keys = KeysOpt.value();
										for (auto& kp : Keys)
										{
											if (!kp.second.is<sol::table>()) continue;
											sol::table KeyEntry = kp.second.as<sol::table>();
											float Time = static_cast<float>(KeyEntry.get_or("time", 0.0));
											float Value = static_cast<float>(KeyEntry.get_or("value", 0.0));
											CurveDI->Curve.AddKey(Time, Value);
										}
										CurveDI->UpdateTimeRanges();
									}
									SetCount++;
								}
								else
								{
									Errors.Add(FString::Printf(TEXT("'%s': SetDataInterface (curve) failed"), *ParamName));
								}
							}
							else if (Mode == TEXT("color_curve"))
							{
								// Create a UNiagaraDataInterfaceColorCurve and populate R/G/B/A curves
								sol::optional<sol::table> KeysOpt = ValTable.get<sol::optional<sol::table>>("keys");
								if (!KeysOpt.has_value()) { Errors.Add(FString::Printf(TEXT("'%s': color_curve mode needs 'keys' array"), *ParamName)); continue; }

								if (OverridePin.LinkedTo.Num() > 0) CleanupOverridePinNodes(OverridePin);
								if (Script) RemoveRapidIterationParam(System, *EmitterHandlePtr, Script, AliasedVar);
								OverridePin.DefaultValue = FString();

								UNiagaraDataInterface* NewDI = nullptr;
								FNiagaraStackGraphUtilities::SetDataInterfaceValueForFunctionInput(
									OverridePin, UNiagaraDataInterfaceColorCurve::StaticClass(), AliasedHandle.GetParameterHandleString().ToString(), NewDI);
								if (NewDI)
								{
									UNiagaraDataInterfaceColorCurve* ColorDI = Cast<UNiagaraDataInterfaceColorCurve>(NewDI);
									if (ColorDI)
									{
										ColorDI->RedCurve.Reset();
										ColorDI->GreenCurve.Reset();
										ColorDI->BlueCurve.Reset();
										ColorDI->AlphaCurve.Reset();
										sol::table Keys = KeysOpt.value();
										for (auto& kp : Keys)
										{
											if (!kp.second.is<sol::table>()) continue;
											sol::table KeyEntry = kp.second.as<sol::table>();
											float Time = static_cast<float>(KeyEntry.get_or("time", 0.0));
											sol::optional<sol::table> ColorOpt = KeyEntry.get<sol::optional<sol::table>>("color");
											if (ColorOpt.has_value())
											{
												sol::table C = ColorOpt.value();
												ColorDI->RedCurve.AddKey(Time, static_cast<float>(C.get_or("r", 0.0)));
												ColorDI->GreenCurve.AddKey(Time, static_cast<float>(C.get_or("g", 0.0)));
												ColorDI->BlueCurve.AddKey(Time, static_cast<float>(C.get_or("b", 0.0)));
												ColorDI->AlphaCurve.AddKey(Time, static_cast<float>(C.get_or("a", 1.0)));
											}
											else
											{
												// Flat: r, g, b, a keys at top level
												ColorDI->RedCurve.AddKey(Time, static_cast<float>(KeyEntry.get_or("r", 0.0)));
												ColorDI->GreenCurve.AddKey(Time, static_cast<float>(KeyEntry.get_or("g", 0.0)));
												ColorDI->BlueCurve.AddKey(Time, static_cast<float>(KeyEntry.get_or("b", 0.0)));
												ColorDI->AlphaCurve.AddKey(Time, static_cast<float>(KeyEntry.get_or("a", 1.0)));
											}
										}
										ColorDI->UpdateTimeRanges();
									}
									SetCount++;
								}
								else
								{
									Errors.Add(FString::Printf(TEXT("'%s': SetDataInterface (color_curve) failed"), *ParamName));
								}
							}
							else if (Mode == TEXT("vector_curve"))
							{
								// Create a UNiagaraDataInterfaceVectorCurve and populate X/Y/Z curves
								sol::optional<sol::table> KeysOpt = ValTable.get<sol::optional<sol::table>>("keys");
								if (!KeysOpt.has_value()) { Errors.Add(FString::Printf(TEXT("'%s': vector_curve mode needs 'keys' array"), *ParamName)); continue; }

								if (OverridePin.LinkedTo.Num() > 0) CleanupOverridePinNodes(OverridePin);
								if (Script) RemoveRapidIterationParam(System, *EmitterHandlePtr, Script, AliasedVar);
								OverridePin.DefaultValue = FString();

								UNiagaraDataInterface* NewDI = nullptr;
								FNiagaraStackGraphUtilities::SetDataInterfaceValueForFunctionInput(
									OverridePin, UNiagaraDataInterfaceVectorCurve::StaticClass(), AliasedHandle.GetParameterHandleString().ToString(), NewDI);
								if (NewDI)
								{
									UNiagaraDataInterfaceVectorCurve* VecDI = Cast<UNiagaraDataInterfaceVectorCurve>(NewDI);
									if (VecDI)
									{
										VecDI->XCurve.Reset();
										VecDI->YCurve.Reset();
										VecDI->ZCurve.Reset();
										sol::table Keys = KeysOpt.value();
										for (auto& kp : Keys)
										{
											if (!kp.second.is<sol::table>()) continue;
											sol::table KeyEntry = kp.second.as<sol::table>();
											float Time = static_cast<float>(KeyEntry.get_or("time", 0.0));
											VecDI->XCurve.AddKey(Time, static_cast<float>(KeyEntry.get_or("x", 0.0)));
											VecDI->YCurve.AddKey(Time, static_cast<float>(KeyEntry.get_or("y", 0.0)));
											VecDI->ZCurve.AddKey(Time, static_cast<float>(KeyEntry.get_or("z", 0.0)));
										}
										VecDI->UpdateTimeRanges();
									}
									SetCount++;
								}
								else
								{
									Errors.Add(FString::Printf(TEXT("'%s': SetDataInterface (vector_curve) failed"), *ParamName));
								}
							}
							else if (Mode == TEXT("reset") || Mode == TEXT("default"))
							{
								if (OverridePin.LinkedTo.Num() > 0) CleanupOverridePinNodes(OverridePin);
								if (Script) RemoveRapidIterationParam(System, *EmitterHandlePtr, Script, AliasedVar);
								OverridePin.DefaultValue = FString();
								SetCount++;
							}
							else
							{
								Errors.Add(FString::Printf(TEXT("'%s': unknown mode '%s'"), *ParamName, *Mode));
							}
						}
					}

					// Static value mode (plain number, string, bool)
					if (!bAdvanced)
					{

						FNiagaraTypeDefinition InputType = MatchedInput->GetType();
						FNiagaraParameterHandle InputHandle(MatchedInput->GetName());
						FNiagaraParameterHandle AliasedHandle = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(InputHandle, ModNode);
						FNiagaraVariable AliasedVar(InputType, AliasedHandle.GetParameterHandleString());

						// Convert to the full RI constant name (e.g. "Constants.EmitterName.ModuleName.ParamName")
						// so we write to the EXISTING param in the store instead of creating a new one with the wrong key
						const TCHAR* EmNameForRI = nullptr;
						FString UniqueEmName;
						if (EmitterHandlePtr->IsValid() && EmitterHandlePtr->GetInstance().Emitter)
						{
							UniqueEmName = EmitterHandlePtr->GetInstance().Emitter->GetUniqueEmitterName();
							if (!UniqueEmName.IsEmpty()) EmNameForRI = *UniqueEmName;
						}

						// Write to RI params on all affected scripts
						TArray<UNiagaraScript*> AffectedScripts = FindAffectedScripts(System, *EmitterHandlePtr, Usage);

						// Helper lambda to set RI param with correct constant name per script.
						// bAdd=true so params that don't yet exist in the store (e.g. static-switch-gated
						// inputs like Lifetime behind Lifetime Mode) get created automatically.
						auto SetRIParam = [&](UNiagaraScript* AS, const auto& Val) -> bool
						{
							if (!AS || !System->ShouldUseRapidIterationParameters()) return false;
							FNiagaraVariable RIVar = FNiagaraUtilities::ConvertVariableToRapidIterationConstantName(
								AliasedVar, EmNameForRI, AS->GetUsage());
							AS->Modify();
							bool bWritten = AS->RapidIterationParameters.SetParameterValue(Val, RIVar, true /*bAdd*/);
							UE_LOG(LogTemp, Log, TEXT("[AIK-Niagara] SetRIParam: '%s' type='%s' bWritten=%d scriptUsage=%d numParams=%d"),
								*RIVar.GetName().ToString(), *RIVar.GetType().GetFName().ToString(), bWritten ? 1 : 0,
								(int32)AS->GetUsage(), AS->RapidIterationParameters.ReadParameterVariables().Num());
							return bWritten;
						};

						bool bSet = false;
						if (kv.second.is<double>() && InputType == FNiagaraTypeDefinition::GetFloatDef())
						{
							float Val = static_cast<float>(kv.second.as<double>());
							for (UNiagaraScript* AS : AffectedScripts) SetRIParam(AS, Val);
							bSet = true;
						}
						else if (kv.second.is<double>() && InputType == FNiagaraTypeDefinition::GetIntDef())
						{
							int32 Val = static_cast<int32>(kv.second.as<double>());
							for (UNiagaraScript* AS : AffectedScripts) SetRIParam(AS, Val);
							bSet = true;
						}
						else if (kv.second.is<bool>() && InputType == FNiagaraTypeDefinition::GetBoolDef())
						{
							FNiagaraBool Val;
							Val.SetValue(kv.second.as<bool>());
							for (UNiagaraScript* AS : AffectedScripts) SetRIParam(AS, Val);
							bSet = true;
						}
						else if (kv.second.is<sol::table>() && System->ShouldUseRapidIterationParameters())
						{
							// Vector/Color/Quat types from table values
							sol::table VT = kv.second.as<sol::table>();
							if (InputType == FNiagaraTypeDefinition::GetVec2Def())
							{
								FVector2f Val(static_cast<float>(VT.get_or("x", 0.0)), static_cast<float>(VT.get_or("y", 0.0)));
								for (UNiagaraScript* AS : AffectedScripts) SetRIParam(AS, Val);
								bSet = true;
							}
							else if (InputType == FNiagaraTypeDefinition::GetVec3Def())
							{
								FVector3f Val(static_cast<float>(VT.get_or("x", 0.0)), static_cast<float>(VT.get_or("y", 0.0)), static_cast<float>(VT.get_or("z", 0.0)));
								for (UNiagaraScript* AS : AffectedScripts) SetRIParam(AS, Val);
								bSet = true;
							}
							else if (InputType == FNiagaraTypeDefinition::GetVec4Def())
							{
								FVector4f Val(static_cast<float>(VT.get_or("x", 0.0)), static_cast<float>(VT.get_or("y", 0.0)), static_cast<float>(VT.get_or("z", 0.0)), static_cast<float>(VT.get_or("w", 0.0)));
								for (UNiagaraScript* AS : AffectedScripts) SetRIParam(AS, Val);
								bSet = true;
							}
							else if (InputType == FNiagaraTypeDefinition::GetColorDef())
							{
								FLinearColor Val(static_cast<float>(VT.get_or("r", 0.0)), static_cast<float>(VT.get_or("g", 0.0)), static_cast<float>(VT.get_or("b", 0.0)), static_cast<float>(VT.get_or("a", 1.0)));
								for (UNiagaraScript* AS : AffectedScripts) SetRIParam(AS, Val);
								bSet = true;
							}
							else if (InputType == FNiagaraTypeDefinition::GetQuatDef())
							{
								FQuat4f Val(static_cast<float>(VT.get_or("x", 0.0)), static_cast<float>(VT.get_or("y", 0.0)), static_cast<float>(VT.get_or("z", 0.0)), static_cast<float>(VT.get_or("w", 1.0)));
								for (UNiagaraScript* AS : AffectedScripts) SetRIParam(AS, Val);
								bSet = true;
							}
						}
						else if (!kv.second.is<sol::table>())
						{
							// Fall back to pin default value for non-RI types
							UEdGraphPin& OverridePin = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
								*ModNode, AliasedHandle, InputType, FGuid(), FGuid());
							if (kv.second.is<double>())
								OverridePin.DefaultValue = FString::SanitizeFloat(kv.second.as<double>());
							else if (kv.second.is<std::string>())
								OverridePin.DefaultValue = UTF8_TO_TCHAR(kv.second.as<std::string>().c_str());
							else if (kv.second.is<bool>())
								OverridePin.DefaultValue = kv.second.as<bool>() ? TEXT("true") : TEXT("false");
							if (UEdGraphNode* OwningNode = OverridePin.GetOwningNode())
								OwningNode->PinDefaultValueChanged(&OverridePin);
							bSet = true;
						}

						if (bSet) SetCount++;
						else Errors.Add(FString::Printf(TEXT("'%s': unsupported value type"), *ParamName));
					}
				}

				// Only notify graph changed for structural modifications (advanced modes like
				// dynamic_input, linked, hlsl, data_interface, curve). RI param writes must NOT
				// trigger graph recompile — the engine rebuilds the RI store from defaults on
				// recompile, which would overwrite our changes. (Matches engine behavior in
				// NiagaraStackFunctionInput.cpp which only calls NotifyGraphNeedsRecompile for
				// static/switch inputs, not regular RI params.)
				if (bGraphChanged)
				{
					UNiagaraGraph* ModGraph = Cast<UNiagaraGraph>(OutputNode->GetGraph());
					if (ModGraph) ModGraph->NotifyGraphChanged();
				}

				System->MarkPackageDirty();
				if (Errors.Num() > 0)
					Session.Log(FString::Printf(TEXT("[OK] configure(\"module\", \"%s\") -> %d set, errors: %s"), *ModName, SetCount, *FString::Join(Errors, TEXT("; "))));
				else
					Session.Log(FString::Printf(TEXT("[OK] configure(\"module\", \"%s\") -> %d set"), *ModName, SetCount));
				return sol::make_object(Lua, SetCount > 0);
			}

			// ---- configure("emitter_mode") ----
			else if (FType.Equals(TEXT("emitter_mode"), ESearchCase::IgnoreCase))
			{
				FString EmName = Id.is<std::string>() ? UTF8_TO_TCHAR(Id.as<std::string>().c_str()) : TEXT("");
				if (EmName.IsEmpty()) { Session.Log(TEXT("[FAIL] configure(\"emitter_mode\") -> emitter name required")); return sol::lua_nil; }
				int32 EmIdx = FindEmitterIndex(System, EmName);
				if (EmIdx == INDEX_NONE) { Session.Log(FString::Printf(TEXT("[FAIL] configure(\"emitter_mode\") -> emitter '%s' not found"), *EmName)); return sol::lua_nil; }

				std::string ModeStr = Params.get_or<std::string>("mode", "standard");
				FString FMode = UTF8_TO_TCHAR(ModeStr.c_str());

				ENiagaraEmitterMode TargetMode = ENiagaraEmitterMode::Standard;
				if (FMode.Contains(TEXT("stateless"), ESearchCase::IgnoreCase))
					TargetMode = ENiagaraEmitterMode::Stateless;

				FNiagaraEmitterHandle& Handle = System->GetEmitterHandle(EmIdx);
				if (Handle.GetEmitterMode() == TargetMode)
				{
					Session.Log(FString::Printf(TEXT("[OK] configure(\"emitter_mode\", \"%s\") -> already in '%s' mode"), *EmName, *FMode));
					return sol::make_object(Lua, true);
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "CfgNEmitterMode", "Configure Niagara Emitter Mode"));
				System->Modify();
				Handle.SetEmitterMode(*System, TargetMode);
				System->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"emitter_mode\", \"%s\", mode=\"%s\")"), *EmName, *FMode));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> unknown type. Valid: module, emitter, emitter_mode, system, renderer, reflected_properties"), *FType));
			return sol::lua_nil;
		});

		// ==================================================================
		// enable_module({emitter, stage, module_name, enabled})
		// ==================================================================
		AssetObj.set_function("enable_module", [System, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			std::string EmitterName = Params.get_or<std::string>("emitter", "");
			std::string StageStr = Params.get_or<std::string>("stage", "particle_update");
			std::string ModName = Params.get_or<std::string>("module_name", "");
			bool bEnabled = Params.get_or("enabled", true);
			WarnUnconsumedKeys(Session, Params, { TEXT("emitter"), TEXT("stage"), TEXT("module_name"), TEXT("enabled") }, TEXT("enable_module"));

			if (ModName.empty()) { Session.Log(TEXT("[FAIL] enable_module -> module_name required")); return sol::lua_nil; }

			ENiagaraScriptUsage Usage = ParseUsage(UTF8_TO_TCHAR(StageStr.c_str()));
			int32 EmIdx = INDEX_NONE;
			bool bSys = (Usage == ENiagaraScriptUsage::SystemSpawnScript || Usage == ENiagaraScriptUsage::SystemUpdateScript);
			if (!bSys) EmIdx = FindEmitterIndex(System, UTF8_TO_TCHAR(EmitterName.c_str()));

			UNiagaraNodeOutput* OutputNode = FindOutputNode(System, EmIdx, Usage);
			if (!OutputNode) { Session.Log(TEXT("[FAIL] enable_module -> no output node")); return sol::lua_nil; }

			UNiagaraNodeFunctionCall* ModNode = FindModuleByName(OutputNode, UTF8_TO_TCHAR(ModName.c_str()));
			if (!ModNode) { Session.Log(FString::Printf(TEXT("[FAIL] enable_module -> '%s' not found"), UTF8_TO_TCHAR(ModName.c_str()))); return sol::lua_nil; }

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "EnableNModule", "Enable Niagara Module"));
			ModNode->Modify();
			ModNode->SetEnabledState(bEnabled ? ENodeEnabledState::Enabled : ENodeEnabledState::Disabled, true);
			System->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] enable_module(\"%s\", %s)"), UTF8_TO_TCHAR(ModName.c_str()), bEnabled ? TEXT("true") : TEXT("false")));
			return sol::make_object(Lua, true);
		});

		// ==================================================================
		// rename_emitter(name, new_name) or rename_emitter({old_name=, new_name=})
		// ==================================================================
		AssetObj.set_function("rename_emitter", [System, &Session](sol::table /*self*/,
			sol::object Arg1, sol::optional<std::string> Arg2, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FName, FNewName;
			if (Arg1.is<sol::table>())
			{
				sol::table T = Arg1.as<sol::table>();
				std::string OldNameStr = T.get_or<std::string>("old_name", "");
				if (OldNameStr.empty()) OldNameStr = T.get_or<std::string>("name", "");
				FName = UTF8_TO_TCHAR(OldNameStr.c_str());
				FNewName = UTF8_TO_TCHAR(T.get_or<std::string>("new_name", "").c_str());
			}
			else if (Arg1.is<std::string>())
			{
				FName = UTF8_TO_TCHAR(Arg1.as<std::string>().c_str());
				FNewName = Arg2.has_value() ? UTF8_TO_TCHAR(Arg2.value().c_str()) : TEXT("");
			}
			if (FName.IsEmpty() || FNewName.IsEmpty()) { Session.Log(TEXT("[FAIL] rename_emitter -> old_name and new_name required")); return sol::lua_nil; }
			int32 EmIdx = FindEmitterIndex(System, FName);
			if (EmIdx == INDEX_NONE) { Session.Log(FString::Printf(TEXT("[FAIL] rename_emitter -> '%s' not found"), *FName)); return sol::lua_nil; }

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "RenameNEmitter", "Rename Niagara Emitter"));
			System->Modify();
			System->GetEmitterHandle(EmIdx).SetName(::FName(*FNewName), *System);
			System->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] rename_emitter(\"%s\", \"%s\")"), *FName, *FNewName));
			return sol::make_object(Lua, true);
		});

		// ==================================================================
		// duplicate_emitter(name, new_name?) or duplicate_emitter({name=, new_name=})
		// ==================================================================
		AssetObj.set_function("duplicate_emitter", [System, &Session](sol::table /*self*/,
			sol::object Arg1, sol::optional<std::string> Arg2, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FName, FNewName;
			if (Arg1.is<sol::table>())
			{
				sol::table T = Arg1.as<sol::table>();
				FName = UTF8_TO_TCHAR(T.get_or<std::string>("name", "").c_str());
				FNewName = UTF8_TO_TCHAR(T.get_or<std::string>("new_name", "").c_str());
			}
			else if (Arg1.is<std::string>())
			{
				FName = UTF8_TO_TCHAR(Arg1.as<std::string>().c_str());
				if (Arg2.has_value()) FNewName = UTF8_TO_TCHAR(Arg2.value().c_str());
			}
			if (FName.IsEmpty()) { Session.Log(TEXT("[FAIL] duplicate_emitter -> name required")); return sol::lua_nil; }
			int32 EmIdx = FindEmitterIndex(System, FName);
			if (EmIdx == INDEX_NONE) { Session.Log(FString::Printf(TEXT("[FAIL] duplicate_emitter -> '%s' not found"), *FName)); return sol::lua_nil; }

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "DupNEmitter", "Duplicate Niagara Emitter"));
			System->Modify();

			const FNiagaraEmitterHandle& SrcHandle = System->GetEmitterHandle(EmIdx);
			UNiagaraEmitter* SrcEmitter = SrcHandle.GetInstance().Emitter;
			if (!SrcEmitter)
			{
				Session.Log(TEXT("[FAIL] duplicate_emitter -> source emitter data unavailable"));
				return sol::lua_nil;
			}

			// Generate a name if not provided — DuplicateEmitterHandle uses it for the emitter's unique name.
			// Passing FName() results in "None" which is not useful.
			if (FNewName.IsEmpty())
			{
				// Try "Fountain_Copy", then "Fountain_Copy_01", etc.
				FNewName = FName + TEXT("_Copy");
				int32 Suffix = 1;
				while (FindEmitterIndex(System, FNewName) != INDEX_NONE)
				{
					FNewName = FString::Printf(TEXT("%s_Copy_%02d"), *FName, Suffix++);
				}
			}
			FNiagaraEmitterHandle NewHandle = System->DuplicateEmitterHandle(SrcHandle, ::FName(*FNewName));
			// DuplicateEmitterHandle returns by value — find the actual handle in the system to set its name
			FGuid NewId = NewHandle.GetId();
			for (int32 i = 0; i < System->GetNumEmitters(); ++i)
			{
				if (System->GetEmitterHandle(i).GetId() == NewId)
				{
					System->GetEmitterHandle(i).SetName(::FName(*FNewName), *System);
					break;
				}
			}

			System->RequestCompile(false);
			System->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] duplicate_emitter(\"%s\") -> %d emitters total"), *FName, System->GetNumEmitters()));
			return sol::make_object(Lua, true);
		});

		// ==================================================================
		// reorder_emitter(name, new_index)
		// ==================================================================
		AssetObj.set_function("reorder_emitter", [System, &Session](sol::table /*self*/,
			const std::string& Name, int NewIndex, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FName = UTF8_TO_TCHAR(Name.c_str());
			int32 EmIdx = FindEmitterIndex(System, FName);
			if (EmIdx == INDEX_NONE) { Session.Log(FString::Printf(TEXT("[FAIL] reorder_emitter -> '%s' not found"), *FName)); return sol::lua_nil; }

			// UNiagaraSystem doesn't expose a public move/reorder API in 5.7
			Session.Log(FString::Printf(TEXT("[FAIL] reorder_emitter -> not supported in this engine version. Remove and re-add emitters in desired order.")));
			return sol::lua_nil;
		});

		// ==================================================================
		// validate()
		// ==================================================================
		AssetObj.set_function("validate", [System, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			sol::table Result = Lua.create_table();

			UNiagaraEffectType* EffectType = System->GetEffectType();
			int32 RuleCount = 0;
			if (EffectType)
			{
				RuleCount = EffectType->ValidationRules.Num();
			}

			Result["effect_type"] = EffectType ? TCHAR_TO_UTF8(*EffectType->GetName()) : "none";
			Result["rule_count"] = RuleCount;
			Result["emitters"] = static_cast<int>(System->GetEmitterHandles().Num());
			Session.Log(FString::Printf(TEXT("[OK] validate() -> %d rules, %d emitters"), RuleCount, System->GetEmitterHandles().Num()));
			return Result;
		});

		// ==================================================================
		// info()
		// ==================================================================
		AssetObj.set_function("info", [System, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			sol::table Result = Lua.create_table();

			Result["emitters"] = System->GetNumEmitters();

			int32 TotalModules = 0;
			int32 TotalRenderers = 0;
			int32 TotalEventHandlers = 0;
			int32 TotalSimStages = 0;

			for (int32 i = 0; i < System->GetNumEmitters(); i++)
			{
				const FNiagaraEmitterHandle& Handle = System->GetEmitterHandle(i);
				FVersionedNiagaraEmitterData* ED = Handle.GetEmitterData();
				if (ED)
				{
					TotalRenderers += ED->GetRenderers().Num();
					TotalEventHandlers += ED->GetEventHandlers().Num();
					TotalSimStages += ED->GetSimulationStages().Num();
				}

				// Count modules across common stages
				ENiagaraScriptUsage Usages[] = {
					ENiagaraScriptUsage::ParticleSpawnScript,
					ENiagaraScriptUsage::ParticleUpdateScript,
					ENiagaraScriptUsage::EmitterSpawnScript,
					ENiagaraScriptUsage::EmitterUpdateScript
				};
				for (ENiagaraScriptUsage U : Usages)
				{
					UNiagaraNodeOutput* Out = FindOutputNode(System, i, U, FGuid(), false);
					if (Out) TotalModules += ListModulesInStack(Out).Num();
				}
			}

			// System-level modules
			UNiagaraNodeOutput* SysSpawn = FindOutputNode(System, INDEX_NONE, ENiagaraScriptUsage::SystemSpawnScript, FGuid(), false);
			UNiagaraNodeOutput* SysUpdate = FindOutputNode(System, INDEX_NONE, ENiagaraScriptUsage::SystemUpdateScript, FGuid(), false);
			if (SysSpawn) TotalModules += ListModulesInStack(SysSpawn).Num();
			if (SysUpdate) TotalModules += ListModulesInStack(SysUpdate).Num();

			FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();
			TArray<FNiagaraVariable> UserParams;
			Store.GetUserParameters(UserParams);

			Result["total_modules"] = TotalModules;
			Result["total_renderers"] = TotalRenderers;
			Result["total_event_handlers"] = TotalEventHandlers;
			Result["total_simulation_stages"] = TotalSimStages;
			Result["user_parameters"] = UserParams.Num();

			UNiagaraEffectType* ET = System->GetEffectType();
			if (ET) Result["effect_type"] = TCHAR_TO_UTF8(*ET->GetName());

			// Performance flags
			Result["bake_out_rapid_iteration"] = !System->ShouldUseRapidIterationParameters();
			Result["trim_attributes"] = System->ShouldTrimAttributes();
			Result["compress_attributes"] = System->ShouldCompressAttributes();

			Session.Log(FString::Printf(TEXT("[OK] info() -> %d emitters, %d modules, %d renderers, %d user params"),
				System->GetNumEmitters(), TotalModules, TotalRenderers, UserParams.Num()));
			return Result;
		});

		// ==================================================================
		// move_module({emitter, stage, module_name, target_emitter?, target_stage?, target_index, copy?})
		// ==================================================================
		AssetObj.set_function("move_module", [System, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			std::string EmitterName = Params.get_or<std::string>("emitter", "");
			std::string StageStr = Params.get_or<std::string>("stage", "particle_update");
			std::string ModName = Params.get_or<std::string>("module_name", "");
			std::string TargetEmitter = Params.get_or<std::string>("target_emitter", EmitterName);
			std::string TargetStage = Params.get_or<std::string>("target_stage", StageStr);
			int32 TargetIndex = Params.get_or("target_index", -1);
			if (TargetIndex < 0) TargetIndex = Params.get_or("new_index", -1);
			bool bCopy = Params.get_or("copy", false);
			WarnUnconsumedKeys(Session, Params, { TEXT("emitter"), TEXT("stage"), TEXT("module_name"), TEXT("target_emitter"), TEXT("target_stage"), TEXT("target_index"), TEXT("new_index"), TEXT("copy") }, TEXT("move_module"));

			if (ModName.empty()) { Session.Log(TEXT("[FAIL] move_module -> module_name required")); return sol::lua_nil; }

			ENiagaraScriptUsage SrcUsage = ParseUsage(UTF8_TO_TCHAR(StageStr.c_str()));
			ENiagaraScriptUsage DstUsage = ParseUsage(UTF8_TO_TCHAR(TargetStage.c_str()));

			bool bSrcSys = (SrcUsage == ENiagaraScriptUsage::SystemSpawnScript || SrcUsage == ENiagaraScriptUsage::SystemUpdateScript);
			bool bDstSys = (DstUsage == ENiagaraScriptUsage::SystemSpawnScript || DstUsage == ENiagaraScriptUsage::SystemUpdateScript);

			int32 SrcEmIdx = INDEX_NONE, DstEmIdx = INDEX_NONE;
			if (!bSrcSys)
			{
				SrcEmIdx = FindEmitterIndex(System, UTF8_TO_TCHAR(EmitterName.c_str()));
				if (SrcEmIdx == INDEX_NONE) { Session.Log(FString::Printf(TEXT("[FAIL] move_module -> source emitter '%s' not found"), UTF8_TO_TCHAR(EmitterName.c_str()))); return sol::lua_nil; }
			}
			if (!bDstSys)
			{
				DstEmIdx = FindEmitterIndex(System, UTF8_TO_TCHAR(TargetEmitter.c_str()));
				if (DstEmIdx == INDEX_NONE) { Session.Log(FString::Printf(TEXT("[FAIL] move_module -> target emitter '%s' not found"), UTF8_TO_TCHAR(TargetEmitter.c_str()))); return sol::lua_nil; }
			}

			UNiagaraNodeOutput* SrcOutput = FindOutputNode(System, SrcEmIdx, SrcUsage);
			UNiagaraNodeOutput* DstOutput = FindOutputNode(System, DstEmIdx, DstUsage);
			if (!SrcOutput || !DstOutput) { Session.Log(TEXT("[FAIL] move_module -> could not resolve output nodes")); return sol::lua_nil; }

			FString FModName = UTF8_TO_TCHAR(ModName.c_str());
			UNiagaraNodeFunctionCall* ModNode = FindModuleByName(SrcOutput, FModName);
			if (!ModNode) { Session.Log(FString::Printf(TEXT("[FAIL] move_module -> '%s' not found"), *FModName)); return sol::lua_nil; }
			if (!ModNode->FunctionScript) { Session.Log(TEXT("[FAIL] move_module -> module has no function script")); return sol::lua_nil; }

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "MoveNModule", "Move Niagara Module"));

			// Save module info before potential removal
			UNiagaraScript* FuncScript = ModNode->FunctionScript;
			FString FuncName = ModNode->GetFunctionName();
			FGuid ScriptVersion = ModNode->SelectedScriptVersion;

			// For same-stack moves (not copy), remove the original FIRST to avoid name collision ("001" suffix)
			bool bSameStack = (SrcOutput == DstOutput) && !bCopy;
			if (bSameStack)
			{
				UEdGraph* Graph = ModNode->GetGraph();
				if (Graph) Graph->Modify();
				UEdGraphPin* ModMapIn = GetParamMapInputPin(ModNode);
				UEdGraphPin* ModMapOut = GetParamMapOutputPin(ModNode);
				UEdGraphPin* UpstreamOut = (ModMapIn && ModMapIn->LinkedTo.Num() > 0) ? ModMapIn->LinkedTo[0] : nullptr;
				TArray<UEdGraphPin*> DownstreamIns = ModMapOut ? ModMapOut->LinkedTo : TArray<UEdGraphPin*>();
				if (ModMapIn) ModMapIn->BreakAllPinLinks(true);
				if (ModMapOut) ModMapOut->BreakAllPinLinks(true);
				if (UpstreamOut)
				{
					for (UEdGraphPin* DownIn : DownstreamIns)
					{
						if (DownIn)
						{
							UpstreamOut->MakeLinkTo(DownIn);
							if (UEdGraphNode* UpOwner = UpstreamOut->GetOwningNode()) UpOwner->PinConnectionListChanged(UpstreamOut);
							if (UEdGraphNode* DownOwner = DownIn->GetOwningNode()) DownOwner->PinConnectionListChanged(DownIn);
						}
					}
				}
				TArray<UEdGraphNode*> NodesToRemove;
				TSet<UEdGraphNode*> Visited;
				CollectSubGraphNodes(ModNode, NodesToRemove, Visited);
				for (UEdGraphNode* N : NodesToRemove) { if (N && Graph) { Graph->RemoveNode(N); } }
				if (Graph) Graph->NotifyGraphChanged();
			}

			UNiagaraNodeFunctionCall* CopiedNode = FNiagaraStackGraphUtilities::AddScriptModuleToStack(
				FuncScript, *DstOutput, TargetIndex, FuncName, ScriptVersion);
			if (!CopiedNode) { Session.Log(TEXT("[FAIL] move_module -> failed to add module at target")); return sol::lua_nil; }

			if (!bCopy && !bSameStack)
			{
				// Cross-stack move: remove original after successful add
				UEdGraph* Graph = ModNode->GetGraph();
				if (Graph) Graph->Modify();
				UEdGraphPin* ModMapIn = GetParamMapInputPin(ModNode);
				UEdGraphPin* ModMapOut = GetParamMapOutputPin(ModNode);
				UEdGraphPin* UpstreamOut = (ModMapIn && ModMapIn->LinkedTo.Num() > 0) ? ModMapIn->LinkedTo[0] : nullptr;
				TArray<UEdGraphPin*> DownstreamIns = ModMapOut ? ModMapOut->LinkedTo : TArray<UEdGraphPin*>();
				if (ModMapIn) ModMapIn->BreakAllPinLinks(true);
				if (ModMapOut) ModMapOut->BreakAllPinLinks(true);
				if (UpstreamOut)
				{
					for (UEdGraphPin* DownIn : DownstreamIns)
					{
						if (DownIn)
						{
							UpstreamOut->MakeLinkTo(DownIn);
							if (UEdGraphNode* UpOwner = UpstreamOut->GetOwningNode()) UpOwner->PinConnectionListChanged(UpstreamOut);
							if (UEdGraphNode* DownOwner = DownIn->GetOwningNode()) DownOwner->PinConnectionListChanged(DownIn);
						}
					}
				}
				TArray<UEdGraphNode*> NodesToRemove;
				TSet<UEdGraphNode*> Visited;
				CollectSubGraphNodes(ModNode, NodesToRemove, Visited);
				for (UEdGraphNode* N : NodesToRemove) { if (N && Graph) { Graph->RemoveNode(N); } }
				if (Graph) Graph->NotifyGraphChanged();
			}

			System->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] %s(\"%s\") -> %s/%s"),
				bCopy ? TEXT("copy_module") : TEXT("move_module"), *FModName,
				bDstSys ? TEXT("System") : UTF8_TO_TCHAR(TargetEmitter.c_str()),
				*UsageToStr(DstUsage)));
			return sol::make_object(Lua, true);
		});

		// ==================================================================
		// create_scratch_pad_script({name, script_type="module"|"dynamic_input", duplicate_from?})
		// ==================================================================
		AssetObj.set_function("create_scratch_pad_script", [System, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			std::string Name = Params.get_or<std::string>("name", "");
			std::string ScriptType = Params.get_or<std::string>("script_type", "module");
			std::string DuplicateFrom = Params.get_or<std::string>("duplicate_from", "");

			if (Name.empty()) { Session.Log(TEXT("[FAIL] create_scratch_pad_script -> name required")); return sol::lua_nil; }

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "CreateNScratch", "Create Niagara Scratch Pad Script"));
			System->Modify();

			FString FName = UTF8_TO_TCHAR(Name.c_str());
			UNiagaraScript* NewScript = nullptr;

			if (!DuplicateFrom.empty())
			{
				UNiagaraScript* ExistingScript = LoadObject<UNiagaraScript>(nullptr, *FString(UTF8_TO_TCHAR(DuplicateFrom.c_str())));
				if (!ExistingScript)
					ExistingScript = FindScratchPadScript(System, UTF8_TO_TCHAR(DuplicateFrom.c_str()));
				if (!ExistingScript) { Session.Log(FString::Printf(TEXT("[FAIL] create_scratch_pad_script -> duplicate_from '%s' not found"), UTF8_TO_TCHAR(DuplicateFrom.c_str()))); return sol::lua_nil; }
				NewScript = Cast<UNiagaraScript>(StaticDuplicateObject(ExistingScript, System, ::FName(*FName)));
			}
			else
			{
				NewScript = NewObject<UNiagaraScript>(System, ::FName(*FName), RF_Transactional);
				if (!NewScript) { Session.Log(TEXT("[FAIL] create_scratch_pad_script -> allocation failed")); return sol::lua_nil; }

				FString FScriptType = FString(UTF8_TO_TCHAR(ScriptType.c_str())).ToLower();
				ENiagaraScriptUsage Usage = (FScriptType == TEXT("dynamic_input")) ? ENiagaraScriptUsage::DynamicInput : ENiagaraScriptUsage::Module;
				NewScript->SetUsage(Usage);

				UNiagaraScriptSource* Source = NewObject<UNiagaraScriptSource>(NewScript, NAME_None, RF_Transactional);
				UNiagaraGraph* Graph = Source ? NewObject<UNiagaraGraph>(Source, NAME_None, RF_Transactional) : nullptr;
				if (!Source || !Graph) { Session.Log(TEXT("[FAIL] create_scratch_pad_script -> graph init failed")); return sol::lua_nil; }
				Source->NodeGraph = Graph;

				const UEdGraphSchema_Niagara* Schema = Cast<UEdGraphSchema_Niagara>(Graph->GetSchema());
				if (!Schema) { Session.Log(TEXT("[FAIL] create_scratch_pad_script -> no schema")); return sol::lua_nil; }

				FGraphNodeCreator<UNiagaraNodeOutput> OutputCreator(*Graph);
				UNiagaraNodeOutput* OutputNode = OutputCreator.CreateNode();
				OutputNode->SetUsage(Usage);
				FGraphNodeCreator<UNiagaraNodeInput> InputCreator(*Graph);
				UNiagaraNodeInput* InputNode = InputCreator.CreateNode();
				InputNode->Usage = ENiagaraInputNodeUsage::Parameter;
				InputNode->Input = FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("MapIn"));

				if (Usage == ENiagaraScriptUsage::DynamicInput)
					OutputNode->Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("Output")));
				else
					OutputNode->Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("Output")));

				OutputCreator.Finalize();
				InputCreator.Finalize();
				Schema->TryCreateConnection(InputNode->GetOutputPin(0), OutputNode->GetInputPin(0));

				NewScript->SetLatestSource(Source);
				NewScript->RequestCompile(FGuid());
			}

			if (!NewScript) { Session.Log(TEXT("[FAIL] create_scratch_pad_script -> creation failed")); return sol::lua_nil; }
			NewScript->ClearFlags(RF_Public | RF_Standalone);
			System->ScratchPadScripts.AddUnique(NewScript);
			System->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] create_scratch_pad_script(\"%s\", type=\"%s\")"), *FName, UTF8_TO_TCHAR(ScriptType.c_str())));
			return sol::make_object(Lua, true);
		});

		// ==================================================================
		// delete_scratch_pad_script(name)
		// ==================================================================
		AssetObj.set_function("delete_scratch_pad_script", [System, &Session](sol::table /*self*/,
			const std::string& Name, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FName = UTF8_TO_TCHAR(Name.c_str());

			for (int32 i = 0; i < System->ScratchPadScripts.Num(); ++i)
			{
				UNiagaraScript* Script = System->ScratchPadScripts[i];
				if (Script && Script->GetName().Equals(FName, ESearchCase::IgnoreCase))
				{
					const FScopedTransaction Tx(NSLOCTEXT("AIK", "DelNScratch", "Delete Niagara Scratch Pad Script"));
					System->Modify();
					System->ScratchPadScripts.RemoveAt(i);
					System->MarkPackageDirty();
					Session.Log(FString::Printf(TEXT("[OK] delete_scratch_pad_script(\"%s\")"), *FName));
					return sol::make_object(Lua, true);
				}
			}
			Session.Log(FString::Printf(TEXT("[FAIL] delete_scratch_pad_script -> '%s' not found"), *FName));
			return sol::lua_nil;
		});

		// ==================================================================
		// rename_scratch_pad_script(name, new_name)
		// ==================================================================
		AssetObj.set_function("rename_scratch_pad_script", [System, &Session](sol::table /*self*/,
			const std::string& Name, const std::string& NewName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FName = UTF8_TO_TCHAR(Name.c_str());
			FString FNewName = UTF8_TO_TCHAR(NewName.c_str());

			for (UNiagaraScript* Script : System->ScratchPadScripts)
			{
				if (Script && Script->GetName().Equals(FName, ESearchCase::IgnoreCase))
				{
					const FScopedTransaction Tx(NSLOCTEXT("AIK", "RenNScratch", "Rename Niagara Scratch Pad Script"));
					System->Modify();
					Script->Rename(*FNewName, System, REN_DontCreateRedirectors);
					System->MarkPackageDirty();
					Session.Log(FString::Printf(TEXT("[OK] rename_scratch_pad_script(\"%s\", \"%s\")"), *FName, *FNewName));
					return sol::make_object(Lua, true);
				}
			}
			Session.Log(FString::Printf(TEXT("[FAIL] rename_scratch_pad_script -> '%s' not found"), *FName));
			return sol::lua_nil;
		});

		// ==================================================================
		// reorder_renderers({emitter, index, new_index})
		// ==================================================================
		AssetObj.set_function("reorder_renderers", [System, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			std::string EmitterName = Params.get<sol::optional<std::string>>("emitter").value_or("");
			int32 OldIdx = Params.get<sol::optional<int>>("index").value_or(-1);
			int32 NewIdx = Params.get<sol::optional<int>>("new_index").value_or(-1);
			if (OldIdx < 0 || NewIdx < 0) { Session.Log(TEXT("[FAIL] reorder_renderers -> index and new_index required")); return sol::lua_nil; }

			FString FEmitter = UTF8_TO_TCHAR(EmitterName.c_str());
			int32 EmIdx = FindEmitterIndex(System, FEmitter);
			if (EmIdx == INDEX_NONE) { Session.Log(FString::Printf(TEXT("[FAIL] reorder_renderers -> emitter '%s' not found"), *FEmitter)); return sol::lua_nil; }

			FNiagaraEmitterHandle& Handle = System->GetEmitterHandle(EmIdx);
			FVersionedNiagaraEmitterData* ED = Handle.GetEmitterData();
			UNiagaraEmitter* Emitter = Handle.GetInstance().Emitter;
			if (!ED || !Emitter) { Session.Log(TEXT("[FAIL] reorder_renderers -> no emitter data")); return sol::lua_nil; }

			const TArray<UNiagaraRendererProperties*>& Renderers = ED->GetRenderers();
			if (OldIdx >= Renderers.Num() || NewIdx >= Renderers.Num()) { Session.Log(TEXT("[FAIL] reorder_renderers -> index out of range")); return sol::lua_nil; }

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "ReorderNRenderers", "Reorder Niagara Renderers"));
			System->Modify();
			Emitter->Modify();
			Emitter->MoveRenderer(Renderers[OldIdx], NewIdx, Handle.GetInstance().Version);
			System->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] reorder_renderers(emitter=\"%s\", %d -> %d)"), *FEmitter, OldIdx, NewIdx));
			return sol::make_object(Lua, true);
		});

		// ==================================================================
		// reorder_simulation_stages({emitter, index, new_index})
		// ==================================================================
		AssetObj.set_function("reorder_simulation_stages", [System, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			std::string EmitterName = Params.get<sol::optional<std::string>>("emitter").value_or("");
			int32 OldIdx = Params.get<sol::optional<int>>("index").value_or(-1);
			int32 NewIdx = Params.get<sol::optional<int>>("new_index").value_or(-1);
			if (OldIdx < 0 || NewIdx < 0) { Session.Log(TEXT("[FAIL] reorder_simulation_stages -> index and new_index required")); return sol::lua_nil; }

			FString FEmitter = UTF8_TO_TCHAR(EmitterName.c_str());
			int32 EmIdx = FindEmitterIndex(System, FEmitter);
			if (EmIdx == INDEX_NONE) { Session.Log(FString::Printf(TEXT("[FAIL] reorder_simulation_stages -> emitter '%s' not found"), *FEmitter)); return sol::lua_nil; }

			FNiagaraEmitterHandle& Handle = System->GetEmitterHandle(EmIdx);
			FVersionedNiagaraEmitterData* ED = Handle.GetEmitterData();
			UNiagaraEmitter* Emitter = Handle.GetInstance().Emitter;
			if (!ED || !Emitter) { Session.Log(TEXT("[FAIL] reorder_simulation_stages -> no emitter data")); return sol::lua_nil; }

			const TArray<UNiagaraSimulationStageBase*>& Stages = ED->GetSimulationStages();
			if (OldIdx >= Stages.Num() || NewIdx >= Stages.Num()) { Session.Log(TEXT("[FAIL] reorder_simulation_stages -> index out of range")); return sol::lua_nil; }

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "ReorderNSimStages", "Reorder Niagara Simulation Stages"));
			System->Modify();
			Emitter->Modify();
			Emitter->MoveSimulationStageToIndex(Stages[OldIdx], NewIdx, Handle.GetInstance().Version);
			System->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] reorder_simulation_stages(emitter=\"%s\", %d -> %d)"), *FEmitter, OldIdx, NewIdx));
			return sol::make_object(Lua, true);
		});

		// ==================================================================
		// set_user_parameter({name, value})
		// ==================================================================
		AssetObj.set_function("set_user_parameter", [System, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			std::string ParamName = Params.get<sol::optional<std::string>>("name").value_or("");
			if (ParamName.empty()) { Session.Log(TEXT("[FAIL] set_user_parameter -> name required")); return sol::lua_nil; }

			FString FParamName = FString(UTF8_TO_TCHAR(ParamName.c_str()));
			FString FullName = TEXT("User.") + FParamName;
			FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();
			TArray<FNiagaraVariable> AllParams;
			Store.GetUserParameters(AllParams);

			// GetUserParameters() returns redirect-map KEYS (short names without "User." prefix).
			// Match against both the short name and the full "User." prefixed name.
			FNiagaraVariable* Found = nullptr;
			for (FNiagaraVariable& V : AllParams)
			{
				FString VName = V.GetName().ToString();
				if (VName.Equals(FParamName, ESearchCase::IgnoreCase) || VName.Equals(FullName, ESearchCase::IgnoreCase))
				{
					Found = &V;
					break;
				}
			}
			if (!Found) { Session.Log(FString::Printf(TEXT("[FAIL] set_user_parameter -> '%s' not found"), *FParamName)); return sol::lua_nil; }

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "SetNUserParam", "Set Niagara User Parameter"));
			System->Modify();

			sol::object Val = Params["value"];
			if (Val.is<double>() && Found->GetType() == FNiagaraTypeDefinition::GetFloatDef())
			{
				float FVal = static_cast<float>(Val.as<double>());
				Store.SetParameterValue(FVal, *Found);
			}
			else if (Val.is<double>() && Found->GetType() == FNiagaraTypeDefinition::GetIntDef())
			{
				int32 IVal = static_cast<int32>(Val.as<double>());
				Store.SetParameterValue(IVal, *Found);
			}
			else if (Val.is<bool>() && Found->GetType() == FNiagaraTypeDefinition::GetBoolDef())
			{
				FNiagaraBool BVal;
				BVal.SetValue(Val.as<bool>());
				Store.SetParameterValue(BVal, *Found);
			}
			else if (Val.is<sol::table>())
			{
				sol::table VT = Val.as<sol::table>();
				FNiagaraTypeDefinition VType = Found->GetType();

				if (VType == FNiagaraTypeDefinition::GetVec2Def())
				{
					FVector2f V2(
						static_cast<float>(VT.get_or("x", 0.0)),
						static_cast<float>(VT.get_or("y", 0.0)));
					Store.SetParameterValue(V2, *Found);
				}
				else if (VType == FNiagaraTypeDefinition::GetVec3Def())
				{
					FVector3f V3(
						static_cast<float>(VT.get_or("x", 0.0)),
						static_cast<float>(VT.get_or("y", 0.0)),
						static_cast<float>(VT.get_or("z", 0.0)));
					Store.SetParameterValue(V3, *Found);
				}
				else if (VType == FNiagaraTypeDefinition::GetVec4Def())
				{
					FVector4f V4(
						static_cast<float>(VT.get_or("x", 0.0)),
						static_cast<float>(VT.get_or("y", 0.0)),
						static_cast<float>(VT.get_or("z", 0.0)),
						static_cast<float>(VT.get_or("w", 0.0)));
					Store.SetParameterValue(V4, *Found);
				}
				else if (VType == FNiagaraTypeDefinition::GetColorDef())
				{
					FLinearColor Color(
						static_cast<float>(VT.get_or("r", 0.0)),
						static_cast<float>(VT.get_or("g", 0.0)),
						static_cast<float>(VT.get_or("b", 0.0)),
						static_cast<float>(VT.get_or("a", 1.0)));
					Store.SetParameterValue(Color, *Found);
				}
				else if (VType == FNiagaraTypeDefinition::GetQuatDef())
				{
					FQuat4f Quat(
						static_cast<float>(VT.get_or("x", 0.0)),
						static_cast<float>(VT.get_or("y", 0.0)),
						static_cast<float>(VT.get_or("z", 0.0)),
						static_cast<float>(VT.get_or("w", 1.0)));
					Store.SetParameterValue(Quat, *Found);
				}
				else
				{
					Session.Log(FString::Printf(TEXT("[FAIL] set_user_parameter -> unsupported table value type '%s' for '%s'"),
						*VType.GetName(), UTF8_TO_TCHAR(ParamName.c_str())));
					return sol::lua_nil;
				}
			}
			else
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_user_parameter -> unsupported value type for '%s' (type: %s). Use number for Float/Int, bool for Bool, table {x,y,z,...} for Vector/Color/Quat"),
					UTF8_TO_TCHAR(ParamName.c_str()), *Found->GetType().GetName()));
				return sol::lua_nil;
			}

			System->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] set_user_parameter(\"%s\")"), UTF8_TO_TCHAR(ParamName.c_str())));
			return sol::make_object(Lua, true);
		});

		// ==================================================================
		// configure_event_handler({emitter, index, ...props})
		// ==================================================================
		AssetObj.set_function("configure_event_handler", [System, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			std::string EmitterName = Params.get<sol::optional<std::string>>("emitter").value_or("");
			int32 Index = Params.get<sol::optional<int>>("index").value_or(0);

			FString FEmitter = UTF8_TO_TCHAR(EmitterName.c_str());
			int32 EmIdx = FindEmitterIndex(System, FEmitter);
			if (EmIdx == INDEX_NONE) { Session.Log(TEXT("[FAIL] configure_event_handler -> emitter not found")); return sol::lua_nil; }

			FVersionedNiagaraEmitterData* ED = System->GetEmitterHandle(EmIdx).GetEmitterData();
			if (!ED) { Session.Log(TEXT("[FAIL] configure_event_handler -> no emitter data")); return sol::lua_nil; }

			const TArray<FNiagaraEventScriptProperties>& Events = ED->GetEventHandlers();
			if (Events.Num() == 0)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] configure_event_handler -> emitter '%s' has no event handlers. Use add(\"event_handler\", {emitter=\"%s\", source_event_name=\"...\"}) first."), *FEmitter, *FEmitter));
				return sol::lua_nil;
			}
			if (Index < 0 || Index >= Events.Num()) { Session.Log(FString::Printf(TEXT("[FAIL] configure_event_handler -> index %d out of range (0-%d)"), Index, Events.Num() - 1)); return sol::lua_nil; }

			// Use GetEventHandlerByIdUnsafe for mutable access
			FNiagaraEventScriptProperties* EvtPtr = ED->GetEventHandlerByIdUnsafe(Events[Index].Script->GetUsageId());
			if (!EvtPtr) { Session.Log(TEXT("[FAIL] configure_event_handler -> could not get mutable event handler")); return sol::lua_nil; }

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "CfgNEvent", "Configure Niagara Event Handler"));
			System->Modify();
			FNiagaraEventScriptProperties& Evt = *EvtPtr;

			sol::optional<int> ExecMode = Params.get<sol::optional<int>>("execution_mode");
			if (ExecMode.has_value()) Evt.ExecutionMode = static_cast<EScriptExecutionMode>(ExecMode.value());
			sol::optional<int> SpawnNum = Params.get<sol::optional<int>>("spawn_number");
			if (SpawnNum.has_value()) Evt.SpawnNumber = SpawnNum.value();
			sol::optional<int> MaxEvents = Params.get<sol::optional<int>>("max_events_per_frame");
			if (MaxEvents.has_value()) Evt.MaxEventsPerFrame = MaxEvents.value();
			sol::optional<bool> RandomSpawn = Params.get<sol::optional<bool>>("random_spawn_number");
			if (RandomSpawn.has_value()) Evt.bRandomSpawnNumber = RandomSpawn.value();
			sol::optional<std::string> SrcEvent = Params.get<sol::optional<std::string>>("source_event_name");
			if (SrcEvent.has_value()) Evt.SourceEventName = FName(UTF8_TO_TCHAR(SrcEvent.value().c_str()));
			sol::optional<std::string> SrcEmitter = Params.get<sol::optional<std::string>>("source_emitter_id");
			if (SrcEmitter.has_value())
			{
				FGuid ParsedGuid;
				FGuid::Parse(UTF8_TO_TCHAR(SrcEmitter.value().c_str()), ParsedGuid);
				Evt.SourceEmitterID = ParsedGuid;
			}

			System->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] configure_event_handler(emitter=\"%s\", index=%d)"), *FEmitter, Index));
			return sol::make_object(Lua, true);
		});

		// ==================================================================
		// configure_simulation_stage({emitter, index, ...props})
		// ==================================================================
		AssetObj.set_function("configure_simulation_stage", [System, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			std::string EmitterName = Params.get<sol::optional<std::string>>("emitter").value_or("");
			int32 Index = Params.get<sol::optional<int>>("index").value_or(0);

			FString FEmitter = UTF8_TO_TCHAR(EmitterName.c_str());
			int32 EmIdx = FindEmitterIndex(System, FEmitter);
			if (EmIdx == INDEX_NONE) { Session.Log(TEXT("[FAIL] configure_simulation_stage -> emitter not found")); return sol::lua_nil; }

			FVersionedNiagaraEmitterData* ED = System->GetEmitterHandle(EmIdx).GetEmitterData();
			if (!ED) { Session.Log(TEXT("[FAIL] configure_simulation_stage -> no emitter data")); return sol::lua_nil; }

			const TArray<UNiagaraSimulationStageBase*>& Stages = ED->GetSimulationStages();
			if (Index < 0 || Index >= Stages.Num()) { Session.Log(TEXT("[FAIL] configure_simulation_stage -> index out of range")); return sol::lua_nil; }

			UNiagaraSimulationStageBase* Stage = Stages[Index];
			if (!Stage) { Session.Log(TEXT("[FAIL] configure_simulation_stage -> null stage")); return sol::lua_nil; }

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "CfgNSimStage", "Configure Niagara Simulation Stage"));
			System->Modify();
			Stage->Modify();

			sol::optional<std::string> NameOpt = Params.get<sol::optional<std::string>>("name");
			if (NameOpt.has_value()) Stage->SimulationStageName = FName(UTF8_TO_TCHAR(NameOpt.value().c_str()));
			sol::optional<bool> EnabledOpt = Params.get<sol::optional<bool>>("enabled");
			if (EnabledOpt.has_value()) Stage->bEnabled = EnabledOpt.value();

			// Set reflected properties if provided
			sol::optional<sol::table> Props = Params.get<sol::optional<sol::table>>("properties");
			if (Props.has_value())
			{
				for (auto& kv : Props.value())
				{
					if (!kv.first.is<std::string>()) continue;
					FString PropName = UTF8_TO_TCHAR(kv.first.as<std::string>().c_str());
					FString Err;
					double NumVal = kv.second.is<double>() ? kv.second.as<double>() : 0.0;
					FString StrVal = kv.second.is<std::string>() ? UTF8_TO_TCHAR(kv.second.as<std::string>().c_str()) : TEXT("");
					bool BoolVal = kv.second.is<bool>() ? kv.second.as<bool>() : false;
					if (!SetReflectedProperty(Stage, PropName, NumVal, StrVal, BoolVal, kv.second.is<std::string>(), kv.second.is<bool>(), Err))
						Session.Log(FString::Printf(TEXT("[WARN] SetReflectedProperty('%s'): %s"), *PropName, *Err));
				}
			}

			Stage->PostEditChange();
			System->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] configure_simulation_stage(emitter=\"%s\", index=%d)"), *FEmitter, Index));
			return sol::make_object(Lua, true);
		});

		// ==================================================================
		// run_validation() — detailed validation with actual results
		// ==================================================================
		AssetObj.set_function("run_validation", [System, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			sol::table Result = Lua.create_table();

			UNiagaraEffectType* EffectType = System->GetEffectType();
			Result["effect_type"] = EffectType ? TCHAR_TO_UTF8(*EffectType->GetName()) : "none";
			Result["emitters"] = static_cast<int>(System->GetEmitterHandles().Num());

			// Create a temporary ViewModel for validation
			FNiagaraSystemViewModelOptions Options;
			Options.EditMode = ENiagaraSystemViewModelEditMode::SystemAsset;
			Options.bCanAutoCompile = false;
			Options.bCanSimulate = false;
			Options.bCanModifyEmittersFromTimeline = false;
			Options.bIsForDataProcessingOnly = true;
			// MessageLogGuid is required — NiagaraMessageManager asserts if not set
			Options.MessageLogGuid = System->GetAssetGuid();
			ConfigureValidationViewModelOptions(Options);

			TSharedRef<FNiagaraSystemViewModel> SysViewModel = MakeShared<FNiagaraSystemViewModel>();
			SysViewModel->Initialize(*System, Options);

			sol::table Findings = Lua.create_table();
			int32 FindIdx = 1;
			NiagaraValidation::ValidateAllRulesInSystem(SysViewModel,
				[&Findings, &FindIdx, &Lua](const FNiagaraValidationResult& R)
				{
					sol::table F = Lua.create_table();
					F["severity"] = R.Severity == ENiagaraValidationSeverity::Error ? "error" :
						(R.Severity == ENiagaraValidationSeverity::Warning ? "warning" : "info");
					F["message"] = TCHAR_TO_UTF8(*R.SummaryText.ToString());
					if (!R.Description.IsEmpty())
						F["description"] = TCHAR_TO_UTF8(*R.Description.ToString());
					Findings[FindIdx++] = F;
				});

			Result["findings"] = Findings;
			Result["finding_count"] = FindIdx - 1;

			Session.Log(FString::Printf(TEXT("[OK] run_validation() -> %d findings"), FindIdx - 1));
			return Result;
		});

		// ==================================================================
		// subscribe_parameter_definitions({asset_path})
		// ==================================================================
		AssetObj.set_function("subscribe_parameter_definitions", [System, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			std::string AssetPath = Params.get<sol::optional<std::string>>("asset_path").value_or("");
			if (AssetPath.empty()) { Session.Log(TEXT("[FAIL] subscribe_parameter_definitions -> asset_path required")); return sol::lua_nil; }

			FString FPath = UTF8_TO_TCHAR(AssetPath.c_str());
			UNiagaraParameterDefinitionsBase* ParamDefs = LoadObject<UNiagaraParameterDefinitionsBase>(nullptr, *FPath);
			if (!ParamDefs) { Session.Log(FString::Printf(TEXT("[FAIL] subscribe_parameter_definitions -> '%s' not found"), *FPath)); return sol::lua_nil; }

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "SubNParamDefs", "Subscribe Niagara Parameter Definitions"));
			System->Modify();
			System->SubscribeToParameterDefinitions(ParamDefs);
			System->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] subscribe_parameter_definitions(\"%s\")"), *FPath));
			return sol::make_object(Lua, true);
		});

		// ==================================================================
		// unsubscribe_parameter_definitions({asset_path})
		// ==================================================================
		AssetObj.set_function("unsubscribe_parameter_definitions", [System, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			std::string AssetPath = Params.get<sol::optional<std::string>>("asset_path").value_or("");
			if (AssetPath.empty()) { Session.Log(TEXT("[FAIL] unsubscribe_parameter_definitions -> asset_path required")); return sol::lua_nil; }

			FString FPath = UTF8_TO_TCHAR(AssetPath.c_str());
			UNiagaraParameterDefinitionsBase* ParamDefs = LoadObject<UNiagaraParameterDefinitionsBase>(nullptr, *FPath);
			if (!ParamDefs) { Session.Log(FString::Printf(TEXT("[FAIL] unsubscribe_parameter_definitions -> '%s' not found"), *FPath)); return sol::lua_nil; }

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "UnsubNParamDefs", "Unsubscribe Niagara Parameter Definitions"));
			System->Modify();
			System->UnsubscribeFromParameterDefinitions(ParamDefs->GetDefinitionsUniqueId());
			System->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] unsubscribe_parameter_definitions(\"%s\")"), *FPath));
			return sol::make_object(Lua, true);
		});

		// ==================================================================
		// synchronize_parameter_definitions()
		// ==================================================================
		AssetObj.set_function("synchronize_parameter_definitions", [System, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			const FScopedTransaction Tx(NSLOCTEXT("AIK", "SyncNParamDefs", "Synchronize Niagara Parameter Definitions"));
			System->Modify();
			System->SynchronizeWithParameterDefinitions();
			// Also synchronize each emitter
			for (int32 i = 0; i < System->GetNumEmitters(); ++i)
			{
				FNiagaraEmitterHandle& Handle = System->GetEmitterHandle(i);
				UNiagaraEmitter* Emitter = Handle.GetInstance().Emitter;
				if (Emitter) Emitter->SynchronizeWithParameterDefinitions();
			}
			System->MarkPackageDirty();
			Session.Log(TEXT("[OK] synchronize_parameter_definitions()"));
			return sol::make_object(Lua, true);
		});

		// ==================================================================
		// list_parameter_definitions()
		// ==================================================================
		AssetObj.set_function("list_parameter_definitions", [System, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			sol::table Result = Lua.create_table();
			int32 Idx = 1;

			TArray<UNiagaraParameterDefinitionsBase*> Subscriptions = System->GetSubscribedParameterDefinitions();
			for (UNiagaraParameterDefinitionsBase* Def : Subscriptions)
			{
				if (!Def) continue;
				sol::table E = Lua.create_table();
				E["name"] = TCHAR_TO_UTF8(*Def->GetName());
				E["path"] = TCHAR_TO_UTF8(*Def->GetPathName());
				Result[Idx++] = E;
			}
			Session.Log(FString::Printf(TEXT("[OK] list_parameter_definitions() -> %d"), Idx - 1));
			return Result;
		});

		// ==================================================================
		// get(type, id, property?)
		// Read individual property values from system/emitter/renderer/module
		// ==================================================================
		AssetObj.set_function("get", [System, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::object> IdOpt, sol::optional<std::string> PropertyOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			// ---- get("system", _, "property") ----
			if (FType.Equals(TEXT("system"), ESearchCase::IgnoreCase))
			{
				if (!PropertyOpt.has_value()) { Session.Log(TEXT("[FAIL] get(\"system\") -> property name required as 3rd arg")); return sol::lua_nil; }
				FString PropName = UTF8_TO_TCHAR(PropertyOpt.value().c_str());
				FProperty* Prop = System->GetClass()->FindPropertyByName(FName(*PropName));
				if (!Prop) { Session.Log(FString::Printf(TEXT("[FAIL] get(\"system\", _, \"%s\") -> property not found"), *PropName)); return sol::lua_nil; }

				const void* Container = Prop->ContainerPtrToValuePtr<void>(System);
				if (FFloatProperty* FP = CastField<FFloatProperty>(Prop)) return sol::make_object(Lua, FP->GetPropertyValue(Container));
				if (FDoubleProperty* DP = CastField<FDoubleProperty>(Prop)) return sol::make_object(Lua, DP->GetPropertyValue(Container));
				if (FIntProperty* IP = CastField<FIntProperty>(Prop)) return sol::make_object(Lua, IP->GetPropertyValue(Container));
				if (FBoolProperty* BP = CastField<FBoolProperty>(Prop)) return sol::make_object(Lua, BP->GetPropertyValue(Container));
				if (FStrProperty* SP = CastField<FStrProperty>(Prop)) return sol::make_object(Lua, TCHAR_TO_UTF8(*SP->GetPropertyValue(Container)));
				if (FNameProperty* NP = CastField<FNameProperty>(Prop)) return sol::make_object(Lua, TCHAR_TO_UTF8(*NP->GetPropertyValue(Container).ToString()));

				// Fallback: ExportText
				FString ExportedValue;
				Prop->ExportTextItem_Direct(ExportedValue, Container, nullptr, System, PPF_None);
				Session.Log(FString::Printf(TEXT("[OK] get(\"system\", _, \"%s\") -> %s"), *PropName, *ExportedValue));
				return sol::make_object(Lua, std::string(TCHAR_TO_UTF8(*ExportedValue)));
			}
			// ---- get("emitter", name, "property") ----
			else if (FType.Equals(TEXT("emitter"), ESearchCase::IgnoreCase))
			{
				if (!IdOpt.has_value() || !IdOpt.value().is<std::string>()) { Session.Log(TEXT("[FAIL] get(\"emitter\") -> emitter name required")); return sol::lua_nil; }
				FString EmName = UTF8_TO_TCHAR(IdOpt.value().as<std::string>().c_str());
				int32 EmIdx = FindEmitterIndex(System, EmName);
				if (EmIdx == INDEX_NONE) { Session.Log(FString::Printf(TEXT("[FAIL] get(\"emitter\") -> '%s' not found"), *EmName)); return sol::lua_nil; }

				if (!PropertyOpt.has_value())
				{
					// Return summary table
					FVersionedNiagaraEmitterData* ED = System->GetEmitterHandle(EmIdx).GetEmitterData();
					if (!ED) return sol::lua_nil;
					sol::table R = Lua.create_table();
					R["name"] = TCHAR_TO_UTF8(*EmName);
					R["sim_target"] = (ED->SimTarget == ENiagaraSimTarget::GPUComputeSim) ? "gpu" : "cpu";
					R["local_space"] = ED->bLocalSpace;
					R["determinism"] = ED->bDeterminism;
					R["random_seed"] = ED->RandomSeed;
					R["requires_persistent_ids"] = (bool)ED->bRequiresPersistentIDs;
					R["allocation_mode"] = (ED->AllocationMode == EParticleAllocationMode::AutomaticEstimate) ? "automatic" : "manual";
					R["pre_allocation_count"] = ED->PreAllocationCount;
					R["max_gpu_particles_spawn_per_frame"] = ED->MaxGPUParticlesSpawnPerFrame;
					R["renderers"] = ED->GetRenderers().Num();
					R["event_handlers"] = ED->GetEventHandlers().Num();
					R["simulation_stages"] = ED->GetSimulationStages().Num();
					Session.Log(FString::Printf(TEXT("[OK] get(\"emitter\", \"%s\")"), *EmName));
					return R;
				}

				// Resolve property — first try FVersionedNiagaraEmitterData (where most per-emitter data lives),
				// then fall back to UNiagaraEmitter UObject for properties defined there.
				FString PropName = UTF8_TO_TCHAR(PropertyOpt.value().c_str());
				FVersionedNiagaraEmitterData* PropED = System->GetEmitterHandle(EmIdx).GetEmitterData();
				UNiagaraEmitter* Emitter = System->GetEmitterHandle(EmIdx).GetInstance().Emitter;

				// Try FVersionedNiagaraEmitterData first (SimTarget, bLocalSpace, bDeterminism, etc.)
				FProperty* Prop = FVersionedNiagaraEmitterData::StaticStruct()->FindPropertyByName(FName(*PropName));
				const void* Container = nullptr;
				UObject* OuterObj = nullptr;
				if (Prop && PropED)
				{
					Container = Prop->ContainerPtrToValuePtr<void>(PropED);
					OuterObj = Emitter;
				}
				else if (Emitter)
				{
					// Fall back to UNiagaraEmitter UObject
					Prop = Emitter->GetClass()->FindPropertyByName(FName(*PropName));
					if (Prop)
					{
						Container = Prop->ContainerPtrToValuePtr<void>(Emitter);
						OuterObj = Emitter;
					}
				}

				if (!Prop || !Container) { Session.Log(FString::Printf(TEXT("[FAIL] get(\"emitter\", \"%s\", \"%s\") -> property not found"), *EmName, *PropName)); return sol::lua_nil; }

				if (FFloatProperty* FP = CastField<FFloatProperty>(Prop)) return sol::make_object(Lua, FP->GetPropertyValue(Container));
				if (FDoubleProperty* DP = CastField<FDoubleProperty>(Prop)) return sol::make_object(Lua, DP->GetPropertyValue(Container));
				if (FIntProperty* IP = CastField<FIntProperty>(Prop)) return sol::make_object(Lua, IP->GetPropertyValue(Container));
				if (FBoolProperty* BP = CastField<FBoolProperty>(Prop)) return sol::make_object(Lua, BP->GetPropertyValue(Container));

				FString ExportedValue;
				Prop->ExportTextItem_Direct(ExportedValue, Container, nullptr, OuterObj, PPF_None);
				return sol::make_object(Lua, std::string(TCHAR_TO_UTF8(*ExportedValue)));
			}
			// ---- get("renderer", {emitter, index}, "property") ----
			else if (FType.Equals(TEXT("renderer"), ESearchCase::IgnoreCase))
			{
				if (!IdOpt.has_value() || !IdOpt.value().is<sol::table>()) { Session.Log(TEXT("[FAIL] get(\"renderer\") -> {emitter, index} required")); return sol::lua_nil; }
				sol::table T = IdOpt.value().as<sol::table>();
				std::string EmName = T.get_or<std::string>("emitter", "");
				int32 RIdx = T.get_or("index", 0);

				int32 EmIdx = FindEmitterIndex(System, UTF8_TO_TCHAR(EmName.c_str()));
				if (EmIdx == INDEX_NONE) { Session.Log(TEXT("[FAIL] get(\"renderer\") -> emitter not found")); return sol::lua_nil; }

				FVersionedNiagaraEmitterData* ED = System->GetEmitterHandle(EmIdx).GetEmitterData();
				if (!ED) return sol::lua_nil;
				const TArray<UNiagaraRendererProperties*>& Renderers = ED->GetRenderers();
				if (RIdx < 0 || RIdx >= Renderers.Num()) { Session.Log(TEXT("[FAIL] get(\"renderer\") -> index out of range")); return sol::lua_nil; }

				UNiagaraRendererProperties* Renderer = Renderers[RIdx];
				if (!Renderer) return sol::lua_nil;

				if (!PropertyOpt.has_value())
				{
					sol::table R = Lua.create_table();
					R["type"] = TCHAR_TO_UTF8(*GetRendererTypeName(Renderer));
					R["enabled"] = Renderer->GetIsEnabled();
					R["class"] = TCHAR_TO_UTF8(*Renderer->GetClass()->GetName());
					Session.Log(FString::Printf(TEXT("[OK] get(\"renderer\", index=%d)"), RIdx));
					return R;
				}

				FString PropName = UTF8_TO_TCHAR(PropertyOpt.value().c_str());
				FProperty* Prop = Renderer->GetClass()->FindPropertyByName(FName(*PropName));
				if (!Prop) { Session.Log(FString::Printf(TEXT("[FAIL] get(\"renderer\") -> property '%s' not found"), *PropName)); return sol::lua_nil; }

				const void* Container = Prop->ContainerPtrToValuePtr<void>(Renderer);
				if (FFloatProperty* FP = CastField<FFloatProperty>(Prop)) return sol::make_object(Lua, FP->GetPropertyValue(Container));
				if (FDoubleProperty* DP = CastField<FDoubleProperty>(Prop)) return sol::make_object(Lua, DP->GetPropertyValue(Container));
				if (FIntProperty* IP = CastField<FIntProperty>(Prop)) return sol::make_object(Lua, IP->GetPropertyValue(Container));
				if (FBoolProperty* BP = CastField<FBoolProperty>(Prop)) return sol::make_object(Lua, BP->GetPropertyValue(Container));

				FString ExportedValue;
				Prop->ExportTextItem_Direct(ExportedValue, Container, nullptr, Renderer, PPF_None);
				return sol::make_object(Lua, std::string(TCHAR_TO_UTF8(*ExportedValue)));
			}
			// ---- get("user_parameter", name) ----
			else if (FType.Contains(TEXT("user_param"), ESearchCase::IgnoreCase) || FType.Contains(TEXT("parameter"), ESearchCase::IgnoreCase))
			{
				if (!IdOpt.has_value() || !IdOpt.value().is<std::string>()) { Session.Log(TEXT("[FAIL] get(\"user_parameter\") -> name required")); return sol::lua_nil; }
				FString ParamName = UTF8_TO_TCHAR(IdOpt.value().as<std::string>().c_str());
				FString FullName = TEXT("User.") + ParamName;

				FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();
				TArray<FNiagaraVariable> AllParams;
				Store.GetUserParameters(AllParams);

				for (const FNiagaraVariable& V : AllParams)
				{
					FString VName = V.GetName().ToString();
					if (VName.Equals(ParamName, ESearchCase::IgnoreCase) || VName.Equals(FullName, ESearchCase::IgnoreCase))
					{
						FNiagaraTypeDefinition VType = V.GetType();
						if (VType == FNiagaraTypeDefinition::GetFloatDef())
							return sol::make_object(Lua, Store.GetParameterValue<float>(V));
						if (VType == FNiagaraTypeDefinition::GetIntDef())
							return sol::make_object(Lua, Store.GetParameterValue<int32>(V));
						if (VType == FNiagaraTypeDefinition::GetBoolDef())
							return sol::make_object(Lua, Store.GetParameterValue<FNiagaraBool>(V).GetValue());
						if (VType == FNiagaraTypeDefinition::GetVec2Def())
						{
							FVector2f Val = Store.GetParameterValue<FVector2f>(V);
							sol::table T = Lua.create_table();
							T["x"] = Val.X; T["y"] = Val.Y;
							return T;
						}
						if (VType == FNiagaraTypeDefinition::GetVec3Def())
						{
							FVector3f Val = Store.GetParameterValue<FVector3f>(V);
							sol::table T = Lua.create_table();
							T["x"] = Val.X; T["y"] = Val.Y; T["z"] = Val.Z;
							return T;
						}
						if (VType == FNiagaraTypeDefinition::GetVec4Def())
						{
							FVector4f Val = Store.GetParameterValue<FVector4f>(V);
							sol::table T = Lua.create_table();
							T["x"] = Val.X; T["y"] = Val.Y; T["z"] = Val.Z; T["w"] = Val.W;
							return T;
						}
						if (VType == FNiagaraTypeDefinition::GetColorDef())
						{
							FLinearColor Val = Store.GetParameterValue<FLinearColor>(V);
							sol::table T = Lua.create_table();
							T["r"] = Val.R; T["g"] = Val.G; T["b"] = Val.B; T["a"] = Val.A;
							return T;
						}
						if (VType == FNiagaraTypeDefinition::GetQuatDef())
						{
							FQuat4f Val = Store.GetParameterValue<FQuat4f>(V);
							sol::table T = Lua.create_table();
							T["x"] = Val.X; T["y"] = Val.Y; T["z"] = Val.Z; T["w"] = Val.W;
							return T;
						}
						// Unknown type - return type name
						sol::table T = Lua.create_table();
						T["type"] = TCHAR_TO_UTF8(*VType.GetName());
						T["_unsupported"] = true;
						return T;
					}
				}
				Session.Log(FString::Printf(TEXT("[FAIL] get(\"user_parameter\", \"%s\") -> not found"), *ParamName));
				return sol::lua_nil;
			}
			// ---- get("module", {emitter, stage, module_name}, "input_name") ----
			else if (FType.Equals(TEXT("module"), ESearchCase::IgnoreCase))
			{
				if (!IdOpt.has_value() || !IdOpt.value().is<sol::table>()) { Session.Log(TEXT("[FAIL] get(\"module\") -> {emitter, stage, module_name} required")); return sol::lua_nil; }
				sol::table T = IdOpt.value().as<sol::table>();
				std::string EmitterName = T.get_or<std::string>("emitter", "");
				std::string StageStr = T.get_or<std::string>("stage", "particle_update");
				std::string ModName = T.get_or<std::string>("module_name", "");
				WarnUnconsumedKeys(Session, T, { TEXT("emitter"), TEXT("stage"), TEXT("module_name") }, TEXT("get(\"module\")"));
				if (ModName.empty()) { Session.Log(TEXT("[FAIL] get(\"module\") -> module_name required")); return sol::lua_nil; }

				ENiagaraScriptUsage Usage = ParseUsage(UTF8_TO_TCHAR(StageStr.c_str()));
				int32 EmIdx = INDEX_NONE;
				bool bSys = (Usage == ENiagaraScriptUsage::SystemSpawnScript || Usage == ENiagaraScriptUsage::SystemUpdateScript);
				if (!bSys) EmIdx = FindEmitterIndex(System, UTF8_TO_TCHAR(EmitterName.c_str()));

				UNiagaraNodeOutput* OutputNode = FindOutputNode(System, EmIdx, Usage, FGuid(), false);
				if (!OutputNode) { Session.Log(TEXT("[FAIL] get(\"module\") -> no output node")); return sol::lua_nil; }

				UNiagaraNodeFunctionCall* ModNode = FindModuleByName(OutputNode, UTF8_TO_TCHAR(ModName.c_str()));
				if (!ModNode) { Session.Log(FString::Printf(TEXT("[FAIL] get(\"module\") -> '%s' not found"), UTF8_TO_TCHAR(ModName.c_str()))); return sol::lua_nil; }

				if (!PropertyOpt.has_value())
				{
					// Return all input pin default values
					sol::table R = Lua.create_table();
					R["name"] = TCHAR_TO_UTF8(*ModNode->GetFunctionName());
					R["enabled"] = ModNode->GetDesiredEnabledState() == ENodeEnabledState::Enabled;
					sol::table Inputs = Lua.create_table();
					int32 Idx2 = 1;
					for (UEdGraphPin* Pin : ModNode->GetAllPins())
					{
						if (Pin->Direction == EGPD_Input && !IsParameterMapPin(Pin))
						{
							sol::table PinInfo = Lua.create_table();
							PinInfo["name"] = TCHAR_TO_UTF8(*Pin->GetName());
							FNiagaraTypeDefinition PinType = UEdGraphSchema_Niagara::PinToTypeDefinition(Pin);
							PinInfo["type"] = TCHAR_TO_UTF8(*PinType.GetName());
							if (!Pin->DefaultValue.IsEmpty())
								PinInfo["default_value"] = TCHAR_TO_UTF8(*Pin->DefaultValue);
							PinInfo["linked"] = Pin->LinkedTo.Num() > 0;
							if (Pin->bHidden)
								PinInfo["hidden"] = true;
							Inputs[Idx2++] = PinInfo;
						}
					}
					R["inputs"] = Inputs;
					Session.Log(FString::Printf(TEXT("[OK] get(\"module\", \"%s\")"), UTF8_TO_TCHAR(ModName.c_str())));
					return R;
				}

				// Resolve owning script for RI param lookups
				UNiagaraScript* GetModScript = nullptr;
				const FNiagaraEmitterHandle* GetModEmHandle = (EmIdx != INDEX_NONE) ? &System->GetEmitterHandle(EmIdx) : nullptr;
				if (EmIdx != INDEX_NONE)
				{
					FVersionedNiagaraEmitterData* GetModED = System->GetEmitterHandle(EmIdx).GetEmitterData();
					if (GetModED)
					{
						TArray<UNiagaraScript*> GetModScripts;
						GetModED->GetScripts(GetModScripts, false);
						for (UNiagaraScript* Sc : GetModScripts) { if (Sc && Sc->GetUsage() == Usage) { GetModScript = Sc; break; } }
					}
				}
				else
				{
					GetModScript = (Usage == ENiagaraScriptUsage::SystemSpawnScript) ? System->GetSystemSpawnScript() : System->GetSystemUpdateScript();
				}

				// Find specific input pin (include hidden pins for static-switch-gated inputs)
				FString InputName = UTF8_TO_TCHAR(PropertyOpt.value().c_str());
				for (UEdGraphPin* Pin : ModNode->GetAllPins())
				{
					if (Pin->Direction != EGPD_Input || IsParameterMapPin(Pin)) continue;
					FString PinName = Pin->GetName();
					FString ShortName = PinName;
					if (ShortName.Contains(TEXT(".")))
						ShortName = ShortName.RightChop(ShortName.Find(TEXT("."), ESearchCase::IgnoreCase, ESearchDir::FromEnd) + 1);

					if (PinName.Equals(InputName, ESearchCase::IgnoreCase) ||
						ShortName.Equals(InputName, ESearchCase::IgnoreCase))
					{
						sol::table R = Lua.create_table();
						R["name"] = TCHAR_TO_UTF8(*PinName);
						FNiagaraTypeDefinition PinType = UEdGraphSchema_Niagara::PinToTypeDefinition(Pin);
						R["type"] = TCHAR_TO_UTF8(*PinType.GetName());
						R["linked"] = Pin->LinkedTo.Num() > 0;
						if (!Pin->DefaultValue.IsEmpty())
							R["default_value"] = TCHAR_TO_UTF8(*Pin->DefaultValue);
						if (Pin->LinkedTo.Num() > 0)
						{
							UEdGraphNode* LinkedNode = Pin->LinkedTo[0]->GetOwningNode();
							if (UNiagaraNodeFunctionCall* FC = Cast<UNiagaraNodeFunctionCall>(LinkedNode))
								R["linked_to"] = TCHAR_TO_UTF8(*FC->GetFunctionName());
							else if (UNiagaraNodeCustomHlsl* Hlsl = Cast<UNiagaraNodeCustomHlsl>(LinkedNode))
								R["hlsl_code"] = TCHAR_TO_UTF8(*GetCustomHlslText(Hlsl));
						}

						// Also check RI params for the current value
						if (GetModScript && System->ShouldUseRapidIterationParameters())
						{
							FNiagaraVariable PinVar(PinType, Pin->GetFName());
							FNiagaraParameterHandle PinHandle(PinVar.GetName());
							FNiagaraParameterHandle AliasedPinHandle = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(PinHandle, ModNode);
							FNiagaraVariable AliasedPinVar(PinType, AliasedPinHandle.GetParameterHandleString());

							FNiagaraEmitterHandle DummyHandle2;
							const FNiagaraEmitterHandle& RIEmHandle = GetModEmHandle ? *GetModEmHandle : DummyHandle2;
							const TCHAR* RIEmName = nullptr;
							FString RIEmUniqueName;
							if (RIEmHandle.IsValid() && RIEmHandle.GetInstance().Emitter)
							{
								RIEmUniqueName = RIEmHandle.GetInstance().Emitter->GetUniqueEmitterName();
								if (!RIEmUniqueName.IsEmpty()) RIEmName = *RIEmUniqueName;
							}
							FNiagaraVariable RIVar = FNiagaraUtilities::ConvertVariableToRapidIterationConstantName(AliasedPinVar, RIEmName, GetModScript->GetUsage());
							const int32* RIOffset = GetModScript->RapidIterationParameters.FindParameterOffset(RIVar);
							if (RIOffset)
							{
								R["value_mode"] = "rapid_iteration";
								if (PinType == FNiagaraTypeDefinition::GetFloatDef())
									R["value"] = GetModScript->RapidIterationParameters.GetParameterValue<float>(RIVar);
								else if (PinType == FNiagaraTypeDefinition::GetIntDef())
									R["value"] = GetModScript->RapidIterationParameters.GetParameterValue<int32>(RIVar);
								else if (PinType == FNiagaraTypeDefinition::GetBoolDef())
									R["value"] = GetModScript->RapidIterationParameters.GetParameterValue<FNiagaraBool>(RIVar).GetValue();
							}
						}

						Session.Log(FString::Printf(TEXT("[OK] get(\"module\", \"%s\", \"%s\")"), UTF8_TO_TCHAR(ModName.c_str()), *InputName));
						return R;
					}
				}
				// Pin search failed — try RI store directly by scanning all RI params for this module
				// Also check ALL emitter scripts, not just the one matching Usage, since RI params
				// may be stored on spawn/update scripts regardless of which stage the module is visually in
				{
					TArray<UNiagaraScript*> ScriptsToSearch;
					if (GetModScript) ScriptsToSearch.Add(GetModScript);
					if (EmIdx != INDEX_NONE)
					{
						FVersionedNiagaraEmitterData* SearchED = System->GetEmitterHandle(EmIdx).GetEmitterData();
						if (SearchED)
						{
							TArray<UNiagaraScript*> AllScripts;
							SearchED->GetScripts(AllScripts, false);
							for (UNiagaraScript* Sc : AllScripts) ScriptsToSearch.AddUnique(Sc);
						}
					}

					FString ModFuncName = ModNode->GetFunctionName();
					for (UNiagaraScript* SearchScript : ScriptsToSearch)
					{
						if (!SearchScript) continue;
						for (const FNiagaraVariableWithOffset& RIVar : SearchScript->RapidIterationParameters.ReadParameterVariables())
						{
							FString RIName = RIVar.GetName().ToString();
							if (!RIName.Contains(ModFuncName)) continue;

							FString ShortName = RIName;
							int32 LastDot = INDEX_NONE;
							if (ShortName.FindLastChar(TEXT('.'), LastDot))
								ShortName = ShortName.RightChop(LastDot + 1);

							if (!ShortName.Equals(InputName, ESearchCase::IgnoreCase)) continue;

							sol::table R = Lua.create_table();
							R["name"] = TCHAR_TO_UTF8(*ShortName);
							R["full_name"] = TCHAR_TO_UTF8(*RIName);
							R["type"] = TCHAR_TO_UTF8(*RIVar.GetType().GetName());
							R["value_mode"] = "rapid_iteration";

							const int32* Off = SearchScript->RapidIterationParameters.FindParameterOffset(RIVar);
							if (Off)
							{
								if (RIVar.GetType() == FNiagaraTypeDefinition::GetFloatDef())
									R["value"] = SearchScript->RapidIterationParameters.GetParameterValue<float>(RIVar);
								else if (RIVar.GetType() == FNiagaraTypeDefinition::GetIntDef())
									R["value"] = SearchScript->RapidIterationParameters.GetParameterValue<int32>(RIVar);
								else if (RIVar.GetType() == FNiagaraTypeDefinition::GetBoolDef())
									R["value"] = SearchScript->RapidIterationParameters.GetParameterValue<FNiagaraBool>(RIVar).GetValue();
							}
							Session.Log(FString::Printf(TEXT("[OK] get(\"module\", \"%s\", \"%s\")"), UTF8_TO_TCHAR(ModName.c_str()), *InputName));
							return R;
						}
					}
				}

				Session.Log(FString::Printf(TEXT("[FAIL] get(\"module\", \"%s\", \"%s\") -> input not found"), UTF8_TO_TCHAR(ModName.c_str()), *InputName));
				return sol::lua_nil;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] get(\"%s\") -> unknown type. Valid: system, emitter, renderer, user_parameter, module"), *FType));
			return sol::lua_nil;
		});

		// ==================================================================
		// version(verb, params?) — emitter versioning
		// ==================================================================
		AssetObj.set_function("version", [System, &Session](sol::table /*self*/,
			const std::string& Verb, sol::optional<sol::table> ParamsOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FVerb = UTF8_TO_TCHAR(Verb.c_str());

			if (!ParamsOpt.has_value()) { Session.Log(TEXT("[FAIL] version() -> params table required")); return sol::lua_nil; }
			sol::table P = ParamsOpt.value();
			std::string EmitterName = P.get_or<std::string>("emitter", "");

			if (EmitterName.empty()) { Session.Log(TEXT("[FAIL] version() -> emitter name required")); return sol::lua_nil; }
			FString FEmitter = UTF8_TO_TCHAR(EmitterName.c_str());
			int32 EmIdx = FindEmitterIndex(System, FEmitter);
			if (EmIdx == INDEX_NONE) { Session.Log(FString::Printf(TEXT("[FAIL] version() -> emitter '%s' not found"), *FEmitter)); return sol::lua_nil; }

			FNiagaraEmitterHandle& Handle = System->GetEmitterHandle(EmIdx);
			UNiagaraEmitter* Emitter = Handle.GetInstance().Emitter;
			if (!Emitter) { Session.Log(TEXT("[FAIL] version() -> no emitter object")); return sol::lua_nil; }

			// ---- version("list") ----
			if (FVerb.Equals(TEXT("list"), ESearchCase::IgnoreCase))
			{
				TArray<FNiagaraAssetVersion> Versions = Emitter->GetAllAvailableVersions();
				FNiagaraAssetVersion ExposedVer = Emitter->GetExposedVersion();

				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Versions.Num(); ++i)
				{
					sol::table V = Lua.create_table();
					V["major"] = Versions[i].MajorVersion;
					V["minor"] = Versions[i].MinorVersion;
					V["version_guid"] = TCHAR_TO_UTF8(*Versions[i].VersionGuid.ToString());
					V["is_exposed"] = (Versions[i].VersionGuid == ExposedVer.VersionGuid);
					V["visible_in_selector"] = Versions[i].bIsVisibleInVersionSelector;
					Result[i + 1] = V;
				}
				Session.Log(FString::Printf(TEXT("[OK] version(\"list\", emitter=\"%s\") -> %d versions"), *FEmitter, Versions.Num()));
				return Result;
			}
			// ---- version("add") ----
			else if (FVerb.Equals(TEXT("add"), ESearchCase::IgnoreCase))
			{
				int32 Major = static_cast<int32>(P.get_or("major", 1));
				int32 Minor = static_cast<int32>(P.get_or("minor", 0));

				if (Major <= 1 && Minor <= 0) { Session.Log(TEXT("[FAIL] version(\"add\") -> version must be > 1.0")); return sol::lua_nil; }

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "AddNVersion", "Add Niagara Emitter Version"));
				Emitter->Modify();

				if (!Emitter->IsVersioningEnabled())
					Emitter->EnableVersioning();

				FGuid NewGuid = Emitter->AddNewVersion(Major, Minor);
				if (!NewGuid.IsValid())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] version(\"add\") -> failed to add version %d.%d (may already exist)"), Major, Minor));
					return sol::lua_nil;
				}

				System->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] version(\"add\", emitter=\"%s\", %d.%d) -> %s"), *FEmitter, Major, Minor, *NewGuid.ToString()));
				sol::table Result = Lua.create_table();
				Result["version_guid"] = TCHAR_TO_UTF8(*NewGuid.ToString());
				Result["major"] = Major;
				Result["minor"] = Minor;
				return Result;
			}
			// ---- version("expose") ----
			else if (FVerb.Equals(TEXT("expose"), ESearchCase::IgnoreCase))
			{
				std::string GuidStr = P.get_or<std::string>("version_guid", "");
				if (GuidStr.empty()) { Session.Log(TEXT("[FAIL] version(\"expose\") -> version_guid required")); return sol::lua_nil; }

				FGuid VersionGuid;
				FGuid::Parse(UTF8_TO_TCHAR(GuidStr.c_str()), VersionGuid);
				if (!VersionGuid.IsValid()) { Session.Log(TEXT("[FAIL] version(\"expose\") -> invalid guid")); return sol::lua_nil; }

				const FNiagaraAssetVersion* FoundVersion = Emitter->FindVersionData(VersionGuid);
				if (!FoundVersion) { Session.Log(FString::Printf(TEXT("[FAIL] version(\"expose\") -> version '%s' not found"), UTF8_TO_TCHAR(GuidStr.c_str()))); return sol::lua_nil; }

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "ExposeNVersion", "Expose Niagara Emitter Version"));
				Emitter->Modify();
				Emitter->ExposeVersion(VersionGuid);
				System->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] version(\"expose\", emitter=\"%s\", guid=\"%s\")"), *FEmitter, UTF8_TO_TCHAR(GuidStr.c_str())));
				return sol::make_object(Lua, true);
			}
			// ---- version("delete") ----
			else if (FVerb.Equals(TEXT("delete"), ESearchCase::IgnoreCase))
			{
				std::string GuidStr = P.get_or<std::string>("version_guid", "");
				if (GuidStr.empty()) { Session.Log(TEXT("[FAIL] version(\"delete\") -> version_guid required")); return sol::lua_nil; }

				FGuid VersionGuid;
				FGuid::Parse(UTF8_TO_TCHAR(GuidStr.c_str()), VersionGuid);
				if (!VersionGuid.IsValid()) { Session.Log(TEXT("[FAIL] version(\"delete\") -> invalid guid")); return sol::lua_nil; }

				// Cannot delete the exposed version
				FNiagaraAssetVersion ExposedVer = Emitter->GetExposedVersion();
				if (VersionGuid == ExposedVer.VersionGuid)
				{
					Session.Log(TEXT("[FAIL] version(\"delete\") -> cannot delete the currently exposed version"));
					return sol::lua_nil;
				}

				const FNiagaraAssetVersion* FoundVersion = Emitter->FindVersionData(VersionGuid);
				if (!FoundVersion) { Session.Log(FString::Printf(TEXT("[FAIL] version(\"delete\") -> version '%s' not found"), UTF8_TO_TCHAR(GuidStr.c_str()))); return sol::lua_nil; }

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "DelNVersion", "Delete Niagara Emitter Version"));
				Emitter->Modify();
				Emitter->DeleteVersion(VersionGuid);
				System->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] version(\"delete\", emitter=\"%s\", guid=\"%s\")"), *FEmitter, UTF8_TO_TCHAR(GuidStr.c_str())));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] version(\"%s\") -> unknown verb. Valid: list, add, expose, delete"), *FVerb));
			return sol::lua_nil;
		});

		// ==================================================================
		// compile() — request compile and report status
		// ==================================================================
		AssetObj.set_function("compile", [System, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			System->RequestCompile(false);
			System->WaitForCompilationComplete();

			sol::table Result = Lua.create_table();
			sol::table Errors = Lua.create_table();
			int32 ErrorIdx = 1;

			TArray<UNiagaraScript*> AllScripts;
			AllScripts.Add(System->GetSystemSpawnScript());
			AllScripts.Add(System->GetSystemUpdateScript());
			for (int32 i = 0; i < System->GetNumEmitters(); ++i)
			{
				FVersionedNiagaraEmitterData* ED = System->GetEmitterHandle(i).GetEmitterData();
				if (ED)
				{
					TArray<UNiagaraScript*> EmitterScripts;
					ED->GetScripts(EmitterScripts, false);
					AllScripts.Append(EmitterScripts);
				}
			}

			for (UNiagaraScript* Script : AllScripts)
			{
				if (!Script) continue;
				const TArray<FNiagaraCompileEvent>& CompileEvents = Script->GetVMExecutableData().LastCompileEvents;
				for (const FNiagaraCompileEvent& Evt : CompileEvents)
				{
					if (Evt.Severity >= FNiagaraCompileEventSeverity::Warning)
					{
						sol::table E = Lua.create_table();
						E["severity"] = (Evt.Severity == FNiagaraCompileEventSeverity::Error) ? "error" : "warning";
						E["message"] = TCHAR_TO_UTF8(*Evt.Message);
						if (!Evt.ShortDescription.IsEmpty())
							E["short_description"] = TCHAR_TO_UTF8(*Evt.ShortDescription);
						if (Evt.NodeGuid.IsValid())
							E["node_guid"] = TCHAR_TO_UTF8(*Evt.NodeGuid.ToString());
						if (Evt.PinGuid.IsValid())
							E["pin_guid"] = TCHAR_TO_UTF8(*Evt.PinGuid.ToString());
						if (Evt.StackGuids.Num() > 0)
						{
							sol::table StackGuids = Lua.create_table();
							for (int32 gi = 0; gi < Evt.StackGuids.Num(); ++gi)
								StackGuids[gi + 1] = TCHAR_TO_UTF8(*Evt.StackGuids[gi].ToString());
							E["stack_guids"] = StackGuids;
						}
						E["script"] = TCHAR_TO_UTF8(*Script->GetName());
						Errors[ErrorIdx++] = E;
					}
				}
			}

			Result["errors"] = Errors;
			Result["error_count"] = ErrorIdx - 1;
			Result["success"] = (ErrorIdx == 1);
			// Also add errors as integer keys on top level so ipairs(result) works
			for (int32 i = 1; i < ErrorIdx; i++)
				Result[i] = Errors[i];
			Session.Log(FString::Printf(TEXT("[OK] compile() -> %d errors/warnings"), ErrorIdx - 1));
			return Result;
		});
	});
}

REGISTER_LUA_BINDING(Niagara, NiagaraDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("Niagara")))
	{
		Session.Log(TEXT("[WARN] Niagara plugin is not loaded. Enable it in Edit > Plugins to use this feature."));
		return;
	}
	BindNiagara(Lua, Session);
});
