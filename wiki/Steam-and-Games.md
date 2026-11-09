# Steam and games

Steam, Proton and most Linux games are built for glibc, and Steam's 32-bit
parts need 32-bit libraries. Alpine has neither: it uses musl, 64-bit only.
So BusyLinux keeps a small **Arch Linux tree** in `/opt/root.x86_64` and runs
Steam inside it with **bubblewrap**. bubblewrap is the same sandbox tool Flatpak and
Steam's own runtime use. There is no container engine, daemon or image. The
tree is an ordinary folder, and `arch-chroot` is a short `sh` script.

The system side is ready already: NTSYNC, xpad, uinput and 32-bit support in
the kernel, `vm.max_map_count=1048576`, and up to 524288 open files for logins.

## Set up

As your user, from a checkout:

```sh
tools/arch-setup
arch-chroot steam
```

`arch-setup`:

1. Installs `bubblewrap` and `zstd` on the host if missing.
2. Downloads Arch's **bootstrap tarball** from `geo.mirror.pkgbuild.com` and
   **checks its SHA-256** against Arch's published sums before unpacking it.
3. Prepares the tree:
   - enables the geo mirror and the **multilib** repository (32-bit packages)
   - turns off pacman's `CheckSpace`, which fails inside bubblewrap
   - generates the `en_US.UTF-8` locale
   - creates the files and folders the sandbox mounts over: `machine-id`,
     `localtime`, `/mnt` and `/media`
   - sets pacman to **skip docs, man pages and translations** (`NoExtract`,
     the list Arch's Docker image uses), with no download timeout for slow mirrors
4. Initialises pacman's keyring and installs: `steam`, `umu-launcher`,
   `mangohud` + `lib32-mangohud`, `vulkan-radeon` + `lib32-vulkan-radeon`,
   `lib32-mesa`, and the **Source Han Sans** JP, KR and CN fonts. Name other
   packages to install those instead: `tools/arch-setup steam vulkan-intel ...`.
5. Installs `arch-chroot` into `~/.local/bin` and a **Steam launcher entry**,
   `~/.local/share/applications/steam.desktop`, which also handles `steam://`
   links. It asks for the dedicated GPU (`PrefersNonDefaultGPU`).

The root steps run through `doas`. The tree takes a few GB, mostly
Mesa, LLVM and the 32-bit libraries. Steam's runtime and games live in
your home.

`Icon=steam` in the launcher entry comes from your icon theme (Papirus has
it). The icon inside the tree is not visible to the host.

## arch-chroot

```sh
arch-chroot steam                       # a program in the tree, as you
arch-chroot run steam                   # the same, spelled out
arch-chroot update                      # update the tree
arch-chroot pacman -S gamescope         # pacman in the tree (as root, through doas)
arch-chroot root                        # a root shell in the tree
arch-chroot root locale-gen             # a root command in the tree
ARCH_ROOT=/elsewhere arch-chroot steam  # a tree somewhere else
```

### As you (`run`)

The tree is mounted **read-only** as `/`. Programs run as your own user, and
they see:

| | |
|---|---|
| `$HOME` | your real home, read-write: Steam's data in `~/.local/share/Steam`, your games, your configs |
| `/mnt`, `/media` | your drives, if mounted there (e.g. a Steam library on a second disk) |
| `/dev` | all devices: the GPU, input devices, `/dev/ntsync`, `/dev/uinput`, hidraw for controllers |
| `/sys` | read-only |
| `/tmp` | the host's, so X11 sockets work |
| `/run/user/UID` | your runtime folder: Wayland, PipeWire, D-Bus session sockets |
| `/etc/passwd`, `/etc/group` | the host's, read-only, so your user exists inside |
| `/etc/resolv.conf`, `/etc/machine-id`, `/etc/localtime` | the host's: DNS, a stable machine ID, your time zone |
| `/home` and `/run` | otherwise empty tmpfs |

And it sets:

| | |
|---|---|
| no `LD_PRELOAD` | the tree's glibc cannot load the host's musl mimalloc; every program would print an error |
| `LANG=en_US.UTF-8` | unless you set one; Steam's runtime insists on a UTF-8 locale |
| `MESA_SHADER_CACHE_MAX_SIZE=10G` | unless you set one; see [Shader cache](#shader-cache) |
| a separate fontconfig cache | `~/.cache/fontconfig-arch` is mounted over `~/.cache/fontconfig`. Arch's fontconfig is newer than Alpine's, and each refuses the other's cache files ("newer version" warnings) |

Because the tree is read-only to your programs, nothing Steam or a game
does can change it. Only `arch-chroot update`, `pacman` and `root` can,
and they need `doas`.

### As root (`update`, `pacman`, `root`)

The tree is mounted read-write, with `--cap-add ALL`. pacman needs the
capabilities to drop to its unprivileged download user, `alpm`, which it then
confines to the download folder with Landlock. Without them, pacman must turn
its sandbox off.

## Updating the tree

```sh
arch-chroot update
```

1. Updates `archlinux-keyring` first. A tree left alone for weeks may not
   know the keys of newer packages, and a full upgrade would then fail on
   signatures.
2. Runs `pacman -Su`, the full upgrade, which asks before it starts.
3. Runs `pacman -Sc`, which keeps only the installed versions in the package
   cache. Otherwise every update leaves its old packages behind.

Do it now and then, and before reporting a problem. Steam updates itself
in your home; this updates Mesa, the drivers and the rest.

## Running games

### Proton

In Steam: **Settings > Compatibility > Enable Steam Play for all other titles**,
or per game: **Properties > Compatibility > Force the use of a specific Steam
Play compatibility tool**. Proton Experimental or the newest Proton is usually
the best start. For Proton-GE and others, unpack them into
`~/.local/share/Steam/compatibilitytools.d/`.

**NTSYNC** (`/dev/ntsync`, mode 666 on BusyLinux) is Windows'
synchronisation primitives in the kernel, faster and more correct than esync
and fsync. Recent Wine, Proton and Proton-GE builds use it when the device is
there; with older Proton-GE, set `PROTON_USE_NTSYNC=1`.

### Launch options

Per game: **Properties > General > Launch options**. `%command%` stands for
the game.

```
MANGOHUD=1 %command%                       # the MangoHud overlay
PROTON_ENABLE_WAYLAND=1 %command%          # Wine's Wayland driver (Proton 10+), no Xwayland
gamescope -W 1920 -H 1080 -r 165 -f -- %command%
```

### gamescope

`arch-chroot pacman -S gamescope`. Valve's micro-compositor runs the game in
a nested session. That fixes many games' fullscreen, resolution and
frame-pacing quirks, and can upscale.

```
gamescope -w 2560 -h 1440 -W 1920 -H 1080 -r 165 -o 30 -f --rt --mangoapp -- %command%
```

| | |
|---|---|
| `-w -h` | the resolution the game renders at (here 1440p, downscaled for sharpness) |
| `-W -H` | the output, your monitor's resolution |
| `-r 165` | the frame limit when focused, your refresh rate |
| `-o 30` | the frame limit when unfocused, to stay cool in the background |
| `-f` | fullscreen |
| `--rt` | realtime scheduling for gamescope; the tty logins allow it |
| `--mangoapp` | MangoHud drawn by gamescope, the reliable way to use both |

### MangoHud

In the tree already, as games load the tree's libraries. Test it:

```sh
arch-chroot pacman -S vulkan-tools
arch-chroot mangohud vkcube
```

`~/.config/MangoHud/MangoHud.conf`:

```ini
fps
frametime
gpu_temp
cpu_temp
fps_limit=165
```

**Leave out `font_file`.** Steam runs games in its own runtime container
(pressure-vessel), where host font paths do not exist, and MangoHud stops the
game when its font is missing. `fps_limit=60` keeps a game, and the room,
cooler.

### Games outside Steam: umu

`umu-launcher` runs any Windows program with Proton and Steam's runtime,
the way Steam would:

```sh
WINEPREFIX=~/Games/name/prefix GAMEID=0 arch-chroot umu-run ~/Games/name/Game.exe
```

`GAMEID` picks game-specific fixes from umu's database (`0` for none, or the
game's `umu-` ID). `PROTONPATH=GE-Proton` uses Proton-GE, which umu downloads.
Keep each game and its prefix in a folder of its own.

A small wrapper saves typing, e.g. `~/.local/bin/umu`:

```sh
#!/bin/sh
# umu GAME.exe [ARGS...]: runs it with Proton in a prefix beside the game.
dir=$(cd "$(dirname "$1")" && pwd)
WINEPREFIX=$dir/prefix GAMEID=${GAMEID:-0} exec arch-chroot umu-run "$@"
```

## Controllers

Xbox-style pads (xpad), and pads that HID or SDL know, work over USB. Steam
needs `/dev/uinput` (group `input`, which your user is in) for **Steam
Input**, its own remapping layer that creates a virtual pad.

That virtual pad has a catch. Steam creates it on the host, and programs in
the tree look for pads through libudev. The tree's libudev is systemd's, and
it expects `udevd`'s database and events, which BusyLinux does not have. So a
game in the tree may not see Steam Input's virtual pad, or any pad. Two fixes:

- **Let SDL scan `/dev/input` itself**, bypassing udev, in the launch options:

  ```
  SDL_JOYSTICK_DISABLE_UDEV=1 %command%
  ```

- **Turn Steam Input off** for the game (Properties > Controller > Disable Steam
  Input), so the game reads the real pad directly.

If the buttons are mapped wrong, give SDL the right mapping in
`SDL_GAMECONTROLLERCONFIG`. For example, the GameSir G7 SE (USB `3537:1010`):

```
SDL_JOYSTICK_DISABLE_UDEV=1 SDL_GAMECONTROLLERCONFIG="03000000373500001010000033060000,GameSir G7 SE,a:b0,b:b1,x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,back:b6,start:b7,guide:b8,leftstick:b9,rightstick:b10,leftx:a0,lefty:a1,lefttrigger:a2,rightx:a3,righty:a4,righttrigger:a5,dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,platform:Linux," %command%
```

To find a pad's GUID and button numbers: `evtest` on the host, or SDL's
`controllermap`. To check what Wine sees, run the game's prefix's
`joy.cpl`:

```sh
WINEPREFIX=~/.local/share/Steam/steamapps/compatdata/APPID/pfx GAMEID=0 arch-chroot umu-run joy.cpl
```

If `joy.cpl` lists the pad under XInput with every button right but the game
still mixes them up, the game itself is at fault. Older ports often read
DirectInput with a fixed layout. Remap in the game's own settings.

## Fonts

Programs in the tree use the tree's fonts, not the host's. `arch-setup`
installs the Source Han Sans fonts for Japanese, Korean and Chinese, which
Steam's store and many games need. On an older tree:

```sh
arch-chroot pacman -S adobe-source-han-sans-jp-fonts adobe-source-han-sans-kr-fonts adobe-source-han-sans-cn-fonts
```

More fonts for in-game text: `noto-fonts`, `ttf-liberation` (metric
replacements for Arial and Times), `wqy-zenhei`.

## Shader cache

Mesa compiles each game's shaders on first use and caches them in
`~/.cache/mesa_shader_cache`. A shader that is not cached has to be compiled
in the middle of play, which shows as a stutter. Mesa's default cache is 1 GB,
and one Proton game can need 1-3 GB, so games would evict each other's
shaders. `arch-chroot` raises the limit to 10 GB. Steam's own pre-caching
(Settings > Downloads > Shader Pre-Caching) adds Valve's per-game caches on top.

## Space

- Proton prefixes: `~/.local/share/Steam/steamapps/compatdata/APPID`, often
  several hundred MB each, left behind by uninstalled games.
- Shader caches: `~/.local/share/Steam/steamapps/shadercache`, `~/.cache/mesa_shader_cache`.
- The tree's package cache: `arch-chroot update` cleans it.

## Problems

| Message | |
|---|---|
| `bwrap: Can't create file at /media` | an old tree without `/mnt` or `/media`; any root command makes them: `arch-chroot root true` |
| `Fontconfig warning: ... newer version` | an old `arch-chroot`; the current one gives the tree its own cache. Re-run `tools/arch-setup` to install it |
| `pacman: error: could not remove download-XXXX` | leftovers of an interrupted download: `doas rm -rf /opt/root.x86_64/var/cache/pacman/pkg/download-*` |
| `unable to use parent for game drive` | Proton's notice that a game shares its folder with others; harmless. Keep each game in a folder of its own |
| `open /dev/snd/seq failed` | no MIDI: the kernel has no ALSA sequencer. Harmless |
| `ERROR: ld.so: object '...libmimalloc...' cannot be preloaded` | `LD_PRELOAD` got into the tree around `arch-chroot`; start tree programs through it |
| signature or keyring errors on update | `arch-chroot update` updates the keyring first; if that fails too, `arch-chroot root pacman-key --populate archlinux` |

## Why not...

- **Flatpak Steam**: it works on musl and has its own sandbox. But it needs
  Flatpak with its portals, D-Bus and system helper, and it pulls a large
  runtime of its own. The Arch tree is one folder and one script.
- **A container engine (Docker, Podman, Distrobox)**: more moving parts and a
  daemon, for what bubblewrap does alone.
- **gcompat**: a thin glibc shim for musl. It covers simple programs, not Steam,
  Proton and their 32-bit side.
- **steam-container** (`~whynothugo/steam-container`) is the closest relative.
  It does the same with bubblewrap, but keeps the tree in your home owned by
  you, bootstraps without a checksum, uses Valve's SteamOS repositories and
  always runs Steam inside gamescope. Its `NoExtract` list and CJK fonts were
  adopted here.
