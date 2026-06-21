Import("env")
import os

# tinycbor normally relies on CMake to generate two headers from .in templates.
# PlatformIO clones the raw source without running CMake, so we generate them here.
# On Arduino/embedded targets all visibility macros expand to nothing, and we
# pin version values to the 0.6.0 release that the pinned SHA tracks.

libdeps_dir = env.subst("$PROJECT_LIBDEPS_DIR")
env_name = env.subst("$PIOENV")
tinycbor_src = os.path.join(libdeps_dir, env_name, "tinycbor", "src")
os.makedirs(tinycbor_src, exist_ok=True)

generated = {
    "tinycbor-export.h": """\
#ifndef CBOR_API
#define CBOR_API
#endif
""",
    "tinycbor-version.h": """\
#ifndef TINYCBOR_VERSION_H
#define TINYCBOR_VERSION_H

#define TINYCBOR_VERSION_MAJOR 0
#define TINYCBOR_VERSION_MINOR 6
#define TINYCBOR_VERSION_PATCH 0
#define TINYCBOR_VERSION_STRING "0.6.0"

#endif /* TINYCBOR_VERSION_H */
""",
}

for filename, content in generated.items():
    path = os.path.join(tinycbor_src, filename)
    if not os.path.exists(path):
        with open(path, "w") as f:
            f.write(content)
        print(f"Generated {filename}")

# open_memstream.c is a POSIX portability shim requiring funopen (BSD) or
# fopencookie (glibc) — neither available on ESP32/Arduino.  PlatformIO's LDF
# scans source files before pre-build scripts run, so library.json srcFilter
# can't exclude it; instead, overwrite the file with an empty stub so that
# SCons compiles something harmless.  Our firmware never calls cbor_value_to_json*
# so the missing open_memstream symbol creates no linker gap.
open_memstream_path = os.path.join(tinycbor_src, "open_memstream.c")
if os.path.exists(open_memstream_path):
    with open(open_memstream_path, "r") as f:
        existing = f.read()
    if "Cannot implement open_memstream" in existing:
        with open(open_memstream_path, "w") as f:
            f.write("/* Stub: POSIX open_memstream not available on ESP32/Arduino */\n")
        print("Stubbed open_memstream.c for ESP32 compatibility")
