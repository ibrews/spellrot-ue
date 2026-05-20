// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Tools/NeoStackToolUtils.h"

#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/PanelSlot.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/VerticalBox.h"
#include "Components/HorizontalBox.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "ScopedTransaction.h"
#include "Animation/WidgetAnimation.h"
#include "Animation/WidgetAnimationBinding.h"
#include "Components/NamedSlot.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "WidgetBlueprintEditorUtils.h"
#include "MovieScene.h"
#include "MovieSceneTrack.h"
#include "MovieSceneSection.h"
#include "Blueprint/WidgetNavigation.h"
#include "BaseWidgetBlueprint.h"

#include <functional>

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// Helpers
// ============================================================================

namespace WidgetHelper
{

// Build a Lua table describing one widget
static sol::table WidgetToTable(sol::state_view& Lua, UWidget* Widget, UWidgetBlueprint* WidgetBP)
{
	sol::table W = Lua.create_table();
	W["name"] = TCHAR_TO_UTF8(*Widget->GetName());
	W["type"] = TCHAR_TO_UTF8(*Widget->GetClass()->GetName());
	W["is_variable"] = (bool)Widget->bIsVariable;
	W["visible"] = Widget->IsVisible();
	W["is_panel"] = Widget->IsA<UPanelWidget>();

	// Visibility enum
	switch (Widget->GetVisibility())
	{
	case ESlateVisibility::Visible:           W["visibility"] = "Visible"; break;
	case ESlateVisibility::Collapsed:         W["visibility"] = "Collapsed"; break;
	case ESlateVisibility::Hidden:            W["visibility"] = "Hidden"; break;
	case ESlateVisibility::HitTestInvisible:  W["visibility"] = "HitTestInvisible"; break;
	case ESlateVisibility::SelfHitTestInvisible: W["visibility"] = "SelfHitTestInvisible"; break;
	}

	// Parent info
	int32 ChildIndex = INDEX_NONE;
	UPanelWidget* Parent = UWidgetTree::FindWidgetParent(Widget, ChildIndex);
	if (Parent)
	{
		W["parent"] = TCHAR_TO_UTF8(*Parent->GetName());
		W["child_index"] = ChildIndex;
	}
	else if (WidgetBP && WidgetBP->WidgetTree && Widget == WidgetBP->WidgetTree->RootWidget)
	{
		W["parent"] = "ROOT";
		W["child_index"] = 0;
	}

	// Slot info
	if (Widget->Slot)
	{
		W["slot_type"] = TCHAR_TO_UTF8(*Widget->Slot->GetClass()->GetName());
	}

	// Children count for panels
	if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
	{
		W["children_count"] = Panel->GetChildrenCount();
	}

	return W;
}

// Recursive hierarchy builder -> returns array of widget tables with depth info
static void BuildHierarchy(sol::state_view& Lua, sol::table& Result, int32& Idx,
	UWidget* Widget, UWidgetBlueprint* WidgetBP, int32 Depth)
{
	if (!Widget) return;

	sol::table W = WidgetToTable(Lua, Widget, WidgetBP);
	W["depth"] = Depth;
	Result[Idx++] = W;

	if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
	{
		for (int32 i = 0; i < Panel->GetChildrenCount(); ++i)
		{
			BuildHierarchy(Lua, Result, Idx, Panel->GetChildAt(i), WidgetBP, Depth + 1);
		}
	}
}

// Get editable properties for a widget
static sol::table GetWidgetProperties(sol::state_view& Lua, UWidget* Widget)
{
	sol::table Props = Lua.create_table();
	int32 Idx = 1;

	for (TFieldIterator<FProperty> PropIt(Widget->GetClass()); PropIt; ++PropIt)
	{
		FProperty* Prop = *PropIt;
		if (!Prop->HasAnyPropertyFlags(CPF_Edit)) continue;
		if (Prop->HasAnyPropertyFlags(CPF_Deprecated)) continue;
		if (CastField<FMulticastDelegateProperty>(Prop)) continue;
		if (CastField<FDelegateProperty>(Prop)) continue;

		FString Value;
		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Widget);
		Prop->ExportTextItem_Direct(Value, ValuePtr, nullptr, nullptr, PPF_None);
		if (Value.Len() > 120) Value = Value.Left(117) + TEXT("...");

		sol::table E = Lua.create_table();
		E["name"] = TCHAR_TO_UTF8(*Prop->GetName());
		E["type"] = TCHAR_TO_UTF8(*NeoStackToolUtils::GetPropertyTypeName(Prop));
		E["value"] = TCHAR_TO_UTF8(*Value);
		FString Category = Prop->GetMetaData(TEXT("Category"));
		E["category"] = Category.IsEmpty() ? "Default" : TCHAR_TO_UTF8(*Category);
		Props[Idx++] = E;
	}

	return Props;
}

// Get slot properties
static sol::table GetSlotProperties(sol::state_view& Lua, UPanelSlot* Slot)
{
	sol::table Props = Lua.create_table();
	if (!Slot) return Props;

	int32 Idx = 1;
	for (TFieldIterator<FProperty> PropIt(Slot->GetClass()); PropIt; ++PropIt)
	{
		FProperty* Prop = *PropIt;
		if (!Prop->HasAnyPropertyFlags(CPF_Edit)) continue;

		FString Value;
		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Slot);
		Prop->ExportTextItem_Direct(Value, ValuePtr, nullptr, nullptr, PPF_None);
		if (Value.Len() > 120) Value = Value.Left(117) + TEXT("...");

		sol::table E = Lua.create_table();
		E["name"] = TCHAR_TO_UTF8(*Prop->GetName());
		E["type"] = TCHAR_TO_UTF8(*NeoStackToolUtils::GetPropertyTypeName(Prop));
		E["value"] = TCHAR_TO_UTF8(*Value);
		Props[Idx++] = E;
	}

	return Props;
}

} // namespace WidgetHelper

// ============================================================================
// Widget Blueprint Enrichment
// ============================================================================

static TArray<FLuaFunctionDoc> WidgetBlueprintDocs = {};

