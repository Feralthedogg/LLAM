# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# See LICENSE.
"""MkDocs hooks for the LLAM site.

Turns rendered Markdown into the theme's markup: the page title and lead, code
block chrome, API symbol rows built from the public headers, the checklist, the
changelog summary, pre-rendered diagrams, and the symbol search index.
"""
import hashlib
import html
import json
import logging
import os
import re
import subprocess

from urllib.parse import quote, urlencode

from pygments import highlight
from pygments.formatters import HtmlFormatter
from pygments.lexers import CLexer

log = logging.getLogger("mkdocs.hooks.llam")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADERS = ("runtime.h", "io.h", "managed.h", "platform.h")
GITHUB_BLOB = "https://github.com/Feralthedogg/LLAM/blob/main/"

SYMBOLS = {}
VERSION = {}
SEARCH_SYMBOLS = []
PAGE_TREE = {}

LANGUAGE_NAMES = {
    "c": "C", "cmake": "CMake", "sh": "sh", "bash": "sh", "shell": "sh", "console": "sh",
    "powershell": "PowerShell", "pwsh": "PowerShell", "text": "text", "python": "Python", "py": "Python",
    "json": "JSON", "yaml": "YAML", "toml": "TOML", "makefile": "Makefile", "ini": "INI", "diff": "diff",
    "asm": "asm", "llvm": "LLVM IR", "rust": "Rust", "go": "Go",
}

CHEVRON = '<svg class="i s" viewBox="0 0 24 24" aria-hidden="true"><path d="m9 18 6-6-6-6"></path></svg>'

# Lucide paths (ISC) for the docs landing cards; see THIRD_PARTY_NOTICES.md
CARD_ICONS = {
    "start/quickstart": '<path d="M4.5 16.5c-1.5 1.26-2 5-2 5s3.74-.5 5-2c.71-.84.7-2.13-.09-2.91a2.18 2.18 0 0 0-2.91-.09z"></path><path d="m12 15-3-3a22 22 0 0 1 2-3.95A12.88 12.88 0 0 1 22 2c0 2.72-.78 7.5-6 11a22.35 22.35 0 0 1-4 2z"></path><path d="M9 12H4s.55-3.03 2-4c1.62-1.08 5 0 5 0"></path><path d="M12 15v5s3.03-.55 4-2c1.08-1.62 0-5 0-5"></path>',
    "examples/": '<path d="m18 16 4-4-4-4"></path><path d="m6 8-4 4 4 4"></path><path d="m14.5 4-5 16"></path>',
    "guides/embedding": '<path d="M21 8a2 2 0 0 0-1-1.73l-7-4a2 2 0 0 0-2 0l-7 4A2 2 0 0 0 3 8v8a2 2 0 0 0 1 1.73l7 4a2 2 0 0 0 2 0l7-4A2 2 0 0 0 21 16Z"></path><path d="m3.3 7 8.7 5 8.7-5"></path><path d="M12 22V12"></path>',
    "reference/abi/": '<path d="M12 22v-5"></path><path d="M9 8V2"></path><path d="M15 8V2"></path><path d="M18 8v5a4 4 0 0 1-4 4h-4a4 4 0 0 1-4-4V8Z"></path>',
    "guides/diagnostics": '<path d="m8 2 1.88 1.88"></path><path d="M14.12 3.88 16 2"></path><path d="M9 7.13v-1a3.003 3.003 0 1 1 6 0v1"></path><path d="M12 20c-3.3 0-6-2.7-6-6v-3a4 4 0 0 1 4-4h4a4 4 0 0 1 4 4v3c0 3.3-2.7 6-6 6"></path><path d="M12 20v-9"></path><path d="M6.53 9C4.6 8.8 3 7.1 3 5"></path><path d="M6 13H2"></path><path d="M3 21c0-2.1 1.7-3.9 3.8-4"></path><path d="M20.97 5c0 2.1-1.6 3.8-3.5 4"></path><path d="M22 13h-4"></path><path d="M17.2 17c2.1.1 3.8 1.9 3.8 4"></path>',
    "production/checklist": '<rect width="20" height="8" x="2" y="2" rx="2" ry="2"></rect><rect width="20" height="8" x="2" y="14" rx="2" ry="2"></rect><line x1="6" x2="6.01" y1="6" y2="6"></line><line x1="6" x2="6.01" y1="18" y2="18"></line>',
    "production/verification": '<path d="M12 7v14"></path><path d="M3 18a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1h5a4 4 0 0 1 4 4 4 4 0 0 1 4-4h5a1 1 0 0 1 1 1v13a1 1 0 0 1-1 1h-6a3 3 0 0 0-3 3 3 3 0 0 0-3-3z"></path>',
    "reference/security/": '<path d="M20 13c0 5-3.5 7.5-7.66 8.95a1 1 0 0 1-.67-.01C7.5 20.5 4 18 4 13V6a1 1 0 0 1 1-1c2 0 4.5-1.2 6.24-2.72a1.17 1.17 0 0 1 1.52 0C14.51 3.81 17 5 19 5a1 1 0 0 1 1 1z"></path><path d="m9 12 2 2 4-4"></path>',
}

