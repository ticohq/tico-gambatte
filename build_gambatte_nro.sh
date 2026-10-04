#!/bin/bash

export DEVKITPRO=/opt/devkitpro
export DEVKITA64=$DEVKITPRO/devkitA64

echo "=== Building Gambatte NRO with Tico Overlay ==="

# Include devkitA64 toolchain
source $DEVKITPRO/devkitA64/base_tools 2>/dev/null || true

PORTLIBS=$DEVKITPRO/portlibs/switch
LIBNX=$DEVKITPRO/libnx

# Project root
ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$ROOT_DIR/build_tico"
TICO_DIR="$ROOT_DIR/tico"

# Prefer a locally built Mesa tree when present so the NRO doesn't keep
# embedding the older portlibs OpenGL stack.
MESA_SOURCE_ROOT="${MESA_SOURCE_ROOT:-}"
MESA_BUILD_ROOT=""

if [ -z "$MESA_SOURCE_ROOT" ]; then
    for candidate in "$HOME/mesa-clean" "/mesa-clean"; do
        if [ -d "$candidate" ]; then
            MESA_SOURCE_ROOT="$candidate"
            break
        fi
    done
fi

if [ -n "$MESA_SOURCE_ROOT" ]; then
    if [ -f "$MESA_SOURCE_ROOT/build/src/egl/libEGL.a" ]; then
        MESA_BUILD_ROOT="$MESA_SOURCE_ROOT/build"
    elif [ -f "$MESA_SOURCE_ROOT/src/egl/libEGL.a" ]; then
        MESA_BUILD_ROOT="$MESA_SOURCE_ROOT"
    fi
fi

USE_CUSTOM_MESA=0
MESA_ARCHIVES=()

if [ -n "$MESA_BUILD_ROOT" ]; then
    REQUIRED_MESA_ARCHIVES=(
        "$MESA_BUILD_ROOT/src/egl/libEGL.a"
        "$MESA_BUILD_ROOT/src/mapi/shared-glapi/libglapi.a"
        "$MESA_BUILD_ROOT/src/gallium/drivers/nouveau/libnouveau.a"
        "$MESA_BUILD_ROOT/src/nouveau/codegen/libnouveau_codegen.a"
        "$MESA_BUILD_ROOT/src/gallium/winsys/nouveau/switch/libnouveauwinsys.a"
        "$MESA_BUILD_ROOT/libdrm_nouveau/lib/libdrm_nouveau.a"
    )

    MISSING_MESA_ARCHIVE=0
    for archive in "${REQUIRED_MESA_ARCHIVES[@]}"; do
        if [ ! -f "$archive" ]; then
            MISSING_MESA_ARCHIVE=1
            echo "Custom Mesa archive missing: $archive"
        fi
    done

    if [ "$MISSING_MESA_ARCHIVE" -eq 0 ]; then
        USE_CUSTOM_MESA=1
        MESA_ARCHIVES=("${REQUIRED_MESA_ARCHIVES[@]}")
        echo "Using custom Mesa build from: $MESA_BUILD_ROOT"
    else
        echo "Falling back to devkitPro portlibs Mesa."
    fi
else
    if [ -n "$MESA_SOURCE_ROOT" ]; then
        echo "Custom Mesa not found at $MESA_SOURCE_ROOT, using devkitPro portlibs Mesa."
    else
        echo "Custom Mesa not found, using devkitPro portlibs Mesa."
    fi
fi

# ============================================================
# Step 1: Build gambatte as a static library (.a)
# ============================================================
echo "--- Step 1: Building gambatte static library ---"

cd "$ROOT_DIR"
make -f Makefile.libretro clean platform=libnx 2>/dev/null || true
make -f Makefile.libretro -j$(nproc) platform=libnx

STATIC_LIB="$ROOT_DIR/gambatte_libretro_libnx.a"
if [ ! -f "$STATIC_LIB" ]; then
    echo "Error: Static library not found at $STATIC_LIB"
    exit 1
fi
echo "Static library built: $STATIC_LIB"

# ============================================================
# Step 2: Compile Tico overlay sources
# ============================================================
echo "--- Step 2: Compiling Tico overlay sources ---"

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

CC="${DEVKITA64}/bin/aarch64-none-elf-gcc"
CXX="${DEVKITA64}/bin/aarch64-none-elf-g++"

COMMON_FLAGS="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -O2 -g"
COMMON_FLAGS="$COMMON_FLAGS -ffunction-sections -fdata-sections -DDISABLE_LOGGING -D__SWITCH__ -DHAVE_LIBNX"
COMMON_FLAGS="$COMMON_FLAGS -DIMGUI_IMPL_OPENGL_LOADER_CUSTOM -include glad.h"
COMMON_FLAGS="$COMMON_FLAGS -I$LIBNX/include -I$PORTLIBS/include -I$PORTLIBS/include/SDL2"
COMMON_FLAGS="$COMMON_FLAGS -I$TICO_DIR -I$TICO_DIR/deps"
COMMON_FLAGS="$COMMON_FLAGS -I$ROOT_DIR/libgambatte/libretro-common/include"
COMMON_FLAGS="$COMMON_FLAGS -I$ROOT_DIR/rcheevos/include -DRC_CLIENT_SUPPORTS_HASH"

