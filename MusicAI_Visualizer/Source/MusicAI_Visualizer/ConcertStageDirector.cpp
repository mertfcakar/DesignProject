#include "ConcertStageDirector.h"
#include "AffectiveAudioActor.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Components/SkeletalMeshComponent.h"
#include "UObject/ConstructorHelpers.h"
#include "DrawDebugHelpers.h"
#include "HAL/IConsoleManager.h"
#include "Engine/PostProcessVolume.h"
#include "NiagaraSystem.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Kismet/GameplayStatics.h"
#include "Components/PoseableMeshComponent.h"
#include "Components/TextRenderComponent.h"

AConcertStageDirector::AConcertStageDirector()
{
    PrimaryActorTick.bCanEverTick = true;
    Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    SetRootComponent(Root);
}

void AConcertStageDirector::BeginPlay()
{
    Super::BeginPlay();

    // Improve volumetric beam quality. Default UE5 settings make the spotlights look
    // pixelated / banded in fog; bumping these CVars makes the beams cinematic.
    if (bHighQualityVolumetric)
    {
        auto SetIntCVar = [](const TCHAR* Name, int32 Val) {
            if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(Name)) {
                CVar->Set(Val, ECVF_SetByConsole);
            }
        };
        auto SetFloatCVar = [](const TCHAR* Name, float Val) {
            if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(Name)) {
                CVar->Set(Val, ECVF_SetByConsole);
            }
        };

        // Volumetric fog grid: lower pixel size + more Z-slices = much smoother beams
        SetIntCVar(TEXT("r.VolumetricFog"),                            1);
        SetIntCVar(TEXT("r.VolumetricFog.GridPixelSize"),              VolumetricFogGridPixelSize);
        SetIntCVar(TEXT("r.VolumetricFog.GridSizeZ"),                  VolumetricFogGridSizeZ);
        SetIntCVar(TEXT("r.VolumetricFog.HistoryMissSupersampleCount"), 8);
        SetFloatCVar(TEXT("r.VolumetricFog.HistoryWeight"),            0.92f);
        // Light shaft quality (godray) and light function quality
        SetIntCVar(TEXT("r.LightShaftQuality"),     1);
        SetIntCVar(TEXT("r.LightFunctionQuality"),  2);
        SetIntCVar(TEXT("r.MaxAnisotropy"),         16);
    }

    BuildStage();

    // Cinematic mode: spawn an unbound post-process volume + push softer source radii.
    // Single biggest "looks expensive" upgrade — bloom on bright lights, vignette
    // pulling focus to the stage, color saturation, and a slightly underexposed scene
    // so the spotlights pop against the darkness.
    if (bCinematicMode && GetWorld())
    {
        APostProcessVolume* PPV = GetWorld()->SpawnActor<APostProcessVolume>(APostProcessVolume::StaticClass());
        if (PPV)
        {
            PPV->bUnbound = true;
            PPV->Priority = 1.0f;
            FPostProcessSettings& S = PPV->Settings;

            // Bloom — only the brightest pixels (spotlight cores) bloom. Higher threshold
            // keeps the beam SHAFTS sharp instead of blurring the whole scene to glow.
            S.bOverride_BloomIntensity = true;     S.BloomIntensity = CinematicBloomIntensity;
            S.bOverride_BloomThreshold = true;     S.BloomThreshold = CinematicBloomThreshold;

            S.bOverride_VignetteIntensity = true;  S.VignetteIntensity = CinematicVignetteIntensity;

            S.bOverride_ColorSaturation = true;
            S.ColorSaturation = FVector4(CinematicSaturation, CinematicSaturation, CinematicSaturation, 1.0f);

            // Auto-exposure stays on, but we CAP its max brightness. Without the cap, in
            // a dark scene the camera over-brightens trying to "see" everything, washing
            // out the contrast between beam-in-fog and surrounding darkness.
            S.bOverride_AutoExposureBias            = true; S.AutoExposureBias            = CinematicExposureBias;
            S.bOverride_AutoExposureMinBrightness   = true; S.AutoExposureMinBrightness   = 0.05f;
            S.bOverride_AutoExposureMaxBrightness   = true; S.AutoExposureMaxBrightness   = CinematicExposureMaxBrightness;

            // Slight contrast lift — darks darker, brights brighter.
            S.bOverride_ColorContrast = true;
            S.ColorContrast = FVector4(1.05f, 1.05f, 1.05f, 1.0f);

            // Lens flare — adds anamorphic streaks on bright sources.
            S.bOverride_LensFlareIntensity = true; S.LensFlareIntensity = 0.6f;

            // Chromatic aberration — subtle "good camera" feel.
            S.bOverride_SceneFringeIntensity = true; S.SceneFringeIntensity = 0.8f;

            // Film grain — tiny noise so flat color regions don't look CG-flat.
            S.bOverride_FilmGrainIntensity = true; S.FilmGrainIntensity = 0.18f;
        }
    }

    if (bSpawnVenue)    BuildVenue();    // FIRST — venue contains everything else
    if (bSpawnAudience) BuildAudience();
    if (bSpawnTables)   BuildTables();
    if (bSpawnBar)      BuildBar();
    if (bSpawnHouseLights) BuildHouseLights();
    if (bSpawnDiscoBall) BuildDiscoBall();
    if (bSpawnDanceFloorLights) BuildDanceFloorLights();
    if (bSpawnStageStructure) BuildStageStructure();
    if (bSpawnLedPanels)      BuildLedPanels();
    if (bSpawnSpectrumAnalyzer) BuildSpectrumAnalyzer();
    if (bSpawnHaze)           BuildHaze();
    InstrumentVisibilityTimers.Init(0.0f, InstrumentRoots.Num());
    // Hide all instruments initially. They appear when their instrument is detected.
    for (USceneComponent* R : InstrumentRoots) {
        if (R) R->SetVisibility(false, true);
    }

    // Cinematic mode: bigger source radius + boosted volumetric scattering on every
    // spotlight. The volumetric boost is the key to "seeing the beam from ceiling to floor"
    // — it's what makes the shaft of light visible in mid-air through fog.
    if (bCinematicMode)
    {
        const float R = CinematicSourceRadius;
        const float SoftR = R * 1.5f;
        const float V = CinematicVolumetricBoost;
        for (USpotLightComponent* L : WashLights) {
            if (!L) continue;
            L->SetSourceRadius(R); L->SetSoftSourceRadius(SoftR);
            L->SetVolumetricScatteringIntensity(V);
        }
        for (USpotLightComponent* L : BeamLights) {
            if (!L) continue;
            L->SetSourceRadius(R * 0.5f); L->SetSoftSourceRadius(SoftR * 0.5f);
            L->SetVolumetricScatteringIntensity(V * 1.4f);   // beams strongest — narrow shafts
        }
        for (USpotLightComponent* L : SideLights) {
            if (!L) continue;
            L->SetSourceRadius(R); L->SetSoftSourceRadius(SoftR);
            L->SetVolumetricScatteringIntensity(V * 0.8f);
        }
        for (USpotLightComponent* L : AudienceLights) {
            if (!L) continue;
            L->SetSourceRadius(R); L->SetSoftSourceRadius(SoftR);
            L->SetVolumetricScatteringIntensity(V * 0.9f);
        }
        for (USpotLightComponent* L : HouseLights) {
            if (!L) continue;
            L->SetSourceRadius(R * 1.3f); L->SetSoftSourceRadius(SoftR * 1.3f);
            L->SetVolumetricScatteringIntensity(V * 0.3f);   // house lights subtle, not beams
        }
        for (USpotLightComponent* L : DiscoBallBeams) {
            if (!L) continue;
            L->SetSourceRadius(R * 0.4f); L->SetSoftSourceRadius(SoftR * 0.4f);
            L->SetVolumetricScatteringIntensity(V * 1.2f);
        }
        for (UPointLightComponent* L : FloorLights) {
            if (!L) continue;
            L->SetSourceRadius(R); L->SetSoftSourceRadius(SoftR);
            L->SetVolumetricScatteringIntensity(V * 0.6f);
        }
        for (UPointLightComponent* L : DanceFloorLights) {
            if (!L) continue;
            L->SetSourceRadius(R * 0.6f); L->SetSoftSourceRadius(SoftR * 0.6f);
            L->SetVolumetricScatteringIntensity(V * 0.5f);
        }
    }
}

void AConcertStageDirector::BuildTables()
{
    // Pick the table mesh: editor override → cube fallback (you can later replace with a real bar table mesh)
    UStaticMesh* TableMesh = TableMeshOverride;
    if (!TableMesh) TableMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (!TableMesh) return;

    // Same mesh-selection logic as the main audience: prefer pool, then override, then capsule fallback.
    const bool bHavePool = AudienceMeshPool.Num() > 0;
    const bool bUsingCustomMesh = bHavePool || (AudienceMeshOverride != nullptr);
    UStaticMesh* FallbackMesh = AudienceMeshOverride;
    if (!FallbackMesh) FallbackMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Capsule.Capsule"));
    if (!FallbackMesh) FallbackMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
    UStaticMesh* HeadMesh = bUsingCustomMesh ? nullptr
        : LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));

    auto PickBody = [&]() -> UStaticMesh* {
        if (bHavePool) {
            for (int32 a = 0; a < 8; a++) {
                const int32 idx = FMath::RandRange(0, AudienceMeshPool.Num() - 1);
                if (AudienceMeshPool[idx]) return AudienceMeshPool[idx].Get();
            }
        }
        return FallbackMesh;
    };

    // Chair mesh: editor override → cube fallback for placeholder stools
    UStaticMesh* ChairMesh = ChairMeshOverride;
    if (!ChairMesh && bSpawnChairs) ChairMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));

    // Place tables in two parallel columns flanking the dance floor (left / right of room center).
    // X is fixed at +/- TableSideX so the dance floor (DanceFloorWidth) stays clear in the middle.
    // Y spans from just behind the audience-front row to slightly past the audience-back row,
    // so the tables form a "VIP lounge" along both side walls. NO tables near the stage.
    const int32 PerSide       = FMath::Max(1, NumTables / 2);
    const float AudFrontY     = -StageDepth * 0.5f - AudienceStartDepth;
    const float AudBackY      = AudFrontY - (AudienceRows - 1) * AudienceRowSpacing;
    const float TableNearY    = AudFrontY - 200.f;            // 2 m back from first dance row
    const float TableFarY     = AudBackY  - 600.f;            // 6 m past last dance row
    for (int32 i = 0; i < NumTables; i++) {
        const float Side  = (i < PerSide) ? -1.0f : 1.0f;     // first half left, second half right
        const int32 IdxOnSide = i % PerSide;
        const float TY    = (PerSide > 1) ? ((float)IdxOnSide / (PerSide - 1)) : 0.5f;
        const float X     = Side * TableSideX;
        const float Y     = FMath::Lerp(TableNearY, TableFarY, TY);

        // The table itself: a cube scaled to look like a high-top cocktail table
        UStaticMeshComponent* Table = NewObject<UStaticMeshComponent>(this);
        Table->SetupAttachment(Root);
        Table->RegisterComponent();
        Table->SetStaticMesh(TableMesh);
        Table->SetRelativeLocation(FVector(X, Y, 50.f));
        // High table proportions: ~60cm wide × 60cm deep × ~1.1m tall (cocktail/bar height)
        Table->SetRelativeScale3D(FVector(0.6f, 0.6f, 1.1f));
        Table->SetCastShadow(false);
        TableMeshes.Add(Table);

        // Spawn a small group of people standing around the table.
        // They use the same audience animation system but with reduced jumpiness
        // (they're chatting at the table, not raving on the dance floor).
        const int32 N = FMath::Max(1, PeoplePerTable);
        for (int32 p = 0; p < N; p++) {
            const float Angle = (2.0f * (float)PI * p) / N + FMath::FRandRange(-0.25f, 0.25f);
            const float PX = X + FMath::Cos(Angle) * TableRadius;
            const float PY = Y + FMath::Sin(Angle) * TableRadius;

            UStaticMeshComponent* Person = NewObject<UStaticMeshComponent>(this);
            Person->SetupAttachment(Root);
            Person->RegisterComponent();
            Person->SetStaticMesh(PickBody());

            const float HeightVar = FMath::FRandRange(0.92f, 1.10f);
            const float BaseZ = bUsingCustomMesh ? 0.f : 90.f;
            const FVector BasePos(PX, PY, BaseZ);
            Person->SetRelativeLocation(BasePos);

            // Face inward toward the table
            const float FacingYaw = FMath::RadiansToDegrees(Angle) + 180.0f
                                  + FMath::FRandRange(-15.f, 15.f);
            if (bUsingCustomMesh) {
                const float S = AudienceScaleMultiplier;
                Person->SetRelativeScale3D(FVector(S, S, S * HeightVar));
                Person->SetRelativeRotation(FRotator(0.f, FacingYaw, 0.f));
            } else {
                Person->SetRelativeScale3D(FVector(0.45f, 0.45f, 1.6f * HeightVar));
                Person->SetRelativeRotation(FRotator(0.f, FacingYaw, 0.f));
            }
            Person->SetCastShadow(false);

            // Optional bar stool RIGHT NEXT to the person (not behind).
            // Sized as a tall bar stool so the standing figure looks like they are
            // leaning on it. We cannot make scanned static meshes sit, so we hide
            // the bottom of the figure behind the stool by placing it slightly in front.
            if (bSpawnChairs && ChairMesh) {
                UStaticMeshComponent* Chair = NewObject<UStaticMeshComponent>(this);
                Chair->SetupAttachment(Root);
                Chair->RegisterComponent();
                Chair->SetStaticMesh(ChairMesh);
                // Place stool just to the side of the person (offset to their right)
                const float SideAngle = Angle + (float)PI * 0.5f;
                const float CX = PX + FMath::Cos(SideAngle) * 50.f;
                const float CY = PY + FMath::Sin(SideAngle) * 50.f;
                Chair->SetRelativeLocation(FVector(CX, CY, 50.f));
                // Bar stool proportions: small seat, ~1m tall
                Chair->SetRelativeScale3D(FVector(0.35f, 0.35f, 1.0f));
                Chair->SetRelativeRotation(FRotator(0.f, FacingYaw + 180.f, 0.f));
                Chair->SetCastShadow(false);
            }

            // Feed into the existing audience animation arrays.
            // Lower jumpiness = chill people at the table, not wild dancers.
            AudienceMeshes.Add(Person);
            AudienceBasePositions.Add(BasePos);
            AudiencePhases.Add(FMath::FRandRange(0.0f, 2.0f * (float)PI));
            AudienceJumpiness.Add(FMath::FRandRange(0.15f, 0.45f));

            // Add a head only when using placeholder shapes
            if (HeadMesh) {
                UStaticMeshComponent* H = NewObject<UStaticMeshComponent>(this);
                H->SetupAttachment(Person);
                H->RegisterComponent();
                H->SetStaticMesh(HeadMesh);
                H->SetRelativeLocation(FVector(0.f, 0.f, 60.f));
                H->SetRelativeScale3D(FVector(0.55f, 0.55f, 0.18f));
                H->SetCastShadow(false);
            }
        }
    }
}

void AConcertStageDirector::BuildBar()
{
    // The bar sits behind the audience block (opposite the stage), full-width along the back.
    // Uses cube fallback meshes if no override is supplied — replace with real meshes in the editor.
    UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (!CubeMesh) return;

    UStaticMesh* CounterMesh = BarCounterMeshOverride ? BarCounterMeshOverride.Get() : CubeMesh;
    UStaticMesh* StoolMesh   = BarStoolMeshOverride   ? BarStoolMeshOverride.Get()   : CubeMesh;

    // Anchor: behind the audience block by BarBehindAudience cm.
    const float AudBackY = -StageDepth * 0.5f - AudienceStartDepth - (AudienceRows - 1) * AudienceRowSpacing;
    const float BarY     = AudBackY - BarBehindAudience;

    // 1) Counter: long cube, ~80 cm deep, BarCounterHeight tall, BarLength wide.
    UStaticMeshComponent* Counter = NewObject<UStaticMeshComponent>(this);
    Counter->SetupAttachment(Root);
    Counter->RegisterComponent();
    Counter->SetStaticMesh(CounterMesh);
    Counter->SetRelativeLocation(FVector(0.f, BarY, BarCounterHeight * 0.5f));
    // Cube default 100 cm cube -> scale (BarLength/100, 80/100, Height/100)
    Counter->SetRelativeScale3D(FVector(BarLength / 100.f, 0.8f, BarCounterHeight / 100.f));
    Counter->SetCastShadow(false);
    BarMeshes.Add(Counter);

    // 2) Backsplash / bottle shelf: another cube right behind the counter, taller and thinner.
    UStaticMeshComponent* Backsplash = NewObject<UStaticMeshComponent>(this);
    Backsplash->SetupAttachment(Root);
    Backsplash->RegisterComponent();
    Backsplash->SetStaticMesh(CounterMesh);
    Backsplash->SetRelativeLocation(FVector(0.f, BarY - 80.f, (BarCounterHeight + 200.f) * 0.5f));
    Backsplash->SetRelativeScale3D(FVector((BarLength * 0.95f) / 100.f, 0.4f, (BarCounterHeight + 200.f) / 100.f));
    Backsplash->SetCastShadow(false);
    BarMeshes.Add(Backsplash);

    // 3) Stools in front of the counter, evenly spaced.
    const float StoolY = BarY + 120.f;            // 1.2 m in front of counter
    const float StoolZ = 50.f;                     // ~1 m tall stool centered at Z=50
    for (int32 i = 0; i < NumBarStools; i++) {
        const float T = (NumBarStools > 1) ? ((float)i / (NumBarStools - 1)) : 0.5f;
        const float X = FMath::Lerp(-BarLength * 0.45f, BarLength * 0.45f, T);

        UStaticMeshComponent* Stool = NewObject<UStaticMeshComponent>(this);
        Stool->SetupAttachment(Root);
        Stool->RegisterComponent();
        Stool->SetStaticMesh(StoolMesh);
        Stool->SetRelativeLocation(FVector(X, StoolY, StoolZ));
        Stool->SetRelativeScale3D(FVector(0.4f, 0.4f, 1.0f));   // ~40 x 40 x 100 cm
        Stool->SetCastShadow(false);
        BarMeshes.Add(Stool);
    }

    // 4) Cyan accent uplights under the counter lip — cheap "neon glow" without emissive material.
    const int32 NumAccent = FMath::Max(3, NumBarStools / 2);
    for (int32 i = 0; i < NumAccent; i++) {
        const float T = (NumAccent > 1) ? ((float)i / (NumAccent - 1)) : 0.5f;
        const float X = FMath::Lerp(-BarLength * 0.45f, BarLength * 0.45f, T);

        UPointLightComponent* L = NewObject<UPointLightComponent>(this);
        L->SetupAttachment(Root);
        L->RegisterComponent();
        L->SetRelativeLocation(FVector(X, BarY + 40.f, BarCounterHeight + 50.f));
        L->SetIntensityUnits(ELightUnits::Lumens);
        L->SetIntensity(BarAccentLightIntensity);
        L->SetAttenuationRadius(450.f);
        L->SetCastShadows(false);
        L->SetVolumetricScatteringIntensity(1.5f);
        L->SetLightColor(FLinearColor(0.20f, 0.85f, 1.0f));    // bar cyan
        BarLights.Add(L);
    }
}

// Physical stage geometry: truss towers, roof beams, speaker stacks, monitors, backdrop, platform.
// All built from the engine cube mesh so no external assets needed — placeholder but reads as
// real concert structure. Each piece tinted via a Dynamic Material Instance so the editor
// color properties (TrussTint / SpeakerTint / BackdropTint / PlatformTint) take effect live.
void AConcertStageDirector::BuildStageStructure()
{
    UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (!CubeMesh) {
        UE_LOG(LogTemp, Warning, TEXT("ConcertStageDirector: BuildStageStructure failed - cube mesh missing"));
        return;
    }
    UMaterialInterface* BaseMat = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineMaterials/WorldGridMaterial"));

    // Helper: spawn a tinted cube at (pos, rot, scale). Scale is in METERS (so scale 5.0 = 5m).
    // Engine cube is 100 cm on a side, so the conversion factor cancels with our cm-based world.
    // We pass scale in absolute world meters here for clarity; convert by dividing by 1.
    auto SpawnBox = [&](FVector PosLocal, FVector ScaleMeters, FRotator Rot, FLinearColor Tint) {
        UStaticMeshComponent* M = NewObject<UStaticMeshComponent>(this);
        M->SetupAttachment(Root);
        M->RegisterComponent();
        M->SetStaticMesh(CubeMesh);
        M->SetRelativeLocation(PosLocal);
        M->SetRelativeRotation(Rot);
        // Engine cube has 100cm dimensions; we want scale in CM-units (1.0 = 100 cm), so scale x100.
        M->SetRelativeScale3D(FVector(ScaleMeters.X, ScaleMeters.Y, ScaleMeters.Z));
        M->SetCastShadow(false);
        if (BaseMat) {
            UMaterialInstanceDynamic* MID = M->CreateAndSetMaterialInstanceDynamic(0);
            if (MID) {
                // WorldGridMaterial uses GridColor parameter when present; fall back to a vector param.
                MID->SetVectorParameterValue(TEXT("Color"), Tint);
                MID->SetVectorParameterValue(TEXT("GridColor"), Tint);
            }
        }
        return M;
    };

    // ----- Stage platform (raised slab the band stands on) -----
    if (bSpawnStagePlatform) {
        const float HalfH = StagePlatformHeight * 0.5f;
        UStaticMeshComponent* Platform = SpawnBox(
            FVector(0.f, 0.f, HalfH),
            FVector(StageWidth / 100.f, StageDepth / 100.f, StagePlatformHeight / 100.f),
            FRotator::ZeroRotator,
            PlatformTint);
        // Set glossy/metallic params if base material supports them (BasicShapeMaterial / WorldGridMaterial do).
        if (bReflectiveStageFloor && Platform) {
            if (UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(Platform->GetMaterial(0))) {
                MID->SetScalarParameterValue(TEXT("Roughness"), StageFloorRoughness);
                MID->SetScalarParameterValue(TEXT("Metallic"),  StageFloorMetallic);
            }
        }
    }

    // ----- 4 truss TOWERS (vertical metal columns at stage corners) -----
    const float TT = TrussThickness;
    const float TallScale = TrussHeight / 100.f;
    const float ThinScale = TT / 100.f;

    const FVector CornerOffsets[4] = {
        FVector(-StageWidth * 0.5f, -StageDepth * 0.5f, 0.f),  // front-left
        FVector( StageWidth * 0.5f, -StageDepth * 0.5f, 0.f),  // front-right
        FVector(-StageWidth * 0.5f,  StageDepth * 0.5f, 0.f),  // back-left
        FVector( StageWidth * 0.5f,  StageDepth * 0.5f, 0.f),  // back-right
    };
    for (int32 k = 0; k < 4; k++) {
        SpawnBox(
            CornerOffsets[k] + FVector(0.f, 0.f, TrussHeight * 0.5f),
            FVector(ThinScale, ThinScale, TallScale),
            FRotator::ZeroRotator,
            TrussTint);
    }

    // ----- Horizontal TOP BEAMS forming the truss rectangle -----
    // Front beam (along X axis at the front edge)
    SpawnBox(FVector(0.f, -StageDepth * 0.5f, TrussHeight),
             FVector(StageWidth / 100.f, ThinScale, ThinScale), FRotator::ZeroRotator, TrussTint);
    // Back beam
    SpawnBox(FVector(0.f, StageDepth * 0.5f, TrussHeight),
             FVector(StageWidth / 100.f, ThinScale, ThinScale), FRotator::ZeroRotator, TrussTint);
    // Left beam
    SpawnBox(FVector(-StageWidth * 0.5f, 0.f, TrussHeight),
             FVector(ThinScale, StageDepth / 100.f, ThinScale), FRotator::ZeroRotator, TrussTint);
    // Right beam
    SpawnBox(FVector(StageWidth * 0.5f, 0.f, TrussHeight),
             FVector(ThinScale, StageDepth / 100.f, ThinScale), FRotator::ZeroRotator, TrussTint);

    // ----- Cross-beams across the top (the lights "hang" from these) -----
    const int32 NCross = FMath::Max(1, NumTrussCrossBeams);
    for (int32 i = 0; i < NCross; i++) {
        const float t = (NCross > 1) ? ((float)(i + 1) / (float)(NCross + 1)) : 0.5f;
        const float Y = FMath::Lerp(-StageDepth * 0.5f, StageDepth * 0.5f, t);
        SpawnBox(FVector(0.f, Y, TrussHeight),
                 FVector(StageWidth / 100.f, ThinScale, ThinScale), FRotator::ZeroRotator, TrussTint);
    }

    // ----- BACKDROP banner between the back truss towers -----
    {
        const float BackdropZ = TrussHeight * 0.55f;
        const float BackdropH = TrussHeight * 0.85f;
        SpawnBox(
            FVector(0.f, StageDepth * 0.5f - 20.f, BackdropZ * 0.5f + 50.f),
            FVector((StageWidth - TT * 2.f) / 100.f, 0.3f, BackdropH / 100.f),
            FRotator::ZeroRotator,
            BackdropTint);
    }

    // ----- PA SPEAKER STACKS (left + right of stage, outside the truss towers) -----
    if (NumSpeakerStacksPerSide > 0)
    {
        // Each speaker box ~120 cm × 80 cm × 150 cm. Stack 2-high per column.
        const float SpkX = 1.2f, SpkY = 0.8f, SpkZ = 1.5f;        // meters
        const float BoxCM_H = SpkZ * 100.f;
        const float StackOffset = StageWidth * 0.5f + 250.f;       // 2.5m outside the truss

        for (int32 s = 0; s < 2; s++) {
            const float Side = (s == 0) ? -1.0f : 1.0f;
            for (int32 col = 0; col < NumSpeakerStacksPerSide; col++) {
                const float ColOffset = col * (SpkX * 100.f + 30.f);    // 30cm gap
                const float X = Side * (StackOffset + ColOffset);
                for (int32 row = 0; row < 2; row++) {                    // 2 high
                    const float Z = BoxCM_H * 0.5f + row * BoxCM_H;
                    UStaticMeshComponent* SpkMesh = SpawnBox(
                        FVector(X, -StageDepth * 0.2f, Z),
                        FVector(SpkX, SpkY, SpkZ),
                        FRotator::ZeroRotator,
                        SpeakerTint);
                    // Track so we can vibration-animate this mesh's Y scale on bass hits.
                    if (SpkMesh) {
                        SpeakerMeshes.Add(SpkMesh);
                        SpeakerBaseScales.Add(SpkMesh->GetRelativeScale3D());
                    }
                }
            }
        }
    }

    // ----- SIDE LED BARS (vertical emissive strips up each truss tower) -----
    if (bSpawnSideLedBars && NumSideLedBarsPerTower > 0)
    {
        const float BarHeight = TrussHeight * 0.85f;
        const float BarThickness = 25.f;   // 25cm wide LED strip
        const float BarZ = BarHeight * 0.5f + TT * 0.5f;

        for (int32 s = 0; s < 2; s++) {
            const float SideX = (s == 0) ? -1.0f : 1.0f;
            const float CornerX = SideX * (StageWidth * 0.5f);
            for (int32 k = 0; k < NumSideLedBarsPerTower; k++) {
                // Multiple bars across the tower depth — one front, one back, etc.
                const float DepthT = (NumSideLedBarsPerTower > 1) ? ((float)k / (float)(NumSideLedBarsPerTower - 1)) : 0.5f;
                const float Y = FMath::Lerp(-StageDepth * 0.5f + TT * 0.5f, StageDepth * 0.5f - TT * 0.5f, DepthT);

                UStaticMeshComponent* M = NewObject<UStaticMeshComponent>(this);
                M->SetupAttachment(Root);
                M->RegisterComponent();
                M->SetStaticMesh(CubeMesh);
                // Position SLIGHTLY inside the truss tower so it's flush against the inner face
                M->SetRelativeLocation(FVector(CornerX - SideX * TT * 0.4f, Y, BarZ));
                M->SetRelativeScale3D(FVector(BarThickness / 100.f, BarThickness / 100.f, BarHeight / 100.f));
                M->SetCastShadow(false);
                if (BaseMat) {
                    UMaterialInstanceDynamic* MID = M->CreateAndSetMaterialInstanceDynamic(0);
                    if (MID) {
                        MID->SetVectorParameterValue(TEXT("Color"), FLinearColor::Black);
                        MID->SetVectorParameterValue(TEXT("Emissive"), FLinearColor::Black);
                    }
                }
                SideLedBars.Add(M);
            }
        }
    }

    // ----- STAGE MONITORS (wedge-shaped speakers at front edge of platform, tilted toward band) -----
    if (NumStageMonitors > 0)
    {
        const int32 N = NumStageMonitors;
        const float MonZ = (bSpawnStagePlatform ? StagePlatformHeight : 0.f) + 20.f;
        for (int32 i = 0; i < N; i++) {
            const float t = (N > 1) ? ((float)i / (float)(N - 1)) : 0.5f;
            const float X = FMath::Lerp(-StageWidth * 0.4f, StageWidth * 0.4f, t);
            SpawnBox(
                FVector(X, -StageDepth * 0.45f, MonZ),
                FVector(0.7f, 0.5f, 0.4f),
                FRotator(20.f, 0.f, 0.f),    // pitch up so it faces the singer
                SpeakerTint);
        }
    }
}