# Where the exact semantics of each API page's functions are written down
SEMANTICS = {
    "reference/api/lifecycle.md": ("../../abi/lifecycle-tasks/#runtime-lifecycle", "Lifecycle semantics"),
    "reference/api/tasks.md": ("../../abi/lifecycle-tasks/#task-semantics", "Task semantics"),
    "reference/api/time.md": ("../../abi/threading/#deadlines-and-cancellation", "Deadline rules"),
    "reference/api/sync.md": ("../../abi/sync/", "Synchronization semantics"),
    "reference/api/io.md": ("../../abi/io/", "I/O semantics"),
}

FILTER_PAGES = {
    "reference/options/build.md": "Filter fields",
    "reference/options/runtime.md": "Filter fields",
    "reference/environment.md": "Filter variables",
    "reference/cli/servers.md": "Filter tools and flags",
    "reference/cli/benchmarks.md": "Filter tools and flags",
    "reference/cli/operations.md": "Filter tools and flags",
}


# Public headers: signatures and comments


def _doc_inline(text):
    text = html.escape(text, quote=False)
    # A period after the name ends the sentence; it is not part of the name
    text = re.sub(r"@[cp]\s+([\w:&*-]+(?:\.[\w:&*-]+)*)", r"<code>\1</code>", text)
    text = re.sub(r"::(\w+)", r"<code>\1</code>", text)
    return re.sub(r"\s+", " ", text).strip()


def _parse_comment(comment):
    """Split a doxygen comment into brief, details, and return text."""
    lines = []
    for line in comment.strip()[3:-2].split("\n"):
        lines.append(re.sub(r"^\s*\*?\s?", "", line))
    parts = {"brief": [], "details": [], "return": []}
    current = None
    for line in lines:
        m = re.match(r"@(\w+)\s*(.*)", line)
        if m and m.group(1) not in ("c", "p"):
            tag = m.group(1)
            if tag == "brief":
                current = "brief"
            elif tag in ("details", "note"):
                current = "details"
            elif tag in ("return", "returns"):
                current = "return"
            else:
                current = None
            if current:
                parts[current].append(m.group(2))
        elif current:
            parts[current].append(line)
    out = {k: _doc_inline(" ".join(v)) for k, v in parts.items()}
    out["errors"] = sorted(set(re.findall(r"\bE[A-Z0-9]{3,}\b", comment)))
    return out


def _pretty_signature(sig):
    """One argument per line once a declaration is too long to read on one."""
    sig = re.sub(r"\s+", " ", sig).strip() + ";"
    if len(sig) <= 76:
        return sig
    head, _, rest = sig.partition("(")
    args = rest.rsplit(")", 1)[0]
    pad = " " * (len(head) + 1)
    pieces = [a.strip() for a in args.split(",")]
    if len(pad) > 40:
        pad = "    "
        return head + "(\n" + ",\n".join(pad + p for p in pieces) + ");"
    return head + "(" + (",\n" + pad).join(pieces) + ");"


