#!/bin/bash

. ~/esp/esp-idf-v5.4/export.sh

MAC=$(esptool.py --port /dev/tty.usbserial-110 read_mac \
      | grep -oE '([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}' \
      | head -n 1)
ID=$(echo "$MAC" | cut -d: -f4-6)

echo $MAC

pushd .
cd ~/src/NiimPrintX
./print.sh "$ID"
if [ $# -eq 1 ]; then
    ./print.sh "$1"
fi
popd

echo "$MAC" >> ../mac-addresses.txt

idf.py erase-flash

./build.sh flash
