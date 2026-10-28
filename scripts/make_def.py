"""Makes a .def file listing all exported functions of a DLL.

Used by the GitHub Actions build: obs.dll from the official OBS release
-> obs.def -> `lib /def:obs.def` -> obs.lib to link the plugin against.

Usage: python make_def.py obs.dll obs.def
"""
import sys

import pefile


def main() -> None:
    dll_path, def_path = sys.argv[1], sys.argv[2]
    pe = pefile.PE(dll_path, fast_load=True)
    pe.parse_data_directories(
        directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]]
    )
    names = [
        sym.name.decode()
        for sym in pe.DIRECTORY_ENTRY_EXPORT.symbols
        if sym.name
    ]
    library = dll_path.replace("\\", "/").split("/")[-1].rsplit(".", 1)[0]
    with open(def_path, "w", encoding="ascii") as f:
        f.write(f"LIBRARY {library}\nEXPORTS\n")
        for name in names:
            f.write(f"    {name}\n")
    print(f"{len(names)} exports written to {def_path}")


if __name__ == "__main__":
    main()
