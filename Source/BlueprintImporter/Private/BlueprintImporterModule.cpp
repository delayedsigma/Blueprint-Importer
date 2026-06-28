#include "BlueprintImporterModule.h"
#include "BlueprintJsonParser.h"
#include "BlueprintGraphBuilder.h"

#include "Modules/ModuleManager.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "LevelEditor.h"
#include "ToolMenus.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "Interfaces/IMainFrameModule.h"
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "AssetToolsModule.h"
#include "AssetRegistryModule.h"
#include "PackageTools.h"
#include "UObject/Package.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Event.h"
#include "Misc/MessageDialog.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Editor.h"

#include "Blueprint/UserWidget.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimInstance.h"

#include "AnimBPTextParser.h"
#include "AnimBPGraphBuilder.h"
#include "SAnimBPPasteDialog.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"

#define LOCTEXT_NAMESPACE "FBlueprintImporterModule"

IMPLEMENT_MODULE(FBlueprintImporterModule, BlueprintImporter)

static void ShowNotification(const FText &Message, bool bSuccess)
{
    FNotificationInfo Info(Message);
    Info.bFireAndForget = true;
    Info.ExpireDuration = 5.f;
    Info.bUseSuccessFailIcons = true;
    TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info);
    if (Item.IsValid())
    {
        Item->SetCompletionState(bSuccess ? SNotificationItem::CS_Success
                                          : SNotificationItem::CS_Fail);
    }
}

void FBlueprintImporterModule::StartupModule()
{
    UToolMenus::RegisterStartupCallback(
        FSimpleMulticastDelegate::FDelegate::CreateRaw(
            this, &FBlueprintImporterModule::RegisterMenus));
}

void FBlueprintImporterModule::ShutdownModule()
{
    UToolMenus::UnRegisterStartupCallback(this);
    UToolMenus::UnregisterOwner(this);
}

void FBlueprintImporterModule::RegisterMenus()
{
    FToolMenuOwnerScoped OwnerScoped(this);

    UToolMenu *FileMenu = UToolMenus::Get()->ExtendMenu("MainFrame.MainMenu.File");
    FToolMenuSection &Section = FileMenu->FindOrAddSection("BlueprintImporterSection");

    Section.AddMenuEntry(
        "ImportBlueprintFromJSON",
        LOCTEXT("ImportBlueprintLabel", "Blueprint Importer\u2026"),
        LOCTEXT("ImportBlueprintTooltip", "Select a UAssetAPI JSON export and recreate its Blueprint graphs."),
        FSlateIcon(FEditorStyle::GetStyleSetName(), "LevelEditor.OpenLevel"),
        FUIAction(FExecuteAction::CreateRaw(this, &FBlueprintImporterModule::ExecuteImport)));

    Section.AddMenuEntry(
        "ImportWidgetBlueprintFromJSON",
        LOCTEXT("ImportWidgetBlueprintLabel", "Widget Blueprint Importer\u2026"),
        LOCTEXT("ImportWidgetBlueprintTooltip", "Select a UAssetAPI JSON export of a Widget Blueprint and recreate its event graph."),
        FSlateIcon(FEditorStyle::GetStyleSetName(), "LevelEditor.OpenLevel"),
        FUIAction(FExecuteAction::CreateRaw(this, &FBlueprintImporterModule::ExecuteWidgetImport)));

    Section.AddMenuEntry(
        "ImportAnimBPFromText",
        LOCTEXT("ImportAnimBPGraphLabel", "AnimBP Graph\u2026"),
        LOCTEXT("ImportAnimBPGraphTooltip",
            "Paste a decompiled AnimBlueprint class dump (CUE4Parse / FModel format) "
            "to recreate its state machines, states, transitions, and layer interface stubs."),
        FSlateIcon(FEditorStyle::GetStyleSetName(), "ClassIcon.AnimBlueprint"),
        FUIAction(FExecuteAction::CreateRaw(this, &FBlueprintImporterModule::ExecuteAnimBPTextImport)));

    Section.AddMenuEntry(
        "ImportLevelFromJSON",
        LOCTEXT("ImportLevelLabel", "Level from JSON\u2026"),
        LOCTEXT("ImportLevelTooltip",
            "Import actors from a UAssetAPI / FModel level JSON export into the currently open level. "
            "Actors whose class doesn't exist in the project are skipped automatically."),
        FSlateIcon(FEditorStyle::GetStyleSetName(), "LevelEditor.NewLevel"),
        FUIAction(FExecuteAction::CreateRaw(this, &FBlueprintImporterModule::ExecuteLevelImport)));

    Section.AddMenuEntry(
        "ImportNiagaraFromUEFN",
        LOCTEXT("ImportNiagaraLabel", "Niagara UE5 \u2192 UE4\u2026"),
        LOCTEXT("ImportNiagaraTooltip",
            "Paste a Niagara script copied from UEFN / UE5. "
            "Every node (FunctionCall, ParameterMap Get/Set, StaticSwitch, Select, Op, "
            "Convert, Input, UsageSelector, Reroute, Comments) is rebuilt individually "
            "via the UE4 C++ API under /Game/ImportedNiagara/."),
        FSlateIcon(FEditorStyle::GetStyleSetName(), "ClassIcon.NiagaraScript"),
        FUIAction(FExecuteAction::CreateRaw(this, &FBlueprintImporterModule::ExecuteNiagaraImport)));
}

