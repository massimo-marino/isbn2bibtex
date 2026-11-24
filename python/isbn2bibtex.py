#!/usr/bin/env python3
"""isbn2bibtex - turn ISBN / DOI codes into BibTeX entries.

Python implementation (standard library only, Python >= 3.10).
See README-py.md for details.
"""

from __future__ import annotations

import argparse
import html
import http
import json
import math
import os
import re
import subprocess
import sys
import time
import unicodedata
import urllib.parse
import urllib.request
from dataclasses import dataclass, field
from urllib.error import HTTPError, URLError

PROG = "isbn2bibtex"
VERSION = "1.0.0"
USER_AGENT = f"{PROG}/{VERSION} (BibTeX generator; +https://github.com/)"
HTTP_TIMEOUT = 20  # seconds, per request
RETRYABLE_STATUS = (408, 429)
OVERRIDE_ENV = "ISBN2BIBTEX_ENDPOINT_OVERRIDE"  # testing hook, see README

EXIT_OK = 0
EXIT_SOME_FAILED = 1
EXIT_USAGE = 2
EXIT_NO_INTERNET = 3

HELP_TEXT = f"""\
{PROG} {VERSION} - generate BibTeX entries from ISBN or DOI codes

USAGE
  {PROG} [ --help ] [ --version ] [ --log <log-file> ]
      {{ <isbn-or-doi> | --read-from-file <isbn-or-doi-file-name> }}
      [ --save-to-file <bibtex-file-name> ]
      [ --retry <max-retries> [ --delay <seconds> ] ]

ARGUMENTS
  <isbn-or-doi>
      A single ISBN-10, ISBN-13 or DOI, optionally in single or double quotes.
      Accepted prefixes: "ISBN-13:", "ISBN-10:", "ISBN:", "doi:",
      "https://doi.org/". ISBN check digits are validated and ISBN-10 codes
      are converted to ISBN-13.
  --read-from-file <isbn-or-doi-file-name>
      Read the codes from a text file, one per line. Blank lines, lines that
      start with '#' and any text after a '#' are ignored. No quotes needed.
      Exactly one of <isbn-or-doi> and --read-from-file must be given.
  --save-to-file <bibtex-file-name>
      Append every generated BibTeX entry to this file (created if missing).
      The extension '.bib' is added automatically (with a warning) if absent.
  --log <log-file>
      Log file, created in / appended to in the working directory. The
      extension '.log' is added automatically (with a warning) if absent.
      Default: isbn2bibtex.log, or <isbn-or-doi-file-name> with extension
      '.log' when --read-from-file is used.
  --retry <max-retries>
      Maximum number of retries of a request that failed with HTTP 408 or
      429 (no other error is retried). Default: 0.
  --delay <seconds>
      Seconds (real number) to wait before each retry. Only used together
      with --retry. Default: 0.5.
  --help       Print this help text and exit.
  --version    Print the version and exit.

All generated BibTeX entries are also copied to the clipboard (wl-copy,
xclip or xsel is needed on Linux). An internet connection is required.

EXIT STATUS
  0 every code produced a BibTeX entry      1 at least one code failed
  2 usage error                             3 no internet connection

EXAMPLES
  {PROG} 978-0-13-110362-7
  {PROG} "ISBN-13: 978-0-306-40615-7" --save-to-file refs
  {PROG} doi:10.1038/nphys1170 --retry 3 --delay 2
  {PROG} --read-from-file codes.txt --save-to-file refs.bib --log run.log
"""


# --------------------------------------------------------------------------
# Errors
# --------------------------------------------------------------------------
class UsageError(Exception):
    pass


class CodeError(Exception):
    """The ISBN/DOI string is not valid."""


class TransportError(Exception):
    """No HTTP response at all (DNS, TLS, timeout, ...)."""


class FetchError(Exception):
    def __init__(self, reason: str, retries_done: int):
        super().__init__(reason)
        self.reason = reason
        self.retries_done = retries_done


