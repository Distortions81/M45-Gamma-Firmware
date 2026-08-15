#!/usr/bin/env python3

import argparse
import pathlib
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--generator", required=True)
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    source = pathlib.Path(args.input)
    output = pathlib.Path(args.output)
    with tempfile.TemporaryDirectory() as temporary:
        binary = pathlib.Path(temporary) / "partition-table.bin"
        subprocess.run(
            [sys.executable, args.generator, str(source), str(binary)], check=True
        )
        table = binary.read_bytes()

    last_used = max(index for index, value in enumerate(table) if value != 0xFF)
    embedded_size = ((last_used + 1 + 31) // 32) * 32
    table = table[:embedded_size]

    lines = [
        "/* Generated from partitions_canonical.csv. Do not edit directly. */",
        "#pragma once",
        "",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
        "static const uint8_t m45_canonical_partition_table[] = {",
    ]
    for offset in range(0, len(table), 12):
        values = ", ".join(f"0x{value:02x}" for value in table[offset : offset + 12])
        lines.append(f"    {values},")
    lines.extend(
        [
            "};",
            "static const size_t m45_canonical_partition_table_size =",
            "    sizeof(m45_canonical_partition_table);",
            "",
        ]
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(lines))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
