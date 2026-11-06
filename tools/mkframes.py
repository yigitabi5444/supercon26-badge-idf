"""Video -> 1-bit e-paper frames for the badge (960x672, black plane only).

    python3 mkframes.py <video> [fps=2] [start_s=0] [dur_s=15] [precrop=ffmpeg crop expr]
    rickroll: python3 mkframes.py src/full.mp4 2 43 15 636:476:109:2   (strips the 4:3 pillarbox)

Writes next to this script:
  frames_<fps>fps.bin   N frames x 80640 bytes, MONO_HLSB (MSB = leftmost px, 1 = black ink),
                        same layout as the black plane in idf_port (red plane = all zero).
  preview_<fps>fps.gif  what it should look like (no ghosting).
COLOR=1 env: black/white/red, frames_<fps>fps_bwr.bin = per frame black plane then red plane
             (161280 B, same as an idf_port image).

Ordered (Bayer) dither, not Floyd-Steinberg: the dot pattern stays put between
frames, so with fast/partial refresh only real changes move = less ghost noise.
"""
import os, subprocess, sys
import numpy as np
from PIL import Image

W, H = 960, 672
HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "main")  # output next to main.c

# ponytail: 8x8 Bayer built by recursion, standard construction
B = np.array([[0]])
for _ in range(3):
    B = np.block([[4 * B, 4 * B + 2], [4 * B + 3, 4 * B + 1]])
BAYER = (B + 0.5) / B.size  # thresholds in (0,1)
THRESH = np.tile(BAYER, (H // 8, W // 8))


def frames(video, fps, start, dur, precrop=None, color=False):
    pre = f"crop={precrop}," if precrop else ""
    vf = f"{pre}fps={fps},scale={W}:{H}:force_original_aspect_ratio=increase,crop={W}:{H},format={'rgb24' if color else 'gray'}"
    raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-ss", str(start), "-t", str(dur), "-i", video,
         "-vf", vf, "-f", "rawvideo", "-"], check=True, capture_output=True).stdout
    return np.frombuffer(raw, np.uint8).reshape((-1, H, W, 3) if color else (-1, H, W))


def to_ink(gray, contrast=1.3):
    # calibration knob: contrast/gamma; e-ink midtones look washed out
    g = np.clip((gray / 255.0 - 0.5) * contrast + 0.5, 0, 1)
    return g < THRESH  # True = black ink


PAL = np.array([[255, 255, 255], [0, 0, 0], [255, 0, 0]], float)  # white, black, red


def to_bwr(rgb, contrast=1.3, spread=96, red_bias=12000):
    """Ordered-dither RGB to the 3 panel colours. Returns (black_ink, red_ink)."""
    x = np.clip((rgb - 127.5) * contrast + 127.5, 0, 255)
    x = x + (THRESH[..., None] - 0.5) * spread  # calibration knob: spread = dither strength
    d = ((x[..., None, :] - PAL) ** 2).sum(-1)
    d[..., 2] += red_bias  # calibration knob: higher = only saturated reds go red
    idx = d.argmin(-1)
    return idx == 1, idx == 2


def pack(ink):
    return np.packbits(ink, axis=1, bitorder="big").tobytes()  # 120 B/row, MSB left


def selftest():
    ink = np.zeros((H, W), bool)
    ink[0, 0] = ink[0, 9] = ink[H - 1, W - 1] = True
    p = pack(ink)
    assert len(p) == W * H // 8 == 80640
    assert p[0] == 0x80 and p[1] == 0x40 and p[-1] == 0x01 and sum(p) == 0xC1
    assert to_ink(np.zeros((H, W))).all() and not to_ink(np.full((H, W), 255)).any()
    for c, want in (((0, 0, 0), (1, 0)), ((255, 255, 255), (0, 0)), ((255, 0, 0), (0, 1))):
        b, r = to_bwr(np.full((H, W, 3), c, float))
        assert (b.all(), r.all()) == (bool(want[0]), bool(want[1])) and not (b & r).any(), c


def main():
    selftest()
    video = sys.argv[1]
    fps = float(sys.argv[2]) if len(sys.argv) > 2 else 2
    start = float(sys.argv[3]) if len(sys.argv) > 3 else 0
    dur = float(sys.argv[4]) if len(sys.argv) > 4 else 15
    color = os.environ.get("COLOR") == "1"  # COLOR=1: black+red planes, 161280 B/frame
    fr = frames(video, fps, start, dur, sys.argv[5] if len(sys.argv) > 5 else None, color)
    tag = f"{fps:g}fps" + ("_bwr" if color else "")
    planes = [to_bwr(f) for f in fr] if color else [(to_ink(f), None) for f in fr]
    with open(os.path.join(HERE, f"frames_{tag}.bin"), "wb") as out:
        for b, r in planes:
            out.write(pack(b) + (pack(r) if color else b""))
    def show(b, r):
        im = np.where(b[..., None], PAL[1], PAL[0])
        if r is not None:
            im = np.where(r[..., None], PAL[2], im)
        return Image.fromarray(im.astype(np.uint8)).resize((480, 336))
    inks = planes
    imgs = [show(b, r) for b, r in planes]
    imgs[0].save(os.path.join(HERE, f"preview_{tag}.gif"), save_all=True,
                 append_images=imgs[1:], duration=int(1000 / fps), loop=0)
    print(f"{len(inks)} frames, {len(inks) * 80640 * (2 if color else 1)} bytes -> frames_{tag}.bin")


if __name__ == "__main__":
    main()
