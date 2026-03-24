// In TextureProjectionWindow.h

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Input/STextComboBox.h"
#include "Widgets/Input/SVectorInputBox.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Engine/StaticMeshActor.h"
#include "PropertyCustomizationHelpers.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/SceneCapture2D.h"
#include "Settings/EditorLoadingSavingSettings.h"
#include "Widgets/Input/SRotatorInputBox.h"
#include "Helpers/TextureProjectionTypes.h" 
#include "Helpers/UnrealComfyUITypes.h"

using FNodeInfoArray = TArray<FUnrealNodeInfo>;
using FControlValueMap = TMap<FString, TSharedPtr<FJsonValue>>;
using FSeedStateMap = TMap<FString, bool>;

// Forward declare delegate types
DECLARE_DELEGATE_OneParam(FOnTabSelected, int32);
DECLARE_DELEGATE_OneParam(FOnTabRemoved, int32);
DECLARE_DELEGATE(FOnTabAdded);
DECLARE_DELEGATE(FOnSingleProjectionConfirmed);
DECLARE_DELEGATE(FOnExportDepthImage);
DECLARE_DELEGATE(FOnExportMaskImage);
DECLARE_DELEGATE(FOnExportCurrentView);
DECLARE_DELEGATE(FOnExportWeightMap);
DECLARE_DELEGATE(FOnSaveFinalTexture);
DECLARE_DELEGATE_OneParam(FOnUVChannelChanged, int32); 
DECLARE_DELEGATE_RetVal_OneParam(UTexture2D*, FOnGetPreviewTexture, FString);
DECLARE_DELEGATE_FiveParams(FOnDynamicProjectionConfirmed, const FString&, const FNodeInfoArray&, const FControlValueMap&, const FSeedStateMap&, const FProjectionSettings&);
DECLARE_DELEGATE_OneParam(FOnMaterialSlotChanged, int32);
DECLARE_DELEGATE_OneParam(FOnOccluderStateChanged, bool);
DECLARE_DELEGATE_OneParam(FOnBlendParametersChangedDelegate, const TArray<FProjectionSettings>&);
DECLARE_DELEGATE_RetVal_OneParam(FLinearColor, FOnGetBaseColorForSlot, int32 /*SlotIndex*/);
DECLARE_DELEGATE_TwoParams(FOnBaseColorChanged, int32 /*SlotIndex*/, FLinearColor /*NewColor*/);
DECLARE_DELEGATE_TwoParams(FOnActiveVariantChanged, int32 /*SlotIndex*/, bool /*bLit*/);
DECLARE_DELEGATE_RetVal(bool, FOnGetActiveVariantState); 
DECLARE_DELEGATE_TwoParams(FOnOutputResolutionChanged, int32 /*W*/, int32 /*H*/);



/**
 * Settings window for texture projection
 */
class STextureProjectionWindow : public SCompoundWidget
{
public: 
      SLATE_BEGIN_ARGS(STextureProjectionWindow) {}
            SLATE_ARGUMENT(TObjectPtr<AStaticMeshActor>, TargetActor)
            SLATE_ARGUMENT(TSharedPtr<SWindow>, ParentWindow)
            SLATE_EVENT(FSimpleDelegate, OnProjectionConfirmed)
            SLATE_EVENT(FOnSaveFinalTexture, OnSaveFinalTexture) 
			SLATE_EVENT(FSimpleDelegate, OnBlendSettingsCommitted) 
			SLATE_EVENT(FOnBlendParametersChangedDelegate, OnBlendParametersChanged)
			SLATE_EVENT(FOnDynamicProjectionConfirmed, OnDynamicProjectionConfirmed)
			SLATE_EVENT(FOnGetPreviewTexture, OnGetPreviewTexture)
			SLATE_EVENT(FOnOccluderStateChanged, OnOccluderStateChanged)
			SLATE_EVENT(FSimpleDelegate, OnGenerateRigClicked)
			SLATE_EVENT(FOnGetBaseColorForSlot, OnGetBaseColorForSlot)
			SLATE_EVENT(FOnBaseColorChanged, OnBaseColorChanged)
			SLATE_EVENT(FOnActiveVariantChanged, OnActiveVariantChanged)
			SLATE_EVENT(FOnGetActiveVariantState, OnGetActiveVariantState)
      SLATE_END_ARGS()

      void Construct(const FArguments& InArgs);
      ~STextureProjectionWindow();


		FProjectionSettings GetProjectionSettings() const { return Settings; }
		void SetCameraSettings(const TArray<FProjectionSettings>& InCameraSettings, int32 InActiveIndex, const FString& InOutputPath, int32 InOutputWidth, int32 InOutputHeight);
		const TArray<FProjectionSettings>& GetAllSettings();

