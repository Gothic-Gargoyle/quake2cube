#!/usr/bin/env python3

import argparse
import re
import struct
import subprocess

from collections import defaultdict
from pathlib import Path


MAGIC = b"Q2SM"
VERSION = 1

TEXT_KINDS = set("TtWw")
DATA_KINDS = set("DdBbRrSs")


def fnv1a32(text):
    value = 0x811C9DC5

    for byte in text.encode("utf-8"):
        value ^= byte
        value = (
            value * 0x01000193
        ) & 0xFFFFFFFF

    return value


def run_nm(nm, path):
    process = subprocess.run(
        [
            nm,
            "-an",
            str(path),
        ],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )

    result = []

    for raw in process.stdout.splitlines():
        parts = raw.strip().split()

        if len(parts) < 2:
            continue

        if len(parts) >= 3:
            address_text = parts[0]
            kind = parts[1]
            name = parts[2]
        else:
            address_text = ""
            kind = parts[0]
            name = parts[1]

        if len(kind) != 1:
            continue

        address = None

        if re.fullmatch(
            r"[0-9A-Fa-f]+",
            address_text,
        ):
            address = int(
                address_text,
                16,
            )

        result.append(
            (
                address,
                kind,
                name,
            )
        )

    return result


def game_function_names(
    root,
    build,
    nm,
):
    definitions = defaultdict(set)

    for source in sorted(
        (root / "game").glob("*.c")
    ):
        obj = build / (
            source.stem + ".o"
        )

        if not obj.exists():
            continue

        for address, kind, name in run_nm(
            nm,
            obj,
        ):
            if kind not in TEXT_KINDS:
                continue

            if not name or name.startswith("."):
                continue

            definitions[name].add(
                source.name
            )

    duplicates = {
        name: sources
        for name, sources
        in definitions.items()
        if len(sources) > 1
    }

    if duplicates:
        message = [
            "duplicate game function symbol names:"
        ]

        for name, sources in sorted(
            duplicates.items()
        ):
            message.append(
                "  "
                + name
                + " -> "
                + ", ".join(
                    sorted(sources)
                )
            )

        raise RuntimeError(
            "\n".join(message)
        )

    return set(
        definitions.keys()
    )


def mmove_names(root):
    pattern = re.compile(
        r"^[ \t]*"
        r"(?:static[ \t]+)?"
        r"(?:const[ \t]+)?"
        r"mmove_t[ \t]+"
        r"([A-Za-z_][A-Za-z0-9_]*)"
        r"[ \t]*=",
        re.MULTILINE,
    )

    result = set()

    for source in sorted(
        (root / "game").glob("*.c")
    ):
        text = source.read_text(
            errors="replace"
        )

        result.update(
            pattern.findall(text)
        )

    return result


def final_symbol_map(
    elf_symbols,
):
    result = defaultdict(list)

    for address, kind, name in elf_symbols:
        if address is None:
            continue

        result[name].append(
            (
                address,
                kind,
            )
        )

    return result


def resolve(
    names,
    final_symbols,
    kinds,
):
    result = []

    for name in sorted(names):
        matches = [
            (
                address,
                kind,
            )
            for address, kind
            in final_symbols.get(
                name,
                []
            )
            if kind in kinds
        ]

        if not matches:
            # #if 0 / unused source / dead-code removal.
            continue

        if len(matches) != 1:
            raise RuntimeError(
                "ambiguous final symbol "
                + name
                + ": "
                + repr(matches)
            )

        address, kind = matches[0]

        result.append(
            (
                name,
                address,
                kind,
            )
        )

    return result


def make_entries(
    namespace,
    symbols,
):
    entries = []

    for name, address, kind in symbols:
        stable_id = fnv1a32(
            namespace + name
        )

        if stable_id == 0:
            raise RuntimeError(
                "stable ID zero reserved: "
                + namespace
                + name
            )

        if not (
            0x80000000
            <= address
            < 0x81800000
        ):
            raise RuntimeError(
                "symbol outside MEM1: "
                + name
                + " "
                + hex(address)
            )

        entries.append(
            {
                "id": stable_id,
                "address": address,
                "name": name,
                "kind": kind,
            }
        )

    entries.sort(
        key=lambda entry:
            entry["id"]
    )

    return entries


