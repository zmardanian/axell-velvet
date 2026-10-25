# Axell Velvet - design decisions

Every decision below was taken on a measurement; the scripts are in `research/`, the checks in `tests/`.

## V1 - One plugin, de-esser first, the two halves linked

A vocal needs both: the de-esser keeps the esses where they belong, the HEAT models give the voice its colour. They run
in that order: saturation adds treble harmonics, so de-essing first means the models are never fed harsh esses. The
de-esser's detector also drives **S-GUARD** (V8), so warmth never makes the esses harsh again, even with the de-esser
switched off. A one-knob de-esser is the point: AMOUNT, plus RANGE and FOCUS for the rare cases that need them. There
is no threshold, because detection does not depend on level (V2, V5).

## V2 - The sibilance detector: recognition, not level

Classic de-essers compare the treble level with a threshold, so they cut whatever is bright and loud: hi-hat bleed,
"t" bursts, bright vowels. The threshold also has to be re-set for every recording level. Velvet's detector recognises
the sound of a sibilant instead.

- **Features** (every 2 ms, from a 12 ms Hann STFT on the mono sum):
  - YIN aperiodicity of the 1.5 kHz low-passed voice (de Cheveigné & Kawahara 2002; validated earlier against the
    EGG channel of CMU ARCTIC, 90 % voicing agreement);
  - HF spectral flatness;
  - level against the voice's own loud level;
  - six band levels and the 1.5-14 kHz total relative to the voice body (100-1500 Hz), adapted to the singer (minus
    their running mean over voiced, loud frames, 3 s);
  - five of these averaged over the past 48 ms: an "s" lasts, a "t" burst does not.
- **Model:** a 19 → 32 tanh → 1 network. It is trained on the features computed by the shipping C++ code
  (`tools/VelvetFeatures.cpp` → `research/train_cpp.py`), so training and plugin cannot drift apart. The C++ inference
  matches numpy to 2.5e-7.
- **Data:** CMU ARCTIC slt / bdl / jmk (the only full-band ARCTIC voices with phone labels), resampled to 44.1, 48 and
  96 kHz. Half the utterances go through a random recording chain (presence peak +0..6 dB at 3-12 kHz, high-shelf tilt,
  fast FET-style compressor with 3-12 dB of gain reduction, a +-8 dB level ride, a level offset, a noise floor), so the
  model learns sibilance, not one microphone.
- **Held out** (each voice tested by a model that never heard it):

  | | slt | bdl | jmk |
  |---|---|---|---|
  | F1 | 0.72-0.74 | 0.79 | 0.79 |
  | Share of sibilant treble energy treated | 73-77 % | 85-89 % | 82 % |
  | Same for vowels | 0.2-0.3 % | 0.2-0.4 % | 0.8-2.9 % |

  A logistic model on the same features scored F1 0.66-0.73; adding the 48 ms context lifted the network from 0.71-0.76
  to 0.72-0.79.

## V3 - Applying the cut: water-filling into linear-phase bands

