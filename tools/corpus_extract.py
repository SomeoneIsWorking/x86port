#!/usr/bin/env python3
"""corpus_extract.py -- turn a Ghidra function export into decoder-diff input.

A title's Ghidra function export carries, for every instruction, the RAW BYTES
beside Ghidra's own mnemonic and length. That makes it an offline, byte-level
second opinion on x86port's decoder, over the whole shipped corpus rather than a
hand-picked sample -- which is the only kind of evidence that settles "is the
decoder right" rather than restating it.

Emits one instruction per line for tools/decode_diff.c:

    <hex bytes>\\t<MNEMONIC>\\t<length>

Refusals, because a corpus tool that returns nothing must not be mistaken for a
corpus that agrees: a path that does not exist, holds no JSON, or holds JSON
with no instruction records exits non-zero and says which. It never prints an
empty stream and exits 0.
"""

import argparse
import json
import pathlib
import sys


def records(path):
    """Yield (hex_bytes, mnemonic, length) from one export, or raise ValueError
    naming what the file lacks. A module whose schema changed must fail here,
    where the file is named, rather than silently contributing zero rows."""
    doc = json.loads(path.read_text())
    functions = doc.get("functions")
    if functions is None:
        raise ValueError(f"{path}: no 'functions' key -- not a Ghidra export?")
    seen = 0
    for fn in functions:
        for insn in fn.get("ins", ()):
            byts, mnemonic, length = insn.get("b"), insn.get("m"), insn.get("n")
            if not byts or not mnemonic or not length:
                continue
            seen += 1
            yield byts, mnemonic, int(length)
    if seen == 0:
        raise ValueError(f"{path}: {len(functions)} function(s) but no instruction "
                         f"records with bytes -- was it exported without --bytes?")


def functions(path):
    """Yield (entry_address, hex_bytes_of_whole_function) from one export.

    A function's instructions are contiguous, so concatenating their bytes
    reconstructs its byte image -- which is what the JIT coverage census needs,
    as opposed to the flat instruction stream a decoder differ needs. Same refusal
    discipline: a file that yields no function bodies says so.
    """
    doc = json.loads(path.read_text())
    fns = doc.get("functions")
    if fns is None:
        raise ValueError(f"{path}: no 'functions' key -- not a Ghidra export?")
    seen = 0
    for fn in fns:
        ins = fn.get("ins") or ()
        if not ins:
            continue
        addr = ins[0].get("a")
        if addr is None:
            continue
        body = "".join(i.get("b") or "" for i in ins)
        if not body:
            continue
        seen += 1
        yield int(addr), body
    if seen == 0:
        raise ValueError(f"{path}: no function bodies with bytes")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("corpus", type=pathlib.Path,
                    help="a Ghidra export .json, or a directory of them")
    ap.add_argument("--functions", action="store_true",
                    help="emit whole function bodies (<entry hex>\\t<hex bytes>) "
                         "for the translator instead of one line per instruction")
    ap.add_argument("--limit", type=int, default=0,
                    help="stop after N instructions (0 = all)")
    args = ap.parse_args()

    if not args.corpus.exists():
        sys.exit(f"corpus_extract: {args.corpus} does not exist. REFUSING rather "
                 f"than emitting an empty corpus, which would read downstream as "
                 f"a decoder that agreed with everything.")

    if args.corpus.is_dir():
        # Ghidra's own bookkeeping files start with a dot and are not exports.
        files = sorted(p for p in args.corpus.glob("*.json") if not p.name.startswith("."))
    else:
        files = [args.corpus]
    if not files:
        sys.exit(f"corpus_extract: no .json exports under {args.corpus}. REFUSING.")

    emitted = 0
    out = sys.stdout
    if args.functions:
        for path in files:
            try:
                for addr, body in functions(path):
                    out.write(f"{addr:08x}\t{body}\n")
                    emitted += 1
                    if args.limit and emitted >= args.limit:
                        print(f"corpus_extract: {emitted} function(s), stopped at --limit",
                              file=sys.stderr)
                        return 0
            except (ValueError, json.JSONDecodeError) as exc:
                sys.exit(f"corpus_extract: {exc}")
        if emitted == 0:
            sys.exit(f"corpus_extract: {len(files)} file(s) yielded 0 functions. REFUSING.")
        print(f"corpus_extract: {emitted} function(s) from {len(files)} file(s)",
              file=sys.stderr)
        return 0

    for path in files:
        try:
            for byts, mnemonic, length in records(path):
                out.write(f"{byts}\t{mnemonic}\t{length}\n")
                emitted += 1
                if args.limit and emitted >= args.limit:
                    print(f"corpus_extract: {emitted} instruction(s) from "
                          f"{len(files)} file(s), stopped at --limit",
                          file=sys.stderr)
                    return 0
        except (ValueError, json.JSONDecodeError) as exc:
            sys.exit(f"corpus_extract: {exc}")

    if emitted == 0:
        sys.exit(f"corpus_extract: {len(files)} file(s) yielded 0 instructions. REFUSING.")
    print(f"corpus_extract: {emitted} instruction(s) from {len(files)} file(s)",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
