#!/bin/bash

CLIENT_IP=192.168.1.110
gst-launch-1.0 nvarguscamerasrc ! tee name=t ! queue ! 'video/x-raw(memory:NVMM), width=1920, height=1080, framerate=30/1' ! omxh264enc control-rate=2 bitrate=9000000 ! video/x-h264, stream-format=byte-stream ! rtph264pay mtu=1400 ! udpsink host=$CLIENT_IP port=5000 sync=false async=false -v t. ! queue ! 'video/x-raw(memory:NVMM), width=1920, height=1080, format=NV12, framerate=30/1' ! nvvidconv ! 'video/x-raw(memory:NVMM), format=I420_10LE' ! omxh265enc ! matroskamux ! filesink location=local_10bit.mkv -e

