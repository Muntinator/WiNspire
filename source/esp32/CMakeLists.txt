# cxlink ESP32 bridge firmware for WiNspire on the original TI-Nspire CX.
#
# Build with ESP-IDF:
#   . $IDF_PATH/export.sh
#   idf.py set-target esp32
#   idf.py build
#   idf.py -p /dev/ttyUSB0 flash monitor
#
# See README.md for wiring.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cxlink-bridge)
