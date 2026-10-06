#!/bin/sh

# QZdesk motor GPIOs: GPIO1_A0/A1 and GPIO1_A3/A4 => Linux GPIO 32/33/35/36.
M1A=32
M1B=33
M2A=35
M2B=36

direction=$(sed -n 's/.*"direction"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')

gpio_init() {
    pin="$1"
    [ -d "/sys/class/gpio/gpio${pin}" ] || echo "$pin" > /sys/class/gpio/export 2>/dev/null || true
    echo out > "/sys/class/gpio/gpio${pin}/direction" 2>/dev/null || true
}

set_gpio() {
    echo "$2" > "/sys/class/gpio/gpio$1/value"
}

gpio_init "$M1A"
gpio_init "$M1B"
gpio_init "$M2A"
gpio_init "$M2B"

case "$direction" in
    forward)
        set_gpio "$M1A" 0; set_gpio "$M1B" 1
        set_gpio "$M2A" 0; set_gpio "$M2B" 1
        ;;
    backward)
        set_gpio "$M1A" 1; set_gpio "$M1B" 0
        set_gpio "$M2A" 1; set_gpio "$M2B" 0
        ;;
    left)
        set_gpio "$M1A" 1; set_gpio "$M1B" 0
        set_gpio "$M2A" 0; set_gpio "$M2B" 1
        ;;
    right)
        set_gpio "$M1A" 0; set_gpio "$M1B" 1
        set_gpio "$M2A" 1; set_gpio "$M2B" 0
        ;;
    stop|*)
        set_gpio "$M1A" 0; set_gpio "$M1B" 0
        set_gpio "$M2A" 0; set_gpio "$M2B" 0
        ;;
esac

printf '{"status":"ok","direction":"%s"}\n' "${direction:-stop}"
