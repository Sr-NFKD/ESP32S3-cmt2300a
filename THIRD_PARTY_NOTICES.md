# Third-party notices

## Flipper protocol reference (2026-10-19)

Protocol timing was cross-checked against the Flipper Zero Dooya decoder:
https://github.com/flipperdevices/flipperzero-firmware/blob/dev/lib/subghz/protocols/dooya.c

The bounded RSSI hopper was also inspected:
https://github.com/flipperdevices/flipperzero-firmware/blob/dev/applications/main/subghz/helpers/subghz_txrx.c

The array-based 40-bit decoder here was independently implemented against measured signal fixtures; Flipper source code was not copied. The strongest-candidate sweep is a local adaptation, not a claim about Flipper's hopper algorithm. Upstream Flipper firmware is GPL-3.0 licensed.

## CMT2300A scanning reference

The CC1101 scan strategy in components/rf_service/rf_service.c adapts the per-frequency asymmetric noise-floor tracking and immediate carrier-hit capture approach from manhere/esp32s3-cmt2300a-spectrum.

Source: https://github.com/manhere/esp32s3-cmt2300a-spectrum
Reference commit inspected: 9e8fdc61ea0942e75e4062d0af53a0915aed5a04; also compared with the user-provided local source.

CC1101 register programming, per-frequency calibration cache, timer scheduling and existing RMT/codec integration are platform-specific adaptations. The original CMT register tables, GPIO interrupt capture, web server and TX configuration are not used.

Upstream license (verbatim):

MIT License

Copyright (c) 2026 [Your Name]

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
