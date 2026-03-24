// TextureUtils.h
#pragma once

#include "CoreMinimal.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/SceneCapture2D.h" 
#include "ImageUtils.h"
#include "IImageWrapper.h"
#include "Helpers/TextureProjectionTypes.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
struct FProjectionSettings;



class FTextureUtils
{
public:

    struct FSimpleFeatherParams
    {
        int32 RadiusTexels = 3;        // 2–4 works well
        bool  bUse8Connectivity = true;
        uint8 AlphaFloor = 0;          // 0..255 (e.g. 8 to avoid pinholes)
    };
    // --- YOUR EXISTING PUBLIC FUNCTIONS ---
    static void InitializeBuffers(int32 Width, int32 Height, TArray<bool>& OutVisibilityBuffer, TArray<FVector>& OutWorldPositionBuffer, TArray<FVector2D>& OutScreenPositionBuffer, TArray<FVector2D>& OutUVBuffer, TArray<float>& OutDepthBuffer);
    static UTexture2D* CreateTextureFromPixelData(int32 Width, int32 Height, const TArray<FColor>& PixelData);
    static UTexture2D* CreateTextureFromsRGBPixelData(
    int32 Width, 
    int32 Height, 
    const TArray<FColor>& sRGB_PixelData);
    /** * Bilinear sampler for 16-bit (PF_FloatRGBA) HDR textures.
     * Reads 16-bit FFloat16Color data and returns interpolated 32-bit FLinearColor.
     */
    static FLinearColor SampleTextureBilinear_HDR(
        const TArray<FFloat16Color>& SourcePixels, 
        int32 SourceWidth, int32 SourceHeight, 
        const FVector2D& UV
    );
    // Creates a UTexture2D from a buffer of linear FLinearColor data.
    // The resulting texture will have SRGB = false, suitable for data textures like normal maps.
    static UTexture2D* CreateTextureFromLinearPixelData(int32 Width, int32 Height, const TArray<FLinearColor>& LinearPixels);
    static bool SaveVisibilityBufferAsTexture(const TArray<bool>& VisibilityBuffer, int32 TextureWidth, int32 TextureHeight, const FString& PackagePath, const FString& AssetName);
    static bool SaveTextureAsAsset(UTexture2D* Texture, const FString& PackagePath, const FString& AssetName);
    static bool SaveTextureFromVisibleTexels(const TArray<bool>& VisibilityBuffer, const TArray<FVector2D>& UVBuffer, const TArray<FColor>& NormalColorBuffer, int32 TextureWidth, int32 TextureHeight, const FString& PackagePath, const FString& AssetName);
    static bool ProjectTextureOntoMesh(UTexture2D* SourceTexture, const TArray<bool>& VisibilityBuffer, const TArray<FVector2D>& ScreenPositionBuffer, int32 TextureWidth, int32 TextureHeight, const FString& PackagePath, const FString& AssetName);
    static FColor SampleSourceTexture(const TArray<uint8, FDefaultAllocator64>& SourceData, int32 X, int32 Y, int32 SourceWidth, ETextureSourceFormat SourceFormat);
    static bool ProjectTextureOntoMesh_DebugCoordinates(UTexture2D* SourceTexture, const TArray<bool>& VisibilityBuffer, const TArray<FVector2D>& ScreenPositionBuffer, int32 TextureWidth, int32 TextureHeight, const FString& PackagePath, const FString& AssetName);
    static void SaveUVIslandMapAsTexture(const TArray<int32>& UVIslandIDMap, int32 TextureWidth, int32 TextureHeight, const FString& PackagePath, const FString& AssetName);
    static bool ExtendTextureMargins(int32 TextureWidth, int32 TextureHeight, int32 Radius, FColor* TextureData, const TArray<int32>& UVIslandIDMap);
    static bool ExtendTextureMarginsLinear(
    int32 TextureWidth,
    int32 TextureHeight,
    int32 Radius,
    TArray<FLinearColor>& TextureData, // Takes TArray by reference
    const TArray<int32>& UVIslandIDMap);

