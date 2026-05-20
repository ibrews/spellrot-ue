#include "Modules/ModuleManager.h"
class FAIK_WaterModule : public IModuleInterface
{
public:
    virtual void StartupModule() override {}
    virtual void ShutdownModule() override {}
};
IMPLEMENT_MODULE(FAIK_WaterModule, AIK_Water)
