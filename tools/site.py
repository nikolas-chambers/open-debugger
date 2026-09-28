#!/usr/bin/env python3
"""Build a self-contained static documentation site from docs/*.md and the landing page."""

import argparse
import html
import pathlib
import re
import shutil
import sys

DOCS = [
    ("audit.html", "docs/AUDIT.md", "Audit"),
    ("roadmap.html", "docs/ROADMAP.md", "Roadmap"),
]

DOC_PAGES = {}
for _href, _rel, _label in DOCS:
    DOC_PAGES[_rel] = _href
    DOC_PAGES[_rel.rsplit("/", 1)[-1]] = _href

DOC_BLURB = {
    "Audit": "Where odbg stands against OllyDbg, x64dbg and WinDbg, feature by feature.",
    "Roadmap": "What is planned, in what order, and why it is planned that way.",
}

STYLE = """
:root{--bg:#fff;--panel:#f6f6f8;--fg:#1c1c21;--muted:#6d6d78;--line:#e4e4ea;--accent:#5b4bdb;--code:#f1f1f4;--quote:#f0eefc}
@media(prefers-color-scheme:dark){:root:not([data-theme=light]){--bg:#111114;--panel:#18181d;--fg:#ececf1;--muted:#9a9aa6;--line:#2a2a32;--accent:#a99cff;--code:#1f1f26;--quote:#1d1b2a}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.65 system-ui,-apple-system,"Segoe UI",sans-serif;display:flex;min-height:100vh}
nav{width:250px;flex:0 0 250px;background:var(--panel);border-right:1px solid var(--line);padding:28px 20px;position:sticky;top:0;height:100vh;overflow-y:auto}
nav .brand{font-weight:700;font-size:18px;letter-spacing:-.02em;margin-bottom:4px}
nav .tag{color:var(--muted);font-size:13px;margin-bottom:22px}
nav a{display:block;padding:7px 10px;margin:0 -10px;border-radius:7px;color:var(--fg);text-decoration:none;font-size:14.5px}
nav a:hover{background:var(--line)}
nav a.on{background:var(--accent);color:#fff}
nav a.top{font-weight:600;margin-bottom:2px}
nav .navlabel{color:var(--muted);font-size:11.5px;text-transform:uppercase;letter-spacing:.09em;margin:20px 10px 6px}
.lede{color:var(--muted);font-size:1.08em;margin-top:-.4em}
.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(250px,1fr));gap:14px;margin-top:1.6em}
.card{display:block;padding:18px 20px;border:1px solid var(--line);border-radius:12px;background:var(--panel);text-decoration:none;color:inherit;transition:border-color .15s,transform .15s}
.card:hover{border-color:var(--accent);transform:translateY(-2px)}
.card h2{margin:0 0 6px;font-size:1.12em;border:0;padding:0}
.card p{margin:0;color:var(--muted);font-size:.94em;line-height:1.5}
main{flex:1;min-width:0;padding:48px 40px 96px;max-width:900px}
h1,h2,h3,h4{line-height:1.25;letter-spacing:-.02em;margin:1.6em 0 .6em}
h1{font-size:2.1em;margin-top:0;padding-bottom:.35em;border-bottom:1px solid var(--line)}
h2{font-size:1.5em;padding-bottom:.3em;border-bottom:1px solid var(--line)}
h3{font-size:1.18em}
p,ul,ol,table,pre{margin:0 0 1.05em}
a{color:var(--accent)}
ul,ol{padding-left:1.5em}
li{margin:.3em 0}
code{background:var(--code);padding:.15em .38em;border-radius:5px;font-size:.87em;font-family:ui-monospace,SFMono-Regular,Menlo,monospace}
pre{background:var(--code);border:1px solid var(--line);border-radius:10px;padding:15px 17px;overflow-x:auto}
pre code{background:none;padding:0;font-size:.86em;line-height:1.55}
table{border-collapse:collapse;width:100%;font-size:.94em;display:block;overflow-x:auto}
th,td{border:1px solid var(--line);padding:8px 12px;text-align:left;vertical-align:top}
th{background:var(--panel);font-weight:600}
tr:nth-child(2n) td{background:color-mix(in srgb,var(--panel) 45%,transparent)}
blockquote{margin:0 0 1.05em;padding:.5em 1em;border-left:3px solid var(--accent);background:var(--quote);color:var(--muted)}
hr{border:0;border-top:1px solid var(--line);margin:2em 0}
.anchor{color:var(--muted);text-decoration:none;opacity:0;margin-left:.35em;font-weight:400}
h2:hover .anchor,h3:hover .anchor{opacity:1}
footer{color:var(--muted);font-size:13px;margin-top:56px;padding-top:20px;border-top:1px solid var(--line)}
@media(max-width:820px){body{display:block}nav{width:auto;flex:none;height:auto;position:static;border-right:0;border-bottom:1px solid var(--line)}main{padding:28px 20px 64px}}
"""


