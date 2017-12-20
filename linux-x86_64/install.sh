rm /usr/lib/libftd3xx.so
cp libftd3xx.so /usr/lib/
cp libftd3xx.so.0.4.11 /usr/lib/
cp 51-ftd3xx.rules /etc/udev/rules.d/
udevadm control --reload-rules
