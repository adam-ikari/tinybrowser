# Vendored dependency versions (pinned by SHA, recorded in deps/VERSIONS.lock)

Most dependencies are vendored as direct sources under `deps/`, pinned to release
tarballs fetched from codeload.github.com and checked into git (no system
packages).

**Exception — the JS engine is now a git submodule.** `deps/qzjs` is
`https://github.com/adam-ikari/qzjs.git`, pinned by commit SHA in `.gitmodules` /
the gitlink. qzjs is an embeddable QuickJS-ng runtime (QuickJS-ng + libuv +
mbedTLS + miniz behind one C API). Because it `add_subdirectory`s its own copies
of QuickJS-ng / libuv / mbedTLS — emitting identically-named CMake targets
(`qjs`, `uv_a`, `mbedtls`, `mbedx509`, `mbedcrypto`) — tinybrowser's former
standalone copies of those three were removed; qzjs's copies are used instead.

| dep        | version  | source                                          |
|------------|----------|-------------------------------------------------|
| googletest | v1.16.0  | codeload: google/googletest@refs/tags/v1.16.0   |
| lexbor     | v2.4.0   | codeload: lexbor/lexbor@refs/tags/v2.4.0        |
| termbox2   | v2.5.0   | codeload: termbox/termbox2@refs/tags/v2.5.0     |
| libcurl    | curl-8_11_0 | codeload: curl/curl@refs/tags/curl-8_11_0   |
| **qzjs**   | `ba4dd94` | **git submodule**: adam-ikari/qzjs@`ba4dd94`   |

qzjs's own dependencies arrive through *its* nested submodules. Only the four
needed for a core build are initialized:

| qzjs submodule | commit    | needed for                    |
|----------------|-----------|-------------------------------|
| `deps/quickjs-ng` | `6d46d07` | the JS engine            |
| `deps/libuv`       | `84af0b1` | qzjs's event loop        |
| `deps/mbedtls`     | `068ff08` | TLS (the only TLS backend)|
| `deps/miniz`       | `77d0dce` | native compression       |

Deliberately **not** initialized, and gated `OFF` in `CMakeLists.txt`:

- `deps/wamr` — WebAssembly engine, largest of the set; tinybrowser runs no wasm
  (`QZ_WITH_WAMR=OFF`)
- `deps/wasm3` — alternative wasm engine (`QZ_WITH_WASM3=OFF`)
- `deps/lz4` — only needed for `QZ_POLYFILL_MODE=compressed` (default is `rodata`)
- `deps/googletest` — qzjs's own tests (`QZ_BUILD_TESTS=OFF`)

Note: `deps/qzjs/deps/mbedtls` has a further nested submodule (`framework`) that
**is** required — mbedTLS's CMake config step fails without it.

### Building qzjs from a fresh clone

qzjs's embedded polyfill bytecode is a build product, not committed, so it must be
generated:

```sh
npm --prefix deps/qzjs/polyfill ci   # installs esbuild
cmake -S . -B build                  # re-configure AFTER npm ci
cmake --build build -j
```

The toolchain check runs at **configure** time, so re-running CMake after
`npm ci` is required; otherwise the build fails with an actionable message about
the missing polyfill toolchain.

### TLS

`QZ_WITH_TLS=ON`, so qzjs links mbedTLS and compiles its HTTPS branches.
`QZ_PROFILE=minimal` would otherwise disable TLS (it assumes no https needed);
we re-enable it explicitly.

⚠️ **All `QZ_WITH_*` values must be written to the CMake cache *before*
`add_subdirectory(deps/qzjs)`.** qzjs reads them *during* that call to decide
whether to link mbedTLS and whether to define `QZ_WITH_TLS` (which gates the
HTTPS code in `uv_io.c` / `tcp_io.c`). Changing the cache afterwards does not
retroactively change already-generated build rules. Setting it after produced a
silent no-op switch: `CMakeCache.txt` said `QZ_WITH_TLS:BOOL=ON` while
`flags.make` compiled `QZ_WITH_TLS=0` — the entire HTTPS path compiled out and
`libqzjs` never linked mbedTLS. **Verify with
`grep QZ_WITH_TLS build/deps/qzjs/CMakeFiles/qzjs.dir/flags.make`, not the
cache.**

`QZ_PROFILE_LAST` is pre-seeded to match `QZ_PROFILE` so qzjs's profile-switch
block (`deps/qzjs/CMakeLists.txt:169`) is skipped; otherwise it `FORCE`-rewrites
the whole `QZ_WITH_*` group and undoes our settings.

Note: our test suite does not exercise HTTPS — every test uses a fake transport
or plain local http, so a broken TLS build still passes all 74 tests. Check the
compiled flag, not the tests.

mbedTLS is the **only** TLS backend. No OpenSSL anywhere in the build
(`CMAKE_USE_OPENSSL=OFF`, `CMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE`).

`deps/VERSIONS.lock` records the SHA256 of each tarball so upgrades are
audited and pinned. It does not cover `qzjs`, which is pinned by commit SHA as a
submodule.