#include "Modules/ModuleManager.h"
class FAIK_ChooserModule : public IModuleInterface
{
public:
    virtual void StartupModule() override {}
    virtual void ShutdownModule() override {}
};
IMPLEMENT_MODULE(FAIK_ChooserModule, AIK_Chooser)
