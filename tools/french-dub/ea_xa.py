"""EA-XA v2 encoder (the codec of version 3 EA SCHl streams: 28-sample frames, 15 bytes).

Reference decoder (vgmstream, coding/ea_xa_decoder.c, decode_ea_xa_v2):
  header byte: coefficients = high nibble, shift s = low nibble;
  for each signed nibble n (high nibble first):
      x = ((n << 28) >> (s + 8)  +  c1 * h1  +  c2 * h2) >> 8, clamped to 16 bits; h2 = h1; h1 = x
  with (c1, c2) = (0, 0), (240, 0), (460, -208), (392, -220).
  Byte 0xEE: raw frame, h1 and h2 (16-bit big endian) then 28 big-endian 16-bit samples.

The encoder works on many streams at once (numpy): for each frame it tries the 4 coefficient sets
and the 13 shifts in closed loop (with the reconstructed history) and keeps the pair with the
smallest squared error. Its output decodes in vgmstream to exactly the signal it reconstructs.
"""
import numpy as np

FRAME = 28
COEFS = np.array([(0, 0), (240, 0), (460, -208), (392, -220)], dtype=np.int64)
SHIFTS = np.arange(13, dtype=np.int64)
# candidates: (coefficient index, shift)
_CI, _SH = np.meshgrid(np.arange(4), SHIFTS, indexing="ij")
_CI, _SH = _CI.ravel(), _SH.ravel()
_C1, _C2 = COEFS[_CI, 0], COEFS[_CI, 1]
_STEP = np.left_shift(1, 20 - _SH)  # n * 2^(20-s) before the >> 8


def raw_frame(x, h1, h2):
    """0xEE frame: history then 28 samples, big endian."""
    return b"\xee" + np.array([h1, h2], dtype=">i2").tobytes() + np.asarray(x, dtype=">i2").tobytes()


def encode_batch(signals):
    """Encode a batch of int16 signals of the same length (a multiple of 28).

    The first frame of each signal is raw (0xEE, 61 bytes), the next ones ADPCM (15 bytes).
    Returns (data, rebuilt): the frames of each signal end to end (bytes) and the signal as the game
    will decode it.
    """
    x = np.stack([np.asarray(s, dtype=np.int64) for s in signals])  # (B, N)
    b, n = x.shape
    frames = n // FRAME
    out = np.zeros_like(x)
    adpcm = np.zeros((b, frames - 1, 15), dtype=np.uint8)
    # frame 0 is raw: the history is its last two samples
    out[:, :FRAME] = x[:, :FRAME]
    h1 = x[:, FRAME - 1].copy()
    h2 = x[:, FRAME - 2].copy()
    raws = [raw_frame(x[i, :FRAME], h1[i], h2[i]) for i in range(b)]
    k = len(_C1)
    c1 = np.broadcast_to(_C1, (b, k))
    c2 = np.broadcast_to(_C2, (b, k))
    step = np.broadcast_to(_STEP, (b, k))
    rows = np.arange(b)
    for t in range(1, frames):
        target = x[:, t * FRAME:(t + 1) * FRAME]  # (B, 28)
        a1 = np.repeat(h1[:, None], k, axis=1)
        a2 = np.repeat(h2[:, None], k, axis=1)
        error = np.zeros((b, k), dtype=np.int64)
        nibbles = np.zeros((b, k, FRAME), dtype=np.int64)
        for j in range(FRAME):
            pred = c1 * a1 + c2 * a2
            q = np.clip(np.rint((target[:, j:j + 1] * 256 - pred) / step), -8, 7).astype(np.int64)
            y = np.clip((q * step + pred) >> 8, -32768, 32767)
            error += (y - target[:, j:j + 1]) ** 2
            nibbles[:, :, j] = q
            a2, a1 = a1, y
        best = np.argmin(error, axis=1)
        q = nibbles[rows, best]  # (B, 28)
        # exact rebuild of the chosen candidate
        cc1, cc2, pp = _C1[best], _C2[best], _STEP[best]
        for j in range(FRAME):
            y = np.clip((q[:, j] * pp + cc1 * h1 + cc2 * h2) >> 8, -32768, 32767)
            out[:, t * FRAME + j] = y
            h2, h1 = h1, y
        nib = q & 0x0F
        adpcm[:, t - 1, 0] = (_CI[best] << 4) | _SH[best]
        adpcm[:, t - 1, 1:] = (nib[:, 0::2] << 4) | nib[:, 1::2]
    return [raws[i] + adpcm[i].tobytes() for i in range(b)], out
