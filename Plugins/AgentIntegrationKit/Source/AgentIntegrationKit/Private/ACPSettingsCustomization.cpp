// Copyright 2026 Betide Studio. All Rights Reserved.

#include "ACPSettingsCustomization.h"
#include "ACPSettings.h"
#include "DetailLayoutBuilder.h"
#include "DetailCategoryBuilder.h"
#include "DetailWidgetRow.h"
#include "IDetailGroup.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Input/SButton.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateColor.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "IDetailPropertyRow.h"

#define LOCTEXT_NAMESPACE "ACPSettingsCustomization"

TSharedRef<IDetailCustomization> FACPSettingsCustomization::MakeInstance()
{
	return MakeShareable(new FACPSettingsCustomization);
}

namespace
{
	/** Helper to add an italic description row at the top of a category */
	void AddCategoryDescription(IDetailCategoryBuilder& Category, const FText& SearchText, const FText& Description)
	{
		Category.AddCustomRow(SearchText)
			.WholeRowContent()
			[
				SNew(SBox)
				.Padding(FMargin(0.0f, 0.0f, 0.0f, 4.0f))
				[
					SNew(STextBlock)
					.Text(Description)
					.Font(FCoreStyle::GetDefaultFontStyle("Italic", 9))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
				]
			];
	}

	/**
	 * Pipe-separated UPROPERTY subcategories (e.g. "Parent | Child") live in a separate
	 * SubCategoryMap inside the detail layout builder. EditCategory() cannot reference them —
	 * it creates a NEW top-level entry in CustomCategoryMap instead, while the parent category
	 * still renders the subcategory via a fallback path, causing duplication.
	 *
	 * Workaround: hide properties from the auto-generated subcategory, then re-create
	 * them inside an IDetailGroup on the parent category with Category display mode.
	 * The group renders identically to a native subcategory, supports description text,
	 * and stays nested under the parent — no duplication.
	 */
	void CustomizeSubcategory(
		IDetailLayoutBuilder& DetailBuilder,
		IDetailCategoryBuilder& ParentCategory,
		const FName& GroupName,
		const FText& DisplayName,
		const TArray<FName>& PropertyNames,
		const FText& Description)
	{
		// 1. Hide all properties from the auto-generated subcategory
		TArray<TSharedPtr<IPropertyHandle>> Handles;
		Handles.Reserve(PropertyNames.Num());
		for (const FName& PropName : PropertyNames)
		{
			TSharedPtr<IPropertyHandle> Handle = DetailBuilder.GetProperty(PropName);
			DetailBuilder.HideProperty(Handle);
			Handles.Add(Handle);
		}

		// 2. Create a group styled as a subcategory (identical look to native pipe subcategories)
		IDetailGroup& Group = ParentCategory.AddGroup(GroupName, DisplayName, /*bForAdvanced*/ false, /*bStartExpanded*/ false);
		Group.SetDisplayMode(EDetailGroupDisplayMode::Category);

		// 3. Add italic description as the first widget row
		Group.AddWidgetRow()
			.WholeRowContent()
			[
				SNew(SBox)
				.Padding(FMargin(0.0f, 0.0f, 0.0f, 4.0f))
				[
					SNew(STextBlock)
					.Text(Description)
					.Font(FCoreStyle::GetDefaultFontStyle("Italic", 9))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
				]
			];

		// 4. Re-add each property to the group
		for (const TSharedPtr<IPropertyHandle>& Handle : Handles)
		{
			if (Handle.IsValid())
			{
				Group.AddPropertyRow(Handle.ToSharedRef());
			}
		}
	}
}

