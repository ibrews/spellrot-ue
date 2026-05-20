// Copyright 2025-2026 Betide Studio. All Rights Reserved.
// Lightweight header for extension modules to register graph resolvers.
// Does NOT include asset-specific headers (BehaviorTree, MetaSound, PCG, etc.)

#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraph.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Framework/Application/SlateApplication.h"
#include "RenderingThread.h"

struct FResolvedGraphInfo
{
	FString Name;
	UEdGraph* Graph;

	FResolvedGraphInfo() : Graph(nullptr) {}
	FResolvedGraphInfo(const FString& InName, UEdGraph* InGraph) : Name(InName), Graph(InGraph) {}
};

namespace LuaGraphResolver
{

// Extension point: modules like AIK_Niagara register a callback to resolve graphs
// for asset types not handled by the main module (e.g. WITH_NIAGARA=0 in main module).
// The callback receives the asset and appends resolved graphs to the result array.
// Returns true if it handled the asset (caller should stop), false to continue default resolution.
using FExternalResolverFunc = TFunction<bool(UObject* Asset, TArray<FResolvedGraphInfo>& OutGraphs)>;

// Storage lives in the main module's .cpp to avoid cross-DLL static duplication.
AGENTINTEGRATIONKIT_API TArray<FExternalResolverFunc>& GetExternalResolvers();
AGENTINTEGRATIONKIT_API void RegisterExternalResolver(FExternalResolverFunc Resolver);

// Helper: block until a predicate returns true (pumps Slate so editor init completes).
inline bool WaitForGraph(TFunctionRef<bool()> Predicate, float TimeoutSeconds = 2.0f, float StepSeconds = 0.05f)
{
	const int32 MaxSteps = FMath::Max(1, FMath::CeilToInt(TimeoutSeconds / StepSeconds));
	for (int32 Step = 0; Step < MaxSteps; ++Step)
	{
		if (Predicate())
		{
			return true;
		}
		FPlatformProcess::Sleep(StepSeconds);
	}
	return Predicate();
}

// Helper: open an asset editor and wait for the graph to become available.
inline UEdGraph* EnsureEditorGraphViaEditorOpen(UObject* Asset, TFunctionRef<UEdGraph*(UObject*)> GraphGetter)
{
	if (UEdGraph* Existing = GraphGetter(Asset))
	{
		return Existing;
	}

	if (!GEditor) return nullptr;

	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!AssetEditorSubsystem) return nullptr;

	AssetEditorSubsystem->OpenEditorForAsset(Asset);

	UEdGraph* Result = nullptr;
	WaitForGraph([&]()
	{
		Result = GraphGetter(Asset);
		return Result != nullptr;
	});

	return Result;
}

} // namespace LuaGraphResolver
