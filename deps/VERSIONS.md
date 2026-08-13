# Vendored dependency versions (pinned by SHA, recorded in deps/VERSIONS.lock)

All dependencies are vendored as direct sources under `deps/`. They are pinned
to release tarballs fetched from codeload.github.com. `.gitmodules` is not used
because this project pins by SHA recorded in `deps/VERSIONS.lock`; the working
copies are checked directly into git (no system packages, no submodule fetch).

| dep        | version  | source                                          |
|------------|----------|-------------------------------------------------|
| googletest | v1.16.0  | codeload: google/googletest@refs/tags/v1.16.0   |
| libuv      | v1.50.0  | codeload: libuv/libuv@refs/tags/v1.50.0         |
| mbedtls    | v3.6.3   | codeload: Mbed-TLS/mbedtls@refs/tags/v3.6.3     |
| lexbor     | v2.4.0   | codeload: lexbor/lexbor@refs/tags/v2.4.0        |
| termbox2   | v2.5.0   | codeload: termbox/termbox2@refs/tags/v2.5.0     |
| libcurl    | curl-8_11_0 | codeload: curl/curl@refs/tags/curl-8_11_0   |
| quickjs-ng | v0.16.1    | codeload: quickjs-ng/quickjs@refs/tags/v0.16.1 |

mbedTLS is the **only** TLS backend. No OpenSSL anywhere in the build
(`CMAKE_USE_OPENSSL=OFF`, `CMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE`).

`deps/VERSIONS.lock` records the SHA256 of each tarball so upgrades are
audited and pinned.
