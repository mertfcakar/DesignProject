import torch
import os
import pandas as pd
import numpy as np
import torchaudio

# ==========================================
# WP3: FIXED Feature Extraction (VGGish)
# ==========================================

print("🖥️ Initializing PyTorch and VGGish...")
device = torch.device("cuda" if torch.cuda.is_available() else "cpu")

# Load VGGish
vggish = torch.hub.load('harritaylor/torchvggish', 'vggish')
vggish.eval().to(device)

TSV_PATH = "autotagging.tsv"
AUDIO_DIR = "D:/MTG_Jamendo_Full"
OUTPUT_FILE = "cached_dataset.pt"

df = pd.read_csv(TSV_PATH, sep='\t')
all_tags = sorted(list(set([tag for tags in df['tags'].dropna().str.split(',') for tag in tags])))
tag_to_idx = {tag: i for i, tag in enumerate(all_tags)}

features_list, labels_list = [], []

print(f"🚀 Starting Extraction on {device}...")
for index, row in df.iterrows():
    tid = str(row['track_id']).replace('track_', '').lstrip('0')
    file_path = os.path.join(AUDIO_DIR, f"{tid}.mp3")
    
    if os.path.exists(file_path):
        try:
            # 1. Load the full audio first to check sample rate
            waveform, sample_rate = torchaudio.load(file_path)
            
            # 2. BUG FIX: Resample to 16kHz if necessary
            if sample_rate != 16000:
                resampler = torchaudio.transforms.Resample(orig_freq=sample_rate, new_freq=16000)
                waveform = resampler(waveform)
                sample_rate = 16000
            
            # 3. Slice the first 30 seconds AFTER resampling
            waveform = waveform[:, :30 * 16000]
            
            # 4. Convert to mono
            if waveform.shape[0] > 1:
                waveform = torch.mean(waveform, dim=0, keepdim=True)

            # 5. BUG FIX: Extract features properly. 
            # The harritaylor repo handles the log-mel spectrogram conversion inside 'forward' 
            # IF you pass it the correct 16kHz numpy array!
            with torch.no_grad():
                audio_np = waveform.squeeze().numpy()
                # Skip files that are too short to generate a spectrogram patch
                if len(audio_np) < 16000: continue 
                embeddings = vggish.forward(audio_np, fs=sample_rate).cpu()
            
            # Create Multi-Hot Label Vector
            label_vector = np.zeros(len(all_tags), dtype=np.float32)
            for tag in str(row['tags']).split(','):
                if tag in tag_to_idx:
                    label_vector[tag_to_idx[tag]] = 1.0
            
            features_list.append(embeddings)
            labels_list.append(torch.tensor(label_vector))
            
            if len(features_list) % 100 == 0:
                print(f"✅ Processed {len(features_list)} tracks...")
                
        except Exception as e:
            continue

print(f"💾 Saving to {OUTPUT_FILE}...")
torch.save({"features": features_list, "labels": torch.stack(labels_list), "tags": all_tags}, OUTPUT_FILE)
print("🎉 Extraction Complete!")