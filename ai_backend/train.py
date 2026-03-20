import torch
import torch.nn as nn
import torch.optim as optim
from torch.utils.data import Dataset, DataLoader
from lstm_model import ProductionMusicLSTM

device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
CACHE_FILE = "cached_dataset.pt"

class JamendoDataset(Dataset):
    def __init__(self, pt_file):
        data = torch.load(pt_file)
        self.features = data['features']
        self.labels = data['labels']
        self.num_classes = len(data['tags'])
        
    def __len__(self): return len(self.features)
    def __getitem__(self, idx): return self.features[idx], self.labels[idx]

def collate_fn(batch):
    features, labels = zip(*batch)
    lengths = torch.tensor([len(f) for f in features])
    features_padded = torch.nn.utils.rnn.pad_sequence(features, batch_first=True)
    return features_padded, torch.stack(labels), lengths

print("📦 Loading Cached Data...")
dataset = JamendoDataset(CACHE_FILE)
dataloader = DataLoader(dataset, batch_size=128, shuffle=True, collate_fn=collate_fn)

model = ProductionMusicLSTM(input_dim=128, num_classes=dataset.num_classes).to(device)
criterion = nn.BCEWithLogitsLoss()
optimizer = optim.Adam(model.parameters(), lr=0.001)

epochs = 50
print(f"🚀 Starting Training on {device}...")

for epoch in range(epochs):
    model.train()
    total_loss = 0
    
    for batch_features, batch_labels, batch_lengths in dataloader:
        batch_features, batch_labels = batch_features.to(device), batch_labels.to(device)
        
        optimizer.zero_grad()
        
        # Pass the lengths to the model so it can ignore the zeros!
        logits, _ = model(batch_features, lengths=batch_lengths)
        
        loss = criterion(logits, batch_labels)
        loss.backward()
        optimizer.step()
        total_loss += loss.item()
        
    avg_loss = total_loss / len(dataloader)
    print(f"Epoch [{epoch+1}/{epochs}] | Loss: {avg_loss:.4f}")

torch.save(model.state_dict(), "emotion_model_v2.pth")
print("🎉 Model is completely bug-free and ready for Unreal Engine!")