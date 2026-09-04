# Terminal Emulator - Installation Guide

A custom OpenGL-based terminal emulator with ANSI support, PTY integration, and modern UI features.

## Quick Install

```bash
./install.sh
```

The install script will:
1. Detect your Linux distribution
2. Install required dependencies via package manager
3. Compile the terminal emulator
4. Optionally install system-wide

## Supported Systems

- **Debian/Ubuntu**: apt/apt-get
- **Arch Linux**: pacman
- **Fedora**: dnf
- **Other**: Manual installation (see below)

## Dependencies

### Required Libraries

- **Build Tools**: gcc, make
- **Graphics**: OpenGL, GLEW, GLFW3
- **Audio**: ALSA (libasound), mpg123
- **System**: pthread

### Install Manually (Debian/Ubuntu)

```bash
sudo apt update
sudo apt install -y build-essential gcc make \
    libglfw3-dev libglew-dev libgl1-mesa-dev \
    libglu1-mesa-dev libasound2-dev libmpg123-dev pkg-config
```

### Install Manually (Arch Linux)

```bash
sudo pacman -Sy --needed base-devel gcc make \
    glfw-x11 glew mesa glu alsa-lib mpg123
```

### Install Manually (Fedora)

```bash
sudo dnf install -y gcc make glfw-devel glew-devel \
    mesa-libGL-devel mesa-libGLU-devel alsa-lib-devel mpg123-devel
```

## Manual Compilation

If you prefer to compile manually:

```bash
# Clean previous build
make clean

# Compile
make

# Run
./terminal
```

## Installation Options

### Local Installation

Run without sudo for local installation:
```bash
./install.sh
```

Binary will be created as `./terminal`

### System-Wide Installation

Run with sudo for system-wide installation:
```bash
sudo ./install.sh
```

Binary will be installed to `/usr/local/bin/terminal-emulator`

You can also choose system-wide installation when prompted during non-root installation.

## Running the Terminal

### Local Binary
```bash
./terminal
```

### System-Wide (if installed)
```bash
terminal-emulator
```

## Features

- **ANSI/VT100 Emulation**: Color support, cursor control, escape sequences
- **PTY Support**: Run interactive programs (vim, ssh, top, htop, etc.)
- **Split Terminals**: Horizontal and vertical split modes
- **Tab Completion**: Directory-aware with double-tab listing
- **Modern UI**: OpenGL-rendered with custom fonts
- **SSH Support**: Full interactive SSH sessions with password prompts
- **Control Keys**: Ctrl+C, Ctrl+D, Ctrl+Z support
- **UTF-8**: Proper UTF-8 encoding (display limited to ASCII font range)
- **Sound**: Enter plays assets/sounds/enter_sound.mp3; toggle it from the right-click menu

## Controls

### General
- **Ctrl+Q**: Quit terminal
- **F8/F9**: Increase/decrease font size
- **Tab**: Auto-complete filenames/directories
- **Tab Tab**: List directory contents

### Interactive Mode (shell/ssh)
- **Ctrl+C**: Send SIGINT (interrupt)
- **Ctrl+D**: Send EOF (logout)
- **Ctrl+Z**: Send SIGTSTP (suspend)
- **ESC**: Escape key for vim/nano

### Navigation
- **Arrow Keys**: Command history and cursor movement
- **Page Up/Down**: Scroll terminal output

## Troubleshooting

### Compilation Errors

**Error**: `fatal error: GLFW/glfw3.h: No such file or directory`
- **Fix**: Install GLFW development headers: `sudo apt install libglfw3-dev`

**Error**: `fatal error: GL/glew.h: No such file or directory`
- **Fix**: Install GLEW development headers: `sudo apt install libglew-dev`

**Error**: `cannot find -lglfw`
- **Fix**: Install GLFW library: `sudo apt install libglfw3`

### Runtime Errors

**Error**: `Failed to initialize GLFW`
- **Fix**: Make sure X11 is running (for GUI environments)
- Check: `echo $DISPLAY` should show something like `:0` or `:1`

**Error**: `Failed to load font`
- **Fix**: Ensure font assets are in the correct location relative to binary

### Performance Issues

If the terminal is slow or laggy:
1. Check OpenGL drivers: `glxinfo | grep "OpenGL version"`
2. Try reducing font size with F9
3. Close other GPU-intensive applications

## Uninstall

### Remove Local Binary
```bash
rm terminal
make clean
```

### Remove System-Wide Installation
```bash
sudo rm /usr/local/bin/terminal-emulator
sudo rm /usr/share/applications/terminal-emulator.desktop
```

## Building from Source

```bash
git clone <repository-url>
cd terminal-emulator
./install.sh
```

## Requirements

- **OS**: Linux (X11 or Wayland with XWayland)
- **Graphics**: OpenGL 2.1+ compatible GPU
- **Memory**: ~50MB RAM
- **Disk**: ~5MB for binary + assets

## License

See LICENSE file for details.

## Contributing

Contributions welcome! Please ensure code follows existing style and passes compilation without warnings.
