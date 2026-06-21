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

# Create a library.json to exclude POSIX-only files that won't compile on ESP32/Arduino.
# open_memstream.c requires funopen/fopencookie (POSIX), cbortojson.c and
# cborpretty_stdio.c require FILE* stdio — none available on bare-metal ESP32.
tinycbor_root = os.path.join(libdeps_dir, env_name, "tinycbor")
lib_json_path = os.path.join(tinycbor_root, "library.json")
if not os.path.exists(lib_json_path):
    with open(lib_json_path, "w") as f:
        f.write("""{
  "name": "tinycbor",
  "version": "0.6.0",
  "build": {
    "srcDir": "src",
    "srcFilter": ["+<*.c>", "-<open_memstream.c>", "-<cbortojson.c>", "-<cborpretty_stdio.c>"]
  }
}
""")
    print("Created tinycbor library.json")