def slug(text):
    s = re.sub(r"[^\w\s-]", "", text.lower())
    return re.sub(r"[\s_]+", "-", s).strip("-")


def inline(text):
    out = []
    i = 0
    while i < len(text):
        c = text[i]
        if c == "\\" and i + 1 < len(text):
            out.append(html.escape(text[i + 1]))
            i += 2
            continue
        if c == "`":
            j = text.find("`", i + 1)
            if j > i:
                out.append("<code>" + html.escape(text[i + 1 : j]) + "</code>")
                i = j + 1
                continue
        if text.startswith("**", i):
            j = text.find("**", i + 2)
            if j > i:
                out.append("<strong>" + inline(text[i + 2 : j]) + "</strong>")
                i = j + 2
                continue
        if c in "*_":
            j = text.find(c, i + 1)
            if j > i and j - i > 1:
                out.append("<em>" + inline(text[i + 1 : j]) + "</em>")
                i = j + 1
                continue
        if c == "[":
            m = re.match(r"\[([^\]]*)\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)", text[i:])
            if m:
                href = m.group(2)
                if not re.match(r"^[a-z]+://|^#|^mailto:", href):
                    if href.endswith(".md"):
                        href = DOC_PAGES.get(href) or DOC_PAGES.get(href.rsplit("/", 1)[-1]) or href[:-3] + ".html"
                out.append('<a href="%s">%s</a>' % (html.escape(href, quote=True), inline(m.group(1))))
                i += m.end()
                continue
        if c == "<" and re.match(r"<https?://[^>]+>", text[i:]):
            m = re.match(r"<(https?://[^>]+)>", text[i:])
            out.append('<a href="%s">%s</a>' % (m.group(1), html.escape(m.group(1))))
            i += m.end()
            continue
        out.append(html.escape(c))
        i += 1
    return "".join(out)


def cells(row):
    row = row.strip()
    if row.startswith("|"):
        row = row[1:]
    if row.endswith("|") and not row.endswith("\\|"):
        row = row[:-1]
    parts, buf, i = [], "", 0
    while i < len(row):
        if row[i] == "\\" and i + 1 < len(row):
            buf += row[i + 1]
            i += 2
            continue
        if row[i] == "|":
            parts.append(buf.strip())
            buf = ""
            i += 1
            continue
        buf += row[i]
        i += 1
    parts.append(buf.strip())
    return parts


def is_table_row(line):
    return line.lstrip().startswith("|") and line.strip().count("|") >= 1


