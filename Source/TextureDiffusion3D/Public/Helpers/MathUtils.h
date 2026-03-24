// MathUtils.h
#pragma once

#include "CoreMinimal.h"

/**
 * Helper class for math operations used in texture projection
 */
class FMathUtils
{
public:
    /**
     * Calculate barycentric coordinates for a point in a triangle
     * @param P The point to calculate coordinates for
     * @param A First vertex of the triangle
     * @param B Second vertex of the triangle
     * @param C Third vertex of the triangle
     * @param OutAlpha First barycentric coordinate (for vertex A)
     * @param OutBeta Second barycentric coordinate (for vertex B)
     * @param OutGamma Third barycentric coordinate (for vertex C)
     * @return True if calculation was successful, false for degenerate triangles
     */
    static bool CalculateBarycentricCoordinates(
        const FVector2D& P,
        const FVector2D& A,
        const FVector2D& B,
        const FVector2D& C,
        float& OutAlpha,
        float& OutBeta,
        float& OutGamma);
    
    /**
     * Check if a point is inside or near a triangle
     * @param Point The point to check
     * @param A First vertex of the triangle
     * @param B Second vertex of the triangle
     * @param C Third vertex of the triangle
     * @param Threshold Distance threshold for "near" determination
     * @return True if the point is inside or near the triangle
     */
    static bool IsPointInOrNearTriangle(
        const FVector2D& Point,
        const FVector2D& A,
        const FVector2D& B,
        const FVector2D& C,
        float Threshold);
    
    /**
     * Calculate distance from a point to a line segment
     * @param Point The point
     * @param SegStart Start of the line segment
     * @param SegEnd End of the line segment
     * @return Squared distance from point to segment
     */
    static float PointDistToSegmentSquared2D(
        const FVector2D& Point,
        const FVector2D& SegStart,
        const FVector2D& SegEnd);

        /**
     * Rasterizes a line with thickness into a color buffer using Bresenham's algorithm.
     * @param Buffer The pixel buffer to draw into.
     * @param W The width of the buffer.
     * @param H The height of the buffer.
     * @param P1 The starting point of the line in pixel coordinates.
     * @param P2 The ending point of the line in pixel coordinates.
     * @param Color The color to draw the line.
     * @param Thickness The thickness of the line in pixels.
     */
    static void DrawLineInColorBufferWithDepth(
    TArray<FColor>& ColorBuffer,
    TArray<double>& ZBuffer,
    int32 Width,
    int32 Height,
    const FVector2D& P1,
    const FVector2D& P2,
    double Depth1,
    double Depth2,
    const FColor& Color,
    float Thickness);
};

