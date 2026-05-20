// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Widgets/SACPAttachmentDropTarget.h"

#include "CoreMinimal.h"
#include "EditorAssetLibrary.h"
#include "DragAndDrop/AssetDragDropOp.h"
#include "Engine/Blueprint.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"

#include "ACPAttachmentManager.h"

namespace
{
	TArray<FAssetData> GetAssetsFromAssetDragDrop(TSharedRef<FAssetDragDropOp> AssetDragDrop)
	{
		TArray<FAssetData> Assets;
		Assets.Append(AssetDragDrop->GetAssets());

		for (const FString& Path : AssetDragDrop->GetAssetPaths())
		{
			TArray<FString> AssetsInPath = UEditorAssetLibrary::ListAssets(Path, true, true);
			for (const FString& AssetPath : AssetsInPath)
			{
				FAssetData AssetData =  UEditorAssetLibrary::FindAssetData(AssetPath);
				Assets.Add(AssetData);
			}
		}
		
		return Assets;
	}
}

void SACPAttachmentDropTarget::Construct(const FArguments& InArgs)
{
	SDropTarget::Construct(
		SDropTarget::FArguments()
		.Content()[InArgs._Content.Widget]
	);
}

bool SACPAttachmentDropTarget::OnIsRecognized(TSharedPtr<FDragDropOperation> DragDropOperation) const
{
	if (DragDropOperation && DragDropOperation->IsOfType<FAssetDragDropOp>())
	{
		return true;
	}

	if (DragDropOperation && DragDropOperation->IsOfType<FExternalDragOperation>())
	{
		return true;
	}

	return false;
}

bool SACPAttachmentDropTarget::OnAllowDrop(TSharedPtr<FDragDropOperation> DragDropOperation) const
{
	//NOTE: here we could add some extra checks (e.g.: don't alloow dropping while the agent thinks).
	return OnIsRecognized(DragDropOperation);
}

void SACPAttachmentDropTarget::OnDragEnter(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent)
{
	SDropTarget::OnDragEnter(MyGeometry, DragDropEvent);

	if (TSharedPtr<FAssetDragDropOp> AssetOp = DragDropEvent.GetOperationAs<FAssetDragDropOp>())
	{
		const TArray<FAssetData>& Assets = GetAssetsFromAssetDragDrop(AssetOp.ToSharedRef());
		const FText Label = Assets.Num() == 1
			? FText::Format(NSLOCTEXT("ACPDropTarget", "AttachSingle", "Attach '{0}' to Agent"), FText::FromName(Assets[0].AssetName))
			: FText::Format(NSLOCTEXT("ACPDropTarget", "AttachMultiple", "Attach {0} assets to Agent"), FText::AsNumber(Assets.Num()));

		AssetOp->SetToolTip(Label, FAppStyle::GetBrush("ContentBrowser.AddContent"));
	}
	if (TSharedPtr<FExternalDragOperation> ExternalOp = DragDropEvent.GetOperationAs<FExternalDragOperation>())
	{
		//TODO: FExternalDragOperation is not a FDecoratedDragDropOp so we cannot set a custom tooltip.
		// If we want feedback we could do a custom Custom overlay widget or cursor override
	}
}

void SACPAttachmentDropTarget::OnDragLeave(const FDragDropEvent& DragDropEvent)
{
	if (TSharedPtr<FAssetDragDropOp> AssetOp = DragDropEvent.GetOperationAs<FAssetDragDropOp>())
	{
		AssetOp->ResetToDefaultToolTip();
	}
	if (TSharedPtr<FExternalDragOperation> ExternalOp = DragDropEvent.GetOperationAs<FExternalDragOperation>())
	{
		//TODO: Reminder to undo whatever we decide to do in SACPAttachmentDropTarget::OnDragEnter
	}

	SDropTarget::OnDragLeave(DragDropEvent);
}

FReply SACPAttachmentDropTarget::OnDrop(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent)
{
	if (TSharedPtr<FAssetDragDropOp> AssetOp = DragDropEvent.GetOperationAs<FAssetDragDropOp>())
	{
		const TArray<FAssetData>& Assets = GetAssetsFromAssetDragDrop(AssetOp.ToSharedRef());
		for (const FAssetData& Asset : Assets)
		{
			UObject* LoadedAsset = Asset.GetAsset();
			if (UBlueprint* Blueprint = Cast<UBlueprint>(LoadedAsset))
			{
				FACPAttachmentManager::Get().AddBlueprintAsset(Blueprint);
			}
			else
			{
				FACPAttachmentManager::Get().AddObjectContext(LoadedAsset);
			}
		}
	}
	if (TSharedPtr<FExternalDragOperation> ExternalOp = DragDropEvent.GetOperationAs<FExternalDragOperation>())
	{
		for (const FString& FilePath : ExternalOp->GetFiles())
		{
			const FString Extension = FPaths::GetExtension(FilePath).ToLower();
			if (Extension == TEXT("png") || Extension == TEXT("jpg") || Extension == TEXT("jpeg"))
			{
				FACPAttachmentManager::Get().AddImageFromFile(FilePath);
			}
			else
			{
				FACPAttachmentManager::Get().AddFileFromPath(FilePath);
			}
		}
	}

	return SDropTarget::OnDrop(MyGeometry, DragDropEvent);
}
