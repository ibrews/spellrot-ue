#include "Modules/ModuleManager.h"
class FAIK_ControlRigModule : public IModuleInterface
{
public:
    virtual void StartupModule() override {}
    virtual void ShutdownModule() override {}
};
IMPLEMENT_MODULE(FAIK_ControlRigModule, AIK_ControlRig)