static void BindWidgetBlueprint(sol::state& Lua, FLuaSessionData& Session)
{
	// ==================================================================
	// _enrich_widget_blueprint (called from open_asset for WidgetBlueprints)
	// ==================================================================
	Lua.set_function("_enrich_widget_blueprint", [&Session](sol::table BPObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = BPObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		UWidgetBlueprint* WidgetBP = LoadObject<UWidgetBlueprint>(nullptr, *FPath);
		if (!WidgetBP) return;

		// Store widget-specific help
		sol::optional<std::string> ExistingHelp = BPObj.get<sol::optional<std::string>>("_help_text");
		std::string BaseHelp = ExistingHelp.has_value() ? ExistingHelp.value() : "";

		BPObj["_help_text"] = BaseHelp +
			"\n=== Widget Blueprint methods ===\n"
			"list_widgets(filter?)         -> full widget tree hierarchy\n"
			"get_widget(name)              -> widget info + ALL properties (colors/fonts/margins) + slot\n"
			"find_widgets(query)           -> search by type or name pattern\n"
			"add_widget(type, opts?)       -> create and add widget (parent, name, index)\n"
			"remove_widget(name)           -> remove widget from tree (with cleanup)\n"
			"configure_widget(name, opts)  -> set ANY property: colors, fonts, margins, text, etc.\n"
			"rename_widget(old_name, new_name) -> rename widget + update all refs\n"
			"reparent_widget(name, new_parent, index?) -> move widget to new parent\n"
			"duplicate_widget(name, new_name?) -> clone widget subtree\n"
			"reorder_widget(name, new_index)   -> change position within parent\n"
			"list_animations()             -> widget animations\n"
			"get_animation(name)           -> animation details with tracks/bindings\n"
			"add_animation(name)           -> create new widget animation\n"
			"remove_animation(name)        -> delete widget animation\n"
			"configure_animation(name, opts) -> set animation timing/display\n"
			"list_bindings()               -> all property bindings (blueprint-level, NOT per-widget)\n"
			"add_binding({object_name=, property_name=, function_name=, source_property=}) -> add property binding\n"
			"remove_binding({object_name=, property_name=}) -> remove property binding (pass table, not string)\n"
			"list_named_slots()            -> named slots with content\n"
			"add_named_slot(name, opts?)   -> create named slot widget\n"
			"remove_named_slot(name)       -> remove named slot\n"
			"set_named_slot_content(slot, widget) -> assign widget to named slot\n"
			"widget_info()                 -> widget tree summary\n"
			"\n"
			"=== configure_widget property examples ===\n"
			"ALL editable UProperties are settable. Use get_widget(name) to discover property names.\n"
			"Values can be Lua scalars, tables (auto-converted to UE struct format), or UE ImportText strings.\n"
			"\n"
			"-- Text color (FSlateColor wraps FLinearColor):\n"
			"configure_widget('MyText', {ColorAndOpacity='(SpecifiedColor=(R=1,G=0,B=0,A=1))'})\n"
			"\n"
			"-- Font size (FSlateFontInfo - use ImportText string):\n"
			"configure_widget('MyText', {Font='(Size=24)'})\n"
			"-- Font with typeface:\n"
			"configure_widget('MyText', {Font='(FontObject=/Game/Fonts/MyFont.MyFont,Size=18)'})\n"
			"\n"
			"-- Shadow:\n"
			"configure_widget('MyText', {ShadowOffset='(X=2,Y=2)', ShadowColorAndOpacity='(R=0,G=0,B=0,A=0.5)'})\n"
			"\n"
			"-- Slot padding/margins (via 'slot' sub-table, property names depend on slot type):\n"
			"configure_widget('MyText', {slot={Padding={left=10, top=5, right=10, bottom=5}}})\n"
			"-- Slot alignment:\n"
			"configure_widget('MyText', {slot={HorizontalAlignment='HAlign_Center', VerticalAlignment='VAlign_Center'}})\n"
			"\n"
			"-- Image tint:\n"
			"configure_widget('MyImage', {ColorAndOpacity='(R=1,G=0.5,B=0,A=1)'})\n"
			"\n"
			"-- Button background (FLinearColor, not FSlateColor):\n"
			"configure_widget('MyButton', {BackgroundColor='(R=0.2,G=0.6,B=1,A=1)'})\n"
			"\n"
			"-- Any UProperty by name (use get_widget to see all available):\n"
			"configure_widget('MyText', {Justification='ETextJustify::Center', AutoWrapText=true})\n"
			"\n"
			"TIP: Call get_widget(name).props to see all property names and current values.\n"
			"TIP: For complex structs, pass the full UE ImportText string (get_widget shows current format).\n"
			"\n"
			"(Also available: list_events, bind_event, unbind_event from Blueprint base)\n";

		// ================================================================
		// widget_info() -> structured summary
		// ================================================================
		BPObj.set_function("widget_info", [FPath, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *FPath);
			if (!WBP)
			{
				Session.Log(TEXT("[FAIL] widget_info -> blueprint not found"));
				return sol::lua_nil;
			}

			sol::table Result = Lua.create_table();
			Result["path"] = TCHAR_TO_UTF8(*FPath);
			Result["type"] = "WidgetBlueprint";

			if (WBP->ParentClass)
				Result["parent_class"] = TCHAR_TO_UTF8(*WBP->ParentClass->GetName());

			// Widget count
			int32 WidgetCount = 0;
			int32 PanelCount = 0;
			if (WBP->WidgetTree)
			{
				TArray<UWidget*> AllWidgets;
				WBP->WidgetTree->GetAllWidgets(AllWidgets);
				WidgetCount = AllWidgets.Num();
				for (UWidget* W : AllWidgets)
				{
					if (W->IsA<UPanelWidget>()) PanelCount++;
				}

				if (WBP->WidgetTree->RootWidget)
					Result["root_widget"] = TCHAR_TO_UTF8(*WBP->WidgetTree->RootWidget->GetName());
			}
			Result["widget_count"] = WidgetCount;
			Result["panel_count"] = PanelCount;
			Result["animation_count"] = WBP->Animations.Num();
			Result["binding_count"] = WBP->Bindings.Num();

			// Named slots
			sol::table Slots = Lua.create_table();
			int32 SlotIdx = 1;
			if (WBP->WidgetTree)
			{
				TArray<FName> SlotNames;
				WBP->WidgetTree->GetSlotNames(SlotNames);
				for (const FName& Name : SlotNames)
				{
					Slots[SlotIdx++] = TCHAR_TO_UTF8(*Name.ToString());
				}
			}
			Result["named_slots"] = Slots;

			Session.Log(FString::Printf(TEXT("[OK] widget_info() -> %d widgets, %d animations"),
				WidgetCount, WBP->Animations.Num()));
			return Result;
		});

		// ================================================================
		// list_widgets(filter?) -> full widget tree hierarchy
		// ================================================================
		BPObj.set_function("list_widgets", [FPath, &Session](sol::table /*self*/,
			sol::optional<std::string> FilterOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *FPath);
			if (!WBP || !WBP->WidgetTree)
			{
				Session.Log(TEXT("[FAIL] list_widgets -> no widget tree"));
				return sol::lua_nil;
			}

			FString Filter = FilterOpt.has_value() ? UTF8_TO_TCHAR(FilterOpt.value().c_str()) : TEXT("");

			sol::table Result = Lua.create_table();

			if (Filter.IsEmpty())
			{
				// Return full hierarchy from root
				int32 Idx = 1;
				WidgetHelper::BuildHierarchy(Lua, Result, Idx, WBP->WidgetTree->RootWidget, WBP, 0);

				Session.Log(FString::Printf(TEXT("[OK] list_widgets() -> %d widgets"), Idx - 1));
			}
			else
			{
				// Filtered: match name or type
				TArray<UWidget*> AllWidgets;
				WBP->WidgetTree->GetAllWidgets(AllWidgets);
				int32 Idx = 1;

				for (UWidget* Widget : AllWidgets)
				{
					FString Name = Widget->GetName();
					FString Type = Widget->GetClass()->GetName();
					if (Name.Contains(Filter, ESearchCase::IgnoreCase) ||
						Type.Contains(Filter, ESearchCase::IgnoreCase))
					{
						Result[Idx++] = WidgetHelper::WidgetToTable(Lua, Widget, WBP);
					}
				}

				Session.Log(FString::Printf(TEXT("[OK] list_widgets(\"%s\") -> %d matches"), *Filter, Idx - 1));
			}

			return Result;
		});

		// ================================================================
		// get_widget(name) -> detailed widget info
		// ================================================================
		BPObj.set_function("get_widget", [FPath, &Session](sol::table /*self*/,
			const std::string& WidgetName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FName = UTF8_TO_TCHAR(WidgetName.c_str());

			UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *FPath);
			if (!WBP || !WBP->WidgetTree)
			{
				Session.Log(TEXT("[FAIL] get_widget -> no widget tree"));
				return sol::lua_nil;
			}

			UWidget* Widget = WBP->WidgetTree->FindWidget(::FName(*FName));
			if (!Widget)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] get_widget(\"%s\") -> not found"), *FName));
				return sol::lua_nil;
			}

			sol::table Result = WidgetHelper::WidgetToTable(Lua, Widget, WBP);

			// Add detailed properties (array of {name, type, value, category} tables)
			Result["properties"] = WidgetHelper::GetWidgetProperties(Lua, Widget);

			// Add flat props map for easy access: props["PropertyName"] = "value"
			sol::table FlatProps = Lua.create_table();
			for (TFieldIterator<FProperty> PropIt(Widget->GetClass()); PropIt; ++PropIt)
			{
				FProperty* Prop = *PropIt;
				if (!Prop->HasAnyPropertyFlags(CPF_Edit)) continue;
				if (Prop->HasAnyPropertyFlags(CPF_Deprecated)) continue;
				if (CastField<FMulticastDelegateProperty>(Prop)) continue;
				if (CastField<FDelegateProperty>(Prop)) continue;

				FString Value;
				const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Widget);
				Prop->ExportTextItem_Direct(Value, ValuePtr, nullptr, nullptr, PPF_None);
				if (Value.Len() > 500) Value = Value.Left(497) + TEXT("...");
				FlatProps[TCHAR_TO_UTF8(*Prop->GetName())] = TCHAR_TO_UTF8(*Value);
			}
			Result["props"] = FlatProps;

			// Add slot properties
			if (Widget->Slot)
			{
				Result["slot_properties"] = WidgetHelper::GetSlotProperties(Lua, Widget->Slot);

				// Flat slot props too
				sol::table FlatSlot = Lua.create_table();
				for (TFieldIterator<FProperty> PropIt(Widget->Slot->GetClass()); PropIt; ++PropIt)
				{
					FProperty* Prop = *PropIt;
					if (!Prop->HasAnyPropertyFlags(CPF_Edit)) continue;
					FString Value;
					const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Widget->Slot);
					Prop->ExportTextItem_Direct(Value, ValuePtr, nullptr, nullptr, PPF_None);
					if (Value.Len() > 500) Value = Value.Left(497) + TEXT("...");
					FlatSlot[TCHAR_TO_UTF8(*Prop->GetName())] = TCHAR_TO_UTF8(*Value);
				}
				Result["slot_props"] = FlatSlot;
			}

			// Add children list for panels
			if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
			{
				sol::table Children = Lua.create_table();
				for (int32 i = 0; i < Panel->GetChildrenCount(); ++i)
				{
					UWidget* Child = Panel->GetChildAt(i);
					if (Child)
					{
						sol::table C = Lua.create_table();
						C["name"] = TCHAR_TO_UTF8(*Child->GetName());
						C["type"] = TCHAR_TO_UTF8(*Child->GetClass()->GetName());
						C["index"] = i;
						Children[i + 1] = C;
					}
				}
				Result["children"] = Children;
			}

			Session.Log(FString::Printf(TEXT("[OK] get_widget(\"%s\") -> %s"),
				*FName, *Widget->GetClass()->GetName()));
			return Result;
		});

		// ================================================================
		// find_widgets(query) -> search by type, name pattern, or property
		// ================================================================
		BPObj.set_function("find_widgets", [FPath, &Session](sol::table /*self*/,
			sol::object QueryObj, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *FPath);
			if (!WBP || !WBP->WidgetTree)
			{
				Session.Log(TEXT("[FAIL] find_widgets -> no widget tree"));
				return sol::lua_nil;
			}

			TArray<UWidget*> AllWidgets;
			WBP->WidgetTree->GetAllWidgets(AllWidgets);

			sol::table Result = Lua.create_table();
			int32 Idx = 1;

			if (QueryObj.is<std::string>())
			{
				// String query: match name or class name
				FString Query = UTF8_TO_TCHAR(QueryObj.as<std::string>().c_str());

				for (UWidget* Widget : AllWidgets)
				{
					FString Name = Widget->GetName();
					FString ClassName = Widget->GetClass()->GetName();

					if (Name.Contains(Query, ESearchCase::IgnoreCase) ||
						ClassName.Equals(Query, ESearchCase::IgnoreCase) ||
						ClassName.Equals(Query + TEXT("Widget"), ESearchCase::IgnoreCase))
					{
						Result[Idx++] = WidgetHelper::WidgetToTable(Lua, Widget, WBP);
					}
				}

				Session.Log(FString::Printf(TEXT("[OK] find_widgets(\"%s\") -> %d matches"), *Query, Idx - 1));
			}
			else if (QueryObj.is<sol::table>())
			{
				// Table query: {type="Button", name="*Submit*", is_variable=true}
				sol::table Q = QueryObj.as<sol::table>();
				FString TypeFilter = UTF8_TO_TCHAR(Q.get_or("type", std::string()).c_str());
				FString NameFilter = UTF8_TO_TCHAR(Q.get_or("name", std::string()).c_str());
				sol::optional<bool> VarFilter = Q.get<sol::optional<bool>>("is_variable");
				sol::optional<bool> PanelFilter = Q.get<sol::optional<bool>>("is_panel");

				for (UWidget* Widget : AllWidgets)
				{
					FString Name = Widget->GetName();
					FString ClassName = Widget->GetClass()->GetName();

					if (!TypeFilter.IsEmpty() &&
						!ClassName.Equals(TypeFilter, ESearchCase::IgnoreCase) &&
						!ClassName.Equals(TypeFilter + TEXT("Widget"), ESearchCase::IgnoreCase) &&
						!Widget->GetClass()->IsChildOf(FindFirstObject<UClass>(*TypeFilter, EFindFirstObjectOptions::NativeFirst)))
					{
						continue;
					}
					if (!NameFilter.IsEmpty() && !Name.Contains(NameFilter, ESearchCase::IgnoreCase))
						continue;
					if (VarFilter.has_value() && Widget->bIsVariable != VarFilter.value())
						continue;
					if (PanelFilter.has_value() && Widget->IsA<UPanelWidget>() != PanelFilter.value())
						continue;

					Result[Idx++] = WidgetHelper::WidgetToTable(Lua, Widget, WBP);
				}

				Session.Log(FString::Printf(TEXT("[OK] find_widgets({...}) -> %d matches"), Idx - 1));
			}

			return Result;
		});

		// ================================================================
		// reparent_widget(name, new_parent, index?)
		// ================================================================
		BPObj.set_function("reparent_widget", [FPath, &Session](sol::table /*self*/,
			const std::string& WidgetName, const std::string& NewParentName,
			sol::optional<int> IndexOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FWidgetName = UTF8_TO_TCHAR(WidgetName.c_str());
			FString FNewParent = UTF8_TO_TCHAR(NewParentName.c_str());

			UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *FPath);
			if (!WBP || !WBP->WidgetTree)
			{
				Session.Log(TEXT("[FAIL] reparent_widget -> no widget tree"));
				return sol::lua_nil;
			}

			UWidget* Widget = WBP->WidgetTree->FindWidget(::FName(*FWidgetName));
			if (!Widget)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] reparent_widget(\"%s\") -> not found"), *FWidgetName));
				return sol::lua_nil;
			}

			UPanelWidget* NewParent = Cast<UPanelWidget>(WBP->WidgetTree->FindWidget(::FName(*FNewParent)));
			if (!NewParent)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] reparent_widget -> parent '%s' not found or not a panel"), *FNewParent));
				return sol::lua_nil;
			}

			// Cannot parent to self or descendant
			if (Widget == NewParent)
			{
				Session.Log(TEXT("[FAIL] reparent_widget -> cannot parent widget to itself"));
				return sol::lua_nil;
			}
			// Walk up from NewParent to check if Widget is an ancestor (prevents circular parenting)
			{
				UWidget* Walk = NewParent;
				while (Walk)
				{
					if (Walk == Widget)
					{
						Session.Log(TEXT("[FAIL] reparent_widget -> cannot parent to own descendant"));
						return sol::lua_nil;
					}
					int32 CI;
					Walk = UWidgetTree::FindWidgetParent(Walk, CI);
				}
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaReparentWidget", "Reparent Widget"));
			WBP->WidgetTree->SetFlags(RF_Transactional);
			WBP->WidgetTree->Modify();

			// Remove from current parent
			int32 OldIndex;
			UPanelWidget* OldParent = UWidgetTree::FindWidgetParent(Widget, OldIndex);
			if (OldParent)
			{
				OldParent->Modify();
				OldParent->RemoveChild(Widget);
			}
			else if (Widget == WBP->WidgetTree->RootWidget)
			{
				// Moving root widget into a panel — need a new root
				Session.Log(TEXT("[FAIL] reparent_widget -> cannot reparent root widget (remove it first or set a new root)"));
				return sol::lua_nil;
			}

			// Add to new parent
			NewParent->Modify();
			if (IndexOpt.has_value() && IndexOpt.value() >= 0)
			{
				int32 Idx = FMath::Min(IndexOpt.value(), NewParent->GetChildrenCount());
				NewParent->InsertChildAt(Idx, Widget);
			}
			else
			{
				NewParent->AddChild(Widget);
			}

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

			Session.Log(FString::Printf(TEXT("[OK] reparent_widget(\"%s\" -> \"%s\")"), *FWidgetName, *FNewParent));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// reorder_widget(name, new_index)
		// ================================================================
		BPObj.set_function("reorder_widget", [FPath, &Session](sol::table /*self*/,
			const std::string& WidgetName, int NewIndex,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FWidgetName = UTF8_TO_TCHAR(WidgetName.c_str());

			UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *FPath);
			if (!WBP || !WBP->WidgetTree)
			{
				Session.Log(TEXT("[FAIL] reorder_widget -> no widget tree"));
				return sol::lua_nil;
			}

			UWidget* Widget = WBP->WidgetTree->FindWidget(::FName(*FWidgetName));
			if (!Widget)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] reorder_widget(\"%s\") -> not found"), *FWidgetName));
				return sol::lua_nil;
			}

			int32 OldIndex;
			UPanelWidget* Parent = UWidgetTree::FindWidgetParent(Widget, OldIndex);
			if (!Parent)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] reorder_widget(\"%s\") -> no parent (root widget?)"), *FWidgetName));
				return sol::lua_nil;
			}

			int32 ClampedIndex = FMath::Clamp(NewIndex, 0, Parent->GetChildrenCount() - 1);

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaReorderWidget", "Reorder Widget"));
			WBP->WidgetTree->Modify();
			Parent->Modify();

			Parent->ShiftChild(ClampedIndex, Widget);

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

			Session.Log(FString::Printf(TEXT("[OK] reorder_widget(\"%s\", %d) (was %d)"),
				*FWidgetName, ClampedIndex, OldIndex));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// duplicate_widget(name, new_name?)
		// ================================================================
		BPObj.set_function("duplicate_widget", [FPath, &Session](sol::table /*self*/,
			const std::string& WidgetName, sol::optional<std::string> NewNameOpt,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FWidgetName = UTF8_TO_TCHAR(WidgetName.c_str());

			UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *FPath);
			if (!WBP || !WBP->WidgetTree)
			{
				Session.Log(TEXT("[FAIL] duplicate_widget -> no widget tree"));
				return sol::lua_nil;
			}

			UWidget* SourceWidget = WBP->WidgetTree->FindWidget(::FName(*FWidgetName));
			if (!SourceWidget)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] duplicate_widget(\"%s\") -> not found"), *FWidgetName));
				return sol::lua_nil;
			}

			int32 OldIndex;
			UPanelWidget* Parent = UWidgetTree::FindWidgetParent(SourceWidget, OldIndex);
			if (!Parent)
			{
				Session.Log(TEXT("[FAIL] duplicate_widget -> cannot duplicate root widget"));
				return sol::lua_nil;
			}

			// Determine new name
			FString FNewName;
			if (NewNameOpt.has_value())
			{
				FNewName = UTF8_TO_TCHAR(NewNameOpt.value().c_str());
			}
			else
			{
				// Auto-generate: WidgetName_Copy, WidgetName_Copy2, etc.
				FNewName = FWidgetName + TEXT("_Copy");
				int32 Suffix = 2;
				while (WBP->WidgetTree->FindWidget(::FName(*FNewName)))
				{
					FNewName = FString::Printf(TEXT("%s_Copy%d"), *FWidgetName, Suffix++);
				}
			}

			if (WBP->WidgetTree->FindWidget(::FName(*FNewName)))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] duplicate_widget -> '%s' already exists"), *FNewName));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaDuplicateWidget", "Duplicate Widget"));
			WBP->WidgetTree->SetFlags(RF_Transactional);
			WBP->WidgetTree->Modify();

			// Create duplicate via DuplicateObject
			UWidget* NewWidget = DuplicateObject<UWidget>(SourceWidget, WBP->WidgetTree, ::FName(*FNewName));
			if (!NewWidget)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] duplicate_widget(\"%s\") -> duplication failed"), *FWidgetName));
				return sol::lua_nil;
			}

			// Ensure child widgets from the duplication have unique names (mirrors engine FindNextValidName pattern)
			if (UPanelWidget* NewPanel = Cast<UPanelWidget>(NewWidget))
			{
				TArray<UWidget*> ChildWidgets;
				UWidgetTree::GetChildWidgets(NewWidget, ChildWidgets);
				for (UWidget* Child : ChildWidgets)
				{
					if (Child && Child != NewWidget)
					{
						FString ChildName = Child->GetName();
						// Check if a widget with the same name already exists in the tree
						if (UWidget* Existing = WBP->WidgetTree->FindWidget(Child->GetFName()))
						{
							if (Existing != Child)
							{
								// Generate unique name
								FString BaseName = ChildName;
								int32 Suffix = 1;
								FString UniqueName = FString::Printf(TEXT("%s_%d"), *BaseName, Suffix);
								while (WBP->WidgetTree->FindWidget(::FName(*UniqueName)))
								{
									Suffix++;
									UniqueName = FString::Printf(TEXT("%s_%d"), *BaseName, Suffix);
								}
								Child->Rename(*UniqueName, WBP->WidgetTree);
							}
						}
					}
				}
			}

			// Register the new widget variable
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			WBP->OnVariableAdded(NewWidget->GetFName());
#endif

			// Add to same parent, right after source
			Parent->Modify();
			UPanelSlot* Slot = Parent->InsertChildAt(OldIndex + 1, NewWidget);
			if (!Slot)
			{
				Session.Log(TEXT("[FAIL] duplicate_widget -> InsertChildAt failed"));
				return sol::lua_nil;
			}

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

			Session.Log(FString::Printf(TEXT("[OK] duplicate_widget(\"%s\") -> \"%s\""), *FWidgetName, *FNewName));

			sol::table Result = WidgetHelper::WidgetToTable(Lua, NewWidget, WBP);
			return Result;
		});

		// ================================================================
		// list_animations() -> widget animations
		// ================================================================
		BPObj.set_function("list_animations", [FPath, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *FPath);
			if (!WBP)
			{
				Session.Log(TEXT("[FAIL] list_animations -> blueprint not found"));
				return sol::lua_nil;
			}

			sol::table Result = Lua.create_table();
			int32 Idx = 1;

			for (UWidgetAnimation* Anim : WBP->Animations)
			{
				if (!Anim) continue;

				sol::table A = Lua.create_table();
				A["name"] = TCHAR_TO_UTF8(*Anim->GetName());
				A["display_name"] = TCHAR_TO_UTF8(*Anim->GetDisplayName().ToString());
				A["start_time"] = Anim->GetStartTime();
				A["end_time"] = Anim->GetEndTime();
				A["duration"] = Anim->GetEndTime() - Anim->GetStartTime();

				// Count bound objects (tracks)
				UMovieScene* MovieScene = Anim->GetMovieScene();
				if (MovieScene)
				{
					const TArray<FMovieSceneBinding>& Bindings = const_cast<const UMovieScene*>(MovieScene)->GetBindings();
					A["track_count"] = Bindings.Num();
				}

				Result[Idx++] = A;
			}

			Session.Log(FString::Printf(TEXT("[OK] list_animations() -> %d animations"), Idx - 1));
			return Result;
		});

		// ================================================================
		// get_animation(name) -> detailed animation info with tracks
		// ================================================================
		BPObj.set_function("get_animation", [FPath, &Session](sol::table /*self*/,
			const std::string& AnimName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FAnimName = UTF8_TO_TCHAR(AnimName.c_str());

			UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *FPath);
			if (!WBP) { Session.Log(TEXT("[FAIL] get_animation -> blueprint not found")); return sol::lua_nil; }

			UWidgetAnimation* FoundAnim = nullptr;
			for (UWidgetAnimation* Anim : WBP->Animations)
			{
				if (Anim && (Anim->GetName().Equals(FAnimName, ESearchCase::IgnoreCase) ||
					Anim->GetDisplayName().ToString().Equals(FAnimName, ESearchCase::IgnoreCase)))
				{
					FoundAnim = Anim;
					break;
				}
			}
			if (!FoundAnim)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] get_animation(\"%s\") -> not found"), *FAnimName));
				return sol::lua_nil;
			}

			sol::table Result = Lua.create_table();
			Result["name"] = TCHAR_TO_UTF8(*FoundAnim->GetName());
			Result["display_name"] = TCHAR_TO_UTF8(*FoundAnim->GetDisplayName().ToString());
			Result["start_time"] = FoundAnim->GetStartTime();
			Result["end_time"] = FoundAnim->GetEndTime();
			Result["duration"] = FoundAnim->GetEndTime() - FoundAnim->GetStartTime();

			// Animation bindings (which widgets are animated)
			sol::table BindingsTable = Lua.create_table();
			int32 BIdx = 1;
			const TArray<FWidgetAnimationBinding>& AnimBindings = FoundAnim->GetBindings();
			for (const FWidgetAnimationBinding& Binding : AnimBindings)
			{
				sol::table B = Lua.create_table();
				B["widget_name"] = TCHAR_TO_UTF8(*Binding.WidgetName.ToString());
				B["is_root_widget"] = Binding.bIsRootWidget;
				if (!Binding.SlotWidgetName.IsNone())
					B["slot_widget_name"] = TCHAR_TO_UTF8(*Binding.SlotWidgetName.ToString());
				BindingsTable[BIdx++] = B;
			}
			Result["bindings"] = BindingsTable;

			// Tracks from MovieScene
			UMovieScene* MovieScene = FoundAnim->GetMovieScene();
			if (MovieScene)
			{
				sol::table TracksTable = Lua.create_table();
				int32 TIdx = 1;
				const auto& SceneBindings = const_cast<const UMovieScene*>(MovieScene)->GetBindings();
				for (const FMovieSceneBinding& SceneBinding : SceneBindings)
				{
					for (UMovieSceneTrack* Track : SceneBinding.GetTracks())
					{
						if (!Track) continue;
						sol::table T = Lua.create_table();
						T["type"] = TCHAR_TO_UTF8(*Track->GetClass()->GetName());
						T["display_name"] = TCHAR_TO_UTF8(*Track->GetDisplayName().ToString());

						// Section count and ranges
						const TArray<UMovieSceneSection*>& Sections = Track->GetAllSections();
						T["section_count"] = Sections.Num();

						sol::table SectionsTable = Lua.create_table();
						int32 SIdx = 1;
						for (UMovieSceneSection* Section : Sections)
						{
							if (!Section) continue;
							sol::table Sec = Lua.create_table();
							Sec["active"] = Section->IsActive();
							Sec["locked"] = Section->IsLocked();
							Sec["row_index"] = Section->GetRowIndex();
							if (Section->HasStartFrame())
								Sec["start_frame"] = Section->GetInclusiveStartFrame().Value;
							if (Section->HasEndFrame())
								Sec["end_frame"] = Section->GetExclusiveEndFrame().Value;
							SectionsTable[SIdx++] = Sec;
						}
						T["sections"] = SectionsTable;
						TracksTable[TIdx++] = T;
					}
				}

				// Also include master tracks
				for (UMovieSceneTrack* Track : MovieScene->GetTracks())
				{
					if (!Track) continue;
					sol::table T = Lua.create_table();
					T["type"] = TCHAR_TO_UTF8(*Track->GetClass()->GetName());
					T["display_name"] = TCHAR_TO_UTF8(*Track->GetDisplayName().ToString());
					T["is_master"] = true;
					const TArray<UMovieSceneSection*>& Sections = Track->GetAllSections();
					T["section_count"] = Sections.Num();
					TracksTable[TIdx++] = T;
				}

				Result["tracks"] = TracksTable;
			}

			Session.Log(FString::Printf(TEXT("[OK] get_animation(\"%s\")"), *FAnimName));
			return Result;
		});

		// ================================================================
		// add_animation(name) -> create new widget animation
		// ================================================================
		BPObj.set_function("add_animation", [FPath, &Session](sol::table /*self*/,
			const std::string& AnimName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FAnimName = UTF8_TO_TCHAR(AnimName.c_str());

			UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *FPath);
			if (!WBP) { Session.Log(TEXT("[FAIL] add_animation -> blueprint not found")); return sol::lua_nil; }

			// Check if animation already exists
			for (UWidgetAnimation* Anim : WBP->Animations)
			{
				if (Anim && Anim->GetName().Equals(FAnimName, ESearchCase::IgnoreCase))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add_animation(\"%s\") -> already exists"), *FAnimName));
					return sol::lua_nil;
				}
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaAddWidgetAnim", "Add Widget Animation"));
			WBP->Modify();

			// Create the animation object
			UWidgetAnimation* NewAnim = NewObject<UWidgetAnimation>(WBP, ::FName(*FAnimName), RF_Transactional);
			if (!NewAnim)
			{
				Session.Log(TEXT("[FAIL] add_animation -> creation failed"));
				return sol::lua_nil;
			}

			// Create the MovieScene for keyframe storage
			UMovieScene* MovieScene = NewObject<UMovieScene>(NewAnim, ::FName("MovieScene"), RF_Transactional);
			NewAnim->MovieScene = MovieScene;

			NewAnim->SetDisplayLabel(FAnimName);

			// Set default playback range (0 to 5 seconds at 30fps, matching engine defaults)
			FFrameRate FrameRate(30, 1);
			MovieScene->SetDisplayRate(FrameRate);
			MovieScene->SetTickResolutionDirectly(FFrameRate(24000, 1));
			FFrameNumber StartFrame(0);
			FFrameNumber EndFrame = FFrameNumber(static_cast<int32>(5.0 * FrameRate.AsDecimal()));
			MovieScene->SetPlaybackRange(TRange<FFrameNumber>(StartFrame, EndFrame));

			// Matches engine flow: AnimationTabSummoner.cpp lines 306-311
			WBP->Animations.Add(NewAnim);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			WBP->OnVariableAdded(NewAnim->GetFName());
