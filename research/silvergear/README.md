# Silvergear research notes

Notes for the Silvergear printer camera (board `silvergear-3222882`). The bring-up material (run folders, dated reports, probe captures, one-off
scripts, binaries and stock-derived tables) was moved out of the repository in four cleanup passes; it is preserved in the archives next to the
repositories (`cleanup-archive-20261017/`, `cleanup-archive-20261107/`, `cleanup-archive-20261108/`, each with `SHA256SUMS`; paths inside the
tarballs are relative to the `appotech-sdk` root, so `tar xzf FILE -C appotech-sdk` restores them in place).

## Kept here
- `printer.md`: printer driver, stock initialisation and timing findings; `cameras.md`: sensors, select lines and clocks; `display-probe.md`: ST7789 panel;
  `power-ddr.md`: PM chip and DDR; `flash-led.md`: the flash LED (PB0, found from the stock firmware); `spi-install-plan.md`: SPI install, cold boot and the
  install log.
- `analyze_thermistor.py`, `printer-thermistor-fit.json`, `printer-contact-candidates.svg` (render the PNG with the command in `printer.md`), and the JSON
  records `printer-acceptance/patterns.json`, `printer-modes-fixtures/*.json`.
- `stock-app.disasm`: git-ignored, regenerated from the stock image; `tools/test_photo_prepare.py` and `tools/stock_photo_reference.py` run it.

## Archived (names still appear in the notes as plain text)
- 2026-11-07 (`cleanup-archive-20261107/`): the raw bring-up runs (`printer-test-*`, `printer-bringup-*`, `camera-test-*`, `printer-modes-*`), power/DDR, LCD,
  camera, ADC and SPI probe captures, recorded firmware and ELF files, stock UART logs, and the bring-up-only code (the examples `printer-adc-*`,
  `printer-heat-watch-test`, `printer-phy-test`, `printer-signal-test`, `dual-camera-test` and others, the LCD probe payload, the printer reference backend,
  `apxtool/src/apxtool/silvergear_lcd.py`, `apxtool/research/silvergear_{camera_test,spi_probe}.py`) plus the flash-LED search tools. The launch path
  (`boot/silvergear_bringup.c`, `apxtool silvergear`, `apxtool/research/silvergear_printer_test.py`) and the stock-restore payload stay in the tree.
- 2026-11-08 (`cleanup-archive-20261108/`): `printer-sd-compare-runs-20261107.tar.gz` (13 print-comparison runs), `flash-led-scan-runs-20261107.tar.gz`,
  `stock-derived-research.tar.gz` (decompilation and tables derived from the stock firmware, kept out of the public history),
  `bringup-verification-scripts-20261108.tar.gz`, `binary-research-files.tar.gz` (every binary file that was here), and
  `dated-bringup-reports-20261102-20261108.tar.gz` (the dated reports: the 2026-11-02 bootloop incident, flash-LED search, printer initialisation and
  acceptance, printer modes, SD print comparison, the audio / JPEG / LVGL / examples bring-up reports, and the two dated acceptance JSON records).
  Nothing in the build, tests or tools reads any of it.
