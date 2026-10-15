# Axell Velvet (VST3, Linux)

The vocal plugin of the Axell suite: a de-esser that recognises sibilants instead of listening for loud treble, and
three analog saturation models (tape, console, tube) to give a voice its signature warmth. The two are linked:
S-GUARD keeps the warmth off the esses, so the saturation never makes them harsh again.

![Axell Velvet](docs/screenshots/velvet-100pct.png)

## What it does

**DE-ESS** is a one-knob de-esser. **AMOUNT** sets how far the esses come down. There is no threshold: the same
setting works on a quiet verse and a loud chorus, at any recording level.
- **Detection:** a small neural network trained on labelled speech recognises s, z, sh, zh, ch and j by their sound
  (noisiness, spectral shape, how long the noise lasts). It is not a treble meter.
- **Cut:** only the frequencies where each ess is harsh are lowered. An "s" whistling at 8.7 kHz is cut at 8.7 kHz,
  a "sh" at 3.3 kHz at 3.3 kHz. Vowels, "t" bursts and hi-hat bleed are left alone.
- **Precision:** ten linear-phase bands with 2 ms look-ahead. With nothing to cut, the output is the input bit for bit.
- **More controls:**
  - RANGE: the most any band may be cut.
  - FOCUS: Auto / S / SH.
  - LISTEN: hear only what is removed.
  - The display shows every ess, the target and the reduction live.

**HEAT** offers three models, run at 16x oversampling:
- **TAPE:** magnetic tape (Jiles-Atherton hysteresis with record and playback emphasis). Treble and esses saturate
  first, as on tape; the low end gets a playback head bump. Smooth, glued, odd harmonics.
- **CONSOLE:** an input transformer (low, loud notes saturate first) into a class-A transistor stage. Punch,
  weight, presence; 2nd and 3rd harmonics.
- **TUBE:** a triode stage (Koren's tube equations on a real load line) with grid conduction and bias shift. Sweet,
  mostly 2nd harmonic, blooms on loud notes.

Each model has these controls:
- **DRIVE (0-10):** calibrated so every model adds the same amount of harmonics at the same setting (-55 dB at 0,
  -35 dB at 5, -15 dB at 10). The level is matched automatically, so turning it up adds character, not volume.
- **S-GUARD:** how much the esses are kept out of the heat.
- **TONE:** dark ↔ bright.
- **MIX:** parallel saturation.
- The display shows the model's curve at your vocal's current level and the harmonics it adds.

**GLOBAL**:
- **LEVEL + LEARN:** play the vocal and press LEARN. LEVEL is set so the models meet your recording at their nominal
  level, so DRIVE means the same on every recording. LEVEL never changes the output volume.
- **LOW CUT** (Off / 30 / 60 / 90 Hz, 24 dB/oct): removes rumble before the models, which would otherwise distort
  the voice with it. The default 30 Hz takes only what you can't hear.
- **OUTPUT**, oversampling 2x-16x, A/B, user presets.

There are 25 factory presets in five groups:
- Essentials
- Rap & Hip-Hop
- Singing
- Special
- MY VOICE: five presets made on Axell's own voice and recording chain (a large-diaphragm condenser into an
  interface preamp with a FET compressor). They also set LEVEL and a 90 Hz low cut for that chain.

## Measured quality

| | |
|---|---|
| Nothing to do (de-ess 0 %, heat off, low cut off) | output = input delayed by the reported latency, bit for bit (44.1 / 48 / 96 kHz, any block size) |
| Silence in, full drive | exactly silence out |
| De-esser vs the classic designs, on voices it never trained on, at equal harsh-ess removal | 10-30x less change on vowels, 2-10x less on t/k/p, 1.5-3x less cut on already-soft esses; at 100 % it leaves fewer harsh esses than either classic design reaches at any setting |
| Detector | rate independent (44.1-96 kHz) and level independent (-44 vs -14 dBFS: reduction within 0.5 dB) |
| HEAT aliasing, 7 kHz at -6 dBFS, full drive | ≤ -95 dBc at 16x (tape -120 dBc) |
| HEAT aliasing at equal saturation (3 % THD) vs certified UAD plugins | Velvet tape -108 / console -129 / tube -113 dBc; UADx Oxide -96, 610-B -89, Century -79, 610-A -70 dBc |
| Auto gain | small-signal gain 0 ± 0.25 dB in every model; every preset keeps the vocal's level within 0.5 dB |
| Latency | 755 samples at 48 kHz (15.7 ms; the de-esser's look-ahead), reported exactly, changes only after the host was told |
| CPU (48 kHz stereo, one core, 16x) | tape ≈ 8-10 %, console ≈ 6 %, tube ≈ 5 % (8x: 8 / 4 / 3.4 %) |
| Real-time safety | no allocation, lock or I/O in the audio path (allocation counter in the test suite) |

All of these come from:
- the automated suites: `tests/`, 289 DSP checks at 44.1 / 48 / 96 kHz plus 51 plugin checks;
- an independent black-box audit through a VST3 host (`tests/independent/velvet_audit.py`, 37 checks, signals and
  analysis from textbook definitions);
- AddressSanitizer + UBSan;
- pluginval at strictness 10;
- Steinberg's VST3 validator (47/47).

The benchmarks are in `research/` (`bench_cpp.py`, `uad_bench.py`). The reasoning behind every design choice, with
what was tried and rejected, is in [docs/DECISIONS.md](docs/DECISIONS.md).

## Install (Linux)

    tar xzf AxellVelvet-1.0.0-linux-x86_64.tar.gz
    cd AxellVelvet-1.0.0-linux-x86_64
    ./install.sh

The installer copies `Axell Velvet.vst3` to `~/.vst3`. Then in REAPER: Options > Preferences > Plug-ins > VST >
Re-scan. Put it on the vocal track, after pitch correction and before the compressor (or after it: the de-esser
does not depend on level).

## Build (Bazzite / any Linux, inside a distrobox)

    bash scripts/setup-toolbox.sh      # creates the juce-build distrobox (Ubuntu 24.04) once
    bash scripts/build.sh
    bash scripts/test.sh               # DSP + plugin suites (--quick, --asan)
    bash scripts/pluginval.sh
    bash scripts/install.sh

- JUCE 8.0.15 is fetched by CMake.
- The DSP lives in JUCE-free headers: `Source/Engine.h`, `Sibilance.h`, `Heat.h` and the shared Axell core in
  `external/axell-core` (or `../axell-core`). It is tested with plain g++.
- The detector's weights (`Source/DetectorModel.h`) are generated by `research/train_cpp.py` from the features of the
  shipping C++ code (`tools/VelvetFeatures.cpp`).

## Contact

Axell Yobag - axellpetre@protonmail.com (questions, bug reports, feedback). Issues on GitHub are welcome too.

Licence: GNU AGPLv3 (`LICENSE`); third-party components and the CMU ARCTIC notice (training data of the detector) in
`THIRD_PARTY_NOTICES.md`. No code, data or graphics of any commercial product are used.