// Procedurally build the venue: floor, ceiling, and 4 walls. Geometry sits in this actor's
// LOCAL space so the room moves with the ConcertStageDirector. Delete any pre-existing
// manually-placed wall/floor cubes in the level before enabling bSpawnVenue, otherwise
// they'll overlap with the procedural ones.
void AConcertStageDirector::BuildVenue()
{
    UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (!CubeMesh) {
        UE_LOG(LogTemp, Warning, TEXT("ConcertStageDirector: BuildVenue failed - cube mesh missing"));
        return;
    }
    UMaterialInterface* DefaultMat = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));

    // Helper that spawns one cube wall/floor/ceiling. If MatOverride is set, use it directly
    // (no MID). Otherwise create a Material Instance Dynamic from DefaultMat and tint it.
    auto SpawnSlab = [&](FVector PosLocal, FVector ScaleVec, UMaterialInterface* MatOverride, FLinearColor Tint) {
        UStaticMeshComponent* M = NewObject<UStaticMeshComponent>(this);
        M->SetupAttachment(Root);
        M->RegisterComponent();
        M->SetStaticMesh(CubeMesh);
        M->SetRelativeLocation(PosLocal);
        M->SetRelativeScale3D(ScaleVec);
        M->SetCastShadow(true);    // walls SHOULD cast shadows so lights bouncing off them feel right
        if (MatOverride) {
            M->SetMaterial(0, MatOverride);
        } else if (DefaultMat) {
            UMaterialInstanceDynamic* MID = M->CreateAndSetMaterialInstanceDynamic(0);
            if (MID) {
                MID->SetVectorParameterValue(TEXT("Color"),     Tint);
                MID->SetVectorParameterValue(TEXT("BaseColor"), Tint);
                MID->SetVectorParameterValue(TEXT("GridColor"), Tint);
            }
        }
        return M;
    };

    // Engine cube is 100 cm on a side; scale 1.0 = 100 cm. Convert metric -> scale by /100.
    const float W       = VenueWidth;
    const float D       = VenueDepth;
    const float H       = VenueHeight;
    const float YBack   = VenueBackOffset;            // back wall local Y
    const float YFront  = VenueBackOffset - D;        // front wall local Y
    const float YCenter = VenueBackOffset - D * 0.5f; // floor/ceiling local Y

    // Thin slabs (2 cm) for walls so they're paper-thin from outside but solid from inside.
    const float Thin = 2.f;

    // ----- Floor -----
    SpawnSlab(
        FVector(0.f, YCenter, -Thin * 0.5f),               // top of floor at Z=0
        FVector(W / 100.f, D / 100.f, Thin / 100.f),
        VenueFloorMaterialOverride,
        VenueFloorTint);

    // ----- Ceiling -----
    SpawnSlab(
        FVector(0.f, YCenter, H + Thin * 0.5f),
        FVector(W / 100.f, D / 100.f, Thin / 100.f),
        VenueCeilingMaterialOverride,
        VenueCeilingTint);

    // ----- Back wall -----
    SpawnSlab(
        FVector(0.f, YBack + Thin * 0.5f, H * 0.5f),
        FVector(W / 100.f, Thin / 100.f, H / 100.f),
        VenueWallMaterialOverride,
        VenueWallTint);

    // ----- Front wall -----
    SpawnSlab(
        FVector(0.f, YFront - Thin * 0.5f, H * 0.5f),
        FVector(W / 100.f, Thin / 100.f, H / 100.f),
        VenueWallMaterialOverride,
        VenueWallTint);

    // ----- Left wall -----
    SpawnSlab(
        FVector(-W * 0.5f - Thin * 0.5f, YCenter, H * 0.5f),
        FVector(Thin / 100.f, D / 100.f, H / 100.f),
        VenueWallMaterialOverride,
        VenueWallTint);

    // ----- Right wall -----
    SpawnSlab(
        FVector( W * 0.5f + Thin * 0.5f, YCenter, H * 0.5f),
        FVector(Thin / 100.f, D / 100.f, H / 100.f),
        VenueWallMaterialOverride,
        VenueWallTint);

    UE_LOG(LogTemp, Warning, TEXT("ConcertStageDirector: BuildVenue spawned %dm x %dm x %dm venue (back at local Y=%d, front at Y=%d)"),
        (int32)(W/100.f), (int32)(D/100.f), (int32)(H/100.f), (int32)YBack, (int32)YFront);
}

// LED video panel backdrop: N emissive plane meshes behind the band.
// Each panel holds a Dynamic Material Instance whose emissive color is updated every frame
// in Tick to pulse with bass + cycle through the scene palette. Different panels get
// slightly different hues so the wall looks like a real LED video setup, not a single block.
void AConcertStageDirector::BuildLedPanels()
{
    UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (!CubeMesh) return;
    UMaterialInterface* BaseMat = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));

    const int32 N = FMath::Max(1, NumLedPanels);
    const float DeckZ = (bSpawnStageStructure && bSpawnStagePlatform) ? StagePlatformHeight : 0.f;
    const float PanelCenterZ = DeckZ + LedPanelHeightFromFloor + LedPanelHeight * 0.5f;
    const float PanelDepthY  = StageDepth * 0.5f - 60.f;     // sit ~60cm in front of back truss

    // Total width of the array including gaps; lay them out side-by-side centered on X=0.
    const float TotalWidth = N * LedPanelWidth + (N - 1) * LedPanelGap;
    const float StartX = -TotalWidth * 0.5f + LedPanelWidth * 0.5f;

    for (int32 i = 0; i < N; i++) {
        UStaticMeshComponent* M = NewObject<UStaticMeshComponent>(this);
        M->SetupAttachment(Root);
        M->RegisterComponent();
        M->SetStaticMesh(CubeMesh);
        const float X = StartX + i * (LedPanelWidth + LedPanelGap);
        M->SetRelativeLocation(FVector(X, PanelDepthY, PanelCenterZ));
        // Thin Y axis (panel-thin), wide X axis, tall Z axis.
        M->SetRelativeScale3D(FVector(LedPanelWidth / 100.f, 0.15f, LedPanelHeight / 100.f));
        M->SetCastShadow(false);
        if (BaseMat) {
            UMaterialInstanceDynamic* MID = M->CreateAndSetMaterialInstanceDynamic(0);
            if (MID) {
                MID->SetVectorParameterValue(TEXT("Color"),    FLinearColor::Black);
                MID->SetVectorParameterValue(TEXT("Emissive"), FLinearColor::Black);
            }
        }
        LedPanels.Add(M);
    }

    UE_LOG(LogTemp, Warning, TEXT("ConcertStageDirector: Spawned %d LED panels behind the band."), N);

    // ----- Genre text -----
    // Floating 3D text in front of the center LED panel, showing detected genre in caps.
    if (bShowGenreText) {
        GenreTextRender = NewObject<UTextRenderComponent>(this);
        GenreTextRender->SetupAttachment(Root);
        GenreTextRender->RegisterComponent();
        GenreTextRender->SetRelativeLocation(FVector(0.f, PanelDepthY - 50.f, PanelCenterZ));
        GenreTextRender->SetRelativeRotation(FRotator(0.f, 90.f, 0.f));   // face audience (-Y)
        GenreTextRender->SetTextRenderColor(FColor::White);
        GenreTextRender->SetWorldSize(GenreTextSize);
        GenreTextRender->SetText(FText::FromString(TEXT("")));
        GenreTextRender->SetHorizontalAlignment(EHTA_Center);
        GenreTextRender->SetVerticalAlignment(EVRTA_TextCenter);
        GenreTextRender->SetCastShadow(false);
    }

    // ----- DJ BOOTH -----
    // Front-center booth for electronic-genre sets. Body box + emissive front panel.
    // Hidden by default; Tick toggles visibility based on detected genre family.
    if (bSpawnDJBooth)
    {
        // (Reuses the outer DeckZ computed at the top of BuildLedPanels.)
        const FVector BoothPos(0.f, -StageDepth * 0.32f, DeckZ + DJBoothHeight * 0.5f);
        // Body
        DJBoothBody = NewObject<UStaticMeshComponent>(this);
        DJBoothBody->SetupAttachment(Root);
        DJBoothBody->RegisterComponent();
        DJBoothBody->SetStaticMesh(CubeMesh);
        DJBoothBody->SetRelativeLocation(BoothPos);
        DJBoothBody->SetRelativeScale3D(FVector(DJBoothWidth/100.f, DJBoothDepth/100.f, DJBoothHeight/100.f));
        DJBoothBody->SetCastShadow(false);
        DJBoothBody->SetVisibility(false);
        if (BaseMat) {
            UMaterialInstanceDynamic* MID = DJBoothBody->CreateAndSetMaterialInstanceDynamic(0);
            if (MID) MID->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.05f, 0.05f, 0.06f));
        }

        // Emissive front panel (the LED face of the booth)
        DJBoothPanel = NewObject<UStaticMeshComponent>(this);
        DJBoothPanel->SetupAttachment(Root);
        DJBoothPanel->RegisterComponent();
        DJBoothPanel->SetStaticMesh(CubeMesh);
        DJBoothPanel->SetRelativeLocation(BoothPos + FVector(0.f, -DJBoothDepth * 0.5f - 5.f, 0.f));
        DJBoothPanel->SetRelativeScale3D(FVector(DJBoothWidth/100.f * 0.95f, 0.1f, DJBoothHeight/100.f * 0.75f));
        DJBoothPanel->SetCastShadow(false);
        DJBoothPanel->SetVisibility(false);
        if (BaseMat) {
            UMaterialInstanceDynamic* MID = DJBoothPanel->CreateAndSetMaterialInstanceDynamic(0);
            if (MID) {
                MID->SetVectorParameterValue(TEXT("Color"), FLinearColor::Black);
                MID->SetVectorParameterValue(TEXT("Emissive"), FLinearColor::Black);
            }
        }
    }
}

// 3D spectrum analyzer: a row of vertical bars that grow/shrink with audio amplitude.
// Bars 0..N/3 represent BASS, N/3..2N/3 represent MIDS, 2N/3..N represent TREBLE.
// Each bar has a per-position phase so neighbors don't move in perfect sync — looks
// like an actual FFT spectrum even though we're synthesizing it from 3 bands.
void AConcertStageDirector::BuildSpectrumAnalyzer()
{
    UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (!CubeMesh) return;
    UMaterialInterface* BaseMat = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));

    const int32 N = FMath::Max(8, NumSpectrumBars);
    SpectrumBarCurrentHeights.Init(0.f, N);

    const float DeckZ = (bSpawnStageStructure && bSpawnStagePlatform) ? StagePlatformHeight : 0.f;
    const float BarY  = StageDepth * 0.5f - 200.f;     // 2m in front of back truss, behind LED panels
    const float UnitWidth = SpectrumBarWidth + SpectrumBarGap;
    const float TotalWidth = N * UnitWidth - SpectrumBarGap;
    const float StartX = -TotalWidth * 0.5f + SpectrumBarWidth * 0.5f;

    for (int32 i = 0; i < N; i++) {
        UStaticMeshComponent* M = NewObject<UStaticMeshComponent>(this);
        M->SetupAttachment(Root);
        M->RegisterComponent();
        M->SetStaticMesh(CubeMesh);
        const float X = StartX + i * UnitWidth;
        // Start collapsed at the floor; Tick will animate.
        M->SetRelativeLocation(FVector(X, BarY, DeckZ + 25.f));
        M->SetRelativeScale3D(FVector(SpectrumBarWidth / 100.f, SpectrumBarWidth / 100.f, 0.5f));
        M->SetCastShadow(false);
        if (BaseMat) {
            UMaterialInstanceDynamic* MID = M->CreateAndSetMaterialInstanceDynamic(0);
            if (MID) {
                MID->SetVectorParameterValue(TEXT("Color"),    FLinearColor::Black);
                MID->SetVectorParameterValue(TEXT("Emissive"), FLinearColor::Black);
            }
        }
        SpectrumBars.Add(M);
    }

    UE_LOG(LogTemp, Warning, TEXT("ConcertStageDirector: Spawned %d spectrum bars."), N);
}

// Stage haze: scattered point lights with very high volumetric scattering and small
// intensity. In the volumetric fog they show up as soft glowing blobs that drift
// slowly — looks like real stage smoke / haze pockets catching the lights.
void AConcertStageDirector::BuildHaze()
{
    const int32 N = FMath::Max(0, NumHazePuffs);
    if (N == 0) return;

    HazePuffBasePositions.SetNum(N);
    HazePuffs.Empty(N);

    // Distribute puffs across an X×Y grid covering stage + front audience area,
    // at varying heights to create depth.
    for (int32 i = 0; i < N; i++) {
        UPointLightComponent* L = NewObject<UPointLightComponent>(this);
        L->SetupAttachment(Root);
        L->RegisterComponent();

        // Pseudo-random position across stage + audience area, varied Z
        const float Hx = FMath::Frac(FMath::Sin((float)i * 12.9898f) * 43758.5453f);
        const float Hy = FMath::Frac(FMath::Sin((float)i * 78.233f)  * 12345.678f);
        const float Hz = FMath::Frac(FMath::Sin((float)i * 39.346f)  * 98765.4321f);

        const float X = FMath::Lerp(-StageWidth * 0.7f, StageWidth * 0.7f, Hx);
        const float Y = FMath::Lerp(-StageDepth * 3.0f, StageDepth * 0.4f, Hy);   // wide audience-side spread
        const float Z = FMath::Lerp(TrussHeight * 0.25f, TrussHeight * 0.9f, Hz);

        const FVector BasePos(X, Y, Z);
        L->SetRelativeLocation(BasePos);
        HazePuffBasePositions[i] = BasePos;

        L->SetAttenuationRadius(HazePuffRadius);
        L->SetSourceRadius(HazePuffRadius * 0.3f);
        L->SetSoftSourceRadius(HazePuffRadius * 0.3f);
        L->SetIntensityUnits(ELightUnits::Lumens);
        L->SetIntensity(HazePuffIntensity);
        L->SetCastShadows(false);
        L->SetVolumetricScatteringIntensity(HazeVolumetricScattering);
        L->SetLightColor(FLinearColor(0.7f, 0.7f, 0.8f));   // pale neutral; Tick will tint to scene color
        HazePuffs.Add(L);
    }

    UE_LOG(LogTemp, Warning, TEXT("ConcertStageDirector: Spawned %d haze puffs."), N);
}

void AConcertStageDirector::BuildDiscoBall()
{
    UStaticMesh* SphereMesh = DiscoBallMeshOverride;
    if (!SphereMesh) SphereMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
    if (!SphereMesh) return;

    // Anchor: hang the ball above the center of the audience block (= dance floor center).
    const float AudienceFront = -StageDepth * 0.5f - AudienceStartDepth;
    const float AudienceBack  = AudienceFront - (AudienceRows - 1) * AudienceRowSpacing;
    const float BallY = (AudienceFront + AudienceBack) * 0.5f;
    const float BallZ = TrussHeight - 350.f;

    // Create a rotating pivot at the ball position. Both the ball mesh and all colored beams
    // are children of this pivot, so rotating the pivot in Tick sweeps the entire beam rig
    // around the room (= disco ball spin effect).
    DiscoBallPivot = NewObject<USceneComponent>(this);
    DiscoBallPivot->SetupAttachment(Root);
    DiscoBallPivot->RegisterComponent();
    DiscoBallPivot->SetRelativeLocation(FVector(0.f, BallY, BallZ));

    // Ball mesh sits at the pivot origin.
    DiscoBallMesh = NewObject<UStaticMeshComponent>(this);
    DiscoBallMesh->SetupAttachment(DiscoBallPivot);
    DiscoBallMesh->RegisterComponent();
    DiscoBallMesh->SetStaticMesh(SphereMesh);
    DiscoBallMesh->SetRelativeLocation(FVector::ZeroVector);
    DiscoBallMesh->SetRelativeScale3D(FVector(DiscoBallScale, DiscoBallScale, DiscoBallScale));
    DiscoBallMesh->SetCastShadow(false);

    // Dim white "ball illuminator" from above so the mirror-ball mesh itself stays visible.
    // Much weaker than before (was 40000) so it does not blow out the floor underneath.
    DiscoBallLight = NewObject<USpotLightComponent>(this);
    DiscoBallLight->SetupAttachment(Root);   // attach to Root (NOT pivot) so it stays still
    DiscoBallLight->RegisterComponent();
    DiscoBallLight->SetRelativeLocation(FVector(0.f, BallY, BallZ + 250.f));
    DiscoBallLight->SetRelativeRotation(FRotator(-90.f, 0.f, 0.f));
    DiscoBallLight->SetInnerConeAngle(3.f);
    DiscoBallLight->SetOuterConeAngle(7.f);
    DiscoBallLight->SetAttenuationRadius(900.f);    // tight: only the ball mesh, not the floor
    DiscoBallLight->SetIntensityUnits(ELightUnits::Lumens);
    DiscoBallLight->SetIntensity(6000.f);            // was 40000
    DiscoBallLight->SetCastShadows(false);
    DiscoBallLight->SetVolumetricScatteringIntensity(0.5f);
    DiscoBallLight->SetLightColor(FLinearColor(1.0f, 0.95f, 0.85f));

    // Ring of colored beams attached to the pivot, aimed OUTWARD (yaw 0..360) and slightly DOWN.
    // Different colors per beam — as the pivot rotates, each colored shaft sweeps the room.
    static const FLinearColor BeamPalette[] = {
        FLinearColor(1.00f, 0.10f, 0.20f),   // red
        FLinearColor(1.00f, 0.45f, 0.05f),   // orange
        FLinearColor(1.00f, 0.95f, 0.20f),   // yellow
        FLinearColor(0.20f, 1.00f, 0.30f),   // green
        FLinearColor(0.20f, 1.00f, 0.85f),   // cyan
        FLinearColor(0.20f, 0.50f, 1.00f),   // blue
        FLinearColor(0.55f, 0.20f, 1.00f),   // violet
        FLinearColor(1.00f, 0.20f, 0.85f),   // magenta
    };
    const int32 PaletteSize = sizeof(BeamPalette) / sizeof(BeamPalette[0]);

    const int32 N = FMath::Max(1, NumDiscoBallBeams);
    for (int32 i = 0; i < N; i++) {
        const float Yaw = (360.f / (float)N) * (float)i;

        USpotLightComponent* Beam = NewObject<USpotLightComponent>(this);
        Beam->SetupAttachment(DiscoBallPivot);   // child of pivot -> rotates with it
        Beam->RegisterComponent();
        Beam->SetRelativeLocation(FVector::ZeroVector);
        // Spotlight default-aims along +X. Yaw rotates around Z; pitch tilts down.
        Beam->SetRelativeRotation(FRotator(DiscoBallBeamPitch, Yaw, 0.f));
        Beam->SetInnerConeAngle(FMath::Max(1.f, DiscoBallBeamOuterCone * 0.4f));
        Beam->SetOuterConeAngle(DiscoBallBeamOuterCone);
        Beam->SetAttenuationRadius(DiscoBallBeamAttenuation);
        Beam->SetIntensityUnits(ELightUnits::Lumens);
        Beam->SetIntensity(DiscoBallBeamIntensity);
        Beam->SetCastShadows(false);
        Beam->SetVolumetricScatteringIntensity(3.0f);   // makes beam shafts visible in fog
        Beam->SetLightColor(BeamPalette[i % PaletteSize]);
        DiscoBallBeams.Add(Beam);
    }
}

void AConcertStageDirector::BuildDanceFloorLights()
{
    DanceFloorPhaseOffsets.Reset();
    // The dance floor is a fixed DanceFloorWidth x DanceFloorDepth square centered on the
    // audience block, which is also exactly under the disco ball. Tables sit OUTSIDE this
    // footprint at +/-TableSideX. This gives a clean nightclub layout: ball in the middle,
    // dance floor under it, tables flanking it on both sides.
    const float AudienceFrontY  = -StageDepth * 0.5f - AudienceStartDepth;
    const float AudienceBackY   = AudienceFrontY - (AudienceRows - 1) * AudienceRowSpacing;
    const float CenterY         = (AudienceFrontY + AudienceBackY) * 0.5f;
    const float NearY           = CenterY + DanceFloorDepth * 0.5f;
    const float FarY            = CenterY - DanceFloorDepth * 0.5f;
    const float WidthHalf       = DanceFloorWidth * 0.5f;

    for (int32 r = 0; r < DanceFloorRows; r++) {
        for (int32 c = 0; c < DanceFloorCols; c++) {
            const float TX = (DanceFloorCols > 1) ? ((float)c / (DanceFloorCols - 1)) : 0.5f;
            const float TY = (DanceFloorRows > 1) ? ((float)r / (DanceFloorRows - 1)) : 0.5f;
            // Concentrate in the middle of the audience block.
            const float X = FMath::Lerp(-WidthHalf, WidthHalf, TX);
            const float Y = FMath::Lerp(NearY, FarY, TY);

            UPointLightComponent* L = NewObject<UPointLightComponent>(this);
            L->SetupAttachment(Root);
            L->RegisterComponent();
            // Lift slightly above the floor so the cylindrical illumination wraps audience legs
            L->SetRelativeLocation(FVector(X, Y, 60.f));
            L->SetIntensityUnits(ELightUnits::Lumens);
            L->SetIntensity(DanceFloorLightIntensity);
            L->SetAttenuationRadius(DanceFloorLightRadius);
            L->SetCastShadows(false);
            L->SetVolumetricScatteringIntensity(1.2f);   // was 2.0, less foggy bloom
            DanceFloorLights.Add(L);
            // Random phase offset so each light cycles through colors at a different moment
            DanceFloorPhaseOffsets.Add(FMath::FRandRange(0.0f, 1.0f));
        }
    }
}

