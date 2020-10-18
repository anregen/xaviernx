#!/bin/bash

gst-launch-1.0 filesrc location=./local_10bit.mkv ! qtdemux ! h265parse ! omxh265dec ! nvvidconv ! fpsdisplaysink -e

