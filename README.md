# Weather for the ESP32-P4 Launcher

A weather app for the [ESP32-P4 Launcher](https://github.com/giltal/ESP32_P4_Launcher): current
conditions and a 5-day forecast from [Open-Meteo](https://open-meteo.com/) (free for non-commercial use,
no API key, data under CC BY 4.0).

![Screenshot](screenshot.png)

It is also an example of the launcher's proposed **HTTP interface** (HTTPS requests done by the
launcher, [giltal/ESP32_P4_Launcher#13](https://github.com/giltal/ESP32_P4_Launcher/issues/13)):
`PAPP_MF_WIFI` in the manifest keeps Wi-Fi on while the app runs, and `papp_http_get()` fetches the
forecast. On a launcher without that interface the app says so instead of showing the weather.

## Use

- **Place and units:** `/sd/data/chneeb.weather/weather.txt`, written with Munich on the first start.
  Edit it on a PC or with the launcher's Wi-Fi file manager:
  ```
  place=Springfield, Illinois
  units=metric
  ```
  `place` is a city, optionally with a country name, country code or region after a comma. When it
  changes, the app looks it up with Open-Meteo's geocoding search and writes `name`, `lat` and `lon`
  back. Without a `place` line, `lat` and `lon` are used as they are. `units` is `metric` or `imperial`.
- **Controls:** A refreshes; X, START or a tap opens the menu (Refresh, Units, Exit). The app refreshes
  every 15 minutes.
- **Offline:** it shows the last answer (`cache.json` beside the settings) with its age.

## Build

Needs the RISC-V toolchain of ESP-IDF (as for the launcher's own apps) and a launcher checkout with the
HTTP interface:

```
python build.py ../ESP32_P4_Launcher       # or set P4_LAUNCHER
```

The result is `build/weather.papp` and `build/icon.png`. Copy both to `/sd/apps/weather/` on the card,
or install and run it over USB with the launcher's DevLink tool:

```
python ../ESP32_P4_Launcher/tools/papp_dev.py --port <port> deploy build/weather.papp
```

`papp_text.h` is a small header-only text renderer (the launcher's 8x16 font plus °, umlauts and
smooth big digits), kept here so the app needs nothing from the SDK beyond the HTTP interface.

## License

MIT (see LICENSE). Weather data by [Open-Meteo.com](https://open-meteo.com/), CC BY 4.0.
