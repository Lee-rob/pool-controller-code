#!/usr/bin/env python3
"""Select a traceable ESP-IDF version for local builds and GitHub Actions."""
import os
import re
import subprocess

CUSTOM_BRANCH = "viron71-controls-v1.17.0"
CUSTOM_PREFIX = "v1.17.0-viron71-c13-g"


def git(*args):
    return subprocess.check_output(["git", *args], text=True,
                                   stderr=subprocess.DEVNULL).strip()


def select_version():
    ref_type = os.environ.get("GITHUB_REF_TYPE", "")
    ref_name = os.environ.get("GITHUB_REF_NAME", "")
    if not ref_name:
        try:
            ref_name = git("branch", "--show-current")
        except (OSError, subprocess.CalledProcessError):
            pass
    custom = ref_type != "tag" and ref_name == CUSTOM_BRANCH
    if ref_type == "tag":
        version = ref_name
    elif custom:
        sha = os.environ.get("GITHUB_SHA") or git("rev-parse", "HEAD")
        if not re.fullmatch(r"[0-9a-fA-F]{40}", sha):
            raise ValueError("Custom build requires a full Git commit SHA")
        version = CUSTOM_PREFIX + sha[:7].lower()
        try:
            if "GITHUB_ACTIONS" not in os.environ and git("status", "--porcelain", "--untracked-files=no"):
                version += "-d"
        except (OSError, subprocess.CalledProcessError):
            pass
    else:
        try:
            version = git("describe", "--always", "--dirty", "--tags")
        except (OSError, subprocess.CalledProcessError):
            version = "unknown"
    # esp_app_desc_t.version has 32 bytes, including its terminating NUL.
    if not re.fullmatch(r"[A-Za-z0-9_.+-]{1,31}", version):
        raise ValueError("Firmware version must be 1-31 filename-safe ASCII characters")
    return version, custom


if __name__ == "__main__":
    version, custom = select_version()
    output = os.environ.get("GITHUB_OUTPUT")
    if output:
        with open(output, "a", encoding="utf-8") as stream:
            stream.write(f"version={version}\ncustom={str(custom).lower()}\n")
    print(version)
