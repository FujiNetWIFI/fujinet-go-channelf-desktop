#!/usr/bin/env python3
"""boipreplay.py -- record a FujiNet BoIP conversation and play it back.

The FujiNet apps talk to fujinet-pc over BoIP: SLIP frames (0xC0 delimited) of
FujiBus packets on one TCP connection, the console asking and FujiNet answering
(and pushing an image's bytes unsolicited while a MOUNT_IMAGE is outstanding).
To hold MAME and our core to the same network without a live FujiNet whose
answers change between runs, the conversation is recorded once from a real
private fujinet-pc and replayed to both:

  Recorder(listen_port, upstream_port)   a proxy: relays the connection and keeps
                                         every frame, in order, with its direction
  Replay(trace, port)                    a server: for each recorded frame in order,
                                         a client-to-server one is waited for and
                                         compared with the recorded bytes, a
                                         server-to-client one is sent

The emulators run the mailbox synchronously, so a transaction takes no emulated
time and the replay's speed does not matter; what matters is that both sides ask
the same questions in the same order, which the replay checks (`mismatches`).

A trace is a list of connections, each a list of [dir, hex] with dir "c" (the
console sent it) or "s" (FujiNet did). Reconnects are kept in order.
"""
import json
import socket
import threading

SLIP_END = 0xC0


class Framer:
    """Split a byte stream into SLIP frames (the bytes between 0xC0s, empty ones dropped)."""

    def __init__(self):
        self.buf = bytearray()

    def feed(self, data):
        out = []
        for b in data:
            if b == SLIP_END:
                if self.buf:
                    out.append(bytes(self.buf))
                    self.buf = bytearray()
            else:
                self.buf.append(b)
        return out


def wire(frame):
    return bytes([SLIP_END]) + frame + bytes([SLIP_END])


class Recorder:
    def __init__(self, listen_port, upstream_port, host="127.0.0.1"):
        self.listen_port, self.upstream_port, self.host = listen_port, upstream_port, host
        self.connections = []
        self.lock = threading.Lock()
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((host, listen_port))
        self.sock.listen(4)
        self.stop = False
        self.thread = threading.Thread(target=self._accept, daemon=True)
        self.thread.start()

    def _accept(self):
        self.sock.settimeout(0.2)
        while not self.stop:
            try:
                c, _ = self.sock.accept()
            except (socket.timeout, OSError):
                continue
            try:
                u = socket.create_connection((self.host, self.upstream_port), timeout=5)
            except OSError:
                c.close()
                continue
            frames = []
            with self.lock:
                self.connections.append(frames)
            for src, dst, tag in ((c, u, "c"), (u, c, "s")):
                threading.Thread(target=self._pump, args=(src, dst, tag, frames), daemon=True).start()

    def _pump(self, src, dst, tag, frames):
        fr = Framer()
        try:
            while True:
                d = src.recv(65536)
                if not d:
                    break
                for f in fr.feed(d):
                    with self.lock:
                        frames.append([tag, f.hex()])
                dst.sendall(d)
        except OSError:
            pass
        for s in (src, dst):
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass

    def save(self, path):
        with self.lock:
            json.dump(self.connections, open(path, "w"))

    def close(self):
        self.stop = True
        self.sock.close()


class Replay:
    def __init__(self, trace, port, host="127.0.0.1"):
        self.trace = trace
        self.mismatches = []        # (connection, index, recorded hex, got hex or None)
        self.extra = 0              # client frames past the end of the script
        self.served = 0             # connections served
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((host, port))
        self.sock.listen(4)
        self.stop = False
        self.thread = threading.Thread(target=self._accept, daemon=True)
        self.thread.start()

    def _accept(self):
        self.sock.settimeout(0.2)
        while not self.stop:
            try:
                c, _ = self.sock.accept()
            except (socket.timeout, OSError):
                continue
            n = self.served
            self.served += 1
            threading.Thread(target=self._serve, args=(c, n), daemon=True).start()

    def _serve(self, c, n):
        script = self.trace[min(n, len(self.trace) - 1)] if self.trace else []
        fr = Framer()
        pending = []                 # client frames read but not yet consumed
        c.settimeout(0.2)

        def next_client_frame():
            while not pending:
                if self.stop:
                    return None
                try:
                    d = c.recv(65536)
                except socket.timeout:
                    continue
                except OSError:
                    return None
                if not d:
                    return None
                pending.extend(fr.feed(d))
            return pending.pop(0)

        try:
            for i, (tag, hx) in enumerate(script):
                rec = bytes.fromhex(hx)
                if tag == "s":
                    c.sendall(wire(rec))
                else:
                    got = next_client_frame()
                    if got is None:
                        return
                    if got != rec:
                        self.mismatches.append((n, i, hx, got.hex()))
            # the script is done: absorb whatever else the client sends
            while True:
                got = next_client_frame()
                if got is None:
                    return
                self.extra += 1
        except OSError:
            pass
        finally:
            try:
                c.close()
            except OSError:
                pass

    def close(self):
        self.stop = True
        self.sock.close()


def load(path):
    return json.load(open(path))
