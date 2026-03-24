#include "Helpers/MathUtils.h"

bool FMathUtils::CalculateBarycentricCoordinates(
    const FVector2D& P,
    const FVector2D& A,
    const FVector2D& B,
    const FVector2D& C,
    float& OutAlpha,
    float& OutBeta,
    float& OutGamma)
{
    // Compute vectors from A to other points
    FVector2D V0 = B - A;
    FVector2D V1 = C - A;
    FVector2D V2 = P - A;
    
    // Compute dot products
    float D00 = FVector2D::DotProduct(V0, V0);
    float D01 = FVector2D::DotProduct(V0, V1);
    float D11 = FVector2D::DotProduct(V1, V1);
    float D20 = FVector2D::DotProduct(V2, V0);
    float D21 = FVector2D::DotProduct(V2, V1);
    
    // Compute barycentric coordinates
    float Denom = D00 * D11 - D01 * D01;
    
    // Check for degenerate triangle
    if (FMath::Abs(Denom) < 0.0f)
    {
        OutAlpha = OutBeta = OutGamma = 0.0f;
        return false;
    }
    
    float InvDenom = 1.0f / Denom;
    OutBeta = (D11 * D20 - D01 * D21) * InvDenom;
    OutGamma = (D00 * D21 - D01 * D20) * InvDenom;
    OutAlpha = 1.0f - OutBeta - OutGamma;
    
    return true;
}

bool FMathUtils::IsPointInOrNearTriangle(
    const FVector2D& Point,
    const FVector2D& A,
    const FVector2D& B,
    const FVector2D& C,
    float Threshold)
{
    // Check if point is inside triangle using barycentric coordinates
    float Alpha, Beta, Gamma;
    if (CalculateBarycentricCoordinates(Point, A, B, C, Alpha, Beta, Gamma))
    {
        // If inside or very close to inside
        if (Alpha >= -Threshold && Beta >= -Threshold && Gamma >= -Threshold &&
            Alpha <= 1.0f + Threshold && Beta <= 1.0f + Threshold && Gamma <= 1.0f + Threshold)
        {
            return true;
        }
    }
    
    // If not inside, check distance to edges
    float DistAB = PointDistToSegmentSquared2D(Point, A, B);
    float DistBC = PointDistToSegmentSquared2D(Point, B, C);
    float DistCA = PointDistToSegmentSquared2D(Point, C, A);
    
    float ThresholdSquared = Threshold * Threshold;
    return DistAB <= ThresholdSquared || DistBC <= ThresholdSquared || DistCA <= ThresholdSquared;
}

float FMathUtils::PointDistToSegmentSquared2D(
    const FVector2D& Point,
    const FVector2D& SegStart,
    const FVector2D& SegEnd)
{
    const FVector2D SegDir = SegEnd - SegStart;
    const float SegLenSquared = SegDir.SizeSquared();
    
    // Handle degenerate segments (points)
    if (SegLenSquared < 1e-12f)
    {
        return FVector2D::DistSquared(Point, SegStart);
    }
    
    // Project point onto segment
    const FVector2D PointToStart = Point - SegStart;
    const float Dot = FVector2D::DotProduct(PointToStart, SegDir);
    const float T = FMath::Clamp(Dot / SegLenSquared, 0.0f, 1.0f);
    const FVector2D ClosestPointOnSeg = SegStart + T * SegDir;
    
    // Return squared distance
    return FVector2D::DistSquared(Point, ClosestPointOnSeg);
}


