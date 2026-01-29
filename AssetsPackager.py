import os
import struct
import argparse
from collections import defaultdict

MAGIC = b"ASPK"
VERSION = 1

import hashlib

def hash_asset(path: str) -> int:
    path = path.replace("\\", "/")  # normalize
    digest = hashlib.md5(path.encode("utf-8")).digest()
    return int.from_bytes(digest[:8], byteorder="little", signed=False)


def collect_ntfg_files(root):
    scenes = {}
    for base, _, files in os.walk(root):
        for f in files:
            if f.endswith(".ntfg"):
                scene = os.path.splitext(f)[0]
                scenes[scene] = os.path.join(base, f)
    return scenes

def read_scene_assets(ntfg_path):
    assets = []
    with open(ntfg_path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#"):
                # normalize Windows backslashes
                assets.append(line.replace("\\", "/"))
    return assets

def pack_bin(bin_name, assets, asset_root, out_dir):
    entries = []
    data_blob = bytearray()

    for asset in assets:
        full_path = os.path.join(asset_root, asset)
        with open(full_path, "rb") as f:
            data = f.read()

        logical_path = asset.replace("\\", "/")
        asset_id = hash_asset(logical_path)

        print(f"Packing: {logical_path}")

        entries.append({
            "asset_id": asset_id,
            "offset": len(data_blob),
            "size": len(data),
        })

        data_blob.extend(data)

    out_path = os.path.join(out_dir, bin_name)
    with open(out_path, "wb") as out:
        out.write(MAGIC)
        out.write(struct.pack("<I", VERSION))
        out.write(struct.pack("<I", len(entries)))

        for e in entries:
            out.write(struct.pack("<Q", e["asset_id"]))
            out.write(struct.pack("<Q", e["offset"]))
            out.write(struct.pack("<Q", e["size"]))

        out.write(data_blob)

    return entries




def main(asset_root, output_dir):
    os.makedirs(output_dir, exist_ok=True)

    scenes = collect_ntfg_files(asset_root)

    # Compute usage across scenes to find shared assets
    usage = defaultdict(set)
    scene_assets = {}
    for scene, ntfg in scenes.items():
        assets = read_scene_assets(ntfg)
        scene_assets[scene] = assets
        for a in assets:
            usage[a].add(scene)

    shared_assets = [a for a, s in usage.items() if len(s) > 1]
    scene_only = {
        scene: [a for a in assets if a not in shared_assets]
        for scene, assets in scene_assets.items()
    }

    asset_index = []  # (asset_id, bin_id, offset, size)
    bin_ids = {}
    next_bin_id = 0

    # Shared bin
    if shared_assets:
        bin_name = "shared.bin"
        bin_ids[bin_name] = next_bin_id
        next_bin_id += 1

        entries = pack_bin(bin_name, shared_assets, asset_root, output_dir)
        for e in entries:
            asset_index.append((
                e["asset_id"],
                bin_ids[bin_name],
                e["offset"],
                e["size"]
            ))

    # Scene bins
    for scene, assets in scene_only.items():
        if not assets:
            continue

        bin_name = f"{scene}.bin"
        bin_ids[bin_name] = next_bin_id
        next_bin_id += 1

        entries = pack_bin(bin_name, assets, asset_root, output_dir)
        for e in entries:
            asset_index.append((
                e["asset_id"],
                bin_ids[bin_name],
                e["offset"],
                e["size"]
            ))


    # Write index
    # Write index (C++ compatible)
    idx_path = os.path.join(output_dir, "assets.idx")
    with open(idx_path, "wb") as out:
        out.write(struct.pack("<I", len(asset_index)))

        for asset_id, bin_id, offset, size in asset_index:
            out.write(struct.pack(
                "<Q I Q Q",
                asset_id,
                bin_id,
                offset,
                size
            ))



    print(f"Packed {len(asset_index)} assets")
    print(f"Bins created: {len(bin_ids)}")

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("assets", help="Assets root directory")
    parser.add_argument("output", help="Output directory")
    args = parser.parse_args()
    main(args.assets, args.output)
