#include "SLevelImportDialog.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SBoxPanel.h"
#include "Framework/Application/SlateApplication.h"
#include "EditorStyleSet.h"
#include "IDesktopPlatform.h"
#include "DesktopPlatformModule.h"
#include "Interfaces/IMainFrameModule.h"

#define LOCTEXT_NAMESPACE "SLevelImportDialog"

void SLevelImportDialog::Construct(const FArguments& InArgs)
{
	ParentWindow = InArgs._ParentWindow;

	TSharedRef<SEditableTextBox> PathBox =
		SNew(SEditableTextBox)
		.IsReadOnly(true)
		.HintText(LOCTEXT("PathHint", "Select a UAssetAPI / FModel level JSON export..."))
		.Text_Lambda([this]() { return FText::FromString(JsonFilePath); });

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FEditorStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(FMargin(12.f))
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("Title", "Import Level from JSON"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 13))
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text(LOCTEXT("Desc",
					"Spawns all actors from a UAssetAPI or FModel level JSON export into the "
					"currently open level.\n\n"
					"Actors whose Blueprint class is not present in this project are silently "
					"skipped. Mesh / particle / material references that don't exist locally "
					"are also skipped — the actor is still spawned at the correct position."))
			]

			// File path row
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
				[
					SNew(STextBlock).Text(LOCTEXT("FileLabel", "JSON File:"))
				]

				+ SHorizontalBox::Slot().FillWidth(1.f).Padding(0, 0, 4, 0)
				[
					PathBox
				]

				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("Browse", "Browse..."))
					.OnClicked(this, &SLevelImportDialog::OnBrowseClicked)
				]
			]

			// Buttons
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.f)
				+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0, 0, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("Import", "Import Level"))
					.ButtonStyle(FEditorStyle::Get(), "FlatButton.Success")
					.ForegroundColor(FLinearColor::White)
					.IsEnabled_Lambda([this]() { return !JsonFilePath.IsEmpty(); })
					.OnClicked(this, &SLevelImportDialog::OnImportClicked)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0, 0, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("Cancel", "Cancel"))
					.OnClicked(this, &SLevelImportDialog::OnCancelClicked)
				]
			]
		]
	];
}

FReply SLevelImportDialog::OnBrowseClicked()
{
	IDesktopPlatform* DP = FDesktopPlatformModule::Get();
	if (!DP) return FReply::Handled();

	void* NativeWindow = nullptr;
	IMainFrameModule& MF = FModuleManager::LoadModuleChecked<IMainFrameModule>("MainFrame");
	if (TSharedPtr<SWindow> Top = MF.GetParentWindow())
		NativeWindow = Top->GetNativeWindow()->GetOSWindowHandle();

	TArray<FString> Files;
	DP->OpenFileDialog(NativeWindow,
		TEXT("Select Level JSON Export"),
		FPaths::ProjectContentDir(),
		TEXT(""),
		TEXT("JSON Files (*.json)|*.json|All Files (*.*)|*.*"),
		EFileDialogFlags::None,
		Files);

	if (Files.Num() > 0)
		JsonFilePath = Files[0];

	return FReply::Handled();
}

FReply SLevelImportDialog::OnImportClicked()
{
	if (JsonFilePath.IsEmpty()) return FReply::Handled();
	bConfirmed = true;
	if (ParentWindow.IsValid()) ParentWindow->RequestDestroyWindow();
	return FReply::Handled();
}

FReply SLevelImportDialog::OnCancelClicked()
{
	bConfirmed = false;
	if (ParentWindow.IsValid()) ParentWindow->RequestDestroyWindow();
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
