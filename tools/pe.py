#!/usr/bin/env python3
"""Minimal PE32 reader for runtime ports and native DLL interop.

The section containing a symbol's RVA is the sound distinction between a
function export and a data export. Inferring that distinction from a mangled
name is only a heuristic, so this tool measures it from the image.

Subcommands:
  sections <pe>            section table
  exports  <pe>            ordinal, RVA, kind (CODE/DATA/FORWARD), name
  imports  <pe>            module -> imported symbol
  surface  <target.dll> <pe>...
                           union of symbols the given PEs import from target.dll
  proxydef <pe> <fwdname>  emit a .def forwarding every export to <fwdname>
  iat      <pe>            address of each import thunk -> module!symbol
                           (a runtime can resolve fixed IAT calls from this)

Every subcommand reports its denominator and exits non-zero if the input is
missing or has no table of the requested kind, so an empty result can never be
confused with "I looked and there was nothing".
"""

import os
import struct
import sys


class PE:
    """The PE32 container facts needed by the inspection commands."""

    def __init__(self, path):
        if not os.path.isfile(path):
            sys.exit("pe.py: no such file: %s (searched NOTHING)" % path)
        with open(path, "rb") as file:
            self.data = file.read()
        data = self.data
        if data[:2] != b"MZ":
            sys.exit("pe.py: %s is not an MZ image" % path)
        pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe_offset : pe_offset + 4] != b"PE\0\0":
            sys.exit(
                "pe.py: %s has no PE signature at e_lfanew=0x%x"
                % (path, pe_offset)
            )
        self.path = path
        coff = pe_offset + 4
        (
            self.machine,
            self.nsections,
            _,
            _,
            _,
            optional_size,
            _,
        ) = struct.unpack_from("<HHIIIHH", data, coff)
        optional = coff + 20
        self.magic = struct.unpack_from("<H", data, optional)[0]
        if self.magic != 0x10B:
            sys.exit(
                "pe.py: %s is PE32+ (magic 0x%x); this tool handles PE32 only"
                % (path, self.magic)
            )
        self.image_base = struct.unpack_from("<I", data, optional + 28)[0]
        directory_count = struct.unpack_from("<I", data, optional + 92)[0]
        self.dirs = [
            struct.unpack_from("<II", data, optional + 96 + 8 * index)
            for index in range(directory_count)
        ]
        section_table = optional + optional_size
        self.sections = []
        for index in range(self.nsections):
            offset = section_table + 40 * index
            name = data[offset : offset + 8].rstrip(b"\0").decode("latin1")
            virtual_size, virtual_address, raw_size, raw_address = struct.unpack_from(
                "<IIII", data, offset + 8
            )
            characteristics = struct.unpack_from("<I", data, offset + 36)[0]
            self.sections.append(
                {
                    "name": name,
                    "vaddr": virtual_address,
                    "vsize": virtual_size,
                    "raddr": raw_address,
                    "rsize": raw_size,
                    "chars": characteristics,
                }
            )

    def sec_of(self, rva):
        for section in self.sections:
            if section["vaddr"] <= rva < section["vaddr"] + max(
                section["vsize"], section["rsize"]
            ):
                return section
        return None

    def off(self, rva):
        section = self.sec_of(rva)
        if section is None:
            return None
        return section["raddr"] + (rva - section["vaddr"])

    def cstr(self, rva):
        offset = self.off(rva)
        if offset is None:
            return None
        end = self.data.index(b"\0", offset)
        return self.data[offset:end].decode("latin1")

    def is_code_rva(self, rva):
        """Return whether the RVA is in a code/executable section."""
        section = self.sec_of(rva)
        return bool(section and (section["chars"] & 0x20000020))

    def exports(self):
        """Return (ordinal, RVA, kind, name, forward-target) records."""
        if len(self.dirs) < 1 or self.dirs[0][0] == 0:
            return None
        data, export_rva = self.data, self.dirs[0][0]
        export_size = self.dirs[0][1]
        offset = self.off(export_rva)
        if offset is None:
            sys.exit("pe.py: export dir RVA 0x%x maps to no section" % export_rva)
        ordinal_base = struct.unpack_from("<I", data, offset + 16)[0]
        function_count, name_count = struct.unpack_from("<II", data, offset + 20)
        function_rvas, name_rvas, name_ordinals = struct.unpack_from(
            "<III", data, offset + 28
        )
        names_by_ordinal = {}
        function_offset = self.off(function_rvas)
        name_offset = self.off(name_rvas)
        ordinal_offset = self.off(name_ordinals)
        for index in range(name_count):
            name_rva = struct.unpack_from("<I", data, name_offset + 4 * index)[0]
            ordinal_index = struct.unpack_from(
                "<H", data, ordinal_offset + 2 * index
            )[0]
            names_by_ordinal[ordinal_index] = self.cstr(name_rva)
        result = []
        for index in range(function_count):
            rva = struct.unpack_from("<I", data, function_offset + 4 * index)[0]
            if rva == 0:
                continue
            if export_rva <= rva < export_rva + export_size:
                kind, target = "FORWARD", self.cstr(rva)
            else:
                kind = "CODE" if self.is_code_rva(rva) else "DATA"
                target = None
            result.append(
                (ordinal_base + index, rva, kind, names_by_ordinal.get(index), target)
            )
        return result

    def iat(self):
        """Return {IAT VA: (module, symbol)} for each import thunk."""
        if len(self.dirs) < 2 or self.dirs[1][0] == 0:
            return None
        data = self.data
        offset = self.off(self.dirs[1][0])
        result = {}
        while True:
            original_first, _, _, name_rva, first_thunk = struct.unpack_from(
                "<IIIII", data, offset
            )
            if name_rva == 0:
                break
            module = self.cstr(name_rva)
            names = self.off(original_first or first_thunk)
            slot = first_thunk
            while True:
                value = struct.unpack_from("<I", data, names)[0]
                if value == 0:
                    break
                if value & 0x80000000:
                    symbol = "@%d" % (value & 0xFFFF)
                else:
                    symbol = self.cstr(value + 2)
                result[self.image_base + slot] = (module, symbol)
                names += 4
                slot += 4
            offset += 20
        return result

    def imports(self):
        """Return (module, name-or-None, ordinal-or-None) records."""
        if len(self.dirs) < 2 or self.dirs[1][0] == 0:
            return None
        data = self.data
        offset = self.off(self.dirs[1][0])
        result = []
        while True:
            original_first, _, _, name_rva, first_thunk = struct.unpack_from(
                "<IIIII", data, offset
            )
            if name_rva == 0:
                break
            module = self.cstr(name_rva)
            thunk = original_first or first_thunk
            thunk_offset = self.off(thunk)
            while True:
                value = struct.unpack_from("<I", data, thunk_offset)[0]
                if value == 0:
                    break
                if value & 0x80000000:
                    result.append((module, None, value & 0xFFFF))
                else:
                    result.append((module, self.cstr(value + 2), None))
                thunk_offset += 4
            offset += 20
        return result


