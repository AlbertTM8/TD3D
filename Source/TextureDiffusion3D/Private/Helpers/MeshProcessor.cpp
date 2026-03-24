// MeshProcessor.cpp
#include "Helpers/MeshProcessor.h"
#include "Helpers/MathUtils.h"
#include "DrawDebugHelpers.h"
#include "Helpers/TextureUtils.h"
#include "Kismet/GameplayStatics.h"

#include "Math/UnrealMathUtility.h"
#include "Math/Matrix.h"
#include "Math/Rotator.h"
#include "Math/Vector.h"


namespace 
{
    TArray<FClippingVertex> ClipPolygonAgainstPlane(const TArray<FClippingVertex>& InVertices, int32 Component, float Factor)
{
      TArray<FClippingVertex> OutVertices;
      if (InVertices.Num() == 0) return OutVertices;
      FClippingVertex PreviousVertex = InVertices.Last();
      for (const FClippingVertex& CurrentVertex : InVertices)
      {
            float PrevDot = PreviousVertex.Position[Component] * Factor + PreviousVertex.Position.W;
            float CurrDot = CurrentVertex.Position[Component] * Factor + CurrentVertex.Position.W;
            bool bPrevInside = PrevDot >= 0;
            bool bCurrInside = CurrDot >= 0;
            if (bPrevInside != bCurrInside)
            {
                  float t = PrevDot / (PrevDot - CurrDot);
                  FClippingVertex IntersectedVertex;
                  IntersectedVertex.Position = FMath::Lerp(PreviousVertex.Position, CurrentVertex.Position, t);
                  IntersectedVertex.UV = FMath::Lerp(PreviousVertex.UV, CurrentVertex.UV, t);
                  IntersectedVertex.WorldPosition = FMath::Lerp(PreviousVertex.WorldPosition, CurrentVertex.WorldPosition, t);
                  IntersectedVertex.Color = FMath::Lerp(PreviousVertex.Color, CurrentVertex.Color, t);             // <<<--- Interpolate Color
                  IntersectedVertex.WorldNormal = FMath::Lerp(PreviousVertex.WorldNormal, CurrentVertex.WorldNormal, t); // <<<--- Interpolate WorldNormal
                  OutVertices.Add(IntersectedVertex);
            }
            if (bCurrInside)
            {
                  OutVertices.Add(CurrentVertex);
            }
            PreviousVertex = CurrentVertex;
      }
      return OutVertices;
}
}

namespace
{
    // A struct to represent a ray for intersection tests.
    // Renamed to FBVHRay to avoid conflicts with UE's native FRay.
    struct FBVHRay
    {
        FVector Origin;
        FVector Direction;
        float MaxDistance;
    };

    // A struct representing an axis-aligned bounding box (AABB).
    struct FAABB
    {
        FVector Min;
        FVector Max;

        FAABB()
        {
            Min = FVector(FLT_MAX);
            Max = FVector(-FLT_MAX);
        }

        // Grows the box to include a point.
        void Grow(const FVector& Point)
        {
            Min.X = FMath::Min(Min.X, Point.X);
            Min.Y = FMath::Min(Min.Y, Point.Y);
            Min.Z = FMath::Min(Min.Z, Point.Z);

            Max.X = FMath::Max(Max.X, Point.X);
            Max.Y = FMath::Max(Max.Y, Point.Y);
            Max.Z = FMath::Max(Max.Z, Point.Z);
        }

        // Grows the box to include another box.
        void Grow(const FAABB& Other)
        {
            Grow(Other.Min);
            Grow(Other.Max);
        }

        // Checks for intersection with a ray using the slab method.
        // This is a fast way to cull entire branches of the BVH.
        bool Intersects(const FBVHRay& Ray) const
        {
            FVector InvDir = FVector(1.0f / Ray.Direction.X, 1.0f / Ray.Direction.Y, 1.0f / Ray.Direction.Z);

            FVector t1 = (Min - Ray.Origin) * InvDir;
            FVector t2 = (Max - Ray.Origin) * InvDir;

            float tmin = FMath::Max(FMath::Max(FMath::Min(t1.X, t2.X), FMath::Min(t1.Y, t2.Y)), FMath::Min(t1.Z, t2.Z));
            float tmax = FMath::Min(FMath::Min(FMath::Max(t1.X, t2.X), FMath::Max(t1.Y, t2.Y)), FMath::Max(t1.Z, t2.Z));

            return tmax >= 0 && tmax >= tmin && tmin < Ray.MaxDistance;
        }
    };

    // Represents a single triangle with its vertices and pre-calculated data for the BVH.
    struct FTriangleData
    {
        FVector V0, V1, V2;
        FAABB BBox;
        FVector Centroid;
        int32 MaterialIndex;
    };

    struct FSharedVertexInfo
{
    uint32 IndexInCurrent;  // The index (0, 1, or 2) of the vertex in the first triangle
    uint32 IndexInNeighbor; // The index (0, 1, or 2) of the vertex in the second triangle
};


    // The core BVH implementation.
    struct FBVH
    {
        // A node in the BVH tree. It's a leaf if TriangleCount > 0.
        struct FNode
        {
            FAABB BBox;
            int32 StartIndex;       // Index into the shared `Triangles` array
            int32 TriangleCount;    // If > 0, this is a leaf node.
            int32 RightChildOffset; // Offset to the right child. Left child is always at (this node's index + 1).
        };

        TArray<FNode> Nodes;
        TArray<FTriangleData> Triangles;

    public:
        // Builds the BVH from a static mesh and its world transform.
        void Build(UStaticMesh* StaticMesh, const FTransform& LocalToWorld)
        {
            if (!StaticMesh || !StaticMesh->GetRenderData() || StaticMesh->GetRenderData()->LODResources.Num() == 0) return;

            const FStaticMeshLODResources& LODModel = StaticMesh->GetRenderData()->LODResources[0];
            FIndexArrayView Indices = LODModel.IndexBuffer.GetArrayView();
            const FPositionVertexBuffer& PositionBuffer = LODModel.VertexBuffers.PositionVertexBuffer;
            uint32 NumTriangles = Indices.Num() / 3;

            // 1. Extract all triangle data, transform to world space, and pre-calculate centroids/bboxes.
            Triangles.Reserve(NumTriangles);
            for (uint32 i = 0; i < NumTriangles; ++i)
            {
                FTriangleData Tri;
                uint32 Idx0 = Indices[i * 3 + 0];
                uint32 Idx1 = Indices[i * 3 + 1];
                uint32 Idx2 = Indices[i * 3 + 2];
                
                Tri.V0 = LocalToWorld.TransformPosition(FVector(PositionBuffer.VertexPosition(Idx0)));
                Tri.V1 = LocalToWorld.TransformPosition(FVector(PositionBuffer.VertexPosition(Idx1)));
                Tri.V2 = LocalToWorld.TransformPosition(FVector(PositionBuffer.VertexPosition(Idx2)));

                Tri.BBox.Grow(Tri.V0);
                Tri.BBox.Grow(Tri.V1);
                Tri.BBox.Grow(Tri.V2);
                Tri.Centroid = (Tri.V0 + Tri.V1 + Tri.V2) / 3.0f;

               Tri.MaterialIndex = -1;
                for (const FStaticMeshSection& Section : LODModel.Sections)
                {
                    uint32 FirstTriIndex = Section.FirstIndex / 3;
                    uint32 LastTriIndex = FirstTriIndex + Section.NumTriangles;
                    if (i >= FirstTriIndex && i < LastTriIndex)
                    {
                        Tri.MaterialIndex = Section.MaterialIndex;
                        break;
                    }
                }
                
                Triangles.Add(Tri);
            }

            // 2. Build the hierarchy recursively, starting from the root.
            Nodes.Reserve(NumTriangles * 2);
            FNode& Root = Nodes.AddDefaulted_GetRef();
            Root.StartIndex = 0;
            Root.TriangleCount = NumTriangles;
            
            UpdateNodeBounds(Root);
            Subdivide(Root);
        }

    private:
        // Recursively subdivides a node into two children until it's small enough to be a leaf.
        void Subdivide(FNode& Node)
        {
            // Leaf node condition: if the number of triangles is small, stop subdividing.
            if (Node.TriangleCount <= 4) return;

            // Find the longest axis of the node's bounding box to use for the split.
            FVector Extent = Node.BBox.Max - Node.BBox.Min;
            int32 Axis = 0;
            if (Extent.Y > Extent.X) Axis = 1;
            if (Extent.Z > Extent.X && Extent.Z > Extent.Y) Axis = 2;

            float SplitPos = Node.BBox.Min[Axis] + Extent[Axis] * 0.5f;

            // Partition the triangles based on which side of the split plane their centroid lies on.
            int32 Mid = Node.StartIndex;
            for (int32 i = Node.StartIndex; i < Node.StartIndex + Node.TriangleCount; ++i)
            {
                if (Triangles[i].Centroid[Axis] < SplitPos)
                {
                    Swap(Triangles[i], Triangles[Mid]);
                    Mid++;
                }
            }
            
            // If the partition was bad (all triangles on one side), force a simple middle split.
            if (Mid == Node.StartIndex || Mid == Node.StartIndex + Node.TriangleCount)
            {
                 Mid = Node.StartIndex + Node.TriangleCount / 2;
            }

            // Create child nodes and link them.
            int32 LeftChildIndex = Nodes.Num();
            Nodes.AddDefaulted();
            Nodes[LeftChildIndex].StartIndex = Node.StartIndex;
            Nodes[LeftChildIndex].TriangleCount = Mid - Node.StartIndex;
            
            int32 RightChildIndex = Nodes.Num();
            Nodes.AddDefaulted();
            Nodes[RightChildIndex].StartIndex = Mid;
            Nodes[RightChildIndex].TriangleCount = (Node.StartIndex + Node.TriangleCount) - Mid;
            
            Node.RightChildOffset = RightChildIndex;
            Node.TriangleCount = 0; // Mark this as an internal (non-leaf) node.
            
            UpdateNodeBounds(Nodes[LeftChildIndex]);
            UpdateNodeBounds(Nodes[RightChildIndex]);

            // Recurse down the tree.
            Subdivide(Nodes[LeftChildIndex]);
            Subdivide(Nodes[RightChildIndex]);
        }

        // Calculates the bounding box of a node by encompassing all triangle bboxes within it.
        void UpdateNodeBounds(FNode& Node)
        {
            Node.BBox = FAABB();
            for (int i = 0; i < Node.TriangleCount; ++i)
            {
                Node.BBox.Grow(Triangles[Node.StartIndex + i].BBox);
            }
        }

    public:
        // Performs an intersection test for a single ray against the entire BVH.
        // This uses a non-recursive stack-based traversal for performance.
        bool Intersects(const FBVHRay& Ray, const TSet<int32>& HiddenSlotIndices) const
        {
            if (Nodes.Num() == 0) return false;

            const float SelfIntersectionEpsilon = 0.001f;
            int32 Stack[64]; // A static stack is usually sufficient for typical scene depths.
            int32 StackPtr = 0;
            Stack[StackPtr++] = 0; // Start with the root node (index 0).

            while (StackPtr > 0)
            {
                const FNode& Node = Nodes[Stack[--StackPtr]];

                // If ray doesn't hit this node's box, we can cull this entire branch.
                if (!Node.BBox.Intersects(Ray))
                {
                    continue;
                }

                if (Node.TriangleCount > 0) // It's a leaf node: test against its triangles.
                {
                    for (int32 i = 0; i < Node.TriangleCount; ++i)
                    {
                        const FTriangleData& Tri = Triangles[Node.StartIndex + i];

                        if (HiddenSlotIndices.Contains(Tri.MaterialIndex))
                        {
                            continue;
                        }

                        FVector IntersectionPoint;
                        FVector IntersectionNormal; // Required for the modern function signature.

                        if (FMath::SegmentTriangleIntersection(Ray.Origin, Ray.Origin + Ray.Direction * Ray.MaxDistance, Tri.V0, Tri.V1, Tri.V2, IntersectionPoint, IntersectionNormal))
                        {
                           // Use FVector::Distance for a true world-space distance check.
                            float Dist = FVector::Dist(Ray.Origin, IntersectionPoint);
                            if (Dist > SelfIntersectionEpsilon && Dist < Ray.MaxDistance - SelfIntersectionEpsilon)
                            {
                                return true; // Found a valid occluder.
                            }
                        }
                    }
                }
                else // It's an internal node: push its children onto the stack.
                {
                    Stack[StackPtr++] = Node.RightChildOffset;
                    Stack[StackPtr++] = Node.RightChildOffset - 1; // Left child is always before the right one.
                }
            }

            return false; // No intersection found after checking all relevant nodes.
        }
    };

} 

int32 FMeshProcessor::InitializeTexelBuffers(
    UStaticMesh* StaticMesh,
    const FTransform& LocalToWorld,
    const FProjectionSettings& Settings, int32 OutputWidth, int32 OutputHeight,
    TArray<bool>& VisibilityBuffer,
    TArray<FVector>& WorldPositionBuffer,
    TArray<FVector2D>& ScreenPositionBuffer,
    TArray<FVector2D>& UVBuffer,
    TArray<float>& DepthBuffer,
     int32 UVChannel)
{
    if (!StaticMesh || !StaticMesh->GetRenderData() || StaticMesh->GetRenderData()->LODResources.Num() == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("InitializeTexelBuffers: Invalid static mesh data"));
        return 0;
    }

    // Get LOD 0 of the static mesh (highest detail level)
    const FStaticMeshLODResources& LODModel = StaticMesh->GetRenderData()->LODResources[0];

    
    // Set up texture dimensions from settings
    int32 TextureWidth = OutputWidth;
    int32 TextureHeight = OutputHeight;
    int32 NumTexels = TextureWidth * TextureHeight;
    
    // Initialize all arrays to the correct size
    VisibilityBuffer.Empty(NumTexels);
    VisibilityBuffer.Init(false, NumTexels);
    
    WorldPositionBuffer.Empty(NumTexels);
    WorldPositionBuffer.Init(FVector::ZeroVector, NumTexels);
    
    ScreenPositionBuffer.Empty(NumTexels);
    ScreenPositionBuffer.Init(FVector2D::ZeroVector, NumTexels);
    
    UVBuffer.Empty(NumTexels);
    UVBuffer.Init(FVector2D::ZeroVector, NumTexels);
    
    DepthBuffer.Empty(NumTexels);
    DepthBuffer.Init(MAX_FLT, NumTexels);
    
    // Create a minimal view info structure with camera settings
    FMinimalViewInfo CameraView;
    CameraView.Location = Settings.CameraPosition;
    CameraView.Rotation = FRotator(Settings.CameraRotation);
    CameraView.FOV = Settings.FOVAngle;
    CameraView.AspectRatio = (float)TextureWidth / (float)TextureHeight;
    CameraView.ProjectionMode = ECameraProjectionMode::Perspective;
    CameraView.OrthoWidth = TextureWidth;
    CameraView.OrthoNearClipPlane = 1.0f;
    CameraView.OrthoFarClipPlane = 10000.0f;
    CameraView.bConstrainAspectRatio = true;

    // Get the view, projection and combined matrices
    FMatrix ViewMatrix;
    FMatrix ProjectionMatrix;
    FMatrix ViewProjectionMatrix;
    UGameplayStatics::GetViewProjectionMatrix(CameraView, ViewMatrix, ProjectionMatrix, ViewProjectionMatrix);

    // Debug log the matrices to verify they're correct
    UE_LOG(LogTemp, Log, TEXT("View Matrix:"));
    for (int32 Row = 0; Row < 4; Row++) {
        UE_LOG(LogTemp, Log, TEXT("[%f %f %f %f]"), 
            ViewMatrix.M[Row][0], 
            ViewMatrix.M[Row][1], 
            ViewMatrix.M[Row][2], 
            ViewMatrix.M[Row][3]);
    }

    UE_LOG(LogTemp, Log, TEXT("Projection Matrix:"));
    for (int32 Row = 0; Row < 4; Row++) {
        UE_LOG(LogTemp, Log, TEXT("[%f %f %f %f]"), 
            ProjectionMatrix.M[Row][0], 
            ProjectionMatrix.M[Row][1], 
            ProjectionMatrix.M[Row][2], 
            ProjectionMatrix.M[Row][3]);
    }

    //print the projectionmatrix
    UE_LOG(LogTemp, Warning, TEXT("[MANUAL MATRIX]:\n%s"), *ProjectionMatrix.ToString());
    
    // Access the mesh data
    FIndexArrayView Indices = LODModel.IndexBuffer.GetArrayView();
    int32 NumTriangles = Indices.Num() / 3;
    
    const FPositionVertexBuffer& PositionBuffer = LODModel.VertexBuffers.PositionVertexBuffer;
    const FStaticMeshVertexBuffer& VertexBuffer = LODModel.VertexBuffers.StaticMeshVertexBuffer;
    
    // Track how many texels we've processed
    int32 ProcessedTexels = 0;
    
    // For each texel in the texture, initialize its UV coordinates
    for (int32 Y = 0; Y < TextureHeight; Y++)
    {
        for (int32 X = 0; X < TextureWidth; X++)
        {
            // Convert pixel coordinates to UV space [0,1]
            float U = ((float)X + 0.5f) / (float)TextureWidth;
            float V = ((float)Y + 0.5f) / (float)TextureHeight;
            
            // Calculate texture index
            int32 TextureIndex = Y * TextureWidth + X;
            
            // Store the UV coordinate
            UVBuffer[TextureIndex] = FVector2D(U, V);
            
            // Default visibility remains false until we find a triangle that covers this texel
        }
    }
    
    // For each triangle in the mesh
    for (int32 TriangleIndex = 0; TriangleIndex < NumTriangles; TriangleIndex++)
    {
        // Get vertex indices for this triangle
        uint32 IndexA = Indices[TriangleIndex * 3 + 0];
        uint32 IndexB = Indices[TriangleIndex * 3 + 1];
        uint32 IndexC = Indices[TriangleIndex * 3 + 2];
        
        // Get vertices in local space and convert from FVector3f to FVector (double precision)
        const FVector3f& VertexPositionA_Float = PositionBuffer.VertexPosition(IndexA);
        const FVector3f& VertexPositionB_Float = PositionBuffer.VertexPosition(IndexB);
        const FVector3f& VertexPositionC_Float = PositionBuffer.VertexPosition(IndexC);
        
        // Convert single-precision to double-precision
        FVector VertexPositionA(VertexPositionA_Float.X, VertexPositionA_Float.Y, VertexPositionA_Float.Z);
        FVector VertexPositionB(VertexPositionB_Float.X, VertexPositionB_Float.Y, VertexPositionB_Float.Z);
        FVector VertexPositionC(VertexPositionC_Float.X, VertexPositionC_Float.Y, VertexPositionC_Float.Z);
        
        // Get UVs from the mesh and convert from FVector2f to FVector2D (double precision)
        const FVector2f& UVA_Float = VertexBuffer.GetVertexUV(IndexA, UVChannel); 
        const FVector2f& UVB_Float = VertexBuffer.GetVertexUV(IndexB, UVChannel);
        const FVector2f& UVC_Float = VertexBuffer.GetVertexUV(IndexC, UVChannel);
        
        // Convert to double precision
        FVector2D UVA(UVA_Float.X, UVA_Float.Y);
        FVector2D UVB(UVB_Float.X, UVB_Float.Y);
        FVector2D UVC(UVC_Float.X, UVC_Float.Y);
        
        // Transform vertices to world space
        FVector WorldPositionA = LocalToWorld.TransformPosition(VertexPositionA);
        FVector WorldPositionB = LocalToWorld.TransformPosition(VertexPositionB);
        FVector WorldPositionC = LocalToWorld.TransformPosition(VertexPositionC);
        
        // Transform to view space
        FVector4 ViewPositionA = ViewMatrix.TransformPosition(WorldPositionA);
        FVector4 ViewPositionB = ViewMatrix.TransformPosition(WorldPositionB);
        FVector4 ViewPositionC = ViewMatrix.TransformPosition(WorldPositionC);
        
        // Transform to clip space (NDC)
        FVector4 ClipPositionA = ProjectionMatrix.TransformPosition(ViewPositionA);
        FVector4 ClipPositionB = ProjectionMatrix.TransformPosition(ViewPositionB);
        FVector4 ClipPositionC = ProjectionMatrix.TransformPosition(ViewPositionC);
        
        // Perspective division
        if (ClipPositionA.W != 0.0f) ClipPositionA = ClipPositionA / ClipPositionA.W;
        if (ClipPositionB.W != 0.0f) ClipPositionB = ClipPositionB / ClipPositionB.W;
        if (ClipPositionC.W != 0.0f) ClipPositionC = ClipPositionC / ClipPositionC.W;
        
        // Convert to screen coordinates [0,1]
        FVector2D ScreenPositionA = FVector2D(
            (ClipPositionA.X * 0.5f + 0.5f),
            (ClipPositionA.Y * 0.5f + 0.5f)
        );
        FVector2D ScreenPositionB = FVector2D(
            (ClipPositionB.X * 0.5f + 0.5f),
            (ClipPositionB.Y * 0.5f + 0.5f)
        );
        FVector2D ScreenPositionC = FVector2D(
            (ClipPositionC.X * 0.5f + 0.5f),
            (ClipPositionC.Y * 0.5f + 0.5f)
        );
        
        // Calculate bounding box of the triangle in UV space
        float MinU = FMath::Min3(UVA.X, UVB.X, UVC.X);
        float MinV = FMath::Min3(UVA.Y, UVB.Y, UVC.Y);
        float MaxU = FMath::Max3(UVA.X, UVB.X, UVC.X);
        float MaxV = FMath::Max3(UVA.Y, UVB.Y, UVC.Y);
        
        // Clamp UV values to [0,1] range
        MinU = FMath::Clamp(MinU, 0.0f, 1.0f);
        MinV = FMath::Clamp(MinV, 0.0f, 1.0f);
        MaxU = FMath::Clamp(MaxU, 0.0f, 1.0f);
        MaxV = FMath::Clamp(MaxV, 0.0f, 1.0f);
        
        // Convert UV bounds to texture space
        int32 MinX = FMath::FloorToInt(MinU * (TextureWidth - 1));
        int32 MinY = FMath::FloorToInt(MinV * (TextureHeight - 1));
        int32 MaxX = FMath::CeilToInt(MaxU * (TextureWidth - 1));
        int32 MaxY = FMath::CeilToInt(MaxV * (TextureHeight - 1));
        
        // Clamp to texture bounds
        MinX = FMath::Clamp(MinX, 0, TextureWidth - 1);
        MinY = FMath::Clamp(MinY, 0, TextureHeight - 1);
        MaxX = FMath::Clamp(MaxX, 0, TextureWidth - 1);
        MaxY = FMath::Clamp(MaxY, 0, TextureHeight - 1);
        
        // For each texel in the triangle's bounding box
        for (int32 Y = MinY; Y <= MaxY; Y++)
        {
            for (int32 X = MinX; X <= MaxX; X++)
            {
                // Convert pixel coordinates to UV space
                float U = ((float)X + 0.5f) / (float)TextureWidth;
                float V = ((float)Y + 0.5f) / (float)TextureHeight;
                FVector2D TexelUV(U, V);
                
                // Check if the point is inside the triangle in UV space using existing MathUtils
                float Alpha, Beta, Gamma;
                if (FMathUtils::CalculateBarycentricCoordinates(TexelUV, UVA, UVB, UVC, Alpha, Beta, Gamma))
                {
                    // Use a small threshold to include points that are very close to the triangle
                    if (Alpha >= -0.00f && Beta >= -0.00f && Gamma >= -0.00f &&
                        Alpha <= 1.00f && Beta <= 1.00f && Gamma <= 1.00f)
                    {
                        // Clamp barycentric coordinates to [0,1] for interpolation
                        Alpha = FMath::Clamp(Alpha, 0.0f, 1.0f);
                        Beta = FMath::Clamp(Beta, 0.0f, 1.0f);
                        Gamma = FMath::Clamp(Gamma, 0.0f, 1.0f);
                        
                        // // Normalize to ensure they sum to 1.0
                        // float Sum = Alpha + Beta + Gamma;
                        // if (Sum > 0.0f)
                        // {
                        //     Alpha /= Sum;
                        //     Beta /= Sum;
                        //     Gamma /= Sum;
                        // }
                        
                        // Interpolate world position
                        FVector WorldPosition = 
                            WorldPositionA * Alpha + 
                            WorldPositionB * Beta + 
                            WorldPositionC * Gamma;
                        
                        // Interpolate screen position
                        FVector2D ScreenPosition = 
                            ScreenPositionA * Alpha +
                            ScreenPositionB * Beta +
                            ScreenPositionC * Gamma;
                        
                        // Calculate depth (distance from camera)
                        FVector CameraForward = Settings.CameraRotation.Vector(); // Get camera forward vector
                        FVector DirectionToPoint = WorldPosition - Settings.CameraPosition;
                        // In InitializeTexelBuffers, make depths consistently positive
                        float Depth = FMath::Abs(FVector::DotProduct(DirectionToPoint, CameraForward));
                        
                        // Calculate texture index
                        int32 TextureIndex = Y * TextureWidth + X;
                        
                        // Mark this texel as visible since it's inside a triangle
                        VisibilityBuffer[TextureIndex] = true;
                        ProcessedTexels++;
                        
                        // Update buffers if this is a closer intersection
                        if (Depth < DepthBuffer[TextureIndex])
                        {
                            WorldPositionBuffer[TextureIndex] = WorldPosition;
                            ScreenPositionBuffer[TextureIndex] = ScreenPosition;
                            DepthBuffer[TextureIndex] = Depth;
                        }
                    }
                }
            }
        }
    }
    
    UE_LOG(LogTemp, Log, TEXT("InitializeTexelBuffers: Processed %d texels"), ProcessedTexels);
    return ProcessedTexels;
}

