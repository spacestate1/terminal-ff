#!/bin/bash

# Terminal Emulator Installation Script
# Installs dependencies and compiles the terminal emulator locally

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Print colored messages
print_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Detect package manager
detect_package_manager() {
    if command -v apt-get &> /dev/null; then
        PKG_MANAGER="apt-get"
        print_info "Detected package manager: apt-get (Debian/Ubuntu)"
    elif command -v apt &> /dev/null; then
        PKG_MANAGER="apt"
        print_info "Detected package manager: apt (Debian/Ubuntu)"
    elif command -v pacman &> /dev/null; then
        PKG_MANAGER="pacman"
        print_info "Detected package manager: pacman (Arch Linux)"
    elif command -v dnf &> /dev/null; then
        PKG_MANAGER="dnf"
        print_info "Detected package manager: dnf (Fedora)"
    else
        print_error "Unsupported package manager. Please install dependencies manually."
        print_info "Required: build-essential, libglfw3-dev, libglew-dev, libgl1-mesa-dev, libasound2-dev, libmpg123-dev"
        exit 1
    fi
}

# Install dependencies based on package manager
install_dependencies() {
    print_info "Installing dependencies..."

    case $PKG_MANAGER in
        apt-get|apt)
            print_info "Updating package lists..."
            sudo $PKG_MANAGER update

            print_info "Installing build tools and libraries..."
            sudo $PKG_MANAGER install -y \
                build-essential \
                gcc \
                make \
                libglfw3-dev \
                libglew-dev \
                libgl1-mesa-dev \
                libglu1-mesa-dev \
                libasound2-dev \
                libmpg123-dev \
                pkg-config
            ;;

        pacman)
            print_info "Installing build tools and libraries..."
            sudo pacman -Sy --needed --noconfirm \
                base-devel \
                gcc \
                make \
                glfw-x11 \
                glew \
                mesa \
                glu \
                alsa-lib \
                mpg123
            ;;

        dnf)
            print_info "Installing build tools and libraries..."
            sudo dnf install -y \
                gcc \
                make \
                glfw-devel \
                glew-devel \
                mesa-libGL-devel \
                mesa-libGLU-devel \
                alsa-lib-devel \
                mpg123-devel
            ;;

        *)
            print_error "Package manager not supported in install script"
            exit 1
            ;;
    esac

    print_success "Dependencies installed successfully"
}

# Check if dependencies are already installed
check_dependencies() {
    print_info "Checking for existing dependencies..."

    local missing=0

    # Check for compiler
    if ! command -v gcc &> /dev/null; then
        print_warning "gcc not found"
        missing=1
    fi

    # Check for make
    if ! command -v make &> /dev/null; then
        print_warning "make not found"
        missing=1
    fi

    # Check for pkg-config
    if ! command -v pkg-config &> /dev/null; then
        print_warning "pkg-config not found"
        missing=1
    fi

    # Check for GLFW library
    if ! pkg-config --exists glfw3 2>/dev/null; then
        print_warning "GLFW3 library not found"
        missing=1
    fi

    # Check for GLEW library
    if ! pkg-config --exists glew 2>/dev/null && ! ldconfig -p | grep -q libGLEW; then
        print_warning "GLEW library not found"
        missing=1
    fi

    if [ $missing -eq 1 ]; then
        return 1
    else
        print_success "All dependencies are already installed"
        return 0
    fi
}

# Compile the terminal emulator
compile_terminal() {
    print_info "Compiling terminal emulator..."

    # Clean previous build
    if [ -f "Makefile" ]; then
        make clean 2>/dev/null || true
    fi

    # Compile
    if make; then
        print_success "Compilation successful"

        # Check if binary exists
        if [ -f "terminal" ]; then
            print_success "Binary created: $(pwd)/terminal"
            chmod +x terminal
            return 0
        else
            print_error "Binary not created"
            return 1
        fi
    else
        print_error "Compilation failed"
        return 1
    fi
}

# Main installation process
main() {
    echo ""
    echo "========================================"
    echo "  Terminal Emulator Installation"
    echo "========================================"
    echo ""

    # Check if we're in the right directory
    if [ ! -f "terminal.c" ] || [ ! -f "Makefile" ]; then
        print_error "Please run this script from the terminal-emulator directory"
        exit 1
    fi

    detect_package_manager

    # Ask user if they want to install dependencies
    if ! check_dependencies; then
        echo ""
        echo -ne "${YELLOW}Install missing dependencies? [Y/n]:${NC} "
        read -n 1 -r
        echo ""
        if [[ $REPLY =~ ^[Yy]$ ]] || [[ -z $REPLY ]]; then
            install_dependencies
        else
            print_warning "Skipping dependency installation"
            print_info "You may need to install them manually"
        fi
    fi

    echo ""

    # Compile
    if compile_terminal; then
        echo ""
        print_success "Installation complete!"
        echo ""
        print_info "Installation summary:"
        echo "  - Binary: ./terminal"
        echo "  - Run with: ./terminal"
        echo ""

    else
        print_error "Installation failed during compilation"
        exit 1
    fi
}

# Run main installation
main
