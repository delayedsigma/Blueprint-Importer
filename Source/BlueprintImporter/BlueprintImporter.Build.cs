using UnrealBuildTool;

public class BlueprintImporter : ModuleRules
{
    public BlueprintImporter(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
        CppStandard = CppStandardVersion.Cpp17;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "CoreUObject",
            "Engine",
            "Slate",
            "SlateCore",
            "EditorStyle",
            "UnrealEd",
            "BlueprintGraph",
            "KismetCompiler",
            "Kismet",
            "GraphEditor",
            "Json",
            "JsonUtilities",
            "ToolMenus",
            "AssetTools",
            "AssetRegistry",
            "MainFrame",
            "DesktopPlatform",
            "InputCore",
            "Projects",
            "LevelEditor",
            "UMG",
            "UMGEditor",
            "AnimationBlueprintEditor",
            "AnimGraphRuntime",
            "AnimGraph",

            // ── Niagara UE5→UE4 feature ───────────────────────
            "Niagara",          // UNiagaraScript, UNiagaraScriptSource, FNiagaraVariable
            "NiagaraEditor",    // UNiagaraGraph, UNiagaraNode* all node types
            "NiagaraCore",      // FNiagaraTypeDefinition, FNiagaraVariable core types
        });
    }
}
