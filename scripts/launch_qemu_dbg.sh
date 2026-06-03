#!/usr/bin/env bash
# Launch the Morello guest directly (mirroring cheribuild's command) + an HMP
# monitor unix socket so we can toggle `-d int` exception logging at runtime.
set -x
TP=/home/bc/projs/stopless-java-gc/third_party
QEMU=$TP/output/sdk/bin/qemu-system-morello
IMG=$TP/output/cheribsd-morello-purecap.img
exec "$QEMU" \
  -M virt,gic-version=3 -cpu morello \
  -L "$TP/output/sdk/share/qemu" -bios edk2-aarch64-code.fd \
  -m 2048 -nographic \
  -drive if=none,file="$IMG",id=drv,format=raw \
  -device virtio-blk-pci,drive=drv \
  -device virtio-net-pci,netdev=net0 \
  -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:10005-:22" \
  -device virtio-rng-pci \
  -monitor unix:/tmp/qmon.sock,server,nowait
