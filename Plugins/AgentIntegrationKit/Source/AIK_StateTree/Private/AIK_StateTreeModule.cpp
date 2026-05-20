#include "Modules/ModuleManager.h"
class FAIK_StateTreeModule : public IModuleInterface
{
public:
    virtual void StartupModule() override {}
    virtual void ShutdownModule() override {}
};
IMPLEMENT_MODULE(FAIK_StateTreeModule, AIK_StateTree)
