# RadFI v1 paper Makefile
# Convert markdown -> PDF via pandoc + lualatex
# Cible: paper.pdf, IEEEtran 2-column 10pt A4

PAPER = paper

all: $(PAPER).pdf

$(PAPER).pdf: $(PAPER).md
	pandoc $(PAPER).md \
		--from markdown \
		--to pdf \
		--pdf-engine=lualatex \
		--variable documentclass=IEEEtran \
		--variable classoption=conference \
		--variable papersize=a4 \
		--variable fontsize=10pt \
		--output $(PAPER).pdf

clean:
	rm -f $(PAPER).pdf

.PHONY: all clean
