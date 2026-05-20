// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Providers/GenerativeProviderRegistry.h"

// ── Singleton ────────────────────────────────────────────────────────

FGenerativeProviderRegistry& FGenerativeProviderRegistry::Get()
{
	static FGenerativeProviderRegistry Instance;
	return Instance;
}

// ── Registration ─────────────────────────────────────────────────────

void FGenerativeProviderRegistry::Register(TSharedRef<IGenerativeProvider> Provider)
{
	const FString Id = Provider->GetId();
	if (ProviderIdIndex.Contains(Id))
	{
		UE_LOG(LogTemp, Warning, TEXT("[GenerativeProviderRegistry] Overwriting provider '%s'"), *Id);
		int32 Idx = ProviderIdIndex[Id];
		Providers[Idx] = Provider;
	}
	else
	{
		ProviderIdIndex.Add(Id, Providers.Num());
		Providers.Add(Provider);
		UE_LOG(LogTemp, Log, TEXT("[GenerativeProviderRegistry] Registered provider '%s' (%s) with %d actions"),
			*Id, *Provider->GetDisplayName(), Provider->GetActions().Num());
	}
}

// ── Lookup ───────────────────────────────────────────────────────────

TSharedPtr<IGenerativeProvider> FGenerativeProviderRegistry::Find(const FString& ProviderId) const
{
	const int32* Idx = ProviderIdIndex.Find(ProviderId.ToLower());
	if (Idx)
	{
		return Providers[*Idx];
	}
	return nullptr;
}

TArray<TSharedRef<IGenerativeProvider>> FGenerativeProviderRegistry::FindByAction(const FString& ActionId) const
{
	TArray<TSharedRef<IGenerativeProvider>> Result;
	for (const auto& Provider : Providers)
	{
		if (Provider->SupportsAction(ActionId))
		{
			Result.Add(Provider);
		}
	}
	return Result;
}

TArray<TSharedRef<IGenerativeProvider>> FGenerativeProviderRegistry::FindByOutputHint(const FString& OutputHint) const
{
	TArray<TSharedRef<IGenerativeProvider>> Result;
	const FString HintLower = OutputHint.ToLower();
	for (const auto& Provider : Providers)
	{
		for (const auto& Action : Provider->GetActions())
		{
			for (const auto& Hint : Action.OutputHints)
			{
				if (Hint.ToLower() == HintLower)
				{
					Result.AddUnique(Provider);
					break;
				}
			}
		}
	}
	return Result;
}

TArray<TPair<FString, FProviderActionDescriptor>> FGenerativeProviderRegistry::GetAllActions(
	const FString& OutputHintFilter) const
{
	TArray<TPair<FString, FProviderActionDescriptor>> Result;
	const FString FilterLower = OutputHintFilter.ToLower();

	for (const auto& Provider : Providers)
	{
		for (const auto& Action : Provider->GetActions())
		{
			if (FilterLower.IsEmpty())
			{
				Result.Add(TPair<FString, FProviderActionDescriptor>(Provider->GetId(), Action));
			}
			else
			{
				for (const auto& Hint : Action.OutputHints)
				{
					if (Hint.ToLower() == FilterLower)
					{
						Result.Add(TPair<FString, FProviderActionDescriptor>(Provider->GetId(), Action));
						break;
					}
				}
			}
		}
	}
	return Result;
}

// ── Deferred registration ────────────────────────────────────────────

FDeferredProviderRegistration& FDeferredProviderRegistration::Get()
{
	static FDeferredProviderRegistration Instance;
	return Instance;
}

void FDeferredProviderRegistration::Add(TFunction<void()> Func)
{
	if (bExecuted)
	{
		// Module already initialized, register immediately
		Func();
	}
	else
	{
		PendingRegistrations.Add(MoveTemp(Func));
	}
}

void FDeferredProviderRegistration::ExecuteAll()
{
	for (const auto& Func : PendingRegistrations)
	{
		Func();
	}
	PendingRegistrations.Empty();
	bExecuted = true;
}
