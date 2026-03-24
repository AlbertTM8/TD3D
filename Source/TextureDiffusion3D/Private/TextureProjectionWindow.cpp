#include "TextureProjectionWindow.h"
#include "SlateOptMacros.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SFilePathPicker.h" 
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SButton.h"
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "TextureDiffusion3D.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Modules/ModuleManager.h"
#include "Widgets/Colors/SColorBlock.h"
#include "Widgets/Colors/SColorPicker.h"
#include "Framework/Application/SlateApplication.h" 
// #include "Misc/GuardValue.h"

BEGIN_SLATE_FUNCTION_BUILD_OPTIMIZATION


void STextureProjectionWindow::Construct(const FArguments& InArgs)
{
    Settings = FProjectionSettings();
    TargetActor = InArgs._TargetActor;
    ParentWindowPtr = InArgs._ParentWindow;
    OnProjectionConfirmed = InArgs._OnProjectionConfirmed;
    OnDynamicProjectionConfirmed = InArgs._OnDynamicProjectionConfirmed;
    OnSaveFinalTexture = InArgs._OnSaveFinalTexture;
    OnBlendSettingsCommitted = InArgs._OnBlendSettingsCommitted;
    OnBlendParametersChanged = InArgs._OnBlendParametersChanged;
    OnGetPreviewTexture = InArgs._OnGetPreviewTexture;
    OnOccluderStateChanged = InArgs._OnOccluderStateChanged;
    OnGenerateRigClicked = InArgs._OnGenerateRigClicked;
    OnGetBaseColorForSlot = InArgs._OnGetBaseColorForSlot;
    OnBaseColorChanged    = InArgs._OnBaseColorChanged;
    OnActiveVariantChanged = InArgs._OnActiveVariantChanged; 
    OnGetActiveVariantState = InArgs._OnGetActiveVariantState;
    ActiveTabIndex = 0;
    bHasBeenCleanedUp = false;
    TWeakPtr<STextureProjectionWindow> WeakSelf = StaticCastSharedRef<STextureProjectionWindow>(this->AsShared()).ToWeakPtr();

    // Autosave Management
    bHasDisabledAutosave = false;
    UEditorLoadingSavingSettings* LoadingSavingSettings = GetMutableDefault<UEditorLoadingSavingSettings>();
    if (LoadingSavingSettings)
    {
        bOriginalAutosaveEnabled = LoadingSavingSettings->bAutoSaveEnable;
        if (bOriginalAutosaveEnabled)
        {
            LoadingSavingSettings->bAutoSaveEnable = false;
            LoadingSavingSettings->SaveConfig();
            bHasDisabledAutosave = true;
            UE_LOG(LogTemp, Log, TEXT("Temporarily disabled editor autosave."));
        }
    }

    if (ParentWindowPtr.IsValid())
    {
        ParentWindowPtr.Pin()->SetOnWindowClosed(FOnWindowClosed::CreateRaw(this, &STextureProjectionWindow::OnWindowClosed));
    }

    // Render Target & Brush for Preview
    PreviewRenderTarget = NewObject<UTextureRenderTarget2D>();
    PreviewRenderTarget->AddToRoot();
    PreviewRenderTarget->RenderTargetFormat = RTF_RGBA8;
    PreviewRenderTarget->ClearColor = FLinearColor(0.0f, 0.0f, 0.0f, 1.0f);
    PreviewRenderTarget->InitAutoFormat(512, 512);
    PreviewRenderTarget->UpdateResourceImmediate(true);
    PreviewBrush = MakeShareable(new FSlateImageBrush(PreviewRenderTarget, FVector2D(256, 256)));

    // UI Construction
    ChildSlot
    [
        SNew(SOverlay)
        
        // Slot 0: Main UI content
        + SOverlay::Slot()
        [
            SNew(SBorder)
            .BorderImage(FCoreStyle::Get().GetBrush("ToolPanel.GroupBorder"))
            .Padding(FMargin(8.0f))
            [
                SNew(SVerticalBox)

                // Title
                + SVerticalBox::Slot().AutoHeight().Padding(FMargin(0, 0, 0, 10))
                [
                    SNew(STextBlock)
                    .Text(FText::FromString("Texture Projection Settings"))
                    .Font(FCoreStyle::Get().GetFontStyle("EmbossedText"))
                    .Justification(ETextJustify::Center)
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 15)
                [
                    SNew(SBorder)
                    .BorderImage(FCoreStyle::Get().GetBrush("ToolPanel.GroupBorder"))
                    .Padding(FMargin(8.0f))
                    [
                        SNew(SVerticalBox)

                        // Section Title
                        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 5)
                        [
                            SNew(STextBlock)
                            .Text(FText::FromString("Global Settings"))
                            .Font(FCoreStyle::Get().GetFontStyle("EmbossedText"))
                        ]

                        + SVerticalBox::Slot().Padding(FMargin(0, 5))
                        [
                            SNew(SHorizontalBox)
                            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 10, 0)
                            [ SNew(SBox).WidthOverride(150) [ SNew(STextBlock).Text(FText::FromString("Output Path:")) ] ]

                            + SHorizontalBox::Slot().FillWidth(1.0f)
                            [
                                SAssignNew(OutputPathInput, SEditableTextBox)
                                .Text_Lambda([this] { return FText::FromString(GlobalUI_OutputPath); })
                                .OnTextChanged(this, &STextureProjectionWindow::OnOutputPathChanged)
                                .IsEnabled_Lambda([this] { return !bAreGlobalSettingsLocked; })
                            ]
                        ]
                        + SVerticalBox::Slot().Padding(FMargin(0, 5))
                        [
                            SNew(SHorizontalBox)
                            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 10, 0)
                            [ SNew(SBox).WidthOverride(150) [ SNew(STextBlock).Text(FText::FromString("Output Resolution:")) ] ]

                            + SHorizontalBox::Slot().FillWidth(1.0f)
                            [
                                SAssignNew(OutputWidthInput, SSpinBox<int32>)
                                .Value_Lambda([this] { return GlobalUI_OutputWidth; })
                                .MinValue(256).MaxValue(8192).Delta(256)
                                .OnValueChanged(this, &STextureProjectionWindow::OnOutputWidthChanged)
                                .IsEnabled_Lambda([this] { return !bAreGlobalSettingsLocked; })
                            ]
                            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(5)
                            [ SNew(STextBlock).Text(FText::FromString("x")) ]

                            + SHorizontalBox::Slot().FillWidth(1.0f)
                            [
                                SAssignNew(OutputHeightInput, SSpinBox<int32>)
                                .Value_Lambda([this] { return GlobalUI_OutputHeight; })
                                .MinValue(256).MaxValue(8192).Delta(256)
                                .OnValueChanged(this, &STextureProjectionWindow::OnOutputHeightChanged)
                                .IsEnabled_Lambda([this] { return !bAreGlobalSettingsLocked; })
                            ]
                        ]
                        + SVerticalBox::Slot().Padding(FMargin(0, 5))
                        [
                            SNew(SHorizontalBox)
                            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 10, 0)
                            [ SNew(SBox).WidthOverride(150) [ SNew(STextBlock).Text(FText::FromString("Target UV Channel:")) ] ]
                            + SHorizontalBox::Slot().FillWidth(1.0f)
                            [
                                SAssignNew(UVChannelComboBox, STextComboBox)
                                .OptionsSource(&UVChannelOptions)
                                .OnSelectionChanged(this, &STextureProjectionWindow::OnUVChannelChanged)
                                .IsEnabled_Lambda([this] { return !bAreGlobalSettingsLocked; })
                            ]
                        ]
                         + SVerticalBox::Slot().Padding(FMargin(0, 5))
                        [
                            SNew(SHorizontalBox)
                            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 10, 0)
                            [
                                SNew(SBox).WidthOverride(150)
                                [
                                    SNew(STextBlock).Text(FText::FromString("Target Material Slot:"))
                                ]
                            ]
                            + SHorizontalBox::Slot().FillWidth(1.0f)
                            [
                                SAssignNew(MaterialSlotComboBox, STextComboBox)
                                .OptionsSource(&MaterialSlotOptions)
                                .OnSelectionChanged(this, &STextureProjectionWindow::HandleMaterialSlotSelectionChanged)
                                .ToolTipText(FText::FromString("Select the material slot to project onto."))
                            ]
                        ]

                        + SVerticalBox::Slot()
.AutoHeight()
.Padding(0, 10, 0, 0) // Add some top padding to separate it from the dropdown
[
    SNew(SExpandableArea)
    .AreaTitle(FText::FromString("Advanced Slot Settings"))
    .InitiallyCollapsed(true)
    .BodyContent()
    [
        SNew(SVerticalBox)

                        +SVerticalBox::Slot()
                        .AutoHeight()
                        .Padding(5)
                        [
                            SNew(SHorizontalBox)
                            +SHorizontalBox::Slot()
                            .VAlign(VAlign_Center)
                            [
                                SNew(STextBlock).Text(FText::FromString("Hide this slot during projection"))
                            ]
                            +SHorizontalBox::Slot()
                            .AutoWidth()
                            .HAlign(HAlign_Right)
                            [
                            SAssignNew(OccluderCheckbox, SCheckBox) // Use SAssignNew directly
                            .OnCheckStateChanged(this, &STextureProjectionWindow::OnCheckboxStateChanged)
                        ]
                        ]
+ SVerticalBox::Slot()
.AutoHeight()
.Padding(5) // match the occluder row padding
[
    SNew(SHorizontalBox)

    // Left: label (fills), just a tiny nudge to the right like the other row
    + SHorizontalBox::Slot()
    .VAlign(VAlign_Center)
    .Padding(0, 0, 10, 0)   // small left indent + space before swatch
    [
        SNew(STextBlock)
        .Text(FText::FromString("Base Color (Slot):"))
    ]

    // Right: the clickable swatch (auto width, right aligned)
    + SHorizontalBox::Slot()
    .AutoWidth()
    .HAlign(HAlign_Right)
    [
        SNew(SButton)
        .ButtonStyle(&FCoreStyle::Get().GetWidgetStyle<FButtonStyle>("NoBorder"))
        .ContentPadding(0)
        .OnClicked_Lambda([this]() -> FReply
        {
            const int32 Slot = Settings.TargetMaterialSlotIndex;
            const FLinearColor Start = OnGetBaseColorForSlot.IsBound()
                                     ? OnGetBaseColorForSlot.Execute(Slot)
                                     : FLinearColor::White;

            FColorPickerArgs PickerArgs;
            PickerArgs.bUseAlpha = false;
            PickerArgs.InitialColor = Start;  // ✅ new API (no deprecation)
            PickerArgs.ParentWidget = AsShared();
            PickerArgs.OnColorCommitted = FOnLinearColorValueChanged::CreateLambda(
                [this, Slot](FLinearColor NewColor)
                {
                    if (OnBaseColorChanged.IsBound())
                    {
                        OnBaseColorChanged.Execute(Slot, NewColor);
                    }
                });

            OpenColorPicker(PickerArgs);
            return FReply::Handled();
        })
        [
            SNew(SColorBlock)
            .Color_Lambda([this]()
            {
                const int32 Slot = Settings.TargetMaterialSlotIndex;
                return OnGetBaseColorForSlot.IsBound()
                     ? OnGetBaseColorForSlot.Execute(Slot)
                     : FLinearColor::White;
            })
            .Size(FVector2D(24, 16)) // a bit smaller so it visually matches a checkbox
            .ToolTipText(FText::FromString("Per-slot base plate color"))
        ]
    ]
]

+ SVerticalBox::Slot()
.AutoHeight()
.Padding(5)
[
    SNew(SHorizontalBox)

    + SHorizontalBox::Slot()
    .VAlign(VAlign_Center)
    [
        SNew(STextBlock)
        .Text(FText::FromString("Baked Lighting:"))
    ]

    + SHorizontalBox::Slot()
    .AutoWidth()
    .HAlign(HAlign_Right)
    .Padding(12,0,0,0)
    [
        SAssignNew(VariantCheckbox, SCheckBox)
        // "Ask" the logic what the state should be
        .IsChecked_Lambda([this]() -> ECheckBoxState {
            if (OnGetActiveVariantState.IsBound())
            {
                return OnGetActiveVariantState.Execute() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
            }
            return ECheckBoxState::Unchecked;
        })
        // "Tell" the logic when the state has changed
        .OnCheckStateChanged_Lambda([this](ECheckBoxState NewState)
        {
            const bool bLit = (NewState == ECheckBoxState::Checked);
            if (OnActiveVariantChanged.IsBound())
            {
                OnActiveVariantChanged.Execute(Settings.TargetMaterialSlotIndex, bLit);
            }
        })
    ]
]


  ]
]
                    ]
                ]

                
                // Tab header
                + SVerticalBox::Slot().AutoHeight().Padding(FMargin(0, 0, 0, 10))
                [
                    SAssignNew(TabsBox, SHorizontalBox)
                ]

                // Settings content
                + SVerticalBox::Slot().FillHeight(1.0f)
                [
                    SNew(SScrollBox) // The ScrollBox for all our settings

                    // Camera Position
                    + SScrollBox::Slot().Padding(FMargin(0, 5))
                    [
                        SNew(SHorizontalBox)
                        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 10, 0)
                        [ SNew(SBox).WidthOverride(150) [ SNew(STextBlock).Text(FText::FromString("Camera Position:")) ] ]
                        + SHorizontalBox::Slot().FillWidth(1.0f)
                        [
                            SAssignNew(CameraPositionInput, SVectorInputBox).bColorAxisLabels(true).AllowSpin(true)
                            .X_Lambda([this] { return Settings.CameraPosition.X; })
                            .Y_Lambda([this] { return Settings.CameraPosition.Y; })
                            .Z_Lambda([this] { return Settings.CameraPosition.Z; })
                            .OnXChanged_Lambda([this](float NewValue) { OnCameraPositionChanged(FVector(NewValue, Settings.CameraPosition.Y, Settings.CameraPosition.Z)); })
                            .OnYChanged_Lambda([this](float NewValue) { OnCameraPositionChanged(FVector(Settings.CameraPosition.X, NewValue, Settings.CameraPosition.Z)); })
                            .OnZChanged_Lambda([this](float NewValue) { OnCameraPositionChanged(FVector(Settings.CameraPosition.X, Settings.CameraPosition.Y, NewValue)); })
                        ]
                    ]

                    // Camera Rotation
                    + SScrollBox::Slot().Padding(FMargin(0, 5))
                    [
                        SNew(SHorizontalBox)
                        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 10, 0)
                        [ SNew(SBox).WidthOverride(150) [ SNew(STextBlock).Text(FText::FromString("Camera Rotation:")) ] ]
                        + SHorizontalBox::Slot().FillWidth(1.0f)
                        [
                            SAssignNew(CameraRotationInput, SRotatorInputBox).bColorAxisLabels(true).AllowSpin(true)
                            .Roll_Lambda([this] { return Settings.CameraRotation.Roll; })
                            .Pitch_Lambda([this] { return Settings.CameraRotation.Pitch; })
                            .Yaw_Lambda([this] { return Settings.CameraRotation.Yaw; })
                            .OnRollChanged_Lambda([this](float New) { OnCameraRotationChanged(FRotator(Settings.CameraRotation.Pitch, Settings.CameraRotation.Yaw, New)); })
                            .OnPitchChanged_Lambda([this](float New) { OnCameraRotationChanged(FRotator(New, Settings.CameraRotation.Yaw, Settings.CameraRotation.Roll)); })
                            .OnYawChanged_Lambda([this](float New) { OnCameraRotationChanged(FRotator(Settings.CameraRotation.Pitch, New, Settings.CameraRotation.Roll)); })
                        ]
                        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(10, 0, 0, 0)
                        [
                            SNew(SButton).Text(FText::FromString("Look At Mesh"))
                            .OnClicked(this, &STextureProjectionWindow::OnLookAtMeshClicked)
                        ]
                    ]

                    // FOV Angle
                    + SScrollBox::Slot().Padding(FMargin(0, 5))
                    [
                        SNew(SHorizontalBox)
                        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 10, 0)
                        [ SNew(SBox).WidthOverride(150) [ SNew(STextBlock).Text(FText::FromString("FOV Angle:")) ] ]
                        + SHorizontalBox::Slot().FillWidth(1.0f)
                        [
                            SAssignNew(FOVAngleInput, SSpinBox<float>)
                            .Value_Lambda([this] { return Settings.FOVAngle; })
                            .MinValue(10.0f).MaxValue(170.0f).Delta(1.0f)
                            .OnValueChanged(this, &STextureProjectionWindow::OnFOVAngleChanged)
                        ]
                    ]
                    //Fade Start Angle
                    + SScrollBox::Slot().Padding(FMargin(0, 5))
                    [
                        SNew(SHorizontalBox)
                        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 10, 0)
                        [ SNew(SBox).WidthOverride(150) [ SNew(STextBlock).Text(FText::FromString("Fade Start Angle:")) ] ]
                        + SHorizontalBox::Slot().FillWidth(1.0f)
                        [
                            SNew(SSpinBox<float>)
                            .Value_Lambda([this] { return Settings.FadeStartAngle; })
                            .MinValue(0.0f).MaxValue(90.0f).Delta(1.0f)
                            .OnValueChanged(this, &STextureProjectionWindow::OnFadeStartAngleChanged)
                            .OnValueCommitted(this, &STextureProjectionWindow::OnFadeStartAngleCommitted) // <-- ADD THIS LINE
                            .ToolTipText(FText::FromString("The view angle at which the projection starts to fade out.\n0 = Fades immediately at edges.\n90 = No angle-based fading."))
                        ]
                    ]
                    // Edge Falloff
                    + SScrollBox::Slot().Padding(FMargin(0, 5))
                    [
                        SNew(SHorizontalBox)
                        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 10, 0)
                        [ SNew(SBox).WidthOverride(150) [ SNew(STextBlock).Text(FText::FromString("Edge Falloff:")) ] ]

                        + SHorizontalBox::Slot().FillWidth(1.0f)
                        [
                            SNew(SSpinBox<float>)
                            .Value_Lambda([this] { return Settings.EdgeFalloff; })
                            .MinValue(0.0f).MaxValue(20.0f).Delta(0.1f)
                            .OnValueChanged(this, &STextureProjectionWindow::OnEdgeFalloffChanged)
                            .OnValueCommitted(this, &STextureProjectionWindow::OnEdgeFalloffCommitted) // <-- ADD THIS LINE
                            .ToolTipText(FText::FromString("Controls the blend softness at grazing angles.\n1.0 = Linear falloff.\n> 1.0 = Sharper edge.\n< 1.0 = Softer edge."))
                        ]
                    ]
                    + SScrollBox::Slot().Padding(FMargin(0, 5))
                    [
                        SNew(SHorizontalBox)
                        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 10, 0)
                        [ SNew(SBox).WidthOverride(150) [ SNew(STextBlock).Text(FText::FromString("Projection Weight:")) ] ]

                        + SHorizontalBox::Slot().FillWidth(1.0f)
                        [
                            SNew(SSpinBox<float>)
                            .Value_Lambda([this] { return Settings.Weight; })
                            .MinValue(0.0f).MaxValue(100.0f).Delta(0.1f)
                            .OnValueChanged(this, &STextureProjectionWindow::OnWeightChanged)
                            .OnValueCommitted(this, &STextureProjectionWindow::OnWeightCommitted)
                            .ToolTipText(FText::FromString("Controls the overall influence of this projection.\nHigher values make it stronger in the final blend."))
                        ]
                    ]

                    // Use ComfyUI Checkbox
                    + SScrollBox::Slot().Padding(FMargin(0, 20, 0, 5))
                    [
                        SNew(SHorizontalBox)
                        + SHorizontalBox::Slot()
                        .AutoWidth()
                        .VAlign(VAlign_Center)
                        .Padding(0, 0, 10, 0)
                        [
                            SNew(SCheckBox)
                            .IsChecked_Lambda([this]() { return Settings.bUseComfyUI ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
                            .OnCheckStateChanged(this, &STextureProjectionWindow::OnUseComfyUIChanged)
                            .ToolTipText(FText::FromString("If checked, the projection will be generated by ComfyUI. If unchecked, it will use the Source Texture selected below."))
                        ]
                        + SHorizontalBox::Slot()
                        .AutoWidth()
                        .VAlign(VAlign_Center)
                        [
                            SNew(STextBlock)
                            .Text(FText::FromString("Use ComfyUI"))
                        ]
                    ]

                    // --- START: This is the collapsible section for ComfyUI ---
                    + SScrollBox::Slot().Padding(FMargin(0, 5))
                    [
                        SNew(SExpandableArea)
                        .AreaTitle(FText::FromString("ComfyUI Parameters"))
                        .InitiallyCollapsed(true)
                        .Visibility_Lambda([this]() {
                            return Settings.bUseComfyUI ? EVisibility::Visible : EVisibility::Collapsed;
                        })
                        .BodyContent()
                        [
                            SNew(SVerticalBox)

                            // 1. WORKFLOW SELECTOR
                            + SVerticalBox::Slot().AutoHeight().Padding(FMargin(0, 5, 0, 15))
                            [
                                SNew(SHorizontalBox)
                                
                                + SHorizontalBox::Slot()
                                    .AutoWidth()
                                    .VAlign(VAlign_Center)
                                    .Padding(0, 0, 10, 0)
                                [
                                    SNew(STextBlock).Text(FText::FromString("ComfyUI Workflow:"))
                                ]
                                + SHorizontalBox::Slot().FillWidth(1.0f)
                                [
                                    SNew(SHorizontalBox)
                                    +SHorizontalBox::Slot()
                                    .FillWidth(1.0f)
                                    .VAlign(VAlign_Center)
                                    [
                                        SNew(STextBlock)
                                            .Text_Lambda([this] { return FText::FromString(Settings.WorkflowApiJson); })
                                            .ToolTipText(FText::FromString("Path to the ComfyUI workflow file."))
                                    ]
                                    +SHorizontalBox::Slot()
                                    .AutoWidth()
                                    .Padding(4.0f, 0.0f, 0.0f, 0.0f)
                                    .VAlign(VAlign_Center)
                                    [
                                        SNew(SButton)
                                        .Text(FText::FromString("..."))
                                        .ToolTipText(FText::FromString("Select a ComfyUI workflow (.json) saved in API format."))
                                        .OnClicked(this, &STextureProjectionWindow::OnBrowseButtonClicked)
                                    ]
                                ]
                            ]

                            // 2. DYNAMIC CONTROLS AREA
                            + SVerticalBox::Slot().AutoHeight()
                            [
                                SAssignNew(DynamicControlsBox, SVerticalBox)
                            ]
                        ]
                    ]
                    // --- END: This is the collapsible section for ComfyUI ---

                    // --- START: This is the Source Texture picker, which is NOT in the collapsible section ---
                    + SScrollBox::Slot().Padding(FMargin(0, 0, 0, 5))
                    [
                        SNew(SHorizontalBox)
                        .Visibility_Lambda([this]()
                        {
                            return Settings.bUseComfyUI ? EVisibility::Collapsed : EVisibility::Visible;
                        })
                        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(10, 0, 10, 0)
                        [ SNew(SBox).WidthOverride(140) [ SNew(STextBlock).Text(FText::FromString("Source Texture:")) ] ]

                        + SHorizontalBox::Slot().FillWidth(1.0f)
                        [
                            SAssignNew(TextureInput, SObjectPropertyEntryBox)
                            .AllowedClass(UTexture2D::StaticClass())
                            .ObjectPath_Lambda([WeakSelf]() -> FString {
                                if (WeakSelf.IsValid())
                                {
                                    return WeakSelf.Pin()->GetTextureObjectPath();
                                }
                                return FString();
                            })
                            .OnObjectChanged_Lambda([WeakSelf](const FAssetData& AssetData) {
                                if (WeakSelf.IsValid())
                                {
                                    WeakSelf.Pin()->OnTextureChanged(AssetData);
                                }
                            })
                            .AllowClear(true)
                        ]
                    ]
                    // --- END: This is the Source Texture picker ---


                    // --- START: This is the Preview Widget, also NOT in the collapsible section ---
                    + SScrollBox::Slot()
                    .Padding(FMargin(0, 25, 0, 5))
                    .HAlign(HAlign_Center)
                    [
                        SNew(SVerticalBox)

                        // Preview Label
                        + SVerticalBox::Slot()
                        .AutoHeight()
                        .HAlign(HAlign_Center)
                        .Padding(0, 0, 0, 5)
                        [
                            SNew(STextBlock)
                            .Text(FText::FromString("Camera Preview"))
                            .Font(FCoreStyle::Get().GetFontStyle("EmbossedText"))
                        ]

                        // Preview Image Box
                        + SVerticalBox::Slot()
                        .AutoHeight()
                        [
                            SNew(SBox)
                            .WidthOverride(256)
                            .HeightOverride(256)
                            [
                                SNew(SBorder)
                                .BorderImage(FCoreStyle::Get().GetBrush("ToolPanel.GroupBorder"))
                                .Padding(2.0f)
                                [
                                    SNew(SImage)
                                    .Image(PreviewBrush.Get())
                                ]
                            ]
                        ]
                    ]
                    // --- END: This is the Preview Widget ---

                ] // End of SScrollBox

                // Main Action Buttons
                + SVerticalBox::Slot().AutoHeight().Padding(FMargin(0, 20, 0, 0)).HAlign(HAlign_Right)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().Padding(2)
                    [ SNew(SButton).Text(FText::FromString("Cancel")).OnClicked(this, &STextureProjectionWindow::OnCancelClicked) ]

                    + SHorizontalBox::Slot().AutoWidth().Padding(2)
                    [
                        SNew(SButton).Text(FText::FromString("Reset"))
                        .OnClicked(this, &STextureProjectionWindow::OnResetClicked)
                        .ToolTipText(FText::FromString("Clears all projected data for this actor and unlocks global settings."))
                    ]

                    + SHorizontalBox::Slot().AutoWidth().Padding(2)
                    [
                        SNew(SButton).Text(FText::FromString("Project Current"))
                        .OnClicked(this, &STextureProjectionWindow::OnSingleProjectionClicked)
                    ]
                    + SHorizontalBox::Slot().AutoWidth().Padding(2)
                    [
                        SNew(SButton).Text(FText::FromString("Project All"))
                        .OnClicked(this, &STextureProjectionWindow::OnConfirmClicked)
                        .ButtonStyle(FCoreStyle::Get(), "PrimaryButton")
                    ]
                         + SHorizontalBox::Slot().AutoWidth().Padding(2)
                    [
                        SNew(SButton).Text(FText::FromString("Save Texture"))
                        .OnClicked(this, &STextureProjectionWindow::OnSaveFinalTextureClicked)
                        .ButtonStyle(FCoreStyle::Get(), "PrimaryButton")
                    ]
                ]
            ]
        ]
        // Slot 1: Lightbox Overlay
        + SOverlay::Slot()
        [
            SAssignNew(LightboxContainer, SBox)
            .Visibility(EVisibility::Collapsed) // Start hidden
            .HAlign(HAlign_Fill)
            .VAlign(VAlign_Fill)
            [
                SNew(SBorder)
                .BorderImage(FCoreStyle::Get().GetBrush("Brushes.Black"))
                .BorderBackgroundColor(FLinearColor(0.f, 0.f, 0.f, 0.75f)) // Semi-transparent black
                .HAlign(HAlign_Center)
                .VAlign(VAlign_Center)
                .OnMouseButtonDown(this, &STextureProjectionWindow::OnLightboxClicked) // Click to dismiss
                [
                    SNew(SScaleBox)
                   .Stretch(EStretch::ScaleToFit) // Ensures the image fits without distortion
                   [
                       SAssignNew(LightboxImage, SImage)
                   ]
                ]
            ]
        ]
    ];
}

