#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Engine/StaticMeshActor.h"
#include "TextureProjectionWindow.h"
#include "Engine/SceneCapture2D.h"


#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Json.h"
#include "JsonUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/MessageDialog.h"
#include "Helpers/UnrealComfyUITypes.h"
#include "Interfaces/IPluginManager.h"
#include "Dom/JsonObject.h"

#include "Containers/Ticker.h" 
// Forward declarations
class FToolBarBuilder;
class FMenuBuilder;
class UTexture2D;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UTextureRenderTarget2D;


/**
 * New Enum for managing different texture projection types.
 */
UENUM(BlueprintType)
enum class EProjectionVariant : uint8
{
	BaseColor,
	Normal,
	Roughness,
	Metallic,
	AO,
	Shaded
};

struct FComfyOutputRequest
{
	FString Prefix;
	EProjectionVariant Variant;
};

static EProjectionVariant VariantFromPrefixLoose(const FString& InPrefix)
{
    FString S = InPrefix;
    S.ToLowerInline();

    // ORDER MATTERS: check tokens that contain other tokens first
    // (e.g., "metallic" contains "lit", so check metallic before lit)

    // Metallic
    if (S.Contains(TEXT("metallic")) || S.Contains(TEXT("metal")) || S.Contains(TEXT("mtl")))
    {
        return EProjectionVariant::Metallic;
    }

    // Roughness
    if (S.Contains(TEXT("roughness")) || S.Contains(TEXT("rough")))
    {
        return EProjectionVariant::Roughness;
    }

    // Ambient Occlusion
    if (S.Contains(TEXT("ambientocclusion")) || S.Contains(TEXT("occlusion")) || S.Contains(TEXT("ao")))
    {
        return EProjectionVariant::AO;
    }

    // Normal
    if (S.Contains(TEXT("normal")) || S.Contains(TEXT("nrm")))
    {
        return EProjectionVariant::Normal;
    }

    if (S.Contains(TEXT("lighting")) || S.Contains(TEXT("shaded")) || S.Contains(TEXT("beauty")))
    {
        return EProjectionVariant::Shaded;
    }

    // Base Color (fallback)
    if (S.Contains(TEXT("basecolor")) || S.Contains(TEXT("albedo")) || S.Contains(TEXT("diffuse")) || S.Contains(TEXT("color")))
    {
        return EProjectionVariant::BaseColor;
    }

    // Default/fallback
    return EProjectionVariant::BaseColor;
}

DECLARE_DELEGATE_TwoParams(FOnImageUploadComplete, bool /* bSuccess */, const FString& /* ServerFilename */);


/**
 * Texture Diffusion Module - Focuses on camera-based texture projection
 */
class FTextureDiffusion3D : public IModuleInterface
{
public:
	FTextureDiffusion3D();
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	
	/** UI Actions */
	void PluginButtonClicked();
	void ProjectTextureButtonClicked();

	/**
	 * Parses a ComfyUI API-format JSON to find nodes tagged for Unreal.
	 * @param FilePath The full path to the .json workflow file.
	 * @param OutSortedNodes An array that will be populated with the parsed node info, in execution order.
	 * @return True if parsing was successful, false otherwise.
	 */
	bool ParseWorkflowForUnrealNodes(const FString& FilePath, TArray<FUnrealNodeInfo>& OutSortedNodes);

	TSharedPtr<FJsonObject> GetWorkflowJsonObject(const FString& FilePath);

	UTexture2D* GetPreviewTextureForKeyword(FString ImageKeyword);
	
private:

	/** Creates a transient texture of the current Base Color view. */
	UTexture2D* CreateBaseColorPreviewTexture();
	
	
	UTexture2D* CreateMaskPreviewTexture();

	/** Creates a transient texture of the current Depth view. */
	UTexture2D* CreateDepthPreviewTexture();

	UTexture2D* CreateReferencePreviewTexture();

	
	UTexture2D* CreateBaseLaidPreviewTexture();

