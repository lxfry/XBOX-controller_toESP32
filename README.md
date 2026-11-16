# Xbox controller to ESP32-S3

Read a wired Xbox controller using the ESP32-S3 native USB Host and display its raw input packets over UART.

The current `main/main.c` recognizes **VID `045E`, PID `0B12`**, sends three startup commands, and prints packets beginning with `20` under the `XBOX_IN` log tag. It uses interface 0 and endpoints `0x82` (IN) / `0x02` (OUT).

**This version prints hexadecimal packets; it does not decode buttons or axes.** ESP-NOW transmission to a second ESP32-S3 onboard a UAV is the next project stage and is not implemented here.

## Hardware bench

- **Board:** ESP32-S3-DevKitC-1 N32R16V.
- **Controller:** Microsoft Xbox wireless controller connected by a **USB-C data cable**, using wired USB.
- **PC connection:** board's USB-to-UART/programming port for flashing and PuTTY logs.
- **Controller connection:** board's **native USB port**, with the appropriate host cable/adapter.
- **Controller power:** supplied by the ESP32 board's **5 V header pin**, through the soldered jumper described below; ground is shared through the USB connection.

```text
PC ---- USB-to-UART/programming port ---- ESP32-S3
                                            |
                                      native USB Host
                                            |
                                   data cable / adapter
                                            |
                                      Xbox controller

Board 5 V header ---- soldered wire ---- USB adapter VBUS (5 V)
                                            |
                                    controller USB power
```

The native USB signals are GPIO19 (D-) and GPIO20 (D+). Use the board's native USB connector; the USB-to-UART connector cannot host the controller. Keep the PC console on UART while native USB hosts the controller. See [Espressif's USB hardware guidance](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32s3/schematic-checklist.html).

**Power modification used on this bench:** according to the owner's hardware observations, a diode in this board's native USB 5 V path prevents the board from supplying VBUS to the controller. A wire was therefore soldered from the board's **5 V header pin directly to the USB adapter's VBUS (5 V) contact**, feeding the controller through its USB cable. This provides a direct power path around that limitation; the firmware does not control it.

To reproduce this setup, use the modified adapter/jumper and power the board through its UART/programming connection. Check the adapter pinout and solder joint before applying power. The jumper connects to **VBUS, not D+, D-, or a GPIO**. Keep the modified native USB connection dedicated to the controller; connecting another powered USB host there can backfeed its supply. The exact adapter model is still to be recorded.

## Build and flash

1. Install **ESP-IDF 6.1.0** with ESP32-S3 tools; development was done through VS Code.
2. Open this folder in an ESP-IDF terminal. Keep `sdkconfig` and `dependencies.lock`: they select the ESP32-S3 configuration and USB component **1.6.1**. The first build downloads dependencies automatically and needs internet access.
3. Connect the PC through the board's UART/programming port and identify its serial port.
4. Close PuTTY if it is using that port, then run:

   ```powershell
   idf.py build
   idf.py -p COM3 flash
   ```

   Replace `COM3` with your port. The target is already configured as `esp32s3`. The supplied configuration uses a 2 MB flash layout and disables PSRAM; it does not use the board's full memory capacity.

## Reproduce the input test

1. Open **PuTTY**, select the board's UART serial port, and use **115200 baud, 8 data bits, no parity, 1 stop bit, no flow control**. Reset the ESP32 if necessary to see startup.
2. Wait for `USB client ready. Plug in the controller.`
3. Confirm the board's 5 V header-to-adapter VBUS jumper is connected, then connect the Xbox controller to native USB. The board supplies the controller's power through this jumper.
4. Check that the console reports `VID=0x045E PID=0x0B12`. Other IDs will not enter packet capture.
5. Press buttons, move both sticks, and squeeze/release both triggers. Look for changing hexadecimal packets tagged `XBOX_IN`. Named buttons and numeric axis values are not expected in this source version.
6. Enable PuTTY session logging to save the results. Record the controller model/firmware, cable/adapter, power wiring, and results alongside the capture.

If no device appears, check the native USB connection, data cable, and VBUS supply. If the IDs appear but no packets follow, check the exact ID match and USB warnings. Close PuTTY before flashing again.

## Test status and remaining issues

- **Owner-confirmed:** project built and flashed; USB Host initialized; controller enumerated; input reports received.
- **Earlier test history:** buttons, sticks, and triggers were decoded and tested in a version containing `print_report()`. That function and decoder are absent from the current `main.c`.
- **Open issue:** slow trigger release sometimes left the last displayed value nonzero. A reporting revision was made historically, but the fix has not been confirmed. Retest with raw and decoded values together once the decoder is restored.
- **Unplug/replug:** cleanup is implemented, but reliable recovery has not been conclusively validated.
- **Next steps:** reconcile/restore the decoder, confirm trigger release and reconnection, then implement ESP-NOW and receiver failsafe behavior before flight-controller integration.

Documentation reviewed on 16 November 2026. Existing build artifacts were inspected; no new build or hardware test was performed during documentation work.
