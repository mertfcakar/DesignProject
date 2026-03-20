import torch
import torch.nn as nn

class ProductionMusicLSTM(nn.Module):
    def __init__(self, input_dim=128, hidden_dim=256, bottleneck_dim=5, num_classes=195):
        super(ProductionMusicLSTM, self).__init__()
        
        self.lstm = nn.LSTM(input_dim, hidden_dim, num_layers=2, batch_first=True, dropout=0.3)
        self.bottleneck = nn.Linear(hidden_dim, bottleneck_dim)
        self.classifier = nn.Linear(bottleneck_dim, num_classes)

    def forward(self, x, lengths=None):
        # BUG FIX: If lengths are provided, we pack the sequence to ignore padding zeros
        if lengths is not None:
            # enforce_sorted=False allows us to pass batches without sorting them by length manually
            packed_x = nn.utils.rnn.pack_padded_sequence(x, lengths.cpu(), batch_first=True, enforce_sorted=False)
            packed_out, (h_n, c_n) = self.lstm(packed_x)
            
            # h_n contains the final hidden state for each sequence, IGNORING padding!
            # Shape of h_n is (num_layers, batch_size, hidden_dim). We want the top layer [-1].
            last_step_out = h_n[-1]
        else:
            # Fallback for single live-stream inferences without padding
            lstm_out, (h_n, c_n) = self.lstm(x)
            last_step_out = lstm_out[:, -1, :]
            
        # Compress to 5-D Aesthetic Vector
        aesthetic_vector = self.bottleneck(last_step_out)
        
        # Expand to 195-D Logits
        logits = self.classifier(aesthetic_vector)
        
        return logits, aesthetic_vector