class RecordError(Exception):
    """The server answered, but the answer holds no usable record."""


# --------------------------------------------------------------------------
# ISBN / DOI parsing
# --------------------------------------------------------------------------
@dataclass(frozen=True)
class Code:
    kind: str  # "isbn" or "doi"
    value: str  # canonical ISBN-13 digits, or bare DOI


_DOI_PREFIX = re.compile(r"^(?:doi\s*:\s*|https?://(?:dx\.)?doi\.org/)", re.I)
_ISBN_PREFIX = re.compile(r"^isbn(?:[-\s]?(10|13))?\s*[:=]?\s*", re.I)
_BARE_DOI = re.compile(r"^10\.\d{4,9}/\S+$")


def strip_quotes(text: str) -> str:
    text = text.strip()
    while len(text) >= 2 and text[0] == text[-1] and text[0] in "'\"":
        text = text[1:-1].strip()
    return text


def isbn10_check_digit(first9: str) -> str:
    total = sum((10 - i) * int(d) for i, d in enumerate(first9))
    value = (11 - total % 11) % 11
    return "X" if value == 10 else str(value)


def isbn13_check_digit(first12: str) -> str:
    total = sum(int(d) * (3 if i % 2 else 1) for i, d in enumerate(first12))
    return str((10 - total % 10) % 10)


def parse_code(raw: str) -> Code:
    """Return the canonical Code for `raw`, or raise CodeError."""
    text = strip_quotes(raw)
    if not text:
        raise CodeError("empty ISBN/DOI")

    if _DOI_PREFIX.match(text):
        doi = urllib.parse.unquote(_DOI_PREFIX.sub("", text, count=1)).strip()
        if not _BARE_DOI.match(doi):
            raise CodeError(f"malformed DOI '{doi}' (expected 10.<registrant>/<suffix>)")
        return Code("doi", doi)

    declared = None
    m = _ISBN_PREFIX.match(text)
    if m:
        declared = m.group(1)
        text = text[m.end():]

    if not m and _BARE_DOI.match(text):
        return Code("doi", text)

    digits = re.sub(r"[\s-]", "", text)
    if re.fullmatch(r"\d{9}[\dXx]", digits):
        if declared == "13":
            raise CodeError("declared ISBN-13 but the code has 10 characters")
        digits = digits.upper()
        expected = isbn10_check_digit(digits[:9])
        if digits[9] != expected:
            raise CodeError(
                f"invalid ISBN-10 check digit '{digits[9]}' (expected '{expected}')")
        core = "978" + digits[:9]
        return Code("isbn", core + isbn13_check_digit(core))
    if re.fullmatch(r"\d{13}", digits):
        if declared == "10":
            raise CodeError("declared ISBN-10 but the code has 13 digits")
        if digits[:3] not in ("978", "979"):
            raise CodeError("invalid ISBN-13 prefix (must be 978 or 979)")
        expected = isbn13_check_digit(digits[:12])
        if digits[12] != expected:
            raise CodeError(
                f"invalid ISBN-13 check digit '{digits[12]}' (expected '{expected}')")
        return Code("isbn", digits)
    raise CodeError("not a valid ISBN-10, ISBN-13 or DOI")


# --------------------------------------------------------------------------
# Records and BibTeX rendering
# --------------------------------------------------------------------------
@dataclass
class Record:
    entry_type: str = "book"
    authors: list[str] = field(default_factory=list)  # BibTeX-ready names
    key_name: str = ""  # surname used for the cite key
    title: str = ""
    publisher: str = ""
    address: str = ""
    year: str = ""
    url: str = ""
    isbn: str = ""
    doi: str = ""
    issn: str = ""
    container: str = ""  # journal or book title
    volume: str = ""
    number: str = ""
    pages: str = ""


_TAG = re.compile(r"<[^>]+>")
_SPECIAL_LETTERS = {
    "ß": "ss", "æ": "ae", "Æ": "AE", "œ": "oe", "Œ": "OE", "ø": "o", "Ø": "O",
    "ł": "l", "Ł": "L", "đ": "d", "Đ": "D", "þ": "th", "Þ": "Th", "ð": "d",
    "Ð": "D", "ı": "i",
}


