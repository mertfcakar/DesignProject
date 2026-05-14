# MusicAI Visualizer — Project Handoff

**Last updated:** 2026-05-14
**Project owner:** Mert Fahri Çakar (CompE senior, capstone project, Spring 2026)
**Email:** f.mertcakar@outlook.com
**Repo:** https://github.com/mertfcakar/DesignProject
**Local path:** `C:\Users\mertf\MyProjects-main\DesignProject`

---

## Read this if you're picking up this project in a new conversation

This is a real-time AI-driven music visualizer built in **Unreal Engine 5.7**. It uses ML to classify audio (genre/mood/instruments) and drives a full concert-scale lighting + stage system in response.

The project is **feature-complete for capstone demo purposes**. Remaining work is polish + presentation prep, NOT more major features. Resist the user's urge to add more features unless they specifically ask — they have a deadline and the technical depth is already strong.

## High-level architecture

```
┌────────────────────────┐         ┌─────────────────────────────────┐
│  System audio          │         │  UE5 ConcertStageDirector       │
│  via VoiceMeeter Out B1│         │  - Reads ML data from AudioSrc  │
└──────────┬─────────────┘         │  - Drives 400+ scene elements   │
           │                       │  - 13 lighting scenes           │
           ├────────────┐          │  - Genre-family aggregation     │
           ▼            ▼          └──────────▲──────────────────────┘
   ┌─────────────┐ ┌─────────────┐            │
   │ Python      │ │ UE5 NNE     │            │
   │ sidecar.py  │ │ in-engine   │            │
   │ (port 17777)│ │ inference   │            │
   │ VGGish+LSTM │ │ AestheticBrain v2         │
   │ Top-15 tags │ │ + tag_names.json          │
   └──────┬──────┘ └─────┬───────┘            │
          │              │                    │
          │ UDP JSON     │ direct C++         │
          └─────────────▶│                    │
                         ▼                    │
              ┌─────────────────────┐         │
              │ AffectiveAudioActor │─────────┘
              │ - GetTopGenresAny() │   (Tick reads accessors)
              │ - GetTopInstr...()  │
              │ - GetBassLevel()    │
              │ - 5-D affect vec    │
              └─────────────────────┘
```

**Hybrid pipeline:** The C++ in-engine NNE inference produces the 5-D affective vector (Arousal/Valence/Timbre/Rhythm/Intensity) + tag predictions. The Python sidecar gives more accurate top-N genre/mood/instrument detection via the full PyTorch pipeline. The sidecar is OPTIONAL — `GetTopGenresAny` falls back to in-engine ONNX if the UDP socket isn't connected.

## Critical files

| File | Purpose |
|---|---|
| `MusicAI_Visualizer/Source/MusicAI_Visualizer/ConcertStageDirector.h` | All lighting + scene + audience properties. ~1200 lines. |
| `MusicAI_Visualizer/Source/MusicAI_Visualizer/ConcertStageDirector.cpp` | Main visualizer logic. ~3200 lines. Tick() drives everything. |
| `MusicAI_Visualizer/Source/MusicAI_Visualizer/AffectiveAudioActor.h/.cpp` | Audio analysis + ML inference + sidecar UDP receiver |
| `MusicAI_Visualizer/Content/AI/AestheticBrain_v2.uasset` | Trained ONNX model wrapper |
| `MusicAI_Visualizer/Content/AI/tag_names.json` | Tag name list (genre/mood/instrument categories) |
| `MusicAI_Visualizer/Content/audioset-vggish-3.uasset` | **275 MB — NOT in git.** VGGish feature extractor. Download separately. |
| `ai_backend/python_sidecar.py` | Python sidecar — top-15 tag classifier, sends JSON over UDP |
| `ai_backend/audioset-vggish-3.onnx` | Same model in raw ONNX form (gitignored due to size) |

## Features fully implemented (huge — do NOT add more without strong reason)

### Lighting (400+ dynamic elements)
- 12 front wash + 8 back wash spotlights on truss
- 10 beam moving heads with bass-driven yaw amplitude
- 8 strobe LED bars in two rows on back wall
- 6 side tower stack lights
- 8 floor uplighters
- 10 laser lights (0.5° pencil-thin cones, sweep fast)
- 16 audience ceiling lights (scene-driven)
- 12 disco ball rotating colored beams
- 12 pyro burst lights (genre-locked: rock/electronic/hiphop, 60s cooldown, fires on peaks)
- 8 CO2 jet lights (white columns, fire on snare hits)
- 24 stage haze puffs (volumetric drifting fog blobs)
- 36 photographer audience flashes (random crowd strobes, more on peak scenes)
- 64-tile RGB dance floor cycling hue
- 2 side LED bar strips on truss towers
- Visible PAR-can fixture meshes at every spotlight — emissive housings glow with light color

