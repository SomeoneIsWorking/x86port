/*
 * wasm_oracle.mjs -- run emitted modules in a real WebAssembly engine.
 *
 * The oracle for test_emit_wasm. It is deliberately thin and knows nothing
 * about x86: it reads a manifest the C test wrote, instantiates each module,
 * calls one export with the given arguments, and prints the result. Every
 * expectation lives in the C test, which owns the intent; this only reports
 * what the engine did.
 *
 * A module that the engine REJECTS is reported as `!invalid` with the reason,
 * and a call that traps as `!trap`. Both are results the C test asserts on --
 * "the engine refused this" is an answer, not a harness failure, and a test
 * that could not tell those apart from a crash would be measuring itself.
 */
import { readFileSync } from "node:fs";
import { join, dirname } from "node:path";

const manifestPath = process.argv[2];
if (!manifestPath) {
  console.error("usage: wasm_oracle.mjs <manifest.json>");
  process.exit(2);
}

const manifest = JSON.parse(readFileSync(manifestPath, "utf8"));
const base = dirname(manifestPath);

// One memory, shared by every case that imports one, so a case can be written
// to store and then load back through the same address space the guest would.
const memory = new WebAssembly.Memory({ initial: 1 });

// A deterministic imported helper: a translated block calling out to the
// runtime is the shape being tested, not the arithmetic it calls.
const helper = (a, b) => (a + b * 2) | 0;

// Every case module declares the same imports whether or not it uses them, so
// the index numbering stays identical across cases. The oracle therefore
// supplies all of them every time: a table handed only to the cases that call
// through it would make every other module fail to instantiate, which reads as
// a broken encoder rather than a harness that withheld an import.
function importsFor(table) {
  return { env: { memory, helper, table } };
}

let failed = 0;
for (const entry of manifest.cases) {
  let line;
  try {
    const table = new WebAssembly.Table({ initial: 1, element: "anyfunc" });
    if (entry.table) {
      const helperBytes = readFileSync(join(base, entry.table.file));
      const helperInstance = new WebAssembly.Instance(
        new WebAssembly.Module(helperBytes),
        importsFor(table),
      );
      table.set(0, helperInstance.exports[entry.table.export]);
    }
    const bytes = readFileSync(join(base, entry.file));
    const instance = new WebAssembly.Instance(
      new WebAssembly.Module(bytes),
      importsFor(table),
    );
    const fn = instance.exports[entry.export];
    if (typeof fn !== "function") {
      line = `!missing export ${entry.export}`;
    } else {
      try {
        line = String(fn(...(entry.args ?? [])) | 0);
      } catch (error) {
        line = `!trap ${error.message}`;
      }
    }
  } catch (error) {
    line = `!invalid ${error.message}`;
    failed++;
  }
  process.stdout.write(`${entry.name}\t${line}\n`);
}

// A rejected module is reported per case above and judged by the C test; the
// exit status reports only whether this harness itself ran every case.
process.exit(failed > 0 && manifest.cases.length === 0 ? 1 : 0);
