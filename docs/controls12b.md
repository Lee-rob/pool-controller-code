# Controls12b: bus-task stack overflow correction

Normal channel broadcasts can trigger Home Assistant discovery from inside the
bus task. That call path retains the bridge's local work buffers and a full
decoder state snapshot while it constructs and formats nested JSON. On
controls12a the combined path exhausted the 8192-byte bridge stack, producing a
stack-protection panic and restart. Checking isolated function frames missed
the sum of the simultaneously live frames.

## Change

- Move the bridge task's UART, client-input, assembled-frame and hex-output
  work buffers to static storage. Only the single bridge task uses them, as
  with the existing framing state. The concurrent logging callback keeps its
  own local buffer.
- Increase the bridge task allocation to 16384 bytes for the nested decoder,
  discovery, JSON, MQTT and logging path.
- Reject bridge-module local frames larger than 768 bytes during target
  compilation. Existing heater/discovery guards remain enabled.
- Sample the bridge's historical minimum unused stack once per second from
  within that task. Expose the atomic result under `/status`:
  `runtime.bridge_stack_min_free_bytes` and `runtime.bridge_stack_size_bytes`.
  The minimum is null until sampled or when the task is stopped. A margin
  below 2048 bytes produces one warning per task lifetime.
- Log the boot reset reason and expose its ESP-IDF numeric value as
  `runtime.reset_reason_code`.

Pool control, bus arbitration, solar decoding, valve sequencing and Spa Jets
behaviour are unchanged. The changes do not force a chlorinator mode or claim
to resolve its G4 fault.

## Validation

ESP32-C6 GCC reports the bridge task's local frame reduced from 2096 to 256
bytes. The new frame-size guard rejects the old source and accepts the fixed
source. All seven C regression suites, including 73 replay frames, pass;
four capture-parser tests and fourteen unattended-watcher tests also pass.
The target image fits the existing OTA partition and its checksum/hash validate.

These are build and host checks, not a hardware soak test. Local frame sizes
exclude callees and context/interrupt overhead; the guard is not a whole-call-
graph proof. After installation, verify uninterrupted uptime for at least ten
minutes of normal bus traffic, stable repeated HTTP access, working HA state
publication, and a stack minimum comfortably above the 2048-byte warning level.
Keep the matching ELF with the release for future crash decoding. The runtime
watermark can only report paths the hardware has actually exercised.

## Upgrade

Use the custom controls12b application image with the existing partition
layout. An OTA update uses the application `.bin` only. For USB recovery, the
release bundle writes an erased 8192-byte OTA-selection image at `0x10000` and
the application at `0x20000`, selecting the first OTA slot. Do not erase flash
or overwrite NVS, the partition table or bootloader. Close serial monitors
before flashing and verify the installed version afterwards.