### Lighting scenes (13 total)
| Scene | Trigger | What it does |
|---|---|---|
| Warm | Various | Static warm wash |
| Cool | Ambient | Static cool wash |
| Neon | Energetic | Magenta + cyan |
| Chase | Most genres | One-light sweep |
| Strobe | Energetic | Fast on/off all lights |
| Spotlight | Calm | Center focus only |
| Burst | Arousal spike | Full bright explosion |
| Rainbow | Pop | Hue cycle across lights |
| HeartBeat | Rock/Electronic | Kick-locked binary flash |
| Cascade | Rock/Electronic | Fast pencil sweep |
| Sparkle | Rock/Electronic | Random per-light flashes |
| **RainbowWave** | Pop | Hue marches across truss like a wave |
| **BeamStab** | Rock | All beams snap straight down on kick |

Scene rotation: every 5-9s. Burst triggered on arousal spike. Scene re-rolls on big intensity jumps.

### Stage geometry (procedural)
- Venue: **200m × 300m × 55m** procedural room (floor + ceiling + 4 walls)
- Stage platform: 80m × 35m × 2m raised slab (glossy/metallic for reflections)
- Truss structure: 4 corner towers, perimeter beams, 3 cross-beams at top
- 4 PA speaker stacks (2 stacks per side) — Y-scale animated by bass
- 5 stage monitors (wedge speakers at front)
- Backdrop wall behind the band
- 3 LED video panels (15m × 10m each) behind drum kit
- 48 spectrum analyzer bars below the LED panels (FFT-style — bass/mid/treble bands)
- Genre text floating in front of center LED panel ("ROCK"/"ELECTRONIC"/"CLASSICAL"/etc.)
- DJ booth (appears only for Electronic genres, has emissive front panel)

### Band (6 instruments, dynamically placed)
- 0: Drum kit
- 1: Mic stand (always visible — stage prop)
- 2: Piano
- 3: Guitar (visual proxy: guitarist character mesh)
- 4: Vocalist (UPoseableMeshComponent — supports bone manipulation)
- 5: Violin/strings

Vocalist features:
- Arms repositioned out of T-pose via bone rotation
- Head bob animation synced to bass

Dynamic placement: when fewer instruments visible, they compress toward stage center. Scale: 5 visible = full spread, 2 visible = 50% compressed, 1 visible = dead center.

