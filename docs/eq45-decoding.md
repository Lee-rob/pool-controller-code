# EQ45 status decoding: controls12

**The original controls12 binary is withdrawn because its heater-freshness
task can overflow its stack. Use controls12a or later.** See
[controls12a](controls12a.md) for the fix and validation. The diagnostic
protocol limitations below still apply.

This is a passive diagnostic release, not a claimed G4 fix. It builds on
controls11 and retains the complete custom valve / Spa Jets controls,
deadhead safeguards, heater support, power tracking, solar activity fix,
graphical status/timer page and bus arbitration changes. It adds no bus
requests, writes, automatic resets or chlorinator controls.

## Interpretation limits

CMD `0x1D` carries chemistry targets; CMD `0x1F` carries measurements.
Firmware announcements and target broadcasts do not establish chlorine
production or successful two-way communication with the touchscreen.
Network capture gaps can also contribute to apparent silence. Site-specific
recordings and findings are intentionally excluded from this public document.

## New diagnostics

`/status` now includes:

- Independent pH/ORP target validity, so a received target can be shown even
  when no chemistry measurement has arrived.
- `ph_reading_age_ms` and `orp_reading_age_ms`. Repeated target replies do
  not refresh these ages. These are ages since the gateway received a
  measurement, not necessarily when the probe physically sampled the water.
- `chlorinator.bus_diagnostics`: last reply age from `0x0084`, longest
  observed gap, and eight bounded raw-message samples. Each entry retains
  command, destination, payload prefix, count, change count and previous
  changed payload. Chemistry and register slots have separate keys.
- Long payloads are explicitly marked truncated; complete debug frames are
  still needed. Full-payload hashes detect changes beyond the saved prefix.
  `sample_evictions` reports when older message keys were displaced. Samples
  are in RAM and reset when the gateway restarts.
- `production_active`, `g4_fault` and `manual_off` are **null**, meaning
  unverified. They are not inferred from the pump, selected mode, chemistry
  targets or the mere presence/absence of replies. No HA fault/production
  entity is published until a mapping is established.

The graphical status page shows measurement/reply ages and explicitly says
the production/fault states are not yet decoded. Existing tentative CMD
`0x12` mode values remain internal and are logged as tentative raw values.

## Capture the missing states

`scripts/Capture-Chlorinator.ps1` works with **controls11 as well as
controls12**. Installing controls12 is optional for collecting the full raw
bus recording; it adds the persistent-in-RAM summaries described above.

1. Close PuTTY and other debug/capture sessions. Port 7373 accepts one client.
2. Put the script in Downloads. Open a normal Windows PowerShell console.
3. Run:

```powershell
cd "$HOME\Downloads"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Capture-Chlorinator.ps1 -Controller "YOUR_CONTROLLER_IP" -Seconds 900 -ObservedStatus "Auto; no G4 currently"
```

4. Keep this console selected. Press a digit when you observe the matching
   state; no Enter is needed. The script records the PC time with timezone,
   avoiding the Connect 10 daylight-saving clock offset.

| Key | Observation |
| --- | --- |
| 0 | Normal / Auto shown |
| 1 | Manual Off shown |
| 2 | G4 visible |
| 3 | G4 cleared / communication restored |
| 4 | Cell production confirmed OFF |
| 5 | Cell production confirmed ON |
| 6 | Filter pump physically stopped |
| 7 | Filter pump physically running |
| 8 | Operator initiated a reset/restart |
| 9 | Touchscreen reports chlorinator disconnected |

Only mark cell production if the chlorinator display or direct evidence
confirms it. Pump operation alone does not establish chlorine production.
The script changes no equipment settings. There is no need to deliberately
induce G4. Let normal operation provide the states. If you normally change
Auto/Off locally, mark the displayed result and capture about a minute on
each side; operating mode and cell production remain separate observations.

5. Send back the `PoolChlorinator-….zip` file saved on the Desktop. If G4
   occurs, mark it before performing your usual recovery, and mark that
   recovery too. We need the before/fault/after sequence, not just a snapshot
   taken after a reset. If the fault does not occur, a healthy baseline still
   helps compare traffic after the cable replacement.

The collector warns if it connects but receives no raw frames. It also saves
HTTP snapshots and unknown messages. In hosts without console-key support,
note PC times separately. Windows execution needs to be verified on the
user's machine; host tests cover the firmware and offline analyzer.

## Existing Bluetooth reference

The independent [pychlorinator parser](https://github.com/pbutterworth/pychlorinator/blob/main/pychlorinator/chlorinator_parsers.py)
already identifies `InfoMessages.G4CommsFailure = 7` and the
`StateFlags.CellIsOperating = 0x20` flag in its **Bluetooth state
characteristic**. Its mode enum is Off=0, ManualOn=1, Auto=2, which also shows
why borrowing the tentative bus mode enum would be unsafe.

These are useful reference meanings, not verified Connect 10 bus offsets.
Bluetooth support/access for this particular unit has not been established.
If available, a simultaneous Bluetooth reading can provide better ground
truth than manually marking the display. Do not transplant those flags into
CMD `0x12` or unknown register data without matched evidence.

Upstream [issue 79](https://github.com/marklynch/pool-controller-code/issues/79)
describes undocumented `0xE9`/`0xEA`, slot `0x0B`, chlorinator register replies.
Their countdown/config interpretations remain unverified for fault/production
status. This release records any such replies passively; it does not poll them.

## Reproduce the analysis and build

```bash
./test/run_tests.sh
python3 -m unittest discover -s test -p test_chlorinator_capture.py -v
python3 scripts/analyze_chlorinator.py capture.zip --output analysis.json
PROJECT_VER=v1.17.0-viron71-controls12 ./scripts/build_firmware.sh
```

Replace `YOUR_CONTROLLER_IP` with the local controller address. The analyzer
validates frame boundaries, lengths and checksums, extracts known
chemistry/firmware values, reports reply gaps, and leaves unknown states null.
It makes no network connections.

Validation: six host suites passed (530 checks: 14 bus, 18 pump type,
258 decoder, 115 commands/valves, 52 framing, 73 replay), plus four offline
capture tests. ESP32-C6 production firmware built successfully using the
existing persistent ESP-IDF v5.5 environment; no hardware runtime verification
has yet been performed on controls12.
