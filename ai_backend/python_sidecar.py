"""
python_sidecar.py — Real-time AI inference broadcast to Unreal Engine.

Architecture:
    Spotify --> VoiceMeeter --> shared by:
        * UE5 (does DSP: bass/mid/treble/onsets, lighting)
        * this Python script (does ML inference: 5 scores + top tags)
                                |
                                |  UDP JSON
                                v
                              UE5 AffectiveAudioActor

Usage:
    python python_sidecar.py
    python python_sidecar.py --port 17777 --device 7
    python python_sidecar.py --list-devices

UE5 listens on 127.0.0.1:17777 by default. Match the port if you change it.
"""
import argparse
import json
import os
import socket
import sys
import time
import warnings
from collections import OrderedDict
from queue import Queue, Empty

import numpy as np
import torch
import sounddevice as sd
import librosa

warnings.filterwarnings("ignore")

BACKEND_DIR = os.path.dirname(os.path.abspath(__file__))
if BACKEND_DIR not in sys.path:
    sys.path.insert(0, BACKEND_DIR)

from lstm_model import StatefulMusicBottleneck
from torchvggish.vggish import VGGish

WEIGHTS_FILE = os.path.join(BACKEND_DIR, "music_emotion_weights.pth")
DATA_FILE   = os.path.join(BACKEND_DIR, "cached_dataset.pt")
URLS = {
    'vggish': 'https://github.com/harritaylor/torchvggish/releases/download/v0.1/vggish-10086976.pth',
    'pca':    'https://github.com/harritaylor/torchvggish/releases/download/v0.1/vggish_pca_params-970ea276.pth',
}

SILENCE_THRESHOLD = 0.001
EMA_ALPHA         = 0.15
TARGET_HZ         = 16000

def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument('--port', type=int, default=17777, help='UDP port (default 17777)')
    p.add_argument('--host', type=str, default='127.0.0.1', help='UDP target host')
    p.add_argument('--device', type=int, default=None, help='Audio input device ID')
    p.add_argument('--list-devices', action='store_true', help='List audio devices and exit')
    return p.parse_args()

def load_models(device):
    print("Loading tag names...")
    data = torch.load(DATA_FILE, weights_only=False)
    all_tags = data["tags"]
    print(f"  {len(all_tags)} tags")

    print("Loading VGGish (with PCA)...")
    vggish = VGGish(urls=URLS).to(device)
    vggish.eval()

    print("Loading classifier...")
    model = StatefulMusicBottleneck(output_dim=len(all_tags), hidden_dim=256).to(device)
    ck = torch.load(WEIGHTS_FILE, map_location=device)
    sd_dict = OrderedDict()
    for k, v in ck['state_dict'].items():
        sd_dict[k.replace("_orig_mod.", "")] = v
    model.load_state_dict(sd_dict)
    model.eval()
    train_mean = ck['mean'].to(device)
    train_std  = ck['std'].to(device)

    return vggish, model, train_mean, train_std, all_tags

def top_per_category(smoothed_probs, all_tags, prefix, k=3):
    items = []
    for i, tag in enumerate(all_tags):
        if tag.startswith(prefix):
            items.append((float(smoothed_probs[i]), tag[len(prefix):]))
    items.sort(reverse=True)
    return items[:k]

