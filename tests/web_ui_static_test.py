#!/usr/bin/env python3
"""Small dependency-free contract check for the standalone device UI."""

from __future__ import annotations

from collections import Counter
from html.parser import HTMLParser
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]


class InterfaceParser(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.ids: list[str] = []
        self.pages: set[str] = set()
        self.navigation: set[str] = set()

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        attributes = dict(attrs)
        if identifier := attributes.get("id"):
            self.ids.append(identifier)
            if tag == "section" and "page" in (attributes.get("class") or "").split():
                self.pages.add(identifier)
        if page := attributes.get("data-page"):
            self.navigation.add(page)


def main() -> None:
    html = (ROOT / "web" / "index.html").read_text(encoding="utf-8")
    script = (ROOT / "web" / "app.js").read_text(encoding="utf-8")
    parser = InterfaceParser()
    parser.feed(html)
    duplicates = [identifier for identifier, count in Counter(parser.ids).items() if count > 1]
    assert not duplicates, f"duplicate element IDs: {duplicates}"
    assert parser.pages == parser.navigation, f"page/navigation mismatch: {parser.pages ^ parser.navigation}"
    literal_refs = set(re.findall(r"\$\('([^']+)'\)", script))
    missing = literal_refs - set(parser.ids)
    assert not missing, f"JavaScript refers to missing IDs: {sorted(missing)}"
    for stage in ("rectification", "preprocess", "match", "postprocess", "other"):
        assert f"{stage}Time" in parser.ids and f"{stage}Bar" in parser.ids
    print(f"UI contract OK: {len(parser.pages)} pages, {len(parser.ids)} unique IDs")


if __name__ == "__main__":
    main()