int32 FMeshProcessor::InitializeStaticTexelBuffers(
    UStaticMesh* StaticMesh,
    const FTransform& LocalToWorld,
    const FProjectionSettings& Settings,
    int32 TextureWidth,
    int32 TextureHeight,
    TArray<FVector2D>& UVBuffer,
    TArray<FVector>& WorldPositionBuffer,
    int32 UVChannel)
{
    if (!StaticMesh || !StaticMesh->GetRenderData() || StaticMesh->GetRenderData()->LODResources.Num() == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("InitializeStaticTexelBuffers: Invalid static mesh data"));
        return 0;
    }

    // LOD fetch still validates mesh; we just won’t rasterize here anymore.
    // (Keeps signature/API stable.)
    // const FStaticMeshLODResources& LODModel = StaticMesh->GetRenderData()->LODResources[0];

    const int32 NumTexels = TextureWidth * TextureHeight;

    // Initialize UVs for pixel-center convention.
    UVBuffer.SetNumUninitialized(NumTexels);
    for (int32 Y = 0; Y < TextureHeight; ++Y)
    {
        const float V = (Y + 0.5f) / float(TextureHeight);
        for (int32 X = 0; X < TextureWidth; ++X)
        {
            const float U = (X + 0.5f) / float(TextureWidth);
            UVBuffer[Y * TextureWidth + X] = FVector2D(U, V);
        }
    }

    // IMPORTANT: Do NOT stamp world positions here.
    // Just size/zero the buffer so later passes can detect “no owner yet.”
    WorldPositionBuffer.Init(FVector::ZeroVector, NumTexels);

    UE_LOG(LogTemp, Log, TEXT("InitializeStaticTexelBuffers: UVs initialized for %dx%d, world buffer zeroed"), TextureWidth, TextureHeight);
    return NumTexels; // number of UV texels prepared
}

void FMeshProcessor::InitializeDynamicTexelBuffers(
    UStaticMesh* StaticMesh,
    const FTransform& LocalToWorld,
    const FProjectionSettings& Settings,
    int32 TextureWidth,
    int32 TextureHeight,
    int32 UVChannel,
    const TSet<int32>& HiddenSlotIndices,
    TArray<FVector>&   OutWorldPositionBuffer,
    TArray<FVector2D>& OutScreenPositionBuffer,
    TArray<float>&     OutDepthBuffer)
{
    if (!StaticMesh || !StaticMesh->GetRenderData() || StaticMesh->GetRenderData()->LODResources.Num() == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("InitializeDynamicTexelBuffers: Invalid mesh/LOD"));
        return;
    }

    const int32 NumTexels = TextureWidth * TextureHeight;

    // Outputs
    OutWorldPositionBuffer.Init(FVector::ZeroVector, NumTexels);
    OutScreenPositionBuffer.Init(FVector2D::ZeroVector, NumTexels);
    OutDepthBuffer.Init(FLT_MAX, NumTexels);

    // Per-texel ownership (camera-independent)
    TArray<int32> OwnerComp;  OwnerComp.Init(-1, NumTexels);
    TArray<float> OwnerBias;  OwnerBias.Init(-FLT_MAX, NumTexels);

    // Per-texel best (within chosen component)
    TArray<float> BestDepth;  BestDepth.Init(FLT_MAX, NumTexels); // choose front-most in chosen component

    // Camera matrices (for projection only; no culling here)
    FMinimalViewInfo ViewInfo;
    ViewInfo.Location = Settings.CameraPosition;
    ViewInfo.Rotation = Settings.CameraRotation;
    ViewInfo.FOV      = Settings.FOVAngle;
    ViewInfo.AspectRatio = float(TextureWidth) / float(TextureHeight);
    ViewInfo.ProjectionMode = ECameraProjectionMode::Perspective;
    ViewInfo.bConstrainAspectRatio = true;

    FMatrix ViewM, ProjM, ViewProjM;
    UGameplayStatics::GetViewProjectionMatrix(ViewInfo, ViewM, ProjM, ViewProjM);

    // Tolerances
    constexpr float kCompEps       = 1e-6f;
    constexpr float kAreaEps       = 1e-12f;
    constexpr float kInteriorGuard = -KINDA_SMALL_NUMBER;   // ignore exact-edge texels for ownership

    auto PixelUV = [TextureWidth, TextureHeight](int32 X, int32 Y)
    {
        return FVector2D((X + 0.5f) / float(TextureWidth),
                         (Y + 0.5f) / float(TextureHeight));
    };

    // Inline barycentric in UV space
    auto Barycentric2D = [&](const FVector2D& P, const FVector2D& A, const FVector2D& B, const FVector2D& C, float& u, float& v, float& w) -> bool
    {
        const double x = P.X,  y = P.Y;
        const double x0 = A.X, y0 = A.Y;
        const double x1 = B.X, y1 = B.Y;
        const double x2 = C.X, y2 = C.Y;

        const double v0x = x1 - x0, v0y = y1 - y0;
        const double v1x = x2 - x0, v1y = y2 - y0;
        const double v2x = x  - x0, v2y = y  - y0;

        const double denom = v0x * v1y - v0y * v1x;
        if (FMath::Abs(denom) < kAreaEps) return false;

        const double invD = 1.0 / denom;
        const double vb = (v2x * v1y - v2y * v1x) * invD;
        const double vc = (v0x * v2y - v0y * v2x) * invD;
        const double va = 1.0 - vb - vc;

        u = float(va); v = float(vb); w = float(vc);
        return true;
    };

    // Mesh data
    const FStaticMeshLODResources& LOD = StaticMesh->GetRenderData()->LODResources[0];
    const FIndexArrayView Indices = LOD.IndexBuffer.GetArrayView();
    const FStaticMeshVertexBuffer& VB = LOD.VertexBuffers.StaticMeshVertexBuffer;
    const FPositionVertexBuffer&   PB = LOD.VertexBuffers.PositionVertexBuffer;

    // ----------------------------------------------------------------------
    // PASS 0: Build triangle connected components (by shared vertex) + per-component bias
    // ----------------------------------------------------------------------
    const int32 NumTrisAll = Indices.Num() / 3;

    struct FDSU {
        TArray<int32> P, R;
        void Init(int32 n){ P.SetNumUninitialized(n); R.Init(0, n); for(int32 i=0;i<n;++i) P[i]=i; }
        int32 Find(int32 a){ while(P[a]!=a){ P[a]=P[P[a]]; a=P[a]; } return a; }
        void Union(int32 a,int32 b){ a=Find(a); b=Find(b); if(a==b) return; if(R[a]<R[b]) Swap(a,b); P[b]=a; if(R[a]==R[b]) ++R[a]; }
    } DSU;
    DSU.Init(NumTrisAll);

    // vertex -> triangles
    TMap<uint32, TArray<int32>> VertToTris;
    VertToTris.Reserve(PB.GetNumVertices());
    for (int32 t = 0; t < NumTrisAll; ++t)
    {
        VertToTris.FindOrAdd(Indices[3*t+0]).Add(t);
        VertToTris.FindOrAdd(Indices[3*t+1]).Add(t);
        VertToTris.FindOrAdd(Indices[3*t+2]).Add(t);
    }
    for (auto& Pair : VertToTris)
    {
        const TArray<int32>& Tris = Pair.Value;
        if (Tris.Num() <= 1) continue;
        const int32 base = Tris[0];
        for (int32 k = 1; k < Tris.Num(); ++k) DSU.Union(base, Tris[k]);
    }

    TArray<int32> TriCompId; TriCompId.SetNumUninitialized(NumTrisAll);
    for (int32 t = 0; t < NumTrisAll; ++t) TriCompId[t] = DSU.Find(t);

    // Per-component center (for bias)
    TMap<int32, FVector> CompSum;
    TMap<int32, int64>   CompCnt;
    for (int32 t = 0; t < NumTrisAll; ++t)
    {
        const uint32 i0 = Indices[3*t+0], i1 = Indices[3*t+1], i2 = Indices[3*t+2];
        const FVector P0 = LocalToWorld.TransformPosition(FVector(PB.VertexPosition(i0)));
        const FVector P1 = LocalToWorld.TransformPosition(FVector(PB.VertexPosition(i1)));
        const FVector P2 = LocalToWorld.TransformPosition(FVector(PB.VertexPosition(i2)));
        const int32 cid = TriCompId[t];

        FVector& S = CompSum.FindOrAdd(cid);
        S += P0 + P1 + P2;
        int64& C = CompCnt.FindOrAdd(cid);
        C += 3;
    }

    // Fixed world axis to break UV stacks: "+X wins" (flip if you prefer)
    const FVector ResolveAxis = FVector(1, 0, 0);

    TMap<int32, float> CompBias;
    for (auto& KVP : CompSum)
    {
        const int32 cid = KVP.Key;
        const FVector center = (CompCnt[cid] > 0) ? (KVP.Value / float(CompCnt[cid])) : FVector::ZeroVector;
        CompBias.Add(cid, FVector::DotProduct(center, ResolveAxis));
    }

    // ----------------------------------------------------------------------
    // PASS 1: choose owner *component* per texel in UV space (camera-independent)
    // ----------------------------------------------------------------------
    for (const FStaticMeshSection& Section : LOD.Sections)
    {
        if (Settings.TargetMaterialSlotIndex >= 0 && Section.MaterialIndex != Settings.TargetMaterialSlotIndex) continue;
        if (HiddenSlotIndices.Contains(Section.MaterialIndex)) continue;

        const uint32 First = Section.FirstIndex;
        const uint32 Last  = First + Section.NumTriangles * 3;

        for (uint32 i = First; i < Last; i += 3)
        {
            const uint32 i0 = Indices[i+0], i1 = Indices[i+1], i2 = Indices[i+2];

            const FVector2D UV0(VB.GetVertexUV(i0, UVChannel));
            const FVector2D UV1(VB.GetVertexUV(i1, UVChannel));
            const FVector2D UV2(VB.GetVertexUV(i2, UVChannel));

            // UV AABB → inclusive texel bounds
            int32 minX = FMath::Clamp(FMath::FloorToInt(FMath::Min3(UV0.X, UV1.X, UV2.X) * TextureWidth),  0, TextureWidth  - 1);
            int32 minY = FMath::Clamp(FMath::FloorToInt(FMath::Min3(UV0.Y, UV1.Y, UV2.Y) * TextureHeight), 0, TextureHeight - 1);
            int32 maxX = FMath::Clamp(FMath::CeilToInt (FMath::Max3(UV0.X, UV1.X, UV2.X) * TextureWidth)  - 1, 0, TextureWidth  - 1);
            int32 maxY = FMath::Clamp(FMath::CeilToInt (FMath::Max3(UV0.Y, UV1.Y, UV2.Y) * TextureHeight) - 1, 0, TextureHeight - 1);

            const int32 triKey = int32(i / 3);
            const int32 compId = TriCompId.IsValidIndex(triKey) ? TriCompId[triKey] : triKey;
            const float bias   = CompBias.Contains(compId) ? CompBias[compId] : 0.0f;

            for (int32 y = minY; y <= maxY; ++y)
            for (int32 x = minX; x <= maxX; ++x)
            {
                const int32 idx = y * TextureWidth + x;
                const FVector2D uv = PixelUV(x, y);

                float A, B, C;
                if (!Barycentric2D(uv, UV0, UV1, UV2, A, B, C)) continue;

                // Ownership only from interior texels (avoid edge/seam ambiguity)
                if (A < kInteriorGuard || B < kInteriorGuard || C < kInteriorGuard) continue;

                if (OwnerComp[idx] == -1)
                {
                    OwnerComp[idx] = compId;
                    OwnerBias[idx] = bias;
                }
                else
                {
                    // Higher bias wins; tie → smaller compId (deterministic)
                    if (bias > OwnerBias[idx] + kCompEps ||
                        (FMath::Abs(bias - OwnerBias[idx]) <= kCompEps && compId < OwnerComp[idx]))
                    {
                        OwnerComp[idx] = compId;
                        OwnerBias[idx] = bias;
                    }
                }
            }
        }
    }

    // ----------------------------------------------------------------------
    // PASS 2: only chosen component may write; within it, pick front-most triangle (min depth)
    // No backface/frustum culling here.
    // ----------------------------------------------------------------------
    int32 FilledTexels = 0; // <-- instead of BestDepth.Count()

    for (const FStaticMeshSection& Section : LOD.Sections)
    {
        if (Settings.TargetMaterialSlotIndex >= 0 && Section.MaterialIndex != Settings.TargetMaterialSlotIndex) continue;
        if (HiddenSlotIndices.Contains(Section.MaterialIndex)) continue;

        const uint32 First = Section.FirstIndex;
        const uint32 Last  = First + Section.NumTriangles * 3;

        for (uint32 i = First; i < Last; i += 3)
        {
            const uint32 i0 = Indices[i+0], i1 = Indices[i+1], i2 = Indices[i+2];

            const FVector P0 = LocalToWorld.TransformPosition(FVector(PB.VertexPosition(i0)));
            const FVector P1 = LocalToWorld.TransformPosition(FVector(PB.VertexPosition(i1)));
            const FVector P2 = LocalToWorld.TransformPosition(FVector(PB.VertexPosition(i2)));

            const FVector2D UV0(VB.GetVertexUV(i0, UVChannel));
            const FVector2D UV1(VB.GetVertexUV(i1, UVChannel));
            const FVector2D UV2(VB.GetVertexUV(i2, UVChannel));

            // UV AABB
            int32 minX = FMath::Clamp(FMath::FloorToInt(FMath::Min3(UV0.X, UV1.X, UV2.X) * TextureWidth),  0, TextureWidth  - 1);
            int32 minY = FMath::Clamp(FMath::FloorToInt(FMath::Min3(UV0.Y, UV1.Y, UV2.Y) * TextureHeight), 0, TextureHeight - 1);
            int32 maxX = FMath::Clamp(FMath::CeilToInt (FMath::Max3(UV0.X, UV1.X, UV2.X) * TextureWidth)  - 1, 0, TextureWidth  - 1);
            int32 maxY = FMath::Clamp(FMath::CeilToInt (FMath::Max3(UV0.Y, UV1.Y, UV2.Y) * TextureHeight) - 1, 0, TextureHeight - 1);

            const int32 triKey = int32(i / 3);
            const int32 compId = TriCompId.IsValidIndex(triKey) ? TriCompId[triKey] : triKey;

            for (int32 y = minY; y <= maxY; ++y)
            for (int32 x = minX; x <= maxX; ++x)
            {
                const int32 idx = y * TextureWidth + x;
                if (OwnerComp[idx] != compId) continue; // only chosen component can write

                const FVector2D uv = PixelUV(x, y);

                float A, B, C;
                if (!Barycentric2D(uv, UV0, UV1, UV2, A, B, C)) continue;
                if (A < 0.f || B < 0.f || C < 0.f) continue;

                const FVector W = P0*A + P1*B + P2*C;
                const float   Depth = (W - Settings.CameraPosition).Size();

                // Keep the front-most triangle within the chosen component
                if (Depth < BestDepth[idx])
                {
                    const bool firstWrite = (BestDepth[idx] == FLT_MAX);
                    BestDepth[idx] = Depth;
                    OutWorldPositionBuffer[idx] = W;

                    // Screen projection (store even if off-screen)
                    const FVector4 Clip = ViewProjM.TransformFVector4(FVector4(W, 1.f));
                    const float invW = (Clip.W != 0.f) ? (1.f / Clip.W) : 0.f;
                    OutScreenPositionBuffer[idx] = FVector2D(Clip.X*invW*0.5f + 0.5f,
                                                             Clip.Y*invW*0.5f + 0.5f);

                    OutDepthBuffer[idx] = Depth;

                    if (firstWrite) ++FilledTexels;
                }
            }
        }
    }

    UE_LOG(LogTemp, Log,
        TEXT("InitializeDynamicTexelBuffers (no-cull): filled %d/%d texels; ResolveAxis=(%.3f,%.3f,%.3f)"),
        FilledTexels, NumTexels,
        ResolveAxis.X, ResolveAxis.Y, ResolveAxis.Z);
}



