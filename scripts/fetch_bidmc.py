"""Fetch original BIDMC 1.0.0 CSVs and verify publisher SHA-256 hashes.

Requires Python 3.9+ and curl (ships with Windows and GitHub runners).
No fixed demographics, credentials, extraction, or third-party Python packages.
"""
import argparse
import concurrent.futures
import hashlib
import json
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
BASE = "https://physionet.org/files/bidmc/1.0.0/"
SUFFIXES = ("Signals.csv", "Numerics.csv", "Breaths.csv")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fetch(item, destination, hashes, base):
    relative = "bidmc_csv/" + item
    expected = hashes[relative]
    path = destination / item
    if not path.exists() or digest(path) != expected:
        temporary = path.with_suffix(path.suffix + ".part")
        if temporary.exists() and digest(temporary) == expected:
            temporary.replace(path)
            return {"file": item, "sha256": expected, "bytes": path.stat().st_size, "url": base + relative}
        for attempt in range(8):
            result = subprocess.run(["curl", "--fail", "--location", "--silent", "--show-error",
                                     "--continue-at", "-", "--max-time", "180", "--output", str(temporary),
                                     base + relative])
            if result.returncode == 0:
                break
        else:
            raise RuntimeError("Download failed after resumable attempts: " + item)
        if digest(temporary) != expected:
            temporary.unlink()
            raise ValueError("Publisher hash mismatch: " + item)
        temporary.replace(path)
    print("verified " + item, flush=True)
    return {"file": item, "sha256": expected, "bytes": path.stat().st_size, "url": base + relative}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--records", nargs="+", default=["01"])
    parser.add_argument("--all", action="store_true", help="Download all 53 records (~150 MB CSV)")
    parser.add_argument("--mirror", action="store_true", help="Use official archive.physionet.org mirror; hashes must still match the versioned publisher manifest")
    parser.add_argument("--output", type=pathlib.Path, default=ROOT / "datasets/bidmc")
    args = parser.parse_args()
    records = [f"{i:02d}" for i in range(1, 54)] if args.all else args.records
    if len(records) != len(set(records)) or any(r not in {f"{i:02d}" for i in range(1, 54)} for r in records):
        parser.error("records must be unique two-digit IDs 01..53")
    args.output.mkdir(parents=True, exist_ok=True)
    manifest = ROOT / "datasets/SHA256SUMS-bidmc-1.0.0.txt"
    hashes = dict((parts[1], parts[0]) for line in manifest.read_text().splitlines()
                  if len(parts := line.split()) == 2)
    items = [f"bidmc_{record}_{suffix}" for record in records for suffix in SUFFIXES]
    base = "https://archive.physionet.org/pn6/bidmc/" if args.mirror else BASE
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        entries = list(pool.map(lambda item: fetch(item, args.output, hashes, base), items))
    (args.output / "download-manifest.json").write_text(json.dumps({
        "dataset": "BIDMC 1.0.0", "source": BASE, "records": records,
        "publisher_manifest_sha256": digest(manifest), "files": entries,
    }, indent=2) + "\n", encoding="utf-8")
    print(f"Verified {len(entries)} original files; destination: {args.output}")


if __name__ == "__main__":
    main()
