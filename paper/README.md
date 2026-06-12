# paper/

LaTeX sources for the two arXiv preprints.

| File | Status | Purpose |
|---|---|---|
| `stopless/` | **Full draft with real measured results (10pp, compiles)** | THE paper: capability revocation as the read barrier of a moving GC (what was actually built) |
| `phase_i.tex` | SUPERSEDED pre-pivot draft (kept for history) | Old plan: port ZGC to CHERI/Morello |
| `phase_ii.tex` | SUPERSEDED scaffolding (kept for history) | Old plan: CHERI-native ZGC barrier |
| `refs.bib` | Legacy bibliography (4 fabricated entries fixed 2026-06-12) | superseded by `stopless/refs.bib` (every entry verified) |
| `data/` | Raw measurement datasets | parsed by `stopless/plots.py` |

## stopless/ (the live paper)

```bash
cd paper/stopless
python3 plots.py   # regenerate figs/*.pdf from ../data
make               # latexmk via the texlive/texlive docker image
```

All numbers in §5 come from `paper/data/*.txt`; every refs.bib entry was
verified against the publisher page / arXiv / project site on 2026-06-12
(four entries inherited from the legacy refs.bib had fabricated authors or
wrong venues — since corrected in both files).

## Building

```bash
cd paper/
latexmk -pdf phase_i.tex
```

Requires a TeX distribution with `acmart`. On macOS:

```bash
brew install --cask mactex
```

## Conventions

- Both papers cite the same `refs.bib`. Add new references there, not
  inline.
- `[Placeholder: ...]` comments mark sections that need empirical data
  before submission. Search-grep for them before a submission cycle.
- The Acknowledgements paragraph in both papers discloses Claude Opus
  4.7 / Claude Code assistance and links to the project's risk
  register for detail on what the LLM did and did not contribute.
  Per current arXiv policy, the LLM is not an author; assistance is
  declared.
- ACM submission format (`acmart`, `acmsmall`, `nonacm`) is used for
  draft layout. Switch to the venue's template at submission time.

## arXiv submission checklist

When ready to push the preprint:

1. Substitute real numbers for every `[Placeholder]`.
2. Confirm `refs.bib` is complete; no missing citations.
3. Replace `[Anonymised]` with author + affiliation.
4. Run `latexmk -pdf` and verify the PDF.
5. `tar -czf submission.tar.gz phase_*.tex refs.bib` and upload to
   arXiv (cs.OS or cs.PL primary; cs.AR secondary).
6. Tag the repo: `git tag preprint/phase_i_v1` and push.
7. Update README.md "Status" table.
