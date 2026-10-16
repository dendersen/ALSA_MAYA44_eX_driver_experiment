# snd-maya44ex

ALSA driver for the ESI MAYA44 eX (PCI `19fe:7000`, ES7010L + 2x WM8776),
written to the structure in "Writing an ALSA Driver" (chip struct in
`card->private_data`, create/free pair, PCM and mixer components, PCI
probe/remove, PM suspend/resume, `index`/`id`/`enable` module arrays).

    make
    sudo insmod snd-maya44ex.ko      # optional: unverified_rates=1

- `hw:eX,0` = out/in 1/2, `hw:eX,1` = out/in 3/4; stereo `S32_LE` (24-bit
  left-justified), 1024-frame periods, 2 periods.
- 32, 44.1 and 48 kHz by default; clock words timed against the wall clock on
  hardware. `unverified_rates=1` adds 88.2/96 kHz (clock timed, codec side not
  proven). `rate_sweep.sh` / `rate_check.sh` are the timing helpers.
- Mixer: per-codec DAC volume/switch and ADC gain.
- Register map derived from traces of the vendor Windows driver, not vendor docs.
  Input mapping, input routing/phantom power and S/PDIF are not supported.
- Hardware is a shared-clock group: streams must use the same rate.
- Suspend/resume implemented but untested.
