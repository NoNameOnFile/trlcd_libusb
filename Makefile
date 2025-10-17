# Makefile for trlcd_libusb - ThermalRight AIO Cooler LCD driver with video support
# Supports PNG/APNG/Video backgrounds via FFmpeg

# Project configuration
APP_NAME = trlcd
BIN_NAME = trlcd_libusb
VERSION = 0.3.0

# Compiler and flags
CC = gcc
CFLAGS = -O2 -Wall -Wextra -std=c99
LDFLAGS = -lusb-1.0 -lavformat -lavcodec -lavutil -lswscale -lm

# Source files
SRC_MAIN = trlcd_libusb.c
SRC_LODE = lodepng.c
HDR_STBI = stb_image.h
HDR_STBT = stb_truetype.h
HDR_LODE = lodepng.h

# Installation paths
PREFIX = /usr/local
BINDIR = $(PREFIX)/bin
SYSCONFDIR = /etc/$(APP_NAME)
SYSTEMDDIR = /etc/systemd/system
UDEVDIR = /etc/udev/rules.d

# User installation paths
USER_BINDIR = $(HOME)/.local/bin
USER_CONFDIR = $(HOME)/.config/$(APP_NAME)
USER_SYSTEMDDIR = $(HOME)/.config/systemd/user

# USB device identifiers
VID_HEX = 0416
PID_HEX = 5302

# Default target
.PHONY: all
all: $(BIN_NAME)

# Build the main binary
$(BIN_NAME): $(SRC_MAIN) $(SRC_LODE) $(HDR_STBI) $(HDR_STBT) $(HDR_LODE)
	@echo "Building $(BIN_NAME)..."
	$(CC) $(CFLAGS) $(SRC_MAIN) $(SRC_LODE) $(LDFLAGS) -o $@
	@echo "Build complete: $@"

# Check dependencies
.PHONY: check-deps
check-deps:
	@echo "Checking build dependencies..."
	@command -v $(CC) >/dev/null 2>&1 || { echo "Error: $(CC) not found"; exit 1; }
	@echo '#include <libusb-1.0/libusb.h>' | $(CC) -H -o /dev/null 2>&1 | grep -q libusb || { \
		echo "Error: libusb-1.0 development headers not found"; \
		echo "Install with: sudo apt-get install libusb-1.0-0-dev"; \
		exit 1; \
	}
	@echo '#include <libavformat/avformat.h>' | $(CC) -H -o /dev/null 2>&1 | grep -q libavformat || { \
		echo "Error: FFmpeg development headers not found"; \
		echo "Install with: sudo apt-get install libavformat-dev libavcodec-dev libavutil-dev libswscale-dev"; \
		exit 1; \
	}
	@echo "All dependencies found"

# Install system-wide
.PHONY: install
install: $(BIN_NAME)
	@echo "Installing $(BIN_NAME) system-wide..."
	install -d $(BINDIR)
	install -m 755 $(BIN_NAME) $(BINDIR)/
	install -d $(SYSCONFDIR)
	install -m 644 layout.cfg $(SYSCONFDIR)/
	install -d $(SYSTEMDDIR)
	install -m 644 trlcd.service $(SYSTEMDDIR)/
	install -d $(UDEVDIR)
	install -m 644 99-$(APP_NAME).rules $(UDEVDIR)/
	@echo "System installation complete"
	@echo "Run 'sudo systemctl daemon-reload' to reload systemd"
	@echo "Run 'sudo udevadm control --reload-rules' to reload udev rules"

# Install for user
.PHONY: install-user
install-user: $(BIN_NAME)
	@echo "Installing $(BIN_NAME) for user..."
	install -d $(USER_BINDIR)
	install -m 755 $(BIN_NAME) $(USER_BINDIR)/
	install -d $(USER_CONFDIR)
	install -m 644 layout.cfg $(USER_CONFDIR)/
	install -d $(USER_SYSTEMDDIR)
	install -m 644 trlcd-user.service $(USER_SYSTEMDDIR)/
	@echo "User installation complete"
	@echo "Run 'systemctl --user daemon-reload' to reload user systemd"

# Uninstall system-wide
.PHONY: uninstall
uninstall:
	@echo "Uninstalling $(BIN_NAME)..."
	rm -f $(BINDIR)/$(BIN_NAME)
	rm -f $(SYSCONFDIR)/layout.cfg
	rm -f $(SYSTEMDDIR)/trlcd.service
	rm -f $(UDEVDIR)/99-$(APP_NAME).rules
	@echo "Uninstall complete"

# Uninstall user installation
.PHONY: uninstall-user
uninstall-user:
	@echo "Uninstalling $(BIN_NAME) for user..."
	rm -f $(USER_BINDIR)/$(BIN_NAME)
	rm -f $(USER_CONFDIR)/layout.cfg
	rm -f $(USER_SYSTEMDDIR)/trlcd-user.service
	@echo "User uninstall complete"

# Create systemd service files
.PHONY: service-files
service-files: trlcd.service trlcd-user.service

trlcd.service:
	@echo "Creating systemd service file..."
	@echo "[Unit]" > $@
	@echo "Description=ThermalRight AIO Cooler LCD Driver" >> $@
	@echo "After=graphical-session.target" >> $@
	@echo "Wants=graphical-session.target" >> $@
	@echo "" >> $@
	@echo "[Service]" >> $@
	@echo "Type=simple" >> $@
	@echo "User=root" >> $@
	@echo "ExecStart=$(BINDIR)/$(BIN_NAME)" >> $@
	@echo "WorkingDirectory=$(SYSCONFDIR)" >> $@
	@echo "Restart=always" >> $@
	@echo "RestartSec=5" >> $@
	@echo "StandardOutput=journal" >> $@
	@echo "StandardError=journal" >> $@
	@echo "" >> $@
	@echo "[Install]" >> $@
	@echo "WantedBy=multi-user.target" >> $@