	/** Creates toolbar buttons for the plugin */
	void RegisterMenus();

	/**
	 * Retrieves the currently selected static mesh actor in the editor
	 * @return The selected actor, or nullptr if none is selected
	 */
	TObjectPtr<AStaticMeshActor> GetSelectedActor();
	
	/**
	 * Called when projection settings are changed in the UI
	 * @param NewSettings The updated settings
	 */
	void OnProjectionSettingsChanged(const FProjectionSettings& NewSettings);
	
	/**
	 * Validates input actor for projection
	 * @param StaticMeshActor The actor to validate
	 * @return True if the actor is valid for projection
	 */
	bool ValidateInputs(TObjectPtr<AStaticMeshActor> StaticMeshActor);
	
	/**
	 * Get projection settings from the UI
	 * @return The current projection settings
	 */
	FProjectionSettings GetProjectionSettings();

	struct FPositionMapCacheKey
	{
		TObjectPtr<UStaticMesh> Mesh;
		int32 Width;
		int32 Height;
		int32 UVChannel;
		int32 MaterialSlotIndex; // <<< ADD THIS LINE

		// Update comparison operators
		bool operator==(const FPositionMapCacheKey& Other) const
		{
			return Mesh == Other.Mesh &&
				Width == Other.Width &&
				Height == Other.Height &&
				UVChannel == Other.UVChannel &&
				MaterialSlotIndex == Other.MaterialSlotIndex; // <<< ADD THIS CHECK
		}

		friend uint32 GetTypeHash(const FPositionMapCacheKey& Key)
		{
			return HashCombine(
				GetTypeHash(Key.Mesh),
				HashCombine(
					GetTypeHash(Key.Width),
					HashCombine(
						GetTypeHash(Key.Height),
						HashCombine(GetTypeHash(Key.UVChannel), GetTypeHash(Key.MaterialSlotIndex)) // <<< ADD THIS HASH
					)
				)
			);
		}
	};


            
    /** Caches generated Position Maps (WorldPos baked to texture) to avoid re-baking. */
    UPROPERTY(Transient)
    TMap<FPositionMapCacheKey, TObjectPtr<UTexture2D>> CachedPositionMaps;

/**
 * @brief Reads Linear pixels (FLinearColor) back from a Render Target to CPU.
 */
	bool ReadLinearPixelsFromRT(UTextureRenderTarget2D* RenderTarget, TArray<FLinearColor>& OutLinearPixelData);


	/**
	 * Generates (or retrieves from cache/disk) a UTexture2D asset storing
	 * World Positions for a *specific material slot*.
	 */
	UTexture2D* GetOrCreatePositionMap(UStaticMesh* MeshToBake, int32 TargetMaterialSlotIndex);


		/** Creates a GPU texture from the precomputed UV island map for seam detection in shaders */
	UTexture2D* CreateUVIslandMapTexture(int32 Width, int32 Height);
	
	bool BakeWeightedLayerGPU(
		UTexture2D* SourceTexture,
		UTexture2D* PositionMap,
		const TArray<float>& CpuWeightBuffer,
		const TArray<bool>& CpuVisibilityBuffer,
		const FProjectionSettings& InCameraSettings,
		int32 OutWidth,
		int32 OutHeight,
		EProjectionVariant CurrentVariant,
		TArray<FLinearColor>& OutWeightedPixels);

