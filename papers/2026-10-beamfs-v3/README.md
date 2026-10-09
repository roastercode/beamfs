# beamfs v3 technical report

*beamfs v3: An EM-Resilient Linux Filesystem under Read-Path Fault
Injection, with an Audit of the Injector.* Technical Report, Version 3.
Aurelien Desbrieres, independent researcher,
ORCID [0009-0002-0912-9487](https://orcid.org/0009-0002-0912-9487).

- Record: [10.5281/zenodo.23253350](https://doi.org/10.5281/zenodo.23253350)
  (all versions: [10.5281/zenodo.19886191](https://doi.org/10.5281/zenodo.19886191))
- PDF: [`beamfs-v3.pdf`](beamfs-v3.pdf)
- Code measured: beamfs 0.1.26, commit `1bf151d`. Tag `beamfs-v3` is
  that code plus this directory.

## What is here, and what is in the record

This directory holds the LaTeX sources, the scripts, the figures, the
generated numbers and tables, and the derived data. The raw run records
(`data/raw`, 2532 files) are in the Zenodo record, inside
`beamfs-v3-workspace.tar.xz`; `data/raw/SHA256SUMS` here freezes them.

The record also holds the root images and kernels measured, their build
and boot records, and the source trees of the Yocto layer, the xfstests
harness, the bench and the Gentoo ebuilds at the commits used. Its
`README.txt` describes every file and how to check it.

## Rebuild

From the root of the workspace archive:

    cd data/raw && sha256sum -c SHA256SUMS && cd ../..
    for m in data/raw/runs/manifest-*.json; do gpg --verify "$m.asc" "$m"; done
    python3 -I scripts/extract.py
    python3 -I scripts/tables.py
    julia scripts/figures.jl
    latexmk -lualatex paper.tex

`scripts/extract.py` writes nothing if a check fails.
`scripts/check-paper.sh --release` runs the tables, the build and the
checks of the appendix: compilation log, typography, raw facts quoted in
the text, DOIs and URLs.

## Cite

    @techreport{desbrieres2026beamfsv3,
      author      = {Aur{\'e}lien Desbri{\`e}res},
      title       = {{beamfs v3}: An {EM}-Resilient {Linux} Filesystem under
                     Read-Path Fault Injection, with an Audit of the Injector},
      type        = {Technical Report},
      number      = {Version 3},
      institution = {Independent researcher},
      publisher   = {Zenodo},
      year        = {2026},
      doi         = {10.5281/zenodo.23253350}
    }

## License

CC BY 4.0, as the Zenodo record.
