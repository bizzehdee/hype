#!/usr/bin/env python3
"""Convert a MediaWiki XML export into one Markdown file per page.

This deliberately conservative converter handles the common prose markup and
keeps anything it cannot faithfully translate visible as MediaWiki source.
"""

from __future__ import annotations

import argparse
import re
import shutil
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


NS = {"mw": "http://www.mediawiki.org/xml/export-0.11/"}


def filename_for(title: str, used: set[str]) -> str:
    base = re.sub(r"[^A-Za-z0-9._-]+", "-", title).strip(".-").lower() or "untitled"
    name = f"{base}.md"
    number = 2
    while name in used:
        name = f"{base}-{number}.md"
        number += 1
    used.add(name)
    return name


def convert_inline(text: str) -> str:
    text = re.sub(r"\[\[([^|\]]+)\|([^\]]+)\]\]", r"[\2](\1)", text)
    text = re.sub(r"\[\[([^\]]+)\]\]", r"[\1](\1)", text)
    text = re.sub(r"\[([^\s\]]+)\s+([^\]]+)\]", r"[\2](\1)", text)
    text = re.sub(r"'''(.*?)'''", r"**\1**", text)
    text = re.sub(r"''(.*?)''", r"*\1*", text)
    return text


def render_templates(text: str) -> str:
    """Expand the templates present in this export into readable Markdown."""
    def replacement(match: re.Match[str]) -> str:
        fields = match.group(1).split("|")
        name = fields[0].strip().replace("_", " ").lower()
        positional = []
        named = {}
        for field in fields[1:]:
            key, separator, value = field.partition("=")
            if separator:
                named[key.strip().lower()] = value.strip()
            else:
                positional.append(field.strip())
        first = positional[0] if positional else ""
        if name == "main":
            return f"*Main article:* [[{first}]]"
        if name in {"in progress", "stub"}:
            message = ("This page or section is a work in progress and may be incomplete."
                       if name == "in progress" else "This page or section is a stub.")
            return f"> {message}"
        if name == "historical":
            return "> This content is a historical curiosity and should not be used for new designs."
        if name == "tone":
            return "> **Note:** This article's tone or style may not be encyclopedic."
        if name == "admonitionnote":
            return f"> **Note:** {named.get('text', first)}"
        if name == "disputed":
            return "> **Disputed:** The factual accuracy of this article or section is disputed."
        if name == "rating":
            levels = {"1": "Beginner", "2": "Medium", "3": "Advanced", "4": "Master"}
            return f"> **Difficulty:** {levels.get(first or named.get('level', ''), 'Not rated')}"
        if name == "template:kernel designs":
            return ("> **Kernel designs:** [Monolithic Kernel](Monolithic Kernel), "
                    "[Microkernel](Microkernel), [Hybrid Kernel](Hybrid Kernel), "
                    "[Exokernel](Exokernel), [Nanokernel](Nanokernel).")
        if name == "acpi":
            return ("> **ACPI tables:** [RSDP](RSDP), [FADT](FADT), [MADT](MADT), "
                    "[MCFG](MCFG), [RSDT](RSDT), [XSDT](XSDT), [DSDT](DSDT), [SSDT](SSDT).")
        if name in {"yes", "no"}:
            return first or ("Yes" if name == "yes" else "No")
        if name == "wikitable" or name.startswith("displaytitle:"):
            return ""
        return match.group(0)

    # Main-namespace pages use only non-nested transclusions. Repeating this
    # handles any expansion that reveals another such call.
    previous = None
    while text != previous:
        previous = text
        text = re.sub(r"\{\{([^{}]+)\}\}", replacement, text)
    return text


def convert_wikitext(source: str, is_template: bool) -> str:
    if is_template:
        return "```mediawiki\n" + source.rstrip() + "\n```\n"

    text = render_templates(source.replace("\r\n", "\n"))
    text = re.sub(r"<syntaxhighlight(?:\s+[^>]*)?>", "```", text, flags=re.I)
    text = re.sub(r"</syntaxhighlight>", "```", text, flags=re.I)
    text = re.sub(r"<source(?:\s+[^>]*)?>", "```", text, flags=re.I)
    text = re.sub(r"</source>", "```", text, flags=re.I)

    def convert_prose(prose: str) -> str:
        prose = re.sub(r"<code>(.*?)</code>", r"`\1`", prose, flags=re.I | re.S)
        prose = re.sub(r"<ref(?:\s+[^>]*)?>(.*?)</ref>", r"[^\1]", prose, flags=re.I | re.S)
        prose = re.sub(r"<ref(?:\s+[^>]*)?\s*/>", "", prose, flags=re.I)
        prose = re.sub(r"^======\s*(.*?)\s*======\s*$", r"###### \1", prose, flags=re.M)
        prose = re.sub(r"^=====\s*(.*?)\s*=====\s*$", r"##### \1", prose, flags=re.M)
        prose = re.sub(r"^====\s*(.*?)\s*====\s*$", r"#### \1", prose, flags=re.M)
        prose = re.sub(r"^===\s*(.*?)\s*===\s*$", r"### \1", prose, flags=re.M)
        prose = re.sub(r"^==\s*(.*?)\s*==\s*$", r"## \1", prose, flags=re.M)
        prose = re.sub(r"^\*\s*", "- ", prose, flags=re.M)
        prose = re.sub(r"^#(?!#)[ \t]+", "1. ", prose, flags=re.M)
        return convert_inline(prose)

    # Do not interpret list markers or formatting in fenced source examples.
    parts = text.split("```")
    for index in range(0, len(parts), 2):
        parts[index] = convert_prose(parts[index])
    return "```".join(parts).rstrip() + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    if args.output.exists():
        shutil.rmtree(args.output)
    args.output.mkdir(parents=True)

    root = ET.parse(args.source).getroot()
    used: set[str] = set()
    count = 0
    for page in root.findall("mw:page", NS):
        title = page.findtext("mw:title", namespaces=NS) or "Untitled"
        revision = page.find("mw:revision", NS)
        if revision is None:
            continue
        text = revision.findtext("mw:text", default="", namespaces=NS) or ""
        page_id = page.findtext("mw:id", namespaces=NS) or ""
        revision_id = revision.findtext("mw:id", namespaces=NS) or ""
        timestamp = revision.findtext("mw:timestamp", namespaces=NS) or ""
        is_template = title.startswith("Template:")
        if is_template:
            continue
        body = convert_wikitext(text, is_template)
        header = (
            "---\n"
            f"title: {title!r}\n"
            f"source_page_id: {page_id!r}\n"
            f"source_revision_id: {revision_id!r}\n"
            f"source_timestamp: {timestamp!r}\n"
            "source_format: mediawiki\n"
            "---\n\n"
            f"# {title}\n\n"
        )
        (args.output / filename_for(title, used)).write_text(header + body, encoding="utf-8")
        count += 1
    print(f"Converted {count} pages to {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
