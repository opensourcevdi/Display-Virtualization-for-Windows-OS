#!/usr/bin/env python3
"""Create an unsigned driver package catalog (.cat), like inf2cat does.

inf2cat does not run under Wine, so the Linux build uses this instead. The
layout matches what inf2cat 10.0.28000 writes: a version 2 catalog (CTL)
where every file is listed by its SHA-256 hash, with Authenticode indirect
data, and by its SHA-1 hash. The output is a PKCS#7 SignedData without
signers; sign it afterwards with osslsigncode.

Usage: makecat.py --inf pkg/foo.inf --out pkg/foo.cat [--os _v100_X64] FILE...
"""

import argparse
import datetime
import hashlib
import os
import re
import struct
import sys
import uuid

# --- Minimal DER encoder -----------------------------------------------------

def der(tag, content):
    n = len(content)
    if n < 0x80:
        length = bytes([n])
    else:
        b = n.to_bytes((n.bit_length() + 7) // 8, "big")
        length = bytes([0x80 | len(b)]) + b
    return bytes([tag]) + length + content

def seq(*items):
    return der(0x30, b"".join(items))

def set_of(*items):
    # DER orders SET OF elements by their encodings.
    return der(0x31, b"".join(sorted(items)))

def ctx(n, content, constructed=True):
    return der((0xA0 if constructed else 0x80) | n, content)

def octets(b):
    return der(0x04, b)

def null():
    return der(0x05, b"")

def integer(v):
    return der(0x02, v.to_bytes((v.bit_length() + 8) // 8, "big"))

def bmpstring(s):
    return der(0x1E, s.encode("utf-16-be"))

def utctime(t):
    return der(0x17, t.strftime("%y%m%d%H%M%SZ").encode())

def oid(dotted):
    parts = [int(p) for p in dotted.split(".")]
    out = bytearray([parts[0] * 40 + parts[1]])
    for p in parts[2:]:
        chunk = [p & 0x7F]
        p >>= 7
        while p:
            chunk.append(0x80 | (p & 0x7F))
            p >>= 7
        out += bytes(reversed(chunk))
    return der(0x06, bytes(out))

OID_SIGNED_DATA = "1.2.840.113549.1.7.2"
OID_SHA1 = "1.3.14.3.2.26"
OID_SHA256 = "2.16.840.1.101.3.4.2.1"
OID_CTL = "1.3.6.1.4.1.311.10.1"
OID_CATALOG_LIST = "1.3.6.1.4.1.311.12.1.1"
OID_CATALOG_LIST_MEMBER2 = "1.3.6.1.4.1.311.12.1.3"
OID_CAT_NAMEVALUE = "1.3.6.1.4.1.311.12.2.1"
OID_CAT_MEMBERINFO2 = "1.3.6.1.4.1.311.12.2.3"
OID_SPC_INDIRECT_DATA = "1.3.6.1.4.1.311.2.1.4"
OID_SPC_PE_IMAGE_DATA = "1.3.6.1.4.1.311.2.1.15"
OID_SPC_CAB_DATA = "1.3.6.1.4.1.311.2.1.25"

# CRYPTCAT_ATTR_NAMEASCII | CRYPTCAT_ATTR_DATAASCII | CRYPTCAT_ATTR_AUTHENTICATED
NAMEVALUE_FLAGS = 0x10010001

def name_value(name, value):
    return seq(bmpstring(name), integer(NAMEVALUE_FLAGS),
               octets((value + "\0").encode("utf-16-le")))

def attribute(attr_oid, value):
    return seq(oid(attr_oid), set_of(value))

# --- Hashing -------------------------------------------------------------------

def pe_layout(data):
    """Return (checksum offset, cert dir entry offset, cert table offset/size)
    for a PE file, or None if data is not a PE image."""
    if len(data) < 0x40 or data[:2] != b"MZ":
        return None
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        return None
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    datadir = opt + (112 if magic == 0x20B else 96)
    certdir = datadir + 4 * 8
    cert_off, cert_size = struct.unpack_from("<II", data, certdir)
    return opt + 64, certdir, cert_off, cert_size

def file_hash(data, algo):
    """Authenticode image hash for PE files, plain hash otherwise."""
    h = hashlib.new(algo)
    layout = pe_layout(data)
    if layout is None:
        h.update(data)
        return h.digest()
    checksum, certdir, cert_off, cert_size = layout
    end = cert_off if cert_size else len(data)
    h.update(data[:checksum])
    h.update(data[checksum + 4:certdir])
    h.update(data[certdir + 8:end])
    if cert_size:
        h.update(data[cert_off + cert_size:])
    return h.digest()

# --- Catalog -------------------------------------------------------------------

def indirect_data(is_pe, digest):
    if is_pe:
        # SpcPeImageData { flags = includeResources/DebugInfo/ImportAddressTable,
        #                  file = SpcLink.file(SpcString.unicode("")) }
        spc = seq(oid(OID_SPC_PE_IMAGE_DATA),
                  seq(der(0x03, b"\x05\xa0"), ctx(0, ctx(2, ctx(0, b"", False)))))
    else:
        spc = seq(oid(OID_SPC_CAB_DATA), ctx(2, ctx(0, b"", False)))
    return seq(spc, seq(seq(oid(OID_SHA256), null()), octets(digest)))

def members(path, osattr):
    data = open(path, "rb").read()
    name = os.path.basename(path).lower()
    is_pe = pe_layout(data) is not None
    memberinfo = attribute(OID_CAT_MEMBERINFO2, ctx(0 if is_pe else 2, b"", False))
    common = [memberinfo,
              attribute(OID_CAT_NAMEVALUE, name_value("OSAttr", osattr)),
              attribute(OID_CAT_NAMEVALUE, name_value("File", name))]
    sha256 = file_hash(data, "sha256")
    sha1 = file_hash(data, "sha1")
    yield sha256, seq(octets(sha256), set_of(
        *common, attribute(OID_SPC_INDIRECT_DATA, indirect_data(is_pe, sha256))))
    yield sha1, seq(octets(sha1), set_of(*common))

def hardware_ids(inf_path):
    """Hardware IDs of all models listed under [Manufacturer], lowercased, in
    INF order."""
    text = open(inf_path, "rb").read()
    text = text.decode("utf-16") if text[:2] in (b"\xff\xfe", b"\xfe\xff") else text.decode("utf-8-sig", "replace")
    sections = {}
    current = None
    for line in text.splitlines():
        line = line.split(";", 1)[0].strip()
        m = re.fullmatch(r"\[(.+)\]", line)
        if m:
            current = sections.setdefault(m.group(1).strip().lower(), [])
        elif line and current is not None:
            current.append(line)
    ids = []
    for line in sections.get("manufacturer", []):
        _, _, value = line.partition("=")
        parts = [p.strip() for p in value.split(",")]
        base, decorations = parts[0], parts[1:]
        for model in [base] + [base + "." + d for d in decorations]:
            for entry in sections.get(model.lower(), []):
                _, _, rhs = entry.partition("=")
                for hwid in [p.strip().lower() for p in rhs.split(",")[1:]]:
                    if hwid and hwid not in ids:
                        ids.append(hwid)
    return ids

def make_catalog(files, inf, os_name, osattr, list_id=None, when=None):
    when = when or datetime.datetime.now(datetime.timezone.utc)
    list_id = list_id or uuid.uuid4().bytes
    subjects = sorted(m for f in files for m in members(f, osattr))
    cat_attrs = [("OS", os_name)]
    cat_attrs += [("HWID%d" % (i + 1), h) for i, h in enumerate(hardware_ids(inf))]
    extensions = seq(*[seq(oid(OID_CAT_NAMEVALUE), octets(name_value(n, v)))
                       for n, v in cat_attrs])
    ctl = seq(seq(oid(OID_CATALOG_LIST)),
              octets(list_id),
              utctime(when),
              seq(oid(OID_CATALOG_LIST_MEMBER2), null()),
              seq(*[s for _, s in subjects]),
              ctx(0, extensions))
    signed = seq(integer(1),
                 set_of(seq(oid(OID_SHA256), null())),
                 seq(oid(OID_CTL), ctx(0, ctl)),
                 set_of())
    return seq(oid(OID_SIGNED_DATA), ctx(0, signed))

def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--inf", required=True, help="INF that provides the hardware IDs")
    ap.add_argument("--out", required=True)
    ap.add_argument("--os", default="_v100_X64", help="catalog OS attribute (inf2cat /os:10_X64)")
    ap.add_argument("--osattr", default="2:10.0", help="per-file OSAttr value")
    ap.add_argument("files", nargs="+", help="files to list, including the INF")
    args = ap.parse_args()
    with open(args.out, "wb") as f:
        f.write(make_catalog(args.files, args.inf, args.os, args.osattr))

if __name__ == "__main__":
    sys.exit(main())
