# Desktop

A Wayland desktop on dwl, with PipeWire for sound and seatd for seat access.
No systemd, elogind, logind, polkit or udev.

## Install

```sh
doas apk add linux-firmware-amdgpu libudev-zero@busylinux \
    mesa-dri-gallium mesa-va-gallium mesa-vulkan-ati vulkan-loader \
    dbus seatd pipewire pipewire-alsa pipewire-pulse pipewire-jack wireplumber \
    dwl@busylinux gammastep@busylinux pipemixer@busylinux cclip@busylinux foot \
    lbsutils@busylinux wlr-randr \
    xdg-desktop-portal xdg-desktop-portal-gtk xdg-desktop-portal-wlr \
    busylinux-fonts@busylinux busylinux-themes@busylinux \
    '!dbus-daemon-launch-helper' '!xdg-document-portal'
```

| | |
|---|---|
| `linux-firmware-amdgpu` | the GPU's firmware; amdgpu needs it to start |
| `libudev-zero@busylinux` | libudev for wlroots, libinput, Mesa, PipeWire (below) |
| `mesa-*`, `vulkan-loader` | OpenGL, VA-API video decoding, RADV Vulkan |
| `dbus`, `seatd` | the system bus and seat access; their services start by themselves within a minute |
| `pipewire*`, `wireplumber` | sound, with ALSA, PulseAudio and JACK programs too |
| `dwl` and the other `@busylinux` packages | built from this repository (below) |
| `wlr-randr` | monitor modes and layout (see [dwl](#dwl)) |
| `xdg-desktop-portal*` | file pickers (`-gtk`) and screen sharing (`-wlr`) for Firefox |
| `!dbus-daemon-launch-helper`, `!xdg-document-portal` | keep two setuid helpers out (below) |
| `busylinux-fonts` | the fonts and their fontconfig setup (see [Fonts](#fonts)) |
| `busylinux-themes` | the GTK theme, icons and cursors (see [Themes](#themes)) |

`pipewire-jack` is named, not left to apk: JACK2's `jack` provides
`libjack.so.0` too, with a higher version, so an upgrade can swap it in and
leave JACK programs (portaudio, fluidsynth) talking to a `jackd` that never
runs. `doctor` warns when that happens.

`dbus` and `xdg-desktop-portal` pull in two setuid root helpers through
`install_if`, so `apk del` alone does not keep them away; the `!` entries in
the world file do. `dbus-daemon-launch-helper` starts system-bus services on
demand, and this system has none. `xdg-document-portal` shares files with
sandboxed (Flatpak) programs through FUSE and needs `fuse3`'s setuid
`fusermount3`. Without them `doas` stays the only setuid program; `doctor`
warns about any other. To use Flatpak, `doas apk del '!xdg-document-portal'`.

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
| Super+M | `file-manager` in a terminal (from `lbsutils`) |
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
| Super+P | `screenshot`: drag a region or click a monitor, Esc or right click cancels; saved in `~/Downloads` and copied to the clipboard |
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
- **lbsutils** (`apk add lbsutils@busylinux`): `file-manager`, a terminal file
  manager (`FILE_MANAGER_HIDDEN`, `FILE_MANAGER_PREVIEW` and
  `FILE_MANAGER_OPENER` change its defaults); `viewer -p file.pdf` or
  `-i image`, a PDF and image viewer that speaks Wayland itself, with arrow keys, Space, Home and End to turn pages and `q` to
  quit; and `extract [-C dir] archive...`, which extracts zip, 7z, rar and tar
  (compressed tars too), refuses entries that would land outside the target,
  and asks for a zip's password when it needs one; `screenshot [-c] [file | -]`,
  which freezes the screen, lets you drag a region or click a monitor, and
  writes a PNG (the pointer is left out). With `-c` it also offers the PNG as
  the clipboard, through the same data-control protocol cclipd watches, and
  serves it from the background until something else is copied: no
  `wl-copy` needed. It needs only zlib and the compositor's screencopy,
  layer-shell and data-control support, all of which dwl has.
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

`busylinux-fonts@busylinux` installs a set of fonts into
`/usr/share/fonts/busylinux` and a fontconfig setup,
`/etc/fonts/conf.d/29-busylinux-fonts.conf`, that puts them in order:

| Font | For |
|---|---|
| **Inter** | sans-serif text (and serif: one family everywhere) |
| **Fira Code** | monospace, with ligatures |
| **Vazirmatn** | Arabic and Persian |
| **OpenMoji Color** | emoji |
| **Sarasa Gothic** / **Sarasa Mono** | Chinese, Japanese and Korean (five upright weights) |
| **Symbols Nerd Font** | icons in terminals and bars |
| **Unifont** | anything none of the others cover |

The setup:

- **Fallback order**: a character Inter lacks comes from Vazirmatn, then
  OpenMoji, Sarasa, the Nerd symbols and Unifont, in that order. Monospace is
  the same with Fira Code and Sarasa Mono.
- **Medium by default**: text that asks for normal weight gets Medium, which
  reads better on a 1080p screen. Bold, semibold and light stay as asked.
- **CJK by language**: the same character is drawn differently in Japanese,
  Simplified and Traditional Chinese and Korean, and the punctuation sits
  differently. Text marked as Japanese (a page's `lang`, which browsers pass
  on) gets Sarasa's J variant, Korean K, Taiwanese TC, Hong Kong and Macau HC,
  and everything else SC.
- **Emoji in colour, text never in emoji**: fontconfig ranks colour fonts
  above any list, which made OpenMoji draw digits and spaces too. Text
  therefore asks for plain fonts, OpenMoji keeps only its emoji (its ASCII is
  dropped) and Unifont gives up its black-and-white emoji, so emoji fall
  through to OpenMoji and everything else comes from the fonts above it. The
  lists are strong preferences, so a font claiming a language (Unifont claims
  nearly all) cannot jump ahead of them.
- **Inter's features**: `dlig` (arrows from `->` and the like), `zero`
  (slashed zero), `ss01` (open digits), `ss02` (I, l and 1 told apart), `ss07`
  and `ss08` (square punctuation and quotes), with `calt` and `ccmp`, which are
  on anyway. GTK programs (through Pango) and foot read them from fontconfig;
  Firefox and Chromium do not. `frac`, `case`, `sups`, `subs`, `sinf`, `numr`
  and `dnom` are left off: they are meant for a selected run of text, and
  turned on for everything they would make every `1/2` and date a fraction,
  raise punctuation to capital height in lowercase text, or turn all digits
  (and with `sups`, letters) into small raised or lowered ones.
- **Fira Code's variants**: `cv01` (a), `cv06` (i), `ss01` (r), `zero` (dotted 0),
  `cv18` (%), `cv31` (parentheses), `ss02` (`<=` `>=`), `cv24` (`/=`), `cv25`
  (`.-`), `cv26` (`:-`), `cv32` (`.=`) and `ss07` (`=~` `!~`), read by the same
  programs as Inter's.
- **Sarasa Gothic's `halt`**: half-width CJK punctuation (`「」、。`), with
  kana and kanji at full width. `palt` (proportional kana) is left off: it
  halves the punctuation too, and both would halve it to nothing. Sarasa Mono
  keeps its grid.
- **Rendering**: antialiasing, slight hinting, RGB subpixel order.

Check it:

```sh
fc-match sans-serif family style                 # Inter: Medium
fc-match sans-serif:bold family style            # Inter: Bold
fc-match -s 'sans-serif:lang=ja' | grep -m1 -i sarasa   # Sarasa Gothic J
```

The file is in `/etc`, so your edits survive updates (a new version lands
beside it as `.apk-new`). To start over with your own fonts instead,
`apk del busylinux-fonts` and put them in `~/.local/share/fonts`. Without it,
`font-noto` covers most scripts but not CJK; `font-noto-cjk` adds that.

The recipe pins each font's version and checksum, and keeps only what the
setup uses. Sarasa's italic files are left out, which saves 430 MB: they
slant only Sarasa's Latin letters, and Inter and Fira Code supply those. Fira
Code's variable font is left out too, so its fixed weights win. The package is
about 440 MB installed, nearly all of it Sarasa's five weights.

Fonts inside the Arch tree are separate; see
[Steam and games](Steam-and-Games.md#fonts).

## Themes

`busylinux-themes@busylinux` installs one look for GTK programs and the
pointer, all in the Gruvbox colours that dwl's bar and borders use:

| Theme | What |
|---|---|
| **Colloid-Green-Dark-Gruvbox** | the GTK 3 and 4 theme, as it came |
| **Papirus-Gruvbox-Dark** (and **Papirus-Gruvbox**, which it builds on) | Papirus's icons, recoloured |
| **Breeze-Gruvbox** | Breeze Light's cursors, recoloured |

The two forks are made at build time by `pkgs/busylinux-themes/files/recolour.py`
from pinned upstream releases:

- **Recoloured**: Papirus's blue folders take the theme's green
  (`#98971a`); its red, green, blue, orange and yellow, its greys and the
  symbolic icons' text colour move to their Gruvbox counterparts. The cursors
  are cream (`#ebdbb2`) with a dark outline (`#1d2021`) instead of white and
  black, and their accents (the busy spinner, the help and copy badges) are
  Gruvbox's too.
- **Trimmed**: Papirus-Light, the other 24 folder colours, and the sizes
  other than 16, 22, 32 and 48 px are left out. The 24 px set is only the
  22 px icons with a margin, so programs that ask for 24 get the 22 px ones
  scaled; HiDPI uses the same SVGs. The cursors keep 24, 36 and 48 px (1x,
  1.5x and 2x) of eleven sizes, and drop the SVG cursors that nothing here reads.
  Papirus's files go from 113 MB to 61 MB, the cursors' from 15 MB to 3 MB.

dwl uses Breeze-Gruvbox itself and exports it as `XCURSOR_THEME`.
`/etc/xdg/gtk-3.0/settings.ini` and `/etc/xdg/gtk-4.0/settings.ini` make all three
the GTK defaults. Your own `~/.config/gtk-3.0/settings.ini` and
`~/.config/gtk-4.0/settings.ini` override them, so remove the theme lines there
if they name the old themes. Programs built on libadwaita ignore GTK themes
and keep their own look.

Check it:

```sh
grep -h name /etc/xdg/gtk-3.0/settings.ini
ls /usr/share/icons/Breeze-Gruvbox/cursors | wc -l    # 115
```
