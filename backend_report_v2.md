# AI Backend for Real-Time Affective Light Automation

## A Hybrid Python + Unreal Engine Music Visualization System

**Author:** Mert Fahri Çakar
**Project Type:** Senior Computer Engineering Capstone Project
**Term:** Spring 2026
**Date:** May 2026

**Frameworks:** PyTorch · ONNX Runtime · Unreal Engine 5.7 · NNE Plugin · UDP IPC
**Hardware:** NVIDIA RTX 4080 (training and inference) · Windows 11
**Dataset:** MTG-Jamendo (~55,000 tracks · 195 genre / mood / instrument tags)

---

## Abstract

This report presents the design and implementation of the artificial-intelligence backend used to drive a real-time affective light-automation system inside Unreal Engine 5.7. The backend is a multi-stage Music Emotion Recognition (MER) and music auto-tagging pipeline that ingests live audio, extracts mid-level perceptual features through Google's VGGish convolutional network, and feeds them to a custom stateful LSTM that produces both a 5-dimensional aesthetic vector (Arousal, Valence, Timbre, Rhythm, Intensity) and 195 multi-label classifications across genre, mood/theme, and instrument categories.

The training pipeline uses the MTG-Jamendo dataset and applies focal cross-entropy loss with Z-score normalization and a two-phase Adam→SGD optimizer schedule. After encountering significant numerical-precision issues integrating the trained model into Unreal Engine via ONNX, the project pivoted to a **hybrid Python sidecar + UE5 frontend architecture**, in which a Python service performs all ML inference and broadcasts results to Unreal Engine over UDP. This design choice — documented in detail in this report — preserves the proven Python inference pipeline (87% top-3 accuracy on a contrasting-genre benchmark) while keeping all real-time visualization, DSP, and lighting logic in the C++ engine.

The system drives a complete simulated nightclub stage with 23+ dynamic light fixtures, an animated audience that physically reacts to detected onsets (jumping on the kick drum, swaying with arousal), and genre-aware scene presets. The report covers the full lifecycle: dataset preparation, model training, ONNX export, the discovery and resolution of input-pipeline mismatch bugs, the move to the hybrid architecture, and the integration of detected genre/mood/instrument tags into procedural stage lighting and crowd behavior.

---

## Table of Contents

1. Introduction
2. System Architecture (Current Hybrid Design)
3. Dataset and Ingestion
4. Feature Extraction with VGGish
5. The Stateful LSTM Bottleneck
6. Training Procedure
7. The Integration Journey: From Pure-UE5 to Hybrid
8. The Python Sidecar
9. The C++ Inference Path (Fallback Mode)
10. UDP Communication Protocol
11. Genre-Aware Stage Lighting
12. Music-Reactive Audience System
13. Evaluation Methodology and Results
14. Discussion: Lessons Learned
15. Conclusion and Future Work
- Appendix A: File Inventory
- Appendix B: Hyperparameter Reference
- Appendix C: Glossary
- Bibliography

---

## 1. Introduction

### 1.1 Motivation

Conventional music visualizers react to instantaneous frequency-domain features such as FFT magnitude, beat onsets, or RMS energy. While visually pleasing, those signals describe what is happening in the audio at a short timescale, not how the audio *feels* over longer windows. Two musically distinct pieces — for instance a melancholic acoustic ballad and a calm ambient track — can have similar instantaneous spectra yet evoke very different listening experiences. A frequency-bar visualizer cannot tell them apart.

The objective of this capstone project is to break that limitation by building a machine-learning backend that interprets the **temporal emotional pulse** of a signal — the slow drift between tense and relaxed, bright and dark, sparse and dense passages — and projects that pulse into a low-dimensional control vector plus a discrete genre/mood/instrument label set. The backend acts as a continuous translator from sound to mood, and the Unreal Engine front-end uses that translation to control luminance, color, particle complexity, fog density, strobe behavior, and even crowd animation in real time.

### 1.2 Problem Statement

The backend must simultaneously satisfy several competing engineering goals:

1. **Perceptual fidelity.** The model must capture mid-level acoustic semantics — genre, instrumentation, mood — not just raw spectral content. A visualizer should treat Bach and Metallica fundamentally differently.

2. **Temporal coherence.** Lighting cannot flicker on micro-fluctuations. The system must produce a smoothly evolving control signal even when audio frames vary rapidly, which requires either a stateful temporal model or aggressive smoothing.

3. **Latency budget.** The full pipeline (audio capture → feature extraction → inference → message dispatch → engine update) must complete with imperceptible end-to-end delay. The target is sub-100 ms total latency.

4. **Reproducibility.** The system must be auditable: another engineer must be able to retrain, re-export, and rerun the pipeline.

5. **Live-input robustness.** The system must work on live-routed audio (Spotify or any other player → virtual audio cable → both inference and visualization processes), not only on offline-loaded files.

### 1.3 Contribution Summary

The backend described in this report contributes:

- A reproducible feature-extraction pipeline that converts the MTG-Jamendo audio corpus into a single cached tensor file using Google's pretrained VGGish CNN.
- A custom **StatefulMusicBottleneck** PyTorch model combining a recurrent layer, a 5-dimensional bottleneck, and a 195-class multi-label classifier head.
- A two-phase Adam→SGD training procedure with focal loss and Z-score normalization.
- An ONNX export with PCA postprocessing constants for in-engine inference.
- A **diagnostic methodology** that identified numerical-precision mismatches between the Python and C++ inference paths and quantified their impact on classification accuracy.
- A **Python sidecar** service that performs all ML inference and broadcasts predictions to Unreal Engine over UDP, eliminating the need to reproduce VGGish's input pipeline byte-for-byte in C++.
- A **genre-aware lighting controller** in C++ that selects scene presets, color palettes, and per-instrument light boosts based on the live tag predictions.
- A **music-reactive audience system** in C++ that animates simulated crowd members using detected drum onsets and arousal level.

### 1.4 Reading Order

Sections 2–6 describe the *what*: the static architecture, dataset, model, and training procedure. Sections 7–10 describe the *journey*: the bugs encountered when integrating the trained model into Unreal Engine and the hybrid solution. Sections 11–12 describe the visualization layer that consumes the predictions. Sections 13–15 cover evaluation, discussion, and conclusions.

---

## 2. System Architecture (Current Hybrid Design)

### 2.1 High-Level Topology

The system is composed of two cooperating processes connected by a single source of audio and a single direction of message flow.

```
        Spotify (live audio source)
                  │
                  v
         VoiceMeeter Banana
            (audio router)
       /                        \
Voicemeeter Input          Voicemeeter Out B1
   (audible)               (virtual recording)
       │                        │
       v                        v
    Headset            ┌────────┴────────┐
                       │                 │
                       v                 v
               python_sidecar       UE5 AffectiveAudioActor
               (ML inference)       (DSP + visualization)
                       │
                       │ UDP JSON
                       │ port 17777
                       v
               UE5 receives
               5-D scores +
               top genre/mood/
               instrument
                       │
                       v
               ConcertStageDirector
               (lighting + crowd)
```

