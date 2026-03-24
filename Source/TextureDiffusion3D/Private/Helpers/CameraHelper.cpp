#include "Helpers/CameraHelper.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/SceneCapture2D.h"
#include "Editor.h"
#include "TextureProjectionWindow.h" 

void FCameraHelper::PositionCaptureCamera(
    TObjectPtr<ASceneCapture2D> CaptureActor,
    TObjectPtr<AStaticMeshActor> TargetActor, 
    const FProjectionSettings& Settings)
{
    if (!CaptureActor || !TargetActor)
    {
        UE_LOG(LogTemp, Error, TEXT("PositionCaptureCamera: Invalid inputs"));
        return;
    }
    
    // Use the specified camera position
    FVector CameraLocation = Settings.CameraPosition;
    
    UE_LOG(LogTemp, Log, TEXT("PositionCaptureCamera: Setting camera to %s"), 
           *CameraLocation.ToString());
    
    // Get the mesh bounds center
    FBoxSphereBounds Bounds = TargetActor->GetStaticMeshComponent()->Bounds;
    FVector TargetLocation = Bounds.Origin;
    
    // Calculate direction from camera to target
    FVector ViewDirection = (TargetLocation - CameraLocation).GetSafeNormal();
    
    // Position and orient the camera - SET LOCATION FIRST!
    CaptureActor->SetActorLocation(CameraLocation);
    CaptureActor->SetActorRotation(ViewDirection.Rotation());
    
    // Configure FOV
    USceneCaptureComponent2D* CaptureComp = CaptureActor->GetCaptureComponent2D();
    if (CaptureComp)
    {
        CaptureComp->FOVAngle = Settings.FOVAngle;
    }
    
    UE_LOG(LogTemp, Log, TEXT("Camera positioned at: %s, looking at: %s"), 
           *CaptureActor->GetActorLocation().ToString(), 
           *CaptureActor->GetActorForwardVector().ToString());
}

TObjectPtr<ASceneCapture2D> FCameraHelper::SpawnPersistentCamera(
    UWorld* World,
    const FProjectionSettings& Settings)
    {
        if (!World)
        {
            UE_LOG(LogTemp, Warning, TEXT("SpawnPersistentCamera: Invalid world"));
            return nullptr;
        }
        
        // Spawn a SceneCapture2D actor
        TObjectPtr<ASceneCapture2D> NewCaptureActor = World->SpawnActor<ASceneCapture2D>(ASceneCapture2D::StaticClass(), 
            FTransform(Settings.CameraRotation, Settings.CameraPosition));
        
        if (NewCaptureActor)
        {
            // Set the FOV
            if (NewCaptureActor->GetCaptureComponent2D())
            {
                NewCaptureActor->GetCaptureComponent2D()->FOVAngle = Settings.FOVAngle;
            }
            
            // Select the new camera in the editor
            GEditor->SelectNone(true, true);
            GEditor->SelectActor(NewCaptureActor, true, true);
            
            UE_LOG(LogTemp, Log, TEXT("Created persistent scene capture at %s with FOV %.1f"), 
                *Settings.CameraPosition.ToString(), Settings.FOVAngle);
        }
        
        return NewCaptureActor;
    }

TObjectPtr<ASceneCapture2D> FCameraHelper::SetupSceneCaptureComponent(UWorld* World)
{
    if (!World) 
    {
        UE_LOG(LogTemp, Error, TEXT("SetupSceneCaptureComponent: Invalid world"));
        return nullptr;
    }

    // Create a scene capture actor
    TObjectPtr<ASceneCapture2D> CaptureActor = World->SpawnActor<ASceneCapture2D>();
    if (!CaptureActor) 
    {
        UE_LOG(LogTemp, Error, TEXT("SetupSceneCaptureComponent: Failed to spawn capture actor"));
        return nullptr;
    }

    // Configure the capture component
    USceneCaptureComponent2D* CaptureComp = CaptureActor->GetCaptureComponent2D();
    if (!CaptureComp)
    {
        UE_LOG(LogTemp, Error, TEXT("SetupSceneCaptureComponent: Invalid capture component"));
        return nullptr;
    }
    
    // Configure the capture component
    CaptureComp->bCaptureEveryFrame = false;
    CaptureComp->bCaptureOnMovement = false;
    CaptureComp->FOVAngle = 90.0f; // Default FOV
    
    UE_LOG(LogTemp, Log, TEXT("SetupSceneCaptureComponent: Created capture actor"));
    
    return CaptureActor;
}

void FCameraHelper::CleanupSceneCapture(UWorld* World, TObjectPtr<ASceneCapture2D> CaptureActor)
{
    if (CaptureActor && World)
    {
        World->DestroyActor(CaptureActor);
        UE_LOG(LogTemp, Log, TEXT("CleanupSceneCapture: Destroyed capture actor"));
    }
}

