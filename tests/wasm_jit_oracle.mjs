/*
 * wasm_jit_oracle.mjs -- run ONE lowered guest block in a real WebAssembly
 * engine.
 *
 * The oracle for test_jit_wasm. Like wasm_oracle.mjs it is deliberately thin
 * and knows nothing about x86, but this one carries a memory image: the C test
 * lays out an X86pCpu and a guest arena in a flat byte image, this loads that
 * image into the engine's memory, calls the block, and writes the memory back
 * out for the C test to compare against the interpreter's result.
 *
 * THE IMPORTED HELPERS ARE RECORDINGS, NOT REIMPLEMENTATIONS. A lowered block
 * calls back into x86p_alu, x86p_cond and x86p_flag_cf, which are C functions
 * this process cannot reach. So the C test calls them ITSELF, on the state the
 * block will start from, and hands over what they returned and which bytes
 * they wrote. This file replays that: apply the writes, return the value,
 * record the arguments it was actually called with. Nothing about the guest's
 * semantics is decided here -- if it were, the differential would be comparing
 * the interpreter against a JavaScript opinion of the same rules.
 *
 * A module the engine REJECTS is reported as `!invalid` with the reason, and a
 * call that traps as `!trap`. Both are answers the C test asserts on.
 */
import { readFileSync, writeFileSync } from "node:fs";
import { dirname, join, isAbsolute } from "node:path";

const jobPath = process.argv[2];
if (!jobPath) {
  console.error("usage: wasm_jit_oracle.mjs <job.json>");
  process.exit(2);
}

const job = JSON.parse(readFileSync(jobPath, "utf8"));
const base = dirname(jobPath);
const resolve = (p) => (isAbsolute(p) ? p : join(base, p));

function main() {
  const memory = new WebAssembly.Memory({ initial: job.pages });
  const bytes = new Uint8Array(memory.buffer);
  bytes.set(new Uint8Array(readFileSync(resolve(job.image))));

  const module = new WebAssembly.Module(readFileSync(resolve(job.wasm)));
  const lines = [];
  const env = { memory };
  for (const entry of WebAssembly.Module.imports(module)) {
    if (entry.kind !== "function") continue;
    const name = entry.name;
    const spec = job.helpers?.[name];
    env[name] = (...args) => {
      if (!spec) throw new Error(`unrecorded helper ${name}`);
      lines.push(`call ${name} ${args.map((a) => a >>> 0).join(" ")}`);
      for (const write of spec.writes ?? []) {
        const raw = write.hex;
        for (let i = 0; i * 2 < raw.length; i++) {
          bytes[write.at + i] = parseInt(raw.substr(i * 2, 2), 16);
        }
      }
      return spec.return | 0;
    };
  }

  const instance = new WebAssembly.Instance(module, { env });
  const fn = instance.exports[job.entry];
  if (typeof fn !== "function") {
    return [`!missing export ${job.entry}`];
  }
  let exit;
  try {
    exit = fn(job.cpu) | 0;
  } catch (error) {
    return [...lines, `!trap ${error.message}`];
  }
  // Written before the exit line, so a C test that saw `exit` knows the image
  // beside it is the one that block produced.
  writeFileSync(resolve(job.out), Buffer.from(bytes.subarray(0, job.imageBytes)));
  return [...lines, `exit ${exit}`];
}

let out;
try {
  out = main();
} catch (error) {
  out = [`!invalid ${error.message}`];
}
process.stdout.write(out.join("\n") + "\n");