void FBlueprintImporterModule::ExecuteImport()
{
    IDesktopPlatform *DesktopPlatform = FDesktopPlatformModule::Get();
    if (!DesktopPlatform)
        return;

    void *ParentWindow = nullptr;
    IMainFrameModule &MainFrame = FModuleManager::LoadModuleChecked<IMainFrameModule>("MainFrame");
    TSharedPtr<SWindow> TopWindow = MainFrame.GetParentWindow();
    if (TopWindow.IsValid())
        ParentWindow = TopWindow->GetNativeWindow()->GetOSWindowHandle();

    TArray<FString> SelectedFiles;
    if (!DesktopPlatform->OpenFileDialog(ParentWindow,
                                         TEXT("Select Blueprint JSON Export"), FPaths::ProjectContentDir(), TEXT(""),
                                         TEXT("JSON Files (*.json)|*.json|All Files (*.*)|*.*"),
                                         EFileDialogFlags::None, SelectedFiles) ||
        SelectedFiles.Num() == 0)
        return;

    const FString JsonFilePath = SelectedFiles[0];

    FString JsonString;
    if (!FFileHelper::LoadFileToString(JsonString, *JsonFilePath))
    {
        FMessageDialog::Open(EAppMsgType::Ok,
                             FText::Format(LOCTEXT("ReadFail", "Failed to read:\n{0}"), FText::FromString(JsonFilePath)));
        return;
    }

    FBlueprintJsonData ParsedData;
    FString ParseError;
    if (!FBlueprintJsonParser::Parse(JsonString, ParsedData, ParseError))
    {
        FMessageDialog::Open(EAppMsgType::Ok,
                             FText::Format(LOCTEXT("ParseFail", "JSON parse error:\n{0}"), FText::FromString(ParseError)));
        return;
    }

    const FString BlueprintName = ParsedData.BlueprintName.IsEmpty()
                                      ? FPaths::GetBaseFilename(JsonFilePath)
                                      : ParsedData.BlueprintName;

    const FString PackagePath = UPackageTools::SanitizePackageName(
        FString::Printf(TEXT("/Game/ImportedBlueprints/%s"), *BlueprintName));

    UPackage *Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("PackageFail", "Failed to create package."));
        return;
    }
    Package->FullyLoad();

    UClass *ParentClass = AActor::StaticClass();
    if (!ParsedData.ParentClass.IsEmpty())
    {
        FString CleanName = ParsedData.ParentClass;
        int32 QuoteIdx;
        if (CleanName.FindChar('\'', QuoteIdx))
            CleanName = CleanName.Mid(QuoteIdx + 1);
        CleanName.RemoveFromEnd(TEXT("'"));
        int32 DotIdx;
        if (CleanName.FindLastChar('.', DotIdx))
            CleanName = CleanName.Mid(DotIdx + 1);

        bool bResolved = false;

        if (UClass *Found = FindObject<UClass>(ANY_PACKAGE, *CleanName))
        {
            ParentClass = Found;
            bResolved = true;
        }

        if (!bResolved && ParsedData.ParentClassPath.StartsWith(TEXT("/Script/")))
        {
            FString FullPath = ParsedData.ParentClassPath + TEXT(".") + CleanName;
            if (UClass *Loaded = StaticLoadClass(UObject::StaticClass(), nullptr, *FullPath))
            {
                ParentClass = Loaded;
                bResolved = true;
            }
        }

        if (!bResolved && ParsedData.ParentClassPath.StartsWith(TEXT("/Game/")))
        {
            FString PkgPath = ParsedData.ParentClassPath;
            int32 PkgDot;
            if (PkgPath.FindLastChar('.', PkgDot))
                PkgPath = PkgPath.Left(PkgDot);

            UBlueprint *ParentBP = LoadObject<UBlueprint>(nullptr, *PkgPath);
            if (!ParentBP)
            {
                FString AssetName = FPaths::GetBaseFilename(PkgPath);
                ParentBP = LoadObject<UBlueprint>(nullptr, *(PkgPath + TEXT(".") + AssetName));
            }
            if (ParentBP && ParentBP->GeneratedClass)
            {
                ParentClass = ParentBP->GeneratedClass;
                bResolved = true;
            }
        }

        if (!bResolved)
        {
            UE_LOG(LogTemp, Warning,
                   TEXT("BlueprintImporter: could not resolve parent class '%s' (path '%s'); using AActor."),
                   *CleanName, *ParsedData.ParentClassPath);
        }
    }

    UBlueprint *Blueprint = FindObject<UBlueprint>(Package, *BlueprintName);
    if (!Blueprint)
    {
        if (ParsedData.bIsAnimBlueprint)
        {
            if (!ParentClass || !ParentClass->IsChildOf(UAnimInstance::StaticClass()))
                ParentClass = UAnimInstance::StaticClass();

            Blueprint = FKismetEditorUtilities::CreateBlueprint(
                ParentClass, Package, *BlueprintName, BPTYPE_Normal,
                UAnimBlueprint::StaticClass(), UAnimBlueprintGeneratedClass::StaticClass(), NAME_None);
        }
        else
        {
            Blueprint = FKismetEditorUtilities::CreateBlueprint(
                ParentClass, Package, *BlueprintName, BPTYPE_Normal,
                UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), NAME_None);
        }
    }
    if (!Blueprint)
    {
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("BPCreateFail", "Failed to create Blueprint."));
        return;
    }

    FString BuildError;
    if (!FBlueprintGraphBuilder::Build(Blueprint, ParsedData, BuildError))
    {
        FMessageDialog::Open(EAppMsgType::Ok,
                             FText::Format(LOCTEXT("BuildFail", "Graph build error:\n{0}"), FText::FromString(BuildError)));
        return;
    }

    FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    // FIX 4: Prevent compiling if unresolved classes exist to avoid editor crash.
    if (FBlueprintGraphBuilder::LastUnresolvedClasses > 0)
    {
        FMessageDialog::Open(EAppMsgType::Ok,
            FText::Format(LOCTEXT("BuildFailUnresolved", "Graph built with {0} unresolved classes. Compilation skipped to prevent an editor crash. Please fix missing classes and compile manually."), FText::AsNumber(FBlueprintGraphBuilder::LastUnresolvedClasses)));
    }
    else
    {
        FKismetEditorUtilities::CompileBlueprint(Blueprint);
    }

    Package->MarkPackageDirty();
    FAssetRegistryModule::AssetCreated(Blueprint);

    const FString PackageFilename = FPackageName::LongPackageNameToFilename(
        PackagePath, FPackageName::GetAssetPackageExtension());
    UPackage::SavePackage(Package, Blueprint, RF_Public | RF_Standalone,
                          *PackageFilename, GError, nullptr, false, true, SAVE_NoError);

    if (GEditor)
    {
        if (UAssetEditorSubsystem *AES = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
            AES->OpenEditorForAsset(Blueprint);
    }

    FFormatNamedArguments Args;
    Args.Add(TEXT("Name"), FText::FromString(BlueprintName));
    Args.Add(TEXT("Nodes"), FText::AsNumber(FBlueprintGraphBuilder::LastNodesCreated));
    Args.Add(TEXT("Exec"), FText::AsNumber(FBlueprintGraphBuilder::LastExecConnections));
    Args.Add(TEXT("Data"), FText::AsNumber(FBlueprintGraphBuilder::LastDataConnections));
    ShowNotification(FText::Format(
                         LOCTEXT("ImportSuccess", "Blueprint '{Name}' imported! {Nodes} nodes | {Exec} exec | {Data} data wires"),
                         Args),
                     true);
}

