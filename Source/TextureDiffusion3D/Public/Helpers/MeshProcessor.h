// MeshProcessor.h
#pragma once

#include "CoreMinimal.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Helpers/TextureProjectionTypes.h"

// Forward declarations
class AStaticMeshActor;
class UTexture2D;

// Holds per-vertex data for interpolation during clipping.
struct FClippingVertex
{
	FVector4 Position;      // Homogeneous clip-space position (X, Y, Z, W)
	FVector WorldPosition;  // World-space position
	FVector2D UV;
    FVector WorldNormal; 
    FLinearColor Color;
};

struct FScreenToTangentOptions
{
    // If your screen/view normal only stores XY (RG) and expects Z to be reconstructed:
    bool bSourceIsXYOnly = false;

    // UE uses DirectX convention (green up). Flip if your source is OpenGL-style.
    bool bInvertGreenOnEncode = false;

    // If true, copy input alpha into output alpha; else 255.
    bool bPreserveInputAlpha = false;
};
/**
 * Helper class for processing mesh data for texture projection
 */
class FMeshProcessor
{
public:
    /**
     * Process a static mesh and apply backface culling
     * @param StaticMesh The mesh to process
     * @param LocalToWorld Transform from local to world space
     * @param CameraPos The camera position for culling
     * @param TextureWidth Width of the output texture
     * @param TextureHeight Height of the output texture
     * @param VisibilityBuffer Output buffer for visibility flags
     * @param WorldPositionBuffer Output buffer for world positions
     * @param ScreenPositionBuffer Output buffer for screen positions
     * @param UVBuffer Output buffer for UV coordinates
     * @return Number of visible triangles
     */
    static int32 RemoveOccludedTexels(
        UStaticMesh* StaticMesh,
        const FTransform& LocalToWorld,
        const FProjectionSettings& Settings, int32 OutputWidth, int32 OutputHeight,
        TArray<bool>& VisibilityBuffer,
        TArray<FVector>& WorldPositionBuffer,
        TArray<FVector2D>& ScreenPositionBuffer,
        TArray<FVector2D>& UVBuffer,
        TArray<float>& DepthBuffer,
        TArray<FColor>& NormalColorBuffer,
        int32 UVChannel);

    // Initialize texel buffers for a mesh
    static int32 InitializeTexelBuffers(
        UStaticMesh* StaticMesh,
        const FTransform& LocalToWorld,
        const FProjectionSettings& Settings, int32 OutputWidth, int32 OutputHeight,
        TArray<bool>& VisibilityBuffer,
        TArray<FVector>& WorldPositionBuffer,
        TArray<FVector2D>& ScreenPositionBuffer,
        TArray<FVector2D>& UVBuffer,
        TArray<float>& DepthBuffer,
     int32 UVChannel
    );

    /**
     * Removes backfacing triangles using vertex normals, similar to the approach in GenerateFaceNormalColorTexture
     * 
     * @param StaticMesh - The static mesh to process
     * @param LocalToWorld - Transform from local to world space
     * @param Settings - Projection settings (camera position, etc.)
     * @param TextureWidth - Width of the output texture
     * @param TextureHeight - Height of the output texture
     * @param VisibilityBuffer - Buffer to mark visibility (will be updated - false for backfacing)
     * @param NormalColorBuffer - Optional buffer to store normal colors for visualization
     * @return Number of texels processed
     */
    static int32 RemoveBackfaces(
        UStaticMesh* StaticMesh,
        const FTransform& LocalToWorld,
        const FProjectionSettings& Settings,
        int32 TextureWidth,
        int32 TextureHeight,
        TArray<bool>& VisibilityBuffer,
        int32 UVChannel,
        TArray<FColor>* NormalColorBuffer = nullptr);
    
    static int32 PerformRayCastOcclusion(
        UStaticMesh* StaticMesh,
        const FTransform& LocalToWorld,
        const FProjectionSettings& Settings,
        TArray<bool>& VisibilityBuffer,
        TArray<FVector>& WorldPositionBuffer,
        TArray<float>& DepthBuffer,
        TArray<float>& NormalWeightBuffer,
        const TSet<int32>& HiddenSlotIndices);

    static bool RayIntersectsTriangle(
        const FVector& RayOrigin, 
        const FVector& RayDirection,
        const FVector& V0, 
        const FVector& V1, 
        const FVector& V2,
        float& OutDistance,
        FVector& OutIntersectionPoint);

