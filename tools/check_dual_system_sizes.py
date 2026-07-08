#!/usr/bin/env python3
"""Validate the real dual-system flash layout against current build outputs."""

from __future__ import annotations

import argparse
import os
import sys
from dataclasses import dataclass


LOW_HEADROOM_RATIO = 0.10
HEADROOM_WARNING_PARTITIONS = {"factory"}


@dataclass(frozen=True)
class Partition:
    name: str
    ptype: str
    subtype: str
    offset: int
    size: int


@dataclass(frozen=True)
class FlashEntry:
    offset: int
    source_path: str
    build_relative_path: str


def parse_size(value: str) -> int:
    text = value.strip()
    if not text:
        raise ValueError("empty size value")

    lower = text.lower()
    if lower.startswith("0x"):
        return int(lower, 16)

    suffixes = (
        ("kb", 1024),
        ("mb", 1024 * 1024),
        ("gb", 1024 * 1024 * 1024),
        ("k", 1024),
        ("m", 1024 * 1024),
        ("g", 1024 * 1024 * 1024),
        ("b", 1),
    )
    for suffix, multiplier in suffixes:
        if lower.endswith(suffix):
            number = text[: -len(suffix)].strip()
            return int(number, 0) * multiplier

    return int(text, 0)


def format_bytes(value: int) -> str:
    units = ("B", "KiB", "MiB", "GiB")
    size = float(value)
    unit = units[0]
    for candidate in units:
        unit = candidate
        if abs(size) < 1024.0 or candidate == units[-1]:
            break
        size /= 1024.0
    if unit == "B":
        return f"{value} B"
    return f"{size:.2f} {unit}"


def parse_partition_csv(path: str) -> list[Partition]:
    partitions: list[Partition] = []
    with open(path, "r", encoding="utf-8") as handle:
        for line_number, raw_line in enumerate(handle, start=1):
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue

            fields = [field.strip() for field in raw_line.split(",")]
            if len(fields) < 5:
                raise ValueError(f"{path}:{line_number}: invalid partition row")

            name, ptype, subtype, offset_text, size_text = fields[:5]
            if not offset_text:
                raise ValueError(
                    f"{path}:{line_number}: partition '{name}' is missing an explicit offset"
                )

            partitions.append(
                Partition(
                    name=name,
                    ptype=ptype,
                    subtype=subtype,
                    offset=parse_size(offset_text),
                    size=parse_size(size_text),
                )
            )

    if not partitions:
        raise ValueError(f"{path}: no partitions found")

    return partitions


def parse_flash_args(path: str) -> list[FlashEntry]:
    base_dir = os.path.dirname(os.path.abspath(path))
    with open(path, "r", encoding="utf-8") as handle:
        lines = [line.strip() for line in handle if line.strip()]

    if len(lines) < 2:
        raise ValueError(f"{path}: invalid flash_args file")

    entries: list[FlashEntry] = []
    for line in lines[1:]:
        parts = line.split(maxsplit=1)
        if len(parts) != 2:
            raise ValueError(f"{path}: invalid flash_args entry '{line}'")

        offset_text, build_relative_path = parts
        source_path = os.path.normpath(os.path.join(base_dir, build_relative_path))
        entries.append(
            FlashEntry(
                offset=parse_size(offset_text),
                source_path=source_path,
                build_relative_path=build_relative_path,
            )
        )

    return entries


def describe_entry(entry: FlashEntry) -> str:
    return os.path.basename(entry.source_path)


def is_special_non_partition_file(entry: FlashEntry) -> bool:
    normalized = entry.build_relative_path.replace("/", "\\").lower()
    return normalized in {
        "bootloader\\bootloader.bin",
        "partition_table\\partition-table.bin",
    }


