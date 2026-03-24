#include "Helpers/TextureUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "FileHelpers.h"
#include "Misc/MessageDialog.h"
#include "Engine/TextureDefines.h"
#include "TextureResource.h"
#include "ImageWriteBlueprintLibrary.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Engine/Canvas.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "EditorFramework/AssetImportData.h"
#include "Math/RandomStream.h"

#include <cmath>


static inline double SrgbToLinearUnit(double c)
{
    // c in [0,1] sRGB -> linear
    if (c <= 0.04045) return c / 12.92;
    return FMath::Pow((c + 0.055) / 1.055, 2.4);
}

static inline double LinearToSrgbUnit(double c)
{
    // clamp to [0,1] then linear -> sRGB
    c = FMath::Clamp(c, 0.0, 1.0);
    if (c <= 0.0031308) return 12.92 * c;
    return 1.055 * FMath::Pow(c, 1.0 / 2.4) - 0.055;
}

// --- sRGB (linear) <-> XYZ (D65) ---
static inline void LinearRgbToXyz(double R, double G, double B, double& X, double& Y, double& Z)
{
    // Matrix from linear sRGB to XYZ (D65)
    X = 0.4124564*R + 0.3575761*G + 0.1804375*B;
    Y = 0.2126729*R + 0.7151522*G + 0.0721750*B;
    Z = 0.0193339*R + 0.1191920*G + 0.9503041*B;
}

static inline void XyzToLinearRgb(double X, double Y, double Z, double& R, double& G, double& B)
{
    // Matrix from XYZ (D65) to linear sRGB
    R =  3.2404542*X - 1.5371385*Y - 0.4985314*Z;
    G = -0.9692660*X + 1.8760108*Y + 0.0415560*Z;
    B =  0.0556434*X - 0.2040259*Y + 1.0572252*Z;
}

static FORCEINLINE double CubeRoot(double x)
{
#if defined(__cpp_lib_math_special_functions) || (defined(_MSC_VER))
    return std::cbrt(x);                 // handles negatives correctly too
#else
    // Fallback: sign-preserving pow
    return (x >= 0.0) ? FMath::Pow(x, 1.0/3.0) : -FMath::Pow(-x, 1.0/3.0);
#endif
}
// --- XYZ <-> Lab (D65) ---
static inline double PivotXYZtoLab(double t)
{
    constexpr double e = 216.0 / 24389.0;   // ~0.008856
    // If above the breakpoint, use cube root; else linear segment per CIE standard
    return (t > e) ? CubeRoot(t) : (7.787037037037037 * t + (16.0 / 116.0));
}

static inline void XyzToLab(double X, double Y, double Z, double& L, double& a, double& b)
{
    // D65 reference white
    constexpr double Xn = 0.95047;
    constexpr double Yn = 1.00000;
    constexpr double Zn = 1.08883;

    double fx = PivotXYZtoLab(X / Xn);
    double fy = PivotXYZtoLab(Y / Yn);
    double fz = PivotXYZtoLab(Z / Zn);

    L = 116.0 * fy - 16.0;
    a = 500.0 * (fx - fy);
    b = 200.0 * (fy - fz);
}

static inline void LabToXyz(double L, double a, double b, double& X, double& Y, double& Z)
{
    // D65 reference white
    constexpr double Xn = 0.95047;
    constexpr double Yn = 1.00000;
    constexpr double Zn = 1.08883;

    double fy = (L + 16.0) / 116.0;
    double fx = fy + (a / 500.0);
    double fz = fy - (b / 200.0);

    auto fInv = [](double f) -> double
    {
        const double f3 = f * f * f;
        if (f3 > 0.008856451679035631) return f3;
        return (116.0 * f - 16.0) / 903.3;
    };

    X = Xn * fInv(fx);
    Y = Yn * fInv(fy);
    Z = Zn * fInv(fz);
}

static double WeightedMedianChannel(TArray<TPair<double,double>>& Samples, double TrimPercent)
{
    if (Samples.Num() == 0) return 0.0;

    Samples.Sort([](const TPair<double,double>& A, const TPair<double,double>& B)
    {
        return A.Key < B.Key; // ascending by value
    });

    double TotalW = 0.0;
    for (const auto& S : Samples) TotalW += S.Value;
    if (TotalW <= KINDA_SMALL_NUMBER) return Samples[Samples.Num()/2].Key;

    TrimPercent = FMath::Clamp(TrimPercent, 0.0, 0.49);
    const double LowCut  = TrimPercent * TotalW;
    const double HighCut = (1.0 - TrimPercent) * TotalW;
    const double Target  = 0.5 * (LowCut + HighCut); // median of the trimmed mass

    double Accum = 0.0;
    for (const auto& S : Samples)
    {
        Accum += S.Value;
        if (Accum >= Target)
        {
            return S.Key;
        }
    }
    return Samples.Last().Key; // fallback
}


static bool ReadTexturePixels_Safe(UTexture2D* Texture, TArray<FColor>& OutPixels, int32& OutW, int32& OutH)
{
    if (!IsValid(Texture)) return false;

    // 1) Try raw platform data if clearly BGRA8 and size matches
    if (Texture->GetPlatformData() && Texture->GetPlatformData()->Mips.Num() > 0)
    {
        const FTexturePlatformData* PD = Texture->GetPlatformData();
        const FTexture2DMipMap& Mip = PD->Mips[0];
        const int32 W = Mip.SizeX;
        const int32 H = Mip.SizeY;

        const void* Data = Mip.BulkData.LockReadOnly();
        const int64 Size = Mip.BulkData.GetBulkDataSize();
        const bool LooksLikeBGRA8 = (Size == int64(W) * int64(H) * 4);

        if (LooksLikeBGRA8)
        {
            OutPixels.SetNumUninitialized(W * H);
            FMemory::Memcpy(OutPixels.GetData(), Data, Size);
            Mip.BulkData.Unlock();
            OutW = W; OutH = H;
            return true;
        }
        Mip.BulkData.Unlock();
    }

    // 2) Fallback: decompress by drawing to an sRGB BGRA8 render target and reading back
#if WITH_EDITOR
    UWorld* World = nullptr;
    if (GEditor)
    {
        if (FWorldContext* Ctx = &GEditor->GetEditorWorldContext())
        {
            World = Ctx->World();
        }
    }
    if (!World) return false;

    const int32 W = Texture->GetSizeX();
    const int32 H = Texture->GetSizeY();

    UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>();
    RT->InitCustomFormat(W, H, PF_B8G8R8A8, /*bForceLinearGamma*/ false); // sRGB=true on resource
    RT->UpdateResourceImmediate(true);

    UCanvas* Canvas = nullptr;
    FVector2D CanvasSize;
    FDrawToRenderTargetContext Ctx;
    UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(World, RT, Canvas, CanvasSize, Ctx);
    if (Canvas)
    {
        Canvas->K2_DrawTexture(
            Texture,
            FVector2D::ZeroVector,
            CanvasSize,
            FVector2D::ZeroVector,
            FVector2D(1.f, 1.f),
            FLinearColor::White,
            EBlendMode::BLEND_Opaque
        );
    }
    UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(World, Ctx);

    FTextureRenderTargetResource* Res = RT->GameThread_GetRenderTargetResource();
    if (!Res || !Res->ReadPixels(OutPixels))
    {
        RT->MarkAsGarbage();
        return false;
    }

    OutW = RT->SizeX;
    OutH = RT->SizeY;
    RT->MarkAsGarbage();
    return true;
#else
    return false;
#endif
}

FLinearColor FTextureUtils::ComputeBaseColor_WeightedMedianLab(
    UTexture2D* Texture,
    float AlphaThreshold,
    float TrimPercent,
    int32 TargetSampleCount)
{
    TArray<FColor> Pixels;
    int32 W = 0, H = 0;
    if (!ReadTexturePixels_Safe(Texture, Pixels, W, H) || W <= 0 || H <= 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("ComputeBaseColor_WeightedMedianLab: Could not read pixels from texture."));
        return FLinearColor::Black;
    }

    auto ComputeFromPixels = [&](int32 StrideIn) -> FLinearColor
    {
        TArray<TPair<double,double>> L_Samples, a_Samples, b_Samples;
        L_Samples.Reserve(TargetSampleCount);
        a_Samples.Reserve(TargetSampleCount);
        b_Samples.Reserve(TargetSampleCount);

        const double AlphaCut = FMath::Clamp<double>(AlphaThreshold, 0.0, 1.0);

        for (int32 y = 0; y < H; y += StrideIn)
        {
            const int32 RowBase = y * W;
            for (int32 x = 0; x < W; x += StrideIn)
            {
                const FColor& C = Pixels[RowBase + x];
                const double w = double(C.A) / 255.0;
                if (w <= AlphaCut) continue;

                const double Rlin = double(C.R) / 255.0;
                const double Glin = double(C.G) / 255.0;
                const double Blin = double(C.B) / 255.0;

                double X, Y, Z; LinearRgbToXyz(Rlin, Glin, Blin, X, Y, Z);
                double L, a, b; XyzToLab(X, Y, Z, L, a, b);

                L_Samples.Emplace(L, w);
                a_Samples.Emplace(a, w);
                b_Samples.Emplace(b, w);
            }
        }

        if (L_Samples.Num() == 0) return FLinearColor(EForceInit::ForceInitToZero);

        const double Lm = WeightedMedianChannel(L_Samples, TrimPercent);
        const double am = WeightedMedianChannel(a_Samples, TrimPercent);
        const double bm = WeightedMedianChannel(b_Samples, TrimPercent);

        double X, Y, Z; LabToXyz(Lm, am, bm, X, Y, Z);
        double Rlin, Glin, Blin; XyzToLinearRgb(X, Y, Z, Rlin, Glin, Blin);

        return FLinearColor(
            float(FMath::Clamp(Rlin, 0.0, 1.0)),
            float(FMath::Clamp(Glin, 0.0, 1.0)),
            float(FMath::Clamp(Blin, 0.0, 1.0)),
            1.0f
        );
    };

    // Choose an initial stride to hit the sample budget.
    int32 Stride = 1;
    if (TargetSampleCount > 0)
    {
        const double Num = double(W) * double(H);
        const double Sugg = FMath::Sqrt(Num / FMath::Max(1.0, double(TargetSampleCount)));
        Stride = FMath::Clamp(int32(FMath::FloorToInt(Sugg)), 1, 16);
    }

    FLinearColor Result = ComputeFromPixels(Stride);

    // Adaptive fallback: if no samples (0,0,0), retry full scan
    if (Result.R == 0.f && Result.G == 0.f && Result.B == 0.f)
    {
        // If the alpha area is tiny we may have stepped over it. Try Stride=1.
        Result = ComputeFromPixels(1);

        // If still black, optionally try lowering the alpha threshold
        if (Result.R == 0.f && Result.G == 0.f && Result.B == 0.f && AlphaThreshold > 0.0f)
        {
            const float BackupAlpha = 0.0f;
            // Temporarily recompute with AlphaThreshold=0 for this last pass
            const float SavedAlpha = AlphaThreshold;
            AlphaThreshold = BackupAlpha;
            Result = ComputeFromPixels(1);
            AlphaThreshold = SavedAlpha;
        }
    }

    return Result;
}


bool FTextureUtils::MatchTextureColor(
    const TArray<FColor>& SourcePixels, int32 Wsrc, int32 Hsrc,
    const TArray<FColor>& TargetPixels, int32 Wref, int32 Href,
    TArray<FColor>& OutMatchedPixels)
{
    // --- VALIDATION ---
    if (SourcePixels.Num() == 0 || TargetPixels.Num() == 0)
    {
        UE_LOG(LogTemp, Error, TEXT("MatchTextureColor: Input pixel arrays cannot be empty."));
        return false;
    }

    // --- SETTINGS (Copied from your function) ---
    const int32   SampleStride          = 2;
    const float   HistogramMinPercent   = 0.05f;
    const int32   HistogramBins         = 32;
    const float   SatDarkThreshold      = 0.05f;
    const float   SatClampMin           = 0.70f;
    const float   SatClampMax           = 1.30f;
    const float   EVDeadzone            = 0.03f;
    const float   EVClampMin            = -1.50f;
    const float   EVClampMax            = 1.50f;
    const float   BrightClampMin        = -0.05f;
    const float   BrightClampMax        = 0.05f;
    const uint8   AlphaCutoff           = 16;

    // --- HELPER LAMBDAS (Self-Contained inside this function) ---
    auto SrgbToLinear = [](float c) { return (c <= 0.04045f) ? (c / 12.92f) : FMath::Pow((c + 0.055f) / 1.055f, 2.4f); };
    auto LinearToSrgb = [](float c) { c = FMath::Clamp(c, 0.0f, 1.0f); return (c <= 0.0031308f) ? (12.92f * c) : (1.055f * FMath::Pow(c, 1.0f/2.4f) - 0.055f); };

    auto RGBToHSV = [](const FColor& C) -> FVector {
        const FLinearColor Lin = FLinearColor::FromSRGBColor(C);
        const FLinearColor HSV = Lin.LinearRGBToHSV();
        return FVector(HSV.R, HSV.G, HSV.B);
    };

    auto HSVToRGB_KeepA = [](const FVector& HSV, uint8 A) -> FColor {
        const FLinearColor HSVLin(HSV.X, HSV.Y, HSV.Z, 1.0f);
        const FLinearColor LinRGB = HSVLin.HSVToLinearRGB();
        FColor Out = LinRGB.ToFColor(true); // bSRGB=true
        Out.A = A;
        return Out;
    };
    
    const FVector3f LumaW(0.2126f, 0.7152f, 0.0722f);
    auto LumaLin = [&](const FColor& p) -> float {
        const float r = SrgbToLinear(p.R / 255.f);
        const float g = SrgbToLinear(p.G / 255.f);
        const float b = SrgbToLinear(p.B / 255.f);
        return r * LumaW.X + g * LumaW.Y + b * LumaW.Z;
    };

    auto HistogramFilteredMean = [&](const TArray<FColor>& Img, int32 W, int32 H) -> float {
        TArray<float> AllSamples;
        AllSamples.Reserve((W / SampleStride + 1) * (H / SampleStride + 1));
        for (int32 y = 0; y < H; y += SampleStride) {
            for (int32 x = 0; x < W; x += SampleStride) {
                const FColor p = Img[y * W + x];
                if (p.A > AlphaCutoff) AllSamples.Add(LumaLin(p));
            }
        }
        if (AllSamples.IsEmpty()) return 0.f;
        
        TArray<int32> HistogramCounts;
        HistogramCounts.Init(0, HistogramBins);
        for (float Luma : AllSamples) {
            HistogramCounts[FMath::Clamp(static_cast<int32>(Luma * HistogramBins), 0, HistogramBins - 1)]++;
        }
        
        const int32 MinPixelsInBin = FMath::Max(1, static_cast<int32>(AllSamples.Num() * HistogramMinPercent));
        double Sum = 0.0;
        int32 Count = 0;
        for (float Sample : AllSamples) {
            int32 Bin = FMath::Clamp(static_cast<int32>(Sample * HistogramBins), 0, HistogramBins - 1);
            if (HistogramCounts[Bin] >= MinPixelsInBin) {
                Sum += Sample;
                Count++;
            }
        }
        return (Count > 0) ? static_cast<float>(Sum / Count) : 0.f;
    };

    // --- ALGORITHM EXECUTION ---
    
    // Calculate exposure adjustment
    float MuSrcL = HistogramFilteredMean(SourcePixels, Wsrc, Hsrc);
    float MuRefL = HistogramFilteredMean(TargetPixels, Wref, Href);
    const float Eps = 1e-6f;
    float AutoEV = FMath::Log2((MuRefL + Eps) / FMath::Max(MuSrcL, Eps));
    if (FMath::Abs(AutoEV) < EVDeadzone) AutoEV = 0.0f;
    AutoEV = FMath::Clamp(AutoEV, EVClampMin, EVClampMax);
    const float ExposureGain = FMath::Pow(2.0f, AutoEV);

    // Calculate saturation adjustment
    auto MeanS_Masked = [&](const TArray<FColor>& Img, int32 W, int32 H) -> float {
        double Sum = 0.0;
        int32 Count = 0;
        for (int32 y = 0; y < H; y += SampleStride) {
            for (int32 x = 0; x < W; x += SampleStride) {
                const FColor px = Img[y * W + x];
                if (px.A <= AlphaCutoff) continue;
                const FVector hsv = RGBToHSV(px);
                if (hsv.Z > SatDarkThreshold) {
                    Sum += hsv.Y;
                    Count++;
                }
            }
        }
        return (Count > 0) ? static_cast<float>(Sum / Count) : 0.f;
    };
    float MeanSs = MeanS_Masked(SourcePixels, Wsrc, Hsrc);
    float MeanSr = MeanS_Masked(TargetPixels, Wref, Href);
    float SatMul = (MeanSs > Eps) ? (MeanSr / FMath::Max(MeanSs, Eps)) : 1.0f;
    SatMul = FMath::Clamp(SatMul, SatClampMin, SatClampMax);

    // Calculate brightness adjustment
    auto MeanV = [&](const TArray<FColor>& Img, int32 W, int32 H, float Gain) -> float {
        double Sum = 0.0;
        int32 Count = 0;
        for (int32 y = 0; y < H; y += SampleStride) {
            for (int32 x = 0; x < W; x += SampleStride) {
                const FColor p = Img[y * W + x];
                if (p.A <= AlphaCutoff) continue;
                
                float r = SrgbToLinear(p.R / 255.f) * Gain;
                float g = SrgbToLinear(p.G / 255.f) * Gain;
                float b = SrgbToLinear(p.B / 255.f) * Gain;
                
                FColor expC(static_cast<uint8>(FMath::RoundToInt(255.f * LinearToSrgb(r))), 
                              static_cast<uint8>(FMath::RoundToInt(255.f * LinearToSrgb(g))), 
                              static_cast<uint8>(FMath::RoundToInt(255.f * LinearToSrgb(b))), p.A);
                Sum += RGBToHSV(expC).Z;
                Count++;
            }
        }
        return (Count > 0) ? static_cast<float>(Sum / Count) : 0.f;
    };
    float MuV_src_afterEV = MeanV(SourcePixels, Wsrc, Hsrc, ExposureGain);
    float MuV_ref = MeanV(TargetPixels, Wref, Href, 1.0f);
    float BrightNudge = FMath::Clamp(MuV_ref - MuV_src_afterEV, BrightClampMin, BrightClampMax);
    
    UE_LOG(LogTemp, Log, TEXT("MatchTextureColor values: AutoEV=%.3f, SatMul=%.3f, BrightNudge=%.3f"), AutoEV, SatMul, BrightNudge);

    // --- FINAL PIXEL PROCESSING ---
    OutMatchedPixels.SetNumUninitialized(Wsrc * Hsrc);

    for (int32 i = 0; i < SourcePixels.Num(); ++i)
    {
        const FColor s = SourcePixels[i];
        if (s.A <= AlphaCutoff) {
            OutMatchedPixels[i] = FColor(0, 0, 0, s.A);
            continue;
        }

        // Apply exposure in linear RGB
        float r_lin = SrgbToLinear(s.R / 255.0f) * ExposureGain;
        float g_lin = SrgbToLinear(s.G / 255.0f) * ExposureGain;
        float b_lin = SrgbToLinear(s.B / 255.0f) * ExposureGain;
        
        FColor expSRGB(static_cast<uint8>(FMath::RoundToInt(255.0f * LinearToSrgb(r_lin))), 
                         static_cast<uint8>(FMath::RoundToInt(255.0f * LinearToSrgb(g_lin))), 
                         static_cast<uint8>(FMath::RoundToInt(255.0f * LinearToSrgb(b_lin))), s.A);

        // Apply saturation and brightness in HSV
        FVector hsv = RGBToHSV(expSRGB);
        hsv.Y = FMath::Clamp(hsv.Y * SatMul, 0.0f, 1.0f);
        hsv.Z = FMath::Clamp(hsv.Z + BrightNudge, 0.0f, 1.0f);

        OutMatchedPixels[i] = HSVToRGB_KeepA(hsv, s.A);
    }

    return true; // Signal success
}
    void FTextureUtils::InitializeBuffers(
        int32 Width,
        int32 Height,
        TArray<bool>& OutVisibilityBuffer,
        TArray<FVector>& OutWorldPositionBuffer,
        TArray<FVector2D>& OutScreenPositionBuffer,
        TArray<FVector2D>& OutUVBuffer,
        TArray<float>& OutDepthBuffer)
    {
        const int32 NumTexels = Width * Height;
        
        // Initialize all arrays to the correct size
        OutVisibilityBuffer.Empty(NumTexels);
        OutVisibilityBuffer.Init(false, NumTexels);
        
        OutWorldPositionBuffer.Empty(NumTexels);
        OutWorldPositionBuffer.Init(FVector::ZeroVector, NumTexels);
        
        OutScreenPositionBuffer.Empty(NumTexels);
        OutScreenPositionBuffer.Init(FVector2D::ZeroVector, NumTexels);
        
        OutUVBuffer.Empty(NumTexels);
        OutUVBuffer.Init(FVector2D::ZeroVector, NumTexels);
        
        OutDepthBuffer.Empty(NumTexels);
        OutDepthBuffer.Init(MAX_FLT, NumTexels);  // Initialize to maximum depth
    }

       UTexture2D* FTextureUtils::CreateTextureFromPixelData(
    int32 Width,
    int32 Height,
    const TArray<FColor>& PixelData)
    {
        if (Width <= 0 || Height <= 0 || PixelData.Num() != Width * Height)
        {
            UE_LOG(LogTemp, Warning, TEXT("CreateTextureFromPixelData: Invalid input data"));
            return nullptr;
        }

        UTexture2D* NewTexture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
        if (!NewTexture)
        {
            UE_LOG(LogTemp, Warning, TEXT("CreateTextureFromPixelData: Failed to create transient texture"));
            return nullptr;
        }

        // --- CORRECTED LOGIC ---
        // The pixel data we receive is in Linear space. We MUST flag the texture
        // as SRGB=false so the engine knows not to apply an incorrect de-gamma.
        NewTexture->CompressionSettings = TC_VectorDisplacementmap; // Use an uncompressed format.
        NewTexture->SRGB = false; // CORRECT: Tell the engine the source data is Linear.
        NewTexture->Filter = TF_Bilinear;
        NewTexture->AddressX = TA_Clamp;
        NewTexture->AddressY = TA_Clamp;
        NewTexture->UpdateResource();

        FTexture2DMipMap& Mip = NewTexture->GetPlatformData()->Mips[0];
        void* TextureData = Mip.BulkData.Lock(LOCK_READ_WRITE);
        if (TextureData)
        {
            FMemory::Memcpy(TextureData, PixelData.GetData(), PixelData.Num() * sizeof(FColor));
            Mip.BulkData.Unlock();
        }
        
        NewTexture->UpdateResource();
        
        return NewTexture;
    }

    FLinearColor FTextureUtils::SampleTextureBilinear_HDR(
    const TArray<FFloat16Color>& SourcePixels, 
    int32 SourceWidth, int32 SourceHeight, 
    const FVector2D& UV)
{
    if (SourcePixels.Num() == 0 || SourceWidth <= 0 || SourceHeight <= 0) return FLinearColor::Black;

    // Bilinear setup
    const float xf = UV.X * (SourceWidth - 1);
    const float yf = UV.Y * (SourceHeight - 1);
    const int32 x0 = FMath::Clamp(FMath::FloorToInt(xf), 0, SourceWidth - 1);
    const int32 y0 = FMath::Clamp(FMath::FloorToInt(yf), 0, SourceHeight - 1);
    const int32 x1 = FMath::Clamp(x0 + 1, 0, SourceWidth - 1);
    const int32 y1 = FMath::Clamp(y0 + 1, 0, SourceHeight - 1);

    const float wx = xf - FMath::FloorToFloat(xf);
    const float wy = yf - FMath::FloorToFloat(yf);
    const float w00 = (1 - wx) * (1 - wy);
    const float w10 = wx * (1 - wy);
    const float w01 = (1 - wx) * wy;
    const float w11 = wx * wy;

    // Read 16-bit FFloat16Color and convert to 32-bit FLinearColor
    const FLinearColor c00(SourcePixels[y0 * SourceWidth + x0]);
    const FLinearColor c10(SourcePixels[y0 * SourceWidth + x1]);
    const FLinearColor c01(SourcePixels[y1 * SourceWidth + x0]);
    const FLinearColor c11(SourcePixels[y1 * SourceWidth + x1]);

    // --- DIRECT INTERPOLATION (no premultiply) ---
    const FLinearColor result = 
        c00 * w00 + c10 * w10 + c01 * w01 + c11 * w11;

    return result;
}
/**
 * Creates a transient 16-bit float (HDR) UTexture2D from a 32-bit linear pixel array.
 * This texture will have the PF_FloatRGBA pixel format and TC_RGBA16F compression.
 */
