#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FBlueprintImporterModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    void RegisterMenus();
    void ExecuteImport();
    void ExecuteWidgetImport();

    /** Opens the "Paste AnimBP" dialog and imports from decompiled text. */
    void ExecuteAnimBPTextImport();

    /** Opens the level JSON import dialog and spawns actors into the current level. */
    void ExecuteLevelImport();

    /** Opens the Niagara UE5 → UE4 dialog and recreates the script in UE4.26. */
    void ExecuteNiagaraImport();
};