STextureProjectionWindow::~STextureProjectionWindow()
{
    if (!bHasBeenCleanedUp)
    {
        if (bHasDisabledAutosave)
        {
            if (UEditorLoadingSavingSettings* LoadingSavingSettings = GetMutableDefault<UEditorLoadingSavingSettings>())
            {
                LoadingSavingSettings->bAutoSaveEnable = bOriginalAutosaveEnabled;
                LoadingSavingSettings->SaveConfig();
            }
        }
        
        if (UObjectInitialized())
        {
            if (PreviewRenderTarget) PreviewRenderTarget->RemoveFromRoot();
            if (OpaquePreviewTexture) OpaquePreviewTexture->RemoveFromRoot();
        }
    }
}

FReply STextureProjectionWindow::OnConfirmClicked()
{
    NotifySettingsChanged();
    OnProjectionConfirmed.ExecuteIfBound();
    // if (ParentWindowPtr.IsValid()) { ParentWindowPtr.Pin()->RequestDestroyWindow(); } 
    return FReply::Handled();
}
    
FReply STextureProjectionWindow::OnSingleProjectionClicked()
{
    NotifySettingsChanged();    
    // Gather all the data needed by the backend and execute the delegate
    OnDynamicProjectionConfirmed.ExecuteIfBound(
        Settings.WorkflowApiJson,
        ParsedNodeData,
        DynamicControlValues,
        SeedRandomizationStates,
        Settings // The current per-camera settings
    );

	if (ParentWindowPtr.IsValid()) { ParentWindowPtr.Pin()->RequestDestroyWindow(); }
	return FReply::Handled();
}

