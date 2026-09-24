# MVisor: A mini x86 hypervisor

## Goal

1. A minimal hypervisor based on KVM and x86 (replace QEMU)
2. A limited number of emulated devices (support plugins in later version)
3. Linux and Windows as guest VMs
4. VFIO (especially vGPU) and migration
5. Extremely stable and high performance


## Screenshot

### Ubuntu

<img src="./docs/ubuntu.jpg" width="640">

### vGPU

<img src="./docs/vgpu.jpg" width="640">

### Multimedia

<img src="./docs/multimedia.jpg" width="640">



## Roadmap And Current Status

What's supported now:

### Basic functions

1. 440FX ✅ / Q35 Chipset ✅
2. SeaBIOS ✅ OVMF ✅
3. Memory Region Management ✅
4. IOPort Management ✅
5. Devices Management ✅
6. RTC (CMOS) ✅
7. PS/2 ✅
8. PCI ISA ICH9-LPC ✅
9. QEMU CFG ✅
10. Legacy DMA ✅
11. IDE ✅ / AHCI ✅
12. Floppy Disk ✅
13. Serial Port ✅
14. VGA / VBE ✅
15. Option Roms ✅ / SMBIOS ✅ / ACPI Table ✅
16. Boot DOS ✅
17. Boot OS (Win98 to Win11 / DOS / Ubuntu / macOS Sonoma) ✅
18. QCOW2 ✅

### Multimedia & Networking

1. Virtio (Console ✅ / Block ✅ / Net ✅ / VirtioFS ✅ / VGPU ✅ / CUDA ✅ / Balloon)
2. SpiceAgent ✅
3. QemuGuestAgent ✅
4. Qxl ✅
5. Audio (ICH9-HDA / AC97) ✅
6. Tap network ✅
7. User network ✅
8. VFIO (mdev & passthrough) ✅
9. Samba
10. USB 1.0 UHCI ✅ / USB 3.0 XHCI ✅ / USB Tablet ✅ / USB Keyboard ✅ / USB Midi ✅ / USB Wacom ✅

### Hyper-V & Migration

1. CPU migration ✅
2. VFIO migration ✅
3. Migration to sparse files ✅
4. Hyper-V enlightenments ✅
5. Live migration ✅

## Compile & Run

### For RockyLinux 9.3,

```bash
dnf install epel-release gdb cmake gcc-c++ acpica-tools
dnf --enablerepo=devel install -y protobuf-compiler protobuf-devel glib2-devel yaml-cpp-devel pixman-devel libzstd-devel zlib-devel

# If SDL enabled
dnf --enablerepo=devel install -y SDL2-devel alsa-lib-devel
```

### For Debian 12,

```bash
apt install meson gdb cmake build-essential g++ acpica-tools
apt install protobuf-compiler libprotobuf-dev libglib2.0-dev libyaml-cpp-dev libpixman-1-dev libzstd-dev zlib1g-dev

# If SDL enabled
apt install libsdl2-dev libasound2-dev
```

### Compile and Run

```bash
meson setup build -Dsdl=true # SDL is disabled by default
meson compile -C build

./build/mvisor -c config/sample.yaml -vnc 5900
```

## CPU model

The guest CPU signature is configured under `machine.cpuid`. Only three keys
matter:

```yaml
machine:
  cpuid:
    type: custom        # default | host_model | host_passthrough | custom
    arch: x86_64-v3     # only used when type is "custom"
    virt: No            # expose VMX (Intel) / SVM (AMD) to the guest
```

`vendor` and `model` are optional extras: `vendor` overrides the vendor string
the guest sees (leave it out to inherit the host's), and `model` overrides the
brand string shown inside the guest (leave it out to have one generated).

### `type`

| value | meaning |
|---|---|
| `default` (or omitted) | the built-in legacy signature (family 15) |
| `host_model` | the host's family/model/stepping, features still trimmed |
| `host_passthrough` | the host CPUID verbatim, no feature trimming |
| `custom` | pick one of the models/levels below via `arch` |

### `arch` - CPU models

Use the architecture codename, not the marketing name (so an i5-14600KF is
`raptorlake`, a Xeon Silver 5120 is `skylake-server`; if you would rather not
think about it, use `type: host_model`).

Intel: `nehalem` `westmere` `sandybridge` `ivybridge` `haswell` `broadwell`
`skylake` `skylake-server` `cascadelake` `icelake` `icelake-server` `tigerlake`
`rocketlake` `alderlake` `raptorlake` `sapphirerapids`

AMD: `zen` `zen-plus` `zen2` `zen3` `zen4` `epyc-naples` `epyc-rome`
`epyc-milan` `epyc-genoa`

A model from the other vendor is allowed (a guest booting from disk re-reads
CPUID, so it can run on either vendor) but logs a warning; the host must still
implement the requested features, which KVM enforces.

### `arch` - x86-64 levels

Levels describe a feature set rather than a specific CPU, so the guest is
presented with the generation that introduced the level:

| level | highlights | signature |
|---|---|---|
| `x86-64` | baseline (v1) | `0x006FB` |
| `x86-64-v2` | + SSE4.2, POPCNT, CMPXCHG16B, LAHF/SAHF | `0x106A5` |
| `x86-64-v3` | + AVX2, BMI1/BMI2, FMA, F16C, MOVBE, LZCNT | `0x306C3` |
| `x86-64-v4` | + AVX-512 (F/BW/CD/DQ/VL) | `0x50654` |

### `virt`

Controls whether hardware virtualization is advertised to the guest: `VMX` on
Intel hosts, `SVM` (AMD-V) on AMD hosts. It defaults to on for
`host_passthrough` and off otherwise, and can be overridden either way. Turn it
on when the guest needs nested virtualization (Hyper-V, WSL2, Device Guard,
running QEMU inside the guest).

## Guest power off

When the guest performs an orderly shutdown it writes the ACPI S5 state to the
PM1 control register. mvisor treats that as a power-off request:

```yaml
machine:
  powerdown: quit     # quit (default) | pause
```

- `quit` - exit the mvisor process as soon as the guest requests S5
- `pause` - only pause the VM and leave the process alive for inspection

This is the only reliable power-off signal: a guest that crashed or hung never
sets the `SLP_EN` bit, so it cannot be mistaken for a shutdown -- mvisor keeps
running, which is what you want when debugging. Sleep states (S1-S4) are not
implemented and are ignored with a warning instead of aborting the VM.

## Paravirtualized Drivers
An ISO image file is needed to install OS. Edit YAML file to configure image path.

Virtio is recommended for Windows guests:

<a href="https://fedorapeople.org/groups/virt/virtio-win/direct-downloads/stable-virtio/virtio-win.iso">Download Virtio Guest Tools</a>
