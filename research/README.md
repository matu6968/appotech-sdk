# Research notes

Hardware findings, register evidence and the Ghidra scripts (`*.java`) used to produce them.

The raw evidence these reports cite — acceptance and audit JSON, live-test logs, captures
(PNG/NV21/WAV), flash and RAM dumps, disassembly listings — was removed before publication to keep
the repository small. File names that appear in the reports without a link refer to those removed
files; the conclusions and the procedures that produced them are in the text. To re-create any of
it, run the corresponding script from `../tools` against a camera (the README of the SDK lists the
commands) or the Ghidra scripts here against your own flash dump.

Your own chip backup is not part of the repository: keep a dump of the stock SPI flash
(`apxtool rom-rescue dump` or `apxtool flash read`) before installing an SDK image.

Names such as `flash.bin.golden`, `original.bin` and `spi-before.bin` in these reports are the author's own flash dumps, taken
from the camera before and during the work; they are not published (they hold the owner's saved photos).
[`dumps/flash.bin`](https://github.com/matu6968/Action_Digital_Kids_Camera_DOOM/blob/main/dumps/flash.bin) in the DOOM port is the
same stock application with the photos removed. Photos named in the reports (`IMG_2026....jpg`, `2026....jpg`) are not included either.