void AConcertStageDirector::BuildHouseLights()
{
    // House lights provide low constant illumination across the venue
    // so the audience and back of the room remain visible even when
    // the dynamic stage lights are dim or focused elsewhere.
    const int32 NumLights = 6;
    for (int32 i = 0; i < NumLights; i++) {
        USpotLightComponent* L = NewObject<USpotLightComponent>(this);
        L->SetupAttachment(Root);
        L->RegisterComponent();

        // Spread across width and over the audience area
        const float TX = (NumLights > 1) ? ((float)i / (NumLights - 1)) : 0.5f;
        const float X  = FMath::Lerp(-StageWidth * 0.6f, StageWidth * 0.6f, TX);
        // Two rows: closer and farther audience
        const float Y  = (i < NumLights / 2)
            ? -StageDepth * 1.0f - 1500.f
            : -StageDepth * 1.0f - 4000.f;

        L->SetRelativeLocation(FVector(X, Y, TrussHeight + 200.f));
        L->SetRelativeRotation(FRotator(-75.f, 0.f, 0.f));
        L->SetInnerConeAngle(40.f);
        L->SetOuterConeAngle(70.f);
        L->SetAttenuationRadius(8000.f);
        L->SetIntensityUnits(ELightUnits::Lumens);
        L->SetIntensity(HouseLightIntensity);
        L->SetCastShadows(false);
        L->SetVolumetricScatteringIntensity(0.4f);
        L->SetLightColor(FLinearColor(1.0f, 0.85f, 0.7f));  // warm white house wash
        HouseLights.Add(L);
    }
}

void AConcertStageDirector::BuildAudience()
{
    // Determine if we are using real human meshes (override or pool) or placeholder shapes.
    const bool bHavePool = AudienceMeshPool.Num() > 0;
    const bool bUsingCustomMesh = bHavePool || (AudienceMeshOverride != nullptr);

    // Fallback chain for when nothing custom is assigned: capsule, then cylinder.
    UStaticMesh* FallbackMesh = AudienceMeshOverride;
    if (!FallbackMesh) FallbackMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Capsule.Capsule"));
    if (!FallbackMesh) FallbackMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
    if (!bHavePool && !FallbackMesh) return;

    // Only add a sphere head when using placeholder shapes. A real human mesh already has a head.
    UStaticMesh* HeadMesh = bUsingCustomMesh ? nullptr
        : LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));

    auto PickBodyMesh = [&]() -> UStaticMesh* {
        if (bHavePool) {
            // Random pick from the pool; skip null entries
            for (int32 attempt = 0; attempt < 8; attempt++) {
                const int32 idx = FMath::RandRange(0, AudienceMeshPool.Num() - 1);
                if (AudienceMeshPool[idx]) return AudienceMeshPool[idx].Get();
            }
        }
        return FallbackMesh;
    };

    for (int32 r = 0; r < AudienceRows; r++) {
        for (int32 c = 0; c < AudiencePerRow; c++) {
            float Tx = (AudiencePerRow > 1) ? ((float)c / (AudiencePerRow - 1)) : 0.5f;
            float X = FMath::Lerp(-StageWidth * 0.9f, StageWidth * 0.9f, Tx);
            float Y = -StageDepth * 0.5f - AudienceStartDepth - r * AudienceRowSpacing;

            // Slight horizontal jitter so rows don't look like a grid.
            X += FMath::FRandRange(-AudienceColSpacing * 0.25f, AudienceColSpacing * 0.25f);

            UStaticMeshComponent* M = NewObject<UStaticMeshComponent>(this);
            M->SetupAttachment(Root);
            M->RegisterComponent();
            M->SetStaticMesh(PickBodyMesh());
            const float HeightVar = FMath::FRandRange(0.92f, 1.10f);
            const float BaseZ = bUsingCustomMesh ? 0.f : 90.f;
            const FVector BasePos(X, Y, BaseZ);
            M->SetRelativeLocation(BasePos);
            if (bUsingCustomMesh) {
                const float S = AudienceScaleMultiplier;
                M->SetRelativeScale3D(FVector(S, S, S * HeightVar));
                M->SetRelativeRotation(FRotator(0.f, FMath::FRandRange(-25.f, 25.f), 0.f));
            } else {
                M->SetRelativeScale3D(FVector(0.45f, 0.45f, 1.6f * HeightVar));
            }
            M->SetCastShadow(false);
            AudienceMeshes.Add(M);
            AudienceBasePositions.Add(BasePos);
            AudiencePhases.Add(FMath::FRandRange(0.0f, 2.0f * (float)PI));
            AudienceJumpiness.Add(FMath::FRandRange(0.6f, 1.4f));

            // Add a head sphere parented to the body so it follows sway/jump animation.
            if (HeadMesh) {
                UStaticMeshComponent* H = NewObject<UStaticMeshComponent>(this);
                H->SetupAttachment(M);
                H->RegisterComponent();
                H->SetStaticMesh(HeadMesh);
                // Head sits at top of capsule. Capsule local space: top is at +1 in Z.
                H->SetRelativeLocation(FVector(0.f, 0.f, 60.f));
                H->SetRelativeScale3D(FVector(0.55f, 0.55f, 0.18f));
                H->SetCastShadow(false);
            }
        }
    }

    // Audience-aimed spot rig: 4 wide spots from the truss pointing into the crowd.
    for (int32 i = 0; i < 4; i++) {
        USpotLightComponent* L = NewObject<USpotLightComponent>(this);
        L->SetupAttachment(Root);
        L->RegisterComponent();
        float Tx = (float)i / 3.0f;
        float X = FMath::Lerp(-StageWidth * 0.5f, StageWidth * 0.5f, Tx);
        float Y = -StageDepth * 0.4f;
        L->SetRelativeLocation(FVector(X, Y, TrussHeight));
        L->SetRelativeRotation(FRotator(-30.f, 0.f, 0.f));
        L->SetInnerConeAngle(25.f);
        L->SetOuterConeAngle(55.f);
        L->SetAttenuationRadius(5000.f);
        L->SetIntensityUnits(ELightUnits::Lumens);
        L->SetIntensity(0.f);
        L->SetCastShadows(false);
        L->SetVolumetricScatteringIntensity(2.5f);
        AudienceLights.Add(L);
    }
}

void AConcertStageDirector::BuildStage()
{
    // Fixture-mesh helper: spawns a small emissive cube at a light position so the user
    // can SEE a physical fixture instead of beams appearing from nowhere. Mesh is parented
    // to Root, holds a Dynamic Material Instance, returns the component for later emissive updates.
    UStaticMesh* CubeMeshShared = nullptr;
    UMaterialInterface* FixtureBaseMat = nullptr;
    if (bSpawnLightFixtures) {
        CubeMeshShared  = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
        FixtureBaseMat  = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
    }
    auto SpawnFixture = [&](FVector Pos, FRotator Rot, FVector ScaleCm) -> UStaticMeshComponent* {
        if (!bSpawnLightFixtures || !CubeMeshShared) return nullptr;
        UStaticMeshComponent* M = NewObject<UStaticMeshComponent>(this);
        M->SetupAttachment(Root);
        M->RegisterComponent();
        M->SetStaticMesh(CubeMeshShared);
        M->SetRelativeLocation(Pos);
        M->SetRelativeRotation(Rot);
        // Engine cube is 100cm — divide by 100 to get scale of 1 unit per cm
        M->SetRelativeScale3D(FVector(ScaleCm.X / 100.f, ScaleCm.Y / 100.f, ScaleCm.Z / 100.f));
        M->SetCastShadow(false);
        if (FixtureBaseMat) {
            UMaterialInstanceDynamic* MID = M->CreateAndSetMaterialInstanceDynamic(0);
            if (MID) {
                MID->SetVectorParameterValue(TEXT("Color"),    FLinearColor(0.05f, 0.05f, 0.05f));
                MID->SetVectorParameterValue(TEXT("Emissive"), FLinearColor::Black);
            }
        }
        return M;
    };

    // Front-row wash lights along the front edge of the truss (down + slightly forward).
    {
        const int32 N = FMath::Max(2, NumWashLights);
        for (int32 i = 0; i < N; i++) {
            USpotLightComponent* L = NewObject<USpotLightComponent>(this);
            L->SetupAttachment(Root);
            L->RegisterComponent();
            const float t = (N > 1) ? ((float)i / (float)(N - 1)) : 0.5f;
            const float X = FMath::Lerp(-StageWidth * 0.5f, StageWidth * 0.5f, t);
            L->SetRelativeLocation(FVector(X, -StageDepth * 0.15f, TrussHeight));
            L->SetRelativeRotation(FRotator(-80.f, 0.f, 0.f));   // slight forward tilt
            L->SetInnerConeAngle(18.f);
            L->SetOuterConeAngle(40.f);
            L->SetAttenuationRadius(3000.f);
            L->SetIntensityUnits(ELightUnits::Lumens);
            L->SetIntensity(0.f);
            L->SetCastShadows(false);
            L->SetVolumetricScatteringIntensity(2.5f);
            WashLights.Add(L);
            // Visible PAR-can fixture hanging from the truss at the light position
            WashFixtures.Add(SpawnFixture(
                FVector(X, -StageDepth * 0.15f, TrussHeight - FixtureSize * 0.5f),
                FRotator(0.f, 0.f, 0.f),
                FVector(FixtureSize, FixtureSize, FixtureSize * 0.6f)));
        }
    }

    // Back-row wash lights — pointing forward over the band toward the audience.
    // Creates back-lit silhouettes of the performers and adds depth to the lighting rig.
    if (NumBackWashLights > 0)
    {
        const int32 N = NumBackWashLights;
        for (int32 i = 0; i < N; i++) {
            USpotLightComponent* L = NewObject<USpotLightComponent>(this);
            L->SetupAttachment(Root);
            L->RegisterComponent();
            const float t = (N > 1) ? ((float)i / (float)(N - 1)) : 0.5f;
            const float X = FMath::Lerp(-StageWidth * 0.45f, StageWidth * 0.45f, t);
            L->SetRelativeLocation(FVector(X, StageDepth * 0.45f, TrussHeight));
            L->SetRelativeRotation(FRotator(-55.f, 180.f, 0.f));  // pointing forward + down toward audience
            L->SetInnerConeAngle(15.f);
            L->SetOuterConeAngle(35.f);
            L->SetAttenuationRadius(3500.f);
            L->SetIntensityUnits(ELightUnits::Lumens);
            L->SetIntensity(0.f);
            L->SetCastShadows(false);
            L->SetVolumetricScatteringIntensity(3.0f);
            WashLights.Add(L);
            // Back-row PAR fixture
            WashFixtures.Add(SpawnFixture(
                FVector(X, StageDepth * 0.45f, TrussHeight - FixtureSize * 0.5f),
                FRotator(0.f, 0.f, 0.f),
                FVector(FixtureSize, FixtureSize, FixtureSize * 0.6f)));
        }
    }

    {
        const int32 N = FMath::Max(2, NumBeamLights);
        for (int32 i = 0; i < N; i++) {
            USpotLightComponent* L = NewObject<USpotLightComponent>(this);
            L->SetupAttachment(Root);
            L->RegisterComponent();
            const float t = (N > 1) ? ((float)i / (float)(N - 1)) : 0.5f;
            const float X = FMath::Lerp(-StageWidth * 0.48f, StageWidth * 0.48f, t);
            // Stagger Y depth slightly so beams interleave instead of all colliding at center
            const float Y = ((i % 2) == 0) ? -StageDepth * 0.25f : -StageDepth * 0.1f;
            L->SetRelativeLocation(FVector(X, Y, TrussHeight));
            L->SetRelativeRotation(FRotator(-70.f, 0.f, 0.f));
            L->SetInnerConeAngle(2.f);
            L->SetOuterConeAngle(7.f);
            L->SetAttenuationRadius(5000.f);
            L->SetIntensityUnits(ELightUnits::Lumens);
            L->SetIntensity(0.f);
            L->SetCastShadows(false);
            L->SetVolumetricScatteringIntensity(6.0f);
            BeamLights.Add(L);
            // Moving-head fixture: longer/skinnier than wash PAR, hangs from truss
            BeamFixtures.Add(SpawnFixture(
                FVector(X, Y, TrussHeight - FixtureSize * 0.8f),
                FRotator(0.f, 0.f, 0.f),
                FVector(FixtureSize * 0.8f, FixtureSize * 0.8f, FixtureSize * 1.3f)));
        }
    }

    // Strobe LED bars on the back wall — more = brighter strobe wall on drops.
    if (NumStrobeLights > 0)
    {
        const int32 N = NumStrobeLights;
        for (int32 i = 0; i < N; i++) {
            URectLightComponent* L = NewObject<URectLightComponent>(this);
            L->SetupAttachment(Root);
            L->RegisterComponent();
            const float t = (N > 1) ? ((float)i / (float)(N - 1)) : 0.5f;
            const float X = FMath::Lerp(-StageWidth * 0.45f, StageWidth * 0.45f, t);
            // Alternate strobe rows: even = upper, odd = lower so the back wall has TWO rows
            const float Z = ((i % 2) == 0) ? TrussHeight * 0.7f : TrussHeight * 0.4f;
            L->SetRelativeLocation(FVector(X, StageDepth * 0.5f, Z));
            L->SetRelativeRotation(FRotator(0.f, -90.f, 0.f));
            L->SetSourceWidth(180.f);
            L->SetSourceHeight(120.f);
            L->SetAttenuationRadius(3500.f);
            L->SetIntensityUnits(ELightUnits::Lumens);
            L->SetIntensity(0.f);
            L->SetCastShadows(false);
            StrobeLights.Add(L);
            // Long LED bar fixture on the back wall behind the strobe
            StrobeFixtures.Add(SpawnFixture(
                FVector(X, StageDepth * 0.5f + 30.f, Z),
                FRotator(0.f, 0.f, 0.f),
                FVector(FixtureSize * 4.0f, FixtureSize * 0.4f, FixtureSize * 0.8f)));
        }
    }

    // Side towers — stacked multiple levels per side for a real-festival look.
    {
        const int32 NPerSide = FMath::Max(0, NumSideLightsPerSide);
        for (int32 s = 0; s < 2; s++) {
            const float Side = (s == 0) ? -1.0f : 1.0f;
            for (int32 k = 0; k < NPerSide; k++) {
                USpotLightComponent* L = NewObject<USpotLightComponent>(this);
                L->SetupAttachment(Root);
                L->RegisterComponent();
                // Stack at increasing heights: k=0 low, k=NPerSide-1 high
                const float t = (NPerSide > 1) ? ((float)k / (float)(NPerSide - 1)) : 0.5f;
                const float Z = FMath::Lerp(TrussHeight * 0.25f, TrussHeight * 0.85f, t);
                // Slight Y offset so the stack reads as a "tower" of multiple lights
                const float Y = FMath::Lerp(-StageDepth * 0.1f, StageDepth * 0.1f, t);
                L->SetRelativeLocation(FVector(Side * StageWidth * 0.7f, Y, Z));
                L->SetRelativeRotation(FRotator(-15.f, Side * 90.f, 0.f));
                L->SetInnerConeAngle(12.f);
                L->SetOuterConeAngle(28.f);
                L->SetAttenuationRadius(3500.f);
                L->SetIntensityUnits(ELightUnits::Lumens);
                L->SetIntensity(0.f);
                L->SetCastShadows(false);
                L->SetVolumetricScatteringIntensity(2.0f);
                SideLights.Add(L);
                // Side-tower PAR fixture
                SideFixtures.Add(SpawnFixture(
                    FVector(Side * StageWidth * 0.7f, Y, Z),
                    FRotator(0.f, Side * 90.f, 0.f),
                    FVector(FixtureSize * 0.9f, FixtureSize * 0.9f, FixtureSize * 0.6f)));
            }
        }
    }

    // Stage-floor uplights (PAR cans pointing slightly forward + up).
    if (NumFloorLights > 0)
    {
        const int32 N = NumFloorLights;
        for (int32 i = 0; i < N; i++) {
            UPointLightComponent* L = NewObject<UPointLightComponent>(this);
            L->SetupAttachment(Root);
            L->RegisterComponent();
            const float t = (N > 1) ? ((float)i / (float)(N - 1)) : 0.5f;
            const float X = FMath::Lerp(-StageWidth * 0.45f, StageWidth * 0.45f, t);
            // Alternate front and back rows of floor uplights
            const float Y = ((i % 2) == 0) ? StageDepth * 0.40f : -StageDepth * 0.05f;
            L->SetRelativeLocation(FVector(X, Y, 60.f));
            L->SetAttenuationRadius(900.f);
            L->SetIntensityUnits(ELightUnits::Lumens);
            L->SetIntensity(0.f);
            L->SetCastShadows(false);
            FloorLights.Add(L);
            // Floor uplight fixture (small PAR can on stage deck)
            const float DeckZ = (bSpawnStageStructure && bSpawnStagePlatform) ? StagePlatformHeight : 0.f;
            FloorFixtures.Add(SpawnFixture(
                FVector(X, Y, DeckZ + FixtureSize * 0.5f),
                FRotator(0.f, 0.f, 0.f),
                FVector(FixtureSize * 0.8f, FixtureSize * 0.8f, FixtureSize)));
        }
    }

    // Lasers — extra-tight beams (0.5 deg cone) hanging from the truss. Look like real
    // concert lasers when they slice through volumetric fog. Off most of the time; fire
    // brightly on Cascade/Sparkle/Burst scenes (handled in Tick).
    if (NumLaserLights > 0)
    {
        const int32 N = NumLaserLights;
        for (int32 i = 0; i < N; i++) {
            USpotLightComponent* L = NewObject<USpotLightComponent>(this);
            L->SetupAttachment(Root);
            L->RegisterComponent();
            const float t = (N > 1) ? ((float)i / (float)(N - 1)) : 0.5f;
            const float X = FMath::Lerp(-StageWidth * 0.48f, StageWidth * 0.48f, t);
            L->SetRelativeLocation(FVector(X, StageDepth * 0.10f, TrussHeight));
            L->SetRelativeRotation(FRotator(-65.f, 0.f, 0.f));
            L->SetInnerConeAngle(0.2f);                 // pencil-thin core
            L->SetOuterConeAngle(0.6f);                 // 0.5 deg-ish — looks like a laser
            L->SetAttenuationRadius(8000.f);
            L->SetIntensityUnits(ELightUnits::Lumens);
            L->SetIntensity(0.f);
            L->SetCastShadows(false);
            L->SetVolumetricScatteringIntensity(8.0f);  // very bright in fog
            LaserLights.Add(L);
        }
    }

    // Audience ceiling lights — spotlights pointing DOWN onto the dance floor / crowd from
    // high above. Stay dim most of the time; pulse bright on dramatic scenes (Burst,
    // Cascade, Sparkle). The audience suddenly being lit reads as "moment of impact".
    if (NumAudienceCeilingLights > 0)
    {
        const int32 N = NumAudienceCeilingLights;
        const int32 Rows = 2;
        const int32 ColsPerRow = FMath::Max(1, N / Rows);
        const float AudienceFront = -StageDepth * 0.5f - 1500.f;  // approx audience start
        const float AudienceDepth = 8000.f;                        // 80m of audience area
        for (int32 row = 0; row < Rows; row++) {
            for (int32 col = 0; col < ColsPerRow; col++) {
                USpotLightComponent* L = NewObject<USpotLightComponent>(this);
                L->SetupAttachment(Root);
                L->RegisterComponent();
                const float tx = (ColsPerRow > 1) ? ((float)col / (float)(ColsPerRow - 1)) : 0.5f;
                const float ty = (Rows > 1) ? ((float)row / (float)(Rows - 1)) : 0.5f;
                const float X = FMath::Lerp(-StageWidth * 0.6f, StageWidth * 0.6f, tx);
                const float Y = FMath::Lerp(AudienceFront - 200.f, AudienceFront - AudienceDepth, ty);
                L->SetRelativeLocation(FVector(X, Y, TrussHeight + 200.f));   // above truss height
                L->SetRelativeRotation(FRotator(-90.f, 0.f, 0.f));            // straight down
                L->SetInnerConeAngle(15.f);
                L->SetOuterConeAngle(35.f);
                L->SetAttenuationRadius(VenueHeight + 1000.f);
                L->SetIntensityUnits(ELightUnits::Lumens);
                L->SetIntensity(0.f);
                L->SetCastShadows(false);
                L->SetVolumetricScatteringIntensity(2.0f);
                AudienceCeilingLights.Add(L);
            }
        }
    }

    // Pyro stinger lights — pre-spawn in the OFF state across the front edge of the stage.
    // Tick() handles the trigger detection + intensity animation.
    if (bEnablePyroBursts)
    {
        const float DeckZ = (bSpawnStageStructure && bSpawnStagePlatform) ? StagePlatformHeight : 0.f;
        const int32 N = FMath::Max(1, NumPyroLights);
        for (int32 i = 0; i < N; i++) {
            USpotLightComponent* L = NewObject<USpotLightComponent>(this);
            L->SetupAttachment(Root);
            L->RegisterComponent();
            const float t = (N > 1) ? ((float)i / (float)(N - 1)) : 0.5f;
            const float X = FMath::Lerp(-StageWidth * 0.45f, StageWidth * 0.45f, t);
            // Position along the FRONT edge of the platform, pointing straight UP.
            L->SetRelativeLocation(FVector(X, -StageDepth * 0.4f, DeckZ + 40.f));
            L->SetRelativeRotation(FRotator(90.f, 0.f, 0.f));   // pitch +90 = pointing UP
            L->SetInnerConeAngle(2.f);
            L->SetOuterConeAngle(6.f);
            L->SetAttenuationRadius(8000.f);
            L->SetIntensityUnits(ELightUnits::Lumens);
            L->SetIntensity(0.f);
            L->SetCastShadows(false);
            L->SetVolumetricScatteringIntensity(8.0f);          // very bright shaft when ON
            PyroLights.Add(L);

            // Visible emissive beam — thin tall cylinder/box at the same spot.
            if (CubeMeshShared) {
                UStaticMeshComponent* Beam = NewObject<UStaticMeshComponent>(this);
                Beam->SetupAttachment(Root);
                Beam->RegisterComponent();
                Beam->SetStaticMesh(CubeMeshShared);
                Beam->SetRelativeLocation(FVector(X, -StageDepth * 0.4f, DeckZ + 800.f));
                Beam->SetRelativeScale3D(FVector(0.3f, 0.3f, 16.f));    // thin tall pillar
                Beam->SetCastShadow(false);
                Beam->SetVisibility(false);    // hidden until pyro fires
                if (FixtureBaseMat) {
                    UMaterialInstanceDynamic* MID = Beam->CreateAndSetMaterialInstanceDynamic(0);
                    if (MID) {
                        MID->SetVectorParameterValue(TEXT("Color"),    FLinearColor::Black);
                        MID->SetVectorParameterValue(TEXT("Emissive"), FLinearColor::Black);
                    }
                }
                PyroBeamMeshes.Add(Beam);
            }
        }
    }

    // CO2 jet lights — pre-spawned at the front edge of the stage, point straight up.
    // Fire white-light columns on SNARE hits during energetic scenes (separate from pyros).
    if (bEnableCO2Jets && NumCO2Jets > 0)
    {
        const float DeckZ = (bSpawnStageStructure && bSpawnStagePlatform) ? StagePlatformHeight : 0.f;
        const int32 N = NumCO2Jets;
        CO2JetTimers.Init(0.f, N);
        for (int32 i = 0; i < N; i++) {
            USpotLightComponent* L = NewObject<USpotLightComponent>(this);
            L->SetupAttachment(Root);
            L->RegisterComponent();
            const float t = (N > 1) ? ((float)i / (float)(N - 1)) : 0.5f;
            const float X = FMath::Lerp(-StageWidth * 0.42f, StageWidth * 0.42f, t);
            L->SetRelativeLocation(FVector(X, -StageDepth * 0.35f, DeckZ + 30.f));
            L->SetRelativeRotation(FRotator(90.f, 0.f, 0.f));   // pitch +90 = straight up
            L->SetInnerConeAngle(3.f);
            L->SetOuterConeAngle(9.f);
            L->SetAttenuationRadius(6000.f);
            L->SetIntensityUnits(ELightUnits::Lumens);
            L->SetIntensity(0.f);
            L->SetCastShadows(false);
            L->SetVolumetricScatteringIntensity(6.0f);
            L->SetLightColor(FLinearColor::White);
            CO2JetLights.Add(L);

            // Visible thin emissive column for the "fog jet" body — hidden by default
            if (CubeMeshShared) {
                UStaticMeshComponent* Col = NewObject<UStaticMeshComponent>(this);
                Col->SetupAttachment(Root);
                Col->RegisterComponent();
                Col->SetStaticMesh(CubeMeshShared);
                Col->SetRelativeLocation(FVector(X, -StageDepth * 0.35f, DeckZ + 500.f));
                Col->SetRelativeScale3D(FVector(0.4f, 0.4f, 10.f));   // 4m tall thin pillar
                Col->SetCastShadow(false);
                Col->SetVisibility(false);
                if (FixtureBaseMat) {
                    UMaterialInstanceDynamic* MID = Col->CreateAndSetMaterialInstanceDynamic(0);
                    if (MID) {
                        MID->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.6f, 0.6f, 0.7f));
                        MID->SetVectorParameterValue(TEXT("Emissive"), FLinearColor::Black);
                    }
                }
                CO2JetMeshes.Add(Col);
            }
        }
    }

    // PHOTOGRAPHER FLASHES — pre-spawn tiny lights randomly scattered through the
    // audience volume. They normally stay dark; on each frame a small random subset
    // briefly fires bright white to simulate camera flashes from phone-cameras in the crowd.
    if (bEnableCrowdFlashes && NumCrowdFlashes > 0)
    {
        const int32 N = NumCrowdFlashes;
        CrowdFlashTimers.Init(0.f, N);
        for (int32 i = 0; i < N; i++) {
            UPointLightComponent* L = NewObject<UPointLightComponent>(this);
            L->SetupAttachment(Root);
            L->RegisterComponent();
            // Random position across the audience area (forward of stage in -Y direction)
            const float Hx = FMath::Frac(FMath::Sin((float)i * 12.9898f + 7.1f) * 43758.5453f);
            const float Hy = FMath::Frac(FMath::Sin((float)i * 78.233f  + 3.4f) * 12345.678f);
            const float Hz = FMath::Frac(FMath::Sin((float)i * 39.346f  + 1.7f) * 98765.4321f);

            const float X = FMath::Lerp(-StageWidth * 0.8f, StageWidth * 0.8f, Hx);
            const float Y = FMath::Lerp(-StageDepth * 0.5f - 1500.f, -StageDepth * 0.5f - 9000.f, Hy);
            const float Z = FMath::Lerp(150.f, 250.f, Hz);   // roughly chest-to-eye height

            L->SetRelativeLocation(FVector(X, Y, Z));
            L->SetAttenuationRadius(400.f);     // tight — single flashes don't bleed
            L->SetSourceRadius(20.f);
            L->SetSoftSourceRadius(40.f);
            L->SetIntensityUnits(ELightUnits::Lumens);
            L->SetIntensity(0.f);                // off
            L->SetCastShadows(false);
            L->SetVolumetricScatteringIntensity(2.0f);
            L->SetLightColor(FLinearColor::White);
            CrowdFlashLights.Add(L);
        }
    }

    // For each instrument: prefer the editor override, otherwise try the candidate paths
    // in order until one loads. This makes the code robust to asset moves/renames AND
    // lets the user swap meshes without recompiling. UE_LOG warns on total failure so
    // the Output Log tells you exactly which mesh didn't resolve.
    auto LoadFirstValidStatic = [](const TArray<const TCHAR*>& Paths) -> UStaticMesh* {
        for (const TCHAR* P : Paths) {
            if (UStaticMesh* M = LoadObject<UStaticMesh>(nullptr, P)) return M;
        }
        return nullptr;
    };
    auto LoadFirstValidSkeletal = [](const TArray<const TCHAR*>& Paths) -> USkeletalMesh* {
        for (const TCHAR* P : Paths) {
            if (USkeletalMesh* M = LoadObject<USkeletalMesh>(nullptr, P)) return M;
        }
        return nullptr;
    };

    UStaticMesh* DrumMesh = DrumMeshOverride
        ? DrumMeshOverride.Get()
        : LoadFirstValidStatic({
            // User-confirmed working reference (StaticMesh'/Game/Instruments/drum_kit/StaticMeshes/drum_kit.drum_kit')
            TEXT("/Game/Instruments/drum_kit/StaticMeshes/drum_kit.drum_kit"),
            // Speculative new-asset path (left as fallback in case the asset is moved later)
            TEXT("/Game/Instruments/drums/Drum_Set.Drum_Set"),
        });

    UStaticMesh* PianoMesh = PianoMeshOverride
        ? PianoMeshOverride.Get()
        : LoadFirstValidStatic({
            TEXT("/Game/Instruments/grand_piano/StaticMeshes/grand_piano.grand_piano"),
        });

    UStaticMesh* RoryMesh = GuitarMeshOverride
        ? GuitarMeshOverride.Get()
        : LoadFirstValidStatic({
            TEXT("/Game/Instruments/gitarist1/StaticMeshes/scene.scene"),
        });

    UStaticMesh* MicMesh = MicMeshOverride
        ? MicMeshOverride.Get()
        : LoadFirstValidStatic({
            TEXT("/Game/Instruments/microphone_stand_guitar_session/StaticMeshes/microphone_stand_guitar_session.microphone_stand_guitar_session"),
        });

    USkeletalMesh* VocalistMesh = VocalistMeshOverride
        ? VocalistMeshOverride.Get()
        : LoadFirstValidSkeletal({
            TEXT("/Game/Instruments/gitarist2/SkeletalMeshes/scene.scene"),
        });

    UStaticMesh* ViolinMesh = ViolinMeshOverride
        ? ViolinMeshOverride.Get()
        : LoadFirstValidStatic({
            TEXT("/Game/Instruments/violin/violin.violin"),
        });

    if (!DrumMesh)     UE_LOG(LogTemp, Warning, TEXT("ConcertStageDirector: DrumMesh failed to load - assign DrumMeshOverride in editor."));
    if (!PianoMesh)    UE_LOG(LogTemp, Warning, TEXT("ConcertStageDirector: PianoMesh failed to load - assign PianoMeshOverride in editor."));
    if (!RoryMesh)     UE_LOG(LogTemp, Warning, TEXT("ConcertStageDirector: GuitarMesh failed to load - assign GuitarMeshOverride in editor."));
    if (!MicMesh)      UE_LOG(LogTemp, Warning, TEXT("ConcertStageDirector: MicMesh failed to load - assign MicMeshOverride in editor."));
    if (!VocalistMesh) UE_LOG(LogTemp, Warning, TEXT("ConcertStageDirector: VocalistMesh failed to load - assign VocalistMeshOverride in editor."));

    auto AddInstrument = [&](FVector Pos, FRotator Rot, UStaticMesh* SM, USkeletalMesh* SkM, float LightRadius, FVector LightOffset, float PerInstrumentScale) {
        USceneComponent* R = NewObject<USceneComponent>(this);
        R->SetupAttachment(Root);
        R->RegisterComponent();
        R->SetRelativeLocation(Pos);
        R->SetRelativeRotation(Rot);
        InstrumentRoots.Add(R);

        // Track whether ANY mesh (static or skeletal) successfully attached to this slot.
        // Aligns 1-to-1 with InstrumentRoots indices so the debug overlay is truthful.
        bInstrumentMeshLoaded.Add(SM != nullptr || SkM != nullptr);

        FVector Scale(InstrumentScale * PerInstrumentScale);
        if (SM) {
            UStaticMeshComponent* M = NewObject<UStaticMeshComponent>(this);
            M->SetupAttachment(R);
            M->RegisterComponent();
            M->SetStaticMesh(SM);
            M->SetRelativeScale3D(Scale);
            M->SetCastShadow(false);
            InstrumentMeshes.Add(M);
        }
        else if (SkM) {
            // Use UPoseableMeshComponent so we can set bone rotations at runtime
            // (USkeletalMeshComponent does NOT expose SetBoneRotationByName).
            UPoseableMeshComponent* M = NewObject<UPoseableMeshComponent>(this);
            M->SetupAttachment(R);
            M->RegisterComponent();
            M->SetSkinnedAssetAndUpdate(SkM, false);
            M->SetRelativeScale3D(Scale);
            M->SetCastShadow(false);
        }

        UPointLightComponent* L = NewObject<UPointLightComponent>(this);
        L->SetupAttachment(R);
        L->RegisterComponent();
        L->SetRelativeLocation(LightOffset);
        L->SetIntensityUnits(ELightUnits::Lumens);
        L->SetAttenuationRadius(LightRadius);
        L->SetSourceRadius(15.f);
        L->SetIntensity(0.f);
        L->SetCastShadows(false);
        L->SetVolumetricScatteringIntensity(3.0f);
        InstrumentLights.Add(L);
    };

    // Index mapping (must match ClassifyInstrument indices in Tick):
    //   0 = Drum kit, 1 = Mic stand, 2 = Piano, 3 = Guitar, 4 = Vocalist
    // Yaw values come from the editor-exposed *YawDeg properties so you can fix any
    // wrong-facing mesh in the editor without recompiling.
    // Lift the band onto the raised stage platform if one is enabled. Without this offset
    // the performers would clip through the platform from below.
    const float StageDeckZ = (bSpawnStageStructure && bSpawnStagePlatform) ? StagePlatformHeight : 0.f;
    // Proper rock band layout:
    //   - Drums on stage LEFT (negative X), back-ish, drummer faces audience
    //   - Guitar on stage RIGHT (positive X), mirror of drums
    //   - Piano BACK CENTER (behind vocalist)
    //   - Vocalist FRONT CENTER, mic in front of vocalist
    AddInstrument(FVector(-StageWidth * 0.28f, StageDepth * DrumDepthFraction, StageDeckZ),      FRotator(0.f, DrumYawDeg,     0.f), DrumMesh,     nullptr,      1100.f, FVector(0, 0, 150), DrumScale);
    AddInstrument(FVector(0.f, -StageDepth * MicDepthFraction, StageDeckZ),                     FRotator(0.f, MicYawDeg,      0.f), MicMesh,      nullptr,       700.f, FVector(0, 0, 150), MicScale);
    AddInstrument(FVector(0.f, StageDepth * 0.30f, StageDeckZ),                                 FRotator(0.f, PianoYawDeg,    0.f), PianoMesh,    nullptr,       950.f, FVector(0, 0, 100), PianoScale);
    AddInstrument(FVector( StageWidth * 0.28f,  StageDepth * 0.05f, StageDeckZ),                FRotator(0.f, GuitarYawDeg,   0.f), RoryMesh,     nullptr,       950.f, FVector(0, 0, 150), GuitarScale);
    AddInstrument(FVector(0.f, -StageDepth * VocalistDepthFraction, StageDeckZ),                FRotator(0.f, VocalistYawDeg, 0.f), nullptr,      VocalistMesh,  850.f, FVector(0, 0, 180), VocalistScale);
    // Index 5: violin / strings — sits on stage right of vocalist
    AddInstrument(FVector( StageWidth * 0.18f, -StageDepth * 0.10f, StageDeckZ),                FRotator(0.f, ViolinYawDeg,   0.f), ViolinMesh,   nullptr,       750.f, FVector(0, 0, 120), ViolinScale);

    // Snapshot base positions for dynamic placement (lerp targets later)
    InstrumentBasePositions.Empty(InstrumentRoots.Num());
    for (int32 i = 0; i < InstrumentRoots.Num(); i++) {
        InstrumentBasePositions.Add(InstrumentRoots[i] ? InstrumentRoots[i]->GetRelativeLocation() : FVector::ZeroVector);
    }

    // Log a single-line summary of which instruments got meshes. Read this in Output Log
    // (Window -> Output Log) after pressing Play. Anything saying "MISSING" needs an
    // override assigned in the editor (Stage|Instruments -> *MeshOverride).
    static const TCHAR* IndexNames[] = { TEXT("Drum"), TEXT("Mic"), TEXT("Piano"), TEXT("Guitar"), TEXT("Vocalist"), TEXT("Violin") };
    for (int32 i = 0; i < bInstrumentMeshLoaded.Num() && i < 6; i++) {
        const bool bOK = bInstrumentMeshLoaded[i];
        const FVector WPos = (i < InstrumentRoots.Num() && InstrumentRoots[i]) ? InstrumentRoots[i]->GetComponentLocation() : FVector::ZeroVector;
        UE_LOG(LogTemp, Warning, TEXT("ConcertStageDirector: [%d] %s mesh = %s, world pos = %s"),
            i, IndexNames[i], bOK ? TEXT("LOADED") : TEXT("MISSING (assign override!)"), *WPos.ToString());
    }

    // ----- Vocalist pose adjustment -----
    // Find the UPoseableMeshComponent (vocalist) and rotate its upper-arm bones to
    // break the default T-pose. PoseableMesh exposes SetBoneRotationByName directly.
    {
        TArray<UPoseableMeshComponent*> AllPoseable;
        GetComponents<UPoseableMeshComponent>(AllPoseable);
        if (AllPoseable.Num() > 0) {
            VocalistSkeletalMesh = AllPoseable[0];
        }

        if (bAdjustVocalistPose && VocalistSkeletalMesh) {
            // Common bone naming conventions across rigs (Mixamo, UE4 mannequin, Renderpeople, etc.)
            const TArray<FName> LeftBones = {
                TEXT("LeftArm"), TEXT("L_UpperArm"), TEXT("upperarm_l"),
                TEXT("L_Arm"),  TEXT("Bip01_L_UpperArm"), TEXT("mixamorig:LeftArm")
            };
            const TArray<FName> RightBones = {
                TEXT("RightArm"), TEXT("R_UpperArm"), TEXT("upperarm_r"),
                TEXT("R_Arm"),  TEXT("Bip01_R_UpperArm"), TEXT("mixamorig:RightArm")
            };

            auto TryRotateBone = [&](FName Override, const TArray<FName>& Candidates, const FRotator& Rot, const TCHAR* Label) {
                FName UseBone = Override;
                if (UseBone.IsNone()) {
                    for (FName Name : Candidates) {
                        if (VocalistSkeletalMesh->GetBoneIndex(Name) != INDEX_NONE) {
                            UseBone = Name;
                            break;
                        }
                    }
                }
                if (!UseBone.IsNone()) {
                    VocalistSkeletalMesh->SetBoneRotationByName(UseBone, Rot, EBoneSpaces::ComponentSpace);
                    UE_LOG(LogTemp, Warning, TEXT("Vocalist %s arm: rotated bone '%s' by %s"),
                        Label, *UseBone.ToString(), *Rot.ToString());
                } else {
                    UE_LOG(LogTemp, Warning, TEXT("Vocalist %s arm: NO matching bone found. Set VocalistLeftArmBoneOverride / VocalistRightArmBoneOverride to one of these:"), Label);
                    // Dump the first ~25 bone names so the user can copy one into the override.
                    if (USkinnedAsset* SkAsset = VocalistSkeletalMesh->GetSkinnedAsset()) {
                        const FReferenceSkeleton& RefSkel = SkAsset->GetRefSkeleton();
                        const int32 MaxLog = FMath::Min(25, RefSkel.GetRawBoneNum());
                        for (int32 b = 0; b < MaxLog; b++) {
                            UE_LOG(LogTemp, Warning, TEXT("  bone[%d]: %s"), b, *RefSkel.GetBoneName(b).ToString());
                        }
                    }
                }
            };

            TryRotateBone(VocalistLeftArmBoneOverride,  LeftBones,  VocalistLeftArmRotation,  TEXT("LEFT"));
            TryRotateBone(VocalistRightArmBoneOverride, RightBones, VocalistRightArmRotation, TEXT("RIGHT"));

            // Resolve the head bone for the head-bob animation in Tick.
            const TArray<FName> HeadCandidates = {
                TEXT("Head"), TEXT("head"), TEXT("Bip01_Head"),
                TEXT("mixamorig:Head"), TEXT("Spine_Head"), TEXT("HeadTop")
            };
            VocalistHeadBoneResolved = VocalistHeadBoneOverride;
            if (VocalistHeadBoneResolved.IsNone()) {
                for (FName Name : HeadCandidates) {
                    if (VocalistSkeletalMesh->GetBoneIndex(Name) != INDEX_NONE) {
                        VocalistHeadBoneResolved = Name;
                        break;
                    }
                }
            }
            if (!VocalistHeadBoneResolved.IsNone()) {
                UE_LOG(LogTemp, Warning, TEXT("Vocalist head bone resolved: '%s'"), *VocalistHeadBoneResolved.ToString());
            }

            // Fallback: hide the arms entirely by scaling their bones to zero.
            // Useful when no rotation looks natural on the rig — vocalist becomes torso+head+legs,
            // mic stand in front fills the visual gap.
            if (bHideVocalistArms) {
                auto TryHideBone = [&](FName Override, const TArray<FName>& Candidates) {
                    FName UseBone = Override;
                    if (UseBone.IsNone()) {
                        for (FName Name : Candidates) {
                            if (VocalistSkeletalMesh->GetBoneIndex(Name) != INDEX_NONE) {
                                UseBone = Name;
                                break;
                            }
                        }
                    }
                    if (!UseBone.IsNone()) {
                        VocalistSkeletalMesh->SetBoneScaleByName(UseBone, FVector(0.001f), EBoneSpaces::ComponentSpace);
                    }
                };
                TryHideBone(VocalistLeftArmBoneOverride,  LeftBones);
                TryHideBone(VocalistRightArmBoneOverride, RightBones);
                UE_LOG(LogTemp, Warning, TEXT("Vocalist arms hidden (bHideVocalistArms = true)"));
            }
        }
    }
}

