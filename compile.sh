#!/bin/bash
#
# compile.sh - Build script for PvZ2Native.
#
# This script configures and compiles the project using CMake, then optionally
# copies or extracts the required Android game assets (libPVZ2.so and OBB files)
# from the local game/ directory or from an APK archive.
#
# Run with --help for a complete list of options.
#

# Exit immediately on error, treat unset variables as errors, and fail on
# errors inside pipelines.
set -euo pipefail

# -----------------------------------------------------------------------------
# Paths and defaults
# -----------------------------------------------------------------------------
# Absolute path to the project root (directory containing this script).
PROJECT_ROOT="$(cd "$(dirname "$0")" && pwd)"

# Directory where CMake performs the out-of-source build.
BUILD_DIR="${PROJECT_ROOT}/build"

# Directory that should contain the original Android game files.
GAME_DIR="${PROJECT_ROOT}/game"

# Directory where the compiled executable is expected to land.
EXE_DIR="${BUILD_DIR}/pvz2native"

# Directory where libPVZ2.so and the OBB must be placed for the game to run.
LIB_DIR="${EXE_DIR}/lib"

# File that receives the full CMake / build output for later inspection.
LOG_FILE="${BUILD_DIR}/compile.log"

# Default build settings.
BUILD_TYPE="Release"
CLEAN="no"
NO_ASSETS="no"
JOBS=""
VERBOSE="no"

# List of supported Android ABIs inside an APK archive. The script tries them
# in order so that modern 64-bit devices are preferred over older 32-bit ones.
ABIS=(
    "lib/arm64-v8a/libPVZ2.so"
    "lib/armeabi-v7a/libPVZ2.so"
    "lib/x86_64/libPVZ2.so"
    "lib/x86/libPVZ2.so"
)

# -----------------------------------------------------------------------------
# Terminal colors helpers
# -----------------------------------------------------------------------------
# ANSI color codes are enabled only when stdout is a terminal and the user has
# not disabled them with NO_COLOR=1.
if [[ -t 1 && -z "${NO_COLOR:-}" ]]; then
    RED=$'\e[31m'
    GREEN=$'\e[32m'
    YELLOW=$'\e[33m'
    BLUE=$'\e[34m'
    BOLD=$'\e[1m'
    RESET=$'\e[0m'
else
    RED=''
    GREEN=''
    YELLOW=''
    BLUE=''
    BOLD=''
    RESET=''
fi

info()    { echo "${BLUE}[INFO]${RESET} $*"; }
success() { echo "${GREEN}[OK]${RESET} $*"; }
warn()    { echo "${YELLOW}[WARN]${RESET} $*" >&2; }
error()   { echo "${RED}[ERROR]${RESET} $*" >&2; }
fail()    { error "$*"; exit 1; }

# -----------------------------------------------------------------------------
# Command-line argument parsing
# -----------------------------------------------------------------------------
usage() {
    cat <<EOF
Usage: $0 [OPTIONS]

Build PvZ2Native and optionally copy/extract game assets.

Options:
  -c, --clean          Delete the build directory before configuring
  -d, --debug          Build in Debug mode (default: Release)
  -r, --release        Build in Release mode (default)
  -j, --jobs N         Use N parallel jobs during compilation
  -n, --no-assets      Skip copying/extracting game assets
  -v, --verbose        Print the full CMake/build output to the terminal
  -h, --help           Show this help message and exit

Environment:
  NO_COLOR=1           Disable colored output

Examples:
  $0                   # Standard Release build
  $0 --debug --clean   # Clean Debug build
  $0 --jobs 4          # Release build with 4 parallel jobs
  $0 --no-assets       # Build only, do not touch game files
EOF
}

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            -c|--clean)
                CLEAN="yes"
                shift
                ;;
            -d|--debug)
                BUILD_TYPE="Debug"
                shift
                ;;
            -r|--release)
                BUILD_TYPE="Release"
                shift
                ;;
            -j|--jobs)
                # Make sure a numeric value follows the --jobs flag.
                if [[ -z "${2:-}" || "$2" =~ ^- ]]; then
                    fail "Option $1 requires a numeric argument"
                fi
                if ! [[ "$2" =~ ^[0-9]+$ ]]; then
                    fail "Invalid number of jobs: $2"
                fi
                JOBS="$2"
                shift 2
                ;;
            -n|--no-assets)
                NO_ASSETS="yes"
                shift
                ;;
            -v|--verbose)
                VERBOSE="yes"
                shift
                ;;
            -h|--help)
                usage
                exit 0
                ;;
            -*)
                fail "Unknown option: $1 (use --help for usage)"
                ;;
            *)
                fail "Unexpected argument: $1"
                ;;
        esac
    done
}

