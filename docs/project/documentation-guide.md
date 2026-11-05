# Documentation guide

This guide keeps the LLAM docs useful after the first launch. The goal is simple:
readers should know what to do next without decoding the whole runtime first.

## Product principle

Write every page so the reader can answer one question quickly.

| Reader question | Page should answer |
| --- | --- |
| Can I install it? | Show one release install command first. |
| Can I run it? | Show a minimal working program before deep concepts. |
| Can I embed it? | Show explicit runtime handles and owner rules. |
| Can I trust it in production? | Show platform gates, stress, soak, diagnostics, and release policy. |
| Can I bind it from another runtime? | Show ABI checks, size macros, and stable struct rules. |
| Can I debug it? | Show stats, dumps, wait ownership, and timeout artifacts. |

## Site structure

Keep these tabs present in `mkdocs.yml`. Each page answers one question, so split
a page when it starts answering two.

| Tab | Required content | Update when |
| --- | --- | --- |
| Docs | Overview. Get started: quickstart, build, platform support. Concepts. Guides. Production. | Install, lifecycle, runtime semantics, operational guidance, CI gates, or release process change. |
| Examples | Copyable task, timer, channel, select, I/O, embedding, and blocking examples. | Public API signatures or recommended usage change. |
| Reference | Public API, options, environment variables, CLI tools, ABI contract, security model. | Headers, option structs, tool flags, ABI, env vars, or the broker boundary change. |
| Internals | Architecture, magic2 integration, testing strategy, capability policy. | Major scheduler, I/O backend, broker, or test architecture changes. |
| Project | Changelog, roadmap, licensing, and this guide. | Releases, plans, license terms, or documentation standards change. |

Files follow the same structure:

- `README.md` stays a short entry point: links, install, build, and common targets.
  Details belong on a page here.
- Pages live under `docs/`, in folders that match the sidebar groups.
- Site-only files live under `website/`. Release packages copy `docs/` as it
  stands, so stylesheets, scripts, and art stay out of it.

## Page shape

Use the same shape for most pages:

1. Start with `# Title` in sentence case.
2. Follow it with one lead paragraph that says who the page is for or what it
   helps them do.
3. Show the most useful command or code path early.
4. Explain only the decision the reader is making on that page.
5. Put edge cases after the happy path.
6. End with the next page through footer navigation or an explicit link.

Good first screen:

```text
Install LLAM, link it from CMake, and run one task.
```

Weak first screen:

```text
This document discusses the concepts and mechanisms involved in...
```

## Page format

Write plain Markdown first. Use these forms when a page needs more.

