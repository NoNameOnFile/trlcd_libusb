# trlcd_libusb ThermalRight AIO Cooler LCD driver


### What's new
- **v0.3.0**: Video background support via FFmpeg, fixed CPU temperature reading (168°C bug), optimized video playback performance
- **Process Priority**: Runs with nice value 10 for better system responsiveness
- **Video Optimizations**: Intelligent frame skipping, adaptive FPS reduction, improved USB handling
- **Installer**: `./install.sh --user` or `sudo ./install.sh --system`
- **APNG**: per-frame decode is tolerant (LodePNG first, stb_image fallback)
- **USB**: better recovery (soft clear → reset → reopen)
- **Docs**: clarified asset locations for services

### Upgrade
User service:
```bash
./install.sh --user
systemctl --user restart trlcd
sudo ./install.sh --system
sudo systemctl restart trlcd
```

Tiny single-file C program that composites PNG/APNG/Video layers + text (with alpha) into RGB565 and streams frames to a 240×320 USB LCD (VID:PID **0416:5302**) over **libusb**.

- Background PNG/APNG/Video with transparency
- Movable background (center or explicit x/y)
- Image layers (PNG, APNG, alpha)
- Color overlays (RGBA)
- Text with TTF fonts
  - Global **portrait/landscape** orientation, **CW/CCW** direction, optional **flip**
  - **Per-text overrides** (make one label vertical, keep others normal)
- Live tokens: **`%CPU_TEMP%`**, **`%CPU_USAGE%`**, **`%MEM_USED%`**, **`%MEM_FREE%`**, **`%GPU_TEMP%`**, **`%GPU_USAGE%`**, **`%TIME%`**, **`%DATE%`**, **`%FAN_RPM%`**, **`%FAN_SPEED%`**, **`%CPU_FREQ%`**, **`%UPTIME%`**, **`%LOAD_AVG%`**
- Large off-screen framebuffer + viewport (easy clipping / future animations)
- Robust USB sender with auto-retry/reset/reopen
- **Video background support** via FFmpeg (MP4, AVI, MOV, etc.)

### Performance Features
- **Adaptive FPS**: Automatically reduces frame rate (24→16→12→8→6 FPS) based on system performance
- **Intelligent Caching**: Short videos (<8s) cached in RAM, long videos use frame skipping
- **USB Bandwidth Monitoring**: Detects USB transmission issues and adjusts accordingly
- **Process Priority**: Runs with nice value 10 to avoid interfering with system processes
- **Memory Optimization**: Frame skipping for long videos (50-67% memory reduction)
- **CPU Temperature**: Accurate readings from k10temp sensors (fixed 168°C bug)

> **Repo contents**
>
> - `trlcd_libusb.c` – main program (single file)
> - `stb_image.h` – vendored image loader (PNG)
> - `layout.cfg` – example configuration

---

## Hardware

- Designed for 240×320 LCD at USB **0416:5302**.
- If your panel differs, you may need to adjust `W/H` and the header in `build_header_fixed()`.

---

## Requirements

- Linux
- GCC or Clang
- `libusb-1.0` development headers
- `FFmpeg` development libraries (libavformat, libavcodec, libavutil, libswscale)
- `stb_image.h` (included in this repo)

Install libusb dev package:

**Debian/Ubuntu**
```bash
sudo apt-get update
sudo apt-get install -y libusb-1.0-0-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev
```

**Fedora/RHEL**
```bash
sudo dnf install libusb1-devel ffmpeg-devel
```

**Arch**
```bash
sudo pacman -S libusb ffmpeg
```

**openSUSE**
```bash
sudo zypper install libusb-1_0-devel ffmpeg4-devel
```

---

## Build

### Using Makefile (Recommended)

```bash
make                    # Build binary
make check-deps         # Check dependencies
make install            # Install system-wide
make install-user       # Install for user
make help               # Show all available targets
```

### Manual Build

```bash
gcc -O2 -Wall trlcd_libusb.c lodepng.c -lusb-1.0 -lavformat -lavcodec -lavutil -lswscale -lm -o trlcd_libusb
```

> The installer script (`install.sh`) will automatically install dependencies and build the binary.

### Makefile Features

The included Makefile provides convenient targets for:

- **Building**: `make` (default), `make debug`, `make release`
- **Dependencies**: `make check-deps`, `make install-deps`, `make install-runtime-deps`
- **Installation**: `make install` (system), `make install-user` (user)
- **Services**: `make service-files`, `make udev-rules`
- **Testing**: `make test`
- **Distribution**: `make dist` (creates tar.gz package)
- **Cleanup**: `make clean`, `make uninstall`

---

## (Optional) udev rule (run without sudo)

Create `/etc/udev/rules.d/99-trlcd.rules` with:

```
SUBSYSTEM=="usb", ATTR{idVendor}=="0416", ATTR{idProduct}=="5302", MODE="0666"
```

Then reload:

```bash
sudo udevadm control --reload-rules
sudo udevadm trigger
# unplug/replug device
```

---

## Run

Put a `background.png` next to the binary, edit `layout.cfg` (see below), then:

```bash
./trlcd_libusb
```

- Set `fps>0` and `once=0` in `layout.cfg` to continuously refresh (live tokens update).

---

## Configuration (`layout.cfg`)

A minimal, working example:

