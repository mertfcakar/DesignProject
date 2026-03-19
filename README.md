# 🧠 AI Backend: Music-to-Emotion Engine
**Project:** AI-Powered Music Visualization (Capstone)
**Target:** Emotion classification using Meta's HuBERT + LSTM.

## 🛠️ The Tech Stack
* **Feature Extractor:** Meta’s `HuBERT-Base-LS960` (Hidden-Unit BERT).
* **Classifier:** Custom 2-Layer LSTM (Long Short-Term Memory).
* **Dataset:** MTG-Jamendo (Academic Music Dataset).
* **Target Emotions:** Happy, Sad, Energetic, Relaxing, Dark.

---

## 📂 File Directory & Purpose

| File | Purpose |
| :--- | :--- |
| `autotagging_moodtheme.tsv` | **The Map:** Contains the mapping of Track IDs to emotional labels. |
| `jamendo_config.json` | **The Legend:** Maps emotion names (e.g., "Happy") to numerical IDs for the AI. |
| `extract_features.py` | **The Ear:** Uses HuBERT to turn MP3s into mathematical tensors (`.pt`). |
| `lstm_model.py` | **The Brain Blueprint:** Defines the neural network layers. |
| `train.py` | **The Teacher:** Trains the LSTM on the extracted features. |
| `emotion_model.pth` | **The Soul:** The final trained weights (The actual AI file). |

---

## 🚀 Step-by-Step Testing Guide

If you are helping to test or run the large-scale version, follow these steps in order:

### 1. Environment Setup
Make sure you are in a virtual environment and have all dependencies.
```bash
python -m venv .venv
source .venv/bin/activate  # Or venv\Scripts\activate on Windows
pip install -r requirements.txt
```

### 2. Data Ingestion (The "Homework")
Run the downloader to fill the `audio_dataset/` folder.
* **Execution:** `python ai_backend/wp3_direct_cdn.py`
* **Check:** You should see folders like `/happy` and `/sad` filling with MP3s.

### 3. Feature Extraction (The "Digestion")
This is the most heavy-duty part. We use HuBERT to listen to the songs so we don't have to reload them during training.
* **Execution:** `python ai_backend/extract_features.py`
* **Check:** It will generate a file called `cached_dataset.pt`. This file should be roughly 10MB - 1GB depending on the dataset size.

### 4. Training (The "Learning")
This is where the AI actually learns. 
* **Execution:** `python ai_backend/train.py`
* **What to watch for:** * **Loss:** Should go down (target < 0.2).
    * **Val Accuracy:** Should go up. We are aiming for **80%+** on the high-resolution run.
* **Result:** It will save `emotion_model.pth`.

---

## 🧪 How to Verify "Success"
1.  **Check the Tensor:** If `cached_dataset.pt` exists, the HuBERT pipeline is working.
2.  **Check the Training:** If the `Val Accuracy` is significantly higher than 20% (random guess), the LSTM is successfully learning emotional patterns.
3.  **Check the Model:** If `emotion_model.pth` is created, the "Soul" is ready for integration.
