# isbn2bibtex — Python implementation

`isbn2bibtex.py` turns ISBN and DOI codes into BibTeX entries. It queries public
bibliographic web services, appends the entries to a `.bib` file (optional),
keeps a log, and copies everything to the clipboard so you can paste it into
your document.

Target platform: Linux (developed for **Ubuntu 24.04 LTS or later**).

## 1. Requirements

* **Python 3.10 or newer** (Ubuntu 24.04 ships Python 3.12). Only the standard
  library is used: there is nothing to `pip install` and nothing to build.
* A **clipboard tool**: `wl-clipboard` (Wayland) **or** `xclip` / `xsel` (X11).
  Without one the program still works, but prints the BibTeX to the terminal
  instead of copying it.
* An **internet connection** at run time.

```bash
sudo apt update
sudo apt install python3 xclip wl-clipboard     # python3 is normally preinstalled
```

## 2. Running

No build step. Run the script directly:

```bash
python3 isbn2bibtex.py 978-0-13-110362-7
```

or make it executable and (optionally) install it as a command:

```bash
chmod +x isbn2bibtex.py
mkdir -p ~/.local/bin
cp isbn2bibtex.py ~/.local/bin/isbn2bibtex      # ~/.local/bin must be in your PATH
isbn2bibtex --version
```

## 3. What it does and how

1. **Parses the command line.** Exactly one of `<isbn-or-doi>` and
   `--read-from-file` must be given.
2. **Normalises each code.**
   * Accepted forms: bare ISBN (hyphens/spaces allowed), `ISBN:`, `ISBN-10:`,
     `ISBN-13:` prefixes, bare DOI (`10.xxxx/…`), `doi:` and
     `https://doi.org/` prefixes. Surrounding single or double quotes are removed.
   * ISBN-10 and ISBN-13 **check digits are validated**; a wrong check digit is
     reported as a failure (with the expected digit). ISBN-10 is **converted to
     ISBN-13**, the form used in queries, logs and the `isbn` field.
3. **Checks the internet connection** first; if none of the service hosts
   answers, it logs/prints an error and exits with status 3.
4. **Fetches the data**, trying each service in turn and stopping at the first
   one that returns a usable record:
   * ISBN: Open Library (`openlibrary.org/api/books`, gives the place of
     publication), then Google Books (`googleapis.com/books/v1/volumes`).
   * DOI: `doi.org` content negotiation (CSL JSON), then the Crossref REST API.
5. **Retries** a request only for HTTP **408** and **429**, up to `--retry`
   times, waiting `--delay` seconds between attempts.
6. **Builds the BibTeX entry** with at least `author`, `title`, `publisher`,
   `address`, `year`, `url` and `isbn` (a field the service does not provide is
   written empty, e.g. `address` for Google Books records and `isbn` for journal
   articles). DOI records of journal articles also get `journal`, `volume`,
   `number`, `pages`, `issn` and `doi`. The entry type is `@book`, `@article`,
   `@incollection`, `@inproceedings`, `@phdthesis` or `@misc` depending on the
   record. The cite key is `<surname><year><first-title-word>`; duplicates within
   one run get a numeric suffix.
7. **Writes the results:** the entry is appended to the `.bib` file (if
   `--save-to-file`), one log line is written to the log file and to the
   terminal, and at the end **all entries are copied to the clipboard** (also
   when saved to a file).

### Log lines

Success: `<ISBN-or-DOI>: BibTeX generated with request: <HTTP request>`

Failure: `<ISBN-or-DOI>: FAILED: <reason>` — the reason lists every HTTP request
that failed, with the HTTP code and its description (e.g.
`HTTP 404 Not Found`), or the network/parse error.

Retry: `<code>: retry attempt 2 of 3 after HTTP 429 Too Many Requests, waiting 0.5s: <request>`;
if a request succeeds after retrying, the success line ends with
`(succeeded on retry attempt N of M)`.

The log is **appended to** on every run. Success lines go to standard output,
failure lines and warnings to standard error.

### Files

* **Log file:** created in the working directory. `--log <log-file>` sets the
  name (`.log` is appended, with a warning, if missing). Default:
  `isbn2bibtex.log`, or the stem of the input file name with `.log` when
  `--read-from-file` is used (`codes.txt` → `codes.log`).
* **BibTeX file:** `--save-to-file`; created if missing, entries are appended;
  `.bib` is appended with a warning if missing (`refs` → `refs.bib`).
* The log, `.bib` and input files must be **three different files**, otherwise
  the program exits with a usage error before touching anything.

### Input file format

```text
# my reading list
978-0-13-110362-7          # The C Programming Language
doi:10.1038/nphys1170

https://doi.org/10.1103/PhysRevLett.116.061102
```

One code per line, no quotes needed; blank lines, lines starting with `#` and
anything after a `#` are ignored.

### Exit status

| Code | Meaning |
|---|---|
| 0 | every code produced a BibTeX entry (also `--help`, `--version`) |
| 1 | at least one code failed |
| 2 | usage error (bad arguments, same file used twice, unreadable input) |
| 3 | no internet connection |

## 4. Usage

```text
isbn2bibtex.py [ --help ] [ --version ] [ --log <log-file> ]
               { <isbn-or-doi> | --read-from-file <isbn-or-doi-file-name> }
               [ --save-to-file <bibtex-file-name> ]
               [ --retry <max-retries> [ --delay <seconds> ] ]
```

| Option | Description |
|---|---|
| `<isbn-or-doi>` | One ISBN or DOI, optionally quoted. |
| `--read-from-file <file>` | Read many codes from a file. Not combinable with `<isbn-or-doi>`. |
| `--save-to-file <file>` | Append the BibTeX entries to this `.bib` file. |
| `--log <file>` | Log file name (`.log`). |
| `--retry <n>` | Max retries for HTTP 408/429 only. Default 0. |
| `--delay <s>` | Seconds between retries (real number). Used only with `--retry`. Default 0.5. |
| `--help` | Print the help text. |
| `--version` | Print the version. |

### Examples

```bash
# One ISBN-13, entry copied to the clipboard, log in isbn2bibtex.log
python3 isbn2bibtex.py 978-0-13-110362-7

# ISBN-10 (converted to ISBN-13) with a prefix, saved to refs.bib
python3 isbn2bibtex.py "ISBN-10: 0-13-110362-8" --save-to-file refs.bib

# A DOI, in any of the accepted forms
python3 isbn2bibtex.py doi:10.1038/nphys1170
python3 isbn2bibtex.py https://doi.org/10.1038/nphys1170

# Retry up to 5 times, 2 s apart, on HTTP 408/429
python3 isbn2bibtex.py 978-0-306-40615-7 --retry 5 --delay 2

# A list of codes, custom log, entries appended to refs.bib (created if missing)
python3 isbn2bibtex.py --read-from-file codes.txt --save-to-file refs --log run.log
```

Sample output entry:

```bibtex
@book{kernighan1988programming,
  author    = {Brian W. Kernighan and Dennis M. Ritchie},
  title     = {The C Programming Language},
  publisher = {Prentice Hall},
  address   = {Englewood Cliffs, N.J},
  year      = {1988},
  url       = {https://openlibrary.org/books/OL1M/The_C_Programming_Language},
  isbn      = {9780131103627},
}
```

## 5. Notes

* The data come from third-party services; their coverage and quality vary, so
  check the entries (especially `address`, which not every service provides).
* Testing hook: if the environment variable `ISBN2BIBTEX_ENDPOINT_OVERRIDE` is
  set (e.g. `http://127.0.0.1:8765`), the scheme and host of every request are
  replaced by it, which allows testing against a local mock server.