int32 FMeshProcessor::RemoveBackfaces(
    UStaticMesh* StaticMesh,
    const FTransform& LocalToWorld,
    const FProjectionSettings& Settings,
    int32 TextureWidth,
    int32 TextureHeight,
    TArray<bool>& VisibilityBuffer,
        int32 UVChannel,
    TArray<FColor>* NormalColorBuffer)
{
    if (!StaticMesh || !StaticMesh->GetRenderData() || 
        StaticMesh->GetRenderData()->LODResources.Num() == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("RemoveBackfaces: Invalid static mesh data"));
        return 0;
    }

    // Get LOD 0 (highest detail level)
    const FStaticMeshLODResources& LODModel = StaticMesh->GetRenderData()->LODResources[0];
    FIndexArrayView Indices = LODModel.IndexBuffer.GetArrayView();
    const FStaticMeshVertexBuffer& VertexBuffer = LODModel.VertexBuffers.StaticMeshVertexBuffer;
    const FPositionVertexBuffer& PositionBuffer = LODModel.VertexBuffers.PositionVertexBuffer;
    
    // Set parameters for the threshold.
    // For example, if we want only faces facing within 10° toward the camera, 
    // you can compute the cosine threshold as follows. Note that using vertex normals,
    // you might need to experiment with this value.
    const float AngleThresholdDegrees = 10.0f;
    const float CosThreshold = FMath::Cos(FMath::DegreesToRadians(90.0f - AngleThresholdDegrees));
    const float BaryEpsilon = 1e-4f;
    
    int32 NumTriangles = Indices.Num() / 3;
    int32 ProcessedTexels = 0;

    // Initialize the visibility buffer (set all texels initially to false)
    int32 TotalTexels = TextureWidth * TextureHeight;
    VisibilityBuffer.Empty();
    VisibilityBuffer.Init(false, TotalTexels);

    // Process each triangle
    for (int32 TriangleIndex = 0; TriangleIndex < NumTriangles; TriangleIndex++)
    {
        uint32 Index0 = Indices[TriangleIndex * 3 + 0];
        uint32 Index1 = Indices[TriangleIndex * 3 + 1];
        uint32 Index2 = Indices[TriangleIndex * 3 + 2];

        // Retrieve UVs from channel 0
        const FVector2f& UV0F = VertexBuffer.GetVertexUV(Index0, UVChannel);
        const FVector2f& UV1F = VertexBuffer.GetVertexUV(Index1, UVChannel);
        const FVector2f& UV2F = VertexBuffer.GetVertexUV(Index2, UVChannel);
        FVector2D UV0(UV0F.X, UV0F.Y);
        FVector2D UV1(UV1F.X, UV1F.Y);
        FVector2D UV2(UV2F.X, UV2F.Y);

        // Get vertex normals from the mesh
        const FVector3f& Norm0F = VertexBuffer.VertexTangentZ(Index0);
        const FVector3f& Norm1F = VertexBuffer.VertexTangentZ(Index1);
        const FVector3f& Norm2F = VertexBuffer.VertexTangentZ(Index2);
        FVector Norm0(Norm0F.X, Norm0F.Y, Norm0F.Z);
        FVector Norm1(Norm1F.X, Norm1F.Y, Norm1F.Z);
        FVector Norm2(Norm2F.X, Norm2F.Y, Norm2F.Z);

        // Transform the vertex normals to world space (rotation only)
        Norm0 = LocalToWorld.TransformVector(Norm0).GetSafeNormal();
        Norm1 = LocalToWorld.TransformVector(Norm1).GetSafeNormal();
        Norm2 = LocalToWorld.TransformVector(Norm2).GetSafeNormal();

        // Compute the face normal as the average of the vertex normals
        FVector FaceNormal = (Norm0 + Norm1 + Norm2) / 3.0f;
        FaceNormal.Normalize();

        // Compute vertex positions (for UV mapping, etc.)
        const FVector3f& VertPos0F = PositionBuffer.VertexPosition(Index0);
        const FVector3f& VertPos1F = PositionBuffer.VertexPosition(Index1);
        const FVector3f& VertPos2F = PositionBuffer.VertexPosition(Index2);
        FVector VertPos0(VertPos0F.X, VertPos0F.Y, VertPos0F.Z);
        FVector VertPos1(VertPos1F.X, VertPos1F.Y, VertPos1F.Z);
        FVector VertPos2(VertPos2F.X, VertPos2F.Y, VertPos2F.Z);
        FVector WorldPos0 = LocalToWorld.TransformPosition(VertPos0);
        // (We use WorldPos0 as a reference for the view vector; you might also use the centroid.)
        // FVector Centroid = (WorldPos0 + LocalToWorld.TransformPosition(VertPos1) + LocalToWorld.TransformPosition(VertPos2)) / 3.0f;

        // Compute view vector (from a vertex toward the camera)
        FVector ViewVector = (Settings.CameraPosition - WorldPos0).GetSafeNormal();

        // Check if the face is front-facing according to vertex normals.
        // Since our vectors are normalized, the dot product gives the cosine of the angle.
        bool bFrontFacing = (FVector::DotProduct(FaceNormal, ViewVector) >= CosThreshold);

        // Prepare normal color for visualization (if provided)
        FColor NormalColor = FColor::Black;
        if (NormalColorBuffer != nullptr)
        {
            FVector NormalRemapped = (FaceNormal + FVector(1.0f)) * 0.5f;
            NormalColor = FColor(
                FMath::RoundToInt(NormalRemapped.X * 255.0f),
                FMath::RoundToInt(NormalRemapped.Y * 255.0f),
                FMath::RoundToInt(NormalRemapped.Z * 255.0f),
                255);
        }

        // Compute the UV bounding box for the triangle
        float MinU = FMath::Clamp(FMath::Min3(UV0.X, UV1.X, UV2.X), 0.0f, 1.0f);
        float MinV = FMath::Clamp(FMath::Min3(UV0.Y, UV1.Y, UV2.Y), 0.0f, 1.0f);
        float MaxU = FMath::Clamp(FMath::Max3(UV0.X, UV1.X, UV2.X), 0.0f, 1.0f);
        float MaxV = FMath::Clamp(FMath::Max3(UV0.Y, UV1.Y, UV2.Y), 0.0f, 1.0f);

        // Convert the UV bounding box to texture space
        int32 MinX = FMath::Clamp(FMath::FloorToInt(MinU * (TextureWidth - 1)), 0, TextureWidth - 1);
        int32 MinY = FMath::Clamp(FMath::FloorToInt(MinV * (TextureHeight - 1)), 0, TextureHeight - 1);
        int32 MaxX = FMath::Clamp(FMath::CeilToInt(MaxU * (TextureWidth - 1)), 0, TextureWidth - 1);
        int32 MaxY = FMath::Clamp(FMath::CeilToInt(MaxV * (TextureHeight - 1)), 0, TextureHeight - 1);

        // Process each texel in the triangle's UV bounding box
        for (int32 Y = MinY; Y <= MaxY; Y++)
        {
            for (int32 X = MinX; X <= MaxX; X++)
            {
                // Compute the UV coordinate at the center of the texel
                float UCoord = (((float)X + 0.5f) / TextureWidth);
                float VCoord = (((float)Y + 0.5f) / TextureHeight);
                FVector2D TexelUV(UCoord, VCoord);

                float A, B, C;
                if (FMathUtils::CalculateBarycentricCoordinates(TexelUV, UV0, UV1, UV2, A, B, C))
                {
                    // Allow a small tolerance in the barycentrics to handle edge cases
                    if (A >= -BaryEpsilon && B >= -BaryEpsilon && C >= -BaryEpsilon &&
                        A <= 1.0f + BaryEpsilon && B <= 1.0f + BaryEpsilon && C <= 1.0f + BaryEpsilon)
                    {
                        int32 TexelIndex = Y * TextureWidth + X;
                        // If a front-facing triangle covers this texel, mark it as visible.
                        if (bFrontFacing)
                        {
                            VisibilityBuffer[TexelIndex] = true;
                            if (NormalColorBuffer != nullptr)
                            {
                                (*NormalColorBuffer)[TexelIndex] = NormalColor;
                            }
                        }
                        ProcessedTexels++;
                    }
                }
            }
        }
    }

    return ProcessedTexels;
}


FMatrix FMeshProcessor::BuildViewProjectionMatrixManual(
    const FVector& CameraPosition,
    const FRotator& CameraRotation,
    float FOVDegrees,
    float AspectRatio,
    float NearPlane,
    float FarPlane
)
{
    // Build the view matrix as the inverse of the camera's transform.
    FMatrix ViewMatrix = FInverseRotationMatrix(CameraRotation) * FTranslationMatrix(-CameraPosition);
    
    // Build a standard perspective projection matrix.
    float FOVRadians = FMath::DegreesToRadians(FOVDegrees);
    float Scale = 1.0f / FMath::Tan(FOVRadians * 0.5f);
    //print scale
    UE_LOG(LogTemp, Log, TEXT("Scale: %f"), Scale);
    //print aspect ratio
    UE_LOG(LogTemp, Log, TEXT("Aspect Ratio: %f"), AspectRatio);
    //print near plane
    UE_LOG(LogTemp, Log, TEXT("Near Plane: %f"), NearPlane);
    //print far plane
    UE_LOG(LogTemp, Log, TEXT("Far Plane: %f"), FarPlane);
    //print fov radians
    UE_LOG(LogTemp, Log, TEXT("FOV Radians: %f"), FOVRadians);
    //print camera position
    UE_LOG(LogTemp, Log, TEXT("Camera Position: %s"), *CameraPosition.ToString());

    FMatrix ProjectionMatrix = FMatrix::Identity;
    ProjectionMatrix.M[0][0] = Scale / AspectRatio;
    ProjectionMatrix.M[1][1] = Scale;
    ProjectionMatrix.M[2][2] = FarPlane / (FarPlane - NearPlane);
    ProjectionMatrix.M[2][3] = 1.0f;
    ProjectionMatrix.M[3][2] = -(FarPlane * NearPlane) / (FarPlane - NearPlane);
    ProjectionMatrix.M[3][3] = 0.0f;
    
    // Combine the matrices.
    FMatrix ViewProjectionMatrix = ViewMatrix * ProjectionMatrix;
    return ViewProjectionMatrix;
}


// Modified RemoveOccludedTexels function that uses the RemoveBackfaces function
int32 FMeshProcessor::RemoveOccludedTexels(
    UStaticMesh* StaticMesh,
    const FTransform& LocalToWorld,
    const FProjectionSettings& Settings, int32 OutputWidth, int32 OutputHeight,
    TArray<bool>& VisibilityBuffer,
    TArray<FVector>& WorldPositionBuffer,
    TArray<FVector2D>& ScreenPositionBuffer,
    TArray<FVector2D>& UVBuffer,
    TArray<float>& DepthBuffer,
    TArray<FColor>& NormalColorBuffer,
    int32 UVChannel)
{
    if (!StaticMesh || !StaticMesh->GetRenderData() || StaticMesh->GetRenderData()->LODResources.Num() == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("RemoveOccludedTexels: Invalid static mesh data"));
        return 0;
    }

    int32 TextureWidth = OutputWidth;
    int32 TextureHeight = OutputHeight;
    int32 NumTexels = TextureWidth * TextureHeight;

    // Initialize buffers
    VisibilityBuffer.Empty(NumTexels);
    VisibilityBuffer.Init(true, NumTexels); // Start with all texels visible
    
    WorldPositionBuffer.Empty(NumTexels);
    WorldPositionBuffer.Init(FVector::ZeroVector, NumTexels);
    
    ScreenPositionBuffer.Empty(NumTexels);
    ScreenPositionBuffer.Init(FVector2D::ZeroVector, NumTexels);
    
    UVBuffer.Empty(NumTexels);
    UVBuffer.Init(FVector2D::ZeroVector, NumTexels);
    
    DepthBuffer.Empty(NumTexels);
    DepthBuffer.Init(MAX_FLT, NumTexels);
    
    NormalColorBuffer.Empty(NumTexels);
    NormalColorBuffer.Init(FColor::Black, NumTexels);

    // Initialize per-texel UVs
    int32 ProcessedTexels = 0;
    for (int32 Y = 0; Y < TextureHeight; Y++)
    {
        for (int32 X = 0; X < TextureWidth; X++)
        {
            float U = ((float)X + 0.5f) / (float)TextureWidth;
            float V = ((float)Y + 0.5f) / (float)TextureHeight;
            int32 TextureIndex = Y * TextureWidth + X;
            UVBuffer[TextureIndex] = FVector2D(U, V);
            ProcessedTexels++;
        }
    }

    InitializeTexelBuffers(StaticMesh, LocalToWorld, Settings, OutputWidth, OutputHeight, VisibilityBuffer, WorldPositionBuffer, ScreenPositionBuffer, UVBuffer, DepthBuffer, UVChannel);
    // Step 1: Remove backfaces
    RemoveBackfaces(StaticMesh, LocalToWorld, Settings, TextureWidth, TextureHeight, VisibilityBuffer, UVChannel, &NormalColorBuffer);

    // Step 2: Depth based occlusion

    // Then perform depth-based occlusion
    // PerformRayCastOcclusion(
    //     StaticMesh,
    //     LocalToWorld,
    //     Settings,
    //     VisibilityBuffer,
    //     WorldPositionBuffer,
    //     DepthBuffer
    // );

    return ProcessedTexels;
}

int32 FMeshProcessor::PerformRayCastOcclusion(
    UStaticMesh* StaticMesh,
    const FTransform& LocalToWorld,
    const FProjectionSettings& Settings,
    TArray<bool>& VisibilityBuffer,
    TArray<FVector>& WorldPositionBuffer,
    TArray<float>& DepthBuffer,
    TArray<float>& NormalWeightBuffer,
    const TSet<int32>& HiddenSlotIndices) 
{
    UE_LOG(LogTemp, Warning, TEXT("Performing OPTIMIZED Ray Cast Occlusion with BVH"));
    if (!StaticMesh || !StaticMesh->GetRenderData() || StaticMesh->GetRenderData()->LODResources.Num() == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("PerformRayCastOcclusion: Invalid static mesh data"));
        return 0;
    }

    // 1. Build the BVH. This is a one-time cost for this function call.
    // It organizes all mesh triangles into a spatial hierarchy for fast ray traversal.
    FBVH BVH;
    BVH.Build(StaticMesh, LocalToWorld);
    UE_LOG(LogTemp, Log, TEXT("BVH built for %d triangles."), BVH.Triangles.Num());


    int32 NumTexels = VisibilityBuffer.Num();
    int32 OccludedTexels = 0;

    // 2. For each visible texel, cast a ray and check for occlusion using the BVH.
    for (int32 TexelIndex = 0; TexelIndex < NumTexels; TexelIndex++)
    {
        // Skip texels that have already been culled (e.g., by backface culling).
        if (!VisibilityBuffer[TexelIndex])
        {
            continue;
        }

        const FVector& TexelWorldPos = WorldPositionBuffer[TexelIndex];
        if (TexelWorldPos.IsNearlyZero())
        {
            continue;
        }

        float RayLength = (TexelWorldPos - Settings.CameraPosition).Size();
        if (RayLength < KINDA_SMALL_NUMBER)
        {
            continue;
        }
        
        // Create the ray for this specific texel.
        FBVHRay Ray;
        Ray.Origin = Settings.CameraPosition;
        Ray.Direction = (TexelWorldPos - Ray.Origin).GetSafeNormal();
        Ray.MaxDistance = RayLength;

        // 3. The optimization: Test the ray against the BVH.
        // This is much faster than iterating through every triangle in the mesh.
        if (BVH.Intersects(Ray, HiddenSlotIndices))
        {
            // If an intersection is found, this texel is occluded.
            VisibilityBuffer[TexelIndex] = false;
            NormalWeightBuffer[TexelIndex] = 0.0f; // Also nullify its contribution weight.
            OccludedTexels++;
        }
    }

    UE_LOG(LogTemp, Log, TEXT("PerformRayCastOcclusion: Marked %d texels as occluded"), OccludedTexels);
    return OccludedTexels;
}


// Ray-triangle intersection helper function
bool FMeshProcessor::RayIntersectsTriangle(
    const FVector& RayOrigin, 
    const FVector& RayDirection,
    const FVector& V0, 
    const FVector& V1, 
    const FVector& V2,
    float& OutDistance,
    FVector& OutIntersectionPoint)
{
    // Implementation of Möller–Trumbore algorithm
    const float EPSILON = 0.0000001f;
    
    // Calculate triangle edges
    FVector Edge1 = V1 - V0;
    FVector Edge2 = V2 - V0;
    
    // Calculate determinant
    FVector H = FVector::CrossProduct(RayDirection, Edge2);
    float Det = FVector::DotProduct(Edge1, H);
    
    // Check if ray is parallel to triangle
    if (FMath::Abs(Det) < EPSILON)
        return false;
    
    float InvDet = 1.0f / Det;
    
    // Calculate U parameter
    FVector S = RayOrigin - V0;
    float U = InvDet * FVector::DotProduct(S, H);
    
    // Check if U is outside bounds
    if (U < 0.0f || U > 1.0f)
        return false;
    
    // Calculate V parameter
    FVector Q = FVector::CrossProduct(S, Edge1);
    float V = InvDet * FVector::DotProduct(RayDirection, Q);
    
    // Check if V is outside bounds or U+V > 1
    if (V < 0.0f || U + V > 1.0f)
        return false;
    
    // Calculate T (distance along ray)
    float T = InvDet * FVector::DotProduct(Edge2, Q);
    
    // Verify intersection is in positive direction
    if (T <= EPSILON)
        return false;
    
    // Store results
    OutDistance = T;
    OutIntersectionPoint = RayOrigin + RayDirection * T;
    
    return true;
}

 
bool FMeshProcessor::CompareDepthsForVisibility(
    const TArray<float>& CalculatedDepthBuffer,
    const TArray<float>& CapturedDepthBuffer,
    const TArray<FVector2D>& ScreenPositionBuffer,
    int32 TextureWidth,
    int32 TextureHeight,
    TArray<bool>& OutVisibilityBuffer,
    float DepthTolerance)
{
    const int32 NumTexels = CalculatedDepthBuffer.Num();
    
    // Validate input sizes
    if (NumTexels != ScreenPositionBuffer.Num() || OutVisibilityBuffer.Num() != NumTexels)
    {
        UE_LOG(LogTemp, Warning, TEXT("CompareDepthsForVisibility: Buffer size mismatch"));
        return false;
    }
    
    // Check captured depth buffer size
    if (CapturedDepthBuffer.Num() != TextureWidth * TextureHeight)
    {
        UE_LOG(LogTemp, Warning, TEXT("CompareDepthsForVisibility: Captured depth buffer has wrong size"));
        return false;
    }
    
    int32 OccludedTexels = 0;         // Occluded by another part of the mesh
    int32 OutOfBoundsTexels = 0;       // Outside screen space
    int32 NoGeometryTexels = 0;        // Points at empty space (very far/max depth)
    
    // Define what counts as "no geometry" (adjust based on your scene scale)
    const float NoGeometryThreshold = 50000.0f;
    
    // For each texel in UV space
    for (int32 i = 0; i < NumTexels; i++)
    {
        // Skip texels that are already marked as invisible
        if (!OutVisibilityBuffer[i])
        {
            continue;
        }
        
        // Get the screen position for this UV texel
        const FVector2D& ScreenPos = ScreenPositionBuffer[i];
        
        // Check if screen position is valid (within [0,1] range)
        if (ScreenPos.X < 0.0f || ScreenPos.X > 1.0f || ScreenPos.Y < 0.0f || ScreenPos.Y > 1.0f)
        {
            OutVisibilityBuffer[i] = false;
            OutOfBoundsTexels++;
            continue;
        }
        
        // Convert screen position [0,1] to pixel coordinates
        int32 ScreenX = FMath::Clamp(FMath::RoundToInt(ScreenPos.X * TextureWidth), 0, TextureWidth - 1);
        int32 ScreenY = FMath::Clamp(FMath::RoundToInt(ScreenPos.Y * TextureHeight), 0, TextureHeight - 1);
        int32 ScreenIndex = ScreenY * TextureWidth + ScreenX;
        
        // Get depths to compare
        float CalculatedDepth = CalculatedDepthBuffer[i];
        float CapturedDepth = CapturedDepthBuffer[ScreenIndex];
        
        // Check if there's no geometry at this screen position
        if (CapturedDepth > NoGeometryThreshold)
        {
            OutVisibilityBuffer[i] = false;
            NoGeometryTexels++;
            continue;
        }
        
        // Check if this texel is occluded by something closer
        if (CalculatedDepth > CapturedDepth + DepthTolerance)
        {
            OutVisibilityBuffer[i] = false;
            OccludedTexels++;
            continue;
        }
        
        // If we get here, the texel is still considered visible
    }
    
    int32 TotalInvisible = OccludedTexels + OutOfBoundsTexels + NoGeometryTexels;
    int32 VisibleTexels = NumTexels - TotalInvisible;
    
    UE_LOG(LogTemp, Log, TEXT("Depth comparison results:"));
    UE_LOG(LogTemp, Log, TEXT("  Total texels: %d"), NumTexels);
    UE_LOG(LogTemp, Log, TEXT("  Out of bounds: %d (%.1f%%)"), 
           OutOfBoundsTexels, (OutOfBoundsTexels * 100.0f) / NumTexels);
    UE_LOG(LogTemp, Log, TEXT("  No geometry: %d (%.1f%%)"), 
           NoGeometryTexels, (NoGeometryTexels * 100.0f) / NumTexels);
    UE_LOG(LogTemp, Log, TEXT("  Occluded: %d (%.1f%%)"), 
           OccludedTexels, (OccludedTexels * 100.0f) / NumTexels);
    UE_LOG(LogTemp, Log, TEXT("  Visible: %d (%.1f%%)"), 
           VisibleTexels, (VisibleTexels * 100.0f) / NumTexels);
    
    return true;
}

