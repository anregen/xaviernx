#!/bin/bash

gst-launch-1.0 filesrc location=./local_10bit.mkv ! matroskademux ! h265parse ! omxh265dec ! nvvidconv ! 'video/x-raw(memory:NVMM), format=(string)NV12' ! nvoverlaysink -e