FReply STextureProjectionWindow::OnCancelClicked()
{
	if (ParentWindowPtr.IsValid()) { ParentWindowPtr.Pin()->RequestDestroyWindow(); }
	return FReply::Handled();
}

void STextureProjectionWindow::NotifySettingsChanged()
{
    if (OnSettingsChangedCallback)
    {
		Settings.ComfyControlValues = this->DynamicControlValues;
		Settings.ComfySeedStates = this->SeedRandomizationStates;

        OnSettingsChangedCallback(Settings);
		
    }
    
    if (!bSuppressPreviewUpdates)
    {
        UpdateCameraPreview();
    }
}

void STextureProjectionWindow::OnCameraPositionChanged(FVector NewValue){ Settings.CameraPosition = NewValue; NotifySettingsChanged(); }
void STextureProjectionWindow::OnCameraRotationChanged(FRotator NewValue){ Settings.CameraRotation = NewValue; NotifySettingsChanged(); }
void STextureProjectionWindow::OnFOVAngleChanged(float NewValue){ Settings.FOVAngle = NewValue; NotifySettingsChanged(); }
void STextureProjectionWindow::OnFadeStartAngleChanged(float NewValue){ Settings.FadeStartAngle = NewValue; NotifySettingsChanged(); }


FReply STextureProjectionWindow::OnLookAtMeshClicked()
{
	if (TargetActor.IsValid())
	{
		const FBoxSphereBounds Bounds = TargetActor->GetComponentsBoundingBox();
		Settings.CameraRotation = (Bounds.Origin - Settings.CameraPosition).Rotation();
		NotifySettingsChanged();
	}
	return FReply::Handled();
}

