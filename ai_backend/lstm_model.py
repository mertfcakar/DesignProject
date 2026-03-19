import torch
import torch.nn as nn
import json
import os

# ==========================================
# WP3: The Emotion Translator (HuBERT-LSTM)
# ==========================================

class HubertEmotionLSTM(nn.Module):
    def __init__(self, input_size=768, hidden_size=256, num_layers=1, num_classes=57):
        """
        input_size: 768 (The default latent feature size of hubert-base-ls960)
        hidden_size: 256 (The memory capacity of the LSTM)
        num_classes: 57 (The MTG-Jamendo mood/theme tags)
        """
        super(HubertEmotionLSTM, self).__init__()
        
        # The LSTM layer to understand the sequence of music over time
        self.lstm = nn.LSTM(input_size, hidden_size, num_layers, batch_first=True)
        
        # A dropout layer to prevent overfitting during training
        self.dropout = nn.Dropout(0.3)
        
        # The final classification layer that squashes the data down to 57 emotions
        self.fc = nn.Linear(hidden_size, num_classes)

    def forward(self, x):
        # x shape expects: (Batch Size, Sequence Length, HuBERT Features)
        lstm_out, (h_n, c_n) = self.lstm(x)
        
        # We only care about the very last LSTM output (after it has "heard" the whole sequence)
        last_time_step = lstm_out[:, -1, :]
        
        # Pass it through dropout and the final linear layer
        dropped = self.dropout(last_time_step)
        logits = self.fc(dropped)
        
        return logits

# ==========================================
# TEST THE ARCHITECTURE ON APPLE SILICON
# ==========================================
if __name__ == "__main__":
    print("⚙️ Initializing HuBERT-LSTM Architecture...")
    
    # 1. Load the 57 classes from your config file
    config_path = os.path.join(os.path.dirname(__file__), "jamendo_config.json")
    try:
        with open(config_path, "r") as f:
            config = json.load(f)
            num_classes = config["num_classes"]
    except FileNotFoundError:
        print("❌ Error: jamendo_config.json not found. Run dataset_prep.py first.")
        exit()

    # 2. Target the M4 GPU
    device = torch.device("mps" if torch.backends.mps.is_available() else "cpu")
    print(f"🔥 Hardware Acceleration: {device}")

    # 3. Build the model and send it to the GPU
    model = HubertEmotionLSTM(num_classes=num_classes).to(device)
    print(f"✅ Model compiled with {num_classes} output neurons.")

    # 4. The Tensor Shape Test (Crucial for avoiding crash loops later)
    # We simulate sending 1 batch of audio, containing 10 time steps, with 768 HuBERT features each.
    print("\n🧪 Running Tensor Forward Pass Test...")
    dummy_input = torch.randn(1, 10, 768).to(device)
    
    try:
        output = model(dummy_input)
        print(f"➡️ Input Tensor Shape:  {dummy_input.shape}  -> (Batch, TimeSteps, HuBERT_Features)")
        print(f"⬅️ Output Tensor Shape: {output.shape}     -> (Batch, Jamendo_Emotions)")
        print("\n✅ SUCCESS: The LSTM successfully translated the simulated HuBERT data!")
    except Exception as e:
        print(f"\n❌ CRASH: Tensor mismatch -> {e}")