def main():
    args = parse_args()

    if args.list_devices:
        print(sd.query_devices())
        return

    device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
    print(f"Compute device: {device}")

    vggish, model, train_mean, train_std, all_tags = load_models(device)

    # Audio device discovery
    if args.device is not None:
        device_id = args.device
    else:
        device_id = sd.default.device[0]
    info = sd.query_devices(device_id, 'input')
    native_sr = int(info['default_samplerate'])
    print(f"Audio input: {info['name']} ({native_sr} Hz)")

    # UDP socket (just sends)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    target = (args.host, args.port)
    print(f"Sending UDP JSON to {target}")

    # Audio queue for callback->main thread handoff
    audio_q: "Queue[np.ndarray]" = Queue()
    def audio_callback(indata, frames, time_info, status):
        audio_q.put(indata.flatten().copy())

    smoothed = np.zeros(len(all_tags), dtype=np.float32)

    print("\nStarting inference loop. Ctrl+C to stop.\n")
    print(f"{'sec':>5}  {'genre':<22}  {'mood':<22}  {'instrument':<22}")
    print("-" * 80)

    inference_count = 0
    t0 = time.time()

    try:
        with sd.InputStream(device=device_id, channels=1, samplerate=native_sr,
                            callback=audio_callback, blocksize=int(native_sr * 0.05)):
            while True:
                # Gather ~1 second of audio at native sample rate
                buf = []
                got = 0
                while got < native_sr:
                    try:
                        chunk = audio_q.get(timeout=0.5)
                        buf.append(chunk)
                        got += len(chunk)
                    except Empty:
                        continue
                full = np.concatenate(buf)[:native_sr]

                vol = float(np.max(np.abs(full)))
                if vol < SILENCE_THRESHOLD:
                    # Send a silence packet so UE5 knows we are alive but quiet
                    sock.sendto(json.dumps({
                        "scores": [0.0]*5,
                        "top_genres": [],
                        "top_moods": [],
                        "top_instruments": [],
                        "silence": True,
                        "rms": vol,
                    }).encode(), target)
                    continue

                # Resample to VGGish's 16 kHz
                resampled = librosa.resample(full.astype(np.float32),
                                              orig_sr=native_sr, target_sr=TARGET_HZ)

                # Inference
                with torch.no_grad():
                    emb = vggish.forward(resampled, fs=TARGET_HZ)
                    if emb.dim() == 1:
                        emb = emb.unsqueeze(0)
                    emb = emb.unsqueeze(0).to(device).float()
                    emb_norm = (emb - train_mean) / train_std
                    h = torch.zeros(1, 1, 256, device=device)
                    c = torch.zeros(1, 1, 256, device=device)
                    logits, aes_vec, _, _ = model(emb_norm, h, c)
                    tag_probs = torch.sigmoid(logits[0]).mean(dim=0).cpu().numpy()
                    scores    = aes_vec[0, -1, :].cpu().numpy()

                # EMA smoothing on tag probabilities
                smoothed = smoothed * (1.0 - EMA_ALPHA) + tag_probs.astype(np.float32) * EMA_ALPHA

                # Send top-15 per category so UE5 can aggregate confidence across the
                # full subgenre family (rock + alternative + heavymetal + indie + grunge + ...
                # all sum to "RockMetal" family). With only top-3, single-tag flicker between
                # near-tied genres (rock 44 / pop 44) made the lighting bounce; top-15 gives
                # the subgenre tail room to break the tie reliably.
                top_genres      = top_per_category(smoothed, all_tags, "genre---", 15)
                top_moods       = top_per_category(smoothed, all_tags, "mood/theme---", 15)
                top_instruments = top_per_category(smoothed, all_tags, "instrument---", 15)

                msg = {
                    "scores": [float(s) for s in scores],
                    "top_genres":      [[name, pct] for pct, name in top_genres],
                    "top_moods":       [[name, pct] for pct, name in top_moods],
                    "top_instruments": [[name, pct] for pct, name in top_instruments],
                    "silence": False,
                    "rms": vol,
                }
                sock.sendto(json.dumps(msg).encode(), target)

                inference_count += 1
                elapsed = time.time() - t0
                tg = top_genres[0]      if top_genres      else (0.0, "?")
                tm = top_moods[0]       if top_moods       else (0.0, "?")
                ti = top_instruments[0] if top_instruments else (0.0, "?")
                print(f"{elapsed:5.1f}  {tg[1]+f' ({tg[0]*100:.0f}%)':<22}  "
                      f"{tm[1]+f' ({tm[0]*100:.0f}%)':<22}  "
                      f"{ti[1]+f' ({ti[0]*100:.0f}%)':<22}")

    except KeyboardInterrupt:
        elapsed = time.time() - t0
        rate = inference_count / elapsed if elapsed > 0 else 0
        print(f"\nStopped after {inference_count} inferences ({rate:.1f} Hz average)")
    finally:
        sock.close()

if __name__ == "__main__":
    main()
