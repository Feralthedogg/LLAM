# Third-Party Notices

Copyright 2026 Feralthedogg
SPDX-License-Identifier: Apache-2.0
Licensed under the Apache License, Version 2.0. See LICENSES/Apache-2.0.txt.

This file records algorithm and implementation provenance for LLAM. The
Apache License, Version 2.0 applies to LLAM's original code; the
upstream grants described below continue to apply to the corresponding
third-party material.

## Magic2

The vendored `third_party/magic2/` snapshot is distributed under its upstream
MIT License, preserved in `third_party/magic2/LICENSE`. Its pinned source and
revision are documented in `third_party/magic2/UPSTREAM.md`. Release packages
retain this license at `LICENSES/THIRD_PARTY/magic2/LICENSE`.
The byte-identical private mirror `src/internal/magic2.h` is also MIT-licensed,
not first-party Apache-2.0 code. The provenance file `UPSTREAM.md` is
LLAM-authored Apache-2.0 metadata and is not part of the upstream snapshot.

## SipHash-2-4

LLAM's capability-token MAC uses SipHash-2-4. Its round function and constants
are derived from the reference implementation by Jean-Philippe Aumasson and
Daniel J. Bernstein:

- Upstream: <https://github.com/veorq/SipHash/blob/master/siphash.c>
- Upstream license: CC0 1.0 Universal public-domain dedication

LLAM supplies its own fixed-shape, dual-lane encoding for capability token v1.

## SplitMix64

LLAM uses the SplitMix64 finalizer and deterministic generator described by
Sebastiano Vigna:

- Upstream: <https://prng.di.unimi.it/splitmix64.c>
- Upstream grant: dedicated to the public domain; where that dedication is not
  recognized, unconditional permission is granted to use, copy, modify, and
  distribute the software for any purpose.

The finalizer is used as a non-cryptographic avalanche mixer. It must not be
treated as a cryptographic hash or message-authentication function.

## Chase-Lev Work-Stealing Deque

LLAM's bounded owner/thief deque is an independent C implementation informed
by these algorithm papers:

- David Chase and Yossi Lev, “Dynamic Circular Work-Stealing Deque,” SPAA 2005,
  <https://doi.org/10.1145/1073970.1073974>.
- Nhat Minh Lê, Antoniu Pop, Albert Cohen, and Francesco Zappa Nardelli,
  “Correct and Efficient Work-Stealing for Weak Memory Models,” PPoPP 2013,
  <https://doi.org/10.1145/2442516.2442524>.

These citations identify algorithmic references. LLAM does not vendor source
code from either paper.