UTexture2D* FTextureUtils::CreateTextureFromLinearPixelData(int32 Width, int32 Height, const TArray<FLinearColor>& LinearPixels)
{
	if (Width <= 0 || Height <= 0 || LinearPixels.Num() != Width * Height)
	{
		UE_LOG(LogTemp, Error, TEXT("CreateTextureFromLinearPixelData: Invalid dimensions or pixel array size."));
		return nullptr;
	}

	// 1. Create the transient texture with a 16-BIT FLOAT PIXEL FORMAT.
	// PF_FloatRGBA is the 16-bit-per-channel float format (using FFloat16Color).
	UTexture2D* NewTexture = UTexture2D::CreateTransient(Width, Height, PF_FloatRGBA);
	if (!NewTexture)
	{
		UE_LOG(LogTemp, Error, TEXT("CreateTextureFromLinearPixelData: Failed to create transient texture."));
		return nullptr;
	}

	// 2. Lock the texture for writing
	FTexture2DMipMap& Mip = NewTexture->GetPlatformData()->Mips[0];
	void* MipData = Mip.BulkData.Lock(LOCK_READ_WRITE);
	
	// 3. Cast the destination to the correct 16-bit float format (8 bytes per pixel)
	FFloat16Color* DestPixels = static_cast<FFloat16Color*>(MipData);

	// 4. Convert 32-bit FLinearColor to 16-bit FFloat16Color, preserving float data
	for (int32 i = 0; i < LinearPixels.Num(); ++i)
	{
		DestPixels[i] = FFloat16Color(LinearPixels[i]);
	}

	// 5. Unlock, set properties, and update
	Mip.BulkData.Unlock();
	
	NewTexture->SRGB = false; // CRITICAL: This is linear data.
	
	// CRITICAL: Use the 16-bit float uncompressed setting that preserves all 4 channels.
	NewTexture->CompressionSettings = TC_HDR_F32; 
	
	NewTexture->UpdateResource();

	return NewTexture;
}
    
    bool FTextureUtils::SaveVisibilityBufferAsTexture(
        const TArray<bool>& VisibilityBuffer,
        int32 TextureWidth,
        int32 TextureHeight,
        const FString& PackagePath,
        const FString& AssetName)
    {
        const int32 NumTexels = TextureWidth * TextureHeight;
        
        if (VisibilityBuffer.Num() != NumTexels)
        {
            UE_LOG(LogTemp, Warning, TEXT("SaveVisibilityBufferAsTexture: Buffer size mismatch"));
            return false;
        }
        
        // Create a color buffer where visible texels are white, invisible are black
        TArray<FColor> ColorBuffer;
        ColorBuffer.SetNumUninitialized(NumTexels);
        
        int32 VisibleTexelsCount = 0;
        
        // For each texel, set the color based on visibility
        for (int32 i = 0; i < NumTexels; i++)
        {
            ColorBuffer[i] = VisibilityBuffer[i] ? FColor::White : FColor::Black;
            if (VisibilityBuffer[i])
            {
                VisibleTexelsCount++;
            }
        }
        
        // Create a texture from this buffer
        UTexture2D* OutputTexture = CreateTextureFromPixelData(TextureWidth, TextureHeight, ColorBuffer);
        if (!OutputTexture)
        {
            UE_LOG(LogTemp, Warning, TEXT("SaveVisibilityBufferAsTexture: Failed to create texture"));
            return false;
        }
        
        // Save the texture as an asset
        bool bSaved = SaveTextureAsAsset(OutputTexture, PackagePath, AssetName);
        
        UE_LOG(LogTemp, Log, TEXT("SaveVisibilityBufferAsTexture: %d visible texels out of %d total (%.1f%%)"),
               VisibleTexelsCount, NumTexels, (VisibleTexelsCount * 100.0f) / NumTexels);
               
        if (bSaved)
        {
            FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(
                FString::Printf(TEXT("Visibility buffer saved!\nVisible Texels: %d / %d (%.1f%%)\nSaved to: %s/%s"), 
                              VisibleTexelsCount, NumTexels, 
                              (VisibleTexelsCount * 100.0f) / NumTexels,
                              *PackagePath, *AssetName)));
        }
        else
        {
            FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("Failed to save visibility buffer texture. Check output log for details."));
        }
        
        return bSaved;
    }

    bool FTextureUtils::SaveTextureAsAsset(
        UTexture2D* Texture,
        const FString& PackagePath,
        const FString& AssetName)
    {
        if (!Texture)
        {
            UE_LOG(LogTemp, Warning, TEXT("SaveTextureAsAsset: Invalid texture"));
            return false;
        }
        
        // Asset Registry for checking existing assets
        FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
        
        // Generate a unique asset name to avoid overwriting existing assets
        FString UniquePackagePath = PackagePath;
        FString UniqueAssetName = AssetName;
        int32 Suffix = 0;
        
        while (true)
        {
            // Check if asset already exists
            FSoftObjectPath ObjectPath(FString::Printf(TEXT("%s/%s"), *UniquePackagePath, *UniqueAssetName));
            FAssetData ExistingAsset = AssetRegistryModule.Get().GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
            
            if (!ExistingAsset.IsValid())
            {
                // Unique name found
                break;
            }
            
            // Try another name
            Suffix++;
            UniqueAssetName = FString::Printf(TEXT("%s_%d"), *AssetName, Suffix);
        }
        
        // Create the package
        FString FullPackagePath = FString::Printf(TEXT("%s/%s"), *UniquePackagePath, *UniqueAssetName);
        UPackage* Package = CreatePackage(*FullPackagePath);
        if (!Package)
        {
            UE_LOG(LogTemp, Warning, TEXT("SaveTextureAsAsset: Failed to create package %s"), *FullPackagePath);
            return false;
        }
        
        // Set appropriate flags for the texture
        Texture->Rename(*UniqueAssetName, Package);
        Texture->ClearFlags(RF_Transient);
        Texture->SetFlags(RF_Public | RF_Standalone);
        
        // Mark the package as dirty
        Texture->MarkPackageDirty();
        
        // Notify asset registry
        FAssetRegistryModule::AssetCreated(Texture);
        
        // Save the package
        TArray<UPackage*> PackagesToSave;
        PackagesToSave.Add(Package);
        
        bool bSaved = UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false);
        
        if (bSaved)
        {
            UE_LOG(LogTemp, Log, TEXT("SaveTextureAsAsset: Saved texture as %s"), *FullPackagePath);
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("SaveTextureAsAsset: Failed to save texture"));
        }
        
        return bSaved;
    }

    bool FTextureUtils::SaveTextureFromVisibleTexels(
        const TArray<bool>& VisibilityBuffer,
        const TArray<FVector2D>& UVBuffer,
        const TArray<FColor>& NormalColorBuffer,  // New parameter with normal colors
        int32 TextureWidth,
        int32 TextureHeight,
        const FString& PackagePath,
        const FString& AssetName)
    {
        const int32 NumTexels = TextureWidth * TextureHeight;
        
        if (VisibilityBuffer.Num() != NumTexels || UVBuffer.Num() != NumTexels || NormalColorBuffer.Num() != NumTexels)
        {
            UE_LOG(LogTemp, Warning, TEXT("SaveTextureFromVisibleTexels: Buffer size mismatch"));
            return false;
        }
        
        // Create a new texture
        UTexture2D* OutputTexture = UTexture2D::CreateTransient(TextureWidth, TextureHeight, PF_B8G8R8A8);
        if (!OutputTexture)
        {
            UE_LOG(LogTemp, Warning, TEXT("SaveTextureFromVisibleTexels: Failed to create transient texture"));
            return false;
        }
        
        // Set texture properties
        OutputTexture->CompressionSettings = TC_VectorDisplacementmap;
        OutputTexture->MipGenSettings = TMGS_NoMipmaps;
        OutputTexture->SRGB = false;
        OutputTexture->Filter = TF_Default;
        OutputTexture->AddressX = TA_Clamp;
        OutputTexture->AddressY = TA_Clamp;
        
        // Lock the texture for modification
        FTexture2DMipMap& Mip = OutputTexture->GetPlatformData()->Mips[0];
        FColor* MipData = static_cast<FColor*>(Mip.BulkData.Lock(LOCK_READ_WRITE));
        
        // Initialize texture data to black
        for (int32 i = 0; i < NumTexels; i++)
        {
            MipData[i] = FColor::Black;
        }
        
        int32 VisibleTexelsCount = 0;
        
        // For each texel, if visible, use the computed normal color
        for (int32 i = 0; i < NumTexels; i++)
        {
            if (VisibilityBuffer[i])
            {
                MipData[i] = NormalColorBuffer[i];
                VisibleTexelsCount++;
            }
        }
        
        // Unlock the texture
        Mip.BulkData.Unlock();
        
        // Update the texture
        OutputTexture->UpdateResource();
        
        UE_LOG(LogTemp, Log, TEXT("SaveTextureFromVisibleTexels: %d visible texels out of %d total"),
            VisibleTexelsCount, NumTexels);
        
        // Save the texture as an asset
        bool bSaved = SaveTextureAsAsset(OutputTexture, PackagePath, AssetName);
        
        if (bSaved)
        {
            UE_LOG(LogTemp, Log, TEXT("Saved projection texture successfully to %s"), *AssetName);
            FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(
                FString::Printf(TEXT("Projection complete!\nVisible Texels: %d / %d\nSaved to: %s/%s"), 
                                VisibleTexelsCount, NumTexels, *PackagePath, *AssetName)));
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("Failed to save projection texture"));
            FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("Failed to save texture. Check output log for details."));
        }
        
        return bSaved;
    }



    bool FTextureUtils::ProjectTextureOntoMesh(
    UTexture2D* SourceTexture,
    const TArray<bool>& VisibilityBuffer,
    const TArray<FVector2D>& ScreenPositionBuffer,
    int32 TextureWidth,
    int32 TextureHeight,
    const FString& PackagePath,
    const FString& AssetName)
    {

        if (!SourceTexture)
        {
            UE_LOG(LogTemp, Warning, TEXT("ProjectTextureOntoMesh: No source texture provided"));
            return false;
        }

        const int32 NumTexels = TextureWidth * TextureHeight;

        if (VisibilityBuffer.Num() != NumTexels || ScreenPositionBuffer.Num() != NumTexels)
        {
            UE_LOG(LogTemp, Warning, TEXT("ProjectTextureOntoMesh: Buffer size mismatch"));
            return false;
        }

        // Create a new texture
        UTexture2D* OutputTexture = UTexture2D::CreateTransient(TextureWidth, TextureHeight, PF_B8G8R8A8);
        if (!OutputTexture)
        {
            UE_LOG(LogTemp, Warning, TEXT("ProjectTextureOntoMesh: Failed to create transient texture"));
            return false;
        }

        // Set texture properties
        OutputTexture->CompressionSettings = TC_VectorDisplacementmap;
        OutputTexture->MipGenSettings = TMGS_NoMipmaps;
        OutputTexture->SRGB = false;
        OutputTexture->Filter = TF_Default;
        OutputTexture->AddressX = TA_Clamp;
        OutputTexture->AddressY = TA_Clamp;

        // Get source texture data
        FTextureSource& SourceTextureSource = SourceTexture->Source;
        TArray<uint8, FDefaultAllocator64> SourceData;
        SourceTextureSource.GetMipData(SourceData, 0);

        int32 SourceWidth = SourceTextureSource.GetSizeX();
        int32 SourceHeight = SourceTextureSource.GetSizeY();
        ETextureSourceFormat SourceFormat = SourceTextureSource.GetFormat();
        int32 SourceBPP = SourceTextureSource.GetBytesPerPixel();

        // Log source texture information for debugging
        UE_LOG(LogTemp, Log, TEXT("Source texture: %dx%d, Format: %d, BPP: %d"), 
        SourceWidth, SourceHeight, SourceFormat, SourceBPP);

        // Lock the output texture for modification
        FTexture2DMipMap& Mip = OutputTexture->GetPlatformData()->Mips[0];
        FColor* MipData = static_cast<FColor*>(Mip.BulkData.Lock(LOCK_READ_WRITE));

        // Initialize texture data to transparent black
        for (int32 i = 0; i < NumTexels; i++)
        {
            MipData[i] = FColor(0, 0, 0, 0);
        }

        int32 VisibleTexelsCount = 0;

        // For each texel, if visible, sample from the source texture
        for (int32 i = 0; i < NumTexels; i++)
        {
            if (VisibilityBuffer[i])
            {
                // Get the screen position (normalized [0,1] coordinates)
                    const FVector2D& ScreenPos = ScreenPositionBuffer[i];

                // Apply bilinear filtering when sampling the texture
                // This helps prevent aliasing and streaking
                float U = ScreenPos.X;
                float V = ScreenPos.Y;

                // Clamp UV coordinates to valid range [0,1]
                U = FMath::Clamp(U, 0.0f, 1.0f);
                V = FMath::Clamp(V, 0.0f, 1.0f);

                // Convert to source texture coordinates (with proper scaling)
                float SourceXf = U * (SourceWidth - 1);
                float SourceYf = V * (SourceHeight - 1);

                // Calculate the four surrounding pixels for bilinear interpolation
                int32 X0 = FMath::FloorToInt(SourceXf);
                int32 Y0 = FMath::FloorToInt(SourceYf);
                int32 X1 = FMath::Min(X0 + 1, SourceWidth - 1);
                int32 Y1 = FMath::Min(Y0 + 1, SourceHeight - 1);

                // Calculate the fractional parts for interpolation weights
                float Wx = SourceXf - X0;
                float Wy = SourceYf - Y0;

                // Sample the four surrounding pixels
                FColor C00 = SampleSourceTexture(SourceData, X0, Y0, SourceWidth, SourceFormat);
                FColor C10 = SampleSourceTexture(SourceData, X1, Y0, SourceWidth, SourceFormat);
                FColor C01 = SampleSourceTexture(SourceData, X0, Y1, SourceWidth, SourceFormat);
                FColor C11 = SampleSourceTexture(SourceData, X1, Y1, SourceWidth, SourceFormat);

                // Perform bilinear interpolation on each color channel
                uint8 R = FMath::RoundToInt(
                (1.0f - Wx) * (1.0f - Wy) * C00.R +
                Wx * (1.0f - Wy) * C10.R +
                (1.0f - Wx) * Wy * C01.R +
                Wx * Wy * C11.R
                );

                uint8 G = FMath::RoundToInt(
                (1.0f - Wx) * (1.0f - Wy) * C00.G +
                Wx * (1.0f - Wy) * C10.G +
                (1.0f - Wx) * Wy * C01.G +
                Wx * Wy * C11.G
                );

                uint8 B = FMath::RoundToInt(
                (1.0f - Wx) * (1.0f - Wy) * C00.B +
                Wx * (1.0f - Wy) * C10.B +
                (1.0f - Wx) * Wy * C01.B +
                Wx * Wy * C11.B
                );

                uint8 A = FMath::RoundToInt(
                (1.0f - Wx) * (1.0f - Wy) * C00.A +
                Wx * (1.0f - Wy) * C10.A +
                (1.0f - Wx) * Wy * C01.A +
                Wx * Wy * C11.A
                );

                // Write the interpolated color to the output
                MipData[i] = FColor(R, G, B, A);
                VisibleTexelsCount++;
            }
        }

        // int32 ExtendedTexels = 0;
        // if (ExtendTextureMargins(TextureWidth, TextureHeight, 1.5, MipData))
        // {
        //     // Count how many texels were actually extended
        //     for (int32 i = 0; i < NumTexels; i++)
        //     {
        //         if (!VisibilityBuffer[i] && MipData[i].A > 0)
        //         {
        //             ExtendedTexels++;
        //         }
        //     }
        //     UE_LOG(LogTemp, Log, TEXT("Extended %d texels around UV edges"), ExtendedTexels);
        // }

        // Unlock the texture
        Mip.BulkData.Unlock();

        // Update the texture
        OutputTexture->UpdateResource();

        UE_LOG(LogTemp, Log, TEXT("ProjectTextureOntoMesh: %d visible texels out of %d total"),
        VisibleTexelsCount, NumTexels);

        // Save the texture as an asset
        bool bSaved = SaveTextureAsAsset(OutputTexture, PackagePath, AssetName);

        if (bSaved)
        {
            UE_LOG(LogTemp, Log, TEXT("Saved projection texture successfully to %s"), *AssetName);
            FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(
            FString::Printf(TEXT("Projection complete!\nVisible Texels: %d / %d\nSaved to: %s/%s"), 
            VisibleTexelsCount, NumTexels, *PackagePath, *AssetName)));
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("Failed to save projection texture"));
            FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("Failed to save texture. Check output log for details."));
        }

        return bSaved;
        }


        FColor FTextureUtils::SampleSourceTexture(const TArray<uint8, FDefaultAllocator64>& SourceData, 
            int32 X, int32 Y, int32 SourceWidth, 
            ETextureSourceFormat SourceFormat)
        {
        FColor SampledColor;

        // Different handling based on source format
        switch ((int32)SourceFormat)
        {
            case 2: // TSF_BGRA8
            {
                int32 SourceIndex = (Y * SourceWidth + X) * 4;
                SampledColor = FColor(
                SourceData[SourceIndex + 2],  // R (from B)
                SourceData[SourceIndex + 1],  // G
                SourceData[SourceIndex],      // B (from R)
                SourceData[SourceIndex + 3]   // A
                );
                break;
            }

            case 3: // TSF_BGRE8
            {
                // Handle BGRE8 (HDR format) - similar to BGRA8 but needs special handling for exponent
                int32 SourceIndex = (Y * SourceWidth + X) * 4;
                SampledColor = FColor(
                SourceData[SourceIndex + 2],  // R (from B)
                SourceData[SourceIndex + 1],  // G
                SourceData[SourceIndex],      // B (from R)
                SourceData[SourceIndex + 3]   // E (exponent)
                );
                break;
            }

            case 4: // TSF_RGBA16
            case 5: // TSF_RGBA16F
            {
                // Handle 16-bit formats - need to sample 16-bit values and convert to 8-bit
                int32 SourceIndex = (Y * SourceWidth + X) * 8; // 8 bytes per pixel (2 bytes per channel * 4 channels)

                // Read 16-bit values (assuming little-endian)
                uint16 R = *(uint16*)&SourceData[SourceIndex];
                uint16 G = *(uint16*)&SourceData[SourceIndex + 2];
                uint16 B = *(uint16*)&SourceData[SourceIndex + 4];
                uint16 A = *(uint16*)&SourceData[SourceIndex + 6];

                // Convert 16-bit to 8-bit (divide by 257 to go from 0-65535 to 0-255)
                SampledColor = FColor(
                R / 257,
                G / 257,
                B / 257,
                A / 257
                );
                break;
            }

            case 6: // TSF_RGBA8_DEPRECATED
            {
                // This is the old RGBA8 format (which is RGBA order instead of BGRA)
                int32 SourceIndex = (Y * SourceWidth + X) * 4;
                SampledColor = FColor(
                SourceData[SourceIndex],      // R
                SourceData[SourceIndex + 1],  // G
                SourceData[SourceIndex + 2],  // B
                SourceData[SourceIndex + 3]   // A
                );
                break;
            }

            case 1: // TSF_G8 (grayscale)
            {
                int32 SourceIndex = Y * SourceWidth + X; // 1 byte per pixel
                uint8 Gray = SourceData[SourceIndex];
                SampledColor = FColor(Gray, Gray, Gray, 255); // Full alpha
                break;
            }

            default:
            {
                // Default to white if format not handled
                SampledColor = FColor::White;
                UE_LOG(LogTemp, Warning, TEXT("Unsupported source texture format %d"), (int32)SourceFormat);
                break;
            }
        }

            return SampledColor;
        }


