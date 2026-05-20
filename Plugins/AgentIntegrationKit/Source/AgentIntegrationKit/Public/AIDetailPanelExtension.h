// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"
#include "ActorDetailsDelegates.h"

class IDetailLayoutBuilder;
class IDetailCategoryBuilder;

/**
 * Provides "Add as AI Context" buttons across the editor's Details panels.
 *
 * Two mechanisms for maximum coverage:
 *
 * 1. OnExtendActorDetails delegate — adds the button to every actor's
 *    Details panel (works alongside engine's FActorDetails, no replacement).
 *
 * 2. IDetailCustomization registered for specific asset classes (Material,
 *    Texture2D, StaticMesh, DataAsset, Blueprint, etc.) — covers common
 *    asset types that don't have engine detail customizations.
 */
class FAIDetailPanelExtension
{
public:
	/** Register both extension mechanisms. Call from module startup. */
	static void Register();

	/** Unregister both. Call from module shutdown. */
	static void Unregister();

	/** Build the "Add as AI Context" button row. Used by both mechanisms. */
	static void AddAIContextButton(IDetailCategoryBuilder& Category, TArray<TWeakObjectPtr<UObject>> Objects);

private:
	static void OnExtendActorDetailsCB(IDetailLayoutBuilder& DetailBuilder, const FGetSelectedActors& GetSelectedActors);
	static FDelegateHandle ActorExtendDelegateHandle;
};

/**
 * IDetailCustomization registered for specific asset classes.
 * Only adds the "AI Assistant" category — all default property rows are preserved.
 */
class FAIObjectDetailCustomization : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance();
	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;

private:
	TArray<TWeakObjectPtr<UObject>> SelectedObjectsList;
};
