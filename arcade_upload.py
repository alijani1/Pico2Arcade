# arcade_upload.py - PlatformIO extra script for [env:pico2_arcade].
#
# Flashes PicoGalagino into the "arcade" window (0x201000) of the
# PicoCartReader board instead of offset 0, so the CartReader firmware
# (0x000000-0x1FFFFF, incl. its LittleFS) is left intact. The image is
# still linked at 0x10000000; the CartReader's ARCADE menu launches it
# with the boot ROM's rom_chain_image, which maps the window there.
#
# Keep ARCADE_OFFSET in sync with ARCADE_FLASH_OFFSET in
# PicoCartReader/src/arcade.h.
import os

Import("env")

# Per-env override: custom_arcade_offset (the Quad's arcade slot is 0x10360000,
# see GameAdapterRGBPico include/arcade.hpp). Default: PicoCartReader window.
ARCADE_OFFSET = int(env.GetProjectOption("custom_arcade_offset", "0x10201000"), 0)
# (PicoCartReader: 0x10200000 is the erased guard sector)

picotool = os.path.join(
    env.subst("$PROJECT_PACKAGES_DIR"),
    "tool-picotool-rp2040-earlephilhower",
    "picotool",
)

env.Replace(
    UPLOADER=picotool,
    UPLOADCMD='"%s" load -f -v -x -t bin "$BUILD_DIR/${PROGNAME}.bin" -o 0x%08X'
              % (picotool, ARCADE_OFFSET),
)
