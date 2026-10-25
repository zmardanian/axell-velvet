# Changelog

## Research after 1.0.0 (no change to the plugin)
- Romanian detector retrain studied and not adopted (DECISIONS V17): 1.0's detector stays.
- De-plosive studied and not built (DECISIONS V18): on real takes it does not beat LOW CUT 90, which remains the
  recommended pop treatment.

## 1.0.0 - 2026-10-15
First release of Axell Velvet, the vocal plugin of the Axell suite.

- **Smart de-esser:**
  - Neural sibilance detector (19 inputs, 32 hidden units), trained on the shipping C++ features, level and sample-rate
    independent.
  - Water-filling cut into ten complementary linear-phase bands, 2 ms look-ahead.
  - AMOUNT, RANGE, FOCUS (Auto / S / SH), LISTEN.
  - Live display of every ess, the target and the reduction.
  - Bit-exact when there is nothing to cut.
- **HEAT:** TAPE (Jiles-Atherton hysteresis, record/playback emphasis, head bump), CONSOLE (transformer + class-A
  stage), TUBE (triode on its load line, grid conduction, bias shift).
  - DRIVE calibrated so every model adds the same harmonics at the same setting.
  - Automatic level matching; S-GUARD keeps the warmth off the esses.
  - TONE, MIX.
  - 2x-16x oversampling (16x default: aliasing ≤ -95 dBc at full drive).
- **LEVEL with LEARN:** the models meet every recording at their nominal level.
- **LOW CUT:** Off / 30 / 60 / 90 Hz, 24 dB/oct.
- **Presets:** 25 factory presets (Essentials, Rap & Hip-Hop, Singing, Special, MY VOICE), user presets, A/B.
- **Latency:** reported exactly and changed only after the host was told; host bypass latency-aligned and exact.
