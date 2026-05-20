// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Tools/NeoStackToolBase.h"

class AGENTINTEGRATIONKIT_API FExecuteNeoStackLuaTool : public FNeoStackToolBase
{
public:
	virtual FString GetName() const override { return TEXT("execute_script"); }
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Args) override;
};
