#include "Modules/ModuleManager.h"
class FAIK_LiveLinkModule : public IModuleInterface
{
public:
    virtual void StartupModule() override {}
    virtual void ShutdownModule() override {}
};
IMPLEMENT_MODULE(FAIK_LiveLinkModule, AIK_LiveLink)