def convert(text):
    lines = text.replace("\r\n", "\n").split("\n")
    out, i, n = [], 0, len(lines)
    while i < n:
        line = lines[i]
        stripped = line.strip()

        if not stripped:
            i += 1
            continue

        if stripped.startswith("```"):
            lang = stripped[3:].strip().split()[0] if stripped[3:].strip() else ""
            i += 1
            buf = []
            while i < n and not lines[i].strip().startswith("```"):
                buf.append(lines[i])
                i += 1
            i += 1
            cls = ' class="language-%s"' % html.escape(lang, quote=True) if lang else ""
            out.append("<pre><code%s>%s</code></pre>" % (cls, html.escape("\n".join(buf))))

        elif re.match(r"^#{1,6}\s", stripped):
            m = re.match(r"^(#{1,6})\s+(.*?)\s*#*$", stripped)
            level, body = len(m.group(1)), m.group(2)
            ident = slug(re.sub(r"[`*_\[\]]", "", body))
            out.append(
                '<h%d id="%s">%s<a class="anchor" href="#%s">#</a></h%d>'
                % (level, ident, inline(body), ident, level)
            )
            i += 1

        elif re.match(r"^(-{3,}|\*{3,}|_{3,})$", stripped):
            out.append("<hr>")
            i += 1

        elif stripped.startswith("> "):
            buf = []
            while i < n and lines[i].strip().startswith("> "):
                buf.append(lines[i].strip()[2:])
                i += 1
            out.append("<blockquote>%s</blockquote>" % inline(" ".join(buf)))

        elif is_table_row(line) and i + 1 < n and re.match(r"^\s*\|[\s:|-]+\|?\s*$", lines[i + 1]):
            head = cells(line)
            i += 2
            body = []
            while i < n and is_table_row(lines[i]):
                body.append(cells(lines[i]))
                i += 1
            t = ["<table><thead><tr>"]
            t += ["<th>%s</th>" % inline(c) for c in head]
            t.append("</tr></thead><tbody>")
            for row in body:
                t.append("<tr>" + "".join("<td>%s</td>" % inline(c) for c in row) + "</tr>")
            t.append("</tbody></table>")
            out.append("".join(t))

        elif re.match(r"^(?:[-*+]|\d+[.)])\s", stripped):
            ordered = bool(re.match(r"^\d+[.)]\s", stripped))
            tag = "ol" if ordered else "ul"
            items, cur = [], None
            while i < n:
                cur_line = lines[i]
                cur_strip = cur_line.strip()
                if not cur_strip:
                    if cur is None:
                        break
                    items.append(cur)
                    cur = None
                    i += 1
                    continue
                m = re.match(r"^(?:[-*+]|\d+[.)])\s+(.*)$", cur_strip)
                if m:
                    if cur is not None:
                        items.append(cur)
                    cur = m.group(1)
                    i += 1
                    continue
                if cur is not None and (cur_line.startswith(("  ", "\t")) or not re.match(r"^(?:[-*+]|\d+[.)])\s", cur_strip)):
                    cur += " " + cur_strip
                    i += 1
                    continue
                break
            if cur is not None:
                items.append(cur)
            out.append("<%s>%s</%s>" % (tag, "".join("<li>%s</li>" % inline(it) for it in items), tag))

        else:
            buf = []
            while i < n and lines[i].strip() and not re.match(r"^(#{1,6}\s|```|> )", lines[i].strip()) and not re.match(r"^(?:[-*+]|\d+[.)])\s", lines[i].strip()):
                raw = lines[i].rstrip()
                hard = len(raw) - len(raw.rstrip(" ")) >= 2
                buf.append(lines[i].strip() + ("<br>" if hard else ""))
                i += 1
            out.append("<p>%s</p>" % inline(" ".join(buf)))

    return "\n".join(out)


def page(title, body, current):
    nav = ['<nav><div class="brand">open-debugger</div><div class="tag">a Windows user-mode debugger on DbgEng</div>']
    for href, label in (("index.html", "Homepage"), ("docs.html", "Docs")):
        cls = "top on" if href == current else "top"
        nav.append('<a class="%s" href="%s">%s</a>' % (cls, href, html.escape(label)))
    nav.append('<div class="navlabel">Reference</div>')
    for href, _, label in DOCS:
        cls = " class=\"on\"" if href == current else ""
        nav.append('<a href="%s"%s>%s</a>' % (href, cls, html.escape(label)))
    nav.append("</nav>")
    return (
        "<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
        '<meta name="viewport" content="width=device-width,initial-scale=1">\n'
        "<title>%s - open-debugger</title>\n"
        '<link rel="stylesheet" href="style.css">\n'
        "</head>\n<body>\n%s\n<main>\n%s\n"
        '<footer>open-debugger &middot; <a href="https://github.com/nikolas-chambers/open-debugger">source</a></footer>\n'
        "</main>\n</body>\n</html>\n"
    ) % (html.escape(title), "\n".join(nav), body)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=".")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    src = pathlib.Path(args.src).resolve()
    out = pathlib.Path(args.out).resolve()
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    landing = src / "open-debugger-page" / "index.html"
    if not landing.is_file():
        sys.exit("missing source: open-debugger-page/index.html")
    shutil.copyfile(landing, out / "index.html")
    print("wrote", out / "index.html")

    cards = "".join(
        '<a class="card" href="%s"><h2>%s</h2><p>%s</p></a>' % (href, html.escape(label), inline(DOC_BLURB[label]))
        for href, _, label in DOCS
    )
    (out / "docs.html").write_text(
        page("Docs", "<h1>Docs</h1><p class=\"lede\">How odbg compares to the field, and where it is going.</p>"
                     '<div class="cards">%s</div>' % cards, "docs.html"),
        encoding="utf-8",
    )
    print("wrote", out / "docs.html")

    for href, rel, label in DOCS:
        path = src / rel
        if not path.is_file():
            sys.exit("missing source: %s" % rel)
        (out / href).write_text(page(label, convert(path.read_text(encoding="utf-8")), href), encoding="utf-8")
        print("wrote", out / href)

    (out / "style.css").write_text(STYLE.lstrip(), encoding="utf-8")
    print("wrote", out / "style.css")


if __name__ == "__main__":
    main()
