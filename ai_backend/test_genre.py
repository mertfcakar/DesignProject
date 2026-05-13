"""
Diagnostic test: does the trained genre classifier actually work?

Loads the existing music_emotion_weights.pth + cached_dataset.pt,
runs each MP3 through VGGish + LSTM, prints top tags per song.

Run from inside the ai_backend folder:
    python test_genre.py
"""
import os
import sys
import warnings
from collections import OrderedDict, defaultdict
import numpy as np
import torch
import librosa

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
TOP_K = 5

device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
print(f"Device: {device}")

print("Loading tag names from cached_dataset.pt ...")
data = torch.load(DATA_FILE, weights_only=False)
all_tags = data["tags"]
print(f"  Loaded {len(all_tags)} tags")

print("Loading VGGish ...")
vggish = VGGish(urls={
    'vggish': 'https://github.com/harritaylor/torchvggish/releases/download/v0.1/vggish-10086976.pth',
    'pca':    'https://github.com/harritaylor/torchvggish/releases/download/v0.1/vggish_pca_params-970ea276.pth'
}).to(device)
vggish.eval()

print("Loading trained classifier ...")
model = StatefulMusicBottleneck(output_dim=len(all_tags), hidden_dim=256).to(device)
ck = torch.load(WEIGHTS_FILE, map_location=device)
sd = OrderedDict()
for k, v in ck['state_dict'].items():
    sd[k.replace("_orig_mod.", "")] = v
model.load_state_dict(sd)
model.eval()
train_mean = ck['mean'].to(device)
train_std = ck['std'].to(device)

mp3_files = sorted(f for f in os.listdir(TEST_FOLDER) if f.lower().endswith(".mp3"))
if not mp3_files:
    print(f"No MP3 files found in {TEST_FOLDER}")
    sys.exit(1)

print(f"\nFound {len(mp3_files)} MP3 files in {TEST_FOLDER}")
print("=" * 90)

results = []

for fname in mp3_files:
    fpath = os.path.join(TEST_FOLDER, fname)
    print(f"\n{fname}")
    print("-" * 90)

    try:
        audio, _ = librosa.load(fpath, sr=16000, mono=True, duration=DURATION_SEC)
    except Exception as e:
        print(f"  Failed to load: {e}")
        continue

    if len(audio) < 16000:
        print("  Too short, skipping")
        continue

    with torch.no_grad():
        emb = vggish.forward(audio, fs=16000)
        if emb.dim() == 1:
            emb = emb.unsqueeze(0)
        emb = emb.unsqueeze(0).to(device).float()
        emb_norm = (emb - train_mean) / train_std

        h = torch.zeros(1, 1, 256, device=device)
        c = torch.zeros(1, 1, 256, device=device)
        logits, _, _, _ = model(emb_norm, h, c)

        avg_probs = torch.sigmoid(logits[0]).mean(dim=0).cpu().numpy()

    by_category = defaultdict(list)
    for i, tag in enumerate(all_tags):
        if "---" in tag:
            cat, name = tag.split("---", 1)
        else:
            cat, name = "other", tag
        by_category[cat].append((avg_probs[i], name))

    print(f"  Top {TOP_K} overall:")
    top_idx = np.argsort(avg_probs)[::-1][:TOP_K]
    for idx in top_idx:
        print(f"    {avg_probs[idx]*100:5.1f}%  {all_tags[idx]}")

    print(f"  Top per category:")
    for cat in sorted(by_category.keys()):
        top = sorted(by_category[cat], reverse=True)[:3]
        line = "  ".join(f"{p*100:4.1f}% {n}" for p, n in top)
        print(f"    [{cat:10s}] {line}")

    results.append((fname, avg_probs))

print("\n" + "=" * 90)
print("SUMMARY: predicted top genre per song")
print("=" * 90)
genre_indices = [i for i, t in enumerate(all_tags) if t.startswith("genre---")]
for fname, probs in results:
    g_probs = [(probs[i], all_tags[i].split("---")[1]) for i in genre_indices]
    g_probs.sort(reverse=True)
    top = ", ".join(f"{n} ({p*100:.0f}%)" for p, n in g_probs[:3])
    print(f"  {fname:40s}  ->  {top}")