void STextureProjectionWindow::SetCameraSettings(const TArray<FProjectionSettings>& InCameraSettings, int32 InActiveIndex, const FString& InOutputPath, int32 InOutputWidth, int32 InOutputHeight)
{

    TGuardValue<bool> RefreshGuard(bIsRefreshingUI, true);

	this->CameraSettings = InCameraSettings;

	if (!CameraSettings.IsValidIndex(InActiveIndex))
	{
		if(TabsBox.IsValid()) TabsBox->ClearChildren();
		return;
	}
	ActiveTabIndex = InActiveIndex;

	// Load the PER-CAMERA settings for the active tab
	Settings = CameraSettings[ActiveTabIndex];

	this->DynamicControlValues = Settings.ComfyControlValues;
    this->SeedRandomizationStates = Settings.ComfySeedStates;

	// Load the GLOBAL settings into our new member variables
	GlobalUI_OutputPath = InOutputPath;
	GlobalUI_OutputWidth = InOutputWidth;
	GlobalUI_OutputHeight = InOutputHeight;

	// Refresh the UI to display all the new values
    UpdateMaterialSlots();
	RefreshTabButtons();
	UpdateUIFromSettings();
	UpdateCameraPreview();
	RebuildDynamicControls();


}

void STextureProjectionWindow::SelectTab(int32 TabIndex)
{
    if (CameraSettings.IsValidIndex(TabIndex) && TabIndex != ActiveTabIndex)
    {
        // 1. SAVE (This is the critical fix)
        // Before doing anything else, we take the current settings from the UI
        // and save them back into the main array at the index of the tab we are leaving.
        if (CameraSettings.IsValidIndex(ActiveTabIndex))
        {
            CameraSettings[ActiveTabIndex] = Settings;
			CameraSettings[ActiveTabIndex].ComfyControlValues = this->DynamicControlValues;
            CameraSettings[ActiveTabIndex].ComfySeedStates = this->SeedRandomizationStates;
        
        }

        // 2. UPDATE and LOAD
        // Now it's safe to update the active index and load the new tab's data.
        ActiveTabIndex = TabIndex;
        Settings = CameraSettings[ActiveTabIndex];

		this->DynamicControlValues = Settings.ComfyControlValues;
        this->SeedRandomizationStates = Settings.ComfySeedStates;

        // 3. REFRESH
        // Finally, update all the UI elements to show the new data.
        UpdateUIFromSettings();
        UpdateTabButtonStyles();
        UpdateCameraPreview();

        // ... notify the main controller
        if (OnTabSelectedCallback.IsBound())
        {
            OnTabSelectedCallback.Execute(TabIndex);
        }
    }
}

