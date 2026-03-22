import torch
import os
import sys
import numpy as np
import librosa
import time
import warnings
from collections import OrderedDict

warnings.filterwarnings("ignore")

# ==========================================
# 🛠️ SETUP & PATHS
# ==========================================
BACKEND_DIR = os.path.dirname(os.path.abspath(__file__))
if BACKEND_DIR not in sys.path:
    sys.path.insert(0, BACKEND_DIR)

from lstm_model import StatefulMusicBottleneck
from torchvggish.vggish import VGGish

WEIGHTS_FILE = os.path.join(BACKEND_DIR, "music_emotion_weights.pth")
DATA_FILE = os.path.join(BACKEND_DIR, "cached_dataset.pt")
# 🚨 Ensure this points to a song you want to test!
TEST_MP3 = "D:/MTG_Jamendo_Full/77/48077.mp3" 

device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')

# ==========================================
# 🧠 LOAD BRAIN & NORMALIZATION STATS
# ==========================================
print("🧠 BRAIN HEALTH CHECK | Professional Integration Mode")

# 1. Load the labels for tag display
dataset = torch.load(DATA_FILE, weights_only=False)
all_tags = dataset["tags"]

# 2. Initialize VGGish (The "Ears")
VGGISH_URLS = {
    'vggish': 'https://github.com/harritaylor/torchvggish/releases/download/v0.1/vggish-10086976.pth',
    'pca': 'https://github.com/harritaylor/torchvggish/releases/download/v0.1/vggish_pca_params-970ea276.pth'
}
vggish = VGGish(urls=VGGISH_URLS).to(device)
vggish.eval()

# 3. Initialize LSTM (The "Brain")
model = StatefulMusicBottleneck(output_dim=len(all_tags), hidden_dim=256).to(device)

if os.path.exists(WEIGHTS_FILE):
    # Load the combined checkpoint (StateDict + Mean + Std)
    checkpoint = torch.load(WEIGHTS_FILE)
    
    # Clean keys if torch.compile was used
    state_dict = checkpoint['state_dict']
    new_state_dict = OrderedDict()
    for k, v in state_dict.items():
        name = k.replace("_orig_mod.", "") 
        new_state_dict[name] = v
    
    model.load_state_dict(new_state_dict)
    
    # Extract the normalization stats used during training
    train_mean = checkpoint['mean'].to(device)
    train_std = checkpoint['std'].to(device)
    
    model.eval()
    print(f"✅ Loaded weights and Z-Score stats from {os.path.basename(WEIGHTS_FILE)}")
else:
    print(f"❌ Error: {WEIGHTS_FILE} not found. You must finish training first!")
    exit()

# ==========================================
# 🎵 REAL-TIME SIMULATION LOOP
# ==========================================
print(f"\n🎧 Analyzing: {os.path.basename(TEST_MP3)}")
print("-" * 75)

# Load 20 seconds of audio
y, _ = librosa.load(TEST_MP3, sr=16000, duration=20)

# 🚨 INITIALIZE MEMORY OUTSIDE THE LOOP (No Amnesia)
h = torch.zeros(1, 1, 256).to(device)
c = torch.zeros(1, 1, 256).to(device)

for i in range(20):
    start_sample = i * 16000
    end_sample = (i + 1) * 16000
    chunk = y[start_sample : end_sample]
    
    if len(chunk) < 16000: break
    
    with torch.no_grad():
        # 1. Get raw VGGish features
        feat = vggish.forward(chunk, fs=16000).to(device).view(1, 1, 128)
        
        # 2. Apply Z-Score Normalization using Training Stats
        feat_norm = (feat - train_mean) / train_std
        
        # 3. Forward Pass through LSTM with Persistent Memory
        tag_preds, aes_vector, h, c = model(feat_norm, h, c)
        
        # 4. Extract results
        # We take the vector from the last frame of the current sequence
        v = aes_vector[0, -1, :].cpu().numpy()
        
        # Get the top Genre prediction
        probs = torch.sigmoid(tag_preds[0, -1, :]).cpu().numpy()
        top_idx = np.argsort(probs)[-1]
        top_tag = all_tags[top_idx].split('---')[-1].upper()

        # 5. Live Print
        sys.stdout.write(f"⏱️ {i+1:02d}s | 🌈 Vector: [{v[0]:+5.2f}, {v[1]:+5.2f}, {v[2]:+5.2f}, {v[3]:+5.2f}, {v[4]:+5.2f}] | 🏷️ {top_tag}\n")
        sys.stdout.flush()
        
        # Artificial delay to feel the "real-time" flow
        time.sleep(0.2)

print("-" * 75)
print("💡 SUCCESS CRITERIA: If the 5 numbers change independently, the model is healthy.")