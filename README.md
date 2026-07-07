# image_viewer

## Building

This project links against `libavif`/`libaom` built from source (submodules under
`libs/`), not checked into git. After cloning:

```
git submodule update --init --recursive
```

Then build the native static libs once (needs the Android NDK, CMake, Ninja, and
Perl on PATH - Git for Windows' bundled Perl is used automatically on Windows):

```
./scripts/build_native_libs.ps1
```

This produces `libs/prebuilt/<ABI>/{libaom.a,libavif.a}`, which
`app/src/main/cpp/CMakeLists.txt` links against. Only `arm64-v8a` is built by
default; pass `-Abis arm64-v8a,armeabi-v7a,x86_64` etc. to build more.

Re-run the script whenever `libs/aom` or `libs/libavif` are updated. After that,
build the app normally via Gradle / Android Studio.
