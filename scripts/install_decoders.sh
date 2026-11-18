#!/bin/sh
# Builds the pieces sondechase runs as child processes: rtl-sdr (RTL-SDR Blog fork, needed for
# the v4 dongle), and rs41mod + fsk_demod from the radiosonde_auto_rx source tree.
# auto_rx itself is not installed or run. NOT yet verified on a CardputerZero.
set -e
sudo apt-get update
sudo apt-get install -y build-essential cmake git libusb-1.0-0-dev pkg-config alsa-utils gpsd gpsd-clients

WORK=${WORK:-$HOME/sondechase-build}
mkdir -p "$WORK" && cd "$WORK"

if ! command -v rtl_fm >/dev/null 2>&1; then
  [ -d rtl-sdr-blog ] || git clone --depth 1 https://github.com/rtlsdrblog/rtl-sdr-blog.git
  cmake -S rtl-sdr-blog -B rtl-sdr-blog/build -DINSTALL_UDEV_RULES=ON -DDETACH_KERNEL_DRIVER=ON
  cmake --build rtl-sdr-blog/build -j2
  sudo cmake --install rtl-sdr-blog/build
  sudo ldconfig
  echo 'blacklist dvb_usb_rtl28xxu' | sudo tee /etc/modprobe.d/blacklist-rtl-sdr.conf
fi

[ -d radiosonde_auto_rx ] || git clone --depth 1 https://github.com/projecthorus/radiosonde_auto_rx.git
(cd radiosonde_auto_rx/auto_rx && ./build.sh)
sudo install -m 755 radiosonde_auto_rx/auto_rx/rs41mod radiosonde_auto_rx/auto_rx/fsk_demod /usr/local/bin/

echo "Check: rtl_test -t ; rs41mod -h ; fsk_demod (prints usage)"