// Rotate a color's hue by a number of degrees (HSV space). Used by the dynamics
// system to oscillate scene colors continuously over time, so the lighting never
// looks "stuck" on one shade.
static FLinearColor ShiftHue(const FLinearColor& In, float DegreeShift)
{
    FLinearColor HSV = In.LinearRGBToHSV();
    HSV.R = FMath::Fmod(HSV.R + DegreeShift, 360.0f);
    if (HSV.R < 0.0f) HSV.R += 360.0f;
    return HSV.HSVToLinearRGB();
}

// Genre families. Coarse buckets the 80+ MTG-Jamendo subtags fold into.
// Declared above PickScene + Tick so both can use ClassifyGenre / DominantGenreFamily.
enum class EGenreFamily : uint8 {
    Unknown,
    RockMetal,
    Electronic,
    Classical,
    AmbientChill,
    JazzBlues,
    HipHopRap,
    Pop,
    SoulFunk,
    ReggaeSka,
    CountryFolk,
    LatinWorld
};

// Classify a single genre tag into a family. Order matters: more specific first.
// Covers the full MTG-Jamendo genre vocabulary (~80 tags), including:
//   indie, newwave, progressive, psychedelic, experimental, contemporary, improvisation,
//   and all the *rock / *metal / *house / *electro / *jazz subgenres.
static EGenreFamily ClassifyGenre(const FString& G)
{
    if (G.IsEmpty()) return EGenreFamily::Unknown;

    // Rock / metal family — adds: indie, progressive, psychedelic (these are nearly
    // always rock-leaning in the Jamendo vocab even though the bare names look generic).
    if (G.Contains(TEXT("rock")) || G.Contains(TEXT("metal")) ||
        G == TEXT("grunge")     || G == TEXT("alternative") ||
        G == TEXT("gothic")     || G == TEXT("industrial")  ||
        G == TEXT("hard")       || G == TEXT("hardcore")    ||
        G == TEXT("indie")      || G == TEXT("progressive") ||
        G == TEXT("psychedelic"))
        return EGenreFamily::RockMetal;

    // Electronic family — adds: experimental, newwave (synth-leaning).
    if (G.Contains(TEXT("electro")) || G.Contains(TEXT("house"))  ||
        G.Contains(TEXT("techno"))  || G.Contains(TEXT("trance")) ||
        G.Contains(TEXT("dance"))   || G == TEXT("dub")          ||
        G == TEXT("dubstep")        || G == TEXT("club")         ||
        G == TEXT("breakbeat")      || G == TEXT("drumnbass")    ||
        G == TEXT("idm")            || G == TEXT("minimal")      ||
        G == TEXT("edm")            || G == TEXT("experimental") ||
        G == TEXT("newwave"))
        return EGenreFamily::Electronic;

    // Classical / orchestral — adds: contemporary (contemporary classical).
    if (G == TEXT("classical")  || G == TEXT("orchestral") ||
        G == TEXT("soundtrack") || G == TEXT("symphonic")  ||
        G == TEXT("choir")      || G == TEXT("medieval")   ||
        G == TEXT("instrumentalpop") || G == TEXT("contemporary"))
        return EGenreFamily::Classical;

    if (G.Contains(TEXT("ambient")) || G == TEXT("atmospheric")   ||
        G == TEXT("chillout")       || G == TEXT("downtempo")     ||
        G == TEXT("easylistening")  || G == TEXT("newage")        ||
        G == TEXT("darkwave"))
        return EGenreFamily::AmbientChill;

    // Jazz / blues — adds: improvisation (jazz improvisation).
    if (G.Contains(TEXT("jazz")) || G == TEXT("blues")  ||
        G == TEXT("swing")       || G == TEXT("lounge") ||
        G == TEXT("fusion")      || G == TEXT("improvisation"))
        return EGenreFamily::JazzBlues;

    if (G == TEXT("hiphop") || G == TEXT("rap") || G == TEXT("triphop"))
        return EGenreFamily::HipHopRap;

    if (G == TEXT("pop") || G == TEXT("synthpop") || G == TEXT("popfolk"))
        return EGenreFamily::Pop;

    if (G == TEXT("soul")  || G == TEXT("funk")  || G == TEXT("groove") ||
        G == TEXT("disco") || G == TEXT("rnb")   || G == TEXT("jazzfunk"))
        return EGenreFamily::SoulFunk;

    if (G == TEXT("reggae") || G == TEXT("ska"))
        return EGenreFamily::ReggaeSka;

    if (G == TEXT("country")          || G == TEXT("folk")    ||
        G == TEXT("singersongwriter") || G == TEXT("chanson") ||
        G == TEXT("celtic"))
        return EGenreFamily::CountryFolk;

    if (G == TEXT("latin")   || G == TEXT("world")     ||
        G == TEXT("worldfusion") || G == TEXT("tribal")    ||
        G == TEXT("ethno")   || G == TEXT("african")   ||
        G == TEXT("oriental")|| G == TEXT("bossanova"))
        return EGenreFamily::LatinWorld;

    // Decade tags (60s/70s/80s/90s) intentionally stay Unknown — they're stylistically
    // ambiguous and shouldn't bias the lighting palette on their own.
    return EGenreFamily::Unknown;
}

// Aggregate top-N tag confidences by family and return the family with the highest
// total confidence, plus that total (so callers can apply a threshold).
// This is what fixes single-tag flicker: rock(40) + alternative(15) + heavymetal(8)
// totals 63% RockMetal, beating pop(40) + synthpop(5) = 45% Pop.
static EGenreFamily DominantGenreFamily(const TArray<FString>& Names,
                                         const TArray<float>& Pcts,
                                         float& OutTotalPct)
{
    OutTotalPct = 0.0f;
    if (Names.Num() == 0 || Names.Num() != Pcts.Num()) return EGenreFamily::Unknown;

    float Totals[12] = {};   // matches enum count
    for (int32 i = 0; i < Names.Num(); i++) {
        const EGenreFamily F = ClassifyGenre(Names[i]);
        if (F == EGenreFamily::Unknown) continue;
        Totals[(int32)F] += Pcts[i];
    }

    EGenreFamily Best = EGenreFamily::Unknown;
    float BestPct = 0.0f;
    for (int32 i = 1; i < 12; i++) {
        if (Totals[i] > BestPct) {
            BestPct = Totals[i];
            Best    = (EGenreFamily)i;
        }
    }
    OutTotalPct = BestPct;
    return Best;
}

// Apply the palette for a given family to the scene colors.
static void ApplyFamilyPalette(EGenreFamily Family, FLinearColor& A, FLinearColor& B, float& BrightnessMul)
{
    const FLinearColor Red    (1.00f, 0.10f, 0.10f);
    const FLinearColor Orange (1.00f, 0.45f, 0.05f);
    const FLinearColor Warm   (1.00f, 0.55f, 0.20f);
    const FLinearColor Cyan   (0.20f, 1.00f, 0.85f);
    const FLinearColor Magenta(1.00f, 0.20f, 0.85f);
    const FLinearColor Cold   (0.20f, 0.50f, 1.00f);
    const FLinearColor Purple (0.55f, 0.10f, 0.85f);
    const FLinearColor Green  (0.20f, 1.00f, 0.30f);
    const FLinearColor White  (1.00f, 0.95f, 0.85f);
    const FLinearColor Pink   (1.00f, 0.55f, 0.75f);
    const FLinearColor Yellow (1.00f, 0.90f, 0.20f);

    switch (Family) {
    case EGenreFamily::RockMetal:    A = Red;     B = Orange;  BrightnessMul *= 1.15f; break;
    case EGenreFamily::Electronic:   A = Magenta; B = Cyan;    BrightnessMul *= 1.10f; break;
    case EGenreFamily::Classical:    A = Warm;    B = White;   BrightnessMul *= 0.75f; break;
    case EGenreFamily::AmbientChill: A = Cold;    B = Cyan;    BrightnessMul *= 0.65f; break;
    case EGenreFamily::JazzBlues:    A = Purple;  B = Warm;    BrightnessMul *= 0.85f; break;
    case EGenreFamily::HipHopRap:    A = Magenta; B = Red;     BrightnessMul *= 1.10f; break;
    case EGenreFamily::Pop:          A = Pink;    B = Cyan;    BrightnessMul *= 1.00f; break;
    case EGenreFamily::SoulFunk:     A = Yellow;  B = Purple;  BrightnessMul *= 1.05f; break;
    case EGenreFamily::ReggaeSka:    A = Green;   B = Orange;  BrightnessMul *= 0.95f; break;
    case EGenreFamily::CountryFolk:  A = Warm;    B = Orange;  BrightnessMul *= 0.90f; break;
    case EGenreFamily::LatinWorld:   A = Orange;  B = Green;   BrightnessMul *= 0.95f; break;
    case EGenreFamily::Unknown:      default:                                          break;
    }
}