		/**
	 * Projects normal map details onto a mesh surface.
	 * 
	 * Extracts high-frequency detail from a view-space normal map (e.g., Marigold output)
	 * and applies it to the mesh's geometric normals, ensuring details conform to the
	 * actual surface orientation rather than appearing as a flat decal.
	 *
	 * @param InViewSpaceNormalTex    Source normal map in view/camera space (Z toward camera)
	 * @param VisibilityBuffer        Per-texel visibility from camera
	 * @param ScreenPositionBuffer    Per-texel screen UV coordinates for sampling source
	 * @param NormalWeightBuffer      Per-texel projection weight (0-1)
	 * @param Settings                Camera/projection settings
	 * @param TextureWidth            Output texture width
	 * @param TextureHeight           Output texture height
	 * @param OutTangentSpaceNormals  Output buffer - tangent space normals with weight in alpha
	 * @return                        True if successful
	 */
bool ProjectNormalDetails(
    UTexture2D* InTangentSpaceNormalTex,
    const TArray<bool>& VisibilityBuffer,
    const TArray<FVector2D>& ScreenPositionBuffer,
    const TArray<float>& NormalWeightBuffer,
    const FProjectionSettings& Settings,
    int32 TextureWidth,
    int32 TextureHeight,
    TArray<FLinearColor>& OutLinearTextureBuffer);

	void ExportBaseColorForComfyUI(TObjectPtr<AStaticMeshActor> StaticMeshActor, const FProjectionSettings& Settings, const FString& FullExportPathWithExtension);
	
	// Tab management functions
	/**
	 * Adds a new camera tab with default settings
	 */
	void AddCameraTab();
	

	// In TextureDiffusion3D.h, private section

    /**
     * @brief Duplicates and saves an HDR UTexture2D (like a Position Map) as a new .uasset.
     * Assumes the input texture has HDR source data (e.g., TSF_RGBA16F).
     * @param TextureToSave The HDR texture to save (e.g., PositionMap)
     * @param BaseAssetName The base name for the new asset (e.g., "Debug_PositionMap")
     */
    void Debug_SaveHDRTextureAsAsset(UTexture2D* TextureToSave, const FString& BaseAssetName);

	/**
     * @brief Decompresses and saves a UTexture2D as a new .uasset in /Game/TD3D_Debug/ for inspection.
     * @param TextureToSave The texture to save (e.g., SourceTexture)
     * @param BaseAssetName The base name for the new asset (e.g., "Debug_SourceTexture")
     */
    void Debug_SaveTextureAsAsset(UTexture2D* TextureToSave, const FString& BaseAssetName);
	
	/**
	 * Removes the specified camera tab
	 * @param TabIndex The index of the tab to remove
	 */
	void RemoveCameraTab(int32 TabIndex);
	
	/**
	 * Selects the specified camera tab
	 * @param TabIndex The index of the tab to select
	 */
	void SelectCameraTab(int32 TabIndex);
	
	/**
	 * Called when a new camera tab is added
	 */
	void OnCameraTabAdded();

	/**
	 * Called when a camera tab is removed
	 * @param TabIndex The index of the removed tab
	 */
	void OnCameraTabRemoved(int32 TabIndex);



	void OnTargetMaterialSlotChanged(int32 NewSlotIndex);

	void OnOccluderCheckboxChanged(bool bIsHidden);

	// Export functionality wrappers
	void ExportDepthImage();
	void ExportBaseColor(); 
	void ExportCurrentView();
	UTexture2D* ApplyWeightsMaterial(const TArray<TArray<float>>& InWeightBuffers);




	/**
		 * Calculates camera contribution, including an occlusion check.
		 * It takes a captured depth map of the scene and modifies the visibility buffer directly.
		 */

	void CalculateCameraWeights(
		UStaticMesh* StaticMesh,
		const FTransform& MeshTransform,
		const FProjectionSettings& Settings,
		int32 OutputWidth,
		int32 OutputHeight,
		TArray<float>& OutNormalWeightBuffer,
		TArray<FVector2D>& OutScreenPositionBuffer, 
		TArray<bool>& OutVisibilityBuffer, 	
		int32 UVChannel,
		const TSet<int32>& HiddenSlotIndices
	);

