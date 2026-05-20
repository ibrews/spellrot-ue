// Copyright 2025 Betide Studio. All Rights Reserved.

#include "Tools/NodeNameRegistry.h"
#include "AgentIntegrationKitModule.h"

FNodeNameRegistry& FNodeNameRegistry::Get()
{
	static FNodeNameRegistry Instance;
	return Instance;
}

FString FNodeNameRegistry::MakeKey(const FString& AssetPath, const FString& GraphName, const FString& NodeName)
{
	return FString::Printf(TEXT("%s|%s|%s"), *AssetPath, *GraphName, *NodeName);
}

bool FNodeNameRegistry::ParseKey(const FString& Key, FString& OutAssetPath, FString& OutGraphName, FString& OutNodeName)
{
	TArray<FString> Parts;
	Key.ParseIntoArray(Parts, TEXT("|"));

	if (Parts.Num() != 3)
	{
		return false;
	}

	OutAssetPath = Parts[0];
	OutGraphName = Parts[1];
	OutNodeName = Parts[2];
	return true;
}

void FNodeNameRegistry::Register(const FString& AssetPath, const FString& GraphName,
                                  const FString& NodeName, const FGuid& NodeGuid)
{
	FString Key = MakeKey(AssetPath, GraphName, NodeName);

	// Add or replace
	if (Registry.Contains(Key))
	{
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("[NodeNameRegistry] Replacing: %s -> %s"), *NodeName, *NodeGuid.ToString());
	}
	else
	{
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("[NodeNameRegistry] Registering: %s -> %s"), *NodeName, *NodeGuid.ToString());
	}

	Registry.Add(Key, NodeGuid);
}

FGuid FNodeNameRegistry::Resolve(const FString& AssetPath, const FString& GraphName,
                                  const FString& NodeName) const
{
	FString Key = MakeKey(AssetPath, GraphName, NodeName);

	const FGuid* Found = Registry.Find(Key);
	if (Found)
	{
		return *Found;
	}

	return FGuid(); // Invalid GUID
}

bool FNodeNameRegistry::IsRegistered(const FString& AssetPath, const FString& GraphName,
                                      const FString& NodeName) const
{
	FString Key = MakeKey(AssetPath, GraphName, NodeName);
	return Registry.Contains(Key);
}

void FNodeNameRegistry::Unregister(const FString& AssetPath, const FString& GraphName,
                                    const FString& NodeName)
{
	FString Key = MakeKey(AssetPath, GraphName, NodeName);
	Registry.Remove(Key);
}

void FNodeNameRegistry::ClearGraph(const FString& AssetPath, const FString& GraphName)
{
	FString Prefix = FString::Printf(TEXT("%s|%s|"), *AssetPath, *GraphName);

	TArray<FString> KeysToRemove;
	for (const auto& Pair : Registry)
	{
		if (Pair.Key.StartsWith(Prefix))
		{
			KeysToRemove.Add(Pair.Key);
		}
	}

	for (const FString& Key : KeysToRemove)
	{
		Registry.Remove(Key);
	}

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("[NodeNameRegistry] Cleared %d entries for %s:%s"),
	       KeysToRemove.Num(), *AssetPath, *GraphName);
}

void FNodeNameRegistry::ClearAsset(const FString& AssetPath)
{
	FString Prefix = AssetPath + TEXT("|");

	TArray<FString> KeysToRemove;
	for (const auto& Pair : Registry)
	{
		if (Pair.Key.StartsWith(Prefix))
		{
			KeysToRemove.Add(Pair.Key);
		}
	}

	for (const FString& Key : KeysToRemove)
	{
		Registry.Remove(Key);
	}

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("[NodeNameRegistry] Cleared %d entries for %s"),
	       KeysToRemove.Num(), *AssetPath);
}

void FNodeNameRegistry::ClearAll()
{
	int32 Count = Registry.Num();
	Registry.Empty();
	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("[NodeNameRegistry] Cleared all %d entries"), Count);
}
