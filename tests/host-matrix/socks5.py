#!/usr/bin/env python3
"""A SOCKS5 proxy fixture for the host matrix: CONNECT by name (socks5h),
answered by the proxy itself with `through-declared-proxy`. Every name it is
asked for is appended to <log>; it never connects anywhere.

    socks5.py <port-file> <log>      # binds 127.0.0.1:0, writes the port
"""
import socket
import struct
import sys
import threading

port_file, log_file = sys.argv[1], sys.argv[2]
lock = threading.Lock()


def serve(c):
    try:
        c.settimeout(20)
        ver, n = c.recv(2)
        c.recv(n)
        c.sendall(b"\x05\x00")
        ver, cmd, _, atyp = c.recv(4)
        if atyp == 3:
            host = c.recv(c.recv(1)[0]).decode()
        elif atyp == 1:
            host = socket.inet_ntoa(c.recv(4))
        else:
            host = socket.inet_ntop(socket.AF_INET6, c.recv(16))
        port = struct.unpack(">H", c.recv(2))[0]
        with lock, open(log_file, "a") as f:
            f.write(f"{host}:{port}\n")
        c.sendall(b"\x05\x00\x00\x01" + socket.inet_aton("127.0.0.1") + struct.pack(">H", 0))
        c.recv(4096)
        body = b"through-declared-proxy"
        c.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s" % (len(body), body))
    except Exception:
        pass
    finally:
        c.close()


s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", 0))
s.listen(16)
with open(port_file, "w") as f:
    f.write(str(s.getsockname()[1]))
while True:
    conn, _ = s.accept()
    threading.Thread(target=serve, args=(conn,), daemon=True).start()
