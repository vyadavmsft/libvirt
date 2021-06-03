#!/bin/bash
set -x -e
source $HOME/.cargo/env

WORKLOADS_DIR="$HOME/workloads"
mkdir -p "$WORKLOADS_DIR"

FW_URL=$(curl --silent https://api.github.com/repos/cloud-hypervisor/rust-hypervisor-firmware/releases/latest | grep "browser_download_url" | grep -o 'https://.*[^ "]')
FW="$WORKLOADS_DIR/hypervisor-fw"
if [ ! -f "$FW" ]; then
    pushd $WORKLOADS_DIR
    time wget --quiet $FW_URL || exit 1
    popd
fi

OVMF_FW="$WORKLOADS_DIR/OVMF-4b47d0c6c8.fd"
# Check OVMF firmware is present
if [[ ! -f ${OVMF_FW} ]]; then
    echo "OVMF firmware not present on the host"
    exit 1
fi

FOCAL_OS_IMAGE_NAME="focal-server-cloudimg-amd64-custom-20210407-0.qcow2"
FOCAL_OS_IMAGE_URL="https://cloudhypervisorstorage.blob.core.windows.net/images/$FOCAL_OS_IMAGE_NAME"
FOCAL_OS_IMAGE="$WORKLOADS_DIR/$FOCAL_OS_IMAGE_NAME"
if [ ! -f "$FOCAL_OS_IMAGE" ]; then
    pushd $WORKLOADS_DIR
    time wget --quiet $FOCAL_OS_IMAGE_URL || exit 1
    popd
fi

FOCAL_OS_RAW_IMAGE_NAME="focal-server-cloudimg-amd64.raw"
FOCAL_OS_RAW_IMAGE="$WORKLOADS_DIR/$FOCAL_OS_RAW_IMAGE_NAME"
if [ ! -f "$FOCAL_OS_RAW_IMAGE" ]; then
    pushd $WORKLOADS_DIR
    time qemu-img convert -p -f qcow2 -O raw $FOCAL_OS_IMAGE_NAME $FOCAL_OS_RAW_IMAGE_NAME || exit 1
    popd
fi

VMLINUX_IMAGE="$WORKLOADS_DIR/vmlinux"
LINUX_CUSTOM_DIR="$WORKLOADS_DIR/linux-custom"

if [ ! -f "$VMLINUX_IMAGE" ]; then
    SRCDIR=$PWD
    pushd $WORKLOADS_DIR
    time git clone --depth 1 "https://github.com/cloud-hypervisor/linux.git" -b "ch-5.10.6" $LINUX_CUSTOM_DIR
    cp $SRCDIR/ch_integration_tests/resources/linux-config-x86_64 $LINUX_CUSTOM_DIR/.config
    pushd $LINUX_CUSTOM_DIR
    time make -j `nproc`
    cp vmlinux $VMLINUX_IMAGE || exit 1
    popd
    popd
fi

if [ -d "$LINUX_CUSTOM_DIR" ]; then
    rm -rf $LINUX_CUSTOM_DIR
fi

cargo_test() {
    bwrap --die-with-parent --ro-bind /usr /usr --ro-bind /etc /etc --ro-bind /var /var --bind /home /home --symlink usr/bin /bin --symlink usr/lib64 /lib64 --symlink usr/lib /lib --symlink usr/sbin /sbin --proc /proc --dev /dev --tmpfs /run --bind /tmp /tmp --bind /sys /sys cargo test -- --test $1
}

pushd ch_integration_tests
cargo test --no-run
cargo_test test_defines &
cargo_test test_direct_kernel_boot &
cargo_test test_libvirt_restart &
cargo_test test_huge_memory &
cargo_test test_multi_cpu &
cargo_test test_ovmf_fw_boot &
cargo_test test_rust_fw_boot &
cargo_test test_track_vm_killed_state &
cargo_test test_uri &
cargo_test test_vm_restart &
wait
popd
