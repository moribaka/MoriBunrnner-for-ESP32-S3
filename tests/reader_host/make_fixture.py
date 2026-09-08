"""Create stored/deflated EPUB fixtures for test_epub (output directory argument)."""
from pathlib import Path
import sys
import zipfile

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