	/**
	 * Captures mesh depth from current camera position
	 * @param StaticMeshActor The actor to capture
	 * @param Settings The projection settings
	 * @param OutDepthBuffer Output depth buffer
	 * @param InOverrideExportDirectory Override export directory
	 * @param InOverrideBaseFileName Override base file name
	 */
	void CaptureMeshDepth(
			TObjectPtr<AStaticMeshActor> StaticMeshActor,
			const FProjectionSettings& Settings,
			int32 Width ,
			int32 Height ,
			TArray<float>& OutDepthBuffer, // This is for getting the raw buffer if needed
			// --- Add these new optional parameters ---
			const FString& InOverrideExportDirectory = TEXT(""), // If empty, uses default
			const FString& InOverrideBaseFileName = TEXT("") 	// If empty, uses defaul
		);

	void CaptureMeshNormals(
		TObjectPtr<AStaticMeshActor> StaticMeshActor,
		const FProjectionSettings& Settings,
		int32 Width,
		int32 Height,
		TArray<FLinearColor>& OutLinearTextureBuffer,
		const FString& InOverrideExportDirectory,
		const FString& InOverrideBaseFileName
	);
		
	/**
	 * @brief Captures the world normals of a mesh from the specified camera's viewpoint,
	 * transforms them into view-space, and saves the result as a PNG.
	 * @param StaticMeshActor The actor to capture.
	 * @param Settings The camera projection settings defining the viewpoint.
	 * @param Width The width of the output texture.
	 * @param Height The height of the output texture.
	 * @param OutputFilePath The full path where the resulting PNG should be saved.
	 * @return True if the capture and save were successful, false otherwise.
	 */
	bool CaptureViewSpaceNormals(
		TObjectPtr<AStaticMeshActor> StaticMeshActor,
		const FProjectionSettings& Settings,
		int32 Width,
		int32 Height,
		const FString& OutputFilePath
	);
	/**
	 * Captures mesh appearance with current materials
	 * @param StaticMeshActor The actor to capture
	 * @param Settings The projection settings
	 * @param OutMaskBuffer Output color buffer
	 * @param CustomSuffix Custom suffix for output files
	 */
	void CaptureMeshTexturedMask(
			TObjectPtr<AStaticMeshActor> StaticMeshActor,
			const FProjectionSettings& Settings,
			int32 Width, 
			int32 Height, 	
			TArray<FColor>& OutMaskBuffer, // Still useful if caller wants the raw pixels
			const FString& CustomSuffixForLogging,
			// --- NEW Optional Parameters for direct saving ---
			const FString& OverrideExportDirectory = TEXT(""),
			const FString& OverrideBaseFileName = TEXT("") // If provided, will save a PNG here
		);

	void QueueNextBatchProjection();
	
	/**
	 * Creates a texture by projecting a source image onto the mesh. This version is simplified
	 * and relies on the pre-calculated VisibilityBuffer for occlusion.
	 */
	bool ProjectColor(
		UTexture2D* InSourceTexture,
		const TArray<bool>& VisibilityBuffer,
		const TArray<FVector2D>& ScreenPositionBuffer,
		const TArray<float>& NormalWeightBuffer,
		int32 TextureWidth,
		int32 TextureHeight,
		TArray<FLinearColor>& OutLinearTextureBuffer);

		


	bool CreateWeightedNormalProjectionLayer(
		UTexture2D* InViewSpaceNormalTex,            // Comfy output (view/camera space normals)
		const TArray<bool>& VisibilityBuffer,
		const TArray<FVector2D>& ScreenPositionBuffer,
		const TArray<float>& NormalWeightBuffer,
		const FProjectionSettings& View,             // for camera basis
		int32 TextureWidth,
		int32 TextureHeight,
		TArray<FLinearColor>& OutLinearTextureBuffer);
	/**
	 * Processes camera weights for blending
	 * @param NormalWeightBuffers Normal weight buffers for each camera
	 * @param TextureWidth Texture width
	 * @param TextureHeight Texture height
	 * @param OutTexelWeights Output normalized weights per texel
	 */
	void ProcessCameraWeights(
			const TArray<TArray<float>>& NormalWeightBuffers,
			const TArray<FProjectionSettings>& InCameraSettings,
			int32 TextureWidth,
			int32 TextureHeight,
			TArray<TArray<float>>& OutTexelWeights);

	
	/**
	 * Creates a blended texture from multiple camera projections
	 * @param CameraProjections Array of camera projection textures
	 * @param ProcessedWeights Array of processed weights
	 * @param TextureWidth Texture width
	 * @param TextureHeight Texture height
	 * @return Blended texture data
	 */
	TArray<FLinearColor> CreateBlendedTexture(
		const TArray<TArray<FLinearColor>>& CameraProjections_Linear,
		const TArray<TArray<float>>& ProcessedWeights,
		int32 TextureWidth,
		int32 TextureHeight);
	
