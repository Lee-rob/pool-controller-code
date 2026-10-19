# Controls12a: correct controls12 task stack overflow

Firmware version: `v1.17.0-viron71-controls12a`, ESP32-C6, ESP-IDF v5.5.
This supersedes the original controls12 binary. It retains the controls12
chlorinator diagnostics and all existing custom valve/Spa Jets controls,
deadhead safeguards, heater support, solar fix, power tracking and status UI.
Chlorine production, G4 and Manual Off bus meanings remain unverified.

## Changes

Controls12 grew `pool_state_t` to 3200 bytes. The heater-freshness task placed
a whole copy on its 3072-byte stack. The ESP32-C6 compiler reports a 3232-byte
local frame for the old function, exceeding that task's entire allocation
before any callees or scheduler overhead.

The worker now receives a heap-allocated snapshot owned for its lifetime.
Allocation failure is handled before creating the worker; task-creation
failure frees the snapshot. The state is copied only when an expiry needs
publishing, with publication after releasing the state mutex.

The full state copy used for favourite discovery in the MQTT callback also
moves to checked heap storage. Its zero-initialised fallback when no snapshot
can be taken is retained. Successful allocation is always paired with free.

Target compilation now records stack usage. Local frames over 1024 bytes in
the heater module, or 2048 bytes in MQTT discovery, fail compilation. These
limits catch a future reintroduction of a full state object in those modules.

## Validation

- Seven C test suites passed: all six existing suites, including valve safety
  and solar decoding checks, plus seven new heater-freshness scenarios.
- Four offline chlorinator capture tests passed.
- Heater scenarios cover a state object larger than its task stack, allocation
  failure, task-creation failure, mutex timeout, one-shot expiry, the exact
  timeout boundary, no timestamp, and uptime wrap. Assertions verify that the
  MQTT publisher receives the heap snapshot after the mutex is released.
- AddressSanitizer/UndefinedBehaviorSanitizer found no errors in those heater
  tests. LeakSanitizer is disabled because this execution sandbox cannot
  inspect process threads; allocation/free failure paths are checked by tests.
- The target-compiler guard rejects the original source and accepts the fix.
- Production ESP32-C6 build passed with image checksum/hash verification.

| Function | Original local frame | Controls12a local frame | Task allocation |
| --- | ---: | ---: | ---: |
| Heater freshness | 3232 | 48 | 3072 |
| MQTT discovery | 3632 | 432 | 6144, shared MQTT task |
| Web status handler | 3520 | 3520 | 8192 |
| Channel energy | 3328 | 3328 | 8192 |
| TCP bridge | 2096 | 2096 | 8192 |

Decoder handlers containing full state snapshots run from the 8192-byte bus
task; the largest recorded handler frame is 3344 bytes. Channel-target and
spa-transition workers have 48- and 32-byte frames on 3072-byte stacks and do
not copy the complete state. Register requester has a 192-byte frame on 4096.

These figures describe local frames. They exclude nested calls, interrupts
and scheduler overhead, and are not runtime high-water measurements. Physical
runtime verification is still required after USB installation: confirm the
version, uninterrupted uptime, repeated HTTP status responses and MQTT
connectivity before reconnecting equipment and resuming capture work.

## USB loading without erasing saved settings

The OTA application binary can be written through USB at `0x20000` for this
custom build. Back up settings first. After successful application writing,
erase only the `otadata` region at `0x10000`, size `0x2000`, so the bootloader
selects the restored first application slot. These addresses match the
included `partitions.csv`; they are not generic ESP32 flashing addresses.

Do not use whole-chip erase or a merged FULL-USB image for this procedure.
NVS at `0x9000` through `0xEFFF` remains untouched. The package contains the
existing `ota_data_initial.bin` as `reset-ota-selection.bin`; it is exactly
8192 bytes of `0xFF` and is an alternative to the targeted erase command.

Use the supplied Windows instructions and replace the example COM port
with the connected gateway's port. If any step fails, stop before the next
write. Close serial terminals before flashing. Keep the gateway's usual
power disconnected while powering through USB.
