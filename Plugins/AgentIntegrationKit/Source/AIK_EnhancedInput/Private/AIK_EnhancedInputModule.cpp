#include "Modules/ModuleManager.h"
class FAIK_EnhancedInputModule : public IModuleInterface
{
public:
    virtual void StartupModule() override {}
    virtual void ShutdownModule() override {}
};
IMPLEMENT_MODULE(FAIK_EnhancedInputModule, AIK_EnhancedInput)
