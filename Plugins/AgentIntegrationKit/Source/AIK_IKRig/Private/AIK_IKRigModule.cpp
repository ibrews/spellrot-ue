#include "Modules/ModuleManager.h"
class FAIK_IKRigModule : public IModuleInterface
{
public:
    virtual void StartupModule() override {}
    virtual void ShutdownModule() override {}
};
IMPLEMENT_MODULE(FAIK_IKRigModule, AIK_IKRig)