FColor FTextureUtils::SampleTextureBilinear(const TArray<FColor>& SourcePixels, int32 SourceWidth, int32 SourceHeight, const FVector2D& UV)
{
   if (SourcePixels.Num() == 0 || SourceWidth <= 0 || SourceHeight <= 0) return FColor::Magenta;

    const float xf = UV.X * (SourceWidth - 1);
    const float yf = UV.Y * (SourceHeight - 1);
    const int32 x0 = FMath::Clamp(FMath::FloorToInt(xf), 0, SourceWidth - 1);
    const int32 y0 = FMath::Clamp(FMath::FloorToInt(yf), 0, SourceHeight - 1);
    const int32 x1 = FMath::Clamp(x0 + 1, 0, SourceWidth - 1);
    const int32 y1 = FMath::Clamp(y0 + 1, 0, SourceHeight - 1);

    const float wx = xf - FMath::FloorToFloat(xf);
    const float wy = yf - FMath::FloorToFloat(yf);
    const float w00 = (1 - wx) * (1 - wy);
    const float w10 = wx       * (1 - wy);
    const float w01 = (1 - wx) * wy;
    const float w11 = wx       * wy;

    auto L = [](const FColor& c){ return FLinearColor::FromSRGBColor(c); };

    const FLinearColor c00 = L(SourcePixels[y0 * SourceWidth + x0]);
    const FLinearColor c10 = L(SourcePixels[y0 * SourceWidth + x1]);
    const FLinearColor c01 = L(SourcePixels[y1 * SourceWidth + x0]);
    const FLinearColor c11 = L(SourcePixels[y1 * SourceWidth + x1]);

    // Premultiply RGB by A
    auto premul = [](const FLinearColor& c){ return FLinearColor(c.R * c.A, c.G * c.A, c.B * c.A, c.A); };
    const FLinearColor p00 = premul(c00);
    const FLinearColor p10 = premul(c10);
    const FLinearColor p01 = premul(c01);
    const FLinearColor p11 = premul(c11);

    // Interpolate premultiplied RGB and alpha
    const FLinearColor p =
        p00 * w00 + p10 * w10 + p01 * w01 + p11 * w11;

    const float a = p.A; // already interpolated alpha

    // Unpremultiply safely
    FLinearColor outLin;
    if (a > 0.0f)
    {
        outLin = FLinearColor(p.R / a, p.G / a, p.B / a, a);
    }
    else
    {
        outLin = FLinearColor(0, 0, 0, 0);
    }

    // Back to sRGB bytes
    return outLin.ToFColorSRGB();
}

// Overload for High Precision FLinearColor arrays
FLinearColor FTextureUtils::SampleTextureBilinear(const TArray<FLinearColor>& SourcePixels, int32 SourceWidth, int32 SourceHeight, const FVector2D& UV)
{
   if (SourcePixels.Num() == 0 || SourceWidth <= 0 || SourceHeight <= 0) return FLinearColor::Red;

    const float xf = UV.X * (SourceWidth - 1);
    const float yf = UV.Y * (SourceHeight - 1);
    const int32 x0 = FMath::Clamp(FMath::FloorToInt(xf), 0, SourceWidth - 1);
    const int32 y0 = FMath::Clamp(FMath::FloorToInt(yf), 0, SourceHeight - 1);
    const int32 x1 = FMath::Clamp(x0 + 1, 0, SourceWidth - 1);
    const int32 y1 = FMath::Clamp(y0 + 1, 0, SourceHeight - 1);

    const float wx = xf - FMath::FloorToFloat(xf);
    const float wy = yf - FMath::FloorToFloat(yf);
    const float w00 = (1 - wx) * (1 - wy);
    const float w10 = wx       * (1 - wy);
    const float w01 = (1 - wx) * wy;
    const float w11 = wx       * wy;

    // DIRECT SAMPLE (No sRGB conversion needed, they are already linear)
    const FLinearColor& c00 = SourcePixels[y0 * SourceWidth + x0];
    const FLinearColor& c10 = SourcePixels[y0 * SourceWidth + x1];
    const FLinearColor& c01 = SourcePixels[y1 * SourceWidth + x0];
    const FLinearColor& c11 = SourcePixels[y1 * SourceWidth + x1];

    // Premultiply RGB by A
    auto premul = [](const FLinearColor& c){ return FLinearColor(c.R * c.A, c.G * c.A, c.B * c.A, c.A); };
    const FLinearColor p00 = premul(c00);
    const FLinearColor p10 = premul(c10);
    const FLinearColor p01 = premul(c01);
    const FLinearColor p11 = premul(c11);

    // Interpolate
    const FLinearColor p = p00 * w00 + p10 * w10 + p01 * w01 + p11 * w11;

    const float a = p.A;

    // Unpremultiply safely
    if (a > 1e-6f)
    {
        return FLinearColor(p.R / a, p.G / a, p.B / a, a);
    }
    
    return FLinearColor(0, 0, 0, 0);
}

void FTextureUtils::FeatherProjectionEdges_Simple(
	TArray<FColor>& InOutLayerSrgb,
	int32 Width, int32 Height,
	const TArray<int32>* UVIslandIDs,
	const FSimpleFeatherParams& Params)
{
	if (Width <= 0 || Height <= 0 || InOutLayerSrgb.Num() != Width * Height || Params.RadiusTexels <= 0)
	{
		return;
	}

	const int32 Num = Width * Height;

	// Neighborhood offsets
	TArray<FIntPoint> Offs4 { { 1,0 },{-1,0 },{0, 1},{0,-1} };
	TArray<FIntPoint> Offs8 = Offs4;
	Offs8.Append({ { 1,1 },{ 1,-1 },{ -1,1 },{ -1,-1 } });
	const TArray<FIntPoint>& Nbh = Params.bUse8Connectivity ? Offs8 : Offs4;

	// Distances in texels from the nearest seed (blank & in-triangle); -1 = unvisited
	TArray<int16> Dist; Dist.Init(int16(-1), Num);

	// Snapshot alpha so attenuation uses the original value once.
	TArray<uint8> Alpha0; Alpha0.SetNumUninitialized(Num);
	for (int32 i = 0; i < Num; ++i) Alpha0[i] = InOutLayerSrgb[i].A;

	auto SameIsland = [&](int32 a, int32 b) -> bool
	{
		if (!UVIslandIDs) return true;
		const int32 IA = (*UVIslandIDs)[a];
		const int32 IB = (*UVIslandIDs)[b];
		// “in a triangle” means valid island id (>= 0). blank seeds must satisfy this.
		return (IA >= 0 && IB >= 0 && IA == IB);
	};

	auto Idx = [&](int32 x, int32 y) { return y * Width + x; };
	auto InBounds = [&](int32 x, int32 y) { return (uint32)x < (uint32)Width && (uint32)y < (uint32)Height; };

	// 1) Seed queue with *blank & in-triangle* texels (alpha==0, island>=0 if given).
	TQueue<int32> Q;
	for (int32 y = 0; y < Height; ++y)
	{
		for (int32 x = 0; x < Width; ++x)
		{
			const int32 i = Idx(x, y);
			const bool bBlank = (Alpha0[i] == 0);
			const bool bInTri = (!UVIslandIDs || (*UVIslandIDs)[i] >= 0);
			if (bBlank && bInTri)
			{
				Dist[i] = 0;
				Q.Enqueue(i);
			}
		}
	}

	// 2) Multi-source BFS outward into covered texels, respecting island boundaries.
	while (!Q.IsEmpty())
	{
		int32 i; Q.Dequeue(i);
		const int32 cx = i % Width;
		const int32 cy = i / Width;
		const int16 cd = Dist[i];

		// Do not explore past the target radius
		if (cd >= Params.RadiusTexels) continue;

		for (const FIntPoint& o : Nbh)
		{
			const int nx = cx + o.X;
			const int ny = cy + o.Y;
			if (!InBounds(nx, ny)) continue;

			const int32 j = Idx(nx, ny);
			if (Dist[j] != -1) continue;                 // already visited
			if (!SameIsland(i, j)) continue;             // must remain within the same UV island
			if (Alpha0[j] == 0)                           // blank neighbor is also a seed, but skip enqueue; it's already dist=0 by init
			{
				Dist[j] = 0;
				continue;
			}

			// Only propagate into covered texels
			if (Alpha0[j] > 0)
			{
				Dist[j] = cd + 1;
				Q.Enqueue(j);
			}
		}
	}

	// 3) Apply attenuation for texels within Radius (1..Radius). RGB unchanged.
	for (int32 i = 0; i < Num; ++i)
	{
		const int16 d = Dist[i];
		if (d <= 0 || d == -1) continue;               // 0 = seed (blank), -1 = far away
		if (d > Params.RadiusTexels) continue;

		const float t = FMath::Clamp(float(d) / float(Params.RadiusTexels), 0.f, 1.f); // 0..1
		const float atten = t;                             // linear falloff; keep it simple
		const float a0 = float(Alpha0[i]);
		const float a1 = a0 * (1.f - atten);
		const uint8 aNew = (uint8)FMath::Clamp(FMath::RoundToInt(a1), int32(Params.AlphaFloor), 255);

		InOutLayerSrgb[i].A = aNew;
	}
}



bool FTextureUtils::ProjectTextureOntoMesh_DebugCoordinates(
    UTexture2D* SourceTexture,
    const TArray<bool>& VisibilityBuffer,
    const TArray<FVector2D>& ScreenPositionBuffer,
    int32 TextureWidth,
    int32 TextureHeight,
    const FString& PackagePath,
    const FString& AssetName)
{
    const int32 NumTexels = TextureWidth * TextureHeight;
    
    if (VisibilityBuffer.Num() != NumTexels || ScreenPositionBuffer.Num() != NumTexels)
    {
        UE_LOG(LogTemp, Warning, TEXT("Debug Coordinates: Buffer size mismatch"));
        return false;
    }
    
    // Create a new texture
    UTexture2D* OutputTexture = UTexture2D::CreateTransient(TextureWidth, TextureHeight, PF_B8G8R8A8);
    if (!OutputTexture)
    {
        UE_LOG(LogTemp, Warning, TEXT("Debug Coordinates: Failed to create transient texture"));
        return false;
    }

    
    // Set texture properties
    OutputTexture->CompressionSettings = TC_VectorDisplacementmap;
    OutputTexture->MipGenSettings = TMGS_NoMipmaps;
    OutputTexture->SRGB = false;
    OutputTexture->Filter = TF_Default;
    OutputTexture->AddressX = TA_Clamp;
    OutputTexture->AddressY = TA_Clamp;
    
    // Lock the output texture for modification
    FTexture2DMipMap& Mip = OutputTexture->GetPlatformData()->Mips[0];
    FColor* MipData = static_cast<FColor*>(Mip.BulkData.Lock(LOCK_READ_WRITE));
    
    // Initialize texture data to black
    for (int32 i = 0; i < NumTexels; i++)
    {
        MipData[i] = FColor(0, 0, 0, 255);
    }
    
    int32 VisibleTexelsCount = 0;
    int32 InvalidCoordCount = 0;
    
    // For each texel, if visible, set its color based on screen position
    for (int32 i = 0; i < NumTexels; i++)
    {
        if (VisibilityBuffer[i])
        {

            //print out screen coords of every 100th texel
            // UE_LOG(LogTemp, Log, TEXT("Screen Position: %f, %f"), ScreenPositionBuffer[i].X, ScreenPositionBuffer[i].Y);
            // Get the screen position (normalized [0,1] coordinates)
            const FVector2D& ScreenPos = ScreenPositionBuffer[i];
            
            // Visualize the coordinates directly as colors
            // Red channel = X coordinate (0-1 maps to 0-255)
            // Green channel = Y coordinate (0-1 maps to 0-255)
            // Blue channel = 0
            // This makes it easy to see the distribution of coordinates
            
            // Check for invalid coordinates (outside 0-1 range)
            bool bInvalidCoord = (ScreenPos.X < 0.0f || ScreenPos.X > 1.0f || 
                                 ScreenPos.Y < 0.0f || ScreenPos.Y > 1.0f);
            
            if (bInvalidCoord)
            {
                // Mark invalid coordinates with a bright blue color
                MipData[i] = FColor(0, 0, 255, 255);
                InvalidCoordCount++;
            }
            else
            {
                // Map coordinates directly to RGB values
                uint8 R = FMath::RoundToInt(ScreenPos.X * 255.0f);
                uint8 G = FMath::RoundToInt(ScreenPos.Y * 255.0f);
                uint8 B = 0; // Use blue channel for additional visualization if needed
                
                MipData[i] = FColor(R, G, B, 255);
            }
            
            VisibleTexelsCount++;
        }
    }
    
    // Log stats about the coordinates
    UE_LOG(LogTemp, Log, TEXT("Coordinate Debug Stats:"));
    UE_LOG(LogTemp, Log, TEXT("  - Visible Texels: %d"), VisibleTexelsCount);
    UE_LOG(LogTemp, Log, TEXT("  - Invalid Coordinates: %d (%.1f%%)"), 
           InvalidCoordCount, (InvalidCoordCount * 100.0f) / FMath::Max(1, VisibleTexelsCount));
    
    // Unlock the texture
    Mip.BulkData.Unlock();
    
    // Update the texture
    OutputTexture->UpdateResource();
    
    // Save the texture as an asset
    bool bSaved = SaveTextureAsAsset(OutputTexture, PackagePath, AssetName);
    
    if (bSaved)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(
            FString::Printf(TEXT("Coordinate debug texture saved!\nVisible Texels: %d / %d\nInvalid Coords: %d\nSaved to: %s/%s"), 
                            VisibleTexelsCount, NumTexels, InvalidCoordCount, *PackagePath, *AssetName)));
    }
    else
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("Failed to save debug texture. Check output log for details."));
    }
    
    return bSaved;
}


