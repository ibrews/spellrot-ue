#include "Lua/LuaBindingRegistry.h"
#include "Lua/LuaPinHelper.h"
#include "Lua/LuaGraphResolver.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"

// External resolver storage — must live in exactly one .cpp in the main module
// to avoid cross-DLL static duplication (each .dylib/.dll would get its own copy).
namespace LuaGraphResolver
{
	TArray<FExternalResolverFunc>& GetExternalResolvers()
	{
		static TArray<FExternalResolverFunc> Resolvers;
		return Resolvers;
	}
	void RegisterExternalResolver(FExternalResolverFunc Resolver)
	{
		GetExternalResolvers().Add(MoveTemp(Resolver));
	}
}

// Populate editor-only graph metadata (guid, subgraphs) when available
static void PopulateEditorOnlyGraphData(sol::state_view& LuaView, sol::table& Result, UEdGraph* Graph)
{
#if WITH_EDITORONLY_DATA
	Result["graph_guid"] = TCHAR_TO_UTF8(*Graph->GraphGuid.ToString());

	if (Graph->SubGraphs.Num() > 0)
	{
		sol::table SubGraphsTable = LuaView.create_table();
		int32 SubIdx = 1;
		for (UEdGraph* SubGraph : Graph->SubGraphs)
		{
			if (SubGraph)
			{
				SubGraphsTable[SubIdx++] = TCHAR_TO_UTF8(*SubGraph->GetName());
			}
		}
		Result["subgraphs"] = SubGraphsTable;
	}
#endif
}

// Look up the resolver-assigned friendly name for a graph (e.g. "MaterialGraph" not "MaterialGraph_0")
static FString GetResolverGraphName(UObject* Asset, UEdGraph* Graph)
{
	TArray<FResolvedGraphInfo> AllGraphs = LuaGraphResolver::GetGraphs(Asset);
	for (const FResolvedGraphInfo& G : AllGraphs)
	{
		if (G.Graph == Graph) return G.Name;
	}
	// Fallback to UObject name if not found in resolver
	return Graph->GetName();
}

static TArray<FLuaFunctionDoc> ReadGraphDocs = {
	{
		TEXT("read_graph(asset_path, graph_name?)"),
		TEXT("Read all nodes and connections in any asset's graph.\n"
			"  Works on: Blueprint, Material, BehaviorTree, NiagaraSystem, NiagaraScript, MetaSound, PCG, ControlRig.\n"
			"  graph_name is optional for single-graph assets (Material, BehaviorTree, MetaSound, PCG).\n"
			"  For Blueprints: use graph names like \"EventGraph\", \"MyFunction\", etc.\n"
			"  For NiagaraSystems: \"SystemSpawn\", \"EmitterName/Spawn\", \"EmitterName/ParticleUpdate\", etc.\n"
			"  Once read, all nodes are registered — use connect(), set_pin(), delete_node() on them.\n"
			"  Tip: call read_graph(path) with no graph_name to list available graphs."),
		TEXT("{graph_name, graph_guid, nodes = [{handle, name, type, tooltip, keywords, x, y, enabled_state, comment, error_type, pins_in, pins_out, connections}], subgraphs = [name]}")
	}
};

