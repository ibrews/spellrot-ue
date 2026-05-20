#include "Modules/ModuleManager.h"
class FAIK_Paper2DModule : public IModuleInterface
{
public:
    virtual void StartupModule() override {}
    virtual void ShutdownModule() override {}
};
IMPLEMENT_MODULE(FAIK_Paper2DModule, AIK_Paper2D)
