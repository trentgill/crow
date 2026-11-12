# recorded runs

Bench crow, serial 206135773756, 2026-11-12, arm-none-eabi-gcc 13.2, `make R=1`.

- `baseline.json`: the firmware as it was (main plus the calibration fix and
  telemetry, commit "telemetry and a hardware test suite").
- `final.json`: the tip of the service-update branch.

Both were taken with the suite as committed here: `python3 tests/hw/report.py baseline final`.

`stages/` holds the run made as each change went in. The suite was still
growing then, so those files lack some later checks and their first few
boot and upload figures were taken with a 6 s wait after each reset.
`report.py` reads only this directory: copy a stage file up to compare it.

| file | firmware |
| --- | --- |
| baseline-first-run | as baseline.json |
| b1b4 | + voltage scale 1, cortex-m7 compiler target |
| a3 | + Lua libraries run from flash |
| b3 | + stack and statics in DTCM, heap in SRAM1+2 |
| b2 | + data cache enabled for RAM |
| a456 | + fault report, wall-clock watchdog, clock resume cap |