void STextureProjectionWindow::OnTabButtonClicked(int32 TabIndex)
{
	SelectTab(TabIndex);
}

FReply STextureProjectionWindow::OnAddTabClicked()
{
	OnTabAddedCallback.ExecuteIfBound();
	return FReply::Handled();
}

FReply STextureProjectionWindow::OnRemoveTabClicked()
{
	OnTabRemovedCallback.ExecuteIfBound(ActiveTabIndex);
	return FReply::Handled();
}


    
void STextureProjectionWindow::RefreshTabButtons()
{
	if (!TabsBox.IsValid()) return;
	TabsBox->ClearChildren();
	TabButtons.Empty(CameraSettings.Num());

	for (int32 i = 0; i < CameraSettings.Num(); ++i)
	{
		TabsBox->AddSlot().AutoWidth().Padding(FMargin(2, 0))
		[
			CreateTabWidget(i)
		];
	}
	
	TabsBox->AddSlot().AutoWidth().Padding(FMargin(4, 0, 0, 0)).VAlign(VAlign_Center)
	[
		SNew(SButton).ButtonStyle(FCoreStyle::Get(), "SimpleButton")
		.OnClicked(this, &STextureProjectionWindow::OnAddTabClicked)
		[ SNew(SImage).Image(FCoreStyle::Get().GetBrush("Icons.Plus")) ]
	];
    
    TabsBox->AddSlot()
	.AutoWidth()
	.Padding(FMargin(8, 0, 0, 0))
	.VAlign(VAlign_Center)
	[
		SNew(SButton)
		.ButtonStyle(FCoreStyle::Get(), "SimpleButton") // Makes it look like the '+' button
		.ToolTipText(FText::FromString("Generates a default 5-camera rig to cover the mesh."))
		.OnClicked_Lambda([this]() -> FReply {
			OnGenerateRigClicked.ExecuteIfBound();
			return FReply::Handled();
		})
		[
			SNew(SImage)
			.Image(FCoreStyle::Get().GetBrush("EditorIcons.Gear")) // Use a gear icon
		]
	];
	
	TabsBox->AddSlot().AutoWidth().Padding(FMargin(2,0,0,0)).VAlign(VAlign_Center)
	[
		SNew(SButton).ButtonStyle(FCoreStyle::Get(), "SimpleButton")
		.OnClicked(this, &STextureProjectionWindow::OnRemoveTabClicked)
		.IsEnabled_Lambda([this] { return CameraSettings.Num() > 1; })
		[ SNew(SImage).Image(FCoreStyle::Get().GetBrush("Icons.Delete")) ]
	];
	UpdateTabButtonStyles();
}

TSharedRef<SWidget> STextureProjectionWindow::CreateTabWidget(int32 TabIndex)
{
    TSharedPtr<SButton> TabButton;
    SAssignNew(TabButton, SButton)
        .OnClicked_Lambda([this, TabIndex]() -> FReply {
            OnTabButtonClicked(TabIndex);
            return FReply::Handled();
        })
        .ContentPadding(FMargin(4, 2))
        [
            SNew(SHorizontalBox)

            // Tab Name Text
            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            .Padding(4, 0)
            [
                SNew(STextBlock).Text(FText::FromString(CameraSettings[TabIndex].TabName))
            ]

        ];

    TabButtons.Add(TabButton);
    return TabButton.ToSharedRef();
}

void STextureProjectionWindow::UpdateTabButtonStyles()
{
	for (int32 i = 0; i < TabButtons.Num(); ++i)
	{
		if (TabButtons[i].IsValid())
		{
			const FName StyleName = (i == ActiveTabIndex) ? "PrimaryButton" : "Button";
			TabButtons[i]->SetButtonStyle(&FCoreStyle::Get().GetWidgetStyle<FButtonStyle>(StyleName));
		}
	}
}

void STextureProjectionWindow::UpdateUIFromSettings()
{
	// With Lambda-bound widgets, we just need to tell Slate to redraw.
	Invalidate(EInvalidateWidget::LayoutAndVolatility);
}

// In STextureProjectionWindow.cpp

void STextureProjectionWindow::UpdateCameraPreview()
{
	if (!TargetActor.IsValid() || !CaptureActor || !PreviewRenderTarget || !PreviewBrush.IsValid()) return;
	USceneCaptureComponent2D* CaptureComp = CaptureActor->GetCaptureComponent2D();
	if (!CaptureComp) return;

	// --- 1. Configure and Perform the BaseColor Capture ---
	UTextureRenderTarget2D* OriginalRT = CaptureComp->TextureTarget;
	const ESceneCaptureSource OriginalCaptureSource = CaptureComp->CaptureSource;

	CaptureComp->TextureTarget = PreviewRenderTarget;
	CaptureComp->CaptureSource = ESceneCaptureSource::SCS_SceneColorHDR;
    CaptureComp->ShowFlags.SetLighting(false);
    CaptureComp->ShowFlags.SetPostProcessing(false);
    CaptureComp->ShowFlags.SetTonemapper(false);
    CaptureComp->ShowFlags.SetBloom(false);
    CaptureComp->ShowFlags.SetMotionBlur(false);
	CaptureComp->FOVAngle = Settings.FOVAngle;
	CaptureActor->SetActorLocationAndRotation(Settings.CameraPosition, Settings.CameraRotation);
	CaptureComp->CaptureScene();

	// Restore original settings
	CaptureComp->TextureTarget = OriginalRT;
	CaptureComp->CaptureSource = OriginalCaptureSource;

	// --- 2. Read the Pixels ---
	FTextureRenderTargetResource* RTResource = PreviewRenderTarget->GameThread_GetRenderTargetResource();
	if (!RTResource) return;

	TArray<FColor> RawPixels;
	// Use the correct size from the render target
	RawPixels.SetNum(PreviewRenderTarget->SizeX * PreviewRenderTarget->SizeY); 
	
	FReadSurfaceDataFlags ReadFlags;
	ReadFlags.SetLinearToGamma(false);
	if (!RTResource->ReadPixels(RawPixels, ReadFlags))
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to read pixels from preview render target."));
		return;
	}

	// --- 3. Manually set alpha ---
	for (FColor& Pixel : RawPixels)
	{
		Pixel.A = 255;
	}

	// --- 4. Create or Update our GC-safe texture ---
	// This was the source of the bug. We now store the texture in a UPROPERTY member.
	if (this->OpaquePreviewTexture)
    {
        this->OpaquePreviewTexture->RemoveFromRoot();
    }
	this->OpaquePreviewTexture = UTexture2D::CreateTransient(PreviewRenderTarget->SizeX, PreviewRenderTarget->SizeY, PF_B8G8R8A8);
	this->OpaquePreviewTexture->AddToRoot();
	if (!this->OpaquePreviewTexture)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to create transient texture for preview."));
		return;
	}

	// Get a pointer to the texture's memory block and copy the pixel data
	void* TextureData = this->OpaquePreviewTexture->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(TextureData, RawPixels.GetData(), RawPixels.Num() * sizeof(FColor));
	this->OpaquePreviewTexture->GetPlatformData()->Mips[0].BulkData.Unlock();

	// Update the texture resource on the GPU
	this->OpaquePreviewTexture->UpdateResource();

	// --- 5. Update the UI Brush to use our new GC-safe Texture ---
	PreviewBrush->SetResourceObject(this->OpaquePreviewTexture);
	Invalidate(EInvalidateWidget::LayoutAndVolatility);
}