    static bool CompareDepthsForVisibility(
        const TArray<float>& CalculatedDepthBuffer,    // From InitializeTexelBuffers
        const TArray<float>& CapturedDepthBuffer,      // From scene capture
        const TArray<FVector2D>& ScreenPositionBuffer, // Mapping from UV to screen
        int32 TextureWidth,
        int32 TextureHeight,
        TArray<bool>& OutVisibilityBuffer,             // Result: which texels are visible
        float DepthTolerance = 0.01f);


    static FMatrix GetManualViewProjectionMatrix(const FProjectionSettings& Settings, int32 TextureWidth, int32 TextureHeight);
            

    static FMatrix BuildViewProjectionMatrixManual(
        const FVector& CameraPosition,
        const FRotator& CameraRotation,
        float FOVDegrees,
        float AspectRatio,
        float NearPlane,
        float FarPlane
    );
    /**
     * Projects UVs onto a static mesh using perspective projection
     * 
     * @param StaticMesh - The static mesh to modify
     * @param LocalToWorld - The transform of the mesh in world space
     * @param Settings - Projection settings including camera position, rotation, FOV, and output dimensions
     * @return True if the projection was successful
     */
    static bool ProjectUVsInPerspective(
        UStaticMesh* StaticMesh,
        const FTransform& LocalToWorld,
        const FProjectionSettings& Settings, int32 OutputWidth, int32 OutputHeight);

    /**
     * Identifies triangles that are facing toward the camera and collects their vertices
     * 
     * @param StaticMesh - The static mesh to analyze
     * @param LocalToWorld - The transform of the mesh in world space
     * @param CameraPosition - The position of the camera in world space
     * @param OutTriangleFrontFacing - Output array indicating which triangles are front-facing
     * @param OutFrontFacingVertices - Output set containing vertices that are part of front-facing triangles
     */
    static void IdentifyFrontFacingTriangles(
        UStaticMesh* StaticMesh,
        const FTransform& LocalToWorld,
        const FProjectionSettings& Settings, 
        const FVector& CameraPosition,
        TArray<bool>& OutTriangleFrontFacing,
        TSet<uint32>& OutFrontFacingVertices);

    static int32 CalculateNormalWeights(
        UStaticMesh* StaticMesh,
        const FTransform& LocalToWorld,
        const FVector& CameraPosition,
        const FProjectionSettings& Settings,
        int32 TextureWidth,
        int32 TextureHeight,
        const TArray<FVector>& WorldPositionBuffer,  
        const TArray<FVector2D>& ScreenPositionBuffer,
        TArray<bool>& VisibilityBuffer,   
        TArray<float>& NormalWeightBuffer,
        TArray<FColor>* NormalColorBuffer,
         int32 UVChannel,
    const TSet<int32>& HiddenSlotIndices);

    static int32 InitializeStaticTexelBuffers(
        UStaticMesh* StaticMesh,
        const FTransform& LocalToWorld, 
        const FProjectionSettings& Settings,
        int32 TextureWidth,
        int32 TextureHeight,
        TArray<FVector2D>& OutUVBuffer,
        TArray<FVector>& OutWorldPositionBuffer,
        int32 UVChannel);

    static void InitializeDynamicTexelBuffers(
        UStaticMesh* StaticMesh,
        const FTransform& LocalToWorld,
        const FProjectionSettings& Settings,
        int32 TextureWidth,
        int32 TextureHeight,
        int32 UVChannel,
        const TSet<int32>& HiddenSlotIndices,
        TArray<FVector>&   OutWorldPositionBuffer,  
        TArray<FVector2D>& OutScreenPositionBuffer, 
        TArray<float>&     OutDepthBuffer            
    );

    static bool GenerateRasterizedMaskView(
        TArray<FColor>& OutImageBuffer,
        AStaticMeshActor* Actor,
        const FProjectionSettings& CameraSettings,
        const TMap<int32, TObjectPtr<UTexture2D>>& AllSlotTextures,
        int32 OutputWidth,
        int32 OutputHeight,
        int32 UVChannel);

