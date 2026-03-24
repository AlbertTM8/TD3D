// CameraHelper.h
#pragma once

#include "CoreMinimal.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/SceneCapture2D.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Helpers/TextureProjectionTypes.h"

/**
 * Helper class for camera-related functionality in texture projection
 */
class FCameraHelper
{
public:
    /**
     * Position a capture camera to focus on a target actor
     * @param CaptureActor The scene capture actor to position
     * @param TargetActor The actor to focus on
     * @param Settings The projection settings
     */
    static void PositionCaptureCamera(
        TObjectPtr<ASceneCapture2D> CaptureActor,
        TObjectPtr<AStaticMeshActor> TargetActor, 
        const FProjectionSettings& Settings);
    
    /**
     * Spawns a persistent camera in the world
     * @param World The world to spawn the camera in
     * @param Location The location for the camera
     * @param Rotation The rotation for the camera
     * @param FOV Field of view in degrees
     * @return The spawned camera actor
     */
    static TObjectPtr<ASceneCapture2D> SpawnPersistentCamera(
        UWorld* World,
        const FProjectionSettings& Settings);
    
    /**
     * Creates and configures a scene capture component
     * @param World The world to spawn the capture in
     * @return The created scene capture actor
     */
    static TObjectPtr<ASceneCapture2D> SetupSceneCaptureComponent(UWorld* World);
    
    /**
     * Destroys a scene capture actor
     * @param World The world the capture is in
     * @param CaptureActor The capture actor to destroy
     */
    static void CleanupSceneCapture(UWorld* World, TObjectPtr<ASceneCapture2D> CaptureActor);
    
    /**
     * Projects a world position to screen coordinates
     * @param WorldPos The world position to project
     * @param CameraLocation The camera location
     * @param CameraForward The camera forward vector
     * @param CameraRight The camera right vector
     * @param CameraUp The camera up vector
     * @param FOVDegrees The field of view in degrees
     * @param AspectRatio The aspect ratio (width/height)
     * @return Screen coordinates in [0,1] range
     */
    static FVector2D WorldToScreenCoordinates(
        const FVector& WorldPos,
        const FProjectionSettings& Settings, int32 Width, int32 Height);
    
    /**
     * Builds a view frustum from camera parameters
     * @param CameraLocation The camera location
     * @param CameraForward The camera forward vector
     * @param FovHalfRadians Half the field of view in radians
     * @param AspectRatio The aspect ratio (width/height)
     * @param OutFrustumPlanes The resulting frustum planes
     */
    static void BuildViewFrustum(
        const FVector& CameraLocation,
        const FVector& CameraForward,
        float FovHalfRadians,
        float AspectRatio,
        TArray<FPlane>& OutFrustumPlanes);

    static void SetupMultiDirectionalCameras(
        TObjectPtr<AStaticMeshActor> TargetActor,
        const FProjectionSettings& Settings,
        TArray<FVector>& OutCameraPositions,
        TArray<FRotator>& OutCameraRotations,
        TArray<TObjectPtr<ASceneCapture2D>>& OutCaptureActors);

    static void SetupMultiDirectionalCameras(
        TObjectPtr<AStaticMeshActor> TargetActor,
        const FProjectionSettings& Settings,
        TArray<FVector>& OutCameraPositions,
        TArray<FRotator>& OutCameraRotations);
};