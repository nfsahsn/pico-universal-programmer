PYTHON ?= python3
PDFLATEX ?= pdflatex
BIBTEX ?= bibtex
TARGET = main
REPLICATION_ARCHIVE = pico-universal-replication-v1.0.tar.gz

.PHONY: all analyze pdf check zenodo clean

all: analyze pdf check

analyze:
	$(PYTHON) scripts/analyze_benchmarks.py --data data --out generated

pdf: analyze
	$(PDFLATEX) -interaction=nonstopmode -halt-on-error $(TARGET).tex
	$(BIBTEX) $(TARGET)
	$(PDFLATEX) -interaction=nonstopmode -halt-on-error $(TARGET).tex
	$(PDFLATEX) -interaction=nonstopmode -halt-on-error $(TARGET).tex

check:
	@if [ ! -f $(TARGET).log ]; then echo "Run 'make pdf' first"; exit 1; fi
	@if grep -E "LaTeX Warning: (Citation|Reference).*undefined|There were undefined references" $(TARGET).log; then \
		echo "FAIL: unresolved citations or references in $(TARGET).log"; exit 1; \
	fi
	@if [ -f $(TARGET).blg ] && grep -E "error message" $(TARGET).blg; then \
		echo "FAIL: BibTeX errors detected in $(TARGET).blg"; exit 1; \
	fi
	@echo "PASS: $(TARGET).pdf verified with zero undefined references or BibTeX errors"

zenodo: analyze
	tar -czf $(REPLICATION_ARCHIVE) \
		-C .. \
		config/ bootloader/ firmware/ host/ catalog/ tools/ hardware/ tests/ paper/main.tex paper/references.bib paper/Makefile paper/REPLICATION.md paper/scripts/ paper/data/ paper/generated/
	sha256sum $(REPLICATION_ARCHIVE) > $(REPLICATION_ARCHIVE).sha256
	@echo "Created Zenodo bundle: paper/$(REPLICATION_ARCHIVE)"

clean:
	rm -f $(TARGET).aux $(TARGET).bbl $(TARGET).blg $(TARGET).log $(TARGET).out $(TARGET).pdf $(REPLICATION_ARCHIVE) $(REPLICATION_ARCHIVE).sha256