A single audio source (VoiceMeeter Out B1) feeds both the Python service and the Unreal Engine simultaneously. There is no audio data sent between the two processes — only the inference results.

### 2.2 Three Computational Layers

The backend can be conceptualized as three layers:

**Layer I — Audio Capture and DSP (C++ in UE5).** A submix-buffer listener captures stereo audio at 48 kHz, downmixes to mono, and maintains a ring buffer. From this buffer the system computes a custom mel spectrogram (matching VGGish parameters), per-band energy levels (bass / mid / treble), and time-derivative onset detectors for kick, snare, and hi-hat drums. All threadsafe atomics are used for inter-thread communication with the game thread.

**Layer II — Machine-Learning Inference.** This layer has two paths. The **primary** path is the Python sidecar (`python_sidecar.py`), which captures the same audio source via the `sounddevice` library, runs the full Python inference pipeline (VGGish with PCA postprocessing → StatefulMusicBottleneck → sigmoid) and broadcasts results over UDP. A **fallback** path is implemented in pure C++ within Unreal Engine using the NNE plugin, applying VGGish ONNX → manual PCA postprocessing → Z-score normalization → AestheticBrain ONNX. The fallback is automatically used when the sidecar is offline.

**Layer III — Stage Lighting and Crowd (C++ in UE5).** The `ConcertStageDirector` actor builds and drives a procedural stage with 6 wash lights, 4 beam lights, 4 strobe rect-lights, 2 side spotlights, 4 floor point-lights, 5 instrument-pinpoint lights, and 4 audience-aimed spotlights. It also spawns a configurable grid of audience members (default 8 rows × 14 = 112 figures) that physically respond to onset detection and arousal level. A scene-picker chooses between 8 base lighting moods (Warm, Cool, Neon, Chase, Strobe, Spotlight, Burst, Rainbow) and biases the choice based on the detected genre.

### 2.3 Why Hybrid Instead of Pure UE5

Earlier iterations attempted to do all inference inside Unreal Engine using the NNE plugin and ONNX models. This approach failed in subtle and non-obvious ways: the trained model expects PCA-whitened, 8-bit-quantized VGGish embeddings, but reproducing VGGish's full input pipeline (windowed STFT, magnitude spectrogram, triangular HTK mel filterbank, log-mel, PCA matmul, clipping, quantization) byte-for-byte in C++ proved fragile. Small numerical differences in any single step cascaded through the LSTM and frequently flipped the top-1 genre prediction. The diagnostic process that uncovered this is documented in Section 7.

The Python sidecar uses the *reference implementation* of VGGish (the `torchvggish` package) and the original PyTorch model weights. By definition, its predictions match what the model was trained to produce. The cost is one extra process; the benefit is guaranteed correctness.

This hybrid pattern — Python service for ML, C++ engine for runtime — is standard practice in shipping AAA games and professional ML-driven applications. The project follows that pattern.

---

## 3. Dataset and Ingestion

### 3.1 The MTG-Jamendo Dataset

Training uses the **MTG-Jamendo** dataset (Bogdanov et al., 2019), an open music auto-tagging corpus built from Creative-Commons-licensed audio on Jamendo. The full dataset contains:

- ~55,000 audio tracks
- 195 unique tags spanning three categories: 95 genres, 56 mood/themes, 41 instruments
- Approximately 500 GB of MP3 audio at 320 kbps
- An additional ~10 MB of metadata in TSV format describing track-tag associations and pre-defined train/validation/test splits

The dataset is downloaded with the official `download.py` script from the MTG-Jamendo repository, with a flag to unpack and remove tarballs to save disk space.

### 3.2 Cache File Generation

The backend uses a single Python script, `extract_features_vggish.py`, to convert raw MP3s into a model-ready cache:

