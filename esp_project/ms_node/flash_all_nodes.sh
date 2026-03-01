#!/bin/bash
# Quick build and flash script for all 3 nodes

set -e

# Source ESP-IDF if needed
if [ -f "$HOME/esp/esp-idf/export.sh" ]; then
    . $HOME/esp/esp-idf/export.sh
fi

echo "=== Building firmware ==="
idf.py build

echo ""
echo "=== Flashing Node 1 (usbmodem1201) ==="
idf.py -p /dev/cu.usbmodem1201 flash

echo ""
echo "=== Flashing Node 2 (usbmodem1301) ==="  
idf.py -p /dev/cu.usbmodem1301 flash

echo ""
echo "=== Flashing Node 3 (usbmodem1401) ==="
idf.py -p /dev/cu.usbmodem1401 flash

echo ""
echo "=== All nodes flashed successfully! ==="
echo "Waiting 5 seconds for nodes to boot..."
sleep 5

echo ""
echo "=== Running cluster check ==="
python3 check_cluster.py

echo ""
echo "Done! Check the output above for cluster status."
