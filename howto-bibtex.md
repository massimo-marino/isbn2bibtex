# How Does BibTeX Work?

BibTeX separates bibliographic data from formatting by storing references in a plain-text .bib database and automatically generating citations and reference lists in LaTeX documents. This allows you to reuse the same reference database across different papers and change citation styles without manually rewriting your bibliography.

[1] (https://www.bibtex.com/g/bibtex-format/),

[2] (https://www.youtube.com/watch?v=dSahJ8ObVLI&t=90),

[3] (https://libguides.usask.ca/bibtex),

[4] (https://www.stat.uchicago.edu/intranet/computing/tutor/bibtex.shtml), [5] (https://guides.lib.purdue.edu/bibtex/basics)

1. The Reference Database (.bib file)

You store all your source metadata in a separate plain-text file (e.g., references.bib). Each entry starts with an @ symbol specifying the source type (such as @article, @book, or @inproceedings), followed by a unique citation key and comma-separated fields:

[1] (https://www.overleaf.com/learn/latex/Bibliography_management_with_bibtex),

[2] (https://www.overleaf.com/learn/latex/Questions/How_to_include_a_bibliography_using_bibtex),

[3] (https://libguides.usask.ca/bibtex),

[4] (https://www.bibtex.com/g/bibtex-format/)

```bibtex
@article{einstein1905,
  author  = {Albert Einstein},
  title   = {Zur Elektrodynamik bewegter K{\"o}rper},
  journal = {Annalen der Physik},
  year    = {1905},
  volume  = {322},
  number  = {10},
  pages   = {891--921}
}
```

• Citation key. The identifier right after the opening brace (einstein1905) acts as a nickname to call the source in your text.
• Exporting. Instead of typing these by hand, you can export .bib entries directly from databases like Google Scholar or reference managers like Zotero and JabRef.
[1] (https://www.bibtex.org/Format/),

[2] (https://www.youtube.com/watch?v=dO71diWMF4o),

[3] (https://www.overleaf.com/learn/latex/Bibliography_management_with_bibtex),

[4] (https://www.bibtex.org/Using/)

2. Citing in Your Document (.tex file)

In your main LaTeX file, you link the database, pick a formatting style, and insert citations using the keys: [1] (https://libguides.mit.edu/latex-bibtex), [2] (https://www.stat.uchicago.edu/intranet/computing/tutor/bibtex.shtml)

```latex
\documentclass{article}
\begin{document}

Special relativity was introduced in 1905 \cite{einstein1905}.

\bibliographystyle{plain}
\bibliography{references}

\end{document}
```

• \cite{key}. Places an in-text citation marker and tells BibTeX to include that specific entry in the final bibliography (uncited items in your .bib file are ignored).
• \bibliographystyle{style}. Dictates how citations and the reference list look (e.g., plain, abbrv, unsrt, or a journal-specific .bst file).
• \bibliography{filename}. Tells LaTeX where to print the reference list and which .bib file to read (omitting the .bib extension).

[1] (https://www.bibtex.org/Using/),

[2] (https://www.overleaf.com/learn/latex/Bibliography_management_with_bibtex),

[3] (https://www.stat.uchicago.edu/intranet/computing/tutor/bibtex.shtml),

[4] (https://www.youtube.com/watch?v=dSahJ8ObVLI&t=90),

[5] (https://tex.stackexchange.com/questions/306218/simple-way-to-use-bibtex)

3. Under-the-Hood Compilation Cycle

Because LaTeX and BibTeX are separate programs, generating the final PDF requires a multi-step build sequence (though modern editors like Overleaf or tools like latexmk automate this for you): [1] (https://www.bibtex.org/Using/), [2] (https://en.wikipedia.org/wiki/BibTeX)

1. First pdflatex run. Scans your .tex file for \cite commands and writes the requested keys to an auxiliary .aux file.

2. bibtex run. Reads the .aux file, pulls matching metadata from your .bib database, formats it according to your .bst style, and outputs a ready-to-typeset .bbl file.

3. Second pdflatex run. Imports the .bbl file into your document to print the bibliography.

4. Third pdflatex run. Resolves and updates all in-text citation numbers or labels to match the newly generated reference list.

   [1] (https://www.bibtex.org/Using/),

   [2] (https://en.wikipedia.org/wiki/BibTeX)
   (Note: For newer projects that don't require legacy journal templates, many users prefer BibLaTeX with the Biber backend—a modern replacement that natively supports Unicode and offers easier style customization while reading the exact same .bib files.)

   [1] (https://tex.stackexchange.com/questions/306218/simple-way-to-use-bibtex),

   [2] (https://www.bibtex.org/Using/)