bool FTextureUtils::ExtendTextureMargins(
    int32 TextureWidth,
    int32 TextureHeight,
    int32 Radius,
    FColor* TextureData,
    const TArray<int32>& UVIslandIDMap)
{
    if (Radius <= 0 || !TextureData || UVIslandIDMap.Num() != (TextureWidth * TextureHeight))
    {
        return false;
    }

    const int32 NumPx = TextureWidth * TextureHeight;
    TArray<FColor> ReadBuffer;
    ReadBuffer.SetNumUninitialized(NumPx);

    const int DX[] = { 1, -1, 0,  0, 1, -1,  1, -1 };
    const int DY[] = { 0,  0, 1, -1, 1, -1, -1,  1 };

    for (int pass = 0; pass < Radius; ++pass)
    {
        FMemory::Memcpy(ReadBuffer.GetData(), TextureData, NumPx * sizeof(FColor));
        
        int32 PixelsFilledThisPass = 0;

        for (int32 y = 0; y < TextureHeight; ++y)
        {
            for (int32 x = 0; x < TextureWidth; ++x)
            {
                const int32 idx = y * TextureWidth + x;

                // We only operate on fully transparent pixels from the start of the pass.
                if (ReadBuffer[idx].A == 0)
                {
                    // --- NEW LOGIC: Find the neighbor with the highest alpha ---
                    FColor BestNeighborColor(0, 0, 0, 0);
                    uint8 MaxAlpha = 0;
                    
                    for (int n = 0; n < 8; ++n)
                    {
                        const int nx = x + DX[n];
                        const int ny = y + DY[n];

                        if (nx >= 0 && nx < TextureWidth && ny >= 0 && ny < TextureHeight)
                        {
                            const int32 nidx = ny * TextureWidth + nx;
                            const FColor& NeighborColor = ReadBuffer[nidx];

                            // Check if this neighbor is stronger than any we've seen so far.
                            if (NeighborColor.A > MaxAlpha)
                            {
                                // Island check: Ensure we don't bleed across UV shells.
                                if (UVIslandIDMap[idx] <= 0 || (UVIslandIDMap[nidx] == UVIslandIDMap[idx]))
                                {
                                    MaxAlpha = NeighborColor.A;
                                    BestNeighborColor = NeighborColor;
                                }
                            }
                        }
                    }

                    // If we found a valid neighbor, copy its color and alpha.
                    if (MaxAlpha > 0)
                    {
                        TextureData[idx] = BestNeighborColor;
                        PixelsFilledThisPass++;
                    }
                }
            }
        }
        
        if (PixelsFilledThisPass == 0)
        {
            break; // Optimization: Stop if a pass fills no pixels.
        }
    }

    return true;
}
bool FTextureUtils::ExtendTextureMarginsLinear(
    int32 TextureWidth,
    int32 TextureHeight,
    int32 Radius,
    TArray<FLinearColor>& TextureData,
    const TArray<int32>& UVIslandIDMap)
{
    const int32 NumPx = TextureWidth * TextureHeight;
    if (Radius <= 0 || TextureData.Num() != NumPx || UVIslandIDMap.Num() != NumPx)
    {
        return false;
    }

    // Store original solid pixels and their colors
    TArray<FLinearColor> OriginalColors;
    OriginalColors.SetNumUninitialized(NumPx);
    FMemory::Memcpy(OriginalColors.GetData(), TextureData.GetData(), NumPx * sizeof(FLinearColor));

    // Track which pixels were originally solid (these are our source pixels)
    TArray<bool> WasOriginalSolid;
    WasOriginalSolid.SetNumUninitialized(NumPx);
    for (int32 i = 0; i < NumPx; ++i)
    {
        WasOriginalSolid[i] = (TextureData[i].A > KINDA_SMALL_NUMBER);
    }

    // For each empty pixel, find the nearest original solid pixel within radius
    for (int32 y = 0; y < TextureHeight; ++y)
    {
        for (int32 x = 0; x < TextureWidth; ++x)
        {
            const int32 idx = y * TextureWidth + x;

            // Skip pixels that were originally solid
            if (WasOriginalSolid[idx])
            {
                continue;
            }

            const int32 CurrentIslandID = UVIslandIDMap[idx];

            // Search for nearest solid pixel in expanding squares
            float BestDistSq = FLT_MAX;
            FLinearColor BestColor = FLinearColor::Transparent;
            bool bFoundSource = false;

            // Search within radius
            for (int32 r = 1; r <= Radius && !bFoundSource; ++r)
            {
                // Check all pixels at exactly distance r (square ring)
                for (int32 dy = -r; dy <= r; ++dy)
                {
                    for (int32 dx = -r; dx <= r; ++dx)
                    {
                        // Only check the perimeter of the square
                        if (FMath::Abs(dx) != r && FMath::Abs(dy) != r)
                        {
                            continue;
                        }

                        const int32 nx = x + dx;
                        const int32 ny = y + dy;

                        if (nx < 0 || nx >= TextureWidth || ny < 0 || ny >= TextureHeight)
                        {
                            continue;
                        }

                        const int32 nidx = ny * TextureWidth + nx;

                        // Only consider originally solid pixels as sources
                        if (!WasOriginalSolid[nidx])
                        {
                            continue;
                        }

                        const int32 NeighborIslandID = UVIslandIDMap[nidx];

                        // Island check
                        if (CurrentIslandID > 0 && NeighborIslandID != CurrentIslandID)
                        {
                            continue;
                        }

                        // Calculate actual Euclidean distance
                        const float DistSq = (float)(dx * dx + dy * dy);

                        if (DistSq < BestDistSq)
                        {
                            BestDistSq = DistSq;
                            BestColor = OriginalColors[nidx];
                            bFoundSource = true;
                        }
                    }
                }
            }

            // Fill with nearest source color
            if (bFoundSource)
            {
                TextureData[idx] = BestColor;
            }
        }
    }

    return true;
}


bool FTextureUtils::ExtendTextureMarginsNormal(
        int32 TextureWidth,
        int32 TextureHeight,
        int32 Radius,
        TArray<FLinearColor>& TextureData,
        const TArray<bool>& ValidPixelMask, // Passed as 'HasValidData' or 'VisibilityBuffer'
        const TArray<int32>& UVIslandIDMap)
{
    const int32 NumPx = TextureWidth * TextureHeight;
    
    // Basic validation
    if (Radius <= 0 || TextureData.Num() != NumPx || ValidPixelMask.Num() != NumPx)
    {
        return false;
    }

    // If UVIslandIDMap is missing or size mismatch, we can optionally run without island checks
    // but for this implementation, we require it as per your snippet.
    if (UVIslandIDMap.Num() != NumPx)
    {
        UE_LOG(LogTemp, Warning, TEXT("ExtendTextureMarginsNormal: UVIslandIDMap size mismatch. Aborting margin extension."));
        return false;
    }

    TArray<FLinearColor> ReadBuffer;
    TArray<bool> FilledBuffer; 
    
    ReadBuffer.SetNumUninitialized(NumPx);
    FilledBuffer = ValidPixelMask; // Start with the mask of what is currently valid

    // 8-way neighbors (N, S, E, W, NE, NW, SE, SW)
    const int DX[] = { 1, -1, 0,  0, 1, -1,  1, -1 };
    const int DY[] = { 0,  0, 1, -1, 1, -1, -1,  1 };

    // Helper to unpack [0..1] Color to [-1..1] Vector
    auto UnpackNormal = [](const FLinearColor& C) -> FVector {
        return FVector(
            C.R * 2.0f - 1.0f,
            C.G * 2.0f - 1.0f,
            C.B * 2.0f - 1.0f
        );
    };

    // Helper to pack [-1..1] Vector to [0..1] Color
    auto PackNormal = [](const FVector& N) -> FLinearColor {
        return FLinearColor(
            (N.X + 1.0f) * 0.5f,
            (N.Y + 1.0f) * 0.5f,
            (N.Z + 1.0f) * 0.5f,
            1.0f // Fully opaque
        );
    };

    for (int pass = 0; pass < Radius; ++pass)
    {
        // 1. Copy current texture state to read buffer for this pass
        FMemory::Memcpy(ReadBuffer.GetData(), TextureData.GetData(), NumPx * sizeof(FLinearColor));
        
        // 2. Snapshot the valid state so we don't bleed "just filled" pixels into "currently filling" pixels in the same pass
        TArray<bool> ReadFilledBuffer = FilledBuffer;
        
        int32 PixelsFilledThisPass = 0;

        for (int32 y = 0; y < TextureHeight; ++y)
        {
            for (int32 x = 0; x < TextureWidth; ++x)
            {
                const int32 idx = y * TextureWidth + x;

                // Skip pixels that already have data
                if (ReadFilledBuffer[idx])
                {
                    continue; 
                }

                // This pixel is empty. Check its neighbors.
                const int32 CurrentIslandID = UVIslandIDMap[idx];
                
                FVector SummedNormal(0.0f, 0.0f, 0.0f);
                int32 NeighborCount = 0;

                for (int n = 0; n < 8; ++n)
                {
                    const int nx = x + DX[n];
                    const int ny = y + DY[n];

                    // Boundary check
                    if (nx >= 0 && nx < TextureWidth && ny >= 0 && ny < TextureHeight)
                    {
                        const int32 nidx = ny * TextureWidth + nx;

                        // Check if neighbor has valid data (from previous state)
                        if (ReadFilledBuffer[nidx])
                        {
                            const int32 NeighborIslandID = UVIslandIDMap[nidx];
                            
                            // Island Logic: 
                            // 1. If CurrentIslandID is <= 0, it's gutter space, accept any neighbor.
                            // 2. If CurrentIslandID > 0, only accept neighbors from the SAME island.
                            if (CurrentIslandID <= 0 || (NeighborIslandID == CurrentIslandID))
                            {
                                SummedNormal += UnpackNormal(ReadBuffer[nidx]);
                                NeighborCount++;
                            }
                        }
                    }
                }
                
                // If we found any valid neighbors, average them and fill the pixel
                if (NeighborCount > 0)
                {
                    FVector AverageNormal = (SummedNormal / (float)NeighborCount).GetSafeNormal();
                    TextureData[idx] = PackNormal(AverageNormal);
                    FilledBuffer[idx] = true; // Mark as valid for the next pass
                    PixelsFilledThisPass++;
                }
            } // end for x
        } // end for y
        
        // Optimization: If a pass didn't fill anything, subsequent passes won't either.
        if (PixelsFilledThisPass == 0)
        {
            break; 
        }
    }
    
    return true;
}
bool FTextureUtils::ExtendGuttersFinal(
    int32 TextureWidth,
    int32 TextureHeight,
    int32 Radius,
    TArray<FLinearColor>& TextureData,
    const TArray<int32>& UVIslandIDMap)
{
    const int32 NumPx = TextureWidth * TextureHeight;
    if (Radius <= 0 || TextureData.Num() != NumPx || UVIslandIDMap.Num() != NumPx) return false;

    // Buffer to hold the state of the texture at the start of the pass
    TArray<FLinearColor> ReadBuffer;
    ReadBuffer.SetNumUninitialized(NumPx);

    // 8-way neighbors
    const int DX[] = { 1, -1, 0,  0, 1, -1,  1, -1 };
    const int DY[] = { 0,  0, 1, -1, 1, -1, -1,  1 };

    // THRESHOLD: Pixels below this alpha are considered "Empty/Jagged" and will be overwritten.
    // 0.05 is usually enough to eat the faint CPU artifacts.
    const float AlphaThreshold = 1.0f;

    for (int pass = 0; pass < Radius; ++pass)
    {
        // 1. Snapshot: Copy current state to read buffer
        FMemory::Memcpy(ReadBuffer.GetData(), TextureData.GetData(), NumPx * sizeof(FLinearColor));
        
        int32 PixelsFilledThisPass = 0;

        for (int32 y = 0; y < TextureHeight; ++y)
        {
            for (int32 x = 0; x < TextureWidth; ++x)
            {
                const int32 idx = y * TextureWidth + x;

                // 2. CHECK: Is this pixel "Solid Enough" to keep?
                // If Alpha is > 0.05, we leave it alone.
                if (!(ReadBuffer[idx].A < AlphaThreshold))
                {
                    continue; 
                }

                const int32 CurrentIslandID = UVIslandIDMap[idx];
                
                // Variables to find the strongest neighbor
                FLinearColor BestNeighbor = FLinearColor::Transparent;
                float MaxNeighborAlpha = -1.0f;

                for (int n = 0; n < 8; ++n)
                {
                    const int nx = x + DX[n];
                    const int ny = y + DY[n];

                    if (nx >= 0 && nx < TextureWidth && ny >= 0 && ny < TextureHeight)
                    {
                        const int32 nidx = ny * TextureWidth + nx;
                        const FLinearColor& NeighborColor = ReadBuffer[nidx];

                        // 3. NEIGHBOR CHECK: Is the neighbor "Solid"?
                        if (NeighborColor.A < AlphaThreshold)
                        {
                            const int32 NeighborIslandID = UVIslandIDMap[nidx];

                            // 4. ISLAND CHECK: 
                            // Only borrow if we are in empty space (ID <= -1) OR on the same island
                            if (CurrentIslandID < 0 || (NeighborIslandID == CurrentIslandID))
                            {
                                // 5. SELECTION: Pick the neighbor with the HIGHEST alpha
                                if (NeighborColor.A > MaxNeighborAlpha)
                                {
                                    MaxNeighborAlpha = NeighborColor.A;
                                    BestNeighbor = NeighborColor;
                                }
                            }
                        }
                    }
                }

                // 6. FILL: If we found a valid, solid neighbor, copy it exactly.
                if (MaxNeighborAlpha > -1.0f)
                {
                    TextureData[idx] = BestNeighbor;
                    PixelsFilledThisPass++;
                }
            } 
        } 

        if (PixelsFilledThisPass == 0) break;
    }
    
    return true;
}


void FTextureUtils::ExtendGuttersFinalNormal(
    TArray<FLinearColor>& NormalData,
    int32 Width,
    int32 Height,
    int32 GutterRadius,
    bool bDebugSave,
    const FString& DebugBaseName)
{
    const int32 NumPx = Width * Height;
    
    if (GutterRadius <= 0 || NormalData.Num() != NumPx)
    {
        UE_LOG(LogTemp, Warning, TEXT("ExtendGuttersFinalNormal: Invalid parameters. Radius=%d, PixelCount=%d, Expected=%d"), 
               GutterRadius, NormalData.Num(), NumPx);
        return;
    }
    
    UE_LOG(LogTemp, Log, TEXT("ExtendGuttersFinalNormal: Starting. Size=%dx%d, Radius=%d"), 
           Width, Height, GutterRadius);
    
    // --- DEBUG: Save input state ---
    if (bDebugSave && !DebugBaseName.IsEmpty())
    {
        const FString BasePath = TEXT("/Game/TD3D_Debug/");
        const FString Timestamp = FDateTime::Now().ToString(TEXT("HHMMSS"));
        
        // Save normal map input
        {
            const FString AssetName = FString::Printf(TEXT("%s_GutterNormal_0_Input_%s"), *DebugBaseName, *Timestamp);
            const FString PackagePath = BasePath + AssetName;
            
            UPackage* Package = CreatePackage(*PackagePath);
            if (Package)
            {
                Package->FullyLoad();
                UTexture2D* NewTexture = NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone);
                if (NewTexture)
                {
                    NewTexture->AddToRoot();
                    NewTexture->CompressionSettings = TC_HDR_F32;
                    NewTexture->SRGB = false;
                    NewTexture->Source.Init(Width, Height, 1, 1, TSF_RGBA16F);
                    
                    TArray<FFloat16Color> HdrPixels;
                    HdrPixels.SetNumUninitialized(NumPx);
                    for (int32 i = 0; i < NumPx; ++i)
                    {
                        HdrPixels[i] = FFloat16Color(NormalData[i]);
                    }
                    
                    void* DestPixels = NewTexture->Source.LockMip(0);
                    if (DestPixels)
                    {
                        FMemory::Memcpy(DestPixels, HdrPixels.GetData(), HdrPixels.Num() * sizeof(FFloat16Color));
                        NewTexture->Source.UnlockMip(0);
                        NewTexture->UpdateResource();
                        Package->MarkPackageDirty();
                        FAssetRegistryModule::AssetCreated(NewTexture);
                        
                        const FString PackageFileName = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
                        FSavePackageArgs SaveArgs;
                        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
                        UPackage::SavePackage(Package, NewTexture, *PackageFileName, SaveArgs);
                        UE_LOG(LogTemp, Warning, TEXT("ExtendGuttersFinalNormal: Saved INPUT to %s"), *PackagePath);
                    }
                    NewTexture->RemoveFromRoot();
                }
            }
        }
        
        // Save alpha channel as grayscale
        {
            const FString AssetName = FString::Printf(TEXT("%s_GutterNormal_0_Input_ALPHA_%s"), *DebugBaseName, *Timestamp);
            const FString PackagePath = BasePath + AssetName;
            
            UPackage* Package = CreatePackage(*PackagePath);
            if (Package)
            {
                Package->FullyLoad();
                UTexture2D* NewTexture = NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone);
                if (NewTexture)
                {
                    NewTexture->AddToRoot();
                    NewTexture->CompressionSettings = TC_HDR_F32;
                    NewTexture->SRGB = false;
                    NewTexture->Source.Init(Width, Height, 1, 1, TSF_RGBA16F);
                    
                    TArray<FFloat16Color> HdrPixels;
                    HdrPixels.SetNumUninitialized(NumPx);
                    for (int32 i = 0; i < NumPx; ++i)
                    {
                        float A = NormalData[i].A;
                        HdrPixels[i] = FFloat16Color(FLinearColor(A, A, A, 1.0f));
                    }
                    
                    void* DestPixels = NewTexture->Source.LockMip(0);
                    if (DestPixels)
                    {
                        FMemory::Memcpy(DestPixels, HdrPixels.GetData(), HdrPixels.Num() * sizeof(FFloat16Color));
                        NewTexture->Source.UnlockMip(0);
                        NewTexture->UpdateResource();
                        Package->MarkPackageDirty();
                        FAssetRegistryModule::AssetCreated(NewTexture);
                        
                        const FString PackageFileName = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
                        FSavePackageArgs SaveArgs;
                        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
                        UPackage::SavePackage(Package, NewTexture, *PackageFileName, SaveArgs);
                        UE_LOG(LogTemp, Warning, TEXT("ExtendGuttersFinalNormal: Saved INPUT ALPHA to %s"), *PackagePath);
                    }
                    NewTexture->RemoveFromRoot();
                }
            }
        }
    }
    
    // Helper: Unpack [0,1] color to [-1,1] vector
    auto UnpackNormal = [](const FLinearColor& C) -> FVector
    {
        return FVector(
            C.R * 2.0f - 1.0f,
            C.G * 2.0f - 1.0f,
            C.B * 2.0f - 1.0f
        );
    };
    
    // Helper: Pack [-1,1] vector to [0,1] color
    auto PackNormalRGB = [](const FVector& N) -> FVector
    {
        const FVector Safe = N.GetSafeNormal();
        return FVector(
            Safe.X * 0.5f + 0.5f,
            Safe.Y * 0.5f + 0.5f,
            Safe.Z * 0.5f + 0.5f
        );
    };
    
    // Build initial "filled" mask
    TArray<bool> FilledMask;
    FilledMask.SetNumUninitialized(NumPx);
    int32 InitialFilledCount = 0;
    
    for (int32 i = 0; i < NumPx; ++i)
    {
        FilledMask[i] = (NormalData[i].A > 0.0f);
        if (FilledMask[i]) InitialFilledCount++;
    }
    
    UE_LOG(LogTemp, Log, TEXT("ExtendGuttersFinalNormal: %d filled pixels (%.1f%%), %d empty pixels to potentially fill"), 
           InitialFilledCount, 
           (float)InitialFilledCount * 100.0f / (float)NumPx,
           NumPx - InitialFilledCount);
    
    // 8-connected neighbor offsets
    const int32 DX[] = { 1, -1, 0, 0, 1, -1, 1, -1 };
    const int32 DY[] = { 0, 0, 1, -1, 1, -1, -1, 1 };
    
    int32 TotalPixelsFilled = 0;
    
    for (int32 Pass = 0; Pass < GutterRadius; ++Pass)
    {
        TArray<FLinearColor> ReadBuffer = NormalData;
        TArray<bool> ReadFilledMask = FilledMask;
        
        int32 PixelsFilledThisPass = 0;
        
        for (int32 Y = 0; Y < Height; ++Y)
        {
            for (int32 X = 0; X < Width; ++X)
            {
                const int32 Idx = Y * Width + X;
                
                if (ReadFilledMask[Idx])
                {
                    continue;
                }
                
                FVector SumNormal = FVector::ZeroVector;
                float SumAlpha = 0.0f;
                int32 NeighborCount = 0;
                
                for (int32 N = 0; N < 8; ++N)
                {
                    const int32 NX = X + DX[N];
                    const int32 NY = Y + DY[N];
                    
                    if (NX >= 0 && NX < Width && NY >= 0 && NY < Height)
                    {
                        const int32 NIdx = NY * Width + NX;
                        
                        if (ReadFilledMask[NIdx])
                        {
                            const FLinearColor& NeighborColor = ReadBuffer[NIdx];
                            SumNormal += UnpackNormal(NeighborColor);
                            SumAlpha += NeighborColor.A;
                            NeighborCount++;
                        }
                    }
                }
                
                if (NeighborCount > 0)
                {
                    const float InvCount = 1.0f / static_cast<float>(NeighborCount);
                    
                    FVector AverageNormal = SumNormal * InvCount;
                    AverageNormal = AverageNormal.GetSafeNormal();
                    
                    if (AverageNormal.IsNearlyZero())
                    {
                        AverageNormal = FVector(0.0f, 0.0f, 1.0f);
                    }
                    
                    const FVector PackedRGB = PackNormalRGB(AverageNormal);
                    
                    NormalData[Idx].R = PackedRGB.X;
                    NormalData[Idx].G = PackedRGB.Y;
                    NormalData[Idx].B = PackedRGB.Z;
                    NormalData[Idx].A = SumAlpha * InvCount;
                    
                    FilledMask[Idx] = true;
                    PixelsFilledThisPass++;
                }
            }
        }
        
        TotalPixelsFilled += PixelsFilledThisPass;
        
        UE_LOG(LogTemp, Log, TEXT("ExtendGuttersFinalNormal: Pass %d filled %d gutter pixels"), 
               Pass + 1, PixelsFilledThisPass);
        
        if (PixelsFilledThisPass == 0)
        {
            break;
        }
    }
    
    // --- DEBUG: Save output state ---
    if (bDebugSave && !DebugBaseName.IsEmpty())
    {
        const FString BasePath = TEXT("/Game/TD3D_Debug/");
        const FString Timestamp = FDateTime::Now().ToString(TEXT("HHMMSS"));
        
        // Save normal map output
        {
            const FString AssetName = FString::Printf(TEXT("%s_GutterNormal_1_Output_%s"), *DebugBaseName, *Timestamp);
            const FString PackagePath = BasePath + AssetName;
            
            UPackage* Package = CreatePackage(*PackagePath);
            if (Package)
            {
                Package->FullyLoad();
                UTexture2D* NewTexture = NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone);
                if (NewTexture)
                {
                    NewTexture->AddToRoot();
                    NewTexture->CompressionSettings = TC_HDR_F32;
                    NewTexture->SRGB = false;
                    NewTexture->Source.Init(Width, Height, 1, 1, TSF_RGBA16F);
                    
                    TArray<FFloat16Color> HdrPixels;
                    HdrPixels.SetNumUninitialized(NumPx);
                    for (int32 i = 0; i < NumPx; ++i)
                    {
                        HdrPixels[i] = FFloat16Color(NormalData[i]);
                    }
                    
                    void* DestPixels = NewTexture->Source.LockMip(0);
                    if (DestPixels)
                    {
                        FMemory::Memcpy(DestPixels, HdrPixels.GetData(), HdrPixels.Num() * sizeof(FFloat16Color));
                        NewTexture->Source.UnlockMip(0);
                        NewTexture->UpdateResource();
                        Package->MarkPackageDirty();
                        FAssetRegistryModule::AssetCreated(NewTexture);
                        
                        const FString PackageFileName = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
                        FSavePackageArgs SaveArgs;
                        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
                        UPackage::SavePackage(Package, NewTexture, *PackageFileName, SaveArgs);
                        UE_LOG(LogTemp, Warning, TEXT("ExtendGuttersFinalNormal: Saved OUTPUT to %s"), *PackagePath);
                    }
                    NewTexture->RemoveFromRoot();
                }
            }
        }
        
        // Save alpha channel as grayscale
        {
            const FString AssetName = FString::Printf(TEXT("%s_GutterNormal_1_Output_ALPHA_%s"), *DebugBaseName, *Timestamp);
            const FString PackagePath = BasePath + AssetName;
            
            UPackage* Package = CreatePackage(*PackagePath);
            if (Package)
            {
                Package->FullyLoad();
                UTexture2D* NewTexture = NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone);
                if (NewTexture)
                {
                    NewTexture->AddToRoot();
                    NewTexture->CompressionSettings = TC_HDR_F32;
                    NewTexture->SRGB = false;
                    NewTexture->Source.Init(Width, Height, 1, 1, TSF_RGBA16F);
                    
                    TArray<FFloat16Color> HdrPixels;
                    HdrPixels.SetNumUninitialized(NumPx);
                    for (int32 i = 0; i < NumPx; ++i)
                    {
                        float A = NormalData[i].A;
                        HdrPixels[i] = FFloat16Color(FLinearColor(A, A, A, 1.0f));
                    }
                    
                    void* DestPixels = NewTexture->Source.LockMip(0);
                    if (DestPixels)
                    {
                        FMemory::Memcpy(DestPixels, HdrPixels.GetData(), HdrPixels.Num() * sizeof(FFloat16Color));
                        NewTexture->Source.UnlockMip(0);
                        NewTexture->UpdateResource();
                        Package->MarkPackageDirty();
                        FAssetRegistryModule::AssetCreated(NewTexture);
                        
                        const FString PackageFileName = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
                        FSavePackageArgs SaveArgs;
                        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
                        UPackage::SavePackage(Package, NewTexture, *PackageFileName, SaveArgs);
                        UE_LOG(LogTemp, Warning, TEXT("ExtendGuttersFinalNormal: Saved OUTPUT ALPHA to %s"), *PackagePath);
                    }
                    NewTexture->RemoveFromRoot();
                }
            }
        }
    }
    
    UE_LOG(LogTemp, Log, TEXT("ExtendGuttersFinalNormal: Done. Total gutter pixels: %d"), TotalPixelsFilled);
}