ELightingScene AConcertStageDirector::PickScene(float A, float V, float R, float I)
{
    // If a genre is detected with confidence, bias scene selection toward it.
    // GetTopGenresAny() reads sidecar OR local ONNX, so this works regardless of UDP state.
    if (AudioSource) {
        TArray<FString> GNames; TArray<float> GPcts;
        AudioSource->GetTopGenresAny(GNames, GPcts, 15);
        float FamilyPct = 0.0f;
        const EGenreFamily Family = DominantGenreFamily(GNames, GPcts, FamilyPct);
        if (Family != EGenreFamily::Unknown && FamilyPct > 0.22f) {
            switch (Family) {
            case EGenreFamily::RockMetal: {
                // 7 picks now: added BeamStab (rock-flavored: beams snap straight down on kick).
                const int32 r = FMath::RandRange(0, 6);
                if (r == 0) return ELightingScene::Strobe;
                if (r == 1) return ELightingScene::HeartBeat;
                if (r == 2) return ELightingScene::Cascade;
                if (r == 3) return ELightingScene::Sparkle;
                if (r == 4) return ELightingScene::BeamStab;
                if (r == 5) return ELightingScene::Neon;
                return ELightingScene::Chase;
            }
            case EGenreFamily::Classical:
                return FMath::RandBool() ? ELightingScene::Spotlight : ELightingScene::Warm;
            case EGenreFamily::Electronic: {
                // Electronic LOVES dynamic effects — bigger weight on HeartBeat/Cascade/Sparkle.
                const int32 r = FMath::RandRange(0, 6);
                if (r == 0) return ELightingScene::Strobe;
                if (r == 1) return ELightingScene::HeartBeat;
                if (r == 2) return ELightingScene::Cascade;
                if (r == 3) return ELightingScene::Sparkle;
                if (r <= 5) return ELightingScene::Neon;
                return ELightingScene::Chase;
            }
            case EGenreFamily::AmbientChill:
                return FMath::RandBool() ? ELightingScene::Cool : ELightingScene::Spotlight;
            case EGenreFamily::JazzBlues:
                return FMath::RandBool() ? ELightingScene::Warm : ELightingScene::Cool;
            case EGenreFamily::HipHopRap: {
                // Hiphop: kick-driven HeartBeat dominates.
                const int32 r = FMath::RandRange(0, 4);
                if (r == 0) return ELightingScene::Strobe;
                if (r == 1) return ELightingScene::HeartBeat;
                if (r == 2) return ELightingScene::Cascade;
                if (r == 3) return ELightingScene::Chase;
                return ELightingScene::Neon;
            }
            case EGenreFamily::Pop: {
                const int32 r = FMath::RandRange(0, 3);
                if (r == 0) return ELightingScene::RainbowWave;
                if (r == 1) return ELightingScene::Rainbow;
                if (r == 2) return ELightingScene::Warm;
                return ELightingScene::Chase;
            }
            case EGenreFamily::SoulFunk:
                return FMath::RandBool() ? ELightingScene::Rainbow : ELightingScene::Chase;
            case EGenreFamily::ReggaeSka:
                return FMath::RandBool() ? ELightingScene::Warm : ELightingScene::Rainbow;
            case EGenreFamily::CountryFolk:
                return FMath::RandBool() ? ELightingScene::Warm : ELightingScene::Spotlight;
            case EGenreFamily::LatinWorld:
                return FMath::RandBool() ? ELightingScene::Warm : ELightingScene::Rainbow;
            default: break;
            }
        }

        // Fallback: legacy top-1 string matching using whichever source is providing data.
        const FString G   = (GNames.Num() > 0) ? GNames[0] : FString();
        const float   GP  = (GPcts.Num()  > 0) ? GPcts[0]  : 0.0f;
        if (!G.IsEmpty() && GP > 0.22f) {

            // Rock / metal family — Chase / Neon / Strobe mix
            if (G.Contains(TEXT("rock")) || G.Contains(TEXT("metal")) ||
                G == TEXT("grunge")     || G == TEXT("alternative") ||
                G == TEXT("gothic")     || G == TEXT("industrial")  ||
                G == TEXT("hard")       || G == TEXT("hardcore")) {
                const int32 r = FMath::RandRange(0, 3);
                if (r == 0) return ELightingScene::Strobe;
                if (r == 1) return ELightingScene::Neon;
                return ELightingScene::Chase;
            }

            // Classical / orchestral — calm, focused
            if (G == TEXT("classical")  || G == TEXT("orchestral") ||
                G == TEXT("soundtrack") || G == TEXT("symphonic")  ||
                G == TEXT("choir")      || G == TEXT("medieval"))
                return FMath::RandBool() ? ELightingScene::Spotlight : ELightingScene::Warm;

            // Electronic / dance / EDM family
            if (G.Contains(TEXT("electro")) || G.Contains(TEXT("house"))  ||
                G.Contains(TEXT("techno"))  || G.Contains(TEXT("trance")) ||
                G.Contains(TEXT("dance"))   || G == TEXT("dub")          ||
                G == TEXT("dubstep")        || G == TEXT("club")         ||
                G == TEXT("breakbeat")      || G == TEXT("drumnbass")    ||
                G == TEXT("idm")            || G == TEXT("minimal")      ||
                G == TEXT("edm")) {
                const int32 r = FMath::RandRange(0, 4);
                if (r == 0) return ELightingScene::Strobe;
                if (r <= 2) return ELightingScene::Neon;
                return ELightingScene::Chase;
            }

            // Ambient / chill / dark — calm scenes
            if (G.Contains(TEXT("ambient")) || G == TEXT("atmospheric")   ||
                G == TEXT("chillout")       || G == TEXT("downtempo")     ||
                G == TEXT("easylistening")  || G == TEXT("newage")        ||
                G == TEXT("darkwave"))
                return FMath::RandBool() ? ELightingScene::Cool : ELightingScene::Spotlight;

            // Jazz / blues — moody warm/cool sweep
            if (G.Contains(TEXT("jazz")) || G == TEXT("blues")  ||
                G == TEXT("swing")       || G == TEXT("lounge") ||
                G == TEXT("fusion"))
                return FMath::RandBool() ? ELightingScene::Warm : ELightingScene::Cool;

            // Hip-hop / rap
            if (G == TEXT("hiphop") || G == TEXT("rap") || G == TEXT("triphop")) {
                const int32 r = FMath::RandRange(0, 3);
                if (r == 0) return ELightingScene::Strobe;
                if (r == 1) return ELightingScene::Chase;
                return ELightingScene::Neon;
            }

            // Pop family
            if (G == TEXT("pop") || G == TEXT("synthpop") || G == TEXT("popfolk")) {
                const int32 r = FMath::RandRange(0, 3);
                if (r == 0) return ELightingScene::RainbowWave;
                if (r == 1) return ELightingScene::Rainbow;
                if (r == 2) return ELightingScene::Warm;
                return ELightingScene::Chase;
            }

            // Soul / funk / disco / rnb
            if (G == TEXT("soul")  || G == TEXT("funk")  ||
                G == TEXT("groove")|| G == TEXT("disco") ||
                G == TEXT("rnb")   || G == TEXT("jazzfunk"))
                return FMath::RandBool() ? ELightingScene::Rainbow : ELightingScene::Chase;

            // Reggae / ska
            if (G == TEXT("reggae") || G == TEXT("ska"))
                return FMath::RandBool() ? ELightingScene::Warm : ELightingScene::Rainbow;

            // Country / folk / singer-songwriter
            if (G == TEXT("country")          || G == TEXT("folk")    ||
                G == TEXT("singersongwriter") || G == TEXT("chanson") ||
                G == TEXT("celtic"))
                return FMath::RandBool() ? ELightingScene::Warm : ELightingScene::Spotlight;

            // Latin / world / tribal / ethnic
            if (G == TEXT("latin")       || G == TEXT("world")     ||
                G == TEXT("worldfusion") || G == TEXT("tribal")    ||
                G == TEXT("ethno")       || G == TEXT("african")   ||
                G == TEXT("oriental")    || G == TEXT("bossanova"))
                return FMath::RandBool() ? ELightingScene::Warm : ELightingScene::Rainbow;
        }
    }

    if (I > 0.85f && A > 0.7f) return ELightingScene::Burst;
    if (A < 0.30f) return ELightingScene::Spotlight;
    if (A > 0.70f && R > 0.65f) return FMath::RandBool() ? ELightingScene::Chase : ELightingScene::Strobe;
    if (V > 0.65f) return FMath::RandBool() ? ELightingScene::Warm : ELightingScene::Rainbow;
    if (V < 0.35f) return FMath::RandBool() ? ELightingScene::Cool : ELightingScene::Neon;
    return (ELightingScene)FMath::RandRange(0, 7);
}

// Per-genre color palette overrides (called after the base scene has set its colors).
// The MTG-Jamendo tag set has 80+ genre subtags (heavymetal, hardrock, classicrock,
// electropop, deephouse, jazzfusion, etc.) so we use substring matching to catch the
// whole family from a single rule, instead of listing every subgenre by name.
// Order matters: more specific categories first to avoid mismatches.
static void ApplyGenrePalette(const FString& Genre, FLinearColor& A, FLinearColor& B, float& BrightnessMul)
{
    if (Genre.IsEmpty()) return;

    const FLinearColor Red    (1.00f, 0.10f, 0.10f);
    const FLinearColor Orange (1.00f, 0.45f, 0.05f);
    const FLinearColor Warm   (1.00f, 0.55f, 0.20f);
    const FLinearColor Cyan   (0.20f, 1.00f, 0.85f);
    const FLinearColor Magenta(1.00f, 0.20f, 0.85f);
    const FLinearColor Cold   (0.20f, 0.50f, 1.00f);
    const FLinearColor Purple (0.55f, 0.10f, 0.85f);
    const FLinearColor Green  (0.20f, 1.00f, 0.30f);
    const FLinearColor White  (1.00f, 0.95f, 0.85f);
    const FLinearColor Pink   (1.00f, 0.55f, 0.75f);
    const FLinearColor Yellow (1.00f, 0.90f, 0.20f);

    // Rock / metal family — catches: rock, metal, heavymetal, hardrock, classicrock,
    //   alternativerock, bluesrock, ethnicrock, grunge, instrumentalrock, poprock,
    //   postrock, punkrock, rocknroll, alternative, gothic, industrial, hard
    if (Genre.Contains(TEXT("rock")) || Genre.Contains(TEXT("metal")) ||
        Genre == TEXT("grunge")     || Genre == TEXT("alternative") ||
        Genre == TEXT("gothic")     || Genre == TEXT("industrial")  ||
        Genre == TEXT("hard")       || Genre == TEXT("hardcore"))
    {
        A = Red; B = Orange; BrightnessMul *= 1.15f;
        return;
    }

    // Electronic family — catches: electronic, electronica, electropop, edm, dance,
    //   eurodance, deephouse, house, techno, trance, dubstep, dub, club, breakbeat,
    //   drumnbass, idm, minimal
    if (Genre.Contains(TEXT("electro")) || Genre.Contains(TEXT("house"))  ||
        Genre.Contains(TEXT("techno"))  || Genre.Contains(TEXT("trance")) ||
        Genre.Contains(TEXT("dance"))   || Genre == TEXT("dub")          ||
        Genre == TEXT("dubstep")        || Genre == TEXT("club")         ||
        Genre == TEXT("breakbeat")      || Genre == TEXT("drumnbass")    ||
        Genre == TEXT("idm")            || Genre == TEXT("minimal")      ||
        Genre == TEXT("edm"))
    {
        A = Magenta; B = Cyan; BrightnessMul *= 1.10f;
        return;
    }

    // Classical / orchestral
    if (Genre == TEXT("classical")  || Genre == TEXT("orchestral") ||
        Genre == TEXT("soundtrack") || Genre == TEXT("symphonic")  ||
        Genre == TEXT("choir")      || Genre == TEXT("medieval")   ||
        Genre == TEXT("instrumentalpop"))
    {
        A = Warm; B = White; BrightnessMul *= 0.75f;
        return;
    }

    // Ambient / chill / dark
    if (Genre.Contains(TEXT("ambient")) || Genre == TEXT("atmospheric")   ||
        Genre == TEXT("chillout")       || Genre == TEXT("downtempo")     ||
        Genre == TEXT("easylistening")  || Genre == TEXT("newage")        ||
        Genre == TEXT("darkwave"))
    {
        A = Cold; B = Cyan; BrightnessMul *= 0.65f;
        return;
    }

    // Jazz / blues
    if (Genre.Contains(TEXT("jazz")) || Genre == TEXT("blues")  ||
        Genre == TEXT("swing")       || Genre == TEXT("lounge") ||
        Genre == TEXT("fusion"))
    {
        A = Purple; B = Warm; BrightnessMul *= 0.85f;
        return;
    }

    // Hip-hop / rap
    if (Genre == TEXT("hiphop") || Genre == TEXT("rap") || Genre == TEXT("triphop"))
    {
        A = Magenta; B = Red; BrightnessMul *= 1.10f;
        return;
    }

    // Pop family (synthpop, popfolk, etc.)
    if (Genre == TEXT("pop") || Genre == TEXT("synthpop") || Genre == TEXT("popfolk"))
    {
        A = Pink; B = Cyan; BrightnessMul *= 1.00f;
        return;
    }

    // Soul / funk / disco / rnb / groove
    if (Genre == TEXT("soul")  || Genre == TEXT("funk")  ||
        Genre == TEXT("groove")|| Genre == TEXT("disco") ||
        Genre == TEXT("rnb")   || Genre == TEXT("jazzfunk"))
    {
        A = Yellow; B = Purple; BrightnessMul *= 1.05f;
        return;
    }

    // Reggae / ska
    if (Genre == TEXT("reggae") || Genre == TEXT("ska"))
    {
        A = Green; B = Orange; BrightnessMul *= 0.95f;
        return;
    }

    // Country / folk / singer-songwriter
    if (Genre == TEXT("country")          || Genre == TEXT("folk")    ||
        Genre == TEXT("singersongwriter") || Genre == TEXT("chanson") ||
        Genre == TEXT("celtic"))
    {
        A = Warm; B = Orange; BrightnessMul *= 0.90f;
        return;
    }

    // Latin / world / tribal / ethnic
    if (Genre == TEXT("latin")  || Genre == TEXT("world")     ||
        Genre == TEXT("worldfusion") || Genre == TEXT("tribal") ||
        Genre == TEXT("ethno")  || Genre == TEXT("african")   ||
        Genre == TEXT("oriental") || Genre == TEXT("bossanova"))
    {
        A = Orange; B = Green; BrightnessMul *= 0.95f;
        return;
    }
}