bool FMeshProcessor::ProjectUVsInPerspective(
    UStaticMesh* StaticMesh,
    const FTransform& LocalToWorld,
    const FProjectionSettings& Settings,
    int32 OutputWidth,
    int32 OutputHeight)
{
    // Check if the static mesh is valid
    if (!StaticMesh)
    {
        UE_LOG(LogTemp, Warning, TEXT("Invalid StaticMesh provided for UV projection"));
        return false;
    }

    // Prepare to modify the mesh
    StaticMesh->Modify();
    
    // Get the render data
    FStaticMeshRenderData* RenderData = StaticMesh->GetRenderData();
    if (!RenderData || RenderData->LODResources.Num() == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("StaticMesh has no render data"));
        return false;
    }

    // Work with LOD 0
    FStaticMeshLODResources& LODResource = RenderData->LODResources[0];
    
    // Create a minimal view info structure with camera settings
    FMinimalViewInfo CameraView;
    CameraView.Location = Settings.CameraPosition;
    
    // Calculate camera rotation to look at mesh center if no specific rotation is set
    if (Settings.CameraRotation.IsNearlyZero())
    {
        // Calculate mesh bounds in world space
        FBoxSphereBounds Bounds;
        if (LocalToWorld.GetScale3D().IsNearlyZero())
        {
            // Use local bounds if no transform provided
            Bounds = StaticMesh->GetBounds();
        }
        else
        {
            // Transform bounds to world space
            Bounds = StaticMesh->GetBounds().TransformBy(LocalToWorld);
        }
        
        FVector LookAtLocation = Bounds.Origin;
        CameraView.Rotation = (LookAtLocation - Settings.CameraPosition).Rotation();
    }
    else
    {
        CameraView.Rotation = FRotator(Settings.CameraRotation);
    }
    
    CameraView.FOV = Settings.FOVAngle;
    CameraView.AspectRatio = (float)OutputWidth / (float)OutputHeight;
    CameraView.ProjectionMode = ECameraProjectionMode::Perspective;
    CameraView.OrthoWidth = OutputWidth;
    CameraView.OrthoNearClipPlane = 1.0f;
    CameraView.OrthoFarClipPlane = 10000.0f;
    CameraView.bConstrainAspectRatio = true;

    // Get the view, projection and combined matrices
    FMatrix ViewMatrix;
    FMatrix ProjectionMatrix;
    FMatrix ViewProjectionMatrix;

    UGameplayStatics::GetViewProjectionMatrix(CameraView, ViewMatrix, ProjectionMatrix, ViewProjectionMatrix);
    
    UE_LOG(LogTemp, Log, TEXT("UV Projection: Camera position: %s, rotation: %s, FOV: %.1f°"),
        *CameraView.Location.ToString(), *CameraView.Rotation.ToString(), CameraView.FOV);
    
    // Log original UVs for debugging
    UE_LOG(LogTemp, Log, TEXT("Original UVs for first 5 vertices:"));
    for (uint32 i = 0; i < FMath::Min(5U, LODResource.VertexBuffers.PositionVertexBuffer.GetNumVertices()); i++)
    {
        FVector2f OrigUV = LODResource.VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(i, 0);
        UE_LOG(LogTemp, Log, TEXT("Vertex %d: UV=(%.4f, %.4f)"), i, OrigUV.X, OrigUV.Y);
    }
    
    // Process each vertex
    uint32 NumVertices = LODResource.VertexBuffers.PositionVertexBuffer.GetNumVertices();
    uint32 SkippedVertices = 0;

    for (uint32 i = 0; i < NumVertices; i++)
    {
        // Get vertex position in local space
        FVector3f LocalPos3f = LODResource.VertexBuffers.PositionVertexBuffer.VertexPosition(i);
        FVector LocalPos(LocalPos3f.X, LocalPos3f.Y, LocalPos3f.Z);
        
        // Transform to world space
        FVector WorldPos = LocalToWorld.TransformPosition(LocalPos);
        
        // Transform to view space
        FVector4 ViewPos = ViewMatrix.TransformPosition(WorldPos);
        
        // Transform to clip space
        FVector4 ClipPos = ProjectionMatrix.TransformPosition(ViewPos);
        
        // Log for debugging first few vertices
        if (i < 5)
        {
            UE_LOG(LogTemp, Log, TEXT("Vertex %d: World=(%f,%f,%f), Clip=(%f,%f,%f,%f)"), 
                i, WorldPos.X, WorldPos.Y, WorldPos.Z, 
                ClipPos.X, ClipPos.Y, ClipPos.Z, ClipPos.W);
        }
        
        // Skip vertices behind the camera
        if (ClipPos.W <= 0.0f)
        {
            if (i < 5)
            {
                UE_LOG(LogTemp, Log, TEXT("Vertex %d: SKIPPED (behind camera)"), i);
            }
            SkippedVertices++;
            continue;
        }
        
        // Perform perspective division
        float InvW = 1.0f / ClipPos.W;
        FVector NDCCoords(
            ClipPos.X * InvW,
            ClipPos.Y * InvW,
            ClipPos.Z * InvW
        );
        
        // Convert to screen coordinates [0,1]
        FVector2D ScreenPos = FVector2D(
            (NDCCoords.X * 0.5f + 0.5f),
            (1.0f - (NDCCoords.Y * 0.5f + 0.5f))
        );
        
        // Set the UV in the vertex buffer
        FVector2f UV2f(ScreenPos.X, ScreenPos.Y);
        LODResource.VertexBuffers.StaticMeshVertexBuffer.SetVertexUV(i, 0, UV2f);
        
        // Log for debugging
        if (i < 5)
        {
            UE_LOG(LogTemp, Log, TEXT("Vertex %d: NDC=(%f,%f,%f), UV=(%f,%f)"), 
                i, NDCCoords.X, NDCCoords.Y, NDCCoords.Z, ScreenPos.X, ScreenPos.Y);
        }
    }
    
    // Log new UVs for debugging
    UE_LOG(LogTemp, Log, TEXT("New UVs after projection for first 5 vertices:"));
    for (uint32 i = 0; i < FMath::Min(5U, LODResource.VertexBuffers.PositionVertexBuffer.GetNumVertices()); i++)
    {
        FVector2f NewUV = LODResource.VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(i, 0);
        UE_LOG(LogTemp, Log, TEXT("Vertex %d: UV=(%.4f, %.4f)"), i, NewUV.X, NewUV.Y);
    }
    
    // Mark the mesh as modified
    StaticMesh->MarkPackageDirty();
    
    UE_LOG(LogTemp, Log, TEXT("Perspective UV projection completed. Processed %d vertices (%d skipped)"), 
        NumVertices, SkippedVertices);
    
    return true;
}


void FMeshProcessor::IdentifyFrontFacingTriangles(
    UStaticMesh* StaticMesh,
    const FTransform& LocalToWorld,
    const FProjectionSettings& Settings, 
    const FVector& CameraPosition,
    TArray<bool>& OutTriangleFrontFacing,
    TSet<uint32>& OutFrontFacingVertices)
{
    if (!StaticMesh || !StaticMesh->GetRenderData() || StaticMesh->GetRenderData()->LODResources.Num() == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("IdentifyFrontFacingTriangles: Invalid static mesh data"));
        return;
    }
    
    // Get LOD 0 of the static mesh (highest detail level)
    const FStaticMeshLODResources& LODResource = StaticMesh->GetRenderData()->LODResources[0];
    
    // Get access to index buffer and vertex buffer
    FIndexArrayView Indices = LODResource.IndexBuffer.GetArrayView();
    int32 NumTriangles = Indices.Num() / 3;
    
    // Initialize the output array
    OutTriangleFrontFacing.Empty(NumTriangles);
    OutTriangleFrontFacing.AddZeroed(NumTriangles);
    
    // Clear the output vertices set
    OutFrontFacingVertices.Empty();
    
// 1. Get the target material slot and the corresponding mesh section
    const int32 TargetSlotIndex = Settings.TargetMaterialSlotIndex;
    if (!LODResource.Sections.IsValidIndex(TargetSlotIndex))
    {
        UE_LOG(LogTemp, Error, TEXT("IdentifyFrontFacingTriangles: Invalid TargetMaterialSlotIndex %d."), TargetSlotIndex);
        return;
    }
    const FStaticMeshSection& Section = LODResource.Sections[TargetSlotIndex];

    uint32 FrontFacingCount = 0;
    
    // 2. Modify the loop to iterate only over triangles in the target section
    const uint32 FirstIndex = Section.FirstIndex;
    const uint32 LastIndex = FirstIndex + (Section.NumTriangles * 3);

    for (uint32 i = FirstIndex; i < LastIndex; i += 3)
    {
        // 3. We still need the original triangle index for the output array
        const int32 TriIndex = i / 3;

        // Get triangle indices directly from the main buffer
        uint32 Idx0 = Indices[i + 0];
        uint32 Idx1 = Indices[i + 1];
        uint32 Idx2 = Indices[i + 2];
        
        // Get vertex positions in local space
        FVector3f LocalPos0F = LODResource.VertexBuffers.PositionVertexBuffer.VertexPosition(Idx0);
        FVector3f LocalPos1F = LODResource.VertexBuffers.PositionVertexBuffer.VertexPosition(Idx1);
        FVector3f LocalPos2F = LODResource.VertexBuffers.PositionVertexBuffer.VertexPosition(Idx2);
        
        // Convert to FVector
        FVector LocalPos0(LocalPos0F.X, LocalPos0F.Y, LocalPos0F.Z);
        FVector LocalPos1(LocalPos1F.X, LocalPos1F.Y, LocalPos1F.Z);
        FVector LocalPos2(LocalPos2F.X, LocalPos2F.Y, LocalPos2F.Z);
        
        // Transform to world space
        FVector WorldPos0 = LocalToWorld.TransformPosition(LocalPos0);
        FVector WorldPos1 = LocalToWorld.TransformPosition(LocalPos1);
        FVector WorldPos2 = LocalToWorld.TransformPosition(LocalPos2);
        
        // Calculate triangle normal in world space
        FVector Edge01 = WorldPos1 - WorldPos0;
        FVector Edge02 = WorldPos2 - WorldPos0;
        FVector TriangleNormal = FVector::CrossProduct(Edge01, Edge02).GetSafeNormal();
        
        // Calculate view direction from camera to triangle center
        FVector TriangleCenter = (WorldPos0 + WorldPos1 + WorldPos2) / 3.0f;
        FVector ViewDirection = (TriangleCenter - CameraPosition).GetSafeNormal();
        
        // Determine if triangle is front-facing (dot product is negative when facing camera)
        float DotProduct = FVector::DotProduct(TriangleNormal, ViewDirection);
        bool bIsFrontFacing = (DotProduct <= 0.0f);
        
        // Store the result
        OutTriangleFrontFacing[TriIndex] = bIsFrontFacing;
        
        if (bIsFrontFacing)
        {
            OutFrontFacingVertices.Add(Idx0);
            OutFrontFacingVertices.Add(Idx1);
            OutFrontFacingVertices.Add(Idx2);
            FrontFacingCount++;
        }
    }

    UE_LOG(LogTemp, Log, TEXT("Front-facing triangles in slot %d: %d out of %d"), 
        TargetSlotIndex, FrontFacingCount, Section.NumTriangles);
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

int32 FMeshProcessor::CalculateNormalWeights(
    UStaticMesh* StaticMesh,
    const FTransform& LocalToWorld,
    const FVector& CameraPosition,
    const FProjectionSettings& Settings,
    int32 TextureWidth,
    int32 TextureHeight,
    const TArray<FVector>& WorldPositionBuffer,
    const TArray<FVector2D>& ScreenPositionBuffer,
    TArray<bool>& OutVisibilityBuffer,
    TArray<float>& OutNormalWeightBuffer,
    TArray<FColor>* NormalColorBuffer,
    int32 UVChannel,
    const TSet<int32>& HiddenSlotIndices)
{
    UE_LOG(LogTemp, Log, TEXT("CalculateNormalWeights: FadeStartAngle=%.2f, EdgeFalloff=%.2f"), 
           Settings.FadeStartAngle, Settings.EdgeFalloff);

    // --- Initial Setup & Validation ---
    if (!StaticMesh || !StaticMesh->GetRenderData() || 
        StaticMesh->GetRenderData()->LODResources.Num() == 0) return 0;

    const FStaticMeshLODResources& LODModel = StaticMesh->GetRenderData()->LODResources[0];
    const FIndexArrayView Indices = LODModel.IndexBuffer.GetArrayView();
    const FStaticMeshVertexBuffer& VertexBuffer = LODModel.VertexBuffers.StaticMeshVertexBuffer;
    const FPositionVertexBuffer& PositionBuffer = LODModel.VertexBuffers.PositionVertexBuffer;

    int32 ProcessedTexels = 0;
    int32 TotalTexels = TextureWidth * TextureHeight;

    // Initialize output buffers
    OutNormalWeightBuffer.Init(0.0f, TotalTexels);
    OutVisibilityBuffer.Init(false, TotalTexels);

    if (NormalColorBuffer)
    {
        NormalColorBuffer->Init(FColor::Black, TotalTexels);
    }

    // Pre-calculate angle cosines
    const float StartAngleCos = FMath::Cos(FMath::DegreesToRadians(Settings.FadeStartAngle));
    const float EndAngleCos = FMath::Cos(FMath::DegreesToRadians(90.0f));

    const bool bIsBinaryMode = FMath::IsNearlyZero(Settings.EdgeFalloff);
    if(bIsBinaryMode) 
    { 
        UE_LOG(LogTemp, Log, TEXT("CalculateNormalWeights: BINARY mode. Cutoff at %.2f deg"), 
               Settings.FadeStartAngle); 
    }
    else 
    { 
        UE_LOG(LogTemp, Log, TEXT("CalculateNormalWeights: SOFT FADE mode (EdgeFalloff=%.2f)"), 
               Settings.EdgeFalloff); 
    }

    // --- Triangle Iteration ---
    for (const FStaticMeshSection& Section : LODModel.Sections)
    {
        if (Section.MaterialIndex != Settings.TargetMaterialSlotIndex || 
            HiddenSlotIndices.Contains(Section.MaterialIndex))
        {
            continue;
        }

        const uint32 FirstIndex = Section.FirstIndex;
        const uint32 LastIndex = FirstIndex + (Section.NumTriangles * 3);

        for (uint32 i = FirstIndex; i < LastIndex; i += 3)
        {
            uint32 Index0 = Indices[i + 0];
            uint32 Index1 = Indices[i + 1];
            uint32 Index2 = Indices[i + 2];

            // Get triangle UVs
            const FVector2D UV0(VertexBuffer.GetVertexUV(Index0, UVChannel));
            const FVector2D UV1(VertexBuffer.GetVertexUV(Index1, UVChannel));
            const FVector2D UV2(VertexBuffer.GetVertexUV(Index2, UVChannel));

            // Get vertex normals and transform to world space
            FVector Norm0 = LocalToWorld.TransformVector(FVector(VertexBuffer.VertexTangentZ(Index0))).GetSafeNormal();
            FVector Norm1 = LocalToWorld.TransformVector(FVector(VertexBuffer.VertexTangentZ(Index1))).GetSafeNormal();
            FVector Norm2 = LocalToWorld.TransformVector(FVector(VertexBuffer.VertexTangentZ(Index2))).GetSafeNormal();

            // Simple backface culling
            FVector FaceNormal = (Norm0 + Norm1 + Norm2).GetSafeNormal();
            FVector P0_World = LocalToWorld.TransformPosition(FVector(PositionBuffer.VertexPosition(Index0)));
            if (FVector::DotProduct(FaceNormal, (CameraPosition - P0_World).GetSafeNormal()) <= 0.0f)
            {
                continue;
            }

            // Texel rasterization bounding box
            int32 MinX = FMath::Clamp(FMath::FloorToInt(FMath::Min3(UV0.X, UV1.X, UV2.X) * TextureWidth), 0, TextureWidth - 1);
            int32 MinY = FMath::Clamp(FMath::FloorToInt(FMath::Min3(UV0.Y, UV1.Y, UV2.Y) * TextureHeight), 0, TextureHeight - 1);
            int32 MaxX = FMath::Clamp(FMath::CeilToInt(FMath::Max3(UV0.X, UV1.X, UV2.X) * TextureWidth), 0, TextureWidth - 1);
            int32 MaxY = FMath::Clamp(FMath::CeilToInt(FMath::Max3(UV0.Y, UV1.Y, UV2.Y) * TextureHeight), 0, TextureHeight - 1);

            for (int32 Y = MinY; Y <= MaxY; Y++)
            {
                for (int32 X = MinX; X <= MaxX; X++)
                {
                    float UCoord = (static_cast<float>(X) + 0.5f) / TextureWidth;
                    float VCoord = (static_cast<float>(Y) + 0.5f) / TextureHeight;
                    FVector2D TexelUV(UCoord, VCoord);

                    float BaryA, BaryB, BaryC;
                    if (FMathUtils::CalculateBarycentricCoordinates(TexelUV, UV0, UV1, UV2, BaryA, BaryB, BaryC) && 
                        (BaryA >= -KINDA_SMALL_NUMBER && BaryB >= -KINDA_SMALL_NUMBER && BaryC >= -KINDA_SMALL_NUMBER))
                    {
                        int32 TexelIndex = Y * TextureWidth + X;

                        // Check if world position is valid
                        if (WorldPositionBuffer[TexelIndex].IsNearlyZero()) continue;

                        // Screen bounds check
                        const FVector2D& ScreenPos = ScreenPositionBuffer[TexelIndex];
                        if (ScreenPos.X < 0.0f || ScreenPos.X > 1.0f || 
                            ScreenPos.Y < 0.0f || ScreenPos.Y > 1.0f || 
                            FMath::IsNaN(ScreenPos.X) || FMath::IsNaN(ScreenPos.Y))
                        {
                            continue;
                        }

                        // Interpolate normal at the texel
                        FVector TexelNormal = (Norm0 * BaryA + Norm1 * BaryB + Norm2 * BaryC).GetSafeNormal();
                        const FVector& TexelWorldPos = WorldPositionBuffer[TexelIndex];
                        FVector ViewVector = (CameraPosition - TexelWorldPos).GetSafeNormal();

                        // Calculate Dot Product
                        const float DotProduct = FMath::Max(0.0f, FVector::DotProduct(TexelNormal, ViewVector));

                        // --- Weight Calculation ---
                        float Weight = 0.0f;

                        if (bIsBinaryMode)
                        {
                            if (DotProduct >= StartAngleCos)
                            {
                                Weight = 1.0f;
                            }
                        }
                        else // Soft Fade Mode
                        {
                            const float LinearWeight = FMath::Clamp(
                                FMath::GetRangePct(EndAngleCos, StartAngleCos, DotProduct),
                                0.0f, 1.0f
                            );
                            Weight = FMath::Pow(LinearWeight, Settings.EdgeFalloff);
                        }

                        // Update buffers if this triangle gives a higher weight
                        if (Weight > OutNormalWeightBuffer[TexelIndex])
                        {
                            OutNormalWeightBuffer[TexelIndex] = Weight;
                            OutVisibilityBuffer[TexelIndex] = true;

                            if (NormalColorBuffer)
                            {
                                (*NormalColorBuffer)[TexelIndex] = EncodeUnitVectorToRGB(TexelNormal);
                            }
                            ProcessedTexels++;
                        }
                    }
                }
            }
        }
    }


    UE_LOG(LogTemp, Log, TEXT("CalculateNormalWeights: Completed. %d visible texels, %.1f%% coverage"),
           ProcessedTexels,
           (float)ProcessedTexels * 100.0f / (float)TotalTexels);

    return ProcessedTexels;
}

FColor FMeshProcessor::SampleTextureBilinearRaw(
    const FColor* Src, int32 W, int32 H, const FVector2D& UV01)
{
    if (!Src || W <= 0 || H <= 0) return FColor::Magenta;

    // Clamp UV to [0,1] then scale to texel space
    const float x = FMath::Clamp(UV01.X, 0.0f, 1.0f) * (W - 1);
    const float y = FMath::Clamp(UV01.Y, 0.0f, 1.0f) * (H - 1);

    const int32 x0 = FMath::FloorToInt(x);
    const int32 y0 = FMath::FloorToInt(y);
    const int32 x1 = FMath::Min(x0 + 1, W - 1);
    const int32 y1 = FMath::Min(y0 + 1, H - 1);

    const float fx = x - x0;
    const float fy = y - y0;

    const FColor& C00 = Src[y0 * W + x0];
    const FColor& C10 = Src[y0 * W + x1];
    const FColor& C01 = Src[y1 * W + x0];
    const FColor& C11 = Src[y1 * W + x1];

    float w00 = (1.0f - fx) * (1.0f - fy);
    float w10 = (       fx) * (1.0f - fy);
    float w01 = (1.0f - fx) * (       fy);
    float w11 = (       fx) * (       fy);

    // Zero weights if alpha==0
    if (C00.A == 0) w00 = 0.0f;
    if (C10.A == 0) w10 = 0.0f;
    if (C01.A == 0) w01 = 0.0f;
    if (C11.A == 0) w11 = 0.0f;

    const float totalW = w00 + w10 + w01 + w11;
    if (totalW <= 0.0f)
    {
        // all 4 texels fully transparent
        return FColor(0, 0, 0, 0);
    }

    const float invW = 1.0f / totalW;

    auto Blend = [&](uint8 a, uint8 b, uint8 c, uint8 d) -> uint8
    {
        return (uint8)FMath::Clamp(
            FMath::RoundToInt((w00 * a + w10 * b + w01 * c + w11 * d) * invW),
            0, 255);
    };

    return FColor(
        Blend(C00.R, C10.R, C01.R, C11.R),
        Blend(C00.G, C10.G, C01.G, C11.G),
        Blend(C00.B, C10.B, C01.B, C11.B),
        // Alpha: resample with the same rule (renormalized)
        Blend(C00.A, C10.A, C01.A, C11.A));
}

