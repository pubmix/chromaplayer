# ChromaPlayer

Experimental music-player firmware for the ModRetro Chromatic, maintained by pubmix. This snapshot contains the ESP32 MCU application and the matching FPGA audio bridge.

## Current status

- Internet Radio: Groove Salad, Drone Zone, and GS Classic tested with live streams, station switching, buffered I2S output, and automatic Wi-Fi reconnection.
- Digital audio: verified through raw USB capture. A 1 kHz test tone measured correctly without spikes. Generated MP3 fixtures verified stereo 48 kHz and mono 22.05 kHz playback with a sample-rate change.
- SD access: **blocked at D0 on the test console**. The card accepts native sector-read commands. A from-scratch GPIO implementation receives checksum-valid sector data on D1/D2/D3 in four-bit mode, while D0 stays high and fails CRC. The same result occurs using the independent RTC input path for GPIO2. Native one-bit mounting therefore times out before filesystem access. Reseating did not change the result; socket DAT0 pin 7, GPIO2, and pull-up R96 need physical inspection.
- Files UI: hierarchical browsing of indexed directories, A to open folders/play MP3s, B to go up or stop/back, and Start to rescan. Filenames/paths up to 255 bytes fit the index. The on-device navigation self-test passed 13 checks. Actual card browsing/playback remains unverified because of the D0 failure. No rename, copy, move, delete, or format operations are implemented.
- Bluetooth: disabled in this build because of ESP32 RAM constraints.
- Speaker: user confirmed clear radio music and that the station-switch glitch is completely gone after serialized DMA transitions and short fades. Headphones have not been confirmed.

This is a development snapshot, not a completed or production-ready release. The FPGA image was programmed and verified in external flash and successfully reloaded from flash with usercode `43505231` (version 19.1). The MCU application is persistent. Normal playback checks the fresh FPGA version and blocks/mutes incompatible images. A user-performed power cycle retained FPGA 19.1. Automatic idle sleep is currently disabled so Wi-Fi and I2S remain active; battery idle optimization is unfinished. FPGA timing reports include memory-reset recovery violations that still need review.

## Layout

- `mcu/`: ESP-IDF application, ChromaPlayer UI, network/radio support, SD scanner, MP3 decoder, and generated diagnostic fixture.
- `fpga/`: Gowin project, I2S receiver, stereo clock-domain transfer, audio routing, mono-mix correction, and USB diagnostics.
- `UPSTREAM.json`: exact upstream repositories and base commits. Original copyright and license notices are retained.

## Checkout

```sh
git clone --recurse-submodules https://github.com/pubmix/chromaplayer.git
cd chromaplayer
```

For an existing checkout, run `git submodule update --init --recursive` at the repository root.

## MCU build

Use ESP-IDF v5.3 with its environment activated:

```sh
cd mcu
idf.py set-target esp32
idf.py build
```

The custom partition layout is in `partitions.csv`. Consult `mcu/README.md` for the upstream toolchain setup. Wi-Fi credentials are entered on the console and stored in device NVS; no credentials or NVS dumps are included here.

## FPGA build

The ChromaPlayer snapshot was built with Gowin V1.9.12.03, targeting GW5A-EV25UG256CC1/I0 (version A). This differs from the older toolchain recommendation in the upstream FPGA README.

```sh
cd fpga/esp32t
gw_sh build_chromaplayer.tcl
```

Generated files go to `impl/pnr/`. The script uses area optimization and placement/routing option 2. FPGA synthesis and place-and-route need the Gowin installation and its license. No vendor license files or toolchains are distributed here.

## Development commands

The MCU serial console uses 115200 baud:

```text
fwversion
music_diag status
music_diag play 0
music_diag stop
music_diag sd
music_diag filetest
music_diag browsertest
music_diag sdnative
music_diag browsertest
music_diag sdnative
```

Station indices are 0 (Groove Salad), 1 (Drone Zone), and 2 (GS Classic). `filetest` uses generated MP3 sine fixtures embedded in firmware and temporarily mutes the codec; it does not access the SD card. SD scans do not format the card. `music_diag wifidrop` deliberately disconnects only the console Wi-Fi to exercise reconnection.

Windows voice-enhancement processing suppressed the console audio during ordinary DirectShow capture. Verification used the console's raw/exclusive WASAPI endpoint. Check host capture processing before diagnosing silence as a firmware failure.

## Licensing and attribution

The MCU and FPGA projects derive from ModRetro's open-source Chromatic repositories and retain their GPL-3.0 license texts and notices. The FPGA references the pinned Gameboy_MiSTer submodule. The bundled minimp3 headers retain their CC0/public-domain dedication. Generated diagnostic tones contain no downloaded music. See component license notices for details.

`browsertest` checks folder filtering and parent navigation without creating synthetic card files. `sdnative` is a read-only, slow GPIO diagnostic for SCR and sector zero, including one-bit, four-bit, and alternate RTC-input checks. It reports per-lane CRCs and restores one-bit bus mode. Stop playback and unmount/rescan before electrical diagnostics; it refuses to run while the normal SD filesystem is mounted.
