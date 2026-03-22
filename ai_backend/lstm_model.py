import torch
import torch.nn as nn

class StatefulMusicBottleneck(nn.Module):
    def __init__(self, input_dim=128, hidden_dim=256, bottleneck_dim=5, output_dim=195):
        super(StatefulMusicBottleneck, self).__init__()
        
        self.lstm = nn.LSTM(input_dim, hidden_dim, batch_first=True, num_layers=1)
        
        # LayerNorm keeps the math from "blowing up"
        self.norm = nn.LayerNorm(hidden_dim)
        
        self.intermediate = nn.Sequential(
            nn.Linear(hidden_dim, 128),
            nn.ReLU(),
            nn.Dropout(0.2), # Prevents memorization
            nn.Linear(128, 64),
            nn.ReLU()
        )
        
        self.bottleneck = nn.Linear(64, bottleneck_dim)
        self.classifier = nn.Linear(bottleneck_dim, output_dim)

    def forward(self, x, h0=None, c0=None):
        lstm_out, (hn, cn) = self.lstm(x, (h0, c0))
        
        # Normalize the LSTM output before it hits the bottleneck
        normalized_out = self.norm(lstm_out)
        
        mid = self.intermediate(normalized_out)
        aesthetic_vector = self.bottleneck(mid)
        
        logits = self.classifier(aesthetic_vector)
        return logits, aesthetic_vector, hn, cn

class InferenceWrapper(nn.Module):
    def __init__(self, model):
        super().__init__()
        self.model = model
    def forward(self, x, h, c):
        logits, aesthetic_vector, hn, cn = self.model(x, h, c)
        return aesthetic_vector[:, -1, :], hn, cn