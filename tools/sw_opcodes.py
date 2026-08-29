"""Recover SoulWorker's packet opcode tables from SoulWorker64.dll (static, no game needed).

    pip install capstone
    python tools/sw_opcodes.py "D:/SteamLibrary/steamapps/common/Soulworker_GB/SoulWorker64.dll" docs/packets

Writes recv_opcodes.txt (server -> client: opcode, handler VA, handler name) and
send_opcodes.txt (client -> server: opcode, sender VA, sender name).

How it works
  * Every receive handler logs its own name ("receive_eSUB_CMD_*") on entry, so a
    rip-relative LEA to that string identifies the handler function.
  * All handlers are registered from one giant function as
        lea rax,[handler] ; mov [rsp+X],rax ; mov [rsp+X+8],0 ; mov [rsp+X+16], KEY
    where KEY = (mainCmd << 8) | subCmd. The function is found through the
    WORLD_ENTER_RES handler's registration and bounded with .pdata.
  * Every sender builds its packet with XPacket::XPacket(this, mainCmd(dl), subCmd(r8b), 0)
    and logs "send_eSUB_CMD_*" afterwards; the ctor is found from the
    send_eSUB_CMD_EXCHANGE_SEARCH sender and all its call sites are walked.
"""
import bisect
import os
import re
import struct
import sys

from capstone import CS_ARCH_X86, CS_MODE_64, Cs


class Image:
    def __init__(self, path):
        self.data = d = open(path, "rb").read()
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        optsz = struct.unpack_from("<H", d, pe + 20)[0]
        self.base = struct.unpack_from("<Q", d, pe + 24 + 24)[0]
        self.secs = {}
        off = pe + 24 + optsz
        for _ in range(nsec):
            name = d[off:off + 8].rstrip(b"\0").decode()
            vsz, va, rsz, raw = struct.unpack_from("<IIII", d, off + 8)
            self.secs[name] = (va, vsz, raw, rsz)
            off += 40
        t = self.secs[".text"]
        self.tbeg, self.tend = t[2], t[2] + t[3]
        pd = self.secs[".pdata"]
        self.funcs = sorted(struct.unpack_from("<III", d, o)[:2]
                            for o in range(pd[2], pd[2] + pd[3] - 12, 12))
        self.fstarts = [b for b, _ in self.funcs]
        self.md = Cs(CS_ARCH_X86, CS_MODE_64)

    def raw2va(self, o):
        for va, vsz, raw, rsz in self.secs.values():
            if raw <= o < raw + rsz:
                return self.base + va + (o - raw)

    def va2raw(self, v):
        for va, vsz, raw, rsz in self.secs.values():
            if self.base + va <= v < self.base + va + rsz:
                return raw + (v - self.base - va)

    def func_of(self, va):
        r = va - self.base
        k = bisect.bisect_right(self.fstarts, r) - 1
        if k >= 0 and self.funcs[k][0] <= r < self.funcs[k][1]:
            return self.base + self.funcs[k][0], self.base + self.funcs[k][1]

    def lea_refs(self):
        """Yield (site_va, target_va) for every rip-relative LEA in .text."""
        d = self.data
        i = self.tbeg
        while i < self.tend - 7:
            if d[i] in (0x48, 0x4C) and d[i + 1] == 0x8D and (d[i + 2] & 0xC7) == 0x05:
                disp = struct.unpack_from("<i", d, i + 3)[0]
                yield self.raw2va(i), self.raw2va(i + 7) + disp
            i += 1

    def calls_to(self, target):
        d = self.data
        i = self.tbeg
        while i < self.tend - 5:
            if d[i] == 0xE8:
                rel = struct.unpack_from("<i", d, i + 1)[0]
                if self.raw2va(i + 5) + rel == target:
                    yield self.raw2va(i)
            i += 1

    def cstring(self, va, prefix):
        r = self.va2raw(va)
        if r is None or self.data[r:r + len(prefix)] != prefix:
            return None
        e = self.data.find(b"\0", r)
        return self.data[r:e].decode(errors="replace")

    def disasm(self, lo, hi):
        raw = self.va2raw(lo)
        return list(self.md.disasm(self.data[raw:raw + (hi - lo)], lo))