```ini
# Required background (PNG, APNG, or video)
background_png=background.png
# OR for video backgrounds:
# background_video=background.mp4

# Move / align background (number or "center"). Optional 180° flip.
background_x=center
background_y=center
background_flip=0

# Video background controls (if using background_video)
video_speed=1.0
video_start_ms=0
video_loop=infinite

# Big off-screen framebuffer (as % of panel) and viewport crop
fb_scale_percent=150
viewport_x=center
viewport_y=center

# Global text UI orientation (per-text blocks can override)
text_orientation=portrait     # portrait|landscape
text_landscape_dir=cw         # when landscape: cw|ccw
text_flip=0                   # 0|1 (180° after orientation)

# Streaming
fps=0        # 0 = send once; >0 = loop (good for live tokens)
once=1       # set 0 to keep sending frames while fps>0
iface=-1     # -1 = auto-pick USB interface
debug=0

# Semi-transparent footer bar (logical UI coords; rotates with text UI)
[overlay]
rect=0,250,240,70
color=0,0,0,128

# Live CPU line (inherits global orientation)
[text]
text=CPU %CPU_TEMP%  •  %CPU_USAGE%
x=12
y=262
color=255,255,255,255
scale=2

# Vertical label example (per-text overrides)
[text]
text=RPM
x=2
y=40
color=255,255,255,255
scale=2
orientation=landscape       # per-text: portrait|landscape|inherit
landscape_dir=ccw           # per-text: cw|ccw|inherit
flip=inherit                # per-text: 0|1|inherit

# Optional PNG overlay layer (absolute FB coords)
[image]
path=overlay1.png
x=12
y=40
alpha=255
scale=1.0
```

### Video Background Support

Video backgrounds are supported via FFmpeg and can use common formats like MP4, AVI, MOV, etc.

**Video Loop Modes:**
- `video_loop=infinite` - Loop forever (default)
- `video_loop=once` - Play once and stop
- `video_loop=default` - Use file's loop setting
- `video_loop=N` - Loop N times (where N is a number)

**Video Controls:**
- `video_speed=1.0` - Playback speed multiplier (1.0 = normal, 2.0 = double speed, 0.5 = half speed)
- `video_start_ms=0` - Start time offset in milliseconds

### Tokens

- `%CPU_TEMP%` → like `42°C`. Reads Linux sensors from `/sys/class/thermal` and `/sys/class/hwmon`. If none available, prints `N/A`.
  - If missing: load kernel modules:
    - Intel: `sudo modprobe coretemp`
    - AMD:   `sudo modprobe k10temp`
- `%CPU_USAGE%` → like `37%`. Computed from `/proc/stat` deltas. On single-shot (`fps=0`) it takes a tiny (~60 ms) sample for a meaningful value.
- `%FAN_RPM%` → like `1200 RPM`. Reads fan RPM from `/sys/class/hwmon/` sensors. Shows highest RPM found.
- `%FAN_SPEED%` → like `65%`. Reads PWM fan speed percentage from `/sys/class/hwmon/` sensors.
- `%CPU_FREQ%` → like `3.20 GHz`. Current CPU frequency from `/proc/cpuinfo`.
- `%UPTIME%` → like `2d 5h 30m` or `3h 45m` or `25m`. System uptime in human-readable format.
- `%LOAD_AVG%` → like `1.25`. 1-minute load average from `/proc/loadavg`.

### Orientation & overrides

- Global: `text_orientation`, `text_landscape_dir`, `text_flip`
- Per-text (optional): `orientation=portrait|landscape|inherit`, `landscape_dir=cw|ccw|inherit`, `flip=0|1|inherit`
  - If a per-text key is present and **not** `inherit`, it overrides the global.

### Background positioning

- `background_x` / `background_y` accept either `center` or a signed pixel offset (can be negative) in the large framebuffer.

### Large framebuffer & viewport

- `fb_scale_percent` grows the internal canvas (default 150% of panel). `viewport_x/y` picks which window of that canvas gets sent.
- Great for manual alignment now and simple animations later.

---

## Troubleshooting

- **“device not found” or needs sudo** → add the **udev rule** above, replug.
- **HTTPS password prompts** when pushing to GitHub → use SSH remotes (`git@github.com:...`) or a Personal Access Token.
- **`stb_image.h: No such file or directory`** → the repo includes it; if missing, drop it next to the source:
  ```bash
  wget https://raw.githubusercontent.com/nothings/stb/master/stb_image.h
  ```
- **Landscape looks wrong** → switch `text_landscape_dir` between `cw` and `ccw`, or add a per-text override; try `text_flip=1` if panel is mounted upside-down.
- **Text ignores global orientation** → per-text overrides take precedence. Set `orientation=inherit` (or remove the key) in that `[text]` block.

---

## License

- This project: GNU GENERAL PUBLIC LICENSE
- `stb_image.h`: Public Domain / GNU GENERAL PUBLIC LICENSE (see its header)

---

## Contributing

PRs welcome! Ideas:
- UI-mapped PNG layers (apply the same portrait/landscape transform as text)
- Larger fonts (e.g., via `stb_truetype`)
- More tokens: `%MEM_USED%`, `%GPU_TEMP%`, `%NET_UP%/%NET_DOWN%`
- Simple animation helpers using the big framebuffer + viewport