#include "Editor.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Engine/Canvas.h"
#include "UObject/SavePackage.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"
#include "Helpers/TextureUtils.h"

/**
 * @brief (In MeshProcessor.cpp) Saves a UTexture2D as a new .uasset.
 * This is the STATIC, FORMAT-AWARE version.
 * It detects if the input is 8-bit sRGB or 16-bit HDR and saves it correctly.
 */
static void TD3D_Debug_SaveTexToAsset(UTexture2D* TextureToSave, const FString& BaseAssetName)
{
    if (!TextureToSave || !TextureToSave->GetPlatformData())
    {
        UE_LOG(LogTemp, Error, TEXT("TD3D_Debug_SaveTexToAsset (static): TextureToSave is null or has no PlatformData for %s."), *BaseAssetName);
        return;
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        UE_LOG(LogTemp, Error, TEXT("TD3D_Debug_SaveTexToAsset (static): Cannot get World."));
        return;
    }

    const int32 W = TextureToSave->GetSizeX();
    const int32 H = TextureToSave->GetSizeY();
    if (W <= 0 || H <= 0)
    {
        UE_LOG(LogTemp, Error, TEXT("TD3D_Debug_SaveTexToAsset (static): Texture %s has invalid dimensions (%dx%d)."), *TextureToSave->GetName(), W, H);
        return;
    }

    // --- Asset Naming ---
    const FString BasePath = TEXT("/Game/TD3D_Debug/Rasterizer/");
    const FString AssetName = FString::Printf(TEXT("%s_%s"), *BaseAssetName, *FDateTime::Now().ToString(TEXT("HHMMSS")));
    const FString PackagePath = BasePath + AssetName;

    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package) { UE_LOG(LogTemp, Error, TEXT("TD3D_Debug_SaveTexToAsset (static): Failed to create package %s"), *PackagePath); return; }
    Package->FullyLoad();

    UTexture2D* NewStaticTexture = NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone);
    if (!NewStaticTexture) { UE_LOG(LogTemp, Error, TEXT("TD3D_Debug_SaveTexToAsset (static): Failed to create NewObject UTexture2D.")); return; }

    NewStaticTexture->AddToRoot(); // Protect during setup
    
    // --- NEW FORMAT-AWARE LOGIC ---
    const EPixelFormat SourceFormat = TextureToSave->GetPlatformData()->PixelFormat;

    if (SourceFormat == PF_FloatRGBA || SourceFormat == PF_A32B32G32R32F)
    {
        // --- PATH A: 16-bit or 32-bit HDR (PF_FloatRGBA / PF_A32B32G32R32F) ---
        UE_LOG(LogTemp, Log, TEXT("TD3D_Debug_SaveTexToAsset (static): Detected 16/32-bit HDR (%s). Saving as HDR."), GPixelFormats[SourceFormat].Name);

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
        UE_LOG(LogTemp, Log, TEXT("TD3D_Debug_SaveTexToAsset (static): Detected 8-bit (or unknown) format (%s). Saving as sRGB."), GPixelFormats[SourceFormat].Name);

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
            
            // *** THE FIX IS HERE ***
            // SE_BLEND_AlphaComposite premultiplies the RGB values when drawing to a transparent target.
            // SE_BLEND_Opaque just copies the (R,G,B,A) channels directly, which is what we want.
            TileItem.BlendMode = SE_BLEND_Opaque; // Was SE_BLEND_AlphaComposite
            
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
    }
    else 
    { 
        UE_LOG(LogTemp, Error, TEXT("TD3D_Debug_SaveTexToAsset (static): Failed to save package: %s"), *PackagePath); 
    }
    
    NewStaticTexture->RemoveFromRoot(); // Unprotect
}


/**
 * (In FMeshProcessor.cpp)
 * Performs a full perspective-correct rasterization.
 * This version is 16-bit HDR-correct. It reads 16-bit (PF_FloatRGBA) input textures,
* performs all sampling and interpolation in 32-bit Linear space,
 * and outputs a 32-bit (TArray<FLinearColor>) pixel buffer.
 */
/**
 * (In FMeshProcessor.cpp)
 * Performs a full perspective-correct rasterization.
 * This version is 16-bit HDR-correct. It reads 16-bit (PF_FloatRGBA) input textures,
 * performs all sampling and interpolation in 32-bit Linear space,
 * and outputs a 32-bit (TArray<FLinearColor>) pixel buffer.
 * 
 * FIXED: Now does per-pixel texture sampling with perspective-correct UV interpolation
 */
bool FMeshProcessor::GenerateRasterizedView(
    TArray<FLinearColor>& OutImageBuffer,
    AStaticMeshActor* Actor,
    const FProjectionSettings& CameraSettings,
    const TMap<int32, TObjectPtr<UTexture2D>>& AllSlotTextures,
    int32 OutputWidth,
    int32 OutputHeight,
    int32 UVChannel)
{
    // --- 1. SETUP & VALIDATION ---
    UE_LOG(LogTemp, Warning, TEXT("--- GenerateRasterizedView START (16-bit HDR Path) ---"));
    UE_LOG(LogTemp, Warning, TEXT("Output: %dx%d, NumSlotTextures: %d"), OutputWidth, OutputHeight, AllSlotTextures.Num());

    if (!Actor || !Actor->GetStaticMeshComponent() || !Actor->GetStaticMeshComponent()->GetStaticMesh())
    {
        UE_LOG(LogTemp, Error, TEXT(" -> ABORT: Invalid actor or mesh."));
        return false;
    }
    if (OutputWidth <= 0 || OutputHeight <= 0)
    {
        UE_LOG(LogTemp, Error, TEXT(" -> ABORT: Invalid output dimensions."));
        return false;
    }

    // Always use 1024x1024 for now (As per your original code comment)
    OutputWidth = 1024;
    OutputHeight = 1024;

    UStaticMesh* StaticMesh = Actor->GetStaticMeshComponent()->GetStaticMesh();
    const FTransform MeshXForm = Actor->GetStaticMeshComponent()->GetComponentTransform();

    // --- 2. INITIALIZE BUFFERS & COUNTERS ---
    OutImageBuffer.Init(FLinearColor::Transparent, OutputWidth * OutputHeight);
    TArray<double> ZBuffer;
    ZBuffer.Init(DBL_MAX, OutputWidth * OutputHeight);

    int32 TotalTrianglesProcessed = 0;
    int32 TotalTrianglesPassedCulling = 0;
    int32 TotalTrianglesSurvivedClipping = 0;
    int32 TotalPixelsWritten = 0;

    auto* RenderData = StaticMesh->GetRenderData();
    if (!RenderData || RenderData->LODResources.Num() == 0)
    {
        UE_LOG(LogTemp, Error, TEXT(" -> ABORT: No LOD0 RenderData found."));
        return false;
    }
    auto& LOD = RenderData->LODResources[0];
    auto& PosBuf = LOD.VertexBuffers.PositionVertexBuffer;
    auto& VertBuf = LOD.VertexBuffers.StaticMeshVertexBuffer; // Renamed for clarity (contains UVs, Normals, Tangents)
    auto Idxs = LOD.IndexBuffer.GetArrayView();

    if (UVChannel >= (int32)VertBuf.GetNumTexCoords()) {
         UE_LOG(LogTemp, Error, TEXT(" -> ABORT: Invalid UVChannel %d requested (Max: %d)."), UVChannel, VertBuf.GetNumTexCoords() - 1);
         return false;
    }

    FMinimalViewInfo VI;
    VI.Location = CameraSettings.CameraPosition; VI.Rotation = CameraSettings.CameraRotation; VI.FOV = CameraSettings.FOVAngle;
    VI.AspectRatio = (float)OutputWidth / (float)OutputHeight; VI.ProjectionMode = ECameraProjectionMode::Perspective;
    FMatrix ViewM, ProjM, ViewProjM;
    UGameplayStatics::GetViewProjectionMatrix(VI, ViewM, ProjM, ViewProjM);

    const float AngleThresholdDegrees = 10.0f; // Backface culling threshold angle
    const float CosThreshold = FMath::Cos(FMath::DegreesToRadians(90.0f - AngleThresholdDegrees));

    // --- 3. MULTI-PASS RASTERIZATION ---
    for (int32 SectionIdx = 0; SectionIdx < LOD.Sections.Num(); ++SectionIdx)
    {
        UE_LOG(LogTemp, Log, TEXT("PASS %d/%d: Processing Material Slot %d"), SectionIdx + 1, LOD.Sections.Num(), SectionIdx);

        // --- Texture Data Setup (16-bit HDR) ---
        const FFloat16Color* SourceTextureDataPtr = nullptr; // Pointer to 16-bit float data
        TArray<FFloat16Color> SourceTextureView_HDR;       // Temporary TArray for 16-bit float sampler
        int32 SourceTexW = 0;
        int32 SourceTexH = 0;
        bool bHasTextureForSlot = false;
        FTexture2DMipMap* MipPtr = nullptr; // Keep track of the mip to unlock later

        const TObjectPtr<UTexture2D>* FoundTexture = AllSlotTextures.Find(SectionIdx);
        if (FoundTexture && IsValid(*FoundTexture))
        {
            UTexture2D* TextureToApply = *FoundTexture;
            FTexturePlatformData* PlatformData = TextureToApply->GetPlatformData();

            // --- DEBUG: SAVE INPUT TEXTURE ---
            // This static function is now format-aware
            TD3D_Debug_SaveTexToAsset(TextureToApply, FString::Printf(TEXT("RasterIn_Slot%d"), SectionIdx));
            // --- END DEBUG ---

            // Check for 16-bit float format
            if (PlatformData && PlatformData->Mips.Num() > 0 && PlatformData->PixelFormat == PF_FloatRGBA)
            {
                MipPtr = &PlatformData->Mips[0]; // Get pointer to the mip
                // Cast to 16-bit float pointer
                SourceTextureDataPtr = static_cast<const FFloat16Color*>(MipPtr->BulkData.LockReadOnly());
                SourceTexW = MipPtr->SizeX;
                SourceTexH = MipPtr->SizeY;

                if (SourceTextureDataPtr)
                {
                    // Copy 16-bit data for the sampler function
                    SourceTextureView_HDR.Append(SourceTextureDataPtr, SourceTexW * SourceTexH);
                    bHasTextureForSlot = true;
                    UE_LOG(LogTemp, Log, TEXT("  -> Using 16-bit HDR Texture: %s (%dx%d)"), *TextureToApply->GetName(), SourceTexW, SourceTexH);
                }
                else
                {
                    UE_LOG(LogTemp, Warning, TEXT("  -> Failed to lock 16-bit texture data for slot %d."), SectionIdx);
                    // Attempt unlock even if lock failed
                    MipPtr->BulkData.Unlock();
                    MipPtr = nullptr; // Nullify mip pointer as we unlocked
                }
            }
             else {
                 UE_LOG(LogTemp, Warning, TEXT("  -> Texture for slot %d invalid or wrong format (Not PF_FloatRGBA)."), SectionIdx);
            }
        }

        if (!bHasTextureForSlot)
        {
            UE_LOG(LogTemp, Log, TEXT("  -> No valid texture found for this slot. Skipping pass."));
            continue; // Skip this section if no valid texture
        }

        // --- Triangle Iteration ---
        const FStaticMeshSection& Section = LOD.Sections[SectionIdx];
        const uint32 FirstIndex = Section.FirstIndex;
        const uint32 NumTrianglesInSection = Section.NumTriangles;
        const uint32 EndIndex = FirstIndex + (NumTrianglesInSection * 3);

        for (uint32 i = FirstIndex; i < EndIndex; i += 3)
        {
          	TotalTrianglesProcessed++;
          	uint32 i0 = Idxs[i + 0], i1 = Idxs[i + 1], i2 = Idxs[i + 2];

          	// --- Vertex Data Fetching ---
          	FVector P0_world = MeshXForm.TransformPosition(FVector(PosBuf.VertexPosition(i0)));
          	FVector P1_world = MeshXForm.TransformPosition(FVector(PosBuf.VertexPosition(i1)));
          	FVector P2_world = MeshXForm.TransformPosition(FVector(PosBuf.VertexPosition(i2)));

          	FVector2D UV0(VertBuf.GetVertexUV(i0, UVChannel));
          	FVector2D UV1(VertBuf.GetVertexUV(i1, UVChannel));
          	FVector2D UV2(VertBuf.GetVertexUV(i2, UVChannel));

          	// --- Backface Culling (Using Averaged Vertex Normals) ---
          	const FVector3f N0f = VertBuf.VertexTangentZ(i0); // Assuming TangentZ is the normal
          	const FVector3f N1f = VertBuf.VertexTangentZ(i1);
          	const FVector3f N2f = VertBuf.VertexTangentZ(i2);
          	FVector N0 = MeshXForm.TransformVectorNoScale(FVector(N0f.X, N0f.Y, N0f.Z)).GetSafeNormal();
          	FVector N1 = MeshXForm.TransformVectorNoScale(FVector(N1f.X, N1f.Y, N1f.Z)).GetSafeNormal();
    	    FVector N2 = MeshXForm.TransformVectorNoScale(FVector(N2f.X, N2f.Y, N2f.Z)).GetSafeNormal();
          	FVector FaceNormalAvg = (N0 + N1 + N2).GetSafeNormal(); // Use averaged normal for culling
          	FVector ViewVector = (CameraSettings.CameraPosition - P0_world).GetSafeNormal(); // View from V0

          	if (FVector::DotProduct(FaceNormalAvg, ViewVector) < CosThreshold) continue; // Skip back-facing
          	TotalTrianglesPassedCulling++;

          	// --- NO LONGER SAMPLE AT VERTICES - We'll sample per-pixel instead ---
          	// REMOVED: FLinearColor L0_Linear = FTextureUtils::SampleTextureBilinear_HDR(...)
          	// REMOVED: FLinearColor L1_Linear = FTextureUtils::SampleTextureBilinear_HDR(...)
          	// REMOVED: FLinearColor L2_Linear = FTextureUtils::SampleTextureBilinear_HDR(...)

          	// --- Clipping ---
          	TArray<FClippingVertex> OriginalPolygon;
          	OriginalPolygon.SetNum(3);
          	OriginalPolygon[0].Position = ViewProjM.TransformPosition(P0_world);
          	OriginalPolygon[0].WorldPosition = P0_world;
          	OriginalPolygon[0].UV = UV0;
          	// Don't store Color - we'll sample per-pixel

          	OriginalPolygon[1].Position = ViewProjM.TransformPosition(P1_world);
          	OriginalPolygon[1].WorldPosition = P1_world;
          	OriginalPolygon[1].UV = UV1;

          	OriginalPolygon[2].Position = ViewProjM.TransformPosition(P2_world);
          	OriginalPolygon[2].WorldPosition = P2_world;
          	OriginalPolygon[2].UV = UV2;

          	TArray<FClippingVertex> ClippedPolygon = OriginalPolygon;
          	ClippedPolygon = ClipPolygonAgainstPlane(ClippedPolygon, 3, 1.0f); // Far
          	ClippedPolygon = ClipPolygonAgainstPlane(ClippedPolygon, 0, 1.0f); // Right
          	ClippedPolygon = ClipPolygonAgainstPlane(ClippedPolygon, 0,-1.0f); // Left
          	ClippedPolygon = ClipPolygonAgainstPlane(ClippedPolygon, 1, 1.0f); // Top
        	ClippedPolygon = ClipPolygonAgainstPlane(ClippedPolygon, 1,-1.0f); // Bottom
          	ClippedPolygon = ClipPolygonAgainstPlane(ClippedPolygon, 2,-1.0f); // Near W >= -Z
          	if (ClippedPolygon.Num() < 3) continue;
          	TotalTrianglesSurvivedClipping++;

          	// --- Rasterize Clipped Polygon ---
          	for (int32 VertexIdx = 1; VertexIdx < ClippedPolygon.Num() - 1; ++VertexIdx)
          	{
              	const FClippingVertex& V0 = ClippedPolygon[0];
              	const FClippingVertex& V1 = ClippedPolygon[VertexIdx];
              	const FClippingVertex& V2 = ClippedPolygon[VertexIdx + 1];

          	    double W0_inv = 1.0 / V0.Position.W;
              	double W1_inv = 1.0 / V1.Position.W;
              	double W2_inv = 1.0 / V2.Position.W;

              	// Perspective correct UVs (UV / W)
              	FVector2D UV0_over_W = V0.UV * W0_inv;
              	FVector2D UV1_over_W = V1.UV * W1_inv;
              	FVector2D UV2_over_W = V2.UV * W2_inv;

              	// Screen coordinates
              	FVector2D S0((V0.Position.X * W0_inv * 0.5 + 0.5) * OutputWidth, (1.0 - (V0.Position.Y * W0_inv * 0.5 + 0.5)) * OutputHeight);
              	FVector2D S1((V1.Position.X * W1_inv * 0.5 + 0.5) * OutputWidth, (1.0 - (V1.Position.Y * W1_inv * 0.5 + 0.5)) * OutputHeight);
              	FVector2D S2((V2.Position.X * W2_inv * 0.5 + 0.5) * OutputWidth, (1.0 - (V2.Position.Y * W2_inv * 0.5 + 0.5)) * OutputHeight);

              	// Bounding box
              	int32 minX = FMath::Clamp(FMath::FloorToInt(FMath::Min3(S0.X, S1.X, S2.X)), 0, OutputWidth - 1);
              	int32 maxX = FMath::Clamp(FMath::CeilToInt(FMath::Max3(S0.X, S1.X, S2.X)), 0, OutputWidth - 1);
              	int32 minY = FMath::Clamp(FMath::FloorToInt(FMath::Min3(S0.Y, S1.Y, S2.Y)), 0, OutputHeight - 1);
              	int32 maxY = FMath::Clamp(FMath::CeilToInt(FMath::Max3(S0.Y, S1.Y, S2.Y)), 0, OutputHeight - 1);

              	// --- Pixel Loop with Per-Pixel Texture Sampling ---
              	for (int32 y = minY; y <= maxY; ++y) {
                  	for (int32 x = minX; x <= maxX; ++x) {
                      	FVector2D PixelPos(x + 0.5f, y + 0.5f);
                      	float A, B, C; // Barycentric coordinates
                      	if (!FMathUtils::CalculateBarycentricCoordinates(PixelPos, S0, S1, S2, A, B, C) || (A < -1e-9 || B < -1e-9 || C < -1e-9)) continue;

                	    // --- Perspective-Correct UV Interpolation ---
                      	double W_interp_inv = A * W0_inv + B * W1_inv + C * W2_inv;
                      	// Avoid division by zero or near-zero if W_interp_inv is too small
                      	if (FMath::Abs(W_interp_inv) < SMALL_NUMBER) continue;

                      	// Interpolate UV with perspective correction
                      	FVector2D UV_over_W = A * UV0_over_W + B * UV1_over_W + C * UV2_over_W;
                      	FVector2D UV_interp = UV_over_W / W_interp_inv;

                      	// --- Sample Texture at Interpolated UV (Per-Pixel Sampling) ---
                      	FLinearColor FinalLinearColor = FTextureUtils::SampleTextureBilinear_HDR(
                          	SourceTextureView_HDR, SourceTexW, SourceTexH, UV_interp);

                      	// --- Depth Test ---
                      	FVector P_world_interp = (A * V0.WorldPosition * W0_inv + B * V1.WorldPosition * W1_inv + C * V2.WorldPosition * W2_inv) / W_interp_inv;
                      	double DepthValue = FVector::Dist(P_world_interp, CameraSettings.CameraPosition);

                      	int32 idx = y * OutputWidth + x;
                	    if (DepthValue < ZBuffer[idx])
                      	{
                          	ZBuffer[idx] = DepthValue;

                          	// Clamp for safety before output
                          	FinalLinearColor.R = FMath::Clamp(FinalLinearColor.R, 0.0f, 1.0f);
                          	FinalLinearColor.G = FMath::Clamp(FinalLinearColor.G, 0.0f, 1.0f);
                          	FinalLinearColor.B = FMath::Clamp(FinalLinearColor.B, 0.0f, 1.0f);
                          	FinalLinearColor.A = FMath::Clamp(FinalLinearColor.A, 0.0f, 1.0f);

                          	// --- Output is 32-bit Linear Color ---
                	        OutImageBuffer[idx] = FinalLinearColor;

                          	TotalPixelsWritten++;
              	        }
                  	}
              	}
          	}
        } // End triangle loop

        // --- Unlock Texture Data for this Section ---
        if (MipPtr) { // Check if we successfully locked earlier
          	MipPtr->BulkData.Unlock();
        }

    } // End of section loop

    // --- Final Summary Logging ---
    UE_LOG(LogTemp, Warning, TEXT("--- GenerateRasterizedView FINISH ---"));
    UE_LOG(LogTemp, Warning, TEXT("Summary: Total Tris Processed=%d | Passed Culling=%d | Survived Clipping=%d | Pixels Written=%d"),
        TotalTrianglesProcessed, TotalTrianglesPassedCulling, TotalTrianglesSurvivedClipping, TotalPixelsWritten);
    
    // --- DEBUG: SAVE FINAL OUTPUT ---
    UE_LOG(LogTemp, Warning, TEXT("--- DEBUG: Saving Rasterizer FINAL OUTPUT ---"));
    // Convert the 32-bit Linear output to 8-bit sRGB for saving as a viewable PNG
    TArray<FColor> Output_sRGB = FTextureUtils::LinearTo_sRGB(OutImageBuffer);
    
    // Call the sRGB-correct texture creator
    UTexture2D* TransientOutputTex = FTextureUtils::CreateTextureFromsRGBPixelData(OutputWidth, OutputHeight, Output_sRGB);

    if (TransientOutputTex)
    {
      	TransientOutputTex->SetFlags(RF_Transient);
      	TransientOutputTex->UpdateResource();
      	TD3D_Debug_SaveTexToAsset(TransientOutputTex, TEXT("RasterOut_Final"));
       	TransientOutputTex->MarkAsGarbage(); // Clean up the temp texture
    }
    // --- END DEBUG ---

    return true; // Indicate success
}

