# Hardware and kernel

## The machine

```
CPU     AMD Ryzen 7 7800X3D (8 cores, one CCD, 96 MB L3)
GPU     Radeon RX 7900 GRE -> amdgpu (RDNA3)
Board   MSI MAG B650 TOMAHAWK WIFI
RAM     32 GB DDR5
LAN     Realtek RTL8125BG -> r8169
Audio   Realtek ALC4080 -> snd-usb-audio (it sits on USB, not HDA)
Disks   NVMe + SATA
Screen  1920x1080 at 165 Hz
```

## The kernel

`pkgs/linux-busylinux` builds the current LTS series (6.18) from
`tinyconfig`, the smallest configuration the kernel has, and merges
`files/busylinux.config` on top (and `files/vm.config` with `VM_SUPPORT=1`).
Then it runs `olddefconfig` and turns off every Ethernet vendor menu but
Realtek's.

### What it has

- **Hardware**: the AMD CPU (`X86_AMD_PSTATE`, microcode, x2APIC), amdgpu with
  Display Core, NVMe, AHCI, xHCI, r8169, USB audio, evdev and HID.
- **Gaming**: `NTSYNC` (Wine's NT synchronisation primitives, much faster
  than esync and fsync), `JOYSTICK_XPAD` (Xbox-style pads such as the GameSir
  G7 SE), `INPUT_JOYDEV`, `INPUT_UINPUT` (Steam Input's virtual pads), 32-bit
  compatibility (`IA32_EMULATION`, 16-bit segments and `modify_ldt` for old
  Windows games).
- **ROCm**: `HSA_AMD` and `HSA_AMD_SVM` for `/dev/kfd`.
- **Memory**: MGLRU (`LRU_GEN`, on by default), zram with zstd.
- **Containers**: user namespaces, cgroups v2 with BPF, overlayfs, FUSE, veth,
  bridges, NAT and the iptables compatibility Docker still uses.
- **Network**: nftables, conntrack, WireGuard.
- **Security**:
  - Landlock, Yama, `dmesg` restricted to root
  - all CPU mitigations, with retpolines, SRSO and straight-line speculation (`MITIGATION_SLS`)
  - stack protector strong, `FORTIFY_SOURCE`, hardened usercopy, `VMAP_STACK`
  - allocator hardening: random freelists (`SLAB_FREELIST_RANDOM`, `SLAB_FREELIST_HARDENED`), random kmalloc caches (`RANDOM_KMALLOC_CACHES`), a shuffled page allocator, memory zeroed on allocation (`INIT_ON_ALLOC_DEFAULT_ON`)
  - a randomised kernel stack offset on every system call
  - `BUG_ON_DATA_CORRUPTION`, `SCHED_STACK_END_CHECK`
  - `DEFAULT_MMAP_MIN_ADDR=65536`
  - **modules must be signed** (`MODULE_SIG_FORCE`) with the key made for that build
- `MAGIC_SYSRQ`, limited to REISUB by sysctl.

### What it leaves out

Wi-Fi, Bluetooth, HDA audio (HDMI audio included), KVM, `vhost`, the
legacy VGA console, Intel and other non-AMD CPU support, every file system but
ext4, FAT and the virtual ones, the IPv6-in-IPv4 tunnel, `userfaultfd`, legacy PTYs,
`TIOCSTI`, line discipline autoloading, `SLUB_TINY`, and compressed initramfs
support (there is no initramfs).

### Guarded options

The build checks the result. A list of options that must be on (the ones
above and the basics: `EFI_STUB`, `DEVTMPFS_MOUNT`, `FHANDLE`, and so on)
fails the build with `config lost: ...` if a kernel update drops or renames
one. A list of options that must stay off fails it with
`config crept back: ...`. Changing the fragment never silently produces a
kernel without, say, NTSYNC.

### Changing it

Edit `pkgs/linux-busylinux/files/busylinux.config` (`CONFIG_FOO=y`, or
`# CONFIG_FOO is not set`), then `doas tools/update.sh linux-busylinux` and
reboot. To browse: build once with `-e MENUCONFIG=1 -it` in the container, and
copy what you choose back into the fragment. If you need an option the
"crept back" list forbids, take it off that list in `build` too.

Modules are installed under `/lib/modules/RELEASE` and signed during the
build. A module from anywhere else, such as an out-of-tree driver, will not load.

### perl and the OID registry

The kernel build needs `perl` (in `makedepends`): it generates the
OID registry that module signing uses and the poly1305 assembly. It is only
in the build root, never on the system.

## Kernel command line

Set in `/boot/EFI/BOOT/limine.conf`:

```
root=PARTLABEL=busylinux-root rootfstype=ext4 rootwait ro
```

The kernel mounts the root itself, read-only. `rcS` checks it and remounts it
read-write. Add options to the `cmdline:` lines; the file survives updates.
