// ============================================================================
// laige-api — public API manifest generator (M0-TOOL-01)
//
// Roadmap:  roadmap/M0-foundations.md, step M0-TOOL-01
// PRD refs: §9.4 (machine-readable API manifest), NFR-13.1 (every public
//           symbol machine-readable; CI regenerates the manifest and
//           fails on drift), NFR-13.4 (deterministic tool output)
// AGENTS:   CORE-006 (docs/manifest update in the same change),
//           CORE-008 (no silent failure: unsupported constructs fail
//           loudly), CORE-004 (smallest complete design),
//           DOC-003 (the manifest must match shipped names)
//
// Usage:
//   laige-api-scanner --root REPO_ROOT (--out FILE | --check FILE)
//
//   --root REPO_ROOT   Repository root (default: ".").
//   --out FILE         Write the regenerated manifest to FILE.
//   --check FILE       Regenerate in memory and compare with FILE:
//                     identical -> exit 0 (CRLF line endings in FILE are
//                     normalized to LF first - the canonical form is LF,
//                     see .gitattributes); stale -> exit 1 with a
//                     symbol-level diff; unreadable/invalid FILE -> exit 2.
//
// Exit codes: 0 = OK · 1 = stale manifest (--check) · 2 = error (usage,
// I/O, unsupported construct, invalid checked-in manifest).
//
// What it emits (laige-api.json)
// ------------------------------
//   {
//     "version": 1,
//     "generatedBy": "laige-api",
//     "headers": [ "src/<module>/include/...", ... ],   // sorted
//     "symbols": [
//       { "name": "laige::Pool<T>::acquire", "kind": "method",
//         "header": "src/laige-core/include/laige/pools.h", "line": 123,
//         "signature": "PoolHandle acquire()",
//         "summary": "Allocates one element from the pool.",
//         "budget": "O(1), no allocation on the success path",
//         "experimental": false },
//       ...
//     ]
//   }
//
//   One entry per PUBLIC symbol. Symbol kinds: class, struct, enum,
//   function, method, constructor, destructor, variable, alias,
//   enumerator, macro. Names are fully qualified (namespace + class
//   path; macros are bare). "line" is the first line of the
//   declaration. "summary" is the first paragraph of the doxygen comment
//   block directly above the declaration (null when absent);
//   "@budget <text>" and "@experimental" tag lines in that block fill
//   "budget" / "experimental".
//   The output is deterministic (no timestamps, NFR-13.4): regenerating
//   over unchanged headers is byte-identical, which is what --check and
//   the api_manifest CTest test rely on.
//
// Which symbols count as public
// ------------------------------
//   - Headers are scanned from each module's public include root
//     (src/<module>/include, PRD §10.1); .h files anywhere under it.
//     The module list mirrors MODULE_STACK in tools/laige-include-lint
//     (PRD §10.1); if the PRD changes the map, update both in one change
//     (CORE-006).
//   - namespace scope: public.
//   - class/struct scope: public access only (members after `private:`
//     / `protected:` are excluded; the default access is struct=public,
//     class=private, per the C++ standard).
//   - everything inside a namespace named `detail` is excluded
//     (implementation detail, per the public-header convention).
//   - namespaces themselves are not symbols (their contents are).
//   - `friend` declarations, forward declarations (`class X;`),
//     out-of-line member definitions (their in-class declaration is the
//     canonical entry), statements (static_assert, ...), and
//     preprocessor control directives produce no symbol entries.
//
// Scanner scope (deliberately narrow, roadmap)
// --------------------------------------------
// The scanner is a doxygen-comment-driven text walker — not a C++
// parser. It understands exactly the constructs the public headers use:
//
//   - namespaces (incl. nested `a::b`), class/struct/enum declarations
//     with access sections, template prefixes (`template <...>`, incl.
//     defaults and a `requires(...)` clause after the prefix), attribute
//     prefixes (`[[nodiscard]]`), default arguments, brace-init,
//     `using` aliases and `typedef`, `enum class` with enumerators
//     (incl. initializers and trailing comma), function definitions with
//     bodies (bodies are skipped whole), operators (`operator==`,
//     `operator<=>`, ...), destructors, constructors, `= default` /
//     `= 0` special members, #define macros (object-like and function-
//     like, incl. backslash continuations), and the transparent
//     directives (#if/#ifdef/#ifndef/#elif/#else/#endif and
//     #pragma [GCC|clang] diagnostic ...).
//
// Anything else — block comments /* */, raw string literals, inline
// namespaces, lambdas at declaration level, function-pointer types in
// parameter lists, the function-call operator `operator()`, `extern
// "C"` blocks, compound typedefs (`typedef struct {...} X;`), and
// #define inside a class body — is a LOUD scanner error (CORE-008).
// Comments inside multi-line declarations are skipped (they carry no
// manifest data). When a future header step needs an unsupported
// construct, extend the scanner in the same change (CORE-006).
//
// Documentation association (the doxygen rules implemented)
// ---------------------------------------------------------
// A doxygen block is a run of consecutive `//` comment lines directly
// above a declaration. Paragraphs split at blank comment lines (`//`
// with nothing else). Banner lines — a pure run of `-`, `=`, `~`, `*`
// characters, or a section header (a leading run of those characters,
// text, and a trailing run of at least three, e.g.
// `// --- Named values (CORE-005) ---- ...`) — finalize the current
// block but do not discard it. A physical blank
// line finalizes the block (the block stays pending; the next comment
// line starts a fresh block that replaces it, so the last block above a
// declaration wins). The block attaches to the next symbol-producing
// declaration or #define; it is discarded by a closing brace, an access
// specifier, or a non-transparent directive. A comment line that
// follows a code line carrying its own trailing `//` comment (or that
// is such a trailing comment itself) is treated as that trailing
// comment's wrapped continuation: it starts no block and extends none.
// Summary = first non-empty paragraph.
//
// Dependencies: laige-core only (its bounded JSON parser, M0-CORE-07,
// is used by --check to parse the checked-in manifest). C++20,
// no exceptions, no RTTI (NFR-8.10). Single translation unit.
// ============================================================================

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "laige/json.h"
#include "laige/result.h"

namespace {

// ---------------------------------------------------------------------------
// Small utilities
// ---------------------------------------------------------------------------

bool isSpaceChar(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool isIdentChar(unsigned char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}

// Operator characters that may follow `operator` in an operator name.
// `~` is handled separately (destructor form), `.` never appears in an
// operator name here (sizeof/member access is not a function name).
bool isOpChar(unsigned char c) noexcept {
  switch (c) {
    case '+': case '-': case '*': case '/': case '%':
    case '^': case '&': case '|': case '!': case '<': case '>':
    case '=': return true;
    default: return false;
  }
}

// The full C++20 keyword set: used to reject a "name" that is actually a
// keyword (e.g. the `(` of a function-pointer type `void(int)` must not
// be mistaken for a parameter list, and a `requires` clause must not be
// taken for a function name).
bool isKeywordWord(const std::string& w) noexcept {
  static const char* const kWords[] = {
      "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor",
      "bool", "break", "case", "catch", "char", "char8_t", "char16_t",
      "char32_t", "class", "concept", "const", "const_cast", "consteval",
      "constexpr", "constinit", "continue", "co_await", "co_return",
      "co_yield", "decltype", "default", "delete", "do", "double",
      "dynamic_cast", "else", "enum", "explicit", "export", "extern",
      "false", "float", "for", "friend", "goto", "if", "inline", "int",
      "long", "mutable", "namespace", "new", "noexcept", "not", "not_eq",
      "nullptr", "operator", "or", "or_eq", "private", "protected",
      "public", "register", "reinterpret_cast", "requires", "return",
      "short", "signed", "sizeof", "static", "static_assert",
      "static_cast", "struct", "switch", "template", "this",
      "thread_local", "throw", "true", "try", "typedef", "typeid",
      "typename", "union", "unsigned", "using", "virtual", "void",
      "volatile", "wchar_t", "while", "xor", "xor_eq",
  };
  for (const char* const k : kWords) {
    if (w == k) return true;
  }
  return false;
}

// First-token statement keywords: a declaration-level candidate starting
// with one of these is a statement, not a declaration (no symbol).
// NOTE: `requires` is NOT here — a `requires(...)` clause after a
// template prefix is part of a declaration, not a statement.
bool isStatementKeyword(const std::string& w) noexcept {
  return w == "static_assert" || w == "assert" || w == "if" || w == "for" ||
         w == "while" || w == "do" || w == "switch" || w == "return" ||
         w == "throw" || w == "case" || w == "break" || w == "continue" ||
         w == "new" || w == "delete" || w == "co_await" ||
         w == "co_return" || w == "co_yield" || w == "sizeof" ||
         w == "alignas" || w == "alignof" || w == "decltype";
}

// True when the word w starts at pos in s and is not followed by an
// identifier character (word boundary).
bool wordAt(const std::string& s, size_t pos, const std::string& w) noexcept {
  if (pos + w.size() > s.size()) return false;
  if (s.compare(pos, w.size(), w) != 0) return false;
  return pos + w.size() >= s.size() ||
         !isIdentChar(static_cast<unsigned char>(s[pos + w.size()]));
}

// Banner characters (comment section decorations).
bool isBannerChar(char c) noexcept {
  return c == '-' || c == '=' || c == '~' || c == '*';
}

// A banner line is either a pure run of banner characters, or a section
// header: a leading run of banner characters, text, and a trailing run of
// at least three (`// --- Named values (CORE-005) ---- ...`).
bool isBannerLine(const std::string& text) noexcept {
  if (text.empty()) return false;
  size_t lead = 0;
  while (lead < text.size() && isBannerChar(text[lead])) ++lead;
  size_t tail = 0;
  while (tail < text.size() - lead && isBannerChar(text[text.size() - 1 - tail])) ++tail;
  if (lead + tail == text.size()) return text.size() >= 3;
  return lead >= 1 && tail >= 3;
}

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && isSpaceChar(s[a])) ++a;
  while (b > a && isSpaceChar(s[b - 1])) --b;
  return s.substr(a, b - a);
}