bool FTextureUtils::SaveColorBufferAsTexture(
    const TArray<FColor>& ColorBuffer,
    int32 TextureWidth,
    int32 TextureHeight,
    const FString& PackagePath,
    const FString& AssetName)
{
    if (ColorBuffer.Num() != TextureWidth * TextureHeight)
    {
        UE_LOG(LogTemp, Warning, TEXT("SaveColorBufferAsTexture: Buffer size mismatch"));
        return false;
    }
    
    // Create a texture from the buffer
    UTexture2D* OutputTexture = CreateTextureFromPixelData(TextureWidth, TextureHeight, ColorBuffer);
    if (!OutputTexture)
    {
        UE_LOG(LogTemp, Warning, TEXT("SaveColorBufferAsTexture: Failed to create texture"));
        return false;
    }
    // Set the texture to use sRGB color space for proper display
    OutputTexture->SRGB = false; // Add this line
    OutputTexture->UpdateResource(); // Make sure to update after changing properties
    
    // Save the texture as an asset
    return SaveTextureAsAsset(OutputTexture, PackagePath, AssetName);
}

/**
 * Creates a transient 8-bit sRGB texture from an 8-bit sRGB pixel array.
 * This is the sRGB-correct version of CreateTextureFromPixelData.
 */
UTexture2D* FTextureUtils::CreateTextureFromsRGBPixelData(
    int32 Width, 
    int32 Height, 
    const TArray<FColor>& sRGB_PixelData)
{
    if (Width <= 0 || Height <= 0 || sRGB_PixelData.Num() != Width * Height)
    {
        UE_LOG(LogTemp, Warning, TEXT("CreateTextureFrom_sRGB_PixelData: Invalid input data"));
        return nullptr;
    }

    UTexture2D* NewTexture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
    if (!NewTexture)
    {
        UE_LOG(LogTemp, Warning, TEXT("CreateTextureFrom_sRGB_PixelData: Failed to create transient texture"));
        return nullptr;
    }

    // --- THIS IS THE CRITICAL PART ---
    // This data is 8-bit sRGB, so flag it as sRGB.
    NewTexture->CompressionSettings = TC_Default; 
    NewTexture->SRGB = true; // CORRECT: Tell the engine the source data is sRGB.
    NewTexture->Filter = TF_Bilinear;
    NewTexture->AddressX = TA_Clamp;
    NewTexture->AddressY = TA_Clamp;
    
    // --- Write Data ---
    FTexture2DMipMap& Mip = NewTexture->GetPlatformData()->Mips[0];
    void* TextureData = Mip.BulkData.Lock(LOCK_READ_WRITE);
    if (TextureData)
    {
        FMemory::Memcpy(TextureData, sRGB_PixelData.GetData(), sRGB_PixelData.Num() * sizeof(FColor));
        Mip.BulkData.Unlock();
    }
    
    NewTexture->UpdateResource();
    
    return NewTexture;
}


// Implementation in TextureUtils.cpp
// UTexture2D* FTextureUtils::CaptureNormalizedDepthTexture(
//     ASceneCapture2D* CaptureComponent,
//     int32 TextureWidth,
//     int32 TextureHeight,
//     float MinDepth,
//     float MaxDepth,
//     const FString& OutputPath,
//     const FString& OutputName,
//     bool bInvertDepth,
//     bool bSaveAsset)
// {
//     if (!CaptureComponent)
//     {
//         UE_LOG(LogTemp, Error, TEXT("CaptureNormalizedDepthTexture: No valid scene capture component provided."));
//         return nullptr;
//     }

//     UWorld* World = CaptureComponent->GetWorld();
//     if (!World)
//     {
//         UE_LOG(LogTemp, Error, TEXT("CaptureNormalizedDepthTexture: Invalid world context."));
//         return nullptr;
//     }

//     // Step 1: Create a render target specifically for depth
//     UTextureRenderTarget2D* DepthRT = NewObject<UTextureRenderTarget2D>();
//     // Use a floating-point format for accurate depth values
//     DepthRT->InitCustomFormat(TextureWidth, TextureHeight, PF_FloatRGBA, false);
//     DepthRT->UpdateResource();
    
//     // Step 2: Store original capture settings to restore later
//     USceneCaptureComponent2D* CaptureComp = CaptureComponent->GetCaptureComponent2D();
//     ESceneCaptureSource OriginalCaptureSource = CaptureComp->CaptureSource;
//     UTextureRenderTarget2D* OriginalTarget = CaptureComp->TextureTarget;
//     TArray<FEngineShowFlags> OriginalShowFlags;
    
//     // Store original show flags
//     OriginalShowFlags.Add(CaptureComp->ShowFlags);
    
//     // Step 3: Configure the capture for depth
//     CaptureComp->TextureTarget = DepthRT;
//     CaptureComp->CaptureSource = ESceneCaptureSource::SCS_SceneDepth;
    
//     // Set show flags for depth capture (disable post-processing, etc.)
//     CaptureComp->ShowFlags.SetPostProcessing(false);
//     CaptureComp->ShowFlags.SetMotionBlur(false);
//     CaptureComp->ShowFlags.SetLightShafts(false);
//     CaptureComp->ShowFlags.SetLensFlares(false);
//     CaptureComp->ShowFlags.SetBloom(false);
    
//     // Step 4: Capture the scene
//     CaptureComp->CaptureScene();
    
//     // Step 5: Read back the depth data
//     TArray<FFloat16Color> DepthData;
//     FTextureRenderTargetResource* RTResource = DepthRT->GameThread_GetRenderTargetResource();
//     RTResource->ReadFloat16Pixels(DepthData);
    
//     // Step 6: Normalize the depth values to 0-1 range
//     TArray<FColor> NormalizedDepthBuffer;
//     NormalizedDepthBuffer.SetNum(TextureWidth * TextureHeight);
    
//     // Find actual min/max values in the depth buffer for auto-normalization
//     float ActualMinDepth = MAX_FLT;
//     float ActualMaxDepth = 0.0f;
    
//     // First pass to find min/max
//     for (int32 i = 0; i < DepthData.Num(); i++)
//     {
//         float Depth = DepthData[i].R; // Depth stored in R channel
        
//         // Skip infinity values or invalid values
//         if (Depth < MAX_FLT && Depth > 0.0f)
//         {
//             ActualMinDepth = FMath::Min(ActualMinDepth, Depth);
//             ActualMaxDepth = FMath::Max(ActualMaxDepth, Depth);
//         }
//     }
    
//     // Use detected values if they're valid
//     if (ActualMinDepth < ActualMaxDepth && ActualMinDepth < MAX_FLT)
//     {
//         MinDepth = ActualMinDepth;
//         MaxDepth = ActualMaxDepth;
        
//         UE_LOG(LogTemp, Log, TEXT("Auto-detected depth range: %.2f to %.2f"), MinDepth, MaxDepth);
//     }
    
//     float DepthRange = MaxDepth - MinDepth;
//     if (DepthRange <= 0.0f)
//     {
//         UE_LOG(LogTemp, Warning, TEXT("CaptureNormalizedDepthTexture: Invalid depth range."));
//         DepthRange = 1.0f; // Prevent division by zero
//     }
    
//     // Second pass to normalize
//     for (int32 i = 0; i < DepthData.Num(); i++)
//     {
//         float Depth = DepthData[i].R; // Depth stored in R channel
        
//         // Normalize the depth value
//         float NormalizedDepth = 0.0f;
//         if (Depth > 0.0f && Depth < MAX_FLT) // Avoid infinity values
//         {
//             NormalizedDepth = FMath::Clamp((Depth - MinDepth) / DepthRange, 0.0f, 1.0f);
            
//             // Invert if requested (near = white, far = black)
//             if (bInvertDepth)
//             {
//                 NormalizedDepth = 1.0f - NormalizedDepth;
//             }
//         }
        
//         // Convert to grayscale color
//         uint8 GrayValue = FMath::RoundToInt(NormalizedDepth * 255.0f);
//         NormalizedDepthBuffer[i] = FColor(GrayValue, GrayValue, GrayValue, 255);
//     }
    
//     // Step 7: Restore original capture settings
//     CaptureComp->TextureTarget = OriginalTarget;
//     CaptureComp->CaptureSource = OriginalCaptureSource;
//     CaptureComp->ShowFlags = OriginalShowFlags[0];
    
//     // Step 8: Create the texture
//     UTexture2D* ResultTexture = nullptr;
    
//     // Create a package for the texture if we're saving it as an asset
//     if (bSaveAsset)
//     {
//         // Create the texture package
//         FString PackageName = OutputPath / OutputName;
//         UPackage* Package = CreatePackage(*PackageName);
//         Package->FullyLoad();
        
//         // Create the texture
//         ResultTexture = NewObject<UTexture2D>(Package, *OutputName, RF_Public | RF_Standalone | RF_MarkAsRootSet);
//     }
//     else
//     {
//         // Create a transient texture
//         ResultTexture = UTexture2D::CreateTransient(TextureWidth, TextureHeight, PF_B8G8R8A8);
//     }
    
//     if (!ResultTexture)
//     {
//         UE_LOG(LogTemp, Error, TEXT("Failed to create depth texture."));
//         return nullptr;
//     }
    
//     // Configure the texture
//     ResultTexture->PlatformData = new FTexturePlatformData();
//     ResultTexture->PlatformData->SizeX = TextureWidth;
//     ResultTexture->PlatformData->SizeY = TextureHeight;
//     ResultTexture->PlatformData->PixelFormat = EPixelFormat::PF_B8G8R8A8;
    
//     // Create the texture data
//     FTexture2DMipMap* Mip = new FTexture2DMipMap();
//     ResultTexture->PlatformData->Mips.Add(Mip);
//     Mip->SizeX = TextureWidth;
//     Mip->SizeY = TextureHeight;
    
//     // Allocate memory for pixel data
//     Mip->BulkData.Lock(LOCK_READ_WRITE);
//     uint8* TextureData = (uint8*)Mip->BulkData.Realloc(TextureWidth * TextureHeight * 4);
    
//     // Copy pixel data to the texture
//     for (int32 i = 0; i < NormalizedDepthBuffer.Num(); i++)
//     {
//         const FColor& PixelColor = NormalizedDepthBuffer[i];
//         int32 ByteIndex = i * 4;
        
//         // BGRA order (platform dependent)
//         TextureData[ByteIndex + 0] = PixelColor.B;
//         TextureData[ByteIndex + 1] = PixelColor.G;
//         TextureData[ByteIndex + 2] = PixelColor.R;
//         TextureData[ByteIndex + 3] = PixelColor.A;
//     }
    
//     // Unlock and update texture
//     Mip->BulkData.Unlock();
    
//     // Configure texture properties
//     ResultTexture->CompressionSettings = TextureCompressionSettings::TC_Default;
//     ResultTexture->SRGB = false; // Linear color space for depth data
//     ResultTexture->AddressX = TextureAddress::TA_Clamp;
//     ResultTexture->AddressY = TextureAddress::TA_Clamp;
//     ResultTexture->LODGroup = TextureGroup::TEXTUREGROUP_World;
    
//     // Update resource
//     ResultTexture->UpdateResource();
    
//     // Save the asset if requested
//     if (bSaveAsset)
//     {
//         // Mark the package as dirty
//         ResultTexture->MarkPackageDirty();
//         FAssetRegistryModule::AssetCreated(ResultTexture);
        
//         // Save the package
//         TArray<UPackage*> PackagesToSave;
//         PackagesToSave.Add(ResultTexture->GetOutermost());
//         UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false);
        
//         UE_LOG(LogTemp, Log, TEXT("Saved normalized depth texture: %s"), *ResultTexture->GetName());
//     }
//     else
//     {
//         UE_LOG(LogTemp, Log, TEXT("Created transient normalized depth texture"));
//     }
    
//     return ResultTexture;
// }

UTexture2D* FTextureUtils::ConvertDepthBufferToTexture(
    const TArray<float>& DepthBuffer,
    int32 TextureWidth,
    int32 TextureHeight,
    const FString& OutputPath,
    const FString& OutputName,
    bool bInvertDepth)
{
    if (DepthBuffer.Num() != TextureWidth * TextureHeight)
    {
        UE_LOG(LogTemp, Error, TEXT("ConvertDepthBufferToTexture: Buffer size mismatch"));
        return nullptr;
    }

    // Find min/max for normalization, but only consider valid mesh values
    float MinDepth = MAX_FLT;
    float MaxDepth = 0.0f;
    
    for (int32 i = 0; i < DepthBuffer.Num(); i++)
    {
        // Only consider valid depth values for normalization range
        if (DepthBuffer[i] < (65504.0f - 1.0f) && DepthBuffer[i] > 0.0f)
        {
            MinDepth = FMath::Min(MinDepth, DepthBuffer[i]);
            MaxDepth = FMath::Max(MaxDepth, DepthBuffer[i]);
        }
    }

    
    float DepthRange = MaxDepth - MinDepth;
    if (DepthRange <= 0.0f)
    {
        DepthRange = 1.0f; // Prevent division by zero
    }

    //print depth range
    UE_LOG(LogTemp, Log, TEXT("Depth range: %.2f to %.2f"), MinDepth, MaxDepth);

    
    // Convert the depth buffer to a color buffer for saving
    TArray<FColor> DepthColorBuffer;
    DepthColorBuffer.SetNum(TextureWidth * TextureHeight);
    
    int32 ValidPixelCount = 0;
    
    for (int32 i = 0; i < DepthBuffer.Num(); i++) 
    {
        // Initialize to black (background)
        DepthColorBuffer[i] = FColor::Black;
        
        // Only process valid mesh points
        if (DepthBuffer[i] < (65504.0f - 1.0f) && DepthBuffer[i] > 0.0f)
        {
            float NormalizedDepth = FMath::Clamp((DepthBuffer[i] - MinDepth) / DepthRange, 0.0f, 1.0f);
            
            // Invert if requested (near = white, far = black within the mesh)
            if (bInvertDepth)
            {
                NormalizedDepth = 1.0f - NormalizedDepth;
            }
            
            uint8 GrayValue = FMath::RoundToInt(NormalizedDepth * 255.0f);
            DepthColorBuffer[i] = FColor(GrayValue, GrayValue, GrayValue, 255);
            ValidPixelCount++;
        }
    }
    
    UE_LOG(LogTemp, Log, TEXT("Valid mesh pixels: %d out of %d (%.1f%%)"), 
           ValidPixelCount, DepthBuffer.Num(), 
           (ValidPixelCount * 100.0f) / DepthBuffer.Num());
    
    // Create a new texture using the UE API
    UTexture2D* ResultTexture = CreateTextureFromPixelData(TextureWidth, TextureHeight, DepthColorBuffer);
    
    if (!ResultTexture)
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to create depth texture."));
        return nullptr;
    }
    
    // Configure texture properties
    ResultTexture->CompressionSettings = TextureCompressionSettings::TC_Default;
    ResultTexture->SRGB = false; // Linear color space for depth data
    ResultTexture->AddressX = TextureAddress::TA_Clamp;
    ResultTexture->AddressY = TextureAddress::TA_Clamp;
    ResultTexture->LODGroup = TextureGroup::TEXTUREGROUP_World;
    
    // Save the texture as an asset
    SaveTextureAsAsset(ResultTexture, OutputPath, OutputName);
    
    UE_LOG(LogTemp, Log, TEXT("Saved normalized depth texture: %s"), *ResultTexture->GetName());
    
    return ResultTexture;
}

