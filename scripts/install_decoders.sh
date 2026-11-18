#!/bin/sh
# Builds the two decoders sondechase runs as a child pipeline, fsk_demod and rs41mod, from the
# radiosonde_auto_rx source tree (auto_rx itself is not installed or run), and installs them in
# /usr/local/bin. The RTL-SDR itself is driven in the app through librtlsdr0 (a package dependency).
# Verified on a CardputerZero (Debian 13) with radiosonde_auto_rx v1.9.0.
set -e
AUTO_RX_TAG=${AUTO_RX_TAG:-v1.9.0}
WORK=${WORK:-$HOME/sondechase-build}

sudo apt-get update
sudo apt-get install -y build-essential git

mkdir -p "$WORK" && cd "$WORK"
[ -d radiosonde_auto_rx ] || git clone --depth 1 --branch "$AUTO_RX_TAG" https://github.com/projecthorus/radiosonde_auto_rx.git
cd radiosonde_auto_rx
# auto_rx's top-level Makefile adds the version to the JSON output; the two programs need nothing else.
# make hands CFLAGS to the shell again, so the C string quotes must arrive escaped.
export CFLAGS="-O3 -w -DVER_JSN_STR=\\\"${AUTO_RX_TAG#v}\\\""
make -s -C demod/mod rs41mod
make -s -C utils fsk_demod
sudo install -m 755 demod/mod/rs41mod utils/fsk_demod /usr/local/bin/

echo "Installed: $(command -v rs41mod) $(command -v fsk_demod)"
