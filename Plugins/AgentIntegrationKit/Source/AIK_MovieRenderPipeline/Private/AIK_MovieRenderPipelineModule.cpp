#include "Modules/ModuleManager.h"
class FAIK_MovieRenderPipelineModule : public IModuleInterface
{
public:
    virtual void StartupModule() override {}
    virtual void ShutdownModule() override {}
};
IMPLEMENT_MODULE(FAIK_MovieRenderPipelineModule, AIK_MovieRenderPipeline)
