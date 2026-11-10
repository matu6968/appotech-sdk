# USB modes this example needs beyond the always-present debug link (read by the top-level Makefile).
# A mode that is not listed is not built into the image: it costs RAM and flash (MSC is about 9 KB of code in extra RAM).
APX_USB_MSC ?= 1