bool FMeshProcessor::GenerateRasterizedViewSpaceNormals(
    TArray<FColor>& OutImageBuffer,
    AStaticMeshActor* Actor,
    const FProjectionSettings& CameraSettings,
    int32 OutputWidth,
    int32 OutputHeight,
    const TSet<int32>& HiddenSlotIndices) // <-- ADD THIS PARAMETER
{
    // --- 1. SETUP & VALIDATION ---
    UE_LOG(LogTemp, Warning, TEXT("--- GenerateRasterizedViewSpaceNormals START ---"));
    if (!Actor || !Actor->GetStaticMeshComponent() || !Actor->GetStaticMeshComponent()->GetStaticMesh())
    {
        UE_LOG(LogTemp, Error, TEXT(" -> ABORT: Invalid actor provided."));
        return false;
    }
    if (OutputWidth <= 0 || OutputHeight <= 0)
    {
        UE_LOG(LogTemp, Error, TEXT(" -> ABORT: Invalid output dimensions."));
        return false;
    }

    //print hiddenslotindices
    UE_LOG(LogTemp, Warning, TEXT("Hidden Slot Indices:"));
    for (int32 SlotIdx : HiddenSlotIndices)
    {
        UE_LOG(LogTemp, Warning, TEXT(" - %d"), SlotIdx);
    }

    UStaticMesh* StaticMesh = Actor->GetStaticMeshComponent()->GetStaticMesh();
    const FTransform MeshXForm = Actor->GetStaticMeshComponent()->GetComponentTransform();

    // --- 2. INITIALIZE BUFFERS & MATRICES ---
    OutImageBuffer.Init(FColor(128, 128, 255, 255), OutputWidth * OutputHeight);
    TArray<double> ZBuffer;
    ZBuffer.Init(DBL_MAX, OutputWidth * OutputHeight);
    
    auto& LOD = StaticMesh->GetRenderData()->LODResources[0];
    auto& PosBuf = LOD.VertexBuffers.PositionVertexBuffer;
    auto& VertBuf = LOD.VertexBuffers.StaticMeshVertexBuffer;
    auto Idxs = LOD.IndexBuffer.GetArrayView();
    
    FMinimalViewInfo VI;
    VI.Location = CameraSettings.CameraPosition; VI.Rotation = CameraSettings.CameraRotation; VI.FOV = CameraSettings.FOVAngle;
    VI.AspectRatio = (float)OutputWidth / (float)OutputHeight; VI.ProjectionMode = ECameraProjectionMode::Perspective;
    
    FMatrix ViewM, ProjM, ViewProjM;
    UGameplayStatics::GetViewProjectionMatrix(VI, ViewM, ProjM, ViewProjM);

    // --- 3. RASTERIZE TRIANGLES ---
    for (int32 SectionIdx = 0; SectionIdx < LOD.Sections.Num(); ++SectionIdx)
    {
        // If the current section's index is in our set of hidden slots,
        // skip this entire loop iteration.
        if (HiddenSlotIndices.Contains(SectionIdx))
        {
            continue;
        }

        const FStaticMeshSection& Section = LOD.Sections[SectionIdx];
        for (uint32 i = Section.FirstIndex; i < Section.FirstIndex + (Section.NumTriangles * 3); i += 3)
        {
            uint32 i0 = Idxs[i + 0], i1 = Idxs[i + 1], i2 = Idxs[i + 2];
            
            FVector P0_world = MeshXForm.TransformPosition(FVector(PosBuf.VertexPosition(i0)));
            FVector P1_world = MeshXForm.TransformPosition(FVector(PosBuf.VertexPosition(i1)));
            FVector P2_world = MeshXForm.TransformPosition(FVector(PosBuf.VertexPosition(i2)));
            
            FVector N0_world = MeshXForm.TransformVectorNoScale((FVector)VertBuf.VertexTangentZ(i0)).GetSafeNormal();
            FVector N1_world = MeshXForm.TransformVectorNoScale((FVector)VertBuf.VertexTangentZ(i1)).GetSafeNormal();
            FVector N2_world = MeshXForm.TransformVectorNoScale((FVector)VertBuf.VertexTangentZ(i2)).GetSafeNormal();

            const FVector GeometricFaceNormal = FVector::CrossProduct((P1_world - P0_world), (P2_world - P0_world)).GetSafeNormal();
            const FVector ViewVector = (CameraSettings.CameraPosition - P0_world).GetSafeNormal();
            if (FVector::DotProduct(GeometricFaceNormal, ViewVector) >= 0.0f) continue;

            TArray<FClippingVertex> OriginalPolygon;
            OriginalPolygon.SetNum(3);
            OriginalPolygon[0] = { ViewProjM.TransformPosition(P0_world), P0_world, FVector2D(VertBuf.GetVertexUV(i0, 0)), N0_world };
            OriginalPolygon[1] = { ViewProjM.TransformPosition(P1_world), P1_world, FVector2D(VertBuf.GetVertexUV(i1, 0)), N1_world };
            OriginalPolygon[2] = { ViewProjM.TransformPosition(P2_world), P2_world, FVector2D(VertBuf.GetVertexUV(i2, 0)), N2_world };
            
            TArray<FClippingVertex> ClippedPolygon = OriginalPolygon;
            ClippedPolygon = ClipPolygonAgainstPlane(ClippedPolygon, 3, 1.0f);
            ClippedPolygon = ClipPolygonAgainstPlane(ClippedPolygon, 0, 1.0f);
            ClippedPolygon = ClipPolygonAgainstPlane(ClippedPolygon, 0, -1.0f);
            ClippedPolygon = ClipPolygonAgainstPlane(ClippedPolygon, 1, 1.0f);
            ClippedPolygon = ClipPolygonAgainstPlane(ClippedPolygon, 1, -1.0f);
            if (ClippedPolygon.Num() < 3) continue;

            for (int32 VertexIdx = 1; VertexIdx < ClippedPolygon.Num() - 1; ++VertexIdx)
            {
                const FClippingVertex& V0 = ClippedPolygon[0];
                const FClippingVertex& V1 = ClippedPolygon[VertexIdx];
                const FClippingVertex& V2 = ClippedPolygon[VertexIdx + 1];
                
                double W0_inv = 1.0 / V0.Position.W, W1_inv = 1.0 / V1.Position.W, W2_inv = 1.0 / V2.Position.W;
                FVector N0_over_W = V0.WorldNormal * W0_inv;
                FVector N1_over_W = V1.WorldNormal * W1_inv;
                FVector N2_over_W = V2.WorldNormal * W2_inv;

                FVector2D S0((V0.Position.X*W0_inv*0.5+0.5)*OutputWidth, (1.0-(V0.Position.Y*W0_inv*0.5+0.5))*OutputHeight);
                FVector2D S1((V1.Position.X*W1_inv*0.5+0.5)*OutputWidth, (1.0-(V1.Position.Y*W1_inv*0.5+0.5))*OutputHeight);
                FVector2D S2((V2.Position.X*W2_inv*0.5+0.5)*OutputWidth, (1.0-(V2.Position.Y*W2_inv*0.5+0.5))*OutputHeight);
                
                int32 minX = FMath::Clamp(FMath::FloorToInt(FMath::Min3(S0.X, S1.X, S2.X)), 0, OutputWidth - 1);
                int32 maxX = FMath::Clamp(FMath::CeilToInt(FMath::Max3(S0.X, S1.X, S2.X)), 0, OutputWidth - 1);
                int32 minY = FMath::Clamp(FMath::FloorToInt(FMath::Min3(S0.Y, S1.Y, S2.Y)), 0, OutputHeight - 1);
                int32 maxY = FMath::Clamp(FMath::CeilToInt(FMath::Max3(S0.Y, S1.Y, S2.Y)), 0, OutputHeight - 1);
                
                for (int32 y = minY; y <= maxY; ++y) {
                    for (int32 x = minX; x <= maxX; ++x) {
                        FVector2D PixelPos(x + 0.5f, y + 0.5f);
                        float A, B, C;
                        if (!FMathUtils::CalculateBarycentricCoordinates(PixelPos, S0, S1, S2, A, B, C) || (A < -1e-9 || B < -1e-9 || C < -1e-9)) continue;
                        
                        double W_interp_inv = A*W0_inv + B*W1_inv + C*W2_inv;
                        FVector P_world_interp = (A*V0.WorldPosition*W0_inv + B*V1.WorldPosition*W1_inv + C*V2.WorldPosition*W2_inv) / W_interp_inv;
                        double DepthValue = FVector::Dist(P_world_interp, CameraSettings.CameraPosition);

                        int idx = y * OutputWidth + x;
                        if (DepthValue < ZBuffer[idx])
                        {
                            ZBuffer[idx] = DepthValue;
                            FVector N_world_interp = (A*N0_over_W + B*N1_over_W + C*N2_over_W) / W_interp_inv;
                            N_world_interp.Normalize();

                            // ======================== KEY MODIFICATION ========================
                            // Transform the final world-space normal into view-space.
                            FVector N_view_interp = ViewM.TransformVector(N_world_interp).GetSafeNormal();

                            // NOTE: Some conventions (like OpenGL) require flipping the Y-axis.
                            // You can uncomment this line to see if it matches your reference better.
                            N_view_interp.Z *= -1.0f;
                            // ================================================================

                            // Encode the new VIEW-SPACE normal into the color buffer.
                            OutImageBuffer[idx] = EncodeUnitVectorToRGB(N_view_interp, 255);
                        }
                    }
                }
            }
        }
    }
    
    UE_LOG(LogTemp, Warning, TEXT("--- GenerateRasterizedViewSpaceNormals FINISH ---"));
    return true;
}


bool FMeshProcessor::GenerateCannyView(
    TArray<FColor>& OutImageBuffer,
    AStaticMeshActor* Actor,
    const FProjectionSettings& CameraSettings,
    int32 OutputWidth,
    int32 OutputHeight,
    const TSet<int32>& HiddenSlotIndices,
    float AngleThresholdDegrees)
{
    UE_LOG(LogTemp, Warning, TEXT("--- GenerateCannyView START ---"));
    if (!Actor || !Actor->GetStaticMeshComponent() || !Actor->GetStaticMeshComponent()->GetStaticMesh()) return false;
    if (OutputWidth <= 0 || OutputHeight <= 0) return false;

    UStaticMesh* StaticMesh = Actor->GetStaticMeshComponent()->GetStaticMesh();
    const FTransform MeshXForm = Actor->GetStaticMeshComponent()->GetComponentTransform();

    OutImageBuffer.Init(FColor::Black, OutputWidth * OutputHeight);
    TArray<double> ZBuffer;
    ZBuffer.Init(DBL_MAX, OutputWidth * OutputHeight);

    auto& LOD     = StaticMesh->GetRenderData()->LODResources[0];
    auto& PosBuf  = LOD.VertexBuffers.PositionVertexBuffer;
    auto& VertBuf = LOD.VertexBuffers.StaticMeshVertexBuffer;
    auto   Idxs   = LOD.IndexBuffer.GetArrayView();

    FMinimalViewInfo VI;
    VI.Location = CameraSettings.CameraPosition;
    VI.Rotation = CameraSettings.CameraRotation;
    VI.FOV = CameraSettings.FOVAngle;
    VI.AspectRatio = (float)OutputWidth / (float)OutputHeight;
    VI.ProjectionMode = ECameraProjectionMode::Perspective;

    FMatrix ViewM, ProjM, ViewProjM;
    UGameplayStatics::GetViewProjectionMatrix(VI, ViewM, ProjM, ViewProjM);

    const FColor EdgeColor = FColor::White;
    const float  CreaseCosThreshold = FMath::Cos(FMath::DegreesToRadians(AngleThresholdDegrees));
    const double DepthBias = 1e-4;

    // --- Facing threshold identical to GenerateRasterizedView ---
    const float CullAngleThresholdDegrees = 10.0f;
    const float CullCosThreshold =
        FMath::Cos(FMath::DegreesToRadians(90.0f - CullAngleThresholdDegrees));

    // --------------------------------------------
    // 1) Build adjacency + per-triangle data
    // --------------------------------------------
    TMap<TTuple<uint32,uint32>, TArray<int32>> EdgeToTri;
    TArray<FVector> TriNormals;     // geometric normals (for crease test)
    TArray<bool>    TriFrontFacing; // facing rule identical to RasterizedView
    int32 TriIdxRunning = 0;

    for (int32 SectionIdx = 0; SectionIdx < LOD.Sections.Num(); ++SectionIdx)
    {
        if (HiddenSlotIndices.Contains(SectionIdx)) continue;
        const FStaticMeshSection& Section = LOD.Sections[SectionIdx];

        for (uint32 i = Section.FirstIndex; i < Section.FirstIndex + (Section.NumTriangles * 3); i += 3)
        {
            const uint32 i0 = Idxs[i+0], i1 = Idxs[i+1], i2 = Idxs[i+2];

            const FVector P0 = MeshXForm.TransformPosition(FVector(PosBuf.VertexPosition(i0)));
            const FVector P1 = MeshXForm.TransformPosition(FVector(PosBuf.VertexPosition(i1)));
            const FVector P2 = MeshXForm.TransformPosition(FVector(PosBuf.VertexPosition(i2)));

            // geometric normal
            const FVector GeoN = FVector::CrossProduct(P1 - P0, P2 - P0).GetSafeNormal();
            TriNormals.Add(GeoN);

            // averaged vertex tangent Z (world-space)
            FVector N0 = MeshXForm.TransformVectorNoScale((FVector)VertBuf.VertexTangentZ(i0)).GetSafeNormal();
            FVector N1 = MeshXForm.TransformVectorNoScale((FVector)VertBuf.VertexTangentZ(i1)).GetSafeNormal();
            FVector N2 = MeshXForm.TransformVectorNoScale((FVector)VertBuf.VertexTangentZ(i2)).GetSafeNormal();
            FVector FaceNormalAvg = (N0 + N1 + N2).GetSafeNormal();

            const FVector ViewVec = (CameraSettings.CameraPosition - P0).GetSafeNormal();
            TriFrontFacing.Add(FVector::DotProduct(FaceNormalAvg, ViewVec) >= CullCosThreshold);

            const int32 T = TriIdxRunning++;
            EdgeToTri.FindOrAdd({FMath::Min(i0,i1), FMath::Max(i0,i1)}).Add(T);
            EdgeToTri.FindOrAdd({FMath::Min(i1,i2), FMath::Max(i1,i2)}).Add(T);
            EdgeToTri.FindOrAdd({FMath::Min(i2,i0), FMath::Max(i2,i0)}).Add(T);
        }
    }

    // --------------------------------------------
    // 2) Z-PREPASS (matches RasterizedView facing)
    // --------------------------------------------
    for (int32 SectionIdx = 0; SectionIdx < LOD.Sections.Num(); ++SectionIdx)
    {
        if (HiddenSlotIndices.Contains(SectionIdx)) continue;
        const FStaticMeshSection& Section = LOD.Sections[SectionIdx];

        for (uint32 i = Section.FirstIndex; i < Section.FirstIndex + (Section.NumTriangles * 3); i += 3)
        {
            const uint32 i0 = Idxs[i+0], i1 = Idxs[i+1], i2 = Idxs[i+2];

            const FVector P0w = MeshXForm.TransformPosition(FVector(PosBuf.VertexPosition(i0)));
            const FVector P1w = MeshXForm.TransformPosition(FVector(PosBuf.VertexPosition(i1)));
            const FVector P2w = MeshXForm.TransformPosition(FVector(PosBuf.VertexPosition(i2)));

            // averaged tangent-Z normal
            FVector N0 = MeshXForm.TransformVectorNoScale((FVector)VertBuf.VertexTangentZ(i0)).GetSafeNormal();
            FVector N1 = MeshXForm.TransformVectorNoScale((FVector)VertBuf.VertexTangentZ(i1)).GetSafeNormal();
            FVector N2 = MeshXForm.TransformVectorNoScale((FVector)VertBuf.VertexTangentZ(i2)).GetSafeNormal();
            FVector FaceNormalAvg = (N0 + N1 + N2).GetSafeNormal();

            const FVector ViewVec = (CameraSettings.CameraPosition - P0w).GetSafeNormal();
            if (FVector::DotProduct(FaceNormalAvg, ViewVec) >= CullCosThreshold)
                continue;

            // clip triangle
            TArray<FClippingVertex> Poly;
            Poly.SetNum(3);
            Poly[0] = { ViewProjM.TransformPosition(P0w), P0w };
            Poly[1] = { ViewProjM.TransformPosition(P1w), P1w };
            Poly[2] = { ViewProjM.TransformPosition(P2w), P2w };

            TArray<FClippingVertex> Clipped = Poly;
            Clipped = ClipPolygonAgainstPlane(Clipped, 3,  1.0f);
            Clipped = ClipPolygonAgainstPlane(Clipped, 0,  1.0f);
            Clipped = ClipPolygonAgainstPlane(Clipped, 0, -1.0f);
            Clipped = ClipPolygonAgainstPlane(Clipped, 1,  1.0f);
            Clipped = ClipPolygonAgainstPlane(Clipped, 1, -1.0f);
            Clipped = ClipPolygonAgainstPlane(Clipped, 2, -1.0f);
            if (Clipped.Num() < 3) continue;

            for (int32 v = 1; v < Clipped.Num() - 1; ++v)
            {
                const FClippingVertex& V0 = Clipped[0];
                const FClippingVertex& V1 = Clipped[v];
                const FClippingVertex& V2 = Clipped[v+1];

                const double W0i = 1.0 / V0.Position.W;
                const double W1i = 1.0 / V1.Position.W;
                const double W2i = 1.0 / V2.Position.W;

                const FVector2D S0((V0.Position.X*W0i*0.5+0.5)*OutputWidth, (1.0-(V0.Position.Y*W0i*0.5+0.5))*OutputHeight);
                const FVector2D S1((V1.Position.X*W1i*0.5+0.5)*OutputWidth, (1.0-(V1.Position.Y*W1i*0.5+0.5))*OutputHeight);
                const FVector2D S2((V2.Position.X*W2i*0.5+0.5)*OutputWidth, (1.0-(V2.Position.Y*W2i*0.5+0.5))*OutputHeight);

                const int32 minX = FMath::Clamp(FMath::FloorToInt(FMath::Min3(S0.X,S1.X,S2.X)), 0, OutputWidth-1);
                const int32 maxX = FMath::Clamp(FMath::CeilToInt (FMath::Max3(S0.X,S1.X,S2.X)), 0, OutputWidth-1);
                const int32 minY = FMath::Clamp(FMath::FloorToInt(FMath::Min3(S0.Y,S1.Y,S2.Y)), 0, OutputHeight-1);
                const int32 maxY = FMath::Clamp(FMath::CeilToInt (FMath::Max3(S0.Y,S1.Y,S2.Y)), 0, OutputHeight-1);

                for (int32 y = minY; y <= maxY; ++y)
                for (int32 x = minX; x <= maxX; ++x)
                {
                    FVector2D P(x+0.5f, y+0.5f);
                    float A,B,C;
                    if (!FMathUtils::CalculateBarycentricCoordinates(P,S0,S1,S2,A,B,C) || A < -1e-9f || B < -1e-9f || C < -1e-9f) continue;

                    const double Wi = A*W0i + B*W1i + C*W2i;
                    FVector Pw = (A*V0.WorldPosition*W0i + B*V1.WorldPosition*W1i + C*V2.WorldPosition*W2i) / Wi;
                    double Depth = FVector::Dist(Pw, CameraSettings.CameraPosition);

                    int32 idx = y*OutputWidth + x;
                    if (Depth < ZBuffer[idx]) ZBuffer[idx] = Depth;
                }
            }
        }
    }

    // --------------------------------------------
    // 3) Select edges (visibility-aware)
    // --------------------------------------------
    TArray<TTuple<FVector,FVector>> EdgesToDraw;
    for (auto const& [EdgeAB, TriList] : EdgeToTri)
    {
        bool bDraw = false;

        if (TriList.Num() == 1)
        {
            bDraw = TriFrontFacing[TriList[0]];
        }
        else if (TriList.Num() == 2)
        {
            int32 t0 = TriList[0], t1 = TriList[1];
            bool f0 = TriFrontFacing[t0], f1 = TriFrontFacing[t1];

            if (f0 != f1) bDraw = true; // silhouette
            else if (f0 || f1)
                bDraw = (FVector::DotProduct(TriNormals[t0], TriNormals[t1]) < CreaseCosThreshold);
        }

        if (bDraw)
        {
            uint32 a = EdgeAB.Get<0>(), b = EdgeAB.Get<1>();
            EdgesToDraw.Emplace(
                MeshXForm.TransformPosition(FVector(PosBuf.VertexPosition(a))),
                MeshXForm.TransformPosition(FVector(PosBuf.VertexPosition(b))));
        }
    }

    // --------------------------------------------
    // 4) Rasterize edges with depth test
    // --------------------------------------------
    for (const auto& E : EdgesToDraw)
    {
        TArray<FClippingVertex> Line;
        Line.SetNum(2);
        Line[0] = { ViewProjM.TransformPosition(E.Get<0>()), E.Get<0>() };
        Line[1] = { ViewProjM.TransformPosition(E.Get<1>()), E.Get<1>() };

        Line = ClipPolygonAgainstPlane(Line, 3,  1.0f);
        Line = ClipPolygonAgainstPlane(Line, 0,  1.0f);
        Line = ClipPolygonAgainstPlane(Line, 0, -1.0f);
        Line = ClipPolygonAgainstPlane(Line, 1,  1.0f);
        Line = ClipPolygonAgainstPlane(Line, 1, -1.0f);
        Line = ClipPolygonAgainstPlane(Line, 2, -1.0f);
        if (Line.Num() < 2) continue;

        const FVector4 P0c = Line[0].Position;
        const FVector4 P1c = Line[1].Position;
        const double W0i = 1.0 / P0c.W;
        const double W1i = 1.0 / P1c.W;

        const FVector2D S0((P0c.X*W0i*0.5+0.5)*OutputWidth, (1.0-(P0c.Y*W0i*0.5+0.5))*OutputHeight);
        const FVector2D S1((P1c.X*W1i*0.5+0.5)*OutputWidth, (1.0-(P1c.Y*W1i*0.5+0.5))*OutputHeight);

        double dx = S1.X - S0.X;
        double dy = S1.Y - S0.Y;
        int32 Steps = FMath::Max(FMath::Abs(dx), FMath::Abs(dy)) + 1;
        if (Steps <= 0) continue;

        double xInc = dx / Steps;
        double yInc = dy / Steps;
        double cx = S0.X, cy = S0.Y;

        for (int32 s = 0; s <= Steps; ++s, cx += xInc, cy += yInc)
        {
            int32 x = FMath::RoundToInt(cx);
            int32 y = FMath::RoundToInt(cy);
            if (x < 0 || x >= OutputWidth || y < 0 || y >= OutputHeight) continue;

            double t = (double)s / Steps;
            double Wi = FMath::Lerp(W0i, W1i, t);
            FVector Pw = FMath::Lerp(Line[0].WorldPosition*W0i, Line[1].WorldPosition*W1i, t) / Wi;
            double Depth = FVector::Dist(Pw, CameraSettings.CameraPosition);

            int32 idx = y*OutputWidth + x;
            if (Depth <= ZBuffer[idx] + DepthBias)
                OutImageBuffer[idx] = EdgeColor;
        }
    }

    UE_LOG(LogTemp, Warning, TEXT("--- GenerateCannyView FINISH ---"));
    return true;
}


