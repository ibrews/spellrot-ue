// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "SDropTarget.h"

// Items dragged onto this target will be automatically added as attachments via FACPAttachmentManager
// Also handles drag & drop feedback (highlight drop zone + add tooltip when hovering)
// This should support:
// - dragging external files (e.g.: pictures) => adds that as a raw picture
// - dragging single assets => adds that asset
// - dragging a folder => adds all the assets in that folder recursively
class SACPAttachmentDropTarget : public SDropTarget
{
public:
	SLATE_BEGIN_ARGS(SACPAttachmentDropTarget) {}
		SLATE_DEFAULT_SLOT(FArguments, Content)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	//~Begin SDropTarget Interface
	virtual bool OnIsRecognized(TSharedPtr<FDragDropOperation> DragDropOperation) const override;
	virtual bool OnAllowDrop(TSharedPtr<FDragDropOperation> DragDropOperation) const override;
	virtual void OnDragEnter(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent) override;
	virtual void OnDragLeave(const FDragDropEvent& DragDropEvent) override;
	virtual FReply OnDrop(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent) override;
	//~End SDropTarget Interface
};
