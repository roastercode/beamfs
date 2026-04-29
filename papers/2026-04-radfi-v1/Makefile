# RadFI v1 paper Makefile
# Build: latexmk -lualatex paper.tex
# Output: paper.pdf, IEEEtran 2-column 10pt A4
PAPER = paper

all: $(PAPER).pdf

$(PAPER).pdf: $(PAPER).tex refs.bib sections/*.tex
	latexmk -lualatex -interaction=nonstopmode $(PAPER).tex

clean:
	latexmk -C
	rm -f $(PAPER).pdf

.PHONY: all clean
