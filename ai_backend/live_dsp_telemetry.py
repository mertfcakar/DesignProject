import sounddevice as sd
import numpy as np
from pythonosc import udp_client

# ==========================================
# WP1: LIVE DSP TELEMETRY PIPELINE
# Target: Apple Silicon (M4) / Core Audio
# ==========================================

# --- CONFIGURATION ---
TARGET_IP = "127.0.0.1"
TARGET_PORT = 8000
SAMPLE_RATE = 48000
BLOCK_SIZE = 1024

# --- SIGNAL CONDITIONING ---
# Noise gate threshold to eliminate ambient room static
NOISE_THRESHOLD = 0.005 

# Initialize OSC Client
client = udp_client.SimpleUDPClient(TARGET_IP, TARGET_PORT)

print("🚀 Booting Live DSP Telemetry Pipeline (WP1 Final)...")
print(f"📡 OSC Target: {TARGET_IP}:{TARGET_PORT}")
print(f"🎚️  Noise Gate Threshold: {NOISE_THRESHOLD}")
print("🎤 Listening to MacBook Mic. Play music to test. Press Ctrl+C to stop.")

def audio_callback(indata, frames, time_info, status):
    if status:
        pass # Suppress overflow warnings for a clean terminal output
    
    # 1. Calculate the Root Mean Square (RMS) Energy (Physical Volume)
    rms_energy = float(np.sqrt(np.mean(indata**2)))
    
    # 2. Apply the Software Noise Gate
    if rms_energy < NOISE_THRESHOLD:
        # Silence detected. Clamp values to absolute zero.
        normalized_energy = 0.0
        normalized_zcr = 0.0
    else:
        # 3. Active Signal Detected. Perform DSP Math.
        # Calculate Zero-Crossing Rate (ZCR) for spectral brightness / pitch
        zcr = ((indata[:-1] * indata[1:]) < 0).sum()
        
        # Normalize the values (0.0 to 1.0) for Unreal Engine
        normalized_energy = min(rms_energy * 10, 1.0) 
        normalized_zcr = min(zcr / (BLOCK_SIZE / 2), 1.0)

    # 4. Transmit the data via UDP/OSC at high frequency
    client.send_message("/audio/features", [float(normalized_energy), float(normalized_zcr)])
    
    # 5. Terminal User Interface
    bar_length = int(normalized_energy * 20)
    print(f"📤 Sent -> Energy: {'█' * bar_length:<20} | Pitch (ZCR): {normalized_zcr:.3f}", end='\r')

try:
    # Open direct hardware stream via PortAudio
    with sd.InputStream(samplerate=SAMPLE_RATE, 
                        blocksize=BLOCK_SIZE, 
                        channels=1, 
                        callback=audio_callback):
        while True:
            sd.sleep(100) # Keep main thread alive
except KeyboardInterrupt:
    print("\n\n🛑 WP1 Telemetry Terminated Successfully.")
