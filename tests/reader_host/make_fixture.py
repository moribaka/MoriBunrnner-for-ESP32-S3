"""Create stored/deflated EPUB fixtures for test_epub (output directory argument)."""
from pathlib import Path
import sys
import zipfile
import struct

root = Path(sys.argv[1])
root.mkdir(parents=True, exist_ok=True)
for mode in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
    with zipfile.ZipFile(root / f"epub-perf-{mode}.epub", "w", compression=mode) as archive:
        archive.writestr("META-INF/container.xml", '<container><rootfiles><rootfile full-path="OEBPS/book.opf" media-type="application/oebps-package+xml"/></rootfiles></container>')
        manifest = "".join(f'<item id="c{i}" href="c{i}.xhtml" media-type="application/xhtml+xml"/>' for i in range(32))
        spine = "".join(f'<itemref idref="c{i}"/>' for i in range(32))
        archive.writestr("OEBPS/book.opf", f"<package><metadata><title>Performance fixture</title></metadata><manifest>{manifest}</manifest><spine>{spine}</spine></package>")
        for i in range(32):
            archive.writestr(f"OEBPS/c{i}.xhtml", f"<html><body><h1>Chapter {i}</h1>" + "<p>Line <b>bold</b> &amp; text.</p>" * 200 + "</body></html>")
        for i in range(300):
            archive.writestr(f"images/unused-{i}.dat", b"reader test asset")

with zipfile.ZipFile(root / "epub-perf-8.epub") as source:
    with zipfile.ZipFile(root / "epub-empty.epub", "w", compression=zipfile.ZIP_DEFLATED) as target:
        for name in source.namelist():
            target.writestr(name, b"" if name == "OEBPS/c0.xhtml" else source.read(name))
data = bytearray((root / "epub-perf-8.epub").read_bytes())
offset = data.find(b"PK\x01\x02")
while data[offset:offset + 4] == b"PK\x01\x02":
    name_len, extra_len, comment_len = struct.unpack_from("<HHH", data, offset + 28)
    if data[offset + 46:offset + 46 + name_len] == b"OEBPS/c1.xhtml":
        crc, = struct.unpack_from("<I", data, offset + 16)
        struct.pack_into("<I", data, offset + 16, crc ^ 1)
        break
    offset += 46 + name_len + extra_len + comment_len
else:
    raise AssertionError("chapter central entry missing")
(root / "epub-bad-crc.epub").write_bytes(data)