		void UpdateUVChannels(int32 NumUVChannels, int32 CurrentChannel);


		void SetCaptureActor(TObjectPtr<ASceneCapture2D> InCaptureActor);
		
		void SetOnSettingsChanged(TFunction<void(const FProjectionSettings&)> InCallback) { OnSettingsChangedCallback = InCallback; }
		void SetOnTabSelected(FOnTabSelected InCallback) { OnTabSelectedCallback = InCallback; }
		void SetOnTabRemoved(FOnTabRemoved InCallback) { OnTabRemovedCallback = InCallback; }
		void SetOnTabAdded(FOnTabAdded InCallback) { OnTabAddedCallback = InCallback; }
		void SetOnSingleProjectionConfirmed(FOnSingleProjectionConfirmed InOnSingleProjectionConfirmed);
		void SetOnExportDepthImage(FOnExportDepthImage InOnExportDepthImage);
		void SetOnExportMaskImage(FOnExportMaskImage InOnExportMaskImage);
		void SetOnExportCurrentView(FOnExportCurrentView InOnExportCurrentView);
		void SetOnExportWeightMap(FOnExportWeightMap InOnExportWeightMap);
		void SetOnUVChannelChanged(FOnUVChannelChanged InCallback) { OnUVChannelChangedCallback = InCallback; } 
		void SetGlobalSettingsLock(bool bIsLocked);
		

		const TArray<FUnrealNodeInfo>& GetParsedNodeData() const { return ParsedNodeData; }
		const TMap<FString, TSharedPtr<FJsonValue>>& GetControlValues() const { return DynamicControlValues; }
		const TMap<FString, bool>& GetSeedStates() const { return SeedRandomizationStates; }

		 void SetOnOutputResolutionChanged(FOnOutputResolutionChanged InDelegate);

		FSimpleDelegate OnReset;
		FOnMaterialSlotChanged OnMaterialSlotChanged;
		void UpdateOccluderCheckbox(bool bIsChecked);
		void UpdateCameraPreview();

		int32 GetOutputWidth()  const { return GlobalUI_OutputWidth; }
	int32 GetOutputHeight() const { return GlobalUI_OutputHeight; }
	const FString& GetOutputPath() const { return GlobalUI_OutputPath; }

private:
	// --- UI Action Handlers ---
	FReply OnConfirmClicked();
	FReply OnCancelClicked();
	FReply OnLookAtMeshClicked();
	FReply OnAddTabClicked();
	FReply OnRemoveTabClicked();
	FReply OnSingleProjectionClicked();
	FReply OnExportDepthClicked();
	FReply OnExportMaskClicked();
	FReply OnExportCurrentViewClicked();
	FReply OnExportWeightMapClicked();
	FReply OnSaveFinalTextureClicked(); 
	FReply OnResetClicked();

	// --- Settings Widget Event Handlers ---
	void OnCameraPositionChanged(FVector NewValue);
	void OnCameraRotationChanged(FRotator NewValue);
	void OnFOVAngleChanged(float NewValue);
	void OnFadeStartAngleChanged(float NewValue);
	void OnTextureChanged(const FAssetData& AssetData);
	FString GetTextureObjectPath() const;
	void OnOutputPathChanged(const FText& NewValue);
	void OnOutputWidthChanged(int32 NewValue);
	void OnOutputHeightChanged(int32 NewValue);
	void OnUseComfyUIChanged(ECheckBoxState NewState);
	void OnEdgeFalloffChanged(float NewValue);
	void OnWeightChanged(float NewValue); 
	void OnUVChannelChanged(TSharedPtr<FString> NewSelection, ESelectInfo::Type SelectInfo);
	void OnFadeStartAngleCommitted(float NewValue, ETextCommit::Type CommitType);
    void OnEdgeFalloffCommitted(float NewValue, ETextCommit::Type CommitType);
	void OnWeightCommitted(float NewValue, ETextCommit::Type CommitType); 

	
	// --- Internal Logic & Management ---
	void OnWindowClosed(const TSharedRef<SWindow>& Window);
	TSharedRef<SWidget> CreateTabWidget(int32 TabIndex);
	void OnTabButtonClicked(int32 TabIndex);
	void RefreshTabButtons();
	void UpdateTabButtonStyles();
	void UpdateUIFromSettings();
	void SelectTab(int32 TabIndex);
	void NotifySettingsChanged();
	
	// --- Data Members ---
	TWeakObjectPtr<AStaticMeshActor> TargetActor;
    TObjectPtr<ASceneCapture2D> CaptureActor = nullptr;

