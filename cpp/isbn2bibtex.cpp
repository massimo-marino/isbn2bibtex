// isbn2bibtex - turn ISBN / DOI codes into BibTeX entries.
//
// C++23 implementation for Ubuntu 24.04+ (g++ >= 13, libcurl, nlohmann-json).
// See README-cpp.md for details.

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <signal.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace std::string_literals;

constexpr std::string_view kProg = "isbn2bibtex";
constexpr std::string_view kVersion = "1.0.0";
constexpr long kHttpTimeoutSeconds = 20;
constexpr const char* kOverrideEnv = "ISBN2BIBTEX_ENDPOINT_OVERRIDE";  // testing hook

constexpr int kExitOk = 0;
constexpr int kExitSomeFailed = 1;
constexpr int kExitUsage = 2;
constexpr int kExitNoInternet = 3;

const std::string kHelpText = R"(isbn2bibtex @VERSION@ - generate BibTeX entries from ISBN or DOI codes

USAGE
  isbn2bibtex [ --help ] [ --version ] [ --log <log-file> ]
      { <isbn-or-doi> | --read-from-file <isbn-or-doi-file-name> }
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
  isbn2bibtex 978-0-13-110362-7
  isbn2bibtex "ISBN-13: 978-0-306-40615-7" --save-to-file refs
  isbn2bibtex doi:10.1038/nphys1170 --retry 3 --delay 2
  isbn2bibtex --read-from-file codes.txt --save-to-file refs.bib --log run.log
)";

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------
struct UsageError : std::runtime_error { using std::runtime_error::runtime_error; };
struct CodeError : std::runtime_error { using std::runtime_error::runtime_error; };
struct TransportError : std::runtime_error { using std::runtime_error::runtime_error; };
struct RecordError : std::runtime_error { using std::runtime_error::runtime_error; };
struct FetchError : std::runtime_error {
    FetchError(const std::string& reason, int retries) : std::runtime_error(reason), retries_done(retries) {}
    int retries_done;
};

// ---------------------------------------------------------------------------
// String helpers
// ---------------------------------------------------------------------------
std::string trim(std::string_view s) {
    constexpr std::string_view ws = " \t\r\n\f\v";
    const auto b = s.find_first_not_of(ws);
    if (b == std::string_view::npos) return {};
    const auto e = s.find_last_not_of(ws);
    return std::string(s.substr(b, e - b + 1));
}

std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ends_with_icase(const std::string& s, std::string_view suffix) {
    return s.size() >= suffix.size() && to_lower(s.substr(s.size() - suffix.size())) == suffix;
}

std::string replace_all(std::string s, std::string_view from, std::string_view to) {
    for (std::size_t pos = 0; (pos = s.find(from, pos)) != std::string::npos; pos += to.size())
        s.replace(pos, from.size(), to);
    return s;
}

std::string strip_quotes(std::string_view in) {
    std::string t = trim(in);
    while (t.size() >= 2 && t.front() == t.back() && (t.front() == '\'' || t.front() == '"'))
        t = trim(std::string_view(t).substr(1, t.size() - 2));
    return t;
}

void append_utf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

