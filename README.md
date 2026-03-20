# 🧠 AI Backend: Music-to-Visual Aesthetic Engine (V2)
**Project:** AI-Powered Real-Time Music Visualization (Capstone)
**Architecture Status:** V2 (Production / Latency-Optimized)

## 🏗️ The Tech Stack (V2 Upgrades)
* **Feature Extractor:** Google's `VGGish` (CNN-based, log-mel spectrograms for ultra-low latency). *(Upgraded from HuBERT)*.
* **Classifier:** Custom Stateful 2-Layer LSTM with Latent Space Compression.
* **Dataset:** MTG-Jamendo Full Dataset (500GB+, 55,000+ tracks).
* **Output Topology:** 195 Multi-Label Tags (Genres, Instruments, Moods) compressed into a **5-Dimensional Aesthetic Vector** (Arousal, Valence, Timbre, Rhythm, Intensity) for real-time Unreal Engine rendering.

---

## 📂 File Directory & Purpose

| File | Purpose |
| :--- | :--- |
| `autotagging.tsv` | **The Master Map:** Contains the mapping of all 55,000+ Track IDs to 195 multi-label tags. |
| `requirements.txt` | **The Dependency List:** A hardware-agnostic list of software required to run the Python environment. |
| `master_downloader.py` | **The Ingestion Engine:** Multi-threaded downloader that pulls the 500GB audio dataset directly from the Jamendo CDN. |
| `extract_features_vggish.py` | **The Ear (CNN):** Slices 30s audio chunks, resamples to 16kHz, and uses VGGish to extract 128-D mathematical tensors (`cached_dataset.pt`). |
| `lstm_model.py` | **The Brain Blueprint:** Defines the Stateful LSTM and the 5-D Autoencoder Bottleneck. |
| `train.py` | **The Teacher:** Trains the LSTM on the 128-D features using `BCEWithLogitsLoss` for multi-label classification. |
| `emotion_model_v2.pth` | **The Soul:** The final trained neural network weights. |

---

## 🚀 Deployment & Execution Guide

Because this pipeline processes 500GB of audio and requires heavy GPU acceleration, the workflow is split between standard preparation and high-performance execution.

### 💻 Phase 1: Preparation (MacBook / Standard PC)
These steps are for setting up the repository and downloading the metadata. Do not run heavy extraction here.

1. **Clone & Setup:**
   ```bash
   git clone [your-repo-link]
   cd DesignProject/ai_backend
   ```
2. **Push to cloud:** Ensure `requirements.txt` only contains software packages (no PyTorch binaries) and push your setup.

### 🖥️ Phase 2: The GPU Pipeline (Windows RTX 4080)
These steps must be executed on the primary workstation.

**1. Hardware-Aware Environment Setup**
You must manually install the NVIDIA CUDA version of PyTorch before installing the rest of the requirements.
```cmd
python -m venv .venv
.venv\Scripts\activate
pip install torch torchvision torchaudio --index-url https://download.pytorch.org/whl/cu121
pip install -r requirements.txt
```

**2. The 500GB Ingestion**
Run the multi-threaded downloader. Ensure your `D:` drive path is correctly configured in the script.
```cmd
python master_downloader.py
```

**3. Feature Extraction (The "Crunch")**
Convert the 500GB of MP3s into a highly compressed mathematical cache. This process is highly parallelized but will take several hours.
```cmd
python extract_features_vggish.py
```
*Result: Generates `cached_dataset.pt` (approx. 250GB).*

**4. Training (The "Learning")**
Train the Stateful LSTM to route all 195 acoustic tags through the 5-D aesthetic bottleneck.
```cmd
python train.py
```
*Result: Generates `emotion_model_v2.pth`.*

---

## 🧪 How to Verify "Success"
1. **Extraction Integrity:** If `cached_dataset.pt` exists and is >100GB, the VGGish CNN successfully parsed the audio.
2. **Training Convergence:** Watch the `Loss` metric during training. Because we use `BCEWithLogitsLoss` for 195 classes, the loss should steadily decrease over 50 epochs. 
3. **Stateful Shape:** The final model output must successfully compress the data into a shape of `[Batch_Size, 5]`, representing the 5 core visual parameters for Unreal Engine.

***