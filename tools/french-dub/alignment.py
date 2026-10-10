"""Time offset between two audio tracks, by correlating a channel without dialogue (front left).

offset(ref, other) -> seconds to add to `other` to land on `ref` (positive = delay it), and a
correlation score (close to 1 = same sounds).
"""
import subprocess

import numpy as np

RATE = 4000


def _channel(ffmpeg, inputs, filtergraph):
    cmd = [ffmpeg, "-v", "error", *inputs, "-filter_complex", filtergraph + f",aresample={RATE}",
           "-f", "f32le", "-ac", "1", "-"]
    data = subprocess.run(cmd, check=True, capture_output=True).stdout
    return np.frombuffer(data, dtype=np.float32)


def offset(ffmpeg, ref_inputs, ref_filter, other_inputs, other_filter):
    a = _channel(ffmpeg, ref_inputs, ref_filter)
    b = _channel(ffmpeg, other_inputs, other_filter)
    n = 1 << int(np.ceil(np.log2(len(a) + len(b))))
    corr = np.fft.irfft(np.fft.rfft(a, n) * np.conj(np.fft.rfft(b, n)), n)
    lag = int(np.argmax(corr))
    if lag > n // 2:
        lag -= n
    score = corr.max() / (np.linalg.norm(a) * np.linalg.norm(b) + 1e-12)
    return lag / RATE, float(score)
