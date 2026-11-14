"""Leser fuer Nintendos Archivformate: Yaz0 (.szs) und SARC (.sarc/.pack)."""
import struct


def yaz0_decompress(data: bytes) -> bytes:
    if data[:4] != b"Yaz0":
        return data
    out_size = struct.unpack_from(">I", data, 4)[0]
    out = bytearray(out_size)
    src, dst = 16, 0
    while dst < out_size:
        code = data[src]
        src += 1
        for bit in range(7, -1, -1):
            if dst >= out_size:
                break
            if code & (1 << bit):
                out[dst] = data[src]
                src += 1
                dst += 1
            else:
                b1, b2 = data[src], data[src + 1]
                src += 2
                dist = ((b1 & 0x0F) << 8 | b2) + 1
                n = b1 >> 4
                if n == 0:
                    n = data[src] + 0x12
                    src += 1
                else:
                    n += 2
                start = dst - dist
                for i in range(n):
                    out[dst] = out[start + i]
                    dst += 1
    return bytes(out)


def is_sarc(data: bytes) -> bool:
    return data[:4] == b"SARC"


def sarc_files(data: bytes):
    """Gibt {name: bytes} fuer ein SARC-Archiv zurueck."""
    if not is_sarc(data):
        raise ValueError("Kein SARC")
    bom = data[6:8]
    e = ">" if bom == b"\xfe\xff" else "<"
    hdr_len, _, _, data_off = struct.unpack_from(e + "HHII", data, 4)
    sfat = hdr_len
    assert data[sfat:sfat + 4] == b"SFAT"
    _, count, _ = struct.unpack_from(e + "HHI", data, sfat + 4)
    nodes = []
    for i in range(count):
        h, attr, start, end = struct.unpack_from(e + "IIII", data, sfat + 12 + i * 16)
        nodes.append((attr, start, end))
    sfnt = sfat + 12 + count * 16
    assert data[sfnt:sfnt + 4] == b"SFNT"
    names_off = sfnt + 8
    files = {}
    for idx, (attr, start, end) in enumerate(nodes):
        if attr & 0x01000000:
            off = names_off + (attr & 0xFFFF) * 4
            name = data[off:data.index(b"\0", off)].decode("utf-8", "replace")
        else:
            name = f"_unnamed_{idx:04d}"
        files[name] = data[data_off + start:data_off + end]
    return files


def walk(data: bytes, prefix=""):
    """Geht rekursiv durch verschachtelte Yaz0/SARC-Archive. Liefert (pfad, bytes)."""
    data = yaz0_decompress(data)
    if is_sarc(data):
        for name, sub in sarc_files(data).items():
            yield from walk(sub, f"{prefix}/{name}" if prefix else name)
    else:
        yield prefix, data