#endif
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

			sol::table Result = Lua.create_table();
			Result["name"] = TCHAR_TO_UTF8(*NewAnim->GetName());
			Result["display_name"] = TCHAR_TO_UTF8(*NewAnim->GetDisplayName().ToString());
			Result["duration"] = 5.0;

			Session.Log(FString::Printf(TEXT("[OK] add_animation(\"%s\")"), *FAnimName));
			return Result;
		});

		// ================================================================
		// remove_animation(name)
		// ================================================================
		BPObj.set_function("remove_animation", [FPath, &Session](sol::table /*self*/,
			const std::string& AnimName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FAnimName = UTF8_TO_TCHAR(AnimName.c_str());

			UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *FPath);
			if (!WBP) { Session.Log(TEXT("[FAIL] remove_animation -> blueprint not found")); return sol::lua_nil; }

			int32 FoundIdx = INDEX_NONE;
			for (int32 i = 0; i < WBP->Animations.Num(); ++i)
			{
				if (WBP->Animations[i] && (WBP->Animations[i]->GetName().Equals(FAnimName, ESearchCase::IgnoreCase) ||
					WBP->Animations[i]->GetDisplayName().ToString().Equals(FAnimName, ESearchCase::IgnoreCase)))
				{
					FoundIdx = i;
					break;
				}
			}
			if (FoundIdx == INDEX_NONE)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove_animation(\"%s\") -> not found"), *FAnimName));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaRemoveWidgetAnim", "Remove Widget Animation"));
			WBP->Modify();

			// Matches engine flow: AnimationTabSummoner.cpp lines 889-898
			UWidgetAnimation* AnimToRemove = WBP->Animations[FoundIdx];
			const FName RemovedAnimName = AnimToRemove->GetFName();
			AnimToRemove->Rename(nullptr, GetTransientPackage());
			WBP->Animations.RemoveAt(FoundIdx);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			WBP->OnVariableRemoved(RemovedAnimName);