	/**
	 * Blends camera weights for visualization
	 * @param NormalWeightBuffers Array of normal weight buffers
	 * @param TextureWidth Texture width
	 * @param TextureHeight Texture height
	 * @param OutBlendedWeightVisualization Output blended visualization
	 */
	void BlendCameraWeights(
		const TArray<TArray<float>>& NormalWeightBuffers,
		int32 TextureWidth, 
		int32 TextureHeight,
		TArray<FColor>& OutBlendedWeightVisualization);
	
	/**
	 * Applies a texture as a material to a mesh
	 * @param StaticMeshActor The actor to apply the material to
	 * @param Texture The texture to use
	 * @param OutOriginalMaterial Original material (output)
	 * @return True if successful
	 */
 bool ApplyTextureAsMaterial(
		TObjectPtr<AStaticMeshActor> StaticMeshActor,
		UTexture2D* Texture,
		UMaterialInterface*& OutOriginalMaterial); 
	
	/**
	 * Resets the projection settings to default values
	 *
	 */
	void ResetProjection();

TArray<FLinearColor> CreateBlendedNormalTexture(
	const TArray<TArray<FLinearColor>>& CameraProjections_Linear,
	const TArray<TArray<float>>& ProcessedWeights,
	int32 TextureWidth,
	int32 TextureHeight);
	/** Takes all valid layers, blends them, and updates the texture/material on the mesh. */
	void ReblendAndUpdateMesh();

	void RecalculateAllWeightsAndReblend(const TArray<FProjectionSettings>& NewCameraSettingsForSlot);

	void SaveFinalTextureAsAsset();

	/** Called by the Save Asset dialog when the user clicks 'Save'. */
	void HandleAssetSave(const FString& ObjectPath);

	/** Called by the Save Asset dialog when the user clicks 'Cancel'. */
	void HandleAssetSaveCancelled();

	void OnTargetUVChannelChanged(int32 NewChannel);

	// void HandleOutputResolutionChanged(int32 W, int32 H);

	 /**
	 * Helper function to perform a topological sort on the workflow graph.
	 * @param WorkflowObject The root JSON object of the loaded workflow.
	 * @param OutSortedNodeIds An array that will be populated with node IDs in the correct execution order.
	 * @return True if the graph is valid (not cyclical) and was sorted, false otherwise.
	 */
	bool TopologicalSort(const TSharedPtr<FJsonObject>& WorkflowObject, TArray<FString>& OutSortedNodeIds);

	// void ExecuteDynamicProjection(TObjectPtr<AStaticMeshActor> StaticMeshActor,
	// 		const TArray<FUnrealNodeInfo>& ParsedNodes,
	// 		 const TMap<FString, TSharedPtr<FJsonValue>>& ControlValues);
	
		/**
	 * Executes a projection using a dynamic ComfyUI workflow.
	 * This function orchestrates the entire process from planning to the final API call.
	 *
	 * @param StaticMeshActor The actor to project onto.
	 * @param WorkflowPath The full path to the selected .json workflow file.
	 * @param ParsedNodes The structured information about nodes tagged with 'Unreal'.
	 * @param ControlValues The current values from the dynamically generated UI widgets.
	 * @param SeedRandomizationStates A map indicating which 'seed' inputs should be randomized.
	 */
	void ExecuteDynamicProjection(
	TObjectPtr<AStaticMeshActor> StaticMeshActor,
	const FString& WorkflowPath,
	const FProjectionSettings& CurrentCameraSettings,
	const TArray<FUnrealNodeInfo>& ParsedNodes,
	const TMap<FString, TSharedPtr<FJsonValue>>& ControlValues,
	const TMap<FString, bool>& SeedRandomizationStates);