# -----------------------------------------------------------------------------
# Dependency checks
# -----------------------------------------------------------------------------
# Abort early with a clear message when the environment is missing required
# build tools. This avoids confusing CMake errors later on.
check_deps() {
    local missing=()

    if ! command -v cmake &>/dev/null; then
        missing+=("cmake")
    fi

    # CMake works with Make or Ninja; at least one backend must exist.
    if ! command -v ninja &>/dev/null && ! command -v make &>/dev/null; then
        missing+=("make or ninja")
    fi

    if ! command -v unzip &>/dev/null; then
        missing+=("unzip")
    fi

    if ! command -v g++ &>/dev/null && ! command -v clang++ &>/dev/null; then
        missing+=("a C++ compiler (g++ or clang++)")
    fi

    if [[ ${#missing[@]} -gt 0 ]]; then
        fail "Missing required tools: ${missing[*]}"
    fi
}

# -----------------------------------------------------------------------------
# CMake build steps
# -----------------------------------------------------------------------------
# Run the CMake configuration step inside BUILD_DIR. All output is captured in
# LOG_FILE unless the user requested --verbose.
configure() {
    info "Configuring with CMake (${BUILD_TYPE})..."

    local cmake_args=(
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
        -DBoost_ROOT="${PROJECT_ROOT}/third_party/boost_1_84_0"
        -DDYNARMIC_USE_PRECOMPILED_HEADERS=OFF
        ..
    )

    if [[ "${VERBOSE}" == "yes" ]]; then
        if ! cmake "${cmake_args[@]}"; then
            fail "CMake configuration failed"
        fi
    else
        if ! cmake "${cmake_args[@]}" >>"${LOG_FILE}" 2>&1; then
            fail "CMake configuration failed. See ${LOG_FILE}"
        fi
    fi
}

# Compile the project. The optional --jobs value is passed through CMake's
# --parallel flag to control parallelism.
build() {
    info "Building (${BUILD_TYPE})..."

    local build_args=()
    if [[ -n "${JOBS}" ]]; then
        build_args+=(--parallel "${JOBS}")
    fi

    if [[ "${VERBOSE}" == "yes" ]]; then
        if ! cmake --build . "${build_args[@]}"; then
            fail "Build failed"
        fi
    else
        if ! cmake --build . "${build_args[@]}" >>"${LOG_FILE}" 2>&1; then
            fail "Build failed. See ${LOG_FILE}"
        fi
    fi
}

# -----------------------------------------------------------------------------
# Game asset helpers
# -----------------------------------------------------------------------------
# Locate the native Android library. Prefer an already extracted libPVZ2.so in
# the game/ directory; otherwise try to extract it from the first APK found.
copy_or_extract_lib() {
    if [[ -f "${GAME_DIR}/libPVZ2.so" ]]; then
        info "Copying game/libPVZ2.so → ${LIB_DIR}/"
        cp "${GAME_DIR}/libPVZ2.so" "${LIB_DIR}/"
        return 0
    fi

    # Find the first APK in the game directory. Using a for loop is safer than
    # parsing the output of ls because file names might contain whitespace.
    local apk=""
    for f in "${GAME_DIR}"/*.apk; do
        if [[ -f "$f" ]]; then
            apk="$f"
            break
        fi
    done

    if [[ -z "${apk}" ]]; then
        warn "No libPVZ2.so or APK found in ${GAME_DIR}/"
        return 1
    fi

    info "Extracting libPVZ2.so from $(basename "${apk}")..."

    # Try each known ABI path inside the APK until one succeeds.
    for abi in "${ABIS[@]}"; do
        if unzip -j -o "${apk}" "${abi}" -d "${LIB_DIR}" >/dev/null 2>&1; then
            success "Extracted ${abi} from APK"
            return 0
        fi
    done

    warn "Could not extract libPVZ2.so from APK (tried all known ABIs)"
    return 1
}

# Copy the first OBB expansion file found in the game/ directory.
copy_obb() {
    local obb=""
    for f in "${GAME_DIR}"/*.obb; do
        if [[ -f "$f" ]]; then
            obb="$f"
            break
        fi
    done

    if [[ -z "${obb}" ]]; then
        warn "No OBB file found in ${GAME_DIR}/"
        return 1
    fi

    info "Copying $(basename "${obb}") → ${LIB_DIR}/"
    cp "${obb}" "${LIB_DIR}/"
    return 0
}

# -----------------------------------------------------------------------------
# Final summary
# -----------------------------------------------------------------------------
# Report the build outcome, list the files under lib/, and warn about missing
# assets that the user still needs to provide manually.
summary() {
    echo ""
    echo "${BOLD}Summary${RESET}"
    echo "  Build type: ${BUILD_TYPE}"
    echo "  Build log:  ${LOG_FILE}"
    echo "  Executable: ${EXE_DIR}/pvz2native"

    if [[ ! -f "${EXE_DIR}/pvz2native" ]]; then
        warn "Executable not found at ${EXE_DIR}/pvz2native"
    else
        success "Executable built successfully"
    fi

    echo ""
    echo "${BOLD}lib/ contents:${RESET}"
    if [[ -d "${LIB_DIR}" ]]; then
        ls -1 "${LIB_DIR}" 2>/dev/null || echo "(empty)"
    else
        echo "(directory missing)"
    fi
    echo ""

    local missing=()

    if [[ ! -f "${LIB_DIR}/libPVZ2.so" ]]; then
        missing+=("libPVZ2.so")
        echo "${YELLOW}Missing:${RESET} libPVZ2.so"
        echo "  Place it at: ${GAME_DIR}/libPVZ2.so"
        echo "  Or an APK at: ${GAME_DIR}/<game>.apk"
        echo ""
    fi

    # Detect whether at least one OBB has been placed inside lib/.
    local obb_found="no"
    for f in "${LIB_DIR}"/*.obb; do
        if [[ -f "$f" ]]; then
            obb_found="yes"
            break
        fi
    done

    if [[ "${obb_found}" == "no" ]]; then
        missing+=("OBB file")
        echo "${YELLOW}Missing:${RESET} main.<version>.com.ea.game.pvz2_<region>.obb"
        echo "  Place it at: ${GAME_DIR}/"
        echo ""
    fi

    if [[ ${#missing[@]} -gt 0 ]]; then
        warn "Some game assets are missing; the executable may not run correctly."
    else
        success "All game assets present."
    fi
}

# -----------------------------------------------------------------------------
# Main entry point
# -----------------------------------------------------------------------------
main() {
    parse_args "$@"
    check_deps

    # Prepare the log file before anything else so every following step can
    # append to it.
    mkdir -p "${BUILD_DIR}"
    : >"${LOG_FILE}"

    # When --clean is requested, remove the entire build tree and recreate it.
    if [[ "${CLEAN}" == "yes" ]]; then
        warn "Cleaning build directory: ${BUILD_DIR}"
        rm -rf "${BUILD_DIR}"
        mkdir -p "${BUILD_DIR}"
        : >"${LOG_FILE}"
    fi

    cd "${BUILD_DIR}"

    configure
    build

    cd "${PROJECT_ROOT}"

    mkdir -p "${LIB_DIR}"

    # Copy/extract game assets unless the user explicitly asked to skip them.
    if [[ "${NO_ASSETS}" != "yes" ]]; then
        if [[ -d "${GAME_DIR}" ]]; then
            copy_or_extract_lib || true
            copy_obb || true
        else
            warn "Game directory not found: ${GAME_DIR}"
        fi
    else
        info "Skipping game assets (--no-assets)"
    fi

    summary
}

# Handle Ctrl+C / SIGTERM gracefully instead of leaving the working directory
# or build in a half-finished state without feedback.
trap 'error "Interrupted"; exit 130' INT TERM

main "$@"