def recv_table(img):
    fn2name, rd = {}, img.secs[".rdata"]
    rlo, rhi = img.base + rd[0], img.base + rd[0] + rd[3]
    for site, tgt in img.lea_refs():
        if rlo <= tgt < rhi:
            nm = img.cstring(tgt, b"receive_eSUB_CMD_")
            if nm:
                f = img.func_of(site)
                if f:
                    fn2name.setdefault(f[0], nm)
    anchor = next(f for f, n in fn2name.items() if n == "receive_eSUB_CMD_WORLD_ENTER_RES")
    reg = None
    for site, tgt in img.lea_refs():
        if tgt == anchor:
            f = img.func_of(site)
            if f and f[1] - f[0] > 0x10000:
                reg = f
                break
    if not reg:
        raise SystemExit("registration function not found")
    ins = img.disasm(*reg)
    rows = []
    for k, x in enumerate(ins):
        if x.mnemonic != "lea":
            continue
        m = re.match(r"rax, \[rip \+ (0x[0-9a-f]+)\]", x.op_str)
        if not m:
            continue
        h = x.address + x.size + int(m.group(1), 16)
        key = None
        for y in ins[k + 1:k + 5]:
            if y.mnemonic != "mov":
                continue
            m2 = re.match(r"dword ptr \[rsp \+ 0x[0-9a-f]+\], (0x[0-9a-f]+|\d+)$", y.op_str)
            if m2 and int(m2.group(1), 0):
                key = int(m2.group(1), 0)
                break
        if key is not None:
            rows.append((key, h, fn2name.get(h, "?")))
    rows.sort()
    return rows


def find_xpacket_ctor(img):
    """The XPacket constructor is the first call after `mov dl, cat` in the EXCHANGE_SEARCH sender."""
    site = next(s for s, t in img.lea_refs() if img.cstring(t, b"send_eSUB_CMD_EXCHANGE_SEARCH"))
    seen_cat = False
    for x in img.disasm(*img.func_of(site)):
        if x.mnemonic == "mov" and x.op_str.startswith("dl, "):
            seen_cat = True
        elif seen_cat and x.mnemonic == "call" and x.op_str.startswith("0x"):
            return int(x.op_str, 16)
    raise SystemExit("XPacket constructor not found")


def send_table(img):
    ctor = find_xpacket_ctor(img)
    rows = []
    for c in img.calls_to(ctor):
        f = img.func_of(c)
        if not f:
            continue
        main = sub = None
        names = set()
        for x in img.disasm(*f):
            if x.address < c and x.mnemonic == "mov":
                if x.op_str.startswith("dl, "):
                    main = int(x.op_str[4:], 0)
                elif x.op_str.startswith("r8b, "):
                    sub = int(x.op_str[5:], 0)
            if x.mnemonic == "lea" and "rip +" in x.op_str:
                m = re.search(r"rip \+ (0x[0-9a-f]+)", x.op_str)
                nm = img.cstring(x.address + x.size + int(m.group(1), 16), b"send_")
                if nm:
                    names.add(nm.split(" - ")[0])
        if main is not None and sub is not None:
            rows.append(((main << 8) | sub, f[0], " | ".join(sorted(names)) or "?"))
    rows.sort()
    return rows


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    img = Image(sys.argv[1])
    out = sys.argv[2]
    os.makedirs(out, exist_ok=True)
    rv = recv_table(img)
    with open(os.path.join(out, "recv_opcodes.txt"), "w") as fo:
        fo.write("# server -> client. opcode  handler_va  handler_name (from the handler's own log string)\n")
        for key, h, nm in rv:
            fo.write("0x%04x  %x  %s\n" % (key, h, nm))
    sd = send_table(img)
    with open(os.path.join(out, "send_opcodes.txt"), "w") as fo:
        fo.write("# client -> server. opcode  sender_va  sender_name (from the sender's own log string)\n")
        for key, h, nm in sd:
            fo.write("0x%04x  %x  %s\n" % (key, h, nm))
    print("recv: %d opcodes (%d named)  send: %d opcodes"
          % (len(rv), sum(1 for r in rv if r[2] != "?"), len(sd)))


if __name__ == "__main__":
    main()
