from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess
import unicodedata
from urllib.parse import urlsplit


WIDTH, HEIGHT, LOGO_SIZE = 384, 216, 72
SIZE_LIMIT = 1536 * 1024 * 1024
CATEGORIES = {
    "emulation": "STORE_EMULATION",
    "ports": "STORE_PORTS",
    "media": "STORE_MEDIA",
    "utilities": "STORE_UTILITIES",
    "development": "STORE_DEVELOPMENT",
}
TEXT_REPLACEMENTS = str.maketrans({"’": "'", "‘": "'", "“": '"', "”": '"', "–": "-", "—": "-",
                                   "…": "...", "×": "x", "·": "|", "™": ""})
SUPPORTED_CHARS = set("ÁÉÍÓÚÜÑáéíóúüñ¿¡")


def clean(value: str | None, limit: int = 420) -> str:
    if not isinstance(value, str):
        return ""
    value = " ".join(unicodedata.normalize("NFC", value).translate(TEXT_REPLACEMENTS).split())
    result = []
    for char in value:
        if 32 <= ord(char) < 127 or char in SUPPORTED_CHARS:
            result.append(char)
        elif not unicodedata.category(char).startswith("C"):
            result.append(unicodedata.normalize("NFKD", char).encode("ascii", "ignore").decode() or "?")
    return "".join(result)[:limit]


def c_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=False)


def clean_relative(path: str) -> bool:
    if not isinstance(path, str) or not path or len(path.encode()) > 224:
        return False
    return all(part not in (".", "..") and len(part) <= 100 and
               re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_. ()+-]*", part) and
               part[-1] not in " ." for part in path.split("/"))


def package_literal(package: dict | None, identifier: str) -> str:
    if not package:
        return "{ .kind = STORE_PACKAGE_NONE }"
    kind = package.get("kind")
    url = package.get("url", "")
    address = urlsplit(url)
    valid_host = address.hostname in {"github.com", "gitlab.com", "codeberg.org"}
    if (kind not in {"nro", "zip"} or address.scheme != "https" or not valid_host
            or address.netloc != address.hostname or address.fragment or not address.path
            or any(ord(char) <= 32 or ord(char) > 126 for char in url)):
        raise ValueError(f"{identifier}: release URL or package format is invalid")
    hash_text = package.get("sha256", "").lower()
    size = package.get("size") or 0
    if not re.fullmatch(r"[a-f0-9]{64}", hash_text) or type(size) is not int or not 0 <= size <= SIZE_LIMIT:
        raise ValueError(f"{identifier}: release integrity metadata is invalid")
    version = clean(package.get("version"), 64)
    target = package.get("target", "")
    root = package.get("archive_root") or ""
    secondary_root = package.get("archive_secondary_root") or ""
    marker = package.get("marker") or ""
    if not version or not target.startswith("sdmc:/") or not clean_relative(target[6:]):
        raise ValueError(f"{identifier}: target or version is invalid")
    if kind == "nro":
        if not target.startswith("sdmc:/switch/") or not target.lower().endswith(".nro"):
            raise ValueError(f"{identifier}: NRO target is outside switch or has the wrong extension")
    elif (not clean_relative(marker) or not marker.lower().endswith(".nro")
          or any(prefix and not clean_relative(prefix) for prefix in (root, secondary_root))):
        raise ValueError(f"{identifier}: ZIP layout is missing a verified NRO marker")
    digest = ", ".join(f"0x{byte:02x}" for byte in bytes.fromhex(hash_text))
    return ("{ .kind = STORE_PACKAGE_" + kind.upper() + ", .version = " + c_string(version)
            + ", .url = " + c_string(url) + ", .target = " + c_string(target)
            + ", .archive_root = " + c_string(root)
            + ", .archive_secondary_root = " + c_string(secondary_root)
            + ", .marker = " + c_string(marker) + f", .size = UINT64_C({size}), .sha256 = {{ {digest} }} }}")


def artwork_records(manifests: list[Path]) -> dict:
    records = {}
    for manifest in manifests:
        directory = manifest.resolve().parent
        for identifier, record in json.loads(manifest.read_text(encoding="utf-8-sig")).items():
            path = (directory / record["file"]).resolve()
            if identifier in records or not path.is_relative_to(directory) or not path.is_file():
                raise ValueError(f"{identifier}: duplicate or missing original project image")
            if urlsplit(record["source"]).scheme != "https" or not record["credit"]:
                raise ValueError(f"{identifier}: missing artwork attribution")
            credit = re.split(r" ?[·|] ?", record["credit"], maxsplit=1)[0]
            records[identifier] = (path, clean(credit, 130))
    return records


def pixels(ffmpeg: Path, source: Path, filters: list[str], width: int, height: int) -> bytes:
    result = subprocess.run([
        str(ffmpeg), "-nostdin", "-v", "error", "-i", str(source), *filters,
        "-frames:v", "1", "-threads", "1", "-f", "rawvideo", "-pix_fmt", "rgb24", "pipe:1",
    ], capture_output=True, timeout=25)
    if result.returncode or len(result.stdout) != width * height * 3:
        raise ValueError(f"Could not resize {source}: {result.stderr.decode(errors='replace').strip()}")
    return result.stdout


