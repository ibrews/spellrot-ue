// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Tools/ExecuteNeoStackLuaTool.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Lua/LuaBindingRegistry.h"
#include "Lua/NeoLuaState.h"

FString FExecuteNeoStackLuaTool::GetDescription() const
{
	return FLuaBindingRegistry::Get().BuildDescription();
}

TSharedPtr<FJsonObject> FExecuteNeoStackLuaTool::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> ScriptProp = MakeShared<FJsonObject>();
	ScriptProp->SetStringField(TEXT("type"), TEXT("string"));
	ScriptProp->SetStringField(TEXT("description"), TEXT("Lua script to execute"));
	Props->SetObjectField(TEXT("script"), ScriptProp);

	Schema->SetObjectField(TEXT("properties"), Props);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("script")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

FToolResult FExecuteNeoStackLuaTool::Execute(const TSharedPtr<FJsonObject>& Args)
{
	if (!Args.IsValid())
	{
		return FToolResult::Fail(TEXT("Invalid arguments"));
	}

	FString Script;
	if (!Args->TryGetStringField(TEXT("script"), Script) || Script.IsEmpty())
	{
		return FToolResult::Fail(TEXT("'script' parameter is required"));
	}

	FNeoLuaState LuaState;
	FScriptResult Result = LuaState.Execute(Script);

	FString Output;
	for (const FString& Line : Result.Trace)
	{
		Output += Line + TEXT("\n");
	}

	FToolResult ToolResult = Result.bSuccess ? FToolResult::Ok(Output) : FToolResult::Fail(Output);
	ToolResult.Images = MoveTemp(Result.Images);
	return ToolResult;
}
