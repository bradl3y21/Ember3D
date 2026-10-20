# tests/theme_swap_values.cmake - owned by Verity / QA. Item 156.
#
# The token VALUES the swapped probe is built with (see theme_probe.cpp). Every entry is
# "kind|member name in src/core/Theme.h|new value". tests/CMakeLists.txt rewrites the matching
# initialiser in a COPY of the real Theme.h and FAILS THE CONFIGURE if a member is not found, so a
# renamed or removed token breaks loudly instead of silently dropping out of the test.
#
# Each value is chosen to be unmistakably different from its default, so "the pixel changed" cannot
# be a rounding accident.
set(EMBER_THEME_SWAP
    "color|headerBackground|200, 40, 40, 255"
    "color|regionBackground|30, 90, 30, 255"
    "color|viewportBodyBackground|255, 255, 0, 255"
    "color|widgetFieldBackground|20, 20, 160, 255"
    "color|widgetButtonBackground|150, 50, 150, 255"
    "color|textPrimary|250, 250, 0, 255"
    "color|textDisabled|0, 250, 250, 255"
    "color|selection|10, 200, 100, 255"
    "color|active|250, 120, 0, 255"
    "color|hover|255, 0, 0, 120"
    "float|regionBackgroundAlpha|0.50"
    "float|viewportBodyAlpha|0.50"
    "float|selectionFillAlpha|0.70"
    "float|activeFillAlpha|0.60"
    "float|headerHeight|40.0"
    "float|widgetHeight|30.0"
)
