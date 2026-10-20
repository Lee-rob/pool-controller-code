# Temperature and pump history sampling

## Problem and changes

Publishing temperature sensor 1 copied both temperature values into the MQTT
change-detection cache. Sensor 2 could then be skipped despite never having
been published. Repeated identical temperature and pump readings were also
suppressed, leaving history without regular sample points during steady operation.

Temperature and pump publishers now send each report they receive, including
unchanged values. Temperature sensors and pump RPM advertise Home Assistant
`state_class: measurement` and `force_update: true`, so actual reports can produce
history points even when the numeric state is unchanged. Existing topics, unique
IDs, availability rules and pump power/energy entities are preserved.

Each temperature in a controller report is checked independently. An invalid or
missing roof reading does not publish the cached roof value as a new sample;
a valid roof reading can still publish when the water reading is invalid.
No periodic replay of cached temperatures has been added. Genuine reporting
outages can still cause gaps, and this change cannot reconstruct missing history.
Unchanged source reports will add recorder entries for these sensors.

## Heater verification remains a separate issue

This patch does not change the 180-second heater communications timeout or
claim to resolve a dashboard's "unverified" label. The firmware provides:

- `Heater 1 Communications`, a connectivity binary sensor.
- `Heater 1 Last Sample`, an ISO timestamp sensor.
- MQTT telemetry at `pool/<device_id>/heater/0/telemetry/state`, with `online`
  and `sample_time` fields.

The last-sample timestamp advances on a decoded heater response even when its
reported values are unchanged. MQTT reconnection and command requests do not
advance it. A dashboard should use that timestamp to assess report freshness;
the time an enable switch last changed does not establish communications health.
Requested controls and verified heater measurements have separate meanings.

The decoder also now publishes the communications heartbeat when a genuine
heater temperature report contains an invalid temperature value, consistent with
the existing state update. The invalid temperature itself is not published.

## Validation

Run `bash test/run_tests.sh` and
`python -m unittest discover -s test -p 'test_*.py'`.
The MQTT publisher tests exercise simultaneous temperature changes, repeated
unchanged samples, invalid inputs and disconnected MQTT. Decoder tests exercise
independent validity of both temperatures and missing second samples.
Existing heater freshness, bus arbitration, spa-valve and protocol replay tests
remain enabled. Hardware operation and the external dashboard still require
verification after building and installing firmware.
