# Desktop

A Wayland desktop on dwl, with PipeWire for sound and seatd for seat access.
No systemd, elogind, logind, polkit or udev.

## Install

```sh
doas apk add linux-firmware-amdgpu libudev-zero@busylinux \
    mesa-dri-gallium mesa-va-gallium mesa-vulkan-ati vulkan-loader \
    dbus seatd pipewire pipewire-alsa pipewire-pulse wireplumber \
    dwl@busylinux gammastep@busylinux pipemixer@busylinux cclip@busylinux foot \
    xdg-desktop-portal xdg-desktop-portal-gtk font-noto
```

| | |
|---|---|
| `linux-firmware-amdgpu` | the GPU's firmware; amdgpu needs it to start |
| `libudev-zero@busylinux` | libudev for wlroots, libinput, Mesa, PipeWire (below) |
| `mesa-*`, `vulkan-loader` | OpenGL, VA-API video decoding, RADV Vulkan |
| `dbus`, `seatd` | the system bus and seat access; their services start by themselves within a minute |
| `pipewire*`, `wireplumber` | sound, with ALSA and PulseAudio programs too |
| `dwl` and the other `@busylinux` packages | built from this repository (below) |
| `xdg-desktop-portal*` | file pickers and screen sharing for Firefox and Flatpak |
| `font-noto` | fonts for Latin, Greek, Cyrillic, Arabic and more; **not** CJK (see [Fonts](#fonts)) |

The installer already put your user in `video input audio seat`.

## Start it

Log in on tty1 and run:

```sh
dbus-run-session dwl -s 'pipewire & pipewire-pulse & wireplumber &
    /usr/libexec/xdg-desktop-portal & gammastep & cclipd'
```

Or put the `-s` part in `~/.config/dwl/autostart.sh` and start
`dbus-run-session dwl -s ~/.config/dwl/autostart.sh`. To start it on login to
tty1, add to `~/.profile`:

```sh
[ "$(tty)" = /dev/tty1 ] && exec dbus-run-session dwl -s ~/.config/dwl/autostart.sh
```

PipeWire runs as your user, started by dwl, not as a service: it belongs to
your session. It gets realtime priority without rtkit because the tty
logins allow it (`/etc/inittab`).

## dwl

`pkgs/dwl` is dwl 0.9 with Xwayland and these patches (`pkgs/dwl/files/`):

| Patch | |
|---|---|
| `bar` + `bar-systray` | a bar with tags, layout, title, status text and a tray |
| `simple_scratchpad` | hide windows in a scratchpad and bring them back |
| `warpcursor` | the pointer jumps to the window that gets focus |
| `cursortheme` | sets the cursor theme and size, for dwl and what it starts |
| `attachbottom` | new windows open after the others, not as the main one |
| `droptiled` | a tiled window moved with Super+drag goes back into the tiling where you drop it |
| `alwayscenter` | floating windows open centred |
| `monrule-tags` | a monitor rule sets the tag each monitor starts on |
| `keybindings-first-layout` | bindings always use the first layout (US), so Super+T still works while Arabic is active |

The configuration is `pkgs/dwl/files/config.h`. Change it, then
`doas tools/update.sh dwl`, and restart dwl.

### Keys (Super is the modifier)

| Keys | |
|---|---|
| Super+T | terminal (`footclient`; start `foot --server` in autostart) |
| Super+Space | launcher (fuzzel) |
| Super+B | Firefox |
| Super+M | `fff` file manager in a terminal |
| Super+V | clipboard history (cclip, picked with fuzzel) |
| Super+R | restart the bar's status text |
| Super+Q | close the window |
| Super+Left / Right | focus the previous / next window |
| Super+Return | swap the focused window with the main one |
| Super+Ctrl+Left / Right | focus the monitor to the left / right |
| Super+Shift+Left / Right | send the window to that monitor |
| Super+F / Super+Shift+F | fullscreen / monocle |
| Super+S / Super+Shift+S | send to / show the scratchpad |
| Super+1, 2 | view a tag; with Shift, move the window there; with Ctrl, toggle |
| Super+P / Super+Ctrl+P | screenshot of a region / the screen (`~/.config/dwl/screenshot.sh`) |
| Super+drag | move (left button), resize (right), float (middle) |
| Ctrl+Alt+E | quit dwl |
| Ctrl+Alt+F1-F12 | switch console |

Caps Lock switches between the US and Arabic layouts. The monitors are
HDMI-A-1 on the left and DP-1 on the right. Set refresh rates with
`wlr-randr` in autostart, e.g. `wlr-randr --output DP-1 --mode 1920x1080@165`.

### X11 programs

Xwayland is built in and starts on demand, so Steam, Wine and other X11
programs just work.

## Other programs from this repository

- **gammastep**: warmer screen colours in the evening. Built Wayland-only,
  without GeoClue, a GUI or translations, so give it a location in
  `~/.config/gammastep/config.ini`:

  ```ini
  [general]
  location-provider=manual
  [manual]
  lat=24.7
  lon=46.7
  ```
- **pipemixer**: a PipeWire volume mixer in the terminal.
- **cclip**: clipboard history, kept in SQLite. `cclipd` watches the clipboard;
  `cclip list`, `cclip copy ID`.

## Devices

There is no udev. `rcS` runs **mdevd** (skarnet's netlink device manager)
with `/etc/mdev.conf`, which names device nodes, sets their owners and modes,
and loads modules for new hardware.

Many programs (wlroots, libinput, Mesa, PipeWire) want **libudev** to find
devices. **libudev-zero** provides it without udev: it reads `/sys` and `/dev`
directly, and learns about hotplugged devices from mdevd, which passes each
event on (`-O 4`), as Alice Linux does. This repository's build of
libudev-zero also marks sound cards as initialised, which PipeWire waits for;
Alpine's build leaves them invisible to PipeWire.

`rcS` sets the modes mdevd does not: `/dev/ntsync` for everyone (Wine) and
`/dev/uinput` for group `input` (Steam Input).

For a custom rule, edit `/etc/mdev.conf` (apk keeps your version), e.g. for
ROCm's `/dev/kfd`:

```
kfd root:video 0660
```

## Sound

PipeWire with WirePlumber, through `pipewire-alsa` and `pipewire-pulse` for
ALSA and PulseAudio programs. The board's ALC4080 is a USB audio device, and
it crackles with buffers under 1024 samples (21 ms at 48 kHz). So
`/etc/pipewire/pipewire.conf.d/10-busylinux.conf` sets
`default.clock.min-quantum = 1024`. On other hardware, remove that file for
lower latency.

The kernel has no ALSA sequencer, so programs that look for MIDI print
`open /dev/snd/seq failed`. That is harmless.

## Fonts

`font-noto` covers most scripts but **not Chinese, Japanese or Korean**.
Without a CJK font, those show as boxes or as glyphs from whatever font happens
to cover them. Add one:

```sh
doas apk add font-noto-cjk
```

Or use any CJK font family in `~/.local/share/fonts`. A family that comes in
regional variants (Sarasa Gothic, Source Han Sans, Noto Sans CJK as separate
files) needs the right variant per language. The same character is drawn
differently in Japanese, Simplified Chinese, Traditional Chinese and Korean, and
the punctuation sits differently. Fontconfig can pick the variant from the
text's language, which browsers pass on from the page's `lang`:

```xml
<!-- after your sans-serif alias, e.g. in /etc/fonts/conf.d/29-user-fonts.conf -->
<match target="pattern">
  <test name="lang" compare="contains"><string>ja</string></test>
  <test name="family"><string>Sarasa Gothic SC</string></test>
  <edit name="family" mode="assign"><string>Sarasa Gothic J</string></edit>
</match>
```

Repeat it for `ko` (K), `zh-tw` (TC) and `zh-hk` (HC). Check with
`fc-match -s 'sans-serif:lang=ja' | grep -m1 -i sarasa`.

A default weight of Medium instead of Regular, keeping real bold:

```xml
<match target="pattern">
  <test name="weight" compare="eq"><const>regular</const></test>
  <edit name="weight" mode="assign"><const>medium</const></edit>
</match>
<match target="pattern">
  <!-- no weight asked: "all" holds for an empty list -->
  <test name="weight" qual="all" compare="eq"><int>-1</int></test>
  <edit name="weight" mode="assign"><const>medium</const></edit>
</match>
```

Fonts inside the Arch tree are separate; see
[Steam and games](Steam-and-Games.md#fonts).