// Collapse every whitespace run to one space; trim both ends. Used for
// signatures (multi-line declarations become one line).
std::string normalizeWs(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  bool pending = false;
  for (char c : s) {
    if (isSpaceChar(c)) {
      pending = true;
    } else {
      if (pending && !out.empty()) out += ' ';
      pending = false;
      out += c;
    }
  }
  return trim(out);
}

// Escape a string for the manifest JSON (ASCII-safe: control characters
// as \uXXXX, the common ones as two-character escapes; UTF-8 text above
// 0x7F passes through raw — the manifest is a UTF-8 document).
std::string jsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          static const char kHex[] = "0123456789abcdef";
          out += "\\u00";
          out += kHex[(c >> 4) & 0xF];
          out += kHex[c & 0xF];
        } else {
          out += static_cast<char>(c);
        }
        break;
    }
  }
  return out;
}

bool readFile(const std::string& path, std::string& out,
              std::string& err) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    err = "cannot open " + path;
    return false;
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  if (out.find('\0') != std::string::npos) {
    err = path + ": NUL byte in a text header (unsupported)";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Doc block -> manifest fields
// ---------------------------------------------------------------------------

struct DocInfo {
  std::string summary;    // first non-empty paragraph ("" when none)
  std::string budget;     // first "@budget <text>" line ("" when none)
  bool experimental = false;  // any "@experimental" line
};

bool isBudgetTagLine(const std::string& ln) noexcept {
  static const char kTag[] = "@budget";
  if (ln.size() < sizeof(kTag) - 1) return false;
  if (ln.compare(0, sizeof(kTag) - 1, kTag) != 0) return false;
  return ln.size() == sizeof(kTag) - 1 || ln[sizeof(kTag) - 1] == ' ';
}

bool isExperimentalTagLine(const std::string& ln) noexcept {
  static const char kTag[] = "@experimental";
  if (ln.size() < sizeof(kTag) - 1) return false;
  if (ln.compare(0, sizeof(kTag) - 1, kTag) != 0) return false;
  return ln.size() == sizeof(kTag) - 1 || ln[sizeof(kTag) - 1] == ' ';
}

// lines: comment texts in block order ("" = blank comment line);
// breaks: parallel, 1 where the line starts a new paragraph.
DocInfo docFromLines(const std::vector<std::string>& lines,
                     const std::vector<char>& breaks) {
  DocInfo d;
  size_t i = 0;
  while (i < lines.size()) {
    size_t j = i;
    std::string para;
    while (j < lines.size()) {
      const std::string& ln = lines[j];
      if (!ln.empty()) {
        if (isBudgetTagLine(ln)) {
          if (d.budget.empty()) d.budget = normalizeWs(ln.substr(8));
        } else if (isExperimentalTagLine(ln)) {
          d.experimental = true;
        } else {
          if (!para.empty()) para += ' ';
          para += ln;
        }
      }
      ++j;
      if (j < lines.size() && breaks[j]) break;
    }
    if (d.summary.empty() && !para.empty()) d.summary = para;
    i = j;
  }
  return d;
}

// ---------------------------------------------------------------------------
// Symbols
// ---------------------------------------------------------------------------

struct Symbol {
  std::string kind;        // class|struct|enum|function|method|constructor|
                           // destructor|variable|alias|enumerator|macro
  std::string name;        // fully qualified (macros: bare)
  std::string header;      // repo-relative path with '/'
  int line = 0;            // first line of the declaration
  std::string signature;   // normalized single line
  std::string summary;     // "" when absent
  std::string budget;      // "" when absent
  bool experimental = false;
};

// ---------------------------------------------------------------------------
// Declaration classification (operates on normalized text, string-aware)
// ---------------------------------------------------------------------------

// Strip a leading `template <...>` prefix (angle-balanced, string-aware).
std::string stripTemplatePrefix(const std::string& text) {
  const std::string kT = "template";
  if (text.compare(0, kT.size(), kT) != 0) return text;
  size_t i = kT.size();
  while (i < text.size() && isSpaceChar(text[i])) ++i;
  if (i >= text.size() || text[i] != '<') return text;
  int angle = 0;
  bool inStr = false;
  char q = 0;
  for (; i < text.size(); ++i) {
    char c = text[i];
    if (inStr) {
      if (c == q) inStr = false;
      continue;
    }
    if (c == '"' || c == '\'') { inStr = true; q = c; continue; }
    if (c == '<') ++angle;
    else if (c == '>') {
      --angle;
      if (angle == 0) return text.substr(i + 1);
    }
  }
  return text;  // unbalanced template list: leave as-is (fails downstream)
}

// First word (identifier-character run) of text, skipping leading
// whitespace. `static_assert(...)` yields "static_assert"; `[[nodiscard]]
// foo` yields "" (the attribute is not a word).
std::string firstWord(const std::string& text) {
  size_t i = 0;
  while (i < text.size() && isSpaceChar(text[i])) ++i;
  size_t j = i;
  while (j < text.size() && isIdentChar(static_cast<unsigned char>(text[j]))) ++j;
  return text.substr(i, j - i);
}

// Read the name token immediately before the '(' at parenPos in text.
// Forms: identifier, ~identifier (destructor), operator<op-run>,
// qualified name (`A::b` — qualified set when the name carries a `::`
// prefix, i.e. an out-of-line member definition).
bool nameTokenBefore(const std::string& text, size_t parenPos,
                     std::string& name, bool& qualified) {
  size_t i = parenPos;
  while (i > 0 && isSpaceChar(text[i - 1])) --i;
  if (i == 0) return false;
  qualified = false;
  unsigned char c0 = static_cast<unsigned char>(text[i - 1]);
  if (isOpChar(c0)) {
    size_t j = i;
    while (j > 0 && isOpChar(static_cast<unsigned char>(text[j - 1]))) --j;
    std::string op;
    op.reserve(i - j);
    for (size_t k = j; k < i; ++k) op += text[k];  // source order
    size_t k2 = j;
    while (k2 > 0 && isIdentChar(static_cast<unsigned char>(text[k2 - 1]))) --k2;
    if (k2 == j) return false;
    std::string id = text.substr(k2, j - k2);
    if (id != "operator") return false;
    name = "operator" + op;
    if (k2 >= 2 && text[k2 - 1] == ':' && text[k2 - 2] == ':') qualified = true;
    return true;
  }
  if (isIdentChar(c0)) {
    size_t j = i;
    while (j > 0 && isIdentChar(static_cast<unsigned char>(text[j - 1]))) --j;
    std::string id = text.substr(j, i - j);
    if (j > 0 && text[j - 1] == '~') {
      id = "~" + id;
      --j;
    }
    name = id;
    if (j >= 2 && text[j - 1] == ':' && text[j - 2] == ':') qualified = true;
    return true;
  }
  return false;
}

// Find the first top-level '(' whose preceding name token is a real
// (non-keyword) function name. Returns its position in text.
bool findParamList(const std::string& text, size_t& paramPos) {
  int paren = 0, brace = 0, bracket = 0;
  bool inStr = false;
  char q = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (inStr) {
      if (c == q) inStr = false;
      continue;
    }
    if (c == '"' || c == '\'') { inStr = true; q = c; continue; }
    if (c == '(') {
      if (paren == 0 && brace == 0 && bracket == 0) {
        std::string nm;
        bool qual = false;
        if (nameTokenBefore(text, i, nm, qual) && !isKeywordWord(nm)) {
          paramPos = i;
          return true;
        }
      }
      ++paren;
    } else if (c == ')') {
      if (paren > 0) --paren;
    } else if (c == '[') {
      ++bracket;
    } else if (c == ']') {
      if (bracket > 0) --bracket;
    } else if (c == '{') {
      ++brace;
    } else if (c == '}') {
      if (brace > 0) --brace;
    }
  }
  return false;
}