UTexture2D* FTextureUtils::ConvertWeightBuffersToTexture(
    const TArray<TArray<float>>& NormalWeightBuffers,
    int32 TextureWidth, 
    int32 TextureHeight,
    const FString& OutputPath,
    const FString& OutputName,
    bool bSaveAsset = true)
{
    if (NormalWeightBuffers.Num() == 0 || TextureWidth <= 0 || TextureHeight <= 0)
    {
        UE_LOG(LogTemp, Error, TEXT("ConvertWeightBuffersToTexture: Invalid input data"));
        return nullptr;
    }

    const int32 NumTexels = TextureWidth * TextureHeight;
    
    // Create a color buffer for the visibility mask
    TArray<FColor> VisibilityColorBuffer;
    VisibilityColorBuffer.SetNumZeroed(NumTexels);
    
    // Fill in visibility buffer based on weights
    int32 VisibleTexelsCount = 0;
    
    for (int32 TexelIndex = 0; TexelIndex < NumTexels; TexelIndex++)
    {
        // Combine weights from all cameras
        float CombinedWeight = 0.0f;
        
        for (int32 CameraIndex = 0; CameraIndex < NormalWeightBuffers.Num(); CameraIndex++)
        {
            if (NormalWeightBuffers[CameraIndex].IsValidIndex(TexelIndex))
            {
                float Weight = NormalWeightBuffers[CameraIndex][TexelIndex];
                // Add to the combined weight (could also use max or another method)
                CombinedWeight += Weight;
            }
        }
        
        // Clamp the combined weight to [0,1]
        CombinedWeight = FMath::Clamp(CombinedWeight, 0.0f, 1.0f);
        
        // Convert weight to color intensity
        uint8 Intensity = FMath::RoundToInt(CombinedWeight * 255.0f);
        
        // Set the color based on weight
        if (Intensity > 0)
        {
            VisibilityColorBuffer[TexelIndex] = FColor(Intensity, Intensity, Intensity, 255);
            VisibleTexelsCount++;
        }
        else
        {
            VisibilityColorBuffer[TexelIndex] = FColor(0, 0, 0, 255);
        }
    }
    
    // Create a texture from this buffer
    UTexture2D* VisibilityTexture = CreateTextureFromPixelData(TextureWidth, TextureHeight, VisibilityColorBuffer);
    
    if (!VisibilityTexture)
    {
        UE_LOG(LogTemp, Error, TEXT("ConvertWeightBuffersToTexture: Failed to create texture"));
        return nullptr;
    }
    
    // Set appropriate texture properties for a mask
    VisibilityTexture->CompressionSettings = TC_Default;
    VisibilityTexture->MipGenSettings = TMGS_NoMipmaps;
    VisibilityTexture->SRGB = false;
    VisibilityTexture->UpdateResource();
    
    // Save the texture if requested
    if (bSaveAsset)
    {
        bool bSaved = SaveTextureAsAsset(VisibilityTexture, OutputPath, OutputName);
        
        if (bSaved)
        {
            UE_LOG(LogTemp, Log, TEXT("Saved weight buffer texture: %s/%s"), *OutputPath, *OutputName);
            UE_LOG(LogTemp, Log, TEXT("Visible Texels: %d out of %d (%.1f%%)"), 
                VisibleTexelsCount, NumTexels, (VisibleTexelsCount * 100.0f) / NumTexels);
        }
    }
    
    return VisibilityTexture;
}

bool FTextureUtils::ExportTextureToFile(
    UTexture2D* Texture,
    const FString& FilePath,
    const FString& FileName,
    bool bOverwriteExisting,
    EImageFormat Format)
{
    if (!Texture)
    {
        UE_LOG(LogTemp, Error, TEXT("ExportTextureToFile: Invalid texture"));
        return false;
    }
    if (!Texture->GetPlatformData() || Texture->GetPlatformData()->Mips.Num() == 0)
    {
        UE_LOG(LogTemp, Error, TEXT("ExportTextureToFile: Texture is not valid or not loaded"));
        return false;
    }
    // Check if this is likely a depth texture based on its properties
    bool bIsDepthTexture = Texture->CompressionSettings == TC_Grayscale || 
                           Texture->CompressionSettings == TC_Alpha ||
                           Texture->CompressionSettings == TC_Displacementmap;
    
    // Set appropriate compression setting
    Texture->CompressionSettings = bIsDepthTexture ? TC_Grayscale : TC_EditorIcon;
    
    // Depth textures typically should not use sRGB
    Texture->SRGB = !bIsDepthTexture; 
    Texture->UpdateResource();
    

    // // Ensure the texture has compatible compression settings for export
    // Texture->CompressionSettings = TC_EditorIcon;
    // Texture->SRGB = true;
    // Texture->UpdateResource();
    
    // Create the full path including file name and extension
    FString Extension = (Format == EImageFormat::PNG) ? TEXT(".png") : TEXT(".jpg");
    
    // Ensure the directory exists
    FString Directory = FilePath;
    if (!FPaths::DirectoryExists(Directory))
    {
        IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
        if (!PlatformFile.CreateDirectoryTree(*Directory))
        {
            UE_LOG(LogTemp, Error, TEXT("ExportTextureToFile: Failed to create directory: %s"), *Directory);
            return false;
        }
    }
    
    // Create the full file path
    FString FullFilePath = FPaths::Combine(FilePath, FileName + Extension);
    
    // Set up the export options
    FImageWriteOptions WriteOptions;
    WriteOptions.Format = (Format == EImageFormat::PNG) ? 
                          EDesiredImageFormat::PNG : EDesiredImageFormat::JPG;
    WriteOptions.bOverwriteFile = bOverwriteExisting;
    WriteOptions.CompressionQuality = 95;
    
    // Export the texture
    UImageWriteBlueprintLibrary::ExportToDisk(Texture, FullFilePath, WriteOptions);
    
    // Log the export attempt
    UE_LOG(LogTemp, Log, TEXT("ExportTextureToFile: Requested export to: %s"), *FullFilePath);
    
    return true;
}

UTexture2D* FTextureUtils::CreateTestColorTexture(int32 Width, int32 Height)
{
    // Create a color buffer with a simple pattern
    TArray<FColor> ColorBuffer;
    ColorBuffer.SetNum(Width * Height);
    
    for (int32 Y = 0; Y < Height; Y++)
    {
        for (int32 X = 0; X < Width; X++)
        {
            // Create a gradient or pattern (e.g., red to blue gradient)
            uint8 R = FMath::Clamp(X * 255 / Width, 0, 255);
            uint8 B = FMath::Clamp(Y * 255 / Height, 0, 255);
            ColorBuffer[Y * Width + X] = FColor(R, 0, B, 255);
        }
    }
    
    // Create a texture from this buffer
    UTexture2D* ColorTexture = FTextureUtils::CreateTextureFromPixelData(Width, Height, ColorBuffer);
    if (ColorTexture)
    {
        ColorTexture->CompressionSettings = TC_EditorIcon;
        ColorTexture->SRGB = true;
        ColorTexture->UpdateResource();
    }
    
    return ColorTexture;
}
// bool FTextureUtils::NormalizeAndExportRenderTarget(
//     UWorld* World,
//     UTextureRenderTarget2D* SourceRT,
//     const FString& ExportDirectory,
//     const FString& BaseFileName,
//     bool bInvertDepth)
// {
//     if (!SourceRT || !SourceRT->GameThread_GetRenderTargetResource())
//     {
//         UE_LOG(LogTemp, Error, TEXT("NormalizeAndExportRenderTarget: Invalid parameters"));
//         return false;
//     }
    
//     // Ensure export directory exists
//     IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
//     if (!PlatformFile.DirectoryExists(*ExportDirectory))
//     {
//         if (!PlatformFile.CreateDirectoryTree(*ExportDirectory))
//         {
//             UE_LOG(LogTemp, Error, TEXT("Failed to create directory: %s"), *ExportDirectory);
//             return false;
//         }
//     }
    
//     // Prepare file path
//     FString FileName = BaseFileName;
//     if (!FileName.EndsWith(".png"))
//     {
//         FileName += TEXT(".png");
//     }
//     FString FullFilePath = FPaths::Combine(ExportDirectory, FileName);
    
//     // Check texture format to determine appropriate read approach
//     EPixelFormat PixelFormat = SourceRT->GetFormat();
//     FTextureRenderTargetResource* RTResource = SourceRT->GameThread_GetRenderTargetResource();
    
//     TArray<FColor> ColorBuffer;
    
//     // For floating-point formats, read and normalize
//     if (PixelFormat == PF_FloatRGBA || PixelFormat == PF_R16F || PixelFormat == PF_G16R16F || PixelFormat == PF_FloatR11G11B10)
//     {
//         UE_LOG(LogTemp, Log, TEXT("Processing floating-point render target with normalization"));
        
//         // Read data from source RT as float16
//         TArray<FFloat16Color> DepthData;
//         RTResource->ReadFloat16Pixels(DepthData);
        
//         // Find min/max for normalization
//         float MinDepth = MAX_FLT;
//         float MaxDepth = 0.0f;
        
//         for (const FFloat16Color& Pixel : DepthData)
//         {
//             float Depth = Pixel.R; // Depth is typically stored in R channel
//             if (Depth < (65504.0f - 1.0f) && Depth > 0.0f)
//             {
//                 MinDepth = FMath::Min(MinDepth, Depth);
//                 MaxDepth = FMath::Max(MaxDepth, Depth);
//             }
//         }
        
//         float DepthRange = MaxDepth - MinDepth;
//         if (DepthRange <= 0.0f)
//         {
//             DepthRange = 1.0f; // Prevent division by zero
//         }
        
//         UE_LOG(LogTemp, Log, TEXT("Depth range: %.2f to %.2f"), MinDepth, MaxDepth);
        
//         // Create normalized color data
//         ColorBuffer.SetNum(DepthData.Num());
        
//         for (int32 i = 0; i < DepthData.Num(); i++)
//         {
//             // Start with black (background)
//             ColorBuffer[i] = FColor::Black;
            
//             float Depth = DepthData[i].R;
//             if (Depth < (65504.0f - 1.0f) && Depth > 0.0f)
//             {
//                 float NormalizedDepth = FMath::Clamp((Depth - MinDepth) / DepthRange, 0.0f, 1.0f);
                
//                 // Invert if requested
//                 if (bInvertDepth)
//                 {
//                     NormalizedDepth = 1.0f - NormalizedDepth;
//                 }
                
//                 uint8 GrayValue = FMath::RoundToInt(NormalizedDepth * 255.0f);
//                 ColorBuffer[i] = FColor(GrayValue, GrayValue, GrayValue, 255);
//             }
//         }
//     }
//     // For standard formats, just read directly
//     else
//     {
//         UE_LOG(LogTemp, Log, TEXT("Processing standard format render target"));
        
//         // Read standard format pixels directly
//         RTResource->ReadPixels(ColorBuffer);
        
//         // Apply inversion if requested (although typically not needed for non-depth data)
//         if (bInvertDepth)
//         {
//             for (int32 i = 0; i < ColorBuffer.Num(); i++)
//             {
//                 ColorBuffer[i].R = 255 - ColorBuffer[i].R;
//                 ColorBuffer[i].G = 255 - ColorBuffer[i].G;
//                 ColorBuffer[i].B = 255 - ColorBuffer[i].B;
//                 // Alpha usually should not be inverted
//             }
//         }
//     }
    
//     // Export as PNG using the same method for all formats
//     TArray64<uint8> CompressedPNG;
//     FImageUtils::PNGCompressImageArray(SourceRT->SizeX, SourceRT->SizeY, ColorBuffer, CompressedPNG);
    
//     // Save to disk
//     if (FFileHelper::SaveArrayToFile(CompressedPNG, *FullFilePath))
//     {
//         UE_LOG(LogTemp, Log, TEXT("Exported render target as PNG: %s"), *FullFilePath);
//         return true;
//     }
//     else
//     {
//         UE_LOG(LogTemp, Error, TEXT("Failed to write PNG file: %s"), *FullFilePath);
//         return false;
//     }
// }


bool FTextureUtils::NormalizeAndExportRenderTarget(
    UWorld* World,
    UTextureRenderTarget2D* SourceRT,
    const FString& ExportDirectory,
    const FString& BaseFileName,
    bool bInvertDepth)
{
    if (!SourceRT || !SourceRT->GameThread_GetRenderTargetResource())
    {
        UE_LOG(LogTemp, Error, TEXT("NormalizeAndExportRenderTarget: Invalid parameters"));
        return false;
    }
    
    // Ensure export directory exists
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    if (!PlatformFile.DirectoryExists(*ExportDirectory))
    {
        if (!PlatformFile.CreateDirectoryTree(*ExportDirectory))
        {
            UE_LOG(LogTemp, Error, TEXT("Failed to create directory: %s"), *ExportDirectory);
            return false;
        }
    }
    
    // Prepare file path
    FString FileName = BaseFileName;
    if (!FileName.EndsWith(".png"))
    {
        FileName += TEXT(".png");
    }
    FString FullFilePath = FPaths::Combine(ExportDirectory, FileName);
    
    // Check texture format to determine appropriate read approach
    EPixelFormat PixelFormat = SourceRT->GetFormat();
    FTextureRenderTargetResource* RTResource = SourceRT->GameThread_GetRenderTargetResource();
    
    TArray<FColor> ColorBuffer;
    
    // For floating-point formats, read and normalize
    if (PixelFormat == PF_FloatRGBA || PixelFormat == PF_R16F || PixelFormat == PF_G16R16F || PixelFormat == PF_FloatR11G11B10)
    {
        UE_LOG(LogTemp, Log, TEXT("Processing floating-point render target with normalization"));
        
        // Read data from source RT as float16
        TArray<FFloat16Color> DepthData;
        RTResource->ReadFloat16Pixels(DepthData);
        
        // Find min/max for normalization
        float MinDepth = MAX_FLT;
        float MaxDepth = 0.0f;
        
        for (const FFloat16Color& Pixel : DepthData)
        {
            float Depth = Pixel.R; // Depth is typically stored in R channel
            if (Depth < (65504.0f - 1.0f) && Depth > 0.0f)
            {
                MinDepth = FMath::Min(MinDepth, Depth);
                MaxDepth = FMath::Max(MaxDepth, Depth);
            }
        }
        
        float DepthRange = MaxDepth - MinDepth;
        if (DepthRange <= 0.0f)
        {
            DepthRange = 1.0f; // Prevent division by zero
        }
        
        UE_LOG(LogTemp, Log, TEXT("Depth range: %.2f to %.2f"), MinDepth, MaxDepth);
        
        // Create normalized color data
        ColorBuffer.SetNum(DepthData.Num());
        
        for (int32 i = 0; i < DepthData.Num(); i++)
        {
            // Start with black (background)
            ColorBuffer[i] = FColor::Black;
            
            float Depth = DepthData[i].R;
            if (Depth < (65504.0f - 1.0f) && Depth > 0.0f)
            {
                float NormalizedDepth = FMath::Clamp((Depth - MinDepth) / DepthRange, 0.0f, 1.0f);
                
                // Invert if requested
                if (bInvertDepth)
                {
                    NormalizedDepth = 1.0f - NormalizedDepth;
                }
                float EncodedDepth = 0.1f + NormalizedDepth * 0.9f;
                

                uint8 GrayValue = FMath::RoundToInt(EncodedDepth * 255.0f);
                ColorBuffer[i] = FColor(GrayValue, GrayValue, GrayValue, 255);
            }
        }
    }
    // For standard formats, just read directly
    else
    {
        UE_LOG(LogTemp, Log, TEXT("Processing standard format render target"));
        
        // Read standard format pixels directly
        RTResource->ReadPixels(ColorBuffer);
        
        // Apply inversion if requested (although typically not needed for non-depth data)
        if (bInvertDepth)
        {
            for (int32 i = 0; i < ColorBuffer.Num(); i++)
            {
                ColorBuffer[i].R = 255 - ColorBuffer[i].R;
                ColorBuffer[i].G = 255 - ColorBuffer[i].G;
                ColorBuffer[i].B = 255 - ColorBuffer[i].B;
                // Alpha usually should not be inverted
            }
        }
    }
    
    // Export as PNG using the same method for all formats
    TArray64<uint8> CompressedPNG;
    FImageUtils::PNGCompressImageArray(SourceRT->SizeX, SourceRT->SizeY, ColorBuffer, CompressedPNG);
    
    // Save to disk
    if (FFileHelper::SaveArrayToFile(CompressedPNG, *FullFilePath))
    {
        UE_LOG(LogTemp, Log, TEXT("Exported render target as PNG: %s"), *FullFilePath);
        return true;
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to write PNG file: %s"), *FullFilePath);
        return false;
    }
}


