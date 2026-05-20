// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Providers/GenerativeProvider.h"

/**
 * Singleton registry for all generative providers (Meshy, Tripo, fal.ai, ElevenLabs, etc.)
 * Providers self-register via REGISTER_GENERATIVE_PROVIDER macro.
 */
class AGENTINTEGRATIONKIT_API FGenerativeProviderRegistry
{
public:
	static FGenerativeProviderRegistry& Get();

	/** Register a provider. Called automatically by REGISTER_GENERATIVE_PROVIDER. */
	void Register(TSharedRef<IGenerativeProvider> Provider);

	/** Find provider by ID ("meshy", "tripo", etc.) */
	TSharedPtr<IGenerativeProvider> Find(const FString& ProviderId) const;

	/** Find all providers that support a given action ID */
	TArray<TSharedRef<IGenerativeProvider>> FindByAction(const FString& ActionId) const;

	/** Find all providers whose actions match an output hint (e.g., "model", "audio", "image") */
	TArray<TSharedRef<IGenerativeProvider>> FindByOutputHint(const FString& OutputHint) const;

	/** Get all registered providers */
	const TArray<TSharedRef<IGenerativeProvider>>& GetAll() const { return Providers; }

	/** Get all action descriptors across all providers, optionally filtered */
	TArray<TPair<FString, FProviderActionDescriptor>> GetAllActions(
		const FString& OutputHintFilter = TEXT("")) const;

	/** Get count */
	int32 Num() const { return Providers.Num(); }

private:
	TArray<TSharedRef<IGenerativeProvider>> Providers;
	TMap<FString, int32> ProviderIdIndex; // ProviderId → index in Providers array
};

