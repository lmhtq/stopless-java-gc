# Build the StoplessGC paper with the texlive/texlive docker image.
TEXIMG := texlive/texlive:latest
DOCKER := docker run --rm -v $(CURDIR):/work -w /work $(TEXIMG)

all: figs main.pdf

figs:
	python3 plots.py

main.pdf: main.tex refs.bib
	$(DOCKER) latexmk -pdf -interaction=nonstopmode main.tex

clean:
	$(DOCKER) latexmk -C main.tex
	rm -f main.bbl

.PHONY: all figs clean