void FBlueprintImporterModule::ExecuteWidgetImport()
{
    IDesktopPlatform *DesktopPlatform = FDesktopPlatformModule::Get();
    if (!DesktopPlatform)
        return;

    void *ParentWindow = nullptr;
    IMainFrameModule &MainFrame = FModuleManager::LoadModuleChecked<IMainFrameModule>("MainFrame");
    TSharedPtr<SWindow> TopWindow = MainFrame.GetParentWindow();
    if (TopWindow.IsValid())
        ParentWindow = TopWindow->GetNativeWindow()->GetOSWindowHandle();

    TArray<FString> SelectedFiles;
    if (!DesktopPlatform->OpenFileDialog(ParentWindow,
                                         TEXT("Select Widget Blueprint JSON Export"), FPaths::ProjectContentDir(), TEXT(""),
                                         TEXT("JSON Files (*.json)|*.json|All Files (*.*)|*.*"),
                                         EFileDialogFlags::None, SelectedFiles) ||
        SelectedFiles.Num() == 0)
        return;

    const FString JsonFilePath = SelectedFiles[0];

    FString JsonString;
    if (!FFileHelper::LoadFileToString(JsonString, *JsonFilePath))
    {
        FMessageDialog::Open(EAppMsgType::Ok,
                             FText::Format(LOCTEXT("WBPReadFail", "Failed to read:\n{0}"), FText::FromString(JsonFilePath)));
        return;
    }

    FBlueprintJsonData ParsedData;
    FString ParseError;
    if (!FBlueprintJsonParser::Parse(JsonString, ParsedData, ParseError))
    {
        FMessageDialog::Open(EAppMsgType::Ok,
                             FText::Format(LOCTEXT("WBPParseFail", "JSON parse error:\n{0}"), FText::FromString(ParseError)));
        return;
    }

    if (!ParsedData.bIsWidgetBlueprint)
    {
        FMessageDialog::Open(EAppMsgType::Ok,
                             LOCTEXT("WBPNotWidget",
                                     "The selected JSON does not appear to be a Widget Blueprint export.\n"
                                     "No WidgetBlueprintGeneratedClass entry was found.\n"
                                     "Use 'Blueprint Importer' for regular Blueprints."));
        return;
    }

    const FString BlueprintName = ParsedData.BlueprintName.IsEmpty()
                                      ? FPaths::GetBaseFilename(JsonFilePath)
                                      : ParsedData.BlueprintName;

    const FString PackagePath = UPackageTools::SanitizePackageName(
        FString::Printf(TEXT("/Game/ImportedBlueprints/%s"), *BlueprintName));

    UPackage *Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("WBPPackageFail", "Failed to create package."));
        return;
    }
    Package->FullyLoad();

    UClass *ParentClass = UUserWidget::StaticClass();
    if (!ParsedData.ParentClass.IsEmpty())
    {
        FString CleanName = ParsedData.ParentClass;
        int32 QuoteIdx;
        if (CleanName.FindChar('\'', QuoteIdx))
            CleanName = CleanName.Mid(QuoteIdx + 1);
        CleanName.RemoveFromEnd(TEXT("'"));
        int32 DotIdx;
        if (CleanName.FindLastChar('.', DotIdx))
            CleanName = CleanName.Mid(DotIdx + 1);

        bool bResolved = false;

        if (UClass *Found = FindObject<UClass>(ANY_PACKAGE, *CleanName))
        {
            if (Found->IsChildOf(UUserWidget::StaticClass()))
            {
                ParentClass = Found;
                bResolved = true;
            }
        }

        if (!bResolved && ParsedData.ParentClassPath.StartsWith(TEXT("/Script/")))
        {
            FString FullPath = ParsedData.ParentClassPath + TEXT(".") + CleanName;
            if (UClass *Loaded = StaticLoadClass(UUserWidget::StaticClass(), nullptr, *FullPath))
            {
                ParentClass = Loaded;
                bResolved = true;
            }
        }

        if (!bResolved && ParsedData.ParentClassPath.StartsWith(TEXT("/Game/")))
        {
            FString PkgPath = ParsedData.ParentClassPath;
            int32 PkgDot;
            if (PkgPath.FindLastChar('.', PkgDot))
                PkgPath = PkgPath.Left(PkgDot);

            UWidgetBlueprint *ParentWBP = LoadObject<UWidgetBlueprint>(nullptr, *PkgPath);
            if (!ParentWBP)
            {
                FString AssetName = FPaths::GetBaseFilename(PkgPath);
                ParentWBP = LoadObject<UWidgetBlueprint>(nullptr, *(PkgPath + TEXT(".") + AssetName));
            }
            if (ParentWBP && ParentWBP->GeneratedClass &&
                ParentWBP->GeneratedClass->IsChildOf(UUserWidget::StaticClass()))
            {
                ParentClass = ParentWBP->GeneratedClass;
                bResolved = true;
            }
        }
    }

    UWidgetBlueprint *WidgetBlueprint = FindObject<UWidgetBlueprint>(Package, *BlueprintName);
    if (!WidgetBlueprint)
    {
        WidgetBlueprint = Cast<UWidgetBlueprint>(
            FKismetEditorUtilities::CreateBlueprint(
                ParentClass, Package, *BlueprintName, BPTYPE_Normal,
                UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass(),
                NAME_None));
    }
    if (!WidgetBlueprint)
    {
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("WBPCreateFail", "Failed to create Widget Blueprint."));
        return;
    }

    FString BuildError;
    if (!FBlueprintGraphBuilder::Build(WidgetBlueprint, ParsedData, BuildError))
    {
        FMessageDialog::Open(EAppMsgType::Ok,
                             FText::Format(LOCTEXT("WBPBuildFail", "Graph build error:\n{0}"), FText::FromString(BuildError)));
        return;
    }

    FBlueprintEditorUtils::RefreshAllNodes(WidgetBlueprint);
    FBlueprintEditorUtils::MarkBlueprintAsModified(WidgetBlueprint);

    // FIX 4: Prevent compiling if unresolved classes exist to avoid editor crash.
    if (FBlueprintGraphBuilder::LastUnresolvedClasses > 0)
    {
        FMessageDialog::Open(EAppMsgType::Ok,
            FText::Format(LOCTEXT("WBPBuildFailUnresolved", "Widget Graph built with {0} unresolved classes. Compilation skipped to prevent an editor crash. Please fix missing classes and compile manually."), FText::AsNumber(FBlueprintGraphBuilder::LastUnresolvedClasses)));
    }
    else
    {
        FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
    }

    Package->MarkPackageDirty();
    FAssetRegistryModule::AssetCreated(WidgetBlueprint);

    const FString PackageFilename = FPackageName::LongPackageNameToFilename(
        PackagePath, FPackageName::GetAssetPackageExtension());
    UPackage::SavePackage(Package, WidgetBlueprint, RF_Public | RF_Standalone,
                          *PackageFilename, GError, nullptr, false, true, SAVE_NoError);

    if (GEditor)
    {
        if (UAssetEditorSubsystem *AES = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
            AES->OpenEditorForAsset(WidgetBlueprint);
    }

    FFormatNamedArguments Args;
    Args.Add(TEXT("Name"), FText::FromString(BlueprintName));
    Args.Add(TEXT("Nodes"), FText::AsNumber(FBlueprintGraphBuilder::LastNodesCreated));
    Args.Add(TEXT("Exec"), FText::AsNumber(FBlueprintGraphBuilder::LastExecConnections));
    Args.Add(TEXT("Data"), FText::AsNumber(FBlueprintGraphBuilder::LastDataConnections));
    ShowNotification(FText::Format(
                         LOCTEXT("WBPImportSuccess", "Widget Blueprint '{Name}' imported! {Nodes} nodes | {Exec} exec | {Data} data wires"),
                         Args),
                     true);
}