std::string percent_decode(std::string_view s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(std::stoi(std::string(s.substr(i + 1, 2)), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

// Percent-encode everything except unreserved characters and "/()<>;:".
std::string percent_encode(std::string_view s) {
    static constexpr std::string_view safe = "/()<>;:-._~";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || safe.find(static_cast<char>(c)) != std::string_view::npos)
            out += static_cast<char>(c);
        else
            out += std::format("%{:02X}", c);
    }
    return out;
}

// ---------------------------------------------------------------------------
// ISBN / DOI parsing
// ---------------------------------------------------------------------------
struct Code {
    enum class Kind { Isbn, Doi } kind;
    std::string value;  // canonical ISBN-13 digits, or bare DOI
};

char isbn10_check_digit(std::string_view first9) {
    int total = 0;
    for (int i = 0; i < 9; ++i) total += (10 - i) * (first9[i] - '0');
    const int v = (11 - total % 11) % 11;
    return v == 10 ? 'X' : static_cast<char>('0' + v);
}

char isbn13_check_digit(std::string_view first12) {
    int total = 0;
    for (int i = 0; i < 12; ++i) total += (first12[i] - '0') * (i % 2 ? 3 : 1);
    return static_cast<char>('0' + (10 - total % 10) % 10);
}

Code parse_code(const std::string& raw) {
    static const std::regex doi_prefix(R"(^(?:doi\s*:\s*|https?://(?:dx\.)?doi\.org/))", std::regex::icase);
    static const std::regex isbn_prefix(R"(^isbn(?:[-\s]?(10|13))?\s*[:=]?\s*)", std::regex::icase);
    static const std::regex bare_doi(R"(^10\.\d{4,9}/\S+$)");
    static const std::regex isbn10_re(R"(\d{9}[\dXx])");
    static const std::regex isbn13_re(R"(\d{13})");

    std::string text = strip_quotes(raw);
    if (text.empty()) throw CodeError("empty ISBN/DOI");

    std::smatch m;
    if (std::regex_search(text, m, doi_prefix)) {
        const std::string doi = trim(percent_decode(text.substr(m.length(0))));
        if (!std::regex_match(doi, bare_doi))
            throw CodeError(std::format("malformed DOI '{}' (expected 10.<registrant>/<suffix>)", doi));
        return {Code::Kind::Doi, doi};
    }

    std::string declared;
    const bool had_prefix = std::regex_search(text, m, isbn_prefix);
    if (had_prefix) {
        declared = m[1].str();
        text = text.substr(m.length(0));
    }
    if (!had_prefix && std::regex_match(text, bare_doi)) return {Code::Kind::Doi, text};

    std::string digits;
    for (char c : text)
        if (!std::isspace(static_cast<unsigned char>(c)) && c != '-') digits += c;

    if (std::regex_match(digits, isbn10_re)) {
        if (declared == "13") throw CodeError("declared ISBN-13 but the code has 10 characters");
        digits[9] = static_cast<char>(std::toupper(static_cast<unsigned char>(digits[9])));
        const char expected = isbn10_check_digit(digits);
        if (digits[9] != expected)
            throw CodeError(std::format("invalid ISBN-10 check digit '{}' (expected '{}')", digits[9], expected));
        std::string core = "978" + digits.substr(0, 9);
        core += isbn13_check_digit(core);
        return {Code::Kind::Isbn, core};
    }
    if (std::regex_match(digits, isbn13_re)) {
        if (declared == "10") throw CodeError("declared ISBN-10 but the code has 13 digits");
        if (digits.compare(0, 3, "978") != 0 && digits.compare(0, 3, "979") != 0)
            throw CodeError("invalid ISBN-13 prefix (must be 978 or 979)");
        const char expected = isbn13_check_digit(digits);
        if (digits[12] != expected)
            throw CodeError(std::format("invalid ISBN-13 check digit '{}' (expected '{}')", digits[12], expected));
        return {Code::Kind::Isbn, digits};
    }
    throw CodeError("not a valid ISBN-10, ISBN-13 or DOI");
}

// ---------------------------------------------------------------------------
// Text cleaning, ASCII folding and BibTeX rendering
// ---------------------------------------------------------------------------
// ASCII replacement for U+00C0 .. U+017F (generated from Unicode NFKD).
constexpr std::array<const char*, 192> kFold = {{
    "A", "A", "A", "A", "A", "A", "AE", "C",
    "E", "E", "E", "E", "I", "I", "I", "I",
    "D", "N", "O", "O", "O", "O", "O", "",
    "O", "U", "U", "U", "U", "Y", "Th", "ss",
    "a", "a", "a", "a", "a", "a", "ae", "c",
    "e", "e", "e", "e", "i", "i", "i", "i",
    "d", "n", "o", "o", "o", "o", "o", "",
    "o", "u", "u", "u", "u", "y", "th", "y",
    "A", "a", "A", "a", "A", "a", "C", "c",
    "C", "c", "C", "c", "C", "c", "D", "d",
    "D", "d", "E", "e", "E", "e", "E", "e",
    "E", "e", "E", "e", "G", "g", "G", "g",
    "G", "g", "G", "g", "H", "h", "", "",
    "I", "i", "I", "i", "I", "i", "I", "i",
    "I", "i", "IJ", "ij", "J", "j", "K", "k",
    "", "L", "l", "L", "l", "L", "l", "L",
    "l", "L", "l", "N", "n", "N", "n", "N",
    "n", "n", "", "", "O", "o", "O", "o",
    "O", "o", "OE", "oe", "R", "r", "R", "r",
    "R", "r", "S", "s", "S", "s", "S", "s",
    "S", "s", "T", "t", "T", "t", "", "",
    "U", "u", "U", "u", "U", "u", "U", "u",
    "U", "u", "U", "u", "W", "w", "Y", "y",
    "Y", "Z", "z", "Z", "z", "Z", "z", "s"
}};

std::string ascii_fold(std::string_view s) {
    std::string out;
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { out += static_cast<char>(c); ++i; continue; }
        int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (len == 1 || i + len > s.size()) { ++i; continue; }  // stray byte
        char32_t cp = c & (0xFF >> (len + 1));
        for (int k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        i += len;
        if (cp >= 0xC0 && cp < 0xC0 + kFold.size()) out += kFold[cp - 0xC0];
    }
    return out;
}

std::string html_unescape(const std::string& s) {
    static const std::map<std::string, std::string> named = {
        {"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"}, {"nbsp", " "},
        {"ndash", "\xE2\x80\x93"}, {"mdash", "\xE2\x80\x94"}, {"hellip", "\xE2\x80\xA6"},
        {"rsquo", "\xE2\x80\x99"}, {"lsquo", "\xE2\x80\x98"}, {"ldquo", "\xE2\x80\x9C"},
        {"rdquo", "\xE2\x80\x9D"}};
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '&') {
            const auto semi = s.find(';', i);
            if (semi != std::string::npos && semi - i <= 10) {
                const std::string name = s.substr(i + 1, semi - i - 1);
                if (auto it = named.find(name); it != named.end()) {
                    out += it->second;
                    i = semi;
                    continue;
                }
                if (name.size() > 1 && name[0] == '#') {
                    const bool hex = name[1] == 'x' || name[1] == 'X';
                    unsigned long cp = 0;
                    const char* b = name.data() + (hex ? 2 : 1);
                    const char* e = name.data() + name.size();
                    auto [ptr, ec] = std::from_chars(b, e, cp, hex ? 16 : 10);
                    if (ec == std::errc() && ptr == e && cp > 0 && cp < 0x110000) {
                        append_utf8(out, static_cast<char32_t>(cp));
                        i = semi;
                        continue;
                    }
                }
            }
        }
        out += s[i];
    }
    return out;
}

