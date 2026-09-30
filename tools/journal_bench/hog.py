# Keeps the disk busy from another process: writes a file over and over,
# flat out (heavy) or at about 50 MB/s (moderate), in 8 MB chunks.
import os, sys, time

mode, base = sys.argv[1], sys.argv[2]
path = os.path.join(base, "hog.bin")
chunk = os.urandom(8 << 20)
limit = 2 << 30
try:
    with open(path, "wb", buffering=0) as f:
        written = 0
        while True:
            t = time.perf_counter()
            f.write(chunk)
            written += len(chunk)
            if written >= limit:
                f.seek(0)
                written = 0
            if mode == "moderate":
                spare = len(chunk) / (50 << 20) - (time.perf_counter() - t)
                if spare > 0:
                    time.sleep(spare)
finally:
    try:
        os.remove(path)
    except OSError:
        pass
