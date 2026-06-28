#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

/**
 * SAnimBPPasteDialog
 *
 * Modal dialog for AnimBP import. Accepts:
 *   - A pasted decompiled text dump (CUE4Parse / FModel format)   [required]
 *   - An optional UAssetAPI JSON export for the same asset        [optional]
 *
 * When a JSON path is provided the builder will use its precise variable
 * type info (structs, enums, object refs, property flags) to supplement
 * what the text parser could infer.
 */
class SAnimBPPasteDialog : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SAnimBPPasteDialog) {}
		SLATE_ARGUMENT(TSharedPtr<SWindow>, ParentWindow)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	bool    WasConfirmed()    const { return bConfirmed; }
	FString GetPastedText()   const { return PastedText; }
	FString GetJsonFilePath() const { return JsonFilePath; }

private:
	FReply OnImportClicked();
	FReply OnCancelClicked();
	FReply OnBrowseJsonClicked();

	TSharedPtr<SWindow> ParentWindow;
	FString             PastedText;
	FString             JsonFilePath;
	bool                bConfirmed = false;
};