bool FMeshProcessor::GenerateRasterizedMaskView(
    TArray<FColor>& OutImageBuffer,
    AStaticMeshActor* Actor,
    const FProjectionSettings& CameraSettings,
    const TMap<int32, TObjectPtr<UTexture2D>>& AllSlotTextures,
    int32 OutputWidth,
    int32 OutputHeight,
    int32 UVChannel)
{
    // --- 1. SETUP & VALIDATION ---
    UE_LOG(LogTemp, Warning, TEXT("--- GenerateRasterizedView START ---"));
    UE_LOG(LogTemp, Warning, TEXT("Output: %dx%d, NumSlotTextures: %d"), OutputWidth, OutputHeight, AllSlotTextures.Num());

    if (!Actor || !Actor->GetStaticMeshComponent() || !Actor->GetStaticMeshComponent()->GetStaticMesh() || AllSlotTextures.Num() == 0)
    {
        UE_LOG(LogTemp, Error, TEXT(" -> ABORT: Invalid actor or no slot textures provided."));
        return false;
    }
    if (OutputWidth <= 0 || OutputHeight <= 0) 
    {
        UE_LOG(LogTemp, Error, TEXT(" -> ABORT: Invalid output dimensions."));
        return false; 
    }

    UStaticMesh* StaticMesh = Actor->GetStaticMeshComponent()->GetStaticMesh();
    const FTransform MeshXForm = Actor->GetStaticMeshComponent()->GetComponentTransform();

    // --- 2. INITIALIZE BUFFERS & COUNTERS ---
    OutImageBuffer.Init(FColor::Transparent, OutputWidth * OutputHeight);
    TArray<double> ZBuffer;
    ZBuffer.Init(DBL_MAX, OutputWidth * OutputHeight);

    int32 TotalTrianglesProcessed = 0;
    int32 TotalTrianglesPassedCulling = 0;
    int32 TotalTrianglesSurvivedClipping = 0;
    int32 TotalPixelsWritten = 0;

    auto& LOD = StaticMesh->GetRenderData()->LODResources[0];
    auto& PosBuf = LOD.VertexBuffers.PositionVertexBuffer;
    auto& UVBuf = LOD.VertexBuffers.StaticMeshVertexBuffer;
    auto Idxs = LOD.IndexBuffer.GetArrayView();
    
    FMinimalViewInfo VI;
    VI.Location = CameraSettings.CameraPosition; VI.Rotation = CameraSettings.CameraRotation; VI.FOV = CameraSettings.FOVAngle;
    VI.AspectRatio = (float)OutputWidth / (float)OutputHeight; VI.ProjectionMode = ECameraProjectionMode::Perspective;
    FMatrix ViewM, ProjM, ViewProjM;
    UGameplayStatics::GetViewProjectionMatrix(VI, ViewM, ProjM, ViewProjM);

    // --- 3. MULTI-PASS RASTERIATION ---
    for (int32 SectionIdx = 0; SectionIdx < LOD.Sections.Num(); ++SectionIdx)
    {
        UE_LOG(LogTemp, Log, TEXT("PASS %d/%d: Processing Material Slot %d"), SectionIdx + 1, LOD.Sections.Num(), SectionIdx);
        const TObjectPtr<UTexture2D>* FoundTexture = AllSlotTextures.Find(SectionIdx);
        if (!FoundTexture || !IsValid(*FoundTexture))
        {
            UE_LOG(LogTemp, Log, TEXT("  -> No texture found for this slot. Skipping pass."));
            continue;
        }
        UTexture2D* TextureToApply = *FoundTexture;
        if (!TextureToApply->GetPlatformData() || TextureToApply->GetPlatformData()->Mips.Num() == 0) continue;

        UE_LOG(LogTemp, Log, TEXT("  -> Using Texture: %s"), *TextureToApply->GetName());
        
        FTexture2DMipMap& Mip = TextureToApply->GetPlatformData()->Mips[0];
        const FColor* SourceTextureData = static_cast<const FColor*>(Mip.BulkData.LockReadOnly());
        const int32 SourceTexW = Mip.SizeX;
        const int32 SourceTexH = Mip.SizeY;

        const FStaticMeshSection& Section = LOD.Sections[SectionIdx];
        const uint32 FirstIndex = Section.FirstIndex;
        const uint32 NumTrianglesInSection = Section.NumTriangles;
        const uint32 EndIndex = FirstIndex + (NumTrianglesInSection * 3);

        for (uint32 i = FirstIndex; i < EndIndex; i += 3)
        {
            TotalTrianglesProcessed++;
            uint32 i0 = Idxs[i + 0], i1 = Idxs[i + 1], i2 = Idxs[i + 2];
            
            FVector P0_world = MeshXForm.TransformPosition(FVector(PosBuf.VertexPosition(i0)));
            FVector P1_world = MeshXForm.TransformPosition(FVector(PosBuf.VertexPosition(i1)));
            FVector P2_world = MeshXForm.TransformPosition(FVector(PosBuf.VertexPosition(i2)));
            
            FVector TriNormal = FVector::CrossProduct((P1_world - P0_world), (P2_world - P0_world)).GetSafeNormal();
            FVector ViewDir = (P0_world - CameraSettings.CameraPosition).GetSafeNormal();
            if (FVector::DotProduct(TriNormal, ViewDir) <= 0.0f) continue;
            TotalTrianglesPassedCulling++;

            TArray<FClippingVertex> OriginalPolygon;
            OriginalPolygon.SetNum(3);
            OriginalPolygon[0] = { ViewProjM.TransformPosition(P0_world), P0_world, FVector2D(UVBuf.GetVertexUV(i0, UVChannel)) };
            OriginalPolygon[1] = { ViewProjM.TransformPosition(P1_world), P1_world, FVector2D(UVBuf.GetVertexUV(i1, UVChannel)) };
            OriginalPolygon[2] = { ViewProjM.TransformPosition(P2_world), P2_world, FVector2D(UVBuf.GetVertexUV(i2, UVChannel)) };
            
            TArray<FClippingVertex> ClippedPolygon = OriginalPolygon;
            ClippedPolygon = ClipPolygonAgainstPlane(ClippedPolygon, 3, 1.0f);
            ClippedPolygon = ClipPolygonAgainstPlane(ClippedPolygon, 0, 1.0f);
            ClippedPolygon = ClipPolygonAgainstPlane(ClippedPolygon, 0, -1.0f);
            ClippedPolygon = ClipPolygonAgainstPlane(ClippedPolygon, 1, 1.0f);
            ClippedPolygon = ClipPolygonAgainstPlane(ClippedPolygon, 1, -1.0f);
            ClippedPolygon = ClipPolygonAgainstPlane(ClippedPolygon, 2, -1.0f);
            if (ClippedPolygon.Num() < 3) continue;
            TotalTrianglesSurvivedClipping++;

            for (int32 VertexIdx = 1; VertexIdx < ClippedPolygon.Num() - 1; ++VertexIdx)
            {
                const FClippingVertex& V0 = ClippedPolygon[0];
                const FClippingVertex& V1 = ClippedPolygon[VertexIdx];
                const FClippingVertex& V2 = ClippedPolygon[VertexIdx + 1];
                
                double W0_inv = 1.0 / V0.Position.W, W1_inv = 1.0 / V1.Position.W, W2_inv = 1.0 / V2.Position.W;
                FVector2D UV0_over_W = V0.UV * W0_inv, UV1_over_W = V1.UV * W1_inv, UV2_over_W = V2.UV * W2_inv;
                FVector2D S0((V0.Position.X*W0_inv*0.5+0.5)*OutputWidth, (1.0-(V0.Position.Y*W0_inv*0.5+0.5))*OutputHeight);
                FVector2D S1((V1.Position.X*W1_inv*0.5+0.5)*OutputWidth, (1.0-(V1.Position.Y*W1_inv*0.5+0.5))*OutputHeight);
                FVector2D S2((V2.Position.X*W2_inv*0.5+0.5)*OutputWidth, (1.0-(V2.Position.Y*W2_inv*0.5+0.5))*OutputHeight);
                
                int32 minX = FMath::Clamp(FMath::FloorToInt(FMath::Min3(S0.X, S1.X, S2.X)), 0, OutputWidth - 1);
                int32 maxX = FMath::Clamp(FMath::CeilToInt(FMath::Max3(S0.X, S1.X, S2.X)), 0, OutputWidth - 1);
                int32 minY = FMath::Clamp(FMath::FloorToInt(FMath::Min3(S0.Y, S1.Y, S2.Y)), 0, OutputHeight - 1);
                int32 maxY = FMath::Clamp(FMath::CeilToInt(FMath::Max3(S0.Y, S1.Y, S2.Y)), 0, OutputHeight - 1);
                
                for (int32 y = minY; y <= maxY; ++y) {
                    for (int32 x = minX; x <= maxX; ++x) {
                        FVector2D PixelPos(x + 0.5f, y + 0.5f);
                        float A, B, C;
                        if (!FMathUtils::CalculateBarycentricCoordinates(PixelPos, S0, S1, S2, A, B, C) || (A < -1e-9 || B < -1e-9 || C < -1e-9)) continue;
                        
                        double W_interp_inv = A*W0_inv + B*W1_inv + C*W2_inv;
                        FVector P_world_interp = (A*V0.WorldPosition*W0_inv + B*V1.WorldPosition*W1_inv + C*V2.WorldPosition*W2_inv) / W_interp_inv;
                        double DepthValue = FVector::Dist(P_world_interp, CameraSettings.CameraPosition);

                        int idx = y * OutputWidth + x;
                        if (DepthValue < ZBuffer[idx])
                        {
                            if (TotalPixelsWritten == 0) // Log only the FIRST time a pixel is about to be written
                            {
                                UE_LOG(LogTemp, Error, TEXT(">>> SUCCESS: ATTEMPTING TO WRITE FIRST PIXEL <<<"));
                            }
                            TotalPixelsWritten++;
                            
                            ZBuffer[idx] = DepthValue;

                            OutImageBuffer[idx] = FColor::Transparent;
                            
                            FVector2D uv_correct = (A*UV0_over_W + B*UV1_over_W + C*UV2_over_W) / W_interp_inv;
                            int mx = FMath::Clamp(FMath::RoundToInt(uv_correct.X * (SourceTexW - 1)), 0, SourceTexW - 1);
                            int my = FMath::Clamp(FMath::RoundToInt(uv_correct.Y * (SourceTexH - 1)), 0, SourceTexH - 1);
                            const FColor& SampledColor = SourceTextureData[my * SourceTexW + mx];
                            if (SampledColor.A > 0)
                            {
                                OutImageBuffer[idx] = SampledColor;
                            }
                        }
                    }
                }
            }
        }
        Mip.BulkData.Unlock();
    }
    
    UE_LOG(LogTemp, Warning, TEXT("--- GenerateRasterizedView FINISH ---"));
    UE_LOG(LogTemp, Warning, TEXT("Summary: Total Tris Processed=%d | Passed Culling=%d | Survived Clipping=%d | Pixels Written=%d"),
        TotalTrianglesProcessed, TotalTrianglesPassedCulling, TotalTrianglesSurvivedClipping, TotalPixelsWritten);

    return true;
}


TArray<int32> FMeshProcessor::GenerateUVIslandIDMap(
    UStaticMesh* StaticMesh, 
    int32 UVChannel, 
    int32 TextureWidth, 
    int32 TextureHeight,
    int32 MaterialSlotIndex)
{
    UE_LOG(LogTemp, Log, TEXT("Starting generation of UV Island ID Map (Position-Aware) for Material Slot %d..."), MaterialSlotIndex);

    // 1. --- Get Mesh Data (LOD0) ---
    if (!StaticMesh || !StaticMesh->GetRenderData() || StaticMesh->GetRenderData()->LODResources.Num() == 0)
    {
        UE_LOG(LogTemp, Error, TEXT("GenerateUVIslandIDMap: Invalid Static Mesh data provided."));
        return {}; 
    }
    
    const FStaticMeshLODResources& LOD = StaticMesh->GetRenderData()->LODResources[0];
    const FIndexArrayView Indices = LOD.IndexBuffer.GetArrayView();
    const FStaticMeshVertexBuffer& UVBuffer = LOD.VertexBuffers.StaticMeshVertexBuffer;
    const FPositionVertexBuffer& PosBuffer = LOD.VertexBuffers.PositionVertexBuffer;
    
    const int32 NumVertices = PosBuffer.GetNumVertices();

    if (UVChannel >= (int32)UVBuffer.GetNumTexCoords())
    {
        UE_LOG(LogTemp, Error, TEXT("GenerateUVIslandIDMap: Requested UV channel %d is invalid. The mesh only has %d UV channel(s)."), UVChannel, UVBuffer.GetNumTexCoords());
        return {};
    }

    // --- Find the section for this material slot ---
    const FStaticMeshSection* TargetSection = nullptr;
    for (const FStaticMeshSection& Section : LOD.Sections)
    {
        if (Section.MaterialIndex == MaterialSlotIndex)
        {
            TargetSection = &Section;
            break;
        }
    }

    if (!TargetSection)
    {
        UE_LOG(LogTemp, Error, TEXT("GenerateUVIslandIDMap: Material slot %d not found in mesh."), MaterialSlotIndex);
        return {};
    }

    const int32 SectionFirstIndex = TargetSection->FirstIndex;
    const int32 SectionNumTriangles = TargetSection->NumTriangles;
    const int32 SectionFirstTriangle = SectionFirstIndex / 3;

    UE_LOG(LogTemp, Log, TEXT("GenerateUVIslandIDMap: Processing section with %d triangles (starting at triangle %d)."), 
        SectionNumTriangles, SectionFirstTriangle);

    // 2. --- Build Position-Based Adjacency Map ---
    // Problem: Hard edges cause vertices to split (different indices), preventing flood fill from crossing the edge.
    // Solution: Map all vertices to a "Canonical ID" based on their 3D Position.
    
    TMap<FVector, uint32> PositionToCanonicalMap;
    TArray<uint32> VertexToCanonicalMap;
    VertexToCanonicalMap.SetNum(NumVertices);

    for (int32 i = 0; i < NumVertices; ++i)
    {
        const FVector Pos = FVector(PosBuffer.VertexPosition(i));
        
        if (const uint32* FoundCanonical = PositionToCanonicalMap.Find(Pos))
        {
            VertexToCanonicalMap[i] = *FoundCanonical;
        }
        else
        {
            PositionToCanonicalMap.Add(Pos, i);
            VertexToCanonicalMap[i] = i;
        }
    }

    // Build the Vertex-to-Triangle map using Canonical Indices.
    // ONLY include triangles from our target material section.
    TMultiMap<uint32, int32> VertToTriMap;
    for (int32 i = 0; i < SectionNumTriangles; ++i)
    {
        const int32 TriIdx = SectionFirstTriangle + i;
        for (int32 j = 0; j < 3; ++j)
        {
            uint32 OriginalVertIdx = Indices[TriIdx * 3 + j];
            uint32 CanonicalVertIdx = VertexToCanonicalMap[OriginalVertIdx];
            VertToTriMap.Add(CanonicalVertIdx, TriIdx);
        }
    }

    // 3. --- Identify Islands using Breadth-First Search (Flood Fill) ---
    // We need a map that covers all triangles in the mesh for indexing,
    // but we only process triangles in our section.
    const int32 TotalNumTriangles = Indices.Num() / 3;
    TArray<int32> TriangleToIslandIDMap;
    TriangleToIslandIDMap.Init(-1, TotalNumTriangles); 
    int32 CurrentIslandID = 0;

    for (int32 i = 0; i < SectionNumTriangles; ++i)
    {
        const int32 StartTriIdx = SectionFirstTriangle + i;
        
        if (TriangleToIslandIDMap[StartTriIdx] == -1) // Found unvisited triangle
        {
            CurrentIslandID++; 
            TArray<int32> Queue;
            Queue.Add(StartTriIdx);
            TriangleToIslandIDMap[StartTriIdx] = CurrentIslandID;
            
            int32 Head = 0; 
            while (Head < Queue.Num())
            {
                const int32 CurrentTriIdx = Queue[Head++];
                const uint32 CurrentTriVerts[] = { 
                    Indices[CurrentTriIdx * 3 + 0], 
                    Indices[CurrentTriIdx * 3 + 1], 
                    Indices[CurrentTriIdx * 3 + 2] 
                };

                // Find potential neighbors using Canonical Indices (Physical Connectivity)
                TSet<int32> PotentialNeighbors;
                for (int32 j = 0; j < 3; ++j)
                {
                    uint32 CanonicalVertIdx = VertexToCanonicalMap[CurrentTriVerts[j]];
                    
                    TArray<int32> FoundTris;
                    VertToTriMap.MultiFind(CanonicalVertIdx, FoundTris);
                    PotentialNeighbors.Append(FoundTris);
                }

                for (const int32 NeighborTriIdx : PotentialNeighbors)
                {
                    if (NeighborTriIdx == CurrentTriIdx || TriangleToIslandIDMap[NeighborTriIdx] != -1)
                    {
                        continue; // Skip self and already-visited
                    }

                    // --- Check for UV Seam (Logical Connectivity) ---
                    const uint32 NeighborTriVerts[] = { 
                        Indices[NeighborTriIdx * 3 + 0], 
                        Indices[NeighborTriIdx * 3 + 1], 
                        Indices[NeighborTriIdx * 3 + 2] 
                    };
                    
                    int32 SharedUVVertCount = 0;

                    for (int32 vi = 0; vi < 3; ++vi)
                    {
                        const FVector2f UV_Current = UVBuffer.GetVertexUV(CurrentTriVerts[vi], UVChannel);
                        
                        for (int32 vj = 0; vj < 3; ++vj)
                        {
                            const FVector2f UV_Neighbor = UVBuffer.GetVertexUV(NeighborTriVerts[vj], UVChannel);
                            
                            if (UV_Current.Equals(UV_Neighbor, KINDA_SMALL_NUMBER))
                            {
                                SharedUVVertCount++;
                                break;
                            }
                        }
                    }

                    // If they share at least 2 vertices with matching UVs, they are on the same island.
                    if (SharedUVVertCount >= 2)
                    {
                        TriangleToIslandIDMap[NeighborTriIdx] = CurrentIslandID;
                        Queue.Add(NeighborTriIdx);
                    }
                }
            }
        }
    }
    
    // 4. --- Rasterize the Triangle IDs into the Final Pixel Map ---
    // Only rasterize triangles from our target material section.
    TArray<int32> FinalPixelMap;
    FinalPixelMap.Init(0, TextureWidth * TextureHeight); 

    for (int32 i = 0; i < SectionNumTriangles; ++i)
    {
        const int32 TriIdx = SectionFirstTriangle + i;
        const int32 IslandID = TriangleToIslandIDMap[TriIdx];
        if (IslandID <= 0) continue;

        const FVector2D V0 = FVector2D(UVBuffer.GetVertexUV(Indices[TriIdx * 3 + 0], UVChannel));
        const FVector2D V1 = FVector2D(UVBuffer.GetVertexUV(Indices[TriIdx * 3 + 1], UVChannel));
        const FVector2D V2 = FVector2D(UVBuffer.GetVertexUV(Indices[TriIdx * 3 + 2], UVChannel));

        // Calculate bounding box
        const float MinX = FMath::Min3(V0.X, V1.X, V2.X) * TextureWidth;
        const float MaxX = FMath::Max3(V0.X, V1.X, V2.X) * TextureWidth;
        const float MinY = FMath::Min3(V0.Y, V1.Y, V2.Y) * TextureHeight;
        const float MaxY = FMath::Max3(V0.Y, V1.Y, V2.Y) * TextureHeight;

        for (int32 Y = FMath::FloorToInt(MinY); Y <= FMath::CeilToInt(MaxY); ++Y)
        {
            for (int32 X = FMath::FloorToInt(MinX); X <= FMath::CeilToInt(MaxX); ++X)
            {
                if (X < 0 || X >= TextureWidth || Y < 0 || Y >= TextureHeight) continue;
                
                const FVector2D PixelPos(X + 0.5f, Y + 0.5f);
                const FVector2D TexelPos(PixelPos.X / TextureWidth, PixelPos.Y / TextureHeight);

                float A, B, C;
                constexpr float kBaryEpsilon = -1e-4f;

                if (FMathUtils::CalculateBarycentricCoordinates(TexelPos, V0, V1, V2, A, B, C))
                {
                    if (A >= kBaryEpsilon && B >= kBaryEpsilon && C >= kBaryEpsilon)
                    {
                        FinalPixelMap[Y * TextureWidth + X] = IslandID;
                    }
                }
            }
        }
    }

    UE_LOG(LogTemp, Log, TEXT("Finished UV Island ID Map generation for Material Slot %d. Found %d islands."), MaterialSlotIndex, CurrentIslandID);
    return FinalPixelMap;
}


