#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Components/SceneComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/RectLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "ConcertStageDirector.generated.h"

UENUM()
enum class ELightingScene : uint8
{
    Warm,
    Cool,
    Neon,
    Chase,
    Strobe,
    Spotlight,
    Burst,
    Rainbow,
    // High-energy scenes — only picked for rock / metal / electronic / hiphop families.
    // Classical / ambient / jazz never enter these because they'd feel obnoxious there.
    HeartBeat,   // All wash + beam lights snap fully ON on each kick drum hit, blackout between
    Cascade,     // Rapid one-light-at-a-time sweep across the truss (faster than Chase)
    Sparkle,     // Random per-light flashes — like camera flashes scattered through the rig
    RainbowWave, // Pop-flavored: hue marches across the truss in a smooth gradient
    BeamStab     // Rock-flavored: all beams pitch straight down on every kick (rain of light)
};

UENUM()
enum class EEnergyMode : uint8
{
    Calm,    // ballad / intro / quiet section — soft, slow, dim
    Normal,  // mid-energy verse / build — standard behavior
    Peak     // chorus / heavy drop / climax — fast, bright, aggressive
};

class AAffectiveAudioActor;

UCLASS()
class MUSICAI_VISUALIZER_API AConcertStageDirector : public AActor
{
    GENERATED_BODY()

public:
    AConcertStageDirector();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Affective")
    TObjectPtr<AAffectiveAudioActor> AudioSource;

    // Geometry defaults are tuned for a 70m x 110m x 18m room (floor scale 70,110,0.2; walls 18 tall).
    // All values are in centimeters (1 UU = 1 cm).
    // Place the AConcertStageDirector actor near the BACK wall (e.g. world Y = +5500) so the stage
    // sits at the back and audience extends forward in -Y.
    UPROPERTY(EditAnywhere, Category = "Stage|Geometry")
    float StageWidth = 8000.f;   // 80 m stadium-scale stage

    UPROPERTY(EditAnywhere, Category = "Stage|Geometry")
    float StageDepth = 3500.f;   // 35 m deep stage zone

    UPROPERTY(EditAnywhere, Category = "Stage|Geometry")
    float TrussHeight = 3500.f;  // 35 m truss (much higher, with visible light fixtures hanging)

    // ----- Light fixture counts (tunable for the "more lights" festival look) -----
    UPROPERTY(EditAnywhere, Category = "Stage|Geometry", meta = (ClampMin = "2", ClampMax = "32"))
    int32 NumWashLights = 12;          // front-row wash lights along truss

    UPROPERTY(EditAnywhere, Category = "Stage|Geometry", meta = (ClampMin = "0", ClampMax = "32"))
    int32 NumBackWashLights = 8;       // back-row wash lights (back-lighting the band)

    UPROPERTY(EditAnywhere, Category = "Stage|Geometry", meta = (ClampMin = "2", ClampMax = "32"))
    int32 NumBeamLights = 10;          // moving beam lights (tight beams)

    UPROPERTY(EditAnywhere, Category = "Stage|Geometry", meta = (ClampMin = "0", ClampMax = "16"))
    int32 NumStrobeLights = 8;         // LED strobe bars on back wall

    UPROPERTY(EditAnywhere, Category = "Stage|Geometry", meta = (ClampMin = "0", ClampMax = "10"))
    int32 NumSideLightsPerSide = 3;    // side-tower lights, per side (so 3 = 6 total)

    UPROPERTY(EditAnywhere, Category = "Stage|Geometry", meta = (ClampMin = "0", ClampMax = "16"))
    int32 NumFloorLights = 8;          // stage-floor uplights

    UPROPERTY(EditAnywhere, Category = "Stage|Geometry", meta = (ClampMin = "0", ClampMax = "32"))
    int32 NumLaserLights = 10;         // ultra-tight beam lights (0.5 deg cone) — laser-show feel

    // When true, lasers form X / cross patterns during Burst scenes.
    // Left-side lasers aim right, right-side aim left, meeting in the center mid-air.
    UPROPERTY(EditAnywhere, Category = "Stage|Geometry")
    bool bLaserCrossPattern = true;

    UPROPERTY(EditAnywhere, Category = "Stage|Geometry", meta = (ClampMin = "0", ClampMax = "32"))
    int32 NumAudienceCeilingLights = 16;  // overhead lights above the audience (off most of the time)

    // ----- Visible light fixtures (cube/cylinder meshes at each light position) -----
    // Spawns small emissive mesh "PAR cans" at each light source so the user can SEE where
    // every spotlight beam originates from instead of beams appearing magically in midair.
    UPROPERTY(EditAnywhere, Category = "Stage|Fixtures")
    bool bSpawnLightFixtures = true;

    // Size (cm) of each fixture box. Real PAR cans are ~25 cm diameter — at 35 they're
    // visible from far away but not chunky-looking close up.
    UPROPERTY(EditAnywhere, Category = "Stage|Fixtures", meta = (ClampMin = "10", ClampMax = "150"))
    float FixtureSize = 35.f;

    // Emissive boost: multiplied onto each fixture's emissive color so the housing actually
    // glows. 5-15 = subtle, 20-40 = punchy "the fixture is ON".
    UPROPERTY(EditAnywhere, Category = "Stage|Fixtures", meta = (ClampMin = "0.0", ClampMax = "100.0"))
    float FixtureEmissiveStrength = 15.f;

    // ----- Physical stage geometry (truss towers, PA stacks, backdrop, monitors) -----
    // When true, BuildStageStructure() spawns the visible concert-stage skeleton: 4 truss
    // towers at the corners, horizontal roof beams, speaker stacks, stage monitors, backdrop.
    // Makes the lights look like they're MOUNTED on something instead of floating in midair.
    UPROPERTY(EditAnywhere, Category = "Stage|Structure")
    bool bSpawnStageStructure = true;

    // Raised stage platform (low slab the band stands on, like a real festival stage).
    UPROPERTY(EditAnywhere, Category = "Stage|Structure")
    bool bSpawnStagePlatform = true;

    // Raised stage height (how high the band stands above the room floor).
    // 60 = club/lounge; 150-200 = real concert; 250+ = arena/festival.
    UPROPERTY(EditAnywhere, Category = "Stage|Structure", meta = (ClampMin = "10", ClampMax = "500"))
    float StagePlatformHeight = 200.f;    // 2 m raised platform — real concert height

    // Truss thickness (cross-section of the metal beams).
    UPROPERTY(EditAnywhere, Category = "Stage|Structure", meta = (ClampMin = "10", ClampMax = "100"))
    float TrussThickness = 35.f;

    // Number of horizontal truss cross-beams visible across the top.
    UPROPERTY(EditAnywhere, Category = "Stage|Structure", meta = (ClampMin = "1", ClampMax = "8"))
    int32 NumTrussCrossBeams = 3;

    // PA speaker stacks per side (1 = single tower, 2 = double-wide).
    UPROPERTY(EditAnywhere, Category = "Stage|Structure", meta = (ClampMin = "0", ClampMax = "4"))
    int32 NumSpeakerStacksPerSide = 1;

    // Stage monitors at front edge of platform.
    UPROPERTY(EditAnywhere, Category = "Stage|Structure", meta = (ClampMin = "0", ClampMax = "12"))
    int32 NumStageMonitors = 5;

    // Tint colors so the user can quickly recolor the structure in editor.
    UPROPERTY(EditAnywhere, Category = "Stage|Structure")
    FLinearColor TrussTint    = FLinearColor(0.55f, 0.55f, 0.60f, 1.f);   // silver metallic

    UPROPERTY(EditAnywhere, Category = "Stage|Structure")
    FLinearColor SpeakerTint  = FLinearColor(0.05f, 0.05f, 0.05f, 1.f);   // matte black

    UPROPERTY(EditAnywhere, Category = "Stage|Structure")
    FLinearColor BackdropTint = FLinearColor(0.05f, 0.10f, 0.30f, 1.f);   // dark concert blue

    UPROPERTY(EditAnywhere, Category = "Stage|Structure")
    FLinearColor PlatformTint = FLinearColor(0.08f, 0.08f, 0.08f, 1.f);   // dark gray

    // Make the stage platform glossy/reflective. Visible only with Lumen GI or screen-space
    // reflections enabled in the project's renderer settings.
    UPROPERTY(EditAnywhere, Category = "Stage|Structure")
    bool bReflectiveStageFloor = true;

