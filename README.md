# ESPSolarMeteo

## License

MIT License

## General description

ESPSolarMeteo is a (yet another) ESP8266-based meteostation.
The design goal is to build station that can work unattended without
external power. It is powered by LiPo battery that is recharged by a small solar panel.

The working cycle is optimized for low power consumption. The station awakes
from deep sleep at 10 (or more) minutes, measures environmental conditions,
sends data to MQTT server and goes to sleep again.

### Extra circuitry

I have added some external circuitry: 
- Reset button
- 10K/40K voltage divisor for VCC self-measurement 
- MCP100 3.15V power supervisory circuit connected to CH_PD pin.

### Data publishing

Measured data (temperature, humidity and self-monitoring data) is send to
NarodMon (http://www.narodmon.ru) using MQTT or TCP protocol.

### Indication

Single LED is used for indication. At measurement cycle it turns on,
after finishing turns off, then makes 1 to 4 slow
blinks - 1 for successful measurement, 2 for successful WiFi connection,
3 or 4 for successful sending data to server (3-test, 4-narodmon).

### VCC self-measurement

For diagnostics purposes, device monitors its own power voltage.
Voltage is measured using internal ADC, input voltage is
converted to ESP-capable range using 40K/10K divisor. 

