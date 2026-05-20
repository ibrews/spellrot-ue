#include "Lua/LuaBindingRegistry.h"
#include "AgentIntegrationKitModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "UObject/UObjectIterator.h"

FLuaBindingRegistry& FLuaBindingRegistry::Get()
{
	static FLuaBindingRegistry Instance;
	return Instance;
}

void FLuaBindingRegistry::Register(const FString& Name, TArray<FLuaFunctionDoc> Functions, FLuaBindingFunc BindFunc)
{
	FLuaBinding Binding;
	Binding.Name = Name;
	Binding.Functions = MoveTemp(Functions);
	Binding.Bind = MoveTemp(BindFunc);
	Bindings.Add(MoveTemp(Binding));
}

FString FLuaBindingRegistry::BuildDescription() const
{
	// Core bindings — always included in prompt
	static const TSet<FString> CoreBindings = {
		TEXT("OpenAsset"), TEXT("CreateBlueprint"), TEXT("Explore"),
		TEXT("AddNode"), TEXT("DeleteNode"), TEXT("Connect"), TEXT("SetPin"),
		TEXT("FindNodes"), TEXT("ReadGraph"), TEXT("GraphOps"), TEXT("ControlRigGraphOps"),
		TEXT("AddVariable"), TEXT("ReadFile"), TEXT("ReadLog"), TEXT("Profile"),
		TEXT("ExecutePython")
	};

	FString Desc = TEXT("Execute a Lua script that interacts with Unreal Editor.\n\nIMPORTANT: Before attempting any operation, call help() or help('domain') to discover the correct function names and parameter syntax.\n\nAvailable functions:\n");
	TArray<FString> DiscoverableDomains;

	for (const FLuaBinding& Binding : Bindings)
	{
		if (Binding.Functions.Num() == 0)
		{
			continue; // Skip enrichments (empty docs)
		}

		if (CoreBindings.Contains(Binding.Name))
		{
			// Emit full docs for core bindings
			for (const FLuaFunctionDoc& Func : Binding.Functions)
			{
				Desc += FString::Printf(TEXT("\n  %s\n    %s\n"), *Func.Signature, *Func.Description);
				if (!Func.Returns.IsEmpty())
				{
					Desc += FString::Printf(TEXT("    Returns: %s\n"), *Func.Returns);
				}
			}
		}
		else
		{
			// Non-core with docs — collect for discoverable list
			DiscoverableDomains.Add(Binding.Name);
		}
	}

	Desc += TEXT("\n  help(domain?) — list functions for a domain, or all available domains\n    Returns: string\n");
	Desc += TEXT("\n  log(msg) — output to trace\n  print(...) — output to trace\n");

	if (DiscoverableDomains.Num() > 0)
	{
		DiscoverableDomains.Sort();
		Desc += TEXT("\nAdditional domains available (call help('domain_name') for functions):\n  ");
		Desc += FString::Join(DiscoverableDomains, TEXT(", "));
		Desc += TEXT("\n");
	}

	// Append plugin integration status so agents know what's available
	const auto& Statuses = FAgentIntegrationKitModule::GetExtensionStatuses();
	if (Statuses.Num() > 0)
	{
		TArray<FString> Active, Unavailable;
		for (const auto& S : Statuses)
		{
			if (S.bLoaded) Active.Add(S.DisplayName);
			else Unavailable.Add(S.DisplayName);
		}
		Active.Sort();
		Unavailable.Sort();

		if (Active.Num() > 0)
		{
			Desc += TEXT("\nActive plugin integrations: ");
			Desc += FString::Join(Active, TEXT(", "));
			Desc += TEXT("\n");
		}
		if (Unavailable.Num() > 0)
		{
			Desc += TEXT("Unavailable plugins (not enabled in project): ");
			Desc += FString::Join(Unavailable, TEXT(", "));
			Desc += TEXT("\n");
		}
	}

	return Desc;
}

void FLuaSessionData::RegisterGraphNodes(UEdGraph* Graph)
{
	if (!Graph || LoadedGraphs.Contains(Graph)) return;
	LoadedGraphs.Add(Graph);

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;
		FString Guid = Node->NodeGuid.ToString();
		if (!Nodes.Contains(Guid))
		{
			Nodes.Add(Guid, Node);
		}
	}
}

UEdGraphNode* FLuaSessionData::FindNode(const FString& Handle)
{
	// Fast path: check session-local map
	if (TWeakObjectPtr<UEdGraphNode>* Found = Nodes.Find(Handle))
	{
		if (Found->IsValid())
		{
			return Found->Get();
		}
		// Node was deleted externally (undo, user action, GC) — remove stale entry
		Nodes.Remove(Handle);
	}

	// Fallback: scan all loaded UEdGraph objects for a node with matching NodeGuid.
	// This handles cross-session lookups where a previous execute_script call registered
	// the node but the current session has a fresh Nodes map. NodeGuid is globally unique.
	FGuid TargetGuid;
	if (!FGuid::Parse(Handle, TargetGuid))
	{
		return nullptr;
	}

	for (TObjectIterator<UEdGraphNode> It; It; ++It)
	{
		UEdGraphNode* Node = *It;
		if (Node && Node->NodeGuid == TargetGuid)
		{
			// Cache it for future lookups in this session
			Nodes.Add(Handle, Node);
			return Node;
		}
	}

	return nullptr;
}

void FLuaSessionData::MarkGraphDirty(UEdGraph* Graph)
{
	if (!Graph) return;
	// Walk outer chain to find the owning asset
	UObject* Outer = Graph->GetOuter();
	while (Outer)
	{
		if (Outer->IsAsset())
		{
			MarkGraphDirty(Graph, Outer);
			return;
		}
		Outer = Outer->GetOuter();
	}
	// Fallback: use direct outer
	MarkGraphDirty(Graph, Graph->GetOuter());
}