    UPROPERTY(EditAnywhere, Category = "Stage|Structure", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float StageFloorRoughness = 0.15f;     // 0 = mirror, 1 = matte

    UPROPERTY(EditAnywhere, Category = "Stage|Structure", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float StageFloorMetallic = 0.8f;       // 1 = full metal, 0 = dielectric

    // ----- LED VIDEO PANEL BACKDROP -----
    // Big emissive panels behind the band that pulse with music + scene color. This is the
    // single biggest visual upgrade — they dominate any screenshot you take. Real concerts
    // ALWAYS have these; without them the back of your stage looks empty.
    UPROPERTY(EditAnywhere, Category = "Stage|LED")
    bool bSpawnLedPanels = true;

    // Number of side-by-side panels behind the band. 1 = single huge panel, 3 = like
    // a real festival "left-center-right" triptych, 5+ = video wall.
    UPROPERTY(EditAnywhere, Category = "Stage|LED", meta = (ClampMin = "1", ClampMax = "9"))
    int32 NumLedPanels = 3;

    UPROPERTY(EditAnywhere, Category = "Stage|LED", meta = (ClampMin = "200", ClampMax = "3000"))
    float LedPanelWidth = 1500.f;     // 15 m per panel

    UPROPERTY(EditAnywhere, Category = "Stage|LED", meta = (ClampMin = "200", ClampMax = "3000"))
    float LedPanelHeight = 1000.f;    // 10 m tall

    UPROPERTY(EditAnywhere, Category = "Stage|LED", meta = (ClampMin = "0", ClampMax = "2000"))
    float LedPanelGap = 80.f;         // horizontal gap between panels

    UPROPERTY(EditAnywhere, Category = "Stage|LED", meta = (ClampMin = "0", ClampMax = "2000"))
    float LedPanelHeightFromFloor = 400.f;   // bottom edge height above stage deck

    // Emissive multiplier — higher = panel glows much brighter than the lights around it
    UPROPERTY(EditAnywhere, Category = "Stage|LED", meta = (ClampMin = "0.1", ClampMax = "200.0"))
    float LedPanelEmissiveStrength = 30.f;

    // Per-panel hue offset (degrees) so each panel can be a different color in real time
    UPROPERTY(EditAnywhere, Category = "Stage|LED", meta = (ClampMin = "0", ClampMax = "180"))
    float LedPanelHueSpread = 30.f;

    // Floating 3D text in front of the center LED panel showing the detected genre name
    // (e.g. "ROCK", "ELECTRONIC", "CLASSICAL"). Updates in real time.
    UPROPERTY(EditAnywhere, Category = "Stage|LED")
    bool bShowGenreText = true;

    UPROPERTY(EditAnywhere, Category = "Stage|LED", meta = (ClampMin = "50", ClampMax = "1000"))
    float GenreTextSize = 180.f;

    // ----- 3D SPECTRUM ANALYZER -----
    // A row of vertical bars below the LED panels that "dance" with the audio. Bars on the
    // LEFT are bass, MIDDLE are mids, RIGHT are treble. Each bar grows/shrinks with its
    // band's amplitude + per-bar randomization. Looks technical AND beautiful, perfect for
    // a capstone showing real-time audio reactivity.
    UPROPERTY(EditAnywhere, Category = "Stage|Spectrum")
    bool bSpawnSpectrumAnalyzer = true;

    UPROPERTY(EditAnywhere, Category = "Stage|Spectrum", meta = (ClampMin = "8", ClampMax = "128"))
    int32 NumSpectrumBars = 48;

    UPROPERTY(EditAnywhere, Category = "Stage|Spectrum", meta = (ClampMin = "100", ClampMax = "2000"))
    float SpectrumBarMaxHeight = 800.f;     // tallest a bar can grow (cm)

    UPROPERTY(EditAnywhere, Category = "Stage|Spectrum", meta = (ClampMin = "10", ClampMax = "200"))
    float SpectrumBarWidth = 30.f;

    UPROPERTY(EditAnywhere, Category = "Stage|Spectrum", meta = (ClampMin = "0", ClampMax = "100"))
    float SpectrumBarGap = 8.f;

    // Smoothing factor — 0=instant, 1=never moves. 0.15 = snappy but not jittery.
    UPROPERTY(EditAnywhere, Category = "Stage|Spectrum", meta = (ClampMin = "0", ClampMax = "0.95"))
    float SpectrumBarSmoothing = 0.18f;

    UPROPERTY(EditAnywhere, Category = "Stage|Spectrum", meta = (ClampMin = "0.1", ClampMax = "100.0"))
    float SpectrumBarEmissiveStrength = 12.f;

    // ----- STAGE HAZE (volumetric drifting puffs) -----
    // Scattered point lights with very high volumetric scattering and tiny intensity.
    // In the volumetric fog they show up as soft glowing blobs that drift slowly —
    // looks like smoke / haze pockets catching the stage lights.
    UPROPERTY(EditAnywhere, Category = "Stage|Haze")
    bool bSpawnHaze = true;

    UPROPERTY(EditAnywhere, Category = "Stage|Haze", meta = (ClampMin = "0", ClampMax = "128"))
    int32 NumHazePuffs = 24;

    UPROPERTY(EditAnywhere, Category = "Stage|Haze", meta = (ClampMin = "1.0", ClampMax = "1000.0"))
    float HazePuffIntensity = 80.f;

    UPROPERTY(EditAnywhere, Category = "Stage|Haze", meta = (ClampMin = "200.0", ClampMax = "5000.0"))
    float HazePuffRadius = 1200.f;

    UPROPERTY(EditAnywhere, Category = "Stage|Haze", meta = (ClampMin = "0.5", ClampMax = "20.0"))
    float HazeVolumetricScattering = 12.f;

    UPROPERTY(EditAnywhere, Category = "Stage|Haze", meta = (ClampMin = "0.0", ClampMax = "1000.0"))
    float HazeDriftAmplitude = 300.f;

    UPROPERTY(EditAnywhere, Category = "Stage|Haze", meta = (ClampMin = "0.01", ClampMax = "5.0"))
    float HazeDriftSpeed = 0.25f;

    // ----- CAMERA SHAKE -----
    // Adds a procedural camera shake to the player camera that's driven by bass energy
    // and big drum onsets. Subtle on quiet sections, visceral during chorus/drops.
    UPROPERTY(EditAnywhere, Category = "Stage|CameraShake")
    bool bEnableCameraShake = true;

    UPROPERTY(EditAnywhere, Category = "Stage|CameraShake", meta = (ClampMin = "0.0", ClampMax = "5.0"))
    float CameraShakeBassStrength = 1.2f;   // multiplier on bass-driven continuous shake

    UPROPERTY(EditAnywhere, Category = "Stage|CameraShake", meta = (ClampMin = "0.0", ClampMax = "5.0"))
    float CameraShakeKickStrength = 2.5f;   // multiplier on kick-driven impact shake

    UPROPERTY(EditAnywhere, Category = "Stage|CameraShake", meta = (ClampMin = "0.0", ClampMax = "20.0"))
    float CameraShakeMaxOffset = 6.0f;      // max camera offset in cm (don't go crazy or it nauseates)

    // ----- Procedural Venue (room) ------
    // Builds the floor, 4 walls, and ceiling programmatically so you don't have to scale up
    // the manually-placed cubes in the level. DELETE your existing wall/floor/ceiling actors
    // before enabling this — otherwise you'll have two sets of walls overlapping.
    UPROPERTY(EditAnywhere, Category = "Stage|Venue")
    bool bSpawnVenue = false;

    UPROPERTY(EditAnywhere, Category = "Stage|Venue", meta = (ClampMin = "5000", ClampMax = "60000"))
    float VenueWidth  = 20000.f;   // 200 m wide (arena scale)

    UPROPERTY(EditAnywhere, Category = "Stage|Venue", meta = (ClampMin = "5000", ClampMax = "60000"))
    float VenueDepth  = 30000.f;   // 300 m deep

    UPROPERTY(EditAnywhere, Category = "Stage|Venue", meta = (ClampMin = "1000", ClampMax = "15000"))
    float VenueHeight = 5500.f;    // 55 m tall ceiling (real arenas: 30-60 m)

    // How far behind the actor's local origin the back wall sits.
    // Larger = more empty space between stage and back wall.
    UPROPERTY(EditAnywhere, Category = "Stage|Venue", meta = (ClampMin = "0", ClampMax = "5000"))
    float VenueBackOffset = 500.f;

    // Tints applied to default material if no override is assigned.
    UPROPERTY(EditAnywhere, Category = "Stage|Venue")
    FLinearColor VenueFloorTint   = FLinearColor(0.10f, 0.10f, 0.12f, 1.f);   // very dark gray

    UPROPERTY(EditAnywhere, Category = "Stage|Venue")
    FLinearColor VenueWallTint    = FLinearColor(0.30f, 0.15f, 0.10f, 1.f);   // brick-red tint

    UPROPERTY(EditAnywhere, Category = "Stage|Venue")
    FLinearColor VenueCeilingTint = FLinearColor(0.06f, 0.06f, 0.06f, 1.f);   // near-black

    // Optional material overrides. If assigned in editor, takes priority over the tint.
    // Use this to apply your existing brick material at the new venue scale.
    UPROPERTY(EditAnywhere, Category = "Stage|Venue")
    TObjectPtr<UMaterialInterface> VenueFloorMaterialOverride;

    UPROPERTY(EditAnywhere, Category = "Stage|Venue")
    TObjectPtr<UMaterialInterface> VenueWallMaterialOverride;

    UPROPERTY(EditAnywhere, Category = "Stage|Venue")
    TObjectPtr<UMaterialInterface> VenueCeilingMaterialOverride;

    UPROPERTY(EditAnywhere, Category = "Stage|Power")
    float WashIntensityMax = 8000.f;

    UPROPERTY(EditAnywhere, Category = "Stage|Power")
    float BeamIntensityMax = 25000.f;

    UPROPERTY(EditAnywhere, Category = "Stage|Power")
    float StrobeIntensityMax = 15000.f;

    UPROPERTY(EditAnywhere, Category = "Stage|Power")
    float FloorIntensityMax = 4000.f;

    // Global scale multiplier applied to ALL instrument meshes. Bumped from 1.0 -> 2.5
    // because default scanned-mesh sizes look tiny in the 70x110 m nightclub room.
    UPROPERTY(EditAnywhere, Category = "Stage|Instruments")
    float InstrumentScale = 2.5f;

    // Per-instrument scale multipliers (multiplied with InstrumentScale).
    // These defaults compensate for the wildly different natural sizes of the imported
    // scanned meshes — piano was already correct at 2.5x, but the mic stand mesh has a
    // huge natural size and the guitarist mesh has a tiny one. Tune in editor if needed.
    UPROPERTY(EditAnywhere, Category = "Stage|Instruments")
    float DrumScale     = 2.5f;    // 2.5 * 2.5 = ~6x final — drum kit fits on the platform now

    UPROPERTY(EditAnywhere, Category = "Stage|Instruments")
    float MicScale      = 0.04f;   // mic stand has huge natural size, tiny scale brings it back

    UPROPERTY(EditAnywhere, Category = "Stage|Instruments")
    float PianoScale    = 1.0f;

    UPROPERTY(EditAnywhere, Category = "Stage|Instruments")
    float GuitarScale   = 1.5f;    // gitarist1 mesh — 2.5 * 1.5 = ~4x — keeps him visible without dwarfing the stage

    UPROPERTY(EditAnywhere, Category = "Stage|Instruments")
    float VocalistScale = 1.0f;

    // Violin / strings — index 5. Triggered by violin / viola / cello / strings tags.
    // Default 8.0 = combined 20× — visible from audience distance on the 80m wide stage.
    UPROPERTY(EditAnywhere, Category = "Stage|Instruments")
    float ViolinScale = 8.0f;

    UPROPERTY(EditAnywhere, Category = "Stage|Instruments")
    float ViolinYawDeg = 0.f;

    UPROPERTY(EditAnywhere, Category = "Stage|Instruments")
    TObjectPtr<UStaticMesh> ViolinMeshOverride;

    // Drum kit depth as a fraction of StageDepth from the actor origin (0=stage center,
    // +0.5=back of stage). Lowered from 0.45 -> 0.15 so the drum kit doesn't clip into
    // the back wall when scaled up. Tune in editor if you want it further forward / back.
    UPROPERTY(EditAnywhere, Category = "Stage|Instruments")
    float DrumDepthFraction = -0.05f;   // Slightly FORWARD of stage center — was 0.15 (clipping back wall)

    // Per-instrument Yaw rotation (degrees) on top of the mesh's natural orientation.
    // Imported scanned meshes have inconsistent default-forward axes — these let you
    // rotate each performer to face the audience without touching code. Default values
    // below were chosen by flipping each wrong-facing mesh 180 degrees from its previous
    // hardcoded rotation. If a mesh still faces the wrong way, just bump its Yaw +/-90.
    UPROPERTY(EditAnywhere, Category = "Stage|Instruments")
    float DrumYawDeg     = 0.f;     // Flipped 180 — drummer now faces the audience

    UPROPERTY(EditAnywhere, Category = "Stage|Instruments")
    float MicYawDeg      = 180.f;   // (was 0, flipped)

    UPROPERTY(EditAnywhere, Category = "Stage|Instruments")
    float PianoYawDeg    = 150.f;   // (was -30, flipped)

    UPROPERTY(EditAnywhere, Category = "Stage|Instruments")
    float GuitarYawDeg   = -90.f;   // face audience (-Y direction). Was 20 (still wrong).

    UPROPERTY(EditAnywhere, Category = "Stage|Instruments")
    float VocalistYawDeg = 180.f;   // (was 0, flipped — vocalist was facing back wall)

    // Front-of-stage depth positions, expressed as fractions of StageDepth FORWARD of stage center.
    // Larger value -> closer to audience, further FROM the back wall.
    // The mic is intentionally further forward than the vocalist so the mic stand is between
    // singer and crowd, like a real stage setup.
    UPROPERTY(EditAnywhere, Category = "Stage|Instruments")
    float MicDepthFraction      = 0.45f;  // mic at local Y = -StageDepth * 0.45 = -675

    UPROPERTY(EditAnywhere, Category = "Stage|Instruments")
    float VocalistDepthFraction = 0.18f;  // vocalist at local Y = -StageDepth * 0.18 = -270

    // Pin the vocalist mesh visible permanently. Voice/singer tags rarely cross the
    // detection threshold on instrumental / heavy / electronic tracks, so without this
    // the vocalist character almost never appears even when there ARE vocals.
    UPROPERTY(EditAnywhere, Category = "Stage|Instruments")
    bool bAlwaysShowVocalist = true;

    // When true, the mic stand stays visible permanently (it's a stage prop, not
    // music-driven). Voice/singer tags rarely cross the detection threshold for
    // heavy/instrumental tracks, so the mic would otherwise never appear.
    UPROPERTY(EditAnywhere, Category = "Stage|Instruments")
    bool bAlwaysShowMicStand = true;

    // Break the vocalist's default T-pose by rotating his upper-arm bones downward,
    // so it looks like he's holding the mic instead of standing arms-out.
    UPROPERTY(EditAnywhere, Category = "Stage|Instruments")
    bool bAdjustVocalistPose = true;

    // Rotation applied to the vocalist's upper-arm bones in COMPONENT space.
    // Both arms use the SAME rotation now (the right-arm-looks-correct case).
    // The bone's local axes already mirror left/right in the rig, so giving them
    // identical rotation produces a symmetric pose.
    UPROPERTY(EditAnywhere, Category = "Stage|Instruments")
    FRotator VocalistLeftArmRotation = FRotator(-60.f, -20.f, 0.f);

    UPROPERTY(EditAnywhere, Category = "Stage|Instruments")
    FRotator VocalistRightArmRotation = FRotator(-60.f, -20.f, 0.f);

    // Optional: explicitly set the bone names if auto-detect doesn't find them.
    // Leave at None to auto-detect (tries LeftArm / L_UpperArm / upperarm_l / etc).
    UPROPERTY(EditAnywhere, Category = "Stage|Instruments")
    FName VocalistLeftArmBoneOverride = NAME_None;

    UPROPERTY(EditAnywhere, Category = "Stage|Instruments")
    FName VocalistRightArmBoneOverride = NAME_None;

    // FALLBACK: if rotating the arms can't produce a natural pose for this rig, just
    // scale the upper-arm bones to zero so the arms disappear. The vocalist will appear
    // as a torso + head + legs, which combined with the mic stand reads as "holding mic".
    UPROPERTY(EditAnywhere, Category = "Stage|Instruments")
    bool bHideVocalistArms = false;

    // Subtle head bob — rotates the vocalist's head bone with bass so he doesn't look frozen.
    UPROPERTY(EditAnywhere, Category = "Stage|Instruments")
    bool bVocalistHeadBob = true;

    // When true, instrument X-positions COMPRESS toward stage center when fewer instruments
    // are visible (so 2-piece classical doesn't look lost on a 80m wide stage), and SPREAD
    // out when more instruments appear.
    UPROPERTY(EditAnywhere, Category = "Stage|Instruments")
    bool bDynamicInstrumentPlacement = true;

    // How fast positions lerp toward target (0=instant, 0.95=glacial). 0.12 = smooth ~1s transition.
    UPROPERTY(EditAnywhere, Category = "Stage|Instruments", meta = (ClampMin = "0", ClampMax = "0.95"))
    float InstrumentPlacementSmoothing = 0.12f;

    // Override head bone name if auto-detect doesn't find it.
    UPROPERTY(EditAnywhere, Category = "Stage|Instruments")
    FName VocalistHeadBoneOverride = NAME_None;

    // Max pitch (degrees) the head bobs down on each beat. 0 = none, 10-20 = subtle bob.
    UPROPERTY(EditAnywhere, Category = "Stage|Instruments", meta = (ClampMin = "0", ClampMax = "45"))
    float VocalistHeadBobAmount = 12.f;

    // When true, every Tick draws an on-screen debug line showing which instrument
    // meshes are currently visible + their detection confidence. Turn on while iterating
    // to see exactly what the lighting code is "seeing".
    UPROPERTY(EditAnywhere, Category = "Stage|Instruments")
    bool bShowInstrumentDebug = false;

    // ----- Visual quality knobs -----
    // When true, BeginPlay overrides several rendering console variables to improve
    // the look of the spotlight beams in volumetric fog (sharper, less banding,
    // smoother fall-off). Costs a little GPU but the difference is dramatic.
    UPROPERTY(EditAnywhere, Category = "Stage|Quality")
    bool bHighQualityVolumetric = true;

    // Master toggle for the cinematic look — spawns a PostProcessVolume with bloom,
    // vignette, saturation, and exposure compensation tuned for nightclub lighting,
    // and adds organic micro-flicker to all lights. Single biggest "make it look pro"
    // switch in the project.
    UPROPERTY(EditAnywhere, Category = "Stage|Quality")
    bool bCinematicMode = true;

    // Bloom intensity. Lower than before so beam SHAFTS stay visible — too much bloom
    // blurs everything into a glow and hides the visible cone of the spotlight in fog.
    UPROPERTY(EditAnywhere, Category = "Stage|Quality", meta = (ClampMin = "0.0", ClampMax = "5.0"))
    float CinematicBloomIntensity = 0.5f;

    // Bloom threshold — only pixels brighter than this bloom. Higher = only the very
    // brightest spots glow, leaving fog volume crisp. Default UE = -1 (every pixel).
    UPROPERTY(EditAnywhere, Category = "Stage|Quality", meta = (ClampMin = "-1.0", ClampMax = "5.0"))
    float CinematicBloomThreshold = 1.2f;

    UPROPERTY(EditAnywhere, Category = "Stage|Quality", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float CinematicVignetteIntensity = 0.4f;

    UPROPERTY(EditAnywhere, Category = "Stage|Quality", meta = (ClampMin = "0.5", ClampMax = "2.0"))
    float CinematicSaturation = 1.15f;

    // Exposure bias in stops. Negative = darker. Keeping it slightly negative helps the
    // beam shafts pop without going pitch-black like the previous attempt.
    UPROPERTY(EditAnywhere, Category = "Stage|Quality", meta = (ClampMin = "-3.0", ClampMax = "3.0"))
    float CinematicExposureBias = -0.3f;

    // Cap how bright auto-exposure can push things. Without this, in a dark room the
    // camera over-brightens to "see" everything and you lose the contrast between
    // beam-shaft and surrounding darkness.
    UPROPERTY(EditAnywhere, Category = "Stage|Quality", meta = (ClampMin = "0.01", ClampMax = "10.0"))
    float CinematicExposureMaxBrightness = 1.5f;

    // Multiplier on every spotlight's VolumetricScatteringIntensity. Higher = beam
    // shafts more visible from ceiling to floor (the user-facing "see the beam"
    // experience). Default UE per-light = 1.0; nightclub = 3-5.
    UPROPERTY(EditAnywhere, Category = "Stage|Quality", meta = (ClampMin = "0.0", ClampMax = "10.0"))
    float CinematicVolumetricBoost = 3.5f;

    // Per-light micro-flicker amplitude (0 = no flicker, 0.05 = 5% wobble). Real stage
    // fixtures aren't perfectly stable; tiny random wobble looks more organic.
    UPROPERTY(EditAnywhere, Category = "Stage|Quality", meta = (ClampMin = "0.0", ClampMax = "0.20"))
    float CinematicFlickerAmount = 0.04f;

    // ============= PYRO BURSTS (firework-style stinger that shoots up from the stage floor) =============
    // Triggered only at MUSICAL PEAKS (high arousal + high bass) AND only for energetic genres
    // (rock/metal/electronic/hiphop). Hard cooldown so it fires AT MOST 1-2 times per song.

    UPROPERTY(EditAnywhere, Category = "Stage|Pyro")
    bool bEnablePyroBursts = true;

    UPROPERTY(EditAnywhere, Category = "Stage|Pyro", meta = (ClampMin = "10.0", ClampMax = "300.0"))
    float PyroCooldownSeconds = 60.f;        // minimum gap between pyros

    UPROPERTY(EditAnywhere, Category = "Stage|Pyro", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float PyroIntensityThreshold = 0.75f;    // I must exceed this

    UPROPERTY(EditAnywhere, Category = "Stage|Pyro", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float PyroBassThreshold = 0.55f;         // BassLvl must exceed this

    UPROPERTY(EditAnywhere, Category = "Stage|Pyro", meta = (ClampMin = "0.5", ClampMax = "10.0"))
    float PyroDurationSeconds = 2.0f;        // how long each burst lasts

    UPROPERTY(EditAnywhere, Category = "Stage|Pyro", meta = (ClampMin = "1", ClampMax = "32"))
    int32 NumPyroLights = 12;                // how many upward beams in each burst

    UPROPERTY(EditAnywhere, Category = "Stage|Pyro", meta = (ClampMin = "5000.0", ClampMax = "500000.0"))
    float PyroIntensityPeak = 150000.f;      // peak intensity per pyro light

    // Optional: assign a Niagara System asset for actual particle effects. If null we fall
    // back to procedural upward-pointing lights (still looks great).
    UPROPERTY(EditAnywhere, Category = "Stage|Pyro")
    TObjectPtr<class UNiagaraSystem> PyroNiagaraSystem;

    // ============= CO2 JETS =============
    // Tall white-light columns at the stage front that fire on SNARE hits during energetic
    // scenes — separate from the rare pyro bursts. Like a DJ-style CO2 fog cannon.
    UPROPERTY(EditAnywhere, Category = "Stage|CO2")
    bool bEnableCO2Jets = true;

    UPROPERTY(EditAnywhere, Category = "Stage|CO2", meta = (ClampMin = "1", ClampMax = "24"))
    int32 NumCO2Jets = 8;

    UPROPERTY(EditAnywhere, Category = "Stage|CO2", meta = (ClampMin = "5000.0", ClampMax = "300000.0"))
    float CO2JetIntensityPeak = 60000.f;

    UPROPERTY(EditAnywhere, Category = "Stage|CO2", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float CO2JetSnareThreshold = 0.5f;     // snare flash must exceed this to fire a jet

    // ============= PHOTOGRAPHER FLASHES =============
    // Random tiny white strobes scattered throughout the audience volume — like a
    // bunch of fans taking phone pictures during a concert. Adds huge "real concert" feel.
    UPROPERTY(EditAnywhere, Category = "Stage|CrowdFlash")
    bool bEnableCrowdFlashes = true;

    UPROPERTY(EditAnywhere, Category = "Stage|CrowdFlash", meta = (ClampMin = "0", ClampMax = "128"))
    int32 NumCrowdFlashes = 36;

    UPROPERTY(EditAnywhere, Category = "Stage|CrowdFlash", meta = (ClampMin = "1000.0", ClampMax = "200000.0"))
    float CrowdFlashIntensity = 25000.f;

    // Probability per frame that each flash bulb fires. Higher = more flashes per second.
    // 0.002 ≈ each light fires once every ~8 seconds on average at 60fps.
    UPROPERTY(EditAnywhere, Category = "Stage|CrowdFlash", meta = (ClampMin = "0.0", ClampMax = "0.5"))
    float CrowdFlashRatePerFrame = 0.005f;

    // Multiplier when in peak scenes (Burst/HeartBeat/Cascade/Sparkle).
    UPROPERTY(EditAnywhere, Category = "Stage|CrowdFlash", meta = (ClampMin = "1.0", ClampMax = "20.0"))
    float CrowdFlashPeakMultiplier = 6.0f;

    // ============= DJ BOOTH (appears for Electronic genres) =============
    // A booth at front-center of the stage that shows up ONLY when the detected
    // genre family is Electronic. Has an emissive front face that pulses with the music.
    UPROPERTY(EditAnywhere, Category = "Stage|DJBooth")
    bool bSpawnDJBooth = true;

    UPROPERTY(EditAnywhere, Category = "Stage|DJBooth")
    float DJBoothWidth = 350.f;     // 3.5m wide

    UPROPERTY(EditAnywhere, Category = "Stage|DJBooth")
    float DJBoothDepth = 150.f;     // 1.5m deep

    UPROPERTY(EditAnywhere, Category = "Stage|DJBooth")
    float DJBoothHeight = 130.f;    // 1.3m tall (standing booth)

    UPROPERTY(EditAnywhere, Category = "Stage|DJBooth", meta = (ClampMin = "0.1", ClampMax = "200.0"))
    float DJBoothEmissiveStrength = 25.f;

    // ============= SIDE LED BARS (vertical strips on the truss towers) =============
    UPROPERTY(EditAnywhere, Category = "Stage|SideLED")
    bool bSpawnSideLedBars = true;

    UPROPERTY(EditAnywhere, Category = "Stage|SideLED", meta = (ClampMin = "1", ClampMax = "4"))
    int32 NumSideLedBarsPerTower = 1;

    UPROPERTY(EditAnywhere, Category = "Stage|SideLED", meta = (ClampMin = "0.1", ClampMax = "200.0"))
    float SideLedEmissiveStrength = 20.f;

    // Source radius (cm) for spotlights — bigger source = softer beam falloff with
    // physically-correct edge blur. Default 15; cinematic feel = 30-50.
    UPROPERTY(EditAnywhere, Category = "Stage|Quality", meta = (ClampMin = "5.0", ClampMax = "100.0"))
    float CinematicSourceRadius = 35.f;

    // ============= DYNAMICS (color & timing variation through the song) =============
    // Without these, a track stuck on one genre family looks the same color the whole song.
    // Real concert lighting drifts color continuously and rotates palettes every 30-60s.

    // Master toggle for the dynamic-color system below.
    UPROPERTY(EditAnywhere, Category = "Stage|Dynamics")
    bool bDynamicHueCycle = true;

    // Unify the disco ball + RGB dance floor with the concert lights' current palette,
    // instead of letting each run an independent rainbow cycle. When ON: rock makes the
    // dance floor cycle red/orange/pink (not full rainbow), disco ball beams sweep
    // red/orange/yellow shades, etc. The Rainbow scene still goes full HSV regardless.
    UPROPERTY(EditAnywhere, Category = "Stage|Dynamics")
    bool bUnifiedColorPalette = true;

    // How wide each tile/beam can vary from the base scene hue, in degrees on the HSV wheel.
    // Smaller = more uniform color (everything red), larger = more variety per tile.
    UPROPERTY(EditAnywhere, Category = "Stage|Dynamics", meta = (ClampMin = "0.0", ClampMax = "180.0"))
    float UnifiedHueSpread = 50.f;

    // Speed of the SLOW continuous hue oscillation, in revolutions/second.
    // 0.04 = 1 full back-and-forth cycle every ~25 seconds. Set 0 to disable.
    UPROPERTY(EditAnywhere, Category = "Stage|Dynamics", meta = (ClampMin = "0.0", ClampMax = "0.5"))
    float HueCycleSpeed = 0.04f;

    // How far the hue oscillates around its base color, in degrees on the HSV wheel.
    // 30 = noticeable but stays in genre family. 60+ = aggressive (rock might briefly look orange-yellow then back).
    UPROPERTY(EditAnywhere, Category = "Stage|Dynamics", meta = (ClampMin = "0.0", ClampMax = "180.0"))
    float HueCycleAmplitude = 25.f;

    // How often the BASE palette rotates by a fixed step. Major shift every ~30s,
    // so within a 3-min song you'll see 5-6 distinct color "stages" (like a real concert).
    UPROPERTY(EditAnywhere, Category = "Stage|Dynamics", meta = (ClampMin = "5.0", ClampMax = "120.0"))
    float PaletteRotationSeconds = 30.f;

    // Each rotation step shifts hue by this many degrees. 45 = 8 distinct color phases per cycle.
    UPROPERTY(EditAnywhere, Category = "Stage|Dynamics", meta = (ClampMin = "0.0", ClampMax = "180.0"))
    float PaletteRotationDegrees = 45.f;

    // How long each scene (Chase / Strobe / HeartBeat / etc.) holds before re-rolling.
    // Lower = more variety, higher = each look has time to "land". Real concerts: 5-12s.
    UPROPERTY(EditAnywhere, Category = "Stage|Dynamics", meta = (ClampMin = "1.0", ClampMax = "30.0"))
    float SceneMinDuration = 5.0f;

    UPROPERTY(EditAnywhere, Category = "Stage|Dynamics", meta = (ClampMin = "1.0", ClampMax = "30.0"))
    float SceneMaxDuration = 9.0f;

    // Burst trigger sensitivity: how big an arousal SPIKE triggers a Burst scene
    // (full white-light explosion). Lower = more frequent, higher = rarer/more dramatic.
    UPROPERTY(EditAnywhere, Category = "Stage|Dynamics", meta = (ClampMin = "0.05", ClampMax = "0.5"))
    float BurstArousalThreshold = 0.18f;

    // Minimum seconds between bursts (cooldown).
    UPROPERTY(EditAnywhere, Category = "Stage|Dynamics", meta = (ClampMin = "0.5", ClampMax = "20.0"))
    float BurstCooldownSeconds = 2.5f;

    // ===== BASS-SPIKE: "lights open and close on bass" =====
    // When the bass level jumps RAPIDLY upward, every light (wash, beams, strobes,
    // dance floor, disco ball, house) flares to full brightness then decays — like
    // a real concert reacting to a heavy drop or kick fill.
    UPROPERTY(EditAnywhere, Category = "Stage|Reactive")
    bool bBassSpikeReact = true;

    // How much the bass level must RISE in one frame to trigger a spike. Lower = more
    // sensitive. 0.15 ≈ a clear kick that wasn't there a moment ago.
    UPROPERTY(EditAnywhere, Category = "Stage|Reactive", meta = (ClampMin = "0.05", ClampMax = "0.5"))
    float BassSpikeThreshold = 0.12f;

    // Brightness multiplier applied to ALL lights at the peak of a bass spike. 2.0 = 2x.
    UPROPERTY(EditAnywhere, Category = "Stage|Reactive", meta = (ClampMin = "1.0", ClampMax = "5.0"))
    float BassSpikeMultiplier = 2.0f;

    // Seconds for the spike flash to decay back to normal. Lower = snappier flash.
    UPROPERTY(EditAnywhere, Category = "Stage|Reactive", meta = (ClampMin = "0.05", ClampMax = "2.0"))
    float BassSpikeDecaySeconds = 0.5f;

    // Cooldown so consecutive kicks don't all stack into one continuous flash.
    UPROPERTY(EditAnywhere, Category = "Stage|Reactive", meta = (ClampMin = "0.05", ClampMax = "2.0"))
    float BassSpikeCooldown = 0.35f;

    // ===== SOLO MODE: "guitar solo -> spotlight on guitarist" =====
    // When the top-1 instrument dominates the top-2 by a wide gap, the system enters
    // "solo mode" — that performer's instrument light stays bright while everything
    // else dims. Recreates the classic "spotlight on the soloist" concert moment.
    UPROPERTY(EditAnywhere, Category = "Stage|Reactive")
    bool bSoloFocusReact = true;

    // Top-1 confidence required to consider it a solo (0..1).
    UPROPERTY(EditAnywhere, Category = "Stage|Reactive", meta = (ClampMin = "0.2", ClampMax = "1.0"))
    float SoloFocusTop1Min = 0.45f;

    // Required gap between top-1 and top-2 confidence — this is what makes it "dominant".
    UPROPERTY(EditAnywhere, Category = "Stage|Reactive", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float SoloFocusGap = 0.12f;

    // While in solo mode, multiply non-soloist light intensity by this (0.3 = 30% of normal).
    UPROPERTY(EditAnywhere, Category = "Stage|Reactive", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float SoloDimAmount = 0.3f;

    // While in solo mode, multiply the soloist's instrument light by this (1.8 = 80% brighter).
    UPROPERTY(EditAnywhere, Category = "Stage|Reactive", meta = (ClampMin = "1.0", ClampMax = "5.0"))
    float SoloBoostAmount = 2.0f;

    // ===== ENERGY MODE: sustained Calm / Normal / Peak detection =====
    // Reads the smoothed Arousal score and bands the music into 3 modes that change the
    // ENTIRE feel of the lighting — faster scene rotation + brighter for Peak, slower +
    // dimmer for Calm. Hysteresis prevents mode flicker at the band boundaries.
    UPROPERTY(EditAnywhere, Category = "Stage|EnergyMode")
    bool bEnergyModeReact = true;

    // Below this arousal we enter Calm mode.
    UPROPERTY(EditAnywhere, Category = "Stage|EnergyMode", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float CalmEnterThreshold = 0.32f;

    // Above this we leave Calm mode (back to Normal). Hysteresis: must be > CalmEnter+gap.
    UPROPERTY(EditAnywhere, Category = "Stage|EnergyMode", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float CalmExitThreshold  = 0.42f;

    UPROPERTY(EditAnywhere, Category = "Stage|EnergyMode", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float PeakEnterThreshold = 0.68f;

    UPROPERTY(EditAnywhere, Category = "Stage|EnergyMode", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float PeakExitThreshold  = 0.58f;

    // Brightness multipliers for each mode (applied to all lights globally).
    UPROPERTY(EditAnywhere, Category = "Stage|EnergyMode", meta = (ClampMin = "0.1", ClampMax = "2.0"))
    float CalmBrightnessMul = 0.55f;

    UPROPERTY(EditAnywhere, Category = "Stage|EnergyMode", meta = (ClampMin = "0.5", ClampMax = "3.0"))
    float PeakBrightnessMul = 1.35f;

    // Scene-duration multipliers — Calm = slower scene changes, Peak = faster.
    UPROPERTY(EditAnywhere, Category = "Stage|EnergyMode", meta = (ClampMin = "0.5", ClampMax = "5.0"))
    float CalmSceneDurationMul = 2.0f;   // 5-9s -> 10-18s in Calm

    UPROPERTY(EditAnywhere, Category = "Stage|EnergyMode", meta = (ClampMin = "0.2", ClampMax = "2.0"))
    float PeakSceneDurationMul = 0.6f;   // 5-9s -> 3-5.4s in Peak

    // Disco ball rotation speed multiplier per mode. Calm = barely moving.
    UPROPERTY(EditAnywhere, Category = "Stage|EnergyMode", meta = (ClampMin = "0.0", ClampMax = "3.0"))
    float CalmDiscoSpeedMul = 0.25f;

    UPROPERTY(EditAnywhere, Category = "Stage|EnergyMode", meta = (ClampMin = "0.5", ClampMax = "3.0"))
    float PeakDiscoSpeedMul = 1.7f;

    // When entering Peak from Calm/Normal, fire a one-shot bright flash similar to Burst
    // but slightly bigger — like the moment a chorus drops. Duration in seconds.
    UPROPERTY(EditAnywhere, Category = "Stage|EnergyMode", meta = (ClampMin = "0.0", ClampMax = "3.0"))
    float PeakEntryFlashSeconds = 0.7f;

    UPROPERTY(EditAnywhere, Category = "Stage|EnergyMode", meta = (ClampMin = "1.0", ClampMax = "5.0"))
    float PeakEntryFlashStrength = 2.5f;

    // Master toggle for the strong music reactivity below.
    UPROPERTY(EditAnywhere, Category = "Stage|Reactivity")
    bool bStrongMusicReactivity = true;

    // 1.0 = baseline, 2.0 = double the visual swing on bass / kick / snare.
    // Higher = more dramatic visible swings; lower = subtler/calmer.
    UPROPERTY(EditAnywhere, Category = "Stage|Reactivity", meta = (ClampMin = "0.0", ClampMax = "3.0"))
    float ReactivityStrength = 1.5f;

    // When 1, wash lights GATE to bass: low bass = dim (30%), high bass = bright.
    // Creates very visible breathing response to bass guitar / sub-bass.
    UPROPERTY(EditAnywhere, Category = "Stage|Reactivity", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float BassGateAmount = 0.7f;

    // When snare hits, wash lights briefly tint toward white. 0 = no flash, 1 = full white pop.
    UPROPERTY(EditAnywhere, Category = "Stage|Reactivity", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float SnareWhiteFlash = 0.85f;

    // Each kick onset advances a hue step by this many degrees, so colors shift through
    // the song with the rhythm. Set 0 to disable.
    UPROPERTY(EditAnywhere, Category = "Stage|Reactivity", meta = (ClampMin = "0.0", ClampMax = "30.0"))
    float HueStepPerKickDeg = 6.0f;

    // When intensity (I) suddenly jumps by this much (verse->chorus, drop, build), force
    // a scene re-roll instead of waiting for the SceneDuration timer. Lower = more reactive.
    UPROPERTY(EditAnywhere, Category = "Stage|Reactivity", meta = (ClampMin = "0.05", ClampMax = "1.0"))
    float SceneReRollIntensityDelta = 0.30f;

    // Volumetric fog grid resolution. Lower = sharper beams, more GPU. Default UE = 8.
    // 4 is sweet-spot for cinematic look. 2 starts to be GPU-expensive.
    UPROPERTY(EditAnywhere, Category = "Stage|Quality", meta = (ClampMin = "2", ClampMax = "16"))
    int32 VolumetricFogGridPixelSize = 4;

    // Number of fog volume slices in Z direction. More = better depth resolution.
    UPROPERTY(EditAnywhere, Category = "Stage|Quality", meta = (ClampMin = "64", ClampMax = "256"))
    int32 VolumetricFogGridSizeZ = 192;

    // Per-instrument mesh overrides. If any of these is assigned in the editor it wins
    // over the C++ default-loaded asset. Use these to swap meshes (e.g. new Drum_Set kit)
    // without recompiling. Order = stage placement order in BuildStage().
    UPROPERTY(EditAnywhere, Category = "Stage|Instruments")
    TObjectPtr<UStaticMesh> DrumMeshOverride;

    UPROPERTY(EditAnywhere, Category = "Stage|Instruments")
    TObjectPtr<UStaticMesh> MicMeshOverride;

    UPROPERTY(EditAnywhere, Category = "Stage|Instruments")
    TObjectPtr<UStaticMesh> PianoMeshOverride;

    UPROPERTY(EditAnywhere, Category = "Stage|Instruments")
    TObjectPtr<UStaticMesh> GuitarMeshOverride;

    UPROPERTY(EditAnywhere, Category = "Stage|Instruments")
    TObjectPtr<USkeletalMesh> VocalistMeshOverride;

    UPROPERTY(EditAnywhere, Category = "Stage|Audience")
    bool bSpawnAudience = true;

    UPROPERTY(EditAnywhere, Category = "Stage|Audience")
    int32 AudienceRows = 8;     // 8 x 12 = 96 dancers, scales for the larger 70x110 m room

    UPROPERTY(EditAnywhere, Category = "Stage|Audience")
    int32 AudiencePerRow = 12;

    UPROPERTY(EditAnywhere, Category = "Stage|Audience")
    float AudienceStartDepth = 1500.f;  // 15 m gap between stage front and first row

    UPROPERTY(EditAnywhere, Category = "Stage|Audience")
    float AudienceRowSpacing = 350.f;   // 3.5 m between rows -> total depth 24.5 m

    UPROPERTY(EditAnywhere, Category = "Stage|Audience")
    float AudienceColSpacing = 250.f;   // 2.5 m between columns

    UPROPERTY(EditAnywhere, Category = "Stage|Audience")
    float AudienceFloorIntensityMax = 2000.f;

    // Optional: assign a custom static mesh in the editor for the audience figures.
    // If null at BeginPlay, the code falls back to a humanoid capsule shape.
    UPROPERTY(EditAnywhere, Category = "Stage|Audience")
    TObjectPtr<UStaticMesh> AudienceMeshOverride;

    // Pool of audience meshes — when non-empty, BuildAudience picks one at random per figure.
    // Use this to mix multiple characters in the crowd for realistic variety.
    // The single AudienceMeshOverride is used as fallback if this array is empty.
    UPROPERTY(EditAnywhere, Category = "Stage|Audience")
    TArray<TObjectPtr<UStaticMesh>> AudienceMeshPool;

    // Global multiplier on audience figure scale. Increase if figures look too small.
    UPROPERTY(EditAnywhere, Category = "Stage|Audience")
    float AudienceScaleMultiplier = 1.5f;

    UPROPERTY(EditAnywhere, Category = "Stage|HouseLights")
    bool bSpawnHouseLights = true;

    UPROPERTY(EditAnywhere, Category = "Stage|HouseLights")
    float HouseLightIntensity = 1200.f;

    UPROPERTY(EditAnywhere, Category = "Stage|Tables")
    bool bSpawnTables = true;

    UPROPERTY(EditAnywhere, Category = "Stage|Tables")
    int32 NumTables = 12;       // 6 per side flanking the dance floor

    UPROPERTY(EditAnywhere, Category = "Stage|Tables")
    int32 PeoplePerTable = 3;

    UPROPERTY(EditAnywhere, Category = "Stage|Tables")
    float TableRadius = 250.f;

    // X distance (cm) of the side-table rows from the room centerline.
    // With StageWidth=3000 and audience width ~5400, tables sit at +/-3000 cm = +/-30 m from center,
    // i.e. clearly outside the dance floor (15 m wide) but inside the 70 m room (35 m half-width).
    UPROPERTY(EditAnywhere, Category = "Stage|Tables")
    float TableSideX = 3000.f;

    UPROPERTY(EditAnywhere, Category = "Stage|Tables")
    TObjectPtr<UStaticMesh> TableMeshOverride;

    UPROPERTY(EditAnywhere, Category = "Stage|Tables")
    TObjectPtr<UStaticMesh> ChairMeshOverride;

    UPROPERTY(EditAnywhere, Category = "Stage|Tables")
    bool bSpawnChairs = true;

    UPROPERTY(EditAnywhere, Category = "Stage|DiscoBall")
    bool bSpawnDiscoBall = true;

    UPROPERTY(EditAnywhere, Category = "Stage|DiscoBall")
    TObjectPtr<UStaticMesh> DiscoBallMeshOverride;

    UPROPERTY(EditAnywhere, Category = "Stage|DiscoBall")
    float DiscoBallScale = 2.0f;

    // Degrees per second the ball + its colored beams rotate around the room.
    UPROPERTY(EditAnywhere, Category = "Stage|DiscoBall")
    float DiscoBallRotationSpeed = 35.f;

    // Number of colored beams shooting outward from the ball.
    // 8 beams = 45 deg apart; 12 = 30 deg apart (denser sweep).
    UPROPERTY(EditAnywhere, Category = "Stage|DiscoBall")
    int32 NumDiscoBallBeams = 12;

    // Per-beam intensity (lumens). Each beam is narrow and aimed slightly downward
    // so beams hit the dance floor + audience as they sweep around.
    UPROPERTY(EditAnywhere, Category = "Stage|DiscoBall")
    float DiscoBallBeamIntensity = 6000.f;

    // Pitch (deg) of each beam relative to horizontal. -25 = beams angle down 25 deg.
    UPROPERTY(EditAnywhere, Category = "Stage|DiscoBall")
    float DiscoBallBeamPitch = -25.f;

    // Outer cone (deg) of each beam. Tight (~6) = sharp shafts; wider (~12) = softer wash.
    UPROPERTY(EditAnywhere, Category = "Stage|DiscoBall")
    float DiscoBallBeamOuterCone = 7.f;

    UPROPERTY(EditAnywhere, Category = "Stage|DiscoBall")
    float DiscoBallBeamAttenuation = 4500.f;

    UPROPERTY(EditAnywhere, Category = "Stage|DanceFloor")
    bool bSpawnDanceFloorLights = true;

    UPROPERTY(EditAnywhere, Category = "Stage|DanceFloor")
    int32 DanceFloorRows = 8;

    UPROPERTY(EditAnywhere, Category = "Stage|DanceFloor")
    int32 DanceFloorCols = 8;

    UPROPERTY(EditAnywhere, Category = "Stage|DanceFloor")
    float DanceFloorLightIntensity = 3500.f;   // lower so the colored circles don't all wash out into white

    UPROPERTY(EditAnywhere, Category = "Stage|DanceFloor")
    float DanceFloorLightRadius = 350.f;       // 3.5 m so adjacent tiles read as separate colors

    UPROPERTY(EditAnywhere, Category = "Stage|DanceFloor")
    float DanceFloorHueCycleSeconds = 6.0f;

    // Explicit dance-floor footprint in cm. The disco ball hangs above its center.
    // Default 1500 x 1500 = 15 m x 15 m, fits comfortably under the audience block.
    UPROPERTY(EditAnywhere, Category = "Stage|DanceFloor")
    float DanceFloorWidth = 1500.f;

    UPROPERTY(EditAnywhere, Category = "Stage|DanceFloor")
    float DanceFloorDepth = 1500.f;

    // ---------------------------- BAR ----------------------------
    // The bar sits behind the audience block, opposite the stage. NumBarStools stools
    // are placed in front of the counter facing it.
    UPROPERTY(EditAnywhere, Category = "Stage|Bar")
    bool bSpawnBar = true;

    UPROPERTY(EditAnywhere, Category = "Stage|Bar")
    int32 NumBarStools = 8;

    UPROPERTY(EditAnywhere, Category = "Stage|Bar")
    float BarLength = 3000.f;       // 30 m long counter

    // Distance (cm) from the back row of the audience to the bar counter.
    UPROPERTY(EditAnywhere, Category = "Stage|Bar")
    float BarBehindAudience = 2500.f;

    UPROPERTY(EditAnywhere, Category = "Stage|Bar")
    float BarCounterHeight = 220.f; // 2.2 m tall counter (incl. backsplash)

    UPROPERTY(EditAnywhere, Category = "Stage|Bar")
    TObjectPtr<UStaticMesh> BarCounterMeshOverride;

    UPROPERTY(EditAnywhere, Category = "Stage|Bar")
    TObjectPtr<UStaticMesh> BarStoolMeshOverride;

    UPROPERTY(EditAnywhere, Category = "Stage|Bar")
    float BarAccentLightIntensity = 1500.f;

    UPROPERTY(EditAnywhere, Category = "Stage|Instruments")
    float InstrumentLingerSeconds = 6.0f;

    // Confidence threshold for showing an instrument mesh on stage. Raised from 0.25 -> 0.35
    // because lower values produce false positives — e.g. piano gets ~28-30% confidence on
    // heavy-metal tracks (model ambiguity), which would spuriously spawn the piano mesh
    // even when there's no piano in the music. Lighting code uses its own (lower) threshold
    // for family aggregation; this property only affects mesh visibility on stage.
    UPROPERTY(EditAnywhere, Category = "Stage|Instruments")
    float InstrumentDetectionThreshold = 0.35f;

    // Only the TOP-N instrument tags by confidence are eligible to spawn a mesh.
    // Keep small (3-5) for clean stage; larger values accept more false positives.
    UPROPERTY(EditAnywhere, Category = "Stage|Instruments")
    int32 InstrumentVisibilityTopN = 4;

protected:
    virtual void BeginPlay() override;

public:
    virtual void Tick(float DeltaTime) override;

private:
    UPROPERTY()
    TObjectPtr<USceneComponent> Root;

    UPROPERTY()
    TArray<TObjectPtr<USpotLightComponent>> WashLights;

    UPROPERTY()
    TArray<TObjectPtr<USpotLightComponent>> BeamLights;

    UPROPERTY()
    TArray<TObjectPtr<URectLightComponent>> StrobeLights;

    UPROPERTY()
    TArray<TObjectPtr<USpotLightComponent>> SideLights;

    UPROPERTY()
    TArray<TObjectPtr<UPointLightComponent>> FloorLights;

    // Laser-thin beams (0.5 deg cone) — sweep fast on Cascade/Sparkle scenes only.
    UPROPERTY()
    TArray<TObjectPtr<USpotLightComponent>> LaserLights;

    // LED video panels — emissive plane meshes behind the band. Their Dynamic Material
    // Instances are updated each Tick to pulse + cycle color in sync with music.
    UPROPERTY()
    TArray<TObjectPtr<UStaticMeshComponent>> LedPanels;

    // 3D spectrum analyzer — row of vertical bars whose height tracks audio amplitude.
    UPROPERTY()
    TArray<TObjectPtr<UStaticMeshComponent>> SpectrumBars;

    // Per-bar current height (cm), used for smoothing.
    TArray<float> SpectrumBarCurrentHeights;

    // Stage haze — drifting volumetric puffs (each is a low-intensity high-scatter PointLight).
    UPROPERTY()
    TArray<TObjectPtr<UPointLightComponent>> HazePuffs;

    // Base position around which each haze puff drifts (sin-wave offset in Tick).
    TArray<FVector> HazePuffBasePositions;

    // Speaker stack meshes — scale Y is animated by bass to make the cones visibly thump.
    UPROPERTY()
    TArray<TObjectPtr<UStaticMeshComponent>> SpeakerMeshes;

    // Base scale for each speaker mesh (so we can pulse around the base).
    TArray<FVector> SpeakerBaseScales;

    // Ceiling lights above the AUDIENCE pointing down. Off by default; activate on
    // Burst / Cascade / Sparkle scenes to light the crowd dramatically.
    UPROPERTY()
    TArray<TObjectPtr<USpotLightComponent>> AudienceCeilingLights;

    // Pyro stinger lights — spawned at stage front edge, point straight up.
    // Off by default; flash bright + fade out on rare musical peaks.
    UPROPERTY()
    TArray<TObjectPtr<USpotLightComponent>> PyroLights;

    // Cylinder meshes co-located with each pyro light — emissive beam visual.
    UPROPERTY()
    TArray<TObjectPtr<UStaticMeshComponent>> PyroBeamMeshes;

    double LastPyroTime    = -1000.0;  // wall time of last pyro
    double PyroStartTime   = -1000.0;  // start of currently-active pyro (or far in past)

    // CO2 jet lights — white columns at stage front, fire on snare hits.
    UPROPERTY()
    TArray<TObjectPtr<USpotLightComponent>> CO2JetLights;

    UPROPERTY()
    TArray<TObjectPtr<UStaticMeshComponent>> CO2JetMeshes;

    // Per-jet timer (decreases each frame, fires set to 0.3s on snare hit)
    TArray<float> CO2JetTimers;

    // Photographer-flash audience strobes — tiny PointLights scattered in the crowd.
    UPROPERTY()
    TArray<TObjectPtr<UPointLightComponent>> CrowdFlashLights;

    // Per-flash decay timer (1.0 when triggered, 0 when dark).
    TArray<float> CrowdFlashTimers;

    // DJ booth — body + emissive front panel. Visibility gated to Electronic family.
    UPROPERTY()
    TObjectPtr<UStaticMeshComponent> DJBoothBody;

    UPROPERTY()
    TObjectPtr<UStaticMeshComponent> DJBoothPanel;

    // Side LED bars mounted on the truss towers
    UPROPERTY()
    TArray<TObjectPtr<UStaticMeshComponent>> SideLedBars;

    // Floating "ROCK"/"ELECTRONIC" 3D text in front of the center LED panel
    UPROPERTY()
    TObjectPtr<class UTextRenderComponent> GenreTextRender;

    // Parallel fixture-mesh arrays — one mesh per light, same index, so the mesh emissive
    // color/intensity can be updated each frame to match its light. Each component has a
    // Dynamic Material Instance so we can set "Color" + "Emissive" parameters at runtime.
    UPROPERTY()
    TArray<TObjectPtr<UStaticMeshComponent>> WashFixtures;

    UPROPERTY()
    TArray<TObjectPtr<UStaticMeshComponent>> BeamFixtures;

    UPROPERTY()
    TArray<TObjectPtr<UStaticMeshComponent>> StrobeFixtures;

    UPROPERTY()
    TArray<TObjectPtr<UStaticMeshComponent>> SideFixtures;

    UPROPERTY()
    TArray<TObjectPtr<UStaticMeshComponent>> FloorFixtures;

    UPROPERTY()
    TArray<TObjectPtr<USceneComponent>> InstrumentRoots;

    // Tracked separately so we can adjust the vocalist's T-pose (rotate arm bones down).
    // UPoseableMeshComponent (not USkeletalMeshComponent) is the right class for direct
    // bone manipulation — it exposes SetBoneRotationByName at runtime without an AnimBP.
    UPROPERTY()
    TObjectPtr<class UPoseableMeshComponent> VocalistSkeletalMesh;

    // Resolved head bone name (auto-detected at BuildStage) for the head bob animation.
    FName VocalistHeadBoneResolved = NAME_None;

    UPROPERTY()
    TArray<TObjectPtr<UStaticMeshComponent>> InstrumentMeshes;

    // Base position from BuildStage for each instrument (used for dynamic placement lerp).
    TArray<FVector> InstrumentBasePositions;

    UPROPERTY()
    TArray<TObjectPtr<UPointLightComponent>> InstrumentLights;

    UPROPERTY()
    TArray<TObjectPtr<UStaticMeshComponent>> AudienceMeshes;

    UPROPERTY()
    TArray<TObjectPtr<USpotLightComponent>> AudienceLights;

    UPROPERTY()
    TArray<TObjectPtr<USpotLightComponent>> HouseLights;

    UPROPERTY()
    TArray<TObjectPtr<UStaticMeshComponent>> TableMeshes;

    UPROPERTY()
    TArray<TObjectPtr<UStaticMeshComponent>> BarMeshes;

    UPROPERTY()
    TArray<TObjectPtr<UPointLightComponent>> BarLights;

    UPROPERTY()
    TObjectPtr<UStaticMeshComponent> DiscoBallMesh;

    UPROPERTY()
    TObjectPtr<USpotLightComponent> DiscoBallLight;

    // Pivot that holds the ball mesh + all colored beams; we rotate this in Tick
    // so the whole beam rig sweeps around the room with the ball.
    UPROPERTY()
    TObjectPtr<USceneComponent> DiscoBallPivot;

    UPROPERTY()
    TArray<TObjectPtr<USpotLightComponent>> DiscoBallBeams;

    UPROPERTY()
    TArray<TObjectPtr<UPointLightComponent>> DanceFloorLights;

    TArray<float> DanceFloorPhaseOffsets;

    // Per-instrument visibility timers (one per InstrumentRoots index).
    // When the corresponding instrument is detected, the timer is reset to InstrumentLingerSeconds.
    // The mesh is shown when timer > 0; timer counts down each frame.
    TArray<float> InstrumentVisibilityTimers;

    TArray<FVector> AudienceBasePositions;
    TArray<float>   AudiencePhases;   // per-person random offset so the crowd doesn't move in lockstep
    TArray<float>   AudienceJumpiness; // per-person bounce intensity (some people dance harder)

    // Parallel to InstrumentRoots: true if a mesh (static OR skeletal) actually loaded for
    // that index. Used by the on-screen debug to distinguish "logic fired, mesh missing"
    // from "logic fired, mesh OK" — without this the debug lies because InstrumentMeshes
    // misaligns when an asset fails to load (subsequent meshes shift down a slot).
    TArray<bool> bInstrumentMeshLoaded;

    float SilenceFade = 0.0f;

    // Reactivity tracking
    float KickAccumDeg = 0.0f;          // accumulated hue rotation from per-kick steps
    float PrevKickFlashForStep = 0.0f;  // previous KickFlash value for rising-edge detection
    float PrevIntensityForReRoll = 0.0f; // previous intensity for scene re-roll on big change
    double LastSceneReRollWall = 0.0;   // cooldown so we don't re-roll every frame

    void BuildStage();
    void BuildAudience();
    void BuildHouseLights();
    void BuildTables();
    void BuildDiscoBall();
    void BuildDanceFloorLights();
    void BuildBar();
    void BuildStageStructure();  // truss towers, PA stacks, backdrop, stage monitors, platform
    void BuildVenue();           // floor, ceiling, 4 walls — procedural room
    void BuildLedPanels();       // emissive video-panel-style backdrop behind the band
    void BuildSpectrumAnalyzer();// vertical bars dancing with audio bands
    void BuildHaze();            // scattered drifting volumetric puffs above the stage + audience
    static float Normalize(float Raw) { return (FMath::Tanh(Raw * 0.4f) + 1.0f) * 0.5f; }

    ELightingScene CurrentScene = ELightingScene::Warm;
    double SceneStartTime = 0.0;
    float SceneDuration = 10.0f;
    double LastBurstTime = 0.0;
    float PrevArousal = 0.0f;

    // Runtime state for the bass-spike "all lights flare" reactive effect.
    float  PrevBassLvl = 0.0f;
    double LastBassSpikeTime = -1000.0;

    // Runtime state for the Calm/Normal/Peak energy mode (with hysteresis + transition flash).
    EEnergyMode CurrentEnergyMode = EEnergyMode::Normal;
    double      LastPeakEntryTime = -1000.0;
    float       SmoothedArousal   = 0.5f;
    ELightingScene PickScene(float A, float V, float R, float I);
};
