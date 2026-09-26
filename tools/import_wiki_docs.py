#!/usr/bin/env python3
"""Convert the GitHub wiki imported into docs/ into Starlight-ready Markdown.

Design: specs/2026-09-26-picodeck-rename-design.md (section 4.2).  Run once,
right after `git subtree add --prefix=docs <wiki> master`:

    python3 tools/import_wiki_docs.py            # converts docs/
    python3 tools/import_wiki_docs.py DOCS_DIR

For each page: adds `title:` frontmatter from the first `# ` heading (and drops
that heading; Starlight renders the title) and rewrites [[Page]] and
[[Text|Page]] wiki links to relative `.md` links.  Home.md becomes index.md.
_Sidebar.md becomes _sidebar.json ([{label, items: [{label, page}]}], read by
the website); it and _Footer.md are deleted.  Wiki links inside code are left
alone (Lua long strings look like [[this]]).  Exits 1 and writes nothing if a
wiki link does not resolve; refuses to convert a page twice.
"""

import json
import re
import sys
from pathlib import Path

WIKILINK = re.compile(r"\[\[([^\]|]+)(?:\|([^\]]+))?\]\]")
FENCE = re.compile(r"^\s*(```|~~~)")
INLINE_CODE = re.compile(r"(`+[^`]*`+)")
# Wiki links to pages that never existed or were renamed: page key -> file.
ALIASES = {"native-app-development": "Native-Loading.md"}


def page_key(name: str) -> str:
    """GitHub wiki page matching: spaces, hyphens and underscores are equivalent; case is ignored."""
    return re.sub(r"[\s_-]+", "-", name.strip()).lower()


def build_index(docs: Path) -> dict[str, str]:
    """Page key -> output file name for every page in docs/ (Home.md -> index.md)."""
    index = {}
    for page in docs.glob("*.md"):
        if page.name.startswith("_"):
            continue
        index[page_key(page.stem)] = "index.md" if page.stem.lower() == "home" else page.name
    for key, target in ALIASES.items():
        index.setdefault(key, target)
    return index


def _parts(m: re.Match) -> tuple[str, str]:
    """(link text, page name) for [[Page]] or [[Text|Page]]."""
    return (m.group(1), m.group(2)) if m.group(2) else (m.group(1), m.group(1))


def convert_links(text: str, index: dict[str, str]) -> tuple[str, list[str]]:
    unresolved: list[str] = []

    def link(m: re.Match) -> str:
        label, page = _parts(m)
        target = index.get(page_key(page))
        if target is None:
            unresolved.append(page.strip())
            return m.group(0)
        return f"[{label.strip()}]({target})"

    out, in_fence = [], False
    for line in text.splitlines(keepends=True):
        if FENCE.match(line):
            in_fence = not in_fence
            out.append(line)
        elif in_fence:
            out.append(line)
        else:
            pieces = INLINE_CODE.split(line)  # odd indices are code spans
            out.append("".join(p if i % 2 else WIKILINK.sub(link, p) for i, p in enumerate(pieces)))
    return "".join(out), unresolved


def split_title(text: str, stem: str) -> tuple[str, str]:
    """(title, body): the first non-blank line if it is a `# ` heading, else the file name."""
    lines = text.splitlines(keepends=True)
    for i, line in enumerate(lines):
        if not line.strip():
            continue
        if line.startswith("# "):
            body = lines[i + 1:]
            while body and not body[0].strip():
                body.pop(0)
            return line[2:].strip(), "".join(body)
        break
    return stem.replace("-", " "), text


def sidebar(text: str, index: dict[str, str]) -> tuple[list[dict], list[str]]:
    groups: list[dict] = []
    unresolved: list[str] = []
    for line in text.splitlines():
        heading = re.match(r"#{1,6}\s+(.+)", line)
        if heading:
            groups.append({"label": heading.group(1).strip(), "items": []})
            continue
        for m in WIKILINK.finditer(line):
            label, page = _parts(m)
            target = index.get(page_key(page))
            if target is None:
                unresolved.append(page.strip())
                continue
            if not groups:
                groups.append({"label": "Docs", "items": []})
            groups[-1]["items"].append({"label": label.strip(), "page": target})
    return groups, unresolved


def main(argv: list[str] | None = None) -> int:
    args = sys.argv[1:] if argv is None else argv
    docs = Path(args[0] if args else "docs")
    pages = sorted(p for p in docs.glob("*.md") if not p.name.startswith("_"))
    for page in pages:
        if page.read_text(encoding="utf-8").startswith("---\n"):
            sys.exit(f"{page} already has frontmatter: {docs} was already converted")
    index = build_index(docs)
    problems: list[str] = []
    converted: dict[Path, str] = {}
    for page in pages:
        text, bad = convert_links(page.read_text(encoding="utf-8"), index)
        problems += [f"{page.name}: [[{b}]]" for b in bad]
        title, body = split_title(text, page.stem)
        converted[page] = f"---\ntitle: {json.dumps(title, ensure_ascii=False)}\n---\n\n{body}"
    groups: list[dict] = []
    side = docs / "_Sidebar.md"
    if side.exists():
        groups, bad = sidebar(side.read_text(encoding="utf-8"), index)
        problems += [f"_Sidebar.md: [[{b}]]" for b in bad]
    if problems:
        print("unresolved wiki links (fix the page or add to ALIASES):", file=sys.stderr)
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        return 1
    for page, content in converted.items():
        target = docs / index[page_key(page.stem)]
        target.write_text(content, encoding="utf-8")
        if target != page:
            page.unlink()
    (docs / "_sidebar.json").write_text(json.dumps(groups, indent=2, ensure_ascii=False) + "\n",
                                        encoding="utf-8")
    for name in ("_Sidebar.md", "_Footer.md"):
        (docs / name).unlink(missing_ok=True)
    print(f"converted {len(converted)} page(s); sidebar has "
          f"{sum(len(g['items']) for g in groups)} link(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