// Plain text from a JSON value: strip markup/entities, squeeze spaces.
std::string clean_text(const json& v) {
    std::string raw;
    if (v.is_string()) raw = v.get<std::string>();
    else if (v.is_number() || v.is_boolean()) raw = v.dump();
    else return {};
    static const std::regex tag("<[^>]+>");
    raw = html_unescape(std::regex_replace(raw, tag, ""));
    std::string out;
    bool pending_space = false;
    for (char c : raw) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            pending_space = !out.empty();
        } else {
            if (pending_space) out += ' ';
            pending_space = false;
            out += c;
        }
    }
    return out;
}

std::string bib_escape(std::string_view s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '{': case '}': break;  // would unbalance the entry
            case '\\': out += R"(\textbackslash{})"; break;
            case '&': out += R"(\&)"; break;
            case '%': out += R"(\%)"; break;
            case '$': out += R"(\$)"; break;
            case '#': out += R"(\#)"; break;
            case '_': out += R"(\_)"; break;
            case '~': out += R"(\textasciitilde{})"; break;
            case '^': out += R"(\textasciicircum{})"; break;
            default: out += c;
        }
    }
    return out;
}

// A name wrapped in braces (corporate author) stays wrapped.
std::string bib_author(const std::string& name) {
    if (name.size() > 1 && name.front() == '{' && name.back() == '}')
        return "{" + bib_escape(std::string_view(name).substr(1, name.size() - 2)) + "}";
    return bib_escape(name);
}

std::string first_year(const std::string& text) {
    static const std::regex year(R"(\b(\d{4})\b)");
    std::smatch m;
    return std::regex_search(text, m, year) ? m[1].str() : std::string{};
}

struct Record {
    std::string entry_type = "book";
    std::vector<std::string> authors;  // BibTeX-ready names
    std::string key_name;              // surname used for the cite key
    std::string title, publisher, address, year, url, isbn, doi, issn;
    std::string container;             // journal or book title
    std::string volume, number, pages;
};

std::string cite_key(const Record& rec) {
    std::string surname;
    for (char c : to_lower(ascii_fold(rec.key_name)))
        if (c >= 'a' && c <= 'z') surname += c;
    if (surname.empty()) surname = "anon";

    static const std::set<std::string> stop = {"a", "an", "the", "on", "of", "in", "and", "for", "to"};
    std::string word;
    std::string cur;
    const std::string folded = ascii_fold(rec.title) + " ";
    for (char c : folded) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            cur += c;
        } else if (!cur.empty()) {
            const std::string lc = to_lower(cur);
            if (word.empty() && cur.size() > 1 && !stop.contains(lc)) word = lc;
            cur.clear();
        }
    }
    return surname + rec.year + word;
}

std::string render_bibtex(const Record& rec, const std::string& key) {
    struct Field { std::string name, value; bool always; };
    std::vector<Field> fields;

    std::string authors;
    for (const auto& a : rec.authors) authors += (authors.empty() ? "" : " and ") + bib_author(a);
    fields.push_back({"author", authors, true});
    fields.push_back({"title", bib_escape(rec.title), true});
    if (!rec.container.empty()) {
        const char* name = rec.entry_type == "article" ? "journal"
                         : (rec.entry_type == "incollection" || rec.entry_type == "inproceedings") ? "booktitle"
                                                                                                    : "howpublished";
        fields.push_back({name, bib_escape(rec.container), false});
    }
    fields.push_back({"publisher", bib_escape(rec.publisher), true});
    fields.push_back({"address", bib_escape(rec.address), true});
    fields.push_back({"year", rec.year, true});
    fields.push_back({"volume", bib_escape(rec.volume), false});
    fields.push_back({"number", bib_escape(rec.number), false});
    static const std::regex dashes("\\s*(?:-|\xE2\x80\x93|\xE2\x80\x94)+\\s*");
    fields.push_back({"pages", std::regex_replace(rec.pages, dashes, "--"), false});
    fields.push_back({"url", rec.url, true});
    fields.push_back({"isbn", rec.isbn, true});
    fields.push_back({"issn", rec.issn, false});
    fields.push_back({"doi", rec.doi, false});

    std::string out = std::format("@{}{{{},\n", rec.entry_type, key);
    for (const auto& f : fields) {
        if (f.value.empty() && !f.always) continue;
        out += std::format("  {:<9} = {{{}}},\n", f.name, f.value);
    }
    out += "}";
    return out;
}