def cmd_sections(argv):
    pe = PE(argv[0])
    print(
        "%-10s %-10s %-10s %-10s %s"
        % ("NAME", "VADDR", "VSIZE", "RAW", "FLAGS")
    )
    for section in pe.sections:
        print(
            "%-10s 0x%08x 0x%08x 0x%08x %s0x%08x"
            % (
                section["name"],
                section["vaddr"],
                section["vsize"],
                section["raddr"],
                "CODE " if section["chars"] & 0x20000020 else "     ",
                section["chars"],
            )
        )
    print("-- %d sections in %s" % (len(pe.sections), pe.path))


def cmd_exports(argv):
    pe = PE(argv[0])
    exports = pe.exports()
    if exports is None:
        sys.exit("pe.py: %s has NO export directory -- nothing was scanned" % pe.path)
    for ordinal, rva, kind, name, target in exports:
        print(
            "%6d 0x%08x %-8s %s%s"
            % (
                ordinal,
                rva,
                kind,
                name or "<noname>",
                " -> " + target if target else "",
            )
        )
    kinds = {}
    for export in exports:
        kinds[export[2]] = kinds.get(export[2], 0) + 1
    print("-- %d exports in %s: %s" % (len(exports), pe.path, kinds))


def cmd_imports(argv):
    pe = PE(argv[0])
    imports = pe.imports()
    if imports is None:
        sys.exit("pe.py: %s has NO import directory -- nothing was scanned" % pe.path)
    by_module = {}
    for module, symbol, ordinal in imports:
        by_module.setdefault(module, []).append(symbol or ("@%d" % ordinal))
    for module in sorted(by_module):
        for symbol in by_module[module]:
            print("%-24s %s" % (module, symbol))
    print(
        "-- %d imports from %d modules in %s"
        % (len(imports), len(by_module), pe.path)
    )


