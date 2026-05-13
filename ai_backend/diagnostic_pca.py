"""
Diagnostic: prove whether PCA postprocessing is what UE5 is missing.

Runs each test MP3 through TWO pipelines:
  1. WITH PCA  - matches Python (proven 87% top-3 accuracy)
  2. NO PCA    - matches UE5's current C++ pipeline

If WITHOUT PCA gives identical bias toward "electronic" for all songs,
PCA is confirmed as the missing piece. Then we fix C++.

Run from inside ai_backend:
    python diagnostic_pca.py
"""
import os
import sys
from collections import OrderedDict, defaultdict
import numpy as np
import torch
import librosa
import warnings

warnings.filterwarnings("ignore")

BACKEND_DIR = os.path.dirname(os.path.abspath(__file__))
if BACKEND_DIR not in sys.path:
    sys.path.insert(0, BACKEND_DIR)

from lstm_model import StatefulMusicBottleneck
from torchvggish.vggish import VGGish

TEST_FOLDER = r"C:\Users\mertf\TestMusic"
WEIGHTS_FILE = os.path.join(BACKEND_DIR, "music_emotion_weights.pth")
DATA_FILE = os.path.join(BACKEND_DIR, "cached_dataset.pt")
DURATION_SEC = 30

URLS = {
    'vggish': 'https://github.com/harritaylor/torchvggish/releases/download/v0.1/vggish-10086976.pth',
    'pca':    'https://github.com/harritaylor/torchvggish/releases/download/v0.1/vggish_pca_params-970ea276.pth'
}

device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
print(f"Device: {device}")

print("Loading tag names...")
data = torch.load(DATA_FILE, weights_only=False)
all_tags = data["tags"]

print("Loading classifier...")
model = StatefulMusicBottleneck(output_dim=len(all_tags), hidden_dim=256).to(device)
ck = torch.load(WEIGHTS_FILE, map_location=device)
sd = OrderedDict()
for k, v in ck['state_dict'].items():
    sd[k.replace("_orig_mod.", "")] = v
model.load_state_dict(sd)
model.eval()
train_mean = ck['mean'].to(device)
train_std = ck['std'].to(device)

print("Building TWO VGGish instances (with and without PCA)...")
vggish_with_pca = VGGish(urls=URLS, postprocess=True).to(device)
vggish_with_pca.eval()
vggish_no_pca = VGGish(urls=URLS, postprocess=False).to(device)
vggish_no_pca.eval()

def predict_top3_per_category(vggish_instance, audio):
    with torch.no_grad():
        emb = vggish_instance.forward(audio, fs=16000)
        if emb.dim() == 1:
            emb = emb.unsqueeze(0)
        emb = emb.unsqueeze(0).to(device).float()
        emb_norm = (emb - train_mean) / train_std
        h = torch.zeros(1, 1, 256, device=device)
        c = torch.zeros(1, 1, 256, device=device)
        logits, _, _, _ = model(emb_norm, h, c)
        avg_probs = torch.sigmoid(logits[0]).mean(dim=0).cpu().numpy()

    by_cat = defaultdict(list)
    for i, tag in enumerate(all_tags):
        if "---" in tag:
            cat, name = tag.split("---", 1)
            by_cat[cat].append((avg_probs[i], name))
    return by_cat

def format_top3(by_cat, cat):
    items = sorted(by_cat.get(cat, []), reverse=True)[:3]
    return "  ".join(f"{p*100:4.1f}% {n}" for p, n in items)

mp3_files = sorted(f for f in os.listdir(TEST_FOLDER) if f.lower().endswith(".mp3"))
print(f"\nTesting {len(mp3_files)} files\n")

results_with = []
results_no   = []

for fname in mp3_files:
    fpath = os.path.join(TEST_FOLDER, fname)
    print("=" * 90)
    print(fname)
    print("-" * 90)

    audio, _ = librosa.load(fpath, sr=16000, mono=True, duration=DURATION_SEC)

    bc_with = predict_top3_per_category(vggish_with_pca, audio)
    bc_no   = predict_top3_per_category(vggish_no_pca,   audio)

    print("WITH PCA  (correct Python pipeline):")
    print(f"  genre      : {format_top3(bc_with, 'genre')}")
    print(f"  mood/theme : {format_top3(bc_with, 'mood/theme')}")
    print(f"  instrument : {format_top3(bc_with, 'instrument')}")

    print("NO PCA    (matches UE5 current behavior):")
    print(f"  genre      : {format_top3(bc_no, 'genre')}")
    print(f"  mood/theme : {format_top3(bc_no, 'mood/theme')}")
    print(f"  instrument : {format_top3(bc_no, 'instrument')}")

    results_with.append((fname, sorted(bc_with['genre'], reverse=True)[0]))
    results_no.append(  (fname, sorted(bc_no['genre'],   reverse=True)[0]))

print("\n" + "=" * 90)
print("SUMMARY: top genre per song")
print("=" * 90)
print(f"{'File':<40} | {'WITH PCA':<25} | {'NO PCA (UE5-like)':<25}")
print("-" * 90)
for (f1, (p1, n1)), (_, (p2, n2)) in zip(results_with, results_no):
    print(f"{f1:<40} | {n1:<15} {p1*100:5.1f}% | {n2:<15} {p2*100:5.1f}%")

print("\nIF the NO PCA column shows the same genre for every song -> PCA is the missing fix.")
print("IF NO PCA gives different but wrong predictions -> mel spectrogram is also wrong.")
