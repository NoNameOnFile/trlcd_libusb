## [0.3.0] - 2025-10-17
### Added
- **Video background support** via FFmpeg (MP4, AVI, MOV, etc.)
- Video loop controls: `video_loop=infinite|once|default|N`
- Video playback controls: `video_speed`, `video_start_ms`
- FFmpeg library dependencies (libavformat, libavcodec, libavutil, libswscale)
- Updated build system to include FFmpeg libraries
- Enhanced configuration system for video backgrounds
- **New live tokens**:
  - `%FAN_RPM%` - Fan RPM from hwmon sensors
  - `%FAN_SPEED%` - Fan speed percentage from PWM
  - `%CPU_FREQ%` - Current CPU frequency in GHz
  - `%UPTIME%` - System uptime in human-readable format
  - `%LOAD_AVG%` - 1-minute load average
- **Comprehensive Makefile** with targets for building, installing, testing, and distribution

### Fixed
- **CPU Temperature Reading**: Fixed incorrect 168°C readings - now properly reads from k10temp sensors
- **Video Performance**: Improved adaptive FPS reduction with gradual steps (24→16→12→8→6 FPS)
- **Memory Usage**: Reduced video cache threshold from 10s to 8s, added frame skipping for long videos
- **USB Bandwidth**: More sensitive detection and better handling of USB transmission issues
- **Process Priority**: Added nice value of 10 for background operation to improve system responsiveness
- **Code Quality**: Removed unused functions and fixed all compilation warnings

### Optimized
- **Video Caching**: Intelligent frame skipping (skip every 2nd/3rd frame for long videos)
- **Performance Monitoring**: Earlier detection of performance issues (1.3x vs 1.5x threshold)
- **USB Handling**: More sensitive bandwidth monitoring (1.15x vs 1.2x threshold)
- **Buffer Management**: Reduced memory allocations and improved buffer reuse

## [0.2.0] - 2025-08-29
### Added
- Installer (`install.sh`) for user/system service, udev rule.
- APNG: lenient per-frame decode (LodePNG + stb_image fallback).
### Fixed
- USB retry/reopen paths more robust.
### Notes
- Assets must be in the service working dir (user: `~/.config/trlcd/`, system: `/etc/trlcd/`), or use absolute paths.