if [ "$USE_CUSTOM_MESA" -eq 1 ]; then
    COMMON_FLAGS="$COMMON_FLAGS -I$MESA_SOURCE_ROOT/include -I$MESA_SOURCE_ROOT/libdrm_nouveau/include"
fi

CXXFLAGS="$COMMON_FLAGS -std=gnu++17 -fvisibility-inlines-hidden -fno-rtti -fno-exceptions"

# Tico C++ sources
TICO_SOURCES=(
    "$TICO_DIR/TicoMain.cpp"
    "$TICO_DIR/TicoCore.cpp"
    "$TICO_DIR/TicoShaders.cpp"
    "$TICO_DIR/TicoStubs.cpp"
    "$TICO_DIR/overlay/imgui_overlay.cpp"
    "$TICO_DIR/overlay/overlay_ui.cpp"
    "$TICO_DIR/overlay/ra_alerts.cpp"
    "$TICO_DIR/overlay/tico_config.cpp"
    "$TICO_DIR/overlay/translation_manager.cpp"
)

# glad.c (OpenGL loader)
TICO_C_SOURCES=(
    "$TICO_DIR/glad.c"
)

# ImGui sources
IMGUI_DIR="$TICO_DIR/deps/imgui"
IMGUI_SOURCES=(
    "$IMGUI_DIR/imgui.cpp"
    "$IMGUI_DIR/imgui_draw.cpp"
    "$IMGUI_DIR/imgui_tables.cpp"
    "$IMGUI_DIR/imgui_widgets.cpp"
    "$IMGUI_DIR/imgui_demo.cpp"
    "$IMGUI_DIR/backends/imgui_impl_sdl2.cpp"
    "$IMGUI_DIR/backends/imgui_impl_opengl3.cpp"
)

IMGUI_FLAGS="-I$IMGUI_DIR -I$IMGUI_DIR/backends"

# rcheevos sources
RCHEEVOS_DIR="$ROOT_DIR/rcheevos"
RCHEEVOS_SOURCES=($(find "$RCHEEVOS_DIR/src" -type f -name "*.c" ! -name "rc_client_external.c" 2>/dev/null || true))

TICO_OBJS=()

# Compile Tico C++ sources
for src in "${TICO_SOURCES[@]}"; do
    obj="$BUILD_DIR/$(basename ${src%.cpp}.o)"
    echo "  CXX $src"
    $CXX $CXXFLAGS $IMGUI_FLAGS -c "$src" -o "$obj"
    if [ $? -ne 0 ]; then
        echo "Error compiling $src"
        exit 1
    fi
    TICO_OBJS+=("$obj")
done

# Compile glad.c
for src in "${TICO_C_SOURCES[@]}"; do
    obj="$BUILD_DIR/$(basename ${src%.c}.o)"
    echo "  CC  $src"
    $CC $COMMON_FLAGS -std=gnu11 -c "$src" -o "$obj"
    if [ $? -ne 0 ]; then
        echo "Error compiling $src"
        exit 1
    fi
    TICO_OBJS+=("$obj")
done

# Compile rcheevos sources
for src in "${RCHEEVOS_SOURCES[@]}"; do
    filename=$(basename "$src")
    dirprefix=$(basename $(dirname "$src"))
    obj="$BUILD_DIR/rc_${dirprefix}_${filename%.c}.o"
    echo "  CC  $src"
    $CC $COMMON_FLAGS -std=gnu11 -c "$src" -o "$obj"
    if [ $? -ne 0 ]; then
        echo "Error compiling $src"
        exit 1
    fi
    TICO_OBJS+=("$obj")
done

# Compile ImGui sources
for src in "${IMGUI_SOURCES[@]}"; do
    obj="$BUILD_DIR/$(basename ${src%.cpp}.o)"
    echo "  CXX $src"
    $CXX $CXXFLAGS $IMGUI_FLAGS -c "$src" -o "$obj"
    if [ $? -ne 0 ]; then
        echo "Error compiling $src"
        exit 1
    fi
    TICO_OBJS+=("$obj")
done

echo "Compiled ${#TICO_OBJS[@]} tico/imgui objects"

# ============================================================
# Step 3: Link everything into ELF
# ============================================================
echo "--- Step 3: Linking gambatte_tico.elf ---"

ELF_OUTPUT="$BUILD_DIR/gambatte_tico.elf"

LINK_FLAGS="-specs=$LIBNX/switch.specs -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"
LINK_FLAGS="$LINK_FLAGS -Wl,--gc-sections -Wl,-Map=$BUILD_DIR/gambatte_tico.map"