def _load_headers():
    SYMBOLS.clear()
    decl = re.compile(r"(/\*\*(?:(?!\*/).)*?\*/\s*)?LLAM_API\s+([^;{}]*?\b(llam_\w+)\s*\([^;{}]*\))\s*;", re.S)
    macro = re.compile(r"(/\*\*(?:(?!\*/).)*?\*/\s*)?^#\s*define\s+(LLAM_\w+)(\([^)]*\))?", re.S | re.M)
    for name in HEADERS:
        with open(os.path.join(ROOT, "include", "llam", name), encoding="utf-8") as fh:
            text = fh.read()
        for m in decl.finditer(text):
            entry = _parse_comment(m.group(1)) if m.group(1) else {"brief": "", "details": "", "return": "", "errors": []}
            entry.update(sig=_pretty_signature(m.group(2)), header="llam/" + name, line=text.count("\n", 0, m.start(2)) + 1)
            SYMBOLS.setdefault(m.group(3), entry)
        for m in macro.finditer(text):
            if m.group(2) in SYMBOLS:
                continue
            entry = _parse_comment(m.group(1)) if m.group(1) else {"brief": "", "details": "", "return": "", "errors": []}
            entry.update(sig="#define %s%s" % (m.group(2), m.group(3) or ""), header="llam/" + name, line=text.count("\n", 0, m.start(2)) + 1)
            SYMBOLS[m.group(2)] = entry
    with open(os.path.join(ROOT, "include", "llam", "runtime.h"), encoding="utf-8") as fh:
        text = fh.read()
    for key, macro_name in (("major", "LLAM_VERSION_MAJOR"), ("minor", "LLAM_VERSION_MINOR"), ("patch", "LLAM_VERSION_PATCH"),
                            ("abi_major", "LLAM_ABI_VERSION_MAJOR"), ("abi_minor", "LLAM_ABI_VERSION_MINOR")):
        VERSION[key] = re.search(r"#define %s (\d+)" % macro_name, text).group(1)


# Code


def _sub_c_tokens(markup):
    """Pygments sees calls and project types as plain names; the theme colors them."""
    markup = re.sub(r'<span class="kt">(void|char|short|int|long|float|double|signed|unsigned|_Bool|bool)</span>', r'<span class="k">\1</span>', markup)
    markup = re.sub(r'<span class="n">(\w+_t)</span>', r'<span class="kt">\1</span>', markup)
    return re.sub(r'<span class="n">(\w+)</span>(\s*)<span class="p">\(', r'<span class="nf">\1</span>\2<span class="p">(', markup)


SHELL_TOKEN = re.compile(
    r'(?P<comment>(?:^|\s)#.*$)|(?P<string>"(?:[^"\\]|\\.)*"|\'[^\']*\')|(?P<flag>(?<=\s)--?[A-Za-z][\w-]*)|(?P<var>\$\{?\w+\}?|\$env:\w+)'
)


def _shell_line(line, continued):
    head = ""
    if not continued:
        m = re.match(r"(\s*)((?:[A-Z_][A-Z0-9_]*=\S*\s+)*)([^\s#]+)", line)
        if m:
            head = html.escape(m.group(1) + m.group(2), quote=False) + '<span class="nf">%s</span>' % html.escape(m.group(3), quote=False)
            line = line[m.end():]
    out, last = [], 0
    for m in SHELL_TOKEN.finditer(line):
        out.append(html.escape(line[last:m.start()], quote=False))
        cls = {"comment": "c1", "string": "s2", "flag": "kt", "var": "nv"}[m.lastgroup]
        out.append('<span class="%s">%s</span>' % (cls, html.escape(m.group(0), quote=False)))
        last = m.end()
    out.append(html.escape(line[last:], quote=False))
    return head + "".join(out)


def _shell_markup(markup):
    plain = html.unescape(re.sub(r"<[^>]+>", "", markup))
    lines, continued = [], False
    for line in plain.split("\n"):
        lines.append(_shell_line(line, continued))
        stripped = line.rstrip()
        continued = stripped.endswith(("\\", "|", "`"))
    return "\n".join(lines)