// Top-level '=' anywhere in [0, limit) (paren/brace/bracket depth 0). The
// '=' inside the '<=>', '<=', '>=' operators is not an initializer.
bool hasTopLevelEqualsBefore(const std::string& text, size_t limit) {
  int paren = 0, brace = 0, bracket = 0;
  bool inStr = false;
  char q = 0;
  for (size_t i = 0; i < limit && i < text.size(); ++i) {
    char c = text[i];
    if (inStr) {
      if (c == q) inStr = false;
      continue;
    }
    if (c == '"' || c == '\'') { inStr = true; q = c; continue; }
    if (c == '(') ++paren;
    else if (c == ')') { if (paren > 0) --paren; }
    else if (c == '[') ++bracket;
    else if (c == ']') { if (bracket > 0) --bracket; }
    else if (c == '{') ++brace;
    else if (c == '}') { if (brace > 0) --brace; }
    else if (c == '=' && paren == 0 && brace == 0 && bracket == 0) {
      bool prevLt = i > 0 && text[i - 1] == '<';
      bool nextGt = i + 1 < text.size() && text[i + 1] == '>';
      if (prevLt || nextGt) continue;  // part of '<=>', '<=', '>='
      // The '=' that IS the assignment-operator name (`operator=`,
      // `operator+=`, ...) is not an initializer either.
      size_t k = i;
      while (k > 0 && isOpChar(static_cast<unsigned char>(text[k - 1]))) --k;
      size_t b = k;
      while (b > 0 && isIdentChar(static_cast<unsigned char>(text[b - 1]))) --b;
      if (k - b == 8 && text.compare(b, 8, "operator") == 0) continue;
      return true;
    }
  }
  return false;
}

// The variable name in `Type name = initializer` / `Type name{...}` /
// `Type state_[N]` / `alignas(T) std::byte data[N]`: the last identifier
// before the first top-level '=', '{', or '['. The text has no trailing
// ';'. The '=' of comparison operators and of operator names is not an
// initializer (it only matters after the cut, so this is cheap).
std::string variableNameIn(const std::string& text) {
  int paren = 0, brace = 0, bracket = 0;
  bool inStr = false;
  char q = 0;
  size_t cut = text.size();
  for (size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (inStr) {
      if (c == q) inStr = false;
      continue;
    }
    if (c == '"' || c == '\'') { inStr = true; q = c; continue; }
    if (c == '(') ++paren;
    else if (c == ')') { if (paren > 0) --paren; }
    else if (c == '[') {
      if (paren == 0 && brace == 0 && bracket == 0) {
        // The '[' of an array type after a template-id
        // (`ElementSlot<T>[]`) is a type suffix, not a bound.
        bool afterGt = false;
        size_t k = i;
        while (k > 0 && isSpaceChar(text[k - 1])) --k;
        if (k > 0 && text[k - 1] == '>') afterGt = true;
        if (!afterGt) { cut = i; break; }
      }
      ++bracket;
    } else if (c == ']') {
      if (bracket > 0) --bracket;
    } else if (c == '{') {
      if (brace == 0 && paren == 0 && bracket == 0) { cut = i; break; }
      ++brace;
    } else if (c == '}') {
      if (brace > 0) --brace;
    } else if (c == '=' && paren == 0 && brace == 0 && bracket == 0) {
      bool prevLt = i > 0 && text[i - 1] == '<';
      bool nextGt = i + 1 < text.size() && text[i + 1] == '>';
      if (prevLt || nextGt) continue;  // '<=>', '<=', '>='
      size_t k = i;
      while (k > 0 && isOpChar(static_cast<unsigned char>(text[k - 1]))) --k;
      size_t b = k;
      while (b > 0 && isIdentChar(static_cast<unsigned char>(text[b - 1]))) --b;
      if (k - b == 8 && text.compare(b, 8, "operator") == 0) continue;
      cut = i;
      break;
    }
  }
  size_t end = cut;
  while (end > 0 && isSpaceChar(text[end - 1])) --end;
  size_t start = end;
  while (start > 0 && isIdentChar(static_cast<unsigned char>(text[start - 1]))) --start;
  return text.substr(start, end - start);
}

// First identifier after the leading `using` keyword.
std::string aliasNameIn(const std::string& text) {
  const std::string kU = "using";
  size_t i = 0;
  while (i < text.size() && isSpaceChar(text[i])) ++i;
  if (text.compare(i, kU.size(), kU) != 0) return "";
  i += kU.size();
  while (i < text.size() && isSpaceChar(text[i])) ++i;
  size_t j = i;
  while (j < text.size() && isIdentChar(static_cast<unsigned char>(text[j]))) ++j;
  return text.substr(i, j - i);
}

// ---------------------------------------------------------------------------
// Header scanner
// ---------------------------------------------------------------------------

struct Scope {
  std::string name;
  int kind = 0;      // 0 namespace, 1 class/struct, 2 enum
  int access = 2;    // kind 1: 0 private, 1 protected, 2 public
  int openBraceDepth = 0;  // braceDepth_ value after the opening '{'
};

class FileScanner {
 public:
  FileScanner(const std::string& relHeader, std::vector<Symbol>& out)
      : relHeader_(relHeader), out_(out) {}

  // Returns false on failure (header:line: message in error()).
  bool scan(const std::string& src) {
    src_ = &src;
    pos_ = 0;
    line_ = 1;
    while (pos_ < src.size()) {
      // Any fail() sets errMsg_ and returns to this loop without consuming
      // the failing character: exit here instead of spinning.
      if (!errMsg_.empty()) break;
      char c = src[pos_];
      if (inString_) {
        stepString(c);
        continue;
      }
      if (c == '\n') {
        endLine();
        continue;
      }
      if (isSpaceChar(c)) {
        // Whitespace inside a candidate/enum segment is preserved (as a
        // single space after normalizeWs); elsewhere it is skipped.
        if (inCandidate_) candText_ += ' ';
        if (inEnum_) enumSeg_ += ' ';
        ++pos_;
        continue;
      }
      if (c == '"' || c == '\'') {
        inString_ = true;
        stringQuote_ = c;
        stringEsc_ = false;
        markCode();
        if (inCandidate_) candText_ += c;
        if (inEnum_) enumSeg_ += c;
        ++pos_;
        continue;
      }
      if (c == '/' && pos_ + 1 < src.size() && src[pos_ + 1] == '/') {
        if (inBody_) {
          skipToNewline();
          continue;
        }
        if (inCandidate_) {
          candText_ += ' ';
          skipToNewline();
          continue;
        }
        stepComment();
        continue;
      }
      if (c == '/' && pos_ + 1 < src.size() && src[pos_ + 1] == '*') {
        fail("block comments are not supported by the laige-api scanner; "
             "use // comments in public headers");
        return false;
      }
      if (inBody_) {
        if (c == '{') {
          ++bodyBrace_;
        } else if (c == '}') {
          --bodyBrace_;
          if (bodyBrace_ == 0) inBody_ = false;
        }
        markCode();
        ++pos_;
        continue;
      }
      if (inEnum_) {
        stepEnum(c);
        continue;
      }
      if (inCandidate_) {
        stepCandidate(c);
        continue;
      }
      stepTop(c);
    }
    if (!errMsg_.empty()) return false;
    if (inCandidate_) {
      fail("unterminated declaration starting at line " +
           std::to_string(candLine_));
      return false;
    }
    if (inBody_) {
      fail("unterminated function body (missing '}')");
      return false;
    }
    if (!scopes_.empty()) {
      fail("unbalanced braces: '" + scopes_.back().name + "' never closed");
      return false;
    }
    return true;
  }

