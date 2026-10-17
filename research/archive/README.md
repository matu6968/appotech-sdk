# Superseded hardware experiments

These scripts were retained as `.py.txt` to document the investigation and were removed from the
repository before publication; only the findings below remain. Use `tools/display_demo.py` with the SDK driver.

`probe_display_dma` incorrectly treated LCD `a85c` as a RAM source pointer;
its apparent address mask was a formatting-register mask. `probe_dma_memory`
treated the `d000` block as a copy engine, although its output resembles a
packed/compressed stream; its semantics remain unverified. `probe_copy_dma`
refused to run because it could not establish that `8900` was idle.
`probe_display_pipeline` established the actual compositor path, identity scaler,
YUV conversion and V/U order, but relied on inherited state and transient scratch.
The SDK driver now initializes and owns that path explicitly.

Their JSON captures remain in the parent directory and record failures as well
as completed controller transactions. A controller completion flag alone did
not establish correct optical output.

`sd_probe.py.txt` is the superseded APXD register probe used to identify the SD controller. The integrated SDK driver and `tools/test_sd.py` replace it; avoid resetting an active FatFs card through direct SPR probes.

`audio_probe.py.txt` identified the speaker DAC/DMA and analog controls. Its
initial rate write cleared DAC enable and produced silence. The corrected
probe was audible, but lost its USB handle during cleanup; the SDK
re-enumerated and was reopened/muted. The integrated audio driver and
`tools/test_audio_live.py` replace this probe. See `../audio.md` for the
verified clock selectors, explicit enable sequence and acceptance results.

`microphone_probe.py.txt` traced the recording block on the accepted speaker
SDK using backed-up scratch RAM. It retained failed stock-rate and SRAM
experiments as well as successful DDR captures. Production capture uses
`appotech/microphone.h` and the `microphone-demo`; the exploratory probe is
not a supported interface. See `../microphone.md`.
