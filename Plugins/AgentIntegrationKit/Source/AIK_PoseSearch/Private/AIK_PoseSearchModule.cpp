// Copyright 2025-2026 Betide Studio. All Rights Reserved.

#include "Modules/ModuleManager.h"

class FAIK_PoseSearchModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// Bindings auto-register via REGISTER_LUA_BINDING static initializers.
		// Nothing else needed here.
	}

	virtual void ShutdownModule() override
	{
	}
};

IMPLEMENT_MODULE(FAIK_PoseSearchModule, AIK_PoseSearch)