bool FTextureUtils::ExportRenderTarget(
    UWorld* /*World*/,
    UTextureRenderTarget2D* SourceRT,
    const FString& ExportDirectory,
    const FString& BaseFileName,
    bool bInvertColors)
{
    if (!SourceRT)
    {
        UE_LOG(LogTemp, Error, TEXT("ExportRenderTarget: SourceRT is null."));
        return false;
    }

    FTextureRenderTargetResource* RTResource = SourceRT->GameThread_GetRenderTargetResource();
    if (!RTResource)
    {
        UE_LOG(LogTemp, Error, TEXT("ExportRenderTarget: RenderTargetResource is null."));
        return false;
    }

    // Ensure export directory exists
    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
    if (!PF.DirectoryExists(*ExportDirectory) && !PF.CreateDirectoryTree(*ExportDirectory))
    {
        UE_LOG(LogTemp, Error, TEXT("ExportRenderTarget: Failed to create directory: %s"), *ExportDirectory);
        return false;
    }

    // Build full file path
    FString FileName = BaseFileName.EndsWith(TEXT(".png"), ESearchCase::IgnoreCase)
                       ? BaseFileName
                       : (BaseFileName + TEXT(".png"));
    const FString FullPath = FPaths::Combine(ExportDirectory, FileName);

    const int32 SizeX = SourceRT->SizeX;
    const int32 SizeY = SourceRT->SizeY;

    TArray<FColor> OutPixels;
    OutPixels.Reserve(SizeX * SizeY);

    const EPixelFormat PixelFormat = SourceRT->GetFormat();

    // ---- Readback (and guarantee sRGB-encoded 8-bit pixels) ----
    bool bReadOK = false;

    // Prefer reading linear then converting, for HDR formats.
    if (PixelFormat == PF_A32B32G32R32F || PixelFormat == PF_FloatRGBA ||
        PixelFormat == PF_R16F || PixelFormat == PF_G16R16F || PixelFormat == PF_FloatR11G11B10)
    {
        // Try direct linear read first.
        TArray<FLinearColor> Linear;
        if (RTResource->ReadLinearColorPixels(Linear))
        {
            OutPixels.SetNumUninitialized(Linear.Num());
            for (int32 i = 0; i < Linear.Num(); ++i)
            {
                FColor C = Linear[i].ToFColor(/*bSRGB=*/true); // linear -> sRGB
                if (bInvertColors) { C.R = 255 - C.R; C.G = 255 - C.G; C.B = 255 - C.B; }
                C.A = 255;
                OutPixels[i] = C;
            }
            bReadOK = true;
        }
        else
        {
            // Fallback: read half-floats then convert
            TArray<FFloat16Color> Float16;
            if (RTResource->ReadFloat16Pixels(Float16))
            {
                OutPixels.SetNumUninitialized(Float16.Num());
                for (int32 i = 0; i < Float16.Num(); ++i)
                {
                    const FLinearColor L(
                        Float16[i].R.GetFloat(),
                        Float16[i].G.GetFloat(),
                        Float16[i].B.GetFloat(),
                        Float16[i].A.GetFloat());

                    FColor C = L.ToFColor(/*bSRGB=*/true); // linear -> sRGB
                    if (bInvertColors) { C.R = 255 - C.R; C.G = 255 - C.G; C.B = 255 - C.B; }
                    C.A = 255;
                    OutPixels[i] = C;
                }
                bReadOK = true;
            }
        }
    }
    else
    {
        // LDR formats: ask the GPU to convert linear->sRGB for us when reading.
        FReadSurfaceDataFlags Flags(RCM_UNorm);
        Flags.SetLinearToGamma(true); // crucial for BaseColor capture (which is linear)

        if (RTResource->ReadPixels(OutPixels, Flags))
        {
            for (FColor& C : OutPixels)
            {
                if (bInvertColors) { C.R = 255 - C.R; C.G = 255 - C.G; C.B = 255 - C.B; }
                C.A = 255;
            }
            bReadOK = true;
        }
    }

    if (!bReadOK || OutPixels.Num() == 0)
    {
        UE_LOG(LogTemp, Error, TEXT("ExportRenderTarget: Failed to read pixels from RT (format %s)."),
               GPixelFormats[PixelFormat].Name);
        return false;
    }

    // ---- PNG encode and save ----
    TArray64<uint8> CompressedPNG;
    FImageUtils::PNGCompressImageArray(SizeX, SizeY, OutPixels, CompressedPNG);

    if (!FFileHelper::SaveArrayToFile(CompressedPNG, *FullPath))
    {
        UE_LOG(LogTemp, Error, TEXT("ExportRenderTarget: Failed to write PNG file: %s"), *FullPath);
        return false;
    }

    UE_LOG(LogTemp, Log, TEXT("ExportRenderTarget: Wrote PNG (sRGB) to: %s"), *FullPath);
    return true;
}

/**
     * Creates a new UTexture2D or updates an existing one with new pixel data.
     * This prevents creating unnecessary transient texture assets on each update.
     * @param InTexture The texture to update. If null, a new one is created.
     * @param SrcWidth The width of the source pixel data.
     * @param SrcHeight The height of the source pixel data.
     * @param SrcData The raw pixel data (must match width/height).
     * @return The created or updated texture, or nullptr on failure.
     */
     UTexture2D* FTextureUtils::CreateOrUpdateTexture(UTexture2D* InTexture, int32 SrcWidth, int32 SrcHeight, const TArray<FColor>& SrcData)
    {
        // --- 1. Validate inputs ---
        if (SrcWidth <= 0 || SrcHeight <= 0 || SrcData.Num() != (SrcWidth * SrcHeight))
        {
            UE_LOG(LogTemp, Error, TEXT("CreateOrUpdateTexture: Invalid dimensions or data size."));
            return nullptr;
        }

        UTexture2D* OutTexture = InTexture;

        // --- 2. Create a new texture if the existing one is invalid or has the wrong size ---
        if (!IsValid(OutTexture) || OutTexture->GetSizeX() != SrcWidth || OutTexture->GetSizeY() != SrcHeight)
        {
            // Create a new transient texture that can be used by the material system.
            OutTexture = UTexture2D::CreateTransient(SrcWidth, SrcHeight, PF_B8G8R8A8);
            if (!OutTexture)
            {
                UE_LOG(LogTemp, Error, TEXT("CreateOrUpdateTexture: Failed to create transient texture."));
                return nullptr;
            }
            // Keep a reference to it so it doesn't get garbage collected.
            OutTexture->AddToRoot();
        }

        // --- 3. Lock the texture's platform data for writing ---
        void* TextureData = OutTexture->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
        if (!TextureData)
        {
            UE_LOG(LogTemp, Error, TEXT("CreateOrUpdateTexture: Could not lock texture data for writing."));
            return nullptr;
        }

        // --- 4. Copy the pixel data and update the texture ---
        const int32 DataSize = SrcData.Num() * sizeof(FColor);
        FMemory::Memcpy(TextureData, SrcData.GetData(), DataSize);
        OutTexture->GetPlatformData()->Mips[0].BulkData.Unlock();
        OutTexture->UpdateResource();

        return OutTexture;
    }

    // Add this at the end of TextureUtils.cpp
#include "Math/RandomStream.h" // Required for generating random colors

void FTextureUtils::SaveUVIslandMapAsTexture(const TArray<int32>& UVIslandIDMap, int32 TextureWidth, int32 TextureHeight, const FString& PackagePath, const FString& AssetName)
{
    if (UVIslandIDMap.Num() == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("SaveUVIslandMapAsTexture: Provided map is empty."));
        return;
    }

    TArray<FColor> ColorBuffer;
    ColorBuffer.SetNum(TextureWidth * TextureHeight);

    // Use a map to store a consistent color for each island ID
    TMap<int32, FColor> IslandColors;
    IslandColors.Add(0, FColor::Black); // Island ID 0 is always black (empty space)

    for (int32 i = 0; i < UVIslandIDMap.Num(); ++i)
    {
        const int32 IslandID = UVIslandIDMap[i];
        if (IslandID == 0)
        {
            ColorBuffer[i] = IslandColors[0];
            continue;
        }

        // Check if we've already generated a color for this island
        if (!IslandColors.Contains(IslandID))
        {
            // If not, generate a new random color. Seeding the random stream with the
            // island ID ensures the color is consistent but unique for each island.
            FRandomStream RandomStream(IslandID);
            FColor NewColor = FColor(
                RandomStream.RandRange(50, 255), // Avoid very dark colors
                RandomStream.RandRange(50, 255),
                RandomStream.RandRange(50, 255),
                255
            );
            IslandColors.Add(IslandID, NewColor);
        }

        ColorBuffer[i] = IslandColors[IslandID];
    }

    // Now use our existing helper function to save this color buffer as a texture asset
    SaveColorBufferAsTexture(ColorBuffer, TextureWidth, TextureHeight, PackagePath, AssetName);
}


UTexture2D* FTextureUtils::CreateTextureFromRenderTarget(UTextureRenderTarget2D* RenderTarget)
{
    if (!RenderTarget)
    {
        UE_LOG(LogTemp, Error, TEXT("CreateTextureFromRenderTarget: RenderTarget is null."));
        return nullptr;
    }

    FTextureRenderTargetResource* RTResource = RenderTarget->GameThread_GetRenderTargetResource();
    if (!RTResource)
    {
        UE_LOG(LogTemp, Error, TEXT("CreateTextureFromRenderTarget: RenderTargetResource is null."));
        return nullptr;
    }

    TArray<FColor> PixelData;
    PixelData.SetNum(RenderTarget->SizeX * RenderTarget->SizeY);

    // Read the pixels from the render target
    FReadSurfaceDataFlags ReadPixelFlags(RCM_UNorm);
    if (!RTResource->ReadPixels(PixelData, ReadPixelFlags))
    {
        UE_LOG(LogTemp, Error, TEXT("CreateTextureFromRenderTarget: Failed to read pixels from RenderTarget."));
        return nullptr;
    }

    // Use our existing helper to create the final texture
    return CreateTextureFromPixelData(RenderTarget->SizeX, RenderTarget->SizeY, PixelData);
}

// =============================================================================
// LAPLACIAN PYRAMID BLENDING IMPLEMENTATION
// =============================================================================

FLinearColor FTextureUtils::GetClampedPixel(const TArray<FColor>& Img, int32 x, int32 y, int32 Width, int32 Height)
{
    x = FMath::Clamp(x, 0, Width - 1);
    y = FMath::Clamp(y, 0, Height - 1);
    return FLinearColor(Img[y * Width + x]);
}

TArray<FColor> FTextureUtils::BlurImage(const TArray<FColor>& Img, int32 Width, int32 Height)
{
    const float G_KERNEL[3] = {0.25f, 0.5f, 0.25f};
    TArray<FLinearColor> TempHorizontal;
    TempHorizontal.SetNumUninitialized(Img.Num());

    // Horizontal pass
    for (int32 y = 0; y < Height; ++y) {
        for (int32 x = 0; x < Width; ++x) {
            FLinearColor Sum = FLinearColor::Black;
            Sum += GetClampedPixel(Img, x - 1, y, Width, Height) * G_KERNEL[0];
            Sum += GetClampedPixel(Img, x,     y, Width, Height) * G_KERNEL[1];
            Sum += GetClampedPixel(Img, x + 1, y, Width, Height) * G_KERNEL[2];
            TempHorizontal[y * Width + x] = Sum;
        }
    }

    TArray<FColor> Blurred;
    Blurred.SetNumUninitialized(Img.Num());

    // Vertical pass
    for (int32 y = 0; y < Height; ++y) {
        for (int32 x = 0; x < Width; ++x) {
            FLinearColor Sum = FLinearColor::Black;
            int32 Y_m1 = FMath::Clamp(y - 1, 0, Height - 1);
            int32 Y_p1 = FMath::Clamp(y + 1, 0, Height - 1);
            Sum += TempHorizontal[Y_m1 * Width + x] * G_KERNEL[0];
            Sum += TempHorizontal[y     * Width + x] * G_KERNEL[1];
            Sum += TempHorizontal[Y_p1 * Width + x] * G_KERNEL[2];
            Blurred[y * Width + x] = Sum.ToFColor(true);
        }
    }
    return Blurred;
}

FTextureUtils::FPyramidLayer FTextureUtils::Downsample(const FPyramidLayer& InLayer)
{
    TArray<FColor> Blurred = BlurImage(InLayer.Pixels, InLayer.Width, InLayer.Height);
    
    FPyramidLayer OutLayer;
    OutLayer.Width = FMath::CeilToInt(static_cast<float>(InLayer.Width) / 2.0f);
    OutLayer.Height = FMath::CeilToInt(static_cast<float>(InLayer.Height) / 2.0f);
    OutLayer.Pixels.SetNumUninitialized(OutLayer.Width * OutLayer.Height);

    for (int32 y = 0; y < OutLayer.Height; ++y) {
        for (int32 x = 0; x < OutLayer.Width; ++x) {
            OutLayer.Pixels[y * OutLayer.Width + x] = Blurred[(y * 2) * InLayer.Width + (x * 2)];
        }
    }
    return OutLayer;
}

FTextureUtils::FPyramidLayer FTextureUtils::Upsample(const FPyramidLayer& InLayer)
{
    FPyramidLayer OutLayer;
    OutLayer.Width = InLayer.Width * 2;
    OutLayer.Height = InLayer.Height * 2;
    OutLayer.Pixels.Init(FColor::Black, OutLayer.Width * OutLayer.Height);

    for (int32 y = 0; y < InLayer.Height; ++y) {
        for (int32 x = 0; x < InLayer.Width; ++x) {
            OutLayer.Pixels[(y * 2) * OutLayer.Width + (x * 2)] = InLayer.Pixels[y * InLayer.Width + x];
        }
    }
    
    OutLayer.Pixels = BlurImage(OutLayer.Pixels, OutLayer.Width, OutLayer.Height);
    return OutLayer;
}

TArray<FLinearColor> FTextureUtils::Subtract(const FPyramidLayer& A, const FPyramidLayer& B)
{
    TArray<FLinearColor> Result;
    if (A.Pixels.Num() != B.Pixels.Num())
    {
        return Result;
    }
    
    Result.SetNumUninitialized(A.Pixels.Num());
    for(int32 i = 0; i < A.Pixels.Num(); ++i)
    {
        Result[i] = FLinearColor(A.Pixels[i]) - FLinearColor(B.Pixels[i]);
    }
    return Result;
}

TArray<FColor> FTextureUtils::Add(const FPyramidLayer& A, const FLinearPyramidLayer& B)
{
    TArray<FColor> Result;
    if (A.Pixels.Num() != B.Pixels.Num())
    {
        return Result;
    }
    
    Result.SetNumUninitialized(A.Pixels.Num());
    for(int32 i = 0; i < A.Pixels.Num(); ++i)
    {
        FLinearColor ResLin = FLinearColor(A.Pixels[i]) + B.Pixels[i];
        Result[i] = ResLin.ToFColor(true);
    }
    return Result;
}

TArray<FTextureUtils::FPyramidLayer> FTextureUtils::BuildGaussianPyramid(const TArray<FColor>& Img, int32 Width, int32 Height, int32 Levels)
{
    TArray<FPyramidLayer> Pyramid;
    Pyramid.Add({Img, Width, Height});

    for (int32 i = 0; i < Levels - 1; ++i) {
        Pyramid.Add(Downsample(Pyramid.Last()));
    }
    return Pyramid;
}

void FTextureUtils::Linear8ToSrgb8(const TArray<FColor>& InLinear, TArray<FColor>& OutSrgb)
{
    OutSrgb.SetNumUninitialized(InLinear.Num());
    for (int32 i = 0; i < InLinear.Num(); ++i)
    {
        const FColor c = InLinear[i];
        const FLinearColor lin(
            c.R / 255.0f,
            c.G / 255.0f,
            c.B / 255.0f,
            c.A / 255.0f
        );
        OutSrgb[i] = lin.ToFColorSRGB();   // linear -> sRGB8
    }
}

TArray<FTextureUtils::FLinearPyramidLayer> FTextureUtils::BuildLaplacianPyramid(const TArray<FPyramidLayer>& GaussianPyramid)
{
    TArray<FLinearPyramidLayer> Pyramid;
    for (int32 i = 0; i < GaussianPyramid.Num() - 1; ++i) {
        FPyramidLayer Upsampled = Upsample(GaussianPyramid[i+1]);

        if (Upsampled.Width != GaussianPyramid[i].Width || Upsampled.Height != GaussianPyramid[i].Height)
        {
             TArray<FColor> CroppedPixels;
             CroppedPixels.SetNumUninitialized(GaussianPyramid[i].Width * GaussianPyramid[i].Height);
             for(int32 y = 0; y < GaussianPyramid[i].Height; ++y)
             {
                 FMemory::Memcpy(&CroppedPixels[y * GaussianPyramid[i].Width], &Upsampled.Pixels[y * Upsampled.Width], GaussianPyramid[i].Width * sizeof(FColor));
             }
             Upsampled.Pixels = CroppedPixels;
             Upsampled.Width = GaussianPyramid[i].Width;
             Upsampled.Height = GaussianPyramid[i].Height;
        }

        Pyramid.Add({Subtract(GaussianPyramid[i], Upsampled), GaussianPyramid[i].Width, GaussianPyramid[i].Height});
    }
    
    FLinearPyramidLayer TopLayer;
    TopLayer.Width = GaussianPyramid.Last().Width;
    TopLayer.Height = GaussianPyramid.Last().Height;
    TopLayer.Pixels.SetNumUninitialized(GaussianPyramid.Last().Pixels.Num());
    for(int32 i = 0; i < TopLayer.Pixels.Num(); ++i)
    {
        TopLayer.Pixels[i] = FLinearColor(GaussianPyramid.Last().Pixels[i]);
    }
    Pyramid.Add(TopLayer);
    
    return Pyramid;
}

void FTextureUtils::CollapsePyramid(const TArray<FLinearPyramidLayer>& Pyramid, TArray<FColor>& OutImage)
{
    FPyramidLayer CurrentLayer;
    CurrentLayer.Width = Pyramid.Last().Width;
    CurrentLayer.Height = Pyramid.Last().Height;
    CurrentLayer.Pixels.SetNumUninitialized(Pyramid.Last().Pixels.Num());
    for(int i=0; i<CurrentLayer.Pixels.Num(); ++i)
    {
        CurrentLayer.Pixels[i] = Pyramid.Last().Pixels[i].ToFColor(true);
    }

    for (int32 i = Pyramid.Num() - 2; i >= 0; --i) {
        FPyramidLayer Upsampled = Upsample(CurrentLayer);
        const FLinearPyramidLayer& LaplacianLevel = Pyramid[i];

        if (Upsampled.Width != LaplacianLevel.Width || Upsampled.Height != LaplacianLevel.Height)
        {
             TArray<FColor> CroppedPixels;
             CroppedPixels.SetNumUninitialized(LaplacianLevel.Width * LaplacianLevel.Height);
             for(int32 y = 0; y < LaplacianLevel.Height; ++y)
             {
                 FMemory::Memcpy(&CroppedPixels[y * LaplacianLevel.Width], &Upsampled.Pixels[y * Upsampled.Width], LaplacianLevel.Width * sizeof(FColor));
             }
             Upsampled.Pixels = CroppedPixels;
             Upsampled.Width = LaplacianLevel.Width;
             Upsampled.Height = LaplacianLevel.Height;
        }
        
        CurrentLayer.Pixels = Add(Upsampled, LaplacianLevel);
        CurrentLayer.Width = LaplacianLevel.Width;
        CurrentLayer.Height = LaplacianLevel.Height;
    }
    OutImage = CurrentLayer.Pixels;
}


void FTextureUtils::BlendWithLaplacianPyramid(
    const TArray<FColor>& BasePixels,
    const TArray<FColor>& TopPixels,
    const TArray<FColor>& MaskPixels,
    int32 Width,
    int32 Height,
    TArray<FColor>& OutBlendedPixels,
    int32 Levels)
{
    // 1. Build Gaussian pyramids
    TArray<FPyramidLayer> GP_Base = BuildGaussianPyramid(BasePixels, Width, Height, Levels);
    TArray<FPyramidLayer> GP_Top = BuildGaussianPyramid(TopPixels, Width, Height, Levels);
    TArray<FPyramidLayer> GP_Mask = BuildGaussianPyramid(MaskPixels, Width, Height, Levels);

    // 2. Build Laplacian pyramids
    TArray<FLinearPyramidLayer> LP_Base = BuildLaplacianPyramid(GP_Base);
    TArray<FLinearPyramidLayer> LP_Top = BuildLaplacianPyramid(GP_Top);

    // 3. Blend each level
    TArray<FLinearPyramidLayer> Blended_LP;
    for (int32 i = 0; i < Levels; ++i)
    {
        const FLinearPyramidLayer& L1 = LP_Base[i];
        const FLinearPyramidLayer& L2 = LP_Top[i];
        const FPyramidLayer& M = GP_Mask[i];
        
        FLinearPyramidLayer BlendedLevel;
        BlendedLevel.Width = L1.Width;
        BlendedLevel.Height = L1.Height;
        BlendedLevel.Pixels.SetNumUninitialized(L1.Pixels.Num());

        for (int32 p = 0; p < L1.Pixels.Num(); ++p)
        {
            float MaskWeight = FLinearColor(M.Pixels[p]).R;
            BlendedLevel.Pixels[p] = FMath::Lerp(L1.Pixels[p], L2.Pixels[p], MaskWeight);
        }
        Blended_LP.Add(BlendedLevel);
    }

    // 4. Collapse the result
    CollapsePyramid(Blended_LP, OutBlendedPixels);
}