def cmd_surface(argv):
    target, paths = argv[0].lower(), argv[1:]
    if not paths:
        sys.exit("pe.py surface: no PE files given -- scanned NOTHING")
    symbols, scanned, contributors = set(), 0, 0
    for path in paths:
        imports = PE(path).imports()
        scanned += 1
        if imports is None:
            print("!! %s has no import directory" % path, file=sys.stderr)
            continue
        previous_count = len(symbols)
        for module, symbol, ordinal in imports:
            if module.lower() == target:
                symbols.add(symbol or ("@%d" % ordinal))
        if len(symbols) > previous_count:
            contributors += 1
    for symbol in sorted(symbols):
        print(symbol)
    print(
        "-- %d unique symbols imported from %s; scanned %d PE files, %d of "
        "which contributed. NOT VISIBLE to this scan: runtime "
        "LoadLibrary/GetProcAddress and delay-load tables."
        % (len(symbols), target, scanned, contributors),
        file=sys.stderr,
    )


def cmd_proxydef(argv):
    pe = PE(argv[0])
    forward_name = argv[1]
    if forward_name.lower().endswith(".dll"):
        forward_name = forward_name[:-4]
    exports = pe.exports()
    if exports is None:
        sys.exit("pe.py: %s has NO export directory" % pe.path)
    # Keep the emitted .def machine-clean; GNU ld's parser rejects a leading
    # semicolon comment. Report the denominator and omissions on stderr.
    print("LIBRARY %s" % os.path.basename(pe.path))
    print("EXPORTS")
    unnamed = data_exports = 0
    for _, _, kind, name, _ in exports:
        if name is None:
            unnamed += 1
            continue
        # GNU ld accepts an ordinal or a forwarder in this form, not both.
        # Consumers must first prove with `pe.py imports` that they do not need
        # the skipped ordinal-only exports.
        line = '  "%s" = "%s.%s"' % (name, forward_name, name)
        if kind == "DATA":
            line += " DATA"
            data_exports += 1
        print(line)
    print(
        "-- %d of %d exports forwarded to %s (%d DATA, %d skipped as "
        "ordinal-only)"
        % (
            len(exports) - unnamed,
            len(exports),
            forward_name,
            data_exports,
            unnamed,
        ),
        file=sys.stderr,
    )
    if unnamed:
        print(
            "!! %d ordinal-only exports were NOT forwarded -- the proxy is "
            "INCOMPLETE" % unnamed,
            file=sys.stderr,
        )


def cmd_iat(argv):
    pe = PE(argv[0])
    table = pe.iat()
    if table is None:
        sys.exit("pe.py: %s has NO import directory -- scanned NOTHING" % pe.path)
    for address in sorted(table):
        module, symbol = table[address]
        print("0x%08x %s %s" % (address, module, symbol))
    print("-- %d IAT slots in %s" % (len(table), pe.path), file=sys.stderr)


COMMANDS = {
    "sections": cmd_sections,
    "exports": cmd_exports,
    "imports": cmd_imports,
    "surface": cmd_surface,
    "proxydef": cmd_proxydef,
    "iat": cmd_iat,
}


def main(argv):
    if not argv or argv[0] not in COMMANDS:
        sys.exit(__doc__)
    COMMANDS[argv[0]](argv[1:])


if __name__ == "__main__":
    main(sys.argv[1:])
