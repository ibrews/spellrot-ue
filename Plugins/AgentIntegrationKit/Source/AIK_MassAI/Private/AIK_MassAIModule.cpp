#include "Modules/ModuleManager.h"
class FAIK_MassAIModule : public IModuleInterface
{
public:
    virtual void StartupModule() override {}
    virtual void ShutdownModule() override {}
};
IMPLEMENT_MODULE(FAIK_MassAIModule, AIK_MassAI)
