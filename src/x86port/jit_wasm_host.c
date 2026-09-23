#include "jit_wasm_host.h"

#include "jit_wasm_module.h"

#include <emscripten.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct X86pWasmHostState {
  char error[256];
  const char *names[kX86pWasmImportCount];
  uintptr_t addresses[kX86pWasmImportCount];
} X86pWasmHostState;

EM_JS_DEPS(x86p_wasm_host,
           "$addFunction,$removeFunction,$getWasmTableEntry,$setWasmTableEntry,$UTF8ToString,$stringToUTF8");

EM_JS(int, host_create, (void *key), {
  if (typeof window != 'undefined' && typeof document != 'undefined') {
    return 0;
  }
  if (!Module.x86pWasmHosts) {
    Module.x86pWasmHosts = new Map();
  }
  Module.x86pWasmHosts.set(key, {modules : new Map(), next : 1, env : null, importBindings : 0});
  return 1;
});

EM_JS(int,
      host_instantiate,
      (void *key,
       const void *bytes,
       size_t length,
       const char *const *names,
       const uintptr_t *addresses,
       unsigned count,
       char *error,
       unsigned error_len),
      {
        const host = Module.x86pWasmHosts.get(key);
        if (!host.env) {
          const env = {memory : wasmMemory, table : wasmTable};
          for (let i = 0; i < count; ++i) {
            env[UTF8ToString(HEAPU32[(names >>> 2) + i])] = getWasmTableEntry(HEAPU32[(addresses >>> 2) + i]);
          }
          host.env = env;
          host.importBindings++;
        }
        let instance;
        try {
          const start = bytes >>> 0;
          const module = new WebAssembly.Module(HEAPU8.subarray(start, start + length));
          instance = new WebAssembly.Instance(module, {env : host.env});
        } catch (failure) {
          // Every failure here is a refusal to publish, not a broken program.
          // Nothing has been added to host.modules yet, and the caller already
          // counts a refusal and falls back, so there is a proven state to
          // return to. Rethrowing did not: measured in Firefox 156, the module
          // compiler threw `InternalError: out of memory` roughly a second into
          // a run, the exception crossed the C stack out of the worker's
          // onmessage, and the guest thread died while the heartbeat went on
          // reporting it as "running guest code" for another seven minutes.
          // The name is kept because an out-of-memory and a bad module need
          // different fixes.
          // WITH the count the engine is actually looking at. The arena's own
          // live count said 15,104 when Firefox refused, and a page that
          // released the same number of modules was accepted immediately --
          // so the two counts disagreeing is the thing to see, and only this
          // side can report the host map's size.
          stringToUTF8(failure.name + ": " + failure.message + " [" + host.modules.size +
                           " module(s) held by this host]",
                       error,
                       error_len);
          // WHOSE fault it was. A module the engine will not compile, link or
          // accept the shape of is this side's defect and says nothing about
          // how many modules the engine holds; anything else -- out of memory,
          // a range error -- is the engine declining to take another one.
          // Telling them apart is what keeps one bad module from teaching the
          // arena a ceiling that does not exist.
          const mine = failure instanceof WebAssembly.CompileError || failure instanceof WebAssembly.LinkError ||
                       failure instanceof TypeError;
          return mine ? -2 : -1;
        }
        const id = host.next++;
        host.modules.set(id, {instance, entries : new Map()});
        return id;
      });

EM_JS(int, host_resolve, (void *key, int id, const char *field), {
  const module = Module.x86pWasmHosts.get(key).modules.get(id);
  if (!module) {
    return 0;
  }
  const name = UTF8ToString(field);
  if (module.entries.has(name)) {
    return module.entries.get(name);
  }
  const fn = module.instance.exports[name];
  if (typeof fn != 'function') {
    return 0;
  }
  const entry = addFunction(fn, 'ii');
  module.entries.set(name, entry);
  return entry;
});

