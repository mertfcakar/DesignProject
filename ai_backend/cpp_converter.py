import torch
import os

# 🛠️ SETUP PATHS
BACKEND_DIR = os.path.dirname(os.path.abspath(__file__))
WEIGHTS_FILE = os.path.join(BACKEND_DIR, "music_emotion_weights.pth")
HEADER_FILE = os.path.join(BACKEND_DIR, "NormalizationConstants.h")

print("📂 Loading training stats...")
# Load your specific weights
checkpoint = torch.load(WEIGHTS_FILE, map_location='cpu', weights_only=False)

# Extract Mean and Std (Standard Deviation)
# We flatten them to a simple list for C++
mean_vals = checkpoint['mean'].flatten().tolist()
std_vals = checkpoint['std'].flatten().tolist()

print(f"📊 Extracted {len(mean_vals)} normalization constants.")

# 📝 WRITE THE C++ HEADER
with open(HEADER_FILE, "w") as f:
    f.write("#pragma once\n\n")
    f.write("// =====================================================\n")
    f.write("// AUTO-GENERATED AI NORMALIZATION CONSTANTS\n")
    f.write("// Copy this file into your Unreal Engine Source folder\n")
    f.write("// =====================================================\n\n")
    
    # Write the Mean Array
    f.write(f"const float VGGISH_MEAN[128] = {{ \n    {', '.join(map(str, mean_vals))} \n}};\n\n")
    
    # Write the Std Array
    f.write(f"const float VGGISH_STD[128] = {{ \n    {', '.join(map(str, std_vals))} \n}};\n")

print(f"✅ Success! File created at: {HEADER_FILE}")