void STextureProjectionWindow::SetCaptureActor(TObjectPtr<ASceneCapture2D> InCaptureActor)
{
    CaptureActor = InCaptureActor;
    if (CaptureActor) { UpdateCameraPreview(); }
}

void STextureProjectionWindow::OnWindowClosed(const TSharedRef<SWindow>& Window)
{
    if (bHasBeenCleanedUp)
    {
        return;
    }
    bHasBeenCleanedUp = true;

    // Restore autosave first (safe operation)
    if (bHasDisabledAutosave)
    {
        if (UEditorLoadingSavingSettings* LoadingSavingSettings = GetMutableDefault<UEditorLoadingSavingSettings>())
        {
            LoadingSavingSettings->bAutoSaveEnable = bOriginalAutosaveEnabled;
            LoadingSavingSettings->SaveConfig();
        }
        bHasDisabledAutosave = false;
    }

    // Defer UObject cleanup to next frame to avoid mid-destruction issues
    if (UObjectInitialized())
    {
        TWeakObjectPtr<UTextureRenderTarget2D> WeakRT = PreviewRenderTarget;
        TWeakObjectPtr<UTexture2D> WeakTex = OpaquePreviewTexture;
        
        AsyncTask(ENamedThreads::GameThread, [WeakRT, WeakTex]()
        {
            if (WeakRT.IsValid())
            {
                WeakRT->RemoveFromRoot();
            }
            if (WeakTex.IsValid())
            {
                WeakTex->RemoveFromRoot();
            }
        });
    }

    PreviewRenderTarget = nullptr;
    OpaquePreviewTexture = nullptr;
}


void STextureProjectionWindow::OnTextureChanged(const FAssetData& AssetData)
{
	Settings.SourceTexture = Cast<UTexture2D>(AssetData.GetAsset());
	NotifySettingsChanged();
}

FString STextureProjectionWindow::GetTextureObjectPath() const
{
	if (IsValid(Settings.SourceTexture))
	{
		return Settings.SourceTexture->GetPathName();
	}

	return FString("None");
}

void STextureProjectionWindow::OnOutputPathChanged(const FText& NewValue)
{
	GlobalUI_OutputPath = NewValue.ToString();
	NotifySettingsChanged();
	// We don't need to call NotifySettingsChanged() here because OnTextChanged
	// doesn't trigger a full update like a button click does. The lambda binding
	// for the text box will ensure the value is saved when needed.
}

void STextureProjectionWindow::OnOutputWidthChanged(int32 NewValue)
{
	GlobalUI_OutputWidth = NewValue;
	NotifySettingsChanged();
    OnOutputResolutionChangedCallback.ExecuteIfBound(GlobalUI_OutputWidth, GlobalUI_OutputHeight);
}

void STextureProjectionWindow::OnOutputHeightChanged(int32 NewValue)
{
	GlobalUI_OutputHeight = NewValue;
	NotifySettingsChanged();
    OnOutputResolutionChangedCallback.ExecuteIfBound(GlobalUI_OutputWidth, GlobalUI_OutputHeight);
}

void STextureProjectionWindow::SetOnOutputResolutionChanged(FOnOutputResolutionChanged InDelegate)
{
    OnOutputResolutionChangedCallback = InDelegate;
}

void STextureProjectionWindow::OnUseComfyUIChanged(ECheckBoxState NewState)
{
	Settings.bUseComfyUI = (NewState == ECheckBoxState::Checked);
	NotifySettingsChanged(); // This will trigger a UI refresh to enable/disable the texture picker
}

void STextureProjectionWindow::OnEdgeFalloffChanged(float NewValue)
{
	Settings.EdgeFalloff = NewValue;
	NotifySettingsChanged();
}


FReply STextureProjectionWindow::OnSaveFinalTextureClicked()
{
    // When clicked, this just calls the delegate that will be bound to our controller's function.
    OnSaveFinalTexture.ExecuteIfBound();
    return FReply::Handled();
}

const TArray<FProjectionSettings>& STextureProjectionWindow::GetAllSettings() 
{
    // Before returning all settings, ensure the currently active tab is saved.
    if(CameraSettings.IsValidIndex(ActiveTabIndex))
    {
        CameraSettings[ActiveTabIndex] = Settings;
		CameraSettings[ActiveTabIndex].ComfyControlValues = this->DynamicControlValues;
        CameraSettings[ActiveTabIndex].ComfySeedStates = this->SeedRandomizationStates;
    }
    return CameraSettings;
}

void STextureProjectionWindow::UpdateUVChannels(int32 NumUVChannels, int32 CurrentChannel)
{
    UVChannelOptions.Empty();
    for (int32 i = 0; i < NumUVChannels; ++i)
    {
        UVChannelOptions.Add(MakeShareable(new FString(FString::Printf(TEXT("UV Channel %d"), i))));
    }

    if (UVChannelComboBox.IsValid())
    {
        // Set the initially selected item if it's valid
        if (UVChannelOptions.IsValidIndex(CurrentChannel))
        {
            UVChannelComboBox->SetSelectedItem(UVChannelOptions[CurrentChannel]);
        }
        // Refresh the options in the combo box
        UVChannelComboBox->RefreshOptions();
    }
}

void STextureProjectionWindow::OnUVChannelChanged(TSharedPtr<FString> NewSelection, ESelectInfo::Type SelectInfo)
{
    if (NewSelection.IsValid())
    {
        // Find the index of the selected option
        int32 SelectedIndex = UVChannelOptions.Find(NewSelection);
        if (SelectedIndex != INDEX_NONE)
        {
            // Execute the callback to notify the main controller of the change
            OnUVChannelChangedCallback.ExecuteIfBound(SelectedIndex);
        }
    }
}

FReply STextureProjectionWindow::OnResetClicked()
{
	OnReset.ExecuteIfBound();
	return FReply::Handled();
}

void STextureProjectionWindow::SetGlobalSettingsLock(bool bIsLocked)
{
	bAreGlobalSettingsLocked = bIsLocked;
}

void STextureProjectionWindow::OnWeightChanged(float NewValue)
{
    Settings.Weight = NewValue;
    NotifySettingsChanged();
}
	
void STextureProjectionWindow::OnWeightCommitted(float NewValue, ETextCommit::Type CommitType)
{
    Settings.Weight = NewValue;
    NotifySettingsChanged();
    OnBlendSettingsCommitted.ExecuteIfBound(); // Uses the fast re-blend
}

void STextureProjectionWindow::OnFadeStartAngleCommitted(float NewValue, ETextCommit::Type CommitType)
{
    Settings.FadeStartAngle = NewValue;
    NotifySettingsChanged();

    // FIX: Get all current settings from the UI and pass them through the delegate
    OnBlendParametersChanged.ExecuteIfBound(GetAllSettings());
}

void STextureProjectionWindow::OnEdgeFalloffCommitted(float NewValue, ETextCommit::Type CommitType)
{
    Settings.EdgeFalloff = NewValue;
    NotifySettingsChanged();
    
    // FIX: Get all current settings from the UI and pass them through the delegate
    OnBlendParametersChanged.ExecuteIfBound(GetAllSettings());
}

void STextureProjectionWindow::SetOnSingleProjectionConfirmed(FOnSingleProjectionConfirmed InOnSingleProjectionConfirmed) { OnSingleProjectionConfirmed = InOnSingleProjectionConfirmed; }
FReply STextureProjectionWindow::OnExportDepthClicked() { return FReply::Handled(); }
FReply STextureProjectionWindow::OnExportMaskClicked() { return FReply::Handled(); }
FReply STextureProjectionWindow::OnExportCurrentViewClicked() { return FReply::Handled(); }
FReply STextureProjectionWindow::OnExportWeightMapClicked() { return FReply::Handled(); }