  const std::string& error() const { return errMsg_; }
  int errorLine() const { return errLine_; }

 private:
  // -----------------------------------------------------------------------
  // State
  // -----------------------------------------------------------------------

  const std::string relHeader_;
  std::vector<Symbol>& out_;

  const std::string* src_ = nullptr;
  size_t pos_ = 0;
  int line_ = 1;
  std::string errMsg_;
  int errLine_ = 0;

  // String literal state.
  bool inString_ = false;
  char stringQuote_ = 0;
  bool stringEsc_ = false;

  // Scope bookkeeping.
  std::vector<Scope> scopes_;
  int braceDepth_ = 0;
  bool inDetail_ = false;

  // Doc block state (the last comment block not yet attached/discarded).
  bool docActive_ = false;
  bool docComplete_ = false;
  std::vector<std::string> docLines_;
  std::vector<char> docBreaks_;

  // Current-line state.
  bool prevLineTrailing_ = false;
  bool lineHasCode_ = false;
  bool lineHasComment_ = false;
  bool lineHasTrailing_ = false;
  bool lineHasPreproc_ = false;
  bool linePreprocTransparent_ = false;

  // Candidate (declaration) accumulation.
  bool inCandidate_ = false;
  std::string candText_;
  int candLine_ = 0;
  int candParen_ = 0;
  int candBracket_ = 0;
  int candBrace_ = 0;
  bool candParamFound_ = false;
  bool candIsTypeOpen_ = false;
  bool candTemplateActive_ = false;
  int candAngle_ = 0;
  bool pendingFirstToken_ = false;
  bool candRequiresClause_ = false;
  int candRequiresParen_ = 0;

  // Function body skip.
  bool inBody_ = false;
  int bodyBrace_ = 0;

  // Enum body.
  bool inEnum_ = false;
  int enumParen_ = 0;
  std::string enumSeg_;
  bool enumSegStarted_ = false;
  int enumSegLine_ = 0;

  // -----------------------------------------------------------------------
  // Basics
  // -----------------------------------------------------------------------

  const std::string& src() const { return *src_; }

  bool fail(const std::string& msg) {
    if (errMsg_.empty()) {
      errMsg_ = msg;
      errLine_ = line_;
    }
    return false;
  }

  void markCode() { lineHasCode_ = true; }

  void skipToNewline() {
    while (pos_ < src().size() && src()[pos_] != '\n') ++pos_;
  }

  // Consume whitespace on the current line (no newlines).
  void skipWsSameLine() {
    while (pos_ < src().size() && isSpaceChar(src()[pos_]) &&
           src()[pos_] != '\n') {
      ++pos_;
    }
  }

  // A physical line ends (pos_ is at the '\n').
  void endLine() {
    prevLineTrailing_ = lineHasTrailing_;
    // A blank line (or a non-transparent directive line) finalizes an open
    // doc block: the block stays pending, and the next comment line starts
    // a fresh block that replaces it (the last block above a declaration
    // wins). Comment lines and transparent directive lines do NOT
    // finalize: a block may span blank *comment* lines and #if/#endif.
    if (!inBody_ && docActive_ && !lineHasCode_ && !lineHasComment_ &&
        !(lineHasPreproc_ && linePreprocTransparent_)) {
      docComplete_ = true;
    }
    if (inCandidate_) candText_ += ' ';
    if (inEnum_) enumSeg_ += ' ';
    lineHasCode_ = false;
    lineHasComment_ = false;
    lineHasTrailing_ = false;
    lineHasPreproc_ = false;
    linePreprocTransparent_ = false;
    ++pos_;
    ++line_;
  }

  // String literal content (pos_ at a content character, inString_ true).
  void stepString(char c) {
    if (c == '\n') {
      // A raw newline inside a string is malformed C++; close the string
      // and let the normal newline bookkeeping run (loud downstream).
      inString_ = false;
      stringEsc_ = false;
      return;  // the main loop's endLine() runs next
    }
    if (stringEsc_) {
      stringEsc_ = false;
    } else if (c == '\\') {
      stringEsc_ = true;
    } else if (c == stringQuote_) {
      inString_ = false;
      stringEsc_ = false;
    }
    if (inCandidate_) candText_ += c;
    if (inEnum_) enumSeg_ += c;
    ++pos_;
  }

  // A line of comments starts at pos_ (c == '/' and src_[pos_+1] == '/').
  void stepComment() {
    size_t start = pos_ + 2;
    size_t end = start;
    while (end < src().size() && src()[end] != '\n') ++end;
    std::string text = trim(src().substr(start, end - start));
    pos_ = end;  // stop at the '\n' (handled by the main loop)
    lineHasComment_ = true;
    if (lineHasCode_) {
      // A trailing comment on a code line (or the wrapped continuation of
      // one): starts no doc block and extends none.
      lineHasTrailing_ = true;
      return;
    }
    if (isBannerLine(text)) {
      // Banner: finalize (not discard) the current block.
      if (docActive_) docComplete_ = true;
      return;
    }
    if (docActive_ && !docComplete_) {
      // Continuation of the open block: a new paragraph only after a blank
      // comment line (already stored as "").
      docBreaks_.push_back(!docLines_.empty() && docLines_.back().empty()
                               ? 1 : 0);
      docLines_.push_back(text);
      return;
    }
    // Start a new block (replacing any completed pending block).
    if (prevLineTrailing_) return;
    docActive_ = true;
    docComplete_ = false;
    docLines_.clear();
    docBreaks_.clear();
    docLines_.push_back(text);
    docBreaks_.push_back(1);
  }

  // Attach the pending doc block to a symbol (consumes it either way).
  DocInfo consumeDoc() {
    DocInfo d;
    if (docActive_) {
      d = docFromLines(docLines_, docBreaks_);
      docActive_ = false;
      docComplete_ = false;
      docLines_.clear();
      docBreaks_.clear();
    }
    return d;
  }

  // -----------------------------------------------------------------------
  // Scope bookkeeping
  // -----------------------------------------------------------------------

  void recomputeInDetail() {
    inDetail_ = false;
    for (const Scope& s : scopes_) {
      if (s.kind == 0 && s.name == "detail") {
        inDetail_ = true;
        break;
      }
    }
  }

  void closeTopScope() {
    if (scopes_.empty()) {
      fail("unbalanced '}'");
      return;
    }
    scopes_.pop_back();
    recomputeInDetail();
    docActive_ = false;  // no symbol attaches across a closing brace
  }

