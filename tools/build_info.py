# PlatformIO extra_script: stamp the build with the short git commit hash.
#
# Generates <build_dir>/build_sha.h defining BUILD_SHA as a C string (e.g.
# "7e20174"), which include/about.h appends to VERSION so the about page
# shows exactly which commit the running firmware was built from. In CI the
# same value is used for the artifact names (see
# .github/workflows/build.yml), so the number on the OLED matches the
# artifact you flashed.
#
# (A command-line -D define does not work: SCons strips the inner quotes
# from string-valued CPPDEFINES, leaving a bare number on the command line.)
import os
import subprocess

from SCons.Script import Import

Import("env")


def _build_sha():
    try:
        return (
            subprocess.check_output(
                ["git", "rev-parse", "--short=7", "HEAD"],
                stderr=subprocess.DEVNULL,
            )
            .decode()
            .strip()
        )
    except Exception:
        return "dev"


sha = _build_sha()
build_dir = env.subst("$BUILD_DIR")
os.makedirs(build_dir, exist_ok=True)
header = os.path.join(build_dir, "build_sha.h")
with open(header, "w") as f:
    f.write("#pragma once\n#define BUILD_SHA \"%s\"\n" % sha)

print("build_info: BUILD_SHA=%s -> %s" % (sha, header))
env.Append(CPPPATH=[build_dir])
