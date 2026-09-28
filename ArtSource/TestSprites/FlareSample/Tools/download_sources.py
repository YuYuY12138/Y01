"""获取两份指定的官方美术附件及授权证据；只写入本小样目录。"""
from datetime import datetime, timezone
from hashlib import sha256
from io import BytesIO
import json
from pathlib import Path
import re
import time
from urllib.request import Request, urlopen
from zipfile import ZipFile

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
SOURCES = {
    "Original/isometric_hero.zip": "https://opengameart.org/sites/default/files/isometric_hero.zip",
    "Original/Skeleton/skeleton_0.png": "https://opengameart.org/sites/default/files/skeleton_0.png",
    "Evidence/hero_page.html": "https://opengameart.org/content/isometric-hero-and-heroine",
    "Evidence/skeleton_page.html": "https://opengameart.org/content/skeleton-warrior-0",
    "Evidence/misha_page.html": "https://opengameart.org/content/bastardsword001-concept",
    "Evidence/cc-by-3.0.html": "https://creativecommons.org/licenses/by/3.0/",
    "Evidence/cc-by-3.0-legalcode.html": "https://creativecommons.org/licenses/by/3.0/legalcode",
    "Evidence/hero_v014.txt": "https://raw.githubusercontent.com/clintbellanger/flare-game/v0.14/animations/hero.txt",
    "Evidence/skeleton_v014.txt": "https://raw.githubusercontent.com/clintbellanger/flare-game/v0.14/animations/skeleton.txt",
}
LAYERS = ("steel_armor.png", "male_head1.png", "longsword.png", "shield.png")


def fetch(url):
    data = bytearray()
    total = None
    for chunk_index in range(1024):
        headers = {"User-Agent": "Y01-ArtSample/1.0", "Accept-Encoding": "identity"}
        if data:
            headers["Range"] = f"bytes={len(data)}-"
        for attempt in range(3):
            try:
                with urlopen(Request(url, headers=headers), timeout=20) as response:
                    print(f"HTTP {response.status}: {url}", flush=True)
                    chunks = []
                    received = 0
                    while True:
                        chunk = response.read(128 * 1024)
                        if not chunk:
                            break
                        chunks.append(chunk)
                        received += len(chunk)
                        if received > 32 * 1024 * 1024:
                            raise ValueError("Resource exceeds sample size limit")
                        print(f"  received {len(data) + received} bytes", flush=True)
                    part = b"".join(chunks)
                    content_range = response.headers.get("Content-Range")
                    if content_range:
                        match = re.fullmatch(r"bytes (\d+)-(\d+)/(\d+)", content_range)
                        if not match or int(match[1]) != len(data):
                            raise ValueError(f"Unexpected range: {content_range}")
                        total = int(match[3])
                        if len(part) != int(match[2]) - int(match[1]) + 1:
                            raise ValueError("Incomplete range")
                    elif data:
                        raise ValueError("Server ignored range")
                    else:
                        length = response.headers.get("Content-Length")
                        if length and len(part) != int(length):
                            raise ValueError("Incomplete response")
                    break
            except Exception as error:
                print(f"ATTEMPT {attempt + 1}: {type(error).__name__}: {error}", flush=True)
                if attempt == 2:
                    raise
                time.sleep(attempt + 1)
        data.extend(part)
        if len(data) > 32 * 1024 * 1024:
            raise ValueError("Resource exceeds sample size limit")
        if total is None or len(data) == total:
            return bytes(data)
    raise ValueError("Too many partial responses")


def validate(relative, data):
    if relative.endswith(".png"):
        with Image.open(BytesIO(data)) as image:
            image.load()
            assert image.size == (4096, 1024), image.size
    elif relative.endswith(".zip"):
        with ZipFile(BytesIO(data)) as archive:
            assert archive.testzip() is None
            for name in LAYERS:
                assert len([p for p in archive.namelist() if Path(p).name == name]) == 1
    elif relative.endswith("_page.html"):
        text = data.decode("utf-8")
        assert "creativecommons.org/licenses/by/3.0" in text or "creativecommons.org/licenses/by/3/" in text
        if "hero_page" in relative:
            assert "Distressed Leather" in text and "Naraphim" in text
            assert "All of these specific files are CC-BY licensed" in text
        if "skeleton_page" in relative:
            assert "Misha" in text
    elif relative.endswith("_v014.txt"):
        assert b"render_offset_y=96" in data and b"[die]" in data
    else:
        assert len(data) > 1000


def main():
    records = []
    for relative, url in SOURCES.items():
        print(f"FETCH {relative}", flush=True)
        path = ROOT / relative
        data = path.read_bytes() if path.exists() else fetch(url)
        validate(relative, data)
        if not path.exists():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        records.append({"file": relative, "url": url, "bytes": len(data), "sha256": sha256(data).hexdigest()})
        print(f"OK {relative}: {len(data)} bytes", flush=True)
    with ZipFile(ROOT / "Original/isometric_hero.zip") as archive:
        for name in LAYERS:
            member = next(p for p in archive.namelist() if Path(p).name == name)
            data = archive.read(member)
            with Image.open(BytesIO(data)) as image:
                image.load()
                assert image.size == (4096, 1024)
            path = ROOT / "Original/Hero" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            if path.exists():
                assert path.read_bytes() == data
            else:
                path.write_bytes(data)
            reference_url = f"https://raw.githubusercontent.com/clintbellanger/flare-game/v0.14/art/characters/hero/{name}"
            reference = fetch(reference_url)
            assert sha256(reference).digest() == sha256(data).digest(), name
            records.append({"file": path.relative_to(ROOT).as_posix(), "archive_member": member,
                            "sha256": sha256(data).hexdigest(), "bytes": len(data),
                            "reference_url": reference_url, "reference_bytes_equal": True})
            print(f"MATCH original and v0.14: {name}", flush=True)
    record = {"retrieved_utc": datetime.now(timezone.utc).isoformat(), "files": records,
              "status": "original_images_only_not_imported_not_ue_verified"}
    (ROOT / "sources.json").write_text(json.dumps(record, indent=2, ensure_ascii=False), encoding="utf-8")
    print("SOURCE ACQUISITION PASS", flush=True)


if __name__ == "__main__":
    main()
