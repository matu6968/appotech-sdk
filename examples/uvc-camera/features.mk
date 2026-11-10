# USB modes this example needs beyond the always-present debug link (read by the top-level Makefile).
# The camera composite device: UVC video + mass storage (the SD card, read-only).
APX_USB_UVC ?= 1
APX_USB_UAC ?= 1
