# ChromaPlayer

Experimental music-player firmware for the ModRetro Chromatic, maintained by pubmix. This snapshot contains the ESP32 MCU application and the matching FPGA audio bridge.

## Current status

- Internet Radio: Groove Salad, Drone Zone, and GS Classic tested with live streams, station switching, buffered I2S output, and automatic Wi-Fi reconnection.
- Digital audio: verified through raw USB capture. A 1 kHz test tone measured correctly without spikes. Generated MP3 fixtures verified stereo 48 kHz and mono 22.05 kHz playback with a sample-rate change.
- SD access: **unresolved on the test console**. The installed card responds to native commands including identification and selection, but its first D0 data transfer (ACMD51/SCR) times out. Reseating the card did not change this result; a separate slow native GPIO test also received valid commands but no data bits. Normal scans stay in native SD mode; SPI is an explicit diagnostic only. SD file playback has not been verified using a real card.
- Files UI: read-only list of scanned files/folder paths and rescan controls. The user confirmed the physical LCD displays the scan error correctly. No interactive folder navigation, rename, copy, move, delete, or folder creation.
- Bluetooth: disabled in this build because of ESP32 RAM constraints.
- Speaker: user confirmed clear radio music. A reported station-switch click prompted serialized DMA transitions and short fades; digital captures pass, speaker confirmation of the transition fix remains pending. Headphones have not been confirmed.

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
```

Station indices are 0 (Groove Salad), 1 (Drone Zone), and 2 (GS Classic). `filetest` uses generated MP3 sine fixtures embedded in firmware and temporarily mutes the codec; it does not access the SD card. SD scans do not format the card. `music_diag wifidrop` deliberately disconnects only the console Wi-Fi to exercise reconnection.

Windows voice-enhancement processing suppressed the console audio during ordinary DirectShow capture. Verification used the console's raw/exclusive WASAPI endpoint. Check host capture processing before diagnosing silence as a firmware failure.

## Licensing and attribution

The MCU and FPGA projects derive from ModRetro's open-source Chromatic repositories and retain their GPL-3.0 license texts and notices. The FPGA references the pinned Gameboy_MiSTer submodule. The bundled minimp3 headers retain their CC0/public-domain dedication. Generated diagnostic tones contain no downloaded music. See component license notices for details.