def clean_text(value: object) -> str:
    """Plain text from a JSON value: strip markup/entities, squeeze spaces."""
    if value is None:
        return ""
    text = html.unescape(_TAG.sub("", str(value)))
    return re.sub(r"\s+", " ", text).strip()


def ascii_fold(text: str) -> str:
    out = []
    for ch in text:
        out.append(_SPECIAL_LETTERS.get(ch) or unicodedata.normalize("NFKD", ch))
    return "".join(c for c in "".join(out) if c.isascii())


_BIB_ESCAPES = {
    "\\": r"\textbackslash{}", "&": r"\&", "%": r"\%", "$": r"\$", "#": r"\#",
    "_": r"\_", "~": r"\textasciitilde{}", "^": r"\textasciicircum{}",
}


def bib_escape(text: str) -> str:
    text = text.replace("{", "").replace("}", "")
    return "".join(_BIB_ESCAPES.get(c, c) for c in text)


def bib_author(name: str) -> str:
    """Escape a name; a name wrapped in braces (corporate author) stays wrapped."""
    if len(name) > 1 and name[0] == "{" and name[-1] == "}":
        return "{" + bib_escape(name[1:-1]) + "}"
    return bib_escape(name)


def first_year(text: object) -> str:
    m = re.search(r"\b(\d{4})\b", str(text or ""))
    return m.group(1) if m else ""


_STOP_WORDS = {"a", "an", "the", "on", "of", "in", "and", "for", "to"}


def cite_key(rec: Record) -> str:
    surname = re.sub(r"[^a-z]", "", ascii_fold(rec.key_name).lower()) or "anon"
    word = ""
    for w in re.findall(r"[A-Za-z0-9]+", ascii_fold(rec.title)):
        if len(w) > 1 and w.lower() not in _STOP_WORDS:
            word = w.lower()
            break
    return f"{surname}{rec.year}{word}"


def render_bibtex(rec: Record, key: str) -> str:
    fields: list[tuple[str, str, bool]] = []  # (name, value, always_present)
    fields.append(("author", " and ".join(bib_author(a) for a in rec.authors), True))
    fields.append(("title", bib_escape(rec.title), True))
    if rec.container:
        name = {"article": "journal", "incollection": "booktitle",
                "inproceedings": "booktitle"}.get(rec.entry_type, "howpublished")
        fields.append((name, bib_escape(rec.container), False))
    fields.append(("publisher", bib_escape(rec.publisher), True))
    fields.append(("address", bib_escape(rec.address), True))
    fields.append(("year", rec.year, True))
    fields.append(("volume", bib_escape(rec.volume), False))
    fields.append(("number", bib_escape(rec.number), False))
    fields.append(("pages", re.sub(r"\s*[-\u2013\u2014]+\s*", "--", rec.pages), False))
    fields.append(("url", rec.url, True))
    fields.append(("isbn", rec.isbn, True))
    fields.append(("issn", rec.issn, False))
    fields.append(("doi", rec.doi, False))
    lines = [f"@{rec.entry_type}{{{key},"]
    for name, value, always in fields:
        if value or always:
            lines.append(f"  {name.ljust(9)} = {{{value}}},")
    lines.append("}")
    return "\n".join(lines)


# --------------------------------------------------------------------------
# Response parsers: one per web service
# --------------------------------------------------------------------------
def load_json(body: bytes) -> object:
    try:
        return json.loads(body.decode("utf-8-sig"))
    except (ValueError, UnicodeDecodeError):
        raise RecordError("response is not valid JSON")


def _names(items: object, key: str = "name") -> list[str]:
    out = []
    for it in items or []:
        name = clean_text(it.get(key) if isinstance(it, dict) else it)
        if name:
            out.append(name)
    return out


