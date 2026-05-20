#include "Modules/ModuleManager.h"
class FAIK_SmartObjectsModule : public IModuleInterface
{
public:
    virtual void StartupModule() override {}
    virtual void ShutdownModule() override {}
};
IMPLEMENT_MODULE(FAIK_SmartObjectsModule, AIK_SmartObjects)
