#!/usr/bin/env python3
"""launch.py - configure, build, and run Varlera.

Usage:
    python launch.py              # build (Release) and run
    python launch.py debug        # build Debug (Vulkan validation layers) and run
    python launch.py --build      # build only, don't run
    python launch.py --rebuild    # wipe build dir and reconfigure
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(ROOT, "build")
VCPKG_TOOLCHAIN = "D:/windsurf/vcpkg/scripts/buildsystems/vcpkg.cmake"


def run(cmd):
    print("+", " ".join(cmd))
    return subprocess.call(cmd, cwd=ROOT)


def main():
    args = sys.argv[1:]
    cfg = "Debug" if "debug" in args or "--debug" in args else "Release"
    build_only = "--build" in args
    rebuild = "--rebuild" in args

    if rebuild and os.path.isdir(BUILD):
        import shutil
        shutil.rmtree(BUILD)

    need_configure = not os.path.exists(os.path.join(BUILD, "CMakeCache.txt"))
    if need_configure or rebuild:
        rc = run(["cmake", "-B", "build", "-S", ".",
                  f"-DCMAKE_TOOLCHAIN_FILE={VCPKG_TOOLCHAIN}"])
        if rc:
            return rc

    rc = run(["cmake", "--build", "build", "--config", cfg])
    if rc:
        return rc
    if build_only:
        return 0

    exe = os.path.join(BUILD, cfg, "varlera.exe")
    if not os.path.exists(exe):
        print("missing exe:", exe)
        return 1
    return subprocess.call([exe], cwd=os.path.dirname(exe))


if __name__ == "__main__":
    sys.exit(main())
