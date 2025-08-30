## [0.2.0] - 2025-08-29
### Added
- Installer (`install.sh`) for user/system service, udev rule.
- APNG: lenient per-frame decode (LodePNG + stb_image fallback).
### Fixed
- USB retry/reopen paths more robust.
### Notes
- Assets must be in the service working dir (user: `~/.config/trlcd/`, system: `/etc/trlcd/`), or use absolute paths.
