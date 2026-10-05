#!/usr/bin/env python3
# make_flamer_sounds.py -- the flamethrower's own sounds, synthesised.
#
# Seb, 2026-10-05: "flamethrower needs an ignition, then whoosh! sound and a wind
# down sound". Like the view model (make_v_flamer.py) these are OURS and
# GENERATED, never committed: filtered noise and a few resonators, seeded, so
# every build writes the same bytes. qc/build.sh and the release script run it.
#
#   flamer_ignite.wav    the trigger: the piezo spark's two ticks, a breath of
#                        gas, the catch (a low "foomp") and the WHOOSH as the
#                        stream lights along its length. The roar (the torches'
#                        own loop, ambience/fire1.wav) comes in under its tail.
#   flamer_winddown.wav  the release: the valve shuts on a soft thump, the
#                        flame gutters down through a falling filter with a few
#                        sputters, and the last of the gas hisses out.
#
# Usage: python3 make_flamer_sounds.py <output directory>
# 22050 Hz 16-bit mono PCM, Quake's own format. Pure Python, no numpy.

import math, os, random, struct, sys

RATE = 22050


class Biquad:
	"""RBJ cookbook biquad, coefficients settable per block."""
	def __init__(self):
		self.x1 = self.x2 = self.y1 = self.y2 = 0.0
		self.b0 = 1.0; self.b1 = self.b2 = self.a1 = self.a2 = 0.0

	def set(self, kind, freq, q):
		freq = max(20.0, min(freq, RATE * 0.45))
		w = 2.0 * math.pi * freq / RATE
		cw, sw = math.cos(w), math.sin(w)
		alpha = sw / (2.0 * q)
		if kind == 'lp':
			b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2
		elif kind == 'hp':
			b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2
		else:	# band-pass, constant 0 dB peak
			b0 = alpha; b1 = 0.0; b2 = -alpha
		a0 = 1 + alpha
		self.b0, self.b1, self.b2 = b0 / a0, b1 / a0, b2 / a0
		self.a1, self.a2 = -2 * cw / a0, (1 - alpha) / a0

	def run(self, x):
		y = self.b0 * x + self.b1 * self.x1 + self.b2 * self.x2 - self.a1 * self.y1 - self.a2 * self.y2
		self.x2, self.x1, self.y2, self.y1 = self.x1, x, self.y1, y
		return y


def lerp(a, b, f):
	return a + (b - a) * f


def env_ad(t, attack, decay):
	"""0 before 0, linear up over `attack`, exponential down with time constant `decay`."""
	if t < 0:
		return 0.0
	if t < attack:
		return t / attack
	return math.exp(-(t - attack) / decay)


def smooth(t, a, b):
	if t <= a:
		return 0.0
	if t >= b:
		return 1.0
	x = (t - a) / (b - a)
	return x * x * (3 - 2 * x)


def filtered(n, kind, freqfn, qfn, rng, block=32):
	"""White noise through one time-varying biquad; freqfn/qfn take seconds."""
	f = Biquad(); out = [0.0] * n
	for i in range(n):
		if i % block == 0:
			t = i / RATE
			f.set(kind, freqfn(t), qfn(t))
		out[i] = f.run(rng.uniform(-1.0, 1.0))
	return out


def ignite(rng):
	dur = 0.80; n = int(dur * RATE); out = [0.0] * n
	# 1. the PIEZO SPARK: two hard ticks 28 ms apart, each an impulse rung on
	# two high resonators (the striker and the electrode) for a few ms
	for t0, amp in ((0.000, 0.55), (0.028, 0.42)):
		for freq, q, a in ((2600, 9.0, 1.0), (5200, 12.0, 0.6)):
			bq = Biquad(); bq.set('bp', freq * rng.uniform(0.95, 1.05), q)
			start = int(t0 * RATE)
			for i in range(start, min(n, start + int(0.012 * RATE))):
				x = (rng.uniform(-1, 1) * 8.0 if i - start < 3 else 0.0)
				out[i] += bq.run(x) * amp * a * math.exp(-(i - start) / (0.0025 * RATE))
	# 2. a BREATH OF GAS from the valve, thin and high, under the spark
	gas = filtered(n, 'hp', lambda t: 3200, lambda t: 0.7, rng)
	for i in range(n):
		t = i / RATE
		out[i] += gas[i] * 0.10 * smooth(t, 0.005, 0.05) * (1 - smooth(t, 0.09, 0.16))
	# 3. the CATCH: a low "foomp" as the gas lights -- a sine falling 150 -> 55 Hz
	# under a burst of noise whose low-pass opens fast
	ph = 0.0
	thump = filtered(n, 'lp', lambda t: lerp(180, 1400, smooth(t, 0.075, 0.12)), lambda t: 0.9, rng)
	for i in range(n):
		t = i / RATE - 0.075
		if t < 0:
			continue
		ph += 2 * math.pi * lerp(150, 55, min(t / 0.09, 1.0)) / RATE
		e = env_ad(t, 0.006, 0.07)
		out[i] += math.sin(ph) * 0.75 * e + thump[i + 0] * 0.9 * env_ad(t, 0.004, 0.05)
	# 4. the WHOOSH: the stream lighting along its length -- noise through a
	# resonant band whose centre sweeps up as it flares and settles back, with a
	# low body under it; peaks at about 0.22 s and leaves the roar to carry on
	def wfreq(t):
		t -= 0.09
		if t < 0.14:
			return lerp(260, 2100, smooth(t, 0.0, 0.14))
		return lerp(2100, 850, smooth(t, 0.14, 0.6))
	whoosh = filtered(n, 'bp', wfreq, lambda t: 1.6, rng, block=16)
	body = filtered(n, 'lp', lambda t: 320, lambda t: 0.8, rng)
	for i in range(n):
		t = i / RATE - 0.09
		if t < 0:
			continue
		e = smooth(t, 0.0, 0.13) * (0.32 + 0.68 * math.exp(-max(0.0, t - 0.13) / 0.16))
		e *= 1 - smooth(t, 0.55, 0.71)	# into the roar
		out[i] += whoosh[i] * 1.5 * e + body[i] * 1.1 * e
	return out