def _title_with_subtitle(title: object, subtitle: object) -> str:
    t, s = clean_text(title), clean_text(subtitle)
    return f"{t}: {s}" if t and s else t


def parse_openlibrary(body: bytes, isbn13: str) -> Record:
    data = load_json(body)
    entry = data.get(f"ISBN:{isbn13}") if isinstance(data, dict) else None
    if not entry:
        raise RecordError("no record found")
    title = _title_with_subtitle(entry.get("title"), entry.get("subtitle"))
    if not title:
        raise RecordError("record has no title")
    authors = _names(entry.get("authors"))
    return Record(
        entry_type="book", authors=authors,
        key_name=authors[0].split()[-1] if authors else "",
        title=title,
        publisher=", ".join(_names(entry.get("publishers"))),
        address=", ".join(_names(entry.get("publish_places"))),
        year=first_year(entry.get("publish_date")),
        url=clean_text(entry.get("url")), isbn=isbn13)


def parse_googlebooks(body: bytes, isbn13: str) -> Record:
    data = load_json(body)
    items = data.get("items") if isinstance(data, dict) else None
    if not items:
        raise RecordError("no record found")
    item = items[0]
    info = item.get("volumeInfo") or {}
    title = _title_with_subtitle(info.get("title"), info.get("subtitle"))
    if not title:
        raise RecordError("record has no title")
    authors = _names(info.get("authors"), "")
    return Record(
        entry_type="book", authors=authors,
        key_name=authors[0].split()[-1] if authors else "",
        title=title, publisher=clean_text(info.get("publisher")),
        address="",  # Google Books does not provide the place of publication
        year=first_year(info.get("publishedDate")),
        url=clean_text(info.get("canonicalVolumeLink") or info.get("infoLink")
                       or item.get("selfLink")),
        isbn=isbn13)


_CSL_TYPES = {
    "book": "book", "monograph": "book", "edited-book": "book",
    "reference-book": "book", "article-journal": "article",
    "journal-article": "article", "chapter": "incollection",
    "book-chapter": "incollection", "paper-conference": "inproceedings",
    "proceedings-article": "inproceedings", "dissertation": "phdthesis",
}


def _first(value: object) -> object:
    return value[0] if isinstance(value, list) and value else (
        "" if isinstance(value, list) else value)


def record_from_csl(csl: dict, doi: str) -> Record:
    """Build a Record from CSL-JSON (doi.org) or a Crossref `message`."""
    title = _title_with_subtitle(_first(csl.get("title")), _first(csl.get("subtitle")))
    if not title:
        raise RecordError("record has no title")
    authors: list[str] = []
    key_name = ""
    for a in csl.get("author") or []:
        if not isinstance(a, dict):
            continue
        family, given = clean_text(a.get("family")), clean_text(a.get("given"))
        literal = clean_text(a.get("literal") or a.get("name"))
        if family and given:
            authors.append(f"{family}, {given}")
        elif family or given:
            authors.append(family or given)
        elif literal:
            authors.append(f"{{{literal}}}")
        else:
            continue
        key_name = key_name or family or given or literal
    issued = csl.get("issued") or csl.get("published") or {}
    parts = issued.get("date-parts") if isinstance(issued, dict) else None
    year = ""
    if parts and parts[0] and parts[0][0]:
        year = str(parts[0][0])
    isbns = csl.get("ISBN") or []
    issns = csl.get("ISSN") or []
    isbn = re.sub(r"[^0-9Xx]", "", str(_first(isbns) or "")).upper() if isbns else ""
    return Record(
        entry_type=_CSL_TYPES.get(str(csl.get("type", "")), "misc"),
        authors=authors, key_name=key_name, title=title,
        publisher=clean_text(csl.get("publisher")),
        address=clean_text(csl.get("publisher-place") or csl.get("publisher-location")),
        year=year, url=f"https://doi.org/{doi}", isbn=isbn, doi=doi,
        issn=clean_text(_first(issns)),
        container=clean_text(_first(csl.get("container-title"))),
        volume=clean_text(csl.get("volume")),
        number=clean_text(csl.get("issue")),
        pages=clean_text(csl.get("page")))


