#include "Modules/ModuleManager.h"
class FAIK_GameplayAbilitiesModule : public IModuleInterface
{
public:
    virtual void StartupModule() override {}
    virtual void ShutdownModule() override {}
};
IMPLEMENT_MODULE(FAIK_GameplayAbilitiesModule, AIK_GameplayAbilities)
