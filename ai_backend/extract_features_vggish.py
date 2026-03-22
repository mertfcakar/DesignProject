import torch
import os
import sys
import numpy as np
import librosa
import csv

# ==========================================
# 🛠️ PATH & PACKAGE CONFIGURATION
# ==========================================
BACKEND_DIR = os.path.dirname(os.path.abspath(__file__))
if BACKEND_DIR not in sys.path:
    sys.path.insert(0, BACKEND_DIR)

try:
    from torchvggish.vggish import VGGish
    print("✅ VGGish Package linked correctly.")
except ImportError as e:
    print(f"❌ Error: Could not find torchvggish package in {BACKEND_DIR}\n{e}")
    exit()

# ==========================================
# 🚀 INITIALIZING MODEL & GPU
# ==========================================
print("🖥️ Initializing PyTorch and VGGish...")
device = torch.device("cuda" if torch.cuda.is_available() else "cpu")

VGGISH_URLS = {
    'vggish': 'https://github.com/harritaylor/torchvggish/releases/download/v0.1/vggish-10086976.pth',
    'pca': 'https://github.com/harritaylor/torchvggish/releases/download/v0.1/vggish_pca_params-970ea276.pth'
}

vggish = VGGish(urls=VGGISH_URLS)
vggish.eval().to(device)

# --- Paths ---
TSV_PATH = os.path.join(BACKEND_DIR, "autotagging.tsv")
OUTPUT_FILE = os.path.join(BACKEND_DIR, "cached_dataset.pt")
AUDIO_DIR = "D:/MTG_Jamendo_Full"

# ==========================================
# 📖 DATASET PARSING
# ==========================================
print(f"📖 Parsing Metadata from: {TSV_PATH}")
tracks_metadata = []
all_tags_set = set()

with open(TSV_PATH, 'r', encoding='utf-8') as f:
    reader = csv.reader(f, delimiter='\t')
    header = next(reader) 
    
    for row in reader:
        if len(row) < 6: continue 
        
        # FIXED: Index [3] is the 'path' column (e.g., '77/48077.mp3')
        # Index [1] was the Artist ID, which caused the previous crash.
        relative_path = row[3].strip().strip('"').strip("'")
        
        # Tags start from index [5]
        tags = [t.strip().strip('"').strip("'") for t in row[5:] if t.strip()]
        for tag in tags: all_tags_set.add(tag)
        
        tracks_metadata.append({'path': relative_path, 'tags': tags})

all_tags = sorted(list(all_tags_set))
tag_to_idx = {tag: i for i, tag in enumerate(all_tags)}
print(f"✅ Found {len(tracks_metadata)} tracks and {len(all_tags)} unique tags.")

# ==========================================
# 🚀 EXTRACTION LOOP
# ==========================================
features_list = []
labels_list = []
found_any = False

print(f"🚀 Starting Extraction on {len(tracks_metadata)} tracks...")

for i, track_data in enumerate(tracks_metadata):
    # This joins 'D:/MTG_Jamendo_Full' with '77/48077.mp3' correctly
    file_path = os.path.normpath(os.path.join(AUDIO_DIR, track_data['path']))
    
    # Debug the first few paths to ensure they match your D: drive
    if i < 3:
        print(f"🔍 Checking path: {file_path}")

    if os.path.exists(file_path):
        found_any = True
        try:
            audio_np, _ = librosa.load(file_path, sr=16000, mono=True, duration=30.0)
            if len(audio_np) < 16000: continue 
                
            with torch.no_grad():
                embeddings = vggish.forward(audio_np, fs=16000).cpu()
            
            label_vector = np.zeros(len(all_tags), dtype=np.float32)
            for tag in track_data['tags']:
                if tag in tag_to_idx:
                    label_vector[tag_to_idx[tag]] = 1.0
            
            features_list.append(embeddings)
            labels_list.append(torch.tensor(label_vector))
            
            if len(features_list) % 50 == 0:
                print(f"✅ Processed {len(features_list)} tracks...")
                
        except Exception:
            continue

if not found_any:
    print("\n❌ FATAL ERROR: Zero files found. Re-check your D: drive folders.")
    print(f"Expected folder structure: {os.path.join(AUDIO_DIR, '00')}")
    exit()

print(f"\n💾 Saving to {OUTPUT_FILE}...")
torch.save({"features": features_list, "labels": torch.stack(labels_list), "tags": all_tags}, OUTPUT_FILE)
print("🎉 Extraction Complete! You can now run train.py.")