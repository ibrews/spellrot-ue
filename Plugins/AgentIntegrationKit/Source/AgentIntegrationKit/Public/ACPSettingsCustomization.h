// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"

/**
 * Detail customization for UACPSettings.
 * Adds descriptive header text to categories in the Project Settings panel.
 */
class FACPSettingsCustomization : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance();
	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;

	/** Call from module startup to register this customization */
	static void Register();

	/** Call from module shutdown to unregister */
	static void Unregister();
};