// ---------------------------------------------------------------------------
// Response parsers: one per web service
// ---------------------------------------------------------------------------
const json& member(const json& j, const char* key) {
    static const json null;
    if (j.is_object()) {
        if (auto it = j.find(key); it != j.end()) return *it;
    }
    return null;
}

bool truthy(const json& j) {
    if (j.is_null()) return false;
    if (j.is_boolean()) return j.get<bool>();
    if (j.is_number()) return j.get<double>() != 0;
    if (j.is_string() || j.is_object() || j.is_array()) return !j.empty();
    return true;
}

const json& first_of(const json& j) {
    static const json null;
    if (j.is_array()) return j.empty() ? null : j.front();
    return j;
}

json load_json(const std::string& body) {
    std::string_view v = body;
    if (v.starts_with("\xEF\xBB\xBF")) v.remove_prefix(3);
    json j = json::parse(v.begin(), v.end(), nullptr, false);
    if (j.is_discarded()) throw RecordError("response is not valid JSON");
    return j;
}

// Names from an array whose items are strings or objects holding `key`.
std::vector<std::string> names(const json& items, const char* key) {
    std::vector<std::string> out;
    if (!items.is_array()) return out;
    for (const auto& it : items) {
        std::string n = it.is_object() ? clean_text(member(it, key)) : clean_text(it);
        if (!n.empty()) out.push_back(std::move(n));
    }
    return out;
}

std::string join(const std::vector<std::string>& v, std::string_view sep) {
    std::string out;
    for (const auto& s : v) out += (out.empty() ? "" : std::string(sep)) + s;
    return out;
}

std::string title_with_subtitle(const json& title, const json& subtitle) {
    const std::string t = clean_text(title), s = clean_text(subtitle);
    return (!t.empty() && !s.empty()) ? t + ": " + s : t;
}

std::string last_word(const std::string& s) {
    const auto p = s.find_last_of(' ');
    return p == std::string::npos ? s : s.substr(p + 1);
}

Record parse_openlibrary(const std::string& body, const std::string& isbn13) {
    const json data = load_json(body);
    const json& entry = member(data, ("ISBN:" + isbn13).c_str());
    if (!truthy(entry)) throw RecordError("no record found");
    Record r;
    r.title = title_with_subtitle(member(entry, "title"), member(entry, "subtitle"));
    if (r.title.empty()) throw RecordError("record has no title");
    r.authors = names(member(entry, "authors"), "name");
    r.key_name = r.authors.empty() ? "" : last_word(r.authors.front());
    r.publisher = join(names(member(entry, "publishers"), "name"), ", ");
    r.address = join(names(member(entry, "publish_places"), "name"), ", ");
    r.year = first_year(clean_text(member(entry, "publish_date")));
    r.url = clean_text(member(entry, "url"));
    r.isbn = isbn13;
    return r;
}

Record parse_googlebooks(const std::string& body, const std::string& isbn13) {
    const json data = load_json(body);
    const json& items = member(data, "items");
    if (!truthy(items) || !items.is_array()) throw RecordError("no record found");
    const json& item = items.front();
    const json& info = member(item, "volumeInfo");
    Record r;
    r.title = title_with_subtitle(member(info, "title"), member(info, "subtitle"));
    if (r.title.empty()) throw RecordError("record has no title");
    r.authors = names(member(info, "authors"), "");
    r.key_name = r.authors.empty() ? "" : last_word(r.authors.front());
    r.publisher = clean_text(member(info, "publisher"));
    // Google Books does not provide the place of publication.
    r.year = first_year(clean_text(member(info, "publishedDate")));
    r.url = clean_text(member(info, "canonicalVolumeLink"));
    if (r.url.empty()) r.url = clean_text(member(info, "infoLink"));
    if (r.url.empty()) r.url = clean_text(member(item, "selfLink"));
    r.isbn = isbn13;
    return r;
}

std::string csl_entry_type(const std::string& type) {
    static const std::map<std::string, std::string> types = {
        {"book", "book"}, {"monograph", "book"}, {"edited-book", "book"}, {"reference-book", "book"},
        {"article-journal", "article"}, {"journal-article", "article"}, {"chapter", "incollection"},
        {"book-chapter", "incollection"}, {"paper-conference", "inproceedings"},
        {"proceedings-article", "inproceedings"}, {"dissertation", "phdthesis"}};
    auto it = types.find(type);
    return it == types.end() ? "misc" : it->second;
}

