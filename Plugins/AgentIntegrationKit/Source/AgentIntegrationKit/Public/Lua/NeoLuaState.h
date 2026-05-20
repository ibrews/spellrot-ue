#pragma once

#include "CoreMinimal.h"
#include "Tools/NeoStackToolBase.h"

struct FScriptResult
{
	bool bSuccess = false;
	TArray<FString> Trace;
	TArray<FToolResultImage> Images;
	FString Error;
	FString ReturnValue;
};

class FNeoLuaState
{
public:
	FScriptResult Execute(const FString& Script);

private:
	TArray<FString> Trace;
	void AppendTrace(const FString& Entry);
};
