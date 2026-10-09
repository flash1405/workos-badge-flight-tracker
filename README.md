# WorkOS Badge Flight Tracker

A live radar of the planes overhead, for the WorkOS init() conference badge (M5Stack StopWatch, ESP32-S3, 466x466 round AMOLED).

## What it does

- Radar sweep centered on your location, with range rings and compass marks
- Live aircraft from free ADS-B feeds, refreshed every 10 seconds and moved smoothly between updates
- Each plane points where it is flying and leaves a short trail
- Color shows altitude: orange low, then yellow, green, cyan, purple high. Grey is on the ground
- Tap a plane to see callsign, type, registration, altitude, speed, heading, distance and bearing
- Buzzes when a plane enters the inner ring, flashes red on emergency squawks (7500, 7600, 7700)

## Controls

| Input | Action |
| --- | --- |
| A (yellow) | Change range: 5, 10, 25, 50, 100 nm |
| B (blue) | Select the next nearest plane |
| Tap | Select a plane, tap empty space to clear |

## Setup

Requires macOS with Homebrew. On Linux, install `arduino-cli` yourself and the script does the rest.

```sh
git clone https://github.com/flash1405/workos-badge-flight-tracker
cd workos-badge-flight-tracker
./scripts/setup.sh
```

Edit `flight_radar/secrets.h` with your Wi-Fi name and password. The badge only supports 2.4 GHz networks. A phone hotspot works.

## Flash

Plug the badge in with a USB-C data cable, then:

```sh
./scripts/flash.sh
```

If the upload cannot find the badge, hold PWR for about 2 seconds until the green LED turns on and try again. Pass a port to pick one: `./scripts/flash.sh /dev/cu.usbmodem2101`.

## Location

On boot the badge scans nearby Wi-Fi networks and asks [BeaconDB](https://beacondb.net) where it is. If that fails it falls back to an IP lookup, which can be off by many miles. The bottom of the screen shows which one was used.

To pin an exact spot, add these to `secrets.h`:

```c
#define HOME_LAT 37.7749
#define HOME_LON -122.4194
```

## Data sources

- [adsb.lol](https://adsb.lol) and [adsb.fi](https://adsb.fi) for aircraft. The badge switches to the other one if a feed rate limits it
- [BeaconDB](https://beacondb.net) and [ip-api.com](https://ip-api.com) for location

All are free and need no API key. Please keep the refresh interval polite.

## Restoring the conference firmware

Open [workos.com/init/badge/install](https://workos.com/init/badge/install) in desktop Chrome or Edge. This project keeps the badge's original partition layout, so the installer works without any extra steps.

## License

MIT
