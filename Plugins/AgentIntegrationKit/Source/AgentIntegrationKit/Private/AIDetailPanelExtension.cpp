// Copyright 2026 Betide Studio. All Rights Reserved.

#include "AIDetailPanelExtension.h"
#include "ACPAttachmentManager.h"
#include "ACPContextSerializer.h"
#include "AgentIntegrationKitModule.h"

#include "DetailLayoutBuilder.h"
#include "DetailCategoryBuilder.h"
#include "DetailWidgetRow.h"
#include "PropertyEditorModule.h"

#include "GameFramework/Actor.h"
#include "Engine/Blueprint.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Images/SImage.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "AIDetailPanelExtension"

// ============================================================================
// Shared click handler (used by both actor extension and asset customization)
// ============================================================================

static FReply HandleAddAsAIContext(const TArray<TWeakObjectPtr<UObject>>& Objects)
{
	int32 AddedCount = 0;
	FACPAttachmentManager& AttachmentMgr = FACPAttachmentManager::Get();

	for (const TWeakObjectPtr<UObject>& WeakObj : Objects)
	{
		UObject* Obj = WeakObj.Get();
		if (!Obj)
		{
			continue;
		}

		if (AActor* Actor = Cast<AActor>(Obj))
		{
			AttachmentMgr.AddActorContext(Actor);
		}
		else if (UBlueprint* Blueprint = Cast<UBlueprint>(Obj))
		{
			AttachmentMgr.AddBlueprintAsset(Blueprint);
		}
		else
		{
			AttachmentMgr.AddObjectContext(Obj);
		}

		AddedCount++;
	}

	if (AddedCount > 0)
	{
		// Open the Agent Chat window
		FGlobalTabmanager::Get()->TryInvokeTab(FName(TEXT("AgentChat")));

		// Brief toast
		FNotificationInfo Info(FText::Format(
			LOCTEXT("ContextAdded", "Added {0} {0}|plural(one=object,other=objects) as AI context"),
			FText::AsNumber(AddedCount)));
		Info.ExpireDuration = 2.0f;
		Info.bUseLargeFont = false;
		FSlateNotificationManager::Get().AddNotification(Info);
	}

	return FReply::Handled();
}

// ============================================================================
// FAIDetailPanelExtension — registration
// ============================================================================

FDelegateHandle FAIDetailPanelExtension::ActorExtendDelegateHandle;

// Asset classes verified to NOT have engine detail customizations.
static const TArray<FName> GAssetClassNames = {
	TEXT("Material"),
	TEXT("MaterialFunction"),
	TEXT("Texture2D"),
	TEXT("TextureCube"),
	TEXT("TextureRenderTarget2D"),
	TEXT("StaticMesh"),
	TEXT("DataAsset"),
	TEXT("DataTable"),
	TEXT("CurveTable"),
	TEXT("Blueprint"),
	TEXT("WidgetBlueprint"),
	TEXT("LevelSequence"),
	TEXT("BehaviorTree"),
	TEXT("BlackboardData"),
	TEXT("PhysicalMaterial"),
};

void FAIDetailPanelExtension::Register()
{
	ActorExtendDelegateHandle = OnExtendActorDetails.AddStatic(&FAIDetailPanelExtension::OnExtendActorDetailsCB);

	FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	for (const FName& ClassName : GAssetClassNames)
	{
		PropertyModule.RegisterCustomClassLayout(
			ClassName,
			FOnGetDetailCustomizationInstance::CreateStatic(&FAIObjectDetailCustomization::MakeInstance)
		);
	}
}

void FAIDetailPanelExtension::Unregister()
{
	if (ActorExtendDelegateHandle.IsValid())
	{
		OnExtendActorDetails.Remove(ActorExtendDelegateHandle);
		ActorExtendDelegateHandle.Reset();
	}

	if (FModuleManager::Get().IsModuleLoaded("PropertyEditor"))
	{
		FPropertyEditorModule& PropertyModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
		for (const FName& ClassName : GAssetClassNames)
		{
			PropertyModule.UnregisterCustomClassLayout(ClassName);
		}
	}
}

