#!/bin/bash

PORTS=(
    "/dev/ttyACM0"
    "/dev/ttyACM1"
    "/dev/ttyACM2"
)

for PORT in "${PORTS[@]}"; do
    echo "----------------------------------------"
    echo "Erasing firmware on device at $PORT..."
    idf.py -p "$PORT" erase-flash
    echo "Done with $PORT"
done
sleep 2

echo "cleaning Previous BUILD..."
idf.py fullclean && rm -rf build
sleep 1

echo "Building firmware..."
idf.py build
sleep 1

for PORT in "${PORTS[@]}"; do
    echo "----------------------------------------"
    echo "Flashing device at $PORT..."
    idf.py -p "$PORT" flash
    echo "Done with $PORT"
done

echo "----------------------------------------"
echo "All devices flashed successfully!"
sleep 3

export IDF_MONITOR_NO_COLORS=1

# script -q -c "idf.py -p /dev/ttyACM0 monitor --no-reset" > /home/punky/esp_1/esp_project/WSN_main_set/logs/out.log &
# PID1=$!
# script -q -c "idf.py -p /dev/ttyACM1 monitor --no-reset" > /home/punky/esp_1/esp_project/WSN_main_set/logs/out1.log &  
# PID2=$!
# script -q -c "idf.py -p /dev/ttyACM2 monitor --no-reset" > /home/punky/esp_1/esp_project/WSN_main_set/logs/out2.log &
# PID3=$!

script -q -c "idf.py -p /dev/ttyACM0 monitor " > /home/punky/esp_1/esp_project/WSN_main_set/logs/out.log &
PID1=$!
script -q -c "idf.py -p /dev/ttyACM1 monitor " > /home/punky/esp_1/esp_project/WSN_main_set/logs/out1.log &  
PID2=$!
script -q -c "idf.py -p /dev/ttyACM2 monitor " > /home/punky/esp_1/esp_project/WSN_main_set/logs/out2.log &
PID3=$!

echo "NODE_1 PID: $PID1"
echo "NODE_2 PID: $PID2"
echo "NODE_3 PID: $PID3"