def winddown(rng):
	dur = 1.25; n = int(dur * RATE); out = [0.0] * n
	# 1. the VALVE shuts: a short soft thump
	ph = 0.0
	for i in range(int(0.12 * RATE)):
		t = i / RATE
		ph += 2 * math.pi * lerp(110, 60, t / 0.12) / RATE
		out[i] += math.sin(ph) * 0.45 * env_ad(t, 0.003, 0.035)
	# 2. the flame GUTTERING down: roar-like noise, a low-pass falling from bright
	# to dull and a band that sinks with it, the level decaying with a little
	# flicker on it
	def lpf(t):
		return lerp(3000, 260, smooth(t, 0.0, 0.9))
	def bpf(t):
		return lerp(1100, 280, smooth(t, 0.0, 0.8))
	roar = filtered(n, 'lp', lpf, lambda t: 0.8, rng)
	band = filtered(n, 'bp', bpf, lambda t: 1.3, rng, block=16)
	flick = 1.0; fl_target = 1.0
	for i in range(n):
		t = i / RATE
		if i % int(RATE * 0.035) == 0:
			fl_target = rng.uniform(0.65, 1.0)
		flick += (fl_target - flick) * 0.004
		e = math.exp(-t / 0.28) * flick * smooth(t, 0.0, 0.02)
		out[i] += roar[i] * 0.95 * e + band[i] * 1.2 * e
	# 3. SPUTTERS: a few pops as the last fuel catches and dies, softer and duller
	# as it goes
	pops = sorted(rng.uniform(0.14, 0.85) for _ in range(5))
	for k, t0 in enumerate(pops):
		bq = Biquad(); bq.set('bp', rng.uniform(700, 1500) * (1 - 0.1 * k), 2.0)
		amp = 0.55 * math.exp(-t0 / 0.45)
		start = int(t0 * RATE)
		for i in range(start, min(n, start + int(0.05 * RATE))):
			out[i] += bq.run(rng.uniform(-1, 1)) * 2.5 * amp * env_ad((i - start) / RATE, 0.002, 0.012)
	# 4. the last HISS of gas out of the nozzle, fading away
	hiss = filtered(n, 'hp', lambda t: 2600, lambda t: 0.7, rng)
	for i in range(n):
		t = i / RATE
		out[i] += hiss[i] * 0.06 * smooth(t, 0.05, 0.25) * (1 - smooth(t, 0.7, 1.22))
	return out


def write_wav(path, samples, peak=0.85):
	m = max(1e-9, max(abs(s) for s in samples))
	g = peak / m
	# a 4 ms fade at both ends so no file starts or stops on a click it did not mean
	f = int(0.004 * RATE)
	data = bytearray()
	for i, s in enumerate(samples):
		e = min(1.0, (i + 1) / f, (len(samples) - i) / f)
		v = int(round(max(-1.0, min(1.0, s * g * e)) * 32767))
		data += struct.pack('<h', v)
	hdr = b'RIFF' + struct.pack('<I', 36 + len(data)) + b'WAVE'
	hdr += b'fmt ' + struct.pack('<IHHIIHH', 16, 1, 1, RATE, RATE * 2, 2, 16)
	hdr += b'data' + struct.pack('<I', len(data))
	os.makedirs(os.path.dirname(path) or '.', exist_ok=True)
	with open(path, 'wb') as fp:
		fp.write(hdr + data)


def main():
	if len(sys.argv) != 2:
		sys.exit('usage: make_flamer_sounds.py <output directory, e.g. ../m5/sound/m5>')
	outdir = sys.argv[1]
	write_wav(os.path.join(outdir, 'flamer_ignite.wav'), ignite(random.Random(2026100501)))
	write_wav(os.path.join(outdir, 'flamer_winddown.wav'), winddown(random.Random(2026100502)))
	print('make_flamer_sounds: wrote flamer_ignite.wav, flamer_winddown.wav to', outdir)


if __name__ == '__main__':
	main()