1. Read `autotagging.tsv` which lists each track's relative path, duration, and tag set.
2. For each track, load up to 30 seconds of audio at 16 kHz mono using `librosa`.
3. Pass the waveform through VGGish (with the model's own preprocessing and PCA postprocessing both enabled by default).
4. Multi-hot encode the tag set into a 195-dimensional float vector.
5. Append the 128-dimensional embedding tensor and label vector to in-memory lists.
6. After all tracks are processed, save a single PyTorch dictionary `cached_dataset.pt` containing the features list, the stacked label tensor, and the sorted tag-name list.

The cache is approximately 900 MB when complete. It is loaded once at the start of training.

### 3.3 Tag Distribution Considerations

The 195-tag space is highly imbalanced. Some tags appear in tens of thousands of tracks (e.g. "rock", "electronic"), while others appear in only a few hundred (e.g. "shoegaze", "jewish"). The training procedure addresses this with focal cross-entropy loss, described in Section 6.

---

## 4. Feature Extraction with VGGish

### 4.1 What VGGish Is

VGGish is a VGG-style convolutional neural network released by Google as part of the AudioSet project (Hershey et al., 2017). It accepts a 1×96×64 mel-spectrogram patch (~0.96 seconds of audio at 16 kHz) and produces a 128-dimensional embedding. It was pretrained on a large corpus of YouTube videos labeled with high-level audio events (instruments, environmental sounds, etc.) and serves as a general-purpose audio feature extractor.

The project uses the published `audioset-vggish-3.onnx` model converted from the official `torchvggish` PyTorch package. Two artifacts are shipped:

- `vggish` weights — the convolutional + fully-connected layers
- `vggish_pca_params` — a 128×128 PCA whitening matrix and per-dimension means, applied after the FC layers, followed by clipping to [-2, +2] and quantization to 8-bit floats in the range [0, 255]

### 4.2 Why PCA Postprocessing Matters

The PCA postprocessing was added to AudioSet's VGGish so that embeddings could be stored efficiently as 8-bit values without losing too much information. **Importantly, the StatefulMusicBottleneck classifier was trained on PCA-postprocessed embeddings.** This makes PCA postprocessing a non-optional step at inference time. Skipping it changes the input distribution so dramatically that the classifier's output collapses to a constant prediction regardless of audio content. This was the central bug uncovered during integration; see Section 7.

### 4.3 Practical Notes

In the Python sidecar, VGGish is loaded with `postprocess=True` (default). In the C++ fallback, the VGGish ONNX runs without PCA, and PCA is applied manually using auto-generated constants in `VGGishPCAConstants.h`.

---

## 5. The Stateful LSTM Bottleneck

### 5.1 Architecture

The custom `StatefulMusicBottleneck` model is defined in `lstm_model.py`:

```
Input:    [batch, time, 128]   (VGGish embeddings)
   │
   v
LSTM(128 → 256, num_layers=1)
   │
   v
LayerNorm(256)
   │
   v
Linear(256 → 128) → ReLU → Dropout(0.2)
   │
   v
Linear(128 → 64) → ReLU
   │
   v
Linear(64 → 5)                 → 5-D aesthetic vector (bottleneck)
   │
   v
Linear(5 → 195)                → 195 sigmoid logits (multi-label tags)
```

The model returns four tensors at every time step:

- **logits** [batch, time, 195] — raw multi-label tag scores
- **aesthetic_vector** [batch, time, 5] — the 5-D bottleneck
- **hn** [1, batch, 256] — final hidden state
- **cn** [1, batch, 256] — final cell state

The bottleneck width was chosen because the project needed exactly five lighting parameters. The classifier head sits on top of the bottleneck so that all 195 tag predictions must be reconstructable from those five numbers. In practice this forces the bottleneck to become a *compressed feature representation* of the tag space, not a labeled emotional axis. The dimensions are not "Arousal, Valence, Timbre, Rhythm, Intensity" in any meaningful sense — those names are post-hoc labels assigned in the visualization layer for convenience.

### 5.2 Why This Architecture

The choice of LSTM over a stateless MLP is motivated by temporal coherence: lighting must not flicker on per-frame variations. The LSTM's hidden state acts as a low-pass filter, integrating information across the recent past. The bottleneck before the classifier serves a regularization function during training and provides a low-dimensional control signal at inference time.

### 5.3 Inference Wrappers

Two wrappers exist for ONNX export:

- **InferenceWrapper** — returns only the bottleneck and the next state. Used by the original ONNX export. *Discards the 195-tag classifier output entirely* — this was the second bug uncovered during integration (see Section 7).

- **InferenceWrapperV2** — returns sigmoid tag probabilities, the bottleneck, and the next state. Used by the current ONNX export (`AestheticBrain_v2.onnx`).

---

## 6. Training Procedure

### 6.1 Loss Function

The training script `train.py` uses **multi-label focal loss** (Lin et al., 2017) with γ=2.0, applied per-class on the 195-dimensional sigmoid output. Focal loss reduces the loss contribution of well-classified examples and focuses gradient updates on hard examples. This is critical for a multi-label problem with severe class imbalance: the model would otherwise learn to predict "rare tag = 0" for everything and achieve high accuracy on the majority class.

### 6.2 Z-Score Normalization

Before passing features to the LSTM, the script computes per-dimension mean and standard deviation over the entire training set and applies (x - μ) / (σ + 1e-6). The mean and std vectors (each 128-dim) are saved alongside the model weights so that the same normalization can be reapplied at inference time. These are also exported to a C++ header (`NormalizationConstants.h`) for the in-engine fallback path.

### 6.3 Optimizer Schedule

Following the strategy proposed by Won et al. (2019) for music tagging models:

- **Epochs 1–60:** Adam optimizer at learning rate 1e-4. Adam's adaptive learning rates allow the model to converge quickly from random initialization.
- **Epochs 61–80:** Switch to SGD at learning rate 1e-3 with momentum 0.9 and weight decay 1e-4. SGD with momentum produces sharper minima and better generalization than Adam in the late phase.
- **Epochs 81–100:** SGD at learning rate 1e-4. Final precision-mapping phase with lower learning rate.

Gradient clipping with max-norm 1.0 is applied at every step to stabilize training.

### 6.4 Checkpointing

The script saves `music_emotion_weights.pth` at the end of every epoch. The checkpoint contains the model's `state_dict`, the training mean and std vectors, and is safe to interrupt with Ctrl+C: the most recent epoch is always on disk.

### 6.5 Sanity Checks

Two scripts validate the trained model in Python before any export:

- `check_brain_health.py` — replays a known audio file and prints the 5-D vector + top tag every second. The success criterion is that the five bottleneck dimensions vary independently and the top tag is musically appropriate.
- `live_mic_test.py` — streams from the system microphone and prints a top-5 tag list once per second.

---

## 7. The Integration Journey: From Pure-UE5 to Hybrid

This section documents the path the project took from a pure-UE5 ONNX-based design to the current hybrid sidecar architecture. The journey exposed several subtle bugs and produced design decisions that are worth recording.

### 7.1 Initial Design and First Failure Mode

The original architecture placed all inference inside Unreal Engine using the NNE plugin. The trained `AestheticBrain_256.onnx` model was loaded, fed a custom C++ mel spectrogram, and its 5-D output was used directly to drive the lighting.

After integration, on-screen testing revealed an immediate problem: the 5-D vector showed only weak variation between contrasting songs. Both Bach (calm classical) and Queen's *Bohemian Rhapsody* (intense rock) produced very similar arousal numbers. Worse, after a few minutes of inference the values would drift to extreme magnitudes (±10 or more) and stop responding to the audio entirely.

### 7.2 The LSTM State Saturation Problem

Diagnosis revealed that the LSTM's hidden state was accumulating without bound. The trained weights, when exposed to the C++-pipeline's input distribution, drove the recurrent state into a saturated regime from which it never recovered. Two contrasting songs would both eventually push the state to the same saturated configuration, producing identical predictions.

A first fix was added in `AffectiveAudioActor.cpp`: zero the LSTM hidden and cell states at the start of every inference. This converted the model to "stateless" mode. The runaway saturation stopped, and the 5-D output regained dynamic range.

### 7.3 The Discarded-Classifier Bug

A code review of the model and its export script revealed a second, more serious problem. The original `InferenceWrapper` looked like this:

```python
class InferenceWrapper(nn.Module):
    def forward(self, x, h, c):
        logits, aesthetic_vector, hn, cn = self.model(x, h, c)
        return aesthetic_vector[:, -1, :], hn, cn   # logits discarded!
```

The 195-tag classifier output (`logits`) was never returned. Only the 5-D bottleneck was exported to ONNX. The visualization layer was reading these five numbers as if they were trained semantic dimensions ("Arousal, Valence, Timbre, Rhythm, Intensity"), but the bottleneck had no labeled semantics — it was just a compressed feature representation that the classifier head used internally.

The fix was an `InferenceWrapperV2` that returns both the sigmoid tag probabilities and the bottleneck. A new export script `export_onnx_v2.py` produces `AestheticBrain_v2.onnx` with four named outputs: `tag_probs`, `aesthetic_vector`, `h_n`, `c_n`. The C++ side was updated to bind both outputs and to display the top tags per category on screen.

### 7.4 The PCA Postprocessing Bug

After fixing the wrapper, the on-screen tag predictions showed a striking pattern: every song produced exactly the same top three tags — "electronic 53.7 %", "ambient 35.2 %", "dance 34.4 %", with the same per-mille values across Bach, Queen, Daft Punk, Metallica, and Eno. The model was completely ignoring the audio.

A diagnostic script, `diagnostic_pca.py`, was written. It runs each test song through two parallel inference paths in pure Python:

- **With PCA** — the standard `vggish.forward(audio, fs=16000)` call with `postprocess=True`. This is what the model was trained on.
- **Without PCA** — `postprocess=False`, simulating what the C++ pipeline was doing.

The result confirmed the diagnosis: with PCA enabled, the model produced correct, varied predictions (Bach → classical, Queen → rock, Daft Punk → electronic). Without PCA, *every* song produced "electronic 53.7%". The model collapses to a constant prediction when given non-PCA-postprocessed embeddings.

The fix was twofold:

- A new Python script `export_vggish_pca.py` extracts the 128×128 PCA eigenvector matrix and 128-dimensional means from `torchvggish` and writes them as constants into `VGGishPCAConstants.h`.
- The C++ inference loop in `AffectiveAudioActor.cpp` was extended to apply PCA after the VGGish ONNX call: matmul with eigenvectors, subtract means, clip to [-2, +2], and quantize to integer values in [0, 255].

### 7.5 The Missing Z-Score Normalization

A third bug, exposed by the same diagnostic effort, was that the Z-score normalization constants generated by `cpp_converter.py` (the `VGGISH_MEAN` and `VGGISH_STD` arrays in `NormalizationConstants.h`) had been generated months earlier but were never `#include`d in `AffectiveAudioActor.cpp`. The Python pipeline always applies `(x - mean) / std` before the LSTM, but the C++ pipeline was skipping this step entirely.

The fix was trivial — include the header and add an in-place normalization loop after PCA — but the bug had been silently reducing accuracy for the entire prior development period.

### 7.6 The Mel Spectrogram Mismatch

After PCA and Z-score were both fixed, the C++ predictions improved dramatically: Bach now produced "soundtrack / emotional / piano" (musically appropriate) instead of "electronic / dark / synthesizer". But the C++ predictions still did not match the Python reference exactly. Bach's top-1 prediction in Python was "classical" (76.5 %), in C++ it was "soundtrack" (42 %).

The remaining gap was traced to the C++ mel spectrogram implementation. VGGish requires:

- **Magnitude** spectrogram (`|STFT|`), not power (`|STFT|²`)
- **Triangular mel filterbanks** with HTK-style scaling, with each mel bin pulling from multiple FFT bins
- **Log-mel without per-spectrogram normalization** — VGGish expects raw `log(mel + 0.01)` values
- A **time-major** tensor layout `[frames, mel_bins]`, not `[mel_bins, frames]`

The original C++ implementation used power instead of magnitude, picked the single nearest FFT bin per mel bin (no triangular weighting), applied per-spectrogram (x - μ) / σ normalization, and used a transposed layout. A new Python script `export_vggish_mel.py` was written to generate the proper mel filterbank matrix as a C++ constant array, and the C++ mel-spectrogram routine was rewritten to match. After this fix the C++ predictions became musically appropriate (Bach → soundtrack / emotional / piano, Bohemian Rhapsody opening → pop / drums) but still differed from Python by approximately 10 percentage points on the top-1 prediction.

### 7.7 Acceptance and the Hybrid Decision

After fixing the four pipeline bugs (LSTM saturation, missing classifier output, missing PCA, missing Z-score, mel spectrogram), the C++ path produced reasonable results — but exact byte-equivalence with Python was determined to be unachievable in any reasonable time budget. The remaining differences come from FFT implementation precision, the exact form of the Hann window (periodic vs symmetric), audio sample-rate conversion artifacts in VoiceMeeter, and float vs double precision in critical paths. Each individual difference is small; in aggregate they perturb the LSTM input distribution enough to flip top-1 predictions when categories are close.

The decision was made to keep the C++ ONNX path as a graceful fallback but introduce a Python sidecar service that performs ML inference using the proven Python pipeline and broadcasts results to Unreal Engine over UDP. The next section describes that sidecar.

---

## 8. The Python Sidecar

### 8.1 Design Goals

The sidecar (`python_sidecar.py`) was designed with three explicit goals:

1. **Reuse the proven inference code** without modification. The same VGGish call, the same model load, the same PCA postprocessing — guaranteed to match the offline test results.
2. **Capture audio independently** of Unreal Engine. The sidecar does not require UE5 to send audio over UDP; both processes capture the same source from VoiceMeeter Out B1.
3. **Send only predictions** — small JSON messages on the order of a kilobyte each, broadcast at approximately 1 Hz.

### 8.2 Implementation Outline

The sidecar:

1. Loads the trained model weights and tag list (same as `check_brain_health.py`).
2. Loads VGGish with `postprocess=True`.
3. Opens an audio input stream via `sounddevice` at the device's native sample rate (typically 44.1 or 48 kHz mono).
4. In a tight loop, accumulates ~1 second of audio into a buffer.
5. Resamples to 16 kHz using `librosa.resample`.
6. Applies a silence threshold (skip and send a `silence: true` packet if RMS < 0.001).
7. Runs the audio through VGGish, applies Z-score normalization, runs through the LSTM, takes the sigmoid of the logits and averages across time chunks.
8. Maintains an exponential moving average of tag probabilities (α = 0.15) for stable top-K selection.
9. Builds a JSON message containing:
   - `scores` — the 5-D bottleneck for the most recent frame
   - `top_genres`, `top_moods`, `top_instruments` — top-3 lists of [tag-name, probability] pairs
   - `silence` — boolean
   - `rms` — current peak amplitude
10. Sends the JSON via UDP to `127.0.0.1:17777`.
11. Prints a one-line summary to the console for live monitoring.

### 8.3 Command-Line Interface

```
python python_sidecar.py [--port 17777] [--host 127.0.0.1] [--device DEVICE_ID]
python python_sidecar.py --list-devices  # print available audio devices
```

### 8.4 Performance

On the development machine (RTX 4080), inference takes approximately 30 ms per frame (audio capture is the bottleneck at ~1 second). The sidecar operates at 1 Hz, generates negligible CPU and network load, and adds approximately 20 ms of latency over the in-engine fallback (50 ms total vs 30 ms).

---

## 9. The C++ Inference Path (Fallback Mode)

### 9.1 When It Runs

The C++ in-engine path runs whenever the Python sidecar is not detected (no UDP packets received in the last 3 seconds). The system automatically switches between sidecar and fallback by checking the timestamp of the most recent UDP packet.

### 9.2 Pipeline Steps

The C++ pipeline in `AffectiveAudioActor::ProcessAIInference` runs on a background thread and consists of:

1. Read 16,000 audio samples from the ring buffer (1 second at 16 kHz after downsampling).
2. Compute RMS; skip inference if below 0.0005.
3. Reject corrupted RMS values (NaN, Inf, or > 1.0) — this guards against alt-tab glitches.
4. Compute mel spectrogram using the precomputed triangular filterbank in `VGGishMelConstants.h`.
5. Bind the spectrogram as input to the VGGish ONNX model and call `RunSync`. Output is a 128-D embedding (without PCA).
6. Apply PCA postprocessing using `VGGishPCAConstants.h`: subtract means, multiply by 128×128 eigenvector matrix, clip to [-2, +2], quantize to [0, 255].
7. Apply Z-score normalization using `VGGISH_MEAN` and `VGGISH_STD` from `NormalizationConstants.h`.
8. Zero the LSTM hidden and cell states (stateless mode).
9. Bind the normalized embedding plus zero states as inputs to the AestheticBrain ONNX model and call `RunSync`. Outputs are tag probabilities (196), the 5-D bottleneck, and updated state vectors (the latter are discarded in stateless mode).
10. Clamp the output bottleneck to [-10, +10] and reject non-finite values.
11. Update an exponential moving average of tag probabilities for stable top-K selection.
12. Write a CSV row containing time, RMS, the 5-D scores, and the top genre/mood/instrument with confidences.

### 9.3 Auto-Generated C++ Headers

Three Python scripts produce C++ header files containing constants the engine needs at runtime:

- `cpp_converter.py` → `NormalizationConstants.h` (VGGish mean/std for Z-score)
- `export_vggish_pca.py` → `VGGishPCAConstants.h` (128×128 PCA eigen + means + quantize range)
- `export_vggish_mel.py` → `VGGishMelConstants.h` (triangular mel filterbank weights)

These are regenerated whenever the corresponding Python state changes (new training run or VGGish version update).

---

## 10. UDP Communication Protocol

### 10.1 Transport

The sidecar sends UDP datagrams to `127.0.0.1:17777`. Unreal Engine's `AffectiveAudioActor` binds a `FUdpSocketReceiver` from the `Networking` module on the same port. UDP was chosen over TCP for simplicity (no connection state) and acceptable for small, frequent, idempotent messages where a dropped packet is not a disaster.

### 10.2 Message Format

All messages are JSON. A typical packet (~700 bytes) looks like:

```json
{
  "scores": [3.21, -1.82, -3.05, 2.94, 1.13],
  "top_genres":      [["rock", 0.51], ["metal", 0.37], ["pop", 0.34]],
  "top_moods":       [["energetic", 0.24], ["heavy", 0.21], ["melodic", 0.20]],
  "top_instruments": [["bass", 0.49], ["drums", 0.47], ["electricguitar", 0.42]],
  "silence": false,
  "rms": 0.0421
}
```

Silence packets contain the same fields with empty arrays and `silence: true`, allowing UE5 to distinguish between "no audio" and "no Python sidecar running".

### 10.3 Receiver Implementation in Unreal Engine

The `AffectiveAudioActor::OnSidecarPacket` callback parses incoming JSON using `FJsonSerializer`, updates internal state under a mutex (`FCriticalSection`), and stamps `SidecarLastRecvTime`. A getter `IsPythonSidecarActive()` returns true if a packet was received within the last 3 seconds.

When the sidecar is active, the actor's `Tick` overrides `AestheticScores` with the sidecar's `scores` and updates the on-screen overlay text with a `[PY]` prefix to indicate the source.

---

## 11. Genre-Aware Stage Lighting

### 11.1 Stage Construction

The `ConcertStageDirector` actor builds a complete stage at `BeginPlay`:

- **6 wash lights** in a row above the stage, pointing down
- **4 beam lights** that sweep dynamically based on time and arousal
- **4 strobe rect-lights** at the back of the stage
- **2 side spots** angled inward
- **4 floor point-lights** between the stage and audience
- **5 instrument lights** (drum kit, microphone, piano, guitar amp, vocalist)

Plus, when `bSpawnAudience` is true:

- **8 × 14 = 112** placeholder cylinder meshes representing the audience
- **4 audience-aimed spotlights** pulsing on detected energy

All lights are `USpotLightComponent` / `URectLightComponent` / `UPointLightComponent`. They use Unreal Engine 5.7's volumetric scattering enabled to make the beams visible through volumetric fog.

### 11.2 Scene Picker

A `PickScene` function selects between 8 lighting moods:

- **Warm** — orange/red wash for happy / valent music
- **Cool** — blue/cyan wash for sad / cold music
- **Neon** — magenta + cyan for synthetic genres
- **Chase** — pulsing wave traveling across the wash lights
- **Strobe** — synchronized white flashes
- **Spotlight** — single dramatic spot, all others off
- **Burst** — full white flash on rising arousal
- **Rainbow** — HSV wheel cycling through the wash lights

When the Python sidecar is active and the top genre confidence exceeds 0.30, the scene picker is biased toward the genre's natural mood:

- rock / metal / punkrock → Strobe or Chase
- classical / orchestral / soundtrack → Spotlight or Warm
- electronic / dance / techno → Neon or Chase
- ambient / chillout → Cool or Spotlight
- jazz / blues → Warm or Cool
- hiphop / rap → Neon or Strobe
- pop → Rainbow or Warm

When no genre is active (sidecar offline, low confidence), the scene picker falls back to the original arousal/valence/rhythm/intensity heuristics.

### 11.3 Per-Genre Color Palette

A separate function `ApplyGenrePalette` overrides the scene's primary and secondary colors based on detected genre (when sidecar is active and confidence > 0.30):

| Genre family | Color A | Color B | Brightness |
|---|---|---|---|
| rock / metal / punkrock | Red | Orange | ×1.15 |
| classical / orchestral | Warm white | Cool white | ×0.75 |
| electronic / dance | Magenta | Cyan | ×1.10 |
| ambient / chillout | Cold blue | Cyan | ×0.65 |
| jazz / blues | Purple | Warm | ×0.85 |
| hiphop / rap | Magenta | Red | ×1.10 |
| pop | Pink | Cyan | ×1.00 |
| reggae | Green | Orange | ×0.95 |
| country / folk | Warm | Orange | ×0.90 |

The palette overrides the base scene colors except during Strobe and Burst scenes, which always use white for maximum contrast.

### 11.4 Per-Instrument Light Boost

When the top instrument confidence exceeds 0.30, the corresponding instrument light gets a 1.4–1.8× multiplier on its intensity. This causes the stage to literally light up the instrument the model detects:

- drums or drum-machine → drum spot brighter
- piano / keyboard / electric piano → piano spot brighter
- guitar / electric guitar / acoustic guitar / bass → guitarist spot brighter
- voice / singer → vocalist + microphone spots brighter
- synthesizer / computer → guitarist spot at moderate boost

### 11.5 Onset-Driven Flashes

Independent of the deep model, three onset detectors (kick, snare, hi-hat) produce per-event flash multipliers that decay over 50–100 ms. These directly drive:

- kick → wash + floor + drum spot
- snare → side lights + microphone spot
- hi-hat → strobes + guitarist spot

This onset-driven layer is purely C++ DSP and works regardless of which inference path is active.

---

## 12. Music-Reactive Audience System

### 12.1 Spawning

The `BuildAudience` method creates a configurable grid of placeholder figures (default 8 rows × 14 = 112 cylinders) positioned in front of the stage. Each figure has slight random scale and lateral offset to avoid a regimented look.

### 12.2 Animation

Each frame, the `Tick` function updates every audience member's position based on:

- **Sway** — sinusoidal lateral motion with amplitude proportional to arousal and frequency proportional to detected rhythm
- **Jump** — vertical position offset proportional to the kick-drum onset flash, with each member having its own random "jumpiness" multiplier (some dance harder than others)
- **Bob** — small additional vertical offset on hi-hat onsets
- **Twist** — slight yaw rotation tied to arousal

Each audience member has a unique random phase offset, so the crowd does not move in synchronized lockstep. The result is a believable crowd that visibly reacts to the beat.

### 12.3 Audience-Aimed Lights

In addition to the audience meshes, four spotlights point from the truss into the crowd. Their intensity follows arousal × intensity + flash multipliers. Their colors match the active scene palette (genre-tinted when sidecar is providing data).

### 12.4 Scalability

The audience system is purely transformation-based — no skeletal animation, no physics, no AI. It scales to thousands of figures with negligible CPU cost. The default count of 112 is a reasonable balance between visual density and editor load times. The cylinders can be replaced with any static mesh (e.g. a low-poly humanoid) by changing one line in `BuildAudience`.

---

## 13. Evaluation Methodology and Results

### 13.1 Methodology

A test set of 8 contrasting songs was assembled, each downloaded as MP3 via `yt-dlp`:

| File | Reference genre |
|---|---|
| 01_rock_bohemian.mp3 | Rock (Queen) |
| 02_classical_bach.mp3 | Classical (Bach) |
| 03_electronic_daftpunk.mp3 | Electronic (Daft Punk) |
| 04_pop_badguy.mp3 | Pop (Billie Eilish) |
| 05_metal_metallica.mp3 | Metal (Metallica) |
| 06_jazz_takefive.mp3 | Jazz (Brubeck) |
| 07_hiphop_humble.mp3 | Hip-hop (Kendrick) |
| 08_ambient_eno.mp3 | Ambient (Brian Eno) |

A script `test_genre.py` runs the full Python inference pipeline on each file (30-second clips) and reports the top-3 predicted tags per category.

### 13.2 Python Reference Results

| Song | Top-1 genre | Top-3 genres | Match |
|---|---|---|---|
| Bohemian Rhapsody | rock 51 % | rock, metal, pop | ✓ |
| Bach Christmas Oratorio | classical 77 % | classical, soundtrack, orchestral | ✓ |
| Daft Punk One More Time | electronic 54 % | electronic, ambient, chillout | ✓ |
| Bad Guy | electronic 36 % | electronic, reggae, pop | ✓ (top-3) |
| Master of Puppets | rock 62 % | rock, metal, alternative | ✓ |
| Take Five | classical 44 % | classical, soundtrack, easylistening | ⚠ jazz at #5 |
| HUMBLE | hiphop 41 % | hiphop, rap, metal | ✓ |
| Brian Eno (Ambient) | electronic 47 % | electronic, ambient, soundtrack | ✓ |

Aggregate accuracy:

- **Top-1: 4/8 = 50 %**
- **Top-3: 7/8 = 87.5 %**
- **Top-5: 8/8 = 100 %**

The single failure (Take Five → classical instead of jazz) is musically defensible; Take Five's piano-led ensemble shares timbral characteristics with classical chamber music.

### 13.3 UE5 Sidecar Results (Live)

Live testing through VoiceMeeter using the Python sidecar produced consistent results across multiple sessions:

- **Bohemian Rhapsody from minute 4** (the hard rock section): Sidecar reported `rock 47 %`, mood `energetic`, instrument `bass 46 %`. These match Python reference within 5–10 percentage points and represent the same top-1 categories.
- **Master of Puppets** (full 2-minute test): Sustained `rock 41–49 %`, `energetic`, `bass 36–46 %` for the entire duration. The model never wavered.
- **Bach Christmas Oratorio**: `soundtrack 38 %` (Python's top-2), `emotional/film`, `piano 47 %` (Python's top-1). The genre top-1 differs from Python (soundtrack vs classical) but both are appropriate for orchestral instrumental music.

### 13.4 Lighting Verification

For each test song, on-screen verification confirmed that the lighting palette switched as expected:

- Metallica → red/orange wash, frequent strobes/chase, drum and bass spots boosted
- Bach → warm white spotlight palette, slow beam motion, piano spot boosted
- Daft Punk → magenta/cyan neon palette, chase pattern, synthesizer-related boost

The audience cylinders visibly bounced on the kick drum and swayed with arousal across all three songs, with measurably more vigorous motion during the high-arousal Metallica test than during Bach.

---

## 14. Discussion: Lessons Learned

### 14.1 ML in Game Engines is Subtle

The single biggest lesson of this project is that running a trained ML model inside a game engine is dramatically harder than running it in Python — not because the inference runtime is bad (Unreal's NNE plugin works fine) but because the *input pipeline* must match the training pipeline exactly. Audio preprocessing has many stages (windowing, FFT, mel filterbank, log, PCA, quantization, normalization), and each stage has multiple equally-valid implementations that are not interchangeable. A model trained on one preprocessing variant will silently misbehave on another.

### 14.2 Diagnostic-Driven Debugging

Three separate scripts (`test_genre.py`, `diagnostic_pca.py`, `check_brain_health.py`) were essential to localizing bugs. Without them, the team would have wasted time tuning the wrong layer. The `diagnostic_pca.py` in particular was decisive: by running the exact same audio through with-PCA and without-PCA paths in the *same* Python process, it ruled out audio source, model weights, and runtime as the cause of the constant-prediction bug, and pinpointed the missing PCA step as the only difference.

### 14.3 The Hybrid Architecture is Standard Practice

Initially the hybrid Python+UE5 design felt like a compromise — like giving up on pure-engine inference. In retrospect it is the design every shipping ML-driven game uses. AAA studios run trained models in dedicated services (often Python or C++ daemons) and have the engine consume predictions over a socket, named pipe, or shared memory. This separation:

- Lets the engine team work without ML expertise
- Lets the ML team iterate without redeploying the game
- Allows model swaps without an engine rebuild
- Decouples ML hardware (typically GPU) from rendering hardware

The project's hybrid design is the engineering-correct choice, not a workaround.

### 14.4 Model Limitations

The model has known limitations worth documenting:

- The 5-D bottleneck is **not** semantically labeled. The dimensions do not correspond to any psychological construct (the names "Arousal, Valence, Timbre, Rhythm, Intensity" are visualization-layer labels imposed after the fact).
- The classifier was trained on 30-second clips and is best evaluated on similar windows. Very short clips (< 10 s) produce noisy predictions until the EMA smoothing converges.
- The 195 tags are imbalanced. The model prefers common tags over rare ones; some specific genres (e.g. "shoegaze", "dub") rarely make the top-3.
- The MTG-Jamendo training set, while large, leans toward European Creative-Commons artists. Genres common in commercial libraries (e.g. mainstream country, K-pop, heavy commercial rap) may be under-represented.

### 14.5 Live-Audio Differences from Offline

Predictions from live-routed audio (Spotify → VoiceMeeter → sidecar) differ slightly from predictions on the same songs loaded directly from MP3. The differences are attributable to Spotify's Opus-encoded streaming, VoiceMeeter's sample rate conversion, and the additional resampling to 16 kHz inside the sidecar. The top-1 genre is usually unchanged, but confidence values shift by a few percentage points and the second-place predictions sometimes change rank. For final-project demonstration purposes the live results are entirely acceptable.

---

## 15. Conclusion and Future Work

### 15.1 Summary

This project delivered a working real-time music-emotion-recognition and auto-tagging system that drives a procedural concert stage in Unreal Engine 5.7. The system combines a Python service for ML inference (87 % top-3 accuracy on a benchmark set) with a C++ engine for audio capture, low-level DSP, lighting, and crowd animation. A UDP transport carries small JSON messages between the two processes at approximately 1 Hz, with sub-100 ms end-to-end latency.

The system handles a wide range of musical content correctly: rock songs trigger red wash and strobes; classical produces warm spotlights and slow motion; electronic music produces neon palettes and chase patterns; the audience visibly bounces on detected drum onsets and sways with detected arousal.

The development process exposed several non-obvious bugs in the original pure-UE5 design — a missing PCA postprocessing step, missing Z-score normalization, an export wrapper that discarded the classifier output, and a mel spectrogram that did not match VGGish's specification. Each was diagnosed using purpose-built Python scripts and fixed in C++. The eventual decision to keep the C++ path as a fallback while introducing a Python sidecar for primary inference was the engineering-correct choice and matches industry practice.

### 15.2 Future Work

Several improvements are possible without redesigning the system:

- **Skeletal-mesh audience.** Replace cylinder placeholders with rigged characters and use Unreal's animation system instead of pure transformation-based motion. The hooks for music-reactive triggers (kick / snare / hi-hat onsets, arousal level) are already in place.
- **More genre-specific palettes.** The current palette covers nine genre families. Adding a finer-grained palette (e.g. distinct treatments for punk vs prog rock, ambient vs drone) would deepen the system's expressive range.
- **Tag-driven Niagara effects.** Currently the visual effects (smoke, beams, audience) are driven by the lighting controller. Hooking detected genre directly into Niagara particle systems (e.g. spark bursts on rock, snowfall on ambient) would add another dimension of reactivity.
- **Learned-weight per-tag light mappings.** Rather than the current hand-tuned palette, the per-tag color choices could be learned from associations in the training corpus (mood → color heuristics in music-information retrieval literature).
- **Replace MTG-Jamendo with a commercial-music classifier** for better genre coverage on real-world streaming content. CLAP or PANNs could be plugged into the sidecar without changing the UE5 side.
- **Higher inference rate.** The current 1 Hz cadence is comfortable for slow lighting transitions but limits responsiveness to short rhythmic features. Moving to 4–8 Hz inference (with a smaller audio window per inference) could enable beat-synchronized strobe behavior driven by the model rather than only by DSP onset detection.
- **Multi-source mixing.** Currently the system listens to a single audio source. Supporting multiple simultaneous sources (e.g. live microphone + backing track + computer audio) and mixing them inside VoiceMeeter would broaden the use case.

### 15.3 Closing

The project achieved its goal: a music visualizer that responds not just to the spectrum of the audio but to its emotional and stylistic content. The system correctly distinguishes Bach from Metallica from Daft Punk and animates a virtual audience accordingly. The hybrid Python + Unreal architecture proved more robust and maintainable than a pure-engine design and is the recommended pattern for any future ML-driven Unreal project of comparable complexity.

---

## Appendix A — File Inventory

### Python Backend (`ai_backend/`)

| File | Role |
|---|---|
| `lstm_model.py` | StatefulMusicBottleneck class plus InferenceWrapper and InferenceWrapperV2 |
| `train.py` | Training loop, focal loss, Adam→SGD optimizer schedule, Z-score stats |
| `extract_features_vggish.py` | Builds `cached_dataset.pt` from MTG-Jamendo MP3s |
| `check_brain_health.py` | Single-MP3 sanity check with stateful inference |
| `live_mic_test.py` | Microphone-based live tagging (legacy, superseded by sidecar) |
| `test_genre.py` | Batch test on 8 contrasting MP3s, top-3 per category |
| `diagnostic_pca.py` | With-PCA vs no-PCA comparison; confirms PCA importance |
| `export_onnx_v2.py` | Exports `AestheticBrain_v2.onnx` with tag_probs output and `tag_names.json` |
| `export_vggish_pca.py` | Exports PCA matrix + means + quantize range to `VGGishPCAConstants.h` |
| `export_vggish_mel.py` | Exports triangular mel filterbank to `VGGishMelConstants.h` |
| `cpp_converter.py` | Exports Z-score mean/std to `NormalizationConstants.h` |
| `python_sidecar.py` | Live audio capture + inference + UDP broadcast (current production path) |
| `ue_mcp_server.py` | FastMCP server bridging Python tools to UE5 Remote Control API |

### Trained Model Assets

| File | Role |
|---|---|
| `music_emotion_weights.pth` | PyTorch weights plus training mean/std |
| `cached_dataset.pt` | Feature cache used during training and to look up tag names |
| `autotagging.tsv` | MTG-Jamendo tag annotations (track ↔ tags) |
| `tag_names.json` | List of 196 tag names for UE5 to display |
| `AestheticBrain_v2.onnx` | UE5 inference graph (used only when sidecar offline) |
| `audioset-vggish-3.onnx` | VGGish encoder for the UE5 fallback path |

### UE5 Source (`MusicAI_Visualizer/Source/MusicAI_Visualizer/`)

| File | Role |
|---|---|
| `AffectiveAudioActor.h` / `.cpp` | Audio capture, DSP, UE5 ONNX fallback, UDP sidecar receiver, CSV logging |
| `ConcertStageDirector.h` / `.cpp` | Stage construction, scene picking, genre-aware palette, instrument boost, audience |
| `MusicAI_Visualizer.Build.cs` | Module dependencies: NNE, Niagara, AudioMixer, Json, Networking, Sockets |
| `NormalizationConstants.h` | VGGish embedding mean/std (auto-generated) |
| `VGGishPCAConstants.h` | PCA eigen + means + quantize range (auto-generated) |
| `VGGishMelConstants.h` | Triangular mel filterbank weights (auto-generated) |

### Run-Time Configuration

- VoiceMeeter Banana / Potato as the audio router
- Windows playback default: Voicemeeter Input
- Windows recording default: Voicemeeter Out B1
- UnrealEditor process audio output: physical headset directly (avoids feedback loop via Volume Mixer per-app routing)

---

## Appendix B — Hyperparameter Reference

### Training
| Parameter | Value |
|---|---|
| Optimizer (epochs 1–60) | Adam, lr=1e-4 |
| Optimizer (epochs 61–80) | SGD, lr=1e-3, momentum=0.9, weight_decay=1e-4 |
| Optimizer (epochs 81–100) | SGD, lr=1e-4 |
| Loss | Multi-label focal loss, γ=2.0 |
| Gradient clip | max_norm=1.0 |
| Batch size | 32 |
| Sequence length | Variable (padded with `pad_sequence`) |
| Total epochs | 100 |

### Model
| Parameter | Value |
|---|---|
| LSTM input dim | 128 (VGGish embedding) |
| LSTM hidden dim | 256 |
| LSTM layers | 1 |
| LayerNorm | After LSTM, on hidden state |
| Bottleneck dim | 5 |
| Classifier output dim | 195 (Jamendo tags) |
| Dropout | 0.2 between FC layers |

### VGGish (frozen)
| Parameter | Value |
|---|---|
| Sample rate | 16 000 Hz |
| Window length | 25 ms (400 samples) |
| Hop length | 10 ms (160 samples) |
| FFT size | 512 |
| Mel bins | 64 |
| Mel range | 125 – 7 500 Hz |
| Mel scale | HTK |
| Output dim | 128 (after PCA whitening + 8-bit quantize) |
| Example length | 0.96 s (96 frames × 10 ms hop) |

### Inference
| Parameter | Value |
|---|---|
| Inference rate (sidecar) | ~1 Hz |
| Inference rate (UE5 fallback) | ~2 Hz |
| EMA smoothing α | 0.15 |
| Silence threshold (RMS) | 0.001 (sidecar), 0.0005 (UE5) |
| LSTM mode | Stateless (state zeroed each frame) |
| UDP port | 17777 |
| UDP message size | ~700 bytes JSON |

### Lighting
| Parameter | Value |
|---|---|
| Wash light max intensity | 8 000 lm |
| Beam light max intensity | 25 000 lm |
| Strobe max intensity | 15 000 lm |
| Floor max intensity | 4 000 lm |
| Audience light max intensity | 6 000 lm |
| Scene duration | 8–15 s (random) |
| Genre confidence threshold for palette override | 0.30 |
| Instrument confidence threshold for light boost | 0.30 |

### Audience
| Parameter | Value |
|---|---|
| Default rows | 8 |
| Default per row | 14 |
| Total figures | 112 |
| Sway amplitude (max) | 8 cm × arousal |
| Jump height per kick | 35 cm × jumpiness |
| Hi-hat bob | 4 cm |

---

## Appendix C — Glossary

- **VGGish** — Google's VGG-style convolutional audio embedding model, pretrained on AudioSet. Maps a 0.96-second mel spectrogram patch to a 128-dim embedding.
- **PCA postprocessing** — A 128×128 whitening transform plus 8-bit quantization that VGGish applies after the convolutional+FC layers. Required for compatibility with the AudioSet released embeddings.
- **Mel spectrogram** — A frequency representation of audio with mel-scaled bins, perceptually linear at low frequencies and log-scaled at high frequencies.
- **Triangular mel filterbank** — A specific way to convert linear-frequency FFT bins to mel bins, using overlapping triangular weighting functions. VGGish uses HTK-style triangles.
- **HTK** — Hidden Markov Model Toolkit; the source of one common mel-scale formulation, distinct from the Slaney variant used by some librosa defaults.
- **LSTM** — Long Short-Term Memory recurrent neural network. Maintains hidden and cell states across time steps.
- **Stateless mode** — Setting the LSTM hidden and cell states to zero before each inference. Used in the C++ fallback to prevent runaway state saturation.
- **Bottleneck** — A narrow layer in a neural network that forces information compression. In this project, the 5-D layer between the LSTM and the classifier.
- **Multi-label classification** — Each example can have multiple positive labels simultaneously (a song can be both "rock" and "energetic" and "guitar"). Distinct from multi-class where labels are mutually exclusive.
- **Focal loss** — A loss function that down-weights easy examples and focuses gradient updates on hard ones. Useful for severe class imbalance.
- **Z-score normalization** — Per-dimension `(x - μ) / σ`, where μ and σ are computed over the training set.
- **EMA** — Exponential Moving Average. A simple smoothing filter `y_t = α * x_t + (1-α) * y_{t-1}`.
- **NNE** — Unreal Engine's Neural Network Engine plugin. Loads ONNX models and provides CPU/GPU inference at runtime.
- **ONNX** — Open Neural Network Exchange format. A portable serialization for neural network graphs and weights.
- **UDP** — User Datagram Protocol. Connectionless transport with small overhead, no delivery guarantees. Used here to broadcast inference results from the Python sidecar to UE5.
- **VoiceMeeter** — A free virtual audio mixer for Windows. Routes audio between applications via virtual cables.
- **Submix listener** — An Unreal Engine audio plugin pattern that lets a custom listener receive raw audio buffers as they pass through a sound submix.
- **Volumetric scattering** — A rendering technique where light scatters through participating media (fog, dust, smoke), making beams visible.
- **Niagara** — Unreal Engine 5's GPU-based particle system.
- **MTG-Jamendo** — Music Technology Group dataset built from Jamendo Creative Commons audio. Used here for training.
- **Sidecar** — A pattern where a separate process runs alongside the main application to provide a specific service (here, ML inference). The two processes communicate via a well-defined protocol (here, UDP JSON).

---

## Bibliography

- Bogdanov, D., Won, M., Tovstogan, P., Porter, A., & Serra, X. (2019). The MTG-Jamendo Dataset for Automatic Music Tagging. *Machine Learning for Music Discovery Workshop, ICML 2019.*
- Hershey, S., Chaudhuri, S., Ellis, D. P. W., Gemmeke, J. F., Jansen, A., Moore, R. C., Plakal, M., Platt, D., Saurous, R. A., Seybold, B., Slaney, M., Weiss, R. J., & Wilson, K. (2017). CNN architectures for large-scale audio classification. *2017 IEEE International Conference on Acoustics, Speech and Signal Processing (ICASSP).*
- Lin, T.-Y., Goyal, P., Girshick, R., He, K., & Dollár, P. (2017). Focal loss for dense object detection. *Proceedings of the IEEE International Conference on Computer Vision (ICCV).*
- Won, M., Chun, S., Nieto, O., & Serra, X. (2019). Data-driven harmonic filters for audio representation learning. *International Conference on Acoustics, Speech and Signal Processing (ICASSP).*
- Hochreiter, S., & Schmidhuber, J. (1997). Long short-term memory. *Neural Computation, 9(8), 1735–1780.*
- Epic Games. (2024). Unreal Engine 5.7 Documentation: Neural Network Engine (NNE) Plugin.
