# Unattended chlorinator evidence capture

`scripts/Watch-Chlorinator.py` runs on a PC with Python 3.10 or later. It uses
only Python's standard library and works with controls12a. No firmware update,
extra packages, button presses or equipment commands are needed.

## Start on Windows

Download `Watch-Chlorinator.py` into Downloads, close PuTTY and any other debug
or capture clients, then run this in PowerShell. Replace the placeholder with
the controller's current IP address:

```powershell
cd "$HOME\Downloads"
py .\Watch-Chlorinator.py --controller YOUR_CONTROLLER_IP --hours 24
```

Look for **HTTP status confirmed** and **Raw bus frames confirmed**. Both are
needed for a useful combined capture. The program retries connection failures
automatically and records them separately from chlorinator events. If no raw
frames arrive, close other users of debug port 7373 and check gateway access.

Leave the window running; minimising it is fine. On Windows the watcher
temporarily requests that idle sleep be inhibited, restoring the thread's
prior setting on exit. It allows the display to turn off. Keep a laptop plugged
in with its lid open: this does not override deliberate sleep, lid actions,
shutdowns or connectivity loss. Other operating systems require manual sleep
configuration. Press Ctrl+C for an orderly stop and to finish an active capture.

## Default triggers

| Observation | Default | Meaning |
| --- | --- | --- |
| No recent ORP measurement message | 5 minutes | Reading age exceeds the threshold, or no reading has appeared after reported filtration started |
| Identical ORP value | 15 minutes | Value has remained exactly the same in successful HTTP observations, even if fresh messages continue |
| Gateway HTTP status unavailable | 90 seconds | Connectivity evidence; explicitly not labelled G4 |
| Gateway uptime decreases | Immediate | A restart event, allowing for normal 32-bit uptime rollover |

The chemistry triggers arm after three minutes of the Filter channel being
reported active. They remain armed for 30 minutes after the last reported
activity, so a fault that stops the pump does not immediately disable capture.
Long inactive overnight periods do not trigger chemistry incidents. A normal
scheduled stop can still produce a low-confidence capture during that recent
activity window. If a fault prevents filtration ever starting, this activity
gate may miss its chemistry symptom; connectivity/restart recording and the
rolling logs continue.

These triggers are **suspected events, not confirmed G4 detections**. An ORP
number can legitimately stay stable, and the normal frequency of unchanged
ORP broadcasts is not yet established for this device. Reading age is time
since the gateway received an ORP message, not necessarily the age of the
physical probe sample. The capture preserves evidence to investigate those
distinctions; it does not infer cell production or reset equipment.

## What gets saved

The output folder is `PoolChlorinatorWatch-…` on the Windows Desktop, including
when Desktop is redirected to OneDrive. Use `--output PATH` for another empty
folder. Each suspected event automatically saves an `incident-XX.zip` with:

- 30 minutes of preceding traffic, when available, and 10 minutes afterwards.
- Raw timestamped bus/debug lines, full HTTP status snapshots and connection
  errors, counters, modes, device ages and pump state from those snapshots.
- `incident.json` with automatic trigger reasons, uncertainty and truncation
  flags. Automatic triggers are not presented as manual display observations.

Timestamps are PC receipt times with timezone, not the Connect 10's clock.
Events during an active capture are merged. A continuous frozen episode
generates one trigger per cause, with a 30-minute cooldown between captures.
Six incident ZIPs are retained per run; existing incident ZIPs are not deleted.
After that, bounded rolling recording continues. If nothing triggers, an
orderly exit saves the latest baseline instead. Start a new run/empty folder
to obtain a new capture allowance.

Two rolling files are limited to approximately 16 MiB each. Each incident has
a 16 MiB raw-record limit. A busy bus may reduce the available prehistory or
end the post-window early; these limits are marked in `incident.json`. Typical
pool traffic is much smaller, but the requested pre-window is not guaranteed
before the program has run that long. Do not delete an active incident folder.
If the window is forcibly closed, flushed rolling/incident records remain;
final ZIP creation requires an orderly finish.

Check the folder later and send back the incident ZIPs. There is no need to
watch the display waiting for a fault. If you happen to see G4, a photo and PC
time can help establish which automatic capture corresponds to a confirmed
display fault.

The watcher only makes HTTP GETs and reads the TCP debug stream. It does not
change the gateway's existing bus behaviour or stop its normal transmissions.
It sends no Home Assistant commands or external notifications.

## Tuning

```powershell
py .\Watch-Chlorinator.py --controller YOUR_CONTROLLER_IP --hours 48 --flat-minutes 20 --stale-minutes 5
```

Other options: `--pre-minutes`, `--post-minutes`, `--warmup-minutes`,
`--poll-seconds` (5–30), `--debug-port`. HTTP polls default to 15 seconds, with
a five-second request timeout. TCP collection runs independently so waiting on
HTTP does not pause bus recording. Proxy settings are bypassed for the LAN
controller. No Python package installation is required.

## Validation

Fourteen automated tests cover frozen values with fresh messages, stale or
missing samples, overnight inactivity, a pump stopping at the fault, uptime
rollover, reboot and network loss. They check bounded recording, preservation
of prehistory/aftermath and trigger merging. Local HTTP/TCP integration tests
exercise connection loss/recovery and run the complete unattended program,
which automatically produces a ZIP with samples before and after a simulated
flatline. Windows sleep-state calls are tested with a mock API. Actual Windows
network/desktop integration still requires confirmation on the user's PC.

```bash
python3 -m unittest discover -s test -p test_chlorinator_watch.py -v
```
