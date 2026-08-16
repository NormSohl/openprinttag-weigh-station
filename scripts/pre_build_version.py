Import("env")
import subprocess

# Embed "which commit is this?" into the firmware itself, so the boot log and
# /api/status can answer that without cross-referencing a build timestamp
# against git log by hand -- the same problem STACK's task table and the
# clean-build gotcha in CLAUDE.md both exist to catch, one level up.
#
# --always falls back to the abbreviated commit hash when no tag is reachable;
# --dirty appends "-dirty" when the working tree has uncommitted changes at
# build time, so a flashed-but-not-committed build never claims to be a tagged
# milestone it isn't.
try:
    version = subprocess.check_output(
        ["git", "describe", "--tags", "--always", "--dirty"],
        cwd=env.subst("$PROJECT_DIR"),
        stderr=subprocess.DEVNULL,
    ).decode("utf-8").strip()
except Exception:
    version = "unknown"

env.Append(BUILD_FLAGS=['-DBUILD_VERSION=\\"%s\\"' % version])
print("Build version: %s" % version)