def validate_layout(partitions: list[Partition], flash_entries: list[FlashEntry], flash_size: int | None) -> int:
    partition_by_offset = {partition.offset: partition for partition in partitions}
    errors: list[str] = []
    warnings: list[str] = []
    max_flash_end = 0

    print("Dual-system partition validation")

    ordered_entries = sorted(flash_entries, key=lambda item: item.offset)
    previous_end = 0
    previous_name = ""
    for entry in ordered_entries:
        if not os.path.exists(entry.source_path):
            errors.append(f"missing build artifact: {entry.source_path}")
            continue

        file_size = os.path.getsize(entry.source_path)
        file_end = entry.offset + file_size
        max_flash_end = max(max_flash_end, file_end)

        if previous_name and entry.offset < previous_end:
            errors.append(
                f"flash overlap: {describe_entry(entry)} @ 0x{entry.offset:x} overlaps {previous_name}"
            )
        previous_end = file_end
        previous_name = describe_entry(entry)

        partition = partition_by_offset.get(entry.offset)
        if partition is None:
            if is_special_non_partition_file(entry):
                print(
                    f"OK   special      {describe_entry(entry):<18} "
                    f"offset 0x{entry.offset:06x} size {format_bytes(file_size)}"
                )
                continue

            errors.append(
                f"no partition starts at 0x{entry.offset:x} for artifact {entry.build_relative_path}"
            )
            continue

        free_bytes = partition.size - file_size
        if free_bytes < 0:
            errors.append(
                f"{partition.name} overflows by {format_bytes(-free_bytes)} "
                f"({describe_entry(entry)} is {format_bytes(file_size)}, partition is {format_bytes(partition.size)})"
            )
            continue

        used_ratio = file_size / partition.size if partition.size else 1.0
        note = ""
        if partition.name == "assets":
            note = " padded SPIFFS image"
        elif partition.name in HEADROOM_WARNING_PARTITIONS and free_bytes / partition.size < LOW_HEADROOM_RATIO:
            warnings.append(
                f"{partition.name} has only {format_bytes(free_bytes)} free ({(free_bytes / partition.size):.1%})"
            )
            note = " low headroom"

        print(
            f"OK   {partition.name:<12} {describe_entry(entry):<18} "
            f"{format_bytes(file_size):>10} / {format_bytes(partition.size):<10} "
            f"used {used_ratio:6.1%}, free {format_bytes(free_bytes)}{note}"
        )

    if flash_size is not None:
        if max_flash_end > flash_size:
            errors.append(
                f"flash image exceeds configured flash size by {format_bytes(max_flash_end - flash_size)}"
            )
        else:
            print(
                f"OK   flash-span    full image span      {format_bytes(max_flash_end)} / "
                f"{format_bytes(flash_size)}"
            )

    if warnings:
        print("")
        print("Warnings:")
        for warning in warnings:
            print(f"- {warning}")

    print("")
    print(
        "Note: ESP-IDF's default app-size warning is expected in this layout because it "
        "compares moriburnner.bin against every app partition, including the smaller RG partitions."
    )
    print(
        "Note: RG partitions are treated as fixed release artifacts here. They are still checked for fit, "
        "but low-headroom warnings are reserved for partitions expected to keep growing."
    )

    if errors:
        print("")
        print("Errors:")
        for error in errors:
            print(f"- {error}")
        return 1

    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate current moriburnner + Retro-Go binaries against the real dual-system partition map."
    )
    parser.add_argument("--partition-csv", required=True, help="Path to partitions.csv")
    parser.add_argument("--flash-args", required=True, help="Path to build/flash_args")
    parser.add_argument("--flash-size", default="", help="Optional total flash size, e.g. 16MB")
    args = parser.parse_args()

    try:
        partitions = parse_partition_csv(os.path.abspath(args.partition_csv))
        flash_entries = parse_flash_args(os.path.abspath(args.flash_args))
        flash_size = parse_size(args.flash_size) if args.flash_size else None
    except Exception as exc:  # noqa: BLE001
        print(f"Error: {exc}", file=sys.stderr)
        return 1

    return validate_layout(partitions, flash_entries, flash_size)


if __name__ == "__main__":
    sys.exit(main())