#endif
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

			Session.Log(FString::Printf(TEXT("[OK] remove_animation(\"%s\")"), *FAnimName));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// configure_animation(name, opts) -> set timing, display name
		// ================================================================
		BPObj.set_function("configure_animation", [FPath, &Session](sol::table /*self*/,
			const std::string& AnimName, sol::table Opts, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FAnimName = UTF8_TO_TCHAR(AnimName.c_str());

			UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *FPath);
			if (!WBP) { Session.Log(TEXT("[FAIL] configure_animation -> blueprint not found")); return sol::lua_nil; }

			UWidgetAnimation* FoundAnim = nullptr;
			for (UWidgetAnimation* Anim : WBP->Animations)
			{
				if (Anim && (Anim->GetName().Equals(FAnimName, ESearchCase::IgnoreCase) ||
					Anim->GetDisplayName().ToString().Equals(FAnimName, ESearchCase::IgnoreCase)))
				{
					FoundAnim = Anim;
					break;
				}
			}
			if (!FoundAnim)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] configure_animation(\"%s\") -> not found"), *FAnimName));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaConfigWidgetAnim", "Configure Widget Animation"));
			FoundAnim->Modify();
			int32 ChangedCount = 0;
			TSet<FString> HandledKeys;

			// Display name
			sol::optional<std::string> DisplayNameOpt = Opts.get<sol::optional<std::string>>("display_name");
			if (DisplayNameOpt.has_value())
			{
				FoundAnim->SetDisplayLabel(UTF8_TO_TCHAR(DisplayNameOpt.value().c_str()));
				ChangedCount++;
				HandledKeys.Add(TEXT("display_name"));
			}

			// Duration / playback range (FFrameNumber is in TickResolution space, NOT DisplayRate)
			UMovieScene* MovieScene = FoundAnim->GetMovieScene();
			if (MovieScene)
			{
				sol::optional<double> DurationOpt = Opts.get<sol::optional<double>>("duration");
				sol::optional<double> StartOpt = Opts.get<sol::optional<double>>("start_time");

				if (DurationOpt.has_value()) HandledKeys.Add(TEXT("duration"));
				if (StartOpt.has_value()) HandledKeys.Add(TEXT("start_time"));

				if (DurationOpt.has_value() || StartOpt.has_value())
				{
					MovieScene->Modify();
					FFrameRate TickRate = MovieScene->GetTickResolution();
					double Start = StartOpt.has_value() ? StartOpt.value() : 0.0;
					double Duration = DurationOpt.has_value() ? DurationOpt.value() : (FoundAnim->GetEndTime() - FoundAnim->GetStartTime());

					FFrameNumber StartFrame = FFrameNumber(static_cast<int32>(Start * TickRate.AsDecimal()));
					FFrameNumber EndFrame = FFrameNumber(static_cast<int32>((Start + Duration) * TickRate.AsDecimal()));
					MovieScene->SetPlaybackRange(TRange<FFrameNumber>(StartFrame, EndFrame));
					ChangedCount++;
				}
			}

			// Warn about any keys we didn't handle so the agent can retry with correct ones
			for (auto& Pair : Opts)
			{
				if (!Pair.first.is<std::string>()) continue;
				FString Key = UTF8_TO_TCHAR(Pair.first.as<std::string>().c_str());
				if (!HandledKeys.Contains(Key))
				{
					Session.Log(FString::Printf(TEXT("[WARN] configure_animation(\"%s\") -> key '%s' was not applied. Keys handled this call: %s"),
						*FAnimName, *Key, HandledKeys.Num() > 0 ? *FString::Join(HandledKeys.Array(), TEXT(", ")) : TEXT("(none)")));
				}
			}

			FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

			Session.Log(FString::Printf(TEXT("[OK] configure_animation(\"%s\") -> %d changes"), *FAnimName, ChangedCount));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// list_bindings() -> property bindings
		// ================================================================
		BPObj.set_function("list_bindings", [FPath, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *FPath);
			if (!WBP) { Session.Log(TEXT("[FAIL] list_bindings -> blueprint not found")); return sol::lua_nil; }

			sol::table Result = Lua.create_table();
			int32 Idx = 1;

			for (const FDelegateEditorBinding& Binding : WBP->Bindings)
			{
				sol::table B = Lua.create_table();
				B["object_name"] = TCHAR_TO_UTF8(*Binding.ObjectName);
				B["property_name"] = TCHAR_TO_UTF8(*Binding.PropertyName.ToString());
				B["function_name"] = TCHAR_TO_UTF8(*Binding.FunctionName.ToString());
				B["source_property"] = TCHAR_TO_UTF8(*Binding.SourceProperty.ToString());
				B["kind"] = (Binding.Kind == EBindingKind::Function) ? "Function" : "Property";
				Result[Idx++] = B;
			}

			Session.Log(FString::Printf(TEXT("[OK] list_bindings() -> %d bindings"), Idx - 1));
			return Result;
		});

		// ================================================================
		// add_binding({object_name, property_name, function_name?, source_property?, kind?})
		// ================================================================
		BPObj.set_function("add_binding", [FPath, &Session](sol::table /*self*/,
			sol::table Opts, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *FPath);
			if (!WBP) { Session.Log(TEXT("[FAIL] add_binding -> blueprint not found")); return sol::lua_nil; }

			std::string ObjName = Opts.get_or<std::string>("object_name", "");
			std::string PropName = Opts.get_or<std::string>("property_name", "");
			if (ObjName.empty() || PropName.empty())
			{
				Session.Log(TEXT("[FAIL] add_binding -> 'object_name' and 'property_name' required"));
				return sol::lua_nil;
			}

			// Check widget exists
			if (WBP->WidgetTree)
			{
				UWidget* W = WBP->WidgetTree->FindWidget(::FName(UTF8_TO_TCHAR(ObjName.c_str())));
				if (!W)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add_binding -> widget '%s' not found"),
						UTF8_TO_TCHAR(ObjName.c_str())));
					return sol::lua_nil;
				}
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaAddBinding", "Add Widget Binding"));
			WBP->Modify();

			FDelegateEditorBinding NewBinding;
			NewBinding.ObjectName = UTF8_TO_TCHAR(ObjName.c_str());
			NewBinding.PropertyName = ::FName(UTF8_TO_TCHAR(PropName.c_str()));
			NewBinding.FunctionName = ::FName(UTF8_TO_TCHAR(Opts.get_or<std::string>("function_name", "").c_str()));
			NewBinding.SourceProperty = ::FName(UTF8_TO_TCHAR(Opts.get_or<std::string>("source_property", "").c_str()));

			std::string KindStr = Opts.get_or<std::string>("kind", "Property");
			NewBinding.Kind = (KindStr == "Function") ? EBindingKind::Function : EBindingKind::Property;

			// Remove existing binding for the same object+property if any
			WBP->Bindings.RemoveAll([&](const FDelegateEditorBinding& Existing)
			{
				return Existing.ObjectName == NewBinding.ObjectName &&
					Existing.PropertyName == NewBinding.PropertyName;
			});

			WBP->Bindings.Add(NewBinding);
			FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

			Session.Log(FString::Printf(TEXT("[OK] add_binding(%s.%s)"),
				UTF8_TO_TCHAR(ObjName.c_str()), *NewBinding.PropertyName.ToString()));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// remove_binding({object_name, property_name})
		// ================================================================
		BPObj.set_function("remove_binding", [FPath, &Session](sol::table /*self*/,
			sol::table Opts, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *FPath);
			if (!WBP) { Session.Log(TEXT("[FAIL] remove_binding -> blueprint not found")); return sol::lua_nil; }

			std::string ObjName = Opts.get_or<std::string>("object_name", "");
			std::string PropName = Opts.get_or<std::string>("property_name", "");

			if (ObjName.empty() && PropName.empty())
			{
				Session.Log(TEXT("[FAIL] remove_binding -> at least 'object_name' or 'property_name' required"));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaRemoveBinding", "Remove Widget Binding"));
			WBP->Modify();

			FString FObjName = UTF8_TO_TCHAR(ObjName.c_str());
			FName FPropName = ::FName(UTF8_TO_TCHAR(PropName.c_str()));

			int32 Removed = WBP->Bindings.RemoveAll([&](const FDelegateEditorBinding& Existing)
			{
				if (!ObjName.empty() && Existing.ObjectName != FObjName) return false;
				if (!PropName.empty() && Existing.PropertyName != FPropName) return false;
				return true;
			});

			if (Removed == 0)
			{
				Session.Log(TEXT("[FAIL] remove_binding -> no matching binding found"));
				return sol::lua_nil;
			}

			FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

			Session.Log(FString::Printf(TEXT("[OK] remove_binding -> removed %d"), Removed));
			return sol::make_object(Lua, Removed);
		});

		// ================================================================
		// list_named_slots() -> named slots with content
		// ================================================================
		BPObj.set_function("list_named_slots", [FPath, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *FPath);
			if (!WBP || !WBP->WidgetTree)
			{
				Session.Log(TEXT("[FAIL] list_named_slots -> no widget tree"));
				return sol::lua_nil;
			}

			sol::table Result = Lua.create_table();
			int32 Idx = 1;

			// Find all UNamedSlot widgets in the tree
			TArray<UWidget*> AllWidgets;
			WBP->WidgetTree->GetAllWidgets(AllWidgets);

			for (UWidget* Widget : AllWidgets)
			{
				UNamedSlot* NamedSlot = Cast<UNamedSlot>(Widget);
				if (!NamedSlot) continue;

				sol::table Slot = Lua.create_table();
				Slot["name"] = TCHAR_TO_UTF8(*NamedSlot->GetName());
#if WITH_EDITORONLY_DATA
				Slot["expose_on_instance_only"] = NamedSlot->bExposeOnInstanceOnly;
#endif

				// Check if slot has content
				if (NamedSlot->GetChildrenCount() > 0)
				{
					UWidget* Content = NamedSlot->GetChildAt(0);
					if (Content)
					{
						Slot["content_name"] = TCHAR_TO_UTF8(*Content->GetName());
						Slot["content_type"] = TCHAR_TO_UTF8(*Content->GetClass()->GetName());
					}
				}
				else
				{
					Slot["content_name"] = sol::lua_nil;
				}

				// Parent info
				int32 ChildIndex;
				UPanelWidget* Parent = UWidgetTree::FindWidgetParent(NamedSlot, ChildIndex);
				if (Parent)
				{
					Slot["parent"] = TCHAR_TO_UTF8(*Parent->GetName());
				}

				Result[Idx++] = Slot;
			}

			Session.Log(FString::Printf(TEXT("[OK] list_named_slots() -> %d slots"), Idx - 1));
			return Result;
		});

		// ================================================================
		// add_named_slot(name, opts?) -> create NamedSlot widget
		// ================================================================
		BPObj.set_function("add_named_slot", [FPath, &Session](sol::table /*self*/,
			const std::string& SlotName, sol::optional<sol::table> OptsOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FSlotName = UTF8_TO_TCHAR(SlotName.c_str());

			UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *FPath);
			if (!WBP || !WBP->WidgetTree)
			{
				Session.Log(TEXT("[FAIL] add_named_slot -> no widget tree"));
				return sol::lua_nil;
			}

			// Check for duplicate name
			if (WBP->WidgetTree->FindWidget(::FName(*FSlotName)))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_named_slot(\"%s\") -> name already exists"), *FSlotName));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaAddNamedSlot", "Add Named Slot"));
			WBP->WidgetTree->SetFlags(RF_Transactional);
			WBP->WidgetTree->Modify();

			UNamedSlot* NewSlot = WBP->WidgetTree->ConstructWidget<UNamedSlot>(UNamedSlot::StaticClass(), ::FName(*FSlotName));
			if (!NewSlot)
			{
				Session.Log(TEXT("[FAIL] add_named_slot -> construction failed"));
				return sol::lua_nil;
			}

			// Options
			if (OptsOpt.has_value())
			{
				sol::table Opts = OptsOpt.value();
#if WITH_EDITORONLY_DATA
				sol::optional<bool> ExposeOpt = Opts.get<sol::optional<bool>>("expose_on_instance_only");
				if (ExposeOpt.has_value())
					NewSlot->bExposeOnInstanceOnly = ExposeOpt.value();
#endif
			}

			// Parent to a panel if specified, otherwise to root
			FString ParentName;
			if (OptsOpt.has_value())
			{
				sol::optional<std::string> ParentOpt = OptsOpt.value().get<sol::optional<std::string>>("parent");
				if (ParentOpt.has_value())
					ParentName = UTF8_TO_TCHAR(ParentOpt.value().c_str());
			}

			if (!ParentName.IsEmpty())
			{
				UPanelWidget* Parent = Cast<UPanelWidget>(WBP->WidgetTree->FindWidget(::FName(*ParentName)));
				if (!Parent)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add_named_slot -> parent '%s' not found or not a panel"), *ParentName));
					return sol::lua_nil;
				}
				Parent->Modify();
				Parent->AddChild(NewSlot);
			}
			else if (WBP->WidgetTree->RootWidget)
			{
				UPanelWidget* RootPanel = Cast<UPanelWidget>(WBP->WidgetTree->RootWidget);
				if (RootPanel)
				{
					RootPanel->Modify();
					RootPanel->AddChild(NewSlot);
				}
				else
				{
					Session.Log(TEXT("[FAIL] add_named_slot -> root is not a panel and no parent specified"));
					return sol::lua_nil;
				}
			}
			else
			{
				// No root yet, set as root (unusual but handled)
				WBP->WidgetTree->RootWidget = NewSlot;
			}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			WBP->OnVariableAdded(NewSlot->GetFName());
