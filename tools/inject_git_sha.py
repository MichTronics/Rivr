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

# Remove any existing RIVR_GIT_SHA definition injected by platformio.ini
# build_flags.  CPPDEFINES entries are either plain strings ("RIVR_GIT_SHA=...")
# or 2-tuples (("RIVR_GIT_SHA", value)).  env.Flatten() would destroy tuples,
# so we filter the raw list instead.
def _has_git_sha(entry):
    if isinstance(entry, (list, tuple)):
        return any("RIVR_GIT_SHA" in str(x) for x in entry)
    return "RIVR_GIT_SHA" in str(entry)

env["CPPDEFINES"] = [d for d in env.get("CPPDEFINES", []) if not _has_git_sha(d)]

# Append the real hash (or "unknown") as a properly quoted string define.
env.Append(CPPDEFINES=[("RIVR_GIT_SHA", f'\\"{sha}\\"')])