static inline uint8 LinearToSrgb8(double c)
{
    c = FMath::Clamp(c, 0.0, 1.0);
    double s = (c <= 0.0031308) ? 12.92 * c : 1.055 * FMath::Pow(c, 1.0 / 2.4) - 0.055;
    return (uint8)FMath::Clamp(FMath::RoundToInt(s * 255.0), 0, 255);
}


bool FMeshProcessor::GenerateSilhouetteBaseColorImage(
    TArray<FColor>& OutImage,                             // sRGB8 output
    AStaticMeshActor* Actor,
    const FProjectionSettings& Camera,
    int32 Width,
    int32 Height,
    int32 UVChannel,
    const TMap<int32, FLinearColor>& SlotBaseColors,
    TMap<int32, bool> SlotHidden,      // slot -> LINEAR base color
    bool bTransparentBackground /*= true*/                     // slots to skip entirely
)
{
    // --- 1. SETUP & VALIDATION ---
    UE_LOG(LogTemp, Warning, TEXT("--- GenerateSilhouetteBaseColorImage START ---"));
    UE_LOG(LogTemp, Warning, TEXT("Output: %dx%d, NumSlotColors: %d, TransparentBG: %s"),
        Width, Height, SlotBaseColors.Num(), bTransparentBackground ? TEXT("true") : TEXT("false"));

    if (!Actor || !Actor->GetStaticMeshComponent() || !Actor->GetStaticMeshComponent()->GetStaticMesh())
    {
        UE_LOG(LogTemp, Error, TEXT(" -> ABORT: Invalid actor"));
        return false;
    }
    if (Width <= 0 || Height <= 0)
    {
        UE_LOG(LogTemp, Error, TEXT(" -> ABORT: Invalid output dimensions."));
        return false;
    }

    UStaticMesh* StaticMesh = Actor->GetStaticMeshComponent()->GetStaticMesh();
    const FTransform MeshXForm = Actor->GetStaticMeshComponent()->GetComponentTransform();

    // --- 2. INITIALIZE BUFFERS & COUNTERS ---
    OutImage.Init(bTransparentBackground ? FColor(0,0,0,0) : FColor::Black, Width * Height);
    TArray<double> ZBuffer;
    ZBuffer.Init(DBL_MAX, Width * Height);

    int32 TotalTrianglesProcessed = 0;
    int32 TotalTrianglesPassedCulling = 0;
    int32 TotalTrianglesSurvivedClipping = 0;
    int32 TotalPixelsWritten = 0;

    auto& LOD   = StaticMesh->GetRenderData()->LODResources[0];
    auto& PosBuf= LOD.VertexBuffers.PositionVertexBuffer;
    auto& UVBuf = LOD.VertexBuffers.StaticMeshVertexBuffer;
    auto  Idxs  = LOD.IndexBuffer.GetArrayView();

    // Camera setup
    FMinimalViewInfo VI;
    VI.Location = Camera.CameraPosition;
    VI.Rotation = Camera.CameraRotation;
    VI.FOV = Camera.FOVAngle;
    VI.AspectRatio = (float)Width / (float)Height;
    VI.ProjectionMode = ECameraProjectionMode::Perspective;

    FMatrix ViewM, ProjM, ViewProjM;
    UGameplayStatics::GetViewProjectionMatrix(VI, ViewM, ProjM, ViewProjM);

    // --- 3. MULTI-PASS RASTERIZATION (one pass per section/material slot) ---
    for (int32 SectionIdx = 0; SectionIdx < LOD.Sections.Num(); ++SectionIdx)
    {
        const bool* bHide = SlotHidden.Find(SectionIdx);
        if (bHide && *bHide)
        {
            UE_LOG(LogTemp, Log, TEXT("PASS %d/%d: Slot %d is hidden. Skipping."), SectionIdx + 1, LOD.Sections.Num(), SectionIdx);
            continue;
        }

        // This is an alternative, more concise way to write the "After" logic.
        
        // FindRef safely returns the value for the key, or a default-constructed
        // FLinearColor if the key is not found. A default FLinearColor is (0,0,0,0), which is black.
    FLinearColor ColorForThisSection;
        const FLinearColor* FoundColorPtr = SlotBaseColors.Find(SectionIdx);

        if (FoundColorPtr != nullptr)
        {
            // SUCCESS: A color was found in the map for this slot. We will use it.
            ColorForThisSection = *FoundColorPtr;
        }
        else
        {
            // FAILURE: No color was found for this slot. We will use our explicit default.
            ColorForThisSection = FLinearColor(0.5f, 0.5f, 0.5f);
        }
        // --- END OF NEW LOGIC ---

        // Convert the chosen LINEAR color -> sRGB8 once per slot.
        FColor SlotSRGB = ColorForThisSection.ToFColorSRGB();
        SlotSRGB.A = 255; 

        const FStaticMeshSection& Section = LOD.Sections[SectionIdx];
        const uint32 FirstIndex = Section.FirstIndex;
        const uint32 NumTrisInSection = Section.NumTriangles;
        const uint32 EndIndex = FirstIndex + (NumTrisInSection * 3);

        for (uint32 i = FirstIndex; i < EndIndex; i += 3)
        {
            TotalTrianglesProcessed++;

            const uint32 i0 = Idxs[i + 0];
            const uint32 i1 = Idxs[i + 1];
            const uint32 i2 = Idxs[i + 2];

            const FVector P0_world = MeshXForm.TransformPosition(FVector(PosBuf.VertexPosition(i0)));
            const FVector P1_world = MeshXForm.TransformPosition(FVector(PosBuf.VertexPosition(i1)));
            const FVector P2_world = MeshXForm.TransformPosition(FVector(PosBuf.VertexPosition(i2)));

            // Backface culling (same convention as your original function)
            const FVector TriNormal = FVector::CrossProduct((P1_world - P0_world), (P2_world - P0_world)).GetSafeNormal();
            const FVector ViewDir = (P0_world - Camera.CameraPosition).GetSafeNormal();
            if (FVector::DotProduct(TriNormal, ViewDir) <= 0.0f)
            {
                continue;
            }
            TotalTrianglesPassedCulling++;

            // Build clip-space polygon (we still carry UVs even though we don't sample; keeps math analogous)
            TArray<FClippingVertex> Poly;
            Poly.SetNum(3);
            Poly[0] = { ViewProjM.TransformPosition(P0_world), P0_world, FVector2D(UVBuf.GetVertexUV(i0, UVChannel)) };
            Poly[1] = { ViewProjM.TransformPosition(P1_world), P1_world, FVector2D(UVBuf.GetVertexUV(i1, UVChannel)) };
            Poly[2] = { ViewProjM.TransformPosition(P2_world), P2_world, FVector2D(UVBuf.GetVertexUV(i2, UVChannel)) };

            // Clip against canonical view volume (same order as your original)
            TArray<FClippingVertex> Clipped = Poly;
            Clipped = ClipPolygonAgainstPlane(Clipped, 3,  1.0f); // w >= +z (far)
            Clipped = ClipPolygonAgainstPlane(Clipped, 0,  1.0f); // x <=  w (right)
            Clipped = ClipPolygonAgainstPlane(Clipped, 0, -1.0f); // x >= -w (left)
            Clipped = ClipPolygonAgainstPlane(Clipped, 1,  1.0f); // y <=  w (top)
            Clipped = ClipPolygonAgainstPlane(Clipped, 1, -1.0f); // y >= -w (bottom)
            Clipped = ClipPolygonAgainstPlane(Clipped, 2, -1.0f); // z >= -w (near)

            if (Clipped.Num() < 3)
            {
                continue;
            }
            TotalTrianglesSurvivedClipping++;

            // Fan triangulation for the (possibly) clipped polygon
            for (int32 v = 1; v < Clipped.Num() - 1; ++v)
            {
                const FClippingVertex& V0 = Clipped[0];
                const FClippingVertex& V1 = Clipped[v];
                const FClippingVertex& V2 = Clipped[v + 1];

                const double W0_inv = 1.0 / V0.Position.W;
                const double W1_inv = 1.0 / V1.Position.W;
                const double W2_inv = 1.0 / V2.Position.W;

                // NDC -> screen
                const FVector2D S0((V0.Position.X * W0_inv * 0.5 + 0.5) * Width,
                                   (1.0 - (V0.Position.Y * W0_inv * 0.5 + 0.5)) * Height);
                const FVector2D S1((V1.Position.X * W1_inv * 0.5 + 0.5) * Width,
                                   (1.0 - (V1.Position.Y * W1_inv * 0.5 + 0.5)) * Height);
                const FVector2D S2((V2.Position.X * W2_inv * 0.5 + 0.5) * Width,
                                   (1.0 - (V2.Position.Y * W2_inv * 0.5 + 0.5)) * Height);

                int32 minX = FMath::Clamp(FMath::FloorToInt(FMath::Min3(S0.X, S1.X, S2.X)), 0, Width  - 1);
                int32 maxX = FMath::Clamp(FMath::CeilToInt (FMath::Max3(S0.X, S1.X, S2.X)), 0, Width  - 1);
                int32 minY = FMath::Clamp(FMath::FloorToInt(FMath::Min3(S0.Y, S1.Y, S2.Y)), 0, Height - 1);
                int32 maxY = FMath::Clamp(FMath::CeilToInt (FMath::Max3(S0.Y, S1.Y, S2.Y)), 0, Height - 1);

                for (int32 y = minY; y <= maxY; ++y)
                {
                    for (int32 x = minX; x <= maxX; ++x)
                    {
                        const FVector2D P(x + 0.5f, y + 0.5f);
                        float A, B, C;
                        if (!FMathUtils::CalculateBarycentricCoordinates(P, S0, S1, S2, A, B, C) ||
                            (A < -1e-9f || B < -1e-9f || C < -1e-9f))
                        {
                            continue;
                        }

                        // Perspective-correct interpolation for world position (for depth)
                        const double W_interp_inv = A * W0_inv + B * W1_inv + C * W2_inv;
                        const FVector P_world_interp =
                            (A * V0.WorldPosition * W0_inv + B * V1.WorldPosition * W1_inv + C * V2.WorldPosition * W2_inv) / W_interp_inv;

                        const double Depth = FVector::Dist(P_world_interp, Camera.CameraPosition);
                        const int32 Idx = y * Width + x;

                        if (Depth < ZBuffer[Idx])
                        {
                            if (TotalPixelsWritten == 0)
                            {
                                UE_LOG(LogTemp, Error, TEXT(">>> SUCCESS: ATTEMPTING TO WRITE FIRST PIXEL (Silhouette) <<<"));
                            }
                            ZBuffer[Idx] = Depth;
                            OutImage[Idx] = SlotSRGB;   // flat, opaque base color fill for the silhouette
                            TotalPixelsWritten++;
                        }
                    }
                }
            }
        }
    }

    UE_LOG(LogTemp, Warning, TEXT("--- GenerateSilhouetteBaseColorImage FINISH ---"));
    UE_LOG(LogTemp, Warning, TEXT("Summary: Total Tris Processed=%d | Passed Culling=%d | Survived Clipping=%d | Pixels Written=%d"),
        TotalTrianglesProcessed, TotalTrianglesPassedCulling, TotalTrianglesSurvivedClipping, TotalPixelsWritten);

    return true;
}




void FMeshProcessor::BuildPerTexelTBN_FromMesh(
    UStaticMesh* StaticMesh,
    const FTransform& LocalToWorld,
    int32 UVChannel,
    int32 TargetMaterialSlotIndex,
    int32 W, int32 H,
    TArray<FVector>& OutTWorld,
    TArray<FVector>& OutBWorld,
    TArray<FVector>& OutNWorld)
{
    const int32 NumTexels = W * H;
    
    // 1. Initialize buffers with default forward vector
    OutTWorld.Init(FVector::ZeroVector, NumTexels);
    OutBWorld.Init(FVector::ZeroVector, NumTexels);
    OutNWorld.Init(FVector::ZeroVector, NumTexels);

    if (!StaticMesh || !StaticMesh->GetRenderData() || StaticMesh->GetRenderData()->LODResources.Num() == 0) return;
    
    const FStaticMeshLODResources& LOD = StaticMesh->GetRenderData()->LODResources[0];
    if (!LOD.Sections.IsValidIndex(TargetMaterialSlotIndex)) return;
    
    const FStaticMeshVertexBuffer& VB      = LOD.VertexBuffers.StaticMeshVertexBuffer;
    const FPositionVertexBuffer&   PB      = LOD.VertexBuffers.PositionVertexBuffer;
    const FIndexArrayView          Indices = LOD.IndexBuffer.GetArrayView();
    const FStaticMeshSection&      Section = LOD.Sections[TargetMaterialSlotIndex];

    // Pre-calculate inverse transpose for correct normal transformation
    // Normals/Tangents must be transformed by the Inverse Transpose to handle non-uniform scaling
    const FMatrix LocalToWorldInvTrans = LocalToWorld.ToMatrixWithScale().Inverse().GetTransposed();

    for (uint32 tri_idx = 0; tri_idx < Section.NumTriangles; ++tri_idx)
    {
        const int32 tri = (Section.FirstIndex / 3) + tri_idx;

        const uint32 i0 = Indices[tri * 3 + 0];
        const uint32 i1 = Indices[tri * 3 + 1];
        const uint32 i2 = Indices[tri * 3 + 2];

        // --- UVs ---
        const FVector2D UV0(VB.GetVertexUV(i0, UVChannel));
        const FVector2D UV1(VB.GetVertexUV(i1, UVChannel));
        const FVector2D UV2(VB.GetVertexUV(i2, UVChannel));

        // --- VERTEX TANGENT BASIS ---
        // We MUST use the engine's stored tangents (VertexTangentX). 
        // Even though they split at UV seams, this split is required to handle UV rotation.
        const FVector T0 = (FVector)VB.VertexTangentX(i0);
        const FVector T1 = (FVector)VB.VertexTangentX(i1);
        const FVector T2 = (FVector)VB.VertexTangentX(i2);

        const FVector N0 = (FVector)VB.VertexTangentZ(i0);
        const FVector N1 = (FVector)VB.VertexTangentZ(i1);
        const FVector N2 = (FVector)VB.VertexTangentZ(i2);

        // Handedness (Sign) is stored in TangentZ.W. 
        // This flips the Bitangent to account for mirrored UVs.
        // We check for < 0 to determine sign (-1 or 1).
        const float S0 = VB.VertexTangentZ(i0).W < 0.0f ? -1.0f : 1.0f;
        const float S1 = VB.VertexTangentZ(i1).W < 0.0f ? -1.0f : 1.0f;
        const float S2 = VB.VertexTangentZ(i2).W < 0.0f ? -1.0f : 1.0f;

        // --- RASTERIZATION ---
        const int32 MinX = FMath::Clamp(FMath::FloorToInt(FMath::Min3(UV0.X, UV1.X, UV2.X) * W), 0, W - 1);
        const int32 MinY = FMath::Clamp(FMath::FloorToInt(FMath::Min3(UV0.Y, UV1.Y, UV2.Y) * H), 0, H - 1);
        const int32 MaxX = FMath::Clamp(FMath::CeilToInt(FMath::Max3(UV0.X, UV1.X, UV2.X) * W), 0, W - 1);
        const int32 MaxY = FMath::Clamp(FMath::CeilToInt(FMath::Max3(UV0.Y, UV1.Y, UV2.Y) * H), 0, H - 1);

        for (int32 py = MinY; py <= MaxY; ++py)
        {
            for (int32 px = MinX; px <= MaxX; ++px)
            {
                const FVector2D uv((px + 0.5f) / W, (py + 0.5f) / H);

                float a, b, c;
                // Standard barycentric check
                constexpr float kBaryEpsilon = -1e-4f;  // Accept slightly negative values
                if (!FMathUtils::CalculateBarycentricCoordinates(uv, UV0, UV1, UV2, a, b, c) || 
                    a < kBaryEpsilon || b < kBaryEpsilon || c < kBaryEpsilon)
                    continue;

                // 1. Interpolate Tangent (T) and Normal (N)
                // This mimics the Vertex Shader -> Pixel Shader interpolation.
                FVector T_Local = (T0 * a + T1 * b + T2 * c).GetSafeNormal();
                FVector N_Local = (N0 * a + N1 * b + N2 * c).GetSafeNormal();

                // 2. Gram-Schmidt Orthogonalization
                // Interpolation creates non-orthogonal vectors. We must force T to be perpendicular to N.
                // T = Normalize(T - N * Dot(T, N));
                T_Local = (T_Local - N_Local * FVector::DotProduct(T_Local, N_Local)).GetSafeNormal();

                // 3. Interpolate Sign
                // If signs differ across a face (rare but possible), we threshold.
                const float SignInterp = (S0 * a + S1 * b + S2 * c);
                const float FinalSign = SignInterp >= 0.0f ? 1.0f : -1.0f;

                // 4. Calculate Bitangent (B)
                // In Unreal/MikkTSpace, B = (N x T) * Sign
                FVector B_Local = FVector::CrossProduct(N_Local, T_Local) * FinalSign;

                // 5. Transform to World Space
                // Note: We use TransformVector (rot + scale), not TransformPosition.
                // Using the InverseTranspose matrix handles non-uniform scaling correctly.
                const int32 texel = py * W + px;
                OutTWorld[texel] = LocalToWorldInvTrans.TransformVector(T_Local).GetSafeNormal();
                OutBWorld[texel] = LocalToWorldInvTrans.TransformVector(B_Local).GetSafeNormal();
                OutNWorld[texel] = LocalToWorldInvTrans.TransformVector(N_Local).GetSafeNormal();
            }
        }
    }
}