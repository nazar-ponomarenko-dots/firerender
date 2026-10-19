#!/usr/bin/env python3
"""Synthesize player sounds: footsteps, jump, landing, rustle, wind.

Usage: python3 tools/make_sounds.py
"""
import math
import os
import random
import struct
import wave

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "audio")
RATE = 22050


def save(name, samples, target_rms=3000.0):
    n = len(samples)
    # normalize to the target RMS, but without clipping (peak limit)
    s = sum(x * x for x in samples) / n
    rms = math.sqrt(s) if s > 0 else 1.0
    peak = max(abs(x) for x in samples) or 1.0
    # target_rms — in int16 units (0..32767); rms/peak — in float scale (-1..1)
    k = min(target_rms / (rms * 32767.0 + 1e-9), 1.0 / peak)
    buf = bytearray()
    for x in samples:
        v = int(max(-1.0, min(1.0, x * k)) * 32767)
        buf += struct.pack("<h", v)
    with wave.open(os.path.join(OUT, name), "wb") as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(RATE)
        f.writeframes(bytes(buf))
    print(f"  + audio/{name}")


def white(rng):
    return rng.uniform(-1.0, 1.0)


# --- Footstep on grass/dirt ---------------------------------------------------
# Low-frequency "bump" (ground thump) + soft sole noise.
def make_step(seed, hardness=0.6, noise=0.3, dur=0.12):
    rng = random.Random(seed)
    n = int(RATE * dur)
    out = []
    for i in range(n):
        t = i / RATE
        bump = math.sin(2 * math.pi * (70 + 12 * rng.random()) * t) \
               * math.exp(-t * 30) * hardness * 0.62
        grit = white(rng) * math.exp(-t * 60) * noise * 0.55
        out.append(bump + grit)
    return out


# --- Jump: airy "lift-off" ----------------------------------------------------
# Short upward whistle (frequency rises) + exhale. Quiet — background detail.
def make_jump(seed=11):
    rng = random.Random(seed)
    dur = 0.32
    n = int(RATE * dur)
    out = []
    ph = 0.0
    for i in range(n):
        t = i / RATE
        f = 150 + 220 * (t / dur)          # sweep upward
        ph += 2 * math.pi * f / RATE
        whistle = math.sin(ph) * math.sin(math.pi * t / dur) ** 2 * 0.06
        breath = white(rng) * math.exp(-t * 14) * 0.22
        out.append(whistle + breath)
    return out


# --- Landing: dull thud + dust ------------------------------------------------
def make_land(seed=5):
    rng = random.Random(seed)
    dur = 0.30
    n = int(RATE * dur)
    out = []
    for i in range(n):
        t = i / RATE
        thud = math.sin(2 * math.pi * 62 * t) * math.exp(-t * 26) * 0.55
        dust = white(rng) * math.exp(-t * 40) * 0.25
        out.append(thud + dust)
    return out


# --- Vegetation rustle --------------------------------------------------------
# Three variants (alternate while walking): light / medium / dense with crunch.
def make_rustle(seed=3, dur=0.6, mod_f=18.0, crack_p=0.03, low=0.20):
    rng = random.Random(seed)
    n = int(RATE * dur)
    out = []
    flt = 0.0
    for i in range(n):
        t = i / RATE
        w = white(rng)
        flt = flt * (1.0 - low) + w * low              # muted noise
        swish = (w * 0.55 + flt * 0.85)
        mod = 0.55 + 0.45 * math.sin(t * mod_f) * math.sin(t * (mod_f * 0.35) + 0.8)
        crack = 0.0
        if rng.random() < crack_p:
            crack = white(rng) * math.exp(-t * 90) * 0.9
        env = math.sin(math.pi * t / dur) ** 0.7       # soft start/end
        out.append(swish * mod * env * 0.6 + crack * env * 0.7)
    return out


# --- Wind ---------------------------------------------------------------------
def make_wind(seed=7):
    rng = random.Random(seed)
    dur = 4.0
    n = int(RATE * dur)
    out = []
    b = 0.0
    for i in range(n):
        t = i / RATE
        b = b * 0.97 + white(rng) * 0.03        # brown noise
        gust = 0.65 + 0.35 * math.sin(t * 0.7) * math.sin(t * 0.23 + 1.3)
        out.append(b * gust * 1.1)
    return out


# --- Rain: white noise with fast droplet modulation (dense rain hiss) ---------
# (Unused: during rain the engine simply turns up the wind loop — see fx.c)

def main():
    os.makedirs(OUT, exist_ok=True)
    save("step1.wav", make_step(101, hardness=0.7, noise=0.30), target_rms=2500.0)
    save("step2.wav", make_step(202, hardness=0.55, noise=0.40), target_rms=2500.0)
    save("step3.wav", make_step(303, hardness=0.62, noise=0.35), target_rms=2500.0)
    save("jump.wav", make_jump(), target_rms=900.0)
    save("land.wav", make_land(), target_rms=3200.0)
    # three rustle variants: light / medium / dense with crunch
    save("rustle1.wav", make_rustle(31, dur=0.45, mod_f=26.0, crack_p=0.01),
         target_rms=1800.0)
    save("rustle2.wav", make_rustle(32, dur=0.60, mod_f=18.0, crack_p=0.03),
         target_rms=2000.0)
    save("rustle3.wav", make_rustle(33, dur=0.75, mod_f=12.0, crack_p=0.06,
                                    low=0.28), target_rms=2200.0)
    save("rustle.wav", make_rustle(3), target_rms=2000.0)   # generic (for maps)
    save("wind.wav", make_wind(), target_rms=2400.0)
    save("rain.wav", make_rain(), target_rms=2600.0)
    save("thunder.wav", make_thunder(), target_rms=4200.0)
    print("Done.")


if __name__ == "__main__":
    main()