    /**
 * Extends gutter/margin pixels for a final blended color texture.
 * This should be called ONCE at the very end of the pipeline, after all blending is complete.
 * Uses alpha channel to determine which pixels are "filled" vs "empty".
 * 
 * @param PixelData     The pixel buffer to modify in-place
 * @param Width         Texture width
 * @param Height        Texture height  
 * @param GutterRadius  Number of pixels to extend outward (default 2)
 * @param AlphaThreshold Minimum alpha to consider a pixel "filled" (default 0.01)
 */
static bool ExtendGuttersFinal(
    int32 TextureWidth,
    int32 TextureHeight,
    int32 Radius,
    TArray<FLinearColor>& TextureData,
    const TArray<int32>& UVIslandIDMap);

/**
 * Extends gutter/margin pixels for a final blended normal map.
 * Fills empty pixels (alpha == 0) that are adjacent to filled pixels (alpha > 0).
 * Uses proper vector averaging and renormalization instead of color averaging.
 * Should be called ONCE at the end of the pipeline after all blending.
 * 
 * @param NormalData    The normal map pixel buffer to modify in-place (packed as 0-1 FLinearColor)
 * @param Width         Texture width
 * @param Height        Texture height  
 * @param GutterRadius  Number of pixels to extend outward (default 2)
 */
static void ExtendGuttersFinalNormal(
    TArray<FLinearColor>& NormalData,
    int32 Width,
    int32 Height,
    int32 GutterRadius = 2,
    bool bDebugSave = false,
    const FString& DebugBaseName = FString());

    static bool ExtendTextureMarginsNormal(
    int32 TextureWidth,
    int32 TextureHeight,
    int32 Radius,
    TArray<FLinearColor>& TextureData,
    const TArray<bool>& VisibilityBuffer,
    const TArray<int32>& UVIslandIDMap);
    static bool SaveColorBufferAsTexture(const TArray<FColor>& ColorBuffer, int32 TextureWidth, int32 TextureHeight, const FString& PackagePath, const FString& AssetName);
    static UTexture2D* ConvertDepthBufferToTexture(const TArray<float>& DepthBuffer, int32 TextureWidth, int32 TextureHeight, const FString& OutputPath, const FString& OutputName, bool bInvertDepth = false);
    static UTexture2D* ConvertWeightBuffersToTexture(const TArray<TArray<float>>& NormalWeightBuffers, int32 TextureWidth, int32 TextureHeight, const FString& OutputPath, const FString& OutputName, bool bSaveAsset);
    static bool ExportTextureToFile(UTexture2D* Texture, const FString& FilePath, const FString& FileName, bool bOverwriteExisting, EImageFormat Format);
    static UTexture2D* CreateTestColorTexture(int32 Width, int32 Height);
    static bool NormalizeAndExportRenderTarget(UWorld* World, UTextureRenderTarget2D* SourceRT, const FString& ExportDirectory, const FString& BaseFileName, bool bInvertDepth = false);
    static bool ExportRenderTarget(UWorld* World, UTextureRenderTarget2D* SourceRT, const FString& ExportDirectory, const FString& BaseFileName, bool bInvertColors);
    static UTexture2D* CreateOrUpdateTexture(UTexture2D* InTexture, int32 SrcWidth, int32 SrcHeight, const TArray<FColor>& SrcData);
    static UTexture2D* CreateTextureFromRenderTarget(UTextureRenderTarget2D* RenderTarget);
    // --- END OF YOUR EXISTING PUBLIC FUNCTIONS ---

    /**
    * Blends two images together using a Laplacian pyramid for seamless results.
    */
    static void BlendWithLaplacianPyramid(
        const TArray<FColor>& BasePixels,
        const TArray<FColor>& TopPixels,
        const TArray<FColor>& MaskPixels,
        int32 Width,
        int32 Height,
        TArray<FColor>& OutBlendedPixels,
        int32 Levels = 4
    );
/**
    * Decompresses any UTexture2D into a raw TArray<FColor> buffer.
    * This is useful for getting CPU access to texture data that might be in a compressed format (like DXT).
    * @param InTexture The source texture to decompress.
    * @param OutPixels The output array that will be filled with the BGRA8 pixel data.
    * @param OutWidth The width of the decompressed texture.
    * @param OutHeight The height of the decompressed texture.
    * @return True if the operation was successful, false otherwise.
    */
    static bool DecompressTexture(UTexture2D* InTexture, TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight);


    static void FeatherProjectionEdges_Simple(
		TArray<FColor>& InOutLayerSrgb,                // modifies .A only
		int32 Width, int32 Height,
		const TArray<int32>* UVIslandIDs,              // nullptr => ignore islands
		const FSimpleFeatherParams& Params);
    
    static bool ExportTexture2DToPNG(UTexture2D* Texture, const FString& FilePath);

    /** Converts an array of sRGB FColors to an array of Linear FLinearColors. */
	static TArray<FLinearColor> sRGBToLinear(const TArray<FColor>& sRGB_Colors);