	TWeakPtr<SWindow> ParentWindowPtr;
	FSimpleDelegate OnProjectionConfirmed;
	FSimpleDelegate OnExportDepthImage;
    FSimpleDelegate OnExportCurrentView;
	FSimpleDelegate OnGenerateRigClicked;
	FOnBlendParametersChangedDelegate OnBlendParametersChanged;

	FOnGetBaseColorForSlot OnGetBaseColorForSlot;
	FOnBaseColorChanged    OnBaseColorChanged;
	
	// --- Callbacks & Delegates ---
	TFunction<void(const FProjectionSettings&)> OnSettingsChangedCallback;
	FOnTabSelected OnTabSelectedCallback;
	FOnTabRemoved OnTabRemovedCallback;
	FOnTabAdded OnTabAddedCallback;
	FOnSingleProjectionConfirmed OnSingleProjectionConfirmed;
	FOnExportMaskImage OnExportMaskImage;
	FOnExportWeightMap OnExportWeightMap;
	FOnSaveFinalTexture OnSaveFinalTexture; 
	FOnUVChannelChanged OnUVChannelChangedCallback;
	FOnOutputResolutionChanged OnOutputResolutionChangedCallback;
	FOnDynamicProjectionConfirmed OnDynamicProjectionConfirmed;
	FSimpleDelegate OnBlendSettingsCommitted; 
	FOnActiveVariantChanged OnActiveVariantChanged;
	FOnGetActiveVariantState OnGetActiveVariantState; 

	// --- State and Data Pointers ---
	TArray<FProjectionSettings> CameraSettings;
	int32 ActiveTabIndex;
	FProjectionSettings Settings;

	FString GlobalUI_OutputPath;
	int32 GlobalUI_OutputWidth;
	int32 GlobalUI_OutputHeight;
	
	TSharedPtr<SCheckBox> VariantCheckbox;

	// --- UI Widget Pointers ---
	TSharedPtr<SHorizontalBox> TabsBox;
	TArray<TSharedPtr<SButton>> TabButtons;
	TSharedPtr<SVectorInputBox> CameraPositionInput;
	TSharedPtr<SRotatorInputBox> CameraRotationInput;
	TSharedPtr<SSpinBox<float>> FOVAngleInput;
	TSharedPtr<SObjectPropertyEntryBox> TextureInput;
	TSharedPtr<SEditableTextBox> OutputPathInput;
	TSharedPtr<SSpinBox<int32>> OutputWidthInput;
	TSharedPtr<SSpinBox<int32>> OutputHeightInput;
	TSharedPtr<STextComboBox> UVChannelComboBox;
	TArray<TSharedPtr<FString>> UVChannelOptions;
	
	// --- Internal State ---
    TObjectPtr<UTextureRenderTarget2D> PreviewRenderTarget;
    TObjectPtr<UTexture2D> OpaquePreviewTexture;
	TSharedPtr<FSlateBrush> PreviewBrush;
	bool bSuppressPreviewUpdates = false;
	bool bOriginalAutosaveEnabled;
	bool bHasDisabledAutosave;
	bool bAreGlobalSettingsLocked;
	bool bIsRefreshingUI = false;


    TSharedPtr<SVerticalBox> DynamicControlsBox;
    TMap<FString, TSharedPtr<FJsonValue>> DynamicControlValues; 
    void OnWorkflowPathPicked(const FString& NewPath);
    void RebuildDynamicControls();
	FReply OnBrowseButtonClicked();
    TArray<FUnrealNodeInfo> ParsedNodeData;
	TMap<FString, bool> SeedRandomizationStates;

	/** Handles clicks on the lightbox overlay to dismiss it. */
    FReply OnLightboxClicked(const FGeometry&, const FPointerEvent&);

    /** Shows the lightbox with the specified texture. */
    void ShowLightboxForTexture(UTexture2D* TextureToShow);

	// The image widget inside the lightbox overlay.
    TSharedPtr<SImage> LightboxImage;
    // The container for the lightbox
    TSharedPtr<SBox> LightboxContainer;
    
    // The delegate instance
	FOnGetPreviewTexture OnGetPreviewTexture;

	// For Material Slot selection
	TSharedPtr<STextComboBox> MaterialSlotComboBox;
    TArray<TSharedPtr<FString>> MaterialSlotOptions;
    void HandleMaterialSlotSelectionChanged(TSharedPtr<FString> NewSelection, ESelectInfo::Type SelectInfo);
    void UpdateMaterialSlots();


	// New handler for when the checkbox state changes internally
    void OnCheckboxStateChanged(ECheckBoxState NewState);
    
    // Delegate instance
    FOnOccluderStateChanged OnOccluderStateChanged;

    // Pointer to the checkbox widget
    TSharedPtr<SCheckBox> OccluderCheckbox;

	bool bHasBeenCleanedUp;

	
};