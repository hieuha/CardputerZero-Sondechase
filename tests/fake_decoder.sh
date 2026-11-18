#!/bin/sh
# Stands in for "rtl_fm | fsk_demod | rs41mod --json": a sonde descending east of Hanoi.
echo "[ 5390] (S4430329) non-JSON text line that the app must ignore"
i=0
while [ $i -lt 3 ]; do
  alt=$((3000 - i * 12))
  echo "{\"type\":\"RS41\",\"frame\":$((5390 + i)),\"id\":\"S4430329\",\"datetime\":\"2026-11-18T00:31:0${i}.000Z\",\"lat\":21.03000,\"lon\":105.95000,\"alt\":${alt}.0,\"vel_h\":8.0,\"heading\":90.0,\"vel_v\":-12.0,\"sats\":9,\"bt\":1200,\"batt\":2.7,\"temp\":12.5,\"subtype\":\"RS41-SGP\"}"
  i=$((i + 1))
  sleep 0.1
done
sleep 30
