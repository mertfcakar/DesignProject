import torch
import librosa
import os
import json
from transformers import Wav2Vec2FeatureExtractor, HubertModel

# ==========================================
# WP3: MTG-Jamendo Feature Caching
# ==========================================
print("⚙️ Initializing HuBERT Feature Extractor (MPS Accelerated)...")

# 1. Setup Hardware & Models
device = torch.device("mps" if torch.backends.mps.is_available() else "cpu")
processor = Wav2Vec2FeatureExtractor.from_pretrained("facebook/hubert-base-ls960")
model = HubertModel.from_pretrained("facebook/hubert-base-ls960").to(device)
model.eval()

# 2. Setup Paths
base_dir = os.path.dirname(os.path.abspath(__file__))
dataset_dir = os.path.join(base_dir, "audio_dataset")
output_file = os.path.join(base_dir, "cached_dataset.pt")

# Load your emotion mapping
config_path = os.path.join(base_dir, "jamendo_config.json")
with open(config_path, "r") as f:
    tag_to_id = json.load(f)["tag_to_id"]

features_list = []
labels_list = []

# 3. Extraction Loop
print(f"📂 Scanning for audio in: {dataset_dir}")

for emotion in os.listdir(dataset_dir):
    emotion_path = os.path.join(dataset_dir, emotion)
    if not os.path.isdir(emotion_path) or emotion not in tag_to_id:
        continue
    
    label_id = tag_to_id[emotion]
    print(f"\n🎧 Processing Emotion: [{emotion.upper()}] (ID: {label_id})")
    
    for audio_file in os.listdir(emotion_path):
        if not audio_file.endswith(".mp3"):
            continue
            
        file_path = os.path.join(emotion_path, audio_file)
        
        try:
            # Load 10 seconds of audio at HuBERT's native 16kHz
            audio, _ = librosa.load(file_path, sr=16000, duration=10.0)
            
            # Process with HuBERT on M4 GPU
            inputs = processor(audio, sampling_rate=16000, return_tensors="pt").to(device)
            with torch.no_grad():
                outputs = model(**inputs)
            
            # Save the embeddings (mean across time steps)
            # This turns the song into a unique "fingerprint"
            embeddings = outputs.last_hidden_state.mean(dim=1).cpu().squeeze(0)
            
            features_list.append(embeddings)
            labels_list.append(label_id)
            print(f"   ✅ Processed: {audio_file}")
            
        except Exception as e:
            print(f"   ❌ Skip {audio_file}: {e}")

# 4. Save the Final Tensor
if features_list:
    final_data = {
        "features": torch.stack(features_list),
        "labels": torch.tensor(labels_list)
    }
    torch.save(final_data, output_file)
    print(f"\n🎉 SUCCESS! Cached {len(features_list)} songs to {output_file}")
else:
    print("\n❌ No features extracted. Check your audio folders.")