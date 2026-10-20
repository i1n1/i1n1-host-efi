# i1n1-host-efi

x64 UEFI host layer: boot runner library and the freestanding i1n1.efi application build.

Experimental, build-only source release: sources, headers and CMake files for the libraries and tools
of this module. No tests, firmware, Apple software or guest images are included, and no complete guest
boot is claimed.

## Dependencies

- `i1n1-core` — https://github.com/i1n1/i1n1-core.git
- `i1n1-engine` — https://github.com/i1n1/i1n1-engine.git
- `i1n1-platform` — https://github.com/i1n1/i1n1-platform.git

Dependencies are resolved by `cmake/i1n1_deps.cmake`: pass `-DI1N1_<NAME>_SOURCE_DIR=<checkout>`
(`CORE`, `ENGINE`, `PLATFORM`) or build through the `https://github.com/i1n1/i1n1.git` superbuild.

## Build

```sh
cmake -S i1n1-host-efi -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DI1N1_CORE_SOURCE_DIR=<path> -DI1N1_ENGINE_SOURCE_DIR=<path> -DI1N1_PLATFORM_SOURCE_DIR=<path>
cmake --build build
```

## Supported hosts and limitations

- Verified build environment: macOS on arm64, Apple clang 21, CMake 4.4, Ninja, C11, `BUILD_TESTING=OFF`.
  Other hosts and compilers have not been verified for this release.
- Warnings are errors by default (`-DI1N1_WARNINGS_AS_ERRORS=OFF` relaxes this for other compilers).
- Unsupported instructions, registers or devices are reported as deterministic stops by the source, not
  emulated; this release is a build of that source, not evidence of a working guest.
- `i1n1.efi` is built only when `clang` can target `x86_64-unknown-windows` and `lld-link` is on PATH; otherwise configure prints a message and only the host libraries are built.

## License

GPL-2.0-only for project-authored material; see `COPYING` and `LICENSE`.