REGISTER_LUA_BINDING(ReadGraph, ReadGraphDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("read_graph", [&Session](const std::string& AssetPath,
		sol::optional<std::string> GraphNameOpt, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		FString FAssetPath = UTF8_TO_TCHAR(AssetPath.c_str());
		FString FGraphName = GraphNameOpt.has_value()
			? UTF8_TO_TCHAR(GraphNameOpt.value().c_str()) : TEXT("");

		// Normalize path
		if (!FAssetPath.StartsWith(TEXT("/")))
		{
			FAssetPath = TEXT("/Game/") + FAssetPath;
		}

		// Load the asset
		UObject* Asset = LoadObject<UObject>(nullptr, *FAssetPath);
		if (!Asset)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] read_graph(\"%s\") -> asset not found"), *FAssetPath));
			return sol::lua_nil;
		}

		// Find the graph
		UEdGraph* Graph = LuaGraphResolver::FindGraph(Asset, FGraphName);
		if (!Graph)
		{
			FString Available = LuaGraphResolver::ListGraphNames(Asset);
			if (FGraphName.IsEmpty())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] read_graph(\"%s\") -> multiple graphs, specify a name. Available: %s"),
					*FAssetPath, *Available));
			}
			else
			{
				Session.Log(FString::Printf(TEXT("[FAIL] read_graph(\"%s\", \"%s\") -> graph not found. Available: %s"),
					*FAssetPath, *FGraphName, *Available));
			}
			return sol::lua_nil;
		}

		Session.RegisterGraphNodes(Graph);

		sol::table Result = LuaView.create_table();
		sol::table NodesTable = LuaView.create_table();
		int32 NodeIdx = 1;

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;

			FString NodeGuid = Node->NodeGuid.ToString();

			sol::table NodeEntry = LuaView.create_table();
			NodeEntry["handle"] = TCHAR_TO_UTF8(*NodeGuid);
			NodeEntry["name"] = TCHAR_TO_UTF8(*Node->GetNodeTitle(ENodeTitleType::MenuTitle).ToString());
			NodeEntry["type"] = TCHAR_TO_UTF8(*Node->GetClass()->GetName());
			NodeEntry["x"] = Node->NodePosX;
			NodeEntry["y"] = Node->NodePosY;

			// Tooltip and keywords help agents understand unfamiliar node types
			FString Tooltip = Node->GetTooltipText().ToString();
			if (!Tooltip.IsEmpty())
			{
				NodeEntry["tooltip"] = TCHAR_TO_UTF8(*Tooltip);
			}
			FString Keywords = Node->GetKeywords().ToString();
			if (!Keywords.IsEmpty())
			{
				NodeEntry["keywords"] = TCHAR_TO_UTF8(*Keywords);
			}

			if (!Node->NodeComment.IsEmpty())
			{
				NodeEntry["comment"] = TCHAR_TO_UTF8(*Node->NodeComment);
				NodeEntry["comment_visible"] = (bool)Node->bCommentBubbleVisible;
			}

			switch (Node->GetDesiredEnabledState())
			{
			case ENodeEnabledState::Enabled: NodeEntry["enabled_state"] = "enabled"; break;
			case ENodeEnabledState::Disabled: NodeEntry["enabled_state"] = "disabled"; break;
			case ENodeEnabledState::DevelopmentOnly: NodeEntry["enabled_state"] = "development_only"; break;
			}

			if (Node->bHasCompilerMessage)
			{
				NodeEntry["error_type"] = Node->ErrorType;
			}

			sol::table InPins = LuaView.create_table();
			sol::table OutPins = LuaView.create_table();
			sol::table Connections = LuaView.create_table();
			int32 InIdx = 1, OutIdx = 1, ConnIdx = 1;

			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->bHidden) continue;

				sol::table PinInfo = NeoLuaPin::BuildPinTable(LuaView, Pin);

				if (Pin->Direction == EGPD_Input)
					InPins[InIdx++] = PinInfo;
				else
					OutPins[OutIdx++] = PinInfo;

				// Only emit connections from output pins to avoid duplicates
				if (Pin->Direction == EGPD_Output)
				{
					FString PinName = NeoLuaPin::GetUsablePinName(Pin);
					for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
					{
						if (!LinkedPin) continue;
						// Use GetOwningNodeUnchecked — GetOwningNode() has check() that crashes on null
						UEdGraphNode* LinkedNode = LinkedPin->GetOwningNodeUnchecked();
						if (!LinkedNode) continue;

						sol::table Conn = LuaView.create_table();
						Conn["from_node"] = TCHAR_TO_UTF8(*NodeGuid);
						Conn["from_pin"] = TCHAR_TO_UTF8(*PinName);
						Conn["to_node"] = TCHAR_TO_UTF8(*LinkedNode->NodeGuid.ToString());
						Conn["to_pin"] = TCHAR_TO_UTF8(*NeoLuaPin::GetUsablePinName(LinkedPin));

						Connections[ConnIdx++] = Conn;
					}
				}
			}

			NodeEntry["pins_in"] = InPins;
			NodeEntry["pins_out"] = OutPins;
			NodeEntry["connections"] = Connections;

			NodesTable[NodeIdx++] = NodeEntry;
		}

		Result["nodes"] = NodesTable;
		FString FriendlyName = GetResolverGraphName(Asset, Graph);
		Result["graph_name"] = TCHAR_TO_UTF8(*FriendlyName);

		PopulateEditorOnlyGraphData(LuaView, Result, Graph);

		Session.Log(FString::Printf(TEXT("[OK] read_graph(\"%s\", \"%s\") -> %d nodes"),
			*FAssetPath, *FriendlyName, Graph->Nodes.Num()));

		return Result;
	});
});