static inline uint8 LinearToSrgb8(double l)
{
    l = FMath::Clamp(l, 0.0, 1.0);
    const double s = (l <= 0.0031308) ? 12.92 * l : 1.055 * FMath::Pow(l, 1.0 / 2.4) - 0.055;
    return (uint8)FMath::Clamp(FMath::RoundToInt(s * 255.0), 0, 255);
}


bool FTextureUtils::ComposeBaseLaid(
    const TArray<FColor>& PerspectiveSrgbRGBA,
    const TArray<FColor>& SilhouetteSrgbRGBA,
    int32 Width,
    int32 Height,
    TArray<FColor>& OutSrgbRGBA,
    bool  bTransparentBackground,
    float AlphaFloor)
{
    OutSrgbRGBA.Reset();

    const int32 N = Width * Height;
    if (Width <= 0 || Height <= 0 ||
        PerspectiveSrgbRGBA.Num() != N ||
        SilhouetteSrgbRGBA.Num()  != N)
    {
        UE_LOG(LogTemp, Error, TEXT("ComposeBaseLaid: invalid sizes (W=%d H=%d, Pers=%d, Sil=%d)"),
            Width, Height, PerspectiveSrgbRGBA.Num(), SilhouetteSrgbRGBA.Num());
        return false;
    }

    OutSrgbRGBA.SetNumUninitialized(N);
    const uint8 BgA = bTransparentBackground ? 0 : 255;

    // --- Debug counters ---
    int32 C_Bg = 0;                    // silhouette off-mesh pixels
    int32 C_In = 0;                    // silhouette on-mesh pixels
    int32 C_A0 = 0;                    // a ~ 0 (<= 1)
    int32 C_Alo = 0;                   // a < 0.05
    int32 C_Ahi = 0;                   // a > 0.95
    int32 C_HighA_BlackRGB = 0;        // a > 0.95 but projected RGB very dark (luma < 8)
    uint8 A_min = 255, A_max = 0;      // min/max alpha inside silhouette
    double A_sum = 0.0;                // average alpha inside silhouette

    // Keep a few samples to print (indices)
    int32 SampleIdxA0[4]  = { -1,-1,-1,-1 };
    int32 SampleIdxAHi[4] = { -1,-1,-1,-1 };
    int32 SampleIdxWeird[4] = { -1,-1,-1,-1 }; // high alpha & black RGB

    auto PushSample = [](int32 idx, int32 (&arr)[4])
    {
        for (int k=0;k<4;++k) { if (arr[k] < 0) { arr[k] = idx; break; } }
    };

    bool bWroteAny = false;

    for (int32 i = 0; i < N; ++i)
    {
        const FColor& Pers = PerspectiveSrgbRGBA[i];
        const FColor& Sil  = SilhouetteSrgbRGBA[i];

        // Gate by silhouette alpha (expect bg alpha 0 from generator)
        if (Sil.A == 0)
        {
            OutSrgbRGBA[i] = FColor(0, 0, 0, BgA); // background stays black
            ++C_Bg;
            continue;
        }

        ++C_In;

        // Projected alpha drives the lerp
        double a = (double)Pers.A / 255.0;
        if (a < (double)AlphaFloor) a = 0.0;

        // Stats
        const uint8 a8_raw = Pers.A;
        A_min = FMath::Min<uint8>(A_min, a8_raw);
        A_max = FMath::Max<uint8>(A_max, a8_raw);
        A_sum += a;

        if (a8_raw <= 1) { ++C_A0; PushSample(i, SampleIdxA0); }
        if (a < 0.05)     { ++C_Alo; }
        if (a > 0.95)     { ++C_Ahi; }

        const float Luma = 0.2126f*Pers.R + 0.7152f*Pers.G + 0.0722f*Pers.B;
        if (a > 0.95 && Luma < 8.0f) { ++C_HighA_BlackRGB; PushSample(i, SampleIdxWeird); }

        // sRGB -> linear (both inputs are sRGB FColor)
        const double Rb = SrgbToLinearUnit((double)Sil.R / 255.0);
        const double Gb = SrgbToLinearUnit((double)Sil.G / 255.0);
        const double Bb = SrgbToLinearUnit((double)Sil.B / 255.0);

        const double Rp = SrgbToLinearUnit((double)Pers.R / 255.0);
        const double Gp = SrgbToLinearUnit((double)Pers.G / 255.0);
        const double Bp = SrgbToLinearUnit((double)Pers.B / 255.0);

        // Linear lerp: Co = (1-a)*Base + a*Projected
        const double Ro = (1.0 - a) * Rb + a * Rp;
        const double Go = (1.0 - a) * Gb + a * Gp;
        const double Bo = (1.0 - a) * Bb + a * Bp;

        OutSrgbRGBA[i] = FColor(LinearToSrgb8(Ro), LinearToSrgb8(Go), LinearToSrgb8(Bo), 255);
        bWroteAny = true;
    }

    // --- Debug summary ---
    if (C_In > 0)
    {
        const double InPct  = 100.0 * double(C_In) / double(N);
        const double BgPct  = 100.0 * double(C_Bg) / double(N);
        const double A0Pct  = 100.0 * double(C_A0) / double(C_In);
        const double AloPct = 100.0 * double(C_Alo) / double(C_In);
        const double AhiPct = 100.0 * double(C_Ahi) / double(C_In);
        const double WeirdPct = 100.0 * double(C_HighA_BlackRGB) / double(C_In);
        const double A_avg = (C_In > 0) ? (A_sum / double(C_In)) : 0.0;

        UE_LOG(LogTemp, Warning,
            TEXT("ComposeBaseLaid: N=%d  OnMesh=%d (%.1f%%)  Bg=%d (%.1f%%)  Alpha[min=%u max=%u avg=%.3f]"),
            N, C_In, InPct, C_Bg, BgPct, (uint32)A_min, (uint32)A_max, A_avg);

        UE_LOG(LogTemp, Warning,
            TEXT("  Inside silhouette alpha distribution:  a<=1: %d (%.1f%%),  a<0.05: %d (%.1f%%),  a>0.95: %d (%.1f%%),  highA & blackRGB: %d (%.1f%%)"),
            C_A0, A0Pct, C_Alo, AloPct, C_Ahi, AhiPct, C_HighA_BlackRGB, WeirdPct);

        auto LogSample = [&](const TCHAR* Label, int32 idx)
        {
            if (idx < 0) return;
            const FColor& p = PerspectiveSrgbRGBA[idx];
            const FColor& s = SilhouetteSrgbRGBA[idx];
            const int32 x = idx % Width, y = idx / Width;
            UE_LOG(LogTemp, Warning,
                TEXT("%s @(%d,%d): Pers RGBA=(%3u,%3u,%3u,%3u)  Sil RGBA=(%3u,%3u,%3u,%3u)"),
                Label, x, y, p.R, p.G, p.B, p.A, s.R, s.G, s.B, s.A);
        };

        // Show a few representative samples
        for (int k=0;k<4;++k) LogSample(TEXT("SAMPLE a≈0  "), SampleIdxA0[k]);
        for (int k=0;k<4;++k) LogSample(TEXT("SAMPLE a>0.95 & blackRGB"), SampleIdxWeird[k]);
        for (int k=0;k<4;++k) LogSample(TEXT("SAMPLE any a>0.95"), SampleIdxAHi[k]);
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("ComposeBaseLaid: No silhouette pixels (Sil.A>0) — output will be entirely background."));
    }

    return bWroteAny;
}
// =============================================================================
// END OF LAPLACIAN PYRAMID BLENDING IMPLEMENTATION
// =============================================================================

bool FTextureUtils::ExportTexture2DToPNG(UTexture2D* Texture, const FString& FilePath)
{
	if (!Texture)
	{
		UE_LOG(LogTemp, Error, TEXT("ExportTexture2DToPNG: Input Texture was null."));
		return false;
	}

	const int32 Width = Texture->GetSizeX();
	const int32 Height = Texture->GetSizeY();

	if (Width == 0 || Height == 0)
	{
		UE_LOG(LogTemp, Error, TEXT("ExportTexture2DToPNG: Input Texture has zero dimensions."));
		return false;
	}

	// Create a temporary Render Target to draw the texture to.
	// This is the most reliable way to get the raw pixel data, as it handles any compression.
	UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>();
	RenderTarget->InitCustomFormat(Width, Height, PF_B8G8R8A8, true); // true for linear gamma

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		UE_LOG(LogTemp, Error, TEXT("ExportTexture2DToPNG: Could not get a valid editor world."));
		RenderTarget->MarkAsGarbage();
		return false;
	}

	// Draw the source texture into the render target
	UCanvas* Canvas;
	FVector2D CanvasSize;
	FDrawToRenderTargetContext Context;
	UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(World, RenderTarget, Canvas, CanvasSize, Context);
	if (Canvas)
	{
		Canvas->K2_DrawTexture(Texture, FVector2D::ZeroVector, CanvasSize, FVector2D::ZeroVector);
	}
	UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(World, Context);

	// Read the pixels back from the render target
	TArray<FColor> PixelData;
	FTextureRenderTargetResource* RTResource = RenderTarget->GameThread_GetRenderTargetResource();
	if (!RTResource || !RTResource->ReadPixels(PixelData))
	{
		UE_LOG(LogTemp, Error, TEXT("ExportTexture2DToPNG: Failed to read pixels from temporary render target."));
		RenderTarget->MarkAsGarbage();
		return false;
	}

	// Compress the pixel data into a PNG byte array
	TArray64<uint8> CompressedPNG;
	FImageUtils::PNGCompressImageArray(Width, Height, PixelData, CompressedPNG);

	if (CompressedPNG.Num() == 0)
	{
		UE_LOG(LogTemp, Error, TEXT("ExportTexture2DToPNG: PNG compression resulted in an empty buffer."));
		RenderTarget->MarkAsGarbage();
		return false;
	}

	// Save the byte array to the specified file path
	const bool bSuccess = FFileHelper::SaveArrayToFile(CompressedPNG, *FilePath);

	// Clean up the temporary render target
	RenderTarget->MarkAsGarbage();

	if (!bSuccess)
	{
		UE_LOG(LogTemp, Error, TEXT("ExportTexture2DToPNG: Failed to save PNG file to disk at path: %s"), *FilePath);
	}

	return bSuccess;
}

TArray<FLinearColor> FTextureUtils::sRGBToLinear(const TArray<FColor>& sRGB_Colors)
{
	TArray<FLinearColor> LinearColors;
	LinearColors.Reserve(sRGB_Colors.Num());
	for (const FColor& sRGBColor : sRGB_Colors)
	{
		// The FLinearColor constructor from FColor correctly converts sRGB to Linear.
		LinearColors.Add(FLinearColor(sRGBColor));
	}
	return LinearColors;
}

TArray<FColor> FTextureUtils::LinearTo_sRGB(const TArray<FLinearColor>& Linear_Colors)
{
	TArray<FColor> sRGB_Colors;
	sRGB_Colors.Reserve(Linear_Colors.Num());
	for (const FLinearColor& LinearColor : Linear_Colors)
	{
		// ToFColor(true) tells the engine to perform the Linear -> sRGB conversion.
		sRGB_Colors.Add(LinearColor.ToFColor(true));
	}
	return sRGB_Colors;
}

// Add this implementation to the file
bool FTextureUtils::DecompressTexture(UTexture2D* InTexture, TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight)
{
	// --- 1. VALIDATION ---
	if (!InTexture || !InTexture->GetResource())
	{
		UE_LOG(LogTemp, Error, TEXT("DecompressTexture: Input texture or its resource was null."));
		return false;
	}

#if WITH_EDITOR
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		UE_LOG(LogTemp, Error, TEXT("DecompressTexture: Could not get a valid editor world context."));
		return false;
	}
#else
	return false;	
#endif

	OutWidth = InTexture->GetSizeX();
	OutHeight = InTexture->GetSizeY();

	if (OutWidth == 0 || OutHeight == 0)
	{
		UE_LOG(LogTemp, Error, TEXT("DecompressTexture: Input texture has zero dimensions."));
		return false;
	}

	// --- 2. PREPARE RENDER TARGET ---
	UTextureRenderTarget2D* TempRT = NewObject<UTextureRenderTarget2D>();
	TempRT->InitCustomFormat(OutWidth, OutHeight, PF_B8G8R8A8, /*bForceLinearGamma=*/false);
	TempRT->UpdateResource();

	// --- 3. DRAW TEXTURE TO RENDER TARGET ---
	UCanvas* Canvas = nullptr;
	FVector2D CanvasSize;
	FDrawToRenderTargetContext Context;
	UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(World, TempRT, Canvas, CanvasSize, Context);
	
	if (Canvas)
	{
		// *** NEWLY ADDED FIX ***
		// Explicitly clear the render target to transparent black before drawing.
		UKismetRenderingLibrary::ClearRenderTarget2D(World, TempRT, FLinearColor::Transparent);

		// Use a tile item to ensure the correct blend mode is used.
		FCanvasTileItem TileItem(
			FVector2D::ZeroVector,
			InTexture->GetResource(),
			CanvasSize,
			FLinearColor::White
		);
		TileItem.BlendMode = SE_BLEND_Translucent; // Preserve source alpha
		Canvas->DrawItem(TileItem);
	}

	UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(World, Context);

	// --- 4. READ PIXELS BACK ---
	FTextureRenderTargetResource* RTResource = TempRT->GameThread_GetRenderTargetResource();
	if (!RTResource)
	{
		UE_LOG(LogTemp, Error, TEXT("DecompressTexture: Failed to get RenderTarget resource."));
		TempRT->MarkAsGarbage();
		return false;
	}

	const bool bReadSuccess = RTResource->ReadPixels(OutPixels);

	// --- 5. CLEANUP & RETURN ---
	TempRT->MarkAsGarbage();
	
	if (!bReadSuccess)
	{
		UE_LOG(LogTemp, Error, TEXT("DecompressTexture: Failed to read pixels from the render target resource."));
		return false;
	}

	return true;
}

// Helper function to decode a color to a [-1, 1] vector
static FVector DecodeNormal(const FColor& Color)
{
    FVector V;
    V.X = (Color.R / 255.0f) * 2.0f - 1.0f;
    V.Y = (Color.G / 255.0f) * 2.0f - 1.0f;
    V.Z = (Color.B / 255.0f) * 2.0f - 1.0f;
    return V.GetSafeNormal();
}

// Helper function to encode a [-1, 1] vector to a color
static FColor EncodeNormal(const FVector& V)
{
    FColor Color;
    Color.R = FMath::Clamp(FMath::RoundToInt(((V.X + 1.0f) / 2.0f) * 255.0f), 0, 255);
    Color.G = FMath::Clamp(FMath::RoundToInt(((V.Y + 1.0f) / 2.0f) * 255.0f), 0, 255);
    Color.B = FMath::Clamp(FMath::RoundToInt(((V.Z + 1.0f) / 2.0f) * 255.0f), 0, 255);
    Color.A = 255;
    return Color;
}


void FTextureUtils::ExtractNormalMapDetails(TArray<FColor>& InOutNormalMapPixels, int32 Width, int32 Height, float BlurRadius)
{
    if (InOutNormalMapPixels.Num() != Width * Height || BlurRadius < 1.0f)
    {
        return;
    }

    const TArray<FColor> OriginalNormals = InOutNormalMapPixels;
    TArray<FColor> BlurredNormals;
    BlurredNormals.SetNum(Width * Height);

    // --- 1. Gaussian Blur to get the "Smooth" base normals ---
    // Pre-calculate the Gaussian kernel
    const int32 KernelSize = FMath::CeilToInt(BlurRadius * 3.0f) * 2 + 1;
    const int32 KernelHalfSize = KernelSize / 2;
    TArray<float> Kernel;
    Kernel.SetNum(KernelSize);
    float KernelSum = 0.0f;
    const float Sigma = BlurRadius;
    for (int32 i = 0; i < KernelSize; ++i)
    {
        float x = i - KernelHalfSize;
        Kernel[i] = FMath::Exp(-(x * x) / (2.0f * Sigma * Sigma));
        KernelSum += Kernel[i];
    }
    for (int32 i = 0; i < KernelSize; ++i)
    {
        Kernel[i] /= KernelSum;
    }

    // Temporary buffer for horizontal pass
    TArray<FColor> TempBuffer;
    TempBuffer.SetNum(Width * Height);

    // Horizontal blur pass
    for (int32 y = 0; y < Height; ++y)
    {
        for (int32 x = 0; x < Width; ++x)
        {
            FVector4f Sum(0, 0, 0, 0);
            for (int32 i = 0; i < KernelSize; ++i)
            {
                int32 SampleX = FMath::Clamp(x - KernelHalfSize + i, 0, Width - 1);
                const FColor& Pixel = OriginalNormals[y * Width + SampleX];
                Sum.X += Pixel.R * Kernel[i];
                Sum.Y += Pixel.G * Kernel[i];
                Sum.Z += Pixel.B * Kernel[i];
                Sum.W += Pixel.A * Kernel[i];
            }
            TempBuffer[y * Width + x] = FColor(FMath::RoundToInt(Sum.X), FMath::RoundToInt(Sum.Y), FMath::RoundToInt(Sum.Z), FMath::RoundToInt(Sum.W));
        }
    }

    // Vertical blur pass
    for (int32 y = 0; y < Height; ++y)
    {
        for (int32 x = 0; x < Width; ++x)
        {
            FVector4f Sum(0, 0, 0, 0);
            for (int32 i = 0; i < KernelSize; ++i)
            {
                int32 SampleY = FMath::Clamp(y - KernelHalfSize + i, 0, Height - 1);
                const FColor& Pixel = TempBuffer[SampleY * Width + x];
                Sum.X += Pixel.R * Kernel[i];
                Sum.Y += Pixel.G * Kernel[i];
                Sum.Z += Pixel.B * Kernel[i];
                Sum.W += Pixel.A * Kernel[i];
            }
            BlurredNormals[y * Width + x] = FColor(FMath::RoundToInt(Sum.X), FMath::RoundToInt(Sum.Y), FMath::RoundToInt(Sum.Z), FMath::RoundToInt(Sum.W));
        }
    }

    // --- 2. Per-Pixel Change of Basis ---
    for (int32 i = 0; i < OriginalNormals.Num(); ++i)
    {
        const FVector N_original = DecodeNormal(OriginalNormals[i]);
        const FVector N_smooth_base = DecodeNormal(BlurredNormals[i]);

        // Create an orthonormal basis from the smooth normal
        FVector UpVector = (FMath::Abs(N_smooth_base.Z) < 0.9f) ? FVector(0, 0, 1) : FVector(0, 1, 0);
        FVector T_base = FVector::CrossProduct(UpVector, N_smooth_base).GetSafeNormal();
        FVector B_base = FVector::CrossProduct(N_smooth_base, T_base).GetSafeNormal();

        // Perform the change of basis (transform N_original into the new TBN space)
        FVector N_detail;
        N_detail.X = FVector::DotProduct(N_original, T_base); // New Tangent   (X)
        N_detail.Y = FVector::DotProduct(N_original, B_base); // New Bitangent (Y)
        N_detail.Z = FVector::DotProduct(N_original, N_smooth_base); // New Normal    (Z)

        // Write the final detail normal to the output buffer
        InOutNormalMapPixels[i] = EncodeNormal(N_detail);
    }
}


