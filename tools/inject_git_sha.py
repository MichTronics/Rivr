"""
tools/inject_git_sha.py

PlatformIO pre-build extra_script.
Replaces the static RIVR_GIT_SHA="unknown" with the actual short git commit hash
at build time. Falls back to "unknown" if git is unavailable (e.g. CI without .git).

Usage in platformio.ini [env] or per-env section:
    extra_scripts = pre:tools/inject_git_sha.py
"""
import subprocess
import os

Import("env")  # type: ignore  # noqa: F821 – injected by PlatformIO SCons


def get_git_sha():
    try:
        result = subprocess.run(
            ["git", "rev-parse", "--short=8", "HEAD"],
            capture_output=True,
            text=True,
            cwd=env.subst("$PROJECT_DIR"),  # type: ignore
            timeout=5,
        )
        sha = result.stdout.strip()
        return sha if sha else "unknown"
    except Exception:
        return "unknown"


sha = get_git_sha()
print(f"[inject_git_sha] RIVR_GIT_SHA = {sha}")

# The static fallback "-DRIVR_GIT_SHA=\"unknown\"" has been removed from
# platformio.ini build_flags, so there is no existing definition to remove.
# Simply append the real (or fallback) hash — one define, no duplication.
env.Append(CPPDEFINES=[("RIVR_GIT_SHA", f'\\"{sha}\\"')])