FVector2D FCameraHelper::WorldToScreenCoordinates(
        const FVector& WorldPos,
        const FProjectionSettings& Settings, int32 Width, int32 Height)
{
    // Calculate camera vectors from rotation
    FVector CameraForward = Settings.CameraRotation.Vector();
    FVector CameraRight = FVector::CrossProduct(FVector::UpVector, CameraForward).GetSafeNormal();
    FVector CameraUp = FVector::CrossProduct(CameraForward, CameraRight).GetSafeNormal();
    
    // Get vector from camera to point
    FVector CameraToPoint = WorldPos - Settings.CameraPosition;
    
    // Project onto camera axes
    float ForwardDist = FVector::DotProduct(CameraToPoint, CameraForward);
    
    // Skip points behind the camera
    if (ForwardDist <= 0.0f)
    {
        return FVector2D(-1, -1); // Invalid screen coordinate
    }
    
    float RightDist = FVector::DotProduct(CameraToPoint, CameraRight);
    float UpDist = FVector::DotProduct(CameraToPoint, CameraUp);
    
    // Apply perspective division
    float HalfFOVRadians = FMath::DegreesToRadians(Settings.FOVAngle * 0.5f);
    float TanHalfFOV = FMath::Tan(HalfFOVRadians);
    
    // Calculate aspect ratio from texture dimensions
    float AspectRatio = static_cast<float>(Width) / 
                         static_cast<float>(Height);
    
    // Calculate normalized device coordinates (-1 to 1)
    float NdcX = RightDist / (ForwardDist * TanHalfFOV * AspectRatio);
    float NdcY = -UpDist / (ForwardDist * TanHalfFOV); // Negate for correct screen Y orientation
    
    // Convert to screen coordinates (0 to 1)
    float ScreenX = (NdcX * 0.5f) + 0.5f;
    float ScreenY = (NdcY * 0.5f) + 0.5f;
    
    return FVector2D(ScreenX, ScreenY);
}

void FCameraHelper::BuildViewFrustum(
    const FVector& CameraLocation,
    const FVector& CameraForward,
    float FovHalfRadians,
    float AspectRatio,
    TArray<FPlane>& OutFrustumPlanes)
{
    OutFrustumPlanes.Empty(6);
    
    // Calculate frustum vectors
    FVector CameraRight = FVector::CrossProduct(FVector::UpVector, CameraForward).GetSafeNormal();
    FVector CameraUp = FVector::CrossProduct(CameraForward, CameraRight).GetSafeNormal();
    
    float TanFovX = FMath::Tan(FovHalfRadians) * AspectRatio;
    float TanFovY = FMath::Tan(FovHalfRadians);
    
    // Calculate frustum corners (near plane)
    FVector TopLeft = CameraForward + CameraUp * TanFovY - CameraRight * TanFovX;
    FVector TopRight = CameraForward + CameraUp * TanFovY + CameraRight * TanFovX;
    FVector BottomLeft = CameraForward - CameraUp * TanFovY - CameraRight * TanFovX;
    FVector BottomRight = CameraForward - CameraUp * TanFovY + CameraRight * TanFovX;
    
    // Normalize direction vectors
    TopLeft.Normalize();
    TopRight.Normalize();
    BottomLeft.Normalize();
    BottomRight.Normalize();
    
    // Add frustum planes (normals point inward)
    // Near plane
    OutFrustumPlanes.Add(FPlane(CameraLocation + CameraForward * 10.0f, CameraForward));
    
    // Far plane (optional, can be used for far clipping)
    //OutFrustumPlanes.Add(FPlane(CameraLocation + CameraForward * FarClipPlane, -CameraForward));
    
    // Top plane
    FVector TopNormal = FVector::CrossProduct(TopLeft, TopRight).GetSafeNormal();
    OutFrustumPlanes.Add(FPlane(CameraLocation, TopNormal));
    
    // Bottom plane
    FVector BottomNormal = FVector::CrossProduct(BottomRight, BottomLeft).GetSafeNormal();
    OutFrustumPlanes.Add(FPlane(CameraLocation, BottomNormal));
    
    // Left plane
    FVector LeftNormal = FVector::CrossProduct(BottomLeft, TopLeft).GetSafeNormal();
    OutFrustumPlanes.Add(FPlane(CameraLocation, LeftNormal));
    
    // Right plane
    FVector RightNormal = FVector::CrossProduct(TopRight, BottomRight).GetSafeNormal();
    OutFrustumPlanes.Add(FPlane(CameraLocation, RightNormal));
}

// void FCameraHelper::SetupMultiDirectionalCameras(
//     TObjectPtr<AStaticMeshActor> TargetActor,
//     const FProjectionSettings& Settings,
//     TArray<FVector>& OutCameraPositions,
//     TArray<FRotator>& OutCameraRotations,
//     TArray<TObjectPtr<ASceneCapture2D>>& OutCaptureActors)
// {
//     if (!TargetActor || !TargetActor->GetStaticMeshComponent())
//     {
//         UE_LOG(LogTemp, Warning, TEXT("SetupMultiDirectionalCameras: Invalid target actor"));
//         return;
//     }
    