	/** Converts an array of Linear FLinearColors to an array of sRGB FColors. */
	static TArray<FColor> LinearTo_sRGB(const TArray<FLinearColor>& Linear_Colors);

   /**
     * Matches the color profile of a source image to a reference image.
     * This is a self-contained function that operates directly on pixel buffers.
     * @param SourcePixels The raw pixel data of the image to modify.
     * @param SourceW Width of the source image.
     * @param SourceH Height of the source image.
     * @param ReferencePixels The raw pixel data of the target color profile.
     * @param RefW Width of the reference image.
     * @param RefH Height of the reference image.
     * @param OutMatchedPixels [OUT] The resulting modified pixel data.
     * @return True if the operation was successful, false otherwise.
     */
    static bool MatchTextureColor(
        const TArray<FColor>& SourcePixels, int32 SourceW, int32 SourceH,
        const TArray<FColor>& ReferencePixels, int32 RefW, int32 RefH,
        TArray<FColor>& OutMatchedPixels
    );


    static FLinearColor ComputeBaseColor_WeightedMedianLab(
        UTexture2D* Texture,
        float AlphaThreshold = 0.10f,      // e.g. 0.10f (ignore very low-coverage haze)
        float TrimPercent = 0.05f,         // e.g. 0.05f (trim 5% low/high tails by weight)
        int32 TargetSampleCount = 16000     // e.g. 16000 (auto stride toward this sample budget)
    );

    static bool ComposeBaseLaid(
        const TArray<FColor>& PerspectiveSrgbRGBA,
        const TArray<FColor>& SilhouetteSrgbRGBA,
        int32 Width,
        int32 Height,
        TArray<FColor>& OutSrgbRGBA,
        bool  bTransparentBackground = true,
        float AlphaFloor = 0.0f
    );

    static FColor SampleTextureBilinear(const TArray<FColor>& SourcePixels, int32 SourceWidth, int32 SourceHeight, const FVector2D& UV);

    // Overload for High Precision FLinearColor arrays
    // static FLinearColor SampleTextureBilinearLinear(const TArray<FLinearColor>& SourcePixels, int32 SourceWidth, int32 SourceHeight, const FVector2D& UV);
    static FLinearColor SampleTextureBilinear(const TArray<FLinearColor>& SourcePixels, int32 SourceWidth, int32 SourceHeight, const FVector2D& UV);
    static void Linear8ToSrgb8(const TArray<FColor>& InLinear, TArray<FColor>& OutSrgb);

     /**
    * "Flattens" a world/view-space normal map to extract only high-frequency details.
    * It does this by blurring the original map to get the base shape, then re-orienting the
    * original normals relative to the base shape's normals.
    * @param InOutNormalMapPixels The source normal map data. This will be modified in-place.
    * @param Width The width of the map.
    * @param Height The height of the map.
    * @param BlurRadius The pixel radius for the Gaussian blur used to find the base shape. Larger values remove more macro detail.
    */
    static void ExtractNormalMapDetails(TArray<FColor>& InOutNormalMapPixels, int32 Width, int32 Height, float BlurRadius);
private:
    // Helper structs to manage image data and dimensions together
 struct FPyramidLayer
    {
        TArray<FColor> Pixels;
        int32 Width;
        int32 Height;
    };

    struct FLinearPyramidLayer
    {
        TArray<FLinearColor> Pixels;
        int32 Width;
        int32 Height;
    };


    // Helper functions for the pyramid blending process
    static TArray<FPyramidLayer> BuildGaussianPyramid(const TArray<FColor>& Img, int32 Width, int32 Height, int32 Levels);
    static TArray<FLinearPyramidLayer> BuildLaplacianPyramid(const TArray<FPyramidLayer>& GaussianPyramid);
    static void CollapsePyramid(const TArray<FLinearPyramidLayer>& Pyramid, TArray<FColor>& OutImage);

    
    // Low-level image operations
    static FPyramidLayer Downsample(const FPyramidLayer& InLayer);
    static FPyramidLayer Upsample(const FPyramidLayer& InLayer);
    static TArray<FLinearColor> Subtract(const FPyramidLayer& A, const FPyramidLayer& B);
    static TArray<FColor> Add(const FPyramidLayer& A, const FLinearPyramidLayer& B);
    static TArray<FColor> BlurImage(const TArray<FColor>& Img, int32 Width, int32 Height);
    static FLinearColor GetClampedPixel(const TArray<FColor>& Img, int32 x, int32 y, int32 Width, int32 Height);
};