// Builds a Record from CSL-JSON (doi.org) or from a Crossref `message`.
Record record_from_csl(const json& csl, const std::string& doi) {
    Record r;
    r.title = title_with_subtitle(first_of(member(csl, "title")), first_of(member(csl, "subtitle")));
    if (r.title.empty()) throw RecordError("record has no title");

    const json& author_list = member(csl, "author");
    if (author_list.is_array()) {
        for (const auto& a : author_list) {
            if (!a.is_object()) continue;
            const std::string family = clean_text(member(a, "family")), given = clean_text(member(a, "given"));
            std::string literal = clean_text(member(a, "literal"));
            if (literal.empty()) literal = clean_text(member(a, "name"));
            if (!family.empty() && !given.empty()) r.authors.push_back(family + ", " + given);
            else if (!family.empty() || !given.empty()) r.authors.push_back(family.empty() ? given : family);
            else if (!literal.empty()) r.authors.push_back("{" + literal + "}");
            else continue;
            if (r.key_name.empty()) r.key_name = !family.empty() ? family : !given.empty() ? given : literal;
        }
    }

    json issued = member(csl, "issued");
    if (!truthy(issued)) issued = member(csl, "published");
    const json& parts = member(issued, "date-parts");
    if (parts.is_array() && !parts.empty() && parts.front().is_array() && !parts.front().empty()) {
        const json& y = parts.front().front();
        if (y.is_number_integer() && y.get<long>() != 0) r.year = std::to_string(y.get<long>());
        else if (y.is_string()) r.year = y.get<std::string>();
    }

    r.entry_type = csl_entry_type(clean_text(member(csl, "type")));
    r.publisher = clean_text(member(csl, "publisher"));
    r.address = clean_text(member(csl, "publisher-place"));
    if (r.address.empty()) r.address = clean_text(member(csl, "publisher-location"));
    r.url = "https://doi.org/" + doi;
    r.doi = doi;
    for (char c : clean_text(first_of(member(csl, "ISBN"))))
        if (std::isdigit(static_cast<unsigned char>(c)) || c == 'X' || c == 'x')
            r.isbn += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    r.issn = clean_text(first_of(member(csl, "ISSN")));
    r.container = clean_text(first_of(member(csl, "container-title")));
    r.volume = clean_text(member(csl, "volume"));
    r.number = clean_text(member(csl, "issue"));
    r.pages = clean_text(member(csl, "page"));
    return r;
}

Record parse_csl_json(const std::string& body, const std::string& doi) {
    const json data = load_json(body);
    if (!data.is_object()) throw RecordError("unexpected JSON structure");
    return record_from_csl(data, doi);
}

Record parse_crossref(const std::string& body, const std::string& doi) {
    const json data = load_json(body);
    const json& message = member(data, "message");
    if (!message.is_object()) throw RecordError("unexpected JSON structure");
    return record_from_csl(message, doi);
}

// ---------------------------------------------------------------------------
// HTTP (libcurl)
// ---------------------------------------------------------------------------
struct CurlGlobal {
    CurlGlobal() { curl_global_init(CURL_GLOBAL_DEFAULT); }
    ~CurlGlobal() { curl_global_cleanup(); }
};

struct HttpResponse {
    long status = 0;
    std::string body;
};

std::size_t write_body(char* data, std::size_t size, std::size_t n, void* user) {
    static_cast<std::string*>(user)->append(data, size * n);
    return size * n;
}

HttpResponse http_get(const std::string& url, const std::string& accept, bool head_only = false) {
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl) throw TransportError("cannot initialise libcurl");

    const std::string user_agent = std::format("{}/{}", kProg, kVersion);
    const std::string accept_header = "Accept: " + accept;
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(
        curl_slist_append(nullptr, accept_header.c_str()), &curl_slist_free_all);

    HttpResponse response;
    char error[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, user_agent.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, kHttpTimeoutSeconds);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl.get(), CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_ERRORBUFFER, error);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response.body);
    if (head_only) curl_easy_setopt(curl.get(), CURLOPT_NOBODY, 1L);

    const CURLcode rc = curl_easy_perform(curl.get());
    if (rc != CURLE_OK) throw TransportError(error[0] ? error : curl_easy_strerror(rc));
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &response.status);
    return response;
}

std::string status_text(long code) {
    static const std::map<long, const char*> table = {
        {100, "Continue"}, {101, "Switching Protocols"}, {200, "OK"}, {201, "Created"},
        {202, "Accepted"}, {204, "No Content"}, {206, "Partial Content"},
        {300, "Multiple Choices"}, {301, "Moved Permanently"}, {302, "Found"}, {303, "See Other"},
        {304, "Not Modified"}, {307, "Temporary Redirect"}, {308, "Permanent Redirect"},
        {400, "Bad Request"}, {401, "Unauthorized"}, {402, "Payment Required"}, {403, "Forbidden"},
        {404, "Not Found"}, {405, "Method Not Allowed"}, {406, "Not Acceptable"},
        {407, "Proxy Authentication Required"}, {408, "Request Timeout"}, {409, "Conflict"},
        {410, "Gone"}, {411, "Length Required"}, {412, "Precondition Failed"},
        {413, "Content Too Large"}, {414, "URI Too Long"}, {415, "Unsupported Media Type"},
        {416, "Range Not Satisfiable"}, {417, "Expectation Failed"}, {418, "I'm a Teapot"},
        {421, "Misdirected Request"}, {422, "Unprocessable Content"}, {423, "Locked"},
        {424, "Failed Dependency"}, {425, "Too Early"}, {426, "Upgrade Required"},
        {428, "Precondition Required"}, {429, "Too Many Requests"},
        {431, "Request Header Fields Too Large"}, {451, "Unavailable For Legal Reasons"},
        {500, "Internal Server Error"}, {501, "Not Implemented"}, {502, "Bad Gateway"},
        {503, "Service Unavailable"}, {504, "Gateway Timeout"}, {505, "HTTP Version Not Supported"},
        {506, "Variant Also Negotiates"}, {507, "Insufficient Storage"}, {508, "Loop Detected"},
        {510, "Not Extended"}, {511, "Network Authentication Required"}};
    auto it = table.find(code);
    return it == table.end() ? "Unknown Status" : it->second;
}

