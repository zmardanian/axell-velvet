# Changelog

## 3.0.3 - 2026-10-27
- **Version on the interface**: the plugin's version, small and quiet after the title - so you can always see which build is loaded. The audio is byte-for-byte the same as 3.0.2 (checked through the released 3.0.2 and the new build).

## 3.0.2 - 2026-10-26
- **Faster, bit-identical.** The pitch analysis inside the sibilance detector's pitch analysis (YIN's difference function) now computes eight lags at a time in registers on a straight copy of the window; every lag still adds its terms in the same order, so the output is byte-for-byte the same as 3.0.1 (checked on four of the artist's takes through the installed 3.0.1 and the new build). CPU at 48 kHz, 128-sample blocks: 8.94 -> 8.72 % of one core (-2.4 %).

## 3.0.1 - 2026-10-26
- **Axell's Dojo look**: the family emblem, a brush-written title with Velvet's seal (絹, "silk"), a gold header rule
  and Velvet's own wine-coloured ground. The sound is unchanged (DSP identical to 3.0.0).
- Title face: Yuji Syuku (SIL OFL 1.1, a 52 KB subset, embedded).
- Verified: DSP 467 checks (44.1-192 kHz), plugin 58, ASan + UBSan clean, independent audit 54/54, pluginval
  strictness 10, Steinberg's VST3 validator 47/47.

## 3.0.0 - 2026-10-25
(There is no 2.0: that work was research only - see the next entry.)
- **LIVE de-esser - zero latency.** A De-Ess Mode switch: STUDIO (as before, for mixing) or LIVE, for recording while
  you hear yourself through the plugin. LIVE has its own causal detector network and cuts with the same magnitude as
  STUDIO, made minimum phase every 2 ms. With HEAT off Velvet then adds no latency at all; with HEAT on, the
  oversampler's 1.7 ms. Bit-exact pass-through when there is nothing to cut. (DECISIONS V20)
- **Detector trained on 111 voices** instead of three: CMU ARCTIC plus 108 VCTK voices from many accents (phone labels by
  the Montreal Forced Aligner). On 12 accents it never heard: F1 0.82 (1.0: 0.72), 97 % of the sibilant energy caught
  (1.0: 92 %). On the artist's own takes it leaves half as many harsh esses uncut. (V19)
- **Benchmarked against a third-party de-esser** (Sibalance) and the classic designs on 15 unseen voices: 30-80x less
  change on vowels than Sibalance at the same harsh-ess removal. (V21)
- **192 kHz** verified for everything (detectors, both de-essers, HEAT, presets, the audit). (V22)
- **Three TRACKING presets** (LIVE): Tracking - De-Ess Only, Tracking - Warm Monitor, My Voice - Tracking (Live).
- Older projects open with the STUDIO de-esser and their saved settings: parameters a saved state does not contain take
  their defaults. (V23)
- Tests: DSP 467 checks (44.1-192 kHz, STUDIO and LIVE), plugin 58, independent audit 54, ASan/UBSan/LSan clean,
  pluginval strictness 10, VST3 validator 47/47.
- Test fixes found on the way: the synthetic test voice's esses grew 3 dB per sample-rate doubling (now the same level
  at every rate); TUBE's 35 kHz plate pole delays an impulse peak by 4.5 us (0.9 sample at 192 kHz) - the latency
  itself is exact.

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