CMAKE_TOKEN = re.compile(r'(?P<comment>#.*$)|(?P<string>"(?:[^"\\]|\\.)*")|(?P<keyword>\b[A-Z][A-Z_]+\b)|(?P<call>\b[a-z_]+(?=\())', re.M)


def _cmake_markup(markup):
    plain = html.unescape(re.sub(r"<[^>]+>", "", markup))
    out, last = [], 0
    for m in CMAKE_TOKEN.finditer(plain):
        out.append(html.escape(plain[last:m.start()], quote=False))
        cls = {"comment": "c1", "string": "s2", "keyword": "k", "call": "nf"}[m.lastgroup]
        out.append('<span class="%s">%s</span>' % (cls, html.escape(m.group(0), quote=False)))
        last = m.end()
    out.append(html.escape(plain[last:], quote=False))
    return "".join(out)


FOLD_FROM_LINES = 30
FOLD_MAX_SHARE = 0.4


def _fold_main(code):
    """Split a long program before a trailing main(), which is boilerplate by then."""
    lines = code.split("\n")
    plain = [html.unescape(re.sub(r"<[^>]+>", "", line)) for line in lines]
    while plain and not plain[-1]:
        plain.pop()
    start = next((i for i, line in enumerate(plain) if line.startswith("int main(")), None)
    if start is None or len(plain) < FOLD_FROM_LINES or plain[-1] != "}":
        return None
    count = len(plain) - start
    if count > len(plain) * FOLD_MAX_SHARE:
        return None
    label = html.escape(plain[start].rstrip(" {"), quote=False)
    return "\n".join(lines[:start]) + "\n", "\n".join(lines[start:]), label, count


CODE_BLOCK = re.compile(
    r'<div class="language-(?P<lang>[\w+-]+) highlight">(?:<span class="filename">(?P<title>.*?)</span>)?<pre>(?:<span></span>)?<code>(?P<code>.*?)</code></pre></div>',
    re.S,
)


def _code_chrome(match):
    lang = match.group("lang").lower()
    code = match.group("code")
    if lang in ("c", "cpp", "h"):
        code = _sub_c_tokens(code)
    elif lang in ("sh", "bash", "shell", "console", "powershell", "pwsh"):
        code = _shell_markup(code)
    elif lang == "cmake":
        code = _cmake_markup(code)
    title = match.group("title")
    name = title or LANGUAGE_NAMES.get(lang, lang)
    cls = "cb cb-out" if title == "Output" else "cb"
    named = "cb-name file" if title and title != "Output" else "cb-name"
    copy = "" if title == "Output" else '<button class="cb-copy" type="button">Copy</button>'
    block = '<pre class="code"><code>%s</code></pre>' % code
    folded = _fold_main(code) if lang == "c" else None
    if folded:
        block = (
            '<pre class="code"><code>%s</code></pre><details class="code-fold"><summary>%s%s { … } %d lines</summary>'
            '<pre class="code"><code>%s</code></pre></details>' % (folded[0], CHEVRON, folded[2], folded[3], folded[1])
        )
    return '<div class="%s"><div class="cb-h"><span class="%s">%s</span>%s</div>%s</div>' % (cls, named, name, copy, block)


# Diagrams


def _diagram(match):
    source = html.unescape(match.group(1)).strip()
    key = hashlib.sha256(source.encode("utf-8")).hexdigest()[:16]
    path = os.path.join(ROOT, "website", "diagrams", key + ".svg")
    if os.path.exists(path):
        with open(path, encoding="utf-8") as fh:
            svg = fh.read()
        # Small screens scroll a wide drawing rather than shrink it past reading
        width = re.search(r'viewBox="[-\d.]+ [-\d.]+ ([\d.]+)', svg)
        natural = ' style="--w:%dpx"' % float(width.group(1)) if width else ""
        return '<figure class="diagram"%s>%s</figure>' % (natural, svg)
    log.warning("No rendered diagram for %s. Run npm run diagrams in website/.", key)
    return '<pre class="diagram-source"><code>%s</code></pre>' % match.group(1)


# API rows