	/**
     * Executes the new, fully asynchronous remote workflow using HTTP uploads and API polling.
     */
    void ExecuteRemoteAPIWorkflow(
        const FString& WorkflowPath,
        const TArray<FUnrealNodeInfo>& ParsedNodes,
        const TMap<FString, TSharedPtr<FJsonValue>>& ControlValues,
        const TMap<FString, bool>& SeedRandomizationStates
    );

	
void UploadComfyUIInputs(
    const TArray<FUnrealNodeInfo>& ParsedNodes,
    TFunction<void(const TMap<FString, FString>&)> OnAllUploadsComplete);
	void GenerateOptimalCameraRig();

	void ExecuteAllCameraProjections();


	/** Handles updates to the global output resolution from the UI. */
	void OnOutputResolutionChanged_Handler(int32 NewWidth, int32 NewHeight);

private:


	TObjectPtr<UTexture2D> LastPreviewTexture;

	/** Plugin commands */
	TSharedPtr<class FUICommandList> PluginCommands;
	
	/** Settings window instance */
	TSharedPtr<SWindow> SettingsWindow;
	
	/** Current projection settings */
	FProjectionSettings CurrentSettings;
	
	/** Settings widget instance */
	TSharedPtr<STextureProjectionWindow> SettingsWidget;
	
	TObjectPtr<AStaticMeshActor> SelectedActor;

	UTexture2D* CreateDetailNormalMapPreview(UTexture2D* InSourceNormalTexture, float BlurAmount);

	TObjectPtr<ASceneCapture2D> CaptureActor;

	/** Camera tab management */
	TMap<int32, TArray<FProjectionSettings>> PerSlotCameraSettings;
	int32 ActiveCameraIndex; 					
	int32 NextTabId; 			
	
	TObjectPtr<UTexture2D> ProjectedTexture;

	TMap<int32, TObjectPtr<UTexture2D>> AllProjectedTextures;

	// --- Global Projection Settings ---
	FString Global_OutputPath;
	int32 Global_OutputTextureWidth;
	int32 Global_OutputTextureHeight;
	int32 TargetUVChannel;

	/** Precomputed buffers for optimization */
	TArray<FVector2D> PrecomputedUVBuffer;
	TArray<FVector> PrecomputedWorldPositionBuffer;
	TArray<int32> PrecomputedUVIslandMap;
	TArray<FVector> PrecomputedTangent_World;
    TArray<FVector> PrecomputedBitangent_World;
    TArray<FVector> PrecomputedNormal_World;  // N
	int32 TBNDataForSlotIndex = -1; 

	// In FTextureDiffusion3D class definition
void SaveDebugTexture(const FString& BasePath, const FString& AssetName, int32 Width, int32 Height, const TArray<FLinearColor>& PixelData);
	
	/** REFACTORED: Storage for multi-camera projections */
	TMap<int32, TMap<EProjectionVariant, TArray<TArray<FLinearColor>>>> PerVariantProjectionLayers;
	TMap<int32, TMap<EProjectionVariant, TObjectPtr<UTexture2D>>> PerSlotFinal;
	
	// This remains as it's variant-agnostic geometry data
	TMap<int32, TArray<TArray<float>>> PerSlotWeightLayers;
	
	TMap<int32, bool> SlotHiddenStates;
	TMap<int32, bool> PerSlotLitState;
	TMap<int32, TObjectPtr<UMaterialInterface>> OriginalActorMaterials;
	TMap<int32, FLinearColor> PerSlotBaseColor; 
	bool bHasStartedProjections = false;

	FDateTime CurrentComfyTaskStartTime;
	FString CurrentComfyExpectedOutputPrefix;
	