std::string apply_override(const std::string& url) {
    const char* env = std::getenv(kOverrideEnv);
    if (!env || !*env) return url;
    const std::string base_full = env;
    auto base_end = [](const std::string& u) {
        const auto scheme = u.find("://");
        const auto from = scheme == std::string::npos ? 0 : scheme + 3;
        return u.find('/', from);
    };
    const auto be = base_end(base_full);
    const std::string base = be == std::string::npos ? base_full : base_full.substr(0, be);
    const auto ue = base_end(url);
    return base + (ue == std::string::npos ? std::string{} : url.substr(ue));
}

bool internet_available() {
    for (const char* url : {"https://openlibrary.org/", "https://doi.org/", "https://www.googleapis.com/",
                            "https://api.crossref.org/"}) {
        try {
            http_get(apply_override(url), "*/*", true);
            return true;  // any HTTP answer proves connectivity
        } catch (const TransportError&) {
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Sources and retry logic
// ---------------------------------------------------------------------------
struct Source {
    enum class Parser { OpenLibrary, GoogleBooks, CslJson, Crossref } parser;
    std::string url;
    std::string accept;
    std::string label;  // the request as it is logged
};

std::vector<Source> sources_for(const Code& code) {
    const std::string json_accept = "application/json";
    if (code.kind == Code::Kind::Isbn) {
        const auto ol = apply_override("https://openlibrary.org/api/books?bibkeys=ISBN:" + code.value +
                                       "&format=json&jscmd=data");
        const auto gb = apply_override("https://www.googleapis.com/books/v1/volumes?q=isbn:" + code.value);
        return {{Source::Parser::OpenLibrary, ol, json_accept, "GET " + ol},
                {Source::Parser::GoogleBooks, gb, json_accept, "GET " + gb}};
    }
    const std::string quoted = percent_encode(code.value);
    const std::string csl = "application/vnd.citationstyles.csl+json";
    const auto dx = apply_override("https://doi.org/" + quoted);
    const auto cr = apply_override("https://api.crossref.org/works/" + quoted);
    return {{Source::Parser::CslJson, dx, csl, std::format("GET {} [Accept: {}]", dx, csl)},
            {Source::Parser::Crossref, cr, json_accept, "GET " + cr}};
}

using RetryCallback = std::function<void(const Source&, int, const std::string&)>;

// GET src.url; retry on HTTP 408/429 only. Returns the body and retries used.
std::pair<std::string, int> fetch(const Source& src, int max_retries, double delay, const RetryCallback& on_retry) {
    int retries = 0;
    for (;;) {
        HttpResponse resp;
        try {
            resp = http_get(src.url, src.accept);
        } catch (const TransportError& e) {
            throw FetchError(std::string("network error: ") + e.what(), retries);
        }
        if (resp.status >= 200 && resp.status < 300) return {std::move(resp.body), retries};
        const std::string reason = std::format("HTTP {} {}", resp.status, status_text(resp.status));
        if ((resp.status == 408 || resp.status == 429) && retries < max_retries) {
            ++retries;
            on_retry(src, retries, reason);
            std::this_thread::sleep_for(std::chrono::duration<double>(delay));
            continue;
        }
        throw FetchError(reason, retries);
    }
}

Record parse_response(Source::Parser parser, const std::string& body, const std::string& value) {
    switch (parser) {
        case Source::Parser::OpenLibrary: return parse_openlibrary(body, value);
        case Source::Parser::GoogleBooks: return parse_googlebooks(body, value);
        case Source::Parser::CslJson: return parse_csl_json(body, value);
        case Source::Parser::Crossref: return parse_crossref(body, value);
    }
    throw RecordError("internal error");
}

// Try every source in turn; return the record and the request that succeeded.
std::pair<Record, std::string> resolve(const Code& code, int max_retries, double delay, const RetryCallback& on_retry) {
    std::string failures;
    auto add_failure = [&](const std::string& text) { failures += (failures.empty() ? "" : "; ") + text; };
    for (const Source& src : sources_for(code)) {
        std::string body;
        int retries = 0;
        try {
            std::tie(body, retries) = fetch(src, max_retries, delay, on_retry);
        } catch (const FetchError& e) {
            std::string note;
            if (e.retries_done)
                note = std::format(" (after {} retr{})", e.retries_done, e.retries_done == 1 ? "y" : "ies");
            add_failure(std::format("{} -> {}{}", src.label, e.what(), note));
            continue;
        }
        try {
            Record rec = parse_response(src.parser, body, code.value);
            std::string label = src.label;
            if (retries) label += std::format(" (succeeded on retry attempt {} of {})", retries, max_retries);
            return {std::move(rec), std::move(label)};
        } catch (const RecordError& e) {
            add_failure(std::format("{} -> {}", src.label, e.what()));
        }
    }
    throw FetchError(failures, 0);
}

// ---------------------------------------------------------------------------
// Clipboard (wl-copy, xclip or xsel)
// ---------------------------------------------------------------------------
bool pipe_to(const char* command, const std::string& text) {
    FILE* p = popen(command, "w");
    if (!p) return false;
    std::fwrite(text.data(), 1, text.size(), p);
    return pclose(p) == 0;
}

bool copy_to_clipboard(const std::string& text) {
    signal(SIGPIPE, SIG_IGN);  // a missing tool must not kill us
    constexpr const char* wl = "wl-copy >/dev/null 2>&1";
    constexpr const char* xc = "xclip -selection clipboard >/dev/null 2>&1";
    constexpr const char* xs = "xsel --clipboard --input >/dev/null 2>&1";
    const std::vector<const char*> commands =
        std::getenv("WAYLAND_DISPLAY") ? std::vector<const char*>{wl, xc, xs} : std::vector<const char*>{xc, xs, wl};
    return std::ranges::any_of(commands, [&](const char* c) { return pipe_to(c, text); });
}

// ---------------------------------------------------------------------------
// Command line
// ---------------------------------------------------------------------------
struct Options {
    std::optional<std::string> code, input_file, bib_file, log_file;
    std::optional<long> retry;
    std::optional<double> delay;
    bool help = false, version = false;
};

void warn(const std::string& text) { std::cerr << "WARNING: " << text << std::endl; }

Options parse_args(const std::vector<std::string>& args) {
    Options o;
    auto set_once = [](std::optional<std::string>& slot, const std::string& name, const std::string& value) {
        if (slot) throw UsageError(name + " given more than once");
        slot = value;
    };
    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string arg = args[i];
        if (!arg.starts_with("-") || arg == "-") {
            if (o.code) throw UsageError("only one <isbn-or-doi> can be given; use --read-from-file for many");
            o.code = arg;
            continue;
        }
        if (!arg.starts_with("--")) throw UsageError("unknown option '" + arg + "' (try --help)");
        std::optional<std::string> inline_value;
        if (const auto eq = arg.find('='); eq != std::string::npos) {
            inline_value = arg.substr(eq + 1);
            arg.resize(eq);
        }
        auto value = [&]() -> std::string {
            if (inline_value) return *inline_value;
            if (i + 1 >= args.size()) throw UsageError(arg + " requires a value");
            return args[++i];
        };
        if (arg == "--help") o.help = true;
        else if (arg == "--version") o.version = true;
        else if (arg == "--read-from-file") set_once(o.input_file, arg, value());
        else if (arg == "--save-to-file") set_once(o.bib_file, arg, value());
        else if (arg == "--log") set_once(o.log_file, arg, value());
        else if (arg == "--retry") {
            const std::string v = value();
            long n = 0;
            const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
            if (ec != std::errc() || ptr != v.data() + v.size() || n < 0)
                throw UsageError("--retry must be an integer >= 0");
            o.retry = n;
        } else if (arg == "--delay") {
            const std::string v = value();
            char* end = nullptr;
            const double d = std::strtod(v.c_str(), &end);
            if (v.empty() || *end != '\0' || !std::isfinite(d) || d < 0)
                throw UsageError("--delay must be a real number >= 0");
            o.delay = d;
        } else {
            throw UsageError("unknown option '" + arg + "' (try --help)");
        }
    }
    return o;
}

std::vector<std::string> read_code_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw UsageError(std::format("cannot read '{}'", path));
    std::vector<std::string> codes;
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (first && line.starts_with("\xEF\xBB\xBF")) line.erase(0, 3);
        first = false;
        line = trim(line.substr(0, line.find('#')));
        if (!line.empty()) codes.push_back(line);
    }
    return codes;
}