def _symbol_row(name, purpose, page_path):
    info = SYMBOLS.get(name)
    wildcard = "*" in name or "[" in name
    anchor = "" if wildcard else ' id="%s"' % name
    head = '<code>%s</code><span>%s</span>' % (html.escape(name), purpose)
    if not info:
        return '<div class="sym plain"%s><div class="sym-h">%s</div></div>' % (anchor, head)
    body = ['<pre class="sig"><code>%s</code></pre>' % html.escape(info["sig"], quote=False)]
    plain_purpose = re.sub(r"<[^>]+>", "", purpose).strip().rstrip(".").lower()
    brief = info["brief"]
    if brief and re.sub(r"<[^>]+>", "", brief).strip().rstrip(".").lower() != plain_purpose:
        body.append("<p>%s</p>" % brief)
    if info["details"]:
        body.append("<p>%s</p>" % info["details"])
    if info["return"]:
        body.append("<p><b>Returns</b> %s</p>" % info["return"])
    tags = ['<span class="tag">%s</span>' % e for e in info["errors"]]
    tags.append('<a class="tag link" href="%sinclude/%s#L%d">%s:%d</a>' % (GITHUB_BLOB, info["header"], info["line"], info["header"], info["line"]))
    if page_path in SEMANTICS:
        target, label = SEMANTICS[page_path]
        tags.append('<a class="tag link" href="%s">%s</a>' % (target, label))
    if not wildcard:
        tags.append('<a class="tag link" href="#%s" data-copy-link>Copy link</a>' % name)
    body.append('<div class="tags">%s</div>' % "".join(tags))
    return '<details class="sym"%s><summary class="sym-h">%s%s</summary><div class="sym-b">%s</div></details>' % (
        anchor, head, CHEVRON, "".join(body))


def _api_table(match, page, group):
    table = match.group(0)
    first = re.search(r"<thead>\s*<tr>\s*<th[^>]*>(.*?)</th>", table, re.S)
    if not first or first.group(1).strip() not in ("API",):
        return table
    rows = []
    for row in re.finditer(r"<tr>\s*<td[^>]*>(.*?)</td>\s*<td[^>]*>(.*?)</td>\s*</tr>", table, re.S):
        names = re.findall(r"<code>(.*?)</code>", row.group(1))
        purpose = row.group(2).strip()
        for name in names or [re.sub(r"<[^>]+>", "", row.group(1)).strip()]:
            name = html.unescape(name)
            rows.append(_symbol_row(name, purpose, page.file.src_uri))
            if "*" not in name and "[" not in name:
                SEARCH_SYMBOLS.append({
                    "n": name,
                    "t": html.unescape(re.sub(r"<[^>]+>", "", purpose)),
                    "w": "Public API / " + group,
                    "u": page.url + "#" + name,
                })
    return '<div class="syms">%s</div>' % "".join(rows)


# Changelog: each version opens to its entries, with its counts on the outside


def _changelog(content):
    parts = re.split(r'(?=<h2 id=")', content)
    out = [parts[0]]
    for i, part in enumerate(parts[1:]):
        m = re.match(r'<h2 id="([^"]*)">(.*?)(?:<a class="headerlink".*?</a>)?</h2>', part, re.S)
        if not m:
            out.append(part)
            continue
        version, _, date = m.group(2).partition(" - ")
        body = part[m.end():]
        groups, order = {}, []
        for g in re.finditer(r'<h3[^>]*>(.*?)(?:<a class="headerlink".*?</a>)?</h3>(.*?)(?=<h3|\Z)', body, re.S):
            title = re.sub(r"<[^>]+>", "", g.group(1)).strip().split(" ")[0]
            depth, count = 0, 0
            for tag in re.finditer(r"<(/?)(ul|li)\b", g.group(2)):
                closing, kind = tag.group(1), tag.group(2)
                if kind == "ul":
                    depth += -1 if closing else 1
                elif not closing and depth == 1:
                    count += 1
            if title not in groups:
                order.append(title)
            groups[title] = groups.get(title, 0) + count
        rank = ["Security", "Added", "Changed", "Fixed", "Performance", "Tests"]
        order.sort(key=lambda t: rank.index(t) if t in rank else len(rank))
        counts = "".join('<span class="ver-n"><b>%d</b> %s</span>' % (groups[t], t) for t in order if groups[t])
        out.append(
            '<details class="ver" id="%s"%s><summary><span class="ver-h"><b>%s</b><small>%s</small></span><span class="ver-c">%s</span>%s</summary><div class="ver-b">%s</div></details>'
            % (m.group(1), " open" if i == 0 else "", version, date, counts, CHEVRON, body)
        )
    return "".join(out)


