import re

features = set()
with open(r'C:\Users\uwu\AppData\Local\rdna4_ags_proxy.log', 'r', encoding='latin-1') as f:
    for line in f:
        m = re.search(r'query: Feature=(\d+)', line)
        if m:
            features.add(int(m.group(1)))

print("Unique Features queried by ForzaTech:")
for feat in sorted(features):
    print(f"Feature {feat} (0x{feat:X})")
