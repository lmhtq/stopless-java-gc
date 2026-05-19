# paper/

LaTeX sources for the two arXiv preprints.

| File | Status | Purpose |
|---|---|---|
| `phase_i.tex` | Workshop draft (~70% prose, results placeholder) | Port of ZGC to CHERI/Morello |
| `phase_ii.tex` | Section scaffolding only | CHERI-native ZGC barrier |
| `refs.bib` | Live bibliography | Shared between both papers |

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
