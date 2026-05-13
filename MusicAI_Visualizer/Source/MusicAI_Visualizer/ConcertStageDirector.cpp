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

    if (bSpawnAudience) BuildAudience();
    if (bSpawnTables)   BuildTables();
    if (bSpawnBar)      BuildBar();
    if (bSpawnHouseLights) BuildHouseLights();
    if (bSpawnDiscoBall) BuildDiscoBall();
    if (bSpawnDanceFloorLights) BuildDanceFloorLights();
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
    for (int32 i = 0; i < 6; i++) {
        USpotLightComponent* L = NewObject<USpotLightComponent>(this);
        L->SetupAttachment(Root);
        L->RegisterComponent();
        float t = (float)i / 5.0f;
        float X = FMath::Lerp(-StageWidth * 0.5f, StageWidth * 0.5f, t);
        L->SetRelativeLocation(FVector(X, 0.f, TrussHeight));
        L->SetRelativeRotation(FRotator(-90.f, 0.f, 0.f));
        L->SetInnerConeAngle(20.f);
        L->SetOuterConeAngle(45.f);
        L->SetAttenuationRadius(2500.f);
        L->SetIntensityUnits(ELightUnits::Lumens);
        L->SetIntensity(0.f);
        L->SetCastShadows(false);
        L->SetVolumetricScatteringIntensity(2.5f);
        WashLights.Add(L);
    }

    for (int32 i = 0; i < 4; i++) {
        USpotLightComponent* L = NewObject<USpotLightComponent>(this);
        L->SetupAttachment(Root);
        L->RegisterComponent();
        float t = (float)i / 3.0f;
        float X = FMath::Lerp(-StageWidth * 0.45f, StageWidth * 0.45f, t);
        L->SetRelativeLocation(FVector(X, -StageDepth * 0.3f, TrussHeight));
        L->SetRelativeRotation(FRotator(-70.f, 0.f, 0.f));
        L->SetInnerConeAngle(2.f);
        L->SetOuterConeAngle(7.f);
        L->SetAttenuationRadius(4000.f);
        L->SetIntensityUnits(ELightUnits::Lumens);
        L->SetIntensity(0.f);
        L->SetCastShadows(false);
        L->SetVolumetricScatteringIntensity(6.0f);
        BeamLights.Add(L);
    }

    for (int32 i = 0; i < 4; i++) {
        URectLightComponent* L = NewObject<URectLightComponent>(this);
        L->SetupAttachment(Root);
        L->RegisterComponent();
        float t = (float)i / 3.0f;
        float X = FMath::Lerp(-StageWidth * 0.4f, StageWidth * 0.4f, t);
        L->SetRelativeLocation(FVector(X, StageDepth * 0.5f, TrussHeight * 0.6f));
        L->SetRelativeRotation(FRotator(0.f, -90.f, 0.f));
        L->SetSourceWidth(150.f);
        L->SetSourceHeight(100.f);
        L->SetAttenuationRadius(3000.f);
        L->SetIntensityUnits(ELightUnits::Lumens);
        L->SetIntensity(0.f);
        L->SetCastShadows(false);
        StrobeLights.Add(L);
    }

    for (int32 i = 0; i < 2; i++) {
        USpotLightComponent* L = NewObject<USpotLightComponent>(this);
        L->SetupAttachment(Root);
        L->RegisterComponent();
        float Side = (i == 0) ? -1.0f : 1.0f;
        L->SetRelativeLocation(FVector(Side * StageWidth * 0.7f, 0.f, TrussHeight * 0.5f));
        L->SetRelativeRotation(FRotator(-15.f, Side * 90.f, 0.f));
        L->SetInnerConeAngle(15.f);
        L->SetOuterConeAngle(35.f);
        L->SetAttenuationRadius(3000.f);
        L->SetIntensityUnits(ELightUnits::Lumens);
        L->SetIntensity(0.f);
        L->SetCastShadows(false);
        L->SetVolumetricScatteringIntensity(2.0f);
        SideLights.Add(L);
    }

    for (int32 i = 0; i < 4; i++) {
        UPointLightComponent* L = NewObject<UPointLightComponent>(this);
        L->SetupAttachment(Root);
        L->RegisterComponent();
        float t = (float)i / 3.0f;
        float X = FMath::Lerp(-StageWidth * 0.4f, StageWidth * 0.4f, t);
        L->SetRelativeLocation(FVector(X, StageDepth * 0.4f, 60.f));
        L->SetAttenuationRadius(800.f);
        L->SetIntensityUnits(ELightUnits::Lumens);
        L->SetIntensity(0.f);
        L->SetCastShadows(false);
        FloorLights.Add(L);
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
            USkeletalMeshComponent* M = NewObject<USkeletalMeshComponent>(this);
            M->SetupAttachment(R);
            M->RegisterComponent();
            M->SetSkeletalMeshAsset(SkM);
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
    AddInstrument(FVector(0.f, StageDepth * DrumDepthFraction, 0.f),      FRotator(0.f, DrumYawDeg,     0.f), DrumMesh,     nullptr,      1100.f, FVector(0, 0, 150), DrumScale);
    AddInstrument(FVector(0.f, -StageDepth * MicDepthFraction, 0.f),      FRotator(0.f, MicYawDeg,      0.f), MicMesh,      nullptr,       700.f, FVector(0, 0, 150), MicScale);
    AddInstrument(FVector(-StageWidth * 0.30f, -StageDepth * 0.05f, 0.f), FRotator(0.f, PianoYawDeg,    0.f), PianoMesh,    nullptr,       950.f, FVector(0, 0, 100), PianoScale);
    AddInstrument(FVector( StageWidth * 0.30f,  StageDepth * 0.20f, 0.f), FRotator(0.f, GuitarYawDeg,   0.f), RoryMesh,     nullptr,       950.f, FVector(0, 0, 150), GuitarScale);
    AddInstrument(FVector(0.f, -StageDepth * VocalistDepthFraction, 0.f), FRotator(0.f, VocalistYawDeg, 0.f), nullptr,      VocalistMesh,  850.f, FVector(0, 0, 180), VocalistScale);

    // Log a single-line summary of which instruments got meshes. Read this in Output Log
    // (Window -> Output Log) after pressing Play. Anything saying "MISSING" needs an
    // override assigned in the editor (Stage|Instruments -> *MeshOverride).
    static const TCHAR* IndexNames[] = { TEXT("Drum"), TEXT("Mic"), TEXT("Piano"), TEXT("Guitar"), TEXT("Vocalist") };
    for (int32 i = 0; i < bInstrumentMeshLoaded.Num() && i < 5; i++) {
        const bool bOK = bInstrumentMeshLoaded[i];
        const FVector WPos = (i < InstrumentRoots.Num() && InstrumentRoots[i]) ? InstrumentRoots[i]->GetComponentLocation() : FVector::ZeroVector;
        UE_LOG(LogTemp, Warning, TEXT("ConcertStageDirector: [%d] %s mesh = %s, world pos = %s"),
            i, IndexNames[i], bOK ? TEXT("LOADED") : TEXT("MISSING (assign override!)"), *WPos.ToString());
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
                // 6 picks: Strobe, Neon, Chase, HeartBeat, Cascade, Sparkle.
                // ~33% of the time we land on a HIGH-ENERGY rhythm-locked scene (HeartBeat/Cascade/Sparkle).
                const int32 r = FMath::RandRange(0, 5);
                if (r == 0) return ELightingScene::Strobe;
                if (r == 1) return ELightingScene::HeartBeat;
                if (r == 2) return ELightingScene::Cascade;
                if (r == 3) return ELightingScene::Sparkle;
                if (r == 4) return ELightingScene::Neon;
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
            case EGenreFamily::Pop:
                return FMath::RandBool() ? ELightingScene::Rainbow : ELightingScene::Warm;
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
            if (G == TEXT("pop") || G == TEXT("synthpop") || G == TEXT("popfolk"))
                return FMath::RandBool() ? ELightingScene::Rainbow : ELightingScene::Warm;

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
        if (I.Contains(TEXT("guitar")) || I.Contains(TEXT("bass")) ||
            I == TEXT("synthesizer")   || I == TEXT("computer")    ||
            I == TEXT("ukulele")       || I == TEXT("doublebass"))
            return 3;
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

    // Decrement timers, set visibility on each instrument root accordingly.
    for (int32 i = 0; i < InstrumentRoots.Num() && i < InstrumentVisibilityTimers.Num(); i++) {
        InstrumentVisibilityTimers[i] = FMath::Max(0.0f, InstrumentVisibilityTimers[i] - DeltaTime);
        if (InstrumentRoots[i]) {
            const bool bShow = InstrumentVisibilityTimers[i] > 0.0f;
            InstrumentRoots[i]->SetVisibility(bShow, true);
        }
    }

    // On-screen debug: show what the lighting code "sees" — visible meshes + the top-N
    // instrument tags that drove the decision. Toggle via bShowInstrumentDebug in editor.
    if (bShowInstrumentDebug && GEngine) {
        static const TCHAR* Names[] = { TEXT("DRUMS"), TEXT("MIC"), TEXT("PIANO"), TEXT("GUITAR"), TEXT("VOCALIST") };
        FString Visible;
        for (int32 i = 0; i < InstrumentVisibilityTimers.Num() && i < 5; i++) {
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
            for (int32 i = 0; i < InstrumentRoots.Num() && i < 5; i++) {
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
    if (AudienceMeshes.Num() == AudienceBasePositions.Num()) {
        const float SwayAmount      = 8.0f * A;                                       // more arousal -> more sway
        const float SwaySpeed       = 2.0f + 1.5f * R;                                // rhythm drives sway speed
        const float BeatJumpHeight  = (35.0f * KickFlash + 12.0f * SnareFlash);       // kick drum makes them jump
        const float HiHatBob        = 4.0f * HiHatFlash;                              // hi-hat adds tiny head bob

        for (int32 i = 0; i < AudienceMeshes.Num(); i++) {
            if (!AudienceMeshes[i]) continue;
            const float Phase = AudiencePhases[i];
            const float Jumpy = AudienceJumpiness[i];

            const float SwayX = FMath::Sin(Time * SwaySpeed + Phase) * SwayAmount;
            const float SwayY = FMath::Cos(Time * SwaySpeed * 0.7f + Phase) * SwayAmount * 0.4f;
            const float JumpZ = (BeatJumpHeight + HiHatBob) * Jumpy * (0.7f + 0.3f * FMath::Sin(Phase * 3.0f));

            AudienceMeshes[i]->SetRelativeLocation(AudienceBasePositions[i] + FVector(SwayX, SwayY, JumpZ));

            // Slight rotation so they appear to bob
            const float TwistDeg = FMath::Sin(Time * SwaySpeed + Phase * 1.3f) * 6.0f * A;
            AudienceMeshes[i]->SetRelativeRotation(FRotator(0.f, TwistDeg, 0.f));
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

    for (USpotLightComponent* L : WashLights)        if (L) L->SetIntensity(L->Intensity * NonSoloMul);
    for (USpotLightComponent* L : BeamLights)        if (L) L->SetIntensity(L->Intensity * NonSoloMul);
    for (URectLightComponent* L : StrobeLights)      if (L) L->SetIntensity(L->Intensity * NonSoloMul);
    for (USpotLightComponent* L : SideLights)        if (L) L->SetIntensity(L->Intensity * NonSoloMul);
    for (UPointLightComponent* L : FloorLights)      if (L) L->SetIntensity(L->Intensity * NonSoloMul);
    for (USpotLightComponent* L : AudienceLights)    if (L) L->SetIntensity(L->Intensity * NonSoloMul);
    for (UPointLightComponent* L : DanceFloorLights) if (L) L->SetIntensity(L->Intensity * DanceMul);
    for (USpotLightComponent* L : DiscoBallBeams)    if (L) L->SetIntensity(L->Intensity * DiscoMul);

    // Instrument lights: the soloist gets a dedicated boost; the rest also dim slightly so
    // the contrast is dramatic (entire stage darker, soloist's spotlight pops).
    for (int32 i = 0; i < InstrumentLights.Num(); i++) {
        if (!InstrumentLights[i]) continue;
        const float Mul = SilenceFade * BassFlashMul * ModeMul *
                          ((i == SoloIndex) ? SoloBoost : (bSoloMode ? SoloDimAmount : 1.0f));
        InstrumentLights[i]->SetIntensity(InstrumentLights[i]->Intensity * Mul);
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
}