void FBlueprintImporterModule::ExecuteAnimBPTextImport()
{
    TSharedRef<SWindow> DialogWindow = SNew(SWindow)
        .Title(LOCTEXT("AnimBPDialogTitle", "AnimBP Graph — Paste Decompiled Text"))
        .ClientSize(FVector2D(760.f, 560.f))
        .SupportsMinimize(false)
        .SupportsMaximize(false)
        .SizingRule(ESizingRule::UserSized);

    TSharedRef<SAnimBPPasteDialog> PasteDialog =
        SNew(SAnimBPPasteDialog).ParentWindow(DialogWindow);

    DialogWindow->SetContent(PasteDialog);

    IMainFrameModule& MainFrame =
        FModuleManager::LoadModuleChecked<IMainFrameModule>("MainFrame");
    TSharedPtr<SWindow> TopWindow = MainFrame.GetParentWindow();

    FSlateApplication::Get().AddModalWindow(DialogWindow, TopWindow);

    if (!PasteDialog->WasConfirmed())
        return;

    const FString PastedText   = PasteDialog->GetPastedText();
    const FString JsonFilePath = PasteDialog->GetJsonFilePath();

    if (PastedText.TrimStartAndEnd().IsEmpty())
        return;

    FAnimBPTextData ParsedData;
    FString ParseError;
    if (!FAnimBPTextParser::Parse(PastedText, ParsedData, ParseError))
    {
        FMessageDialog::Open(EAppMsgType::Ok,
            FText::Format(LOCTEXT("AnimBPParseFail", "AnimBP parse error:\n{0}"),
                FText::FromString(ParseError)));
        return;
    }

    bool bHasJson = false;
    FBlueprintJsonData JsonData;
    if (!JsonFilePath.IsEmpty())
    {
        FString JsonString;
        if (!FFileHelper::LoadFileToString(JsonString, *JsonFilePath))
        {
            FMessageDialog::Open(EAppMsgType::Ok,
                FText::Format(LOCTEXT("AnimBPJsonReadFail", "Could not read JSON file:\n{0}\n\nImporting with text data only."),
                    FText::FromString(JsonFilePath)));
        }
        else
        {
            FString JsonError;
            if (!FBlueprintJsonParser::Parse(JsonString, JsonData, JsonError))
            {
                FMessageDialog::Open(EAppMsgType::Ok,
                    FText::Format(LOCTEXT("AnimBPJsonParseFail", "JSON parse error:\n{0}\n\nImporting with text data only."),
                        FText::FromString(JsonError)));
            }
            else
            {
                bHasJson = true;
            }
        }
    }

    FString BlueprintName = ParsedData.ClassName;
    BlueprintName.RemoveFromEnd(TEXT("_C"));
    if (BlueprintName.IsEmpty())
        BlueprintName = TEXT("ImportedAnimBP");

    UClass* ParentClass = UAnimInstance::StaticClass();
    if (!ParsedData.ParentClassName.IsEmpty())
    {
        FString CleanName = ParsedData.ParentClassName;
        if (UClass* Found = FindObject<UClass>(ANY_PACKAGE, *CleanName))
        {
            if (Found->IsChildOf(UAnimInstance::StaticClass()))
                ParentClass = Found;
        }
    }

    const FString PackagePath = UPackageTools::SanitizePackageName(
        FString::Printf(TEXT("/Game/ImportedBlueprints/%s"), *BlueprintName));

    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        FMessageDialog::Open(EAppMsgType::Ok,
            LOCTEXT("AnimBPPackageFail", "Failed to create package for AnimBlueprint."));
        return;
    }
    Package->FullyLoad();

    UAnimBlueprint* AnimBP = FindObject<UAnimBlueprint>(Package, *BlueprintName);
    if (!AnimBP)
    {
        AnimBP = Cast<UAnimBlueprint>(
            FKismetEditorUtilities::CreateBlueprint(
                ParentClass, Package, *BlueprintName, BPTYPE_Normal,
                UAnimBlueprint::StaticClass(),
                UAnimBlueprintGeneratedClass::StaticClass(),
                NAME_None));
    }

    if (!AnimBP)
    {
        FMessageDialog::Open(EAppMsgType::Ok,
            LOCTEXT("AnimBPCreateFail", "Failed to create AnimBlueprint asset."));
        return;
    }

    FString BuildError;
    bool bBuildOk = bHasJson
        ? FAnimBPGraphBuilder::BuildWithJson(AnimBP, ParsedData, JsonData, BuildError)
        : FAnimBPGraphBuilder::Build(AnimBP, ParsedData, BuildError);

    if (!bBuildOk)
    {
        FMessageDialog::Open(EAppMsgType::Ok,
            FText::Format(LOCTEXT("AnimBPBuildFail", "AnimBP graph build error:\n{0}"),
                FText::FromString(BuildError)));
        return;
    }

    FKismetEditorUtilities::CompileBlueprint(AnimBP);

    Package->MarkPackageDirty();
    FAssetRegistryModule::AssetCreated(AnimBP);

    const FString PackageFilename = FPackageName::LongPackageNameToFilename(
        PackagePath, FPackageName::GetAssetPackageExtension());
    UPackage::SavePackage(Package, AnimBP, RF_Public | RF_Standalone,
        *PackageFilename, GError, nullptr, false, true, SAVE_NoError);

    if (GEditor)
    {
        if (UAssetEditorSubsystem* AES = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
            AES->OpenEditorForAsset(AnimBP);
    }

    FFormatNamedArguments Args;
    Args.Add(TEXT("Name"),        FText::FromString(BlueprintName));
    Args.Add(TEXT("Machines"),    FText::AsNumber(FAnimBPGraphBuilder::LastStateMachinesCreated));
    Args.Add(TEXT("States"),      FText::AsNumber(FAnimBPGraphBuilder::LastStatesCreated));
    Args.Add(TEXT("Transitions"), FText::AsNumber(FAnimBPGraphBuilder::LastTransitionsCreated));
    Args.Add(TEXT("Layers"),      FText::AsNumber(FAnimBPGraphBuilder::LastLayerFunctionsCreated));
    Args.Add(TEXT("EventNodes"),  FText::AsNumber(FAnimBPGraphBuilder::LastEventGraphNodesCreated));
    ShowNotification(
        FText::Format(
            LOCTEXT("AnimBPImportSuccess",
                "'{Name}' imported!  "
                "{Machines} state machine(s) | {States} states | "
                "{Transitions} transitions | {Layers} layer stubs | "
                "{EventNodes} event graph node(s)"),
            Args),
        true);
}

