import torch
import sounddevice as sd
import numpy as np
import librosa
from transformers import Wav2Vec2FeatureExtractor, HubertModel
from pythonosc import udp_client

# ==========================================
# 1. SYSTEM CONFIGURATION
# ==========================================
SAMPLE_RATE = 48000       # Mac native mic rate
HUBERT_RATE = 16000       # AI required rate
BUFFER_DURATION = 1.0     # 1 second of audio memory
CHUNK_SIZE = 1024         # 21ms updates

# Calculate buffer size in samples
BUFFER_SIZE = int(SAMPLE_RATE * BUFFER_DURATION)
audio_buffer = np.zeros(BUFFER_SIZE, dtype=np.float32)

# OSC Telemetry Setup (Sending to Unreal Engine)
OSC_IP = "127.0.0.1"
OSC_PORT = 8000
osc_client = udp_client.SimpleUDPClient(OSC_IP, OSC_PORT)

# ==========================================
# 2. LOAD THE AI MODEL (Apple Silicon Optimized)
# ==========================================
print("⏳ Loading HuBERT Neural Network into Memory...")

# Force PyTorch to use the M4 Metal GPU
device = torch.device("mps" if torch.backends.mps.is_available() else "cpu")
print(f"🔥 Hardware Acceleration: {device}")

# Load the Hugging Face model
processor = Wav2Vec2FeatureExtractor.from_pretrained("facebook/hubert-base-ls960")
model = HubertModel.from_pretrained("facebook/hubert-base-ls960").to(device)
model.eval() # Set model to evaluation mode (no training)

print("✅ AI Brain Online. Listening to microphone...")

# ==========================================
# 3. THE AUDIO CALLBACK (The "Ear")
# ==========================================
def audio_callback(indata, frames, time, status):
    global audio_buffer
    
    if status:
        print(status)
    
    # Flatten the incoming stereo/mono data to 1D
    new_audio = indata[:, 0]
    
    # Shift the buffer left (drop the oldest audio) and append the newest audio
    audio_buffer = np.roll(audio_buffer, -frames)
    audio_buffer[-frames:] = new_audio
    
    # To save CPU, we only run the heavy AI math if the audio is loud enough (Noise Gate)
    rms_energy = np.sqrt(np.mean(new_audio**2))
    if rms_energy > 0.005:
        process_ai_features(audio_buffer)

# ==========================================
# 4. THE AI INFERENCE (The "Brain")
# ==========================================
def process_ai_features(raw_audio):
    # 1. Resample from 48kHz to 16kHz for HuBERT
    audio_16k = librosa.resample(y=raw_audio, orig_sr=SAMPLE_RATE, target_sr=HUBERT_RATE)
    
    # 2. Format for PyTorch and move to the M4 GPU
    inputs = processor(audio_16k, sampling_rate=HUBERT_RATE, return_tensors="pt")
    input_values = inputs.input_values.to(device)
    
    # 3. Feed forward through the Neural Network
    with torch.no_grad(): # Disable gradients for maximum speed
        outputs = model(input_values)
        
    # 4. Extract the features from the final hidden state
    # This tensor is huge (e.g., [1, 49, 768]). We take the mean across the time dimension.
    hidden_states = outputs.last_hidden_state
    mean_features = torch.mean(hidden_states, dim=1).squeeze()
    
    # 5. Bring data back to the CPU to send over the network
    features_cpu = mean_features.cpu().numpy()
    
    # For WP2, we will just extract the first 4 deep features to test the pipeline
    f1, f2, f3, f4 = float(features_cpu[0]), float(features_cpu[1]), float(features_cpu[2]), float(features_cpu[3])
    
    # Print to terminal so you can see the AI thinking
    print(f"🧠 AI Embedding Output -> F1: {f1:.2f} | F2: {f2:.2f} | F3: {f3:.2f} | F4: {f4:.2f}")
    
    # Send the AI data to Unreal Engine
    osc_client.send_message("/ai_data", [f1, f2, f3, f4])

# ==========================================
# 5. START THE SYSTEM
# ==========================================
with sd.InputStream(samplerate=SAMPLE_RATE, channels=1, blocksize=CHUNK_SIZE, callback=audio_callback):
    try:
        sd.sleep(9999999) # Keep the script running forever
    except KeyboardInterrupt:
        print("\n🔴 System Shutting Down.")
