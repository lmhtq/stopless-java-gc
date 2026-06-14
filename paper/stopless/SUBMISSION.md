# arXiv submission checklist — StoplessGC paper

Paper: *Trap, Forward, Resume: Capability Revocation as the Read Barrier of a
Moving Garbage Collector on CHERI*
Author: bluecat (lmhtq1991@gmail.com)
Status as of 2026-06-13: **content-complete, compiles standalone, 14 pages.**
External adversarial review (codex/gpt-5.5, 8 rounds): **Strong Accept, 9.5/10.**

---

## A. Done & verified (no action needed)

- [x] **Compiles clean** — `make` (texlive docker) and a from-scratch
      `pdflatex → bibtex → pdflatex ×2` both produce `main.pdf`, 14 pp,
      0 errors, 0 undefined references/citations. (The lone log warning is a
      harmless inconsolata-italic font substitution.)
- [x] **Standalone tarball verified** — `arxiv-submission.tar.gz`
      (`main.tex`, `refs.bib`, `main.bbl`, `figs/*.pdf`) was extracted to a
      clean directory and recompiled in isolation; identical 14-pp output.
      This is the exact file to upload.
- [x] **No placeholders** — 0 hits for Placeholder/TODO/FIXME/TBD/`\todo`.
- [x] **No leftover anonymisation markers** in the body.
- [x] **Bibliography integrity** — every `\cite` resolves to a `refs.bib`
      entry; every entry web-verified against publisher/arXiv on 2026-06-12
      (see `refs.bib` header). One unused entry (`g1`) is harmless.
- [x] **Figures** — all three (`heap_independence`, `batched_revoke`,
      `sound_concurrent`) are vector PDFs, present, referenced, regenerable
      via `python3 plots.py` from `../data/`.
- [x] **Every §5 number traces to `paper/data/`** — pause/batching/sound
      traces (`*.txt`), heal/acmp logs (`*.out`), full trial logs
      (`trials/*.gz` incl. fault-injection runs).
- [x] **Layout** — 0 overfull boxes > 20 pt.
- [x] **AI contribution statement** present (first-page author note + end
      statement); complies with arXiv "AI not an author, disclose use" policy.
- [x] **Artifact-availability paragraph** present (repo URL currently
      `[anon]` — de-anonymise at submission, see B).

## B. Author decisions required before upload

- [ ] **De-anonymise the artifact URL.** `main.tex` has
      `https://github.com/[anon]/stopless-java-gc`. Replace `[anon]` with the
      real GitHub org/user, OR keep a neutral placeholder until the repo is
      public. The author name/email/affiliation are already real
      ("Independent Researcher, China" — change affiliation if desired).
- [ ] **Make a public artifact** (recommended, not required for arXiv).
      Options: push the repo public, or deposit a Zenodo archive and cite its
      DOI. NOTE: `third_party/openjdk-jdk17` and `third_party/cheribsd` are
      large upstream trees — publish the *patches* (`patches/`) + the
      `src/cap_runtime/` runtime + benchmarks + `paper/data/`, not the full
      checkouts. A `scripts/bootstrap.sh`-style fetch+apply is the norm.
- [ ] **Pick arXiv categories.** Recommended:
      primary **cs.PL** (programming languages — GC, barriers) or **cs.OS**;
      secondary **cs.AR** (capability hardware/Morello). Cross-list the other
      of PL/OS.
- [ ] **Pick a licence** for the arXiv submission (default arXiv
      non-exclusive, or CC-BY). Add a code licence (e.g. GPLv2 to match
      OpenJDK, or Apache-2.0 for the cap_runtime) in the public repo.
- [ ] **Abstract for the arXiv web form.** The paper's own abstract is
      ~2950 chars, OVER arXiv's ~1920-char metadata limit, so the web form
      needs a trimmed version (the in-paper abstract stays full). A ready
      condensed one (1535 chars):

      > Concurrent moving garbage collectors pay for relocation with a read
      > barrier on reference loads. Production collectors implement it in
      > software (ZGC's colored pointers, Shenandoah's forwarding word) or,
      > once, in custom silicon. We repurpose CHERI capability revocation --
      > a hardware temporal-safety mechanism that makes the load-store unit
      > fault on any use of a revoked pointer -- as a moving collector's read
      > barrier. StoplessGC relocates objects, revokes the old copies, and
      > heals each trapped stale reference in a signal handler via a
      > forwarding table; reference loads run no barrier code, since the
      > barrier is the tag check purecap hardware performs anyway. We
      > implement it in the OpenJDK 17 interpreter on Arm Morello and measure
      > it under QEMU. The relocation pause is bounded by roots plus copied
      > bytes, not heap size (it varies 7% while the heap grows 46x). The
      > dominant cost is the whole-address-space revocation sweep; using
      > Morello's per-page capability-load barrier, the implemented
      > load-side mode opens a revocation epoch inside the pause
      > (milliseconds) and runs the heap-linear sweep off-pause, giving a
      > median mutator pause of 18.5 ms on a heap where the synchronous
      > design pauses for seconds. We report the design laws capability
      > hardware forces on a moving collector -- forwarding metadata must be
      > integers re-derived from revocation-exempt roots, chased
      > transitively -- and a taxonomy of pure-capability porting hazards.

## C. Upload steps (when B is settled)

1. De-anonymise the URL in `main.tex`, then `make` to refresh `main.pdf`
   and `main.bbl`.
2. Rebuild the tarball:
   ```
   cd paper/stopless
   rm -rf /tmp/arxiv && mkdir /tmp/arxiv
   cp main.tex refs.bib main.bbl /tmp/arxiv/ && cp -r figs /tmp/arxiv/
   (cd /tmp/arxiv && tar -czf arxiv-submission.tar.gz main.tex refs.bib main.bbl figs)
   ```
   (`main.bbl` is included so arXiv needs no `.bib` pass; both are shipped
   anyway for robustness.)
3. Upload `arxiv-submission.tar.gz` at arxiv.org/submit; set categories and
   licence; paste the abstract; verify arXiv's auto-build PDF matches
   `main.pdf`.
4. (Optional) Tag the repo: `git tag preprint/stopless_v1 && git push --tags`.

## D. Venue note (beyond arXiv)

14 pp acmsmall ≈ ~9–10 pp double-column. Fits a workshop (VMIL@SPLASH,
MoreVMs, ICOOOLPS) as-is; for ISMM/ASPLOS the reviewers' standing asks are
real-Morello-hardware numbers (QEMU only today) and a complete-collector
story (liveness/reclamation/reuse are future work, stated in §6). The
acmart class switches to any ACM venue template by changing the
`\documentclass` options.
