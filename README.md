# trlcd_libusb

Tiny single-file C program that composites PNG layers + text (with alpha) into RGB565 and streams frames to a 240×320 USB LCD (VID:PID **0416:5302**) over **libusb**.

- Background PNG with transparency
- Movable background (center or explicit x/y)
- Image layers (PNG, alpha)
- Color overlays (RGBA)
- Text with a small 5×7 bitmap font
  - Global **portrait/landscape** orientation, **CW/CCW** direction, optional **flip**
  - **Per-text overrides** (make one label vertical, keep others normal)
- Live tokens: **`%CPU_TEMP%`**, **`%CPU_USAGE%`**
- Large off-screen framebuffer + viewport (easy clipping / future animations)
- Robust USB sender with auto-retry/reset/reopen

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
- `stb_image.h` (included in this repo)

Install libusb dev package:

**Debian/Ubuntu**
```bash
sudo apt-get update
sudo apt-get install -y libusb-1.0-0-dev
```

**Fedora/RHEL**
```bash
sudo dnf install libusb1-devel
```

**Arch**
```bash
sudo pacman -S libusb
```

**openSUSE**
```bash
sudo zypper install libusb-1_0-devel
```

---

## Build

```bash
gcc -O2 -Wall trlcd_libusb.c -lusb-1.0 -o trlcd_libusb
```

> If you removed the `STBI_NO_LINEAR` define and see a linker error about `pow`, compile with `-lm`:
> ```bash
> gcc -O2 -Wall trlcd_libusb.c -lusb-1.0 -lm -o trlcd_libusb
> ```

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
# Required background image (PNG with transparency is OK)
background_png=background.png

# Move / align background (number or "center"). Optional 180° flip.
background_x=center
background_y=center
background_flip=0

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

### Tokens

- `%CPU_TEMP%` → like `42°C`. Reads Linux sensors from `/sys/class/thermal` and `/sys/class/hwmon`. If none available, prints `N/A`.
  - If missing: load kernel modules:
    - Intel: `sudo modprobe coretemp`
    - AMD:   `sudo modprobe k10temp`
- `%CPU_USAGE%` → like `37%`. Computed from `/proc/stat` deltas. On single-shot (`fps=0`) it takes a tiny (~60 ms) sample for a meaningful value.

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
