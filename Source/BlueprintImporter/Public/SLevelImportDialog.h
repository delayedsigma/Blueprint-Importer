#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

/**
 * SLevelImportDialog
 *
 * File-picker dialog for importing a level JSON export into the current editor level.
 * Accepts a UAssetAPI/FModel JSON file, spawns all actors whose classes exist locally,
 * and silently skips any actor whose class or asset references can't be resolved.
 */
class SLevelImportDialog : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SLevelImportDialog) {}
		SLATE_ARGUMENT(TSharedPtr<SWindow>, ParentWindow)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	bool    WasConfirmed()   const { return bConfirmed; }
	FString GetJsonFilePath() const { return JsonFilePath; }

private:
	FReply OnImportClicked();
	FReply OnCancelClicked();
	FReply OnBrowseClicked();

	TSharedPtr<SWindow> ParentWindow;
	FString             JsonFilePath;
	bool                bConfirmed = false;
};
