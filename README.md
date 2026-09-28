# Triax ESP-NOW Hub Demo

Minimal ESP-IDF hub firmware for polling up to six pump nodes and forwarding
their telemetry to a PC over the USB-UART serial connection.

## Requirements and wiring

- ESP32 development board supported by ESP-IDF 5.x/6.x.
- Connect the board to the computer using its USB-UART interface.
- Serial settings: 115200 baud, 8 data bits, no parity, 1 stop bit.
- Pump and hub must use ESP-NOW channel 1. Configure each pump with the hub's
  Station MAC (shown by `list`); configure the hub with each pump's Station MAC.

The project uses UART0 for both ESP-IDF logs and the line-oriented demo CLI.
For clean computer parsing, set `CONFIG_LOG_DEFAULT_LEVEL_NONE=y` in
`sdkconfig.defaults` (the firmware still prints its CLI/JSON protocol using
`printf`). Some boards share UART0 with the USB console; use a USB-UART bridge
or configure a different UART/pin pair if that port is needed for flashing.

## Build and flash

From an ESP-IDF PowerShell or export shell:

```powershell
idf.py set-target esp32
idf.py build
idf.py -p COMx flash monitor
```

Replace `COMx` with the hub's serial port. If your hub is an ESP32-S3, change
the target to `esp32s3` instead. The selected chip must support ESP-NOW.

## UART commands

Commands are ASCII lines ending in Enter/newline:

```text
help
list
add AA:BB:CC:DD:EE:FF
del AA:BB:CC:DD:EE:FF
poll AA:BB:CC:DD:EE:FF
poll all
```

Node MAC configuration is saved in NVS and restored after reboot. `poll all`
polls configured nodes sequentially, waiting up to 700 ms for each response.
Firmware uses JSON Lines internally on UART for its simple demo protocol; the
Windows GUI parses those lines and presents normal node/telemetry fields in a
table, so operators do not need to interact with JSON.

## Windows C++ GUI app

`pc_app/` contains a small Windows GUI. It detects available COM ports, lets
you enter a node MAC in a form, and shows nodes and decoded telemetry in a
table. Build it with Visual Studio Build Tools or another C++17 compiler plus
CMake:

```powershell
cmake -S pc_app -B pc_app/build
cmake --build pc_app/build --config Release
.\pc_app\build\Release\hub_node_manager.exe
```

Select the hub COM port and click Connect. Enter the pump's MAC and choose Add
Node; the hub saves it in NVS and retains it after reboot. Select a row and
click Poll Selected, or click Poll All. Close ESP-IDF's serial monitor before
opening the same COM port in this app.

The request uses the same v1 frame as the pump firmware: magic `0xA7`, version
`1`, type COMMAND (`16`), opcode POLL_TELEMETRY (`1`), and a 16-bit request ID.
The pump replies with TELEMETRY type (`1`) and the same request ID.
