# QZdesk Ball Port

Source: https://github.com/sam70361/aora-bot
Pinned revision: e3b6148c818da4a8e1966f2bc89cdb3cee473b73
Copyright (c) 2026 sam70361.

Only the ball (blob) is used. The firmware's light and dark themes provide
the two backgrounds; the upstream ball palette, 25 eye pairs and all 32
expression definitions are shared. Cloud, star, wedge and gem characters
are not exposed by the firmware.

Unmodified upstream JavaScript is retained for reproducible conversion and
reference. Run `node tools/build_aora_face.cjs` from the project root to
regenerate `app/ai_face_data.h`. `app/ai_face.c` implements the animation
and spherical projection with LVGL vector paths, without a browser.
Motion runs at two thirds of the upstream speed, with longer idle holds.
Hidden faces pause; reduced motion renders the final static pose.
Ribbons and confetti use bounded native paths suitable for the small panel.

Retain LICENSE and NOTICE.md with source and binary distributions.
The CMake build stages these notices in `build/licenses/aora-bot`; firmware
and device deployments install them in `/oem/usr/share/licenses/aora-bot`.

Run `bash tools/check_face.sh` to check all expressions in RGB565, RGB888
and XRGB8888 with animation and reduced motion. The checks save light/dark
frames and verify that hidden or inactive faces pause without jumping.