LINK_LIBS="-L$PORTLIBS/lib -L$LIBNX/lib"
LINK_LIBS="$LINK_LIBS -lSDL2_mixer -lmpg123 -lmodplug -lopusfile -lopus -lvorbisidec -logg -lSDL2"

if [ "$USE_CUSTOM_MESA" -eq 0 ]; then
    if [ -f "$PORTLIBS/lib/libdrm_nouveau.a" ]; then
        LINK_LIBS="$LINK_LIBS -lEGL -lglapi -ldrm_nouveau"
    else
        # Horizon-native Mesa (the switch-dev image): no libdrm_nouveau, see egl.pc
        LINK_LIBS="$LINK_LIBS -lEGL -lglapi -lexpat"
    fi
fi

LINK_LIBS="$LINK_LIBS -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lzstd"
LINK_LIBS="$LINK_LIBS -lnx -lm -lstdc++ -lpthread"

$CXX $LINK_FLAGS \
    "${TICO_OBJS[@]}" \
    "$STATIC_LIB" \
    "${MESA_ARCHIVES[@]}" \
    $LINK_LIBS \
    -o "$ELF_OUTPUT"

if [ $? -ne 0 ]; then
    echo "Error: Linking failed"
    exit 1
fi

echo "ELF created: $ELF_OUTPUT"

# ============================================================
# Step 4: Convert to NRO
# ============================================================
echo "--- Step 4: Creating NRO ---"

NRO_OUTPUT="$BUILD_DIR/tico-gambatte.nro"
ELF2NRO="$DEVKITPRO/tools/bin/elf2nro"
NACPTOOL="$DEVKITPRO/tools/bin/nacptool"

# Create NACP
NACP_FILE="$BUILD_DIR/gambatte.nacp"
$NACPTOOL --create "tico Gambatte" "ticoverse.com" "1.0.3" "$NACP_FILE"

# Convert ELF to NRO with romfs
ROMFS_DIR="$BUILD_DIR/romfs"
rm -rf "$ROMFS_DIR"
mkdir -p "$ROMFS_DIR"

[ -d "$TICO_DIR/fonts" ] && cp -r "$TICO_DIR/fonts" "$ROMFS_DIR/"
[ -d "$TICO_DIR/lang" ] && cp -r "$TICO_DIR/lang" "$ROMFS_DIR/"
[ -d "$TICO_DIR/assets" ] && cp -r "$TICO_DIR/assets" "$ROMFS_DIR/"
# the overlay builds its settings menu from the module's own definition
mkdir -p "$ROMFS_DIR/module"
cp "$TICO_DIR/module/settings.json" "$ROMFS_DIR/module/"

ELF2NRO_ARGS=(--nacp="$NACP_FILE")

if [ -n "$(find "$ROMFS_DIR" -type f 2>/dev/null | head -1)" ]; then
    echo "Embedding romfs from: $ROMFS_DIR"
    ELF2NRO_ARGS+=(--romfsdir="$ROMFS_DIR")
fi

$ELF2NRO "$ELF_OUTPUT" "$NRO_OUTPUT" "${ELF2NRO_ARGS[@]}"

if [ ! -f "$NRO_OUTPUT" ]; then
    echo "Error: tico-gambatte.nro not found"
    exit 1
fi

#---------------------------------------------------------------------------------
# Module bundle
#
# A module is a directory, not a bare NRO: tico discovers it by reading
# module.json, and everything the module owns -- its settings definition, the
# strings that label it, gamelists and console artwork -- travels with it.
#
# The NRO sits beside module.json, so an installed bundle is self-contained and
# extracts straight into sdmc:/tico/modules/<id>/.
#---------------------------------------------------------------------------------
MODULE_SRC="$TICO_DIR/module"
MODULE_ID=$(sed -n 's/.*"id"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$MODULE_SRC/module.json" | head -1)
MODULE_OUT="$BUILD_DIR/module/$MODULE_ID"

rm -rf "$BUILD_DIR/module"
mkdir -p "$MODULE_OUT"
cp -r "$MODULE_SRC/." "$MODULE_OUT/"
cp "$NRO_OUTPUT" "$MODULE_OUT/"
# tico merges these into its own strings to label the settings screen
cp -R "$TICO_DIR/lang" "$MODULE_OUT/"

# Tico prefers .json.gz when resolving a gamelist.
if [ -d "$MODULE_OUT/gamelists" ]; then
    gzip -f -9 "$MODULE_OUT"/gamelists/*.json 2>/dev/null || true
fi

BUNDLE="$BUILD_DIR/tico-$MODULE_ID-module.zip"
rm -f "$BUNDLE"
( cd "$BUILD_DIR/module" && zip -qr "$BUNDLE" "$MODULE_ID" )

echo "======================================"
echo "Build successful!"
echo "  NRO:    $NRO_OUTPUT"
echo "  Module: $BUNDLE"
echo "          extracts to sdmc:/tico/modules/$MODULE_ID/"
echo "======================================"
find "$MODULE_OUT" -type f | sed "s|$BUILD_DIR/module/|    |"
