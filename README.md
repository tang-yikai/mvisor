# MVisor: A mini x86 hypervisor

> **This is a fork of [tenclass/mvisor](https://github.com/tenclass/mvisor).**
> Upstream is the original project; this fork adds a small set of local changes
> on top of it, all listed under
> [Local changes over upstream](#local-changes-over-upstream) and tagged
> `local:` in the commit history.

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



## Local changes over upstream

Everything in this section is specific to this fork; the
[roadmap below](#roadmap-and-current-status) describes upstream's status, not
ours. Each change is also a tagged commit.

| change | tag | what it does |
|---|---|---|
| **Configurable CPU model** | `cpuid-20260923` | `machine.cpuid` lets you pick the vendor/model and a `type` + `arch` pair, plus VMX/SVM exposure through `virt`. Also fixes AMD family decoding, which truncated `0x17`/`0x19` to `7`. |
| **Guest power off exits mvisor** | `poweroff-20260924` | Declares `_S5` in the i440fx and q35 DSDTs and quits when the guest sets `SLP_EN`, so ACPI power off actually works. Before this the S5 path never fired at all, because the tables omitted `_S5`. |
| **Whole-machine snapshots** | `snapshots-20260924` | A `machine.snapshot` directory: restored automatically at startup, saved with and resumed by R_Ctrl+F2, written atomically (build `<path>.tmp`, then rename), and validated against the host CPU in `host.yaml` when moved between machines. See [Snapshots](#snapshots). |
| **Discard returns space to the host** | `discard-unmap-20260924` | A guest discard now punches a hole instead of only freeing the cluster inside the image, so the qcow2 file actually shrinks. Per-device `discard: unmap\|ignore`, defaulting to `unmap`. See [Discard](#discard). |
| **SDL viewer display and audio** | `working-20260923` | Fills the window when it is created and refreshes after redraws. On the audio side, picks an available capture device instead of failing under PipeWire, and loops partial `snd_pcm_writei` writes that were dropping frames. |
| **Optional virtio-fs limits** | `working-20260923` | `disk_size: 0` now means "no limit" rather than asserting when the shared directory is larger than the configured size. |
| **Configuration documentation** | `snapshots-20260924` | The [Configuration](#configuration), [Snapshots](#snapshots) and [UEFI boot](#uefi-boot) sections. UEFI is documented as *tested and not working* here, which contradicts upstream's roadmap claim. |

The fork's scope is deliberately narrow - see [Goal](#goal) above. It is not
aimed at becoming a general QEMU replacement.

## Roadmap And Current Status

What's supported now:

### Basic functions

1. 440FX ✅ / Q35 Chipset ✅
2. SeaBIOS ✅ OVMF ❌ (tested, does not work here - see [UEFI boot](#uefi-boot))
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

## Configuration

A machine is described by a YAML file with three top-level keys:

```yaml
name: Windows 11 LTSC      # optional, the VM name
base: q35.yaml             # optional, inherit from another config file
machine:
  ...
objects:
  - class: q35-host
  - class: qxl
  ...
```

`base` is resolved relative to the config file. Values in the current file
override the base's `machine` section, while the base's `objects` are loaded
first, so a child file only has to add its own devices.

### machine

| key | format | default | meaning |
|---|---|---|---|
| `memory` | `<n>G` or `<n>M` | | guest RAM. **The unit suffix is required** |
| `vcpu` | integer | | number of vCPUs. **Must be even** unless it is exactly 1; the guest is told 2 threads per core |
| `bios` | path | `share/bios-256k.bin` | firmware image; it is mapped at the top of the 4GB address space |
| `priority` | -20..19 | 1 | `nice` value applied to **every vCPU thread** (not the mvisor process). Lower means higher priority; `0` leaves the kernel default |
| `debug` | yes/no | No | per-device verbose tracing, see below |
| `hypervisor` | yes/no | No | set the CPUID hypervisor bit and inject Hyper-V enlightenments (PV clock, IPI, EOI, TLB flush). Lowers CPU usage, mainly useful for Windows guests |
| `powerdown` | `quit`/`pause` | `quit` | what to do when the guest requests an ACPI S5 power off |
| `snapshot` | path | | whole-machine snapshot directory, see [Snapshots](#snapshots) |
| `cpuid` | mapping | | guest CPU signature, see [CPU model](#cpu-model) |

Two of them are easy to get wrong: `memory` **must** carry a `G`/`M` suffix (a
bare number panics), and `vcpu` **must be even** unless it is exactly 1.

#### A note on `debug`

`debug` is not a logging on/off switch. mvisor's logger filters nothing:
`debug:` lines from `MV_LOG` are written to stdout all the time. This flag
opens a second, far more verbose set of messages that the code guards
explicitly - per-port IO/MMIO registration and access, AHCI/IDE command
tracing, display mode changes, thread lifecycle. Use it to chase a specific
device, not to control how much you see.

#### Why `vcpu` must be even

mvisor takes no topology as input; it derives one, and the derivation is
hard-coded to a single socket with SMT:

```cpp
// core/configuration.cc
if (num_vcpus_ == 1) {
  num_cores_ = 1;
  num_threads_ = 1;
} else {
  MV_ASSERT(num_vcpus_ % 2 == 0);      // <- the even-number requirement
  num_threads_ = 2;
  num_cores_ = num_vcpus_ / num_threads_;
}
```

So an 8-vCPU guest is presented as **1 socket, 4 cores, 2 threads per core**,
and that shape is written into CPUID leaf 4 (`core/vcpu.cc`), where L2 is
reported as shared by the 2 threads and L3 by every vCPU. There is no
`num_sockets_`: the socket count is implicitly 1, and there is no ACPI PPTT to
describe the hierarchy either, so Windows falls back to CPUID and SMBIOS.

Three consequences:

- **`vcpu` must be even** unless it is exactly 1, because every core has to
  carry a complete SMT pair. This comes from the "emulate one modern SMT CPU"
  design choice, not from a KVM limit.
- **The SMT claim is not backed by pinning.** mvisor does not pin vCPU threads,
  so the host may place the two vCPUs of a "core" anywhere, including on
  different physical cores or NUMA nodes. A guest that trusts this topology for
  co-scheduling or cache-sharing decisions is optimizing on fiction. This is the
  same reason libvirt's maintainer advises never exposing `threads != 1` unless
  the vCPUs are pinned 1:1 to host CPUs.
- **vCPU hotplug is not implemented**, and the fixed `threads=2` would be the
  first obstacle if it were. QEMU hits the same wall from the other side: with
  `threads > 1` it cannot emit ACPI processor containers, because `package-id=0`
  and `core-id=0` would collide.

For comparison, this is the opposite of what a libvirt VM does by default. There,
omitting `<topology>` means libvirt explicitly asks QEMU for
`sockets = vCPUs, cores = 1, threads = 1` - it deliberately does not follow
QEMU 6.2's own change of default to `sockets = 1, cores = vCPUs`. That
"socket per vCPU" layout is convenient for vCPU hotplug but collides with
Windows' per-socket licensing limits, which is why a Windows guest can end up
reporting all vCPUs in Device Manager while using only 2 in Task Manager.

### objects

Each entry instantiates one device:

```yaml
objects:
  - class: virtio-vgpu     # device class (required)
    parent: ich9-hda       # optional, normally inferred from the class
    debug: Yes             # optional, verbose tracing for this device only
    memory: 1G
    node: /dev/dri/renderD128
```

`parent` names another object explicitly when the default parent inferred from
the class is not what you want; `debug` is the per-device counterpart of the
machine-wide flag above.

The classes that take configuration keys:

| class | keys |
|---|---|
| `ata-disk` `ata-cdrom` `ide-disk` `ide-cdrom` `ahci-disk` `ahci-cdrom` `virtio-block` `floppy` | `image` (path), `readonly` (yes/no), `snapshot` (yes/no - discard writes on exit), `discard` (unmap/ignore) - see [Disk snapshots and discard](#disk-snapshots-and-discard) |
| `virtio-network` | `mac`, `backend` (`tap`/`user`), `mtu`, `ifname` (tap), `map` (user, e.g. `tcp:0.0.0.0:8022-:22`) |
| `virtio-fs` | `path`, `disk_name`, `disk_size`, `inode_count` |
| `virtio-vgpu` | `memory`, `staging`, `blob`, `node` |
| `vfio-pci` | `sysfs` |
| `qxl` | `vram_size`, `vga_size`, `rom` |
| `vga` | `vram_size`, `rom` |
| `ivshmem` | `shmem_path`, `shmem_size` |
| `cmos` | `rtc` (`localtime`/`gmtime`) |
| `apple-smc` | `osk` |
| `spice-agent` `qemu-guest-agent` | `max_clipboard` |

The remaining classes take no options: `q35-host`, `i440fx-host`, `kvm-irqchip`,
`kvm-clock`, `firmware-config`, `debug-console`, `dummy-device`, `ich9-lpc`,
`ich9-hda`, `hda-duplex`, `ich9-ahci`, `ich9-smbus`, `piix3`, `piix3-ide`,
`piix3-uhci`, `piix4-pm`, `ps2`, `uart`, `i8257-dma`, `i82078-fdc`, `pvpanic`,
`usb-keyboard`, `usb-tablet`, `usb-wacom`, `usb-midi`, `xhci-host`,
`virtio-console`, `webdav-agent`.

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

## Snapshots

Point `machine.snapshot` at a directory and mvisor keeps a whole-machine
snapshot there (RAM, device and vCPU state, disk state and the configuration):

```yaml
machine:
  snapshot: /root/vms/w11-snapshot
```

| | directory absent | directory present and complete |
|---|---|---|
| **on startup** | nothing happens, the guest boots normally | the snapshot is loaded and the machine resumes right away |
| **R_Ctrl+F2** | created, then the machine keeps running | refreshed in place, then the machine keeps running |

Note that R_Ctrl+F2 no longer leaves the machine paused, so F11 is not needed
afterwards. Without `machine.snapshot` the shortcut keeps writing to
`/tmp/save` as before.

Three things worth knowing:

- **Updates are atomic.** A save goes into `<snapshot>.tmp` and is only renamed
  into place once the whole image is complete, so an interrupted save cannot
  destroy the previous snapshot.
- **The directory is self-contained.** It carries `configuration.yaml` and
  `host.yaml`, so it can be copied to another machine and started with
  `-c <snapshot>/configuration.yaml`.
- **Cross-host restores are validated** against `host.yaml`: a snapshot taken
  on a host with a **different CPU vendor is refused** (a memory image carries
  MSRs and FPU state that do not move across vendors), while a different CPU
  model, RAM size or vCPU count only warns.

## Disk snapshots and discard

### Snapshots

Three different things are called "snapshot", and they do not overlap:

| setting | where | type | meaning |
|---|---|---|---|
| `machine.snapshot` | `machine:` | path | **whole-machine** snapshot: RAM, device and vCPU state, plus disk state. See [Snapshots](#snapshots) |
| `snapshot` | a disk device | yes/no | **this run does not persist**: the image is opened as a read-only backing file and every write goes to a temporary `snapshot_XXXXXX.qcow2` beside it (`/tmp/snapshot_XXXXXX.img` for raw images), which is deleted on exit |
| `readonly` | a disk device | yes/no | **the guest sees a read-only disk**: `VIRTIO_BLK_F_RO` is set, so writes are refused rather than silently discarded |

So `snapshot: yes` is the equivalent of QEMU's `snapshot=on` - handy for booting
a guest, trying something and throwing the writes away. It is **not** a way to
create a named, restorable disk snapshot.

mvisor has no disk-snapshot management of its own; there is no `qemu-img
snapshot` equivalent. Take disk snapshots **outside** mvisor instead:

- **Volume snapshots** (LVM, ZFS, btrfs) are the simplest - mvisor just sees a
  file on the snapshot volume.
- **qcow2 backing file chains** also work: `qemu-img create -f qcow2 -b base.qcow2
  -F qcow2 run.qcow2`, then point `image:` at `run.qcow2`.

Two things to avoid:

- **Do not use qcow2 *internal* snapshots.** `qemu-img snapshot -c` breaks
  mvisor: the header's `nb_snapshots` becomes non-zero and it refuses to start
  at all (`images/qcow2.cc` - `MV_PANIC("Qcow2 file with snapshots is not
  supported yet")`).
- **Stop mvisor before taking an external snapshot.** It keeps write caches, so
  snapshotting a live image captures an inconsistent disk state.

### Discard

When a guest discards blocks - `fstrim`, Windows' `Optimize-Volume -ReTrim`, or
just deleting files on a filesystem mounted with `-o discard` - mvisor releases
the matching qcow2 clusters. By default it also hands that space back to the
host filesystem, so the image file actually shrinks:

```yaml
objects:
  - class: virtio-block
    image: /path/to/disk.qcow2
    discard: unmap      # default: free the clusters and return the space
    # discard: ignore   # only mark clusters reusable inside the image
```

Measured on a 1G qcow2 with 200M written inside it, discarding those 200M moves
the file from 200 MiB of allocated blocks to 260 KiB, while its length is left
alone. `ignore` leaves both untouched. An unrecognised value panics at startup
rather than silently falling back.

How it works, and what it does not do:

- **The space comes back as a hole.** Each freed cluster gets
  `fallocate(FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE)`. `KEEP_SIZE` matters:
  qcow2 addresses clusters by absolute file offset, so trimming the file would
  move every cluster past the hole.
- **It only runs when the guest discards**, so it is off the hot path, and the
  hole punch happens only after the refcount drops to zero and the L2 entry is
  cleared.
- **An unsupported filesystem is not an error.** If hole punching returns
  `EOPNOTSUPP`/`ENOTSUP`/`EINVAL` the call is dropped and the image behaves as
  under `ignore` - clusters stay reusable, the file just does not shrink.
- **qcow2 only.** virtio-block advertises `VIRTIO_BLK_F_DISCARD` and
  `VIRTIO_BLK_F_WRITE_ZEROES` only for qcow2 images, so a raw image never
  receives a discard at all. ATA disks advertise TRIM only when discards are
  passed through.
- **This is not compaction.** Discard frees clusters wherever they sit; it never
  moves data down. To produce a minimal file offline, stop mvisor and run
  `qemu-img convert -O qcow2 disk.qcow2 compact.qcow2`, then replace the image -
  and re-take any `machine.snapshot`, since it embeds disk state.
- **`fallocate -d` will not help here.** QEMU writes zeros when it discards, so
  the host can detect the holes; mvisor only drops the reference and leaves the
  bytes in place, so there is nothing for `fallocate --dig-holes` to find.

## UEFI boot

`machine.bios` says **which** firmware image to load, not *whether* to boot via
UEFI. It is not an optional or UEFI-specific key: BIOS boot needs a firmware
image just as much, and the built-in default (`share/bios-256k.bin`) *is*
SeaBIOS. The key is loaded at the top of the 4GB address space, so pointing it
at an OVMF build is how you would switch the guest to UEFI:

```yaml
machine:
  bios: /path/to/OVMF.fd
```

OVMF is normally shipped as two files. Concatenate them, code first, to get the
single image mvisor expects (this example yields a 2MB image):

```bash
cat OVMF_CODE.fd OVMF_VARS.fd > OVMF.fd    # 1920K + 128K = 2MB
```

**Status: tested, and it does not work.** An OVMF image loads and starts
executing (the vcpu stays busy) but never produces any graphics output, so the
guest never appears. This was reproduced in the open-source build with 2MB and
4MB OVMF images, with both `qxl` and `vga`, with `cpuid.type: host_passthrough`,
and with the SeaBIOS-specific 1MB mapping disabled - the screen stayed black in
every case, while the same setup with SeaBIOS works. The fw_cfg entries, ACPI
tables and PCI ids that OVMF needs are all present, so the gap is deeper in the
emulation.

The mechanism above is documented only to explain how the firmware file is
loaded, not because it is usable: **UEFI boot is not supported. Use SeaBIOS,
which is the default.** A UEFI guest with Secure Boot would in addition need a
TPM, which mvisor does not emulate.

## Paravirtualized Drivers
An ISO image file is needed to install OS. Edit YAML file to configure image path.

Virtio is recommended for Windows guests:

<a href="https://fedorapeople.org/groups/virt/virtio-win/direct-downloads/stable-virtio/virtio-win.iso">Download Virtio Guest Tools</a>
