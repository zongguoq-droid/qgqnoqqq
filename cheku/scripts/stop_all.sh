#!/bin/sh
echo "停止所有服务..."
killall gps_daemon dvr_daemon av_daemon input_daemon canbus_daemon car_ui 2>/dev/null
sleep 1
killall -9 av_daemon car_ui 2>/dev/null
ip link set can0 down 2>/dev/null
rm -f /tmp/car_*.sock /tmp/dvr_preview.jpg /tmp/av_volume /tmp/av_track
echo "已停止"
