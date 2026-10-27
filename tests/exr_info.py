"""The tests' exrinfo runs: its output lines, checked. A run that fails or
prints no pixel (seen under heavy machine load: exit 0, no pixel lines) is
retried and logged, never read as an image of zeros."""
import subprocess
import sys


def lines(exrinfo, *args, tries=3):
    """exrinfo's stdout lines for `args` (image [x y [w h]])."""
    want = "pixel " if len(args) >= 3 else "size "
    for attempt in range(tries):
        r = subprocess.run([exrinfo] + [str(a) for a in args], capture_output=True, text=True, timeout=60)
        out = r.stdout.splitlines()
        if r.returncode == 0 and any(l.startswith(want) for l in out):
            return out
        print(f"exrinfo {' '.join(map(str, args))} (try {attempt + 1}): rc {r.returncode:#x}, "
              f"stdout {r.stdout[-300:]!r}, stderr {r.stderr[-300:]!r}", file=sys.stderr)
    raise RuntimeError(f"exrinfo {' '.join(map(str, args))} failed {tries} times")


def pixel(exrinfo, *args):
    """The channel values exrinfo prints for `args` (image x y [w h])."""
    return {l.split()[1]: float(l.split()[2]) for l in lines(exrinfo, *args) if l.startswith("pixel ")}
