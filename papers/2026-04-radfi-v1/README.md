# RadFI v1 — Methodology paper

Technical Report v1: An Algebraic Fault Injection Operator for Empirical Filesystem Resilience Validation.

Author: Aurélien Desbrières
ORCID: 0009-0002-0912-9487
Date: 2026-04-29

This directory contains the public methodology paper for RadFI. The reference implementation source code is held in a private repository (`roastercode/radfi`) pending peer review.

## Build

`make` produces `paper.pdf` via pandoc + lualatex (IEEEtran 2-column 10pt A4).

## Source

`paper.md` — markdown source, 8 pages IEEEtran target.

## Reproducibility

Both empirical results in Section VI are reproducible from the public BEAMFS repository at the tagged commits cited in Section IX (`v1.0.0-paper-pending` and `v0.2.0-palier3-validated`).
