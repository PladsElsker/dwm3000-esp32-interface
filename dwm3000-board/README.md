# DWM3000 Interface Board

KiCad hardware design for the DWM3000 and ESP32 interface project.

## Project files

- `dwm3kb.kicad_pro` — KiCad project
- `dwm3kb.kicad_sch` — schematic
- `dwm3kb.kicad_pcb` — PCB layout
- `dwm3000/` — local DWM3000 library assets
- Gerber, drill and job files — fabrication outputs from a design revision

The schematic title block identifies revision 0.2. Check the current schematic and PCB revision before using manufacturing files; generated outputs can become stale after design changes.

## Open and develop

1. Open `dwm3kb.kicad_pro` in KiCad.
2. Keep the local `dwm3000/` library assets available when opening or editing the design.
3. Review the schematic and run Electrical Rules Checker (ERC).
4. Review the PCB, refill copper zones and run Design Rules Checker (DRC).
5. After any layout change, regenerate Gerber and drill files from the current PCB.
6. Review the generated outputs and assembly requirements before sending them to a fabricator.

## Hardware integration

Check the firmware [pin map](../dwm3000-firmware/README.md#hardware-and-pin-map) against the actual schematic and assembled board. Pay particular attention to ESP32-C3 strapping pins, 3.3 V logic, power integrity, reset and wake signals, and the DWM3000 antenna/RF layout.

## Before fabrication

- Confirm schematic and PCB revisions match.
- Resolve or explicitly review ERC and DRC findings.
- Verify footprints, component values, connector orientation and assembly constraints.
- Regenerate fabrication outputs from the reviewed PCB.
- Review stack-up, board-house capabilities and RF layout requirements.

The presence of KiCad files and fabrication outputs does not imply electrical, RF, assembly or manufacturing qualification. Validate the assembled hardware before deployment.
