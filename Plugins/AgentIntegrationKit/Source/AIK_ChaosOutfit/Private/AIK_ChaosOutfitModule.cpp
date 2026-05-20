#include "Modules/ModuleManager.h"
class FAIK_ChaosOutfitModule : public IModuleInterface
{
public:
    virtual void StartupModule() override {}
    virtual void ShutdownModule() override {}
};
IMPLEMENT_MODULE(FAIK_ChaosOutfitModule, AIK_ChaosOutfit)