# MkDocs events


def on_config(config):
    _load_headers()
    SEARCH_SYMBOLS.clear()
    config.extra["llam_version"] = "%(major)s.%(minor)s.%(patch)s" % VERSION
    config.extra["llam_abi"] = "%(abi_major)s.%(abi_minor)s" % VERSION
    return config


def on_nav(nav, config, files):
    PAGE_TREE.clear()

    def walk(items, trail):
        for item in items:
            if item.is_section:
                walk(item.children, trail + [item.title])
            elif item.is_page:
                where = trail[1:] if trail[:1] == ["Docs"] else trail
                PAGE_TREE[item.file.url] = {"t": item.title, "w": " / ".join(where or trail), "tab": trail[0] if trail else ""}

    walk(nav.items, [])
    return nav


def first_page(item):
    while item.is_section:
        item = item.children[0]
    return item


def _load_sample(name):
    with open(os.path.join(ROOT, "website", "samples", name + ".c"), encoding="utf-8") as fh:
        text = fh.read()
    start = text.index("*/") + 2 if text.startswith("/*") else 0
    head, sep, tail = text[start:].strip("\n").partition("\n/* fold */\n")
    formatter = HtmlFormatter(nowrap=True)

    def render(code, first):
        markup = _sub_c_tokens(highlight(code, CLexer(), formatter)).rstrip("\n")
        return [
            '<div class="ln"><span class="gut">%d</span><span>%s</span></div>' % (first + i, line or " ")
            for i, line in enumerate(markup.split("\n"))
        ]

    head_lines = render(head.rstrip("\n"), 1)
    # The blank line between the two parts keeps its number
    tail_lines = render(tail.rstrip("\n"), len(head_lines) + 2) if sep else []
    first = tail.split("\n", 1)[0].rstrip(" {") if sep else ""
    return {"head": "".join(head_lines), "tail": "".join(tail_lines), "tail_count": len(tail_lines), "tail_first": first}


def on_env(env, config, files):
    env.globals["first_page"] = first_page
    env.globals["home_sample"] = _load_sample
    return env