- **Reference:** the voice-body level of voiced frames (V5).
- **Target:** the sibilant's 2-17 kHz energy may reach reference + AMOUNT dB (0 % +6 dB, 50 % -12 dB, 100 % -30 dB).
- **Cut:** above the target, water-filling finds the one cap that brings the total down to it by lowering only the
  bands above the cap. An "s" peaking at 8.7 kHz is cut at 8.7 kHz, a "sh" at 3.3 kHz at 3.3 kHz (the two clusters
  measured on Axell's voice). A fixed-band de-esser catches one and misses the other.
- **Weight:** the per-band cut is limited by RANGE and weighted by p² (no cut where the detector is unsure).
- **Timing:** it looks ahead 2 hops (the attack starts before the onset) and releases in 12 ms, all in dB.
- **Bands:** ten complementary linear-phase bands, 2-17 kHz: Kaiser low-passes at every edge, 500 Hz transition, 70 dB.
  Band b = LP(e[b+1]) - LP(e[b]), so the bands sum to a pure delay. Per hop one composite FIR is built and crossfaded
  sample by sample with the previous one, which is the same as per-sample gain interpolation. With no reduction the
  output is the input delayed, bit for bit (`tests/DspTests.cpp`, check 1).
- **Latency:** half the analysis window, plus 2 hops of feature smoothing, plus 2 hops of look-ahead = 14 ms at 48 kHz.
  The FIR's own delay fits inside it. Velvet is a mixing tool, and the host compensates the latency.

## V4 - Rejected: a level-only "safety net"

Tested: cut any treble far above the voice body even where the detector is unsure.

Benchmark (`research/bench_cpp.py`): it removed 0.1-0.5 % more harsh frames, but changed vowels up to 30x more on the
male voice bdl (0.02 → 0.5 dB). Rejected; the detector's confidence alone decides.

## V5 - The voice-body reference (a bug found by the safety-net test)

The first reference started from whatever came first, often the noise floor. It then sat up to 45 dB too low for the
first ~0.3 s after every start of playback: every frame looked harsh, and only the detector's caution hid it.

The reference now works like this:
- Only clearly periodic frames teach it: aperiodicity < 0.25 for 3 frames (6 ms) in a row. Noise sometimes reaches
  0.22 for one frame, never for three.
- It follows their peak level, instant up and 400 ms down, gated by a running 90th percentile.
- A frame whose body is louder than the reference while its treble stays below its body (a vowel swelling before its
  pitch settles) also lifts it.
- Nothing is cut before a reference exists.

Result: the 99.9th percentile of vowel treble re reference fell from +41.7 dB to -0.3 dB.

## V6 - TAPE: Jiles-Atherton hysteresis made clean

- **Signal path:** record pre-emphasis (50 µs / 20 µs, +8 dB at the top) → J-A magnetisation → exact inverse
  de-emphasis. The treble saturates first, as on tape, and that softens esses and cymbals.
- **Reversibility:** the first version (c = 0.55) behaved like unbiased tape: 1.4 % distortion even at -24 dB, and gain
  rising with level. c = 0.995 behaves like well-biased tape: -59 dB at the bottom of the DRIVE range.
- **Aliasing:** the J-A irreversible term switches on and off at every field reversal. That kink sprays harmonics far
  beyond the oversampled band: -73.6 dBc at 16x, full drive, 7 kHz. A smooth gate took it to -120 dBc at 8x and 16x
  (`tests/`, check 8). The first gate, 0.5 (1 + x / sqrt(1 + x²)) with x = δ (Man - M) / 0.01, was replaced by
  0.5 (1 + x / (1 + |x|)): the same aliasing (-141 dBc in the model test), the same harmonics, 25 % less CPU.
- **Solver:** the smooth gate makes the equation stiffer, so it always runs at ≥ 768 kHz (substeps at lower
  oversampling) with Heun steps. RK4 gives identical harmonics.
- **Speed:** alpha = 0 makes L and L' functions of the field alone. They are tabulated (|error| < 2e-6, output
  identical to -127 dB) and the J-A equation reduces exactly to dM_gate (Man - M) / (δ k) + c dMan/dH (-262 dB vs the
  general form). The tape model alone went from 20 % to 4.8 % of a core (16x, stereo, heavy drive). L and L' are interleaved in one
  table, so each lookup touches one cache line.

## V7 - DRIVE means the same on every model

Tape distortion is mostly 3rd harmonic and grows 2 dB per dB of drive; tube and console are mostly 2nd and grow 1 dB per
dB. A fixed dB-per-step scale therefore felt like a cliff on tape and like nothing on tube.

`tools/HeatCalibrate.cpp` measures, on a vocal at the nominal level, the harmonics each model adds above 40 Hz,
relative to its own linear response (so filters are not counted as distortion). It then writes `kDriveTable`: DRIVE
0 → -55 dB, rising 4 dB per step to DRIVE 10 → -15 dB.

Verified on Axell's whole vocal:

| | DRIVE 0 | DRIVE 5 | DRIVE 10 |
|---|---|---|---|
| Tape | -55.1 dB | -35.3 dB | -15.2 dB |
| Console | -55.0 dB | -35.6 dB | -15.3 dB |
| Tube | -51.3 dB | -35.2 dB | -15.3 dB |

## V8 - S-GUARD blends; it does not lower the drive

The first S-GUARD lowered the drive on sibilants (up to 40 dB in a few ms) with the inverse gain after the model. The
models have memory: emphasis filters, transformer flux, the tube's bias shift, coupling capacitors. A fast gain swing
around memory turns into thumps; the test measured only 3.4 dB less heat on the esses.

S-GUARD now fades the model's contribution out of each ess: y = dry + (wet - dry) · mix · (1 - guard · p). The models
always see a steady drive, so no artefacts. At 100 % the esses come out clean (test check 11: more than 10 dB less added
energy on the esses).