#include "LevelJsonParser.h"
#include "LevelBuilder.h"
#include "SLevelImportDialog.h"

void FBlueprintImporterModule::ExecuteLevelImport()
{
    TSharedPtr<SWindow> DialogWindow = SNew(SWindow)
        .Title(LOCTEXT("LevelImportWindowTitle", "Import Level from JSON"))
        .ClientSize(FVector2D(540.f, 220.f))
        .SupportsMinimize(false)
        .SupportsMaximize(false);

    TSharedPtr<SLevelImportDialog> Dialog;
    DialogWindow->SetContent(
        SAssignNew(Dialog, SLevelImportDialog)
        .ParentWindow(DialogWindow));

    IMainFrameModule& MainFrame =
        FModuleManager::LoadModuleChecked<IMainFrameModule>("MainFrame");
    if (TSharedPtr<SWindow> RootWindow = MainFrame.GetParentWindow())
        FSlateApplication::Get().AddModalWindow(DialogWindow.ToSharedRef(), RootWindow);
    else
        FSlateApplication::Get().AddWindow(DialogWindow.ToSharedRef());

    if (!Dialog->WasConfirmed()) return;

    const FString JsonPath = Dialog->GetJsonFilePath();
    if (JsonPath.IsEmpty()) return;

    FString JsonString;
    if (!FFileHelper::LoadFileToString(JsonString, *JsonPath))
    {
        FMessageDialog::Open(EAppMsgType::Ok,
            FText::Format(
                LOCTEXT("LevelJsonReadFail", "Could not read file:\n{0}"),
                FText::FromString(JsonPath)));
        return;
    }

    FLevelJsonData LevelData;
    FString ParseError;
    if (!FLevelJsonParser::Parse(JsonString, LevelData, ParseError))
    {
        FMessageDialog::Open(EAppMsgType::Ok,
            FText::Format(
                LOCTEXT("LevelJsonParseFail", "Level JSON parse error:\n{0}"),
                FText::FromString(ParseError)));
        return;
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        FMessageDialog::Open(EAppMsgType::Ok,
            LOCTEXT("LevelBuildNoWorld", "No editor world found. Open a level first."));
        return;
    }
    ULevel* Level = World->GetCurrentLevel();
    if (!Level)
    {
        FMessageDialog::Open(EAppMsgType::Ok,
            LOCTEXT("LevelBuildNoLevel", "No current level found."));
        return;
    }

    FLevelBuilder::BuildLevel(World, Level, LevelData);

    FFormatNamedArguments Args;
    Args.Add(TEXT("Level"),   FText::FromString(LevelData.LevelName));
    Args.Add(TEXT("Spawned"), FText::AsNumber(FLevelBuilder::LastActorsSpawned));
    Args.Add(TEXT("Skipped"), FText::AsNumber(FLevelBuilder::LastActorsSkipped));
    ShowNotification(
        FText::Format(
            LOCTEXT("LevelImportSuccess",
                "'{Level}' imported!  "
                "{Spawned} actor(s) spawned | {Skipped} skipped (missing class)"),
            Args),
        true);
}