def parse_csl_json(body: bytes, doi: str) -> Record:
    data = load_json(body)
    if not isinstance(data, dict):
        raise RecordError("unexpected JSON structure")
    return record_from_csl(data, doi)


def parse_crossref(body: bytes, doi: str) -> Record:
    data = load_json(body)
    message = data.get("message") if isinstance(data, dict) else None
    if not isinstance(message, dict):
        raise RecordError("unexpected JSON structure")
    return record_from_csl(message, doi)


# --------------------------------------------------------------------------
# HTTP
# --------------------------------------------------------------------------
def apply_override(url: str) -> str:
    override = os.environ.get(OVERRIDE_ENV)
    if not override:
        return url
    base = urllib.parse.urlsplit(override)
    u = urllib.parse.urlsplit(url)
    return urllib.parse.urlunsplit((base.scheme, base.netloc, u.path, u.query, ""))


def status_text(code: int) -> str:
    try:
        return http.HTTPStatus(code).phrase
    except ValueError:
        return "Unknown Status"


def http_get(url: str, accept: str, method: str = "GET") -> tuple[int, bytes]:
    req = urllib.request.Request(
        url, method=method, headers={"User-Agent": USER_AGENT, "Accept": accept})
    try:
        with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT) as resp:
            return resp.status, resp.read()
    except HTTPError as err:
        try:
            return err.code, err.read()
        finally:
            err.close()
    except (URLError, OSError, ValueError) as err:
        raise TransportError(str(getattr(err, "reason", err)) or type(err).__name__)


def internet_available() -> bool:
    hosts = ["https://openlibrary.org/", "https://doi.org/",
             "https://www.googleapis.com/", "https://api.crossref.org/"]
    for url in hosts:
        try:
            http_get(apply_override(url), "*/*", method="HEAD")
            return True  # any HTTP answer proves connectivity
        except TransportError:
            continue
    return False


@dataclass
class Source:
    url: str
    accept: str
    label: str  # the request as it is logged
    parse: object  # callable(body, value) -> Record


def sources_for(code: Code) -> list[Source]:
    json_accept = "application/json"
    if code.kind == "isbn":
        ol = apply_override(
            f"https://openlibrary.org/api/books?bibkeys=ISBN:{code.value}"
            "&format=json&jscmd=data")
        gb = apply_override(
            f"https://www.googleapis.com/books/v1/volumes?q=isbn:{code.value}")
        return [
            Source(ol, json_accept, f"GET {ol}", parse_openlibrary),
            Source(gb, json_accept, f"GET {gb}", parse_googlebooks),
        ]
    quoted = urllib.parse.quote(code.value, safe="/()<>;:-._~")
    csl = "application/vnd.citationstyles.csl+json"
    dx = apply_override(f"https://doi.org/{quoted}")
    cr = apply_override(f"https://api.crossref.org/works/{quoted}")
    return [
        Source(dx, csl, f"GET {dx} [Accept: {csl}]", parse_csl_json),
        Source(cr, json_accept, f"GET {cr}", parse_crossref),
    ]


def fetch(src: Source, max_retries: int, delay: float, on_retry) -> tuple[bytes, int]:
    """GET src.url; retry on HTTP 408/429. Returns (body, retries_used)."""
    retries = 0
    while True:
        try:
            status, body = http_get(src.url, src.accept)
        except TransportError as err:
            raise FetchError(f"network error: {err}", retries)
        if 200 <= status < 300:
            return body, retries
        reason = f"HTTP {status} {status_text(status)}"
        if status in RETRYABLE_STATUS and retries < max_retries:
            retries += 1
            on_retry(src, retries, reason)
            time.sleep(delay)
            continue
        raise FetchError(reason, retries)