/*
 * Point an existing table entry at another module's export, and move the
 * entry's ownership with it.
 *
 * The entry's INDEX does not change, which is the whole point: it is the
 * address the block was published at, and compiled code and the block cache
 * both hold it. Only what sits behind it changes. Ownership moves by adding
 * the index to the destination's entry map and dropping it from the source's,
 * so the later release of the source calls neither removeFunction nor
 * setWasmTableEntry on an index a block is still entered through -- which
 * would recycle a live index and leave the table's null entry in its place.
 */
EM_JS(int, host_adopt, (void *key, int to_id, const char *to_field, int from_id, const char *from_field, int entry), {
  const host = Module.x86pWasmHosts.get(key);
  const to = host.modules.get(to_id);
  const from = host.modules.get(from_id);
  if (!to || !from) {
    return 0;
  }
  const name = UTF8ToString(to_field);
  const fn = to.instance.exports[name];
  if (typeof fn != 'function') {
    return 0;
  }
  setWasmTableEntry(entry, fn);
  to.entries.set(name, entry);
  from.entries.delete(UTF8ToString(from_field));
  return 1;
});

EM_JS(void, host_release, (void *key, int id), {
  const host = Module.x86pWasmHosts.get(key);
  const module = host.modules.get(id);
  if (!module) {
    return;
  }
  for (const entry of module.entries.values()) {
    removeFunction(entry);
    // removeFunction recycles the index; clear the reference immediately so a
    // released instance can be collected even if no new block replaces it.
    setWasmTableEntry(entry, null);
  }
  host.modules.delete(id);
});

EM_JS(void, host_destroy, (void *key), {
  const host = Module.x86pWasmHosts.get(key);
  for (const module of host.modules.values()) {
    for (const entry of module.entries.values()) {
      removeFunction(entry);
      setWasmTableEntry(entry, null);
    }
  }
  Module.x86pWasmHosts.delete(key);
});

static int instantiate(void *user, const void *bytes, size_t length, char *error, unsigned error_len) {
  X86pWasmHostState *state = user;
  int module;
  state->error[0] = '\0';
  module = host_instantiate(
      user, bytes, length, state->names, state->addresses, kX86pWasmImportCount, state->error, sizeof state->error);
  if (module < 0 && error && error_len) {
    snprintf(error, error_len, "%s", state->error);
  }
  return module;
}

static int resolve(void *user, int module, const char *field) {
  return host_resolve(user, module, field);
}

static int adopt(void *user, int to, const char *to_field, int from, const char *from_field, int entry) {
  return host_adopt(user, to, to_field, from, from_field, entry);
}

static void release(void *user, int module) {
  host_release(user, module);
}

int x86p_wasm_host_create(X86pWasmHost *host, char *reason, unsigned reason_len) {
  X86pWasmHostState *state;
  unsigned i;
  if (!host) {
    if (reason && reason_len) {
      snprintf(reason, reason_len, "no WebAssembly host output");
    }
    return 0;
  }
  memset(host, 0, sizeof *host);
  state = calloc(1u, sizeof *state);
  if (!state || !host_create(state)) {
    if (reason && reason_len) {
      snprintf(reason, reason_len, "%s", state ? "WebAssembly JIT requires a browser worker" : "out of memory");
    }
    free(state);
    return 0;
  }
  for (i = 0; i < kX86pWasmImportCount; ++i) {
    state->names[i] = x86p_wasm_import_field((X86pWasmImport)i);
    state->addresses[i] = (uintptr_t)x86p_wasm_import_address((X86pWasmImport)i);
  }
  host->instantiate = instantiate;
  host->resolve = resolve;
  host->adopt = adopt;
  host->release = release;
  host->user = state;
  return 1;
}

void x86p_wasm_host_destroy(X86pWasmHost *host) {
  if (host && host->user) {
    host_destroy(host->user);
    free(host->user);
    memset(host, 0, sizeof *host);
  }
}

const char *x86p_wasm_host_error(const X86pWasmHost *host) {
  const X86pWasmHostState *state = host->user;
  return state->error;
}