	FString GetComfyUIBasePath() const; // Ensure it returns FString and is const
	FString LastExportedDepthPath;
	FString LastExportedNormalsPath;

	void SaveWorkflowTemplateIfNotExists(); // Declaration is okay

	void RunComfyUIWorkflow(TSharedPtr<FJsonObject> WorkflowPrompt, const FString& ExpectedOutputPrefix, const FString& FinalPromptText);
	void OnWorkflowRequestComplete(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);
	void ExportDepthToComfyAndRunWorkflow();
	
	// Simplified signature
	UTexture2D* WaitForAndLoadComfyUIOutput(
		const FString& ExpectedFilePrefix,
		float TimeoutSeconds,
		const FDateTime& TaskStartTime,
		FString* OutFoundPath = nullptr
	);

	FTSTicker::FDelegateHandle PollingTickerHandle; 
	FTSTicker::FDelegateHandle DelayedCaptureTickerHandle; 
	
	TUniquePtr<FScopedSlowTask> PollingSlowTask; 

	FDateTime PollingStartTime;
	
	void PerformDelayedCapture(TObjectPtr<AStaticMeshActor> TargetActor, UMaterialInterface* AppliedMaterialIfValid, UTexture2D* SourceTextureIfValid);

	struct FBlockingCaptureState
	{
		bool bIsComplete = false;
		bool bWasSuccessful = false; // You can expand this to hold more data
	};
	TSharedPtr<FBlockingCaptureState> CurrentBlockingCaptureState;
	FTSTicker::FDelegateHandle 	BlockingCaptureTickerHandle;


		// --- Resources ---
	// Parent materials for our MIDs (Load these in StartupModule)
	UMaterialInterface* ParentMaterial_CumulativeDisplay;
	UMaterialInterface* ParentMaterial_WeightMaskDisplay;

	// MIDs for dynamic texture application during export
	TMap<int32, TObjectPtr<UMaterialInstanceDynamic>> PerSlotMIDs;
	UMaterialInstanceDynamic* MID_WeightMaskExport;

	// --- Context for the current camera operation ---
	TObjectPtr<AStaticMeshActor> CurrentProjection_Actor;
	UStaticMeshComponent* CurrentProjection_MeshComponent;
	UStaticMesh* CurrentProjection_StaticMesh;

	FProjectionSettings CurrentProjection_Settings;
	FString CurrentProjection_ComfyUIInputDirectory;
	int32 CurrentProjection_CameraIndex;
	int32 CurrentProjection_TextureWidth;
	int32 CurrentProjection_TextureHeight;
	int32 CurrentProjection_NumTexels;

	// --- Paths to Generated Files ---
	FString Path_ExportedDepthMap;
	FString Path_ExportedTexturedMask;
	FString Path_ExportedWeightMask;

	// --- Data Buffers ---
	TArray<float> CurrentProjection_NormalWeightBuffer;
	TArray<FVector2D> CurrentProjection_ScreenPositionBuffer;
	TArray<bool> CurrentProjection_VisibilityBuffer;

	// --- State Management ---
	UMaterialInterface* MaterialToRestoreAfterWeightMaskVisualization;

	// --- Forward declarations for helper functions we will define in later steps ---
	void InitializeMIDsForCurrentActor_Internal();
	void ExecuteSingleCameraProjection_Part2();
	void HandleProjectionError_Internal(const FString& ErrorMessage);

	TArray<int32> PendingCameraIndices;
	bool bIsBatchMode = false;
	FSimpleDelegate OnSingleProjectionFinished;

	void StartNextCameraAfterDelay();

