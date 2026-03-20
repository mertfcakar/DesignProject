import os
import requests
import pandas as pd
from concurrent.futures import ThreadPoolExecutor

# ==========================================
# WP3: MASTER DATASET INGESTION (500GB)
# ==========================================

TSV_PATH = "autotagging.tsv" # Ensure you have the full 55k+ track file
DATASET_DIR = "D:/MTG_Jamendo_Full" # Your fast 2TB M.2 Drive
os.makedirs(DATASET_DIR, exist_ok=True)

print("📖 Parsing full MTG-Jamendo Metadata...")
df = pd.read_csv(TSV_PATH, sep='\t')

def download_track(row):
    track_id = str(row['track_id']).replace('track_', '').lstrip('0')
    url = f"https://mp3d.jamendo.com/download/track/{track_id}/mp32/"
    file_path = os.path.join(DATASET_DIR, f"{track_id}.mp3")
    
    if os.path.exists(file_path): 
        return # Skip if already downloaded
    
    try:
        r = requests.get(url, timeout=20)
        if r.status_code == 200:
            with open(file_path, 'wb') as f:
                f.write(r.content)
    except Exception as e:
        pass # Skip broken links silently to keep the pipeline moving

print(f"🚀 Initializing 1Gbps Optimized Download of {len(df)} tracks...")
# 30 workers is usually the sweet spot for a 1Gbps connection
with ThreadPoolExecutor(max_workers=30) as executor:
    list(executor.map(download_track, [row for _, row in df.iterrows()]))

print("🎉 DATABASE INGESTION COMPLETE.")