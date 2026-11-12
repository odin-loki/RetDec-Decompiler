#!/bin/sh
# freebsd job for ci/run-local.sh: cross-build with clang + FreeBSD sysroot (a native build under QEMU TCG takes
# hours), boot the official BASIC-CLOUDINIT image in QEMU, copy source + build tree to the same paths in the guest,
# install the workflow's packages and run its ctest line there.
#   FBSD_DIR must hold sysroot/ (from base.txz) and base.qcow2 (BASIC-CLOUDINIT-ufs). Usage: ci/freebsd-vm.sh <src>
set -eu
src=$1; d=${FBSD_DIR:-/workspace/ci-local/fbsd}; b=$d/build; port=${FBSD_SSH_PORT:-2255}
LLVM=${LLVM:-/workspace/opt/llvm18/bin}; key=${FBSD_KEY:-$HOME/.ssh/id_ed25519}
export FREEBSD_SYSROOT=$d/sysroot
cmake -S "$src" -B "$b" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$src/ci/freebsd-cross.cmake" -DCMAKE_C_COMPILER="$LLVM/clang" \
  -DCMAKE_CXX_COMPILER="$LLVM/clang++" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFORGE_BUILD_EXAMPLES=OFF
cmake --build "$b" -j"${J:-6}"
cd "$d"; rm -f vm.qcow2; qemu-img create -q -f qcow2 -b base.qcow2 -F qcow2 vm.qcow2 20G
mkdir -p seed; printf 'instance-id: forge-ci\nlocal-hostname: forge-ci\n' > seed/meta-data
cat > seed/user-data <<UD
#cloud-config
users:
  - name: ci
    shell: /bin/sh
    groups: wheel
    ssh_authorized_keys: ["$(cat "$key.pub")"]
runcmd:
  # QEMU user-mode DNS cannot answer SRV lookups, so use the plain https pkg mirror.
  - mkdir -p /usr/local/etc/pkg/repos
  - |
    printf 'FreeBSD: { url: "https://pkg.FreeBSD.org/\${ABI}/quarterly", mirror_type: "none" }\nFreeBSD-kmods: { enabled: no }\n' > /usr/local/etc/pkg/repos/FreeBSD.conf
  # the first pkg run only upgrades pkg itself; refresh the catalogue and install for real (tcsh is base csh;
  # cmake is packaged as cmake-core since 2026Q4).
  - env ASSUME_ALWAYS_YES=yes pkg update -f
  - env ASSUME_ALWAYS_YES=yes pkg install -y cmake-core ninja python3 py312-numpy bash zsh mksh fish
  - mkdir -p /workspace
  - chown ci /workspace
  - touch /var/ci-ready
UD
rm -f seed.iso; pycdlib-genisoimage -quiet -o seed.iso -V cidata -J -r seed/user-data seed/meta-data
qemu-system-x86_64 -accel tcg,thread=multi,tb-size=512 -cpu Nehalem -smp 4 -m 4096 -drive file=vm.qcow2,if=virtio,cache=unsafe \
  -drive file=seed.iso,media=cdrom -nic user,model=virtio-net-pci,hostfwd=tcp:127.0.0.1:$port-:22 -display none \
  -serial file:serial.log -daemonize -pidfile vm.pid
trap 'kill "$(cat "$d/vm.pid")" 2>/dev/null || true' EXIT
ssh_() { ssh -i "$key" -p "$port" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=10 -o LogLevel=ERROR ci@127.0.0.1 "$@"; }
i=0; until ssh_ true 2>/dev/null; do i=$((i+1)); [ $i -lt 180 ] || { echo "VM did not come up"; tail -30 serial.log; exit 1; }; sleep 10; done
echo "VM up after $((i*10))s"; ssh_ uname -a
i=0; until ssh_ test -f /var/ci-ready; do i=$((i+1)); [ $i -lt 360 ] || { echo "cloud-init runcmd (pkg install) did not finish"; exit 1; }; sleep 10; done
ssh_ "pkg info -q cmake-core ninja bash zsh mksh fish python3 && mkdir -p '$src' '$b'"
(cd "$src" && git ls-files -z | tar --null -T - -cf -) | ssh_ "tar xf - -C '$src'"
# ctest names the host's python (golden/sign/onnx/docs tests); point that path at the guest's python3.
hp=$(sed -n 's/^Python3_EXECUTABLE:[A-Z]*=//p' "$b/CMakeCache.txt")
[ -z "$hp" ] || ssh_ "echo | su root -c 'mkdir -p $(dirname "$hp") && ln -sf /usr/local/bin/python3 $hp'"
tar cf - -C "$b" --exclude='*.o' --exclude='*.a' . | ssh_ "tar xf - -C '$b'"
# FreeBSD packages no python safetensors/onnx (pip would build them from Rust/C++ source under TCG), so the
# tests that need them (and doc_example_inference_generate, which uses golden_tiny_llama's model) run on linux-* only.
ssh_ "cd '$b' && env CTEST_OUTPUT_ON_FAILURE=1 FORGE_THREADS=2 ctest -LE 'slow|emu|gpu' -E '^(golden_tiny_llama|golden_moe_mla|modelfile_signing|onnx_import|doc_example_inference_generate)\$' -j2 --timeout 1200 --output-on-failure"
