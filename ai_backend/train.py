import torch
import torch.nn as nn
import torch.optim as optim
from torch.utils.data import DataLoader, TensorDataset, random_split
import json
import os

# ==========================================
# WP3: FINAL EMOTION TRAINING LOOP
# ==========================================
print("⚙️ Initializing Final Training Phase...")

# 1. Setup Hardware
device = torch.device("mps" if torch.backends.mps.is_available() else "cpu")

# 2. Load the Cached Data
data_path = os.path.join(os.path.dirname(__file__), "cached_dataset.pt")
if not os.path.exists(data_path):
    print("❌ Error: cached_dataset.pt not found. Run extraction first!")
    exit()

data = torch.load(data_path)
X = data["features"] # The HuBERT math
y = data["labels"]   # The Emotion IDs

# The LSTM expects (Batch, Sequence, Features). 
# Since we averaged the features, we add a "fake" sequence dimension of 1.
X = X.unsqueeze(1) 

print(f"📊 Loaded {len(X)} samples across 5 emotion categories.")

# 3. Split into Training (80%) and Validation (20%)
dataset = TensorDataset(X, y)
train_size = int(0.8 * len(dataset))
val_size = len(dataset) - train_size
train_db, val_db = random_split(dataset, [train_size, val_size])

train_loader = DataLoader(train_db, batch_size=16, shuffle=True)
val_loader = DataLoader(val_db, batch_size=16)

# 4. Initialize the Model (using the 57-class architecture we defined)
from lstm_model import HubertEmotionLSTM
model = HubertEmotionLSTM(num_classes=57).to(device)

criterion = nn.CrossEntropyLoss()
optimizer = optim.Adam(model.parameters(), lr=0.0005)

# 5. The Training Loop
epochs = 100
print(f"🚀 Training on {device} for {epochs} epochs...")

for epoch in range(epochs):
    model.train()
    train_loss = 0
    for batch_X, batch_y in train_loader:
        batch_X, batch_y = batch_X.to(device), batch_y.to(device)
        
        optimizer.zero_grad()
        outputs = model(batch_X)
        loss = criterion(outputs, batch_y)
        loss.backward()
        optimizer.step()
        train_loss += loss.item()

    # Validation Phase (Testing the AI on songs it hasn't seen)
    model.eval()
    correct = 0
    total = 0
    with torch.no_grad():
        for batch_X, batch_y in val_loader:
            batch_X, batch_y = batch_X.to(device), batch_y.to(device)
            outputs = model(batch_X)
            _, predicted = torch.max(outputs.data, 1)
            total += batch_y.size(0)
            correct += (predicted == batch_y).sum().item()

    accuracy = 100 * correct / total
    if (epoch + 1) % 10 == 0:
        print(f"📈 Epoch [{epoch+1}/{epochs}] | Loss: {train_loss/len(train_loader):.4f} | Val Accuracy: {accuracy:.2f}%")

# 6. Save the Final Brain
model_path = os.path.join(os.path.dirname(__file__), "emotion_model.pth")
torch.save(model.state_dict(), model_path)
print(f"\n🎉 TRAINING COMPLETE!")
print(f"💾 Final Model saved as: {model_path}")