## V9 - CONSOLE and TUBE

- **CONSOLE:**
  - Input transformer: the core flux is the integral of the voltage, and the magnetising current rises steeply with
    flux, so low, loud notes saturate first. 6 Hz small-signal corner.
  - Class-A transistor stage with emitter degeneration: R (i - 1) + ln i = g v, solved by Newton into a table. It has
    an exponential cut-off on one side and a soft rail on the other: 2nd and 3rd harmonics.
- **TUBE:**
  - Common-cathode triode, Koren's plate equation (12AX7-like constants), B+ 250 V, 100k plate load, cathode bias
    solved self-consistently. The load line is solved once into a table.
  - Grid conduction above 0 V charges the coupling capacitor, so the bias shifts on loud peaks (2 ms charge, 120 ms
    release). The stage gets richer and compresses after loud notes.
  - Plate capacitance gives -3 dB at 35 kHz.
- **Tables:** symmetric and odd-sized, with f(0) subtracted, so silence stays exactly silence (test check 3).

## V10 - LOW CUT before everything

Axell's dry vocal carries rumble below 20 Hz only 16.5 dB under the voice (stand, floor, air). Any saturator driven
with it intermodulates the voice. LOW CUT is a 24 dB/oct Butterworth (Off / 30 / 60 / 90 Hz), 30 Hz by default (only
the inaudible part). Switching crossfades over 10 ms.

## V11 - LEVEL and LEARN

The models' operating point is "the vocal at -18 LUFS". LEARN measures the input's integrated loudness (BS.1770) and
sets LEVEL = -18 - measured. LEVEL never changes the output volume (the pre/post gains cancel exactly).

Factory presets keep LEVEL, because it belongs to the recording. The five MY VOICE presets set it to Axell's measured
recording level (+15 dB), so they are one click on that chain.

## V12 - Oversampling 16x by default

At full drive with a 7 kHz tone at -6 dBFS, the worst alias is ≤ -95 dBc at 16x for every model; tape reaches -120.
CPU (48 kHz stereo, one core, defaults): tape 9.2 %, console 6.1 %, tube 4.6 %; at 8x 8.1 / 4.0 / 3.3 %.

## V13 - Benchmarks

- **De-esser** (`research/bench_cpp.py`):
  - Three unseen voices, clean and through random recording chains, peak-normalised, with +-8 dB level rides.
  - Two classic designs, each swept over its strength: a fixed-threshold split-band compressor, and the stronger
    auto-threshold variant (treble relative to full band).
  - At equal harsh-ess removal, Velvet changes vowels 10-30x less, t/k/p 2-10x less, and cuts already-soft esses
    1.5-3x less (lisp risk).
  - At AMOUNT 100 % it leaves 1.3-2.9 % harsh frames, lower than either classic design reaches at any setting on two
    of the three voices.
