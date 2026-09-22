import os
import sys
import struct
import argparse
import tempfile
from collections import defaultdict

import hashlib

try:
    import zstandard
except ImportError:
    sys.exit("AssetsPackager: the 'zstandard' Python package is required (pip install zstandard)")

# Bin file:  "ASPK", u32 VERSION, u32 entryCount,
#            entryCount x (u64 id, u64 offset, u64 storedSize, u64 rawSize), then the data blob.
# Index file: "AIDX", u32 IDX_VERSION, u32 entryCount,
#            entryCount x (u64 id, u32 binId, u64 offset, u64 storedSize, u64 rawSize),
#            u32 binCount, binCount x (u32 binId, u16 nameLen, name).
# Every asset is stored as a Zstd frame (storedSize = frame bytes, rawSize = decompressed bytes).
MAGIC = b"ASPK"
VERSION = 3
IDX_MAGIC = b"AIDX"
IDX_VERSION = 3

DEFAULT_LEVEL = 19

def hash_asset(path: str) -> int:
    path = path.replace("\\", "/")
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
                assets.append(line.replace("\\", "/"))
    return assets

def pack_bin(bin_name, assets, asset_root, out_dir, cctx):
    entries = []
    data_blob = bytearray()

    for asset in assets:
        full_path = os.path.join(asset_root, asset)
        with open(full_path, "rb") as f:
            data = f.read()

        logical_path = asset.replace("\\", "/")
        asset_id = hash_asset(logical_path)

        stored = cctx.compress(data)
        print(f"Packing: {logical_path}  {len(data)} -> {len(stored)}")

        entries.append({
            "asset_id": asset_id,
            "offset": len(data_blob),
            "size": len(stored),
            "raw_size": len(data),
        })

        data_blob.extend(stored)

    out_path = os.path.join(out_dir, bin_name)
    with open(out_path, "wb") as out:
        out.write(MAGIC)
        out.write(struct.pack("<I", VERSION))
        out.write(struct.pack("<I", len(entries)))

        for e in entries:
            out.write(struct.pack("<QQQQ", e["asset_id"], e["offset"], e["size"], e["raw_size"]))

        out.write(data_blob)

    return entries


def inputs_stamp(scenes, scene_assets, asset_root, level):
    """Fingerprint of everything the output depends on, so an unchanged asset set isn't recompressed on every build."""
    h = hashlib.sha256()
    h.update(f"{VERSION}/{IDX_VERSION}/{level}/{zstandard.__version__}".encode())
    with open(os.path.abspath(__file__), "rb") as f:
        h.update(f.read())

    files = set(scenes.values())
    for assets in scene_assets.values():
        files.update(os.path.join(asset_root, a) for a in assets)
    for path in sorted(files):
        st = os.stat(path)
        h.update(f"{path}|{st.st_size}|{st.st_mtime_ns}".encode())
    return h.hexdigest()

def stamp_path(output_dir):
    # Kept out of the output folder so it never ends up in a shipped build.
    key = hashlib.sha1(os.path.abspath(output_dir).encode("utf-8")).hexdigest()[:16]
    return os.path.join(tempfile.gettempdir(), "nettfg-assets", key + ".stamp")


def main(asset_root, output_dir, level=DEFAULT_LEVEL, force=False):
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

    expected_bins = ["assets.idx"]
    if shared_assets:
        expected_bins.append("shared.bin")
    expected_bins += [f"{scene}.bin" for scene in scene_only]

    stamp_file = stamp_path(output_dir)
    stamp = inputs_stamp(scenes, scene_assets, asset_root, level)
    if not force and all(os.path.exists(os.path.join(output_dir, n)) for n in expected_bins):
        try:
            with open(stamp_file, "r") as f:
                if f.read().strip() == stamp:
                    print("Assets up to date, skipping packing (use --force to repack)")
                    return
        except OSError:
            pass

    # threads=-1: one worker per core. Large assets (tens of MB) dominate the pack time.
    cctx = zstandard.ZstdCompressor(level=level, threads=-1)

    asset_index = []  # (asset_id, bin_id, offset, stored_size, raw_size)
    bin_ids = {}
    next_bin_id = 0

    def pack(bin_name, assets):
        nonlocal next_bin_id
        bin_ids[bin_name] = next_bin_id
        next_bin_id += 1

        entries = pack_bin(bin_name, assets, asset_root, output_dir, cctx)
        for e in entries:
            asset_index.append((
                e["asset_id"],
                bin_ids[bin_name],
                e["offset"],
                e["size"],
                e["raw_size"]
            ))

    if shared_assets:
        pack("shared.bin", shared_assets)

    for scene, assets in scene_only.items():
        pack(f"{scene}.bin", assets)


    # Write index (C++ compatible)
    idx_path = os.path.join(output_dir, "assets.idx")
    with open(idx_path, "wb") as out:
        out.write(IDX_MAGIC)
        out.write(struct.pack("<I", IDX_VERSION))
        out.write(struct.pack("<I", len(asset_index)))

        for asset_id, bin_id, offset, size, raw_size in asset_index:
            out.write(struct.pack(
                "<Q I Q Q Q",
                asset_id,
                bin_id,
                offset,
                size,
                raw_size
            ))

        # Bin table, after the entries: the numbering used above, by bin name.
        # The ids alone don't say which bin is which, and the engine numbering
        # bins in the order a game happens to load them only matches this
        # numbering by luck (it did with two scenes; a third broke it).
        out.write(struct.pack("<I", len(bin_ids)))
        for bin_name, bin_id in bin_ids.items():
            raw_name = bin_name.encode("utf-8")
            out.write(struct.pack("<I H", bin_id, len(raw_name)))
            out.write(raw_name)

    total_raw = sum(e[4] for e in asset_index)
    total_stored = sum(e[3] for e in asset_index)
    print(f"Packed {len(asset_index)} assets: {total_raw} -> {total_stored} bytes")
    print(f"Bins created: {len(bin_ids)}")

    try:
        os.makedirs(os.path.dirname(stamp_file), exist_ok=True)
        with open(stamp_file, "w") as f:
            f.write(stamp)
    except OSError:
        pass  # only costs a repack next build

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("assets", help="Assets root directory")
    parser.add_argument("output", help="Output directory")
    parser.add_argument("--level", type=int, default=DEFAULT_LEVEL, help=f"Zstd compression level (default {DEFAULT_LEVEL})")
    parser.add_argument("--force", action="store_true", help="Repack even if the inputs haven't changed")
    args = parser.parse_args()
    main(args.assets, args.output, args.level, args.force)