// ──────────────────────────────────────────────────────────────────────────────
//  Niagara UE5 → UE4  (menu entry + implementation)
// ──────────────────────────────────────────────────────────────────────────────
#include "SNiagaraConvertDialog.h"
#include "NiagaraConverter.h"
#include "NiagaraImporter.h"
#include "NiagaraScript.h"
#include "Subsystems/AssetEditorSubsystem.h"

void FBlueprintImporterModule::ExecuteNiagaraImport()
{
    // ── Open dialog ─────────────────────────────────────────────
    TSharedPtr<SWindow> DialogWindow = SNew(SWindow)
        .Title(LOCTEXT("NiagaraWindowTitle", "Niagara UE5 \u2192 UE4 Importer"))
        .ClientSize(FVector2D(760.f, 580.f))
        .SupportsMinimize(false)
        .SupportsMaximize(false);

    TSharedPtr<SNiagaraConvertDialog> Dialog;
    DialogWindow->SetContent(
        SAssignNew(Dialog, SNiagaraConvertDialog)
        .ParentWindow(DialogWindow));

    IMainFrameModule& MainFrame =
        FModuleManager::LoadModuleChecked<IMainFrameModule>("MainFrame");
    if (TSharedPtr<SWindow> Root = MainFrame.GetParentWindow())
        FSlateApplication::Get().AddModalWindow(DialogWindow.ToSharedRef(), Root);
    else
        FSlateApplication::Get().AddWindow(DialogWindow.ToSharedRef());

    if (!Dialog->WasConfirmed()) return;

    const FString InputText = Dialog->GetPastedText();
    if (InputText.TrimStartAndEnd().IsEmpty()) return;

    // ── Convert text (UE5 → UE4) ───────────────────────────────
    FNiagaraConvertResult Converted = FNiagaraConverter::Convert(InputText);
    if (!Converted.bSuccess)
    {
        FMessageDialog::Open(EAppMsgType::Ok,
            FText::Format(
                LOCTEXT("NiagaraConvertFail", "Niagara conversion error:\n{0}"),
                FText::FromString(Converted.ErrorMessage)));
        return;
    }

    // Warn about unknown types
    if (Converted.Warnings.Num() > 0)
    {
        FString WarnMsg = FString::Printf(
            TEXT("%d type warning(s):\n"), Converted.Warnings.Num());
        for (const FString& W : Converted.Warnings)
            WarnMsg += TEXT("  \u2022 ") + W + TEXT("\n");
        WarnMsg += TEXT("\nImport will continue — unknown types are passed through unchanged.");
        FMessageDialog::Open(EAppMsgType::Ok,
            FText::FromString(WarnMsg));
    }

    // ── Choose a script name ────────────────────────────────────
    // Default to "ImportedNiagaraModule"; user can rename after import
    const FString ScriptName = TEXT("ImportedNiagaraModule");

    // ── Build the graph ─────────────────────────────────────────
    FString BuildError;
    UNiagaraScript* Script = FNiagaraImporter::Import(Converted, ScriptName, BuildError);

    if (!Script)
    {
        FMessageDialog::Open(EAppMsgType::Ok,
            FText::Format(
                LOCTEXT("NiagaraBuildFail", "Niagara import error:\n{0}"),
                FText::FromString(BuildError)));
        return;
    }

    // ── Sync content browser to the new asset (safe) ────────────
    // NOTE: We intentionally do NOT call OpenEditorForAsset here.
    // Opening a freshly-created UNiagaraScript immediately causes
    // GetBaseChangeID() to dereference an uninitialized internal pointer
    // inside the Niagara toolkit. The user can double-click the asset in
    // the Content Browser once it appears there.
    if (GEditor)
    {
        TArray<UObject*> ToSync;
        ToSync.Add(Script);
        GEditor->SyncBrowserToObjects(ToSync);
    }

    // ── Success notification ────────────────────────────────────
    FFormatNamedArguments Args;
    Args.Add(TEXT("Name"),     FText::FromString(ScriptName));
    Args.Add(TEXT("Nodes"),    FText::AsNumber(FNiagaraImporter::LastNodesCreated));
    Args.Add(TEXT("Comments"), FText::AsNumber(FNiagaraImporter::LastCommentsCreated));
    Args.Add(TEXT("Links"),    FText::AsNumber(FNiagaraImporter::LastLinksWired));
    Args.Add(TEXT("Vars"),     FText::AsNumber(Converted.ScriptVars.Num()));
    ShowNotification(
        FText::Format(
            LOCTEXT("NiagaraImportSuccess",
                "'{Name}' imported!  "
                "{Nodes} node(s) | {Comments} comment(s) | "
                "{Links} link(s) | {Vars} script variable(s)"),
            Args),
        true);
}

#undef LOCTEXT_NAMESPACE