def resize_cover(ffmpeg: Path, source: Path) -> bytes:
    # Preserve the complete original. Extend its edge colors into any side bars.
    filter_graph = (
        "[0:v]split=2[fill][fit];"
        f"[fill]scale={WIDTH}:{HEIGHT}:force_original_aspect_ratio=increase:flags=lanczos,"
        f"crop={WIDTH}:{HEIGHT},format=yuv444p,gblur=sigma=18,eq=brightness=-0.12:saturation=0.8[matte];"
        f"[fit]format=rgba,scale={WIDTH}:{HEIGHT}:force_original_aspect_ratio=decrease:flags=lanczos[art];"
        "[matte][art]overlay=(W-w)/2:(H-h)/2:format=yuv444:shortest=1,setsar=1"
    )
    return pixels(ffmpeg, source, ["-filter_complex_threads", "1", "-filter_complex", filter_graph], WIDTH, HEIGHT)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--reviewed", type=Path, required=True)
    parser.add_argument("--english", type=Path, required=True)
    parser.add_argument("--sources", type=Path, required=True)
    parser.add_argument("--artwork", type=Path, nargs="+", required=True)
    parser.add_argument("--logo", type=Path, required=True)
    parser.add_argument("--ffmpeg", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    catalog = json.loads(args.reviewed.read_text(encoding="utf-8-sig"))
    features = {identifier: i for i, identifier in enumerate(
        ("kaiplayer", "retroarch", "kaivideo", "kairadio", "kaikart", "switchnow", "sphaira", "scummvm-s2"))}
    entries = sorted(catalog["entries"], key=lambda item: (
        features.get(item["id"], len(features)), not bool(item.get("package"))))
    revision = catalog.get("source_revision", 0)
    if type(revision) is not int or not 0 <= revision < 2**32 or not entries:
        raise ValueError("The catalogue snapshot is empty or invalid")
    images = artwork_records(args.artwork)
    translations = json.loads(args.english.read_text(encoding="utf-8-sig"))
    source_urls = json.loads(args.sources.read_text(encoding="utf-8-sig"))
    expected = {entry["id"] for entry in entries}
    if set(images) - expected or set(translations) != expected:
        raise ValueError(f"Artwork or translation coverage failure: "
                         f"artwork {sorted(set(images) ^ expected)}, English {sorted(set(translations) ^ expected)}")
    if set(source_urls) != expected:
        raise ValueError(f"Missing or extra official links: {sorted(set(source_urls) ^ expected)}")
    args.output.mkdir(parents=True, exist_ok=True)
    identifiers: set[str] = set()
    count_installable = 0
    records = ['#include "catalog.h"', "", "const StoreCatalogEntry store_catalog[] = {"]
    covers = bytearray()
    for entry in entries:
        identifier, category = entry["id"], entry["category"]
        if identifier in identifiers or not re.fullmatch(r"[a-z0-9][a-z0-9-]{0,119}", identifier) or category not in CATEGORIES:
            raise ValueError(f"Unknown or duplicate catalogue entry: {identifier}")
        identifiers.add(identifier)
        credit = ""
        if identifier in images:
            source, credit = images[identifier]
            covers.extend(resize_cover(args.ffmpeg, source))
        else:
            covers.extend(bytes(WIDTH * HEIGHT * 3))
        package = package_literal(entry.get("package"), identifier)
        count_installable += bool(entry.get("package"))
        title = clean(entry["name"], 150)
        summary, requirement = translations[identifier]
        summary_es, rules_es = clean(entry.get("summary")), clean(entry.get("requirement"), 580)
        if not summary or bool(requirement) != bool(rules_es):
            raise ValueError(f"{identifier}: English project copy is incomplete")
        records.append(f"    {{ {c_string(identifier)}, {c_string(title)}, {CATEGORIES[category]},")
        web_id = entry.get("cover_id", identifier)
        if not re.fullmatch(r"[a-z0-9][a-z0-9-]{0,119}", web_id):
            raise ValueError(f"{identifier}: invalid project page")
        project_url = c_string(f"https://switch2homebrew.com/projects/{web_id}")
        official_url = source_urls[identifier]
        address = urlsplit(official_url)
        if (not 1 <= len(official_url) <= 300 or
                any(not 33 <= ord(char) < 127 for char in official_url) or
                address.scheme != "https" or not address.hostname or
                address.netloc != address.hostname or "\\" in official_url or
                official_url == json.loads(project_url)):
            raise ValueError(f"{identifier}: invalid official project link")
        records.append(f"      {c_string(clean(summary))}, {c_string(clean(requirement, 580))}, "
                       f"{c_string(summary_es)}, {c_string(rules_es)}, {c_string(credit)}, {package}, "
                       f".project_url = {project_url}, .source_url = {c_string(official_url)} }},")
    records.extend([
        "};", "",
        "const size_t store_catalog_count = sizeof store_catalog / sizeof store_catalog[0];",
        f"const size_t store_installable_count = {count_installable};",
        f"const unsigned store_catalog_revision = {revision};", "",
    ])
    (args.output / "catalog-data.c").write_text("\n".join(records), encoding="utf-8")
    (args.output / "store-covers.rgb").write_bytes(covers)
    logo = pixels(args.ffmpeg, args.logo, ["-vf", f"scale={LOGO_SIZE}:{LOGO_SIZE}:flags=lanczos"], LOGO_SIZE, LOGO_SIZE)
    (args.output / "store-logo.rgb").write_bytes(logo)
    print(f"Catalogue revision {revision}: {len(entries)} entries, {count_installable} packages, "
          f"{len(images)} credited original images, English and Spanish.")


if __name__ == "__main__":
    main()
