# Native runtime dependencies

These sources are compiled into `TWebFrame.lib` by `TWebFrame.vcxproj`. A network download, CMake installation, separate DLL, or external JavaScript engine is not needed to build them.

| Directory | Upstream version | Source | License |
| --- | --- | --- | --- |
| `pcre2` | 10.44 | [PCRE2 release tag](https://github.com/PCRE2Project/pcre2/tree/pcre2-10.44) | [BSD license](pcre2/LICENCE) |
| `wasm3` | 0.5.0 | [wasm3 release tag](https://github.com/wasm3/wasm3/tree/v0.5.0) | [MIT license](wasm3/LICENSE) |

The release archives used have SHA-256 hashes:

- PCRE2: `07A002E8216382A96F722BC4A831F3D77457FE3E9E62A6DFF250A2DD0E9C5E6D`
- wasm3: `B778DD72EE2251F4FE9E2666EE3FE1C26F06F517C3FFCE572416DB067546536C`

The JavaScript runtime uses PCRE2 only when `SUPPORT_PCRE2` is defined in `src/JavaScript.h` or the library's build settings. Otherwise it uses C++ `std::wregex` with ECMAScript syntax. Both backends map captures, offsets and legacy RegExp capture properties. The standard backend has no named groups or lookbehind, and uses UTF-16 code-unit matching rather than JavaScript Unicode-mode matching. VS 2019's standard library treats anchors as multiline even without the `m` flag.

PCRE2 uses the UTF-16 static library, generic character tables, and Unicode support. `config.h`, `pcre2.h`, and `pcre2_chartables.c` come from the upstream generic/distributed files. Native JIT support is disabled. The adapter also maps named groups in PCRE2 mode.

wasm3 builds only its core interpreter; WASI and host filesystem bindings are disabled. Linear memory is limited to 4,096 pages (256 MiB) and the execution stack to 1 MiB per runtime. `m3_core.c` delegates `m3_Yield` to `TWebFrameWasmYield`, and `m3_exec.h` polls at loop back edges. The adapter installs the calling realm's cancellation callback during exported calls and start functions. These are the only upstream interpreter modifications.

The adapter currently rejects table exports and imported memories/globals. SIMD, threads, GC and newer WebAssembly proposals are not implemented. PCRE2 is not an ECMAScript conformance implementation; differences outside the regression coverage, including some legacy syntax and lone surrogates, remain.