| Need | Write |
| --- | --- |
| Commands that differ by platform | Content tabs: `=== "Linux, macOS"` and `=== "Windows"`, with the code indented four spaces. The site keeps every tab on the reader's platform. Use `Linux`, `macOS`, and `Windows` as labels, or join two with a comma. |
| A file name on a code block | A title: ```` ```c title="main.c" ````. |
| A note the reader must not miss | A blockquote. The site draws it as a callout. |
| Entry points on an overview page | A list of links inside `<div class="cards" markdown>`. Add `big` for the top two. |
| A numbered path through a page | An ordered list of links inside `<div class="steps" markdown>`. |
| A checklist | A task list: `- [ ] Item`. |
| A diagram | A `mermaid` code block, then draw it. See [the site theme](#the-site-theme). |

Links between pages use relative paths to the `.md` file, so
`mkdocs build --strict` can check them.

## Writing style

Use a Toss-inspired UX writing style adapted for technical English.

| Rule | Use | Avoid |
| --- | --- | --- |
| Make it easy to answer | "Use `llam_runtime_create()` when the host owns lifecycle." | "Runtime lifecycle can be managed through several APIs depending on context." |
| Use active voice | "LLAM parks the task." | "The task is parked by LLAM." |
| Prefer positive framing | "Use HANDLE APIs for Windows file I/O." | "`llam_pread()` does not work for Windows files." |
| Cut noun stacks | "runtime handle ownership" only when needed | "runtime ownership lifecycle boundary management" |
| One decision per section | "Choose explicit runtime handles when..." | Mixing install, ABI, and scheduler internals in one section |
| Show the next action | "Run `mkdocs build --strict`." | "The docs should be validated." |

Keep headings short and in sentence case. Prefer:

- `Install the release`
- `Use from CMake`
- `Run a minimal program`
- `Verify the install`

Avoid decorative headings that do not tell the reader what changes after
reading the section.

## Example rules

Every example must be clearly one of these. Name the type at the start of the
section, in bold.

| Type | Requirement |
| --- | --- |
| Complete program | Includes headers, `main`, runtime init/run/shutdown, and current API signatures. It builds and runs as written. Show its real output when it prints. |
| Focused snippet | The surrounding context is named in text, and helper functions are obviously user code. |
| Platform-specific example | The platform is named before the code block. |
| Internal example | It is marked as internal and does not imply stable public ABI. |

Examples must use current public signatures. For example:

```c
llam_runtime_init_compat(NULL);
llam_task_t *task = llam_spawn(fn, arg, NULL);
```

Do not publish outdated convenience shapes such as:

```c
llam_runtime_init();
llam_spawn(fn, arg);
```

Plain C examples select a `*_compat` constructor, because the default constructors
refuse task code the managed toolchain did not build.

## Maintenance checklist

Use this checklist for every docs change:

- The page answers one reader question.
- The first screen shows the most useful action or decision.
- Commands and code blocks are copyable.
- Examples use current public API signatures.
- Platform-specific behavior is labeled before the code.
- Reference pages link to deeper semantic contracts.
- Long implementation detail is moved to Concepts, Production, or Internals.
- Footer navigation leads to a useful next page.
- A moved or removed page has a redirect in `mkdocs.yml`. Skip the redirect when
  the old and new URLs match, such as `reference/api.md` and
  `reference/api/index.md`: it would overwrite the page.
- `python3 -m mkdocs build --strict --site-dir /tmp/llam-mkdocs-build` passes.

## Coverage checks

The docs workflow fails unless every public `LLAM_API` function in `include/llam/`
has exactly one row in an `API` table under `docs/reference/api/`, renders as a card
with an anchor, and is in the symbol search index. A mention in prose does not count.
Functions only the compiler calls belong under `Reserved compiler ABI`. The check also
requires each `*_CURRENT_SIZE` macro on a reference page, and each `@param` in a header
to name a real parameter. Run it locally after public header changes:

```sh
python3 scripts/check_docs_api_coverage.py --site-dir /tmp/llam-mkdocs-build
```

Leave out `--site-dir` to check only the headers and pages, without a site build.

## Update triggers

| Change | Required docs update |
| --- | --- |
| Public header signature changes | Examples, Public API, ABI contract, Quickstart if affected. |
| Public option struct changes | Options, Public API, ABI contract, and affected examples. |
| Command-line tool option changes | CLI tools, Production, and affected examples. |
| New environment variable | Environment variables, Options if relevant, and affected guide. |
| Install or CMake option changes | Options, Quickstart, Build from source, README, and the home install panel if affected. |
| Platform status changes | Home, Platform support, and Platform differences. |
| CI or release gate changes | Verification, Production checklist, Testing strategy. |
| Benchmark policy changes | Benchmarks and Performance tuning. |
| Broker or security boundary changes | Security model and Architecture. |
| Installer/package changes | Home install panel, Quickstart, Build from source. |

## The site theme

The site is MkDocs with its own theme. Everything site-only lives in `website/`.

| Path | Holds |
| --- | --- |
| `theme/` | Page templates, `assets/css/llam.css`, and the scripts in `assets/js/`. |
| `hooks.py` | Turns rendered Markdown into the theme's markup. It reads the public headers for API rows, signatures, and the version. |
| `samples/` | The C programs on the home page. They build and run as written, so check them against the headers when a signature changes. |
| `src/`, `build.mjs` | The demo and the libraries the theme loads on demand. `npm run build` writes them and the fonts to `theme/assets/vendor/`, which is not committed. |
| `diagrams/` | A drawing of each `mermaid` block. The site ships these instead of Mermaid. |

The theme adds API rows and filters by page. A table under `docs/reference/api/`
whose first column is headed `API` becomes symbol rows, with each row's signature,
parameters, and return values taken from the headers. Declarations directly under
one comment share it, and each row shows only the `@param` entries for its own
parameters. Write `@param[out]` or `@param[in,out]` when a pointer is written. Pages listed in `FILTER_PAGES` in
`hooks.py` get a filter. A C block of 30 lines or more starts with its trailing
`main()` folded when `main()` is a small part of it, and Copy still takes the whole
program.

Each page's Report a problem link opens
`.github/ISSUE_TEMPLATE/docs-problem.yml` with the page address filled in.

Draw again after changing a diagram. The site build fails when a drawing is
missing, so a changed `mermaid` block cannot ship without one:

```sh
cd website/diagrams && npm ci && node draw.mjs
```

The drawings need the fonts from `npm run build`, and a local Chrome.

Third-party code stays out of the first-party folders. A demo dependency that is
not on npm goes under `third_party/`, and every library the site uses is listed in
`THIRD_PARTY_NOTICES.md`.

## Publishing

The docs publish as a static site on GitHub Pages from
`.github/workflows/docs.yml`. Nothing runs on a server, so search runs in the
browser and moved pages redirect through generated HTML.

Local check:

```sh
python3 -m pip install -r website/requirements.txt
python3 -m mkdocs build --strict --site-dir /tmp/llam-mkdocs-build
```

Local preview, with the fonts, demo, and skeletons:

```sh
cd website && npm ci --ignore-scripts && npm run build && cd ..
python3 -m mkdocs serve -a 127.0.0.1:8001
```

Without `npm run build` the site still works, in system fonts and without the
demo, the glass, or skeletons.

The generated `site/` directory is ignored. Commit source files under `docs/` and
`website/`, `mkdocs.yml`, and workflow changes, not local build output.
