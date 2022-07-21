# Alarmclock

--------------- ALARM CLOCK PROJECT FOR RASPBERRY PI PICO ----------------

SSD1306 128x64 OLED display is used as screen.

OLED library used in the project is taken from https://github.com/MR-Addict/Pi-Pico-SSD1306-C-Library

It uses builtin RTC to handle time operations.

While Core 0 displays texts on the screen, Core 1 handles mainly logical operations and user inputs.

The alarm is disabled by default and multiple alarms cannot be set.

The alarm keeps firing at most 60 seconds.

Setting the alarm automatically enables it.

When the current mode is CLOCK, it activate SLEEP MODE after 10 seconds.

In SLEEP MODE, the display is blank. To awake the machine, press a button.

Although it is not safe, the communication between cores is maintained by global variables.

That the builtin LED is HIGH indicates that Pico gets power and starts both cores.

Instead of sleep, busy_wait is used because of the interrupt of alarm. Check sleep_ms functions for more information.