void AConcertStageDirector::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    if (!AudioSource) return;
    const TArray<float>& Scores = AudioSource->GetAffectiveScores();
    if (Scores.Num() < 5) return;

    bool bSilence = true;
    for (float S : Scores) {
        if (FMath::Abs(S) > 0.0001f) { bSilence = false; break; }
    }
    SilenceFade = FMath::FInterpTo(SilenceFade, bSilence ? 0.0f : 1.0f, DeltaTime, 2.0f);

    float A = Normalize(Scores[0]);
    float V = Normalize(Scores[1]);
    float T = Normalize(Scores[2]);
    float R = Normalize(Scores[3]);
    float I = Normalize(Scores[4]);

    float Time = GetWorld()->GetTimeSeconds();
    double NowWall = FPlatformTime::Seconds();

    float BassLvl   = AudioSource->GetBassLevel();
    float MidLvl    = AudioSource->GetMidLevel();
    float TrebleLvl = AudioSource->GetTrebleLevel();

    auto Flash = [&](double LastOnset, float Window) {
        double Age = NowWall - LastOnset;
        return (Age >= 0.0 && Age < Window) ? (float)(1.0 - (Age / Window)) : 0.0f;
    };
    float KickFlash  = Flash(AudioSource->GetLastKickOnset(),  0.10f);
    float SnareFlash = Flash(AudioSource->GetLastSnareOnset(), 0.08f);
    float HiHatFlash = Flash(AudioSource->GetLastHiHatOnset(), 0.05f);

    // ===== BASS SPIKE DETECTION =====
    // One-shot global "open" flash on rapid bass increase.
    {
        const float BassRise = BassLvl - PrevBassLvl;
        const bool bElapsed  = (NowWall - LastBassSpikeTime) > BassSpikeCooldown;
        if (bBassSpikeReact && bElapsed && BassLvl > 0.45f && BassRise > BassSpikeThreshold) {
            LastBassSpikeTime = NowWall;
        }
        PrevBassLvl = FMath::FInterpTo(PrevBassLvl, BassLvl, DeltaTime, 6.0f);
    }
    float BassFlashMul = 1.0f;
    if (bBassSpikeReact) {
        const double Age = NowWall - LastBassSpikeTime;
        if (Age >= 0.0 && Age < BassSpikeDecaySeconds) {
            const float Norm = (float)(Age / BassSpikeDecaySeconds);
            const float Env  = FMath::Pow(1.0f - Norm, 2.0f);
            BassFlashMul = 1.0f + (BassSpikeMultiplier - 1.0f) * Env;
        }
    }

    // ===== ENERGY MODE: Calm / Normal / Peak =====
    // Read smoothed arousal, decide which band we're in (with hysteresis to prevent
    // flicker at boundaries), and produce two side effects:
    //   1) PeakFlashMul: a brief spike when ENTERING Peak mode (chorus-drop moment)
    //   2) ModeBrightnessMul: sustained brightness scaling per mode
    SmoothedArousal = FMath::FInterpTo(SmoothedArousal, A, DeltaTime, 1.5f);
    if (bEnergyModeReact) {
        const EEnergyMode PrevMode = CurrentEnergyMode;
        // Hysteresis: enter Calm at <0.32, exit at >0.42 (etc.)
        if (CurrentEnergyMode == EEnergyMode::Calm) {
            if (SmoothedArousal > CalmExitThreshold) CurrentEnergyMode = EEnergyMode::Normal;
        } else if (CurrentEnergyMode == EEnergyMode::Peak) {
            if (SmoothedArousal < PeakExitThreshold) CurrentEnergyMode = EEnergyMode::Normal;
        } else {  // Normal
            if      (SmoothedArousal < CalmEnterThreshold) CurrentEnergyMode = EEnergyMode::Calm;
            else if (SmoothedArousal > PeakEnterThreshold) CurrentEnergyMode = EEnergyMode::Peak;
        }
        // ENTERING Peak from Normal/Calm: fire the chorus-drop flash.
        if (PrevMode != EEnergyMode::Peak && CurrentEnergyMode == EEnergyMode::Peak) {
            LastPeakEntryTime = NowWall;
        }
    }
    // Compute the peak-entry flash multiplier (decays over PeakEntryFlashSeconds).
    float PeakFlashMul = 1.0f;
    if (PeakEntryFlashSeconds > 0.0f) {
        const double Age = NowWall - LastPeakEntryTime;
        if (Age >= 0.0 && Age < PeakEntryFlashSeconds) {
            const float Norm = (float)(Age / PeakEntryFlashSeconds);
            const float Env  = FMath::Pow(1.0f - Norm, 2.0f);
            PeakFlashMul = 1.0f + (PeakEntryFlashStrength - 1.0f) * Env;
        }
    }
    // Sustained brightness multiplier per mode.
    float ModeBrightnessMul = 1.0f;
    switch (CurrentEnergyMode) {
    case EEnergyMode::Calm:   ModeBrightnessMul = CalmBrightnessMul; break;
    case EEnergyMode::Peak:   ModeBrightnessMul = PeakBrightnessMul; break;
    default: break;
    }

    // ===== SOLO MODE DETECTION =====
    // When one instrument tag dominates, dim everything else and boost the soloist's
    // dedicated instrument light. Indices match InstrumentRoots: 0 drum, 1 mic, 2 piano,
    // 3 guitar/bass, 4 vocalist.
    bool  bSoloMode = false;
    int32 SoloIndex = -1;
    if (bSoloFocusReact) {
        TArray<FString> SoloNames; TArray<float> SoloPcts;
        AudioSource->GetTopInstrumentsAny(SoloNames, SoloPcts, 4);
        if (SoloNames.Num() >= 2 && SoloPcts.Num() >= 2 &&
            SoloPcts[0] >= SoloFocusTop1Min &&
            (SoloPcts[0] - SoloPcts[1]) >= SoloFocusGap) {
            // Renamed local from 'I' to 'SoloTag' to avoid shadowing the outer Intensity score 'I'.
            const FString& SoloTag = SoloNames[0];
            if (SoloTag == TEXT("drums") || SoloTag == TEXT("drummachine") || SoloTag == TEXT("percussion") ||
                SoloTag == TEXT("beat")  || SoloTag == TEXT("bongo"))                                          SoloIndex = 0;
            else if (SoloTag == TEXT("piano") || SoloTag == TEXT("keyboard") || SoloTag == TEXT("electricpiano") ||
                     SoloTag == TEXT("rhodes") || SoloTag == TEXT("organ") || SoloTag == TEXT("pipeorgan") ||
                     SoloTag == TEXT("accordion"))                                                              SoloIndex = 2;
            else if (SoloTag.Contains(TEXT("guitar")) || SoloTag.Contains(TEXT("bass")) ||
                     SoloTag == TEXT("synthesizer") || SoloTag == TEXT("computer") ||
                     SoloTag == TEXT("ukulele") || SoloTag == TEXT("doublebass"))                              SoloIndex = 3;
            else if (SoloTag == TEXT("voice") || SoloTag == TEXT("singer"))                                    SoloIndex = 4;
            bSoloMode = (SoloIndex >= 0);
        }
    }
    const float SoloDim   = bSoloMode ? SoloDimAmount   : 1.0f;
    const float SoloBoost = bSoloMode ? SoloBoostAmount : 1.0f;

    // Per-kick rising-edge detector: each fresh kick advances the hue accumulator.
    // Used by the dynamics layer below to give the colors a beat-locked drift.
    if (bStrongMusicReactivity) {
        if (PrevKickFlashForStep < 0.3f && KickFlash > 0.5f) {
            KickAccumDeg = FMath::Fmod(KickAccumDeg + HueStepPerKickDeg, 360.0f);
        }
        PrevKickFlashForStep = KickFlash;
    }

    // Force-re-roll scene when intensity jumps suddenly (musical-section change like
    // verse->chorus or build->drop). Cooldown 1s so we don't spam re-rolls.
    if (bStrongMusicReactivity
        && (NowWall - LastSceneReRollWall) > 1.0
        && (I - PrevIntensityForReRoll) > SceneReRollIntensityDelta)
    {
        CurrentScene = PickScene(A, V, R, I);
        SceneStartTime = NowWall;
        SceneDuration = FMath::FRandRange(SceneMinDuration, SceneMaxDuration);
        LastSceneReRollWall = NowWall;
    }
    PrevIntensityForReRoll = FMath::FInterpTo(PrevIntensityForReRoll, I, DeltaTime, 1.5f);

    if ((NowWall - SceneStartTime) > SceneDuration) {
        CurrentScene  = PickScene(A, V, R, I);
        SceneStartTime = NowWall;
        // Scene duration scales with energy mode — Calm holds scenes longer, Peak flips faster.
        float DurationMul = 1.0f;
        if (bEnergyModeReact) {
            switch (CurrentEnergyMode) {
            case EEnergyMode::Calm: DurationMul = CalmSceneDurationMul; break;
            case EEnergyMode::Peak: DurationMul = PeakSceneDurationMul; break;
            default: break;
            }
        }
        SceneDuration  = FMath::FRandRange(SceneMinDuration, SceneMaxDuration) * DurationMul;
    }
    float ArousalRise = A - PrevArousal;
    if (ArousalRise > BurstArousalThreshold && (NowWall - LastBurstTime) > BurstCooldownSeconds) {
        CurrentScene  = ELightingScene::Burst;
        SceneStartTime = NowWall;
        SceneDuration  = 1.5f;
        LastBurstTime  = NowWall;
    }
    PrevArousal = FMath::FInterpTo(PrevArousal, A, DeltaTime, 1.0f);

    const FLinearColor Warm(1.0f, 0.55f, 0.20f);
    const FLinearColor Cold(0.20f, 0.50f, 1.0f);
    const FLinearColor Magenta(1.0f, 0.20f, 0.85f);
    const FLinearColor Cyan(0.20f, 1.0f, 0.85f);
    const FLinearColor Red(1.0f, 0.10f, 0.10f);
    const FLinearColor Green(0.20f, 1.0f, 0.30f);
    const FLinearColor White(1.0f, 1.0f, 1.0f);

    FLinearColor MoodColor   = FMath::Lerp(Cold, Warm, V);
    FLinearColor TimbreColor = FMath::Lerp(Cyan, Magenta, T);
    FLinearColor SceneColorA = MoodColor;
    FLinearColor SceneColorB = TimbreColor;
    float SceneBrightness = 1.0f;
    bool  bChase = false;
    bool  bAllSync = false;
    bool  bSpotlightFocus = false;
    bool  bBurst = false;
    bool  bHeartBeat = false;   // binary on/off snapped to kick drum
    bool  bCascade   = false;   // fast one-light-at-a-time sweep
    bool  bSparkle   = false;   // random per-light flashes
    bool  bRainbowWave = false; // hue gradient marches across the truss (pop)
    bool  bBeamStab    = false; // all beams pitch straight down on kick (rock)

    switch (CurrentScene) {
    case ELightingScene::Warm:     SceneColorA = Warm;    SceneColorB = Red;     break;
    case ELightingScene::Cool:     SceneColorA = Cold;    SceneColorB = Cyan;    break;
    case ELightingScene::Neon:     SceneColorA = Magenta; SceneColorB = Cyan;    break;
    case ELightingScene::Chase:    SceneColorA = MoodColor; SceneColorB = TimbreColor; bChase = true; break;
    // Strobe and Burst keep their high-energy on/off behavior, but the FLASH COLOR is now
    // mood/timbre driven (so rock strobes are red, electronic strobes are magenta, etc.)
    // instead of pure white. The palette override below will further refine per-genre.
    case ELightingScene::Strobe:   SceneColorA = MoodColor;   SceneColorB = TimbreColor;   bAllSync = true; break;
    case ELightingScene::Spotlight: SceneColorA = Warm;   SceneBrightness = 0.3f; bSpotlightFocus = true; break;
    case ELightingScene::Burst:    SceneColorA = TimbreColor; SceneColorB = MoodColor;     SceneBrightness = 1.6f; bBurst = true; break;
    case ELightingScene::Rainbow:  SceneColorA = MoodColor; SceneColorB = TimbreColor; break;
    // High-energy scenes — colors driven by mood/timbre+genre, behavior driven by flags.
    case ELightingScene::HeartBeat: SceneColorA = MoodColor;   SceneColorB = TimbreColor; bHeartBeat = true; break;
    case ELightingScene::Cascade:   SceneColorA = MoodColor;   SceneColorB = TimbreColor; bCascade   = true; break;
    case ELightingScene::Sparkle:   SceneColorA = TimbreColor; SceneColorB = MoodColor;   bSparkle   = true; break;
    case ELightingScene::RainbowWave: SceneColorA = MoodColor; SceneColorB = TimbreColor; bRainbowWave = true; break;
    case ELightingScene::BeamStab:    SceneColorA = MoodColor; SceneColorB = TimbreColor; bBeamStab    = true; break;
    }

    // Genre palette: aggregate confidence by family across the top-N tags.
    // Uses GetTopGenresAny() which prefers the Python sidecar (top-15 with EMA smoothing)
    // but FALLS BACK to the in-engine ONNX inference (SmoothedTagProbs) when the sidecar
    // UDP isn't reaching us. So lighting works regardless of whether the sidecar is connected.
    if (AudioSource) {
        TArray<FString> GNames;
        TArray<float>   GPcts;
        AudioSource->GetTopGenresAny(GNames, GPcts, 15);
        float FamilyPct = 0.0f;
        const EGenreFamily Family = DominantGenreFamily(GNames, GPcts, FamilyPct);
        if (Family != EGenreFamily::Unknown && FamilyPct > 0.22f) {
            ApplyFamilyPalette(Family, SceneColorA, SceneColorB, SceneBrightness);
        }
        else if (GNames.Num() > 0 && GPcts.Num() > 0 && GPcts[0] > 0.22f) {
            // Fallback: legacy top-1 substring matcher.
            ApplyGenrePalette(GNames[0], SceneColorA, SceneColorB, SceneBrightness);
        }
    }

    // === PYRO BURST === — genre-locked, rare, peak-triggered upward flash.
    // Conditions: enabled + cooldown elapsed + energetic family + I & Bass both above thresholds.
    if (bEnablePyroBursts && PyroLights.Num() > 0)
    {
        // Re-aggregate the family decision so we can lock pyros to energetic genres only.
        EGenreFamily PyroFamily = EGenreFamily::Unknown;
        if (AudioSource) {
            TArray<FString> GN; TArray<float> GP;
            AudioSource->GetTopGenresAny(GN, GP, 15);
            float Tot = 0.f;
            PyroFamily = DominantGenreFamily(GN, GP, Tot);
            if (Tot < 0.22f) PyroFamily = EGenreFamily::Unknown;
        }
        const bool bFamilyAllowed =
            (PyroFamily == EGenreFamily::RockMetal)  ||
            (PyroFamily == EGenreFamily::Electronic) ||
            (PyroFamily == EGenreFamily::HipHopRap);

        const double TimeSincePyro = NowWall - LastPyroTime;
        const bool bCanFire =
            bFamilyAllowed
            && TimeSincePyro > PyroCooldownSeconds
            && I       > PyroIntensityThreshold
            && BassLvl > PyroBassThreshold;

        if (bCanFire) {
            LastPyroTime  = NowWall;
            PyroStartTime = NowWall;
            // Optional Niagara particle spawn alongside the procedural lights.
            if (PyroNiagaraSystem) {
                UWorld* W = GetWorld();
                if (W) {
                    for (int32 i = 0; i < PyroLights.Num(); i++) {
                        if (!PyroLights[i]) continue;
                        const FVector SpawnLoc = PyroLights[i]->GetComponentLocation();
                        UNiagaraFunctionLibrary::SpawnSystemAtLocation(W, PyroNiagaraSystem, SpawnLoc, FRotator::ZeroRotator, FVector(1.f), true);
                    }
                }
            }
        }

        // Animate the lights + emissive beams during the burst window.
        const double PyroAge = NowWall - PyroStartTime;
        if (PyroAge < (double)PyroDurationSeconds) {
            const float T01     = FMath::Clamp((float)PyroAge / FMath::Max(0.1f, PyroDurationSeconds), 0.f, 1.f);
            // Fast attack (first 10%), slow decay (rest).
            const float Env     = (T01 < 0.10f) ? (T01 / 0.10f) : FMath::Pow(1.0f - (T01 - 0.10f) / 0.90f, 2.0f);
            const FLinearColor PyroColor = FMath::Lerp(SceneColorA, FLinearColor::White, 0.4f);
            const FLinearColor PyroEmissive = PyroColor * 80.0f * Env;
            for (int32 i = 0; i < PyroLights.Num(); i++) {
                if (PyroLights[i]) {
                    PyroLights[i]->SetIntensity(PyroIntensityPeak * Env);
                    PyroLights[i]->SetLightColor(PyroColor);
                }
                if (i < PyroBeamMeshes.Num() && PyroBeamMeshes[i]) {
                    PyroBeamMeshes[i]->SetVisibility(true);
                    if (UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(PyroBeamMeshes[i]->GetMaterial(0))) {
                        MID->SetVectorParameterValue(TEXT("Color"),    PyroColor);
                        MID->SetVectorParameterValue(TEXT("Emissive"), PyroEmissive);
                    }
                }
            }
        } else {
            // Outside the burst window: lights off, beam meshes hidden.
            for (USpotLightComponent* L : PyroLights) { if (L) L->SetIntensity(0.f); }
            for (UStaticMeshComponent* B : PyroBeamMeshes) { if (B) B->SetVisibility(false); }
        }
    }

    // === VOCALIST HEAD BOB === — subtle head movement so the singer doesn't look frozen.
    if (bVocalistHeadBob && VocalistSkeletalMesh && !VocalistHeadBoneResolved.IsNone()) {
        // Slow continuous bob driven by bass + small wobble in time
        const float Pitch = -VocalistHeadBobAmount * (BassLvl * 0.5f + 0.5f) * FMath::Sin(Time * 5.0f);
        const float YawWobble = 6.0f * FMath::Sin(Time * 1.7f);
        // Compose with the head's existing reference orientation (we don't know that
        // exactly, so just set the delta rotation in component space — looks natural).
        VocalistSkeletalMesh->SetBoneRotationByName(VocalistHeadBoneResolved,
            FRotator(Pitch, YawWobble, 0.f), EBoneSpaces::ComponentSpace);
    }

    // === PHOTOGRAPHER FLASHES === — random tiny white strobes in the crowd.
    if (bEnableCrowdFlashes && CrowdFlashLights.Num() == CrowdFlashTimers.Num()) {
        const bool bPeakScene = (CurrentScene == ELightingScene::Burst)
                             || (CurrentScene == ELightingScene::HeartBeat)
                             || (CurrentScene == ELightingScene::Cascade)
                             || (CurrentScene == ELightingScene::Sparkle);
        const float RateMul = bPeakScene ? CrowdFlashPeakMultiplier : 1.0f;
        const float TriggerRate = CrowdFlashRatePerFrame * RateMul;

        for (int32 i = 0; i < CrowdFlashLights.Num(); i++) {
            // Random trigger this frame
            if (CrowdFlashTimers[i] <= 0.f && FMath::FRand() < TriggerRate) {
                CrowdFlashTimers[i] = 1.0f;
            }
            // Decay each frame — fast attack (instant), 100ms decay
            CrowdFlashTimers[i] = FMath::Max(0.f, CrowdFlashTimers[i] - DeltaTime * 10.f);
            if (CrowdFlashLights[i]) {
                CrowdFlashLights[i]->SetIntensity(CrowdFlashIntensity * CrowdFlashTimers[i]);
            }
        }
    }

    // === CO2 JETS === — fire white-light columns on snare hits during energetic scenes.
    // Each jet has its own timer; on snare, randomly pick which subset of jets to fire.
    if (bEnableCO2Jets && CO2JetLights.Num() > 0 && CO2JetTimers.Num() == CO2JetLights.Num())
    {
        const bool bSceneAllowed = (CurrentScene != ELightingScene::Spotlight)
                                && (CurrentScene != ELightingScene::Warm)
                                && (CurrentScene != ELightingScene::Cool);

        // Detect rising-edge snare to trigger jets
        static float PrevSnareForCO2 = 0.0f;
        if (bSceneAllowed && PrevSnareForCO2 < 0.2f && SnareFlash > CO2JetSnareThreshold) {
            // Fire a random subset (50-100%) of jets on this snare hit
            for (int32 i = 0; i < CO2JetTimers.Num(); i++) {
                if (FMath::FRand() > 0.5f) {
                    CO2JetTimers[i] = 0.35f;   // fire for 350ms
                }
            }
        }
        PrevSnareForCO2 = SnareFlash;

        // Animate active jets
        for (int32 i = 0; i < CO2JetLights.Num(); i++) {
            CO2JetTimers[i] = FMath::Max(0.f, CO2JetTimers[i] - DeltaTime);
            const float t = CO2JetTimers[i] / 0.35f;   // 1..0 over jet lifetime
            const float Env = t;                       // simple linear decay
            const float Intensity = CO2JetIntensityPeak * Env;
            if (CO2JetLights[i]) {
                CO2JetLights[i]->SetIntensity(Intensity);
            }
            if (i < CO2JetMeshes.Num() && CO2JetMeshes[i]) {
                const bool bActive = t > 0.01f;
                CO2JetMeshes[i]->SetVisibility(bActive);
                if (bActive) {
                    if (UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(CO2JetMeshes[i]->GetMaterial(0))) {
                        MID->SetVectorParameterValue(TEXT("Emissive"), FLinearColor::White * 40.f * Env);
                    }
                }
            }
        }
    }

    // === DYNAMICS LAYER === — color VARIATION on top of the (otherwise constant) genre palette.
    // Two effects stacked:
    //   1) SLOW continuous oscillation (HueCycleAmplitude * sin(t)) — colors gently breathe.
    //   2) STEPPED palette rotation every PaletteRotationSeconds — bigger periodic shift.
    // Together: a song that's all "rock" no longer looks identical the whole time —
    // there are clear "stages" of color over each ~30s, plus continuous gentle drift.
    if (bDynamicHueCycle) {
        const float HueOsc = HueCycleAmplitude * FMath::Sin(Time * 2.0f * (float)PI * HueCycleSpeed);
        const float StepIdx = FMath::Floor(Time / FMath::Max(1.0f, PaletteRotationSeconds));
        const float PaletteShift = StepIdx * PaletteRotationDegrees;
        // Per-kick accumulator: extra hue drift tied directly to the rhythm.
        const float KickShift = bStrongMusicReactivity ? KickAccumDeg : 0.0f;
        const float TotalShift = HueOsc + PaletteShift + KickShift;
        SceneColorA = ShiftHue(SceneColorA, TotalShift);
        SceneColorB = ShiftHue(SceneColorB, -TotalShift * 0.6f);   // counter-shift B for richer contrast
    }

    {
        int32 N = WashLights.Num();
        float ChasePos = FMath::Frac(Time * 1.2f) * N;
        // Cascade sweeps faster than Chase — 4x speed, tighter falloff
        float CascadePos = FMath::Frac(Time * 4.0f) * N;
        for (int32 i = 0; i < N; i++) {
            int32 Mirror = (i < N / 2) ? i : (N - 1 - i);
            float Local = 0.7f + 0.3f * FMath::Sin(Time * 1.5f + Mirror * 0.6f);

            // STRONG music reactivity. Two effects stacked:
            //   (1) Bass GATE: lights dim when bass is low (creates breathing) and pump
            //       hard when bass is high. Range 0.3 .. 1.7 vs old "1.0 + 0.4*Bass" (1.0 .. 1.4).
            //   (2) Kick punch: each kick adds a ~2x flash on top of the gated baseline.
            float BassGate = bStrongMusicReactivity
                ? FMath::Lerp(1.0f, (1.0f - BassGateAmount) + BassGateAmount * 2.0f * BassLvl, ReactivityStrength * 0.6f)
                : (1.0f + 0.4f * BassLvl);
            float KickPunch = bStrongMusicReactivity
                ? (1.0f + 1.4f * KickFlash * ReactivityStrength)
                : (1.0f + 0.6f * KickFlash);
            float Boost = BassGate * KickPunch;
            float SceneMul = SceneBrightness;

            if (bChase) {
                float Dist = FMath::Abs(i - ChasePos);
                if (Dist > N * 0.5f) Dist = N - Dist;
                SceneMul *= 0.15f + FMath::Exp(-Dist * 1.4f);
            }
            if (bCascade) {
                // Sharp pulse follows CascadePos; everything else nearly black.
                float Dist = FMath::Abs(i - CascadePos);
                if (Dist > N * 0.5f) Dist = N - Dist;
                SceneMul *= 0.05f + FMath::Exp(-Dist * 3.5f) * 1.6f;
            }
            if (bHeartBeat) {
                // Binary on/off — fully bright on kick (KickFlash > 0.4), nearly black between.
                // Use the kick-flash decay so we get a quick fade rather than a hard click.
                const float Beat = (KickFlash > 0.05f) ? (0.2f + 1.6f * KickFlash) : 0.04f;
                SceneMul *= Beat;
                Local = 1.0f;   // bypass slow sine when in HeartBeat
            }
            if (bSparkle) {
                // Each light has a deterministic per-index time-stamped flicker. Hash i+Time
                // to get a pseudo-random window where this specific light lights up briefly.
                // Renamed local to FlashAmt to avoid shadowing the outer 'Flash' lambda.
                const float Phase    = FMath::Frac(Time * 5.0f + i * 0.3137f);
                const float FlashAmt = (Phase < 0.08f) ? 1.7f : 0.06f;
                SceneMul *= FlashAmt;
                Local = 1.0f;
            }
            if (bSpotlightFocus) {
                int32 Center = N / 2;
                int32 D = FMath::Abs(i - Center);
                SceneMul *= (D <= 1) ? 1.4f : 0.05f;
            }
            if (bAllSync) Local = 1.0f;
            if (bBurst) { SceneMul *= 1.4f; Local = 1.0f; }

            FLinearColor C;
            if (CurrentScene == ELightingScene::Rainbow) {
                float Hue = FMath::Frac((float)i / N + Time * 0.1f);
                C = FLinearColor::MakeFromHSV8((uint8)(Hue * 255), 220, 255);
            } else if (bRainbowWave) {
                // Pop-flavored: smooth hue gradient that marches across the truss FASTER than Rainbow,
                // with more saturated colors. Wave moves in one direction (no oscillation).
                float Hue = FMath::Frac((float)i / N + Time * 0.35f);
                C = FLinearColor::MakeFromHSV8((uint8)(Hue * 255), 240, 255);
            } else if (bSparkle) {
                // Sparkle uses a hue rotation per-light too, for camera-flash variety.
                float Hue = FMath::Frac((float)i * 0.137f + Time * 0.05f);
                C = FLinearColor::MakeFromHSV8((uint8)(Hue * 255), 80, 255);  // low saturation = white-ish
            } else {
                C = (i % 2 == 0) ? SceneColorA : SceneColorB;
            }

            // Snare white-flash overlay — every snare hit briefly tints all wash lights
            // toward white. Highly visible and rhythm-locked.
            if (bStrongMusicReactivity && SnareFlash > 0.05f) {
                C = FMath::Lerp(C, FLinearColor::White, SnareFlash * SnareWhiteFlash);
            }

            // ROCK CHORUS RED-FLAME WASH — when rock/metal AND high intensity, push
            // the wash color toward red-orange so it feels like a real rock chorus.
            // The mix amount scales with intensity, so verses don't get the flame look.
            {
                EGenreFamily WashFamily = EGenreFamily::Unknown;
                if (AudioSource) {
                    TArray<FString> GN; TArray<float> GP;
                    AudioSource->GetTopGenresAny(GN, GP, 15);
                    float Tot = 0.f;
                    WashFamily = DominantGenreFamily(GN, GP, Tot);
                    if (Tot < 0.22f) WashFamily = EGenreFamily::Unknown;
                }
                if (WashFamily == EGenreFamily::RockMetal && I > 0.65f) {
                    const FLinearColor FlameOrange(1.0f, 0.30f, 0.05f);
                    const float FlameMix = FMath::Clamp((I - 0.65f) * 1.5f, 0.f, 0.5f);
                    C = FMath::Lerp(C, FlameOrange, FlameMix);
                }
            }

            float Power = WashIntensityMax * (0.2f + 0.8f * A) * (0.4f + 0.6f * I) * Local * Boost * SceneMul;
            WashLights[i]->SetIntensity(Power);
            WashLights[i]->SetLightColor(C);
        }
    }

    {
        int32 N = BeamLights.Num();
        for (int32 i = 0; i < N; i++) {
            int32 Mirror = (i < N / 2) ? i : (N - 1 - i);
            float Side = (i < N / 2) ? -1.0f : 1.0f;
            float Yaw, Pitch;
            if (bSpotlightFocus) {
                Yaw = 0.0f;
                Pitch = -75.0f;
            } else if (bBurst) {
                Yaw   = FMath::Sin(Time * 6.0f + Mirror) * 35.0f * Side;
                Pitch = -70.0f + FMath::Cos(Time * 5.0f) * 15.0f;
            } else if (CurrentScene == ELightingScene::Chase) {
                Yaw   = FMath::Sin(Time * 2.5f + Mirror * PI * 0.5f) * 40.0f * Side;
                Pitch = -65.0f + FMath::Cos(Time * 1.8f + Mirror) * 18.0f;
            } else if (bBeamStab) {
                // Rock-flavored: beams snap STRAIGHT DOWN, kick drum makes them fire bright.
                // Creates "rain of light columns" effect. Slight micro-pitch wobble so they're
                // not perfectly identical.
                Pitch = -90.0f + FMath::Sin(Time * 4.0f + i * 0.5f) * 3.0f;
                Yaw   = 0.0f;
            } else {
                // Bass-driven yaw amplitude: low bass = beams hold steady, high bass = beams sway harder.
                const float YawAmp = bStrongMusicReactivity
                    ? (15.0f + 50.0f * BassLvl * ReactivityStrength)
                    : 30.0f;
                Yaw   = FMath::Sin(Time * (1.0f + A * 1.5f) + Mirror * PI * 0.5f) * YawAmp * Side;
                Pitch = -70.0f + FMath::Cos(Time * (0.7f + I) + Mirror) * 12.0f;
            }
            BeamLights[i]->SetRelativeRotation(FRotator(Pitch, Yaw, 0.f));

            // Strong reactivity: bass GATES base power (low bass dim, high bass bright),
            // kick adds an additional punch, and snare adds a brief flash.
            float ReactBoost = 1.0f;
            if (bStrongMusicReactivity) {
                const float BassG = (1.0f - BassGateAmount) + BassGateAmount * 2.0f * BassLvl; // 0.3..1.7 at 0.7 amount
                const float KickP = 1.0f + 1.4f * KickFlash * ReactivityStrength;
                const float SnareP = 1.0f + 1.2f * SnareFlash * ReactivityStrength;
                ReactBoost = BassG * KickP * SnareP;
            }
            float Power = BeamIntensityMax * (0.15f + 0.85f * I) * (0.5f + 0.5f * A) * SceneBrightness * ReactBoost;
            if (bBurst) Power *= 1.5f;
            if (bSpotlightFocus) Power *= (i == N / 2 || i == N / 2 - 1) ? 1.5f : 0.1f;
            if (bHeartBeat) {
                // Binary — full bright on kick, near-black between.
                Power *= (KickFlash > 0.05f) ? (0.3f + 1.8f * KickFlash) : 0.05f;
            }
            if (bBeamStab) {
                // Same kick-lock behavior but slightly brighter so the rain effect punches harder.
                Power *= (KickFlash > 0.05f) ? (0.4f + 2.2f * KickFlash) : 0.08f;
            }
            if (bCascade) {
                // Same fast chase as wash, but applied to beam intensity.
                float CPos = FMath::Frac(Time * 4.0f) * N;
                float Dist = FMath::Abs(i - CPos);
                if (Dist > N * 0.5f) Dist = N - Dist;
                Power *= 0.05f + FMath::Exp(-Dist * 3.5f) * 1.8f;
            }
            if (bSparkle) {
                float Phase = FMath::Frac(Time * 5.0f + i * 0.541f);
                Power *= (Phase < 0.10f) ? 1.8f : 0.06f;
            }

            FLinearColor C = (i % 2 == 0) ? SceneColorA : SceneColorB;
            // Snare white-flash overlay so beams pop on the backbeat.
            if (bStrongMusicReactivity && SnareFlash > 0.05f) {
                C = FMath::Lerp(C, FLinearColor::White, SnareFlash * SnareWhiteFlash);
            }
            BeamLights[i]->SetIntensity(Power);
            BeamLights[i]->SetLightColor(C);
        }
    }

    float StrobePhase = FMath::Frac(Time * (1.0f + R * 8.0f));
    float StrobeGate  = (StrobePhase < 0.08f) ? 1.0f : 0.05f;
    if (CurrentScene == ELightingScene::Strobe) StrobeGate = (StrobePhase < 0.15f) ? 1.0f : 0.0f;
    for (int32 i = 0; i < StrobeLights.Num(); i++) {
        float Mood = StrobeIntensityMax * R * StrobeGate * 0.4f;
        float Beat = StrobeIntensityMax * KickFlash;
        if (bBurst) Beat = StrobeIntensityMax * 1.5f;
        if (bSpotlightFocus) { Mood = 0.0f; Beat = 0.0f; }
        StrobeLights[i]->SetIntensity(Mood + Beat);
        StrobeLights[i]->SetLightColor(SceneColorA);
    }

    for (int32 i = 0; i < SideLights.Num(); i++) {
        float Base  = WashIntensityMax * 0.7f * (0.3f + 0.7f * A) * SceneBrightness;
        float Snare = WashIntensityMax * 1.2f * SnareFlash;
        if (bSpotlightFocus) Base *= 0.1f;
        SideLights[i]->SetIntensity(Base + Snare);
        FLinearColor C = FMath::Lerp(SceneColorB, White, SnareFlash);
        SideLights[i]->SetLightColor(C);
    }

    {
        int32 N = FloorLights.Num();
        for (int32 i = 0; i < N; i++) {
            int32 Mirror = (i < N / 2) ? i : (N - 1 - i);
            float Pulse = 0.5f + 0.5f * FMath::Sin(Time * 2.0f + Mirror * PI * 0.5f);
            float Boost = 1.0f + 0.6f * BassLvl + 1.0f * KickFlash + 0.5f * HiHatFlash;
            float Power = FloorIntensityMax * (0.2f + 0.8f * A) * Pulse * Boost * SceneBrightness;
            if (bBurst) Power *= 1.6f;
            FLinearColor C = (i % 2 == 0) ? SceneColorA : SceneColorB;
            FloorLights[i]->SetIntensity(Power);
            FloorLights[i]->SetLightColor(C);
        }
    }

    // Instrument detection: only show meshes for the TOP-N instruments above the threshold.
    // The matching is deliberately conservative — strict piano/keyboard list (no synth pads,
    // no harp, no sampler — those misfire on metal/electronic tracks). Strings and orchestra
    // are ALSO removed from the guitar group because the model often hallucinates them on
    // heavy-distorted guitars, spawning a guitarist mesh that already represents the guitar.
    // InstrumentRoots index mapping: 0=Drum, 1=Mic, 2=Piano, 3=Guitar, 4=Vocalist
    auto ClassifyInstrument = [](const FString& I) -> int32 {
        // Drums / percussion -> index 0
        if (I == TEXT("drums") || I == TEXT("drummachine") || I == TEXT("percussion") ||
            I == TEXT("beat")  || I == TEXT("bongo"))
            return 0;
        // Piano / keyboard / organ -> index 2 (strict acoustic-keyboard family only)
        if (I == TEXT("piano")     || I == TEXT("keyboard")  || I == TEXT("electricpiano") ||
            I == TEXT("rhodes")    || I == TEXT("organ")     || I == TEXT("pipeorgan")     ||
            I == TEXT("accordion"))
            return 2;
        // Guitar / bass / synth -> index 3 (visual proxy: a guitarist character mesh)
        if (I.Contains(TEXT("guitar")) || I == TEXT("synthesizer") ||
            I == TEXT("computer")      || I == TEXT("ukulele"))
            return 3;
        // Bass (electric bass) — also routes to guitar slot since they share a player visually
        if (I == TEXT("bass") || I == TEXT("acousticbassguitar"))
            return 3;
        // Violin / strings family -> index 5
        if (I == TEXT("violin")    || I == TEXT("viola")     || I == TEXT("cello") ||
            I == TEXT("doublebass")|| I == TEXT("strings")   || I == TEXT("orchestra") ||
            I == TEXT("classicalguitar"))
            return 5;
        // Voice / singer / wind -> indexes 1 (mic) AND 4 (vocalist), wind picks mic only
        if (I == TEXT("voice") || I == TEXT("singer"))                                return 1;
        if (I == TEXT("brass")    || I == TEXT("trumpet")   || I == TEXT("trombone") ||
            I == TEXT("horn")     || I == TEXT("saxophone") || I == TEXT("clarinet") ||
            I == TEXT("oboe")     || I == TEXT("flute")     || I == TEXT("harmonica"))
            return 1;
        return -1;
    };

    if (AudioSource && InstrumentVisibilityTimers.Num() == InstrumentRoots.Num()) {
        TArray<FString> INames; TArray<float> IPcts;
        const int32 TopN = FMath::Max(1, InstrumentVisibilityTopN);
        AudioSource->GetTopInstrumentsAny(INames, IPcts, TopN);
        for (int32 k = 0; k < INames.Num() && k < IPcts.Num(); k++) {
            if (IPcts[k] < InstrumentDetectionThreshold) continue;
            const int32 Idx = ClassifyInstrument(INames[k]);
            if (Idx < 0 || Idx >= InstrumentVisibilityTimers.Num()) continue;
            InstrumentVisibilityTimers[Idx] = InstrumentLingerSeconds;
            // "voice" lights up BOTH the mic (1) AND the vocalist mesh (4).
            if (INames[k] == TEXT("voice") || INames[k] == TEXT("singer")) {
                if (InstrumentVisibilityTimers.Num() > 4)
                    InstrumentVisibilityTimers[4] = InstrumentLingerSeconds;
            }
        }
    }

    // The mic stand (index 1) is a stage prop — pin it visible if bAlwaysShowMicStand,
    // since vocal tags rarely cross the detection threshold for instrumental/heavy tracks.
    if (bAlwaysShowMicStand && InstrumentVisibilityTimers.Num() > 1) {
        InstrumentVisibilityTimers[1] = InstrumentLingerSeconds;
    }
    // The vocalist (index 4) — same rationale. Voice tags miss the threshold on
    // instrumental, screamed, growled, or heavily-distorted vocals.
    if (bAlwaysShowVocalist && InstrumentVisibilityTimers.Num() > 4) {
        InstrumentVisibilityTimers[4] = InstrumentLingerSeconds;
    }

    // ----- GENRE-AWARE INSTRUMENT VISIBILITY -----
    // Force the "right" instrument(s) to show based on the detected genre family.
    // Solves the case where, for example, classical music doesn't always get the
    // piano tag above the detection threshold even though piano IS the dominant
    // instrument. Index mapping: 0=Drum, 1=Mic, 2=Piano, 3=Guitar, 4=Vocalist.
    {
        EGenreFamily VisFamily = EGenreFamily::Unknown;
        if (AudioSource) {
            TArray<FString> GN; TArray<float> GP;
            AudioSource->GetTopGenresAny(GN, GP, 15);
            float Tot = 0.f;
            VisFamily = DominantGenreFamily(GN, GP, Tot);
            if (Tot < 0.22f) VisFamily = EGenreFamily::Unknown;
        }
        auto ForceShow = [&](int32 Idx) {
            if (Idx >= 0 && Idx < InstrumentVisibilityTimers.Num()) {
                InstrumentVisibilityTimers[Idx] = InstrumentLingerSeconds;
            }
        };
        auto ForceHide = [&](int32 Idx) {
            if (Idx >= 0 && Idx < InstrumentVisibilityTimers.Num()) {
                InstrumentVisibilityTimers[Idx] = 0.f;
            }
        };
        // STEP A — INDEPENDENT TAG FALLBACK FIRST (only fires SHOW operations).
        // Solves "classical music doesn't trigger Classical family but piano IS dominant".
        // Family override below WINS over this, so e.g. Classical family will still hide
        // drums even if the model has drums confidence at 38%.
        if (AudioSource) {
            TArray<FString> TagNames;
            TArray<float>   TagPcts;
            AudioSource->GetTopInstrumentsAny(TagNames, TagPcts, 15);
            for (int32 k = 0; k < TagNames.Num() && k < TagPcts.Num(); k++) {
                if (TagPcts[k] < 0.18f) continue;
                const FString& Tag = TagNames[k];
                if (Tag == TEXT("piano") || Tag == TEXT("keyboard") || Tag == TEXT("electricpiano") ||
                    Tag == TEXT("rhodes") || Tag == TEXT("organ"))                                     ForceShow(2);
                if (Tag == TEXT("violin") || Tag == TEXT("viola") || Tag == TEXT("cello") ||
                    Tag == TEXT("doublebass") || Tag == TEXT("strings") || Tag == TEXT("orchestra"))   ForceShow(5);
                if (Tag == TEXT("drums") || Tag == TEXT("drummachine") || Tag == TEXT("percussion"))   ForceShow(0);
            }
        }

        // STEP B — FAMILY OVERRIDE (FINAL — wins over tag fallback above).
        switch (VisFamily) {
            case EGenreFamily::Classical:
                ForceShow(2);          // piano always on
                ForceShow(5);          // violin always on
                ForceHide(0);          // drums off
                ForceHide(3);          // guitar off
                break;
            case EGenreFamily::JazzBlues:
                ForceShow(2);          // piano always on
                ForceShow(0);          // jazz often has drums
                ForceHide(3);          // electric guitar uncommon in jazz
                ForceHide(5);          // no violin
                break;
            case EGenreFamily::RockMetal:
                ForceShow(0); ForceShow(3); ForceHide(2); ForceHide(5);
                break;
            case EGenreFamily::HipHopRap:
                ForceShow(0); ForceHide(2); ForceHide(3); ForceHide(5);
                break;
            case EGenreFamily::Electronic:
                ForceHide(0); ForceHide(2); ForceHide(3); ForceHide(5);
                break;
            case EGenreFamily::Pop:
            case EGenreFamily::SoulFunk:
                ForceShow(0); ForceShow(3); ForceShow(2);
                break;
            default:
                break;
        }
    }

    // Decrement timers, set visibility on each instrument root accordingly.
    for (int32 i = 0; i < InstrumentRoots.Num() && i < InstrumentVisibilityTimers.Num(); i++) {
        InstrumentVisibilityTimers[i] = FMath::Max(0.0f, InstrumentVisibilityTimers[i] - DeltaTime);
        if (InstrumentRoots[i]) {
            const bool bShow = InstrumentVisibilityTimers[i] > 0.0f;
            InstrumentRoots[i]->SetVisibility(bShow, true);
        }
    }

    // ---------------- DYNAMIC INSTRUMENT PLACEMENT ----------------
    // Count visible instruments. If FEWER are visible (e.g. just piano + vocalist for
    // classical), compress their X positions toward stage center so they don't look
    // scattered on the 80m-wide stage. If MORE are visible, spread them out.
    if (bDynamicInstrumentPlacement
        && InstrumentRoots.Num() == InstrumentBasePositions.Num()
        && InstrumentRoots.Num() == InstrumentVisibilityTimers.Num())
    {
        // Count "non-prop" visible instruments (exclude index 1 = mic which is just a stand).
        int32 NumVisible = 0;
        for (int32 i = 0; i < InstrumentVisibilityTimers.Num(); i++) {
            if (i == 1) continue;   // mic stand isn't "performer-driven"
            if (InstrumentVisibilityTimers[i] > 0.0f) NumVisible++;
        }
        // Compression factor: 5+ visible = 1.0 (full spread), fewer = compress to center.
        //   5 = 1.00, 4 = 0.85, 3 = 0.70, 2 = 0.50, 1 = 0.0 (dead center)
        float Compress;
        switch (NumVisible) {
            case 0: case 1: Compress = 0.0f; break;
            case 2:         Compress = 0.50f; break;
            case 3:         Compress = 0.70f; break;
            case 4:         Compress = 0.85f; break;
            default:        Compress = 1.0f; break;
        }
        const float SmoothFactor = FMath::Clamp(InstrumentPlacementSmoothing, 0.0f, 0.95f);
        for (int32 i = 0; i < InstrumentRoots.Num(); i++) {
            if (!InstrumentRoots[i]) continue;
            const FVector Base = InstrumentBasePositions[i];
            // Target X compressed toward center; Y/Z unchanged so depth layout is preserved.
            const FVector Target(Base.X * Compress, Base.Y, Base.Z);
            const FVector Cur = InstrumentRoots[i]->GetRelativeLocation();
            const FVector NewPos = FMath::Lerp(Target, Cur, SmoothFactor);
            InstrumentRoots[i]->SetRelativeLocation(NewPos);
        }
    }

    // On-screen debug: show what the lighting code "sees" — visible meshes + the top-N
    // instrument tags that drove the decision. Toggle via bShowInstrumentDebug in editor.
    if (bShowInstrumentDebug && GEngine) {
        static const TCHAR* Names[] = { TEXT("DRUMS"), TEXT("MIC"), TEXT("PIANO"), TEXT("GUITAR"), TEXT("VOCALIST"), TEXT("VIOLIN") };
        FString Visible;
        for (int32 i = 0; i < InstrumentVisibilityTimers.Num() && i < 6; i++) {
            const bool bV = InstrumentVisibilityTimers[i] > 0.0f;
            const bool bMeshOK = (i < bInstrumentMeshLoaded.Num()) && bInstrumentMeshLoaded[i];
            const TCHAR* Status = bV ? (bMeshOK ? TEXT("ON") : TEXT("ON-NOMESH!")) : TEXT("off");
            Visible += FString::Printf(TEXT("%s=%s "), Names[i], Status);
        }
        GEngine->AddOnScreenDebugMessage(50, 0.0f, FColor::Cyan,
            FString::Printf(TEXT("[CSD] Instruments: %s"), *Visible));

        // World-space wireframe boxes at each instrument's actual world position so the
        // user can SEE if the drum kit is sitting somewhere they're not looking at,
        // OR if the mesh is missing (red box = no mesh attached, green = mesh attached).
        UWorld* W = GetWorld();
        if (W) {
            for (int32 i = 0; i < InstrumentRoots.Num() && i < 6; i++) {
                if (!InstrumentRoots[i]) continue;
                const FVector Pos = InstrumentRoots[i]->GetComponentLocation();
                const bool bMeshOK = (i < bInstrumentMeshLoaded.Num()) && bInstrumentMeshLoaded[i];
                const FColor Col = bMeshOK ? FColor::Green : FColor::Red;
                DrawDebugBox(W, Pos, FVector(150.f, 150.f, 200.f), Col, false, 0.0f, 0, 6.0f);
                DrawDebugString(W, Pos + FVector(0, 0, 350.f),
                    FString::Printf(TEXT("%s %s"), Names[i], bMeshOK ? TEXT("(mesh OK)") : TEXT("(NO MESH)")),
                    nullptr, Col, 0.0f, true, 1.5f);
            }
        }

        if (AudioSource) {
            TArray<FString> DbgINames; TArray<float> DbgIPcts;
            AudioSource->GetTopInstrumentsAny(DbgINames, DbgIPcts, 6);
            FString TopList;
            for (int32 k = 0; k < DbgINames.Num() && k < DbgIPcts.Num() && k < 6; k++) {
                TopList += FString::Printf(TEXT("%s %d%% "), *DbgINames[k], FMath::RoundToInt(DbgIPcts[k] * 100.0f));
            }
            GEngine->AddOnScreenDebugMessage(51, 0.0f, FColor::Yellow,
                FString::Printf(TEXT("[CSD] Top instruments seen: %s"), *TopList));
        }
    }

    if (InstrumentLights.Num() >= 5) {
        // Boost instrument lights based on detected instruments. Reads top-N from sidecar
        // OR local ONNX (whichever is available) so this works without UDP.
        float DrumBoost = 1.0f, MicBoost = 1.0f, PianoBoost = 1.0f, GuitarBoost = 1.0f, VocalBoost = 1.0f;
        if (AudioSource) {
            TArray<FString> INames; TArray<float> IPcts;
            AudioSource->GetTopInstrumentsAny(INames, IPcts, 10);
            for (int32 k = 0; k < INames.Num() && k < IPcts.Num(); k++) {
                if (IPcts[k] < 0.30f) continue;
                const FString& Inst = INames[k];
                if (Inst == TEXT("drums") || Inst == TEXT("drummachine") ||
                    Inst == TEXT("percussion") || Inst == TEXT("beat"))                                  DrumBoost   = FMath::Max(DrumBoost,   1.8f);
                else if (Inst == TEXT("piano") || Inst == TEXT("keyboard") ||
                         Inst == TEXT("electricpiano") || Inst == TEXT("organ") ||
                         Inst == TEXT("rhodes"))                                                          PianoBoost  = FMath::Max(PianoBoost,  1.8f);
                else if (Inst.Contains(TEXT("guitar")) || Inst.Contains(TEXT("bass")))                    GuitarBoost = FMath::Max(GuitarBoost, 1.8f);
                else if (Inst == TEXT("voice") || Inst == TEXT("singer"))                                 { VocalBoost = FMath::Max(VocalBoost, 1.8f); MicBoost = FMath::Max(MicBoost, 1.5f); }
                else if (Inst == TEXT("synthesizer") || Inst == TEXT("computer"))                         GuitarBoost = FMath::Max(GuitarBoost, 1.4f);
            }
        }

        float DrumP = 28000.f * DrumBoost * (0.2f + 1.6f * KickFlash + 0.5f * BassLvl);
        InstrumentLights[0]->SetIntensity(DrumP);
        InstrumentLights[0]->SetLightColor(FLinearColor(1.0f, 0.15f, 0.10f));

        float MicP = 20000.f * MicBoost * (0.15f + 0.85f * MidLvl + 0.5f * SnareFlash);
        InstrumentLights[1]->SetIntensity(MicP);
        InstrumentLights[1]->SetLightColor(FLinearColor(1.0f, 0.85f, 0.6f));

        float PianoP = 24000.f * PianoBoost * (0.15f + 0.75f * MidLvl + 0.35f * TrebleLvl);
        InstrumentLights[2]->SetIntensity(PianoP);
        InstrumentLights[2]->SetLightColor(FLinearColor(0.80f, 0.80f, 1.0f));

        float RoryP = 26000.f * GuitarBoost * (0.1f + 0.85f * TrebleLvl + 1.6f * HiHatFlash + 0.3f * MidLvl);
        InstrumentLights[3]->SetIntensity(RoryP);
        InstrumentLights[3]->SetLightColor(SceneColorA);

        float VocalP = 22000.f * VocalBoost * (0.15f + 0.85f * MidLvl + 0.5f * SnareFlash);
        InstrumentLights[4]->SetIntensity(VocalP);
        InstrumentLights[4]->SetLightColor(FLinearColor(1.0f, 0.85f, 0.6f));
    }

    // Audience lights: gently follow arousal/intensity, plus brief flash on strong onsets so the crowd zone feels alive.
    {
        const int32 N = AudienceLights.Num();
        const float CrowdEnergy = (0.4f + 0.6f * A) * (0.5f + 0.5f * I);
        const float CrowdFlash  = 1.0f * KickFlash + 0.6f * SnareFlash;
        for (int32 i = 0; i < N; i++) {
            float SceneMul = SceneBrightness;
            if (bBurst) SceneMul *= 1.4f;
            if (bSpotlightFocus) SceneMul *= 0.2f;
            float Power = AudienceFloorIntensityMax * (CrowdEnergy + CrowdFlash) * SceneMul;
            FLinearColor C = (i % 2 == 0) ? SceneColorA : SceneColorB;
            AudienceLights[i]->SetIntensity(Power);
            AudienceLights[i]->SetLightColor(C);
        }
    }

    // Dancing crowd: sway side-to-side with arousal, jump on kick drum, each person has unique timing.
    // Amplitudes bumped substantially — the previous values were too subtle to read on camera.
    if (AudienceMeshes.Num() == AudienceBasePositions.Num()) {
        const float SwayAmount      = 25.0f * A;                                      // 25cm sway at peak arousal (was 8)
        const float SwaySpeed       = 2.0f + 1.5f * R;
        const float BeatJumpHeight  = (90.0f * KickFlash + 35.0f * SnareFlash);       // up to 90cm jump on kicks (was 35)
        const float HiHatBob        = 12.0f * HiHatFlash;                             // (was 4)
        const float BassBob         = 18.0f * BassLvl;                                // NEW — continuous bobbing with bass loudness

        for (int32 i = 0; i < AudienceMeshes.Num(); i++) {
            if (!AudienceMeshes[i]) continue;
            const float Phase = AudiencePhases[i];
            const float Jumpy = AudienceJumpiness[i];

            const float SwayX = FMath::Sin(Time * SwaySpeed + Phase) * SwayAmount;
            const float SwayY = FMath::Cos(Time * SwaySpeed * 0.7f + Phase) * SwayAmount * 0.4f;
            // Continuous bass-driven bob (sin wave at slow bass rate) + discrete kick/snare jumps + hi-hat micro-bob
            const float ContinuousBob = BassBob * FMath::Sin(Time * 4.5f + Phase * 1.3f);
            const float JumpZ = (BeatJumpHeight + HiHatBob + ContinuousBob) * Jumpy * (0.7f + 0.3f * FMath::Sin(Phase * 3.0f));

            AudienceMeshes[i]->SetRelativeLocation(AudienceBasePositions[i] + FVector(SwayX, SwayY, JumpZ));

            // Twist and tilt rotation for more organic feel
            const float TwistDeg = FMath::Sin(Time * SwaySpeed + Phase * 1.3f) * 15.0f * A;
            const float TiltDeg  = FMath::Cos(Time * SwaySpeed * 0.9f + Phase) * 4.0f * BassLvl;
            AudienceMeshes[i]->SetRelativeRotation(FRotator(TiltDeg, TwistDeg, 0.f));
        }
    }

    // Disco ball: continuous rotation, speed-modulated by energy mode (Calm = slow,
    // Peak = fast). Lives with the rest of the beam rig under DiscoBallPivot.
    float DiscoSpeedMul = 1.0f;
    if (bEnergyModeReact) {
        switch (CurrentEnergyMode) {
        case EEnergyMode::Calm: DiscoSpeedMul = CalmDiscoSpeedMul; break;
        case EEnergyMode::Peak: DiscoSpeedMul = PeakDiscoSpeedMul; break;
        default: break;
        }
    }
    const float EffectiveDiscoSpeed = DiscoBallRotationSpeed * DiscoSpeedMul;
    if (DiscoBallPivot) {
        DiscoBallPivot->AddRelativeRotation(FRotator(0.f, EffectiveDiscoSpeed * DeltaTime, 0.f));
    } else if (DiscoBallMesh) {
        DiscoBallMesh->AddRelativeRotation(FRotator(0.f, EffectiveDiscoSpeed * DeltaTime, 0.f));
    }

    // Disco ball beams: pulse intensity with bass / kick. When bUnifiedColorPalette is on,
    // the beams sweep around SceneColorA's hue (rock=red shades, electronic=magenta shades)
    // matching the concert wash lights. Otherwise they cycle full HSV (legacy rainbow look).
    if (DiscoBallBeams.Num() > 0) {
        const float BeamBeatBoost = 1.0f + 0.6f * BassLvl + 1.0f * KickFlash;
        const float HueShift = Time * 0.07f;   // slow color drift
        const int32 N = DiscoBallBeams.Num();

        const bool bUnify = bUnifiedColorPalette && CurrentScene != ELightingScene::Rainbow;
        const FLinearColor SceneAHSV = SceneColorA.LinearRGBToHSV();
        const float BaseHueDeg = SceneAHSV.R;
        // Disco ball gets a slightly tighter spread than dance floor — enough to vary
        // but still readable as "the same color family".
        const float SpreadDeg = UnifiedHueSpread * 0.7f;

        for (int32 i = 0; i < N; i++) {
            USpotLightComponent* Beam = DiscoBallBeams[i];
            if (!Beam) continue;
            FLinearColor C;
            if (bUnify) {
                const float Phase = ((float)i / (float)N) * 2.0f * (float)PI + Time * 0.5f;
                const float HueDeg = FMath::Fmod(BaseHueDeg + SpreadDeg * FMath::Sin(Phase) + 360.0f, 360.0f);
                C = FLinearColor(HueDeg, 0.95f, 1.0f).HSVToLinearRGB();
            } else {
                const float Hue = FMath::Frac((float)i / (float)N + HueShift);
                C = FLinearColor::MakeFromHSV8((uint8)(Hue * 255), 230, 255);
            }
            Beam->SetLightColor(C);
            Beam->SetIntensity(DiscoBallBeamIntensity * BeamBeatBoost);
        }
    }

    // Dance floor RGB. Either FREE rainbow (legacy) or UNIFIED — driven by SceneColorA hue
    // so the floor matches whatever the concert lights are doing (rock = red/orange tiles,
    // electronic = magenta/cyan tiles). Each tile still gets a per-tile phase offset so
    // the floor isn't all one solid color — it's variation AROUND the genre hue.
    if (DanceFloorLights.Num() > 0 && DanceFloorPhaseOffsets.Num() == DanceFloorLights.Num()) {
        const float Cycle = FMath::Max(0.5f, DanceFloorHueCycleSeconds);
        const float BeatBoost = 1.0f + 1.5f * KickFlash + 0.7f * SnareFlash;
        const float EnergyMul = 0.5f + 0.5f * I + 0.4f * BassLvl;

        const bool bUnify = bUnifiedColorPalette && CurrentScene != ELightingScene::Rainbow;
        // Convert SceneColorA to HSV so we can use its hue (degrees 0..360) as the base.
        const FLinearColor SceneAHSV = SceneColorA.LinearRGBToHSV();
        const float BaseHueDeg = SceneAHSV.R;
        const float SpreadDeg  = UnifiedHueSpread;

        for (int32 i = 0; i < DanceFloorLights.Num(); i++) {
            if (!DanceFloorLights[i]) continue;
            FLinearColor C;
            if (bUnify) {
                // Each tile oscillates around BaseHueDeg by +/-SpreadDeg, with its own
                // phase so adjacent tiles look different. Cycle period stays the same.
                const float Phase = DanceFloorPhaseOffsets[i] * 2.0f * (float)PI + Time * (2.0f * (float)PI / Cycle);
                const float HueDeg = FMath::Fmod(BaseHueDeg + SpreadDeg * FMath::Sin(Phase) + 360.0f, 360.0f);
                C = FLinearColor(HueDeg, 0.95f, 1.0f).HSVToLinearRGB();
            } else {
                const float Hue = FMath::Frac(Time / Cycle + DanceFloorPhaseOffsets[i]);
                C = FLinearColor::MakeFromHSV8((uint8)(Hue * 255), 230, 255);
            }
            DanceFloorLights[i]->SetLightColor(C);
            DanceFloorLights[i]->SetIntensity(DanceFloorLightIntensity * BeatBoost * EnergyMul);
        }
    }

    // === GLOBAL COORDINATED MULTIPLIERS ===
    // SilenceFade        — pull everything down to 0 when no audio is playing.
    // BassFlashMul       — when bass spikes, EVERY light flares together (open/close on bass).
    // SoloDim            — when one instrument dominates, dim everything else.
    // ModeBrightnessMul  — Calm = 0.55x, Normal = 1x, Peak = 1.35x  (sustained per-section vibe).
    // PeakFlashMul       — one-shot bright flare (~0.7s) when ENTERING Peak from Calm/Normal.
    const float ModeMul    = ModeBrightnessMul * PeakFlashMul;
    const float NonSoloMul = SilenceFade * BassFlashMul * SoloDim   * ModeMul;
    const float DanceMul   = SilenceFade * BassFlashMul * (bSoloMode ? SoloDimAmount * 0.6f : 1.0f) * ModeMul;
    const float DiscoMul   = SilenceFade * BassFlashMul * (bSoloMode ? 0.4f : 1.0f)                * ModeMul;

    for (USpotLightComponent* L : WashLights)             if (L) L->SetIntensity(L->Intensity * NonSoloMul);
    for (USpotLightComponent* L : BeamLights)             if (L) L->SetIntensity(L->Intensity * NonSoloMul);
    for (URectLightComponent* L : StrobeLights)           if (L) L->SetIntensity(L->Intensity * NonSoloMul);
    for (USpotLightComponent* L : SideLights)             if (L) L->SetIntensity(L->Intensity * NonSoloMul);
    for (UPointLightComponent* L : FloorLights)           if (L) L->SetIntensity(L->Intensity * NonSoloMul);
    for (USpotLightComponent* L : AudienceLights)         if (L) L->SetIntensity(L->Intensity * NonSoloMul);
    for (UPointLightComponent* L : DanceFloorLights)      if (L) L->SetIntensity(L->Intensity * DanceMul);
    for (USpotLightComponent* L : DiscoBallBeams)         if (L) L->SetIntensity(L->Intensity * DiscoMul);
    for (USpotLightComponent* L : LaserLights)            if (L) L->SetIntensity(L->Intensity * NonSoloMul);
    for (USpotLightComponent* L : AudienceCeilingLights)  if (L) L->SetIntensity(L->Intensity * NonSoloMul);

    // Instrument lights: the soloist gets a dedicated boost; the rest also dim slightly so
    // the contrast is dramatic (entire stage darker, soloist's spotlight pops).
    for (int32 i = 0; i < InstrumentLights.Num(); i++) {
        if (!InstrumentLights[i]) continue;
        const float Mul = SilenceFade * BassFlashMul * ModeMul *
                          ((i == SoloIndex) ? SoloBoost : (bSoloMode ? SoloDimAmount : 1.0f));
        InstrumentLights[i]->SetIntensity(InstrumentLights[i]->Intensity * Mul);
    }

    // ----- GENRE TEXT -----
    // Update the floating "ROCK"/"ELECTRONIC"/etc. text shown in front of the center LED panel.
    if (GenreTextRender) {
        EGenreFamily TextFamily = EGenreFamily::Unknown;
        if (AudioSource) {
            TArray<FString> GN; TArray<float> GP;
            AudioSource->GetTopGenresAny(GN, GP, 15);
            float Tot = 0.f;
            TextFamily = DominantGenreFamily(GN, GP, Tot);
            if (Tot < 0.22f) TextFamily = EGenreFamily::Unknown;
        }
        const TCHAR* Label = TEXT("");
        switch (TextFamily) {
            case EGenreFamily::RockMetal:    Label = TEXT("ROCK");      break;
            case EGenreFamily::Electronic:   Label = TEXT("ELECTRONIC");break;
            case EGenreFamily::Classical:    Label = TEXT("CLASSICAL"); break;
            case EGenreFamily::AmbientChill: Label = TEXT("AMBIENT");   break;
            case EGenreFamily::JazzBlues:    Label = TEXT("JAZZ");      break;
            case EGenreFamily::HipHopRap:    Label = TEXT("HIP-HOP");   break;
            case EGenreFamily::Pop:          Label = TEXT("POP");       break;
            case EGenreFamily::SoulFunk:     Label = TEXT("FUNK");      break;
            case EGenreFamily::ReggaeSka:    Label = TEXT("REGGAE");    break;
            case EGenreFamily::CountryFolk:  Label = TEXT("FOLK");      break;
            case EGenreFamily::LatinWorld:   Label = TEXT("WORLD");     break;
            default:                         Label = TEXT(""); break;
        }
        GenreTextRender->SetText(FText::FromString(FString(Label)));
        // Tint text with scene color and pulse brightness with bass
        const float Pulse = 0.6f + 0.6f * BassLvl;
        const FLinearColor TextColor = SceneColorA * Pulse;
        GenreTextRender->SetTextRenderColor(TextColor.ToFColorSRGB());
    }

    // ----- SIDE LED BARS -----
    // Pulse with bass + kick. Left tower bars use SceneColorA, right tower use SceneColorB
    // so the two sides of the stage have contrasting colors visible from the audience.
    if (SideLedBars.Num() > 0) {
        const int32 N = SideLedBars.Num();
        const float Pump = 0.4f + 0.7f * BassLvl + 1.0f * KickFlash;
        for (int32 i = 0; i < N; i++) {
            UStaticMeshComponent* B = SideLedBars[i];
            if (!B) continue;
            // First half = left tower, second half = right tower
            const bool bLeft = i < (N / 2);
            const FLinearColor BarColor = bLeft ? SceneColorA : SceneColorB;
            const FLinearColor Emissive = BarColor * SideLedEmissiveStrength * Pump;
            if (UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(B->GetMaterial(0))) {
                MID->SetVectorParameterValue(TEXT("Color"),    BarColor * 0.25f);
                MID->SetVectorParameterValue(TEXT("Emissive"), Emissive);
            }
        }
    }

    // ----- DJ BOOTH -----
    // Visibility gated to Electronic family. Front panel pulses with bass + kick.
    if (DJBoothBody && DJBoothPanel) {
        EGenreFamily BoothFamily = EGenreFamily::Unknown;
        if (AudioSource) {
            TArray<FString> GN; TArray<float> GP;
            AudioSource->GetTopGenresAny(GN, GP, 15);
            float Tot = 0.f;
            BoothFamily = DominantGenreFamily(GN, GP, Tot);
            if (Tot < 0.22f) BoothFamily = EGenreFamily::Unknown;
        }
        const bool bShow = (BoothFamily == EGenreFamily::Electronic);
        DJBoothBody->SetVisibility(bShow);
        DJBoothPanel->SetVisibility(bShow);

        if (bShow) {
            // Panel pulses with bass + scene color hue
            const float Pump = 0.5f + 0.8f * BassLvl + 0.7f * KickFlash;
            const FLinearColor PanelColor = SceneColorA;
            const FLinearColor Emissive = PanelColor * DJBoothEmissiveStrength * Pump;
            if (UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(DJBoothPanel->GetMaterial(0))) {
                MID->SetVectorParameterValue(TEXT("Color"),    PanelColor * 0.3f);
                MID->SetVectorParameterValue(TEXT("Emissive"), Emissive);
            }
        }
    }

    // ----- LED VIDEO PANELS -----
    // Each panel pulses with bass, cycles hue around the scene palette, and gets a
    // strong white flash on snare hits. Different panels are offset in hue so the
    // backdrop reads as multiple "screens" rather than a single block.
    if (LedPanels.Num() > 0)
    {
        const int32 N = LedPanels.Num();
        // Base color = scene color, adjusted by bass for brightness, snare for white-flash
        const float BassPump  = 0.6f + 0.8f * BassLvl + 0.6f * KickFlash;
        const float SnareFlsh = SnareFlash * 0.8f;

        for (int32 i = 0; i < N; i++) {
            UStaticMeshComponent* M = LedPanels[i];
            if (!M) continue;
            UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(M->GetMaterial(0));
            if (!MID) continue;

            // Per-panel hue offset spreads colors across panels (e.g. left=red, center=orange, right=yellow)
            const float HueShift = (N > 1) ? (((float)i / (float)(N - 1)) - 0.5f) * 2.f * LedPanelHueSpread : 0.f;
            // Time-based hue drift so even a single panel cycles colors
            const float HueDrift = FMath::Sin(Time * 0.4f + i * 0.7f) * 15.f;
            FLinearColor PanelColor = ShiftHue(SceneColorA, HueShift + HueDrift);

            // Mix toward white on snare for flash punctuation
            if (SnareFlsh > 0.05f) {
                PanelColor = FMath::Lerp(PanelColor, FLinearColor::White, SnareFlsh);
            }

            const FLinearColor Emissive = PanelColor * LedPanelEmissiveStrength * BassPump;
            MID->SetVectorParameterValue(TEXT("Color"),    PanelColor * 0.4f);   // base albedo darker so emissive dominates
            MID->SetVectorParameterValue(TEXT("Emissive"), Emissive);
        }
    }

    // ----- 3D SPECTRUM ANALYZER -----
    // Animate per-bar height based on audio band amplitude + per-bar phase noise.
    // Bars 0..N/3 = bass, N/3..2N/3 = mids, 2N/3..N = treble.
    if (SpectrumBars.Num() > 0 && SpectrumBars.Num() == SpectrumBarCurrentHeights.Num())
    {
        const int32 N = SpectrumBars.Num();
        const float DeckZ = (bSpawnStageStructure && bSpawnStagePlatform) ? StagePlatformHeight : 0.f;
        const float Smoothing = FMath::Clamp(SpectrumBarSmoothing, 0.f, 0.95f);
        const float OneThird  = N / 3.0f;
        const float TwoThirds = 2.0f * N / 3.0f;

        for (int32 i = 0; i < N; i++) {
            // Pick which band drives this bar (with a smooth transition near the boundaries)
            float Band;
            if (i < OneThird)      Band = BassLvl;
            else if (i < TwoThirds) Band = MidLvl;
            else                    Band = TrebleLvl;

            // Per-bar phase wobble so bars in the same band don't move identically
            // (real FFTs have unique values per bin even when frequency content is similar)
            const float Hash = FMath::Frac(FMath::Sin((float)i * 12.9898f) * 43758.5453f);
            const float Phase = Hash * 2.0f * (float)PI;
            const float Wobble = 0.55f + 0.45f * FMath::Sin(Time * 6.0f + Phase + i * 0.7f);

            // Kick-driven extra punch on bass bars; snare on mid bars; hi-hat on treble bars
            float BeatPunch = 1.0f;
            if (i < OneThird)       BeatPunch += KickFlash * 0.8f;
            else if (i < TwoThirds) BeatPunch += SnareFlash * 0.6f;
            else                    BeatPunch += HiHatFlash * 0.5f;

            // Target height = band amplitude × wobble × beat punch × max height
            const float Target = FMath::Clamp(Band * Wobble * BeatPunch, 0.f, 1.2f) * SpectrumBarMaxHeight;
            SpectrumBarCurrentHeights[i] = FMath::Lerp(Target, SpectrumBarCurrentHeights[i], Smoothing);

            const float H = FMath::Max(20.f, SpectrumBarCurrentHeights[i]);
            UStaticMeshComponent* M = SpectrumBars[i];
            if (!M) continue;

            // Update transform: scale Z to bar height, slide bar up so BOTTOM stays at deck
            const FVector Pos = M->GetRelativeLocation();
            M->SetRelativeLocation(FVector(Pos.X, Pos.Y, DeckZ + H * 0.5f));
            M->SetRelativeScale3D(FVector(SpectrumBarWidth / 100.f, SpectrumBarWidth / 100.f, H / 100.f));

            // Color: bass=red gradient, mid=yellow/green gradient, treble=cyan/blue gradient
            // Also blend with scene color so the analyzer reads the same palette as the lights
            FLinearColor BandColor;
            if (i < OneThird) {
                BandColor = FLinearColor(1.0f, 0.15f, 0.10f);        // red
            } else if (i < TwoThirds) {
                BandColor = FLinearColor(1.0f, 0.85f, 0.20f);        // yellow
            } else {
                BandColor = FLinearColor(0.20f, 0.85f, 1.0f);        // cyan
            }
            // 50/50 blend with the scene's dominant color so the analyzer obeys the palette
            BandColor = FMath::Lerp(BandColor, SceneColorA, 0.5f);

            const float NormH = H / FMath::Max(50.f, SpectrumBarMaxHeight);
            const FLinearColor Emissive = BandColor * SpectrumBarEmissiveStrength * (0.4f + 1.2f * NormH);

            if (UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(M->GetMaterial(0))) {
                MID->SetVectorParameterValue(TEXT("Color"),    BandColor * 0.3f);
                MID->SetVectorParameterValue(TEXT("Emissive"), Emissive);
            }
        }
    }

    // ----- SPEAKER CONE VIBRATION -----
    // PA stacks visibly "thump" — Y-axis scale oscillates around base so the speaker face
    // appears to pump in/out with the bass + kick. Detail you don't notice consciously but
    // makes the scene feel responsive.
    if (SpeakerMeshes.Num() > 0 && SpeakerMeshes.Num() == SpeakerBaseScales.Num()) {
        const float BassPump = 1.0f + 0.06f * BassLvl + 0.10f * KickFlash;
        for (int32 i = 0; i < SpeakerMeshes.Num(); i++) {
            if (!SpeakerMeshes[i]) continue;
            const FVector Base = SpeakerBaseScales[i];
            // Y axis = the "front face" of the speaker — pump that with bass for a thump effect.
            const float Wob = 1.0f + 0.02f * FMath::Sin(Time * 15.0f + (float)i * 0.7f);
            SpeakerMeshes[i]->SetRelativeScale3D(FVector(Base.X, Base.Y * BassPump * Wob, Base.Z));
        }
    }

    // ----- STAGE HAZE -----
    // Drift each haze puff in a slow noise pattern and tint it slightly toward scene color
    // so the room atmosphere matches the active palette. Pulse subtly with bass for "breathing".
    if (HazePuffs.Num() > 0 && HazePuffs.Num() == HazePuffBasePositions.Num())
    {
        const float DriftAmp = HazeDriftAmplitude;
        const float DriftSpeed = HazeDriftSpeed;
        const float BassMul = 0.7f + 0.6f * BassLvl;
        for (int32 i = 0; i < HazePuffs.Num(); i++) {
            UPointLightComponent* L = HazePuffs[i];
            if (!L) continue;
            const FVector Base = HazePuffBasePositions[i];
            // Per-puff phase so they drift independently
            const float Ph = (float)i * 0.541f;
            const FVector Offset(
                DriftAmp * FMath::Sin(Time * DriftSpeed       + Ph),
                DriftAmp * FMath::Cos(Time * DriftSpeed * 1.3f + Ph * 1.7f),
                DriftAmp * 0.4f * FMath::Sin(Time * DriftSpeed * 0.7f + Ph * 0.3f)
            );
            L->SetRelativeLocation(Base + Offset);
            // Tint subtly toward scene color so room atmosphere shifts with the palette
            const FLinearColor Tint = FMath::Lerp(FLinearColor(0.7f, 0.7f, 0.8f), SceneColorA, 0.35f);
            L->SetLightColor(Tint);
            L->SetIntensity(HazePuffIntensity * BassMul);
        }
    }

    // ----- LASER lights -----
    // Bright + active ONLY on Cascade/Sparkle/Burst scenes, otherwise nearly dark. Sweep
    // very fast (3x beam-light speed) and stay in a tight pencil-thin shape.
    if (LaserLights.Num() > 0) {
        const bool bLasersFire = (CurrentScene == ELightingScene::Cascade)
                              || (CurrentScene == ELightingScene::Sparkle)
                              || (CurrentScene == ELightingScene::Burst);
        const float LaserBase = bLasersFire ? (45000.f + 25000.f * BassLvl) : 200.f;
        // CROSS PATTERN MODE: during Burst scenes, lasers form an X by aiming inward
        // toward the center. Also activates briefly during big kick hits on Cascade.
        const bool bCrossMode = bLaserCrossPattern &&
                                ((CurrentScene == ELightingScene::Burst)
                                 || (CurrentScene == ELightingScene::Cascade && KickFlash > 0.6f));
        const int32 LN = LaserLights.Num();
        for (int32 i = 0; i < LN; i++) {
            USpotLightComponent* L = LaserLights[i];
            if (!L) continue;
            float Pitch, Yaw;
            if (bCrossMode) {
                // Aim each laser toward the OPPOSITE side at audience-floor level so they cross.
                const FVector LaserPos = L->GetRelativeLocation();
                const float NormX = LaserPos.X / FMath::Max(1.f, StageWidth * 0.5f);   // -1..+1
                Yaw = -NormX * 55.f;    // negate so positive-X lasers aim toward negative X
                Pitch = -50.f + FMath::Sin(Time * 6.f + i * 0.3f) * 5.f;   // small wobble
            } else {
                const float SideMul = (i % 2 == 0) ? 1.0f : -1.0f;
                const float YawAmp  = 40.f + 30.f * BassLvl;
                const float Speed   = 3.0f + 1.5f * I;
                Yaw   = FMath::Sin(Time * Speed + i * 0.7f) * YawAmp * SideMul;
                Pitch = -65.f + FMath::Cos(Time * (Speed * 0.6f) + i * 0.4f) * 18.f;
            }
            L->SetRelativeRotation(FRotator(Pitch, Yaw, 0.f));
            // Saturated bright color — alternate between the scene's two colors.
            FLinearColor C = (i % 2 == 0) ? SceneColorA : SceneColorB;
            // Lasers in real concerts are saturated jewel tones; punch up saturation.
            FLinearColor Hsv = C.LinearRGBToHSV();
            Hsv.G = FMath::Min(255.f, Hsv.G * 1.3f);  // boost saturation
            C = Hsv.HSVToLinearRGB();
            L->SetIntensity(LaserBase * (1.0f + 0.6f * KickFlash));
            L->SetLightColor(C);
        }
    }

    // ----- AUDIENCE CEILING lights -----
    // Off (5% intensity) most of the time. Pulse bright on Burst/Cascade/Sparkle scenes
    // OR on kick onsets during Strobe/HeartBeat. Activates 4-5 distinct visual moments per song.
    if (AudienceCeilingLights.Num() > 0) {
        const bool bAudienceActive = (CurrentScene == ELightingScene::Burst)
                                  || (CurrentScene == ELightingScene::Cascade)
                                  || (CurrentScene == ELightingScene::Sparkle)
                                  || (CurrentScene == ELightingScene::HeartBeat);
        const float BaseMul = bAudienceActive ? 1.0f : 0.05f;
        const int32 N = AudienceCeilingLights.Num();
        for (int32 i = 0; i < N; i++) {
            USpotLightComponent* L = AudienceCeilingLights[i];
            if (!L) continue;
            // Add per-light wobble so they don't all pulse identically.
            const float Wobble = 0.7f + 0.3f * FMath::Sin(Time * 2.0f + i * 0.5f);
            // Strong kick punch makes the crowd lights pump with the bass.
            const float Punch = 1.0f + 1.6f * KickFlash;
            const float Power = 35000.f * BaseMul * Wobble * Punch * (0.4f + 0.6f * I);
            L->SetIntensity(Power);
            // Alternate colors across the array so the audience isn't monochrome.
            const FLinearColor C = (i % 2 == 0) ? SceneColorA : SceneColorB;
            L->SetLightColor(C);
        }
    }

    // Update emissive color/intensity on visible light fixtures so the PAR-can housings
    // glow the same color as the light they represent. Pulses brightness with light intensity.
    if (bSpawnLightFixtures && FixtureEmissiveStrength > 0.f) {
        auto UpdateFixtureMID = [&](UStaticMeshComponent* Fixture, const FLinearColor& BaseColor, float NormIntensity) {
            if (!Fixture) return;
            UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(Fixture->GetMaterial(0));
            if (!MID) return;
            const FLinearColor Emissive = BaseColor * FixtureEmissiveStrength * FMath::Clamp(NormIntensity, 0.02f, 5.0f);
            MID->SetVectorParameterValue(TEXT("Emissive"), Emissive);
            MID->SetVectorParameterValue(TEXT("Color"),    BaseColor * 0.3f);
        };

        for (int32 i = 0; i < WashLights.Num() && i < WashFixtures.Num(); i++) {
            if (!WashLights[i]) continue;
            const float Norm = WashLights[i]->Intensity / FMath::Max(1.f, WashIntensityMax);
            UpdateFixtureMID(WashFixtures[i], WashLights[i]->GetLightColor(), Norm);
        }
        for (int32 i = 0; i < BeamLights.Num() && i < BeamFixtures.Num(); i++) {
            if (!BeamLights[i]) continue;
            const float Norm = BeamLights[i]->Intensity / FMath::Max(1.f, BeamIntensityMax);
            UpdateFixtureMID(BeamFixtures[i], BeamLights[i]->GetLightColor(), Norm);
        }
        for (int32 i = 0; i < StrobeLights.Num() && i < StrobeFixtures.Num(); i++) {
            if (!StrobeLights[i]) continue;
            const float Norm = StrobeLights[i]->Intensity / FMath::Max(1.f, StrobeIntensityMax);
            UpdateFixtureMID(StrobeFixtures[i], StrobeLights[i]->GetLightColor(), Norm);
        }
        for (int32 i = 0; i < SideLights.Num() && i < SideFixtures.Num(); i++) {
            if (!SideLights[i]) continue;
            const float Norm = SideLights[i]->Intensity / FMath::Max(1.f, WashIntensityMax);
            UpdateFixtureMID(SideFixtures[i], SideLights[i]->GetLightColor(), Norm);
        }
        for (int32 i = 0; i < FloorLights.Num() && i < FloorFixtures.Num(); i++) {
            if (!FloorLights[i]) continue;
            const float Norm = FloorLights[i]->Intensity / FMath::Max(1.f, FloorIntensityMax);
            UpdateFixtureMID(FloorFixtures[i], FloorLights[i]->GetLightColor(), Norm);
        }
    }

    // Cinematic micro-flicker: tiny per-light wobble (~3-5% by default). Each light has
    // its own pseudo-random phase + frequency derived from a hash on its pointer address,
    // so they wobble independently — looks like real fixtures with imperfect drivers.
    if (bCinematicMode && CinematicFlickerAmount > 0.0f)
    {
        const float Amp = CinematicFlickerAmount;
        auto Wobble = [&](void* Ptr) -> float {
            const uint32 H = (uint32)(reinterpret_cast<UPTRINT>(Ptr) >> 4);
            const float Freq = 4.0f + (float)(H % 7);              // 4-10 Hz per light
            const float Phase = (float)(H & 0xFF) * 0.0245f;       // 0..2pi
            return 1.0f + Amp * FMath::Sin(Time * Freq + Phase);
        };
        for (USpotLightComponent* L : WashLights)        if (L) L->SetIntensity(L->Intensity * Wobble(L));
        for (USpotLightComponent* L : BeamLights)        if (L) L->SetIntensity(L->Intensity * Wobble(L));
        for (USpotLightComponent* L : SideLights)        if (L) L->SetIntensity(L->Intensity * Wobble(L));
        for (USpotLightComponent* L : HouseLights)       if (L) L->SetIntensity(L->Intensity * Wobble(L));
        for (UPointLightComponent* L : FloorLights)      if (L) L->SetIntensity(L->Intensity * Wobble(L));
        for (USpotLightComponent* L : DiscoBallBeams)    if (L) L->SetIntensity(L->Intensity * Wobble(L));
    }

    // ----- CAMERA SHAKE -----
    // Procedural camera offset driven by bass + kick. Subtle on quiet sections, visceral on drops.
    // Applied via PlayerCameraManager view-target offset; resets each frame so no state leaks.
    if (bEnableCameraShake) {
        if (APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0)) {
            if (APlayerCameraManager* CamMgr = PC->PlayerCameraManager) {
                // Combine continuous bass shake + impulsive kick shake.
                const float BassMag = CameraShakeBassStrength * BassLvl;
                const float KickMag = CameraShakeKickStrength * KickFlash;
                const float TotalMag = FMath::Min(BassMag + KickMag, CameraShakeMaxOffset);

                if (TotalMag > 0.05f) {
                    // High-frequency noise on three axes — perlin-like via sin combinations
                    const float Tx = Time * 23.0f;
                    const float Ty = Time * 31.0f;
                    const float Tz = Time * 17.0f;
                    const FVector ShakeOffset(
                        TotalMag * FMath::Sin(Tx) * 0.6f,
                        TotalMag * FMath::Cos(Ty) * 0.6f,
                        TotalMag * FMath::Sin(Tz) * 0.8f
                    );
                    const FRotator ShakeRot(
                        TotalMag * 0.3f * FMath::Sin(Tx * 0.7f),
                        TotalMag * 0.3f * FMath::Cos(Ty * 0.8f),
                        TotalMag * 0.2f * FMath::Sin(Tz * 0.5f)
                    );
                    // APlayerCameraManager hides GetActor* publicly, so cast to AActor base
                    // pointer to reach the inherited public versions.
                    AActor* CamActor = CamMgr;
                    const FVector  CurLoc = CamActor->GetActorLocation();
                    const FRotator CurRot = CamActor->GetActorRotation();
                    CamActor->SetActorLocation(CurLoc + ShakeOffset);
                    CamActor->SetActorRotation((CurRot.Quaternion() * ShakeRot.Quaternion()).Rotator());
                }
            }
        }
    }
}