std::string ensure_extension(const std::string& name, std::string_view ext, std::string_view what) {
    if (ends_with_icase(name, ext)) return name;
    const std::string fixed = name + std::string(ext);
    warn(std::format("{} '{}' does not have extension '{}'; using '{}' instead", what, name, ext, fixed));
    return fixed;
}

bool same_file(const std::string& a, const std::string& b) {
    std::error_code ec1, ec2;
    const auto pa = fs::weakly_canonical(a, ec1), pb = fs::weakly_canonical(b, ec2);
    if (ec1 || ec2) return fs::absolute(a).lexically_normal() == fs::absolute(b).lexically_normal();
    return pa == pb;
}

// Writes a line to the log file and to the terminal.
class Reporter {
public:
    explicit Reporter(const std::string& path) : file_(path, std::ios::app) {
        if (!file_) throw UsageError(std::format("cannot open log file '{}'", path));
    }
    void line(const std::string& text, bool error = false) {
        file_ << text << std::endl;
        (error ? std::cerr : std::cout) << text << std::endl;
    }

private:
    std::ofstream file_;
};

int run(const std::vector<std::string>& argv) {
    const Options opt = parse_args(argv);
    if (opt.help) {
        std::cout << replace_all(kHelpText, "@VERSION@", kVersion);
        return kExitOk;
    }
    if (opt.version) {
        std::cout << kProg << ' ' << kVersion << '\n';
        return kExitOk;
    }

    // ---- usage validation ------------------------------------------------
    if (opt.code && opt.input_file) throw UsageError("give either <isbn-or-doi> or --read-from-file, not both");
    if (!opt.code && !opt.input_file) throw UsageError("missing <isbn-or-doi> or --read-from-file (try --help)");
    const int max_retries = opt.retry ? static_cast<int>(std::min<long>(*opt.retry, 1'000'000)) : 0;
    if (opt.delay && !opt.retry) warn("--delay is ignored because --retry was not provided");
    const double delay = opt.delay.value_or(0.5);

    // ---- file names ------------------------------------------------------------
    std::string log_path;
    if (opt.log_file) {
        log_path = ensure_extension(*opt.log_file, ".log", "log file");
    } else if (opt.input_file) {
        std::string stem = fs::path(*opt.input_file).stem().string();
        log_path = (stem.empty() ? std::string(kProg) : stem) + ".log";
    } else {
        log_path = std::string(kProg) + ".log";
    }
    std::optional<std::string> bib_path;
    if (opt.bib_file) bib_path = ensure_extension(*opt.bib_file, ".bib", "BibTeX file");

    std::vector<std::pair<std::string, std::string>> named = {{"log", log_path}};
    if (bib_path) named.emplace_back(".bib", *bib_path);
    if (opt.input_file) named.emplace_back("input", *opt.input_file);
    for (std::size_t i = 0; i < named.size(); ++i)
        for (std::size_t j = i + 1; j < named.size(); ++j)
            if (same_file(named[i].second, named[j].second))
                throw UsageError(std::format("the {} file and the {} file must be different files (both are '{}')",
                                             named[i].first, named[j].first, named[i].second));

    const std::vector<std::string> raw_codes = opt.input_file ? read_code_file(*opt.input_file)
                                                              : std::vector<std::string>{*opt.code};
    if (raw_codes.empty()) throw UsageError(std::format("no ISBN/DOI codes found in '{}'", *opt.input_file));

    Reporter reporter(log_path);
    std::ofstream bib;
    if (bib_path) {
        bib.open(*bib_path, std::ios::app);
        if (!bib) throw UsageError(std::format("cannot open output file '{}'", *bib_path));
    }

    // ---- internet ---------------------------------------------------------------
    if (!internet_available()) {
        reporter.line("ERROR: no internet connection available; cannot fetch bibliographic data", true);
        return kExitNoInternet;
    }

    // ---- main loop ---------------------------------------------------------------
    std::vector<std::string> entries;
    std::set<std::string> used_keys;
    int failed = 0;
    for (const std::string& raw : raw_codes) {
        std::string shown = strip_quotes(raw);
        Code code;
        try {
            code = parse_code(raw);
        } catch (const CodeError& e) {
            reporter.line(std::format("{}: FAILED: {}", shown, e.what()), true);
            ++failed;
            continue;
        }
        shown = code.value;

        const RetryCallback on_retry = [&](const Source& src, int attempt, const std::string& reason) {
            reporter.line(std::format("{}: retry attempt {} of {} after {}, waiting {:g}s: {}", shown, attempt,
                                      max_retries, reason, delay, src.label));
        };

        Record record;
        std::string request;
        try {
            std::tie(record, request) = resolve(code, max_retries, delay, on_retry);
        } catch (const FetchError& e) {
            reporter.line(std::format("{}: FAILED: {}", shown, e.what()), true);
            ++failed;
            continue;
        }

        const std::string base = cite_key(record);
        std::string key = base;
        for (int n = 2; used_keys.contains(key); ++n) key = base + std::to_string(n);
        used_keys.insert(key);

        const std::string bibtex = render_bibtex(record, key);
        entries.push_back(bibtex);
        if (bib) bib << bibtex << "\n\n" << std::flush;
        reporter.line(std::format("{}: BibTeX generated with request: {}", shown, request));
    }

    // ---- clipboard -----------------------------------------------------------------
    if (!entries.empty()) {
        std::string text;
        for (const auto& e : entries) text += (text.empty() ? "" : "\n\n") + e;
        text += "\n";
        if (copy_to_clipboard(text)) {
            std::cout << std::format("{} BibTeX entr{} copied to the clipboard\n", entries.size(),
                                     entries.size() == 1 ? "y" : "ies");
        } else {
            warn("could not copy to the clipboard (install wl-copy, xclip or xsel); the BibTeX code follows:");
            std::cout << text << std::endl;
        }
    }
    return failed ? kExitSomeFailed : kExitOk;
}

}  // namespace

int main(int argc, char** argv) {
    const CurlGlobal curl_global;
    try {
        return run(std::vector<std::string>(argv + 1, argv + argc));
    } catch (const UsageError& e) {
        std::cerr << kProg << ": error: " << e.what() << std::endl;
        return kExitUsage;
    } catch (const std::exception& e) {
        std::cerr << kProg << ": unexpected error: " << e.what() << std::endl;
        return kExitSomeFailed;
    }
}
