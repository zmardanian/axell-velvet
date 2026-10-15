#pragma once
// Axell Velvet - factory presets (framework independent: the test suite renders and checks every one).
// Factory presets keep LEVEL (it belongs to the recording; set it with LEARN). The MY VOICE set is made for Axell's own
// voice and recording chain (a large-diaphragm condenser into an interface preamp with a FET-style compressor), measured
// from his dry vocal: it also sets LEVEL to that recording level. Names describe the use; no third-party branding.
#include "Engine.h"

#include <array>

namespace axv
{
struct FactoryPreset
{
    const char* category;
    const char* name;
    const char* description;
    Params params;
    bool setsLevel = false;
};

// level the MY VOICE presets set: -18 LUFS nominal minus the measured integrated loudness of the dry vocal
constexpr float kMyVoiceLevelDb = 15.0f;

inline Params makeParams (float amount, float range, int focus, bool heat, int mode, float drive, float guard, float tone,
                          float mix, float output = 0.0f, bool deess = true, int lowCut = 1)
{
    Params p;
    p.deessOn = deess;
    p.amountPct = amount;
    p.rangeDb = range;
    p.focus = focus;
    p.heatOn = heat;
    p.mode = mode;
    p.drive = drive;
    p.guardPct = guard;
    p.tone = tone;
    p.mixPct = mix;
    p.outputDb = output;
    p.osLog2 = 4;
    p.lowCut = lowCut;
    return p;
}

constexpr int kNumFactoryPresets = 25;

inline std::array<FactoryPreset, kNumFactoryPresets> factoryPresets()
{
    // MY VOICE: LEVEL for the measured recording level, LOW CUT 90 Hz (the dry vocal carries strong rumble below 20 Hz,
    // only 16.5 dB under the voice; the voice itself sits at 131-187 Hz)
    auto mine = [] (Params p) { p.levelDb = kMyVoiceLevelDb; p.lowCut = 3; return p; };
    return { {
        // ESSENTIALS
        { "ESSENTIALS", "Init", "Smart de-essing at 50 % and warm tape at drive 4.", makeParams (50, 18, 0, true, kTape, 4.0f, 60, 0, 100) },
        { "ESSENTIALS", "De-Ess Only", "Just the de-esser: harsh esses down, nothing else touched.", makeParams (50, 18, 0, false, kTape, 4.0f, 60, 0, 100) },
        { "ESSENTIALS", "Gentle Polish", "A light touch of both: softer esses and a little tape glue.", makeParams (35, 15, 0, true, kTape, 3.0f, 60, 0, 100) },
        { "ESSENTIALS", "Heat Only", "Tape warmth with the de-esser off (S-GUARD still keeps the esses clean).",
          makeParams (0, 18, 0, true, kTape, 5.0f, 80, 0, 100, 0.0f, false) },
        // RAP & HIP-HOP
        { "RAP & HIP-HOP", "Rap Lead - Upfront", "Console punch and presence for a lead that sits on top of the beat.",
          makeParams (55, 18, 0, true, kConsole, 5.0f, 70, 10, 100) },
        { "RAP & HIP-HOP", "Rap Lead - Warm Tape", "Thick, glued, slightly dark: classic tape on a rap lead.", makeParams (50, 18, 0, true, kTape, 6.0f, 70, -10, 100) },
        { "RAP & HIP-HOP", "Trap Vocal - Bright & Smooth", "Air on top without hiss: firm de-essing, bright tone, esses kept out of the drive.",
          makeParams (65, 18, 0, true, kTape, 4.0f, 85, 20, 100) },
        { "RAP & HIP-HOP", "Boom Bap Grit", "Driven console in parallel for a gritty, dusty edge.", makeParams (45, 15, 0, true, kConsole, 7.0f, 60, -15, 80) },
        { "RAP & HIP-HOP", "Ad-Libs Thick", "Tube thickness for ad-libs and hype layers behind the lead.", makeParams (60, 18, 0, true, kTube, 6.0f, 70, 0, 70) },
        { "RAP & HIP-HOP", "Doubles & Stacks", "Stacked takes add up their esses: stronger de-essing and darker tape so the stack stays smooth.",
          makeParams (70, 21, 0, true, kTape, 5.0f, 90, -20, 100) },
        { "RAP & HIP-HOP", "Hype Shouts", "Hard console drive blended in for energy without losing the words.", makeParams (60, 18, 0, true, kConsole, 8.0f, 80, 0, 60) },
        // SINGING
        { "SINGING", "R&B Silk", "Smooth tube sheen with esses kept out of the drive.", makeParams (55, 18, 0, true, kTube, 4.0f, 80, 10, 100) },
        { "SINGING", "Pop Vocal Air", "Bright and present: the sharp S hiss controlled, a little tape, more air.", makeParams (60, 18, 1, true, kTape, 3.0f, 80, 25, 100) },
        { "SINGING", "Soul Tube Bloom", "Rich even harmonics that bloom on the loud notes.", makeParams (45, 15, 0, true, kTube, 6.5f, 60, 0, 85) },
        { "SINGING", "Intimate & Close", "For close-miked, breathy takes: softer esses, gentle tape, a darker top.", makeParams (55, 15, 0, true, kTape, 3.0f, 70, -20, 100) },
        { "SINGING", "Belting Control", "Loud notes get bright and spitty: wider range, console weight, esses fully guarded.",
          makeParams (65, 21, 0, true, kConsole, 4.0f, 95, 0, 100) },
        // SPECIAL
        { "SPECIAL", "Podcast & Spoken Word", "Clear speech: steady de-essing and a touch of console body.", makeParams (55, 18, 0, true, kConsole, 2.0f, 60, 0, 100) },
        { "SPECIAL", "Harsh Mic Rescue", "For bright, spitty recordings: strong de-essing, wide range, a darker tape top.",
          makeParams (80, 24, 0, true, kTape, 3.0f, 100, -30, 100) },
        { "SPECIAL", "Lo-Fi Saturated", "Heavily driven tube with a dark top for a vintage, lo-fi colour.", makeParams (40, 18, 0, true, kTube, 9.0f, 60, -45, 100) },
        { "SPECIAL", "Parallel Crunch", "Full console crunch blended under the clean vocal: energy with clarity.", makeParams (50, 18, 0, true, kConsole, 9.0f, 70, 0, 35) },
        // MY VOICE (sets LEVEL)
        { "MY VOICE", "My Voice - Everyday Lead", "Made for Axell's voice and chain: both ess families (s at 8.7 kHz, sh at 3.3 kHz) controlled, warm tape, a touch darker.",
          mine (makeParams (55, 18, 0, true, kTape, 4.5f, 75, -10, 100)), true },
        { "MY VOICE", "My Voice - Hard Rap Lead", "Axell's chain, aggressive verses: firmer de-essing and console punch; the compressor's emphasised esses stay out of the drive.",
          mine (makeParams (60, 18, 0, true, kConsole, 6.0f, 85, 0, 100)), true },
        { "MY VOICE", "My Voice - Melodic Hook", "Axell's chain, sung hooks: smooth tube harmonics, esses fully guarded.",
          mine (makeParams (55, 18, 0, true, kTube, 4.5f, 90, 5, 100)), true },
        { "MY VOICE", "My Voice - Doubles & Ad-libs", "Axell's chain, stacked layers: strong de-essing and darker tape so the stack never hisses.",
          mine (makeParams (70, 21, 0, true, kTape, 6.0f, 90, -20, 90)), true },
        { "MY VOICE", "My Voice - Dark & Intimate", "Axell's chain, close and quiet parts: gentle tube, darker top, soft esses.",
          mine (makeParams (50, 15, 0, true, kTube, 3.5f, 80, -30, 100)), true },
    } };
}
} // namespace axv