// ============================================================================
// Actor extension callback
// ============================================================================

void FAIDetailPanelExtension::OnExtendActorDetailsCB(
	IDetailLayoutBuilder& DetailBuilder,
	const FGetSelectedActors& GetSelectedActors)
{
	TArray<TWeakObjectPtr<UObject>> Objects;
	if (GetSelectedActors.IsBound())
	{
		const TArray<TWeakObjectPtr<AActor>>& Actors = GetSelectedActors.Execute();
		for (const TWeakObjectPtr<AActor>& WeakActor : Actors)
		{
			if (WeakActor.IsValid())
			{
				Objects.Add(WeakActor);
			}
		}
	}

	if (Objects.Num() == 0)
	{
		return;
	}

	// Variable = highest sort priority — appears at the very top
	IDetailCategoryBuilder& AICategory = DetailBuilder.EditCategory(
		TEXT("AI Assistant"),
		LOCTEXT("AICategoryDisplayName", "AI Assistant"),
		ECategoryPriority::Variable);

	AddAIContextButton(AICategory, Objects);
}

// ============================================================================
// Shared button builder — compact header button at top of Details panel
// ============================================================================

void FAIDetailPanelExtension::AddAIContextButton(
	IDetailCategoryBuilder& Category,
	TArray<TWeakObjectPtr<UObject>> Objects)
{
	if (Objects.Num() == 0)
	{
		return;
	}

	FText ButtonTooltip;
	if (Objects.Num() == 1)
	{
		ButtonTooltip = LOCTEXT("AddSingleContextTooltip",
			"Serialize this object and attach as context for the AI chat window.");
	}
	else
	{
		ButtonTooltip = FText::Format(
			LOCTEXT("AddMultiContextTooltip", "Serialize {0} selected objects and attach as AI context."),
			FText::AsNumber(Objects.Num()));
	}

	// Single compact row — button aligned right, minimal padding
	Category.AddCustomRow(LOCTEXT("AIContextSearch", "AI Context NeoStack Agent"))
		.WholeRowContent()
		[
			SNew(SBox)
			.Padding(FMargin(4.0f, 2.0f))
			.HAlign(HAlign_Right)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ContentPadding(FMargin(6.0f, 2.0f))
				.ToolTipText(ButtonTooltip)
				.OnClicked_Lambda([Objects]() -> FReply
				{
					return HandleAddAsAIContext(Objects);
				})
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 4.0f, 0.0f)
					[
						SNew(SImage)
						.Image(FAppStyle::GetBrush("Icons.Plus"))
						.DesiredSizeOverride(FVector2D(12.0f, 12.0f))
						.ColorAndOpacity(FSlateColor::UseForeground())
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("AddAsAIContext", "Add as AI Context"))
						.TextStyle(FAppStyle::Get(), "SmallButtonText")
						.ColorAndOpacity(FSlateColor::UseForeground())
					]
				]
			]
		];
}

// ============================================================================
// FAIObjectDetailCustomization — asset classes
// ============================================================================

TSharedRef<IDetailCustomization> FAIObjectDetailCustomization::MakeInstance()
{
	return MakeShareable(new FAIObjectDetailCustomization);
}

void FAIObjectDetailCustomization::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	DetailBuilder.GetObjectsBeingCustomized(SelectedObjectsList);
	TArray<TWeakObjectPtr<UObject>> ValidObjects;
	for (const TWeakObjectPtr<UObject>& Obj : SelectedObjectsList)
	{
		if (Obj.IsValid())
		{
			ValidObjects.Add(Obj);
		}
	}

	if (ValidObjects.Num() > 0)
	{
		IDetailCategoryBuilder& AICategory = DetailBuilder.EditCategory(
			TEXT("AI Assistant"),
			LOCTEXT("AICategoryDisplayName", "AI Assistant"),
			ECategoryPriority::Variable);

		FAIDetailPanelExtension::AddAIContextButton(AICategory, ValidObjects);
	}
}

#undef LOCTEXT_NAMESPACE