  // The nearest enclosing class/struct scope (empty when none).
  const Scope* classScope() const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      if (it->kind == 1) return &*it;
    }
    return nullptr;
  }

  // Public = namespace scope, or inside a class with current access
  // public, and not inside a `detail` namespace.
  bool isEligible() const {
    if (inDetail_) return false;
    const Scope* c = classScope();
    return c == nullptr || c->access == 2;
  }

  std::string qualifiedBase() const {
    std::string q;
    for (const Scope& s : scopes_) {
      if (!q.empty()) q += "::";
      q += s.name;
    }
    return q;
  }

  void emit(const std::string& kind, const std::string& localName,
            const std::string& signature, int line, const DocInfo& doc) {
    if (!isEligible()) return;
    Symbol sym;
    sym.kind = kind;
    sym.name =
        kind == "macro" ? localName : qualifiedBase() + "::" + localName;
    sym.header = relHeader_;
    sym.line = line;
    sym.signature = signature;
    sym.summary = doc.summary;
    sym.budget = doc.budget;
    sym.experimental = doc.experimental;
    out_.push_back(std::move(sym));
  }

  // -----------------------------------------------------------------------
  // Top level
  // -----------------------------------------------------------------------

  void stepTop(char c) {
    if (c == '#') {
      if (lineHasCode_) {
        fail("preprocessor directive must start a line");
        return;
      }
      stepPreproc();
      return;
    }
    if (c == '}') {
      markCode();
      ++pos_;
      --braceDepth_;
      if (scopes_.empty() || braceDepth_ < 0) {
        fail("unbalanced '}'");
        return;
      }
      // A nested `namespace a::b {` pushes several scopes that share the
      // single opening brace: close them all here.
      while (!scopes_.empty() && braceDepth_ < scopes_.back().openBraceDepth) {
        closeTopScope();
      }
      skipWsSameLine();
      if (pos_ < src().size() && src()[pos_] == ';') ++pos_;
      return;
    }
    if (c == ';') {
      markCode();
      ++pos_;
      return;
    }
    if (tryAccessSpecifier()) return;
    markCode();
    beginCandidate();
    stepCandidate(c);
  }

  // Consume a `public:` / `private:` / `protected:` line inside a class.
  bool tryAccessSpecifier() {
    if (lineHasCode_) return false;
    const Scope* c = classScope();
    if (c == nullptr) return false;
    std::string tok;
    size_t i = pos_;
    const std::string& s = src();
    while (i < s.size() && s[i] != '\n' &&
           !(s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/')) {
      if (isSpaceChar(s[i])) {
        ++i;
      } else {
        tok += s[i];
        ++i;
      }
    }
    if (tok == "public:" || tok == "private:" || tok == "protected:") {
      skipToNewline();
      markCode();
      docActive_ = false;  // no symbol attaches across an access specifier
      scopes_.back().access =
          tok == "public:" ? 2 : (tok == "private:" ? 0 : 1);
      return true;
    }
    return false;
  }

  // -----------------------------------------------------------------------
  // Preprocessor directives (line starts at pos_, c == '#')
  // -----------------------------------------------------------------------

  void stepPreproc() {
    markCode();
    lineHasPreproc_ = true;
    // Read the logical directive: physical lines joined at backslash
    // continuations. Continuation lines carry no per-line state of their
    // own (they are part of this directive).
    std::string logical;
    size_t end = 0;
    while (true) {
      end = pos_;
      while (end < src().size() && src()[end] != '\n') ++end;
      std::string phys = trim(src().substr(pos_, end - pos_));
      bool cont = !phys.empty() && phys.back() == '\\';
      if (cont) phys.pop_back();
      if (!logical.empty()) logical += ' ';
      logical += phys;
      if (!cont) {
        pos_ = end;  // stop at the '\n' (or EOF); the main loop ends the line
        break;
      }
      if (end >= src().size()) {
        pos_ = src().size();  // continuation at EOF: malformed (loud later)
        break;
      }
      pos_ = end + 1;  // over the '\n'
      ++line_;
      lineHasCode_ = false;
      lineHasComment_ = false;
      lineHasTrailing_ = false;
    }

    // Classify: first token after '#'.
    std::string body = logical;
    if (!body.empty() && body[0] == '#') body = body.substr(1);
    std::string tok = firstWord(body);

    bool transparent =
        tok == "if" || tok == "ifdef" || tok == "ifndef" || tok == "elif" ||
        tok == "else" || tok == "endif";
    if (tok == "pragma") {
      size_t i = 0;
      while (i < body.size() && isSpaceChar(body[i])) ++i;
      if (body.compare(i, 6, "pragma") == 0) i += 6;
      while (i < body.size() && isSpaceChar(body[i])) ++i;
      if (body.compare(i, 3, "GCC") == 0 || body.compare(i, 5, "clang") == 0) {
        // `#pragma [GCC|clang] diagnostic ...` (sim_math.h pins its
        // -Wfloat-equal suppression this way; the pinned-math ADR 0002
        // documents it). Transparent: it neither discards nor creates
        // docs.
        transparent = true;
      }
    }
    linePreprocTransparent_ = transparent;
    if (tok == "define") {
      // A #define is a symbol: it consumes the doc block itself (before
      // the non-transparent discard below).
      if (classScope() != nullptr) {
        fail("a #define inside a class body is not supported by the "
             "laige-api scanner");
        return;
      }
      handleMacro(logical);
      return;
    }
    if (!transparent) {
      docActive_ = false;  // non-transparent directive: no doc across it
    }
  }

  // Emit a #define macro symbol (namespace scope).
  void handleMacro(const std::string& logical) {
    std::string body = logical;
    if (!body.empty() && body[0] == '#') body = body.substr(1);
    size_t i = 0;
    while (i < body.size() && isSpaceChar(body[i])) ++i;
    if (body.compare(i, 6, "define") != 0) return;
    i += 6;
    while (i < body.size() && isSpaceChar(body[i])) ++i;
    size_t start = i;
    while (i < body.size() && isIdentChar(static_cast<unsigned char>(body[i]))) ++i;
    if (i == start) {
      fail("malformed #define (missing macro name)");
      return;
    }
    std::string name = body.substr(start, i - start);
    while (i < body.size() && isSpaceChar(body[i])) ++i;
    std::string sig;
    if (i < body.size() && body[i] == '(') {
      // Function-like: capture the balanced parameter list.
      size_t j = i + 1;
      int paren = 1;
      bool inStr = false;
      char q = 0;
      for (; j < body.size(); ++j) {
        char c = body[j];
        if (inStr) {
          if (c == q) inStr = false;
          continue;
        }
        if (c == '"' || c == '\'') { inStr = true; q = c; continue; }
        if (c == '(') ++paren;
        else if (c == ')') {
          --paren;
          if (paren == 0) break;
        }
      }
      if (j >= body.size()) {
        fail("malformed #define (unbalanced parameter list for " + name + ")");
        return;
      }
      sig = "#define " + name + "(" + normalizeWs(body.substr(i + 1, j - i - 1)) + ")";
    } else {
      std::string rest = trim(body.substr(i));
      sig = "#define " + name + (rest.empty() ? "" : " " + rest);
    }
    DocInfo doc = consumeDoc();
    emit("macro", name, sig, line_, doc);
  }

  // -----------------------------------------------------------------------
  // Candidate (declaration) accumulation
  // -----------------------------------------------------------------------

  void beginCandidate() {
    candLine_ = line_;
    candText_.clear();
    candParen_ = 0;
    candBracket_ = 0;
    candBrace_ = 0;
    candParamFound_ = false;
    candIsTypeOpen_ = false;
    candTemplateActive_ = false;
    candAngle_ = 0;
    pendingFirstToken_ = false;
    candRequiresClause_ = false;
    candRequiresParen_ = 0;
    inCandidate_ = true;
    // First token at the candidate start: a type keyword opens a type
    // declaration (its '{' starts the body/scope, not a brace-init); a
    // `template` keyword opens the angle list (the type keyword is
    // recognized after the '>' closes).
    const std::string& s = src();
    size_t i = pos_;
    if (wordAt(s, i, "class") || wordAt(s, i, "struct") ||
        wordAt(s, i, "enum") || wordAt(s, i, "namespace")) {
      candIsTypeOpen_ = true;
    } else if (wordAt(s, i, "template") &&
               (i + 8 >= s.size() || s[i + 8] == '<' ||
                isSpaceChar(s[i + 8]))) {
      candTemplateActive_ = true;
    }
  }

  void stepCandidate(char c) {
    candText_ += c;
    // NOTE: this block must run before the template-angle update below, so
    // that a pendingFirstToken_ set on this character (the template's '>')
    // is evaluated on the NEXT character, not consumed by it.
    if (pendingFirstToken_) {
      const std::string& s = src();
      if (candRequiresClause_) {
        // Inside a `requires(...)` clause (constrained declaration):
        // keep waiting for the type keyword after the clause closes.
        if (c == '(') {
          ++candRequiresParen_;
        } else if (c == ')') {
          if (candRequiresParen_ > 0) --candRequiresParen_;
          if (candRequiresParen_ == 0) candRequiresClause_ = false;
        }
      } else if (wordAt(s, pos_, "class") || wordAt(s, pos_, "struct") ||
                 wordAt(s, pos_, "enum") || wordAt(s, pos_, "namespace")) {
        candIsTypeOpen_ = true;
        pendingFirstToken_ = false;
      } else if (wordAt(s, pos_, "requires") && pos_ + 9 < s.size() &&
                 s[pos_ + 8] == '(') {
        candRequiresClause_ = true;
        candRequiresParen_ = 0;
      } else {
        pendingFirstToken_ = false;
      }
    }
    if (candTemplateActive_) {
      if (c == '<') ++candAngle_;
      else if (c == '>') {
        --candAngle_;
        if (candAngle_ == 0) {
          candTemplateActive_ = false;
          pendingFirstToken_ = true;
        }
      }
    }
    ++pos_;  // consume the character (the finish* paths resume past it)
    switch (c) {
      case '(':
        if (candParen_ == 0 && candBrace_ == 0 && candBracket_ == 0 &&
            !candParamFound_ && !candTemplateActive_) {
          std::string nm;
          bool qual = false;
          if (nameTokenBefore(candText_, candText_.size() - 1, nm, qual) &&
              !isKeywordWord(nm)) {
            candParamFound_ = true;
          }
        }
        ++candParen_;
        break;
      case ')':
        if (candParen_ > 0) --candParen_;
        break;
      case '[':
        ++candBracket_;
        break;
      case ']':
        if (candBracket_ > 0) --candBracket_;
        break;
      case '{':
        if (candBrace_ == 0 && candParen_ == 0 && candBracket_ == 0 &&
            !candTemplateActive_) {
          if (candIsTypeOpen_) {
            finishTypeOpen();
            return;
          }
          if (candParamFound_) {
            finishFunctionBody();
            return;
          }
        }
        ++candBrace_;
        break;
      case '}':
        if (candBrace_ > 0) --candBrace_;
        break;
      case ';':
        if (candParen_ == 0 && candBrace_ == 0 && candBracket_ == 0) {
          finishDeclaration();
          return;
        }
        break;
      default:
        break;
    }
  }

  void endCandidateState() {
    inCandidate_ = false;
    candParen_ = candBracket_ = candBrace_ = 0;
    candParamFound_ = false;
    candIsTypeOpen_ = false;
    candTemplateActive_ = false;
    candAngle_ = 0;
    pendingFirstToken_ = false;
    candRequiresClause_ = false;
    candRequiresParen_ = 0;
  }

  // `namespace a::b {` / `class X ... {` / `struct ... {` / `enum ... {`
  void finishTypeOpen() {
    std::string header = candText_;
    header.pop_back();  // the '{'
    std::string text = normalizeWs(header);
    std::string rest = stripTemplatePrefix(text);
    std::string ft = firstWord(rest);
    // Offset of the keyword in rest (firstWord() skipped leading space).
    size_t kw = 0;
    while (kw < rest.size() && isSpaceChar(rest[kw])) ++kw;
    endCandidateState();

    if (ft == "namespace") {
      std::string name = trim(rest.substr(kw + ft.size()));
      // Split on '::'; every part must be a plain (non-keyword)
      // identifier. All parts share the single opening brace.
      std::vector<std::string> parts;
      bool ok = !name.empty();
      size_t a = 0;
      while (ok) {
        size_t b = name.find("::", a);
        std::string part =
            (b == std::string::npos) ? name.substr(a) : name.substr(a, b - a);
        ok = !part.empty() && !isKeywordWord(part);
        for (char ch : part) {
          if (!isIdentChar(static_cast<unsigned char>(ch))) {
            ok = false;
            break;
          }
        }
        parts.push_back(part);
        if (b == std::string::npos) break;
        a = b + 2;
      }
      if (!ok || parts.empty()) {
        fail("cannot parse namespace name in: " + text);
        return;
      }
      int depth = ++braceDepth_;
      for (const std::string& p : parts) {
        Scope sc;
        sc.name = p;
        sc.kind = 0;
        sc.openBraceDepth = depth;
        scopes_.push_back(std::move(sc));
      }
      recomputeInDetail();
      docActive_ = false;  // a namespace header never carries a symbol doc
      return;
    }

    if (ft == "class" || ft == "struct" || ft == "enum") {
      // Name: first identifier after the keywords (`enum class X` skips
      // the second `class`).
      std::string after = rest.substr(kw + ft.size());
      size_t i = 0;
      while (i < after.size() && isSpaceChar(after[i])) ++i;
      if (ft == "enum" && wordAt(after, i, "class")) {
        i += 5;
        while (i < after.size() && isSpaceChar(after[i])) ++i;
      }
      size_t j = i;
      while (j < after.size() && isIdentChar(static_cast<unsigned char>(after[j]))) ++j;
      std::string name = after.substr(i, j - i);
      if (name.empty() || isKeywordWord(name)) {
        fail("cannot parse type name in: " + text);
        return;
      }
      DocInfo doc = consumeDoc();
      emit(ft == "enum" ? "enum" : ft, name, text, candLine_, doc);

      Scope sc;
      sc.name = name;
      sc.kind = ft == "enum" ? 2 : 1;
      sc.access = (ft == "struct") ? 2 : 0;  // struct=public, class=private
      sc.openBraceDepth = ++braceDepth_;
      scopes_.push_back(std::move(sc));

      if (ft == "enum") {
        inEnum_ = true;
        enumSeg_.clear();
        enumSegStarted_ = false;
        enumParen_ = 0;
        enumSegLine_ = 0;
      }
      return;
    }

    fail("unrecognized type declaration: " + text);
  }

  // A function-like declaration opening its body at '{'.
  void finishFunctionBody() {
    std::string header = candText_;
    header.pop_back();  // the '{'
    std::string text = normalizeWs(header);
    DocInfo doc = consumeDoc();
    classifyFunction(text, doc);
    endCandidateState();
    inBody_ = true;
    bodyBrace_ = 1;  // the body's own '{'
  }

  // A ';' -terminated declaration: alias / typedef / statement / function
  // / variable / forward declaration.
  void finishDeclaration() {
    std::string header = candText_;
    header.pop_back();  // the ';'
    std::string text = normalizeWs(header);
    endCandidateState();
    classifyDeclaration(text);
  }

  // Shared function/method/constructor/destructor classification for a
  // declaration whose parameter list was found (text: normalized, no
  // trailing ';' or '{').
  void classifyFunction(const std::string& text, const DocInfo& doc) {
    std::string rest = stripTemplatePrefix(text);
    std::string ft = firstWord(rest);
    if (ft == "using" || isStatementKeyword(ft) || ft == "class" ||
        ft == "struct" || ft == "enum" || ft == "namespace" ||
        ft == "friend") {
      return;  // no symbol (the doc is consumed by the caller either way)
    }
    size_t ppos = 0;
    if (!findParamList(rest, ppos)) {
      fail("cannot find parameter list in function declaration: " + text);
      return;
    }
    std::string pname;
    bool qualified = false;
    if (!nameTokenBefore(rest, ppos, pname, qualified)) {
      fail("cannot determine function name in: " + text);
      return;
    }
    if (qualified) {
      // An out-of-line member definition (`Prng::seedState(...)` at
      // namespace scope): the in-class declaration is the canonical
      // manifest entry; the definition adds no new symbol.
      return;
    }
    std::string unq = pname;
    size_t qpos = unq.rfind("::");
    if (qpos != std::string::npos) unq = unq.substr(qpos + 2);
    std::string kind;
    if (!pname.empty() && pname[0] == '~') {
      kind = "destructor";
    } else if (!qualified) {
      const Scope* c = classScope();
      if (c != nullptr && c->name == unq) kind = "constructor";
      else kind = (c != nullptr) ? "method" : "function";
    } else {
      kind = (classScope() != nullptr) ? "method" : "function";
    }
    emit(kind, pname, text, candLine_, doc);
  }

  void classifyDeclaration(const std::string& text) {
    std::string rest = stripTemplatePrefix(text);
    std::string ft = firstWord(rest);
    DocInfo doc = consumeDoc();
    if (ft == "using") {
      std::string nm = aliasNameIn(rest);
      if (nm.empty() || isKeywordWord(nm)) {
        fail("cannot determine alias name in: " + text);
        return;
      }
      emit("alias", nm, text, candLine_, doc);
      return;
    }
    if (ft == "typedef") {
      // `typedef Type Name;`: the name is the last identifier.
      std::string nm = variableNameIn(text);
      if (nm.empty() || isKeywordWord(nm)) {
        fail("cannot determine typedef name in: " + text);
        return;
      }
      emit("alias", nm, text, candLine_, doc);
      return;
    }
    if (isStatementKeyword(ft) || ft == "class" || ft == "struct" ||
        ft == "enum" || ft == "namespace" || ft == "friend") {
      return;  // statement or forward declaration: no symbol
    }
    size_t ppos = 0;
    if (findParamList(rest, ppos) &&
        !hasTopLevelEqualsBefore(rest, ppos)) {
      classifyFunction(text, doc);
      return;
    }
    // A variable (or a function-pointer type with '=' before the param
    // list, e.g. `Type name = makeFn(...)`).
    std::string nm = variableNameIn(text);
    if (nm.empty()) {
      fail("cannot determine variable name in: " + text);
      return;
    }
    emit("variable", nm, text, candLine_, doc);
  }

  // -----------------------------------------------------------------------
  // Enum body
  // -----------------------------------------------------------------------

  void stepEnum(char c) {
    markCode();
    if (c == '}') {
      finalizeEnumerator();
      inEnum_ = false;
      ++pos_;
      --braceDepth_;
      if (scopes_.empty() || braceDepth_ < 0) {
        fail("unbalanced '}'");
        return;
      }
      while (!scopes_.empty() && braceDepth_ < scopes_.back().openBraceDepth) {
        closeTopScope();
      }
      skipWsSameLine();
      if (pos_ < src().size() && src()[pos_] == ';') ++pos_;
      return;
    }
    ++pos_;  // consume the character (the '}' branch above advances itself)
    if (c == '(') {
      ++enumParen_;
    } else if (c == ')') {
      if (enumParen_ > 0) --enumParen_;
    } else if (c == ',' && enumParen_ == 0) {
      finalizeEnumerator();
      enumSegStarted_ = false;
      return;
    } else if (!enumSegStarted_ && !isSpaceChar(c)) {
      // First non-blank character of an enumerator segment: its line is
      // the enumerator's line (the segment starts empty/blank, so the
      // 'empty' test alone would miss segments preceded by whitespace).
      enumSegStarted_ = true;
      enumSegLine_ = line_;
    }
    enumSeg_ += c;
  }

  void finalizeEnumerator() {
    std::string seg = normalizeWs(enumSeg_);
    // A space may precede the separating comma (`Mean , Min`); fold it.
    std::string folded;
    folded.reserve(seg.size());
    for (size_t i = 0; i < seg.size(); ++i) {
      if (seg[i] == ' ' && i + 1 < seg.size() && seg[i + 1] == ',') continue;
      folded += seg[i];
    }
    seg = std::move(folded);
    enumSeg_.clear();
    if (seg.empty()) return;
    // Name: the identifier before the first top-level '=' (if any).
    std::string name = seg;
    int paren = 0;
    for (size_t i = 0; i < seg.size(); ++i) {
      char c = seg[i];
      if (c == '(') ++paren;
      else if (c == ')') { if (paren > 0) --paren; }
      else if (c == '=' && paren == 0) {
        name = trim(seg.substr(0, i));
        break;
      }
    }
    bool ok = !name.empty() && !isKeywordWord(name);
    for (char ch : name) {
      if (!isIdentChar(static_cast<unsigned char>(ch))) {
        ok = false;
        break;
      }
    }
    if (!ok) {
      fail("unsupported enum initializer: " + seg);
      return;
    }
    DocInfo doc = consumeDoc();
    emit("enumerator", name, seg, enumSegLine_, doc);
  }
};

