#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

/**
 * SNiagaraConvertDialog
 *
 * Modal dialog for the Niagara UEFN → UE4.26 converter.
 * The user pastes their full UEFN Niagara script (or NiagaraClipboardContent
 * wrapper) into the text area and clicks Convert.
 */
class SNiagaraConvertDialog : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SNiagaraConvertDialog) {}
        SLATE_ARGUMENT(TSharedPtr<SWindow>, ParentWindow)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

    bool    WasConfirmed() const { return bConfirmed; }
    FString GetPastedText() const { return PastedText; }

private:
    FReply OnConvertClicked();
    FReply OnCancelClicked();

    TSharedPtr<SWindow> ParentWindow;
    FString             PastedText;
    bool                bConfirmed = false;
};
