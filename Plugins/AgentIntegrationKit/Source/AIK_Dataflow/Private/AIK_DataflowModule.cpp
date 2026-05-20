#include "Modules/ModuleManager.h"
class FAIK_DataflowModule : public IModuleInterface
{
public:
    virtual void StartupModule() override {}
    virtual void ShutdownModule() override {}
};
IMPLEMENT_MODULE(FAIK_DataflowModule, AIK_Dataflow)