def check_collisions(
    function_entries,
    mmove_entries,
):
    ids = defaultdict(list)

    for entry in function_entries:
        ids[entry["id"]].append(
            "function:"
            + entry["name"]
        )

    for entry in mmove_entries:
        ids[entry["id"]].append(
            "mmove:"
            + entry["name"]
        )

    collisions = {
        id_: names
        for id_, names in ids.items()
        if len(names) > 1
    }

    if collisions:
        message = [
            "stable-ID collision:"
        ]

        for id_, names in sorted(
            collisions.items()
        ):
            message.append(
                f"  0x{id_:08x}"
            )

            for name in names:
                message.append(
                    "    " + name
                )

        raise RuntimeError(
            "\n".join(message)
        )


def write_binary(
    path,
    function_entries,
    mmove_entries,
):
    path.parent.mkdir(
        parents=True,
        exist_ok=True,
    )

    temp = path.with_suffix(
        path.suffix + ".tmp"
    )

    with temp.open("wb") as output:
        output.write(
            struct.pack(
                ">4sIII",
                MAGIC,
                VERSION,
                len(function_entries),
                len(mmove_entries),
            )
        )

        for entry in (
            function_entries
            + mmove_entries
        ):
            output.write(
                struct.pack(
                    ">II",
                    entry["id"],
                    entry["address"],
                )
            )

    temp.replace(path)


def write_report(
    path,
    function_entries,
    mmove_entries,
):
    with path.open("w") as output:
        output.write(
            "type\tid\taddress\tkind\tsymbol\n"
        )

        for type_, entries in (
            (
                "FUNCTION",
                function_entries,
            ),
            (
                "MMOVE",
                mmove_entries,
            ),
        ):
            for entry in entries:
                output.write(
                    f'{type_}\t'
                    f'0x{entry["id"]:08x}\t'
                    f'0x{entry["address"]:08x}\t'
                    f'{entry["kind"]}\t'
                    f'{entry["name"]}\n'
                )


def main():
    parser = argparse.ArgumentParser()

    parser.add_argument(
        "--root",
        required=True,
    )

    parser.add_argument(
        "--build",
        required=True,
    )

    parser.add_argument(
        "--elf",
        required=True,
    )

    parser.add_argument(
        "--output",
        required=True,
    )

    parser.add_argument(
        "--report",
        required=True,
    )

    parser.add_argument(
        "--nm",
        required=True,
    )

    args = parser.parse_args()

    root = Path(args.root).resolve()
    build = Path(args.build).resolve()
    elf = Path(args.elf).resolve()
    output = Path(args.output).resolve()
    report = Path(args.report).resolve()

    final_symbols = final_symbol_map(
        run_nm(
            args.nm,
            elf,
        )
    )

    functions = resolve(
        game_function_names(
            root,
            build,
            args.nm,
        ),
        final_symbols,
        TEXT_KINDS,
    )

    mmoves = resolve(
        mmove_names(root),
        final_symbols,
        DATA_KINDS,
    )

    function_entries = make_entries(
        "function:",
        functions,
    )

    mmove_entries = make_entries(
        "mmove:",
        mmoves,
    )

    check_collisions(
        function_entries,
        mmove_entries,
    )

    write_binary(
        output,
        function_entries,
        mmove_entries,
    )

    write_report(
        report,
        function_entries,
        mmove_entries,
    )

    print(
        "Q2SM functions:",
        len(function_entries),
    )

    print(
        "Q2SM mmoves:",
        len(mmove_entries),
    )

    print(
        "Q2SM bytes:",
        output.stat().st_size,
    )


if __name__ == "__main__":
    main()