def resolve(code: Code, max_retries: int, delay: float, on_retry) -> tuple[Record, str]:
    """Try every source in turn; return (record, logged request)."""
    failures = []
    for src in sources_for(code):
        try:
            body, retries = fetch(src, max_retries, delay, on_retry)
        except FetchError as err:
            note = ""
            if err.retries_done:
                note = f" (after {err.retries_done} retr{'y' if err.retries_done == 1 else 'ies'})"
            failures.append(f"{src.label} -> {err.reason}{note}")
            continue
        try:
            rec = src.parse(body, code.value)
        except RecordError as err:
            failures.append(f"{src.label} -> {err}")
            continue
        label = src.label
        if retries:
            label += f" (succeeded on retry attempt {retries} of {max_retries})"
        return rec, label
    raise FetchError("; ".join(failures), 0)


# --------------------------------------------------------------------------
# Clipboard
# --------------------------------------------------------------------------
def copy_to_clipboard(text: str) -> bool:
    candidates = [["wl-copy"], ["xclip", "-selection", "clipboard"],
                  ["xsel", "--clipboard", "--input"]]
    if not os.environ.get("WAYLAND_DISPLAY"):
        candidates = candidates[1:] + candidates[:1]
    for cmd in candidates:
        try:
            done = subprocess.run(cmd, input=text.encode("utf-8"),
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                  timeout=5)
        except (OSError, subprocess.SubprocessError):
            continue
        if done.returncode == 0:
            return True
    return False


# --------------------------------------------------------------------------
# Command line
# --------------------------------------------------------------------------
class Reporter:
    """Writes a line to the log file and to the terminal."""

    def __init__(self, log_path: str):
        self.file = open(log_path, "a", encoding="utf-8")

    def line(self, text: str, error: bool = False) -> None:
        self.file.write(text + "\n")
        self.file.flush()
        print(text, file=sys.stderr if error else sys.stdout, flush=True)

    def close(self) -> None:
        self.file.close()


def warn(text: str) -> None:
    print(f"WARNING: {text}", file=sys.stderr, flush=True)


def ensure_extension(name: str, ext: str, what: str) -> str:
    if name.lower().endswith(ext):
        return name
    new = name + ext
    warn(f"{what} '{name}' does not have extension '{ext}'; using '{new}' instead")
    return new


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog=PROG, add_help=False, allow_abbrev=False)
    p.add_argument("code", nargs="?")
    p.add_argument("--read-from-file", dest="input_file")
    p.add_argument("--save-to-file", dest="bib_file")
    p.add_argument("--log", dest="log_file")
    p.add_argument("--retry", type=int)
    p.add_argument("--delay", type=float)
    p.add_argument("--help", action="store_true")
    p.add_argument("--version", action="store_true")
    return p


def read_code_file(path: str) -> list[str]:
    try:
        with open(path, encoding="utf-8-sig") as fh:
            lines = fh.read().splitlines()
    except (OSError, UnicodeDecodeError) as err:
        raise UsageError(f"cannot read '{path}': {err}")
    codes = []
    for line in lines:
        line = line.split("#", 1)[0].strip()
        if line:
            codes.append(line)
    return codes


def same_file(a: str, b: str) -> bool:
    return os.path.normcase(os.path.realpath(a)) == os.path.normcase(os.path.realpath(b))


