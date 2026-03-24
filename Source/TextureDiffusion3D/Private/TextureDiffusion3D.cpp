//

#include "TextureDiffusion3D.h"
#include "TextureDiffusion3DStyle.h"
#include "TextureDiffusion3DCommands.h"
#include "Misc/MessageDialog.h"
#include "ToolMenus.h"



#include "Kismet/GameplayStatics.h"

#include "Kismet/KismetRenderingLibrary.h"

#include "Editor.h"
#include "Engine/Selection.h"

#include "Materials/Material.h"
#include "Materials/MaterialExpressionTextureSample.h"

#include "StaticMeshAttributes.h" 

#include "Engine/TextureRenderTarget2D.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/SceneCapture2D.h"

// Include helper headers
#include "Helpers/CameraHelper.h"
#include "Helpers/MeshProcessor.h"
#include "Helpers/TextureUtils.h"
#include "Helpers/MathUtils.h"


#include "UObject/SavePackage.h"
#include "RenderingThread.h" 

#include "DrawDebugHelpers.h"

#include "Materials/MaterialInstanceConstant.h"

#include "Engine/Canvas.h"

#include "HAL/FileManager.h"

#include "Materials/MaterialExpressionConstant.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Modules/ModuleManager.h"
#include "EditorAssetLibrary.h"
#include "MaterialEditingLibrary.h"
#include "Factories/MaterialFactoryNew.h"

#include "Containers/Ticker.h"

#include "IContentBrowserSingleton.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "Framework/Application/SlateApplication.h"



#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"


#include "TextureDiffusion3DSettings.h"


static const FName TextureDiffusion3DTabName("TextureDiffusion3D");

#define LOCTEXT_NAMESPACE "FTextureDiffusion3D"


FTextureDiffusion3D::FTextureDiffusion3D()
	: PluginCommands(nullptr)
	, SettingsWindow(nullptr)
	, SettingsWidget(nullptr)
	, SelectedActor(nullptr)
	, CaptureActor(nullptr)
	, ActiveCameraIndex(0)
	, NextTabId(0)
	, ProjectedTexture(nullptr)
	, Global_OutputPath(FPaths::ProjectSavedDir() / TEXT("TextureExports"))
	, Global_OutputTextureWidth(1024)
	, Global_OutputTextureHeight(1024)
	, TargetUVChannel(0)
	, bHasStartedProjections(false)
	// These members were added based on the warnings to ensure they are initialized
	, CurrentComfyTaskStartTime()
	, CurrentComfyExpectedOutputPrefix()
	, LastExportedDepthPath()
	, DelayedCaptureTickerHandle()
	, CurrentBlockingCaptureState(nullptr)
	, BlockingCaptureTickerHandle()
	, ParentMaterial_CumulativeDisplay(nullptr)
	, ParentMaterial_WeightMaskDisplay(nullptr)
	, MID_WeightMaskExport(nullptr)
	, CurrentProjection_Actor(nullptr)
	, CurrentProjection_MeshComponent(nullptr)
	, CurrentProjection_StaticMesh(nullptr)
	, CurrentProjection_CameraIndex(0)
	, CurrentProjection_TextureWidth(0)
	, CurrentProjection_TextureHeight(0)
	, CurrentProjection_NumTexels(0)
	, MaterialToRestoreAfterWeightMaskVisualization(nullptr)
	, bIsBatchMode(false)
	
{
	// The initializer list above handles setting the default values.
}

void FTextureDiffusion3D::StartupModule()
{
	UE_LOG(LogTemp, Log, TEXT("FTextureDiffusion3D::StartupModule called!"));
	FTextureDiffusion3DStyle::Initialize();
	FTextureDiffusion3DStyle::ReloadTextures();

	// Initialize member variables
	ProjectedTexture = nullptr;

	FTextureDiffusion3DCommands::Register();

	PluginCommands = MakeShareable(new FUICommandList);

	PluginCommands->MapAction(
		FTextureDiffusion3DCommands::Get().PluginAction,
		FExecuteAction::CreateRaw(this, &FTextureDiffusion3D::PluginButtonClicked),
		FCanExecuteAction());

	PluginCommands->MapAction(
		FTextureDiffusion3DCommands::Get().ProjectTextureAction,
		FExecuteAction::CreateRaw(this, &FTextureDiffusion3D::ProjectTextureButtonClicked),
		FCanExecuteAction());

	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FTextureDiffusion3D::RegisterMenus));


	ParentMaterial_CumulativeDisplay = LoadObject<UMaterialInterface>(nullptr, TEXT("/TextureDiffusion3D/Materials/M_Parent_CumulativeView.M_Parent_CumulativeView"));
	ParentMaterial_WeightMaskDisplay = LoadObject<UMaterialInterface>(nullptr, TEXT("/TextureDiffusion3D/Materials/M_Parent_WeightMaskView.M_Parent_WeightMaskView"));

	if (!ParentMaterial_CumulativeDisplay) {
		UE_LOG(LogTemp, Error, TEXT("CRITICAL: Failed to load ParentMaterial_CumulativeDisplay! Please check path: /TextureDiffusion3D/Materials/M_Parent_CumulativeView"));
	}
	if (!ParentMaterial_WeightMaskDisplay) {
		UE_LOG(LogTemp, Error, TEXT("CRITICAL: Failed to load ParentMaterial_WeightMaskDisplay! Please check path: /TextureDiffusion3D/Materials/M_Parent_WeightMaskView"));
	}
}

void FTextureDiffusion3D::ShutdownModule()
{
	// Unregister our menus and style
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);

	FTextureDiffusion3DStyle::Shutdown();
	FTextureDiffusion3DCommands::Unregister();
	
	// If we created a scene-capture actor, tear it down—but only if we
	// actually still have a valid GEditor + editor world.
	if (CaptureActor)
	{
		if (GEditor)
		{
			// Get the editor world context
			FWorldContext& EditorContext = GEditor->GetEditorWorldContext();
			UWorld* World = EditorContext.World();
			
			if (World)
			{
				FCameraHelper::CleanupSceneCapture(World, CaptureActor);
			}
		}

		CaptureActor = nullptr;
	}
}


// #include "Editor.h"
// #include "LevelEditor.h"
// #include "HAL/IConsoleManager.h"
// #include "Engine/Engine.h"
#include "LevelEditorViewport.h" 

struct FScopedUEPerformanceThrottler
{
private:
	// Store original settings
	float OriginalScreenPercentage;
	int32 OriginalStreamingPoolSize; // <-- NEW: For VRAM
	TMap<FLevelEditorViewportClient*, bool> OriginalRealtimeStates; 
	IConsoleVariable* ScreenPercentageCVar;
	IConsoleVariable* StreamingPoolSizeCVar; // <-- NEW: For VRAM

public:
	// Constructor: Called when the object is created. Applies low-power settings.
	FScopedUEPerformanceThrottler() : OriginalScreenPercentage(100.0f), OriginalStreamingPoolSize(1000), ScreenPercentageCVar(nullptr), StreamingPoolSizeCVar(nullptr)
	{
		UE_LOG(LogTemp, Log, TEXT("Throttling UE performance for ComfyUI task..."));

		// 1. Force Garbage Collection to free up System RAM immediately
		if (GEngine)
		{
			GEngine->ForceGarbageCollection(true);
			UE_LOG(LogTemp, Log, TEXT("PerformanceThrottler: Forced garbage collection."));
		}

		// 2. Reduce Screen Percentage to drastically lower GPU LOAD
		ScreenPercentageCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ScreenPercentage"));
		if (ScreenPercentageCVar)
		{
			OriginalScreenPercentage = ScreenPercentageCVar->GetFloat();
			const float LowPerformancePercentage = 10.0f; // Can go even lower for pure waiting
			ScreenPercentageCVar->Set(LowPerformancePercentage, ECVF_SetByCode);
			UE_LOG(LogTemp, Log, TEXT("PerformanceThrottler: Screen Percentage lowered from %.f to %.f."), OriginalScreenPercentage, LowPerformancePercentage);
		}

        // 3. Reduce Texture Streaming Pool to free up VRAM
        StreamingPoolSizeCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Streaming.PoolSize"));
        if (StreamingPoolSizeCVar)
        {
            OriginalStreamingPoolSize = StreamingPoolSizeCVar->GetInt();
            // Set to a very small pool size (e.g., 200MB) to force textures out of VRAM
            const int32 LowVRAMPoolSize = 200;
            StreamingPoolSizeCVar->Set(LowVRAMPoolSize, ECVF_SetByCode);
            UE_LOG(LogTemp, Log, TEXT("PerformanceThrottler: VRAM Streaming Pool lowered from %d MB to %d MB."), OriginalStreamingPoolSize, LowVRAMPoolSize);
        }

		// 4. Disable Real-time rendering in all editor viewports
		if (GEditor)
		{
			for (FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
			{
				if (ViewportClient && ViewportClient->IsRealtime())
				{
					OriginalRealtimeStates.Add(ViewportClient, true);
					ViewportClient->SetRealtime(false);
				}
			}
			if(OriginalRealtimeStates.Num() > 0)
			{
				UE_LOG(LogTemp, Log, TEXT("PerformanceThrottler: Disabled real-time rendering on %d viewport(s)."), OriginalRealtimeStates.Num());
			}
		}
	}

	// Destructor: Called automatically when the object goes out of scope. Restores settings.
	~FScopedUEPerformanceThrottler()
	{
		UE_LOG(LogTemp, Log, TEXT("Restoring UE performance after ComfyUI task."));

		// 1. Restore Screen Percentage
		if (ScreenPercentageCVar)
		{
			ScreenPercentageCVar->Set(OriginalScreenPercentage, ECVF_SetByCode);
			UE_LOG(LogTemp, Log, TEXT("PerformanceThrottler: Restored Screen Percentage to %.f."), OriginalScreenPercentage);
		}

        // 2. Restore Texture Streaming Pool
        if (StreamingPoolSizeCVar)
        {
            StreamingPoolSizeCVar->Set(OriginalStreamingPoolSize, ECVF_SetByCode);
            UE_LOG(LogTemp, Log, TEXT("PerformanceThrottler: Restored VRAM Streaming Pool to %d MB."), OriginalStreamingPoolSize);
        }

		// 3. Restore Real-time Viewports
		if (GEditor)
		{
			for (auto const& Pair : OriginalRealtimeStates)
			{
				if (Pair.Key) 
				{
					Pair.Key->SetRealtime(Pair.Value);
				}
			}
			if (OriginalRealtimeStates.Num() > 0)
			{
				UE_LOG(LogTemp, Log, TEXT("PerformanceThrottler: Restored real-time rendering on %d viewport(s)."), OriginalRealtimeStates.Num());
			}
		}
	}
};


void FTextureDiffusion3D::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);
	
	// Add to Window menu
	{
		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Window");
		{
			FToolMenuSection& Section = Menu->FindOrAddSection("WindowLayout");
			Section.AddMenuEntryWithCommandList(FTextureDiffusion3DCommands::Get().PluginAction, PluginCommands);
			// Add the new command to Window menu
			Section.AddMenuEntryWithCommandList(FTextureDiffusion3DCommands::Get().ProjectTextureAction, PluginCommands);
		}
	}

	// Add to toolbar
	{
		UToolMenu* ToolbarMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar.PlayToolBar");
		{
			FToolMenuSection& Section = ToolbarMenu->FindOrAddSection("PluginTools");
			{
				// Add original button
				FToolMenuEntry& Entry = Section.AddEntry(FToolMenuEntry::InitToolBarButton(FTextureDiffusion3DCommands::Get().PluginAction));
				Entry.SetCommandList(PluginCommands);
				
				// Add new projection button
				FToolMenuEntry& ProjectEntry = Section.AddEntry(FToolMenuEntry::InitToolBarButton(FTextureDiffusion3DCommands::Get().ProjectTextureAction));
				ProjectEntry.SetCommandList(PluginCommands);
			}
		}
	}
}

// Returns something like: <Project>/Saved/TD3D_Debug/<prefix>_<label>_<WxH>_<timestamp>.png
static FString TD3D_BuildDebugPath(const FString& Prefix, const FString& Label, int32 W, int32 H)
{
	const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("TD3D_Debug"));
	IFileManager::Get().MakeDirectory(*Dir, /*Tree=*/true);

	const FString Time = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
	return FPaths::Combine(
		Dir,
		FString::Printf(TEXT("%s_%s_%dx%d_%s.png"), *Prefix, *Label, W, H, *Time)
	);
}


static FORCEINLINE FVector DecodeRGBToUnitVector(const FColor& C)
{
    // Assumes the normal texture is linear already. If the texture is sRGB,
    // convert to linear before this mapping.
    const FVector n(
        (C.R / 255.0f) * 2.0f - 1.0f,
        (C.G / 255.0f) * 2.0f - 1.0f,
        (C.B / 255.0f) * 2.0f - 1.0f
    );
    return n.GetSafeNormal();
}

static FORCEINLINE FColor EncodeUnitVectorToRGB(const FVector& n, uint8 Alpha = 255)
{
    const FVector v = n.GetSafeNormal();
    FColor out;
    out.R = (uint8)FMath::Clamp(FMath::RoundToInt((v.X * 0.5f + 0.5f) * 255.0f), 0, 255);
    out.G = (uint8)FMath::Clamp(FMath::RoundToInt((v.Y * 0.5f + 0.5f) * 255.0f), 0, 255);
    out.B = (uint8)FMath::Clamp(FMath::RoundToInt((v.Z * 0.5f + 0.5f) * 255.0f), 0, 255);
    out.A = Alpha;
    return out;
}


// FTextureDiffusion3D.cpp

// --- ADD THIS HELPER FUNCTION ---
// Finds the ID of a node in the workflow JSON based on its title.
static FString FindNodeIDByTitle(const TSharedPtr<FJsonObject>& WorkflowObject, const FString& NodeTitle)
{
    if (!WorkflowObject.IsValid())
    {
        return FString();
    }

    for (const auto& NodePair : WorkflowObject->Values)
    {
        const TSharedPtr<FJsonObject>* NodeObjectPtr;
        if (NodePair.Value->TryGetObject(NodeObjectPtr))
        {
            const TSharedPtr<FJsonObject> NodeObject = *NodeObjectPtr;
            const TSharedPtr<FJsonObject>* MetaObjectPtr;
            if (NodeObject->TryGetObjectField(TEXT("_meta"), MetaObjectPtr))
            {
                FString Title;
                if ((*MetaObjectPtr)->TryGetStringField(TEXT("title"), Title) && Title.Equals(NodeTitle, ESearchCase::IgnoreCase))
                {
                    return NodePair.Key; // This is the Node ID (e.g., "3")
                }
            }
        }
    }
    return FString(); // Not found
}

// FTextureDiffusion3D.cpp

void FTextureDiffusion3D::PluginButtonClicked()
{
//     UE_LOG(LogTemp, Log, TEXT("--- [Multi-Output Test] Starting Full Workflow & Polling Test ---"));

//     TObjectPtr<AStaticMeshActor> ActorToProcess = GetSelectedActor();
//     if (!ActorToProcess)
//     {
//         FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("Please select a single Static Mesh Actor to test the upload."));
//         return;
//     }
//     SelectedActor = ActorToProcess;

//     // --- THIS IS THE FIX ---
//     // Set up the context for the entire operation. This stores the actor,
//     // mesh, and settings so ProcessProjectionResults can use them later.
//     const FBoxSphereBounds Bounds = ActorToProcess->GetComponentsBoundingBox(true);
//     CurrentSettings.CameraPosition = Bounds.Origin + FVector(Bounds.SphereRadius * 2.5f, 0.0f, 0.0f);
//     CurrentSettings.CameraRotation = (Bounds.Origin - CurrentSettings.CameraPosition).Rotation();
//     CurrentSettings.CameraRotation.Normalize();
//     CurrentSettings.FOVAngle = 45.0f; // A sensible default FOV.

// 	PerSlotCameraSettings.FindOrAdd(CurrentSettings.TargetMaterialSlotIndex).Add(CurrentSettings);


//     // Now that CurrentSettings is valid, we can set up the projection context.
//     if (!SetupProjectionContext(ActorToProcess, CurrentSettings))
//     {
//         return;
//     }


//     UTexture2D* DepthTexture = CreateDepthPreviewTexture();
//     if (!DepthTexture || !DepthTexture->GetPlatformData() || DepthTexture->GetPlatformData()->Mips.Num() == 0)
//     {
//         FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("Failed to generate a valid depth texture for upload."));
//         return;
//     }
    
//     // ... (The rest of the function remains exactly the same) ...
    
//     FTexture2DMipMap& Mip = DepthTexture->GetPlatformData()->Mips[0];
//     const int32 Width = Mip.SizeX;
//     const int32 Height = Mip.SizeY;
    
//     const void* MipData = Mip.BulkData.LockReadOnly();
//     TArray<FColor> PixelData;
//     PixelData.SetNum(Width * Height);
//     FMemory::Memcpy(PixelData.GetData(), MipData, Width * Height * sizeof(FColor));
//     Mip.BulkData.Unlock();
    
//     TArray64<uint8> PngData;
//     FImageUtils::PNGCompressImageArray(Width, Height, PixelData, PngData);

//     if (PngData.Num() == 0)
//     {
//         FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("Failed to compress texture to PNG format."));
//         return;
//     }

//     FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
//     FString Filename = FString::Printf(TEXT("ue_depth_test_%s.png"), *Timestamp);

//     FOnImageUploadComplete OnUploadCompleteDelegate = FOnImageUploadComplete::CreateLambda(
//         [this](bool bSuccess, const FString& ServerFilename)
//         {
//             if (!bSuccess)
//             {
//                 FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("❌ Upload Failed. Cannot submit workflow.")));
//                 return;
//             }

//             FString WorkflowJsonContent;
//             const FString WorkflowFilePath = TEXT("C:\\Users\\tanmu\\Downloads\\qwedit_2025-10-10_15-57-48_00001_.json"); 
//             if (!FFileHelper::LoadFileToString(WorkflowJsonContent, *WorkflowFilePath))
//             {
//                 FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString::Printf(TEXT("Failed to load workflow file from: %s"), *WorkflowFilePath)));
//                 return;
//             }

//             TSharedPtr<FJsonObject> WorkflowObject;
//             TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(WorkflowJsonContent);
//             if (FJsonSerializer::Deserialize(Reader, WorkflowObject) && WorkflowObject.IsValid())
//             {
//                 FString NodeToModifyID = FindNodeIDByTitle(WorkflowObject, TEXT("Unreal Depth"));
//                 if (!NodeToModifyID.IsEmpty())
//                 {
//                     WorkflowObject->GetObjectField(NodeToModifyID)->GetObjectField(TEXT("inputs"))->SetStringField(TEXT("image"), ServerFilename);
//                 }
//                 else
//                 {
//                      FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("Could not find a node titled 'Unreal Depth' in your workflow JSON.")));
//                      return;
//                 }

//                 this->ExpectedOutputs.Empty();
//                 for (const auto& NodePair : WorkflowObject->Values)
//                 {
//                     const TSharedPtr<FJsonObject> NodeObject = NodePair.Value->AsObject();
//                     FString ClassType;
//                     if (NodeObject->TryGetStringField(TEXT("class_type"), ClassType) && ClassType.Equals(TEXT("SaveImage")))
//                     {
//                         const TSharedPtr<FJsonObject> Inputs = NodeObject->GetObjectField(TEXT("inputs"));
//                         FString Prefix = Inputs->GetStringField(TEXT("filename_prefix"));
                        
//                         EProjectionVariant Variant = VariantFromPrefixLoose(Prefix);
//                         this->ExpectedOutputs.Add(Prefix, Variant);
//                         UE_LOG(LogTemp, Log, TEXT("Found expected output: Prefix='%s', Identified Variant='%s'"), *Prefix, *UEnum::GetValueAsString(Variant));
//                     }
//                 }
                
//                 if (this->ExpectedOutputs.Num() == 0)
//                 {
//                     FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("Warning: No 'Save Image' nodes were found in the workflow.")));
//                 }

//                 TSharedPtr<FJsonObject> PayloadJson = MakeShareable(new FJsonObject);
//                 PayloadJson->SetObjectField(TEXT("prompt"), WorkflowObject);
//                 FString PayloadString;
//                 TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&PayloadString);
//                 FJsonSerializer::Serialize(PayloadJson.ToSharedRef(), Writer);

//                 const UTextureDiffusion3DSettings* Settings = GetDefault<UTextureDiffusion3DSettings>();
//                 FString RequestURL = Settings->ComfyUIServerAddress + TEXT("prompt");

//                 TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = FHttpModule::Get().CreateRequest();
//                 HttpRequest->SetURL(RequestURL);
//                 HttpRequest->SetVerb(TEXT("POST"));
//                 HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
//                 HttpRequest->SetContentAsString(PayloadString);
                
//                 HttpRequest->OnProcessRequestComplete().BindLambda(
//                     [this](FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
//                     {
//                         if (bWasSuccessful && Response.IsValid() && Response->GetResponseCode() == 200)
//                         {
//                             TSharedPtr<FJsonObject> JsonObject;
//                             TSharedRef<TJsonReader<>> JsonReader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
//                             if (FJsonSerializer::Deserialize(JsonReader, JsonObject) && JsonObject->HasField(TEXT("prompt_id")))
//                             {
//                                 this->CurrentPromptId = JsonObject->GetStringField(TEXT("prompt_id"));
//                                 UE_LOG(LogTemp, Log, TEXT("Workflow submitted successfully. Got prompt_id: %s. Starting to poll for %d outputs..."), *this->CurrentPromptId, this->ExpectedOutputs.Num());
                                
//                                 this->PollingTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
//                                     FTickerDelegate::CreateRaw(this, &FTextureDiffusion3D::PollHistoryEndpoint), 2.0f);
//                             }
//                         }
//                         else
//                         {
//                             FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("❌ Workflow submission failed. Check logs.")));
//                         }
//                     });
//                 HttpRequest->ProcessRequest();
//             }
//         });

//     UploadImageToComfyUI(PngData, Filename, OnUploadCompleteDelegate);
}
 
void FTextureDiffusion3D::ProjectTextureButtonClicked()
{
	UE_LOG(LogTemp, Log, TEXT("--- 'Project Texture' button clicked ---"));

	// 1. Get Actor and Validate
	TObjectPtr<AStaticMeshActor> NewSelectedActor = GetSelectedActor();
	if (!NewSelectedActor)
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("Please select a single Static Mesh Actor in the world."));
		return;
	}

	// 2. Handle Actor Switching
	if (SelectedActor != NewSelectedActor)
	{
		// If we are switching from a previously selected actor, confirm with the user.
		if (SelectedActor) 
		{
			const EAppReturnType::Type Result = FMessageDialog::Open(
				EAppMsgType::YesNo,
				FText::Format(
					LOCTEXT("SwitchActorWarning", "You are switching from '{0}' to '{1}'.\n\nThis will reset all existing projection data. Are you sure?"),
					FText::FromString(SelectedActor->GetActorLabel()),
					FText::FromString(NewSelectedActor->GetActorLabel())
				)
			);
			
			if (Result == EAppReturnType::No) { return; }
			
			// This will restore the old actor's materials before we switch.
			ResetProjection(); 
		}

		// Set the new actor
		SelectedActor = NewSelectedActor;

		// Now, populate the original materials map for the NEWLY selected actor.
		OriginalActorMaterials.Empty();
		if (UStaticMeshComponent* MeshComp = SelectedActor->GetStaticMeshComponent())
		{
			for (int32 i = 0; i < MeshComp->GetNumMaterials(); ++i)
			{
				OriginalActorMaterials.Add(i, MeshComp->GetMaterial(i));
			}
			UE_LOG(LogTemp, Log, TEXT("Stored %d original materials for %s."), OriginalActorMaterials.Num(), *SelectedActor->GetName());
		}
	}
	
	
	// SelectedActor = NewSelectedActor;
	UE_LOG(LogTemp, Log, TEXT("Target actor set to: %s"), *SelectedActor->GetName());


	
	// Get the camera list for the current material slot (defaulting to 0)
	const int32 CurrentSlotIndex = CurrentSettings.TargetMaterialSlotIndex;
	TArray<FProjectionSettings>& SlotCameraSettings = PerSlotCameraSettings.FindOrAdd(CurrentSlotIndex);

	// 3. Initialize Projection Data if Necessary for THIS slot
	if (SlotCameraSettings.Num() == 0)
	{
		AddCameraTab();
	}

	// 4. Safely Get the Editor World
	UWorld* World = nullptr;
	if (GEditor)
	{
		if (FWorldContext* EditorContext = &GEditor->GetEditorWorldContext())
		{
			World = EditorContext->World();
		}
	}

	if (!World)
	{
		UE_LOG(LogTemp, Error, TEXT("Could not get a valid editor world. Aborting."));
		return;
	}

	// 5. Ensure the Scene Capture Actor is Ready
	if (!IsValid(CaptureActor))
	{
		CaptureActor = FCameraHelper::SetupSceneCaptureComponent(World);
		if (USceneCaptureComponent2D* CaptureComp = CaptureActor->GetCaptureComponent2D())
		{
			CaptureComp->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
		}
	}
	if (USceneCaptureComponent2D* CaptureComp = CaptureActor->GetCaptureComponent2D())
	{
		CaptureComp->ShowOnlyActors.Empty();
		CaptureComp->ShowOnlyActors.Add(SelectedActor);
	}

	// 6. Create and Configure the Settings Window
	if (SettingsWindow.IsValid())
	{
		SettingsWindow->RequestDestroyWindow();
	}
	
	SAssignNew(SettingsWindow, SWindow)
		.Title(FText::FromString("Texture Projection Settings"))
		.SizingRule(ESizingRule::UserSized)
		.ClientSize(FVector2D(600, 900));

	SAssignNew(SettingsWidget, STextureProjectionWindow)
		.TargetActor(SelectedActor)
		.ParentWindow(SettingsWindow)
		.OnProjectionConfirmed(FSimpleDelegate::CreateRaw(this, &FTextureDiffusion3D::ExecuteAllCameraProjections)) 
	
		.OnDynamicProjectionConfirmed_Lambda([this](const FString& WorkflowPath, const FNodeInfoArray& ParsedNodes, const FControlValueMap& ControlValues, const FSeedStateMap& SeedStates, const FProjectionSettings& CurrentCamSettings) {

		ExecuteDynamicProjection(SelectedActor, WorkflowPath, CurrentCamSettings, ParsedNodes, ControlValues, SeedStates);
	})
		.OnSaveFinalTexture(FOnSaveFinalTexture::CreateRaw(this, &FTextureDiffusion3D::SaveFinalTextureAsAsset))
		.OnBlendSettingsCommitted(FSimpleDelegate::CreateRaw(this, &FTextureDiffusion3D::ReblendAndUpdateMesh))
		.OnBlendParametersChanged(FOnBlendParametersChangedDelegate::CreateRaw(this, &FTextureDiffusion3D::RecalculateAllWeightsAndReblend))
		.OnGetPreviewTexture(FOnGetPreviewTexture::CreateRaw(this, &FTextureDiffusion3D::GetPreviewTextureForKeyword))
		.OnOccluderStateChanged(FOnOccluderStateChanged::CreateRaw(this, &FTextureDiffusion3D::OnOccluderCheckboxChanged))
		.OnGenerateRigClicked(FSimpleDelegate::CreateRaw(this, &FTextureDiffusion3D::GenerateOptimalCameraRig))
		.OnGetBaseColorForSlot(FOnGetBaseColorForSlot::CreateRaw(this, &FTextureDiffusion3D::GetBaseColorForSlot))
		.OnBaseColorChanged(FOnBaseColorChanged::CreateRaw(this, &FTextureDiffusion3D::SetBaseColorForSlot))
		.OnActiveVariantChanged(FOnActiveVariantChanged::CreateRaw(this, &FTextureDiffusion3D::SetActiveVariant))
		.OnGetActiveVariantState(FOnGetActiveVariantState::CreateRaw(this, &FTextureDiffusion3D::GetActiveVariantState));
	SettingsWidget->SetOnOutputResolutionChanged(
	FOnOutputResolutionChanged::CreateRaw(this, &FTextureDiffusion3D::OnOutputResolutionChanged_Handler)
);


	SettingsWidget->SetOnUVChannelChanged(FOnUVChannelChanged::CreateRaw(this, &FTextureDiffusion3D::OnTargetUVChannelChanged));
	if (UStaticMesh* Mesh = SelectedActor->GetStaticMeshComponent()->GetStaticMesh())
	{
		SettingsWidget->UpdateUVChannels(Mesh->GetNumUVChannels(0), TargetUVChannel);
	}
	

		
	// 7. Connect All Delegates
	SettingsWidget->OnReset = FSimpleDelegate::CreateRaw(this, &FTextureDiffusion3D::ResetProjection);
	SettingsWidget->SetCaptureActor(CaptureActor);
	// SettingsWidget->SetOnSingleProjectionConfirmed(FSimpleDelegate::CreateRaw(this, &FTextureDiffusion3D::ExecuteActiveCameraProjection_FromUI));
	// SettingsWidget->SetOnProjectionConfirmed(FSimpleDelegate::CreateRaw(this, &FTextureDiffusion3D::ExecuteAllCameraProjections, SelectedActor));
	SettingsWidget->SetOnTabAdded(FOnTabAdded::CreateRaw(this, &FTextureDiffusion3D::OnCameraTabAdded));
	SettingsWidget->SetOnTabRemoved(FOnTabRemoved::CreateRaw(this, &FTextureDiffusion3D::OnCameraTabRemoved));
	SettingsWidget->SetOnTabSelected(FOnTabSelected::CreateRaw(this, &FTextureDiffusion3D::SelectCameraTab));
	SettingsWidget->OnMaterialSlotChanged = FOnMaterialSlotChanged::CreateRaw(this, &FTextureDiffusion3D::OnTargetMaterialSlotChanged);

	
	SettingsWidget->SetOnSettingsChanged([this](const FProjectionSettings& NewSettings)
	{
		OnProjectionSettingsChanged(NewSettings);
	});

	if (bHasStartedProjections)
	{
		SettingsWidget->SetGlobalSettingsLock(true);
	}
	
	// 8. Link Data Source and Show Window
	SettingsWidget->SetCameraSettings(
		SlotCameraSettings, 
		ActiveCameraIndex, 
		Global_OutputPath, 
		Global_OutputTextureWidth, 
		Global_OutputTextureHeight);
	SettingsWindow->SetContent(SettingsWidget.ToSharedRef());
	FSlateApplication::Get().AddWindow(SettingsWindow.ToSharedRef());
	
	if (CaptureActor && SettingsWidget.IsValid())
	{
		FCameraHelper::PositionCaptureCamera(CaptureActor, SelectedActor, CurrentSettings);
	}
}





TObjectPtr<AStaticMeshActor> FTextureDiffusion3D::GetSelectedActor()
{
	if (!GEditor)
	{
		return nullptr;
	}

	// Retrieve selected objects
	TArray<UObject*> SelectedObjects;
	GEditor->GetSelectedActors()->GetSelectedObjects(SelectedObjects);

	for (UObject* Obj : SelectedObjects)
	{
		if (TObjectPtr<AStaticMeshActor> SMActor = Cast<AStaticMeshActor>(Obj))
		{
			return SMActor; // Return the first selected static mesh actor
		}
	}

	return nullptr; // No static mesh actor found in selection
}

void FTextureDiffusion3D::OnProjectionSettingsChanged(const FProjectionSettings& NewSettings)
{
	// Get the camera list for the active material slot
	TArray<FProjectionSettings>& SlotCameraSettings = PerSlotCameraSettings.FindOrAdd(NewSettings.TargetMaterialSlotIndex);

	if (!SlotCameraSettings.IsValidIndex(ActiveCameraIndex)) return;

	// Update the settings for the active camera within the correct slot's array
	SlotCameraSettings[ActiveCameraIndex] = NewSettings;
	CurrentSettings = NewSettings;

	// The rest of the logic remains the same
	if (CaptureActor && SelectedActor)
	{
		FCameraHelper::PositionCaptureCamera(CaptureActor, SelectedActor, CurrentSettings);
		if (USceneCaptureComponent2D* CaptureComp = CaptureActor->GetCaptureComponent2D())
		{
			CaptureComp->CaptureScene();
		}
	}
}



void FTextureDiffusion3D::CaptureMeshDepth(
	TObjectPtr<AStaticMeshActor> StaticMeshActor,
		const FProjectionSettings& Settings,
		int32 Width ,
		int32 Height ,
		TArray<float>& OutDepthBuffer,
		const FString& InOverrideExportDirectory,
		const FString& InOverrideBaseFileName
	)
{
	// Create a render target specifically for depth
	UTextureRenderTarget2D* DepthRT = NewObject<UTextureRenderTarget2D>();
	// Use a floating-point format for accurate depth values
	//always use 1024 for consistency
	DepthRT->InitCustomFormat(1024, 1024, PF_FloatRGBA, false);
	DepthRT->UpdateResource();

	//always use 102
	
	// Set up a scene capture
	UWorld* World = GEditor->GetEditorWorldContext().World();
	ASceneCapture2D* TempCaptureActor = World->SpawnActor<ASceneCapture2D>();
	USceneCaptureComponent2D* CaptureComp = TempCaptureActor->GetCaptureComponent2D();
	
	// Position the camera
	TempCaptureActor->SetActorLocation(Settings.CameraPosition);
	TempCaptureActor->SetActorRotation(Settings.CameraRotation);
	// Get the mesh bounds center
	// FBoxSphereBounds Bounds = StaticMeshActor->GetComponentsBoundingBox();
	// FVector TargetCenter = Bounds.Origin;
	
	// Calculate look-at direction
	// FVector LookDirection = (TargetCenter - Settings.CameraPosition).GetSafeNormal();
	// TempCaptureActor->SetActorRotation(LookDirection.Rotation());
	
	// Configure the capture for depth
	CaptureComp->TextureTarget = DepthRT;
	CaptureComp->FOVAngle = Settings.FOVAngle;
	CaptureComp->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
	CaptureComp->ShowOnlyActors.Add(StaticMeshActor);
	CaptureComp->CaptureSource = ESceneCaptureSource::SCS_SceneDepth;
	
	// Capture the scene
	CaptureComp->CaptureScene();
	
	// Read back the result as floating point values
	TArray<FFloat16Color> DepthData;
	FTextureRenderTargetResource* RTResource = DepthRT->GameThread_GetRenderTargetResource();
	RTResource->ReadFloat16Pixels(DepthData);
	
	// Convert to floating point depth buffer
	OutDepthBuffer.SetNum(DepthData.Num());
	for (int32 i = 0; i < DepthData.Num(); i++)
	{
		OutDepthBuffer[i] = DepthData[i].R; // The depth is usually stored in the R channel
	}
	
	// After you've captured the scene
	CaptureComp->CaptureScene();
	FString ExportDir;
	FString BaseFileNameToUse;

	if (!InOverrideExportDirectory.IsEmpty() && !InOverrideBaseFileName.IsEmpty())
	{
		ExportDir = InOverrideExportDirectory;
		BaseFileNameToUse = InOverrideBaseFileName;

		// Ensure the ComfyUI input directory exists
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		if (!PlatformFile.DirectoryExists(*ExportDir))
		{
			if (!PlatformFile.CreateDirectoryTree(*ExportDir))
			{
				UE_LOG(LogTemp, Error, TEXT("CaptureMeshDepth: Failed to create custom export directory: %s"), *ExportDir);
				// Clean up TempCaptureActor before returning on error
				if (TempCaptureActor) World->DestroyActor(TempCaptureActor);
				return;
			}
			UE_LOG(LogTemp, Log, TEXT("CaptureMeshDepth: Created directory: %s"), *ExportDir);
		}
	}
	else
	{
		// Fallback to your original default export logic
		ExportDir = FPaths::ProjectSavedDir() + TEXT("TextureExports");

		FString DepthSuffix = FString::Printf(TEXT("_Depth_Camera%d"), ActiveCameraIndex);
		BaseFileNameToUse = StaticMeshActor->GetName() + DepthSuffix;
	}

	UE_LOG(LogTemp, Log, TEXT("CaptureMeshDepth: Exporting depth to Dir: %s, File: %s.png"), *ExportDir, *BaseFileNameToUse);

	// Your existing call to NormalizeAndExportRenderTarget
	bool bExportedSuccessfully = FTextureUtils::NormalizeAndExportRenderTarget(
		World,
		DepthRT,			  // Your floating-point depth render target
		ExportDir,
		BaseFileNameToUse,	// No .png needed here if your util adds it
		true				  // Assuming true means normalize for depth
	);

	if (bExportedSuccessfully)
	{
		LastExportedDepthPath = FPaths::Combine(ExportDir, BaseFileNameToUse + TEXT(".png")); // Store the full path
		UE_LOG(LogTemp, Log, TEXT("CaptureMeshDepth: Successfully exported depth map to %s"), *LastExportedDepthPath);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("CaptureMeshDepth: Failed to export depth map to %s/%s.png"), *ExportDir, *BaseFileNameToUse);
	}

	// Clean up
	if (TempCaptureActor)  	
	{
		World->DestroyActor(TempCaptureActor);
		TempCaptureActor = nullptr;
	}

	if (CaptureComp)
	{
		CaptureComp->TextureTarget = nullptr; // NEW: break reference before GC
	}

	if (DepthRT)
	{
		DepthRT->MarkAsGarbage();				// NEW
		DepthRT = nullptr;						// optional, but nice
	}

	// Clean up capture actor
	if (TempCaptureActor)
	{
		World->DestroyActor(TempCaptureActor);
		TempCaptureActor = nullptr;
	}
}


void FTextureDiffusion3D::CalculateCameraWeights(
    UStaticMesh* StaticMesh,
    const FTransform& MeshTransform,
    const FProjectionSettings& Settings, // Use the original Settings directly
    int32 OutputWidth,
    int32 OutputHeight,
    TArray<float>& OutNormalWeightBuffer,
    TArray<FVector2D>& OutScreenPositionBuffer,
    TArray<bool>& OutVisibilityBuffer,
    int32 UVChannel,
    const TSet<int32>& HiddenSlotIndices)
{
    int32 TextureWidth = OutputWidth;
    int32 TextureHeight = OutputHeight;
    int32 NumTexels = TextureWidth * TextureHeight;
    const int32 TargetSlot = Settings.TargetMaterialSlotIndex;

    // --- Step 1: DATA PREPARATION (Unchanged) ---
    if (PrecomputedWorldPositionBuffer.Num() != NumTexels || TBNDataForSlotIndex != TargetSlot)
    {
        PrecomputedWorldPositionBuffer.Init(FVector::ZeroVector, NumTexels);
        PrecomputedUVBuffer.Init(FVector2D::ZeroVector, NumTexels);
        FMeshProcessor::InitializeStaticTexelBuffers(StaticMesh, MeshTransform, Settings,
            TextureWidth, TextureHeight, PrecomputedUVBuffer, PrecomputedWorldPositionBuffer, UVChannel);
        UE_LOG(LogTemp, Log, TEXT("Generated static buffers for texel world positions."));

        UE_LOG(LogTemp, Log, TEXT("Generating static per-texel TBN frame for material slot %d..."), TargetSlot);
        FMeshProcessor::BuildPerTexelTBN_FromMesh(
            StaticMesh, MeshTransform, UVChannel, TargetSlot, TextureWidth, TextureHeight,
            PrecomputedTangent_World, PrecomputedBitangent_World, PrecomputedNormal_World);
        TBNDataForSlotIndex = TargetSlot;
    }

    // Dynamic buffers (Unchanged)
    TArray<float> DepthBuffer;
    FMeshProcessor::InitializeDynamicTexelBuffers(
        StaticMesh, MeshTransform, Settings,
        OutputWidth, OutputHeight, UVChannel, HiddenSlotIndices,
        /*out*/ PrecomputedWorldPositionBuffer,
        /*out*/ OutScreenPositionBuffer,
        /*out*/ DepthBuffer);

    // --- Step 2: PASS 1 - NORMAL & FRUSTUM CULLING ---
    // Calculate weights based on angle and screen position using the processor function.
    // The core logic for binary vs. soft fade is now *inside* FMeshProcessor::CalculateNormalWeights.
    OutVisibilityBuffer.Init(true, NumTexels); // Initialize visibility, FMeshProcessor might change it
    OutNormalWeightBuffer.Init(0.0f, NumTexels);

    FMeshProcessor::CalculateNormalWeights(
        StaticMesh, MeshTransform, Settings.CameraPosition, Settings, // <-- Use original Settings
        OutputWidth, OutputHeight,
        /*World pos*/ PrecomputedWorldPositionBuffer,
        /*Screen*/	OutScreenPositionBuffer,
        /*OUT vis*/    OutVisibilityBuffer,
        /*OUT weight*/OutNormalWeightBuffer,
        nullptr, UVChannel, HiddenSlotIndices);

    // --- Step 3: PASS 2 - SELF-OCCLUSION CULLING (Unchanged) ---
    // Perform ray cast occlusion on the results from Pass 1.
    FMeshProcessor::PerformRayCastOcclusion(
        StaticMesh, MeshTransform, Settings,
        OutVisibilityBuffer, PrecomputedWorldPositionBuffer,
        DepthBuffer, OutNormalWeightBuffer, HiddenSlotIndices);

    // --- Step 4: REMOVED ---
    // The conditional binarization loop is no longer needed here.
    UE_LOG(LogTemp, Log, TEXT("CalculateCameraWeights completed. Weight calculation handled by FMeshProcessor."));
}

// This function's input and output are now TArray<FLinearColor>
TArray<FLinearColor> FTextureDiffusion3D::CreateBlendedTexture(
	const TArray<TArray<FLinearColor>>& CameraProjections_Linear, 
	const TArray<TArray<float>>& ProcessedWeights,
	int32 TextureWidth,
	int32 TextureHeight)
{
	const int32 NumTexels = TextureWidth * TextureHeight;
	TArray<FLinearColor> BlendedProjection_Linear; // <--- TYPE CHANGED
	BlendedProjection_Linear.Init(FLinearColor::Transparent, NumTexels);

	if (CameraProjections_Linear.Num() == 0 || ProcessedWeights.Num() != NumTexels)
	{
		return BlendedProjection_Linear;
	}

	for (int32 TexelIndex = 0; TexelIndex < NumTexels; TexelIndex++)
	{
		FLinearColor SummedColor(0, 0, 0, 0);
		float TotalFinalWeight = 0.0f;

		for (int32 CamIndex = 0; CamIndex < CameraProjections_Linear.Num(); CamIndex++)
		{
			const float BlendWeight = ProcessedWeights[TexelIndex][CamIndex];
			if (BlendWeight > KINDA_SMALL_NUMBER)
			{
				const FLinearColor& PixelColor_Linear = CameraProjections_Linear[CamIndex][TexelIndex];
				const float FinalWeight = BlendWeight * PixelColor_Linear.A;

				if (FinalWeight > KINDA_SMALL_NUMBER)
				{
					// All math is now correctly done on Linear values
					SummedColor.R += PixelColor_Linear.R * FinalWeight;
					SummedColor.G += PixelColor_Linear.G * FinalWeight;
					SummedColor.B += PixelColor_Linear.B * FinalWeight;
					TotalFinalWeight += FinalWeight;
				}
			}
		}

		if (TotalFinalWeight > KINDA_SMALL_NUMBER)
		{
			const float InvTotalWeight = 1.0f / TotalFinalWeight;
			FLinearColor FinalColor(
				SummedColor.R * InvTotalWeight,
				SummedColor.G * InvTotalWeight,
				SummedColor.B * InvTotalWeight,
				FMath::Min(TotalFinalWeight, 1.0f)
			);
			BlendedProjection_Linear[TexelIndex] = FinalColor;
		}
	}
	return BlendedProjection_Linear;
}




// Encodes a normalized FVector (-1 to 1) into an FColor (0 to 255).
static FORCEINLINE FColor EncodeUnitVectorToRGB_Debug(const FVector& InVector, bool bNormalize = true)
{
    const FVector V = bNormalize ? InVector.GetSafeNormal() : InVector;
    FColor Out;
    Out.R = (uint8)FMath::Clamp(FMath::RoundToInt((V.X * 0.5f + 0.5f) * 255.0f), 0, 255);
    Out.G = (uint8)FMath::Clamp(FMath::RoundToInt((V.Y * 0.5f + 0.5f) * 255.0f), 0, 255);
    Out.B = (uint8)FMath::Clamp(FMath::RoundToInt((V.Z * 0.5f + 0.5f) * 255.0f), 0, 255);
    Out.A = 255;
    return Out;
}


static FORCEINLINE FQuat SafeFindBetween(const FVector& From, const FVector& To)
{
    FVector A = From.GetSafeNormal();
    FVector B = To.GetSafeNormal();

    float d = FVector::DotProduct(A, B);
    d = FMath::Clamp(d, -1.0f, 1.0f);

    if (d > 1.0f - 1e-6f) // almost the same
    {
        return FQuat::Identity;
    }
    else if (d < -1.0f + 1e-6f) // opposite
    {
        // pick an arbitrary orthogonal axis
        FVector Axis = FVector::CrossProduct(A, FVector::UpVector);
        if (Axis.SizeSquared() < 1e-8f)
            Axis = FVector::CrossProduct(A, FVector::RightVector);
        Axis.Normalize();
        return FQuat(Axis, PI);
    }
    else
    {
        FVector Axis = FVector::CrossProduct(A, B);
        const float s = FMath::Sqrt((1 + d) * 2);
        const float invs = 1.0f / s;
        return FQuat(Axis.X * invs, Axis.Y * invs, Axis.Z * invs, s * 0.5f).GetNormalized();
    }
}

// High Precision (FLinearColor) Version
static void SeparableBoxBlurNormals_Tangent(
    const TArray<FLinearColor>& InPixels, int32 W, int32 H,
    const TArray<float>& Mask01, int32 Radius,
    TArray<FLinearColor>& OutPixels) // Changed to FLinearColor
{
    check(InPixels.Num() == W * H);
    check(Mask01.Num()  == W * H);

    // Helper to unpack 0..1 float to -1..1 vector
    auto UnpackLin = [](const FLinearColor& C) {
        return FVector(C.R * 2.f - 1.f, C.G * 2.f - 1.f, C.B * 2.f - 1.f);
    };

    Radius = FMath::Clamp(Radius, 0, 64);
    if (Radius == 0)
    {
        OutPixels = InPixels;
        return;
    }

    struct FAccum { FVector Sum = FVector::ZeroVector; float Wsum = 0; };

    TArray<FVector> Tmp;  Tmp.SetNumUninitialized(W * H);
    TArray<FLinearColor> Out; Out.SetNumUninitialized(W * H); // Local buffer is now Linear

    // Horizontal pass
    for (int32 y = 0; y < H; ++y)
    {
        FAccum acc;
        auto idx = [&](int32 x) { return y * W + x; };

        // prime
        for (int32 k = -Radius; k <= Radius; ++k)
        {
            int32 x = FMath::Clamp(k, 0, W - 1);
            const int32 i = idx(x);
            const float w = Mask01[i];
            if (w > 0.f)
            {
                // Use UnpackLin here
                acc.Sum += UnpackLin(InPixels[i]) * w; 
                acc.Wsum += w;
            }
        }

        for (int32 x = 0; x < W; ++x)
        {
            FVector N = (acc.Wsum > 1e-6f) ? (acc.Sum / acc.Wsum).GetSafeNormal() : FVector(0,0,1);
            Tmp[idx(x)] = N;

            int32 x_rem = FMath::Clamp(x - Radius, 0, W - 1);
            int32 x_add = FMath::Clamp(x + Radius + 1, 0, W - 1);

            const int32 irem = idx(x_rem);
            const int32 iadd = idx(x_add);
            const float wrem = Mask01[irem];
            const float wadd = Mask01[iadd];

            if (wrem > 0.f) { acc.Sum -= UnpackLin(InPixels[irem]) * wrem; acc.Wsum -= wrem; }
            if (wadd > 0.f) { acc.Sum += UnpackLin(InPixels[iadd]) * wadd; acc.Wsum += wadd; }
        }
    }

    // Vertical pass
    for (int32 x = 0; x < W; ++x)
    {
        FAccum acc;
        auto idx = [&](int32 y) { return y * W + x; };

        // prime
        for (int32 k = -Radius; k <= Radius; ++k)
        {
            int32 y = FMath::Clamp(k, 0, H - 1);
            const int32 i = idx(y);
            const float w = Mask01[i];
            if (w > 0.f)
            {
                acc.Sum += Tmp[i] * w;
                acc.Wsum += w;
            }
        }

        for (int32 y = 0; y < H; ++y)
        {
            FVector N = (acc.Wsum > 1e-6f) ? (acc.Sum / acc.Wsum).GetSafeNormal() : FVector(0,0,1);
            
            // Repack to 0..1 Float
            Out[idx(y)] = FLinearColor(
                (N.X + 1.f) * 0.5f,
                (N.Y + 1.f) * 0.5f,
                (N.Z + 1.f) * 0.5f,
                1.0f);
            
            int32 y_rem = FMath::Clamp(y - Radius, 0, H - 1);
            int32 y_add = FMath::Clamp(y + Radius + 1, 0, H - 1);

            const int32 irem = idx(y_rem);
            const int32 iadd = idx(y_add);
            const float wrem = Mask01[irem];
            const float wadd = Mask01[iadd];

            if (wrem > 0.f) { acc.Sum -= Tmp[irem] * wrem; acc.Wsum -= wrem; }
            if (wadd > 0.f) { acc.Sum += Tmp[iadd] * wadd; acc.Wsum += wadd; }
        }
    }

    OutPixels = MoveTemp(Out);
}






bool FTextureDiffusion3D::ProjectNormalDetails(
    UTexture2D* InViewSpaceNormalTex,
    const TArray<bool>& VisibilityBuffer,
    const TArray<FVector2D>& ScreenPositionBuffer,
    const TArray<float>& NormalWeightBuffer,
    const FProjectionSettings& Settings,
    int32 TextureWidth,
    int32 TextureHeight,
    TArray<FLinearColor>& OutTangentSpaceNormals)
{
    // =========================================================================
    // CONFIGURATION
    // =========================================================================
    constexpr float DetailGain = 1.8f;
    constexpr float MaxAngleDegrees = 80.0f;
    const int32 HighPassFilterRadius = 8;
    
    // NOTE: Keep this true if your source is DirectX style (Green = Down), 
    // false if OpenGL (Green = Up). Unreal usually expects Green = Down ( -Y ).
    constexpr bool bFlipGreenChannel = true; 

    const float MaxAngleRadians = FMath::DegreesToRadians(MaxAngleDegrees);
    const int32 NumTexels = TextureWidth * TextureHeight;

    UE_LOG(LogTemp, Warning, TEXT("--- ProjectNormalDetails START --- Slot %d, Camera %d ---"), 
        CurrentProjection_Settings.TargetMaterialSlotIndex, CurrentProjection_CameraIndex);

    // =========================================================================
    // STEP 1: VALIDATION
    // =========================================================================
    if (!IsValid(InViewSpaceNormalTex))
    {
        UE_LOG(LogTemp, Error, TEXT("ProjectNormalDetails: Source texture is null"));
        return false;
    }
    // Check all required buffers including TBN
    if (VisibilityBuffer.Num() != NumTexels ||
        ScreenPositionBuffer.Num() != NumTexels ||
        NormalWeightBuffer.Num() != NumTexels ||
        PrecomputedNormal_World.Num() != NumTexels ||
        PrecomputedTangent_World.Num() != NumTexels ||
        PrecomputedBitangent_World.Num() != NumTexels)
    {
        UE_LOG(LogTemp, Error, TEXT("ProjectNormalDetails: Buffer size mismatch."));
        return false;
    }

    // =========================================================================
    // DEBUG: SAVE INPUT SOURCE TEXTURE
    // =========================================================================
    UE_LOG(LogTemp, Warning, TEXT("ProjectNormalDetails (Debug): Saving input source texture..."));
    Debug_SaveTextureAsAsset(InViewSpaceNormalTex, TEXT("Debug_NormalInput_SourceTexture"));

    // =========================================================================
    // DEBUG: SAVE CPU WEIGHT MAP (same as BakeWeightedLayerGPU)
    // =========================================================================
    {
        UE_LOG(LogTemp, Warning, TEXT("ProjectNormalDetails (Debug): Saving NormalWeightBuffer as HDR asset..."));

        const FString BasePath = TEXT("/Game/TD3D_Debug/");
        const FString AssetName = FString::Printf(TEXT("Debug_NormalInput_WeightMap_Slot%d_Cam%d_%s"),
            CurrentProjection_Settings.TargetMaterialSlotIndex,
            CurrentProjection_CameraIndex,
            *FDateTime::Now().ToString(TEXT("HHMMSS"))
        );
        const FString PackagePath = BasePath + AssetName;

        UPackage* Package = CreatePackage(*PackagePath);
        if (Package)
        {
            Package->FullyLoad();
            UTexture2D* NewStaticTexture = NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone);
            if (NewStaticTexture)
            {
                NewStaticTexture->AddToRoot();
                NewStaticTexture->CompressionSettings = TC_HDR_F32;
                NewStaticTexture->SRGB = false;
                NewStaticTexture->Source.Init(TextureWidth, TextureHeight, 1, 1, TSF_RGBA16F);
                
                TArray<FFloat16Color> HdrPixels;
                HdrPixels.SetNumUninitialized(NumTexels);
                for (int32 i = 0; i < NumTexels; ++i)
                {
                    const float Weight = VisibilityBuffer[i] ? NormalWeightBuffer[i] : 0.0f;
                    HdrPixels[i] = FFloat16Color(FLinearColor(Weight, Weight, Weight, 1.0f));
                }

                void* DestPixels = NewStaticTexture->Source.LockMip(0);
                if (DestPixels)
                {
                    FMemory::Memcpy(DestPixels, HdrPixels.GetData(), HdrPixels.Num() * sizeof(FFloat16Color));
                    NewStaticTexture->Source.UnlockMip(0);
                    
                    NewStaticTexture->UpdateResource();
                    Package->MarkPackageDirty();
                    FAssetRegistryModule::AssetCreated(NewStaticTexture);

                    const FString PackageFileName = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
                    FSavePackageArgs SaveArgs;
                    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
                    
                    if (UPackage::SavePackage(Package, NewStaticTexture, *PackageFileName, SaveArgs))
                    {
                        UE_LOG(LogTemp, Warning, TEXT("--- DEBUG: Saved Normal Input WEIGHT MAP to: %s ---"), *PackagePath);
                    }
                }
                NewStaticTexture->RemoveFromRoot();
            }
        }
    }

    // =========================================================================
    // DEBUG: SAVE VISIBILITY BUFFER
    // =========================================================================
    {
        UE_LOG(LogTemp, Warning, TEXT("ProjectNormalDetails (Debug): Saving VisibilityBuffer..."));

        const FString BasePath = TEXT("/Game/TD3D_Debug/");
        const FString AssetName = FString::Printf(TEXT("Debug_NormalInput_VisibilityMask_Slot%d_Cam%d_%s"),
            CurrentProjection_Settings.TargetMaterialSlotIndex,
            CurrentProjection_CameraIndex,
            *FDateTime::Now().ToString(TEXT("HHMMSS"))
        );
        const FString PackagePath = BasePath + AssetName;

        UPackage* Package = CreatePackage(*PackagePath);
        if (Package)
        {
            Package->FullyLoad();
            UTexture2D* NewStaticTexture = NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone);
            if (NewStaticTexture)
            {
                NewStaticTexture->AddToRoot();
                NewStaticTexture->CompressionSettings = TC_Default;
                NewStaticTexture->SRGB = false;
                NewStaticTexture->Source.Init(TextureWidth, TextureHeight, 1, 1, TSF_BGRA8);
                
                TArray<FColor> Pixels;
                Pixels.SetNumUninitialized(NumTexels);
                for (int32 i = 0; i < NumTexels; ++i)
                {
                    const uint8 Val = VisibilityBuffer[i] ? 255 : 0;
                    Pixels[i] = FColor(Val, Val, Val, 255);
                }

                void* DestPixels = NewStaticTexture->Source.LockMip(0);
                if (DestPixels)
                {
                    FMemory::Memcpy(DestPixels, Pixels.GetData(), Pixels.Num() * sizeof(FColor));
                    NewStaticTexture->Source.UnlockMip(0);
                    
                    NewStaticTexture->UpdateResource();
                    Package->MarkPackageDirty();
                    FAssetRegistryModule::AssetCreated(NewStaticTexture);

                    const FString PackageFileName = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
                    FSavePackageArgs SaveArgs;
                    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
                    UPackage::SavePackage(Package, NewStaticTexture, *PackageFileName, SaveArgs);
                    UE_LOG(LogTemp, Warning, TEXT("--- DEBUG: Saved Normal Input VISIBILITY MASK to: %s ---"), *PackagePath);
                }
                NewStaticTexture->RemoveFromRoot();
            }
        }
    }

    // =========================================================================
    // DEBUG: SAVE GEOMETRIC NORMAL BUFFER (PrecomputedNormal_World)
    // =========================================================================
    {
        UE_LOG(LogTemp, Warning, TEXT("ProjectNormalDetails (Debug): Saving PrecomputedNormal_World (Geometric Normals)..."));

        const FString BasePath = TEXT("/Game/TD3D_Debug/");
        const FString AssetName = FString::Printf(TEXT("Debug_NormalInput_GeometricNormals_Slot%d_Cam%d_%s"),
            CurrentProjection_Settings.TargetMaterialSlotIndex,
            CurrentProjection_CameraIndex,
            *FDateTime::Now().ToString(TEXT("HHMMSS"))
        );
        const FString PackagePath = BasePath + AssetName;

        UPackage* Package = CreatePackage(*PackagePath);
        if (Package)
        {
            Package->FullyLoad();
            UTexture2D* NewStaticTexture = NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone);
            if (NewStaticTexture)
            {
                NewStaticTexture->AddToRoot();
                NewStaticTexture->CompressionSettings = TC_Normalmap;
                NewStaticTexture->SRGB = false;
                NewStaticTexture->Source.Init(TextureWidth, TextureHeight, 1, 1, TSF_RGBA16F);
                
                TArray<FFloat16Color> HdrPixels;
                HdrPixels.SetNumUninitialized(NumTexels);
                for (int32 i = 0; i < NumTexels; ++i)
                {
                    const FVector& N = PrecomputedNormal_World[i];
                    // Encode world normal as 0-1 range
                    HdrPixels[i] = FFloat16Color(FLinearColor(
                        N.X * 0.5f + 0.5f,
                        N.Y * 0.5f + 0.5f,
                        N.Z * 0.5f + 0.5f,
                        1.0f
                    ));
                }

                void* DestPixels = NewStaticTexture->Source.LockMip(0);
                if (DestPixels)
                {
                    FMemory::Memcpy(DestPixels, HdrPixels.GetData(), HdrPixels.Num() * sizeof(FFloat16Color));
                    NewStaticTexture->Source.UnlockMip(0);
                    
                    NewStaticTexture->UpdateResource();
                    Package->MarkPackageDirty();
                    FAssetRegistryModule::AssetCreated(NewStaticTexture);

                    const FString PackageFileName = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
                    FSavePackageArgs SaveArgs;
                    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
                    UPackage::SavePackage(Package, NewStaticTexture, *PackageFileName, SaveArgs);
                    UE_LOG(LogTemp, Warning, TEXT("--- DEBUG: Saved GEOMETRIC NORMALS to: %s ---"), *PackagePath);
                }
                NewStaticTexture->RemoveFromRoot();
            }
        }
    }

    // =========================================================================
    // STEP 2: READ SOURCE TEXTURE
    // =========================================================================
    const int32 SrcWidth = InViewSpaceNormalTex->GetSizeX();
    const int32 SrcHeight = InViewSpaceNormalTex->GetSizeY();
    
    TArray<FLinearColor> SourcePixels;
    SourcePixels.SetNumUninitialized(SrcWidth * SrcHeight);
    bool bReadSuccess = false;

    // Lambda to normalize 8-bit read
    auto ReadRawColor = [](const FColor& C) -> FLinearColor
    {
        return FLinearColor(C.R / 255.0f, C.G / 255.0f, C.B / 255.0f, C.A / 255.0f);
    };

    // Try reading Platform Data (Runtime)
    if (InViewSpaceNormalTex->GetPlatformData() && InViewSpaceNormalTex->GetPlatformData()->Mips.Num() > 0)
    {
        FTexturePlatformData* PlatformData = InViewSpaceNormalTex->GetPlatformData();
        const void* MipData = PlatformData->Mips[0].BulkData.LockReadOnly();
        
        if (MipData)
        {
            const int32 NumSourcePixels = SrcWidth * SrcHeight;
            const EPixelFormat Format = PlatformData->PixelFormat;

            if (Format == PF_B8G8R8A8 || Format == PF_R8G8B8A8)
            {
                const FColor* ColorData = static_cast<const FColor*>(MipData);
                for (int32 i = 0; i < NumSourcePixels; ++i) SourcePixels[i] = ReadRawColor(ColorData[i]);
                bReadSuccess = true;
            }
            else if (Format == PF_FloatRGBA)
            {
                const FFloat16Color* FloatData = static_cast<const FFloat16Color*>(MipData);
                for (int32 i = 0; i < NumSourcePixels; ++i) SourcePixels[i] = FLinearColor(FloatData[i]);
                bReadSuccess = true;
            }
            else if (Format == PF_A32B32G32R32F)
            {
                const FLinearColor* LinearData = static_cast<const FLinearColor*>(MipData);
                FMemory::Memcpy(SourcePixels.GetData(), LinearData, NumSourcePixels * sizeof(FLinearColor));
                bReadSuccess = true;
            }
            PlatformData->Mips[0].BulkData.Unlock();
        }
    }

#if WITH_EDITOR
    // Fallback to Source Data (Editor)
    if (!bReadSuccess && InViewSpaceNormalTex->Source.GetNumMips() > 0)
    {
        FTextureSource& Source = InViewSpaceNormalTex->Source;
        const uint8* Data = static_cast<const uint8*>(Source.LockMip(0));
        if (Data)
        {
            const int32 NumSourcePixels = SrcWidth * SrcHeight;
            if (Source.GetFormat() == TSF_BGRA8)
            {
                const FColor* ColorData = reinterpret_cast<const FColor*>(Data);
                for (int32 i = 0; i < NumSourcePixels; ++i) SourcePixels[i] = ReadRawColor(ColorData[i]);
                bReadSuccess = true;
            }
            else if (Source.GetFormat() == TSF_RGBA16F)
            {
                const FFloat16Color* FloatData = reinterpret_cast<const FFloat16Color*>(Data);
                for (int32 i = 0; i < NumSourcePixels; ++i) SourcePixels[i] = FLinearColor(FloatData[i]);
                bReadSuccess = true;
            }
            Source.UnlockMip(0);
        }
    }
#endif

    if (!bReadSuccess)
    {
        UE_LOG(LogTemp, Error, TEXT("ProjectNormalDetails: Failed to read source texture data."));
        return false;
    }

    // =========================================================================
    // DEBUG: SAVE DECODED SOURCE PIXELS
    // =========================================================================
    {
        UE_LOG(LogTemp, Warning, TEXT("ProjectNormalDetails (Debug): Saving decoded SourcePixels..."));

        const FString BasePath = TEXT("/Game/TD3D_Debug/");
        const FString AssetName = FString::Printf(TEXT("Debug_NormalStage1_DecodedSource_Slot%d_Cam%d_%s"),
            CurrentProjection_Settings.TargetMaterialSlotIndex,
            CurrentProjection_CameraIndex,
            *FDateTime::Now().ToString(TEXT("HHMMSS"))
        );
        const FString PackagePath = BasePath + AssetName;

        UPackage* Package = CreatePackage(*PackagePath);
        if (Package)
        {
            Package->FullyLoad();
            UTexture2D* NewStaticTexture = NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone);
            if (NewStaticTexture)
            {
                NewStaticTexture->AddToRoot();
                NewStaticTexture->CompressionSettings = TC_Normalmap;
                NewStaticTexture->SRGB = false;
                NewStaticTexture->Source.Init(SrcWidth, SrcHeight, 1, 1, TSF_RGBA16F);
                
                TArray<FFloat16Color> HdrPixels;
                HdrPixels.SetNumUninitialized(SrcWidth * SrcHeight);
                for (int32 i = 0; i < SourcePixels.Num(); ++i)
                {
                    HdrPixels[i] = FFloat16Color(SourcePixels[i]);
                }

                void* DestPixels = NewStaticTexture->Source.LockMip(0);
                if (DestPixels)
                {
                    FMemory::Memcpy(DestPixels, HdrPixels.GetData(), HdrPixels.Num() * sizeof(FFloat16Color));
                    NewStaticTexture->Source.UnlockMip(0);
                    
                    NewStaticTexture->UpdateResource();
                    Package->MarkPackageDirty();
                    FAssetRegistryModule::AssetCreated(NewStaticTexture);

                    const FString PackageFileName = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
                    FSavePackageArgs SaveArgs;
                    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
                    UPackage::SavePackage(Package, NewStaticTexture, *PackageFileName, SaveArgs);
                    UE_LOG(LogTemp, Warning, TEXT("--- DEBUG: Saved DECODED SOURCE to: %s ---"), *PackagePath);
                }
                NewStaticTexture->RemoveFromRoot();
            }
        }
    }

    // =========================================================================
    // STEP 3: SETUP BUFFERS & MATRICES
    // =========================================================================
    FMinimalViewInfo ViewInfo;
    ViewInfo.Location = Settings.CameraPosition;
    ViewInfo.Rotation = Settings.CameraRotation;
    ViewInfo.FOV = Settings.FOVAngle;
    ViewInfo.AspectRatio = (float)TextureWidth / (float)TextureHeight;
    ViewInfo.ProjectionMode = ECameraProjectionMode::Perspective;

    FMatrix ViewM, ProjM, ViewProjM;
    UGameplayStatics::GetViewProjectionMatrix(ViewInfo, ViewM, ProjM, ViewProjM);

    const FMatrix WorldToCamera = ViewM;
    const FMatrix CameraToWorld = ViewM.Inverse();

    TArray<FVector> WorldSpaceNormals;
    WorldSpaceNormals.Init(FVector::ZeroVector, NumTexels);
    
    TArray<float> Weights;
    Weights.SetNumUninitialized(NumTexels);
    
    TArray<bool> HasValidData;
    HasValidData.Init(false, NumTexels);

    // =========================================================================
    // STEP 4: HELPER LAMBDAS
    // =========================================================================
    auto DecodeViewSpaceNormal = [&](const FLinearColor& C) -> FVector
    {
        float X = C.R * 2.0f - 1.0f;
        float Y = C.G * 2.0f - 1.0f;
        float Z = C.B * 2.0f - 1.0f;
        if (bFlipGreenChannel) Y = -Y;
        return FVector(X, Y, Z).GetSafeNormal();
    };

    auto FindRotationBetweenVectors = [](const FVector& From, const FVector& To) -> FQuat
    {
        const float Dot = FVector::DotProduct(From, To);
        if (Dot > 0.99999f) return FQuat::Identity;
        if (Dot < -0.99999f) return FQuat(FVector::RightVector, PI); // 180 deg flip
        
        const FVector Axis = FVector::CrossProduct(From, To);
        const float S = FMath::Sqrt((1.0f + Dot) * 2.0f);
        return FQuat(Axis.X / S, Axis.Y / S, Axis.Z / S, S * 0.5f).GetNormalized();
    };

    auto SampleSourceBilinear = [&SourcePixels, SrcWidth, SrcHeight](const FVector2D& UV) -> FLinearColor
    {
        // Careful clamping to edge logic
        const float U = FMath::Clamp(UV.X, 0.0f, 1.0f);
        const float V = FMath::Clamp(UV.Y, 0.0f, 1.0f);
        
        const float X = U * (SrcWidth - 1);
        const float Y = V * (SrcHeight - 1);
        
        const int32 X0 = (int32)X; 
        const int32 Y0 = (int32)Y;
        const int32 X1 = FMath::Min(X0 + 1, SrcWidth - 1); 
        const int32 Y1 = FMath::Min(Y0 + 1, SrcHeight - 1);
        
        const FLinearColor& C00 = SourcePixels[Y0 * SrcWidth + X0];
        const FLinearColor& C10 = SourcePixels[Y0 * SrcWidth + X1];
        const FLinearColor& C01 = SourcePixels[Y1 * SrcWidth + X0];
        const FLinearColor& C11 = SourcePixels[Y1 * SrcWidth + X1];

        return FMath::Lerp(
            FMath::Lerp(C00, C10, X - X0),
            FMath::Lerp(C01, C11, X - X0),
            Y - Y0
        );
    };

	auto SampleSourceNearest = [&SourcePixels, SrcWidth, SrcHeight](const FVector2D& UV) -> FLinearColor
{
    const float U = FMath::Clamp(UV.X, 0.0f, 1.0f);
    const float V = FMath::Clamp(UV.Y, 0.0f, 1.0f);
    
    const int32 X = FMath::Clamp(FMath::RoundToInt(U * (SrcWidth - 1)), 0, SrcWidth - 1);
    const int32 Y = FMath::Clamp(FMath::RoundToInt(V * (SrcHeight - 1)), 0, SrcHeight - 1);
    
    return SourcePixels[Y * SrcWidth + X];
};

    // =========================================================================
    // STEP 5: CORE PROJECTION LOOP
    // =========================================================================
    TArray<bool> OriginalValidData;
    OriginalValidData.Init(false, NumTexels);

    // Debug: Track sampled source normals for visualization
    TArray<FLinearColor> SampledSourceNormals;
    SampledSourceNormals.Init(FLinearColor(0.5f, 0.5f, 1.0f, 0.0f), NumTexels);

    for (int32 TexelIndex = 0; TexelIndex < NumTexels; ++TexelIndex)
    {
        Weights[TexelIndex] = 0.0f;
        
        if (!VisibilityBuffer[TexelIndex]) continue;

// 		// === UV SEAM BOUNDARY REJECTION (EXPANDED RADIUS) ===
// if (PrecomputedUVIslandMap.Num() == NumTexels) // Only run if island map is valid
// {
//     const int32 MyIslandID = PrecomputedUVIslandMap[TexelIndex];
    
//     if (MyIslandID >= 0)
//     {
//         const int32 X = TexelIndex % TextureWidth;
//         const int32 Y = TexelIndex / TextureWidth;
        
//         // How many pixels away from a seam should we reject?
//         // 1 = original behavior (immediate neighbors only)
//         // 2-3 = accounts for ComfyUI output drift
//         const int32 SeamRejectionRadius = 2;
        
//         auto GetIslandID = [&](int32 NX, int32 NY) -> int32 {
//             if (NX >= 0 && NX < TextureWidth && NY >= 0 && NY < TextureHeight) {
//                 int32 NIdx = NY * TextureWidth + NX;
//                 return PrecomputedUVIslandMap[NIdx];
//             }
//             return -1;
//         };
        
//         bool bIsNearUVBoundary = false;
        
//         // Check all texels within the rejection radius
//         for (int32 DY = -SeamRejectionRadius; DY <= SeamRejectionRadius && !bIsNearUVBoundary; ++DY)
//         {
//             for (int32 DX = -SeamRejectionRadius; DX <= SeamRejectionRadius && !bIsNearUVBoundary; ++DX)
//             {
//                 if (DX == 0 && DY == 0) continue; // Skip self
                
//                 const int32 NeighborIslandID = GetIslandID(X + DX, Y + DY);
                
//                 // If neighbor is valid AND belongs to a different island, we're near a seam
//                 if (NeighborIslandID >= 0 && NeighborIslandID != MyIslandID)
//                 {
//                     bIsNearUVBoundary = true;
//                 }
//             }
//         }
        
//         if (bIsNearUVBoundary)
//         {
//             HasValidData[TexelIndex] = false;
//             continue;
//         }
//     }
// }
// // === END UV SEAM BOUNDARY REJECTION ===



        
        float Weight = NormalWeightBuffer[TexelIndex];
        if (Weight <= KINDA_SMALL_NUMBER) continue;

        const FVector2D& ScreenPos = ScreenPositionBuffer[TexelIndex];
        if (ScreenPos.IsZero() || !FMath::IsFinite(ScreenPos.X) || !FMath::IsFinite(ScreenPos.Y)) continue;

        const FVector& N_Geo_World = PrecomputedNormal_World[TexelIndex];
        if (N_Geo_World.IsNearlyZero()) continue;

        // Transform Geometric normal to View Space
        FVector N_Geo_View_Unreal = WorldToCamera.TransformVector(N_Geo_World).GetSafeNormal();
        
        // Match the encoding from GenerateRasterizedViewSpaceNormals
        FVector N_Geo_ImageSpace;
        N_Geo_ImageSpace.X = N_Geo_View_Unreal.X;
        N_Geo_ImageSpace.Y = N_Geo_View_Unreal.Y;
        N_Geo_ImageSpace.Z = -N_Geo_View_Unreal.Z;
        N_Geo_ImageSpace.Normalize();

        const FVector2D SampleUV(ScreenPos.X, 1.0f - ScreenPos.Y);
        
        const FLinearColor SourceSample = SampleSourceBilinear(SampleUV);
        
        // Store sampled source for debug visualization
        SampledSourceNormals[TexelIndex] = SourceSample;

        if (SourceSample.A < 0.05f) 
        {
            WorldSpaceNormals[TexelIndex] = N_Geo_World;
            Weights[TexelIndex] = Weight;
            HasValidData[TexelIndex] = false;
            continue; 
        }
        
        // Decode the source texture normal
        FVector N_Source_ImageSpace = DecodeViewSpaceNormal(SourceSample);


        // Find rotation from Geometric Normal to Source Texture Normal
        FQuat DetailRotation = FindRotationBetweenVectors(N_Geo_ImageSpace, N_Source_ImageSpace);

        FVector RotationAxis_ImageSpace;
        float RotationAngle;
        DetailRotation.ToAxisAndAngle(RotationAxis_ImageSpace, RotationAngle);

        if (FMath::IsFinite(RotationAngle) && FMath::Abs(RotationAngle) > KINDA_SMALL_NUMBER)
        {
            float ScaledAngle = RotationAngle * DetailGain * Weight;
            ScaledAngle = FMath::Clamp(ScaledAngle, -MaxAngleRadians, MaxAngleRadians);
            
            // Un-do the Z negation to get back to Unreal View space
            FVector Axis_UnrealView;
            Axis_UnrealView.X = RotationAxis_ImageSpace.X;
            Axis_UnrealView.Y = RotationAxis_ImageSpace.Y;
            Axis_UnrealView.Z = -RotationAxis_ImageSpace.Z;
            
            // Transform to world space and apply rotation
            FVector Axis_World = CameraToWorld.TransformVector(Axis_UnrealView);
            FQuat WorldRotation(Axis_World, ScaledAngle);
            
            WorldSpaceNormals[TexelIndex] = WorldRotation.RotateVector(N_Geo_World).GetSafeNormal();
        }
        else
        {
            WorldSpaceNormals[TexelIndex] = N_Geo_World;
        }

        Weights[TexelIndex] = Weight;
        HasValidData[TexelIndex] = true;
        OriginalValidData[TexelIndex] = true;
    }

	
	// =========================================================================
	// STEP 6: HIGH-PASS FILTER (world-space blur to avoid UV seam artifacts)
	// =========================================================================


	if (HighPassFilterRadius > 0)
	{
		// Compute world-space blur radius based on mesh scale and texel density
		FVector MinPos(FLT_MAX), MaxPos(-FLT_MAX);
		int32 ValidTexelCount = 0;
		for (int32 i = 0; i < NumTexels; ++i)
		{
			if (OriginalValidData[i])
			{
				const FVector& P = PrecomputedWorldPositionBuffer[i];
				MinPos = MinPos.ComponentMin(P);
				MaxPos = MaxPos.ComponentMax(P);
				ValidTexelCount++;
			}
		}
		
		const float MeshExtent = (MaxPos - MinPos).Size();
		const float TexelWorldSize = MeshExtent / FMath::Sqrt((float)FMath::Max(ValidTexelCount, 1));
		const float WorldBlurRadius = TexelWorldSize * HighPassFilterRadius;

		UE_LOG(LogTemp, Log, TEXT("HighPass: MeshExtent=%.2f, TexelWorldSize=%.4f, WorldBlurRadius=%.4f"),
			MeshExtent, TexelWorldSize, WorldBlurRadius);

		// 6a: Build spatial acceleration structure (simple grid)
		const float CellSize = FMath::Max(WorldBlurRadius, KINDA_SMALL_NUMBER);
		TMultiMap<FIntVector, int32> SpatialHash;
		
		auto WorldToCell = [&](const FVector& P) -> FIntVector
		{
			return FIntVector(
				FMath::FloorToInt((P.X - MinPos.X) / CellSize),
				FMath::FloorToInt((P.Y - MinPos.Y) / CellSize),
				FMath::FloorToInt((P.Z - MinPos.Z) / CellSize)
			);
		};

		for (int32 i = 0; i < NumTexels; ++i)
		{
			if (OriginalValidData[i])
			{
				SpatialHash.Add(WorldToCell(PrecomputedWorldPositionBuffer[i]), i);
			}
		}

		// 6b: Blur each texel using world-space neighbors
		TArray<FVector> BlurredNormals;
		BlurredNormals.SetNumUninitialized(NumTexels);

		const float WorldBlurRadiusSq = WorldBlurRadius * WorldBlurRadius;

		for (int32 TexelIndex = 0; TexelIndex < NumTexels; ++TexelIndex)
		{
			if (!OriginalValidData[TexelIndex])
			{
				BlurredNormals[TexelIndex] = FVector::ZeroVector;
				continue;
			}

			const FVector& CenterWorldPos = PrecomputedWorldPositionBuffer[TexelIndex];
			const FIntVector CenterCell = WorldToCell(CenterWorldPos);

			FVector AccumulatedNormal = FVector::ZeroVector;
			int32 ValidCount = 0;

			// Check neighboring cells (3x3x3 grid around center)
			for (int32 dz = -1; dz <= 1; ++dz)
			for (int32 dy = -1; dy <= 1; ++dy)
			for (int32 dx = -1; dx <= 1; ++dx)
			{
				FIntVector NeighborCell(CenterCell.X + dx, CenterCell.Y + dy, CenterCell.Z + dz);
				
				TArray<int32> TexelsInCell;
				SpatialHash.MultiFind(NeighborCell, TexelsInCell);
				
				for (int32 NeighborIndex : TexelsInCell)
				{
					if (!OriginalValidData[NeighborIndex])
						continue;
						
					const FVector& NeighborWorldPos = PrecomputedWorldPositionBuffer[NeighborIndex];
					const float DistSq = FVector::DistSquared(CenterWorldPos, NeighborWorldPos);
					
					if (DistSq <= WorldBlurRadiusSq)
					{
						AccumulatedNormal += WorldSpaceNormals[NeighborIndex];
						ValidCount++;
					}
				}
			}

			if (ValidCount > 0)
			{
				BlurredNormals[TexelIndex] = AccumulatedNormal.GetSafeNormal();
			}
			else
			{
				BlurredNormals[TexelIndex] = WorldSpaceNormals[TexelIndex];
			}
		}

		// 6c: Extract high-frequency detail and apply to geometric normal
		for (int32 TexelIndex = 0; TexelIndex < NumTexels; ++TexelIndex)
		{
			if (!OriginalValidData[TexelIndex])
				continue;

			const FVector& N_Geo = PrecomputedNormal_World[TexelIndex];
			const FVector& N_Projected = WorldSpaceNormals[TexelIndex];
			const FVector& N_Blurred = BlurredNormals[TexelIndex];

			const float Dot = FVector::DotProduct(N_Blurred, N_Projected);
			
			if (Dot > 0.99999f)
			{
				WorldSpaceNormals[TexelIndex] = N_Geo;
			}
			else if (Dot < -0.99999f)
			{
				WorldSpaceNormals[TexelIndex] = N_Geo;
			}
			else
			{
				const FVector Axis = FVector::CrossProduct(N_Blurred, N_Projected).GetSafeNormal();
				const float Angle = FMath::Acos(FMath::Clamp(Dot, -1.0f, 1.0f));
				
				const FQuat DetailRotation(Axis, Angle);
				WorldSpaceNormals[TexelIndex] = DetailRotation.RotateVector(N_Geo).GetSafeNormal();
			}
		}
	}

    // =========================================================================
    // DEBUG: SAVE SAMPLED SOURCE NORMALS (what was actually read from source)
    // =========================================================================
    {
        UE_LOG(LogTemp, Warning, TEXT("ProjectNormalDetails (Debug): Saving sampled source normals (reprojected)..."));

        const FString BasePath = TEXT("/Game/TD3D_Debug/");
        const FString AssetName = FString::Printf(TEXT("Debug_NormalStage2_SampledSource_Slot%d_Cam%d_%s"),
            CurrentProjection_Settings.TargetMaterialSlotIndex,
            CurrentProjection_CameraIndex,
            *FDateTime::Now().ToString(TEXT("HHMMSS"))
        );
        const FString PackagePath = BasePath + AssetName;

        UPackage* Package = CreatePackage(*PackagePath);
        if (Package)
        {
            Package->FullyLoad();
            UTexture2D* NewStaticTexture = NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone);
            if (NewStaticTexture)
            {
                NewStaticTexture->AddToRoot();
                NewStaticTexture->CompressionSettings = TC_Normalmap;
                NewStaticTexture->SRGB = false;
                NewStaticTexture->Source.Init(TextureWidth, TextureHeight, 1, 1, TSF_RGBA16F);
                
                TArray<FFloat16Color> HdrPixels;
                HdrPixels.SetNumUninitialized(NumTexels);
                for (int32 i = 0; i < NumTexels; ++i)
                {
                    HdrPixels[i] = FFloat16Color(SampledSourceNormals[i]);
                }

                void* DestPixels = NewStaticTexture->Source.LockMip(0);
                if (DestPixels)
                {
                    FMemory::Memcpy(DestPixels, HdrPixels.GetData(), HdrPixels.Num() * sizeof(FFloat16Color));
                    NewStaticTexture->Source.UnlockMip(0);
                    
                    NewStaticTexture->UpdateResource();
                    Package->MarkPackageDirty();
                    FAssetRegistryModule::AssetCreated(NewStaticTexture);

                    const FString PackageFileName = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
                    FSavePackageArgs SaveArgs;
                    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
                    UPackage::SavePackage(Package, NewStaticTexture, *PackageFileName, SaveArgs);
                    UE_LOG(LogTemp, Warning, TEXT("--- DEBUG: Saved SAMPLED SOURCE NORMALS to: %s ---"), *PackagePath);
                }
                NewStaticTexture->RemoveFromRoot();
            }
        }
    }

    // =========================================================================
    // DEBUG: SAVE WORLD SPACE NORMALS (after projection, before tangent conversion)
    // =========================================================================
    {
        UE_LOG(LogTemp, Warning, TEXT("ProjectNormalDetails (Debug): Saving world space normals (post-projection)..."));

        const FString BasePath = TEXT("/Game/TD3D_Debug/");
        const FString AssetName = FString::Printf(TEXT("Debug_NormalStage3_WorldSpace_Slot%d_Cam%d_%s"),
            CurrentProjection_Settings.TargetMaterialSlotIndex,
            CurrentProjection_CameraIndex,
            *FDateTime::Now().ToString(TEXT("HHMMSS"))
        );
        const FString PackagePath = BasePath + AssetName;

        UPackage* Package = CreatePackage(*PackagePath);
        if (Package)
        {
            Package->FullyLoad();
            UTexture2D* NewStaticTexture = NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone);
            if (NewStaticTexture)
            {
                NewStaticTexture->AddToRoot();
                NewStaticTexture->CompressionSettings = TC_Normalmap;
                NewStaticTexture->SRGB = false;
                NewStaticTexture->Source.Init(TextureWidth, TextureHeight, 1, 1, TSF_RGBA16F);
                
                TArray<FFloat16Color> HdrPixels;
                HdrPixels.SetNumUninitialized(NumTexels);
                for (int32 i = 0; i < NumTexels; ++i)
                {
                    const FVector& N = WorldSpaceNormals[i];
                    float Alpha = HasValidData[i] ? Weights[i] : 0.0f;
                    HdrPixels[i] = FFloat16Color(FLinearColor(
                        N.X * 0.5f + 0.5f,
                        N.Y * 0.5f + 0.5f,
                        N.Z * 0.5f + 0.5f,
                        Alpha
                    ));
                }

                void* DestPixels = NewStaticTexture->Source.LockMip(0);
                if (DestPixels)
                {
                    FMemory::Memcpy(DestPixels, HdrPixels.GetData(), HdrPixels.Num() * sizeof(FFloat16Color));
                    NewStaticTexture->Source.UnlockMip(0);
                    
                    NewStaticTexture->UpdateResource();
                    Package->MarkPackageDirty();
                    FAssetRegistryModule::AssetCreated(NewStaticTexture);

                    const FString PackageFileName = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
                    FSavePackageArgs SaveArgs;
                    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
                    UPackage::SavePackage(Package, NewStaticTexture, *PackageFileName, SaveArgs);
                    UE_LOG(LogTemp, Warning, TEXT("--- DEBUG: Saved WORLD SPACE NORMALS to: %s ---"), *PackagePath);
                }
                NewStaticTexture->RemoveFromRoot();
            }
        }
    }

    // =========================================================================
    // STEP 7: CONVERT TO TANGENT SPACE
    // =========================================================================
    OutTangentSpaceNormals.SetNumUninitialized(NumTexels);
    
    // Default Flat Normal (0,0,1) encoded
    const FLinearColor DefaultNormal(0.5f, 0.5f, 1.0f, 0.0f); 

    for (int32 i = 0; i < NumTexels; ++i)
    {
        // DEBUG: Check for degenerate TBN at this texel
        const FVector& T_W = PrecomputedTangent_World[i];
        const FVector& B_W = PrecomputedBitangent_World[i];
        const FVector& N_Geo = PrecomputedNormal_World[i];
        
        if (HasValidData[i] && (T_W.IsNearlyZero() || B_W.IsNearlyZero() || N_Geo.IsNearlyZero()))
        {
            UE_LOG(LogTemp, Warning, TEXT("Degenerate TBN at texel %d: T=%s, B=%s, N=%s"), 
                i, *T_W.ToString(), *B_W.ToString(), *N_Geo.ToString());
        }

        if (!VisibilityBuffer[i])
        { 
            OutTangentSpaceNormals[i] = DefaultNormal; 
            continue; 
        }

        if (!HasValidData[i])
        {
            // No projected detail - write flat tangent-space normal with zero alpha
            OutTangentSpaceNormals[i] = FLinearColor(0.5f, 0.5f, 1.0f, 0.0f);
            continue;
        }

        const FVector& N_W = WorldSpaceNormals[i];

        // Transform World Normal -> Tangent Space
        FVector N_Tan;
        N_Tan.X = FVector::DotProduct(N_W, T_W);
        N_Tan.Y = FVector::DotProduct(N_W, B_W);
        N_Tan.Z = FVector::DotProduct(N_W, N_Geo); 
        N_Tan.Normalize();

        OutTangentSpaceNormals[i] = FLinearColor(
            N_Tan.X * 0.5f + 0.5f,
            N_Tan.Y * 0.5f + 0.5f,
            -N_Tan.Z * 0.5f + 0.5f,
            Weights[i]
        );
    }

    // =========================================================================
    // DEBUG: SAVE FINAL TANGENT SPACE OUTPUT
    // =========================================================================
    {
        UE_LOG(LogTemp, Warning, TEXT("ProjectNormalDetails (Debug): Saving final tangent space output..."));

        const FString BasePath = TEXT("/Game/TD3D_Debug/");
        const FString AssetName = FString::Printf(TEXT("Debug_NormalStage4_FinalTangentSpace_Slot%d_Cam%d_%s"),
            CurrentProjection_Settings.TargetMaterialSlotIndex,
            CurrentProjection_CameraIndex,
            *FDateTime::Now().ToString(TEXT("HHMMSS"))
        );
        const FString PackagePath = BasePath + AssetName;

        UPackage* Package = CreatePackage(*PackagePath);
        if (Package)
        {
            Package->FullyLoad();
            UTexture2D* NewStaticTexture = NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone);
            if (NewStaticTexture)
            {
                NewStaticTexture->AddToRoot();
                NewStaticTexture->CompressionSettings = TC_Normalmap;
                NewStaticTexture->SRGB = false;
                NewStaticTexture->Source.Init(TextureWidth, TextureHeight, 1, 1, TSF_RGBA16F);
                
                TArray<FFloat16Color> HdrPixels;
                HdrPixels.SetNumUninitialized(NumTexels);
                for (int32 i = 0; i < NumTexels; ++i)
                {
                    HdrPixels[i] = FFloat16Color(OutTangentSpaceNormals[i]);
                }

                void* DestPixels = NewStaticTexture->Source.LockMip(0);
                if (DestPixels)
                {
                    FMemory::Memcpy(DestPixels, HdrPixels.GetData(), HdrPixels.Num() * sizeof(FFloat16Color));
                    NewStaticTexture->Source.UnlockMip(0);
                    
                    NewStaticTexture->UpdateResource();
                    Package->MarkPackageDirty();
                    FAssetRegistryModule::AssetCreated(NewStaticTexture);

                    const FString PackageFileName = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
                    FSavePackageArgs SaveArgs;
                    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
                    
                    if (UPackage::SavePackage(Package, NewStaticTexture, *PackageFileName, SaveArgs))
                    {
                        UE_LOG(LogTemp, Warning, TEXT("--- DEBUG: Saved FINAL TANGENT SPACE OUTPUT to: %s ---"), *PackagePath);
                        if (GEditor) 
                        { 
                            TArray<UObject*> AssetsToSync = { NewStaticTexture }; 
                            GEditor->SyncBrowserToObjects(AssetsToSync); 
                        }
                    }
                }
                NewStaticTexture->RemoveFromRoot();
            }
        }
    }

    UE_LOG(LogTemp, Warning, TEXT("--- ProjectNormalDetails END --- Success: TRUE ---"));
    return true;
}

// Function to process camera weights: preserves original weights for single-camera texels
// and normalizes weights for multi-camera texels. Organizes output per texel.
// This is the new, single function that correctly applies user weights AND normalizes them.
void FTextureDiffusion3D::ProcessCameraWeights(
	const TArray<TArray<float>>& NormalWeightBuffers,
	const TArray<FProjectionSettings>& InCameraSettings,
	int32 TextureWidth,
	int32 TextureHeight,
	TArray<TArray<float>>& OutTexelWeights)
{
	const int32 NumTexels = TextureWidth * TextureHeight;
	const int32 NumCameras = NormalWeightBuffers.Num();

	if (NumCameras == 0 || NumTexels == 0) { return; }

	OutTexelWeights.SetNum(NumTexels);

	for (int32 TexelIndex = 0; TexelIndex < NumTexels; TexelIndex++)
	{
		TArray<float>& TexelWeights = OutTexelWeights[TexelIndex];
		TexelWeights.Init(0.0f, NumCameras);

		float TotalWeight = 0.0f;

		// Step 1: Apply user-defined weights from the sliders and calculate the total sum for this pixel.
		for (int32 CameraIndex = 0; CameraIndex < NumCameras; CameraIndex++)
		{
			if (NormalWeightBuffers[CameraIndex].IsValidIndex(TexelIndex) && InCameraSettings.IsValidIndex(CameraIndex))
			{
				// Get the weight from the geometric calculation (view angle, etc.)
				const float RawGeometricWeight = NormalWeightBuffers[CameraIndex][TexelIndex];
				
				// Get the user-defined intensity weight from the camera's settings slider.
				const float UserSliderWeight = InCameraSettings[CameraIndex].Weight;

				// The final pre-normalized weight is the product of the two.
				const float CombinedWeight = RawGeometricWeight * UserSliderWeight;
				TexelWeights[CameraIndex] = CombinedWeight;
				
				// Add to the total sum for this pixel, which we'll use to normalize.
				TotalWeight += CombinedWeight;
			}
		}

		// Step 2: Normalize all the weights for this pixel if there's any contribution.
		if (TotalWeight > KINDA_SMALL_NUMBER)
		{
			for (int32 CameraIndex = 0; CameraIndex < NumCameras; CameraIndex++)
			{
				TexelWeights[CameraIndex] /= TotalWeight;
			}
		}
	}
}


void FTextureDiffusion3D::GenerateOptimalCameraRig()
{
    // ========================================================================
    // PHASE 1: VALIDATION & SETUP
    // ========================================================================
    
    if (!SelectedActor)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("Please select a Static Mesh Actor first."));
        return;
    }
    
    UStaticMeshComponent* MeshComp = SelectedActor->GetStaticMeshComponent();
    UStaticMesh* StaticMesh = MeshComp ? MeshComp->GetStaticMesh() : nullptr;
    if (!StaticMesh || !StaticMesh->GetRenderData())
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("Invalid static mesh."));
        return;
    }
    
    const int32 CurrentSlotIndex = CurrentSettings.TargetMaterialSlotIndex;
    TArray<FProjectionSettings>& SlotCameraSettings = PerSlotCameraSettings.FindOrAdd(CurrentSlotIndex);
    
    // Must have Camera 1 already placed
    if (SlotCameraSettings.Num() == 0)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("Please set up Camera 1 first. This will be used as the anchor for the camera rig."));
        return;
    }
    
    const FStaticMeshLODResources& LOD = StaticMesh->GetRenderData()->LODResources[0];
    if (!LOD.Sections.IsValidIndex(CurrentSlotIndex))
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("Invalid material slot."));
        return;
    }
    
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("Cannot get editor world."));
        return;
    }
    
    // ========================================================================
    // PHASE 2: ANALYZE CAMERA 1 (THE ANCHOR)
    // ========================================================================
    
    FScopedSlowTask SlowTask(6.f, FText::FromString("Generating optimal camera rig..."));
    SlowTask.MakeDialog(true);
    SlowTask.EnterProgressFrame(1.f, FText::FromString("Analyzing Camera 1..."));
    
    const FProjectionSettings& Cam1Settings = SlotCameraSettings[0];
    
    // Compute mesh bounds for this material slot
    const FTransform& ComponentTransform = MeshComp->GetComponentTransform();
    const FPositionVertexBuffer& PB = LOD.VertexBuffers.PositionVertexBuffer;
    const FStaticMeshSection& Section = LOD.Sections[CurrentSlotIndex];
    
    FBox MaterialBounds(EForceInit::ForceInit);
    for (uint32 i = Section.MinVertexIndex; i <= Section.MaxVertexIndex; ++i)
    {
        MaterialBounds += ComponentTransform.TransformPosition(FVector(PB.VertexPosition(i)));
    }
    
    const FVector MeshCenter = MaterialBounds.GetCenter();
    const float MeshRadius = MaterialBounds.GetExtent().Size() * 0.5f;
    
    // Extract Camera 1's orbital parameters
    const FVector Cam1Position = Cam1Settings.CameraPosition;
    const float MeshBoundingRadius = MaterialBounds.GetExtent().Size();
const float Cam1Distance = FVector::Dist(Cam1Position, MeshCenter);
const float HalfFovRad = FMath::DegreesToRadians(Cam1Settings.FOVAngle * 0.5f);
const float MinDistanceForFOV = MeshBoundingRadius / FMath::Tan(HalfFovRad);
const float OrbitRadius = FMath::Max(Cam1Distance, MinDistanceForFOV * 1.1f);
    const FVector Cam1Direction = (Cam1Position - MeshCenter).GetSafeNormal();
    const float Cam1Azimuth = FMath::Atan2(Cam1Direction.Y, Cam1Direction.X);
    const float Cam1Elevation = FMath::Asin(FMath::Clamp(Cam1Direction.Z, -1.0f, 1.0f));
    
    UE_LOG(LogTemp, Log, TEXT("AutoRig: Camera 1 at azimuth %.1f°, elevation %.1f°, distance %.1f"), 
        FMath::RadiansToDegrees(Cam1Azimuth),
        FMath::RadiansToDegrees(Cam1Elevation),
        OrbitRadius);
    
    // ========================================================================
    // PHASE 3: BAKE GEOMETRY INTO UV SPACE
    // ========================================================================
    
    SlowTask.EnterProgressFrame(1.f, FText::FromString("Baking geometry..."));
    
    const int32 EvalWidth = 128;
    const int32 EvalHeight = 128;
    const int32 NumEvalTexels = EvalWidth * EvalHeight;
    
    const FMatrix LocalToWorldInvTrans = ComponentTransform.ToMatrixWithScale().Inverse().GetTransposed();
    const FStaticMeshVertexBuffer& VB = LOD.VertexBuffers.StaticMeshVertexBuffer;
    const FIndexArrayView Indices = LOD.IndexBuffer.GetArrayView();
    
    TArray<FVector> BakedWorldPos;
    TArray<FVector> BakedWorldNorm;
    BakedWorldPos.Init(FVector::ZeroVector, NumEvalTexels);
    BakedWorldNorm.Init(FVector::ZeroVector, NumEvalTexels);
    
    for (uint32 tri = 0; tri < Section.NumTriangles; ++tri)
    {
        const uint32 TriIdx = Section.FirstIndex + (tri * 3);
        const uint32 I0 = Indices[TriIdx + 0];
        const uint32 I1 = Indices[TriIdx + 1];
        const uint32 I2 = Indices[TriIdx + 2];
        
        const FVector2D UV0(VB.GetVertexUV(I0, TargetUVChannel));
        const FVector2D UV1(VB.GetVertexUV(I1, TargetUVChannel));
        const FVector2D UV2(VB.GetVertexUV(I2, TargetUVChannel));
        
        const FVector P0 = ComponentTransform.TransformPosition(FVector(PB.VertexPosition(I0)));
        const FVector P1 = ComponentTransform.TransformPosition(FVector(PB.VertexPosition(I1)));
        const FVector P2 = ComponentTransform.TransformPosition(FVector(PB.VertexPosition(I2)));
        
        const FVector N0 = LocalToWorldInvTrans.TransformVector(FVector(VB.VertexTangentZ(I0))).GetSafeNormal();
        const FVector N1 = LocalToWorldInvTrans.TransformVector(FVector(VB.VertexTangentZ(I1))).GetSafeNormal();
        const FVector N2 = LocalToWorldInvTrans.TransformVector(FVector(VB.VertexTangentZ(I2))).GetSafeNormal();
        
        const int32 MinX = FMath::Clamp(FMath::FloorToInt(FMath::Min3(UV0.X, UV1.X, UV2.X) * EvalWidth), 0, EvalWidth - 1);
        const int32 MinY = FMath::Clamp(FMath::FloorToInt(FMath::Min3(UV0.Y, UV1.Y, UV2.Y) * EvalHeight), 0, EvalHeight - 1);
        const int32 MaxX = FMath::Clamp(FMath::CeilToInt(FMath::Max3(UV0.X, UV1.X, UV2.X) * EvalWidth), 0, EvalWidth - 1);
        const int32 MaxY = FMath::Clamp(FMath::CeilToInt(FMath::Max3(UV0.Y, UV1.Y, UV2.Y) * EvalHeight), 0, EvalHeight - 1);
        
        for (int32 Y = MinY; Y <= MaxY; ++Y)
        {
            for (int32 X = MinX; X <= MaxX; ++X)
            {
                const FVector2D PixelUV((X + 0.5f) / EvalWidth, (Y + 0.5f) / EvalHeight);
                
                const FVector2D v0 = UV1 - UV0;
                const FVector2D v1 = UV2 - UV0;
                const FVector2D v2 = PixelUV - UV0;
                
                const float d00 = v0 | v0;
                const float d01 = v0 | v1;
                const float d11 = v1 | v1;
                const float d20 = v2 | v0;
                const float d21 = v2 | v1;
                const float denom = d00 * d11 - d01 * d01;
                
                if (FMath::IsNearlyZero(denom)) continue;
                
                const float v = (d11 * d20 - d01 * d21) / denom;
                const float w = (d00 * d21 - d01 * d20) / denom;
                const float u = 1.0f - v - w;
                
                if (u >= -KINDA_SMALL_NUMBER && v >= -KINDA_SMALL_NUMBER && w >= -KINDA_SMALL_NUMBER)
                {
                    const int32 Idx = Y * EvalWidth + X;
                    BakedWorldPos[Idx] = P0 * u + P1 * v + P2 * w;
                    BakedWorldNorm[Idx] = (N0 * u + N1 * v + N2 * w).GetSafeNormal();
                }
            }
        }
    }
    
    // Build list of valid texel indices
    TArray<int32> ValidTexelIndices;
    ValidTexelIndices.Reserve(NumEvalTexels);
    for (int32 i = 0; i < NumEvalTexels; ++i)
    {
        if (!BakedWorldPos[i].IsZero())
        {
            ValidTexelIndices.Add(i);
        }
    }
    
    const int32 TotalValidTexels = ValidTexelIndices.Num();
    if (TotalValidTexels == 0)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("No valid texels found. Check UV mapping."));
        return;
    }
    
    UE_LOG(LogTemp, Log, TEXT("AutoRig: Baked %d valid texels (%.1f%% UV coverage)"), 
        TotalValidTexels, (float)TotalValidTexels * 100.f / NumEvalTexels);
    
    // ========================================================================
    // PHASE 4: SETUP VISIBILITY EVALUATION
    // ========================================================================
    
    SlowTask.EnterProgressFrame(0.5f, FText::FromString("Setting up visibility evaluation..."));
    
    const float MinDotThreshold = 0.15f; // ~81 degrees from surface normal
    
    // Lambda to evaluate a camera's visibility
    auto EvaluateCameraVisibility = [&](const FVector& CamPos) -> TBitArray<>
    {
        TBitArray<> VisibleMask;
        VisibleMask.Init(false, NumEvalTexels);
        
        for (int32 TexelIdx : ValidTexelIndices)
        {
            const FVector& SurfPos = BakedWorldPos[TexelIdx];
            const FVector& SurfNorm = BakedWorldNorm[TexelIdx];
            
            const FVector ToCam = (CamPos - SurfPos).GetSafeNormal();
            const float Dot = FVector::DotProduct(SurfNorm, ToCam);
            
            if (Dot < MinDotThreshold) continue;
            
            FHitResult Hit;
            const FVector RayStart = SurfPos + SurfNorm * 0.5f;
            const bool bHit = World->LineTraceSingleByChannel(Hit, RayStart, CamPos, ECC_Visibility);
            
            const float DistToCamera = FVector::Dist(SurfPos, CamPos);
            if (!bHit || Hit.Distance > (DistToCamera - 1.0f))
            {
                VisibleMask[TexelIdx] = true;
            }
        }
        
        return VisibleMask;
    };
    
    // Lambda to count visible texels
    auto CountVisibleTexels = [&](const TBitArray<>& Mask) -> int32
    {
        int32 Count = 0;
        for (int32 Idx : ValidTexelIndices)
        {
            if (Mask[Idx]) Count++;
        }
        return Count;
    };
    
    // Lambda to count NEW texels (marginal coverage)
    auto CountMarginalTexels = [&](const TBitArray<>& CandidateMask, const TBitArray<>& AlreadyCovered) -> int32
    {
        int32 Count = 0;
        for (int32 Idx : ValidTexelIndices)
        {
            if (CandidateMask[Idx] && !AlreadyCovered[Idx]) Count++;
        }
        return Count;
    };
    
    // Evaluate Camera 1
    TBitArray<> Cam1Visibility = EvaluateCameraVisibility(Cam1Position);
    const int32 Cam1VisibleCount = CountVisibleTexels(Cam1Visibility);
    const float Cam1CoveragePercent = (float)Cam1VisibleCount / TotalValidTexels;
    
    UE_LOG(LogTemp, Log, TEXT("AutoRig: Camera 1 covers %d texels (%.1f%%)"), 
        Cam1VisibleCount, Cam1CoveragePercent * 100.f);
    
    // Warn if Camera 1 has poor coverage
    if (Cam1CoveragePercent < 0.15f)
    {
        EAppReturnType::Type Result = FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(
            FString::Printf(TEXT("Warning: Camera 1 only covers %.0f%% of the surface.\n\nThis might indicate the camera is poorly positioned (e.g., looking at an edge).\n\nContinue anyway?"), 
            Cam1CoveragePercent * 100.f)));
        
        if (Result != EAppReturnType::Yes)
        {
            return;
        }
    }
    
    // ========================================================================
    // PHASE 5: GENERATE CANDIDATE CAMERAS
    // ========================================================================
    
    SlowTask.EnterProgressFrame(1.f, FText::FromString("Generating candidates..."));
    
    // Angular constraints
    const float MinAngularSpacing = FMath::DegreesToRadians(30.0f);
    
    struct FCameraCandidate
    {
        FVector Position;
        FRotator Rotation;
        float Azimuth;
        float Elevation;
        float Radius;
        TBitArray<> VisibilityMask;
        int32 VisibleCount;
        float QualityMultiplier = 1.0f;
    };
    
    TArray<FCameraCandidate> Candidates;
    
    // Helper to check if too close to Camera 1
    auto IsTooCloseToCam1 = [&](float Azimuth) -> bool
    {
        float DeltaToCam1 = FMath::Abs(FMath::FindDeltaAngleRadians(Azimuth, Cam1Azimuth));
        return DeltaToCam1 < MinAngularSpacing;
    };
    
    // Strategy A: Gap-directed candidates (face uncovered surface normals)
    TMap<int32, FVector> UncoveredNormalSum;
    TMap<int32, int32> UncoveredNormalCount;
    const int32 NumAzimuthBuckets = 12; // 30° buckets
    
    for (int32 TexelIdx : ValidTexelIndices)
    {
        if (!Cam1Visibility[TexelIdx])
        {
            const FVector& SurfNorm = BakedWorldNorm[TexelIdx];
            const FVector CamDir = -SurfNorm;
            float Azimuth = FMath::Atan2(CamDir.Y, CamDir.X);
            int32 Bucket = FMath::FloorToInt((Azimuth + PI) / (2.0f * PI) * NumAzimuthBuckets) % NumAzimuthBuckets;
            
            UncoveredNormalSum.FindOrAdd(Bucket) += CamDir;
            UncoveredNormalCount.FindOrAdd(Bucket)++;
        }
    }
    
    // Create candidates from dominant uncovered directions
    for (const auto& Pair : UncoveredNormalSum)
    {
        int32 Count = UncoveredNormalCount[Pair.Key];
        if (Count < TotalValidTexels * 0.02f) continue;
        
        FVector AvgDir = (Pair.Value / Count).GetSafeNormal();
        float Azimuth = FMath::Atan2(AvgDir.Y, AvgDir.X);
        
        if (IsTooCloseToCam1(Azimuth)) continue;
        
        // Try multiple elevations for each azimuth
        for (float ElevationDeg : {-45.0f, -20.0f, 0.0f, 20.0f, 35.0f, 45.0f})
        {
            float Elevation = FMath::DegreesToRadians(ElevationDeg);
            
            if (FMath::Abs(Elevation) > FMath::DegreesToRadians(70.0f)) continue;
            
            FVector Dir;
            Dir.X = FMath::Cos(Elevation) * FMath::Cos(Azimuth);
            Dir.Y = FMath::Cos(Elevation) * FMath::Sin(Azimuth);
            Dir.Z = FMath::Sin(Elevation);
            
            // Try different radii
            for (float RadiusMult : {1.0f, 0.9f, 1.1f})
            {
                float Radius = OrbitRadius * RadiusMult;
                FVector CamPos = MeshCenter + Dir * Radius;
                FRotator CamRot = (MeshCenter - CamPos).Rotation();
                
                FCameraCandidate Candidate;
                Candidate.Position = CamPos;
                Candidate.Rotation = CamRot;
                Candidate.Azimuth = Azimuth;
                Candidate.Elevation = Elevation;
                Candidate.Radius = Radius;
                
                Candidates.Add(Candidate);
            }
        }
    }
    
    // Strategy B: Fill ring candidates (ensure coverage around the mesh)
    for (int32 AzimuthStep = 0; AzimuthStep < 12; ++AzimuthStep)
    {
        float Azimuth = -PI + (AzimuthStep / 12.0f) * 2.0f * PI;
        
        if (IsTooCloseToCam1(Azimuth)) continue;
        
        for (float ElevationDeg : {-30.0f, 0.0f, 20.0f, 35.0f})
        {
            float Elevation = FMath::DegreesToRadians(ElevationDeg);
            
            FVector Dir;
            Dir.X = FMath::Cos(Elevation) * FMath::Cos(Azimuth);
            Dir.Y = FMath::Cos(Elevation) * FMath::Sin(Azimuth);
            Dir.Z = FMath::Sin(Elevation);
            
            FVector CamPos = MeshCenter + Dir * OrbitRadius;
            FRotator CamRot = (MeshCenter - CamPos).Rotation();
            
            FCameraCandidate Candidate;
            Candidate.Position = CamPos;
            Candidate.Rotation = CamRot;
            Candidate.Azimuth = Azimuth;
            Candidate.Elevation = Elevation;
            Candidate.Radius = OrbitRadius;
            
            Candidates.Add(Candidate);
        }
    }
    
    UE_LOG(LogTemp, Log, TEXT("AutoRig: Generated %d candidate positions"), Candidates.Num());
    
    if (Candidates.Num() == 0)
    {
        // No candidates means Camera 1 might already cover everything reachable
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(
            FString::Printf(TEXT("Camera 1 covers %.0f%% of the surface. No additional camera angles found that would improve coverage."), 
            Cam1CoveragePercent * 100.f)));
        return;
    }
    
    // ========================================================================
    // PHASE 6: EVALUATE ALL CANDIDATES & CALCULATE THEORETICAL MAXIMUM
    // ========================================================================
    
    SlowTask.EnterProgressFrame(1.f, FText::FromString("Evaluating candidates & calculating theoretical maximum..."));
    
    // Initialize visibility masks
    for (FCameraCandidate& Candidate : Candidates)
    {
        Candidate.VisibilityMask.Init(false, NumEvalTexels);
    }
    
    // Helper to calculate quality multiplier for a candidate
    auto CalculateQualityMultiplier = [&](const FCameraCandidate& Candidate) -> float
    {
        float Quality = 1.0f;
        
        const float ElevationDeg = FMath::RadiansToDegrees(Candidate.Elevation);
        const float RelativeAzimuth = FMath::FindDeltaAngleRadians(Cam1Azimuth, Candidate.Azimuth);
        const float RelativeAzimuthDeg = FMath::RadiansToDegrees(FMath::Abs(RelativeAzimuth));
        
        // --- Elevation-based quality ---
        
        // Sweet spot: 15-45° elevation (classic 3/4 hero view)
        if (ElevationDeg > 15.0f && ElevationDeg < 45.0f)
        {
            Quality *= 1.3f;
        }
        // Good: slight elevation (0-15°)
        else if (ElevationDeg >= 0.0f && ElevationDeg <= 15.0f)
        {
            Quality *= 1.15f;
        }
        // Acceptable: slight downward (-15 to 0°)
        else if (ElevationDeg >= -15.0f && ElevationDeg < 0.0f)
        {
            Quality *= 1.0f;
        }
        // Less ideal: moderate downward (-15 to -45°)
        else if (ElevationDeg >= -45.0f && ElevationDeg < -15.0f)
        {
            Quality *= 0.9f;
        }
        // Penalty: extreme angles
        else if (FMath::Abs(ElevationDeg) > 60.0f)
        {
            Quality *= 0.7f;
        }
        else if (ElevationDeg < -45.0f)
        {
            Quality *= 0.75f;
        }
        
        // --- Azimuth-based quality (favor interesting angles over pure cardinal) ---
        
        float DistFromCardinal = FLT_MAX;
        for (float Cardinal : {90.0f, 180.0f, 270.0f})
        {
            float Dist = FMath::Abs(RelativeAzimuthDeg - Cardinal);
            Dist = FMath::Min(Dist, 360.0f - Dist);
            DistFromCardinal = FMath::Min(DistFromCardinal, Dist);
        }
        
        // Bonus for being offset from pure cardinal
        if (DistFromCardinal > 15.0f && DistFromCardinal < 45.0f)
        {
            Quality *= 1.15f;
        }
        else if (DistFromCardinal > 5.0f && DistFromCardinal <= 15.0f)
        {
            Quality *= 1.05f;
        }
        
        return Quality;
    };
    
    // Parallel evaluation of all candidates
    ParallelFor(Candidates.Num(), [&](int32 CandidateIdx)
    {
        FCameraCandidate& Candidate = Candidates[CandidateIdx];
        Candidate.VisibilityMask = EvaluateCameraVisibility(Candidate.Position);
        Candidate.VisibleCount = CountVisibleTexels(Candidate.VisibilityMask);
        Candidate.QualityMultiplier = CalculateQualityMultiplier(Candidate);
    });
    
    // Calculate theoretical maximum coverage (union of Camera 1 + all candidates)
    TBitArray<> TheoreticallyReachable;
    TheoreticallyReachable.Init(false, NumEvalTexels);
    
    // Include Camera 1's visibility
    for (int32 TexelIdx : ValidTexelIndices)
    {
        if (Cam1Visibility[TexelIdx])
        {
            TheoreticallyReachable[TexelIdx] = true;
        }
    }
    
    // Include all candidates' visibility
    for (const FCameraCandidate& Candidate : Candidates)
    {
        for (int32 TexelIdx : ValidTexelIndices)
        {
            if (Candidate.VisibilityMask[TexelIdx])
            {
                TheoreticallyReachable[TexelIdx] = true;
            }
        }
    }
    
    const int32 MaxReachableTexels = CountVisibleTexels(TheoreticallyReachable);
    const float TheoreticalMaxCoverage = (float)MaxReachableTexels / TotalValidTexels;
    
    // Our target is 99% of what's theoretically achievable
    const float TargetCoverageRatio = TheoreticalMaxCoverage * 1.0f;
    
    UE_LOG(LogTemp, Log, TEXT("AutoRig: Theoretical maximum coverage: %.1f%% (%d texels)"), 
        TheoreticalMaxCoverage * 100.f, MaxReachableTexels);
    UE_LOG(LogTemp, Log, TEXT("AutoRig: Target coverage (99%% of max): %.1f%%"), 
        TargetCoverageRatio * 100.f);
    
    // Check if Camera 1 already achieves target
    if (Cam1CoveragePercent >= TargetCoverageRatio)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(
            FString::Printf(TEXT("Camera 1 already achieves %.1f%% coverage.\n\nTheoretical maximum for this mesh is %.1f%%.\n\nNo additional cameras needed."), 
            Cam1CoveragePercent * 100.f,
            TheoreticalMaxCoverage * 100.f)));
        return;
    }
    
    // ========================================================================
    // PHASE 7: GREEDY SELECTION (Quality-weighted coverage)
    // ========================================================================
    
    SlowTask.EnterProgressFrame(1.f, FText::FromString("Selecting optimal cameras..."));
    
    TBitArray<> CoveredTexels = Cam1Visibility;
    TArray<int32> SelectedCandidateIndices;
    TArray<bool> CandidateUsed;
    CandidateUsed.Init(false, Candidates.Num());
    
    const int32 MaxCameras = 10;
    const float MinMarginalPercent = 0.001f; // Stop if no candidate adds more than 1%
    
    auto GetCurrentCoverage = [&]() -> float
    {
        return (float)CountVisibleTexels(CoveredTexels) / TotalValidTexels;
    };
    
    // Mark candidates too close to Camera 1 as used
    for (int32 i = 0; i < Candidates.Num(); ++i)
    {
        float DeltaToCam1 = FMath::Abs(FMath::FindDeltaAngleRadians(Candidates[i].Azimuth, Cam1Azimuth));
        if (DeltaToCam1 < MinAngularSpacing)
        {
            CandidateUsed[i] = true;
        }
    }
    
    while (GetCurrentCoverage() < TargetCoverageRatio && (SelectedCandidateIndices.Num() + 1) < MaxCameras)
    {
        int32 BestCandidateIdx = -1;
        float BestScore = 0.0f;
        int32 BestMarginalCount = 0;
        
        // Find candidate with maximum quality-weighted marginal coverage
        for (int32 i = 0; i < Candidates.Num(); ++i)
        {
            if (CandidateUsed[i]) continue;
            
            const FCameraCandidate& Candidate = Candidates[i];
            const int32 MarginalCount = CountMarginalTexels(Candidate.VisibilityMask, CoveredTexels);
            const float MarginalPercent = (float)MarginalCount / TotalValidTexels;
            
            if (MarginalPercent < MinMarginalPercent) continue;
            
            // Quality-weighted score
            const float Score = MarginalPercent * FMath::Pow(Candidate.QualityMultiplier, 2.0f);
            
            if (Score > BestScore)
            {
                BestScore = Score;
                BestCandidateIdx = i;
                BestMarginalCount = MarginalCount;
            }
        }
        
        if (BestCandidateIdx < 0)
        {
            UE_LOG(LogTemp, Log, TEXT("AutoRig: No more candidates add >%.0f%% coverage. Stopping selection."), 
                MinMarginalPercent * 100.f);
            break;
        }
        
        // Accept this candidate
        const FCameraCandidate& Selected = Candidates[BestCandidateIdx];
        const float MarginalPercent = (float)BestMarginalCount / TotalValidTexels;
        
        SelectedCandidateIndices.Add(BestCandidateIdx);
        CandidateUsed[BestCandidateIdx] = true;
        
        // Update coverage
        for (int32 TexelIdx : ValidTexelIndices)
        {
            if (Selected.VisibilityMask[TexelIdx])
            {
                CoveredTexels[TexelIdx] = true;
            }
        }
        
        // Mark nearby candidates as used (enforce minimum spacing)
        for (int32 i = 0; i < Candidates.Num(); ++i)
        {
            if (CandidateUsed[i]) continue;
            
            float Delta = FMath::Abs(FMath::FindDeltaAngleRadians(Candidates[i].Azimuth, Selected.Azimuth));
            if (Delta < MinAngularSpacing)
            {
                CandidateUsed[i] = true;
            }
        }
        
        const float CurrentCov = GetCurrentCoverage();
        const float PercentOfMax = CurrentCov / TheoreticalMaxCoverage * 100.f;
        
        UE_LOG(LogTemp, Log, TEXT("AutoRig: Selected camera at azimuth %.1f°, elevation %.1f° (quality: %.2fx): +%.1f%% coverage, total %.1f%% (%.1f%% of max)"),
            FMath::RadiansToDegrees(Selected.Azimuth),
            FMath::RadiansToDegrees(Selected.Elevation),
            Selected.QualityMultiplier,
            MarginalPercent * 100.f,
            CurrentCov * 100.f,
            PercentOfMax);
    }
    
    const float FinalCoverage = GetCurrentCoverage();
    const float FinalPercentOfMax = FinalCoverage / TheoreticalMaxCoverage * 100.f;
    
    UE_LOG(LogTemp, Log, TEXT("AutoRig: Selection complete. %d additional cameras, %.1f%% coverage (%.1f%% of theoretical max)"), 
        SelectedCandidateIndices.Num(), FinalCoverage * 100.f, FinalPercentOfMax);
    
    if (SelectedCandidateIndices.Num() == 0)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(
            FString::Printf(TEXT("Camera 1 provides %.1f%% coverage (%.1f%% of the %.1f%% theoretical maximum).\n\nNo additional cameras would significantly improve this."), 
            FinalCoverage * 100.f,
            FinalPercentOfMax,
            TheoreticalMaxCoverage * 100.f)));
        return;
    }
    
    // ========================================================================
    // PHASE 8: SORT BY AZIMUTH FOR TRAVERSAL ORDER
    // ========================================================================
    
    SelectedCandidateIndices.Sort([&](int32 A, int32 B)
    {
        float AzimuthA = Candidates[A].Azimuth;
        float AzimuthB = Candidates[B].Azimuth;
        
        // Normalize relative to Camera 1
        float DeltaA = FMath::FindDeltaAngleRadians(Cam1Azimuth, AzimuthA);
        float DeltaB = FMath::FindDeltaAngleRadians(Cam1Azimuth, AzimuthB);
        
        // Make positive (counter-clockwise from Camera 1)
        if (DeltaA < 0) DeltaA += 2.0f * PI;
        if (DeltaB < 0) DeltaB += 2.0f * PI;
        
        return DeltaA < DeltaB;
    });
    
    // ========================================================================
// PHASE 9: CREATE CAMERA TABS
// ========================================================================

const int32 StartingCameraCount = SlotCameraSettings.Num();

// Get workflow paths
TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("TextureDiffusion3D"));
FString WorkflowsDir;
if (Plugin.IsValid())
{
    WorkflowsDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources"), TEXT("Workflows"));
}

const UTextureDiffusion3DSettings* Settings = GetDefault<UTextureDiffusion3DSettings>();

// Extract prompts from Camera 1's settings to carry over
FString PositivePrompt, NegativePrompt;
if (FPaths::FileExists(Cam1Settings.WorkflowApiJson))
{
    TArray<FUnrealNodeInfo> ParsedNodes;
    ParseWorkflowForUnrealNodes(Cam1Settings.WorkflowApiJson, ParsedNodes);
    
    for (const FUnrealNodeInfo& Node : ParsedNodes)
    {
        const bool bIsPrompt = Node.ExposedInputs.ContainsByPredicate([](const FUnrealInputInfo& I) {
            return I.InputName.Equals(TEXT("text"), ESearchCase::IgnoreCase) ||
                   I.InputName.Equals(TEXT("prompt"), ESearchCase::IgnoreCase);
        });
        
        if (bIsPrompt)
        {
            for (const FString& Key : {FString::Printf(TEXT("%s.text"), *Node.NodeId), 
                                       FString::Printf(TEXT("%s.prompt"), *Node.NodeId)})
            {
                if (const TSharedPtr<FJsonValue>* Val = Cam1Settings.ComfyControlValues.Find(Key))
                {
                    if (Node.CleanTitle.Contains(TEXT("Positive")))
                        PositivePrompt = (*Val)->AsString();
                    else if (Node.CleanTitle.Contains(TEXT("Negative")))
                        NegativePrompt = (*Val)->AsString();
                    break;
                }
            }
        }
    }
}

// Lambda to generate human-readable direction descriptor
auto GenerateDirectionDescriptor = [&](const FCameraCandidate& Cam) -> FString
{
    // Calculate relative azimuth from Camera 1 (in degrees, -180 to 180)
    float RelativeAzimuthDeg = FMath::RadiansToDegrees(
        FMath::FindDeltaAngleRadians(Cam1Azimuth, Cam.Azimuth));
    
    // Elevation in degrees
    float ElevationDeg = FMath::RadiansToDegrees(Cam.Elevation);
    
    // Determine horizontal direction
    FString HorizontalDir;
    if (RelativeAzimuthDeg >= -22.5f && RelativeAzimuthDeg < 22.5f)
        HorizontalDir = TEXT("front");
    else if (RelativeAzimuthDeg >= 22.5f && RelativeAzimuthDeg < 67.5f)
        HorizontalDir = TEXT("front-left");
    else if (RelativeAzimuthDeg >= 67.5f && RelativeAzimuthDeg < 112.5f)
        HorizontalDir = TEXT("left side");
    else if (RelativeAzimuthDeg >= 112.5f && RelativeAzimuthDeg < 157.5f)
        HorizontalDir = TEXT("back-left");
    else if (RelativeAzimuthDeg >= 157.5f || RelativeAzimuthDeg < -157.5f)
        HorizontalDir = TEXT("back");
    else if (RelativeAzimuthDeg >= -157.5f && RelativeAzimuthDeg < -112.5f)
        HorizontalDir = TEXT("back-right");
    else if (RelativeAzimuthDeg >= -112.5f && RelativeAzimuthDeg < -67.5f)
        HorizontalDir = TEXT("right side");
    else // -67.5 to -22.5
        HorizontalDir = TEXT("front-right");
    
    // Determine vertical direction
    FString VerticalDir;
    if (ElevationDeg > 25.0f)
        VerticalDir = TEXT("high");
    else if (ElevationDeg < -25.0f)
        VerticalDir = TEXT("low");
    else
        VerticalDir = TEXT(""); // Eye level, no descriptor needed
    
    // Combine into final descriptor
    if (VerticalDir.IsEmpty())
        return FString::Printf(TEXT(", %s view"), *HorizontalDir);
    else
        return FString::Printf(TEXT(", %s %s view"), *HorizontalDir, *VerticalDir);
};

// Create camera tabs for selected candidates
for (int32 i = 0; i < SelectedCandidateIndices.Num(); ++i)
{
    const FCameraCandidate& Candidate = Candidates[SelectedCandidateIndices[i]];
    
    FProjectionSettings NewCam;
    NewCam.CameraPosition = Candidate.Position;
    NewCam.CameraRotation = Candidate.Rotation;
    NewCam.FOVAngle = Cam1Settings.FOVAngle;
    NewCam.TargetMaterialSlotIndex = CurrentSlotIndex;
    NewCam.TabId = NextTabId++;
    NewCam.TabName = FString::Printf(TEXT("Camera %d"), StartingCameraCount + i + 1);
    NewCam.FadeStartAngle = Cam1Settings.FadeStartAngle;
    NewCam.EdgeFalloff = Cam1Settings.EdgeFalloff;
    
    // Use inpaint workflow for all auto-generated cameras
    FString WorkflowPath = Settings->InpaintProjectionWorkflow.FilePath;
    if (WorkflowPath.IsEmpty())
    {
        WorkflowPath = FPaths::Combine(WorkflowsDir, TEXT("Fooocus_Inpaint.json"));
    }
    
    if (!FPaths::FileExists(WorkflowPath))
    {
        UE_LOG(LogTemp, Error, TEXT("Workflow not found: %s"), *WorkflowPath);
        continue;
    }
    
    NewCam.WorkflowApiJson = WorkflowPath;
    NewCam.ComfyControlValues.Empty();
    
	// Generate the direction descriptor for THIS camera
	FString DirectionDescriptor = GenerateDirectionDescriptor(Candidate);

	// Inject prompts from Camera 1 (with direction descriptor for positive prompt)
	TArray<FUnrealNodeInfo> NewNodes;
	ParseWorkflowForUnrealNodes(WorkflowPath, NewNodes);

	for (const FUnrealNodeInfo& Node : NewNodes)
	{
		const bool bIsPrompt = Node.ExposedInputs.ContainsByPredicate([](const FUnrealInputInfo& I) {
			return I.InputName.Equals(TEXT("text"), ESearchCase::IgnoreCase) ||
				I.InputName.Equals(TEXT("prompt"), ESearchCase::IgnoreCase);
		});
		
		if (bIsPrompt)
		{
			FString InputKey = TEXT("text");
			for (const auto& I : Node.ExposedInputs)
			{
				if (I.InputName.Equals(TEXT("prompt"), ESearchCase::IgnoreCase))
				{
					InputKey = TEXT("prompt");
					break;
				}
			}
			
			const FString ControlKey = FString::Printf(TEXT("%s.%s"), *Node.NodeId, *InputKey);
			
			// Append direction descriptor to positive prompts (even if base prompt is empty)
			if (Node.CleanTitle.Contains(TEXT("Positive")))
			{
				// If PositivePrompt is empty, just use the descriptor (without leading comma)
				FString ModifiedPrompt;
				if (PositivePrompt.IsEmpty())
				{
					// Remove the leading ", " from the descriptor
					ModifiedPrompt = DirectionDescriptor.RightChop(2); // Removes ", " prefix
				}
				else
				{
					ModifiedPrompt = PositivePrompt + DirectionDescriptor;
				}
				NewCam.ComfyControlValues.Add(ControlKey, MakeShared<FJsonValueString>(ModifiedPrompt));
				UE_LOG(LogTemp, Log, TEXT("AutoRig: Camera %d prompt: \"%s\""), StartingCameraCount + i + 1, *ModifiedPrompt);
			}
			else if (Node.CleanTitle.Contains(TEXT("Negative")))
			{
				if (!NegativePrompt.IsEmpty())
				{
					NewCam.ComfyControlValues.Add(ControlKey, MakeShared<FJsonValueString>(NegativePrompt));
				}
			}
		}
	}
    
    SlotCameraSettings.Add(NewCam);
    
    // Sync data structures
    auto& SlotVariantMap = PerVariantProjectionLayers.FindOrAdd(CurrentSlotIndex);
    SlotVariantMap.FindOrAdd(EProjectionVariant::BaseColor).AddDefaulted();
    SlotVariantMap.FindOrAdd(EProjectionVariant::Normal).AddDefaulted();
    SlotVariantMap.FindOrAdd(EProjectionVariant::Shaded).AddDefaulted();
    SlotVariantMap.FindOrAdd(EProjectionVariant::Roughness).AddDefaulted();
    SlotVariantMap.FindOrAdd(EProjectionVariant::Metallic).AddDefaulted();
    SlotVariantMap.FindOrAdd(EProjectionVariant::AO).AddDefaulted();
    PerSlotWeightLayers.FindOrAdd(CurrentSlotIndex).AddDefaulted();
}
    
    // ========================================================================
    // PHASE 10: FINAL REPORT
    // ========================================================================
    
    // Build camera summary
    FString CameraSummary = TEXT("\n\nCamera angles (relative to Camera 1):");
    for (int32 i = 0; i < SelectedCandidateIndices.Num(); ++i)
    {
        const FCameraCandidate& Candidate = Candidates[SelectedCandidateIndices[i]];
        float RelativeAzimuth = FMath::RadiansToDegrees(FMath::FindDeltaAngleRadians(Cam1Azimuth, Candidate.Azimuth));
        float ElevationDeg = FMath::RadiansToDegrees(Candidate.Elevation);
        
        CameraSummary += FString::Printf(TEXT("\n  Camera %d: %.0f° around, %.0f° elevation (quality: %.2fx)"),
            StartingCameraCount + i + 1,
            RelativeAzimuth,
            ElevationDeg,
            Candidate.QualityMultiplier);
    }
    
    // Build coverage report
    FString CoverageReport;
    if (TheoreticalMaxCoverage < 0.99f)
    {
        // Mesh has unreachable areas
        const float UnreachablePercent = (1.0f - TheoreticalMaxCoverage) * 100.f;
        CoverageReport = FString::Printf(
            TEXT("\n\nNote: %.1f%% of this mesh's surface is unreachable from any camera angle (deep recesses, interior faces, etc.)."),
            UnreachablePercent);
    }
    
    FString CoverageWarning;
    if (FinalPercentOfMax < 95.0f)
    {
        CoverageWarning = TEXT("\n\nWarning: Could not achieve 95% of theoretical maximum. Consider adding manual cameras for remaining gaps.");
    }
    
    FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(
        FString::Printf(TEXT("Generated %d cameras.\n\nCoverage: %.1f%% absolute (%.1f%% of %.1f%% theoretical maximum)%s%s%s"), 
        SelectedCandidateIndices.Num() + 1, // +1 for Camera 1
        FinalCoverage * 100.f,
        FinalPercentOfMax,
        TheoreticalMaxCoverage * 100.f,
        *CameraSummary,
        *CoverageReport,
        *CoverageWarning)));
}

void FTextureDiffusion3D::OnTargetMaterialSlotChanged(int32 NewSlotIndex)
{
	UE_LOG(LogTemp, Log, TEXT("Switched active material slot to %d"), NewSlotIndex);

	PrecomputedWorldPositionBuffer.Empty();
	PrecomputedUVBuffer.Empty();
	// Get the camera list for the NEW slot
	TArray<FProjectionSettings>& NewSlotCameraSettings = PerSlotCameraSettings.FindOrAdd(NewSlotIndex);
	TObjectPtr<UTexture2D>* FoundTexture = AllProjectedTextures.Find(NewSlotIndex);
	ProjectedTexture = FoundTexture ? *FoundTexture : nullptr;

	if (NewSlotCameraSettings.Num() == 0)
	{
		// Temporarily update current settings to the new slot before adding a tab
		CurrentSettings.TargetMaterialSlotIndex = NewSlotIndex;
		AddCameraTab(); 
	}
	else
	{
		// Otherwise, just make the first camera in that list the active one
		ActiveCameraIndex = 0;
		CurrentSettings = NewSlotCameraSettings[ActiveCameraIndex];
	}
	
	// Refresh the entire UI with the data for the new slot
	if (SettingsWidget.IsValid())
	{
		SettingsWidget->SetCameraSettings(
			PerSlotCameraSettings.FindOrAdd(NewSlotIndex),
			ActiveCameraIndex,
			Global_OutputPath,
			Global_OutputTextureWidth,
			Global_OutputTextureHeight
		);

		const bool bIsHidden = SlotHiddenStates.FindRef(NewSlotIndex);
		SettingsWidget->UpdateOccluderCheckbox(bIsHidden);
	}
}



void FTextureDiffusion3D::AddCameraTab()
{
	// Get the camera array for the currently active material slot.
	TArray<FProjectionSettings>& SlotCameraSettings = PerSlotCameraSettings.FindOrAdd(CurrentSettings.TargetMaterialSlotIndex);

	FProjectionSettings NewSettings;
	const int32 NewCameraNumber = SlotCameraSettings.Num() + 1;

	// --- No changes to this section: Logic for finding workflows and carrying over prompts is the same ---
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("TextureDiffusion3D"));
	FString WorkflowsDir;
	if (Plugin.IsValid())
	{
		WorkflowsDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources"), TEXT("Workflows"));
	}
	const UTextureDiffusion3DSettings* Settings = GetDefault<UTextureDiffusion3DSettings>();
	const FProjectionSettings OldSettings = CurrentSettings;
	FString PositivePromptToCarryOver, NegativePromptToCarryOver;
	if (SlotCameraSettings.Num() == 0)
	{
		UE_LOG(LogTemp, Log, TEXT("AddCameraTab: Creating initial camera for slot %d."), CurrentSettings.TargetMaterialSlotIndex);
		const FBoxSphereBounds Bounds = SelectedActor->GetComponentsBoundingBox(true);
		const FVector ActorForwardVector = SelectedActor->GetActorRightVector();
		const float OffsetDistance = Bounds.SphereRadius * 2.5f;
		// Position the camera along the actor's right vector, offset from the bounds origin
		NewSettings.CameraPosition = Bounds.Origin + (ActorForwardVector * OffsetDistance);
		NewSettings.CameraRotation = (Bounds.Origin - NewSettings.CameraPosition).Rotation();
		NewSettings.CameraRotation.Normalize();
		NewSettings.FOVAngle = 45.0f;
		FString FinalWorkflowPath = Settings->InitialProjectionWorkflow.FilePath;
		if (FinalWorkflowPath.IsEmpty())
		{
			FinalWorkflowPath = FPaths::Combine(WorkflowsDir, TEXT("Juggernaut_ControlNet.json"));
		}
		NewSettings.WorkflowApiJson = FinalWorkflowPath;
		
		if (!FPaths::FileExists(NewSettings.WorkflowApiJson))
		{
			FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString::Printf(TEXT("Initial Projection Workflow file was not found!\n\nPlease check the path in your settings.\n\nPath Checked:\n%s"), *NewSettings.WorkflowApiJson)));
			return;
		}
	}
	else 
	{
		UE_LOG(LogTemp, Log, TEXT("AddCameraTab: Creating subsequent camera for slot %d."), CurrentSettings.TargetMaterialSlotIndex);
		NewSettings = CurrentSettings;
		FString FinalWorkflowPath = Settings->InpaintProjectionWorkflow.FilePath;
		if (FinalWorkflowPath.IsEmpty())
		{
			FinalWorkflowPath = FPaths::Combine(WorkflowsDir, TEXT("Fooocus_Inpaint.json"));
		}
		NewSettings.WorkflowApiJson = FinalWorkflowPath;
		if (!FPaths::FileExists(NewSettings.WorkflowApiJson))
		{
			FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString::Printf(TEXT("Inpaint Workflow file was not found!\n\nPlease check the path in your settings.\n\nPath Checked:\n%s"),*NewSettings.WorkflowApiJson)));
			return;
		}
		if (FPaths::FileExists(OldSettings.WorkflowApiJson))
		{
			TArray<FUnrealNodeInfo> OldParsedNodes;
			ParseWorkflowForUnrealNodes(OldSettings.WorkflowApiJson, OldParsedNodes);
			for (const FUnrealNodeInfo& NodeInfo : OldParsedNodes)
			{
				bool bIsPromptNode = NodeInfo.ExposedInputs.ContainsByPredicate(
					[](const FUnrealInputInfo& Input) { 
						return Input.InputName.Equals(TEXT("text"), ESearchCase::IgnoreCase) || 
							Input.InputName.Equals(TEXT("prompt"), ESearchCase::IgnoreCase); 
					}
				);
				if (bIsPromptNode)
				{
					const FString TextKey = FString::Printf(TEXT("%s.text"), *NodeInfo.NodeId);
					const FString PromptKey = FString::Printf(TEXT("%s.prompt"), *NodeInfo.NodeId);

					const TSharedPtr<FJsonValue>* FoundValue = OldSettings.ComfyControlValues.Find(TextKey);
					if (!FoundValue)
					{
						FoundValue = OldSettings.ComfyControlValues.Find(PromptKey);
					}

					if (FoundValue && (*FoundValue).IsValid())
					{
						if (NodeInfo.CleanTitle.Contains(TEXT("Positive"), ESearchCase::IgnoreCase)) { PositivePromptToCarryOver = (*FoundValue)->AsString(); }
						else if (NodeInfo.CleanTitle.Contains(TEXT("Negative"), ESearchCase::IgnoreCase)) { NegativePromptToCarryOver = (*FoundValue)->AsString(); }
					}
				}
			}
		}
		if (FPaths::FileExists(NewSettings.WorkflowApiJson))
		{
			TArray<FUnrealNodeInfo> NewParsedNodes;
			ParseWorkflowForUnrealNodes(NewSettings.WorkflowApiJson, NewParsedNodes);
			for (const FUnrealNodeInfo& NodeInfo : NewParsedNodes)
			{
				bool bIsPromptNode = NodeInfo.ExposedInputs.ContainsByPredicate(
					[](const FUnrealInputInfo& Input) { 
						return Input.InputName.Equals(TEXT("text"), ESearchCase::IgnoreCase) || 
							Input.InputName.Equals(TEXT("prompt"), ESearchCase::IgnoreCase); 
					}
				);
				if (bIsPromptNode)
				{
					FString PromptInputKey = TEXT("text"); // Default
					for(const auto& Input : NodeInfo.ExposedInputs)
					{
						if(Input.InputName.Equals(TEXT("prompt"), ESearchCase::IgnoreCase))
						{
							PromptInputKey = TEXT("prompt");
							break;
						}
					}
					const FString ControlKey = FString::Printf(TEXT("%s.%s"), *NodeInfo.NodeId, *PromptInputKey);
					if (!PositivePromptToCarryOver.IsEmpty() && NodeInfo.CleanTitle.Contains(TEXT("Positive"), ESearchCase::IgnoreCase))
					{
						NewSettings.ComfyControlValues.Emplace(ControlKey, MakeShared<FJsonValueString>(PositivePromptToCarryOver));
					}
					if (!NegativePromptToCarryOver.IsEmpty() && NodeInfo.CleanTitle.Contains(TEXT("Negative"), ESearchCase::IgnoreCase))
					{
						NewSettings.ComfyControlValues.Emplace(ControlKey, MakeShared<FJsonValueString>(NegativePromptToCarryOver));
					}
					
				}
			}
		}
	}
	// --- End of unchanged section ---

	NewSettings.TabId = NextTabId++;
	NewSettings.TabName = FString::Printf(TEXT("Camera %d"), NewCameraNumber);
	NewSettings.TargetMaterialSlotIndex = CurrentSettings.TargetMaterialSlotIndex;

	SlotCameraSettings.Add(NewSettings);
	ActiveCameraIndex = SlotCameraSettings.Num() - 1;
	CurrentSettings = NewSettings;

	// *** CORRECTED DATA SYNC LOGIC ***
	// Get the map of all variants for the current material slot.
	TMap<EProjectionVariant, TArray<TArray<FLinearColor>>>& SlotVariantMap = PerVariantProjectionLayers.FindOrAdd(CurrentSettings.TargetMaterialSlotIndex);

	// Add a placeholder for every known variant. This ensures that no matter which variant is
	// projected first for this new camera, its slot in the array already exists.
	SlotVariantMap.FindOrAdd(EProjectionVariant::BaseColor).AddDefaulted();
	SlotVariantMap.FindOrAdd(EProjectionVariant::Normal).AddDefaulted();
	SlotVariantMap.FindOrAdd(EProjectionVariant::Shaded).AddDefaulted();
	SlotVariantMap.FindOrAdd(EProjectionVariant::Roughness).AddDefaulted();
	SlotVariantMap.FindOrAdd(EProjectionVariant::Metallic).AddDefaulted();
	SlotVariantMap.FindOrAdd(EProjectionVariant::AO).AddDefaulted();

	// The weights map is variant-agnostic and already keyed by Slot.
	PerSlotWeightLayers.FindOrAdd(CurrentSettings.TargetMaterialSlotIndex).AddDefaulted();
	// *** END OF CORRECTION ***

	if (SettingsWidget.IsValid())
	{
		SettingsWidget->SetCameraSettings(SlotCameraSettings, ActiveCameraIndex, Global_OutputPath, Global_OutputTextureWidth, Global_OutputTextureHeight);
	}
		
	UE_LOG(LogTemp, Log, TEXT("Added new camera tab. Total cameras for slot %d: %d."), CurrentSettings.TargetMaterialSlotIndex, SlotCameraSettings.Num());
}

void FTextureDiffusion3D::RecalculateAllWeightsAndReblend(const TArray<FProjectionSettings>& NewCameraSettingsForSlot)
{
	if (!CurrentProjection_Actor || !bHasStartedProjections) return;

	const int32 TargetSlot = CurrentProjection_Settings.TargetMaterialSlotIndex;
	PerSlotCameraSettings.FindOrAdd(TargetSlot) = NewCameraSettingsForSlot;
	TArray<FProjectionSettings>& SlotCameraSettings = PerSlotCameraSettings.FindOrAdd(TargetSlot);
	TArray<TArray<float>>& SlotWeightLayers = PerSlotWeightLayers.FindOrAdd(TargetSlot);
	TSet<int32> HiddenSlotIndices;
	for (auto const& Elem : SlotHiddenStates) { if (Elem.Value) HiddenSlotIndices.Add(Elem.Key); }

	for (int32 i = 0; i < SlotCameraSettings.Num(); ++i)
	{
		if (SlotWeightLayers.IsValidIndex(i))
		{
			const FProjectionSettings& CurrentCamSettings = SlotCameraSettings[i];
			TArray<float> NewNormalWeightBuffer;
			TArray<FVector2D> ScreenPositionBuffer;
			TArray<bool> VisibilityBuffer;		

			CalculateCameraWeights(CurrentProjection_StaticMesh, CurrentProjection_MeshComponent->GetComponentTransform(),
				CurrentCamSettings, CurrentProjection_TextureWidth, CurrentProjection_TextureHeight,
				NewNormalWeightBuffer, ScreenPositionBuffer, VisibilityBuffer, TargetUVChannel, HiddenSlotIndices);
			
			SlotWeightLayers[i] = NewNormalWeightBuffer;

			// *** CORRECTED TYPE FOR RefreshAlpha ***
			auto RefreshAlpha = [&](TArray<TArray<FLinearColor>>& Layers)
			{
				if (!Layers.IsValidIndex(i) || Layers[i].Num() != NewNormalWeightBuffer.Num()) return;
				
				TArray<FLinearColor>& L = Layers[i];
				for (int32 t = 0; t < L.Num(); ++t)
				{
					L[t].A = FMath::Clamp(NewNormalWeightBuffer[t], 0.0f, 1.0f);
				}
			};

			if (TMap<EProjectionVariant, TArray<TArray<FLinearColor>>>* SlotVariantMap = PerVariantProjectionLayers.Find(TargetSlot))
			{
				for(auto& Pair : *SlotVariantMap)
				{
					RefreshAlpha(Pair.Value); // Pair.Value is TArray<TArray<FLinearColor>>
				}
			}
		}
	}

	ReblendAndUpdateMesh();
}

void FTextureDiffusion3D::SelectCameraTab(int32 TabIndex)
{
	// Get the camera array for the currently active material slot.
	TArray<FProjectionSettings>& SlotCameraSettings = PerSlotCameraSettings.FindOrAdd(CurrentSettings.TargetMaterialSlotIndex);

	if (!SlotCameraSettings.IsValidIndex(TabIndex)) return;
	
	ActiveCameraIndex = TabIndex;
	CurrentSettings = SlotCameraSettings[TabIndex]; // Get settings from the correct array
	
	// Update camera preview
	if (CaptureActor && SelectedActor)
	{
		FCameraHelper::PositionCaptureCamera(CaptureActor, SelectedActor, CurrentSettings);
	}

	// Refresh the UI
	if (SettingsWidget.IsValid())
	{
		SettingsWidget->SetCameraSettings(SlotCameraSettings, ActiveCameraIndex, Global_OutputPath, Global_OutputTextureWidth, Global_OutputTextureHeight);
	}
	
	UE_LOG(LogTemp, Log, TEXT("Selected camera tab %d for material slot %d"), TabIndex, CurrentSettings.TargetMaterialSlotIndex);
}

void FTextureDiffusion3D::OnCameraTabAdded()
{
	// Add a new tab
	AddCameraTab();
	
	UE_LOG(LogTemp, Log, TEXT("Added new camera tab via UI callback"));
}

void FTextureDiffusion3D::OnCameraTabRemoved(int32 TabIndex)
{
	// Remove the tab
	RemoveCameraTab(TabIndex);
	
	UE_LOG(LogTemp, Log, TEXT("Removed camera tab %d via UI callback"), TabIndex);

	
}


FLinearColor FTextureDiffusion3D::GetBaseColorForSlot(int32 SlotIndex) const
{
	if (const FLinearColor* C = PerSlotBaseColor.Find(SlotIndex)) return *C;
	return FLinearColor::Black;
}

// setter
void FTextureDiffusion3D::SetBaseColorForSlot(int32 SlotIndex, FLinearColor NewColor)
{
	PerSlotBaseColor.FindOrAdd(SlotIndex) = NewColor;
	// ReblendAndUpdateMesh();
}
void FTextureDiffusion3D::SetActiveVariant(int32 SlotIndex, bool bIsLit)
{
    UE_LOG(LogTemp, Log, TEXT("Setting active variant for slot %d to: %s"), SlotIndex, bIsLit ? TEXT("Lit (Shaded)") : TEXT("Unlit (BaseColor)"));
    PerSlotLitState.FindOrAdd(SlotIndex) = bIsLit;

    // Re-apply the material with the correct texture
    ReblendAndUpdateMesh();
}

bool FTextureDiffusion3D::GetActiveVariantState() const
{
    // The invalid check has been removed.
    const int32 CurrentSlotIndex = CurrentSettings.TargetMaterialSlotIndex;
    // FindRef correctly returns false if the key doesn't exist, which is the desired default behavior.
    return PerSlotLitState.FindRef(CurrentSlotIndex);
}

void FTextureDiffusion3D::OnOutputResolutionChanged_Handler(int32 NewWidth, int32 NewHeight)
{
	Global_OutputTextureWidth = NewWidth;
	Global_OutputTextureHeight = NewHeight;
	UE_LOG(LogTemp, Log, TEXT("Global output resolution updated to %d x %d"), Global_OutputTextureWidth, Global_OutputTextureHeight);
}



void FTextureDiffusion3D::OnOccluderCheckboxChanged(bool bIsHidden)
{
	const int32 CurrentSlotIndex = CurrentSettings.TargetMaterialSlotIndex;
	UStaticMeshComponent* MeshComp = SelectedActor ? SelectedActor->GetStaticMeshComponent() : nullptr;
	if (!MeshComp) return;

	UE_LOG(LogTemp, Log, TEXT("OnOccluderCheckboxChanged: Slot %d is now %s"), CurrentSlotIndex, bIsHidden ? TEXT("hidden") : TEXT("visible"));

	if (bIsHidden)
	{
		UE_LOG(LogTemp, Log, TEXT("OnOccluderCheckboxChanged: Hiding slot %d"), CurrentSlotIndex);
		// Hide the slot by applying the invisible material.
		UMaterialInterface* InvisibleMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/TextureDiffusion3D/Materials/M_Invis.M_Invis"));
		if (!InvisibleMaterial)
		{
			UE_LOG(LogTemp, Error, TEXT("OnOccluderCheckboxChanged: Failed to load invisible material!"));
			return;
		}
		else{
			UE_LOG(LogTemp, Log, TEXT("OnOccluderCheckboxChanged: Applying invisible material to slot %d"), CurrentSlotIndex);
		}
		if (InvisibleMaterial)
		{
			MeshComp->SetMaterial(CurrentSlotIndex, InvisibleMaterial);
		}
	}
	else
	{
		// Show the slot by restoring the original material from our map.
		if (TObjectPtr<UMaterialInterface>* FoundMat = OriginalActorMaterials.Find(CurrentSlotIndex))
		{
			MeshComp->SetMaterial(CurrentSlotIndex, *FoundMat);
		}
	}
	
	// Save the state to the map.
	SlotHiddenStates.Add(CurrentSlotIndex, bIsHidden);
	UE_LOG(LogTemp, Log, TEXT("Slot %d hidden state set to: %s"), CurrentSlotIndex, bIsHidden ? TEXT("true") : TEXT("false"));

	if (SettingsWidget.IsValid())
	{
		SettingsWidget->UpdateCameraPreview();
	}
}


void FTextureDiffusion3D::ResetProjection()
{
	UE_LOG(LogTemp, Warning, TEXT("--- Resetting All Projection Data ---"));

	// --- 1. Cancel any in-progress async operations ---

	// Stop any active polling ticker for remote ComfyUI jobs
	if (PollingTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(PollingTickerHandle);
		PollingTickerHandle.Reset();
		UE_LOG(LogTemp, Log, TEXT("Reset: Stopped ComfyUI polling ticker."));
	}
	if (PollingSlowTask.IsValid())
	{
		// FIX: Just reset the TUniquePtr. This destroys the slow task object.
		PollingSlowTask.Reset();
		UE_LOG(LogTemp, Log, TEXT("Reset: Destroyed active PollingSlowTask."));
	}
	
    // --- REMOVED TimerBake_TimerHandle cleanup, as it's not a member ---

	// Stop any other misc tickers (if they were used)
	if (DelayedCaptureTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(DelayedCaptureTickerHandle);
		DelayedCaptureTickerHandle.Reset();
	}
	if (BlockingCaptureTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(BlockingCaptureTickerHandle);
		BlockingCaptureTickerHandle.Reset();
	}


	// --- 2. Restore original materials ---
	if (SelectedActor)
	{
		if (UStaticMeshComponent* MeshComp = SelectedActor->GetStaticMeshComponent())
		{
			if (OriginalActorMaterials.Num() > 0)
			{
				// Restore the exact original materials we captured
				for (const auto& Elem : OriginalActorMaterials)
				{
                    // --- FIX for IsValidSlotIndex ---
                    // Check if the index is valid for the component's material list
					if (Elem.Key >= 0 && Elem.Key < MeshComp->GetNumMaterials())
					{
						MeshComp->SetMaterial(Elem.Key, Elem.Value);
					}
				}
				UE_LOG(LogTemp, Log, TEXT("Reset: Restored %d original materials."), OriginalActorMaterials.Num());
			}
			else if (UStaticMesh* StaticMeshAsset = MeshComp->GetStaticMesh())
			{
				// Fall back to the mesh’s asset defaults for ALL slots
				const int32 NumSlots = MeshComp->GetNumMaterials();
				for (int32 SlotIdx = 0; SlotIdx < NumSlots; ++SlotIdx)
				{
					UMaterialInterface* DefaultMat = StaticMeshAsset->GetMaterial(SlotIdx);
					MeshComp->SetMaterial(SlotIdx, DefaultMat);
				}
				UE_LOG(LogTemp, Log, TEXT("Reset: No original materials found, restored %d asset defaults."), NumSlots);
			}
		}
	}

	// --- 3. Clear all cached data and state variables ---
	UE_LOG(LogTemp, Log, TEXT("Reset: Clearing all cached state..."));
	
	OriginalActorMaterials.Empty();
	AllProjectedTextures.Empty();

	// Clear transient texture caches
	for (auto& SlotPair : PerSlotFinal)
	{
		TMap<EProjectionVariant, TObjectPtr<UTexture2D>>& VariantMap = SlotPair.Value;
		for (auto& VariantPair : VariantMap)
		{
			if (IsValid(VariantPair.Value))
			{
				VariantPair.Value->RemoveFromRoot();
			}
		}
	}
	PerSlotFinal.Empty();

	// Clear pixel data caches
	PerSlotWeightLayers.Empty();
	PerVariantProjectionLayers.Empty();
	PrecomputedWorldPositionBuffer.Empty();
	PrecomputedUVBuffer.Empty();
	PrecomputedUVIslandMap.Empty();

	// Clear state maps
	SlotHiddenStates.Empty();
	PerSlotBaseColor.Empty();
	PerSlotReferenceImagePath.Empty();
	PerSlotLitState.Empty();
	PerSlotCameraSettings.Empty();
	
    // --- REMOVED Async/Bake state variables ---
	// GpuBake_AccumulatedResults.Reset();
	// GpuBake_PendingBakes.Reset();
	// GpuBake_StoredWeightBufferForFinalize.Empty();
    // TimerBake_MID = nullptr;
	// TimerBake_RT = nullptr;
	// TimerBake_WeightBuffer.Empty();
	// TimerBake_VisibilityBuffer.Empty();
	// TimerBake_CompletionCallback = nullptr;

	// Clear global settings
	Global_OutputPath = FPaths::ProjectSavedDir() / TEXT("TextureExports");
	Global_OutputTextureWidth = 1024;
	Global_OutputTextureHeight = 1024;
    TargetUVChannel = 0; // Reset UV channel as well

	// Clear projection context
	bHasStartedProjections = false;
	ProjectedTexture = nullptr;
	CurrentSettings = FProjectionSettings();
	ActiveCameraIndex = 0;
	NextTabId = 0;
	bIsBatchMode = false;
	if (OnSingleProjectionFinished.IsBound())
	{
		OnSingleProjectionFinished.Unbind();
	}

	// --- 4. Clean up UObject resources ---

	// Clear Material Instance cache
	for (auto& KVP : PerSlotMIDs)
	{
		if (IsValid(KVP.Value)) { KVP.Value->RemoveFromRoot(); }
	}
	PerSlotMIDs.Empty();

	// Clear Position Map cache
	for (auto& Pair : CachedPositionMaps)
	{
		if (IsValid(Pair.Value))
		{
			Pair.Value->RemoveFromRoot(); // Stop protecting from GC
		}
	}
	CachedPositionMaps.Empty();
	UE_LOG(LogTemp, Log, TEXT("Cleared cached Position Maps and MIDs."));
	
	// --- 5. Close UI ---
	if (SettingsWidget.IsValid())
	{
		if (SettingsWindow.IsValid())
		{
			SettingsWindow->RequestDestroyWindow();
		}
		SettingsWindow = nullptr;
		SettingsWidget = nullptr;
	}

	UE_LOG(LogTemp, Warning, TEXT("--- Projection data reset complete ---"));
}

/**
 * Correctly blends multiple normal map layers by performing a weighted average of the vectors.
 * @param CameraProjections_Linear An array of normal map layers, where each FLinearColor represents a packed normal vector.
 * @param ProcessedWeights A per-texel, per-camera array of final blend weights.
 * @param TextureWidth The width of the textures.
 * @param TextureHeight The height of the textures.
 * @return A new TArray<FLinearColor> containing the correctly blended and packed normal vectors.
 * */
/**
 * Correctly blends multiple normal map layers by performing a weighted average of the vectors.
 * @param CameraProjections_Linear An array of normal map layers, where each FLinearColor represents a packed normal vector.
 * @param ProcessedWeights A per-texel, per-camera array of final blend weights.
 * @param TextureWidth The width of the textures.
 * @param TextureHeight The height of the textures.
 * @return A new TArray<FLinearColor> containing the correctly blended and packed normal vectors.
 */
TArray<FLinearColor> FTextureDiffusion3D::CreateBlendedNormalTexture(
    const TArray<TArray<FLinearColor>>& CameraProjections_Linear,
    const TArray<TArray<float>>& ProcessedWeights,
    int32 TextureWidth,
    int32 TextureHeight)
{
    const int32 NumTexels = TextureWidth * TextureHeight;
    TArray<FLinearColor> BlendedProjection_Linear;
    // Initialize with a flat normal (0.5, 0.5, 1.0) as a fallback.
    BlendedProjection_Linear.Init(FLinearColor(0.5f, 0.5f, 1.0f, 1.0f), NumTexels);

    if (CameraProjections_Linear.Num() == 0 || ProcessedWeights.Num() != NumTexels)
    {
        return BlendedProjection_Linear;
    }

    // Helper lambda to unpack a 0-1 FLinearColor into a -1 to 1 FVector.
    auto UnpackNormal = [](const FLinearColor& C) -> FVector {
        return FVector(
            C.R * 2.0f - 1.0f,
            C.G * 2.0f - 1.0f,
            C.B * 2.0f - 1.0f
        );
    };

    // Helper lambda to pack a -1 to 1 FVector back into a 0-1 FLinearColor.
    auto PackNormal = [](const FVector& N) -> FLinearColor {
        return FLinearColor(
            N.X * 0.5f + 0.5f,
            N.Y * 0.5f + 0.5f,
            N.Z * 0.5f + 0.5f,
            1.0f // Keep alpha at 1.0 for the final normal map
        );
    };

    for (int32 TexelIndex = 0; TexelIndex < NumTexels; TexelIndex++)
    {
        FVector SummedVector = FVector::ZeroVector;
        float TotalWeight = 0.0f; // Keep track of weight sum for texels with partial coverage

        for (int32 CamIndex = 0; CamIndex < CameraProjections_Linear.Num(); CamIndex++)
        {
            const float BlendWeight = ProcessedWeights[TexelIndex][CamIndex];
            if (BlendWeight > KINDA_SMALL_NUMBER)
            {
                // Unpack the normal from the current projection layer.
                const FVector NormalVector = UnpackNormal(CameraProjections_Linear[CamIndex][TexelIndex]);

                // Add the weighted vector to our sum.
                SummedVector += NormalVector * BlendWeight;
                TotalWeight += BlendWeight;
            }
        }

        if (TotalWeight > KINDA_SMALL_NUMBER)
        {
            // Normalize the final summed vector to ensure it's a valid unit vector.
            const FVector FinalNormal = SummedVector.GetSafeNormal();

            // Pack the result back into a color and store it.
            BlendedProjection_Linear[TexelIndex] = PackNormal(FinalNormal);
        }
    }

    return BlendedProjection_Linear;
}


void FTextureDiffusion3D::ReblendAndUpdateMesh()
{
    UE_LOG(LogTemp, Warning, TEXT("--- [CORE LOGIC] ReblendAndUpdateMesh ---"));

    // --- 1. VALIDATION & SETUP ---
    if (!SelectedActor || !bHasStartedProjections)
    {
        UE_LOG(LogTemp, Log, TEXT("Reblend: Aborting. No selected actor or no projections have been run yet."));
        return;
    }

    UStaticMeshComponent* MeshComp = SelectedActor->GetStaticMeshComponent();
    if (!MeshComp || Global_OutputTextureWidth == 0 || Global_OutputTextureHeight == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("Reblend: Aborting. Invalid mesh component or texture dimensions."));
        return;
    }

    const int32 TargetSlot = CurrentSettings.TargetMaterialSlotIndex;
    const TArray<EProjectionVariant> VariantsToProcess = { 
        EProjectionVariant::BaseColor, 
        EProjectionVariant::Normal, 
        EProjectionVariant::Shaded,
        EProjectionVariant::Roughness, 
        EProjectionVariant::Metallic  
    };
    bool bAnyLayersWereBlended = false;

    // --- 2. CLEAR OLD CACHE ---
    if (TMap<EProjectionVariant, TObjectPtr<UTexture2D>>* OldVariantMap = PerSlotFinal.Find(TargetSlot))
    {
        for (auto& Pair : *OldVariantMap)
        {
            if (IsValid(Pair.Value))
            {
                Pair.Value->RemoveFromRoot();
            }
        }
        PerSlotFinal.Remove(TargetSlot);
    }
    
    // --- 3. PROCESS EACH VARIANT ---
    for (EProjectionVariant CurrentVariant : VariantsToProcess)
    {
        TArray<TArray<FLinearColor>> ValidProjectionLayers_Linear;
        TArray<TArray<float>> ValidWeightLayers;
        TArray<FProjectionSettings> ValidCameraSettings;

        // Gather valid layers for this variant
        auto GatherValidLayersForVariant = [&]() -> bool
        {
            if (const TMap<EProjectionVariant, TArray<TArray<FLinearColor>>>* SlotVariantMap = PerVariantProjectionLayers.Find(TargetSlot))
            {
                if (const TArray<TArray<FLinearColor>>* SlotProjectionLayers = SlotVariantMap->Find(CurrentVariant))
                {
                    if (const TArray<TArray<float>>* SlotWeightLayersPtr = PerSlotWeightLayers.Find(TargetSlot))
                    {
                        if (const TArray<FProjectionSettings>* SlotCameraSettingsPtr = PerSlotCameraSettings.Find(TargetSlot))
                        {
                            const TArray<TArray<float>>& SlotWeightLayers = *SlotWeightLayersPtr;
                            const TArray<FProjectionSettings>& SlotCameraSettings = *SlotCameraSettingsPtr;
                            
                            for (int32 i = 0; i < SlotCameraSettings.Num(); ++i)
                            {
                                if (SlotProjectionLayers->IsValidIndex(i) && (*SlotProjectionLayers)[i].Num() > 0 &&
                                    SlotWeightLayers.IsValidIndex(i) && SlotWeightLayers[i].Num() > 0)
                                {
                                    ValidProjectionLayers_Linear.Add((*SlotProjectionLayers)[i]);
                                    ValidWeightLayers.Add(SlotWeightLayers[i]);
                                    ValidCameraSettings.Add(SlotCameraSettings[i]);
                                }
                            }
                        }
                    }
                }
            }
            return ValidProjectionLayers_Linear.Num() > 0;
        };

        if (!GatherValidLayersForVariant())
        {
            UE_LOG(LogTemp, Log, TEXT("Reblend: No valid layers found for variant '%s' in slot %d. Skipping."), 
                   *UEnum::GetValueAsString(CurrentVariant), TargetSlot);
            continue;
        }

        bAnyLayersWereBlended = true;
        UE_LOG(LogTemp, Log, TEXT("Reblend: Found %d valid layers to blend for variant '%s'."), 
               ValidProjectionLayers_Linear.Num(), *UEnum::GetValueAsString(CurrentVariant));
        
        // --- BLENDING ---
        TArray<FLinearColor> BlendedLinearPixelBuffer;
        if (ValidProjectionLayers_Linear.Num() > 1)
        {
            TArray<TArray<float>> ProcessedWeights;
            ProcessCameraWeights(ValidWeightLayers, ValidCameraSettings, 
                                 CurrentProjection_TextureWidth, CurrentProjection_TextureHeight, ProcessedWeights);

            if (CurrentVariant == EProjectionVariant::Normal)
            {
                UE_LOG(LogTemp, Log, TEXT("Reblend: Using vector-based blending for Normal variant."));
                BlendedLinearPixelBuffer = CreateBlendedNormalTexture(ValidProjectionLayers_Linear, ProcessedWeights, 
                                                                       CurrentProjection_TextureWidth, CurrentProjection_TextureHeight);
            }
            else
            {
                UE_LOG(LogTemp, Log, TEXT("Reblend: Using color-based blending for %s variant."), 
                       *UEnum::GetValueAsString(CurrentVariant));
                BlendedLinearPixelBuffer = CreateBlendedTexture(ValidProjectionLayers_Linear, ProcessedWeights, 
                                                                 CurrentProjection_TextureWidth, CurrentProjection_TextureHeight);
            }
        }
        else
        {
            BlendedLinearPixelBuffer = ValidProjectionLayers_Linear[0];
        }

		// ============================================================
		// APPLY GUTTER EXTENSION TO FINAL BLENDED RESULT
		// This is the ONLY place gutters are applied in the entire pipeline
		// ============================================================
		if (CurrentVariant == EProjectionVariant::Normal)
		{
			// Normal maps need vector averaging, not color averaging
			UE_LOG(LogTemp, Log, TEXT("Reblend: Applying normal gutter extension..."));

			// 1. Generate the ValidPixelMask based on the Alpha of the blended buffer
			// (If Alpha > 0, we treat it as valid geometry that can be extended FROM)
			TArray<bool> BlendedValidityMask;
			BlendedValidityMask.SetNumUninitialized(BlendedLinearPixelBuffer.Num());
			
			for (int32 i = 0; i < BlendedLinearPixelBuffer.Num(); ++i)
			{
				BlendedValidityMask[i] = (BlendedLinearPixelBuffer[i].A > KINDA_SMALL_NUMBER);
			}

			// 2. Call the function
			FTextureUtils::ExtendTextureMarginsNormal(
				CurrentProjection_TextureWidth,
				CurrentProjection_TextureHeight,
				3, // Radius (Match the gutter radius used in the linear version)
				BlendedLinearPixelBuffer,
				BlendedValidityMask,
				PrecomputedUVIslandMap
			);
		}
		else
		{
			// Color variants use standard color averaging
			UE_LOG(LogTemp, Log, TEXT("Reblend: Applying color gutter extension to %s..."), 
				*UEnum::GetValueAsString(CurrentVariant));
			FTextureUtils::ExtendTextureMarginsLinear(
				CurrentProjection_TextureWidth,
				CurrentProjection_TextureHeight,
				3,  // GutterRadius
				BlendedLinearPixelBuffer,
				PrecomputedUVIslandMap // UV Island Map for color variants
			);
		}
        // --- CREATE TEXTURE ---
        UTexture2D* FinalTextureForVariant = nullptr;

        if (CurrentVariant == EProjectionVariant::Normal)
        {
            FinalTextureForVariant = FTextureUtils::CreateTextureFromLinearPixelData(
                CurrentProjection_TextureWidth,    
                CurrentProjection_TextureHeight,    
                BlendedLinearPixelBuffer
            );
        }
        else
        {
            FinalTextureForVariant = FTextureUtils::CreateTextureFromLinearPixelData(
                CurrentProjection_TextureWidth,    
                CurrentProjection_TextureHeight,    
                BlendedLinearPixelBuffer
            );
        }

        // --- CACHE THE TEXTURE ---
        if (FinalTextureForVariant)
        {
            FinalTextureForVariant->AddToRoot();
            PerSlotFinal.FindOrAdd(TargetSlot).FindOrAdd(CurrentVariant) = FinalTextureForVariant;
                    
            if (CurrentVariant == EProjectionVariant::BaseColor)
            {
                this->ProjectedTexture = FinalTextureForVariant;
            }
        }
    } // End variant loop

    // --- 4. APPLY TO MATERIAL ---
    if (!bAnyLayersWereBlended)
    {
        UE_LOG(LogTemp, Warning, TEXT("Reblend: No valid layers found for any variant in slot %d. Restoring original material."), TargetSlot);
        if (TObjectPtr<UMaterialInterface>* FoundMat = OriginalActorMaterials.Find(TargetSlot))
        {
            MeshComp->SetMaterial(TargetSlot, *FoundMat);
        }
        return;
    }

    UMaterialInstanceDynamic* MID_ForThisSlot = PerSlotMIDs.FindRef(TargetSlot);
    if (!MID_ForThisSlot)
    {
        MID_ForThisSlot = UMaterialInstanceDynamic::Create(ParentMaterial_CumulativeDisplay, SelectedActor);
        if (MID_ForThisSlot)
        {
            MID_ForThisSlot->SetFlags(RF_Transient);
            PerSlotMIDs.Add(TargetSlot, MID_ForThisSlot);
        }
    }

    if (MID_ForThisSlot)
    {
        TMap<EProjectionVariant, TObjectPtr<UTexture2D>>& FinalVariantTextures = PerSlotFinal.FindOrAdd(TargetSlot);

        const bool bShouldBeLit = PerSlotLitState.FindRef(TargetSlot);
        const EProjectionVariant DiffuseVariantToUse = bShouldBeLit ? EProjectionVariant::Shaded : EProjectionVariant::BaseColor;
        UTexture2D* DiffuseTexture = FinalVariantTextures.FindRef(DiffuseVariantToUse);

        UE_LOG(LogTemp, Log, TEXT("Applying Diffuse Texture: Variant='%s' (bIsLit=%s)"), 
               *UEnum::GetValueAsString(DiffuseVariantToUse), bShouldBeLit ? TEXT("true") : TEXT("false"));
        MID_ForThisSlot->SetTextureParameterValue(FName("DiffuseTexture"), DiffuseTexture);

        UTexture2D* NormalTexture = FinalVariantTextures.FindRef(EProjectionVariant::Normal);
        MID_ForThisSlot->SetTextureParameterValue(FName("NormalTexture"), NormalTexture);
        
        UTexture2D* MetallicTexture = FinalVariantTextures.FindRef(EProjectionVariant::Metallic);
        MID_ForThisSlot->SetTextureParameterValue(FName("MetallicTexture"), MetallicTexture);

        UTexture2D* RoughnessTexture = FinalVariantTextures.FindRef(EProjectionVariant::Roughness);
        MID_ForThisSlot->SetTextureParameterValue(FName("RoughnessTexture"), RoughnessTexture);
        
        MeshComp->SetMaterial(TargetSlot, MID_ForThisSlot);
        UE_LOG(LogTemp, Warning, TEXT("Applied all variant textures to material instance for slot %d."), TargetSlot);
    }
}

void FTextureDiffusion3D::RemoveCameraTab(int32 TabIndex)
{
	const int32 CurrentSlotIndex = CurrentSettings.TargetMaterialSlotIndex;
	TArray<FProjectionSettings>& SlotCameraSettings = PerSlotCameraSettings.FindOrAdd(CurrentSlotIndex);

	if (!SlotCameraSettings.IsValidIndex(TabIndex) || SlotCameraSettings.Num() <= 1) return;

	// *** CORRECTED DATA REMOVAL LOGIC ***
	if (TMap<EProjectionVariant, TArray<TArray<FLinearColor>>>* SlotVariantMap = PerVariantProjectionLayers.Find(CurrentSlotIndex))
	{
		for (auto& Pair : *SlotVariantMap)
		{
			if (Pair.Value.IsValidIndex(TabIndex))
			{
				Pair.Value.RemoveAt(TabIndex);
			}
		}
	}

	if (TArray<TArray<float>>* SlotWeightLayers = PerSlotWeightLayers.Find(CurrentSlotIndex))
	{
		if (SlotWeightLayers->IsValidIndex(TabIndex))
		{
			SlotWeightLayers->RemoveAt(TabIndex);
		}
	}
	// *** END OF CORRECTION ***

	SlotCameraSettings.RemoveAt(TabIndex);
	
	if (ActiveCameraIndex >= TabIndex && ActiveCameraIndex > 0)
	{
		ActiveCameraIndex--;
	}
	CurrentSettings = SlotCameraSettings[ActiveCameraIndex];
	
	if (SettingsWidget.IsValid())
	{
		SettingsWidget->SetCameraSettings(SlotCameraSettings, ActiveCameraIndex, Global_OutputPath, Global_OutputTextureWidth, Global_OutputTextureHeight);
	}

	ReblendAndUpdateMesh();
}


void FTextureDiffusion3D::RunComfyUIWorkflow(TSharedPtr<FJsonObject> WorkflowPrompt, const FString& ExpectedOutputPrefix, const FString& FinalPromptText)
{
	UE_LOG(LogTemp, Log, TEXT("RunComfyUIWorkflow: Initialized with direct JSON prompt."));

	const UTextureDiffusion3DSettings* Settings = GetDefault<UTextureDiffusion3DSettings>();


	// 1. Validate the incoming JSON object.
	if (!WorkflowPrompt.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("RunComfyUIWorkflow: Provided workflow prompt is invalid."));
		return;
	}

	// 2. Prepare the final payload for the API.
	TSharedPtr<FJsonObject> PayloadJson = MakeShareable(new FJsonObject);
	PayloadJson->SetStringField(TEXT("client_id"), TEXT("UnrealTextureDiffusion3D_Client"));
	PayloadJson->SetObjectField(TEXT("prompt"), WorkflowPrompt);

	// 3. Serialize the payload to a string.
	FString PayloadString;
	TSharedRef<TJsonWriter<>> JsonWriter = TJsonWriterFactory<>::Create(&PayloadString);
	if (!FJsonSerializer::Serialize(PayloadJson.ToSharedRef(), JsonWriter))
	{
		UE_LOG(LogTemp, Error, TEXT("RunComfyUIWorkflow: Failed to serialize payload JSON."));
		return;
	}

	// 4. Set up and send the HTTP request.
	this->CurrentComfyTaskStartTime = FDateTime::UtcNow();
	this->CurrentComfyExpectedOutputPrefix = ExpectedOutputPrefix;
	FString RequestURL = Settings->ComfyUIServerAddress + TEXT("prompt");


	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = FHttpModule::Get().CreateRequest();
	HttpRequest->SetURL(RequestURL);
	HttpRequest->SetVerb(TEXT("POST"));
	HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	HttpRequest->SetContentAsString(PayloadString);
	HttpRequest->OnProcessRequestComplete().BindRaw(this, &FTextureDiffusion3D::OnWorkflowRequestComplete);
	
	UE_LOG(LogTemp, Log, TEXT("RunComfyUIWorkflow: Sending HTTP request..."));
	if (!HttpRequest->ProcessRequest())
	{
		UE_LOG(LogTemp, Error, TEXT("RunComfyUIWorkflow: Failed to start HTTP request."));
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("Failed to send request to ComfyUI.")));
		this->CurrentComfyExpectedOutputPrefix.Empty();
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("RunComfyUIWorkflow: HTTP request sent. Awaiting callback..."));
	}
}


void FTextureDiffusion3D::OnWorkflowRequestComplete(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
	if (bWasSuccessful && Response.IsValid() && Response->GetResponseCode() == 200)
	{
		UE_LOG(LogTemp, Log, TEXT("OnWorkflowRequestComplete: ComfyUI prompt accepted (HTTP 200)."));
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("OnWorkflowRequestComplete: ComfyUI request failed."));
	}
}


// --- ComfyUI Path Implementation ---
FString FTextureDiffusion3D::GetComfyUIBasePath() const
{
	// Get the settings object
	const UTextureDiffusion3DSettings* Settings = GetDefault<UTextureDiffusion3DSettings>();
	if (Settings && !Settings->ComfyUIBasePath.Path.IsEmpty())
	{
		// Ensure the path always ends with a slash for consistency
		FString Path = Settings->ComfyUIBasePath.Path;
		if (!Path.EndsWith(TEXT("/")) && !Path.EndsWith(TEXT("\\")))
		{
			Path += TEXT("/");
		}
		return Path;
	}

	// Return an empty string if the path is not set
	return FString();
}



UTexture2D* FTextureDiffusion3D::WaitForAndLoadComfyUIOutput(
	const FString& ExpectedFilePrefix,
	float TimeoutSeconds,
	const FDateTime& TaskStartTime,
	FString* OutFoundPath)
{
	if (ExpectedFilePrefix.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("WaitForAndLoadComfyUIOutput: ExpectedFilePrefix is empty. Cannot poll for output."));
		return nullptr;
	}

	FString OutputDirRelative = GetComfyUIBasePath() + TEXT("output/");
	FString OutputDir = FPaths::ConvertRelativePathToFull(OutputDirRelative);
	FPaths::NormalizeDirectoryName(OutputDir);
	if (!OutputDir.EndsWith(TEXT("/")))
	{
		OutputDir += TEXT("/");
	}

	const FString RefPrefix = TEXT("Reference_Image_");	// this is the prefix Comfy uses for the reference
	const FString InputDir  = GetComfyUIBasePath() + TEXT("input/");

	// So we only capture once per poller run
	bool bReferenceCapturedThisRun = false;

	// We'll need slot index to store per-slot path
	const int32 TargetSlotForThisRun = this->CurrentProjection_Settings.TargetMaterialSlotIndex;

	// Initial log indicating the start of polling.
	UE_LOG(LogTemp, Log, 
		TEXT("WaitForAndLoadComfyUIOutput: Polling for file with prefix '%s' in '%s', newer than %s. Timeout: %.1fs"),
		*ExpectedFilePrefix, *OutputDir, *TaskStartTime.ToString(), TimeoutSeconds);

	double PollingStartTimeSeconds = FPlatformTime::Seconds();
	FString FoundAndValidFilePath; // This will store the path of the chosen file
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	int32 LoopIteration = 0;

	if (!PlatformFile.DirectoryExists(*OutputDir))
	{
		UE_LOG(LogTemp, Error, TEXT("WaitForAndLoadComfyUIOutput: ComfyUI output directory does not exist: %s"), *OutputDir);
		return nullptr;
	}

while (FPlatformTime::Seconds() - PollingStartTimeSeconds < TimeoutSeconds)
{
	LoopIteration++;
	TArray<FString> PathsFoundByFindFiles;
	const TCHAR* FileExtensionFilter = TEXT(".png");

	UE_LOG(LogTemp, Verbose, TEXT("Poll #%d: Calling FindFiles. Target Dir: '%s', Filter: '%s'"),
		LoopIteration, *OutputDir, FileExtensionFilter);
	PlatformFile.FindFiles(PathsFoundByFindFiles, *OutputDir, FileExtensionFilter);

	FString BestCandidateFilePathThisScan;
	FDateTime BestCandidateTimestampThisScan(0);

	if (PathsFoundByFindFiles.Num() > 0 && LoopIteration == 1)
	{
		UE_LOG(LogTemp, Log, TEXT("Poll #%d: FindFiles initially found %d file(s) with filter '%s'."),
			LoopIteration, PathsFoundByFindFiles.Num(), FileExtensionFilter);
	}

	// NEW: ensure input dir exists once per scan
	{
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		if (!PF.DirectoryExists(*InputDir))
		{
			PF.CreateDirectoryTree(*InputDir);
		}
	}

	for (const FString& FullPathReturnedByFindFiles : PathsFoundByFindFiles)
	{
		const FString ActualFileName = FPaths::GetCleanFilename(FullPathReturnedByFindFiles);
		const FDateTime FileModTimestamp = PlatformFile.GetTimeStamp(*FullPathReturnedByFindFiles);
		const int64 FileSize = PlatformFile.FileSize(*FullPathReturnedByFindFiles);

		
		// --- (B) Projection candidate selection (unchanged logic, but kept in same loop) ---
		if (ActualFileName.StartsWith(ExpectedFilePrefix))
		{
			UE_LOG(LogTemp, Verbose, TEXT("  Candidate: '%s', ModTime: %s, Size: %lld bytes. (TaskStartTime: %s)"),
				*ActualFileName, *FileModTimestamp.ToString(), FileSize, *TaskStartTime.ToString());

			if (FileModTimestamp >= (TaskStartTime - FTimespan::FromSeconds(2)) && FileSize > 100) // <--- FIXED LINE
    			{
				if (BestCandidateFilePathThisScan.IsEmpty() || FileModTimestamp > BestCandidateTimestampThisScan)
				{
					UE_LOG(LogTemp, Verbose, TEXT("		>> '%s' is current best candidate for this scan iteration."),
						*ActualFileName);
					BestCandidateTimestampThisScan = FileModTimestamp;
					BestCandidateFilePathThisScan = FullPathReturnedByFindFiles;
				}
			}
		}
	} // end for

	// Note: by doing ref-capture inside the loop, if both files appear together,
	// we still capture the reference before we break on the projection.
	if (!BestCandidateFilePathThisScan.IsEmpty())
	{
		FoundAndValidFilePath = BestCandidateFilePathThisScan;
		UE_LOG(LogTemp, Log, TEXT("WaitForAndLoadComfyUIOutput: Selected file after Poll #%d: %s (Timestamp: %s)"),
			LoopIteration, *FoundAndValidFilePath, *BestCandidateTimestampThisScan.ToString());
		break; // EXIT: projection found; ref was already captured if present.
	}

	FPlatformProcess::Sleep(1.0f); // Poll interval
}

	if (FoundAndValidFilePath.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("WaitForAndLoadComfyUIOutput: TIMEOUT after %d polls. No suitable file found for prefix '%s' newer than %s in '%s' after %.1f seconds."),
			LoopIteration, *ExpectedFilePrefix, *TaskStartTime.ToString(), *OutputDir, TimeoutSeconds);
		return nullptr;
	}

	if (OutFoundPath) { *OutFoundPath = FoundAndValidFilePath;}

	// --- NEW: Add resilience to file loading ---
	UTexture2D* LoadedTexture = nullptr;
	const int MaxLoadAttempts = 5; // Try up to 5 times
	const float LoadAttemptDelaySeconds = 0.3f; // Wait 300ms between attempts

	for (int Attempt = 1; Attempt <= MaxLoadAttempts; ++Attempt)
	{
		if (!FPaths::FileExists(FoundAndValidFilePath))
		{
			UE_LOG(LogTemp, Error, TEXT("WaitForAndLoadComfyUIOutput: File %s was found by polling but NO LONGER EXISTS before attempt %d!"), *FoundAndValidFilePath, Attempt);
			return nullptr; // File disappeared, no point retrying
		}

		UE_LOG(LogTemp, Log, TEXT("WaitForAndLoadComfyUIOutput: Attempting to load texture (Attempt %d/%d) from: %s"), Attempt, MaxLoadAttempts, *FoundAndValidFilePath);
		LoadedTexture = FImageUtils::ImportFileAsTexture2D(FoundAndValidFilePath);

		if (LoadedTexture)
		{
			UE_LOG(LogTemp, Log, TEXT("Successfully loaded texture '%s' on attempt %d."), *FPaths::GetCleanFilename(FoundAndValidFilePath), Attempt);
			LoadedTexture->SRGB = true;
			LoadedTexture->CompressionSettings = TextureCompressionSettings::TC_Default;
			LoadedTexture->Filter = TF_Bilinear;
			LoadedTexture->UpdateResource();
			break; // Success! Exit the retry loop.
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("Failed to load texture on attempt %d from %s. Error (check logs above for FImageUtils/LogStreaming). Will retry if attempts remain..."), Attempt, *FoundAndValidFilePath);
			if (Attempt < MaxLoadAttempts)
			{
				FPlatformProcess::Sleep(LoadAttemptDelaySeconds);
			}
		}
	}

	if (!LoadedTexture)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to load texture from file: %s after %d attempts (FImageUtils::ImportFileAsTexture2D returned null consistently)."), *FoundAndValidFilePath, MaxLoadAttempts);
	}
	
	return LoadedTexture;
}


void FTextureDiffusion3D::HandleProjectionError_Internal(const FString& ErrorMessage)
{
	UE_LOG(LogTemp, Error, TEXT("PROJECTION ERROR on actor %s: %s"), CurrentProjection_Actor ? *CurrentProjection_Actor->GetName() : TEXT("Unknown"), *ErrorMessage);
	
	// In the future, we can add logic here to:
	// - Restore the original material that was on the mesh before the projection started.
	// - Clean up any temporary resources.
	// - Display a message to the user.
}

void FTextureDiffusion3D::SaveFinalTextureAsAsset()
{
	const int32 TargetSlot = CurrentSettings.TargetMaterialSlotIndex;

	// Check if there are any final, blended textures to save for the current slot.
	if (!PerSlotFinal.Contains(TargetSlot) || PerSlotFinal.Find(TargetSlot)->Num() == 0)
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(
			TEXT("No final textures exist to save for the selected material slot. Please run a projection first.")));
		return;
	}
	
	FSaveAssetDialogConfig SaveAssetDialogConfig;
	SaveAssetDialogConfig.DialogTitleOverride = FText::FromString("Save Final Textures As (Base Name)");
	SaveAssetDialogConfig.DefaultPath = TEXT("/Game/");

	// The user now only picks a base name, without any suffix.
	SaveAssetDialogConfig.DefaultAssetName = FString::Printf(TEXT("T_%s_Slot%d_Final"), *SelectedActor->GetName(), TargetSlot);
	SaveAssetDialogConfig.ExistingAssetPolicy = ESaveAssetDialogExistingAssetPolicy::AllowButWarn;

	FOnObjectPathChosenForSave OnSave = FOnObjectPathChosenForSave::CreateRaw(this, &FTextureDiffusion3D::HandleAssetSave);
	FOnAssetDialogCancelled OnCancel = FOnAssetDialogCancelled::CreateRaw(this, &FTextureDiffusion3D::HandleAssetSaveCancelled);
	
	FContentBrowserModule& CB = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
	CB.Get().CreateSaveAssetDialog(SaveAssetDialogConfig, OnSave, OnCancel);
}

void FTextureDiffusion3D::HandleAssetSave(const FString& BaseObjectPath)
{
	UE_LOG(LogTemp, Log, TEXT("HandleAssetSave: User selected base path '%s'. Saving all available variants."), *BaseObjectPath);

	const int32 TargetSlot = CurrentSettings.TargetMaterialSlotIndex;
	const TMap<EProjectionVariant, TObjectPtr<UTexture2D>>* FinalVariantTextures = PerSlotFinal.Find(TargetSlot);

	if (!FinalVariantTextures)
	{
		UE_LOG(LogTemp, Error, TEXT("Save failed: Could not find final textures in cache for slot %d."), TargetSlot);
		return;
	}

	FString BasePackageName, BaseAssetName;
	BaseObjectPath.Split(TEXT("."), &BasePackageName, &BaseAssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
	if (BasePackageName.IsEmpty() || BaseAssetName.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("Save failed: Invalid base object path '%s'."), *BaseObjectPath);
		return;
	}

	TArray<UObject*> SavedObjectsToSync;

	for (const auto& Pair : *FinalVariantTextures)
	{
		const EProjectionVariant Variant = Pair.Key;
		UTexture2D* TextureToSave = Pair.Value; // This is now your 8-byte TC_RGBA16F texture

		if (!TextureToSave || !TextureToSave->GetPlatformData() || TextureToSave->GetPlatformData()->Mips.Num() == 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("Skipping variant '%s' as it has no valid pixel data."), *UEnum::GetValueAsString(Variant));
			continue;
		}

		// Build suffix and desired names
		FString Suffix;
		switch (Variant)
		{
			case EProjectionVariant::BaseColor: Suffix = TEXT("_BaseColor"); break;
			case EProjectionVariant::Normal:    Suffix = TEXT("_Normal");    break;
			case EProjectionVariant::Shaded:    Suffix = TEXT("_Shaded");    break;
			case EProjectionVariant::Roughness: Suffix = TEXT("_Roughness"); break;
			case EProjectionVariant::Metallic:  Suffix = TEXT("_Metallic");  break;
			case EProjectionVariant::AO:        Suffix = TEXT("_AO");        break;
			default:                            Suffix = TEXT("_Unknown");    break;
		}

		FString FinalPackageName = BasePackageName + Suffix;
		FString FinalAssetName   = BaseAssetName   + Suffix;

		bool bForceOpaque = true;
		if (Variant == EProjectionVariant::BaseColor)
		{
			const EAppReturnType::Type DialogResult = FMessageDialog::Open(
				EAppMsgType::YesNo,
				FText::Format(LOCTEXT("SaveOpacityDialogText", "Save BaseColor variant '{0}' with full opacity?"),
				FText::FromString(FinalAssetName)));
			bForceOpaque = (DialogResult == EAppReturnType::Yes);
		}

		FTexture2DMipMap& SourceMip = TextureToSave->GetPlatformData()->Mips[0];
		const int32 MipWidth  = SourceMip.SizeX;
		const int32 MipHeight = SourceMip.SizeY;

		// Make the package/object names unique
		FString UniquePackageName, UniqueAssetName;
		FAssetToolsModule::GetModule().Get().CreateUniqueAssetName(FinalPackageName, TEXT(""), UniquePackageName, UniqueAssetName);

		UPackage* Package = CreatePackage(*UniquePackageName);
		if (!Package)
		{
			UE_LOG(LogTemp, Error, TEXT("Failed to create package '%s'."), *UniquePackageName);
			continue;
		}

		// We are saving as an 8-bit texture, so the new asset is TSF_BGRA8
		UTexture2D* NewTexture = NewObject<UTexture2D>(Package, *UniqueAssetName, RF_Public | RF_Standalone);
		if (!NewTexture)
		{
			UE_LOG(LogTemp, Error, TEXT("Failed to create texture '%s' in package '%s'."), *UniqueAssetName, *UniquePackageName);
			continue;
		}

		NewTexture->Source.Init(MipWidth, MipHeight, 1, 1, ETextureSourceFormat::TSF_BGRA8);

		// --- THIS IS THE CORRECTED READ/WRITE ---

		// 1. Lock the 16-bit (HDR) source (8 bytes per pixel)
		const FFloat16Color* SourcePixels = static_cast<const FFloat16Color*>(SourceMip.BulkData.LockReadOnly());

		// 2. Lock the 8-bit (LDR) destination (4 bytes per pixel)
		FColor* DestPixels = reinterpret_cast<FColor*>(NewTexture->Source.LockMip(0));

		const FLinearColor BaseColorLinear = GetBaseColorForSlot(TargetSlot);
		const FColor BaseColorSRGB = BaseColorLinear.ToFColor(true);

		// 3. Loop, CONVERT, and write
		for (int32 i = 0; i < MipWidth * MipHeight; ++i)
		{
			// Read the 16-bit float pixel and convert it to a 32-bit FLinearColor
			const FLinearColor LinearColor(SourcePixels[i]);

			if (Variant == EProjectionVariant::Normal)
			{
				// Use 'false' to skip sRGB conversion for linear normal map data
				DestPixels[i] = LinearColor.ToFColor(false);
				DestPixels[i].A = 255; // Ensure full alpha for normal map
			}
			else
			{
				// --- Original path for BaseColor, Shaded, etc. ---
				const FColor sRGBColor = LinearColor.ToFColor(true); // 'true' applies sRGB gamma

				if (bForceOpaque)
				{
					if (LinearColor.A > 0.01f) // Compare alpha in float space
					{
						DestPixels[i] = FColor(sRGBColor.R, sRGBColor.G, sRGBColor.B, 255);
					}
					else
					{
						DestPixels[i] = BaseColorSRGB;
						DestPixels[i].A = 255;
					}
				}
				else
				{
					DestPixels[i] = sRGBColor; // Save with the converted alpha
				}
			}
		}

		// 4. Unlock both mips
		NewTexture->Source.UnlockMip(0);
		SourceMip.BulkData.Unlock();

		// --- END OF CORRECTION ---

		// Set final texture properties
		if (Variant == EProjectionVariant::Normal)
		{
			NewTexture->CompressionSettings = TC_Normalmap;
			NewTexture->SRGB = false;
		}
		else
		{
			NewTexture->CompressionSettings = TC_Default;
			NewTexture->SRGB = true; // This is now correct 8-bit sRGB data
		}

		// Save and register
		NewTexture->UpdateResource();
		Package->MarkPackageDirty();
		NewTexture->PostEditChange();

		FAssetRegistryModule::AssetCreated(NewTexture);
		const FString PackageFileName = FPackageName::LongPackageNameToFilename(UniquePackageName, FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		UPackage::SavePackage(Package, NewTexture, *PackageFileName, SaveArgs);

		SavedObjectsToSync.Add(NewTexture);
	}

	if (SavedObjectsToSync.Num() > 0)
	{
		GEditor->SyncBrowserToObjects(SavedObjectsToSync);
	}
}


void FTextureDiffusion3D::HandleAssetSaveCancelled()
{
	UE_LOG(LogTemp, Log, TEXT("Save Asset Dialog was cancelled by the user."));
}

void FTextureDiffusion3D::OnTargetUVChannelChanged(int32 NewChannel)
{
	TargetUVChannel = NewChannel;
	UE_LOG(LogTemp, Log, TEXT("Target UV Channel changed to: %d"), TargetUVChannel);
	// In the future, we might want to regenerate precomputed data here
}

bool FTextureDiffusion3D::TopologicalSort(const TSharedPtr<FJsonObject>& WorkflowObject, TArray<FString>& OutSortedNodeIds)
{
	TMap<FString, TArray<FString>> AdjList;
	TMap<FString, int32> InDegree;
	TArray<FString> AllNodeIds;

	for (const auto& Pair : WorkflowObject->Values)
	{
		AllNodeIds.Add(Pair.Key);
	}

	// 1. Initialize maps (no changes here)
	for (const FString& NodeId : AllNodeIds)
	{
		InDegree.Add(NodeId, 0);
		AdjList.Add(NodeId, TArray<FString>());
	}

	// 2. Build the graph (no changes here)
	for (const FString& NodeId : AllNodeIds)
	{
		const TSharedPtr<FJsonObject> Node = WorkflowObject->GetObjectField(NodeId);
		const TSharedPtr<FJsonObject> Inputs = Node->GetObjectField(TEXT("inputs"));
		for (const auto& InputPair : Inputs->Values)
		{
			if (InputPair.Value->Type == EJson::Array)
			{
				const auto& Connection = InputPair.Value->AsArray();
				if (Connection.Num() > 0 && Connection[0]->Type == EJson::String)
				{
					FString ParentNodeId = Connection[0]->AsString();
					AdjList.FindOrAdd(ParentNodeId).Add(NodeId);
					InDegree.FindOrAdd(NodeId)++;
				}
			}
		}
	}

	// Define the sorting lambda once to be reused
		auto SortParallelNodes = [&](TArray<FString>& NodeList)
		{
			NodeList.Sort([&](const FString& NodeA_ID, const FString& NodeB_ID) {
				const TArray<FString>& ChildrenOfA = AdjList.FindOrAdd(NodeA_ID);
				const TArray<FString>& ChildrenOfB = AdjList.FindOrAdd(NodeB_ID);
				FString CommonChildID;
				for (const FString& ChildA : ChildrenOfA) {
					if (ChildrenOfB.Contains(ChildA)) {
						CommonChildID = ChildA;
						break;
					}
				}

				if (!CommonChildID.IsEmpty()) {
					const TSharedPtr<FJsonObject> ChildInputs = WorkflowObject->GetObjectField(CommonChildID)->GetObjectField(TEXT("inputs"));
					TArray<FString> InputKeysInOrder;
					for (const auto& Pair : ChildInputs->Values) {
						InputKeysInOrder.Add(Pair.Key);
					}

					FString InputForA, InputForB;
					for (const auto& Pair : ChildInputs->Values) {
						if (Pair.Value->Type == EJson::Array && Pair.Value->AsArray().Num() > 0) {
							const FString& ParentNodeID = Pair.Value->AsArray()[0]->AsString();
							if (ParentNodeID == NodeA_ID) {
								InputForA = Pair.Key;
							} else if (ParentNodeID == NodeB_ID) {
								InputForB = Pair.Key;
							}
						}
					}

					const int32 IndexA = InputKeysInOrder.Find(InputForA);
					const int32 IndexB = InputKeysInOrder.Find(InputForB);
					if (IndexA != INDEX_NONE && IndexB != INDEX_NONE && IndexA != IndexB) {
						return IndexA < IndexB;
					}
				}

				const FString TitleA = WorkflowObject->GetObjectField(NodeA_ID)->GetObjectField(TEXT("_meta"))->GetStringField(TEXT("title"));
				const FString TitleB = WorkflowObject->GetObjectField(NodeB_ID)->GetObjectField(TEXT("_meta"))->GetStringField(TEXT("title"));

				const bool bTitleAIsPositive = TitleA.Contains(TEXT("Positive"), ESearchCase::IgnoreCase);
				const bool bTitleAIsNegative = TitleA.Contains(TEXT("Negative"), ESearchCase::IgnoreCase);
				const bool bTitleBIsPositive = TitleB.Contains(TEXT("Positive"), ESearchCase::IgnoreCase);
				const bool bTitleBIsNegative = TitleB.Contains(TEXT("Negative"), ESearchCase::IgnoreCase);

				if (bTitleAIsPositive && bTitleBIsNegative)
				{
					return true; // A (Positive) should come before B (Negative)
				}
				if (bTitleAIsNegative && bTitleBIsPositive)
				{
					return false; // A (Negative) should come after B (Positive)
				}

				return TitleA < TitleB;
			});
		};

	// 3. Find and sort the initial set of start nodes
	TArray<FString> StartNodes;
	for (const auto& Pair : InDegree)
	{
		if (Pair.Value == 0)
		{
			StartNodes.Add(Pair.Key);
		}
	}
	SortParallelNodes(StartNodes);

	// 4. Initialize a queue with the sorted start nodes
	TQueue<FString> Queue;
	for (const FString& NodeId : StartNodes)
	{
		Queue.Enqueue(NodeId);
	}

	// 5. Process the queue
	while (!Queue.IsEmpty())
	{
		FString CurrentNodeId;
		Queue.Dequeue(CurrentNodeId);
		OutSortedNodeIds.Add(CurrentNodeId);

		if (AdjList.Contains(CurrentNodeId))
		{
			TArray<FString> NewlyReadyNodes;
			for (const FString& NeighborId : AdjList[CurrentNodeId])
			{
				InDegree[NeighborId]--;
				if (InDegree[NeighborId] == 0)
				{
					NewlyReadyNodes.Add(NeighborId);
				}
			}
			
			if (NewlyReadyNodes.Num() > 1)
			{
				SortParallelNodes(NewlyReadyNodes);
			}
			
			for (const FString& NodeId : NewlyReadyNodes)
			{
				Queue.Enqueue(NodeId);
			}
		}
	}
	
	return OutSortedNodeIds.Num() == AllNodeIds.Num();
}


bool FTextureDiffusion3D::ParseWorkflowForUnrealNodes(const FString& FilePath, TArray<FUnrealNodeInfo>& OutSortedNodes)
{
	OutSortedNodes.Empty();
	FString JsonContent;
	if (!FFileHelper::LoadFileToString(JsonContent, *FilePath))
	{
		UE_LOG(LogTemp, Error, TEXT("ParseWorkflow: Failed to load file: %s"), *FilePath);
		return false;
	}

	TSharedPtr<FJsonObject> WorkflowObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonContent);
	if (!FJsonSerializer::Deserialize(Reader, WorkflowObject) || !WorkflowObject.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("ParseWorkflow: Failed to parse JSON from file: %s"), *FilePath);
		return false;
	}

	TArray<FString> SortedNodeIds;
	if (!TopologicalSort(WorkflowObject, SortedNodeIds))
	{
		UE_LOG(LogTemp, Error, TEXT("ParseWorkflow: Failed to sort workflow graph. Check for cycles."));
		return false;
	}

	for (const FString& NodeId : SortedNodeIds)
	{
		const TSharedPtr<FJsonObject> Node = WorkflowObject->GetObjectField(NodeId);
		if (!Node.IsValid()) continue;

		const TSharedPtr<FJsonObject> Meta = Node->GetObjectField(TEXT("_meta"));
		if (!Meta.IsValid()) continue;

		const FString Title = Meta->GetStringField(TEXT("title"));
		
		bool bIsTagged = Title.StartsWith(TEXT("Unreal "), ESearchCase::IgnoreCase) || 
						 Title.StartsWith(TEXT("Unreal_"), ESearchCase::IgnoreCase) || 
						 Title.StartsWith(TEXT("Unreal-"), ESearchCase::IgnoreCase);
		
		if (bIsTagged)
		{
			UE_LOG(LogTemp, Warning, TEXT("Parser Found Tagged Node -> Title: '%s' (ID: %s)"), *Title, *NodeId);

			FUnrealNodeInfo Info;
			Info.NodeId = NodeId;
			Info.CleanTitle = Title.RightChop(7);

			Info.CleanTitle = Info.CleanTitle.TrimStartAndEnd();
			while (Info.CleanTitle.RemoveFromStart(TEXT("_")) | Info.CleanTitle.RemoveFromStart(TEXT("-")) | Info.CleanTitle.RemoveFromStart(TEXT(" "))) {}


			auto Canon = [](FString S)
			{
				S = S.ToLower();
				S.ReplaceInline(TEXT(" "), TEXT(""));
				S.ReplaceInline(TEXT("_"), TEXT(""));
				S.ReplaceInline(TEXT("-"), TEXT(""));
				return S;
			};

			const FString CanonTitle = Canon(Info.CleanTitle);

			const FString ClassType = Node->GetStringField(TEXT("class_type"));

			if (ClassType == TEXT("LoadImage") || ClassType == TEXT("LoadImageMask"))
{
	Info.bIsImageInput = true;

	if (CanonTitle == TEXT("depth"))
	{
		Info.ImageKeyword = TEXT("depth");
	}
	else if (CanonTitle == TEXT("canny")
		|| CanonTitle == TEXT("edge")
		|| CanonTitle == TEXT("cannyedge")
		|| CanonTitle == TEXT("edges")
		|| CanonTitle == TEXT("crease")
		|| CanonTitle == TEXT("creases")
		|| CanonTitle == TEXT("creasemap"))
	{
		Info.ImageKeyword = TEXT("canny");
	}
	else if (CanonTitle == TEXT("basecolor")	// matches "base color", "base-color", "base_color", "BaseColor"
		  || CanonTitle == TEXT("albedo")		// optional synonyms
		  || CanonTitle == TEXT("diffuse"))	// optional synonyms
	{
		Info.ImageKeyword = TEXT("base color");
	}
	else if (CanonTitle == TEXT("normals") || CanonTitle == TEXT("normal"))
	{
		Info.ImageKeyword = TEXT("normals");
	}
	else if (CanonTitle == TEXT("mask"))
	{
		Info.ImageKeyword = TEXT("mask");
	}
	else if (CanonTitle == TEXT("referenceimage") || CanonTitle == TEXT("reference"))
	{
		Info.ImageKeyword = TEXT("reference image");
	}
}
else
{
	Info.bIsImageInput = false;
	const TSharedPtr<FJsonObject> InputsObject = Node->GetObjectField(TEXT("inputs"));
	if (InputsObject.IsValid())
	{
		for (const auto& InputPair : InputsObject->Values)
		{
			if (InputPair.Value->Type != EJson::Array)
			{
				FUnrealInputInfo InputInfo;
				InputInfo.InputName = InputPair.Key;
				InputInfo.JsonType  = InputPair.Value->Type;
				Info.ExposedInputs.Add(InputInfo);
			}
		}
	}
}
			OutSortedNodes.Add(Info);
		}
	}
	return true;
}




TSharedPtr<FJsonObject> FTextureDiffusion3D::GetWorkflowJsonObject(const FString& FilePath)
{
	FString JsonContent;
	if (!FFileHelper::LoadFileToString(JsonContent, *FilePath)) return nullptr;
	TSharedPtr<FJsonObject> WorkflowObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonContent);
	FJsonSerializer::Deserialize(Reader, WorkflowObject);
	return WorkflowObject;
}


UTexture2D* FTextureDiffusion3D::GetPreviewTextureForKeyword(FString ImageKeyword)
{
	// This is the dispatcher function called by the UI's delegate.
	if (ImageKeyword.Equals(TEXT("depth"), ESearchCase::IgnoreCase))
	{
		return CreateDepthPreviewTexture();
	}
	else if (ImageKeyword.Equals(TEXT("mask"), ESearchCase::IgnoreCase))
	{
		return CreateBaseColorPreviewTexture();
	}
	
	else if (ImageKeyword.Equals(TEXT("base color"), ESearchCase::IgnoreCase))
	{
		// The composed image is the "base color" or "base laid" preview.
		return CreateBaseColorPreviewTexture();
	}
	else if (ImageKeyword.Equals(TEXT("reference image"), ESearchCase::IgnoreCase))
	{
		return CreateReferencePreviewTexture();
	}


	UE_LOG(LogTemp, Warning, TEXT("GetPreviewTextureForKeyword: Unknown keyword '%s'"), *ImageKeyword);
	return nullptr;
}


	
UTexture2D* FTextureDiffusion3D::CreateBaseColorPreviewTexture()
{
	// --- 1. VALIDATION ---
	if (!CurrentProjection_Actor)
	{
		UE_LOG(LogTemp, Warning, TEXT("CreateBaseColorPreviewTexture: No valid actor for preview."));
		// Return a placeholder texture
		TArray<FColor> BlackPixels;
		BlackPixels.Init(FColor::Black, 256 * 256);
		return FTextureUtils::CreateTextureFromPixelData(256, 256, BlackPixels);
	}

	// --- 2. GENERATE THE CONTEXTUAL VIEW ---
	UE_LOG(LogTemp, Log, TEXT("Creating contextual Base Color Preview..."));

	// Step A: Prepare the contextual texture map, excluding the current projection.
	TMap<int32, TObjectPtr<UTexture2D>> TexturesForRasterization = PrepareContextualTextureMap(CurrentSettings, ActiveCameraIndex);


	// Step B: Generate the perspective view using the contextual map.
	TArray<FLinearColor> RasterizedPixels_Linear; // <-- CHANGED
	bool bSuccess = FMeshProcessor::GenerateRasterizedView(
		RasterizedPixels_Linear,
		CurrentProjection_Actor,
		CurrentSettings,
		TexturesForRasterization,
		Global_OutputTextureWidth,
		Global_OutputTextureHeight,
		TargetUVChannel
	);

	// Step C: Clean up the temporary textures to avoid memory leaks.
	for (auto const& TempTexturePair : TexturesForRasterization)
	{
		if (TempTexturePair.Value)
		{
			TempTexturePair.Value->MarkAsGarbage();
		}
	}

	if (!bSuccess)
	{
		UE_LOG(LogTemp, Error, TEXT("CreateBaseColorPreviewTexture: Rasterization failed."));
		// Return an error texture
		TArray<FColor> RedPixels;
		RedPixels.Init(FColor::Red, 256 * 256);
		return FTextureUtils::CreateTextureFromPixelData(256, 256, RedPixels);
	}

	// --- 3. CREATE AND RETURN THE FINAL PREVIEW TEXTURE ---
	TArray<FColor> RasterizedPixels_sRGB = FTextureUtils::LinearTo_sRGB(RasterizedPixels_Linear); // <-- ADDED
	UTexture2D* PreviewTexture = FTextureUtils::CreateTextureFromPixelData(Global_OutputTextureWidth, Global_OutputTextureHeight, RasterizedPixels_sRGB);
	this->LastPreviewTexture = PreviewTexture; // Keep a reference
	return PreviewTexture;
}

UTexture2D* FTextureDiffusion3D::CreateDepthPreviewTexture()
{
    if (!SelectedActor) return nullptr;

    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World) return nullptr;

    // FIX 1: Use a smaller, more reasonable size for a preview.
    const int32 PreviewSize = 1024;

    // Create a temporary Render Target
    UTextureRenderTarget2D* TempRT = NewObject<UTextureRenderTarget2D>();
    // Use the new smaller size here
    TempRT->InitCustomFormat(PreviewSize, PreviewSize, PF_FloatRGBA, false);
    TempRT->UpdateResource();

    // Create a temporary, clean capture actor.
    ASceneCapture2D* TempCaptureActor = World->SpawnActor<ASceneCapture2D>();
    USceneCaptureComponent2D* CaptureComp = TempCaptureActor->GetCaptureComponent2D();

    // Set the camera transform directly from the current settings
    TempCaptureActor->SetActorLocation(CurrentSettings.CameraPosition);
    TempCaptureActor->SetActorRotation(CurrentSettings.CameraRotation);

    // Configure the capture for depth
    CaptureComp->TextureTarget = TempRT;
    CaptureComp->FOVAngle = CurrentSettings.FOVAngle;
    CaptureComp->CaptureSource = ESceneCaptureSource::SCS_SceneDepth;
    CaptureComp->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
    CaptureComp->ShowOnlyActors.Add(SelectedActor);
    
    // FIX 2: Explicitly disable continuous capturing for this one-shot operation.
    CaptureComp->bCaptureEveryFrame = false;
    
    CaptureComp->CaptureScene();

    // Read the high-precision float data
    TArray<FFloat16Color> FloatPixels;
    FTextureRenderTargetResource* RTResource = TempRT->GameThread_GetRenderTargetResource();
    RTResource->ReadFloat16Pixels(FloatPixels);

    // --- Robust Normalization Logic (This part is fine) ---
    TArray<FColor> NormalizedPixels;
    NormalizedPixels.AddUninitialized(FloatPixels.Num());
    float MinDepth = MAX_FLT;
    float MaxDepth = 0.0f;
    for (const FFloat16Color& Pixel : FloatPixels)
    {
        float Depth = Pixel.R.GetFloat();
        if (Depth > 0.0f && Depth < (65504.0f - 1.0f))
        {
            if (Depth < MinDepth) MinDepth = Depth;
            if (Depth > MaxDepth) MaxDepth = Depth;
        }
    }
    float Range = MaxDepth - MinDepth;
    if (Range < KINDA_SMALL_NUMBER) Range = 1.0f;
    for (int32 i = 0; i < FloatPixels.Num(); ++i)
    {
        float Depth = FloatPixels[i].R.GetFloat();
        uint8 GrayValue = 0;
        if (Depth >= MinDepth && Depth <= MaxDepth)
        {
            float NormalizedDepth = (Depth - MinDepth) / Range;
            GrayValue = FMath::Clamp(FMath::RoundToInt((1.0f - NormalizedDepth) * 255.0f), 0, 255);
        }
        NormalizedPixels[i] = FColor(GrayValue, GrayValue, GrayValue, 255);
    }

    // Create the transient texture from the normalized pixel data
    UTexture2D* PreviewTexture = FTextureUtils::CreateTextureFromPixelData(PreviewSize, PreviewSize, NormalizedPixels);
    this->LastPreviewTexture = PreviewTexture;
    
    // FIX 3: Clean up all temporary resources to prevent memory leaks.
    World->DestroyActor(TempCaptureActor);
    TempCaptureActor = nullptr;

    if(TempRT)
    {
        // This is crucial. It tells the garbage collector that this object is no longer needed.
        TempRT->MarkAsGarbage();
    }

    return PreviewTexture;
}

UTexture2D* FTextureDiffusion3D::CreateReferencePreviewTexture()
{
	// 1. Get the current material slot index from the active settings.
	const int32 CurrentSlotIndex = CurrentSettings.TargetMaterialSlotIndex;

	// 2. Find the path for this slot's reference image in our map.
	const FString* ReferenceImagePathPtr = PerSlotReferenceImagePath.Find(CurrentSlotIndex);

	// 3. Handle cases where the path is not found, is empty, or the file doesn't exist.
	if (!ReferenceImagePathPtr || ReferenceImagePathPtr->IsEmpty() || !FPaths::FileExists(*ReferenceImagePathPtr))
	{
		// Log a warning for debugging purposes.
		UE_LOG(LogTemp, Warning, TEXT("CreateReferencePreviewTexture: No valid reference image found for slot %d. Path was '%s'."), 
			CurrentSlotIndex, 
			ReferenceImagePathPtr ? **ReferenceImagePathPtr : TEXT("nullptr"));

		// Return a neutral placeholder texture to avoid UI errors.
		TArray<FColor> GrayPixels;
		GrayPixels.Init(FColor(128, 128, 128, 255), 256 * 256);
		UTexture2D* PlaceholderTexture = FTextureUtils::CreateTextureFromPixelData(256, 256, GrayPixels);
		if (PlaceholderTexture)
		{
			PlaceholderTexture->SetFlags(RF_Transient);
		}
		return PlaceholderTexture;
	}

	// 4. If a valid path is found, load the texture from disk.
	UE_LOG(LogTemp, Log, TEXT("CreateReferencePreviewTexture: Loading reference image for slot %d from '%s'."), CurrentSlotIndex, **ReferenceImagePathPtr);
	UTexture2D* LoadedTexture = FImageUtils::ImportFileAsTexture2D(*ReferenceImagePathPtr);

	// 5. Configure the loaded texture and return it.
	if (LoadedTexture)
	{
		LoadedTexture->SetFlags(RF_Transient); // Mark as transient so it's not saved with the project.
		LoadedTexture->SRGB = true;				// Ensure correct color display in the UI.
		LoadedTexture->UpdateResource();
		this->LastPreviewTexture = LoadedTexture; // Keep a reference, consistent with other preview functions.
		return LoadedTexture;
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("CreateReferencePreviewTexture: FImageUtils failed to load texture from path: %s"), **ReferenceImagePathPtr);
		
		// Return a different placeholder (e.g., red) to indicate a loading failure.
		TArray<FColor> RedPixels;
		RedPixels.Init(FColor::Red, 256 * 256);
		UTexture2D* ErrorTexture = FTextureUtils::CreateTextureFromPixelData(256, 256, RedPixels);
		if (ErrorTexture)
		{
			ErrorTexture->SetFlags(RF_Transient);
		}
		return ErrorTexture;
	}
}

bool FTextureDiffusion3D::GenerateDepthDataForUpload(TArray64<uint8>& OutPngData)
{
    if (!SelectedActor)
    {
        UE_LOG(LogTemp, Error, TEXT("GenerateDepthDataForUpload: Cannot generate data, SelectedActor is invalid."));
        return false;
    }

    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World)
    {
        UE_LOG(LogTemp, Error, TEXT("GenerateDepthDataForUpload: Cannot get a valid editor world."));
        return false;
    }

    const int32 CaptureSize = 1024;

    UTextureRenderTarget2D* TempRT = NewObject<UTextureRenderTarget2D>();
    TempRT->InitCustomFormat(CaptureSize, CaptureSize, PF_FloatRGBA, false);
    TempRT->UpdateResource();

    ASceneCapture2D* TempCaptureActor = World->SpawnActor<ASceneCapture2D>();
    USceneCaptureComponent2D* CaptureComp = TempCaptureActor->GetCaptureComponent2D();

    // --- THIS IS THE FIX ---
    // Set the location and rotation on the ACTOR, not the component.
    TempCaptureActor->SetActorLocationAndRotation(CurrentSettings.CameraPosition, CurrentSettings.CameraRotation);

    CaptureComp->FOVAngle = CurrentSettings.FOVAngle;
    CaptureComp->TextureTarget = TempRT;
    CaptureComp->CaptureSource = ESceneCaptureSource::SCS_SceneDepth;
    CaptureComp->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
    CaptureComp->ShowOnlyActors.Add(SelectedActor);
    CaptureComp->bCaptureEveryFrame = false;
    
    CaptureComp->CaptureScene();

    TArray<FFloat16Color> FloatPixels;
    FTextureRenderTargetResource* RTResource = TempRT->GameThread_GetRenderTargetResource();
    RTResource->ReadFloat16Pixels(FloatPixels);

    TArray<FColor> NormalizedPixels;
    NormalizedPixels.AddUninitialized(FloatPixels.Num());

    float MinDepth = MAX_FLT;
    float MaxDepth = 0.0f;

    for (const FFloat16Color& Pixel : FloatPixels)
    {
        float Depth = Pixel.R.GetFloat();
        if (Depth > 0.0f && Depth < (65504.0f - 1.0f))
        {
            if (Depth < MinDepth) MinDepth = Depth;
            if (Depth > MaxDepth) MaxDepth = Depth;
        }
    }

    float Range = MaxDepth - MinDepth;
    if (Range < KINDA_SMALL_NUMBER) Range = 1.0f;

    for (int32 i = 0; i < FloatPixels.Num(); ++i)
    {
        float Depth = FloatPixels[i].R.GetFloat();
        uint8 GrayValue = 0;

        if (Depth >= MinDepth && Depth <= MaxDepth)
        {
            float NormalizedDepth = (Depth - MinDepth) / Range;
            GrayValue = FMath::Clamp(FMath::RoundToInt((1.0f - NormalizedDepth) * 255.0f), 0, 255);
        }
        NormalizedPixels[i] = FColor(GrayValue, GrayValue, GrayValue, 255);
    }

    World->DestroyActor(TempCaptureActor);
    TempCaptureActor = nullptr;

    if(TempRT)
    {
        TempRT->MarkAsGarbage();
    }

    if (NormalizedPixels.Num() > 0)
    {
        FImageUtils::PNGCompressImageArray(CaptureSize, CaptureSize, NormalizedPixels, OutPngData);
        return OutPngData.Num() > 0;
    }

    return false;
}

// In TextureDiffusion3D.cpp

bool FTextureDiffusion3D::GenerateBaseColorDataForUpload(TArray64<uint8>& OutPngData)
{
    if (!CurrentProjection_Actor) return false;
    const int32 CaptureSize = 1024;

    TMap<int32, TObjectPtr<UTexture2D>> TexturesForRasterization = PrepareContextualTextureMap(CurrentSettings, ActiveCameraIndex);
    
    // This variable receives the linear pixel data stored as bytes (FColor).
    TArray<FLinearColor> RasterizedPixels_LinearFloats; // <-- CHANGED
    
    bool bSuccess = FMeshProcessor::GenerateRasterizedView(
        RasterizedPixels_LinearFloats, // Output from rasterizer
        CurrentProjection_Actor, CurrentSettings,
        TexturesForRasterization, CaptureSize, CaptureSize, TargetUVChannel
    );

    for (auto const& TempTexturePair : TexturesForRasterization)
    {
        if (TempTexturePair.Value) TempTexturePair.Value->MarkAsGarbage();
    }

    if (bSuccess && RasterizedPixels_LinearFloats.Num() > 0) // <-- FIX: Use new variable name
    {
        // 1. Convert the final linear float data to 8-bit sRGB for PNG compression.
        TArray<FColor> RasterizedPixels_sRGB = FTextureUtils::LinearTo_sRGB(RasterizedPixels_LinearFloats); // <-- FIX: Old block is gone
        
        // 2. Compress the correctly converted sRGB byte data.
        FImageUtils::PNGCompressImageArray(CaptureSize, CaptureSize, RasterizedPixels_sRGB, OutPngData);
        return OutPngData.Num() > 0;
    }
    
    return false; 
}


/**
 * (In FTextureDiffusion3D.cpp)
 * Prepares a map of textures for the contextual rasterizer (GenerateRasterizedView).
 * It blends all existing projection layers *except* for the current one.
 * This version is 16-bit HDR correct: it blends in Linear space and creates
 * 16-bit (PF_FloatRGBA) transient textures.
 */
TMap<int32, TObjectPtr<UTexture2D>> FTextureDiffusion3D::PrepareContextualTextureMap(
    const FProjectionSettings& SettingsForCurrentView,
    int32 IndexOfCurrentView)
{
    TMap<int32, TObjectPtr<UTexture2D>> TexturesForRasterization;
    // We use the 'Shaded' variant as the source for 'Base Color' projections.
    const EProjectionVariant ContextVariant = EProjectionVariant::Shaded;

    // Helper lambda to blend multiple linear layers based on their alpha.
    auto BlendByStoredLayerAlpha = [&](const TArray<TArray<FLinearColor>>& Layers) -> TArray<FLinearColor>
    {
        const int32 NumTexels = Global_OutputTextureWidth * Global_OutputTextureHeight;
        TArray<FLinearColor> Out; Out.Init(FLinearColor::Transparent, NumTexels);
        if (Layers.Num() == 0) return Out;

        for (int32 t = 0; t < NumTexels; ++t)
        {
            FLinearColor SummedColor(0, 0, 0, 0);
            float sumW = 0.0f;
            for (const TArray<FLinearColor>& layer : Layers)
            {
                if (!layer.IsValidIndex(t)) continue;
                const FLinearColor& px = layer[t];
                const float w = px.A; // Use the stored alpha as the blend weight
                if (w <= KINDA_SMALL_NUMBER) continue;
                
                // Add premultiplied color
                SummedColor += FLinearColor(px.R, px.G, px.B, 0) * w; 
                sumW += w;
            }
            if (sumW > KINDA_SMALL_NUMBER)
            {
                SummedColor /= sumW; // Unpremultiply
                SummedColor.A = FMath::Min(sumW, 1.0f); // Final alpha is the clamped sum of weights
                Out[t] = SummedColor;
            }
        }
        return Out;
    };

    // Iterate over every material slot that has projection data
    for (const auto& SlotPair : PerVariantProjectionLayers)
    {
        const int32 CurrentSlotIndex = SlotPair.Key;
        const TMap<EProjectionVariant, TArray<TArray<FLinearColor>>>& SlotVariantMap = SlotPair.Value;

        // Skip this slot if it's marked as a hidden occluder
        if (SlotHiddenStates.Contains(CurrentSlotIndex) && SlotHiddenStates[CurrentSlotIndex]) continue;

        // Find the 'Shaded' (or BaseColor) layers for this slot
        const TArray<TArray<FLinearColor>>* ContextLayers = SlotVariantMap.Find(ContextVariant);
        if (!ContextLayers || ContextLayers->Num() == 0) continue;
        
        // Collect all layers *except* the one we are currently projecting
        TArray<TArray<FLinearColor>> LayersToBlend;
        for (int32 LayerIdx = 0; LayerIdx < ContextLayers->Num(); ++LayerIdx)
        {
            // This is the exclusion logic:
            // if (we are on the same material slot AND the same camera index) then skip.
            if (CurrentSlotIndex == SettingsForCurrentView.TargetMaterialSlotIndex && LayerIdx == IndexOfCurrentView)
            {
                continue;
            }

            // Otherwise, if the layer is valid, add it to our blend list
            if ((*ContextLayers).IsValidIndex(LayerIdx) && (*ContextLayers)[LayerIdx].Num() > 0)
            {
                const TArray<FLinearColor>& LayerData = (*ContextLayers)[LayerIdx];
                LayersToBlend.Add(LayerData);

                // // --- NEW DEBUG SAVE (BEFORE BLENDING) ---
                // // This block is a copy of the HDR-correct save logic from BakeWeightedLayerGPU's debug save
                // UE_LOG(LogTemp, Warning, TEXT("PrepareContextualTextureMap: Saving debug asset for PRE-BLEND layer (Slot %d, Cam %d)..."), CurrentSlotIndex, LayerIdx);
                // {
                //     const FString BasePath = TEXT("/Game/TD3D_Debug/");
                //     FString VariantString = StaticEnum<EProjectionVariant>()->GetNameStringByValue((int64)ContextVariant);
                //     const FString AssetName = FString::Printf(TEXT("PreBlend_Layer_%s_Slot%d_Cam%d_%s"),
                //         *VariantString,
                //         CurrentSlotIndex,
                //         LayerIdx,
                //         *FDateTime::Now().ToString(TEXT("HHMMSS"))
                //     );
                //     const FString PackagePath = BasePath + AssetName;
                //     UPackage* Package = CreatePackage(*PackagePath);

                //     if (Package)
                //     {
                //         Package->FullyLoad();
                //         UTexture2D* NewStaticTexture = NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone);
                //         if (NewStaticTexture)
                //         {
                //             NewStaticTexture->AddToRoot();
                //             NewStaticTexture->CompressionSettings = TC_HDR_F32;
                //             NewStaticTexture->SRGB = false; // It's linear data!
                //             NewStaticTexture->Source.Init(Global_OutputTextureWidth, Global_OutputTextureHeight, 1, 1, TSF_RGBA16F);

                //             // Convert 32-bit TArray<FLinearColor> to 16-bit TArray<FFloat16Color>
                //             TArray<FFloat16Color> HdrPixels;
                //             HdrPixels.SetNumUninitialized(LayerData.Num());
                //             for (int32 i = 0; i < LayerData.Num(); ++i)
                //             {
                //                 HdrPixels[i] = FFloat16Color(LayerData[i]);
                //             }

                //             void* DestPixels = NewStaticTexture->Source.LockMip(0);
                //             if (DestPixels)
                //             {
                //                 FMemory::Memcpy(DestPixels, HdrPixels.GetData(), HdrPixels.Num() * sizeof(FFloat16Color));
                //                 NewStaticTexture->Source.UnlockMip(0);

                //                 NewStaticTexture->UpdateResource();
                //                 Package->MarkPackageDirty();
                //                 FAssetRegistryModule::AssetCreated(NewStaticTexture);

                //                 const FString PackageFileName = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
                //                 FSavePackageArgs SaveArgs;
                //                 SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
                                
                //                 if (UPackage::SavePackage(Package, NewStaticTexture, *PackageFileName, SaveArgs))
                //                 {
                //                     UE_LOG(LogTemp, Warning, TEXT("--- DEBUG: Saved PRE-BLEND Layer ASSET to: %s ---"), *PackagePath);
                //                 }
                //                 else 
                //                 { 
                //                     UE_LOG(LogTemp, Error, TEXT("Debug_SavePreBlend: Failed to save package: %s"), *PackagePath); 
                //                 }
                //             }
                //             else { UE_LOG(LogTemp, Error, TEXT("Debug_SavePreBlend: Failed to lock mip.")); }
                //             NewStaticTexture->RemoveFromRoot();
                //         }
                //     }
                // }
                // // --- END NEW DEBUG SAVE ---
            }
        }
        
        if (LayersToBlend.Num() == 0) continue;

        // Blend all valid contextual layers together in Linear space
        TArray<FLinearColor> BlendedLinear = BlendByStoredLayerAlpha(LayersToBlend);
        
        // --- HDR FIX ---
        // Create a 16-bit HDR texture directly from the blended 32-bit linear data.
        UTexture2D* TempSlotTexture = FTextureUtils::CreateTextureFromLinearPixelData(Global_OutputTextureWidth, Global_OutputTextureHeight, BlendedLinear);

        if (TempSlotTexture)
        {
            TempSlotTexture->SetFlags(RF_Transient);
            // SRGB=false is set by CreateTextureFromLinearPixelData
            TempSlotTexture->UpdateResource();

            // // --- DEBUG SAVE (AFTER BLENDING) ---
            // // This saves the 16-bit HDR texture we just created
            // UE_LOG(LogTemp, Warning, TEXT("PrepareContextualTextureMap: Saving debug asset for POST-BLEND context texture (Slot %d)..."), CurrentSlotIndex);
            // this->Debug_SaveTextureAsAsset(
            //     TempSlotTexture, 
            //     FString::Printf(TEXT("ContextMap_Blended_Slot%d"), CurrentSlotIndex)
            // );
            // // --- END DEBUG SAVE ---

            TexturesForRasterization.Add(CurrentSlotIndex, TempSlotTexture);
        }
    }

    return TexturesForRasterization;
}



void FTextureDiffusion3D::FinishBatchProjection()
{
	UE_LOG(LogTemp, Log, TEXT("--- Batch Projection Complete ---"));

	// Clean up state
	bIsBatchMode = false;
	PendingCameraIndices.Empty();
	OnSingleProjectionFinished.Unbind();

	// Notify the user
	FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("All pending projections are complete."));
}


void FTextureDiffusion3D::ProcessNextInBatch()
{
	// If the queue is empty, the batch job is finished.
	if (PendingCameraIndices.Num() == 0)
	{
		FinishBatchProjection();
		return;
	}

	// --- The Correct Logical Order ---

	// 1. Dequeue the next camera index. We now know which job to do.
	const int32 CameraIndexToProcess = PendingCameraIndices[0];
	PendingCameraIndices.RemoveAt(0);

	UE_LOG(LogTemp, Error, TEXT("--- BATCH --- INTENT: Starting job for Camera Index [ %d ] ---"), CameraIndexToProcess);


	// 2. Set the global UI/State context. This call updates the UI and, critically,
	//	the `CurrentSettings` member variable to match the camera we are about to process.
	SelectCameraTab(CameraIndexToProcess);

	// 3. Now that `CurrentSettings` is guaranteed to be correct for this job,
	//	we can safely get a reference to this specific camera's settings.
	const FProjectionSettings& CamSettings = PerSlotCameraSettings.FindChecked(CurrentSettings.TargetMaterialSlotIndex)[CameraIndexToProcess];

	// 4. Now that the `CamSettings` variable exists, we can use its properties (like TabName)
	//	for UI labels and logging.
	int32 RemainingAfterThis = PendingCameraIndices.Num();
	CurrentBatchLabel = FString::Printf(TEXT("%d %s remaining — %s"),
		RemainingAfterThis + 1,
		RemainingAfterThis == 1 ? TEXT("projection") : TEXT("projections"),
		*CamSettings.TabName);

	// 5. Parse its specific workflow
	TArray<FUnrealNodeInfo> ParsedNodes;
	ParseWorkflowForUnrealNodes(CamSettings.WorkflowApiJson, ParsedNodes);
	
	UE_LOG(LogTemp, Log, TEXT("Processing batch camera %d / %d: '%s'"), 
		(CurrentProjection_NumTexels - PendingCameraIndices.Num()), // A bit of a hack for progress
		CurrentProjection_NumTexels, 
		*CamSettings.TabName);

	// 6. Execute the projection.
	ExecuteDynamicProjection(
		SelectedActor,
		CamSettings.WorkflowApiJson,
		CamSettings,
		ParsedNodes,
		CamSettings.ComfyControlValues,
		CamSettings.ComfySeedStates
	);
}

// This is the public-facing function that the UI button will call.
void FTextureDiffusion3D::ExecuteAllCameraProjections()
{
	UE_LOG(LogTemp, Log, TEXT("--- Initializing Batch Projection for All Unprojected Cameras ---"));

	if (bIsBatchMode)
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("A batch projection is already in progress."));
		return;
	}

	if (!SelectedActor)
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("Please select a Static Mesh Actor first."));
		return;
	}

	bIsBatchMode = true;
	PendingCameraIndices.Empty();
	
	const int32 CurrentSlotIndex = CurrentSettings.TargetMaterialSlotIndex;
	TArray<FProjectionSettings>& SlotCameraSettings = PerSlotCameraSettings.FindOrAdd(CurrentSlotIndex);
	
	// *** CORRECTED TYPE AND ACCESS PATTERN ***
	// We check against the BaseColor layers to see if a camera has been projected.
	TArray<TArray<FLinearColor>>& SlotProjectionLayers = PerVariantProjectionLayers.FindOrAdd(CurrentSlotIndex)
																				.FindOrAdd(EProjectionVariant::BaseColor);
	
	// Populate the queue with indices of un-projected cameras.
	for (int32 i = 0; i < SlotCameraSettings.Num(); ++i)
	{
		// A camera is considered "unprojected" if its corresponding layer data is empty or invalid.
		if (!SlotProjectionLayers.IsValidIndex(i) || SlotProjectionLayers[i].Num() == 0)
		{
			PendingCameraIndices.Add(i);
		}
	}

	if (PendingCameraIndices.Num() == 0)
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("All cameras for the current material slot have already been projected for the BaseColor variant."));
		bIsBatchMode = false; // Reset state
		return;
	}

	// Store the total count for logging/progress purposes.
	CurrentProjection_NumTexels = PendingCameraIndices.Num();

	OnSingleProjectionFinished.BindRaw(this, &FTextureDiffusion3D::ProcessNextInBatch);

	// Kick off the first projection in the chain.
	ProcessNextInBatch();
}

// In FTextureDiffusion3D.cpp

void FTextureDiffusion3D::ExecuteDynamicProjection(
    TObjectPtr<AStaticMeshActor> StaticMeshActor,
    const FString& WorkflowPath,
    const FProjectionSettings& CurrentCameraSettings,
    const TArray<FUnrealNodeInfo>& ParsedNodes,
    const TMap<FString, TSharedPtr<FJsonValue>>& ControlValues,
    const TMap<FString, bool>& SeedRandomizationStates)
{
    // This initial setup is required for all paths.
    if (!SetupProjectionContext(StaticMeshActor, CurrentCameraSettings))
    {
        return; // Abort if setup fails.
    }

    // --- Path 1: Simple Local Texture Projection ---
    // This is the simplest case, using a texture asset directly without ComfyUI.
    if (!CurrentCameraSettings.bUseComfyUI)
    {
        UE_LOG(LogTemp, Log, TEXT("Executing simple local texture projection."));
        ExecuteLocalProjection();
        return; // We are done, no need to proceed.
    }

    // --- If we get here, we are using a ComfyUI workflow. Now decide if it's local or remote. ---

    const UTextureDiffusion3DSettings* Settings = GetDefault<UTextureDiffusion3DSettings>();
    const FString ServerAddress = Settings->ComfyUIServerAddress;

    // --- Path 2: Local ComfyUI Workflow ---
    // This uses your original file-saving and folder-polling method.
    if (ServerAddress.Contains(TEXT("localhost")) || ServerAddress.Contains(TEXT("127.0.0.1")))
    {
        UE_LOG(LogTemp, Log, TEXT("Local server detected. Using local file system workflow."));
        
        // Call your ORIGINAL function that handles the local file workflow.
        ExecuteComfyUIProjection(WorkflowPath, ParsedNodes, ControlValues, SeedRandomizationStates);
    }
    // --- Path 3: Remote ComfyUI Workflow ---
    // This uses the new, fully remote API method.
    else if (ServerAddress.Contains(TEXT(".runpod.net")) || ServerAddress.StartsWith(TEXT("http")))
    {
        UE_LOG(LogTemp, Log, TEXT("Remote server detected. Using remote API workflow."));

        // Call our NEW function that will contain the logic from our test button.
        ExecuteRemoteAPIWorkflow(WorkflowPath, ParsedNodes, ControlValues, SeedRandomizationStates);
    }
    // --- Error Path ---
    else
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("Invalid Server Address in Settings. Please provide a valid 'localhost' or 'http' URL.")));
        HandleProjectionError_Internal(TEXT("Invalid server address."));
    }
}


void FTextureDiffusion3D::ExecuteRemoteAPIWorkflow(
    const FString& WorkflowPath,
    const TArray<FUnrealNodeInfo>& ParsedNodes,
    const TMap<FString, TSharedPtr<FJsonValue>>& ControlValues,
    const TMap<FString, bool>& SeedRandomizationStates)
{
    // The first step is to create and upload all necessary input images for the workflow.
    // The rest of the pipeline is chained inside the completion lambda of this function.
    UploadComfyUIInputs(ParsedNodes, 
        [this, WorkflowPath, ControlValues, SeedRandomizationStates](const TMap<FString, FString>& ServerFilenames)
    {
        // This code block runs ONLY after all input image uploads are complete.
        
        FString WorkflowJsonContent;
        if (!FFileHelper::LoadFileToString(WorkflowJsonContent, *WorkflowPath))
        {
            HandleProjectionError_Internal(FString::Printf(TEXT("Failed to load workflow file: %s"), *WorkflowPath));
            return;
        }

        TSharedPtr<FJsonObject> WorkflowObject;
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(WorkflowJsonContent);
        if (FJsonSerializer::Deserialize(Reader, WorkflowObject) && WorkflowObject.IsValid())
        {
            // Step 1: Inject the uploaded image filenames into the JSON.
            for (const auto& Elem : ServerFilenames)
            {
                if (TSharedPtr<FJsonObject> NodeToUpdate = WorkflowObject->GetObjectField(Elem.Key))
                {
                    NodeToUpdate->GetObjectField(TEXT("inputs"))->SetStringField(TEXT("image"), Elem.Value);
                }
            }
            
            // Step 2: Inject user-controlled values from the UI (prompts, sliders, etc.).
            for (const auto& Elem : ControlValues)
            {
                FString NodeId, InputName;
                if (Elem.Key.Split(TEXT("."), &NodeId, &InputName))
                {
                    if (TSharedPtr<FJsonObject> NodeToUpdate = WorkflowObject->GetObjectField(NodeId))
                    {
                        NodeToUpdate->GetObjectField(TEXT("inputs"))->SetField(InputName, Elem.Value);
                    }
                }
            }

            // Step 3: Inject randomized seeds if requested by the UI.
            for (const auto& Elem : SeedRandomizationStates)
            {
                if (Elem.Value) // Check if randomization is enabled for this seed.
                {
                     FString NodeId, InputName;
                     if (Elem.Key.Split(TEXT("."), &NodeId, &InputName))
                     {
                         if (TSharedPtr<FJsonObject> NodeToUpdate = WorkflowObject->GetObjectField(NodeId))
                         {
                             const int64 NewSeed = FMath::RandRange(0LL, 2000000000LL);
                             NodeToUpdate->GetObjectField(TEXT("inputs"))->SetNumberField(InputName, static_cast<double>(NewSeed));
                         }
                     }
                }
            }

            // Step 4: Build the "shopping list" of expected output files from the Save Image nodes.
            this->ExpectedOutputs.Empty();
            for (const auto& NodePair : WorkflowObject->Values)
            {
                const TSharedPtr<FJsonObject> NodeObject = NodePair.Value->AsObject();
                FString ClassType;
                if (NodeObject->TryGetStringField(TEXT("class_type"), ClassType) && ClassType.Equals(TEXT("SaveImage")))
                {
                    const FString Prefix = NodeObject->GetObjectField(TEXT("inputs"))->GetStringField(TEXT("filename_prefix"));
                    EProjectionVariant Variant = VariantFromPrefixLoose(Prefix);
                    this->ExpectedOutputs.Add(Prefix, Variant);
                }
            }
            
            // Step 5: Submit the fully prepared workflow to the server.
            TSharedPtr<FJsonObject> PayloadJson = MakeShareable(new FJsonObject);
            PayloadJson->SetObjectField(TEXT("prompt"), WorkflowObject);
            FString PayloadString;
            TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&PayloadString);
            FJsonSerializer::Serialize(PayloadJson.ToSharedRef(), Writer);

            const UTextureDiffusion3DSettings* Settings = GetDefault<UTextureDiffusion3DSettings>();
            FString RequestURL = Settings->ComfyUIServerAddress + TEXT("prompt");

            TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = FHttpModule::Get().CreateRequest();
            HttpRequest->SetURL(RequestURL);
            HttpRequest->SetVerb(TEXT("POST"));
            HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
            HttpRequest->SetContentAsString(PayloadString);
            
            // The completion of this request kicks off the polling stage.
            HttpRequest->OnProcessRequestComplete().BindLambda(
                [this](FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
                {
                    if (bWasSuccessful && Response.IsValid() && Response->GetResponseCode() == 200)
                    {
                        TSharedPtr<FJsonObject> JsonObject;
                        TSharedRef<TJsonReader<>> JsonReader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
                        if (FJsonSerializer::Deserialize(JsonReader, JsonObject) && JsonObject->HasField(TEXT("prompt_id")))
                        {
                            this->CurrentPromptId = JsonObject->GetStringField(TEXT("prompt_id"));
                            
                            // Create and show the slow task dialog for the polling phase.
                            this->PollingSlowTask = MakeUnique<FScopedSlowTask>(0, FText::FromString("Waiting for ComfyUI to process job..."));
                            this->PollingSlowTask->MakeDialog(true); // Make it a cancellable pop-up window.

                            // Store the start time for timeout checks.
                            this->PollingStartTime = FDateTime::UtcNow();
                            
                            // Start the ticker to call PollHistoryEndpoint every 2 seconds.
                            this->PollingTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
                                FTickerDelegate::CreateRaw(this, &FTextureDiffusion3D::PollHistoryEndpoint), 2.0f);
                        }
                    }
                    else 
                    { 
                        HandleProjectionError_Internal(TEXT("Workflow submission failed.")); 
                    }
                });
            HttpRequest->ProcessRequest();
        }
    });
}



void FTextureDiffusion3D::UploadComfyUIInputs(
    const TArray<FUnrealNodeInfo>& ParsedNodes,
    TFunction<void(const TMap<FString, FString>&)> OnAllUploadsComplete)
{
    TSharedPtr<TMap<FString, FString>> UploadedFilesMap = MakeShared<TMap<FString, FString>>();
    TSharedPtr<int32> PendingUploads = MakeShared<int32>(0);
    FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));

    for (const FUnrealNodeInfo& Node : ParsedNodes)
    {
        if (!Node.bIsImageInput) continue;

        TArray64<uint8> PngData;
        FString Filename;
        
        if (Node.ImageKeyword.Equals(TEXT("reference image"), ESearchCase::IgnoreCase))
        {
            // For reference images, we don't generate anything. We find the file that was saved earlier.
            const int32 CurrentSlotIndex = CurrentProjection_Settings.TargetMaterialSlotIndex;
            if (const FString* RefPath = PerSlotReferenceImagePath.Find(CurrentSlotIndex))
            {
                if (FPaths::FileExists(*RefPath))
                {
                    // Load the existing file's data directly into a buffer.
                    if (FFileHelper::LoadFileToArray(PngData, **RefPath))
                    {
                        Filename = FPaths::GetCleanFilename(*RefPath);
                    }
                }
            }
        }
        else
        {
            // For all other inputs, we generate a texture first.
			bool bGenerated = false;
            if (Node.ImageKeyword.Equals(TEXT("depth"), ESearchCase::IgnoreCase))
            {
                bGenerated = this->GenerateDepthDataForUpload(PngData);
                Filename = FString::Printf(TEXT("ue_depth_%s.png"), *Timestamp);
            }
            else if (Node.ImageKeyword.Equals(TEXT("base color"), ESearchCase::IgnoreCase) || Node.ImageKeyword.Equals(TEXT("mask"), ESearchCase::IgnoreCase))
            {
                bGenerated = this->GenerateBaseColorDataForUpload(PngData);
                Filename = FString::Printf(TEXT("ue_basecolor_%s.png"), *Timestamp);
            }


        }

        // If we have valid PNG data (either generated or loaded from file), start the upload.
        if (PngData.Num() > 0)
        {
            (*PendingUploads)++;
            FOnImageUploadComplete OnCompleteDelegate = FOnImageUploadComplete::CreateLambda(
                [Node, UploadedFilesMap, PendingUploads, OnAllUploadsComplete](bool bSuccess, const FString& ServerFilename)
                {
                    if (bSuccess)
                    {
                        UploadedFilesMap->Add(Node.NodeId, ServerFilename);
                    }
                    
                    (*PendingUploads)--;
                    if (*PendingUploads == 0)
                    {
                        OnAllUploadsComplete(*UploadedFilesMap);
                    }
                });
            this->UploadImageToComfyUI(PngData, Filename, OnCompleteDelegate);
        }
    }

    // If there were no images to upload, complete immediately.
    if (*PendingUploads == 0)
    {
        OnAllUploadsComplete(*UploadedFilesMap);
    }
}

bool FTextureDiffusion3D::SetupProjectionContext(
    TObjectPtr<AStaticMeshActor> StaticMeshActor, 
    const FProjectionSettings& CurrentCameraSettings)
{
    if (!ParentMaterial_CumulativeDisplay || !ParentMaterial_WeightMaskDisplay)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("Projection Failed: Parent materials for visualization could not be loaded.")));
        return false;
    }

    UE_LOG(LogTemp, Log, TEXT("SetupProjectionContext: Preparing for Slot %d, Camera Index %d ('%s')."), 
        CurrentCameraSettings.TargetMaterialSlotIndex, ActiveCameraIndex, *CurrentCameraSettings.TabName);

    this->CurrentProjection_Actor = StaticMeshActor;
    this->CurrentProjection_MeshComponent = StaticMeshActor->GetStaticMeshComponent();
    this->CurrentProjection_StaticMesh = CurrentProjection_MeshComponent->GetStaticMesh();
    this->CurrentProjection_Settings = CurrentCameraSettings;
    this->CurrentProjection_CameraIndex = ActiveCameraIndex;
    this->CurrentProjection_TextureWidth = Global_OutputTextureWidth;
    this->CurrentProjection_TextureHeight = Global_OutputTextureHeight;
    this->CurrentProjection_NumTexels = Global_OutputTextureWidth * Global_OutputTextureHeight;
    this->bHasStartedProjections = true;

    if (this->PrecomputedUVIslandMap.Num() != this->CurrentProjection_NumTexels)
    {
        UE_LOG(LogTemp, Log, TEXT("Generating precomputed UV Island Map for seam fixing..."));
			this->PrecomputedUVIslandMap = FMeshProcessor::GenerateUVIslandIDMap(
			this->CurrentProjection_StaticMesh,
			this->TargetUVChannel,
			this->CurrentProjection_TextureWidth,
			this->CurrentProjection_TextureHeight,
			CurrentProjection_Settings.TargetMaterialSlotIndex);
			{
            UE_LOG(LogTemp, Warning, TEXT("SetupProjectionContext (Debug): Saving UV Island Map visualization..."));

            const int32 W = this->CurrentProjection_TextureWidth;
            const int32 H = this->CurrentProjection_TextureHeight;
            const FString BasePath = TEXT("/Game/TD3D_Debug/");
            
            const FString AssetName = FString::Printf(TEXT("Debug_UVIslandMap_Slot%d_Cam%d_%s"),
                CurrentProjection_Settings.TargetMaterialSlotIndex,
                ActiveCameraIndex,
                *FDateTime::Now().ToString(TEXT("HHMMSS"))
            );

            const FString PackagePath = BasePath + AssetName;
            UPackage* Package = CreatePackage(*PackagePath);
            
            if (Package)
            {
                Package->FullyLoad();
                UTexture2D* DebugTexture = NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone);
                if (DebugTexture)
                {
                    DebugTexture->AddToRoot();
                    DebugTexture->CompressionSettings = TC_VectorDisplacementmap; // Lossless
                    DebugTexture->SRGB = false;
                    DebugTexture->Source.Init(W, H, 1, 1, TSF_BGRA8);

                    TArray<FColor> DebugPixels;
                    DebugPixels.SetNumUninitialized(W * H);

                    int32 MaxIslandID = 0;

                    for (int32 i = 0; i < this->PrecomputedUVIslandMap.Num(); ++i)
                    {
                        const int32 IslandID = this->PrecomputedUVIslandMap[i];
                        if (IslandID > MaxIslandID) MaxIslandID = IslandID;

                        if (IslandID < 0)
                        {
                            DebugPixels[i] = FColor::Black; // Empty/Invalid
                        }
                        else
                        {
                            // Golden Ratio Color Generator for distinct Island IDs
                            const float Hue = FMath::Frac(IslandID * 0.618033988749895f);
                            const float Saturation = 0.7f + 0.3f * FMath::Frac(IslandID * 0.381966f);
                            const float Value = 0.8f + 0.2f * FMath::Frac(IslandID * 0.5f);

                            // HSV -> RGB conversion
                            const float H_val = Hue * 6.0f;
                            const int32 Hi = FMath::FloorToInt(H_val) % 6;
                            const float F = H_val - FMath::FloorToInt(H_val);
                            const float P = Value * (1.0f - Saturation);
                            const float Q = Value * (1.0f - F * Saturation);
                            const float T = Value * (1.0f - (1.0f - F) * Saturation);

                            float R, G, B;
                            switch (Hi) {
                                case 0: R = Value; G = T; B = P; break;
                                case 1: R = Q; G = Value; B = P; break;
                                case 2: R = P; G = Value; B = T; break;
                                case 3: R = P; G = Q; B = Value; break;
                                case 4: R = T; G = P; B = Value; break;
                                default: R = Value; G = P; B = Q; break;
                            }

                            DebugPixels[i] = FColor(
                                (uint8)(FMath::Clamp(R, 0.0f, 1.0f) * 255.0f),
                                (uint8)(FMath::Clamp(G, 0.0f, 1.0f) * 255.0f),
                                (uint8)(FMath::Clamp(B, 0.0f, 1.0f) * 255.0f),
                                255
                            );
                        }
                    }

                    void* DestPixels = DebugTexture->Source.LockMip(0);
                    if (DestPixels)
                    {
                        FMemory::Memcpy(DestPixels, DebugPixels.GetData(), DebugPixels.Num() * sizeof(FColor));
                        DebugTexture->Source.UnlockMip(0);

                        DebugTexture->UpdateResource();
                        Package->MarkPackageDirty();
                        FAssetRegistryModule::AssetCreated(DebugTexture);

                        // Save to disk immediately
                        const FString PackageFileName = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
                        FSavePackageArgs SaveArgs;
                        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
                        UPackage::SavePackage(Package, DebugTexture, *PackageFileName, SaveArgs);
                        
                        UE_LOG(LogTemp, Warning, TEXT("--- DEBUG: Saved UV ISLAND MAP to: %s (Max Island ID: %d) ---"), *PackagePath, MaxIslandID);
                    }
                    DebugTexture->RemoveFromRoot();
                }
            }
        }

    }

    const int32 TargetSlot = CurrentProjection_Settings.TargetMaterialSlotIndex;
    const int32 CameraIndex = CurrentProjection_CameraIndex;
    
    // This section ensures any old data for this specific camera is cleared before a new projection begins.

    // 1. Clear data from the main variant layers map (PerVariantProjectionLayers)
    if (TMap<EProjectionVariant, TArray<TArray<FLinearColor>>>* SlotVariantMap = PerVariantProjectionLayers.Find(TargetSlot))
    {
        // If the slot exists, iterate through all of its variants (BaseColor, Normal, etc.).
        for (auto& VariantPair : *SlotVariantMap)
        {
            // *** THE FIX IS HERE: The type of CameraLayers must match the data in the map ***
            TArray<TArray<FLinearColor>>& CameraLayers = VariantPair.Value;

            // If a layer for our target camera index exists, empty it.
            if (CameraLayers.IsValidIndex(CameraIndex))
            {
                CameraLayers[CameraIndex].Empty();
            }
        }
    }

    // 2. Clear data from the weights map.
    if (TArray<TArray<float>>* WeightLayers = PerSlotWeightLayers.Find(TargetSlot))
    {
        if (WeightLayers->IsValidIndex(CameraIndex))
        {
            (*WeightLayers)[CameraIndex].Empty();
        }
    }
    
    UE_LOG(LogTemp, Log, TEXT("Cleared any existing layer data for Slot %d, Camera %d in preparation for new projection."), TargetSlot, CameraIndex);

    return true;
}


void FTextureDiffusion3D::ExecuteLocalProjection()
{
	UTexture2D* Tex = CurrentProjection_Settings.SourceTexture.Get();
	if (!IsValid(Tex))
	{
		HandleProjectionError_Internal(TEXT("'Use ComfyUI' is unchecked, but no Source Texture was selected."));
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("No Source Texture selected.")));
		return;
	}

	// Wrap single result as BaseColor and process via the unified path
	TMap<EProjectionVariant, UTexture2D*> Results;
	Results.Add(EProjectionVariant::Shaded, Tex);

	ProcessProjectionResults(Results);
}

void FTextureDiffusion3D::ExecuteComfyUIProjection(
	const FString& WorkflowPath,
	const TArray<FUnrealNodeInfo>& ParsedNodes,
	const TMap<FString, TSharedPtr<FJsonValue>>& ControlValues,
	const TMap<FString, bool>& SeedRandomizationStates)
{
	const FString BasePath = GetComfyUIBasePath();
	if (BasePath.IsEmpty())
	{
		FMessageDialog::Open(EAppMsgType::Ok,
			FText::FromString(TEXT("The ComfyUI Installation Path is not set.\nPlease configure it in Editor Preferences -> Plugins -> Texture Diffusion 3D")));
		HandleProjectionError_Internal(TEXT("ComfyUI path is not configured."));
		return;
	}

	// 1) Generate & save inputs for ComfyUI
	const FString FileTimestamp = FDateTime::Now().ToString();
	const FString ComfyInputPath = BasePath + TEXT("input/");
	TMap<FString, FString> KeywordToFilenameMap =
		PrepareComfyUIInputs(ParsedNodes, FileTimestamp, ComfyInputPath);

	// 2) Load workflow JSON
	TSharedPtr<FJsonObject> WorkflowObject = GetWorkflowJsonObject(WorkflowPath);
	if (!WorkflowObject.IsValid())
	{
		HandleProjectionError_Internal(TEXT("Failed to load workflow JSON."));
		return;
	}

	// 3) Walk SaveImage nodes, inject unique prefixes, collect output requests
	const FString JobToken = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);

	// Map of NewPrefix -> Variant (1:1)
	TMap<FString, EProjectionVariant> OutputRequests;

	for (const auto& Pair : WorkflowObject->Values)
	{
		const TSharedPtr<FJsonObject>* NodeObjPtr = nullptr;
		if (!Pair.Value->TryGetObject(NodeObjPtr)) continue;
		const TSharedPtr<FJsonObject> NodeObj = *NodeObjPtr;

		FString ClassType;
		if (!NodeObj->TryGetStringField(TEXT("class_type"), ClassType)) continue;
		if (!ClassType.Equals(TEXT("SaveImage"), ESearchCase::IgnoreCase)) continue;

		TSharedPtr<FJsonObject> Inputs = NodeObj->GetObjectField(TEXT("inputs"));
		if (!Inputs.IsValid()) continue;

		FString PrefixValue;
		if (!Inputs->TryGetStringField(TEXT("filename_prefix"), PrefixValue)) continue;

		// map to a variant
		EProjectionVariant Variant = VariantFromPrefixLoose(PrefixValue);

		// avoid accidental reference collisions
		if (PrefixValue.StartsWith(TEXT("Reference_Image_"), ESearchCase::IgnoreCase))
		{
			UE_LOG(LogTemp, Warning, TEXT("Output prefix looked like a reference; overriding to BaseColor."));
			Variant = EProjectionVariant::BaseColor;
		}

		// inject unique prefix and record request
		const FString NewPrefix = FString::Printf(TEXT("%s__%s__id%s"), *PrefixValue, *JobToken, *Pair.Key);
		Inputs->SetStringField(TEXT("filename_prefix"), NewPrefix);
		OutputRequests.Add(NewPrefix, Variant);

		UE_LOG(LogTemp, Verbose, TEXT("SaveImage node: old prefix='%s' -> new prefix='%s' (Variant=%d)"),
			*PrefixValue, *NewPrefix, (int32)Variant);
	}

	if (OutputRequests.Num() == 0)
	{
		HandleProjectionError_Internal(TEXT("No SaveImage nodes found in workflow."));
		return;
	}

	// 4) Inject image file paths for tagged Unreal inputs (by node id)
	for (const auto& Elem : KeywordToFilenameMap)
	{
		if (TSharedPtr<FJsonObject> NodeToUpdate = WorkflowObject->GetObjectField(Elem.Key))
		{
			if (TSharedPtr<FJsonObject> Inputs = NodeToUpdate->GetObjectField(TEXT("inputs")))
			{
				Inputs->SetStringField(TEXT("image"), Elem.Value);
			}
		}
	}

	// 5) Inject exposed control values (text, sliders, etc.)
	for (const auto& Elem : ControlValues)
	{
		FString NodeId, InputName;
		if (Elem.Key.Split(TEXT("."), &NodeId, &InputName))
		{
			if (TSharedPtr<FJsonObject> NodeToUpdate = WorkflowObject->GetObjectField(NodeId))
			{
				if (TSharedPtr<FJsonObject> Inputs = NodeToUpdate->GetObjectField(TEXT("inputs")))
				{
					Inputs->SetField(InputName, Elem.Value);
				}
			}
		}
	}

	// 6) Inject randomized seeds when requested
	for (const auto& Elem : SeedRandomizationStates)
	{
		if (!Elem.Value) continue;

		FString NodeId, InputName;
		if (Elem.Key.Split(TEXT("."), &NodeId, &InputName))
		{
			if (TSharedPtr<FJsonObject> NodeToUpdate = WorkflowObject->GetObjectField(NodeId))
			{
				if (TSharedPtr<FJsonObject> Inputs = NodeToUpdate->GetObjectField(TEXT("inputs")))
				{
					const int64 NewSeed = FMath::RandRange(0LL, 2000000000LL);
					Inputs->SetNumberField(InputName, static_cast<double>(NewSeed));
				}
			}
		}
	}

	TMap<EProjectionVariant, int32> VariantCounts;
	for (const auto& It : OutputRequests)
	{
		VariantCounts.FindOrAdd(It.Value)++;
	}
	for (const auto& VC : VariantCounts)
	{
		if (VC.Value > 1)
		{
			UE_LOG(LogTemp, Warning, TEXT("Workflow emits %d outputs for variant %d. First-found will be used."), VC.Value, (int32)VC.Key);
		}
	}


	// 7) Submit job to ComfyUI with the modified workflow
	//    (we keep the "expected prefix" arg for logging/debug; pick the first request)
	const FString FirstPrefix = OutputRequests.CreateConstIterator()->Key;
	{
    // Create the throttler here. It will activate immediately.
		FScopedUEPerformanceThrottler PerfGuard;
		UE_LOG(LogTemp, Log, TEXT("Performance throttler activated"));

		// Now send the request while UE is throttled.
		RunComfyUIWorkflow(WorkflowObject, FirstPrefix, TEXT(""));
	}

	// 8) Poll until ALL requested outputs are available, then hand results back by variant
	BeginPollingForComfyOutputs(
		OutputRequests,
		FOnComfyAllImagesReady::CreateLambda([this](const TMap<EProjectionVariant, UTexture2D*>& ResultsByVariant)
		{
			this->ProcessProjectionResults(ResultsByVariant);
		}));
}



// NOTE: This function is still long, but its responsibility is now singular: generate files.
TMap<FString, FString> FTextureDiffusion3D::PrepareComfyUIInputs(
	const TArray<FUnrealNodeInfo>& ParsedNodes,
	const FString& FileTimestamp,
	const FString& ComfyInputPath)
{
	TMap<FString, FString> KeywordToFilenameMap;
	TArray<FColor> PerspectiveLinearRGBAData; // cache: linear (from rasterizer)
	TArray<FLinearColor> SrgbPerspectiveRGBAData;   // cache: sRGB (derived once)

auto GetOrComputePerspectiveView = [&]() -> const TArray<FLinearColor>& {
    // 1. Check if we've already computed this view
		if (SrgbPerspectiveRGBAData.IsEmpty())
    {
        // 2. Prepare the textures from other slots for context
			TMap<int32, TObjectPtr<UTexture2D>> TexturesForRasterization = PrepareContextualTextureMap(CurrentProjection_Settings, CurrentProjection_CameraIndex);

			// // --- 3. NEW DEBUG SAVE ---
            // // Save all textures that are about to be passed *into* the rasterizer.
            // UE_LOG(LogTemp, Warning, TEXT("GetOrComputePerspectiveView: Saving %d pre-rasterization textures for debugging..."), TexturesForRasterization.Num());
            // for (auto const& Pair : TexturesForRasterization)
            // {
            //     if (Pair.Value)
            //     {
            //         // This calls the FTextureDiffusion3D member function for saving assets
            //         this->Debug_SaveTextureAsAsset(
            //             Pair.Value, 
            //             FString::Printf(TEXT("PreRaster_Input_Slot%d"), Pair.Key)
            //         );
            //     }
            // }
            // // --- END NEW DEBUG SAVE ---

        // 3. Call the rasterizer. It outputs sRGB FColor data directly.
			//    Store it in the SrgbPerspectiveRGBAData cache.
			FMeshProcessor::GenerateRasterizedView(
            SrgbPerspectiveRGBAData, // <--- Output directly into the sRGB cache
				CurrentProjection_Actor, CurrentProjection_Settings,
            TexturesForRasterization, Global_OutputTextureWidth, Global_OutputTextureHeight, TargetUVChannel);

        // 4. Clean up temporary textures
			for (auto const& TempTexturePair : TexturesForRasterization) {
            if (TempTexturePair.Value) TempTexturePair.Value->MarkAsGarbage();
        }
    }
    
		// 5. Return the cached sRGB data (which was never double-converted)
		return SrgbPerspectiveRGBAData;
};
	for (const FUnrealNodeInfo& Node : ParsedNodes)
	{
		if (!Node.bIsImageInput) continue;

		FString UniqueFilename;
		bool bSuccess = false;

		if (Node.ImageKeyword.Equals(TEXT("depth"), ESearchCase::IgnoreCase))
		{
			UniqueFilename = FString::Printf(TEXT("ue_depth_%s.png"), *FileTimestamp);
			TArray<float> TempDepthBuffer;
			CaptureMeshDepth(CurrentProjection_Actor, CurrentProjection_Settings, 1024, 1024, TempDepthBuffer, ComfyInputPath, FPaths::GetBaseFilename(UniqueFilename));
			bSuccess = FPaths::FileExists(ComfyInputPath / UniqueFilename);
		}
		else if (Node.ImageKeyword.Equals(TEXT("canny"), ESearchCase::IgnoreCase))
        {
            UE_LOG(LogTemp, Log, TEXT("Found 'crease_map' input node. Generating procedural hard edges from mesh."));
            TSet<int32> HiddenSlotIndices;
			for (const auto& Elem : SlotHiddenStates)
			{
				if (Elem.Value) // Elem.Value is true if the slot is hidden
				{
					HiddenSlotIndices.Add(Elem.Key); // Elem.Key is the slot index
				}
			}

            TArray<FColor> CreaseMapPixels;
            bSuccess = FMeshProcessor::GenerateCannyView(
                CreaseMapPixels,
                CurrentProjection_Actor,
				CurrentProjection_Settings,
                1024, // Or Global_OutputTextureWidth
                1024, // Or Global_OutputTextureHeight
                HiddenSlotIndices,
                15.0f // Angle Threshold in Degrees
            );

            if (bSuccess)
            {
                UniqueFilename = FString::Printf(TEXT("ue_crease_map_%s.png"), *FileTimestamp);
                TArray64<uint8> CompressedPNG;
                FImageUtils::PNGCompressImageArray(1024, 1024, CreaseMapPixels, CompressedPNG);
                bSuccess = FFileHelper::SaveArrayToFile(CompressedPNG, *(ComfyInputPath / UniqueFilename));
            }
        }

		else if (Node.ImageKeyword.Equals(TEXT("normals"), ESearchCase::IgnoreCase))
		{
			UniqueFilename = FString::Printf(TEXT("ue_normal_%s.png"), *FileTimestamp);
			const FString FullOutputPath = ComfyInputPath / UniqueFilename;

			// 1. Prepare the output buffer. Note the change to TArray<FColor>.
			TArray<FColor> TempNormalsBuffer;

			TSet<int32> HiddenSlotIndices;
			for (const auto& Elem : SlotHiddenStates)
			{
				if (Elem.Value) // Elem.Value is true if the slot is hidden
				{
					HiddenSlotIndices.Add(Elem.Key); // Elem.Key is the slot index
				}
			}

			// Call the new, reliable CPU rasterizer, passing the hidden slots.
			const bool bRasterizationSuccess = FMeshProcessor::GenerateRasterizedViewSpaceNormals(
				TempNormalsBuffer,
				CurrentProjection_Actor,
				CurrentProjection_Settings,
				1024,
				1024,
				HiddenSlotIndices // Pass the set of hidden slots here
			);

			// 3. If the rasterizer created the pixel data, save it to a file.
			if (bRasterizationSuccess && TempNormalsBuffer.Num() > 0)
			{
				TArray64<uint8> PngData;
				FImageUtils::PNGCompressImageArray(1024, 1024, TempNormalsBuffer, PngData);
				
				// FFileHelper::SaveArrayToFile returns true on success. This is our final success check.
				bSuccess = FFileHelper::SaveArrayToFile(PngData, *FullOutputPath);
			}
			else
			{
				// If rasterization failed, ensure bSuccess is false.
				bSuccess = false;
				UE_LOG(LogTemp, Error, TEXT("CPU Normal Rasterization failed to generate a pixel buffer."));
			}

			UE_LOG(LogTemp, Log, TEXT("Captured normals for ComfyUI input via CPU rasterizer. File saved successfully: %s"), bSuccess ? TEXT("true") : TEXT("false"));
		}
		else if (
			Node.ImageKeyword.Equals(TEXT("base color"), ESearchCase::IgnoreCase) ||
			Node.ImageKeyword.Equals(TEXT("mask"), ESearchCase::IgnoreCase)
		)
		{
			const TArray<FLinearColor>& LinearPixels = GetOrComputePerspectiveView();
			UniqueFilename = FString::Printf(TEXT("ue_rgba_mask_%s.png"), *FileTimestamp);
			TArray64<uint8> CompressedPNG;
			TArray<FColor> SrgbPixels = FTextureUtils::LinearTo_sRGB(LinearPixels);
			FImageUtils::PNGCompressImageArray(1024, 1024, SrgbPixels, CompressedPNG);
			bSuccess = FFileHelper::SaveArrayToFile(CompressedPNG, *(ComfyInputPath / UniqueFilename));
		}

		else if (Node.ImageKeyword.Equals(TEXT("reference image"), ESearchCase::IgnoreCase))
		{
			if (const FString* SavedRefPath = PerSlotReferenceImagePath.Find(CurrentProjection_Settings.TargetMaterialSlotIndex))
			{
				if (FPaths::FileExists(*SavedRefPath)) {
					UniqueFilename = FPaths::GetCleanFilename(*SavedRefPath);
					bSuccess = true;
				}
			}
		}
		
		if (bSuccess) {
			KeywordToFilenameMap.Add(Node.NodeId, UniqueFilename);
		}
	}
	return KeywordToFilenameMap;
}


void FTextureDiffusion3D::UploadImageToComfyUI(const TArray64<uint8>& PngData, const FString& OriginalFilename, FOnImageUploadComplete OnComplete)
{
    const UTextureDiffusion3DSettings* Settings = GetDefault<UTextureDiffusion3DSettings>();
    if (Settings->ComfyUIServerAddress.IsEmpty())
    {
        UE_LOG(LogTemp, Error, TEXT("UploadImageToComfyUI: ComfyUI API URL is not set in project settings."));
        OnComplete.ExecuteIfBound(false, TEXT(""));
        return;
    }

    FString RequestURL = Settings->ComfyUIServerAddress + TEXT("upload/image");

    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = FHttpModule::Get().CreateRequest();
    HttpRequest->SetURL(RequestURL);
    HttpRequest->SetVerb(TEXT("POST"));

    // --- Build multipart/form-data payload ---
    FString Boundary = "---------------------------" + FGuid::NewGuid().ToString();
    HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("multipart/form-data; boundary=") + Boundary);

    TArray<uint8> RequestContent;
    FString Header = "\r\n--" + Boundary + "\r\n";
    // The 'overwrite' flag is useful for testing, so you don't fill your pod with test files.
    Header += "Content-Disposition: form-data; name=\"overwrite\"\r\n\r\n";
    Header += "true\r\n";
    Header += "--" + Boundary + "\r\n";
    Header += "Content-Disposition: form-data; name=\"image\"; filename=\"" + OriginalFilename + "\"\r\n";
    Header += "Content-Type: image/png\r\n\r\n";
    RequestContent.Append((uint8*)TCHAR_TO_ANSI(*Header), Header.Len());
    RequestContent.Append(PngData.GetData(), PngData.Num());
    FString Footer = "\r\n--" + Boundary + "--\r\n";
    RequestContent.Append((uint8*)TCHAR_TO_ANSI(*Footer), Footer.Len());

    HttpRequest->SetContent(RequestContent);

    // --- Bind completion callback ---
    HttpRequest->OnProcessRequestComplete().BindLambda(
        [OnComplete, OriginalFilename](FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
        {
            if (bWasSuccessful && Response.IsValid() && Response->GetResponseCode() == 200)
            {
                FString ResponseStr = Response->GetContentAsString();
                TSharedPtr<FJsonObject> JsonObject;
                TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseStr);
                if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid())
                {
                    FString ServerFilename = JsonObject->GetStringField(TEXT("name"));
                    UE_LOG(LogTemp, Log, TEXT("Image '%s' uploaded successfully. Server filename: %s"), *OriginalFilename, *ServerFilename);
                    OnComplete.ExecuteIfBound(true, ServerFilename);
                    return;
                }
            }
            
            UE_LOG(LogTemp, Error, TEXT("Image upload failed. Response code: %d"), Response.IsValid() ? Response->GetResponseCode() : -1);
            OnComplete.ExecuteIfBound(false, TEXT(""));
        });

    UE_LOG(LogTemp, Log, TEXT("Sending upload request for '%s' to %s"), *OriginalFilename, *RequestURL);
    HttpRequest->ProcessRequest();
}

void FTextureDiffusion3D::DownloadAllFinalImages(const TMap<FString, EProjectionVariant>& FilesToDownload)
{
	// This map will store the results as they arrive.
	TSharedPtr<TMap<EProjectionVariant, UTexture2D*>> DownloadedTextures = MakeShared<TMap<EProjectionVariant, UTexture2D*>>();
	
	// We use a counter to know when all downloads are finished.
	TSharedPtr<int32> PendingDownloads = MakeShared<int32>(FilesToDownload.Num());

	if (*PendingDownloads == 0)
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("No files to download."));
		return;
	}

	// Get the current slot index for saving reference image.
	const int32 CurrentSlotIndex = this->CurrentProjection_Settings.TargetMaterialSlotIndex;

	for (const auto& FilePair : FilesToDownload)
	{
		const FString& Filename = FilePair.Key;
		const EProjectionVariant Variant = FilePair.Value;

		const UTextureDiffusion3DSettings* Settings = GetDefault<UTextureDiffusion3DSettings>();
		FString RequestURL = FString::Printf(TEXT("%sview?filename=%s"), *Settings->ComfyUIServerAddress, *Filename);

		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = FHttpModule::Get().CreateRequest();
		HttpRequest->SetURL(RequestURL);
		HttpRequest->SetVerb(TEXT("GET"));

		HttpRequest->OnProcessRequestComplete().BindLambda(
			[this, Variant, DownloadedTextures, PendingDownloads, CurrentSlotIndex](FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
			{
				if (bWasSuccessful && Response.IsValid() && Response->GetResponseCode() == 200)
				{
					const TArray<uint8> PngData = Response->GetContent();
					UTexture2D* NewTexture = FImageUtils::ImportBufferAsTexture2D(PngData);
					if (NewTexture)
					{
						// Set the correct properties for the newly imported texture
						
						if (Variant == EProjectionVariant::Normal)
						{
							// Normal maps are linear data
							NewTexture->CompressionSettings = TC_VectorDisplacementmap; 
							NewTexture->SRGB = false;
						}
						else
						{
							// All other maps (BaseColor, Shaded) are sRGB color
							NewTexture->SRGB = true;
							NewTexture->CompressionSettings = TC_Default;
						}
						// Set the filter to Bilinear to prevent blockiness
						NewTexture->Filter = TF_Bilinear;
						// Apply the settings to the texture resource
						NewTexture->UpdateResource();
						// Add the successful download to our results map
						DownloadedTextures->Add(Variant, NewTexture);

						if (Variant == EProjectionVariant::Shaded && this->CurrentProjection_CameraIndex == 0)
                        {
                            // 1. Define a persistent save path
                            const FString CacheDir = FPaths::ProjectSavedDir() / TEXT("TD3D_Cache");
                            IFileManager::Get().MakeDirectory(*CacheDir, /*Tree=*/true);
                            const FString SavedRefPath = FPaths::Combine(CacheDir, FString::Printf(TEXT("ref_slot_%d.png"), CurrentSlotIndex));

                            // 2. Save the downloaded PNG data to that file
                            if (FFileHelper::SaveArrayToFile(PngData, *SavedRefPath))
                            {
                                // 3. Store this new path in our map for the next projection to find.
                                this->PerSlotReferenceImagePath.Add(CurrentSlotIndex, SavedRefPath);
                                UE_LOG(LogTemp, Log, TEXT("Saved new reference image for Slot %d from FIRST PROJECTION (Cam 0) to: %s"), CurrentSlotIndex, *SavedRefPath);
                            }
                            else
                            {
                                UE_LOG(LogTemp, Error, TEXT("Failed to save reference image to: %s"), *SavedRefPath);
                            }
                        }
					}
				}
				
				// Decrement the counter whether it succeeded or failed
				(*PendingDownloads)--;

				// If this was the last download, we are done!
				if (*PendingDownloads == 0)
				{
					UE_LOG(LogTemp, Log, TEXT("All downloads complete. Received %d textures. Processing results..."), DownloadedTextures->Num());
					
					// Instead of showing a message, call your main processing function.
					// This will calculate weights, create projection layers, and update the mesh material.
					ProcessProjectionResults(*DownloadedTextures);
				}
			});
		HttpRequest->ProcessRequest();
	}
}

bool FTextureDiffusion3D::PollHistoryEndpoint(float DeltaTime)
{
    // First, check for user cancellation from the slow task dialog.
    if (PollingSlowTask.IsValid() && PollingSlowTask->ShouldCancel())
    {
        UE_LOG(LogTemp, Warning, TEXT("Polling for ComfyUI job cancelled by user."));
        PollingSlowTask.Reset(); // This destroys the dialog window.
        CurrentPromptId.Empty();
        return false; // Returning false stops the ticker.
    }

    // Next, check for a timeout to prevent an infinite loop.
    const float TimeoutSeconds = 600.0f; // 10-minute timeout.
    if ((FDateTime::UtcNow() - PollingStartTime).GetTotalSeconds() > TimeoutSeconds)
    {
        UE_LOG(LogTemp, Error, TEXT("Polling for ComfyUI job timed out after %f seconds."), TimeoutSeconds);
        PollingSlowTask.Reset(); // Close the dialog.
        CurrentPromptId.Empty();
        HandleProjectionError_Internal(TEXT("Polling timed out waiting for ComfyUI response."));
        return false; // Stop the ticker.
    }

    // Keep the marquee progress bar on the dialog moving.
    if (PollingSlowTask.IsValid())
    {
        PollingSlowTask->EnterProgressFrame(0);
    }

    // If the prompt ID was cleared for any reason, stop.
    if (CurrentPromptId.IsEmpty())
    {
        PollingSlowTask.Reset();
        return false;
    }
    
    // The rest of the function sends the HTTP request to check the job status.
    const UTextureDiffusion3DSettings* Settings = GetDefault<UTextureDiffusion3DSettings>();
    FString RequestURL = Settings->ComfyUIServerAddress + TEXT("history/") + CurrentPromptId;

    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = FHttpModule::Get().CreateRequest();
    HttpRequest->SetURL(RequestURL);
    HttpRequest->SetVerb(TEXT("GET"));

    HttpRequest->OnProcessRequestComplete().BindLambda(
        [this](FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
        {
            if (bWasSuccessful && Response.IsValid() && Response->GetResponseCode() == 200)
            {
                TSharedPtr<FJsonObject> JsonObject;
                TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());

                // The history endpoint's response contains the prompt ID as a key when the job is finished.
                if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject->HasField(this->CurrentPromptId))
                {
                    // --- Job is complete! ---
                    
                    // 1. First, destroy the slow task dialog to close the pop-up window.
                    this->PollingSlowTask.Reset();
                    
                    // 2. Stop the polling timer.
                    FTSTicker::GetCoreTicker().RemoveTicker(this->PollingTickerHandle);
                    this->PollingTickerHandle.Reset();
                    
                    // 3. Parse the JSON response to find all our expected output images.
                    TMap<FString, EProjectionVariant> FoundOutputsToDownload;
                    const TSharedPtr<FJsonObject> HistoryEntry = JsonObject->GetObjectField(this->CurrentPromptId);
                    const TSharedPtr<FJsonObject> Outputs = HistoryEntry->GetObjectField(TEXT("outputs"));

                    for (const auto& OutputNodePair : Outputs->Values)
                    {
                        const TSharedPtr<FJsonObject> OutputNodeObject = OutputNodePair.Value->AsObject();
                        const TArray<TSharedPtr<FJsonValue>>* ImagesArray;

                        if (OutputNodeObject->TryGetArrayField(TEXT("images"), ImagesArray))
                        {
                            for(const auto& ImageValue : *ImagesArray)
                            {
                                FString OutputFilename = ImageValue->AsObject()->GetStringField(TEXT("filename"));
                                
                                // Check if this filename starts with one of the prefixes we're looking for.
                                for(const auto& ExpectedPair : this->ExpectedOutputs)
                                {
                                    if(OutputFilename.StartsWith(ExpectedPair.Key))
                                    {
                                        FoundOutputsToDownload.Add(OutputFilename, ExpectedPair.Value);
                                        break; // Found a match for this image, move to the next one.
                                    }
                                }
                            }
                        }
                    }

                    // 4. If we found files to download, start the download process.
                    if (FoundOutputsToDownload.Num() > 0)
                    {
                         UE_LOG(LogTemp, Log, TEXT("✅ Job Complete! Found %d output files. Starting download..."), FoundOutputsToDownload.Num());
                         this->DownloadAllFinalImages(FoundOutputsToDownload);
                    }
                    else
                    {
                        HandleProjectionError_Internal(TEXT("Job finished, but no expected output images were found in the history."));
                    }
                    
                    // 5. Clear the prompt ID for the next run.
                    this->CurrentPromptId.Empty();
                }
                // If the prompt ID key doesn't exist yet, the job is still running. The ticker will try again.
            }
        });

    HttpRequest->ProcessRequest();
    
    // Return true to keep the ticker running until we manually stop it.
    return true; 
}



void FTextureDiffusion3D::BeginPollingForComfyOutputs(
    const TMap<FString, EProjectionVariant>& OutputRequests,
    FOnComfyAllImagesReady OnAllReady)
{

	
    const double TimeoutSec = 600.0;
    const FDateTime TaskStart = this->CurrentComfyTaskStartTime;

    // Track per-prefix completion, then fold into variants at the end
    TMap<FString, UTexture2D*> FoundByPrefix;
    FoundByPrefix.Reserve(OutputRequests.Num());

    bool bAbortEarly = false;

    FScopedSlowTask SlowTask(
        FMath::Max(1.0, TimeoutSec),
        FText::FromString(CurrentBatchLabel + TEXT(" — Waiting for ComfyUI…")));
    SlowTask.MakeDialog(true);

    const double Start = FPlatformTime::Seconds();
    while ((FPlatformTime::Seconds() - Start) < TimeoutSec)
    {
        if (SlowTask.ShouldCancel())
        {
            HandleProjectionError_Internal(TEXT("Polling cancelled by user."));
            bAbortEarly = true;
            break;
        }

        FPlatformProcess::Sleep(1.0);
        SlowTask.EnterProgressFrame(1.0f);

        for (const auto& Pair : OutputRequests)
        {
            const FString& Prefix = Pair.Key;
            if (FoundByPrefix.Contains(Prefix))
            {
                continue;
            }

            if (UTexture2D* Tex = WaitForAndLoadComfyUIOutput(Prefix, 1.0f, TaskStart))
            {
				Tex->SetFlags(RF_Transient);
                FoundByPrefix.Add(Prefix, Tex);
                UE_LOG(LogTemp, Log, TEXT("Comfy output ready: prefix='%s' (variant=%d)"),
                    *Prefix, (int32)Pair.Value);
            }
        }

        if (FoundByPrefix.Num() == OutputRequests.Num())
        {
            break;
        }
    }

    if (bAbortEarly)
    {
        OnSingleProjectionFinished.ExecuteIfBound();
        return;
    }

    if (FoundByPrefix.Num() == OutputRequests.Num())
    {
        // Fold per-prefix into per-variant (first wins)
        TMap<EProjectionVariant, UTexture2D*> ResultsByVariant;
        for (const auto& Req : OutputRequests)
        {
            const FString& Prefix = Req.Key;
            const EProjectionVariant Variant = Req.Value;

            if (!ResultsByVariant.Contains(Variant))
            {
                if (UTexture2D* const* Found = FoundByPrefix.Find(Prefix))
                {
                    ResultsByVariant.Add(Variant, *Found);
                }
            }
        }

        if (OnAllReady.IsBound())
        {
            OnAllReady.Execute(ResultsByVariant);
        }
    }
    else
    {
        HandleProjectionError_Internal(TEXT("Polling timed out while waiting for all requested outputs."));
    }

// 	FTSTicker::GetCoreTicker().AddTicker(
// 	FTickerDelegate::CreateLambda([this](float)
// 	{
// 		this->OnSingleProjectionFinished.ExecuteIfBound();
// 		return false;
// 	}),
// 	0.05f
// );


}




/**
 * @brief Calculates 2D barycentric coordinates.
 * @param P The texel's UV coordinate.
 * @param A UV coordinate of the triangle's first vertex.
 * @param B UV coordinate of the triangle's second vertex.
 * @param C UV coordinate of the triangle's third vertex.
 * @param OutU Stores the weight for vertex A.
 * @param OutV Stores the weight for vertex B.
 * @param OutW Stores the weight for vertex C.
 * @return true if the point is inside the triangle (or on edge), false otherwise.
 */
static bool GetBarycentricCoords(const FVector2D& P, const FVector2D& A, const FVector2D& B, const FVector2D& C, float& OutU, float& OutV, float& OutW)
{
    const FVector2D v0 = B - A;
    const FVector2D v1 = C - A;
    const FVector2D v2 = P - A;

    const double d00 = FVector2D::DotProduct(v0, v0);
    const double d01 = FVector2D::DotProduct(v0, v1);
    const double d11 = FVector2D::DotProduct(v1, v1);
    const double d20 = FVector2D::DotProduct(v2, v0);
    const double d21 = FVector2D::DotProduct(v2, v1);

    const double denom = d00 * d11 - d01 * d01;
    if (FMath::Abs(denom) < 1e-12) // Use a small epsilon for degenerate triangles
    {
        return false;
    }

    const double invDenom = 1.0 / denom;
    OutV = (float)((d11 * d20 - d01 * d21) * invDenom);
    OutW = (float)((d00 * d21 - d01 * d20) * invDenom);
    OutU = 1.0f - OutV - OutW;

    // Check if point is inside the triangle (with a small tolerance for edges)
    const float kEps = -1e-5f;
    return (OutU >= kEps) && (OutV >= kEps) && (OutW >= kEps);
}


bool FTextureDiffusion3D::ReadLinearPixelsFromRT(UTextureRenderTarget2D* RenderTarget, TArray<FLinearColor>& OutLinearPixelData)
{
    if (!IsValid(RenderTarget))
    {
        UE_LOG(LogTemp, Error, TEXT("ReadLinearPixelsFromRT: RenderTarget is null."));
        return false;
    }
    
    FTextureRenderTargetResource* RTResource = RenderTarget->GameThread_GetRenderTargetResource();
    if (!RTResource)
    {
        UE_LOG(LogTemp, Error, TEXT("ReadLinearPixelsFromRT: RenderTargetResource is null."));
        return false;
    }

    if (RTResource->ReadLinearColorPixels(OutLinearPixelData))
    {
        UE_LOG(LogTemp, Log, TEXT("ReadLinearPixelsFromRT: Successfully read %d linear pixels."), OutLinearPixelData.Num());
        return true;
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("ReadLinearPixelsFromRT: ReadLinearColorPixels failed."));
        return false;
    }
}




/**
 * @brief Saves a UTexture2D as a new .uasset in /Game/TD3D_Debug/ for inspection.
 * THIS IS THE CORRECTED, FORMAT-AWARE VERSION.
 * It detects if the input is 8-bit sRGB or 16-bit HDR and saves it correctly.
 */
void FTextureDiffusion3D::Debug_SaveTextureAsAsset(UTexture2D* TextureToSave, const FString& BaseAssetName)
{
	if (!TextureToSave || !TextureToSave->GetPlatformData())
	{
		UE_LOG(LogTemp, Error, TEXT("Debug_SaveTextureAsAsset: TextureToSave is null or has no PlatformData for %s."), *BaseAssetName);
		return;
	}

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		UE_LOG(LogTemp, Error, TEXT("Debug_SaveTextureAsAsset: Cannot get World."));
		return;
	}

	const int32 W = TextureToSave->GetSizeX();
	const int32 H = TextureToSave->GetSizeY();
	if (W <= 0 || H <= 0)
	{
		UE_LOG(LogTemp, Error, TEXT("Debug_SaveTextureAsAsset: Texture %s has invalid dimensions (%dx%d)."), *TextureToSave->GetName(), W, H);
		return;
	}

    // --- Asset Naming (Unchanged) ---
	const FString BasePath = TEXT("/Game/TD3D_Debug/");
	// Add current context to name to make it unique
	const FString AssetName = FString::Printf(TEXT("%s_Slot%d_Cam%d_%s"),
		*BaseAssetName,
		CurrentProjection_Settings.TargetMaterialSlotIndex,
		CurrentProjection_CameraIndex,
		*FDateTime::Now().ToString(TEXT("HHMMSS"))
	);
	const FString PackagePath = BasePath + AssetName;

	UPackage* Package = CreatePackage(*PackagePath);
	if (!Package) { UE_LOG(LogTemp, Error, TEXT("Debug_SaveTextureAsAsset: Failed to create package %s"), *PackagePath); return; }
	Package->FullyLoad();

	UTexture2D* NewStaticTexture = NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone);
	if (!NewStaticTexture) { UE_LOG(LogTemp, Error, TEXT("Debug_SaveTextureAsAsset: Failed to create NewObject UTexture2D.")); return; }

	NewStaticTexture->AddToRoot(); // Protect during setup
    
    // --- NEW FORMAT-AWARE LOGIC ---

    const EPixelFormat SourceFormat = TextureToSave->GetPlatformData()->PixelFormat;

    if (SourceFormat == PF_FloatRGBA || SourceFormat == PF_A32B32G32R32F)
    {
        // --- PATH A: 16-bit or 32-bit HDR (PF_FloatRGBA / PF_A32B32G32R32F) ---
        UE_LOG(LogTemp, Log, TEXT("Debug_SaveTextureAsAsset: Detected 16/32-bit HDR (%s). Saving as HDR."), GPixelFormats[SourceFormat].Name);

		// We will save as 16-bit, which is precise enough for debugging.
        NewStaticTexture->Source.Init(W, H, 1, 1, TSF_RGBA16F); // 16-bit source format
        NewStaticTexture->CompressionSettings = TC_HDR;
        NewStaticTexture->SRGB = false; // This is Linear data

        // Lock source mip (16-bit or 32-bit)
        FTexture2DMipMap& SourceMip = TextureToSave->GetPlatformData()->Mips[0];
        const void* SourcePixels = SourceMip.BulkData.LockReadOnly();
		
        // Lock destination mip (16-bit)
        void* DestPixels = NewStaticTexture->Source.LockMip(0);

		if (SourceFormat == PF_FloatRGBA) // Source is 16-bit
		{
        	FMemory::Memcpy(DestPixels, SourcePixels, (SIZE_T)W * H * sizeof(FFloat16Color));
		}
		else // Source is 32-bit, needs conversion
		{
			const FLinearColor* Source32bit = static_cast<const FLinearColor*>(SourcePixels);
			FFloat16Color* Dest16bit = static_cast<FFloat16Color*>(DestPixels);
			for(int32 i=0; i < W*H; ++i)
			{
				Dest16bit[i] = FFloat16Color(Source32bit[i]);
			}
		}

        NewStaticTexture->Source.UnlockMip(0);
        SourceMip.BulkData.Unlock();
    }
    else
    {
        // --- PATH B: 8-bit sRGB (PF_B8G8R8A8 or other) ---
        // This is the sRGB-correct decompression logic.
        UE_LOG(LogTemp, Log, TEXT("Debug_SaveTextureAsAsset: Detected 8-bit (or unknown) format (%s). Saving as sRGB."), GPixelFormats[SourceFormat].Name);

        // 1. Create a sRGB Render Target
	    UTextureRenderTarget2D* DecompressedRT = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
	    DecompressedRT->InitCustomFormat(W, H, PF_B8G8R8A8, false); // false = sRGB
	    DecompressedRT->UpdateResourceImmediate(true);

        // 2. Draw with AlphaComposite to preserve alpha
	    UCanvas* Canvas;
	    FVector2D CanvasSize;
	    FDrawToRenderTargetContext Context;
	    UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(World, DecompressedRT, Canvas, CanvasSize, Context);
	    if (Canvas)
	    {
            UKismetRenderingLibrary::ClearRenderTarget2D(World, DecompressedRT, FLinearColor::Transparent);
            FCanvasTileItem TileItem(FVector2D::ZeroVector, TextureToSave->GetResource(), CanvasSize, FLinearColor::White);
            TileItem.BlendMode = SE_BLEND_AlphaComposite; // Preserve alpha
		    Canvas->DrawItem(TileItem);
	    }
	    UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(World, Context);
	    FlushRenderingCommands();

        // 3. Read back the 8-bit sRGB pixels
	    FTextureRenderTargetResource* RTResource = DecompressedRT->GameThread_GetRenderTargetResource();
	    TArray<FColor> PixelData;
	    RTResource->ReadPixels(PixelData);
	    DecompressedRT->MarkAsGarbage();

        // 4. Configure new asset for 8-bit sRGB
        NewStaticTexture->Source.Init(W, H, 1, 1, ETextureSourceFormat::TSF_BGRA8);
        NewStaticTexture->CompressionSettings = TC_Default;
        NewStaticTexture->SRGB = true; // This is sRGB color data

        // 5. Copy 8-bit pixel data
        void* DestPixels = NewStaticTexture->Source.LockMip(0);
        FMemory::Memcpy(DestPixels, PixelData.GetData(), (SIZE_T)W * H * sizeof(FColor));
        NewStaticTexture->Source.UnlockMip(0);
    }

    // --- COMMON SAVE LOGIC (Unchanged) ---
	NewStaticTexture->UpdateResource();
	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(NewStaticTexture);

	const FString PackageFileName = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;

	if (UPackage::SavePackage(Package, NewStaticTexture, *PackageFileName, SaveArgs))
	{
		UE_LOG(LogTemp, Warning, TEXT("--- DEBUG: Saved Texture ASSET to: %s ---"), *PackagePath);
		if (GEditor) { TArray<UObject*> AssetsToSync = { NewStaticTexture }; GEditor->SyncBrowserToObjects(AssetsToSync); }
	}
	else 
	{ 
		UE_LOG(LogTemp, Error, TEXT("Debug_SaveTextureAsAsset: Failed to save package: %s"), *PackagePath); 
	}
	
	NewStaticTexture->RemoveFromRoot(); // Unprotect
}

bool FTextureDiffusion3D::ProjectColor(
    UTexture2D* InSourceTexture,
    const TArray<bool>& VisibilityBuffer,
    const TArray<FVector2D>& ScreenPositionBuffer,
    const TArray<float>& NormalWeightBuffer,
    int32 TextureWidth,
    int32 TextureHeight,
    TArray<FLinearColor>& OutTextureBuffer)
{
    if (!InSourceTexture)
    {
        UE_LOG(LogTemp, Error, TEXT("CreateWeightedProjectionTexture: Provided source texture is null."));
        return false;
    }

    // --- DECOMPRESSION STEP (No changes here) ---
	UTextureRenderTarget2D* DecompressedRT = NewObject<UTextureRenderTarget2D>();
    // 1. Use PF_FloatRGBA for high precision
    // 2. Set bForceLinearGamma = true so the GPU gives us mathematically correct linear values
    DecompressedRT->InitCustomFormat(InSourceTexture->GetSizeX(), InSourceTexture->GetSizeY(), PF_FloatRGBA, true);
    DecompressedRT->UpdateResourceImmediate(true);

    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World)
    {
        UE_LOG(LogTemp, Error, TEXT("CreateWeightedProjectionTexture: Cannot get World to decompress texture."));
        return false;
    }

    UCanvas* Canvas;
    FVector2D CanvasSize;
    FDrawToRenderTargetContext Context;
    UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(World, DecompressedRT, Canvas, CanvasSize, Context);

    if (Canvas)
    {
        Canvas->K2_DrawTexture(
            InSourceTexture, FVector2D::ZeroVector, CanvasSize, FVector2D::ZeroVector,
            FVector2D(1.0f, 1.0f), FLinearColor::White, EBlendMode::BLEND_Opaque
        );
    }
    UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(World, Context);

    TArray<FColor> SourcePixels;
    FTextureRenderTargetResource* RTResource = DecompressedRT->GameThread_GetRenderTargetResource();
    if (!RTResource || !RTResource->ReadPixels(SourcePixels))
    {
        UE_LOG(LogTemp, Error, TEXT("CreateWeightedProjectionTexture: Failed to read pixels from decompressed Render Target."));
        DecompressedRT->MarkAsGarbage();
        return false;
    }

    const int32 SourceMipWidth = DecompressedRT->SizeX;
    const int32 SourceMipHeight = DecompressedRT->SizeY;

    const int32 NumOutTexels = TextureWidth * TextureHeight;
    OutTextureBuffer.Init(FColor::Transparent, NumOutTexels);

    int32 VisibleTexelsProcessedCount = 0;
    int32 SeamRejectedCount = 0;
    
    for (int32 TexelIndex = 0; TexelIndex < NumOutTexels; TexelIndex++)
    {
        // Skip non-visible texels
        if (!VisibilityBuffer[TexelIndex])
        {
            continue;
        }
        
        // Skip texels with no weight
        if (NormalWeightBuffer[TexelIndex] <= KINDA_SMALL_NUMBER)
        {
            continue;
        }

        // // === UV SEAM BOUNDARY REJECTION (EXPANDED RADIUS) ===
        // if (PrecomputedUVIslandMap.Num() == NumOutTexels) // Only run if island map is valid
        // {
        //     const int32 MyIslandID = PrecomputedUVIslandMap[TexelIndex];
            
        //     if (MyIslandID >= 0)
        //     {
        //         const int32 X = TexelIndex % TextureWidth;
        //         const int32 Y = TexelIndex / TextureWidth;
                
        //         // How many pixels away from a seam should we reject?
        //         // 1 = original behavior (immediate neighbors only)
        //         // 2-3 = accounts for ComfyUI output drift
        //         const int32 SeamRejectionRadius = 2;
                
        //         auto GetIslandID = [&](int32 NX, int32 NY) -> int32 {
        //             if (NX >= 0 && NX < TextureWidth && NY >= 0 && NY < TextureHeight) {
        //                 int32 NIdx = NY * TextureWidth + NX;
        //                 return PrecomputedUVIslandMap[NIdx];
        //             }
        //             return -1;
        //         };
                
        //         bool bIsNearUVBoundary = false;
                
        //         // Check all texels within the rejection radius
        //         for (int32 DY = -SeamRejectionRadius; DY <= SeamRejectionRadius && !bIsNearUVBoundary; ++DY)
        //         {
        //             for (int32 DX = -SeamRejectionRadius; DX <= SeamRejectionRadius && !bIsNearUVBoundary; ++DX)
        //             {
        //                 if (DX == 0 && DY == 0) continue; // Skip self
                        
        //                 const int32 NeighborIslandID = GetIslandID(X + DX, Y + DY);
                        
        //                 // If neighbor is valid AND belongs to a different island, we're near a seam
        //                 if (NeighborIslandID >= 0 && NeighborIslandID != MyIslandID)
        //                 {
        //                     bIsNearUVBoundary = true;
        //                 }
        //             }
        //         }
                
        //         if (bIsNearUVBoundary)
        //         {
        //             SeamRejectedCount++;
        //             continue;
        //         }
        //     }
        // }
        // // === END UV SEAM BOUNDARY REJECTION ===

        const FVector2D& ScreenPos = ScreenPositionBuffer[TexelIndex];
        if (FMath::IsNaN(ScreenPos.X) || FMath::IsNaN(ScreenPos.Y) || !FMath::IsFinite(ScreenPos.X) || !FMath::IsFinite(ScreenPos.Y))
        {
            continue;
        }

        const FVector2D SampleUV(ScreenPositionBuffer[TexelIndex].X, 1.0f - ScreenPositionBuffer[TexelIndex].Y);

// 1. Use the FLinearColor overload of your sampler (you already defined this in FTextureUtils)
    // This performs pure math interpolation without applying gamma curves.
    const FLinearColor SampledPixel = FTextureUtils::SampleTextureBilinear(SourcePixels, SourceMipWidth, SourceMipHeight, SampleUV);
    
    FLinearColor ProjectedPixel_Linear = SampledPixel;

    // 2. Get weight and mask (Alpha is already 0.0-1.0 float, no need to divide by 255)
    const float Weight = NormalWeightBuffer[TexelIndex];
    const float SourceMask = SampledPixel.A; 

    // 3. Multiply
    ProjectedPixel_Linear.A = Weight * SourceMask;
    
    OutTextureBuffer[TexelIndex] = ProjectedPixel_Linear;
        // 3. Multiply them. If SourceMask is 0, the result is 0. If it's 1, the result is the Weight.
        ProjectedPixel_Linear.A = Weight * SourceMask;
        
        // Store the final linear color with the correctly combined alpha
        OutTextureBuffer[TexelIndex] = ProjectedPixel_Linear;

        VisibleTexelsProcessedCount++;
    }

    // Clean up the temporary render target
    DecompressedRT->MarkAsGarbage();

    UE_LOG(LogTemp, Log, TEXT("CreateWeightedProjectionTexture: Processed %d visible texels, rejected %d near UV seams."), 
        VisibleTexelsProcessedCount, SeamRejectedCount);
    return (VisibleTexelsProcessedCount > 0);
}




#include "Materials/MaterialRenderProxy.h"
#include "ShaderCompiler.h"
// --- BakeWeightedLayerGPU (Synchronous with Explicit Resource Readiness Checks) ---
bool FTextureDiffusion3D::BakeWeightedLayerGPU(
    UTexture2D* SourceTexture,
    UTexture2D* PositionMap,
    const TArray<float>& CpuWeightBuffer,
    const TArray<bool>& CpuVisibilityBuffer,
    const FProjectionSettings& InCameraSettings,
    int32 OutWidth,
    int32 OutHeight,
    EProjectionVariant CurrentVariant,
    TArray<FLinearColor>& OutWeightedPixels)
{
    UE_LOG(LogTemp, Warning, TEXT("--- BakeWeightedLayerGPU START (Sync + Resource Ready Checks) --- Variant: %s"), *UEnum::GetValueAsString(CurrentVariant));
    OutWeightedPixels.Empty();



    // --- 0. Validation ---
    if (!IsValid(SourceTexture) || !IsValid(PositionMap)) 
    {
        UE_LOG(LogTemp, Error, TEXT("BakeWeightedLayerGPU: Invalid input texture(s). Source: %s, PosMap: %s"), *GetNameSafe(SourceTexture), *GetNameSafe(PositionMap));
        return false; 
    }
    const int32 NumTexels = OutWidth * OutHeight;
    if (CpuWeightBuffer.Num() != NumTexels || CpuVisibilityBuffer.Num() != NumTexels) 
    {
        UE_LOG(LogTemp, Error, TEXT("BakeWeightedLayerGPU: CPU Weight/Visibility buffer size mismatch."));
        return false; 
    }
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World) { UE_LOG(LogTemp, Error, TEXT("BakeWeightedLayerGPU (Resource Ready): Invalid World pointer.")); return false; }

    // --- <<< MODIFIED DEBUG STEP >>> ---
    UE_LOG(LogTemp, Warning, TEXT("BakeWeightedLayerGPU: Saving debug asset for SourceTexture..."));
    // Save the 8-bit color source texture
    Debug_SaveTextureAsAsset(SourceTexture, TEXT("Debug_SourceTexture")); 
    
    // Log the path to the already-existing PositionMap asset
    UE_LOG(LogTemp, Warning, TEXT("BakeWeightedLayerGPU: SourceTexture saved to /Game/TD3D_Debug/."));
    UE_LOG(LogTemp, Warning, TEXT("BakeWeightedLayerGPU: Find the PositionMap to use for debugging here: %s"), *PositionMap->GetPathName());
    // --- <<< END MODIFIED STEP >>> ---


	// --- <<< ADDED: DEBUG SAVE CPU WEIGHT MAP ASSET >>> ---
    {
        UE_LOG(LogTemp, Warning, TEXT("BakeWeightedLayerGPU (Debug): Saving CpuWeightBuffer as HDR asset..."));

        const FString BasePath = TEXT("/Game/TD3D_Debug/");

        // --- Sanitize the enum name for use in a file path ---
        FString VariantString = StaticEnum<EProjectionVariant>()->GetNameStringByValue((int64)CurrentVariant);

        // Use member variables for context
        const FString AssetName = FString::Printf(TEXT("Debug_CpuWeightMap_%s_Slot%d_Cam%d_%s"),
            *VariantString,
            CurrentProjection_Settings.TargetMaterialSlotIndex,
            CurrentProjection_CameraIndex,
            *FDateTime::Now().ToString(TEXT("HHMMSS"))
        );
        const FString PackagePath = BasePath + AssetName;

        UPackage* Package = CreatePackage(*PackagePath);
        if (Package)
        {
            Package->FullyLoad();
            UTexture2D* NewStaticTexture = NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone);
            if (NewStaticTexture)
            {
                NewStaticTexture->AddToRoot();
                NewStaticTexture->CompressionSettings = TC_HDR_F32; // HDR format
                NewStaticTexture->SRGB = false; // It's linear data
                NewStaticTexture->Source.Init(OutWidth, OutHeight, 1, 1, TSF_RGBA16F); // 16-bit float format
                
                // Convert TArray<float> to TArray<FFloat16Color> (Grayscale)
                TArray<FFloat16Color> HdrPixels;
                HdrPixels.SetNumUninitialized(NumTexels);
                for (int32 i = 0; i < NumTexels; ++i)
                {
                    // If not visible, weight is 0. Otherwise, use the CPU weight.
                    const float Weight = CpuVisibilityBuffer[i] ? CpuWeightBuffer[i] : 0.0f;
                    // Create a grayscale linear color (R=G=B) with full alpha
                    HdrPixels[i] = FFloat16Color(FLinearColor(Weight, Weight, Weight, 1.0f));
                }

                void* DestPixels = NewStaticTexture->Source.LockMip(0);
                if (DestPixels)
                {
                    FMemory::Memcpy(DestPixels, HdrPixels.GetData(), HdrPixels.Num() * sizeof(FFloat16Color));
                    NewStaticTexture->Source.UnlockMip(0);
                    
                    NewStaticTexture->UpdateResource();
                    Package->MarkPackageDirty();
                    FAssetRegistryModule::AssetCreated(NewStaticTexture);

                    const FString PackageFileName = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
                    FSavePackageArgs SaveArgs;
                    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
                    
                    if (UPackage::SavePackage(Package, NewStaticTexture, *PackageFileName, SaveArgs))
                    {
                        UE_LOG(LogTemp, Warning, TEXT("--- DEBUG: Saved CPU WEIGHT MAP Texture ASSET to: %s ---"), *PackagePath);
                        if (GEditor) { TArray<UObject*> AssetsToSync = { NewStaticTexture }; GEditor->SyncBrowserToObjects(AssetsToSync); }
                    }
                    else { UE_LOG(LogTemp, Error, TEXT("Debug_SaveCpuWeightMap: Failed to save package: %s"), *PackagePath); }
                }
                else { UE_LOG(LogTemp, Error, TEXT("Debug_SaveCpuWeightMap: Failed to lock mip.")); }
                NewStaticTexture->RemoveFromRoot();
            }
        }
    }
    // --- <<< END DEBUG SAVE CPU WEIGHT MAP >>> ---
	
    // --- 1. Load Material ---
	// At the top of BakeWeightedLayerGPU, change the material loading:
	FString MaterialPath;
	if (CurrentVariant == EProjectionVariant::Normal)
	{
		MaterialPath = TEXT("/TextureDiffusion3D/Materials/M_GpuProjectNormal.M_GpuProjectNormal");
	}
	else
	{
		MaterialPath = TEXT("/TextureDiffusion3D/Materials/m_GpuApplyWeight.m_GpuApplyWeight");
	}
	UMaterialInterface* ProjectionMaterialBase = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);

    // --- 2. Create MID ---
    UMaterialInstanceDynamic* ProjectionMID = UMaterialInstanceDynamic::Create(ProjectionMaterialBase, GetTransientPackage());
    if (!ProjectionMID) { UE_LOG(LogTemp, Error, TEXT("BakeWeightedLayerGPU (Resource Ready): Failed to create MID.")); return false; }

    // --- 3. Create Temporary FLOAT Render Target ---
    UTextureRenderTarget2D* TempRT = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
    if (!TempRT) { UE_LOG(LogTemp, Error, TEXT("BakeWeightedLayerGPU (Resource Ready): Failed to create RT.")); return false; }
    TempRT->InitCustomFormat(OutWidth, OutHeight, PF_FloatRGBA, true);
    TempRT->RenderTargetFormat = RTF_RGBA16f;
    TempRT->bForceLinearGamma = true;
    UE_LOG(LogTemp, Log, TEXT("BakeWeightedLayerGPU (Resource Ready): Updating RT Resource..."));
    TempRT->UpdateResourceImmediate(true); 
    FlushRenderingCommands(); 
    UE_LOG(LogTemp, Log, TEXT("BakeWeightedLayerGPU (Resource Ready): Created Float Render Target."));

    // --- 4. Set Parameters on MID ---
	// --- ADD THIS SECTION ---
    // Get the object's current transform matrix
    const FTransform& ActorTransform = CurrentProjection_MeshComponent->GetComponentTransform();
    const FMatrix LocalToWorldMatrix = ActorTransform.ToMatrixWithScale();

	// --- Create and pass UV Island Map for seam rejection ---
	UTexture2D* IslandMapTex = CreateUVIslandMapTexture(OutWidth, OutHeight);
	if (IslandMapTex)
	{
    // --- DEBUG: Save UV Island Map as visible texture ---
    {
        UE_LOG(LogTemp, Warning, TEXT("BakeWeightedLayerGPU (Debug): Saving UV Island Map..."));
        
        const FString BasePath = TEXT("/Game/TD3D_Debug/");
        const FString AssetName = FString::Printf(TEXT("Debug_UVIslandMap_Slot%d_Cam%d_%s"),
            CurrentProjection_Settings.TargetMaterialSlotIndex,
            CurrentProjection_CameraIndex,
            *FDateTime::Now().ToString(TEXT("HHMMSS"))
        );
        const FString PackagePath = BasePath + AssetName;
        
        UPackage* Package = CreatePackage(*PackagePath);
        if (Package)
        {
            Package->FullyLoad();
            UTexture2D* DebugTexture = NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone);
            if (DebugTexture)
            {
                DebugTexture->AddToRoot();
                DebugTexture->CompressionSettings = TC_VectorDisplacementmap;
                DebugTexture->SRGB = false;
                DebugTexture->Source.Init(OutWidth, OutHeight, 1, 1, TSF_BGRA8);
                
                // Find max island ID for normalization
                int32 MaxIslandID = 0;
                for (int32 ID : PrecomputedUVIslandMap)
                {
                    if (ID > MaxIslandID) MaxIslandID = ID;
                }
                
                // Create colorful visualization - each island gets a unique color
                TArray<FColor> DebugPixels;
                DebugPixels.SetNumUninitialized(OutWidth * OutHeight);
                
                for (int32 i = 0; i < PrecomputedUVIslandMap.Num(); ++i)
                {
                    const int32 IslandID = PrecomputedUVIslandMap[i];
                    
                    if (IslandID < 0)
                    {
                        // Invalid/empty texel - black
                        DebugPixels[i] = FColor::Black;
                    }
                    else
                    {
                        // Generate a unique color per island using golden ratio hue distribution
                        const float Hue = FMath::Frac(IslandID * 0.618033988749895f); // Golden ratio
                        const float Saturation = 0.7f + 0.3f * FMath::Frac(IslandID * 0.381966f);
                        const float Value = 0.8f + 0.2f * FMath::Frac(IslandID * 0.5f);
                        
                        // HSV to RGB conversion
                        const float H = Hue * 6.0f;
                        const int32 Hi = FMath::FloorToInt(H) % 6;
                        const float F = H - FMath::FloorToInt(H);
                        const float P = Value * (1.0f - Saturation);
                        const float Q = Value * (1.0f - F * Saturation);
                        const float T = Value * (1.0f - (1.0f - F) * Saturation);
                        
                        float R, G, B;
                        switch (Hi)
                        {
                            case 0: R = Value; G = T; B = P; break;
                            case 1: R = Q; G = Value; B = P; break;
                            case 2: R = P; G = Value; B = T; break;
                            case 3: R = P; G = Q; B = Value; break;
                            case 4: R = T; G = P; B = Value; break;
                            default: R = Value; G = P; B = Q; break;
                        }
                        
                        DebugPixels[i] = FColor(
                            (uint8)FMath::Clamp(FMath::RoundToInt(R * 255.0f), 0, 255),
                            (uint8)FMath::Clamp(FMath::RoundToInt(G * 255.0f), 0, 255),
                            (uint8)FMath::Clamp(FMath::RoundToInt(B * 255.0f), 0, 255),
                            255
                        );
                    }
                }
                
                void* DestPixels = DebugTexture->Source.LockMip(0);
                if (DestPixels)
                {
                    FMemory::Memcpy(DestPixels, DebugPixels.GetData(), DebugPixels.Num() * sizeof(FColor));
                    DebugTexture->Source.UnlockMip(0);
                    
                    DebugTexture->UpdateResource();
                    Package->MarkPackageDirty();
                    FAssetRegistryModule::AssetCreated(DebugTexture);
                    
                    const FString PackageFileName = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
                    FSavePackageArgs SaveArgs;
                    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
                    
                    if (UPackage::SavePackage(Package, DebugTexture, *PackageFileName, SaveArgs))
                    {
                        UE_LOG(LogTemp, Warning, TEXT("--- DEBUG: Saved UV ISLAND MAP to: %s (Max Island ID: %d) ---"), *PackagePath, MaxIslandID);
                    }
                }
                DebugTexture->RemoveFromRoot();
            }
        }
    }
    // --- END DEBUG ---

		ProjectionMID->SetTextureParameterValue(FName("UVIslandMapParam"), IslandMapTex);
		ProjectionMID->SetScalarParameterValue(FName("SeamRejectionRadius"), 1.0f);
		ProjectionMID->SetScalarParameterValue(FName("TexelSizeX"), 1.0f / static_cast<float>(OutWidth));
		ProjectionMID->SetScalarParameterValue(FName("TexelSizeY"), 1.0f / static_cast<float>(OutHeight));
		UE_LOG(LogTemp, Log, TEXT("BakeWeightedLayerGPU: UV Island Map passed to shader for seam rejection."));
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("BakeWeightedLayerGPU: Could not create UV Island Map. Seam rejection disabled."));
	}

// Pass the 4 rows of the matrix to the material.
    // We access elements with .M[row][col] and cast the doubles to floats
    // by passing them to the FLinearColor constructor.
    ProjectionMID->SetVectorParameterValue(FName("ObjectLocalToWorld_Row0"), FLinearColor(
        LocalToWorldMatrix.M[0][0],
        LocalToWorldMatrix.M[0][1],
        LocalToWorldMatrix.M[0][2],
        LocalToWorldMatrix.M[0][3]
    ));
    ProjectionMID->SetVectorParameterValue(FName("ObjectLocalToWorld_Row1"), FLinearColor(
        LocalToWorldMatrix.M[1][0],
        LocalToWorldMatrix.M[1][1],
        LocalToWorldMatrix.M[1][2],
        LocalToWorldMatrix.M[1][3]
    ));
    ProjectionMID->SetVectorParameterValue(FName("ObjectLocalToWorld_Row2"), FLinearColor(
        LocalToWorldMatrix.M[2][0],
        LocalToWorldMatrix.M[2][1],
        LocalToWorldMatrix.M[2][2],
        LocalToWorldMatrix.M[2][3]
    ));
    ProjectionMID->SetVectorParameterValue(FName("ObjectLocalToWorld_Row3"), FLinearColor(
        LocalToWorldMatrix.M[3][0],
        LocalToWorldMatrix.M[3][1],
        LocalToWorldMatrix.M[3][2],
        LocalToWorldMatrix.M[3][3]
    ));
    // --- END FIX ---
	
    const FRotator& CameraRotation = InCameraSettings.CameraRotation;
    const FMatrix CameraMatrix = FRotationMatrix(CameraRotation);
    const FVector CamForward = CameraMatrix.GetScaledAxis(EAxis::X);
    const FVector CamRight = CameraMatrix.GetScaledAxis(EAxis::Y);
    const FVector CamUp = CameraMatrix.GetScaledAxis(EAxis::Z);
    const float AspectRatio = (OutHeight > 0) ? (static_cast<float>(OutWidth) / static_cast<float>(OutHeight)) : 1.0f;

    ProjectionMID->SetTextureParameterValue(FName("SourceTextureParam"), SourceTexture);
    ProjectionMID->SetTextureParameterValue(FName("PositionMapParam"), PositionMap);
    ProjectionMID->SetVectorParameterValue(FName("VirtualCamera_Position"), FLinearColor(InCameraSettings.CameraPosition));
    ProjectionMID->SetVectorParameterValue(FName("VirtualCamera_Right"), FLinearColor(CamRight));
    ProjectionMID->SetVectorParameterValue(FName("VirtualCamera_Up"), FLinearColor(CamUp));
    ProjectionMID->SetVectorParameterValue(FName("VirtualCamera_Forward"), FLinearColor(CamForward));
    ProjectionMID->SetScalarParameterValue(FName("VirtualCamera_FOV"), InCameraSettings.FOVAngle);
    ProjectionMID->SetScalarParameterValue(FName("VirtualCamera_AspectRatio"), AspectRatio);
    UE_LOG(LogTemp, Log, TEXT("BakeWeightedLayerGPU (Resource Ready): Set parameters on MID."));

	if (CurrentVariant == EProjectionVariant::Normal)
	{
		ProjectionMID->SetScalarParameterValue(FName("DetailGain"), 1.0f);
		ProjectionMID->SetScalarParameterValue(FName("MaxAngleDegrees"), 80.0f);
		ProjectionMID->SetScalarParameterValue(FName("FlipGreenChannel"), 1.0f);
	}


    // --- 5. WARM-UP: Ensure Shaders, Textures, and Parameters are Ready ---
    UE_LOG(LogTemp, Log, TEXT("BakeWeightedLayerGPU (Resource Ready): Starting Warm-up..."));
#if WITH_EDITOR
    UE_LOG(LogTemp, Log, TEXT("  -> Submitting shader jobs..."));
    ProjectionMID->SubmitRemainingJobsForWorld(World, EMaterialShaderPrecompileMode::Default);
    FlushRenderingCommands();
    if (GShaderCompilingManager)
    {
        UE_LOG(LogTemp, Log, TEXT("  -> Waiting for shader compilation..."));
        GShaderCompilingManager->FinishAllCompilation();
        UE_LOG(LogTemp, Log, TEXT("  -> Shader compilation finished."));
    }
    FlushRenderingCommands();
#endif

    UE_LOG(LogTemp, Log, TEXT("  -> Forcing texture residency..."));
    auto ForceResidentAndWait = [](UTexture* Tex, const FName& TexName)
    {
        if (!Tex) { UE_LOG(LogTemp, Warning, TEXT("    -> Texture '%s' is null, skipping residency check."), *TexName.ToString()); return; }
        UE_LOG(LogTemp, Log, TEXT("    -> Setting ForceMipLevelsToBeResident for %s..."), *Tex->GetName());
        Tex->SetForceMipLevelsToBeResident(30.0f);
        UE_LOG(LogTemp, Log, TEXT("    -> Waiting for streaming for %s..."), *Tex->GetName());
        Tex->WaitForStreaming();
        UE_LOG(LogTemp, Log, TEXT("    -> Streaming complete for %s."), *Tex->GetName());
    };
    ForceResidentAndWait(SourceTexture, FName("SourceTexture"));
    ForceResidentAndWait(PositionMap, FName("PositionMap"));

    UE_LOG(LogTemp, Log, TEXT("  -> Blocking until all texture streaming requests finished..."));
    IStreamingManager::Get().GetTextureStreamingManager().BlockTillAllRequestsFinished();
    FlushRenderingCommands();
    UE_LOG(LogTemp, Log, TEXT("  -> Texture residency forced."));

    UE_LOG(LogTemp, Log, TEXT("  -> Updating MID cached uniform expressions..."));
    ProjectionMID->UpdateCachedData();
    FlushRenderingCommands();
    UE_LOG(LogTemp, Log, TEXT("  -> MID parameters flushed. Warm-up complete."));

    // --- 6. Draw Material to Render Target & Flush ---
    bool bSuccess = false;
    TArray<FLinearColor> ProjectedPixels_NoWeight;

    UE_LOG(LogTemp, Log, TEXT("BakeWeightedLayerGPU (Resource Ready): Clearing Render Target..."));
    UKismetRenderingLibrary::ClearRenderTarget2D(World, TempRT, FLinearColor::Transparent);
    FlushRenderingCommands();

    UE_LOG(LogTemp, Log, TEXT("BakeWeightedLayerGPU (Resource Ready): Calling DrawMaterialToRenderTarget..."));
    UKismetRenderingLibrary::DrawMaterialToRenderTarget(World, TempRT, ProjectionMID);

    UE_LOG(LogTemp, Log, TEXT("BakeWeightedLayerGPU (Resource Ready): Flushing Rendering Commands (Post-Draw)..."));
    FlushRenderingCommands();
    UE_LOG(LogTemp, Log, TEXT("BakeWeightedLayerGPU (Resource Ready): Post-Draw Flush complete."));

    // --- 7. Read Back Pixels ---
    UE_LOG(LogTemp, Log, TEXT("BakeWeightedLayerGPU (Resource Ready): Reading back LINEAR pixels..."));
    FlushRenderingCommands(); 
    bool bReadbackSuccess = ReadLinearPixelsFromRT(TempRT, ProjectedPixels_NoWeight);
    UE_LOG(LogTemp, Warning, TEXT("BakeWeightedLayerGPU (Resource Ready): ReadLinearColorPixels returned: %s"), bReadbackSuccess ? TEXT("true") : TEXT("false"));

    if (bReadbackSuccess && ProjectedPixels_NoWeight.Num() == NumTexels)
    {
        // --- Log Center Pixel (Before Weight Apply) ---
        int32 CenterX_Pre = OutWidth / 2; int32 CenterY_Pre = OutHeight / 2;
        int32 CenterIndex_Pre = CenterY_Pre * OutWidth + CenterX_Pre;
        if(ProjectedPixels_NoWeight.IsValidIndex(CenterIndex_Pre)) {
             FLinearColor CenterPixel_Pre = ProjectedPixels_NoWeight[CenterIndex_Pre];
             UE_LOG(LogTemp, Warning, TEXT("BakeWeightedLayerGPU (Resource Ready): Center Pixel (Linear, BEFORE Weight Apply): R=%.4f G=%.4f B=%.4f A=%.4f"), CenterPixel_Pre.R, CenterPixel_Pre.G, CenterPixel_Pre.B, CenterPixel_Pre.A);
        }

		// --- <<< NEWLY ADDED DEBUG SAVE >>> ---
        {
            UE_LOG(LogTemp, Warning, TEXT("BakeWeightedLayerGPU (Debug): Saving PRE-WEIGHT ProjectedPixels_NoWeight as HDR asset..."));

            const FString BasePath = TEXT("/Game/TD3D_Debug/");
    
            // --- FIX: Sanitize the enum name for use in a file path ---
            FString VariantString = StaticEnum<EProjectionVariant>()->GetNameStringByValue((int64)CurrentVariant);
            // --- END FIX ---
    
            const FString AssetName = FString::Printf(TEXT("Debug_PreBake_%s_Slot%d_Cam%d_%s"),
                *VariantString,
                CurrentProjection_Settings.TargetMaterialSlotIndex,
                CurrentProjection_CameraIndex,
                *FDateTime::Now().ToString(TEXT("HHMMSS"))
            );
            const FString PackagePath = BasePath + AssetName;
    
            UPackage* Package = CreatePackage(*PackagePath);
            if (Package)
            {
                Package->FullyLoad();
                UTexture2D* NewStaticTexture = NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone);
                if (NewStaticTexture)
                {
                    NewStaticTexture->AddToRoot();
                    NewStaticTexture->CompressionSettings = TC_HDR_F32;
                    NewStaticTexture->SRGB = false;
                    NewStaticTexture->Source.Init(OutWidth, OutHeight, 1, 1, TSF_RGBA16F);
                    
                    TArray<FFloat16Color> HdrPixels;
                    HdrPixels.SetNumUninitialized(NumTexels);
                    for (int32 i = 0; i < NumTexels; ++i)
                    {
                        HdrPixels[i] = FFloat16Color(ProjectedPixels_NoWeight[i]); // <-- Use ProjectedPixels_NoWeight
                    }
    
                    void* DestPixels = NewStaticTexture->Source.LockMip(0);
                    if (DestPixels)
                    {
                        FMemory::Memcpy(DestPixels, HdrPixels.GetData(), HdrPixels.Num() * sizeof(FFloat16Color));
                        NewStaticTexture->Source.UnlockMip(0);
                        
                        NewStaticTexture->UpdateResource();
                        Package->MarkPackageDirty();
                        FAssetRegistryModule::AssetCreated(NewStaticTexture);
    
                        const FString PackageFileName = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
                        FSavePackageArgs SaveArgs;
                        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
                        
                        if (UPackage::SavePackage(Package, NewStaticTexture, *PackageFileName, SaveArgs))
                        {
                            UE_LOG(LogTemp, Warning, TEXT("--- DEBUG: Saved PRE-BAKE Texture ASSET to: %s ---"), *PackagePath);
                            if (GEditor) { TArray<UObject*> AssetsToSync = { NewStaticTexture }; GEditor->SyncBrowserToObjects(AssetsToSync); }
                        }
                        else { UE_LOG(LogTemp, Error, TEXT("Debug_SavePreBake: Failed to save package: %s"), *PackagePath); }
                    }
                    else { UE_LOG(LogTemp, Error, TEXT("Debug_SavePreBake: Failed to lock mip.")); }
                    NewStaticTexture->RemoveFromRoot();
                }
            }
        }
        // --- <<< END DEBUG SAVE PRE-WEIGHT >>> ---

		// --- 8. Apply CPU Weights to Alpha Channel (with GPU seam rejection detection) ---
		UE_LOG(LogTemp, Log, TEXT("BakeWeightedLayerGPU (Resource Ready): Applying CPU weights to Alpha channel..."));
		OutWeightedPixels = ProjectedPixels_NoWeight; // Copy

		int32 GpuSeamRejectedCount = 0;
		for(int32 i = 0; i < NumTexels; ++i)
		{
			const FLinearColor& SrcPixel = ProjectedPixels_NoWeight[i];
			
			// Detect GPU seam rejection marker (shader outputs alpha < 0 when UV is -1,-1)
			const bool bGpuSeamRejected = (SrcPixel.A < -0.5f);
			
			if (bGpuSeamRejected)
			{
				// GPU shader rejected this pixel due to UV seam proximity
				OutWeightedPixels[i] = FLinearColor::Transparent;
				GpuSeamRejectedCount++;
			}
			else if (!CpuVisibilityBuffer[i])
			{
				// CPU visibility culled this pixel
				OutWeightedPixels[i] = FLinearColor::Transparent;
			}
			else if (SrcPixel.A > KINDA_SMALL_NUMBER)
			{
				// Valid pixel - apply CPU weight to alpha
				OutWeightedPixels[i].A = CpuWeightBuffer[i];
			}
			else
			{
				// No valid data
				OutWeightedPixels[i] = FLinearColor::Transparent;
			}
		}

		if (GpuSeamRejectedCount > 0)
		{
			UE_LOG(LogTemp, Log, TEXT("BakeWeightedLayerGPU: GPU seam rejection set %d pixels to transparent (%.2f%% of texture)"), 
				GpuSeamRejectedCount, 
				(float)GpuSeamRejectedCount * 100.0f / (float)NumTexels);
		}

        bSuccess = true; // Mark overall success

        // --- Log Center Pixel (After Weight Apply) ---
         if(OutWeightedPixels.IsValidIndex(CenterIndex_Pre)) {
             FLinearColor CenterPixel_Post = OutWeightedPixels[CenterIndex_Pre];
             UE_LOG(LogTemp, Warning, TEXT("BakeWeightedLayerGPU (Resource Ready): Center Pixel (Linear, Weight Applied): R=%.4f G=%.4f B=%.4f A=%.4f"), CenterPixel_Post.R, CenterPixel_Post.G, CenterPixel_Post.B, CenterPixel_Post.A);
        }
        	UE_LOG(LogTemp, Log, TEXT("BakeWeightedLayerGPU (Resource Ready): Successfully applied weights to %d pixels."), OutWeightedPixels.Num());
		} else {
			UE_LOG(LogTemp, Error, TEXT("BakeWeightedLayerGPU (Resource Ready): Failed readback or size mismatch after draw (%d pixels vs expected %d)."), ProjectedPixels_NoWeight.Num(), NumTexels);
		}


	// --- <<< ADDED: DEBUG SAVE FINAL OUTPUT ASSET >>> ---
        if (bSuccess) // Only save if the bake and weight apply succeeded
        {
            UE_LOG(LogTemp, Warning, TEXT("BakeWeightedLayerGPU (Debug): Saving final OutWeightedPixels as HDR asset..."));

            const FString BasePath = TEXT("/Game/TD3D_Debug/");

			// --- FIX: Sanitize the enum name for use in a file path ---
			FString VariantString = StaticEnum<EProjectionVariant>()->GetNameStringByValue((int64)CurrentVariant);
			// This gives you "Metallic", "BaseColor", etc., which are file-safe.
			// --- END FIX ---

            // Use member variables for context as other debug save functions do
            const FString AssetName = FString::Printf(TEXT("Debug_FinalBake_%s_Slot%d_Cam%d_%s"),
                *VariantString, // Use the sanitized string
                CurrentProjection_Settings.TargetMaterialSlotIndex,
                CurrentProjection_CameraIndex,
                *FDateTime::Now().ToString(TEXT("HHMMSS"))
            );
            const FString PackagePath = BasePath + AssetName;

            UPackage* Package = CreatePackage(*PackagePath);
            if (Package)
            {
                Package->FullyLoad();
                UTexture2D* NewStaticTexture = NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone);
                if (NewStaticTexture)
                {
                    NewStaticTexture->AddToRoot();
                    NewStaticTexture->CompressionSettings = TC_HDR_F32; // HDR format
                    NewStaticTexture->SRGB = false; // It's linear data!
                    NewStaticTexture->Source.Init(OutWidth, OutHeight, 1, 1, TSF_RGBA16F); // 16-bit float format
                    
        	        // Convert TArray<FLinearColor> (32-bit float) to TArray<FFloat16Color> (16-bit float) for TSF_RGBA16F
                    TArray<FFloat16Color> HdrPixels;
                    HdrPixels.SetNumUninitialized(NumTexels);
                    for (int32 i = 0; i < NumTexels; ++i)
                    {
                        HdrPixels[i] = FFloat16Color(OutWeightedPixels[i]); // Convert
                    }

                    void* DestPixels = NewStaticTexture->Source.LockMip(0);
                    if (DestPixels)
                    {
                        FMemory::Memcpy(DestPixels, HdrPixels.GetData(), HdrPixels.Num() * sizeof(FFloat16Color));
                        NewStaticTexture->Source.UnlockMip(0);
                        
                        NewStaticTexture->UpdateResource();
                        Package->MarkPackageDirty();
                        FAssetRegistryModule::AssetCreated(NewStaticTexture);

                        const FString PackageFileName = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
                        FSavePackageArgs SaveArgs;
                        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
                        
                        if (UPackage::SavePackage(Package, NewStaticTexture, *PackageFileName, SaveArgs))
                        {
                            UE_LOG(LogTemp, Warning, TEXT("--- DEBUG: Saved FINAL BAKED Texture ASSET to: %s ---"), *PackagePath);
                            if (GEditor) { TArray<UObject*> AssetsToSync = { NewStaticTexture }; GEditor->SyncBrowserToObjects(AssetsToSync); }
                        }
                        else { UE_LOG(LogTemp, Error, TEXT("Debug_SaveFinalBake: Failed to save package: %s"), *PackagePath); }
                    }
                    else { UE_LOG(LogTemp, Error, TEXT("Debug_SaveFinalBake: Failed to lock mip.")); }
                    NewStaticTexture->RemoveFromRoot();
                }
            }
        }
        // --- <<< END DEBUG SAVE >>> ---

    // --- 9. Cleanup ---

	// --- Cleanup Island Map Texture ---
	if (IslandMapTex)
	{
		IslandMapTex->RemoveFromRoot();
		IslandMapTex->MarkAsGarbage();
	}
    // MID and TempRT are transient UObjects and will be garbage collected.
    UE_LOG(LogTemp, Log, TEXT("BakeWeightedLayerGPU (Resource Ready): MID and TempRT are transient and will be GC'd."));

    UE_LOG(LogTemp, Warning, TEXT("--- BakeWeightedLayerGPU END (Sync + Resource Ready Checks) --- Success: %s ---"), bSuccess ? TEXT("TRUE") : TEXT("FALSE"));
    return bSuccess;
}

// --- ADD THIS NEW HELPER FUNCTION ---
/**
 * Saves a TArray<FVector> buffer as an 8-bit sRGB PNG, normalizing
 * the vectors based on the min/max bounds of the data.
 */
static void TD3D_Debug_SaveVectorBufferAsNormalizedPNG(const TArray<FVector>& VectorBuffer, int32 W, int32 H, const FString& Label)
{
	if (VectorBuffer.Num() != W * H)
	{
		UE_LOG(LogTemp, Error, TEXT("[TD3D_Debug] SaveVectorBufferAsNormalizedPNG: Size mismatch for '%s'."), *Label);
		return;
	}

	// First pass: Find the min and max bounds
	FVector MinPos(FLT_MAX, FLT_MAX, FLT_MAX);
	FVector MaxPos(-FLT_MAX, -FLT_MAX, -FLT_MAX);

	for (const FVector& Pos : VectorBuffer)
	{
		if (!Pos.IsNearlyZero())
		{
			MinPos.X = FMath::Min(MinPos.X, Pos.X);
			MinPos.Y = FMath::Min(MinPos.Y, Pos.Y);
			MinPos.Z = FMath::Min(MinPos.Z, Pos.Z);
			MaxPos.X = FMath::Max(MaxPos.X, Pos.X);
			MaxPos.Y = FMath::Max(MaxPos.Y, Pos.Y);
			MaxPos.Z = FMath::Max(MaxPos.Z, Pos.Z);
		}
	}

	FVector Range = MaxPos - MinPos;
	// Prevent divide by zero if the mesh is flat on one axis
	if (FMath::IsNearlyZero(Range.X)) Range.X = 1.0f;
	if (FMath::IsNearlyZero(Range.Y)) Range.Y = 1.0f;
	if (FMath::IsNearlyZero(Range.Z)) Range.Z = 1.0f;

	TArray<FColor> PixelData;
	PixelData.SetNumUninitialized(W * H);
	for (int32 i = 0; i < VectorBuffer.Num(); ++i)
	{
		const FVector& Pos = VectorBuffer[i];
		if (Pos.IsNearlyZero())
		{
			PixelData[i] = FColor::Black; // Gutter/empty space
		}
		else
		{
			// Normalize from [Min, Max] to [0, 1]
			const FVector Normalized = (Pos - MinPos) / Range;
			
			PixelData[i] = FColor(
				(uint8)FMath::Clamp(FMath::RoundToInt(Normalized.X * 255.0f), 0, 255),
				(uint8)FMath::Clamp(FMath::RoundToInt(Normalized.Y * 255.0f), 0, 255),
				(uint8)FMath::Clamp(FMath::RoundToInt(Normalized.Z * 255.0f), 0, 255),
				255
			);
		}
	}
	
	const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("TD3D_Debug"));
	IFileManager::Get().MakeDirectory(*Dir, true);
	const FString Time = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
	const FString Path = FPaths::Combine(Dir, FString::Printf(TEXT("%s_%dx%d_%s.png"), *Label, W, H, *Time));

	TArray64<uint8> PngData;
	FImageUtils::PNGCompressImageArray(W, H, PixelData, PngData);
	if (FFileHelper::SaveArrayToFile(PngData, *Path)) {
		UE_LOG(LogTemp, Warning, TEXT("[TD3D_Debug] Wrote Normalized Position Map: %s"), *Path);
	} else {
		UE_LOG(LogTemp, Error, TEXT("[TD3D_Debug] FAILED to save Normalized Position Map: %s"), *Path);
	}
}

// --- ADD THIS NEW HELPER FUNCTION ---
/**
 * Saves a TArray<float> buffer (assumed 0.0-1.0 range) to a grayscale PNG file.
 */
static void TD3D_Debug_SaveFloatBufferAsGrayscalePNG(const TArray<float>& FloatBuffer, int32 W, int32 H, const FString& Label)
{
	if (FloatBuffer.Num() != W * H)
	{
		UE_LOG(LogTemp, Error, TEXT("[TD3D_Debug] SaveFloatBufferAsGrayscalePNG: Size mismatch for '%s'."), *Label);
		return;
	}

	TArray<FColor> PixelData;
	PixelData.SetNumUninitialized(W * H);
	for (int32 i = 0; i < FloatBuffer.Num(); ++i)
	{
		const uint8 Val = (uint8)FMath::Clamp(FMath::RoundToInt(FloatBuffer[i] * 255.0f), 0, 255);
		PixelData[i] = FColor(Val, Val, Val, 255);
	}
	
	const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("TD3D_Debug"));
	IFileManager::Get().MakeDirectory(*Dir, true);
	const FString Time = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
	const FString Path = FPaths::Combine(Dir, FString::Printf(TEXT("%s_%dx%d_%s.png"), *Label, W, H, *Time));

	TArray64<uint8> PngData;
	FImageUtils::PNGCompressImageArray(W, H, PixelData, PngData);
	if (FFileHelper::SaveArrayToFile(PngData, *Path)) {
		UE_LOG(LogTemp, Warning, TEXT("[TD3D_Debug] Wrote debug image: %s"), *Path);
	} else {
		UE_LOG(LogTemp, Error, TEXT("[TD3D_Debug] FAILED to save debug image: %s"), *Path);
	}
}

/**
 * Generates (or retrieves from cache/disk) a UTexture2D asset where each pixel's
 * RGB value stores the Absolute World Position (X,Y,Z) corresponding
 * to that texel's UV coordinate for a *specific material slot*.
 *
 * This version SAVES THE BAKED TEXTURE as a new .uasset file.
 */
UTexture2D* FTextureDiffusion3D::GetOrCreatePositionMap(UStaticMesh* MeshToBake, int32 TargetMaterialSlotIndex)
{
	// 1. Validation
	if (!IsValid(MeshToBake) || !IsValid(CurrentProjection_MeshComponent))
	{
		UE_LOG(LogTemp, Error, TEXT("GetOrCreatePositionMap: Current mesh or component is invalid. Cannot get/create map."));
		return nullptr;
	}
	
	const int32 BakeWidth = Global_OutputTextureWidth;
	const int32 BakeHeight = Global_OutputTextureHeight;
	const int32 BakeUVChannel = TargetUVChannel;
	
	// 2. Check In-Session Cache (Cache Hit)
	FPositionMapCacheKey Key = {MeshToBake, BakeWidth, BakeHeight, BakeUVChannel, TargetMaterialSlotIndex};
	if (TObjectPtr<UTexture2D>* FoundMap = CachedPositionMaps.Find(Key))
	{
		if (IsValid(*FoundMap))
		{
			UE_LOG(LogTemp, Log, TEXT("GetOrCreatePositionMap: Found in-session cached Position Map for %s (Slot %d)."), *MeshToBake->GetName(), TargetMaterialSlotIndex);
			return *FoundMap;
		}
		else
		{
			// Stale entry, remove it
			CachedPositionMaps.Remove(Key);
		}
	}

	
	// 4. Not in cache or on disk. Bake a new one.
	UE_LOG(LogTemp, Warning, TEXT("GetOrCreatePositionMap: No valid session cache for %s (Slot %d). Baking new 32-BIT TRANSIENT asset..."), 
			*MeshToBake->GetName(), TargetMaterialSlotIndex);
    
    const double StartTime = FPlatformTime::Seconds();

    FScopedSlowTask BakeTask(1.0f, FText::FromString(TEXT("Baking Position Map for Mesh... (Session Only)")));
    BakeTask.MakeDialog(true);
    BakeTask.EnterProgressFrame(0.1f, FText::FromString(TEXT("Accessing mesh data...")));

	// --- 5. BAKE WORLD POSITIONS ---

	// --- 5a. Get Mesh Data ---
	if (!MeshToBake->GetRenderData() || MeshToBake->GetRenderData()->LODResources.Num() == 0)
	{
		UE_LOG(LogTemp, Error, TEXT("GetOrCreatePositionMap: Mesh '%s' has no render data."), *MeshToBake->GetName());
		return nullptr;
	}
	
	const FStaticMeshLODResources& LOD = MeshToBake->GetRenderData()->LODResources[0];
	const FIndexArrayView Indices = LOD.IndexBuffer.GetArrayView();
	const FPositionVertexBuffer& PosBuffer = LOD.VertexBuffers.PositionVertexBuffer;
	const FStaticMeshVertexBuffer& StaticVB = LOD.VertexBuffers.StaticMeshVertexBuffer;
	
	if (BakeUVChannel >= (int32)StaticVB.GetNumTexCoords())
	{
		 UE_LOG(LogTemp, Error, TEXT("GetOrCreatePositionMap: Invalid UVChannel %d. Mesh only has %d UV channels."), BakeUVChannel, StaticVB.GetNumTexCoords());
		 return nullptr;
	}
	
	const FTransform& MeshTransform = CurrentProjection_MeshComponent->GetComponentTransform();
	const int32 NumTexels = BakeWidth * BakeHeight;

	TArray<FVector> WorldPositionData;
	WorldPositionData.Init(FVector::ZeroVector, NumTexels);

	// --- (FIX) Create a separate buffer for the Alpha Mask ---
	TArray<float> AlphaMaskData;
	AlphaMaskData.Init(0.0f, NumTexels);
	// ---

	BakeTask.EnterProgressFrame(0.3f, FText::FromString(TEXT("Rasterizing triangles in UV space...")));

	// --- 5b. Rasterize Triangles (SLOT-AWARE) ---
	for (const FStaticMeshSection& Section : LOD.Sections)
	{
		if (Section.MaterialIndex != TargetMaterialSlotIndex)
		{
			continue; // Skip sections not matching our target
		}

		for (uint32 i = 0; i < Section.NumTriangles; ++i)
		{
			const uint32 TriIndex = Section.FirstIndex / 3 + i;
			
			const uint32 i0 = Indices[TriIndex * 3 + 0];
			const uint32 i1 = Indices[TriIndex * 3 + 1];
			const uint32 i2 = Indices[TriIndex * 3 + 2];

			const FVector2D UV0(StaticVB.GetVertexUV(i0, BakeUVChannel));
			const FVector2D UV1(StaticVB.GetVertexUV(i1, BakeUVChannel));
			const FVector2D UV2(StaticVB.GetVertexUV(i2, BakeUVChannel));

			const FVector P0 = FVector(PosBuffer.VertexPosition(i0)); 
			const FVector P1 = FVector(PosBuffer.VertexPosition(i1)); 
			const FVector P2 = FVector(PosBuffer.VertexPosition(i2));

			const float MinU = FMath::Min3(UV0.X, UV1.X, UV2.X);
			const float MinV = FMath::Min3(UV0.Y, UV1.Y, UV2.Y);
			const float MaxU = FMath::Max3(UV0.X, UV1.X, UV2.X);
			const float MaxV = FMath::Max3(UV0.Y, UV1.Y, UV2.Y);

			const int32 MinX = FMath::Clamp(FMath::FloorToInt(MinU * BakeWidth), 0, BakeWidth - 1);
			const int32 MinY = FMath::Clamp(FMath::FloorToInt(MinV * BakeHeight), 0, BakeHeight - 1);
			const int32 MaxX = FMath::Clamp(FMath::CeilToInt(MaxU * BakeWidth), 0, BakeWidth - 1);
			const int32 MaxY = FMath::Clamp(FMath::CeilToInt(MaxV * BakeHeight), 0, BakeHeight - 1);

			for (int32 Y = MinY; Y <= MaxY; ++Y)
			{
				for (int32 X = MinX; X <= MaxX; ++X)
				{
					const FVector2D TexelUV((X + 0.5f) / BakeWidth, (Y + 0.5f) / BakeHeight);
					float u, v, w;
					if (GetBarycentricCoords(TexelUV, UV0, UV1, UV2, u, v, w))
					{
						const int32 TexelIndex = Y * BakeWidth + X;
						WorldPositionData[TexelIndex] = P0 * u + P1 * v + P2 * w;
						
						// --- (FIX) Write to the separate AlphaMaskData buffer ---
						AlphaMaskData[TexelIndex] = 1.0f;
					}
				}
			}
		} // End triangle loop
	} // End section loop

	// --- (DEBUG) Save the pre-gutter pass data ---
	const FString DebugBaseName = FString::Printf(TEXT("PosMap_%s_Slot%d"), *MeshToBake->GetName(), TargetMaterialSlotIndex);
	TD3D_Debug_SaveVectorBufferAsNormalizedPNG(WorldPositionData, BakeWidth, BakeHeight, DebugBaseName + TEXT("_PreGutter_RGB"));
	TD3D_Debug_SaveFloatBufferAsGrayscalePNG(AlphaMaskData, BakeWidth, BakeHeight, DebugBaseName + TEXT("_PreGutter_Alpha"));
	// ---

	// --- 5c. Gutter Extrapolation ---
	BakeTask.EnterProgressFrame(0.2f, FText::FromString(TEXT("Extrapolating Gutter...")));
	TArray<FVector> ReadData = WorldPositionData;
	TArray<FVector> WriteData = WorldPositionData;
	const int32 GutterSize = 2; // Or your preferred gutter size
	
	for (int32 Pass = 0; Pass < GutterSize; ++Pass)
	{
		ReadData = WriteData; 
		for (int32 Y = 0; Y < BakeHeight; ++Y)
		{
			for (int32 X = 0; X < BakeWidth; ++X)
			{
				const int32 TexelIndex = Y * BakeWidth + X;
				if (!ReadData[TexelIndex].IsNearlyZero()) continue; 
				FVector NeighborValue = FVector::ZeroVector;
				int NeighborCount = 0;
				if (X > 0 && !ReadData[TexelIndex - 1].IsNearlyZero()) { NeighborValue += ReadData[TexelIndex - 1]; NeighborCount++; }
				if (X < BakeWidth - 1 && !ReadData[TexelIndex + 1].IsNearlyZero()) { NeighborValue += ReadData[TexelIndex + 1]; NeighborCount++; }
				if (Y > 0 && !ReadData[TexelIndex - BakeWidth].IsNearlyZero()) { NeighborValue += ReadData[TexelIndex - BakeWidth]; NeighborCount++; }
				if (Y < BakeHeight - 1 && !ReadData[TexelIndex + BakeWidth].IsNearlyZero()) { NeighborValue += ReadData[TexelIndex + BakeWidth]; NeighborCount++; }
				if (NeighborCount > 0) { WriteData[TexelIndex] = NeighborValue / (float)NeighborCount; }
			}
		}
	}
	WorldPositionData = WriteData; // Final padded data

	// --- (DEBUG) Save the post-gutter pass data ---
	TD3D_Debug_SaveVectorBufferAsNormalizedPNG(WorldPositionData, BakeWidth, BakeHeight, DebugBaseName + TEXT("_PostGutter_RGB"));
	// ---

	// --- 5d. Validation (After Padding) ---
	int32 NonZeroPositions = 0;
	for (const FVector& Pos : WorldPositionData) { if (!Pos.IsNearlyZero()) { NonZeroPositions++; } }
	
	if (NonZeroPositions == 0)
	{
		UE_LOG(LogTemp, Error, TEXT("GetOrCreatePositionMap: Position buffer is ALL ZEROS after padding. Check mesh UVs (Channel %d) or component transform."), BakeUVChannel);
		return nullptr;
	}
	UE_LOG(LogTemp, Log, TEXT("GetOrCreatePositionMap: Bake & Padding complete. Found %d valid texels (%.1f%% coverage)."), 
		NonZeroPositions, (float)NonZeroPositions * 100.0f / (float)NumTexels);

	// --- 6. Convert FVector to FLinearColor (32-BIT) ---
	BakeTask.EnterProgressFrame(0.1f, FText::FromString(TEXT("Converting position data...")));
	
    // --- (FIX) We now use FLinearColor (32-bit) instead of FFloat16Color (16-bit) ---
	TArray<FLinearColor> PositionMapPixels; 
	PositionMapPixels.SetNumUninitialized(NumTexels);
	
	for (int32 i = 0; i < NumTexels; ++i)
	{
		const FVector& WorldPos = WorldPositionData[i];
		const float Alpha = AlphaMaskData[i];
        // FLinearColor is 32-bit per channel, just like FVector + a float
		PositionMapPixels[i] = FLinearColor(WorldPos.X, WorldPos.Y, WorldPos.Z, Alpha); 
	}

// --- 7. Create a new TRANSIENT UTexture2D Asset ---
    BakeTask.EnterProgressFrame(0.1f, FText::FromString(TEXT("Creating Texture Resource...")));
    TObjectPtr<UTexture2D> NewPosMap = UTexture2D::CreateTransient(BakeWidth, BakeHeight, PF_A32B32G32R32F);
    if (!NewPosMap)
    {
        UE_LOG(LogTemp, Error, TEXT("GetOrCreatePositionMap: Failed to create transient 32-bit texture."));
        return nullptr;
    }
    
    // We do NOT set RF_Public or RF_Standalone.
    // We DO AddToRoot to keep it from being GC'd during this session.
    NewPosMap->AddToRoot();
    
    // --- 8. Set properties and copy data ---
    NewPosMap->CompressionSettings = TC_HDR_F32;
    NewPosMap->SRGB = false;
    NewPosMap->Filter = TF_Bilinear;
    NewPosMap->AddressX = TA_Clamp;
    NewPosMap->AddressY = TA_Clamp;
    NewPosMap->LODGroup = TEXTUREGROUP_World;
    
    FTexture2DMipMap& Mip = NewPosMap->GetPlatformData()->Mips[0];
    void* MipData = Mip.BulkData.Lock(LOCK_READ_WRITE);
    FMemory::Memcpy(MipData, PositionMapPixels.GetData(), NumTexels * sizeof(FLinearColor));
    Mip.BulkData.Unlock();
    
    NewPosMap->UpdateResource(); 
    FlushRenderingCommands(); 

    if (!NewPosMap->GetPlatformData() || NewPosMap->GetPlatformData()->Mips.Num() == 0)
    {
        UE_LOG(LogTemp, Error, TEXT("GetOrCreatePositionMap: Failed to build PlatformData after 32-bit bake."));
        NewPosMap->RemoveFromRoot(); // Clean up root
        return nullptr; 
    }

	// --- 9. Add to in-session cache ONLY ---
    // (All saving and AssetRegistry logic is removed)
    CachedPositionMaps.Add(Key, NewPosMap); // Add new asset to session cache

    BakeTask.EnterProgressFrame(0.1f); // Final progress
    
    const double EndTime = FPlatformTime::Seconds();
    UE_LOG(LogTemp, Warning, TEXT("GetOrCreatePositionMap: 32-BIT TRANSIENT Bake complete in %.2f seconds. Asset is session-only."), 
        EndTime - StartTime);
    
    return NewPosMap;
}

UTexture2D* FTextureDiffusion3D::CreateUVIslandMapTexture(int32 Width, int32 Height)
{
    const int32 NumTexels = Width * Height;
    
    if (PrecomputedUVIslandMap.Num() != NumTexels)
    {
        UE_LOG(LogTemp, Error, TEXT("CreateUVIslandMapTexture: Island map size mismatch. Expected %d, got %d"), 
            NumTexels, PrecomputedUVIslandMap.Num());
        return nullptr;
    }

    // Create a transient R32F texture to store island IDs as floats
    // We use float because integer textures have limited shader support
    UTexture2D* IslandMapTex = UTexture2D::CreateTransient(Width, Height, PF_R32_FLOAT);
    if (!IslandMapTex)
    {
        UE_LOG(LogTemp, Error, TEXT("CreateUVIslandMapTexture: Failed to create transient texture"));
        return nullptr;
    }

    IslandMapTex->AddToRoot(); // Prevent GC during this session
    IslandMapTex->CompressionSettings = TC_HDR; // No compression for data texture
    IslandMapTex->SRGB = false; // Linear data
    IslandMapTex->Filter = TF_Nearest; // CRITICAL: No interpolation! We need exact integer IDs
    IslandMapTex->AddressX = TA_Clamp;
    IslandMapTex->AddressY = TA_Clamp;
    IslandMapTex->LODGroup = TEXTUREGROUP_World;

    // Fill with island IDs (stored as float for R32F format)
    TArray<float> IslandData;
    IslandData.SetNumUninitialized(NumTexels);
    
    for (int32 i = 0; i < NumTexels; ++i)
    {
        // Store island ID directly. -1 becomes -1.0f (invalid/empty texel)
        IslandData[i] = static_cast<float>(PrecomputedUVIslandMap[i]);
    }

    // Copy data to texture
    FTexture2DMipMap& Mip = IslandMapTex->GetPlatformData()->Mips[0];
    void* MipData = Mip.BulkData.Lock(LOCK_READ_WRITE);
    FMemory::Memcpy(MipData, IslandData.GetData(), NumTexels * sizeof(float));
    Mip.BulkData.Unlock();

    IslandMapTex->UpdateResource();
    FlushRenderingCommands();

    UE_LOG(LogTemp, Log, TEXT("CreateUVIslandMapTexture: Created %dx%d island map texture"), Width, Height);
    
    return IslandMapTex;
}


void FTextureDiffusion3D::ProcessProjectionResults(const TMap<EProjectionVariant, UTexture2D*>& ResultsByVariant)
{
    UE_LOG(LogTemp, Warning, TEXT("--- ProcessProjectionResults START (Synchronous Loop - Fence Path) for Slot %d, Camera %d ---"), CurrentProjection_Settings.TargetMaterialSlotIndex, CurrentProjection_CameraIndex);
    UE_LOG(LogTemp, Log, TEXT("Processing %d variant texture(s)..."), ResultsByVariant.Num());

    // --- Step 0: Initial Validation ---
    if (!CurrentProjection_StaticMesh || !CurrentProjection_MeshComponent || ResultsByVariant.Num() == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("ProcessProjectionResults Sync(Fence): Missing context or empty results map. Aborting processing."));
        OnSingleProjectionFinished.ExecuteIfBound(); // Signal completion/failure
        return;
    }

    const int32 Slot = CurrentProjection_Settings.TargetMaterialSlotIndex;
    const int32 Cam  = CurrentProjection_CameraIndex;
    const int32 NumTexels = CurrentProjection_NumTexels; // Width * Height
    const int32 Width = CurrentProjection_TextureWidth;
    const int32 Height = CurrentProjection_TextureHeight;

    // --- Step 1: Calculate CPU Weights (Done once for all variants) ---
    UE_LOG(LogTemp, Log, TEXT("ProcessProjectionResults Sync(Fence): [Step 1] Calculating CPU weights..."));
    TArray<float>     NormalWeightBuffer; // We need to keep this buffer until the end
    TArray<FVector2D> ScreenPositionBuffer; // Needed for CPU Normal path
    TArray<bool>      VisibilityBuffer;
    TSet<int32> HiddenSlotIndices;
    for (const auto& Elem : SlotHiddenStates) { if (Elem.Value) HiddenSlotIndices.Add(Elem.Key); }

    CalculateCameraWeights(
        CurrentProjection_StaticMesh, CurrentProjection_MeshComponent->GetComponentTransform(),
        CurrentProjection_Settings, Width, Height,
        NormalWeightBuffer, ScreenPositionBuffer, VisibilityBuffer,
        TargetUVChannel, HiddenSlotIndices
    );

    if (NormalWeightBuffer.Num() != NumTexels || VisibilityBuffer.Num() != NumTexels)
    {
         UE_LOG(LogTemp, Error, TEXT("ProcessProjectionResults Sync(Fence): Weight/Visibility buffer size mismatch after calculation! Expected %d, Got W:%d V:%d. Aborting."), NumTexels, NormalWeightBuffer.Num(), VisibilityBuffer.Num());
         HandleProjectionError_Internal("Weight buffer size mismatch.");
         OnSingleProjectionFinished.ExecuteIfBound();
         return;
    }
    UE_LOG(LogTemp, Log, TEXT("ProcessProjectionResults Sync(Fence): [Step 1] CPU Weights Calculated (Buffer size: %d)."), NormalWeightBuffer.Num());


    // --- Step 3: Get PositionMap Texture ---
    UE_LOG(LogTemp, Log, TEXT("ProcessProjectionResults Sync(Fence): [Step 3] Getting or Creating Position Map..."));
    TObjectPtr<UTexture2D> PositionMap = GetOrCreatePositionMap(CurrentProjection_StaticMesh, Slot); 
     if (!IsValid(PositionMap)) {
        HandleProjectionError_Internal("Failed to get/create Position Map.");
        OnSingleProjectionFinished.ExecuteIfBound();
        return;
    }
     // Assuming GetOrCreatePositionMap handles AddToRoot if needed.


    // --- Step 4 & 5: PROCESS EACH VARIANT SYNCHRONOUSLY & STORE RESULT ---
    UE_LOG(LogTemp, Log, TEXT("ProcessProjectionResults Sync(Fence): [Step 4/5] Starting synchronous processing loop..."));
    bool bAnyDataStored = false;
    for (const auto& Pair : ResultsByVariant)
    {
        const EProjectionVariant Variant = Pair.Key;
        UTexture2D* const ResultTexture = Pair.Value; // This is the input (e.g., from ComfyUI)

        if (!ResultTexture) {
             UE_LOG(LogTemp, Warning, TEXT("ProcessProjectionResults Sync(Fence): Skipping Variant %s - Input Texture pointer is null."), *UEnum::GetValueAsString(Variant));
             continue; // Skip this variant
         }
        UE_LOG(LogTemp, Log, TEXT("  -> Processing Variant %s..."), *UEnum::GetValueAsString(Variant));

        TArray<FLinearColor> ProcessedPixels_Linear; // Output buffer for this variant
        bool bProcessedSuccessfully = false;

        if (Variant == EProjectionVariant::Normal)
		{
			UE_LOG(LogTemp, Log, TEXT("    -> Using ProjectNormalDetails for normal map..."));
			bProcessedSuccessfully = ProjectNormalDetails(
				ResultTexture,
				VisibilityBuffer,
				ScreenPositionBuffer,
				NormalWeightBuffer,
				CurrentProjection_Settings,
				Width,
				Height,
				ProcessedPixels_Linear
			);
		}
		else
        {
            // --- CPU Path for Color Variants ---
            UE_LOG(LogTemp, Log, TEXT("    -> Using CPU path (ProjectColor)..."));
            
            // Note: We use ScreenPositionBuffer here (calculated in Step 1 of this function), 
            // whereas the GPU path used PositionMap.
            bProcessedSuccessfully = ProjectColor(
                ResultTexture,          // Source UTexture2D*
                VisibilityBuffer,       // Calculated in Step 1
                ScreenPositionBuffer,   // Calculated in Step 1
                NormalWeightBuffer,     // Calculated in Step 1
                Width, 
                Height,
                ProcessedPixels_Linear  // Output Buffer
            );

            if (!bProcessedSuccessfully) 
            {
                UE_LOG(LogTemp, Error, TEXT("    -> CPU Projection FAILED for Variant %s."), *UEnum::GetValueAsString(Variant));
            }
        }

        // --- Store Result if Successful ---
        if (bProcessedSuccessfully && ProcessedPixels_Linear.Num() > 0)
        {
             UE_LOG(LogTemp, Log, TEXT("    -> Storing processed data (Size: %d)..."), ProcessedPixels_Linear.Num());
             TMap<EProjectionVariant, TArray<TArray<FLinearColor>>>& SlotVariantMap = PerVariantProjectionLayers.FindOrAdd(Slot);
             TArray<TArray<FLinearColor>>& CameraLayers = SlotVariantMap.FindOrAdd(Variant);
             if (CameraLayers.Num() <= Cam) { CameraLayers.SetNum(Cam + 1); }
             CameraLayers[Cam] = MoveTemp(ProcessedPixels_Linear); // Store result

              // Compute Base Color if applicable
             const EProjectionVariant BaseColorReferenceVariant = EProjectionVariant::Shaded; // Use Shaded result for base color calculation
             if (Variant == BaseColorReferenceVariant && Cam == 0)
             {
                 UE_LOG(LogTemp, Log, TEXT("    -> Computing base color..."));
                 TArray<FColor> sRGB_Pixels = FTextureUtils::LinearTo_sRGB(CameraLayers[Cam]); // Use the just-stored data
                 ComputeAndStoreBaseColor(sRGB_Pixels);
             }
             bAnyDataStored = true; // Mark that we successfully stored something for this camera view
        }
        else
        {
             UE_LOG(LogTemp, Warning, TEXT("    -> FAILED to process or empty pixels returned for Variant %s. Skipping storage."), *UEnum::GetValueAsString(Variant));
        }
    } // End variant loop


    // --- Step 6, 7, 8: Store Weights, Reblend, Signal Completion ---
    if (bAnyDataStored)
    {
        // --- Step 6: Store original CPU weights ---
        UE_LOG(LogTemp, Log, TEXT("ProcessProjectionResults Sync(Fence): [Step 6] Storing original CPU weight buffer..."));
        TArray<TArray<float>>& SlotWeightLayers = PerSlotWeightLayers.FindOrAdd(Slot);
        if (SlotWeightLayers.Num() <= Cam) { SlotWeightLayers.SetNum(Cam + 1); }
        // Move the weights now that the loop using them is done
        SlotWeightLayers[Cam] = MoveTemp(NormalWeightBuffer);

        // --- Step 7: Trigger Reblend ---
        UE_LOG(LogTemp, Log, TEXT("ProcessProjectionResults Sync(Fence): [Step 7] Triggering ReblendAndUpdateMesh..."));
        ReblendAndUpdateMesh();
    }
    else
    {
         UE_LOG(LogTemp, Warning, TEXT("ProcessProjectionResults Sync(Fence): No valid variant data processed or stored. Skipping weight storage and reblend."));
         // Clear the NormalWeightBuffer since it wasn't moved
         NormalWeightBuffer.Empty();
    }

    // --- Step 8: Signal Overall Completion ---
    UE_LOG(LogTemp, Warning, TEXT("--- ProcessProjectionResults END (Synchronous Loop - Fence Path) for Slot %d, Camera %d ---"), Slot, Cam);
    OnSingleProjectionFinished.ExecuteIfBound();
}

void FTextureDiffusion3D::ComputeAndStoreBaseColor(const TArray<FColor>& ProjectedPixels)
{
	UE_LOG(LogTemp, Log, TEXT("First projection for slot %d. Computing base color..."), CurrentProjection_Settings.TargetMaterialSlotIndex);

	UTexture2D* TempTextureForAnalysis = FTextureUtils::CreateTextureFromPixelData(
		CurrentProjection_TextureWidth, CurrentProjection_TextureHeight, ProjectedPixels);

	if (TempTextureForAnalysis)
	{
		TempTextureForAnalysis->SetFlags(RF_Transient);
		TempTextureForAnalysis->SRGB = true;

		FLinearColor ComputedBaseColor = FTextureUtils::ComputeBaseColor_WeightedMedianLab(
			TempTextureForAnalysis, 0.1f, 0.05f, 16000);

		const int32 TargetSlot = CurrentProjection_Settings.TargetMaterialSlotIndex;
		PerSlotBaseColor.Add(TargetSlot, ComputedBaseColor);
		
		// Log and refresh UI
		FColor sRGBColor = ComputedBaseColor.ToFColor(true);
		UE_LOG(LogTemp, Log, TEXT("Base color for slot %d computed: (sRGB: #%02X%02X%02X)"), TargetSlot, sRGBColor.R, sRGBColor.G, sRGBColor.B);
		if (SettingsWidget.IsValid())
		{
			TArray<FProjectionSettings>& SlotCameraSettings = PerSlotCameraSettings.FindOrAdd(TargetSlot);
			SettingsWidget->SetCameraSettings(SlotCameraSettings, ActiveCameraIndex, Global_OutputPath, Global_OutputTextureWidth, Global_OutputTextureHeight);
		}
		TempTextureForAnalysis->MarkAsGarbage();
	}
	if (GEditor)
	{
		GEditor->RedrawAllViewports(true);
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FTextureDiffusion3D, TextureDiffusion3D)
