#include "Modules/ModuleManager.h"
class FAIK_MetaHumanModule : public IModuleInterface
{
public:
    virtual void StartupModule() override {}
    virtual void ShutdownModule() override {}
};
IMPLEMENT_MODULE(FAIK_MetaHumanModule, AIK_MetaHuman)
