# Project Handoff — Continue from Here

**Project:** Real-Time Affective Light Automation (Senior Capstone, Spring 2026)
**Owner:** Mert Fahri Çakar
**Last update:** 2026-05-10 (end of nightclub-build session)
**Project root:** `C:\Users\mertf\MyProjects-main\DesignProject`

To continue with Claude in a new chat, paste this entire file as your first message and ask Claude to continue from "WHERE YOU LEFT OFF" below.

---

## WHERE YOU LEFT OFF

The nightclub venue and all major scene features are built. Last session covered:

- Built the venue: 6 cubes (floor, ceiling, 4 walls) at nightclub scale (50m × 80m × 12m)
- Applied 3 materials (M_Floor concrete, M_Wall brick, M_Ceiling rusty metal)
- Imported Renderpeople "Scanned 3D People Pack" (9 photoreal characters) into `Content/Scanned3DPeoplePack/RP_Character/`
- Audience system uses Renderpeople characters (assign via `AudienceMeshOverride` or `AudienceMeshPool` for variety)
- Tables with people standing around them (lounge/VIP zones)
- Bar stools next to people at tables
- Disco ball hanging in the middle of the audience block, rotating at runtime
- 50 RGB dance-floor lights cycling through HSV with kick/snare beat boost
- House lights for general venue illumination
- 96 grid audience members + 24 table audience members, all music-reactive

The system runs the full hybrid Python+UE5 architecture (sidecar + UE5 lighting), and the Renderpeople characters appear in the audience.

**Last issue (unresolved at end of session):** When `AudienceStartDepth` was set to 5000, the audience block ended up so far from the stage that the disco ball and dance floor lights (which the code places at the center of the audience block) ended up far from the camera and looked like they were "at the back" of the room. The recommended fix is to use `AudienceStartDepth = 2800`.

**Next things to do at start of next session:**
1. Set `AudienceStartDepth = 2800` on ConcertStageDirector (fixes dance floor / disco ball position)
2. If still not framed well, move MainCamera to Loc(0, -2500, 600)
3. Take a final screenshot to verify the look is right
4. Optional polish: assign all 9 Renderpeople characters to `AudienceMeshPool` for crowd variety
5. Optional polish: download free chair/table meshes from Fab and assign to `ChairMeshOverride` and `TableMeshOverride`
6. Final demo recording with sidecar running and 3 contrasting songs (Bach, Bohemian, Daft Punk or Metallica)
7. Submit final report (`backend_report_corrections.pdf` already prepared for the friend writing the academic report)

---

## SYSTEM STATUS — WHAT WORKS

| Component | Status |
|---|---|
| Python AI inference pipeline | ✅ 87% top-3 accuracy on benchmark |
| Python sidecar (`python_sidecar.py`) | ✅ broadcasts UDP JSON on port 17777 |
| UE5 receives Python predictions | ✅ shows `[PY] GENRE:` overlay |
| UE5 fallback ONNX inference | ✅ used if sidecar offline |
| C++ DSP (bass/mid/treble + onsets) | ✅ |
| Genre-aware lighting palette | ✅ rock=red, classical=warm, electronic=neon, etc. |
| Instrument-aware light boost | ✅ |
| Music-reactive audience system | ✅ sway with arousal, jump on kick drum |
| Instrument show/hide on detection | ✅ piano/drums/guitar appear when detected |
| Nightclub venue (cubes) | ✅ 50×80×12m enclosed, materials applied |
| Renderpeople audience figures | ✅ Dennis (or any character) assigned via override |
| Tables with people standing around | ✅ 8 tables, 3 people each |
| Bar stools next to people | ✅ |
| Disco ball | ✅ rotating, hanging below ceiling |
| RGB dance floor lights | ✅ 50 lights cycling HSV, beat-boosted |
| House lights | ✅ warm ambient illumination |
| CSV logging with top tags | ✅ |
| AudienceMeshPool for crowd variety | ✅ coded — needs all 9 characters assigned in editor |

---

## CURRENT VENUE — EXACT TRANSFORMS

```
ALL ROTATIONS = (0, 0, 0)

Cube         Location              Scale
─────────────────────────────────────────────────
floor        (0, 0, -10)           (50, 80, 0.2)
ceiling      (0, 0, 1210)          (50, 80, 0.2)
wall_back    (0, 4000, 600)        (50, 0.2, 12)
wall_front   (0, -4000, 600)       (50, 0.2, 12)
wall_left    (-2500, 0, 600)       (0.2, 80, 12)
wall_right   (2500, 0, 600)        (0.2, 80, 12)
```

(Some users have moved to bigger scale — check current values in their level.)

---

## CURRENT ConcertStageDirector PROPERTIES (recommended)