//     // Get the world
//     UWorld* World = TargetActor->GetWorld();
//     if (!World)
//     {
//         UE_LOG(LogTemp, Error, TEXT("SetupMultiDirectionalCameras: Invalid world"));
//         return;
//     }
    
//     // Clear the output arrays
//     OutCameraPositions.Empty(6);
//     OutCameraRotations.Empty(6);
    
//     // Clean up any existing capture actors
//     for (TObjectPtr<ASceneCapture2D> CaptureActor : OutCaptureActors)
//     {
//         if (CaptureActor)
//         {
//             World->DestroyActor(CaptureActor);
//         }
//     }
//     OutCaptureActors.Empty(6);
    
//     // Calculate the center of the mesh in world space
//     FBoxSphereBounds MeshBounds = TargetActor->GetComponentsBoundingBox();
//     FVector MeshCenter = MeshBounds.Origin;
    
//     // Use the camera distance from settings
//     float Distance = Settings.CameraDistance;
    
//     // Define the 6 camera positions along main axes
//     TArray<FVector> DirectionVectors;
//     DirectionVectors.Add(FVector(1, 0, 0));   // +X
//     DirectionVectors.Add(FVector(-1, 0, 0));  // -X
//     DirectionVectors.Add(FVector(0, 1, 0));   // +Y
//     DirectionVectors.Add(FVector(0, -1, 0));  // -Y
//     DirectionVectors.Add(FVector(0, 0, 1));   // +Z
//     DirectionVectors.Add(FVector(0, 0, -1));  // -Z
    
//     // Direction names for debug output
//     TArray<FString> DirectionNames;
//     DirectionNames.Add(TEXT("+X"));
//     DirectionNames.Add(TEXT("-X"));
//     DirectionNames.Add(TEXT("+Y"));
//     DirectionNames.Add(TEXT("-Y"));
//     DirectionNames.Add(TEXT("+Z"));
//     DirectionNames.Add(TEXT("-Z"));
    
//     // Create six cameras positioned along the main axes
//     for (int32 i = 0; i < DirectionVectors.Num(); i++)
//     {
//         // Calculate position
//         FVector CameraPosition = MeshCenter + DirectionVectors[i] * Distance;
//         OutCameraPositions.Add(CameraPosition);
        
//         // Calculate proper look-at rotation to ensure the camera is pointing at mesh center
//         FVector LookDirection = (MeshCenter - CameraPosition).GetSafeNormal();
//         FRotator CameraRotation = LookDirection.Rotation();
//         OutCameraRotations.Add(CameraRotation);
        
//         // Spawn a scene capture actor at this position
//         TObjectPtr<ASceneCapture2D> CaptureActor = World->SpawnActor<ASceneCapture2D>(
//             ASceneCapture2D::StaticClass(),
//             CameraPosition,
//             CameraRotation
//         );
        
//         if (CaptureActor)
//         {
//             // Configure the capture component
//             USceneCaptureComponent2D* CaptureComp = CaptureActor->GetCaptureComponent2D();
//             if (CaptureComp)
//             {
//                 // Set FOV and other camera properties
//                 CaptureComp->FOVAngle = Settings.FOVAngle;
//                 CaptureComp->bCaptureEveryFrame = false;
//                 CaptureComp->bCaptureOnMovement = false;
                
//                 // Set capture to only show the target mesh
//                 CaptureComp->ShowOnlyActors.Add(TargetActor);
                
//                 // Take a capture
//                 CaptureComp->CaptureScene();
//             }
            
//             // Add this capture actor to the output array
//             OutCaptureActors.Add(CaptureActor);
            
//             // Name the actor for easier identification in the editor
//             FString CameraName = FString::Printf(TEXT("Camera_%s_%s"), 
//                 *TargetActor->GetName(), *DirectionNames[i]);
//             // FName CameraActorName(*CameraName);
//             // CaptureActor->Rename(CameraActorName);
            
//             // UE_LOG(LogTemp, Log, TEXT("Created camera %d: Position=%s"), 
//             // i, *CameraPosition.ToString());
//         }
//         else
//         {
//             UE_LOG(LogTemp, Error, TEXT("Failed to spawn camera actor for direction %s"), *DirectionNames[i]);
//         }
//     }
// }

// void FCameraHelper::SetupMultiDirectionalCameras(
//     TObjectPtr<AStaticMeshActor> TargetActor,
//     const FProjectionSettings& Settings,
//     TArray<FVector>& OutCameraPositions,
//     TArray<FRotator>& OutCameraRotations)
// {
//     // Temporary array that will be discarded
//     TArray<TObjectPtr<ASceneCapture2D>> TempCaptureActors;
//     SetupMultiDirectionalCameras(TargetActor, Settings, OutCameraPositions, OutCameraRotations, TempCaptureActors);
// }