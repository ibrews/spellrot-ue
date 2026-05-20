#include "Modules/ModuleManager.h"
class FAIK_PythonModule : public IModuleInterface
{
public:
    virtual void StartupModule() override {}
    virtual void ShutdownModule() override {}
};
IMPLEMENT_MODULE(FAIK_PythonModule, AIK_Python)