def run(argv: list[str]) -> int:
    parser = build_parser()
    try:
        args = parser.parse_args(argv)
    except SystemExit as exc:  # argparse already printed its message
        return EXIT_USAGE if exc.code else EXIT_OK
    if args.help:
        print(HELP_TEXT)
        return EXIT_OK
    if args.version:
        print(f"{PROG} {VERSION}")
        return EXIT_OK

    # ---- usage validation -------------------------------------------------
    if args.code is not None and args.input_file is not None:
        raise UsageError("give either <isbn-or-doi> or --read-from-file, not both")
    if args.code is None and args.input_file is None:
        raise UsageError("missing <isbn-or-doi> or --read-from-file (try --help)")
    if args.retry is not None and args.retry < 0:
        raise UsageError("--retry must be an integer >= 0")
    if args.delay is not None and (not math.isfinite(args.delay) or args.delay < 0):
        raise UsageError("--delay must be a real number >= 0")
    max_retries = args.retry if args.retry is not None else 0
    if args.delay is not None and args.retry is None:
        warn("--delay is ignored because --retry was not provided")
    delay = args.delay if args.delay is not None else 0.5

    # ---- file names ---------------------------------------------------------
    if args.log_file:
        log_path = ensure_extension(args.log_file, ".log", "log file")
    elif args.input_file:
        stem = os.path.splitext(os.path.basename(args.input_file))[0] or PROG
        log_path = stem + ".log"
    else:
        log_path = PROG + ".log"
    bib_path = (ensure_extension(args.bib_file, ".bib", "BibTeX file")
                if args.bib_file else None)
    named = [("log", log_path)]
    if bib_path:
        named.append((".bib", bib_path))
    if args.input_file:
        named.append(("input", args.input_file))
    for i in range(len(named)):
        for j in range(i + 1, len(named)):
            if same_file(named[i][1], named[j][1]):
                raise UsageError(
                    f"the {named[i][0]} file and the {named[j][0]} file must be "
                    f"different files (both are '{named[i][1]}')")

    raw_codes = read_code_file(args.input_file) if args.input_file else [args.code]
    if not raw_codes:
        raise UsageError(f"no ISBN/DOI codes found in '{args.input_file}'")

    try:
        reporter = Reporter(log_path)
        bib_fh = open(bib_path, "a", encoding="utf-8") if bib_path else None
    except OSError as err:
        raise UsageError(f"cannot open output file: {err}")

    # ---- internet ---------------------------------------------------------------
    if not internet_available():
        reporter.line("ERROR: no internet connection available; "
                      "cannot fetch bibliographic data", error=True)
        reporter.close()
        if bib_fh:
            bib_fh.close()
        return EXIT_NO_INTERNET

    # ---- main loop ----------------------------------------------------------------
    entries: list[str] = []
    used_keys: set[str] = set()
    failed = 0
    for raw in raw_codes:
        shown = strip_quotes(raw)
        try:
            code = parse_code(raw)
        except CodeError as err:
            reporter.line(f"{shown}: FAILED: {err}", error=True)
            failed += 1
            continue
        shown = code.value

        def on_retry(src: Source, attempt: int, reason: str, _c=shown) -> None:
            reporter.line(f"{_c}: retry attempt {attempt} of {max_retries} "
                          f"after {reason}, waiting {delay:g}s: {src.label}")

        try:
            record, request = resolve(code, max_retries, delay, on_retry)
        except FetchError as err:
            reporter.line(f"{shown}: FAILED: {err.reason}", error=True)
            failed += 1
            continue

        key = base = cite_key(record)
        n = 1
        while key in used_keys:  # keep cite keys unique within one run
            n += 1
            key = f"{base}{n}"
        used_keys.add(key)
        bibtex = render_bibtex(record, key)
        entries.append(bibtex)
        if bib_fh:
            bib_fh.write(bibtex + "\n\n")
            bib_fh.flush()
        reporter.line(f"{shown}: BibTeX generated with request: {request}")

    # ---- clipboard ----------------------------------------------------------------
    if entries:
        text = "\n\n".join(entries) + "\n"
        if copy_to_clipboard(text):
            print(f"{len(entries)} BibTeX entr{'y' if len(entries) == 1 else 'ies'} "
                  "copied to the clipboard")
        else:
            warn("could not copy to the clipboard (install wl-copy, xclip or xsel); "
                 "the BibTeX code follows:")
            print(text)
    if bib_fh:
        bib_fh.close()
    reporter.close()
    return EXIT_SOME_FAILED if failed else EXIT_OK


def main() -> None:
    try:
        sys.exit(run(sys.argv[1:]))
    except UsageError as err:
        print(f"{PROG}: error: {err}", file=sys.stderr)
        sys.exit(EXIT_USAGE)
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
        sys.exit(130)


if __name__ == "__main__":
    main()