// ---------------------------------------------------------------------------
// Header discovery
// ---------------------------------------------------------------------------

// PRD §10.1 module map, bottom to top — mirrors MODULE_STACK in
// tools/laige-include-lint; update both in one change if the PRD changes
// the map (CORE-006).
const char* const kModules[] = {
    "laige-core", "laige-sim", "laige-render", "laige-assets", "laige-net",
    "laige-server", "laige-script", "laige-editor", "laige-sample",
};

// Collect the public headers: every .h under src/<module>/include
// (recursive). Returns false with err set on I/O trouble (CORE-008).
bool discoverHeaders(const std::string& root,
                     std::vector<std::string>& headers, std::string& err) {
  headers.clear();
  namespace fs = std::filesystem;
  std::error_code ec;
  if (!fs::is_directory(fs::path(root), ec)) {
    err = "root is not a directory: " + root;
    return false;
  }
  for (const char* const m : kModules) {
    fs::path inc = fs::path(root) / "src" / m / "include";
    if (!fs::is_directory(inc, ec)) continue;
    fs::recursive_directory_iterator it(inc, ec), end;
    if (ec) {
      err = "cannot iterate " + inc.generic_string() + ": " + ec.message();
      return false;
    }
    for (; it != end; it.increment(ec)) {
      if (ec) {
        err = "cannot iterate " + inc.generic_string() + ": " + ec.message();
        return false;
      }
      if (!it->is_regular_file(ec)) continue;
      if (it->path().extension() != ".h") continue;
      fs::path rel = fs::relative(it->path(), root, ec);
      if (ec) {
        err = "cannot relativize " + it->path().generic_string() + ": " +
              ec.message();
        return false;
      }
      headers.push_back(rel.generic_string());
    }
  }
  std::sort(headers.begin(), headers.end());
  return true;
}

