# Documentation Guide

This guide keeps the LLAM docs useful after the first launch. The goal is simple:
readers should know what to do next without decoding the whole runtime first.

## Product Principle

Write every page so the reader can answer one question quickly.

| Reader question | Page should answer |
| --- | --- |
| Can I install it? | Show one release install command first. |
| Can I run it? | Show a minimal working program before deep concepts. |
| Can I embed it? | Show explicit runtime handles and owner rules. |
| Can I trust it in production? | Show platform gates, stress, soak, diagnostics, and release policy. |
| Can I bind it from another runtime? | Show ABI checks, size macros, and stable struct rules. |
| Can I debug it? | Show stats, dumps, wait ownership, and timeout artifacts. |

## Required Site Sections

Keep these sections present in `mkdocs.yml`.

| Section | Required content | Update when |
| --- | --- | --- |
| Home | Install panel, 3-step start path, purpose-based entry cards. | Install command, platform status, or core positioning changes. |
| Getting Started | Install, link, minimal program, install verification. | Package layout, CMake target, or public lifecycle API changes. |
| Examples | Copyable task, timer, channel, select, I/O, embedding, and blocking examples. | Public API signatures or recommended usage changes. |
| Concepts | Execution model, runtime handles, synchronization, I/O model. | Runtime semantics, ownership rules, or backend behavior changes. |
| Guides | Embedding, diagnostics, performance tuning. | Operational guidance or recommended integration patterns change. |
| Reference | Public API, options, CLI tools, ABI, environment variables, platform support. | Headers, option structs, tool flags, ABI, env vars, or target support changes. |
| Operations | Verification, benchmarks, troubleshooting, production checklist. | CI gates, stress commands, benchmark policy, or release process changes. |
| Internals | Architecture and testing strategy. | Major scheduler, I/O backend, broker, or test architecture changes. |
| Project | Roadmap and this documentation guide. | Roadmap, site structure, or documentation standards change. |

## Page Shape

Use the same shape for most pages:

1. Say who the page is for in the first paragraph.
2. Show the most useful command or code path early.
3. Explain only the decision the reader is making on that page.
4. Put edge cases after the happy path.
5. End with the next page through footer navigation or an explicit link.

Good first screen:

```text
Install LLAM, link it from CMake, and run one task.
```

Weak first screen:

```text
This document discusses the concepts and mechanisms involved in...
```

## Writing Style

Use a Toss-inspired UX writing style adapted for technical English.

| Rule | Use | Avoid |
| --- | --- | --- |
| Make it easy to answer | "Use `llam_runtime_create()` when the host owns lifecycle." | "Runtime lifecycle can be managed through several APIs depending on context." |
| Use active voice | "LLAM parks the task." | "The task is parked by LLAM." |
| Prefer positive framing | "Use HANDLE APIs for Windows file I/O." | "`llam_pread()` does not work for Windows files." |
| Cut noun stacks | "runtime handle ownership" only when needed | "runtime ownership lifecycle boundary management" |
| One decision per section | "Choose explicit runtime handles when..." | Mixing install, ABI, and scheduler internals in one section |
| Show the next action | "Run `mkdocs build --strict`." | "The docs should be validated." |

Keep headings short. Prefer:

- `Install Release`
- `Use From CMake`
- `Run The First Task`
- `Verify The Install`

Avoid decorative headings that do not tell the reader what changes after
reading the section.

## Example Rules

Every example must be clearly one of these:

| Type | Requirement |
| --- | --- |
| Complete program | Includes headers, `main`, runtime init/run/shutdown, and current API signatures. |
| Focused snippet | The surrounding context is named in text, and helper functions are obviously user code. |
| Platform-specific example | The platform is named before the code block. |
| Internal example | It is marked as internal and does not imply stable public ABI. |

Examples must use current public signatures. For example:

```c
llam_runtime_init(NULL);
llam_task_t *task = llam_spawn(fn, arg, NULL);
```

Do not publish outdated convenience shapes such as:

```c
llam_runtime_init();
llam_spawn(fn, arg);
```

## Maintenance Checklist

Use this checklist for every docs change:

- The page answers one reader question.
- The first screen shows the most useful action or decision.
- Commands and code blocks are copyable.
- Examples use current public API signatures.
- Platform-specific behavior is labeled before the code.
- Reference pages link to deeper semantic contracts.
- Long implementation detail is moved to Concepts, Operations, or Internals.
- Footer navigation leads to a useful next page.
- `python3 -m mkdocs build --strict --site-dir /tmp/llam-mkdocs-build` passes.

## Coverage Checks

The docs workflow fails when a public `LLAM_API` symbol exists in
`include/llam/runtime.h` or `include/llam/io.h` but is missing from
`docs/reference/api.md`. Run the same intent locally after public header
changes:

```sh
for name in $(rg --no-filename -o 'LLAM_API[^(;]+\s+(llam_[A-Za-z0-9_]+)\(' -r '$1' include/llam/runtime.h include/llam/io.h | sort -u); do
  rg -q "$name" docs/reference/api.md || printf '%s\n' "$name"
done
```

No output means the API reference mentions every public symbol.

## Update Triggers

| Change | Required docs update |
| --- | --- |
| Public header signature changes | Examples, Public API, ABI Contract, Quickstart if affected. |
| Public option struct changes | Options, Public API, ABI Contract, and affected examples. |
| Command-line tool option changes | CLI And Tools, Operations, and affected examples. |
| New environment variable | Environment Variables, Options if relevant, and affected guide. |
| Install or CMake option changes | Options, Getting Started, Build From Source, and Home install panel if affected. |
| Platform status changes | Home platform cards and Platform Support. |
| CI or release gate changes | Verification, Operations Guide, Testing Strategy. |
| Benchmark policy changes | Benchmarks and Performance Tuning. |
| Broker or security boundary changes | Security Model and Architecture. |
| Installer/package changes | Home install panel, Getting Started, Build From Source. |

## Publishing

The docs publish through GitHub Pages from `.github/workflows/docs.yml`.

Local check:

```sh
python3 -m pip install -r docs/requirements.txt
python3 -m mkdocs build --strict --site-dir /tmp/llam-mkdocs-build
```

Local preview:

```sh
python3 -m mkdocs serve -a 127.0.0.1:8001
```

The generated `site/` directory is ignored. Commit source files under `docs/`,
`mkdocs.yml`, and workflow changes, not local build output.
