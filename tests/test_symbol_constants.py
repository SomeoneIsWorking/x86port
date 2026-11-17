"""tools/symbol_constants.py refuses every map the build must not compile."""

import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(os.path.dirname(HERE), "tools", "symbol_constants.py")
FIXTURE = os.path.join(HERE, "symbol_constants")

REFUSED = {
    "malformed": ("Game_Update = 401000;", "not `Name = 0xADDR"),
    "attribute": ("Game_Update = 0x00401000; // type:func align:4", "unknown attribute"),
    "type": ("Game_Update = 0x00401000; // type:code", "unknown type"),
    "name": ("Game_Update = 0x00401000;\nGame_Update = 0x00401004;", "Game_Update named twice"),
    "address": ("A = 0x00401000;\nB = 0x00401000;", "0x00401000 named twice"),
    "section": ("Game_Gap = 0x00403000;", "is in no section of Game.exe"),
    "code": ("Game_Data = 0x00402000; // type:func", "is code but 0x00402000 is in .data"),
}


def run(root, *args):
    images = os.path.join(root, "config", "images.toml")
    return subprocess.run([sys.executable, TOOL, "--images", images, *args],
                          capture_output=True, text=True)


def main():
    failures = []
    with tempfile.TemporaryDirectory() as scratch:
        listing = run(FIXTURE, "--list")
        expected = "game_symbols.hpp|%s\n" % os.path.join(FIXTURE, "config", "symbols", "game.txt")
        if listing.returncode or listing.stdout != expected:
            failures.append("--list printed %r" % listing.stdout)
        for case, (line, message) in REFUSED.items():
            root = os.path.join(scratch, case)
            shutil.copytree(FIXTURE, root)
            with open(os.path.join(root, "config", "symbols", "game.txt"), "w") as file:
                file.write(line + "\n")
            out = os.path.join(root, "out")
            result = run(root, "--namespace", "fixture", "--output", out)
            if result.returncode != 1 or message not in result.stderr:
                failures.append("%s: exit %d, %r" % (case, result.returncode, result.stderr))
            if os.path.exists(out):
                failures.append("%s: wrote %s" % (case, out))
    for failure in failures:
        print(failure)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