// ---------------------------------------------------------------------------
// Manifest serialization (deterministic, NFR-13.4)
// ---------------------------------------------------------------------------

std::string serializeManifest(const std::vector<std::string>& headers,
                              const std::vector<Symbol>& symbols) {
  std::string out;
  out += "{\n";
  out += "  \"version\": 1,\n";
  out += "  \"generatedBy\": \"laige-api\",\n";
  out += "  \"headers\": [";
  for (size_t i = 0; i < headers.size(); ++i) {
    out += std::string(i == 0 ? "\n  " : ",\n  ") + "\"" + jsonEscape(headers[i]) + "\"";
  }
  if (!headers.empty()) out += "\n  ";
  out += "],\n";
  out += "  \"symbols\": [\n";
  for (size_t i = 0; i < symbols.size(); ++i) {
    const Symbol& s = symbols[i];
    out += "    {\"name\": \"" + jsonEscape(s.name) + "\"";
    out += ", \"kind\": \"" + jsonEscape(s.kind) + "\"";
    out += ", \"header\": \"" + jsonEscape(s.header) + "\"";
    out += ", \"line\": " + std::to_string(s.line);
    out += ", \"signature\": \"" + jsonEscape(s.signature) + "\"";
    out += ", \"summary\": " +
           (s.summary.empty() ? "null" : "\"" + jsonEscape(s.summary) + "\"");
    out += ", \"budget\": " +
           (s.budget.empty() ? "null" : "\"" + jsonEscape(s.budget) + "\"");
    out += ", \"experimental\": " + std::string(s.experimental ? "true" : "false");
    out += "}";
    if (i + 1 < symbols.size()) out += ",";
    out += "\n";
  }
  out += "  ]\n";
  out += "}\n";
  return out;
}

// ---------------------------------------------------------------------------
// --check: compare against the checked-in manifest
// ---------------------------------------------------------------------------

struct OldSymbol {
  std::string name, kind, header, signature, summary, budget;
  int line = 0;
  bool experimental = false;
};

// Parse a checked-in manifest and collect its symbol entries.
bool parseOldManifest(const std::string& text, std::vector<OldSymbol>& out,
                      std::string& err) {
  laige::Result<laige::JsonValue> parsed = laige::parseJson(text);
  if (parsed.isError()) {
    err = std::string("checked-in manifest is not valid JSON: ") +
          laige::errorText(parsed.error());
    return false;
  }
  const laige::JsonValue& root = parsed.value();
  const laige::JsonValue* syms = root.findMember("symbols");
  if (syms == nullptr || !syms->isArray()) {
    err = "checked-in manifest has no \"symbols\" array";
    return false;
  }
  for (const laige::JsonValue& el : syms->asArray()) {
    OldSymbol os;
    if (const laige::JsonValue* v = el.findMember("name")) {
      if (v->isString()) os.name = std::string(v->asString());
    }
    if (const laige::JsonValue* v = el.findMember("kind")) {
      if (v->isString()) os.kind = std::string(v->asString());
    }
    if (const laige::JsonValue* v = el.findMember("header")) {
      if (v->isString()) os.header = std::string(v->asString());
    }
    if (const laige::JsonValue* v = el.findMember("line")) {
      if (v->isNumber()) os.line = static_cast<int>(v->asNumber());
    }
    if (const laige::JsonValue* v = el.findMember("signature")) {
      if (v->isString()) os.signature = std::string(v->asString());
    }
    if (const laige::JsonValue* v = el.findMember("summary")) {
      if (v->isString()) os.summary = std::string(v->asString());
    }
    if (const laige::JsonValue* v = el.findMember("budget")) {
      if (v->isString()) os.budget = std::string(v->asString());
    }
    if (const laige::JsonValue* v = el.findMember("experimental")) {
      if (v->isBool()) os.experimental = v->asBool();
    }
    out.push_back(std::move(os));
  }
  return true;
}

