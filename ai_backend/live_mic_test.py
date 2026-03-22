import torch
import os
import sys
import numpy as np
import sounddevice as sd
import queue
import warnings
import librosa
import time
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
device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')

# ==========================================
# 🧠 LOAD BRAIN & STATS
# ==========================================
print("🖥️ Loading Professional AI Brain...")
dataset = torch.load(DATA_FILE, weights_only=False)
all_tags = dataset["tags"]

vggish = VGGish(urls={
    'vggish': 'https://github.com/harritaylor/torchvggish/releases/download/v0.1/vggish-10086976.pth',
    'pca': 'https://github.com/harritaylor/torchvggish/releases/download/v0.1/vggish_pca_params-970ea276.pth'
}).to(device)
vggish.eval()

model = StatefulMusicBottleneck(output_dim=len(all_tags), hidden_dim=256).to(device)

if os.path.exists(WEIGHTS_FILE):
    checkpoint = torch.load(WEIGHTS_FILE)
    state_dict = checkpoint['state_dict']
    new_state_dict = OrderedDict()
    for k, v in state_dict.items():
        name = k.replace("_orig_mod.", "") 
        new_state_dict[name] = v
    model.load_state_dict(new_state_dict)
    
    train_mean = checkpoint['mean'].to(device)
    train_std = checkpoint['std'].to(device)
    
    model.eval()
    print(f"✅ Brain Synchronized. Ready for Live Feed.")
else:
    print("❌ Error: Weights not found!")
    exit()

# ==========================================
# 🎤 MIC SETTINGS
# ==========================================
DEVICE_ID = 1  
THRESHOLD = 0.005 

info = sd.query_devices(DEVICE_ID, 'input')
NATIVE_SR = int(info['default_samplerate'])
audio_queue = queue.Queue()

def audio_callback(indata, frames, time, status):
    audio_queue.put(indata.flatten().copy())

print("\n" + "="*100)
print(f"🎙️ LIVE AFFECTIVE TIMELINE ACTIVATED")
print(f"Device: {info['name']} | Analyzing Top 5 Genre/Mood Tags")
print("="*100 + "\n")

h = torch.zeros(1, 1, 256).to(device)
c = torch.zeros(1, 1, 256).to(device)
second_counter = 0

try:
    with sd.InputStream(device=DEVICE_ID, channels=1, samplerate=NATIVE_SR, callback=audio_callback):
        while True:
            chunks = []
            samples_collected = 0
            while samples_collected < NATIVE_SR:
                chunk = audio_queue.get()
                chunks.append(chunk)
                samples_collected += len(chunk)
            
            full_chunk = np.concatenate(chunks)[:NATIVE_SR]
            second_counter += 1
            
            vol = np.max(np.abs(full_chunk))
            
            if vol < THRESHOLD:
                # Still use \r for silence so we don't spam the timeline with empty lines
                sys.stdout.write(f"\r⏱️ {second_counter:03d}s | 🔇 Silence Detected...                   ")
                sys.stdout.flush()
                h *= 0.95
                c *= 0.95
                continue

            resampled = librosa.resample(full_chunk, orig_sr=NATIVE_SR, target_sr=16000)
            
            with torch.no_grad():
                feat = vggish.forward(resampled, fs=16000).to(device).view(1, 1, 128)
                feat_norm = (feat - train_mean) / train_std
                
                tag_preds, aes_vector, h, c = model(feat_norm, h, c)
                
                v = aes_vector[0, -1, :].cpu().numpy()
                probs = torch.sigmoid(tag_preds[0, -1, :]).cpu().numpy()
                
                # --- GET TOP 5 TAGS ---
                top_indices = np.argsort(probs)[::-1][:5]
                top_5_strings = []
                for idx in top_indices:
                    tag_name = all_tags[idx].split('---')[-1].upper()
                    conf = probs[idx] * 100
                    top_5_strings.append(f"{tag_name} ({conf:.0f}%)")
                
                tags_display = ", ".join(top_5_strings)

                # --- PRINT NEW LINE FOR TIMELINE ---
                # We add a clear-line space at the beginning to overwrite any "Silence Detected" text
                print(f"⏱️ {second_counter:03d}s | 🌈 [{v[0]:+5.1f}, {v[1]:+5.1f}, {v[2]:+5.1f}, {v[3]:+5.1f}, {v[4]:+5.1f}] | 🏷️ {tags_display}")
                sys.stdout.flush()

except KeyboardInterrupt:
    print("\n\n🛑 Live Stream Stopped.")
except Exception as e:
    print(f"\n❌ Error: {e}")