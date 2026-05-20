#include "Modules/ModuleManager.h"
class FAIK_ChaosFractureModule : public IModuleInterface
{
public:
    virtual void StartupModule() override {}
    virtual void ShutdownModule() override {}
};
IMPLEMENT_MODULE(FAIK_ChaosFractureModule, AIK_ChaosFracture)
