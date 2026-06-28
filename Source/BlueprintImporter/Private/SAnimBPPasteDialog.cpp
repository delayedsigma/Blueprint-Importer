#include "SAnimBPPasteDialog.h"

#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SBoxPanel.h"
#include "Framework/Application/SlateApplication.h"
#include "EditorStyleSet.h"
#include "IDesktopPlatform.h"
#include "DesktopPlatformModule.h"
#include "Interfaces/IMainFrameModule.h"

#define LOCTEXT_NAMESPACE "SAnimBPPasteDialog"

void SAnimBPPasteDialog::Construct(const FArguments& InArgs)
{
	ParentWindow = InArgs._ParentWindow;

	TSharedRef<SMultiLineEditableTextBox> TextBox =
		SNew(SMultiLineEditableTextBox)
		.Font(FCoreStyle::GetDefaultFontStyle("Mono", 10))
		.HintText(LOCTEXT("PasteHint",
			"Paste your decompiled AnimBP here...\n\n"
			"Expected format:\n"
			"  class UMyAnimBP_C : public UAnimInstance\n"
			"  {\n"
			"  public:\n"
			"      TArray<struct FBakedAnimationStateMachine> BakedStateMachines = { ... };\n"
			"      ...\n"
			"  };"))
		.OnTextChanged_Lambda([this](const FText& NewText)
		{
			PastedText = NewText.ToString();
		})
		.AutoWrapText(false);

	// JSON path display box (read-only, updated by the Browse button)
	TSharedRef<SEditableTextBox> JsonPathBox =
		SNew(SEditableTextBox)
		.IsReadOnly(true)
		.HintText(LOCTEXT("JsonHint", "(optional) Path to UAssetAPI JSON export..."))
		.Text_Lambda([this]() { return FText::FromString(JsonFilePath); });

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FEditorStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(FMargin(8.f))
		[
			SNew(SVerticalBox)

			// ---- Header ----
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.f, 0.f, 0.f, 4.f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("DialogTitle", "Import Decompiled AnimBP"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
			]

			// ---- Sub-title ----
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.f, 0.f, 0.f, 8.f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("DialogSubtitle",
					"Paste the full class dump from CUE4Parse / FModel.\n"
					"Optionally provide a UAssetAPI JSON export for the same asset — "
					"it gives the importer precise variable types, struct names, and property flags."))
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]

			// ---- Text area ----
			+ SVerticalBox::Slot()
			.FillHeight(1.f)
			.Padding(0.f, 0.f, 0.f, 8.f)
			[
				SNew(SBox)
				.MinDesiredWidth(700.f)
				.MinDesiredHeight(400.f)
				[
					SNew(SScrollBox)
					+ SScrollBox::Slot()
					[
						TextBox
					]
				]
			]

			// ---- JSON row ----
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.f, 0.f, 0.f, 8.f)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.f, 0.f, 6.f, 0.f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("JsonLabel", "JSON (optional):"))
				]

				+ SHorizontalBox::Slot()
				.FillWidth(1.f)
				.Padding(0.f, 0.f, 4.f, 0.f)
				[
					JsonPathBox
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("BrowseBtn", "Browse..."))
					.OnClicked(this, &SAnimBPPasteDialog::OnBrowseJsonClicked)
				]
			]

			// ---- Buttons ----
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.FillWidth(1.f)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(4.f, 0.f, 0.f, 0.f)
				[
					SNew(SButton)
					.Text(LOCTEXT("ImportBtn", "Import AnimBP"))
					.ButtonStyle(FEditorStyle::Get(), "FlatButton.Success")
					.ForegroundColor(FLinearColor::White)
					.OnClicked(this, &SAnimBPPasteDialog::OnImportClicked)
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(4.f, 0.f, 0.f, 0.f)
				[
					SNew(SButton)
					.Text(LOCTEXT("CancelBtn", "Cancel"))
					.OnClicked(this, &SAnimBPPasteDialog::OnCancelClicked)
				]
			]
		]
	];
}

FReply SAnimBPPasteDialog::OnImportClicked()
{
	if (PastedText.TrimStartAndEnd().IsEmpty())
		return FReply::Handled();

	bConfirmed = true;
	if (ParentWindow.IsValid())
		ParentWindow->RequestDestroyWindow();
	return FReply::Handled();
}

FReply SAnimBPPasteDialog::OnCancelClicked()
{
	bConfirmed = false;
	if (ParentWindow.IsValid())
		ParentWindow->RequestDestroyWindow();
	return FReply::Handled();
}

FReply SAnimBPPasteDialog::OnBrowseJsonClicked()
{
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (!DesktopPlatform)
		return FReply::Handled();

	void* NativeWindow = nullptr;
	IMainFrameModule& MainFrame = FModuleManager::LoadModuleChecked<IMainFrameModule>("MainFrame");
	TSharedPtr<SWindow> TopWindow = MainFrame.GetParentWindow();
	if (TopWindow.IsValid())
		NativeWindow = TopWindow->GetNativeWindow()->GetOSWindowHandle();

	TArray<FString> SelectedFiles;
	DesktopPlatform->OpenFileDialog(
		NativeWindow,
		TEXT("Select UAssetAPI JSON Export"),
		FPaths::ProjectContentDir(),
		TEXT(""),
		TEXT("JSON Files (*.json)|*.json|All Files (*.*)|*.*"),
		EFileDialogFlags::None,
		SelectedFiles);

	if (SelectedFiles.Num() > 0)
		JsonFilePath = SelectedFiles[0];

	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
