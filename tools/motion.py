"""How evenly the camera moves from frame to frame (XIVLEGACY_MOTIONLOG).

  python3 tools/motion.py <motion log>

The log has a line per presented frame: monotonic ns, camera yaw and pitch (degrees), camera position,
and the main loop's ticks since the frame before; '# <ns> <text>' lines are marks (the control FIFO's
`mark`). Each part between marks gets: fps, frame time spread, and for the camera's motion in that part,
how evenly it advances per frame: the spread (coefficient of variation) of turn speed or of travel
speed per frame, and the share of frames where it did not move at all while the rest did."""
import math
import statistics
import sys


def parts(path):
    name, rows, ticks = 'start', [], []
    for line in open(path):
        if line.startswith('#'):
            if rows:
                yield name, rows, ticks
            name, rows, ticks = line.split(None, 2)[2].strip(), [], []
            continue
        if line.startswith('T '):
            ticks.append(int(line.split()[1]))
            continue
        if line[:2] in ('W ',):
            continue
        f = line.split()
        if len(f) >= 7:
            rows.append((int(f[0]), float(f[1]), float(f[2]), float(f[3]), float(f[4]), float(f[5]), int(f[6])))
    if rows:
        yield name, rows, ticks


def spread(v):
    m = statistics.fmean(v)
    return statistics.pstdev(v) / m if m else 0.0


def report(name, rows, tick_ns=()):
    if len(rows) < 10:
        return
    dt = [(b[0] - a[0]) / 1e9 for a, b in zip(rows, rows[1:])]
    yaw = []
    for a, b in zip(rows, rows[1:]):
        d = b[1] - a[1]
        d = (d + 180.0) % 360.0 - 180.0
        yaw.append(abs(d))
    move = [math.dist(a[3:6], b[3:6]) for a, b in zip(rows, rows[1:])]
    ticks = [r[6] for r in rows[1:]]
    out = ['%-14s %4d frames %5.1f fps  frame time spread %.2f, worst %.0f ms' % (
        name, len(dt), len(dt) / sum(dt), spread(dt), 1000 * max(dt))]
    turning = sum(yaw) / sum(dt) > 5.0  # degrees a second
    if turning:
        rate = [y / t for y, t in zip(yaw, dt)]
        still = sum(1 for y in yaw if y < 0.01) / len(yaw)
        out.append('   turn: %.0f deg/s, per-frame speed spread %.2f, frames not turning %.0f%%' % (
            sum(yaw) / sum(dt), spread(rate), 100 * still))
    if sum(move) / sum(dt) > 0.5:
        speed = [m / t for m, t in zip(move, dt)]
        still = sum(1 for m in move if m < 1e-4) / len(move)
        out.append('   travel: %.2f units/s, per-frame speed spread %.2f, frames not moving %.0f%%' % (
            sum(move) / sum(dt), spread(speed), 100 * still))
    hist = {}
    for t in ticks:
        hist[min(t, 3)] = hist.get(min(t, 3), 0) + 1
    if len(tick_ns) > 3:
        iv = [(b - a) / 1e6 for a, b in zip(tick_ns, tick_ns[1:])]
        out.append('   main loop: %.1f ticks/s, interval spread %.2f (%.0f-%.0f ms)' % (
            len(iv) / (sum(iv) / 1000), spread(iv), min(iv), max(iv)))
    out.append('   main loop ticks a frame: ' + ', '.join('%s%s: %.0f%%' % (k, '+' if k == 3 else '', 100 * v / len(ticks))
                                                     for k, v in sorted(hist.items())))
    print('\n'.join(out))


if __name__ == '__main__':
    for name, rows, ticks in parts(sys.argv[1]):
        report(name, rows, ticks)
