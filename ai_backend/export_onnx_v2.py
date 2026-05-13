"""
Export AestheticBrain_v2.onnx with FOUR outputs:
  - tag_probs        (195 sigmoid probabilities for genre/mood/instrument)
  - aesthetic_vector (5-dim bottleneck, kept for compatibility)
  - h_n, c_n         (LSTM state for next inference)

Also writes tag_names.json so UE5 can map tag indices to readable names.

Run from inside ai_backend:
    python export_onnx_v2.py
"""
import os
import json
from collections import OrderedDict
import torch

from lstm_model import StatefulMusicBottleneck, InferenceWrapperV2

BACKEND_DIR = os.path.dirname(os.path.abspath(__file__))
WEIGHTS_FILE = os.path.join(BACKEND_DIR, "music_emotion_weights.pth")
DATA_FILE = os.path.join(BACKEND_DIR, "cached_dataset.pt")
ONNX_OUT = os.path.join(BACKEND_DIR, "AestheticBrain_v2.onnx")
TAGS_OUT = os.path.join(BACKEND_DIR, "tag_names.json")

print("Loading checkpoint and tag list ...")
ck = torch.load(WEIGHTS_FILE, map_location='cpu')
data = torch.load(DATA_FILE, weights_only=False)
all_tags = data["tags"]
print(f"  {len(all_tags)} tags in checkpoint")

print("Building model ...")
model = StatefulMusicBottleneck(output_dim=len(all_tags), hidden_dim=256)
sd = OrderedDict()
for k, v in ck['state_dict'].items():
    sd[k.replace("_orig_mod.", "")] = v
model.load_state_dict(sd)
model.eval()

wrapper = InferenceWrapperV2(model)
wrapper.eval()

x = torch.zeros(1, 1, 128)
h = torch.zeros(1, 1, 256)
c = torch.zeros(1, 1, 256)

print(f"Tracing and exporting to {ONNX_OUT} ...")
torch.onnx.export(
    wrapper,
    (x, h, c),
    ONNX_OUT,
    input_names=['x', 'h', 'c'],
    output_names=['tag_probs', 'aesthetic_vector', 'h_n', 'c_n'],
    dynamic_axes=None,
    opset_version=17,
)
print("ONNX exported.")

print(f"Writing tag names to {TAGS_OUT} ...")
with open(TAGS_OUT, 'w', encoding='utf-8') as f:
    json.dump(all_tags, f, ensure_ascii=False, indent=2)
print(f"Wrote {len(all_tags)} tag names.")

print("\nVerifying ONNX with onnxruntime ...")
try:
    import onnxruntime as ort
    import numpy as np
    sess = ort.InferenceSession(ONNX_OUT, providers=['CPUExecutionProvider'])
    feeds = {
        'x': np.zeros((1, 1, 128), dtype=np.float32),
        'h': np.zeros((1, 1, 256), dtype=np.float32),
        'c': np.zeros((1, 1, 256), dtype=np.float32),
    }
    outs = sess.run(None, feeds)
    print(f"  Output shapes: tag_probs={outs[0].shape}, aesthetic_vector={outs[1].shape}, h_n={outs[2].shape}, c_n={outs[3].shape}")
    print(f"  tag_probs first 5: {outs[0][0, :5]}")
    print("OK")
except Exception as e:
    print(f"  Verification failed: {e}")

print("\nDone. Next: copy AestheticBrain_v2.onnx + tag_names.json into UE5 Content folder.")