	struct FClippingVertex
{
	FVector4 Position; // Homogeneous clip-space position (X, Y, Z, W)
	FVector2D UV;
};

FString GenerateDefaultPromptForCamera(const FProjectionSettings& InSettings);

/**
 * @brief Parses the workflow JSON to find the filename_prefix from the first SaveImage node.
 * @param WorkflowObject The JSON object representing the ComfyUI workflow.
 * @return The filename_prefix string if found, otherwise an empty string.
 */
FString FindSaveImagePrefix(const TSharedPtr<FJsonObject>& WorkflowObject);

TObjectPtr<UTexture2D> CreateCompositeTexture() const;


TArray<int32> SaveQueue;

TObjectPtr<UTexture2D> TexturePendingSave;

/** Processes the next camera in the batch projection queue. */
	void ProcessNextInBatch();

/** Cleans up and finalizes the batch projection process. */
void FinishBatchProjection();

void ProcessNextInSaveQueue();

TMap<int32, TObjectPtr<UTexture2D>> PrepareContextualTextureMap(const FProjectionSettings& SettingsForCurrentView, int32 IndexOfCurrentView);

	FLinearColor GetBaseColorForSlot(int32 SlotIndex) const;
	void SetBaseColorForSlot(int32 SlotIndex, FLinearColor NewColor);
void SetActiveVariant(int32 SlotIndex, bool bIsLit);
    bool GetActiveVariantState() const;
TMap<int32, FString> PerSlotReferenceImagePath;


/** Delegate to be called when the ComfyUI image is successfully generated and loaded. */
using FComfyResultsMap = TMap<EProjectionVariant, UTexture2D*>;
DECLARE_DELEGATE_OneParam(FOnComfyAllImagesReady, const FComfyResultsMap&);

	/** Sets up the context for the current projection operation. Returns false if setup fails. */
	bool SetupProjectionContext(
		TObjectPtr<AStaticMeshActor> StaticMeshActor, 
		const FProjectionSettings& CurrentCameraSettings
	);

	/** Executes the projection using a local texture asset. */
	void ExecuteLocalProjection();

	/** Executes the projection by generating inputs and sending a request to ComfyUI. */
	void ExecuteComfyUIProjection(
		const FString& WorkflowPath, 
		const TArray<FUnrealNodeInfo>& ParsedNodes,
		const TMap<FString, TSharedPtr<FJsonValue>>& ControlValues,
		const TMap<FString, bool>& SeedRandomizationStates
	);

	/** Generates all required input images for a ComfyUI workflow and saves them to disk. */
	TMap<FString, FString> PrepareComfyUIInputs(
		const TArray<FUnrealNodeInfo>& ParsedNodes,
		const FString& FileTimestamp,
		const FString& ComfyInputPath
	);

	/** Generates PNG data for the depth map for uploading. */
    bool GenerateDepthDataForUpload(TArray64<uint8>& OutPngData);

    /** Generates PNG data for the base color/mask for uploading. */
    bool GenerateBaseColorDataForUpload(TArray64<uint8>& OutPngData);

    /** Uploads raw PNG image data to a remote ComfyUI /upload/image endpoint. */
    void UploadImageToComfyUI(const TArray64<uint8>& PngData, const FString& OriginalFilename, FOnImageUploadComplete OnComplete);

    FString CurrentPromptId;
    // FTSTicker::FDelegateHandle PollingTickerHandle;

    bool PollHistoryEndpoint(float DeltaTime);

	void DownloadAllFinalImages(const TMap<FString, EProjectionVariant>& FilesToDownload);
TMap<FString, EProjectionVariant> ExpectedOutputs;

	/** Begins the asynchronous polling process for the ComfyUI output image. */
	void BeginPollingForComfyOutputs(
		const TMap<FString, EProjectionVariant>& OutputRequests,
		FOnComfyAllImagesReady OnAllReady);

	FString CurrentBatchLabel;

	/** * The core processing logic for a completed projection.
	 * This is called when an image is ready, either from ComfyUI or a local projection.
	 */
	// void ProcessProjectionResult(UTexture2D* ResultTexture);
	void ProcessProjectionResults(const TMap<EProjectionVariant, UTexture2D*>& ResultsByVariant);
	

	/** If this is the first projection on a material slot, computes and stores its dominant color. */
	void ComputeAndStoreBaseColor(const TArray<FColor>& ProjectedPixels);
};
