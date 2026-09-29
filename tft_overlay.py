# tft_overlay.py - PlatformIO pre-build script (all envs).
#
# The GMT024-01 ST7789 panel needs a custom init sequence; stock
# TFT_eSPI gives inverted colours / red-blue swap and wrong gamma.
# Each PlatformIO env gets its own copy of TFT_eSPI under
# .pio/libdeps/<env>/, so a hand-patched copy doesn't carry over to new
# envs. This copies tft_espi_override/ST7789_Init.h.custom over the
# driver file in the current env before every build (only when it
# differs, so it doesn't force a recompile).
#
# Source of truth for the overlay: PicoCartReader/tft_espi_override/.
import filecmp
import os
import shutil

Import("env")

src = os.path.join(env.subst("$PROJECT_DIR"), "tft_espi_override",
                   "ST7789_Init.h.custom")
dst = os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env.subst("$PIOENV"),
                   "TFT_eSPI", "TFT_Drivers", "ST7789_Init.h")

if not os.path.isdir(os.path.dirname(dst)):
    print("tft_overlay: TFT_eSPI not installed yet for %s; skipping" % env.subst("$PIOENV"))
elif not os.path.exists(dst) or not filecmp.cmp(src, dst, shallow=False):
    shutil.copyfile(src, dst)
    print("tft_overlay: installed custom ST7789_Init.h for %s" % env.subst("$PIOENV"))