```
Location:                  (0, 3000, 0)
StageWidth:                3500
StageDepth:                1500
TrussHeight:               1100

AudienceStartDepth:        2800            ← was 5000, RECOMMENDED 2800
AudienceRows:              8
AudiencePerRow:            12              (= 96 grid audience)
AudienceFloorIntensityMax: 2000

AudienceMeshOverride:      rp_dennis_posed_004 static mesh   (or assign different)
AudienceMeshPool:          (empty — populate with all 9 RP characters for variety)
AudienceScaleMultiplier:   2.0

WashIntensityMax:          25000
BeamIntensityMax:          80000
StrobeIntensityMax:        40000
FloorIntensityMax:         12000

bSpawnTables:              true
NumTables:                 8
PeoplePerTable:            3
TableRadius:               250
ChairMeshOverride:         (empty — uses cube; replace with free Fab bar stool)
bSpawnChairs:              true

bSpawnHouseLights:         true
HouseLightIntensity:       1200

bSpawnDiscoBall:           true
DiscoBallScale:            2.0
DiscoBallRotationSpeed:    35

bSpawnDanceFloorLights:    true
DanceFloorRows:            5
DanceFloorCols:            10
DanceFloorLightIntensity:  6000
DanceFloorHueCycleSeconds: 6.0
```

MainCamera: Location (0, -2500, 600), Rotation (-3, 90, 0).

---

## FILE INVENTORY (do not modify unless intentional)

### Python backend (`ai_backend/`)
- `python_sidecar.py` — current production runtime
- `lstm_model.py` — model definition (StatefulMusicBottleneck + InferenceWrapperV2)
- `train.py` — training script
- `test_genre.py` — batch evaluation
- `diagnostic_pca.py` — confirms PCA postprocessing requirement
- `export_onnx_v2.py` — exports AestheticBrain_v2.onnx
- `export_vggish_pca.py` — generates VGGishPCAConstants.h
- `export_vggish_mel.py` — generates VGGishMelConstants.h
- `cpp_converter.py` — generates NormalizationConstants.h

### UE5 source (`MusicAI_Visualizer/Source/MusicAI_Visualizer/`)
- `AffectiveAudioActor.h/.cpp` — audio capture, DSP, UDP receiver, ONNX fallback
- `ConcertStageDirector.h/.cpp` — stage, lights, audience, tables, disco ball, dance floor
- `MusicAI_Visualizer.Build.cs` — module deps (NNE, Niagara, Json, Networking, Sockets)
- `NormalizationConstants.h` — auto-generated z-score
- `VGGishPCAConstants.h` — auto-generated PCA matrix
- `VGGishMelConstants.h` — auto-generated mel filterbank

### Documents
- `PROJECT_MASTER_BLUEPRINT.md` — full architecture description
- `backend_report_corrections.pdf` — corrections doc for the existing report (for friend writing academic report)
- `backend_report_v2.md` — alternative new report (markdown)
- `HANDOFF.md` — this file

---

## HOW TO RUN

```powershell
# Terminal 1 — Python AI sidecar
cd C:\Users\mertf\MyProjects-main\DesignProject\ai_backend
python python_sidecar.py

# UE5 — open project, press Play
# Spotify — play music (audio routes via VoiceMeeter Out B1)
```

---

## CRITICAL CONTEXT

### MicTest actor
DO NOT delete `MicTest`. Its AudioCapture component feeds Windows recording device audio into UE5's submix. Without it, no audio analysis.

### Audio routing
- Spotify → Voicemeeter Input (Windows playback default)
- VoiceMeeter routes to: A1 (headset), B1 (recording bus)
- Voicemeeter Out B1 = Windows recording default
- Both Python sidecar AND UE5 capture from B1 in parallel
- UnrealEditor.exe audio output bypasses VoiceMeeter (set per-app in Volume Mixer) to avoid feedback loop

### Hybrid architecture rationale
Earlier iterations tried pure-UE5 ONNX inference. Found four bugs (PCA missing, z-score missing, mel filterbank wrong, LSTM saturation). Even after all fixes, residual numerical precision differences with VoiceMeeter audio path prevented exact match with Python. Sidecar pattern guarantees parity. This is industry-standard for ML in games.

### Verified test results
- Bach (full classical) → soundtrack/emotional/piano (lighting: warm white)
- Bohemian Rhapsody from minute 4 (rock) → rock/energetic/bass (red strobes)
- Metallica Master of Puppets → rock 49% / energetic / bass 46% sustained for 2 min
- Daft Punk → electronic/synthesizer (magenta/cyan)

---

## USER PREFERENCES (for new chat)

- Prefers very detailed step-by-step instructions, exact numbers, no assumptions
- Uses cubes for venue walls (rotation 0,0,0; scale axes determine wall vs floor)
- Working on Windows 11 + RTX 4080 + UE 5.7
- Uses VoiceMeeter Banana/Potato for audio routing
- Uses Spotify as live audio source
- Has VS Code + Python venv at `.venv/` in project root
- Uses Renderpeople characters for audience (assigned via AudienceMeshOverride)
- May get frustrated if instructions aren't concrete — pivot to exact actions immediately

---

## TO START NEW CHAT

1. Open new Claude conversation (any project — memory files for this project will auto-load if you're in the same project directory)
2. Paste this entire HANDOFF.md content as the first message
3. Add: "Continue from WHERE YOU LEFT OFF — set AudienceStartDepth=2800 and tell me what's next"
4. Claude will pick up exactly where this conversation ended

For deeper context, also reference `PROJECT_MASTER_BLUEPRINT.md` in the project root.