- **HEAT vs certified UAD plugins** (`research/uad_bench.py`; Axell's own licences, run through the same VST3 host):
  - Method: each device at its default settings with the input level swept; aliasing measured where each one reaches
    3 % THD at 1 kHz (equal saturation depth), with a 7 kHz tone.

  | | Worst alias at 3 % THD | Response 30 Hz-18 kHz | Harmonics at 3 % THD | Noise, silence in |
  |---|---|---|---|---|
  | UADx Oxide Tape (15 ips, NAB) | -96.4 dBc | +4.6 dB at 30 Hz, +1.7 dB at 18 kHz | H3 -28.5, H2 -60 dBc | none |
  | UADx 610-A | -70.3 dBc | -2.3 dB at 18 kHz | H2 -26, H3 -27, H4 -33, H5 -34 | -122.6 dBFS |
  | UADx 610-B | -88.7 dBc | -1.1 dB at 18 kHz | H2 -36, H3 -43 | -129.0 dBFS |
  | UADx Century Tube Strip | -79.0 dBc | -2.3 dB at 18 kHz | H2 -42, H3 -47 | -116.9 dBFS |
  | **Velvet TAPE** | **-108.4 dBc** | flat (+1.5 dB head bump since V15) | H3 -29, H5 -55 | none |
  | **Velvet CONSOLE** | **-129.2 dBc** | -0.2 dB | H2 -29, H3 -49 | none |
  | **Velvet TUBE** | **-113.1 dBc** | -1.0 dB at 18 kHz | H2 -30, H3 -54 | none |

  - Aliasing: every Velvet model is cleaner than every UAD plugin tested (12-59 dB).
  - Character: Velvet TAPE matches Oxide's odd-harmonic signature. The 610-A is denser (H2 to H5 within 8 dB), Velvet
    TUBE is an almost pure 2nd: a cleaner, "sweeter" tube. Which one sounds better on a voice is a listening decision;
    the renders are in the user's listening folder.
  - Latency: UAD 86 samples; Velvet 755 (the de-esser's look-ahead, V3).

## V14 - Known limits

(1.0; see V19-V21 for 3.0, which closes the first three.)
- **Training data (1.0):** three voices of read speech. 3.0: 111 voices (V19). Still read speech: no labelled rap or
  sung material exists in a free full-band corpus; the artist's own rap takes are used for evaluation (V19, V21).
- **Latency (1.0):** 14 ms plus the oversampler, no zero-latency mode. 3.0: LIVE, zero latency (V20).
- **Commercial de-essers (1.0):** not benchmarked. 3.0: a third-party sibilance-aware de-esser, measured (V21).

## V15 - TAPE head bump

A playback head's contact length (~5 mm) resonates with the tape speed (15 ips = 0.381 m/s) near v / L = 76 Hz. TAPE
adds that bump after the model: +1.5 dB at 70 Hz, Q 1, linear. It is designed from the physics, not measured from any
product (the UAD measurement in V13 showed a different, much larger lift, which was not copied). Measured: +1.48 dB at
70 Hz, +0.94 dB at 50 Hz (`tests/`, check 6).

## V16 - Every smoother lands exactly (a bug found by the independent audit)

The audit measured host bypass at 2.5e-6 instead of 0. A one-pole recursion v += (target - v) k stalls once its step
(distance x k) falls below half a float ulp: with k = 0.002 the bypass crossfade stopped at 0.999985 and never reached
the snap threshold (1e-6), leaving 0.0015 % of the processed signal in the "bypassed" output. The same applied to the
HEAT on/off and MIX glides and the output gain. All of them now use one glide that snaps within 1e-4 of the target,
and full bypass returns the dry samples themselves. Audit: 37/37, bypass error 0.

## V17 - Romanian detector retrain (2.0 research, not adopted)

The 1.0 detector was trained on English read speech (CMU ARCTIC), where "s" dominates: held-out, it flags only 28-49 %
of ch / zh / jh frames - the sounds of Romanian ce/ci, ge/gi, j and ț. Tried for 2.0 (research/train_v2.py,
eval_v2.py, romanian_data.py): class-balanced and energy-weighted training, plus silver labels from the artist's own
Romanian takes (frames labelled only where the acoustics leave no doubt; two takes held out). Judged on outcomes with
the shipping C++ engine (bench_cpp.py), not on frame scores: the retrained detectors removed about 0.2 points more of
the harsh energy but cut vowels 3-4x more. On the held-out Romanian take, 1.0 already leaves at most one harsh
sibilant uncut and leaves vowels untouched. 1.0's detector stays.

## V18 - De-plosive (2.0 research, not built)

Plosive pops on the artist's takes are 50-110 ms bursts, mostly below 90 Hz, the strong ones reaching 100-250 Hz.
Two prototypes: band-to-body ratio learning (research/deplosive_proto.py: cut 75-99 % of the time, rejected) and a
burst-gated detector in the sub-80 Hz band with zero-phase 20-250 Hz removal (deplosive2.py). On synthetic pops added
to ARCTIC (synth_pops.py) the burst-gated design reached -17.4 dB of pop left vs -7.3 dB for LOW CUT 90 alone, at a
small thinning cost. On the artist's real takes it removed nothing beyond LOW CUT 90 (median 0.0 dB on 42 pops) while
acting 5 % of the time elsewhere.

The reason (research/pop_vs_onset.py): after LOW CUT 90 the pops stand no higher in 90-250 Hz than ordinary word
starts - median +2.0 dB over the following vowel for the 42 pops vs +1.8 dB for 447 ordinary onsets (p90 +5.9 vs
+8.0). LOW CUT 90 already takes the sub-90 Hz thump, and what is left is indistinguishable from the voice's own onsets
by level, so any detector loose enough to find it would also thin normal words. The release rule was "ship only if it
clearly beats a static low cut"; it does not, so 2.0 ships no de-plosive. Recommended for pops: LOW CUT 90 (the
artist's chains already use it). A future attempt needs pops marked by ear as ground truth.

## V19 - 3.0 detector: 111 voices instead of three

1.0 learned sibilance from three CMU ARCTIC voices. 3.0 adds 108 voices of the CSTR VCTK Corpus 0.92 (University of
Edinburgh, CC BY 4.0; 60 utterances each, 48 kHz, many accents). VCTK has no phone labels, so they come from the
Montreal Forced Aligner (MIT) with its English (US) ARPA acoustic model and dictionary v3.0.0 (CC BY 4.0, licences
checked on the models' own pages). Checked first on ARCTIC, where labels exist: MFA's sibilant frames agree with the
release labels at F1 0.956-0.978 away from boundaries (`research/mfa_arctic_check.py`).

Same features, same network size (19 -> 32 -> 1), same recipe as 1.0 (`research/train3.py`: 44.1 / 48 / 96 kHz, half
through random recording chains). Twelve VCTK voices (six female, six male, twelve accents; chosen by a fixed rule
before any result: `research/corpus3.py`) are never trained on by the evaluation models.

Frame level, held out (`train3.py`, `eval3_v1.py`):

| | 1.0 | 3.0 |
|---|---|---|
| 12 VCTK voices, F1 clean / chain | 0.721 / 0.699 | 0.816 / 0.789 |
| sibilant energy caught clean / chain | 92.4 / 90.5 % | 96.9 / 95.3 % |
| vowel frames flagged clean / chain | 0.15 / 0.07 % | 0.13 / 0.07 % |
| the artist's 11 Romanian takes (no model saw them): unvoiced / voiced sibilant energy caught | 85.4 / 55.4 % | 89.8 / 66.1 % |
| same, vowel frames flagged | 0.145 % | 0.169 % |

On ARCTIC (each voice by a model trained without it) 3.0 catches more on every voice and is better on slt and jmk; on
bdl it flags more f / th / v frames (F1 0.794 -> 0.774). At the outcome level (V21) the two detectors are equal within
the benchmark's resolution - 3.0's gain is coverage: the rarer sibilants (zh, ch, j - Romanian j, ge / gi, ce / ci)
and accents 1.0 never heard.

## V20 - LIVE: a zero-latency de-esser

STUDIO looks 4 ms ahead and cuts with linear-phase bands: 14 ms of latency, compensated by the host when mixing, but
too late to hear yourself through the plugin while recording. LIVE removes the latency completely:

- **Detector:** causal (`DetectorConfig::live()`): each frame is used as soon as its 12 ms window is complete, features
  are smoothed over the past only, and LIVE has its own network trained on exactly those features (`train3.py live`).
  Training the label a few ms ahead of the frame (3 or 6 ms) changed nothing measurable, so the label sits at the
  frame centre, as in STUDIO.
- **Filter:** the same water-filling gains as STUDIO, the same band magnitude, made minimum phase every 2 ms (real
  cepstrum: log |H| -> cepstrum -> fold -> exp). A minimum-phase filter has no delay: its energy is at the front.
  The first design, a cascade of ten bell filters, was rejected after the benchmark: wanting a flat -18 dB, the bells
  give -18 dB at their centres but -1 to -8 dB between them (a simulation confirmed it), and LIVE left 3-6 % harsh
  esses where STUDIO left 0.3-0.5 %. With the minimum-phase filter LIVE's cut matches STUDIO's (synthetic voice:
  13.9 vs 13.8 dB).
- **Exactness:** with nothing to cut the output is the input itself, bit for bit, at any block size.
- **HEAT:** the oversampler keeps 1.7 ms at 44.1 / 48 kHz (0.4 ms at 96 kHz). In LIVE with HEAT off that delay goes
  too: the plugin has no latency at all, and switching HEAT in LIVE is a latency change handled like an oversampling
  change (host told first, a short fade). In STUDIO the timing never depends on HEAT, as in 1.0.

On the 12 held-out VCTK voices LIVE reaches 1.0 % (clean) / 1.4 % (chain) harsh esses left at full strength (STUDIO
0.4 / 0.6 %); on all 15 voices, at equal harshness, its change on vowels is 2-3x STUDIO's (0.008 vs 0.003 dB) and on
stops about the same (V21).

## V21 - Benchmark: Velvet 1.0, 3.0 STUDIO, 3.0 LIVE, a third-party de-esser, the classic designs

`research/bench3.py`: 15 voices no Velvet model was trained on (the 12 VCTK test voices; slt, bdl and jmk by models
trained without them), one-minute takes, clean and through random recording chains, every de-esser swept over its
strength. The third-party de-esser is nClear Sibalance 1.1.0 (installed by the artist; 20 ms look-ahead, reported and
compensated), rendered black-box through the independent VST3 host at Sensitivity 50 and 80. Collateral is measured
at equal harshness (the same share of harsh esses left), so a de-esser cannot look gentle by doing less.

All 15 voices, change at equal harshness (`research/bench3_results.txt`; "vowels" / "stops" = mean |change| of their
2-16 kHz energy away from sibilant edges, "soft esses" = what already-soft esses lose - the lisp risk):

| harsh esses left (of 17.3 % clean / 27.1 % chain) | | Velvet 1.0 | Velvet 3.0 | 3.0 LIVE | Sibalance | classic abs | classic rel |
|---|---|---|---|---|---|---|---|
| 5.2 % clean | vowels | 0.003 dB | 0.003 | 0.008 | 0.240 | 0.105 | 0.117 |
| | stops | 0.11 dB | 0.11 | 0.13 | 1.19 | 0.55 | 0.91 |
| | soft esses | -0.3 dB | -0.3 | -0.7 | -3.2 | -1.4 | -2.5 |
| 8.1 % chain | vowels | 0.004 dB | 0.004 | 0.009 | 0.333 | 0.124 | 0.187 |
| | stops | 0.15 dB | 0.15 | 0.16 | 1.62 | 0.67 | 1.27 |
| | soft esses | -0.3 dB | -0.3 | -0.5 | -3.7 | -1.3 | -2.9 |
| 2.6 % clean | vowels | 0.009 dB | 0.009 | 0.019 | 0.370 | 0.257 | 0.244 |
| 4.1 % chain | vowels | 0.009 dB | 0.011 | 0.022 | 0.482 | 0.285 | 0.378 |

(Sibalance at Sensitivity 50; 80 is slightly worse everywhere.) Lowest harshness each reaches at full strength
(clean / chain): Velvet 3.0 0.9 / 1.1 %, 3.0 LIVE 1.7 / 2.1 %, 1.0 1.1 / 1.2 %, Sibalance 0.6 / 0.6 %, classic 1.2-1.3 /
1.6-1.7 %. Sibalance goes lower by cutting harder everywhere: even its gentlest setting (-3 dB) removes more than half
of the harsh esses (8.8 % of 17.3 % left), and at every harshness both reach, it changes everything else far more.

- **Velvet vs the third-party de-esser:** 30-80x less change on vowels, 7-11x less on stops, 5-12x less taken from soft
  esses, at every harshness both reach; LIVE, with zero latency, still 20-40x less on vowels than Sibalance with its
  20 ms look-ahead.
- **3.0 vs 1.0 (STUDIO):** equal within the benchmark's resolution on these voices (the gain is coverage, V19); 3.0
  reaches a lower harshness at full strength (0.9 vs 1.1 % clean).
- **The artist's own 12 takes** (`research/realtakes3.py`, `realtakes3_results.txt`; acoustic classes per 4 ms frame,
  no model saw these takes): harsh esses left uncut at AMOUNT 50: 1.0 16, 3.0 8, 3.0 LIVE 6; at AMOUNT 70: 15, 7, 5.
  Vowels: mean change 0.0058 dB (1.0) vs 0.0062 dB (3.0) at AMOUNT 50 - both inaudible.

## V22 - Sample rates up to 192 kHz

The detector's features are defined in Hz and seconds, so they do not depend on the rate. 1.0 was tested at 44.1, 48
and 96 kHz; 3.0's suite adds 192 kHz for both detectors and both de-essers (the synthetic voice: sibilants found, vowels
not, the exact-delay and LIVE pass-through checks, LISTEN, level independence).

## V23 - Older projects open as they were saved

A project saved by 1.0 has no De-Ess Mode. Every parameter a saved state does not contain now takes its default
(STUDIO), whatever the instance was set to before - not the instance's current value. A 1.0 project therefore opens
with the STUDIO de-esser and its saved settings (`tests/PluginTests.cpp`). The detector is 3.0's, so the de-essing is
not bit-identical to 1.0 (V19, V21).
