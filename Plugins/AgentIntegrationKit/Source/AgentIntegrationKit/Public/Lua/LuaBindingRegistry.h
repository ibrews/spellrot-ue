#pragma once

#include "CoreMinimal.h"
#include "BlueprintNodeSpawner.h"
#include "EdGraph/EdGraphSchema.h"
#include "Tools/NeoStackToolBase.h"

struct lua_State;
namespace sol { class state; }

class UBlueprint;
class UEdGraph;
class UEdGraphNode;

/** Max trace entries per script execution — prevents runaway log() calls from exhausting memory.
 *  7341 lines ≈ 2MB at avg 280 bytes/line, which fits comfortably in a single MCP response. */
constexpr int32 LUA_MAX_TRACE_ENTRIES = 7341;

// Shared state for one script execution — all bindings can read/write this
struct AGENTINTEGRATIONKIT_API FLuaSessionData
{
	TArray<FString> Trace;

	// Node map: add_node/read_graph store nodes here, connect/set_pin look them up.
	// Uses TWeakObjectPtr to prevent dangling pointers if nodes are deleted externally
	// (e.g., editor undo, user action, another script call).
	TMap<FString, TWeakObjectPtr<UEdGraphNode>> Nodes;

	// Spawner cache: find_nodes stores spawners here, add_node looks them up
	// Keyed by unique IDs we generate (not engine signatures, which collide for variable nodes)
	TMap<FString, TWeakObjectPtr<UBlueprintNodeSpawner>> CachedSpawners;
	int32 NextSpawnerId = 0;

	FString CacheSpawner(UBlueprintNodeSpawner* Spawner)
	{
		FString Id = FString::Printf(TEXT("spawner_%d"), NextSpawnerId++);
		CachedSpawners.Add(Id, Spawner);
		return Id;
	}

	UBlueprintNodeSpawner* FindCachedSpawner(const FString& Id) const
	{
		if (const TWeakObjectPtr<UBlueprintNodeSpawner>* Found = CachedSpawners.Find(Id))
		{
			return Found->Get();
		}
		return nullptr;
	}

	// Schema action cache: find_nodes stores schema actions here, add_node looks them up.
	// Used for non-Blueprint graphs (BehaviorTree, Material, SoundCue, EQS, etc.)
	// where nodes are created via FEdGraphSchemaAction::PerformAction() instead of UBlueprintNodeSpawner.
	struct FCachedSchemaAction
	{
		TSharedPtr<FEdGraphSchemaAction> Action;
		TWeakObjectPtr<UEdGraph> Graph;
	};
	TMap<FString, FCachedSchemaAction> CachedSchemaActions;
	int32 NextActionId = 0;

	FString CacheSchemaAction(TSharedPtr<FEdGraphSchemaAction> Action, UEdGraph* Graph)
	{
		FString Id = FString::Printf(TEXT("action_%d"), NextActionId++);
		CachedSchemaActions.Add(Id, { Action, Graph });
		return Id;
	}

	FCachedSchemaAction* FindCachedSchemaAction(const FString& Id)
	{
		return CachedSchemaActions.Find(Id);
	}

	// Loaded graphs for auto-resolving node handles
	TSet<UEdGraph*> LoadedGraphs;

	// Dirty graphs: track (Graph, Asset) pairs that need post-mutation finalization.
	// Finalized once at the end of script execution, not after every operation.
	TMap<UEdGraph*, UObject*> DirtyGraphs;

	void MarkGraphDirty(UEdGraph* Graph, UObject* Asset)
	{
		if (Graph && Asset) DirtyGraphs.FindOrAdd(Graph, Asset);
	}

	// Mark a graph dirty using its owning asset (walks outer chain)
	void MarkGraphDirty(UEdGraph* Graph);

	// Image pipeline: Lua bindings add images here, pipeline copies to FScriptResult → FToolResult
	TArray<FToolResultImage> Images;

	void AddImage(const FString& Base64, const FString& MimeType, int32 Width, int32 Height)
	{
		FToolResultImage Img;
		Img.Base64Data = Base64;
		Img.MimeType = MimeType;
		Img.Width = Width;
		Img.Height = Height;
		Images.Add(MoveTemp(Img));
	}

	void AddImages(const TArray<FToolResultImage>& InImages)
	{
		Images.Append(InImages);
	}

	void Log(const FString& Entry) { if (Trace.Num() < LUA_MAX_TRACE_ENTRIES) Trace.Add(Entry); }

	// Find node by handle — if not in map, scans all loaded graphs
	UEdGraphNode* FindNode(const FString& Handle);

	// Register all nodes from a graph
	void RegisterGraphNodes(UEdGraph* Graph);
};

DECLARE_DELEGATE_TwoParams(FLuaBindingFunc, sol::state& /*Lua*/, FLuaSessionData& /*Session*/);

struct FLuaFunctionDoc
{
	FString Signature;
	FString Description;
	FString Returns;
};

struct FLuaBinding
{
	FString Name;
	TArray<FLuaFunctionDoc> Functions;
	FLuaBindingFunc Bind;
};

class AGENTINTEGRATIONKIT_API FLuaBindingRegistry
{
public:
	static FLuaBindingRegistry& Get();

	void Register(const FString& Name, TArray<FLuaFunctionDoc> Functions, FLuaBindingFunc BindFunc);
	const TArray<FLuaBinding>& GetAll() const { return Bindings; }

	FString BuildDescription() const;

private:
	TArray<FLuaBinding> Bindings;
};

#define REGISTER_LUA_BINDING(Name, Docs, BindLambda) \
	static struct FAutoReg_##Name { \
		FAutoReg_##Name() { \
			FLuaBindingRegistry::Get().Register(TEXT(#Name), Docs, FLuaBindingFunc::CreateLambda(BindLambda)); \
		} \
	} GAutoReg_##Name;