int runCheck(const std::string& checkFile, const std::string& manifestText,
             const std::vector<Symbol>& symbols) {
  std::string oldText;
  std::string err;
  if (!readFile(checkFile, oldText, err)) {
    std::cout << "laige-api: error: " << err << std::endl;
    return 2;
  }
  // Line-ending normalization: the manifest is canonically LF (the
  // serializer emits LF; the repo's .gitattributes pins eol=lf on every
  // checkout). CRLF can reach a checked-in copy only as a Windows tooling
  // artifact (text-mode CMake file(WRITE) test fixtures, editor saves), so
  // normalize CRLF -> LF before the byte compare. A lone \r is NOT
  // normalized, so a genuinely corrupted file still fails (CORE-008).
  {
    std::string norm;
    norm.reserve(oldText.size());
    for (size_t i = 0; i < oldText.size(); ++i) {
      if (oldText[i] == '\r' && i + 1 < oldText.size() &&
          oldText[i + 1] == '\n') {
        norm += '\n';  // fold the CRLF pair to one LF
        ++i;          // the paired \n belongs to the fold - skip it
      } else {
        norm += oldText[i];
      }
    }
    oldText = std::move(norm);
  }
  if (oldText == manifestText) {
    std::cout << "laige-api: OK — " << checkFile
              << " is up to date (" << symbols.size() << " symbol(s) from the "
              << "current public headers)" << std::endl;
    return 0;
  }
  std::vector<OldSymbol> old;
  if (!parseOldManifest(oldText, old, err)) {
    std::cout << "laige-api: error: " << err << std::endl;
    return 2;
  }
  // Symbol-level diff (the byte mismatch is already established).
  auto keyOf = [](const std::string& n, const std::string& h, int l) {
    return n + "\x1f" + h + "\x1f" + std::to_string(l);
  };
  std::vector<std::pair<std::string, const OldSymbol*>> oldMap;
  for (const OldSymbol& os : old) {
    oldMap.push_back({keyOf(os.name, os.header, os.line), &os});
  }
  std::vector<std::string> newKeys;
  for (const Symbol& s : symbols) {
    newKeys.push_back(keyOf(s.name, s.header, s.line));
  }
  std::vector<std::string> added, removed, changed;
  for (const std::string& k : newKeys) {
    bool found = false;
    for (const auto& p : oldMap) {
      if (p.first == k) { found = true; break; }
    }
    if (!found) added.push_back(k);
  }
  for (const auto& p : oldMap) {
    bool found = false;
    for (const std::string& k : newKeys) {
      if (k == p.first) { found = true; break; }
    }
    if (!found) removed.push_back(p.first);
  }
  // Changed: same key, any field differs.
  for (const Symbol& s : symbols) {
    std::string k = keyOf(s.name, s.header, s.line);
    for (const auto& p : oldMap) {
      if (p.first != k) continue;
      const OldSymbol& os = *p.second;
      if (os.kind != s.kind || os.signature != s.signature ||
          os.summary != s.summary || os.budget != s.budget ||
          os.experimental != s.experimental) {
        std::vector<std::string> f;
        if (os.kind != s.kind) f.push_back("kind");
        if (os.signature != s.signature) f.push_back("signature");
        if (os.summary != s.summary) f.push_back("summary");
        if (os.budget != s.budget) f.push_back("budget");
        if (os.experimental != s.experimental) f.push_back("experimental");
        std::string fields;
        for (size_t i = 0; i < f.size(); ++i) {
          if (i) fields += ", ";
          fields += f[i];
        }
        size_t sep = k.find('\x1f');
        std::string nm = (sep == std::string::npos) ? k : k.substr(0, sep);
        changed.push_back(nm + " (" + fields + ")");
      }
      break;
    }
  }
  std::sort(added.begin(), added.end());
  std::sort(removed.begin(), removed.end());
  std::sort(changed.begin(), changed.end());
  int total = static_cast<int>(added.size() + removed.size() + changed.size());
  std::cout << "laige-api: ERROR — " << checkFile << " is stale (" << total
            << " difference(s) vs the regenerated manifest)" << std::endl;
  int shown = 0;
  for (const std::string& s : added) {
    size_t sep = s.find('\x1f');
    std::string name = (sep == std::string::npos) ? s : s.substr(0, sep);
    std::cout << "  + added: " << name << std::endl;
    if (++shown >= 20) break;
  }
  for (const std::string& s : removed) {
    size_t sep = s.find('\x1f');
    std::string name = (sep == std::string::npos) ? s : s.substr(0, sep);
    std::cout << "  - removed: " << name << std::endl;
    if (++shown >= 20) break;
  }
  for (const std::string& s : changed) {
    std::cout << "  ~ changed: " << s << std::endl;
    if (++shown >= 20) break;
  }
  if (total > shown) {
    std::cout << "  ... and " << (total - shown) << " more" << std::endl;
  }
  std::cout << "laige-api: regenerate with: ./build/bin/laige-api-scanner "
                "--root . --out laige-api.json" << std::endl;
  return 1;
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

void printUsage(std::ostream& os) {
  os << "Usage: laige-api-scanner --root REPO_ROOT (--out FILE | --check FILE)\n"
     << "\n"
     << "  --root REPO_ROOT   repository root (default: \".\")\n"
     << "  --out FILE         write the regenerated manifest to FILE\n"
     << "  --check FILE       compare the regenerated manifest with FILE:\n"
     << "                     exit 0 if identical (CRLF normalized to LF),\n"
     << "                     exit 1 if stale\n"
     << "\n"
     << "Exit codes: 0 = OK · 1 = stale manifest · 2 = error\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string root = ".";
  std::string outPath;
  std::string checkPath;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--root") {
      if (i + 1 >= argc) { printUsage(std::cerr); return 2; }
      root = argv[++i];
    } else if (a == "--out") {
      if (i + 1 >= argc) { printUsage(std::cerr); return 2; }
      outPath = argv[++i];
    } else if (a == "--check") {
      if (i + 1 >= argc) { printUsage(std::cerr); return 2; }
      checkPath = argv[++i];
    } else if (a == "--help" || a == "-h") {
      printUsage(std::cout);
      return 0;
    } else {
      std::cerr << "laige-api: error: unknown argument '" << a << "'"
                << std::endl;
      printUsage(std::cerr);
      return 2;
    }
  }
  bool hasOut = !outPath.empty();
  bool hasCheck = !checkPath.empty();
  if (hasOut == hasCheck) {
    std::cerr << "laige-api: error: exactly one of --out / --check is required"
              << std::endl;
    printUsage(std::cerr);
    return 2;
  }

  std::vector<std::string> headers;
  std::string err;
  if (!discoverHeaders(root, headers, err)) {
    std::cout << "laige-api: error: " << err << std::endl;
    return 2;
  }

  std::vector<Symbol> symbols;
  for (const std::string& h : headers) {
    std::string text;
    std::string path = root == "." ? h : root + "/" + h;
    if (!readFile(path, text, err)) {
      std::cout << "laige-api: error: " << err << std::endl;
      return 2;
    }
    FileScanner scanner(h, symbols);
    if (!scanner.scan(text)) {
      std::cout << "laige-api: error: " << h << ":" << scanner.errorLine()
                << ": " << scanner.error() << std::endl;
      return 2;
    }
  }

  std::string manifest = serializeManifest(headers, symbols);

  if (hasOut) {
    std::ofstream f(outPath, std::ios::binary | std::ios::trunc);
    if (!f) {
      std::cout << "laige-api: error: cannot write " << outPath << std::endl;
      return 2;
    }
    f << manifest;
    f.close();
    if (!f) {
      std::cout << "laige-api: error: write failed for " << outPath << std::endl;
      return 2;
    }
    std::cout << "laige-api: wrote " << outPath << " (" << symbols.size()
              << " symbol(s) from " << headers.size() << " header(s))"
              << std::endl;
    return 0;
  }
  return runCheck(checkPath, manifest, symbols);
}