void FACPSettingsCustomization::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	// Set category ordering via priority (Auto-Update first so users see token/credits setup immediately)
	DetailBuilder.EditCategory("Auto-Update", FText::GetEmpty(), ECategoryPriority::Important);
	DetailBuilder.EditCategory("General", FText::GetEmpty(), ECategoryPriority::Important);

	// ACP Agents - parent category with description + subcategory groups
	IDetailCategoryBuilder& ACPCategory = DetailBuilder.EditCategory("ACP Agents");
	AddCategoryDescription(ACPCategory,
		LOCTEXT("ACPAgentsSearch", "ACP Agents"),
		LOCTEXT("ACPAgentsDesc",
			"ACP (Agent Client Protocol) agents are external CLI tools (Claude Code, Gemini CLI, Codex, etc.) "
			"that the plugin spawns as subprocesses and communicates with over stdin/stdout."));

	// Agent Process Overrides (Advanced) — group styled as subcategory
	{
		const TArray<FName> Props = {
			GET_MEMBER_NAME_CHECKED(UACPSettings, ClaudeCodePath),
			GET_MEMBER_NAME_CHECKED(UACPSettings, GeminiCliPath),
			GET_MEMBER_NAME_CHECKED(UACPSettings, CodexCliPath),
			GET_MEMBER_NAME_CHECKED(UACPSettings, OpenCodePath),
			GET_MEMBER_NAME_CHECKED(UACPSettings, CursorAgentPath),
			GET_MEMBER_NAME_CHECKED(UACPSettings, KimiCliPath),
			GET_MEMBER_NAME_CHECKED(UACPSettings, CopilotCliPath),
			GET_MEMBER_NAME_CHECKED(UACPSettings, BunOverridePath),
		};

		CustomizeSubcategory(DetailBuilder, ACPCategory,
			FName(TEXT("AgentProcessOverrides")),
			LOCTEXT("PathOverridesDisplayName", "Agent Process Overrides (Advanced)"),
			Props,
			LOCTEXT("PathOverridesDesc",
				"Advanced overrides for the process spawned by each agent session. Leave these empty for normal setup. "
				"Most users should not change these fields."));
	}

	// Claude Setup — group styled as subcategory
	{
		const TArray<FName> Props = {
			GET_MEMBER_NAME_CHECKED(UACPSettings, ClaudeCodeExecutablePath),
			GET_MEMBER_NAME_CHECKED(UACPSettings, bInstallClaudeInProcessFirst),
			GET_MEMBER_NAME_CHECKED(UACPSettings, bAutoSaveClaudeCodeExecutablePathAfterInstall),
		};

		CustomizeSubcategory(DetailBuilder, ACPCategory,
			FName(TEXT("ClaudeSetup")),
			LOCTEXT("ClaudeSetupDisplayName", "Claude Setup"),
			Props,
			LOCTEXT("ClaudeSetupDesc",
				"Controls first-time Claude setup. The bundled claude-code-acp adapter can use an explicit Claude executable "
				"path (CLAUDE_CODE_EXECUTABLE) so users do not need to restart the editor after installer PATH changes."));
	}

	// MCP Server - add description
	IDetailCategoryBuilder& MCPCategory = DetailBuilder.EditCategory("MCP Server");
	AddCategoryDescription(MCPCategory,
		LOCTEXT("MCPServerSearch", "MCP Server"),
		LOCTEXT("MCPServerDesc",
			"The MCP (Model Context Protocol) server exposes the plugin's tools over HTTP, allowing any "
			"MCP-compatible agent to connect. The server starts automatically (default port 9315). Check the status bar for the active port."));

	// Auto-Update - add description and "Generate Token" button
	IDetailCategoryBuilder& AutoUpdateCategory = DetailBuilder.EditCategory("Auto-Update");
	AddCategoryDescription(AutoUpdateCategory,
		LOCTEXT("AutoUpdateSearch", "Auto-Update"),
		LOCTEXT("AutoUpdateDesc",
			"Download plugin updates directly from betide.studio and optionally route OpenRouter/Meshy/fal.ai requests "
			"through betide.studio proxies to use NeoStack credits. You can set the token in Project Settings or via BETIDE_API_TOKEN environment variable."));

	// Add a "Generate Token" button next to the BetideApiToken property
	TSharedPtr<IPropertyHandle> TokenProp = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UACPSettings, BetideApiToken));
	if (TokenProp.IsValid())
	{
		IDetailPropertyRow& TokenRow = AutoUpdateCategory.AddProperty(TokenProp);
		TokenRow.CustomWidget()
			.NameContent()
			[
				TokenProp->CreatePropertyNameWidget()
			]
			.ValueContent()
			.MaxDesiredWidth(400.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					TokenProp->CreatePropertyValueWidget()
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(4.0f, 0.0f, 0.0f, 0.0f)
				.VAlign(VAlign_Center)
				[
					SNew(SButton)
					.Text(LOCTEXT("GenerateToken", "Get Token"))
					.ToolTipText(LOCTEXT("GenerateTokenTip", "Open betide.studio NeoStack dashboard to generate an API token"))
					.OnClicked_Lambda([]()
					{
						FPlatformProcess::LaunchURL(TEXT("https://betide.studio/dashboard/neostack"), nullptr, nullptr);
						return FReply::Handled();
					})
				]
			];
	}

	// Ensure remaining categories appear in order
	DetailBuilder.EditCategory("Tools");
	DetailBuilder.EditCategory("Debug");
}

void FACPSettingsCustomization::Register()
{
	FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	PropertyModule.RegisterCustomClassLayout(
		UACPSettings::StaticClass()->GetFName(),
		FOnGetDetailCustomizationInstance::CreateStatic(&FACPSettingsCustomization::MakeInstance)
	);
}

void FACPSettingsCustomization::Unregister()
{
	if (FModuleManager::Get().IsModuleLoaded("PropertyEditor"))
	{
		FPropertyEditorModule& PropertyModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
		PropertyModule.UnregisterCustomClassLayout(UACPSettings::StaticClass()->GetFName());
	}
}

#undef LOCTEXT_NAMESPACE
