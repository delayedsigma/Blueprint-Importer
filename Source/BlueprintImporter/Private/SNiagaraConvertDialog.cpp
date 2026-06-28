#include "SNiagaraConvertDialog.h"

#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SBoxPanel.h"
#include "EditorStyleSet.h"

#define LOCTEXT_NAMESPACE "SNiagaraConvertDialog"

void SNiagaraConvertDialog::Construct(const FArguments& InArgs)
{
    ParentWindow = InArgs._ParentWindow;

    TSharedRef<SMultiLineEditableTextBox> TextBox =
        SNew(SMultiLineEditableTextBox)
        .Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
        .HintText(LOCTEXT("PasteHint",
            "Paste your UEFN / UE5 Niagara script here.\n\n"
            "Accepts:\n"
            "  - Full NiagaraClipboardContent wrapper (from UEFN copy)\n"
            "  - Raw Begin Object / End Object node text\n\n"
            "A new UNiagaraScript asset will be created under\n"
            "/Game/ImportedNiagara/ with every node rebuilt from scratch."))
        .OnTextChanged_Lambda([this](const FText& NewText)
        {
            PastedText = NewText.ToString();
        })
        .AutoWrapText(false);

    ChildSlot
    [
        SNew(SBorder)
        .BorderImage(FEditorStyle::GetBrush("ToolPanel.GroupBorder"))
        .Padding(FMargin(10.f))
        [
            SNew(SVerticalBox)

            // Title
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.f, 0.f, 0.f, 4.f)
            [
                SNew(STextBlock)
                .Text(LOCTEXT("Title", "Niagara UE5 → UE4 Importer"))
                .Font(FCoreStyle::GetDefaultFontStyle("Bold", 13))
            ]

            // Subtitle
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.f, 0.f, 0.f, 10.f)
            [
                SNew(STextBlock)
                .Text(LOCTEXT("Subtitle",
                    "Paste a Niagara script copied from UEFN / UE5. All node types "
                    "(FunctionCall, ParameterMapGet/Set, StaticSwitch, Select, Op, "
                    "Convert, Input, UsageSelector, Reroute, Comments) are rebuilt "
                    "individually using the UE4 C++ API — no clipboard paste involved."))
                .AutoWrapText(true)
                .ColorAndOpacity(FSlateColor::UseSubduedForeground())
            ]

            // Text area
            + SVerticalBox::Slot()
            .FillHeight(1.f)
            .Padding(0.f, 0.f, 0.f, 10.f)
            [
                SNew(SBox)
                .MinDesiredWidth(720.f)
                .MinDesiredHeight(440.f)
                [
                    SNew(SScrollBox)
                    + SScrollBox::Slot()
                    [
                        TextBox
                    ]
                ]
            ]

            // Buttons
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
                    .Text(LOCTEXT("ConvertBtn", "Convert & Import"))
                    .ButtonStyle(FEditorStyle::Get(), "FlatButton.Success")
                    .ForegroundColor(FLinearColor::White)
                    .OnClicked(this, &SNiagaraConvertDialog::OnConvertClicked)
                ]

                + SHorizontalBox::Slot()
                .AutoWidth()
                .Padding(4.f, 0.f, 0.f, 0.f)
                [
                    SNew(SButton)
                    .Text(LOCTEXT("CancelBtn", "Cancel"))
                    .OnClicked(this, &SNiagaraConvertDialog::OnCancelClicked)
                ]
            ]
        ]
    ];
}

FReply SNiagaraConvertDialog::OnConvertClicked()
{
    if (PastedText.TrimStartAndEnd().IsEmpty())
        return FReply::Handled();

    bConfirmed = true;
    if (ParentWindow.IsValid())
        ParentWindow->RequestDestroyWindow();
    return FReply::Handled();
}

FReply SNiagaraConvertDialog::OnCancelClicked()
{
    bConfirmed = false;
    if (ParentWindow.IsValid())
        ParentWindow->RequestDestroyWindow();
    return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
