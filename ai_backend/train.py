import os
import sys
import torch
import torch.nn as nn
import torch.optim as optim
from torch.utils.data import Dataset, DataLoader
from torch.nn.utils.rnn import pad_sequence

# ==========================================
# 🎯 REFINEMENT: MULTI-LABEL FOCAL LOSS
# ==========================================
class MultiLabelFocalLoss(nn.Module):
    def __init__(self, gamma=2.0):
        super().__init__()
        self.gamma = gamma
        self.bce = nn.BCEWithLogitsLoss(reduction='none')

    def forward(self, logits, targets):
        probs = torch.sigmoid(logits)
        # pt is the probability of the correct class
        pt = targets * probs + (1 - targets) * (1 - probs)
        bce_loss = self.bce(logits, targets)
        # Apply weighting: (1 - pt)^gamma forces focus on "hard" tags
        loss = (1 - pt) ** self.gamma * bce_loss
        return loss.mean()

# ==========================================
# 📊 REFINEMENT: Z-SCORE STATS
# ==========================================
def get_dataset_stats(dataset):
    print("📊 Calculating Dataset Z-Score Stats...")
    all_feats = torch.cat(dataset.features, dim=0)
    mean = all_feats.mean(dim=0)
    std = all_feats.std(dim=0) + 1e-6
    return mean, std

# ==========================================
# 🛠️ SETUP & DATA
# ==========================================
BACKEND_DIR = os.path.dirname(os.path.abspath(__file__))
from lstm_model import StatefulMusicBottleneck

class MusicDataset(Dataset):
    def __init__(self, data_path):
        data = torch.load(data_path, weights_only=False)
        self.features = data["features"]
        self.labels = data["labels"]
    def __len__(self): return len(self.features)
    def __getitem__(self, idx): return self.features[idx], self.labels[idx]

def collate_fn(batch):
    feats, labels = zip(*batch)
    return pad_sequence(feats, batch_first=True), torch.stack(labels)

if __name__ == '__main__':
    device = torch.device('cuda')
    DATA_FILE = os.path.join(BACKEND_DIR, "cached_dataset.pt")
    dataset = MusicDataset(DATA_FILE)
    
    # Pre-calculate Normalization Stats
    mean, std = get_dataset_stats(dataset)
    mean, std = mean.to(device), std.to(device)

    loader = DataLoader(dataset, batch_size=32, shuffle=True, collate_fn=collate_fn)

    # Use the 256 hidden dim model
    model = StatefulMusicBottleneck(output_dim=dataset.labels.shape[1], hidden_dim=256).to(device)
    criterion = MultiLabelFocalLoss(gamma=2.0)

    # Start with Adam (Won et al. 2019 strategy)
    current_lr = 1e-4
    optimizer = optim.Adam(model.parameters(), lr=current_lr)

    print(f"\n🔥 RESEARCH TRAINING START | Mode: Adam | Device: {device}\n")

    for epoch in range(100):
        # Won et al. Optimizer Switch Logic
        if epoch == 60:
            current_lr = 1e-3
            optimizer = optim.SGD(model.parameters(), lr=current_lr, momentum=0.9, weight_decay=1e-4)
            print(f"\n🔄 SWITCHING TO SGD (Stable Refinement) at Epoch {epoch+1}")
        
        if epoch == 80:
            current_lr = 1e-4
            for g in optimizer.param_groups: g['lr'] = current_lr
            print(f"\n📉 DECAYING SGD (Precision Mapping) at Epoch {epoch+1}")

        model.train()
        total_loss = 0
        for i, (x, y) in enumerate(loader):
            x, y = x.to(device), y.to(device)
            
            # 🚨 Apply Z-Score Normalization
            x = (x - mean) / std

            h0 = torch.zeros(1, x.size(0), 256).to(device)
            c0 = torch.zeros(1, x.size(0), 256).to(device)
            
            optimizer.zero_grad()
            preds, _, _, _ = model(x, h0, c0)
            
            # We predict tags based on the full sequence output
            loss = criterion(preds, y.unsqueeze(1).expand(-1, preds.size(1), -1))
            
            loss.backward()
            nn.utils.clip_grad_norm_(model.parameters(), max_norm=1.0)
            optimizer.step()
            total_loss += loss.item()

            if i % 100 == 0:
                sys.stdout.write(f"\rEpoch {epoch+1}/100 | Batch {i}/{len(loader)} | Loss: {loss.item():.4f} ")

        print(f"\n✅ Epoch {epoch+1} Complete | Avg Loss: {total_loss/len(loader):.5f}")
        torch.save({
            'state_dict': model.state_dict(),
            'mean': mean.cpu(),
            'std': std.cpu()
        }, os.path.join(BACKEND_DIR, "music_emotion_weights.pth"))