trlcd-user.service:
	@echo "Creating user systemd service file..."
	@echo "[Unit]" > $@
	@echo "Description=ThermalRight AIO Cooler LCD Driver (User)" >> $@
	@echo "After=graphical-session.target" >> $@
	@echo "Wants=graphical-session.target" >> $@
	@echo "" >> $@
	@echo "[Service]" >> $@
	@echo "Type=simple" >> $@
	@echo "ExecStart=$(USER_BINDIR)/$(BIN_NAME)" >> $@
	@echo "WorkingDirectory=$(USER_CONFDIR)" >> $@
	@echo "Restart=always" >> $@
	@echo "RestartSec=5" >> $@
	@echo "StandardOutput=journal" >> $@
	@echo "StandardError=journal" >> $@
	@echo "" >> $@
	@echo "[Install]" >> $@
	@echo "WantedBy=default.target" >> $@

# Create udev rules file
.PHONY: udev-rules
udev-rules: 99-$(APP_NAME).rules

99-$(APP_NAME).rules:
	@echo "Creating udev rules file..."
	@echo "# ThermalRight AIO Cooler LCD Driver udev rules" > $@
	@echo "SUBSYSTEM==\"usb\", ATTR{idVendor}==\"$(VID_HEX)\", ATTR{idProduct}==\"$(PID_HEX)\", MODE=\"0660\", GROUP=\"plugdev\"" >> $@

# Test the binary
.PHONY: test
test: $(BIN_NAME)
	@echo "Testing $(BIN_NAME)..."
	@if [ -f layout.cfg ]; then \
		echo "Configuration file found"; \
	else \
		echo "Warning: layout.cfg not found"; \
	fi
	@if [ -f test_video.mp4 ]; then \
		echo "Test video found"; \
	else \
		echo "Warning: test_video.mp4 not found"; \
	fi
	@echo "Binary size: $$(stat -c%s $(BIN_NAME)) bytes"
	@echo "Test complete"

# Clean build artifacts
.PHONY: clean
clean:
	@echo "Cleaning build artifacts..."
	rm -f $(BIN_NAME)
	rm -f trlcd.service trlcd-user.service
	rm -f 99-$(APP_NAME).rules
	@echo "Clean complete"

# Create distribution package
.PHONY: dist
dist: clean check-deps $(BIN_NAME) service-files udev-rules
	@echo "Creating distribution package..."
	@DIST_DIR=$(APP_NAME)-$(VERSION) && \
	mkdir -p $$DIST_DIR && \
	cp $(BIN_NAME) $$DIST_DIR/ && \
	cp $(SRC_MAIN) $$DIST_DIR/ && \
	cp $(SRC_LODE) $$DIST_DIR/ && \
	cp $(HDR_STBI) $$DIST_DIR/ && \
	cp $(HDR_STBT) $$DIST_DIR/ && \
	cp $(HDR_LODE) $$DIST_DIR/ && \
	cp layout.cfg $$DIST_DIR/ && \
	cp install.sh $$DIST_DIR/ && \
	cp README.md $$DIST_DIR/ && \
	cp CHANGELOG.md $$DIST_DIR/ && \
	cp LICENSE $$DIST_DIR/ && \
	cp Makefile $$DIST_DIR/ && \
	cp .gitignore $$DIST_DIR/ && \
	cp trlcd.service $$DIST_DIR/ && \
	cp trlcd-user.service $$DIST_DIR/ && \
	cp 99-$(APP_NAME).rules $$DIST_DIR/ && \
	tar -czf $(APP_NAME)-$(VERSION).tar.gz $$DIST_DIR/ && \
	rm -rf $$DIST_DIR && \
	echo "Distribution package created: $(APP_NAME)-$(VERSION).tar.gz"

# Show help
.PHONY: help
help:
	@echo "ThermalRight AIO Cooler LCD Driver Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  all          - Build the binary (default)"
	@echo "  check-deps   - Check build dependencies"
	@echo "  install      - Install system-wide"
	@echo "  install-user - Install for current user"
	@echo "  uninstall    - Uninstall system-wide"
	@echo "  uninstall-user - Uninstall user installation"
	@echo "  service-files - Create systemd service files"
	@echo "  udev-rules   - Create udev rules file"
	@echo "  test         - Test the binary"
	@echo "  clean        - Clean build artifacts"
	@echo "  dist         - Create distribution package"
	@echo "  help         - Show this help"
	@echo ""
	@echo "Examples:"
	@echo "  make                    # Build binary"
	@echo "  make check-deps         # Check dependencies"
	@echo "  make install            # Install system-wide"
	@echo "  make install-user       # Install for user"
	@echo "  make clean              # Clean build files"

# Debug build
.PHONY: debug
debug: CFLAGS = -g -O0 -Wall -Wextra -std=c99 -DDEBUG
debug: $(BIN_NAME)

# Release build (default)
.PHONY: release
release: CFLAGS = -O2 -Wall -Wextra -std=c99 -DNDEBUG
release: clean $(BIN_NAME)

# Install dependencies (Ubuntu/Debian)
.PHONY: install-deps
install-deps:
	@echo "Installing build dependencies..."
	sudo apt-get update
	sudo apt-get install -y gcc libusb-1.0-0-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev
	@echo "Dependencies installed"

# Install runtime dependencies
.PHONY: install-runtime-deps
install-runtime-deps:
	@echo "Installing runtime dependencies..."
	sudo apt-get update
	sudo apt-get install -y libusb-1.0-0 libavformat61 libavcodec61 libavutil59 libswscale8
	@echo "Runtime dependencies installed"