void STextureProjectionWindow::OnWorkflowPathPicked(const FString& NewPath)
{
    // Check if our data is valid
    if (CameraSettings.IsValidIndex(ActiveTabIndex))
    {
        // Update the master data array with the new path
        CameraSettings[ActiveTabIndex].WorkflowApiJson = NewPath;
        // Also update our local 'Settings' copy so the UI refreshes instantly
        Settings.WorkflowApiJson = NewPath;
    }
    
    // Trigger a rebuild of the dynamic UI controls based on the new workflow
    RebuildDynamicControls();

    // Notify the main controller that settings have changed
    NotifySettingsChanged();
}

void STextureProjectionWindow::RebuildDynamicControls()
{
    if (!DynamicControlsBox.IsValid()) return;

    DynamicControlsBox->ClearChildren();
    ParsedNodeData.Empty();

    if (Settings.WorkflowApiJson.IsEmpty() || !FPaths::FileExists(Settings.WorkflowApiJson))
    {
        DynamicControlsBox->AddSlot().AutoHeight()
        [
            SNew(STextBlock)
            .Text(FText::FromString("[Select a valid ComfyUI workflow file]"))
            .Font(FCoreStyle::Get().GetFontStyle("Italic"))
        ];
        return;
    }

    FTextureDiffusion3D& TextureDiffusionModule = FModuleManager::LoadModuleChecked<FTextureDiffusion3D>("TextureDiffusion3D");
    TextureDiffusionModule.ParseWorkflowForUnrealNodes(Settings.WorkflowApiJson, ParsedNodeData);

    const TSet<FString> KnownIntegerInputs = { TEXT("seed"), TEXT("steps"), TEXT("width"), TEXT("height"), TEXT("batch_size") };
    const TMap<FString, int32> FractionalDigitRules = {
        { TEXT("cfg"), 1 }, { TEXT("denoise"), 2 }, { TEXT("strength"), 2 }, { TEXT("scale"), 2 }
    };

    TSharedPtr<FJsonObject> WorkflowObject = TextureDiffusionModule.GetWorkflowJsonObject(Settings.WorkflowApiJson);
    if (!WorkflowObject.IsValid()) return;

    for (const FUnrealNodeInfo& NodeInfo : ParsedNodeData)
    {
        if (NodeInfo.bIsImageInput)
        {
            DynamicControlsBox->AddSlot().AutoHeight().Padding(5, 8)
            [
                SNew(SBorder).BorderImage(FCoreStyle::Get().GetBrush("ToolPanel.GroupBorder")).Padding(FMargin(8.0f))
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
                    [
                        SNew(STextBlock).Text(FText::FromString(TEXT("🖼️")))
                    ]
                    + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
                    [
                        SNew(STextBlock).Text(FText::FromString(NodeInfo.CleanTitle))
                    ]
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                    [
                        SNew(SButton)
                        .ButtonStyle(FCoreStyle::Get(), "SimpleButton")
                        .ToolTipText(FText::FromString("Preview this generated input"))
                        .OnClicked_Lambda([this, NodeInfo]() -> FReply {
                            if (OnGetPreviewTexture.IsBound())
                            {
                                UTexture2D* TextureToPreview = OnGetPreviewTexture.Execute(NodeInfo.ImageKeyword);
                                ShowLightboxForTexture(TextureToPreview);
                            }
                            return FReply::Handled();
                        })
                        [
                            SNew(SImage).Image(FCoreStyle::Get().GetBrush("LevelEditor.ViewOptions"))
                        ]
                    ]
                ]
            ];
        }
        else 
        {
            TSharedRef<SVerticalBox> NodeWidgetsBox = SNew(SVerticalBox);
            const TSharedPtr<FJsonObject> DefaultInputs = WorkflowObject->GetObjectField(NodeInfo.NodeId)->GetObjectField(TEXT("inputs"));

            for (const FUnrealInputInfo& InputInfo : NodeInfo.ExposedInputs)
            {
                TSharedRef<SHorizontalBox> RowWidget = SNew(SHorizontalBox);
                RowWidget->AddSlot()
                    .FillWidth(0.4f).VAlign(VAlign_Top).Padding(0, 3, 0, 0)
                    [
                        SNew(STextBlock).Text(FText::FromString(InputInfo.InputName))
                    ];

                const FString WidgetKey = FString::Printf(TEXT("%s.%s"), *NodeInfo.NodeId, *InputInfo.InputName);

                if (InputInfo.InputName.Equals(TEXT("seed"), ESearchCase::IgnoreCase))
                {
                    RowWidget->AddSlot()
                        .FillWidth(0.6f)
                        [
                            SNew(SHorizontalBox)
                            +SHorizontalBox::Slot().FillWidth(1.0f)
                            [
                                SNew(SSpinBox<double>)
                                .Value_Lambda([this, WidgetKey, DefaultValue = DefaultInputs->GetNumberField(InputInfo.InputName)]() {
                                    if (const TSharedPtr<FJsonValue>* FoundValue = DynamicControlValues.Find(WidgetKey))
                                    {
                                        return (*FoundValue)->AsNumber();
                                    }
                                    return DefaultValue;
                                })
                                .MinValue(0).Delta(1.0)
                                .IsEnabled_Lambda([this, WidgetKey] { 
                                    return !SeedRandomizationStates.FindOrAdd(WidgetKey, true); 
                                })
                                .OnValueChanged_Lambda([this, WidgetKey](double NewValue) {
                                    DynamicControlValues.Emplace(WidgetKey, MakeShareable(new FJsonValueNumber(static_cast<int64>(NewValue))));
                                    NotifySettingsChanged();
                                })
                            ]
                            +SHorizontalBox::Slot().AutoWidth().Padding(8, 0, 0, 0).VAlign(VAlign_Center)
                            [
                                SNew(SCheckBox)
                                .IsChecked_Lambda([this, WidgetKey]() -> ECheckBoxState {
                                    return SeedRandomizationStates.FindOrAdd(WidgetKey, true) 
                                        ? ECheckBoxState::Checked 
                                        : ECheckBoxState::Unchecked;
                                })
                                .OnCheckStateChanged_Lambda([this, WidgetKey](ECheckBoxState NewState) {
                                    SeedRandomizationStates.Add(WidgetKey, NewState == ECheckBoxState::Checked);
                                    NotifySettingsChanged();
                                })
                            ]
                            +SHorizontalBox::Slot().AutoWidth().Padding(4, 0, 0, 0).VAlign(VAlign_Center)
                            [
                                SNew(STextBlock).Text(FText::FromString("Randomize"))
                            ]
                        ];
                }
                else // Logic for all other inputs
                {
                    TSharedRef<SWidget> ValueWidget = SNullWidget::NullWidget;

                    if (InputInfo.JsonType == EJson::String)
                    {
                        SAssignNew(ValueWidget, SMultiLineEditableTextBox)
                            .Text_Lambda([this, WidgetKey, DefaultValue = DefaultInputs->GetStringField(InputInfo.InputName)]() {
                                if (const TSharedPtr<FJsonValue>* FoundValue = DynamicControlValues.Find(WidgetKey))
                                {
                                    return FText::FromString((*FoundValue)->AsString());
                                }
                                return FText::FromString(DefaultValue);
                            })
                            .OnTextChanged_Lambda([this, WidgetKey](const FText& NewText) {
                                DynamicControlValues.Emplace(WidgetKey, MakeShareable(new FJsonValueString(NewText.ToString())));
                                NotifySettingsChanged();
                            })
                            .AutoWrapText(true)
                            .ModiferKeyForNewLine(EModifierKey::Shift);
                    }
                    else if (InputInfo.JsonType == EJson::Number)
                    {
                        const double DefaultValue = DefaultInputs->GetNumberField(InputInfo.InputName);

                        if (KnownIntegerInputs.Contains(InputInfo.InputName))
                        {
                            SAssignNew(ValueWidget, SSpinBox<double>)
                                .Value_Lambda([this, WidgetKey, DefaultValue]() {
                                    if (const TSharedPtr<FJsonValue>* FoundValue = DynamicControlValues.Find(WidgetKey))
                                    {
                                        return (*FoundValue)->AsNumber();
                                    }
                                    return DefaultValue;
                                })
                                .Delta(1.0).MinValue(0)
                                .OnValueChanged_Lambda([this, WidgetKey](double NewValue) {
                                    DynamicControlValues.Emplace(WidgetKey, MakeShareable(new FJsonValueNumber(static_cast<int64>(NewValue))));
                                    NotifySettingsChanged();
                                });
                        }
                        else // Float Inputs
                        {
                            int32 MaxDigits = FractionalDigitRules.FindRef(InputInfo.InputName);
                            if (MaxDigits == 0) MaxDigits = 2;

                            SAssignNew(ValueWidget, SSpinBox<double>)
                                .Value_Lambda([this, WidgetKey, DefaultValue]() {
                                    if (const TSharedPtr<FJsonValue>* FoundValue = DynamicControlValues.Find(WidgetKey))
                                    {
                                        return (*FoundValue)->AsNumber();
                                    }
                                    return DefaultValue;
                                })
                                .Delta(FMath::Pow(10.0, -MaxDigits))
                                .MaxFractionalDigits(MaxDigits)
                                .OnValueChanged_Lambda([this, WidgetKey](double NewValue) {
                                    DynamicControlValues.Emplace(WidgetKey, MakeShareable(new FJsonValueNumber(NewValue)));
                                    NotifySettingsChanged();
                                });
                        }
                    }
                    RowWidget->AddSlot().FillWidth(0.6f)[ValueWidget];
                }
                NodeWidgetsBox->AddSlot().AutoHeight().Padding(2, 4)[RowWidget];
            }

            if (NodeInfo.ExposedInputs.Num() > 0)
            {
                DynamicControlsBox->AddSlot().AutoHeight().Padding(5, 8)
                [
                    SNew(SExpandableArea)
                    .AreaTitle(FText::FromString(NodeInfo.CleanTitle))
                    .InitiallyCollapsed(false)
                    .Padding(FMargin(8.0f)).BodyContent()[NodeWidgetsBox]
                ];
            }
        }
    }
}