#endif
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

			sol::table Result = Lua.create_table();
			Result["name"] = TCHAR_TO_UTF8(*NewSlot->GetName());
			Result["type"] = "NamedSlot";

			Session.Log(FString::Printf(TEXT("[OK] add_named_slot(\"%s\")"), *FSlotName));
			return Result;
		});

		// ================================================================
		// remove_named_slot(name)
		// ================================================================
		BPObj.set_function("remove_named_slot", [FPath, &Session](sol::table /*self*/,
			const std::string& SlotName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FSlotName = UTF8_TO_TCHAR(SlotName.c_str());

			UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *FPath);
			if (!WBP || !WBP->WidgetTree)
			{
				Session.Log(TEXT("[FAIL] remove_named_slot -> no widget tree"));
				return sol::lua_nil;
			}

			UWidget* Widget = WBP->WidgetTree->FindWidget(::FName(*FSlotName));
			UNamedSlot* NamedSlot = Cast<UNamedSlot>(Widget);
			if (!NamedSlot)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove_named_slot(\"%s\") -> not found or not a NamedSlot"), *FSlotName));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaRemoveNamedSlot", "Remove Named Slot"));
			WBP->WidgetTree->SetFlags(RF_Transactional);
			WBP->WidgetTree->Modify();

			// Remove from parent
			int32 ChildIdx;
			UPanelWidget* Parent = UWidgetTree::FindWidgetParent(NamedSlot, ChildIdx);
			if (Parent)
			{
				Parent->Modify();
				Parent->RemoveChild(NamedSlot);
			}
			else if (NamedSlot == WBP->WidgetTree->RootWidget)
			{
				WBP->WidgetTree->RootWidget = nullptr;
			}

			// Matches engine delete flow: Rename to transient before removing
			const FName SlotFName = NamedSlot->GetFName();
			NamedSlot->Rename(nullptr, GetTransientPackage());
			WBP->WidgetTree->RemoveWidget(NamedSlot);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			WBP->OnVariableRemoved(SlotFName);
