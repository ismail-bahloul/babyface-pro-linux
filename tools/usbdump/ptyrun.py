#!/usr/bin/env python3
"""Run a TUI in a properly-sized pty (120x40) and capture its render
for N seconds — the agent tool's own pty is only 2 rows tall, which
makes any full-screen TUI panic in ratatui."""
import os, pty, sys, time, select, signal, struct, fcntl, termios

def run_tui(cmd, secs, outfile, cols=120, rows=40):
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', rows, cols, 0, 0))
    pid = os.fork()
    if pid == 0:
        os.setsid()
        try:
            fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
        except OSError:
            pass
        os.dup2(slave, 0); os.dup2(slave, 1); os.dup2(slave, 2)
        os.close(master)
        os.execvp(cmd[0], cmd)
    out = b''
    end = time.time() + secs
    while time.time() < end:
        r, _, _ = select.select([master], [], [], 0.2)
        if r:
            try:
                data = os.read(master, 65536)
                if not data:
                    break
                out += data
            except OSError:
                break
    try:
        os.kill(pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    os.close(master)
    open(outfile, 'wb').write(out)
    return len(out)

if __name__ == '__main__':
    cmd = sys.argv[1:-3]
    secs = float(sys.argv[-3])
    outfile = sys.argv[-2]
    cols = int(sys.argv[-1]) if len(sys.argv) > 4 else 120
    n = run_tui(cmd, secs, outfile, cols=cols)
    print(f'captured {n} bytes')
