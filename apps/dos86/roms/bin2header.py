#!/usr/bin/env python3
"""Convert a binary file to a C header with a uint8_t array."""

import sys
import os

def bin2header(inpath, outpath, array_name, size_macro):
    with open(inpath, 'rb') as f:
        data = f.read()

    guard = os.path.basename(outpath).upper().replace('.', '_')
    with open(outpath, 'w') as f:
        f.write(f"/* Auto-generated from {os.path.basename(inpath)} by bin2header.py */\n")
        f.write(f"#ifndef {guard}\n")
        f.write(f"#define {guard}\n\n")
        f.write(f"#include <stdint.h>\n\n")
        f.write(f"#define {size_macro} {len(data)}\n\n")
        f.write(f"static const uint8_t {array_name}[] = {{\n")
        for i in range(0, len(data), 16):
            chunk = data[i:i+16]
            f.write("    " + ", ".join(f"0x{b:02X}" for b in chunk))
            if i + 16 < len(data):
                f.write(",")
            f.write("\n")
        f.write("};\n\n")
        f.write(f"#endif /* {guard} */\n")

    print(f"Generated {outpath}: {len(data)} bytes -> {array_name}[{len(data)}]")

if __name__ == '__main__':
    if len(sys.argv) != 5:
        print(f"Usage: {sys.argv[0]} <input.bin> <output.h> <array_name> <SIZE_MACRO>")
        sys.exit(1)
    bin2header(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4])
