# Custom firmware builds

The custom branch is `viron71-controls-v1.17.0` in
`Lee-rob/pool-controller-code`. It retains the custom valves, coordinated Spa
Jets modes and deadhead safeguards, solar output decoding, heater diagnostics,
bus changes, power tracking and graphical status page.

## GitHub builds and downloads

Pushes and manual **Build & Release** runs on this branch now:

1. Build with ESP-IDF v5.5 for ESP32-C6 using the existing workflow.
2. Set a traceable version such as `v1.17.0-viron71-c13-gabcdef0`. The shortened
   `c13` means controls13; the suffix identifies the exact source commit. This
   fits the firmware's 31-character version limit. Change `CUSTOM_PREFIX` in
   `scripts/firmware_version.py` when advancing the custom version series.
3. Upload the OTA application, initial full-flash image, matching ELF/map debug
   ZIP and SHA-256 checksums as workflow artifacts.
4. Publish the same files under that version in the fork's **Releases**, marked
   as a prerelease. Pull requests do not publish releases. A draft is published
   only after all matching files have uploaded successfully.

Previously branch builds could compile successfully but only created an Actions
artifact, named with `git describe` or a bare commit hash. They never published
a release. The custom firmware also still looked at the upstream author's
repository for updates. Both parts are corrected here: this branch checks
`Lee-rob/pool-controller-code`, whose release feed includes the custom builds.

Download an `update` application image from
<https://github.com/Lee-rob/pool-controller-code/releases> and upload it through
the controller's `/update` page for the first upgrade from older custom firmware.
Later custom firmware can discover this fork's releases through its GitHub
update check. Publishing a release never installs it automatically.

Use the `update` image for existing controllers. The `full` image is for initial
provisioning on a fresh board, not an update preserving saved settings. USB
recovery that must preserve NVS still uses the application and targeted OTA-slot
selection procedure in [controls12b.md](controls12b.md).

## Local builds

The existing command and persistent toolchain cache remain supported:

```sh
./scripts/build_firmware.sh
```

The script selects the same custom version from the checked-out branch and
commit. Modified tracked source adds `-d`. An explicit `PROJECT_VER=...` still
overrides automatic selection, including when building a source archive without
Git metadata. The compiler, target, partitions and saved device settings have
not been changed by this build-process fix.