def on_page_content(content, page, config, files):
    src = page.file.src_uri
    # A license comment at the top of a page must not hide its title
    content = re.sub(r"^\s*(?:<!--.*?-->\s*)+", "", content, flags=re.S)
    # Title and lead: the template places them around the page meta
    m = re.match(r'\s*<h1 id="[^"]*">(.*?)(?:<a class="headerlink".*?</a>)?</h1>\s*', content, re.S)
    if m:
        page.meta["h1"] = m.group(1).strip()
        content = content[m.end():]
    if src == "project/changelog.md":
        page.meta["h1"] = "Changelog"
        page.meta["toc_flat"] = True
        content = _changelog(content)
    content = re.sub(r"^<p>", '<p class="lead">', content, count=1)
    lead = re.match(r'<p class="lead">(.*?)</p>', content, re.S)
    if lead and "description" not in page.meta:
        text = re.sub(r"\s+", " ", html.unescape(re.sub(r"<[^>]+>", "", lead.group(1)))).strip()
        page.meta["description"] = text if len(text) <= 180 else text[:180].rsplit(" ", 1)[0] + "…"

    content = re.sub(r'<pre class="mermaid"><code>(.*?)</code></pre>', _diagram, content, flags=re.S)
    content = CODE_BLOCK.sub(_code_chrome, content)

    if src.startswith("reference/api/"):
        group = page.title if src != "reference/api/index.md" else "Overview"
        content = re.sub(r"<table>.*?</table>", lambda t: _api_table(t, page, group), content, flags=re.S)
        names = [html.unescape(n) for n in re.findall(r'class="sym-h"><code>(.*?)</code>', content)]
        if names:
            # One name column per page, as wide as the page's longest name
            content = content.replace('<div class="syms">', '<div class="syms" style="--n:%d">' % max(map(len, names)))
            headers = sorted({SYMBOLS[n]["header"] for n in names if n in SYMBOLS})
            if src != "reference/api/index.md":
                page.meta["declared"] = ", ".join("<%s>" % h for h in headers)
        if src != "reference/api/index.md":
            page.meta["filter"] = "Filter this page"
    elif src in FILTER_PAGES:
        page.meta["filter"] = FILTER_PAGES[src]

    if src == "reference/abi/index.md":
        versions = '<div class="gh-ver"><span>Library<b>%(major)s.%(minor)s.%(patch)s</b></span><span>Public ABI<b>%(abi_major)s.%(abi_minor)s</b></span></div>' % VERSION
        content = content.replace("</p>", "</p>" + versions, 1)

    if src == "start/platforms.md":
        content = re.sub(r"<td>(Primary[^<]*)</td>", r'<td><span class="pill">\1</span></td>', content)
        content = re.sub(r"<td>(Supported)</td>", r'<td><span class="pill q">\1</span></td>', content)

    if src == "production/checklist.md":
        page.meta["checklist"] = True
        content = content.replace('<ul class="task-list">', '<ul class="task-list checklist" data-checklist>', 1)
        content = re.sub(
            r'(<label class="task-list-control">)<input type="checkbox"\s*/?>(?:<span class="task-list-indicator"></span>)?</label>\s*(.*?)\. See <a href="([^"]+)">(.*?)</a>\.</li>',
            lambda c: '%s<input type="checkbox"><span class="tx">%s</span></label><a class="to" href="%s">%s%s</a></li>'
            % (c.group(1), c.group(2), c.group(3), c.group(4), CHEVRON),
            content,
        )

    if src == "overview.md":
        def add_icon(card):
            target = card.group(1)
            for key, paths in CARD_ICONS.items():
                if key in target:
                    return '<li><svg class="i ci" viewBox="0 0 24 24" aria-hidden="true">%s</svg>\n<p><a href="%s">' % (paths, target)
            return card.group(0)
        content = re.sub(r'<li>\s*<p><a href="([^"]+)">', add_icon, content)

    content = re.sub(r"(?<!<div class=\"syms\">)<table>", '<div class="t-wrap"><table>', content)
    content = content.replace("</table>", "</table></div>")

    # The small-screen page list goes right after the lead
    marked, found = re.subn(r'(<p class="lead">.*?</p>)', r"\1<!--toc-m-->", content, count=1, flags=re.S)
    content = marked if found else "<!--toc-m-->" + content
    content = re.sub(
        r'<a class="headerlink" href="(#[^"]*)" title="[^"]*">#</a>',
        r'<a class="headerlink" href="\1" title="Copy link to this section" aria-label="Copy link to this section"></a>',
        content,
    )

    # Last modified, from the file's own history
    page.meta["last_modified"] = _last_modified(page)
    page.meta["report_url"] = _report_url(page, config)
    return content


def _report_url(page, config):
    """Open the documentation problem form with this page filled in."""
    fields = {
        "template": "docs-problem.yml",
        "title": "[Docs] " + page.title,
        "page": page.canonical_url or page.url,
    }
    return config.repo_url.rstrip("/") + "/issues/new?" + urlencode(fields, quote_via=quote)


def _last_modified(page):
    path = page.file.abs_src_path
    try:
        out = subprocess.run(
            ["git", "-C", ROOT, "log", "-1", "--format=%cd", "--date=format:%d %B %Y", "--", path],
            capture_output=True, text=True, timeout=10, check=False,
        ).stdout.strip()
    except OSError:
        return ""
    return out


def on_post_build(config):
    """The search palette reads symbols and page labels from one small file."""
    target = os.path.join(config.site_dir, "search", "site.json")
    os.makedirs(os.path.dirname(target), exist_ok=True)
    with open(target, "w", encoding="utf-8") as fh:
        json.dump({"symbols": SEARCH_SYMBOLS, "pages": PAGE_TREE}, fh, ensure_ascii=False, separators=(",", ":"))
