# Genset Control

This project allows you to connect a Victron CerboGX or any other device with a static generator run state to your power generator.
In my case, I use it to start and stop a Cummins Onan MDKDR in my Boat.

## Features

- Connect to your WiFi network an control your Genset from your Mobile Phone using a Browser

## Prerequisites

- ESP32 Relais 2 Channel Modul [LC-Relay-ESP32-2R-D5](https://www.amazon.de/dp/B0CYSMFB49) from [OEM](http://www.chinalctech.com/cpzx/Programmer/Relay_Module/518.html)
- Generator to control using dedicated START and STOP relays
- For Cummins Onan MDKDR connection a [Deutsch 8-pin connector](https://www.amazon.de/dp/B0CQR1GXSV)
- Resistor and 3.3V Zener diode to limit the generator running signal

## Required connections

- Connect the START signal from the CerboGX Relay NO (Normally Open) to GPIO PIN 26.
- Connect the STOP signal from the CerboGX Relay NC (Normally Closed) to GPIO PIN 27.
- Power the CerboGX Relay by connecting the 3.3V PIN to the relay's COM (Common) terminal.
- Connect a 3.3V limited running signal (do not use the 12/24V from the generator directly) to GPIO PIN 25. (optional)

### Deutsch 8-pin Cummins Onan Connector

Please verify with your own Generator.

My MDKDR will be connected to:
- Pin 4 to COM of both Relays
- Pin 2 to NO of Relay 2 (closer to the middle)
- Pin 3 to NO of Relay 1 (outer side of the board)
- Pin 6 to GPIO PIN 25 using a 3.3V limiter (example, resistor and zenner diode)

## Web UI

It's not required to use the WebUI, but it certainly adds some value.

![Genset Control Web UI](docs/web-ui.png)

## Contributing

Contributions are welcome! If you find any issues or have suggestions for improvements, please open an issue or submit a pull request.

# License

genset-control (c) by Martin Verges.

This project is licensed under a Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International License.

You should have received a copy of the license along with this work.
If not, see <http://creativecommons.org/licenses/by-nc-sa/4.0/>.

## Commercial Licenses 

If you want to use this software on a comercial product, you can get an commercial license on request.