### Genre-aware visibility (the rules)
| Detected genre family | Visible instruments |
|---|---|
| Classical | Piano + Violin + Mic + Vocalist |
| Jazz | Piano + Drums + Mic + Vocalist |
| Rock/Metal | Drums + Guitar + Mic + Vocalist |
| Hip-hop | Drums + Mic + Vocalist |
| Electronic | DJ booth + Mic + Vocalist (no band) |
| Pop / Funk | Full band visible |
| Unknown | Detection-driven (whoever's tag passes threshold) |

Order of operations: tag fallback (low threshold) → family override (final). Family override always wins over tag fallback.

### Music reactivity
- Bass-gating wash lights (lights dim when bass low, bright when high)
- Snare white-flash overlay on every snare hit
- Kick-driven hue stepping (color advances per beat)
- Scene re-roll on big intensity changes
- Speaker cone Y-axis vibration with bass
- Per-light micro-flicker (3-5%)
- Camera shake on bass drops (may need proper UCameraShakeBase class for reliability)
- Crowd bobbing — 120 audience members jump on kick, sway with arousal, continuous bass bob
- Red flame wash overlay on rock chorus moments
- Pyro bursts (1-2 per song, rock/electronic only, on peak)
- Photographer flashes (more frequent on peak scenes)
- CO2 jets (50% random fire on snare)

### Visual quality
- Cinematic post-process volume (bloom, vignette, saturation, exposure, lens flare, film grain)
- High-quality volumetric fog (GridPixelSize=4, GridSizeZ=192)
- High-quality light shafts (godrays)
- 16x anisotropic filtering
- Soft light source radii for cinematic beam falloff

### Audio pipeline (`AffectiveAudioActor`)
- 5-D affective scores: Arousal, Valence, Timbre, Rhythm, Intensity
- Top-15 genre tags per category — family-aggregated (15+ rock subgenres → RockMetal family)
- Bass / Mid / Treble levels (DSP-derived)
- Kick / Snare / Hi-hat onset detection
- Silence detection (3-second timeout, fades all lights out)
- Time-based hue oscillation + stepped palette rotation every 30s

## Sidecar setup (Python)

```powershell
cd C:\Users\mertf\MyProjects-main\DesignProject\ai_backend
.\.venv\Scripts\Activate.ps1
python python_sidecar.py
```

Uses VoiceMeeter Out B1 by default. Sends JSON to `127.0.0.1:17777`. Top-15 tags per category.

If sidecar UDP fails to connect, UE5's in-engine ONNX takes over — lighting still works.

## Build instructions

### Live Coding (most edits)
`Ctrl + Alt + F11` in UE5 editor. Takes 5-10s. Use for `.cpp` body changes.

### Full rebuild (after header changes)
1. Close editor entirely
2. In Visual Studio: Build → Rebuild Solution. Takes 2-5 min.

OR via command line:
```powershell
& "C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat" MusicAI_VisualizerEditor Win64 Development -Project="C:\Users\mertf\MyProjects-main\DesignProject\MusicAI_Visualizer\MusicAI_Visualizer.uproject" -waitmutex
```

### Common compile gotchas (will save you debugging time)
- **`IN` and `OUT` are Windows preprocessor macros** — never name local variables `IN` or `OUT`. They'll be silently stripped, causing 100+ cascade errors. Use `TagNames` / `OutPcts` / etc.
- **`SetBoneRotationByName` is on `UPoseableMeshComponent`**, NOT `USkeletalMeshComponent`. Use poseable mesh for runtime bone manipulation.
- **`APlayerCameraManager::GetActorLocation/Rotation` are private** — cast to `AActor*` first to use inherited public versions.
- **`-WarningsAsErrors` is on** — variable shadowing (e.g., shadowing a lambda named `Flash` with a local `Flash`) fails the build.
- **Functions used must be declared BEFORE their use** — no forward-declarations of static helpers. The genre `EGenreFamily` enum + helpers were moved above `PickScene` for this reason.
- **UE5 property defaults don't auto-apply to existing actor instances.** When you change a C++ default, the editor still shows the OLD serialized value. User must click yellow reset arrow or manually retype.

## Editor properties to know

Under `ConcertStageDirector` Details panel:

### Most-tuned categories
- `Stage|Geometry` — StageWidth 8000, Depth 3500, TrussHeight 3500
- `Stage|Venue` — `bSpawnVenue=true`, 20000×30000×5500
- `Stage|Instruments` — Scale + YawDeg properties per instrument
- `Stage|Quality` — `bCinematicMode=true`, bloom 0.5, vignette 0.4, volumetric boost 3.5
- `Stage|Reactivity` — `bStrongMusicReactivity=true`, strength 1.5
- `Stage|Dynamics` — color hue cycling parameters
- `Stage|LED` — `bSpawnLedPanels=true`, 3 panels, emissive 30
- `Stage|Spectrum` — `bSpawnSpectrumAnalyzer=true`, 48 bars
- `Stage|Haze` — 24 puffs, intensity 80
- `Stage|CrowdFlash` — `bEnableCrowdFlashes=true`, 36 lights
- `Stage|Pyro` — `bEnablePyroBursts=true`, 60s cooldown
- `Stage|CO2` — `bEnableCO2Jets=true`, 8 jets
- `Stage|DJBooth` — `bSpawnDJBooth=true`
- `Stage|Fixtures` — `bSpawnLightFixtures=true`, fixture size 35
- `Stage|Structure` — `bSpawnStageStructure=true`, platform height 200
- `Stage|SideLED` — side LED bars on towers

### Mesh override slots (drag asset to assign without recompile)
- `Drum Mesh Override`
- `Mic Mesh Override`
- `Piano Mesh Override`
- `Guitar Mesh Override`
- `Vocalist Mesh Override` (skeletal)
- `Violin Mesh Override` (loads from `/Game/Instruments/violin/violin.violin` by default)
- `Disco Ball Mesh Override`
- `Audience Mesh Pool` (Renderpeople meshes)
- `Venue Floor/Wall/Ceiling Material Override`

## Known issues / unresolved

1. **Piano not visible during classical music** — last fix: reordered tag fallback BEFORE family override so family wins. User should verify after compiling latest changes.
2. **Vocalist arm pose** — user fixed manually in editor. Default rotations: `(-60, -20, 0)` for both arms (matching symmetric).
3. **Violin scale** — bumped to 8.0 (combined 20× with InstrumentScale=2.5) after user reported too small.
4. **Camera shake may not visibly fire** — UE5's PlayerCameraManager re-derives transform each frame; our changes might get overwritten. If shake doesn't show, would need a proper `UCameraShakeBase` subclass (~15 min more code).

## Editor setup checklist (fresh clone)

1. Clone repo
2. **Download `audioset-vggish-3.uasset` from external source** (275 MB, NOT in git). Place at `MusicAI_Visualizer/Content/audioset-vggish-3.uasset`.
3. Open `.uproject` in UE5.7
4. Compile (Build → Build Solution in Visual Studio)
5. In Outliner → `ConcertStageDirector` → Details panel:
   - Verify all `bSpawn*` toggles are checked
   - Verify dimensions match values above
   - Drag instrument mesh assets into override slots if auto-load paths fail

## What the user likely wants next

In priority order (please push back — current state is enough for demo):

1. **Live AI overlay HUD** (~45 min) — top corner showing top-3 detected genres + confidence + 5-D affect vector graph. PERFECT for capstone presentation, shows the ML working live.
2. **LED panel scrolling gradient** (~45 min) — colors scroll across the 3 panels instead of solid pulse
3. **Stage floor emissive grid** (~30 min) — pulsing line pattern on platform
4. **Audience floor uplighting** (~30 min) — lights from beneath the dance floor

## What's truly missing for capstone delivery (push these instead)

**The user should work on these, not more code:**
1. Final demo video recording (60-90s with great song — try Master of Puppets, Daft Punk, Bach in sequence)
2. Poster (4-6 hero screenshots + architecture diagram + accuracy numbers from training)
3. Final report writing
4. Presentation rehearsal

The technical depth is genuinely strong:
- Top-15 substring matching across 80+ MTG-Jamendo subgenre tags
- Family aggregation (rock + alternative + heavymetal + indie sum to RockMetal)
- Hybrid in-engine + sidecar inference
- 400+ music-reactive scene elements
- Genre-aware instrument visibility
- Dynamic placement based on visible-instrument count

## Personality / communication style for this user

- Senior CompE student. Wants very detailed step-by-step instructions, exact numbers, no assumptions.
- Wants you to take ownership and just do things rather than ask for confirmation.
- Sometimes gets frustrated when fixes don't work first try — assume your guesses about rig orientations, asset paths, etc. will be wrong and provide fallback options + Output Log logging.
- Prefers seeing the work done immediately rather than discussion of options.
- Don't use emojis.

## Quick-start prompt for next chat

Paste this into your next conversation:

> I'm continuing work on a UE5 music visualizer capstone project. The full project handoff is in `C:\Users\mertf\MyProjects-main\DesignProject\HANDOFF.md` — please read it first before suggesting changes. The project is feature-complete; my remaining work is polish + presentation. Specific task today: [describe what you want to do]

That single prompt + the handoff file is everything Claude needs to pick up exactly where we left off.

---

## Session log (high-level)

Major work completed across this conversation:
- Stage layout: dance floor centered under disco ball, tables/bar around perimeter
- Genre-aware lighting (80+ subgenre tags → 11 family palettes)
- Family aggregation across top-15 tags (solves single-tag flicker)
- 13 lighting scenes including HeartBeat / Cascade / Sparkle / RainbowWave / BeamStab
- Procedural venue (200m × 300m × 55m room)
- Procedural stage structure (truss, speakers, monitors, platform, backdrop)
- LED video panels (3 emissive panels behind band)
- 48-bar 3D spectrum analyzer
- Pyro bursts (rock/electronic only, rare)
- CO2 jets (snare-driven)
- Stage haze particles (24 volumetric puffs)
- Photographer audience flashes
- DJ booth (Electronic only)
- Side LED bars
- Reflective stage floor
- Genre text on backdrop
- Crowd bobbing animation with bass
- Speaker cone vibration
- Camera shake (may need polish)
- Vocalist head bob + pose adjustment (T-pose fix)
- Violin instrument added (index 5)
- Dynamic instrument placement (compress toward center when fewer visible)
- Music reactivity: bass-gating, snare white-flash, per-kick hue stepping
- Cinematic post-process volume
- Top-N audio analysis with hybrid sidecar + in-engine inference
