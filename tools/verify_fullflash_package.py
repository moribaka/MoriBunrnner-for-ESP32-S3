"""Verify a packaged flash map, merged images, manifest and ZIP without a board."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import zipfile


def digest(data):
    return hashlib.sha256(data).hexdigest()


def verify(directory, archive):
    config = json.loads((directory / "flasher_args.json").read_text(encoding="utf-8-sig"))
    assert config["extra_esptool_args"]["chip"] == "esp32s3"
    assert config["flash_settings"] == {"flash_mode": "dio", "flash_size": "16MB", "flash_freq": "80m"}
    merged = (directory / "FULL.bin").read_bytes()
    assert len(merged) == 16 * 1024 * 1024
    for alias in ("fullflash-single.bin", "moriburnner_dualsystem_fullflash_merged.bin"):
        assert (directory / alias).read_bytes() == merged, f"Merged alias differs: {alias}"
    ranges = []
    assert len(config["flash_files"]) == 10
    for address, filename in config["flash_files"].items():
        relative = PurePosixPath(filename)
        assert not relative.is_absolute() and ".." not in relative.parts and ":" not in filename
        data = (directory / filename).read_bytes()
        start = int(address, 0)
        end = start + len(data)
        assert end <= len(merged)
        assert merged[start:end] == data, f"Merged payload differs: {filename} at {address}"
        ranges.append((start, end, filename))
    ranges.sort()
    for previous, following in zip(ranges, ranges[1:]):
        assert previous[1] <= following[0], f"Overlap: {previous[2]}, {following[2]}"

    expected = {}
    for line in (directory / "SHA256SUMS.txt").read_text(encoding="utf-8-sig").splitlines():
        checksum, filename = line.split(" *", 1)
        filename = filename.replace("\\", "/")
        assert filename not in expected
        expected[filename] = checksum.lower()
        assert digest((directory / filename).read_bytes()) == checksum.lower(), filename
    files = {path.relative_to(directory).as_posix(): path for path in directory.rglob("*") if path.is_file()}
    assert set(expected) == set(files) - {"SHA256SUMS.txt"}, "Manifest does not cover package files"
    with zipfile.ZipFile(archive) as package:
        members = [entry.filename.replace("\\", "/") for entry in package.infolist() if not entry.is_dir()]
        assert len(members) == len(set(members)), "Duplicate ZIP members"
        assert set(members) == set(files), "ZIP contents differ from package directory"
        for filename in members:
            assert digest(package.read(filename)) == digest(files[filename].read_bytes()), filename
    print(json.dumps({"ok": True, "firmware_images": len(ranges), "package_files": len(files),
                      "merged_bytes": len(merged), "merged_sha256": digest(merged),
                      "zip_sha256": digest(archive.read_bytes())}, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("archive", type=Path)
    args = parser.parse_args()
    verify(args.directory.resolve(), args.archive.resolve())