    /**
     * Generates a 2D rasterized image of a static mesh from a specific camera's viewpoint using a software rasterizer.
     * This function performs a full pipeline including clipping and perspective-correct texture mapping.
     */
    static bool GenerateRasterizedView(
        TArray<FLinearColor>& OutImageBuffer,
        AStaticMeshActor* Actor,
        const FProjectionSettings& CameraSettings,
        const TMap<int32, TObjectPtr<UTexture2D>>& AllSlotTextures,
        int32 OutputWidth,
        int32 OutputHeight,
        int32 UVChannel);
    /**
    * Generates a perspective view of a mesh, combining an existing UV-space texture for RGB
    * and a pre-calculated UV-space weight map for Alpha.
    *
    * @param OutImageBuffer The resulting RGBA pixel buffer for the perspective view.
    * @param Actor The static mesh actor to render.
    * @param CameraSettings The camera settings defining the perspective.
    * @param ColorTexture The UTexture2D to sample for RGB color data (e.g., the cumulative projected texture).
    * @param MaskData A TArray of floats (0-1) in UV space to use for the alpha channel.
    * @param OutputWidth The width of the output image.
    * @param OutputHeight The height of the output image.
    * @param UVChannel The UV channel to use for sampling both color and mask data.
    * @return True if the view was generated successfully, false otherwise.
    */
    static bool GeneratePerspectiveRGBAView(
    TArray<FColor>& OutImageBuffer,
    AStaticMeshActor* Actor,
    const FProjectionSettings& CameraSettings,
    UTexture2D* ColorTexture,
    const TArray<float>& UVSpaceWeightData,
    int32 OutputWidth,
    int32 OutputHeight,
    int32 UVChannel);

    static TArray<int32> GenerateUVIslandIDMap(
    UStaticMesh* StaticMesh, 
    int32 UVChannel, 
    int32 TextureWidth, 
    int32 TextureHeight,
    int32 MaterialSlotIndex);

    // static bool GenerateSilhouetteBaseColorImage(
    //     TArray<FColor>& OutImage,                 // sRGB8 output: mesh=base color, bg=black
    //     AStaticMeshActor* Actor,
    //     const FProjectionSettings& CameraSettings,
    //     int32 OutputWidth,
    //     int32 OutputHeight,
    //     int32 UVChannel,
    //     const FLinearColor& BaseLinear,           // base color in LINEAR space
    //     bool bTransparentBackground = true        // if true: bg alpha = 0, mesh alpha = 255
    // );


    static bool GenerateSilhouetteBaseColorImage(
        TArray<FColor>& OutImage,                             // sRGB8 output
        AStaticMeshActor* Actor,
        const FProjectionSettings& Camera,
        int32 Width,
        int32 Height,
        int32 UVChannel,
        const TMap<int32, FLinearColor>& SlotBaseColors,
        TMap<int32, bool> SlotHidden,      // slot -> LINEAR base color
        bool bTransparentBackground = true
    // skip these slots entirely
    );

        // Build a per-texel TBN from your per-texel world positions using finite differences.
    static void BuildPerTexelTBN_FromWorldBuffer(
        const TArray<FVector>& WorldPos, int32 Width, int32 Height,
        TArray<FVector>& OutT, TArray<FVector>& OutB, TArray<FVector>& OutN);

    // Convert a per-texel VIEW/SCREEN-space normal field (already projected into UV layout)
    // into a tangent-space normal map using the per-texel TBN and camera rotation.
    static void ConvertProjectedNormals_ViewToTangent(
        const TArray<FColor>& InViewNormalsRGBA,            // size = Width*Height
        const TArray<FVector>& WorldPosPerTexel,            // size = Width*Height
        int32 Width, int32 Height,
        const FRotator& ViewRotation,                       // camera rotation that defined "view space"
        const FScreenToTangentOptions& Options,
        TArray<FColor>& OutTangentNormalsRGBA);             // size = Width*Height

    static void BuildPerTexelTBN_FromMesh(
        UStaticMesh* StaticMesh,
        const FTransform& LocalToWorld,
        int32 UVChannel,
        int32 TargetMaterialSlotIndex,
        int32 W, int32 H,
        TArray<FVector>& OutTWorld,
        TArray<FVector>& OutBWorld,
        TArray<FVector>& OutNWorld);
        

static bool GenerateRasterizedViewSpaceNormals(
    TArray<FColor>& OutImageBuffer,
    AStaticMeshActor* Actor,
    const FProjectionSettings& CameraSettings,
    int32 OutputWidth,
    int32 OutputHeight,
    const TSet<int32>& HiddenSlotIndices);


    static bool GenerateCannyView(
        TArray<FColor>& OutImageBuffer,
        AStaticMeshActor* Actor,
        const FProjectionSettings& CameraSettings,
        int32 OutputWidth,
        int32 OutputHeight,
        const TSet<int32>& HiddenSlotIndices,
        float AngleThresholdDegrees);

    static FColor SampleTextureBilinearRaw(
        const FColor* Src, int32 W, int32 H, const FVector2D& UV01);

    private:
    // Helpers (exposed public later if you want)
    static FVector DecodeViewNormalFromRGB(const FColor& C, bool bXYOnly);
    static FColor  EncodeTangentNormalToRGB(const FVector& N_tan, bool bInvertGreen, uint8 Alpha);
};