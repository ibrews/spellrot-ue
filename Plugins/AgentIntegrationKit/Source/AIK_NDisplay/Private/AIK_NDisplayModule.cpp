#include "Modules/ModuleManager.h"
class FAIK_NDisplayModule : public IModuleInterface
{
public:
    virtual void StartupModule() override {}
    virtual void ShutdownModule() override {}
};
IMPLEMENT_MODULE(FAIK_NDisplayModule, AIK_NDisplay)