FReply STextureProjectionWindow::OnBrowseButtonClicked()
{
    IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
    if (DesktopPlatform)
    {
        // --- START: NEW LOGIC ---
        FString DefaultPath;
        // Get the plugin's base directory to build a default path.
        TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("TextureDiffusion3D"));
        if (Plugin.IsValid())
        {
            // Set the default path to the plugin's workflow folder.
            DefaultPath = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources"), TEXT("Workflows"));
        }
        // --- END: NEW LOGIC ---

        void* ParentWindowHandle = nullptr;
        if (ParentWindowPtr.IsValid())
        {
            ParentWindowHandle = ParentWindowPtr.Pin()->GetNativeWindow()->GetOSWindowHandle();
        }

        TArray<FString> OutFileNames;
        bool bOpened = DesktopPlatform->OpenFileDialog(
            ParentWindowHandle,
            TEXT("Select a ComfyUI Workflow"),
            DefaultPath, // <-- USE THE NEW DEFAULT PATH HERE
            TEXT(""),    // Default file (can be empty)
            TEXT("JSON Workflows (*.json)|*.json"),
            EFileDialogFlags::None,
            OutFileNames
        );

        if (bOpened && OutFileNames.Num() > 0)
        {
            OnWorkflowPathPicked(OutFileNames[0]);
        }
    }
    return FReply::Handled();
}

FReply STextureProjectionWindow::OnLightboxClicked(const FGeometry&, const FPointerEvent&)
{
    if (LightboxContainer.IsValid())
    {
        LightboxContainer->SetVisibility(EVisibility::Collapsed);
    }
    return FReply::Handled();
}

void STextureProjectionWindow::ShowLightboxForTexture(UTexture2D* TextureToShow)
{
    if (TextureToShow && LightboxImage.IsValid() && LightboxContainer.IsValid())
    {
		UE_LOG(LogTemp, Log, TEXT("ShowLightboxForTexture: Displaying texture '%s'"), *TextureToShow->GetName());
        
        // Update the image and its brush
        LightboxImage->SetImage(new FSlateImageBrush(TextureToShow, FVector2D(TextureToShow->GetSizeX(), TextureToShow->GetSizeY())));

        // Show the overlay
        LightboxContainer->SetVisibility(EVisibility::Visible);
    }
	else{
		UE_LOG(LogTemp, Warning, TEXT("ShowLightboxForTexture: Called with an invalid texture. Nothing to show."));
	}
}


void STextureProjectionWindow::UpdateMaterialSlots()
{
    MaterialSlotOptions.Empty();

    // 1. Break the validation into clear, debuggable steps.
    if (!TargetActor.IsValid())
    {
        UE_LOG(LogTemp, Warning, TEXT("UpdateMaterialSlots: TargetActor is not valid."));
        return; 
    }

    UStaticMeshComponent* MeshComponent = TargetActor->GetStaticMeshComponent();
    if (!MeshComponent)
    {
        UE_LOG(LogTemp, Warning, TEXT("UpdateMaterialSlots: Actor '%s' has no StaticMeshComponent."), *TargetActor->GetName());
        return; // Exit early
    }

    UStaticMesh* StaticMesh = MeshComponent->GetStaticMesh();
    if (!StaticMesh)
    {
        UE_LOG(LogTemp, Warning, TEXT("UpdateMaterialSlots: StaticMeshComponent on '%s' has no StaticMesh assigned."), *TargetActor->GetName());
        // Add a default option so the dropdown isn't blank
        MaterialSlotOptions.Add(MakeShareable(new FString(TEXT("No Mesh Assigned"))));
    }
    else
    {
        // 2. This part of the logic was correct.
        const TArray<FStaticMaterial>& Materials = StaticMesh->GetStaticMaterials();

        if (Materials.Num() == 0)
        {
            MaterialSlotOptions.Add(MakeShareable(new FString(TEXT("Slot 0: Default"))));
        }
        else
        {
            for (int32 i = 0; i < Materials.Num(); ++i)
            {
                const FStaticMaterial& Material = Materials[i];
                FString SlotName = Material.MaterialSlotName.ToString();
                FString DisplayName = FString::Printf(TEXT("Slot %d: %s"), i, *SlotName);
                MaterialSlotOptions.Add(MakeShareable(new FString(DisplayName)));
            }
        }
        UE_LOG(LogTemp, Log, TEXT("UpdateMaterialSlots: Found %d material slots for '%s'."), Materials.Num(), *StaticMesh->GetName());
    }

    // 3. Refresh the widget with the new options.
    if (MaterialSlotComboBox.IsValid())
    {
        MaterialSlotComboBox->RefreshOptions();

        // Set the current selection safely.
        if (MaterialSlotOptions.IsValidIndex(Settings.TargetMaterialSlotIndex))
        {
            MaterialSlotComboBox->SetSelectedItem(MaterialSlotOptions[Settings.TargetMaterialSlotIndex]);
        }
        else if (MaterialSlotOptions.Num() > 0)
        {
            // Fallback to the first item if the saved index is out of bounds
            MaterialSlotComboBox->SetSelectedItem(MaterialSlotOptions[0]);
            Settings.TargetMaterialSlotIndex = 0;
        }
    }

}

void STextureProjectionWindow::HandleMaterialSlotSelectionChanged(TSharedPtr<FString> NewSelection, ESelectInfo::Type SelectInfo)
{

    if (SelectInfo == ESelectInfo::Direct)
    {
        return;
    }

    if (NewSelection.IsValid())
    {
        int32 SelectedIndex = MaterialSlotOptions.Find(NewSelection);
        if (SelectedIndex != INDEX_NONE)
        {
            Settings.TargetMaterialSlotIndex = SelectedIndex;
            
            // Notify the main controller that the active slot has changed
            OnMaterialSlotChanged.ExecuteIfBound(SelectedIndex);
        }
    }
}

void STextureProjectionWindow::OnCheckboxStateChanged(ECheckBoxState NewState)
{
    // When the checkbox is clicked, execute the delegate to notify the main plugin class
    const bool bIsChecked = (NewState == ECheckBoxState::Checked);
    OnOccluderStateChanged.ExecuteIfBound(bIsChecked);
}

// Define the new update function
void STextureProjectionWindow::UpdateOccluderCheckbox(bool bIsChecked)
{
    if (OccluderCheckbox.IsValid())
    {
        // Set the checkbox state without triggering the OnCheckStateChanged event
        ECheckBoxState NewState = bIsChecked ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
        OccluderCheckbox->SetIsChecked(NewState);
    }
}




END_SLATE_FUNCTION_BUILD_OPTIMIZATION