#endif
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

			Session.Log(FString::Printf(TEXT("[OK] remove_named_slot(\"%s\")"), *FSlotName));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// set_named_slot_content(slot_name, widget_name)
		// ================================================================
		BPObj.set_function("set_named_slot_content", [FPath, &Session](sol::table /*self*/,
			const std::string& SlotName, sol::object WidgetObj, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FSlotName = UTF8_TO_TCHAR(SlotName.c_str());

			UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *FPath);
			if (!WBP || !WBP->WidgetTree)
			{
				Session.Log(TEXT("[FAIL] set_named_slot_content -> no widget tree"));
				return sol::lua_nil;
			}

			UWidget* SlotWidget = WBP->WidgetTree->FindWidget(::FName(*FSlotName));
			UNamedSlot* NamedSlot = Cast<UNamedSlot>(SlotWidget);
			if (!NamedSlot)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_named_slot_content(\"%s\") -> not found or not a NamedSlot"), *FSlotName));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaSetSlotContent", "Set Named Slot Content"));
			WBP->WidgetTree->Modify();
			NamedSlot->Modify();

			// Clear existing content if setting nil/empty
			if (WidgetObj.get_type() == sol::type::lua_nil)
			{
				if (NamedSlot->GetChildrenCount() > 0)
				{
					NamedSlot->ClearChildren();
				}
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
				Session.Log(FString::Printf(TEXT("[OK] set_named_slot_content(\"%s\") -> cleared"), *FSlotName));
				return sol::make_object(Lua, true);
			}

			// Find the widget to place in the slot
			if (!WidgetObj.is<std::string>())
			{
				Session.Log(TEXT("[FAIL] set_named_slot_content -> second arg must be widget name string or nil"));
				return sol::lua_nil;
			}

			FString FWidgetName = UTF8_TO_TCHAR(WidgetObj.as<std::string>().c_str());
			UWidget* ContentWidget = WBP->WidgetTree->FindWidget(::FName(*FWidgetName));
			if (!ContentWidget)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_named_slot_content -> widget '%s' not found"), *FWidgetName));
				return sol::lua_nil;
			}

			// Remove from current parent first
			int32 OldIdx;
			UPanelWidget* OldParent = UWidgetTree::FindWidgetParent(ContentWidget, OldIdx);
			if (OldParent)
			{
				OldParent->Modify();
				OldParent->RemoveChild(ContentWidget);
			}

			// Clear existing content and add new
			NamedSlot->ClearChildren();
			NamedSlot->AddChild(ContentWidget);

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

			Session.Log(FString::Printf(TEXT("[OK] set_named_slot_content(\"%s\", \"%s\")"), *FSlotName, *FWidgetName));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// add_widget(type, opts?) -> create and add widget to tree
		// ================================================================
		BPObj.set_function("add_widget", [FPath, &Session](sol::table /*self*/,
			const std::string& TypeName, sol::optional<sol::table> OptsOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FTypeName = UTF8_TO_TCHAR(TypeName.c_str());

			UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *FPath);
			if (!WBP || !WBP->WidgetTree)
			{
				Session.Log(TEXT("[FAIL] add_widget -> no widget tree"));
				return sol::lua_nil;
			}

			// Resolve widget class - try exact name, then with suffix variations
			UClass* WidgetClass = FindFirstObject<UClass>(*FTypeName, EFindFirstObjectOptions::NativeFirst);
			if (!WidgetClass)
				WidgetClass = FindFirstObject<UClass>(*(FTypeName + TEXT("Widget")), EFindFirstObjectOptions::NativeFirst);

			if (!WidgetClass || !WidgetClass->IsChildOf(UWidget::StaticClass()))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_widget(\"%s\") -> widget class not found"), *FTypeName));
				return sol::lua_nil;
			}

			// Parse options
			FString WidgetName;
			FString ParentName;
			int32 InsertIndex = INDEX_NONE;
			bool bIsVariable = false;

			if (OptsOpt.has_value())
			{
				sol::table Opts = OptsOpt.value();
				sol::optional<std::string> NameOpt = Opts.get<sol::optional<std::string>>("name");
				if (NameOpt.has_value())
					WidgetName = UTF8_TO_TCHAR(NameOpt.value().c_str());

				sol::optional<std::string> ParentOpt = Opts.get<sol::optional<std::string>>("parent");
				if (ParentOpt.has_value())
					ParentName = UTF8_TO_TCHAR(ParentOpt.value().c_str());

				sol::optional<int> IndexOpt = Opts.get<sol::optional<int>>("index");
				if (IndexOpt.has_value())
					InsertIndex = IndexOpt.value();

				sol::optional<bool> VarOpt = Opts.get<sol::optional<bool>>("is_variable");
				if (VarOpt.has_value())
					bIsVariable = VarOpt.value();
			}

			// Generate name if not provided
			FName FWidgetName = WidgetName.IsEmpty()
				? MakeUniqueObjectName(WBP->WidgetTree, WidgetClass)
				: ::FName(*WidgetName);

			// Check for name collision
			if (!WidgetName.IsEmpty() && WBP->WidgetTree->FindWidget(FWidgetName))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_widget -> widget '%s' already exists"), *WidgetName));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaAddWidget", "Add Widget"));
			WBP->WidgetTree->SetFlags(RF_Transactional);
			WBP->WidgetTree->Modify();

			// Construct the widget via WidgetTree (handles RF_Transactional, CreateWidget for UserWidgets)
			UWidget* NewWidget = WBP->WidgetTree->ConstructWidget<UWidget>(WidgetClass, FWidgetName);
			if (!NewWidget)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_widget(\"%s\") -> construction failed"), *FTypeName));
				return sol::lua_nil;
			}

			NewWidget->CreatedFromPalette();
			NewWidget->bIsVariable = bIsVariable;

			// Parent to specified parent, root panel, or set as root
			bool bAdded = false;
			if (!ParentName.IsEmpty())
			{
				UPanelWidget* Parent = Cast<UPanelWidget>(WBP->WidgetTree->FindWidget(::FName(*ParentName)));
				if (!Parent)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add_widget -> parent '%s' not found or not a panel"), *ParentName));
					return sol::lua_nil;
				}
				if (!Parent->CanAddMoreChildren())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add_widget -> parent '%s' cannot accept more children"), *ParentName));
					return sol::lua_nil;
				}
				Parent->Modify();
				if (InsertIndex >= 0)
				{
					int32 Idx = FMath::Min(InsertIndex, Parent->GetChildrenCount());
					Parent->InsertChildAt(Idx, NewWidget);
				}
				else
				{
					Parent->AddChild(NewWidget);
				}
				bAdded = true;
			}
			else if (WBP->WidgetTree->RootWidget)
			{
				UPanelWidget* RootPanel = Cast<UPanelWidget>(WBP->WidgetTree->RootWidget);
				if (RootPanel && RootPanel->CanAddMoreChildren())
				{
					RootPanel->Modify();
					if (InsertIndex >= 0)
					{
						int32 Idx = FMath::Min(InsertIndex, RootPanel->GetChildrenCount());
						RootPanel->InsertChildAt(Idx, NewWidget);
					}
					else
					{
						RootPanel->AddChild(NewWidget);
					}
					bAdded = true;
				}
				else if (!RootPanel)
				{
					// Root exists but is not a panel, and no parent specified
					Session.Log(TEXT("[FAIL] add_widget -> root is not a panel and no parent specified"));
					return sol::lua_nil;
				}
				else
				{
					Session.Log(TEXT("[FAIL] add_widget -> root panel cannot accept more children"));
					return sol::lua_nil;
				}
			}
			else
			{
				// No root widget yet, set as root
				WBP->WidgetTree->RootWidget = NewWidget;
				bAdded = true;
			}

			if (bAdded)
			{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
				WBP->OnVariableAdded(NewWidget->GetFName());
#endif
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
			}

			sol::table Result = WidgetHelper::WidgetToTable(Lua, NewWidget, WBP);
			Session.Log(FString::Printf(TEXT("[OK] add_widget(\"%s\") -> \"%s\""),
				*FTypeName, *NewWidget->GetName()));
			return Result;
		});

		// ================================================================
		// remove_widget(name) -> remove widget from tree with proper cleanup
		// ================================================================
		BPObj.set_function("remove_widget", [FPath, &Session](sol::table /*self*/,
			const std::string& WidgetName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FWidgetName = UTF8_TO_TCHAR(WidgetName.c_str());

			UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *FPath);
			if (!WBP || !WBP->WidgetTree)
			{
				Session.Log(TEXT("[FAIL] remove_widget -> no widget tree"));
				return sol::lua_nil;
			}

			UWidget* Widget = WBP->WidgetTree->FindWidget(::FName(*FWidgetName));
			if (!Widget)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove_widget(\"%s\") -> not found"), *FWidgetName));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaRemoveWidget", "Remove Widget"));
			WBP->WidgetTree->SetFlags(RF_Transactional);
			WBP->WidgetTree->Modify();
			WBP->Modify();

			const FName WidgetFName = Widget->GetFName();
			Widget->SetFlags(RF_Transactional);

			// Remove binding references for this widget
			for (int32 i = WBP->Bindings.Num() - 1; i >= 0; --i)
			{
				if (WBP->Bindings[i].ObjectName == Widget->GetName())
				{
					WBP->Bindings.RemoveAt(i);
				}
			}

			// Modify the widget's parent
			UPanelWidget* Parent = Widget->GetParent();
			if (Parent)
			{
				Parent->SetFlags(RF_Transactional);
				Parent->Modify();
			}

			Widget->Modify();
			bool bRemoved = WBP->WidgetTree->RemoveWidget(Widget);

			// If no parent, it may be in a named slot
			if (!bRemoved && !Parent)
			{
				if (Widget == WBP->WidgetTree->RootWidget)
				{
					WBP->WidgetTree->RootWidget = nullptr;
					bRemoved = true;
				}
			}

			if (bRemoved)
			{
				// Update desired focus
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
				FWidgetBlueprintEditorUtils::ReplaceDesiredFocus(WBP, WidgetFName, FName());
#endif

				// Rename to transient package to free the name
				Widget->Rename(nullptr, GetTransientPackage());

				// Remove variable data if no other widget has the same name
				const bool bHasWidgetWithSameName = WBP->GetAllSourceWidgets().ContainsByPredicate(
					[WidgetFName](const UWidget* W) { return WidgetFName == W->GetFName(); });
				if (!bHasWidgetWithSameName)
				{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
					WBP->OnVariableRemoved(WidgetFName);
#endif
				}

				// Also clean up child widgets
				TArray<UWidget*> ChildWidgets;
				UWidgetTree::GetChildWidgets(Widget, ChildWidgets);
				for (UWidget* Child : ChildWidgets)
				{
					const FName ChildName = Child->GetFName();
					Child->SetFlags(RF_Transactional);
					Child->Modify();
					Child->Rename(nullptr, GetTransientPackage());

					const bool bHasChild = WBP->GetAllSourceWidgets().ContainsByPredicate(
						[ChildName](const UWidget* W) { return ChildName == W->GetFName(); });
					if (!bHasChild)
					{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
						WBP->OnVariableRemoved(ChildName);
#endif
					}
				}

				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
			}

			Session.Log(FString::Printf(TEXT("[OK] remove_widget(\"%s\") -> %s"),
				*FWidgetName, bRemoved ? TEXT("removed") : TEXT("not found in tree")));
			return sol::make_object(Lua, bRemoved);
		});

		// ================================================================
		// configure_widget(name, opts) -> set properties, visibility, is_variable
		// ================================================================
		BPObj.set_function("configure_widget", [FPath, &Session](sol::table /*self*/,
			const std::string& WidgetName, sol::table Opts, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FWidgetName = UTF8_TO_TCHAR(WidgetName.c_str());

			UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *FPath);
			if (!WBP || !WBP->WidgetTree)
			{
				Session.Log(TEXT("[FAIL] configure_widget -> no widget tree"));
				return sol::lua_nil;
			}

			UWidget* Widget = WBP->WidgetTree->FindWidget(::FName(*FWidgetName));
			if (!Widget)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] configure_widget(\"%s\") -> not found"), *FWidgetName));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaConfigWidget", "Configure Widget"));
			Widget->Modify();
			int32 ChangedCount = 0;

			// Special keys handled explicitly
			static const TSet<FString> SpecialKeys = { TEXT("is_variable"), TEXT("visibility"), TEXT("tooltip"), TEXT("properties"), TEXT("slot") };

			// Helper: convert a sol::object to ImportText string (handles scalars + nested tables)
			std::function<TOptional<FString>(const sol::object&)> SolValueToImportText;
			SolValueToImportText = [&SolValueToImportText](const sol::object& Val) -> TOptional<FString>
			{
				if (Val.is<std::string>())
					return FString(UTF8_TO_TCHAR(Val.as<std::string>().c_str()));
				if (Val.is<bool>())
					return FString(Val.as<bool>() ? TEXT("True") : TEXT("False"));
				if (Val.is<int>())
					return FString::FromInt(Val.as<int>());
				if (Val.is<double>())
					return FString::SanitizeFloat(Val.as<double>());
				if (Val.is<sol::table>())
				{
					// Convert table to UE struct ImportText format: (Key=Val,Key=Val,...)
					// Supports nested tables for nested structs (e.g. FSlateColor.SpecifiedColor)
					sol::table T = Val.as<sol::table>();
					TArray<FString> Parts;
					for (auto& P : T)
					{
						if (!P.first.is<std::string>()) continue;
						FString K = UTF8_TO_TCHAR(P.first.as<std::string>().c_str());
						// Capitalize first letter for UE property name convention (left->Left, size->Size)
						if (K.Len() > 0)
						{
							K[0] = FChar::ToUpper(K[0]);
						}
						// Recurse for nested tables
						TOptional<FString> V = SolValueToImportText(P.second);
						if (V.IsSet())
						{
							Parts.Add(FString::Printf(TEXT("%s=%s"), *K, *V.GetValue()));
						}
					}
					if (Parts.Num() > 0)
						return FString::Printf(TEXT("(%s)"), *FString::Join(Parts, TEXT(",")));
					return TOptional<FString>();
				}
				return TOptional<FString>();
			};

			// Helper: set a UProperty on a UObject, using CallSetter when available
			// Supports partial struct updates: copies current value first, then applies ImportText on top,
			// so {Font='(Size=24)'} only changes Size without wiping FontObject/TypefaceFontName/etc.
			auto SetPropertyValue = [&Session](FProperty* Prop, UObject* Obj, const FString& TextValue, const FString& WidgetName) -> bool
			{
				// Allocate temp buffer and seed it with the CURRENT property value so that
				// partial ImportText only overwrites mentioned fields (e.g. Font.Size)
				TArray<uint8> TempBuf;
				TempBuf.SetNumZeroed(Prop->GetSize());
				Prop->InitializeValue(TempBuf.GetData());

				// Copy current value into temp buffer so unmentioned struct fields are preserved
				if (const void* CurrentPtr = Prop->ContainerPtrToValuePtr<void>(Obj))
				{
					Prop->CopyCompleteValue(TempBuf.GetData(), CurrentPtr);
				}

				const TCHAR* Result = Prop->ImportText_Direct(*TextValue, TempBuf.GetData(), Obj, PPF_None);
				if (!Result)
				{
					Prop->DestroyValue(TempBuf.GetData());
					Session.Log(FString::Printf(TEXT("[WARN] configure_widget(\"%s\") -> failed to parse '%s' for property '%s'"),
						*WidgetName, *TextValue, *Prop->GetName()));
					return false;
				}

				if (Prop->HasSetter())
				{
					Prop->CallSetter(Obj, TempBuf.GetData());
				}
				else
				{
					void* DestPtr = Prop->ContainerPtrToValuePtr<void>(Obj);
					Prop->CopyCompleteValue(DestPtr, TempBuf.GetData());
				}

				Prop->DestroyValue(TempBuf.GetData());

				// Auto-enable edit-condition toggle (e.g. bOverride_WidthOverride for SizeBox)
				// UE uses "editcondition" metadata to link a property to its boolean toggle.
				// When the value property is set, the toggle must be enabled too.
				const FString EditCondition = Prop->GetMetaData(TEXT("editcondition"));
				if (!EditCondition.IsEmpty())
				{
					FBoolProperty* ToggleProp = CastField<FBoolProperty>(Obj->GetClass()->FindPropertyByName(*EditCondition));
					if (ToggleProp)
					{
						// Check if the toggle property has InlineEditConditionToggle metadata
						// (this distinguishes override booleans from arbitrary editconditions)
						if (ToggleProp->HasMetaData(TEXT("InlineEditConditionToggle")))
						{
							ToggleProp->SetPropertyValue_InContainer(Obj, true);
						}
					}
				}

				// Fire PostEditChangeProperty for the specific property
				FPropertyChangedEvent PCE(Prop);
				Obj->PostEditChangeProperty(PCE);
				return true;
			};

			// is_variable
			sol::optional<bool> VarOpt = Opts.get<sol::optional<bool>>("is_variable");
			if (VarOpt.has_value())
			{
				Widget->bIsVariable = VarOpt.value();
				ChangedCount++;
			}

			// visibility
			sol::optional<std::string> VisOpt = Opts.get<sol::optional<std::string>>("visibility");
			if (VisOpt.has_value())
			{
				FString VisStr = UTF8_TO_TCHAR(VisOpt.value().c_str());
				if (VisStr.Equals(TEXT("Visible"), ESearchCase::IgnoreCase))
					Widget->SetVisibility(ESlateVisibility::Visible);
				else if (VisStr.Equals(TEXT("Collapsed"), ESearchCase::IgnoreCase))
					Widget->SetVisibility(ESlateVisibility::Collapsed);
				else if (VisStr.Equals(TEXT("Hidden"), ESearchCase::IgnoreCase))
					Widget->SetVisibility(ESlateVisibility::Hidden);
				else if (VisStr.Equals(TEXT("HitTestInvisible"), ESearchCase::IgnoreCase))
					Widget->SetVisibility(ESlateVisibility::HitTestInvisible);
				else if (VisStr.Equals(TEXT("SelfHitTestInvisible"), ESearchCase::IgnoreCase))
					Widget->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
				else
				{
					Session.Log(FString::Printf(TEXT("[WARN] configure_widget -> unknown visibility '%s'"), *VisStr));
				}
				ChangedCount++;
			}

			// tooltip
			sol::optional<std::string> TooltipOpt = Opts.get<sol::optional<std::string>>("tooltip");
			if (TooltipOpt.has_value())
			{
				FProperty* Prop = Widget->GetClass()->FindPropertyByName(TEXT("ToolTipText"));
				if (Prop)
				{
					FString Value = UTF8_TO_TCHAR(TooltipOpt.value().c_str());
					SetPropertyValue(Prop, Widget, Value, FWidgetName);
					ChangedCount++;
				}
			}

			// Generic property setting — collect from both top-level keys and "properties" sub-table
			auto ApplyProperties = [&](const sol::table& Props, UObject* Target, const TCHAR* TargetLabel)
			{
				for (auto& Pair : Props)
				{
					if (!Pair.first.is<std::string>()) continue;
					FString PropName = UTF8_TO_TCHAR(Pair.first.as<std::string>().c_str());

					TOptional<FString> PropValue = SolValueToImportText(Pair.second);
					if (!PropValue.IsSet())
					{
						Session.Log(FString::Printf(TEXT("[WARN] configure_widget(\"%s\") -> cannot convert value for '%s' on %s"),
							*FWidgetName, *PropName, TargetLabel));
						continue;
					}

					FProperty* Prop = Target->GetClass()->FindPropertyByName(::FName(*PropName));
					if (Prop && Prop->HasAnyPropertyFlags(CPF_Edit))
					{
						if (SetPropertyValue(Prop, Target, PropValue.GetValue(), FWidgetName))
							ChangedCount++;
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[WARN] configure_widget(\"%s\") -> property '%s' not found or not editable on %s"),
							*FWidgetName, *PropName, TargetLabel));
					}
				}
			};

			// Explicit "properties" sub-table
			sol::optional<sol::table> PropsOpt = Opts.get<sol::optional<sol::table>>("properties");
			if (PropsOpt.has_value())
			{
				ApplyProperties(PropsOpt.value(), Widget, TEXT("widget"));
			}

			// Top-level keys that aren't special — treat as property names on the widget
			for (auto& Pair : Opts)
			{
				if (!Pair.first.is<std::string>()) continue;
				FString Key = UTF8_TO_TCHAR(Pair.first.as<std::string>().c_str());
				if (SpecialKeys.Contains(Key)) continue;

				TOptional<FString> PropValue = SolValueToImportText(Pair.second);
				if (!PropValue.IsSet()) continue;

				FProperty* Prop = Widget->GetClass()->FindPropertyByName(::FName(*Key));
				if (Prop && Prop->HasAnyPropertyFlags(CPF_Edit))
				{
					if (SetPropertyValue(Prop, Widget, PropValue.GetValue(), FWidgetName))
						ChangedCount++;
				}
			}

			// Slot properties via "slot" sub-table
			sol::optional<sol::table> SlotOpt = Opts.get<sol::optional<sol::table>>("slot");
			if (SlotOpt.has_value() && Widget->Slot)
			{
				Widget->Slot->Modify();
				ApplyProperties(SlotOpt.value(), Widget->Slot, TEXT("slot"));
				Widget->Slot->SynchronizeProperties();
			}

			if (ChangedCount > 0)
			{
				FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
			}

			Session.Log(FString::Printf(TEXT("[OK] configure_widget(\"%s\") -> %d changes"), *FWidgetName, ChangedCount));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// rename_widget(old_name, new_name) -> rename widget + update all references
		// ================================================================
		BPObj.set_function("rename_widget", [FPath, &Session](sol::table /*self*/,
			const std::string& OldName, const std::string& NewName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FOldName = UTF8_TO_TCHAR(OldName.c_str());
			FString FNewDisplayName = UTF8_TO_TCHAR(NewName.c_str());

			UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *FPath);
			if (!WBP || !WBP->WidgetTree)
			{
				Session.Log(TEXT("[FAIL] rename_widget -> no widget tree"));
				return sol::lua_nil;
			}

			FName OldObjectName(*FOldName);
			UWidget* Widget = WBP->WidgetTree->FindWidget(OldObjectName);
			if (!Widget)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] rename_widget(\"%s\") -> not found"), *FOldName));
				return sol::lua_nil;
			}

			UClass* ParentClass = WBP->ParentClass;
			if (!ParentClass)
			{
				Session.Log(TEXT("[FAIL] rename_widget -> no parent class"));
				return sol::lua_nil;
			}

			// Sanitize the new name (mirrors engine SanitizeWidgetName)
			FString GeneratedName = SlugStringForValidName(FNewDisplayName);
			if (GeneratedName.IsEmpty())
			{
				GeneratedName = FOldName;
			}
			FName NewFName(*GeneratedName);

			// Validate uniqueness
			TSharedPtr<INameValidatorInterface> NameValidator = MakeShareable(new FKismetNameValidator(WBP, OldObjectName));

			// Check for BindWidget property match (allows reuse of existing property name)
			FObjectPropertyBase* ExistingProperty = CastField<FObjectPropertyBase>(ParentClass->FindPropertyByName(NewFName));
			const bool bBindWidget = ExistingProperty
				&& FWidgetBlueprintEditorUtils::IsBindWidgetProperty(ExistingProperty)
				&& Widget->IsA(ExistingProperty->PropertyClass);

			const bool bUniqueNameForTemplate = (EValidatorResult::Ok == NameValidator->IsValid(NewFName) || bBindWidget);
			if (!bUniqueNameForTemplate)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] rename_widget(\"%s\", \"%s\") -> name '%s' is not valid or already in use"),
					*FOldName, *FNewDisplayName, *NewFName.ToString()));
				return sol::lua_nil;
			}

			const FString NewNameStr = NewFName.ToString();
			const FString OldNameStr = OldObjectName.ToString();

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaRenameWidget", "Rename Widget"));
			WBP->Modify();
			Widget->Modify();

			// Notify blueprint about variable rename (updates WidgetRenameMap etc.)
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			WBP->OnVariableRenamed(OldObjectName, NewFName);
#endif

			// Rename the template widget
			Widget->SetDisplayLabel(FNewDisplayName);
			Widget->Rename(*NewNameStr);

			// Update desired focus widget reference
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			FWidgetBlueprintEditorUtils::ReplaceDesiredFocus(WBP, OldObjectName, NewFName);
#endif

			// Update delegate/property binding references
			for (FDelegateEditorBinding& Binding : WBP->Bindings)
			{
				if (Binding.ObjectName == OldNameStr)
				{
					Binding.ObjectName = NewNameStr;
				}
			}

			// Update animation binding references
			for (UWidgetAnimation* WidgetAnimation : WBP->Animations)
			{
				if (!WidgetAnimation) continue;
				for (FWidgetAnimationBinding& AnimBinding : WidgetAnimation->AnimationBindings)
				{
					if (AnimBinding.WidgetName == OldObjectName)
					{
						AnimBinding.WidgetName = NewFName;

						if (WidgetAnimation->MovieScene)
						{
							WidgetAnimation->MovieScene->Modify();

							if (AnimBinding.SlotWidgetName == NAME_None)
							{
								FMovieScenePossessable* Possessable = WidgetAnimation->MovieScene->FindPossessable(AnimBinding.AnimationGuid);
								if (Possessable)
								{
									Possessable->SetName(NewFName.ToString());
								}
							}
							else
							{
								break;
							}
						}
					}
				}
			}

			// Update navigation widget references
			WBP->WidgetTree->ForEachWidget([OldObjectName, NewFName](UWidget* W)
			{
				if (W && W->Navigation)
				{
					W->Navigation->SetFlags(RF_Transactional);
					W->Navigation->Modify();
					W->Navigation->TryToRenameBinding(OldObjectName, NewFName);
				}
			});

			// Validate child blueprints and adjust variable names
			FBlueprintEditorUtils::ValidateBlueprintChildVariables(WBP, NewFName);

			// Refresh references and flush editors
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

			// Update variable references and event references
			FBlueprintEditorUtils::ReplaceVariableReferences(WBP, OldObjectName, NewFName);

			Session.Log(FString::Printf(TEXT("[OK] rename_widget(\"%s\" -> \"%s\")"), *FOldName, *NewNameStr));

			sol::table Result = WidgetHelper::WidgetToTable(Lua, Widget, WBP);
			return Result;
		});
	});
}

REGISTER_LUA_BINDING(WidgetBlueprint, WidgetBlueprintDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindWidgetBlueprint(Lua, Session);
});
