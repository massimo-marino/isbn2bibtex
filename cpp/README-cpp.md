# isbn2bibtex — C++23 implementation

`isbn2bibtex` turns ISBN and DOI codes into BibTeX entries. It queries public
bibliographic web services, appends the entries to a `.bib` file (optional),
keeps a log, and copies everything to the clipboard so you can paste it into
your document.

Target platform: **Ubuntu 24.04 LTS or later** (Linux only).

## 1. Requirements

| Purpose | Package (apt) | Notes |
|---|---|---|
| C++23 compiler | `build-essential` | g++ 13 (Ubuntu 24.04) or newer |
| `make` | `build-essential` | |
| Locating libcurl | `pkg-config` | |
| HTTP client library | `libcurl4-openssl-dev` | build; the runtime library is `libcurl4t64`, installed by default |
| JSON parser (header-only) | `nlohmann-json3-dev` | |
| Clipboard (runtime) | `wl-clipboard` (Wayland) **or** `xclip` / `xsel` (X11) | needed only for the clipboard copy; without one the BibTeX is printed to the terminal instead |

Install everything in one go:

```bash
sudo apt update
sudo apt install build-essential pkg-config libcurl4-openssl-dev nlohmann-json3-dev xclip wl-clipboard
```

An internet connection is required at run time.

## 2. Building, installing, uninstalling

```bash
make                 # compile -> build/isbn2bibtex
sudo make install    # install to /usr/local/bin/isbn2bibtex
sudo make uninstall  # remove it
make clean           # delete build/
```

All Makefile capabilities:

| Target | What it does |
|---|---|
| `make` / `make all` | Builds `build/isbn2bibtex` (objects, dependency files and binary all stay in `build/`). Default target. |
| `make strip` | Strips the `build/isbn2bibtex` binary. |
| `make clean` | Deletes the `build/` directory. |
| `make install` | Builds if needed, then installs the executable (mode 0755) into `$(BINDIR)`, by default `/usr/local/bin`, the standard place on Ubuntu for locally built software. Usually needs `sudo`. |
| `make uninstall` | Removes `$(BINDIR)/isbn2bibtex`. Usually needs `sudo`. |
| `make help` | Lists targets and variables. |

Variables you can override on the command line:

| Variable | Default | Meaning |
|---|---|---|
| `PREFIX` | `/usr/local` | Installation prefix; the executable goes to `PREFIX/bin`. |
| `BINDIR` | `$(PREFIX)/bin` | Executable directory (overrides `PREFIX`). |
| `DESTDIR` | *(empty)* | Staging root for packaging, e.g. `DESTDIR=/tmp/stage`. |
| `CXX` | `g++` | C++ compiler (must support `-std=c++23`). |
| `CXXFLAGS` | `-O2 -g0` | Extra compiler flags. |

Install without root, into your home directory:

```bash
make install PREFIX=$HOME/.local     # make sure ~/.local/bin is in your PATH
make uninstall PREFIX=$HOME/.local
```

## 3. What it does and how

1. **Parses the command line** (see below). Exactly one of `<isbn-or-doi>` and
   `--read-from-file` must be given.
2. **Normalises each code.**
   * Accepted forms: bare ISBN (hyphens/spaces allowed), `ISBN:`, `ISBN-10:`,
     `ISBN-13:` prefixes, bare DOI (`10.xxxx/…`), `doi:` and
     `https://doi.org/` prefixes. Surrounding single or double quotes are removed.
   * ISBN-10 and ISBN-13 **check digits are validated**; a wrong check digit is
     reported as a failure (with the expected digit). ISBN-10 is **converted to
     ISBN-13**, which is the form used in queries, logs and the `isbn` field.
3. **Checks the internet connection** before doing any work; if none of the
   service hosts answers, it logs/prints an error and exits with status 3.
4. **Fetches the data**, trying each service in turn and stopping at the first
   one that returns a usable record:
   * ISBN: Open Library (`openlibrary.org/api/books`, gives the place of
     publication), then Google Books (`googleapis.com/books/v1/volumes`).
   * DOI: `doi.org` content negotiation (CSL JSON), then the Crossref REST API.
5. **Retries** a request only for HTTP **408** and **429**, up to `--retry`
   times, waiting `--delay` seconds between attempts. Every other failure is
   final for that service.
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
isbn2bibtex [ --help ] [ --version ] [ --log <log-file> ]
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
isbn2bibtex 978-0-13-110362-7

# ISBN-10 (converted to ISBN-13) with a prefix, saved to refs.bib
isbn2bibtex "ISBN-10: 0-13-110362-8" --save-to-file refs.bib

# A DOI, in any of the accepted forms
isbn2bibtex doi:10.1038/nphys1170
isbn2bibtex https://doi.org/10.1038/nphys1170

# Retry up to 5 times, 2 s apart, on HTTP 408/429
isbn2bibtex 978-0-306-40615-7 --retry 5 --delay 2

# A list of codes, custom log, entries appended to refs.bib (created if missing)
isbn2bibtex --read-from-file codes.txt --save-to-file refs --log run.log

# Paste-ready result: the clipboard now holds all entries
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
