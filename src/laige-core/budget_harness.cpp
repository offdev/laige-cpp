// laige-core budget harness implementation (M0-CORE-08).
//
// Implements the cold-path half of the budget harness: loading and
// validating budgets.json (schema v1, via the bounded JSON parser —
// ADR 0003, no new dependency) and the budgetCheck pass/fail + AGENTS 12
// report formatting. The hot-path half (Histogram::record, TimeIt) lives
// header-only in include/laige/budget_harness.h.

#include "laige/budget_harness.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>

#include "laige/json.h"

namespace laige {
namespace {

// The file-size bound for budgets.json: identical to JsonOptions'
// default maxDocumentBytes (ADR 0003), named here (CORE-005) because the
// read loop enforces it before the whole file lands in memory.
constexpr std::size_t kBudgetsMaxDocumentBytes =
    static_cast<std::size_t>(1u) << 20;  // 1 MiB

// The schema version this reader accepts (ARCH-007: anything else is
// rejected explicitly, never guessed at).
constexpr int kBudgetsSchemaVersion = 1;

// Fixed entry field count for the v1 schema: name, metric, unit, target,
// measured, workload. A v1 entry object has exactly these six keys —
// more (unknown field) or fewer (missing field) is MalformedInput.
constexpr std::size_t kBudgetEntryFieldCount = 6;

const char* metricName(BudgetMetric metric) {
  switch (metric) {
    case BudgetMetric::Mean: return "mean";
    case BudgetMetric::Min: return "min";
    case BudgetMetric::Max: return "max";
    case BudgetMetric::P50: return "p50";
    case BudgetMetric::P95: return "p95";
    case BudgetMetric::P99: return "p99";
    default:
      assert(false && "unreachable: exhaustive BudgetMetric switch");
      return "unknown";
  }
}

double metricValue(BudgetMetric metric, const HistogramStats& s) {
  switch (metric) {
    case BudgetMetric::Mean: return s.mean;
    case BudgetMetric::Min: return s.min;
    case BudgetMetric::Max: return s.max;
    case BudgetMetric::P50: return s.p50;
    case BudgetMetric::P95: return s.p95;
    case BudgetMetric::P99: return s.p99;
    default:
      assert(false && "unreachable: exhaustive BudgetMetric switch");
      return std::nan("");
  }
}

// Locale-free rendering of a double for the report: at most 6 significant
// digits (%.6g, "C" locale — exact for every double up to 6 digits,
// plenty for ms/us budget numbers and byte counts alike). NaN/inf render
// as plain "nan"/"inf" text so the report stays greppable (LOG-001).
void formatDouble(std::string& out, double value) {
  char buf[32];
  if (std::isnan(value)) {
    std::snprintf(buf, sizeof(buf), "nan");
  } else if (std::isinf(value)) {
    std::snprintf(buf, sizeof(buf), value > 0.0 ? "inf" : "-inf");
  } else {
    std::snprintf(buf, sizeof(buf), "%.6g", value);
  }
  out += buf;
}

// The stable report layout (docs/api/budget_harness.md documents it as
// the machine-greppable contract):
//
//   budget=<name> result=<PASS|FAIL|NO_SAMPLES> metric=<m> unit=<u>
//     after=<v> before=<v> target=<v>
//     stats: n=<n> min=<v> mean=<v> p50=<v> p95=<v> p99=<v> max=<v>
//     context: workload=<s> build=<s> machine=<s> warmup=<n>
std::string buildReport(const BudgetEntry& entry, const char* outcome,
                        double measured, const HistogramStats& s,
                        const BudgetReportContext& ctx) {
  std::string r;
  r += "budget=";
  r += entry.name;
  r += " result=";
  r += outcome;
  r += " metric=";
  r += metricName(entry.metric);
  r += " unit=";
  r += entry.unit;
  r += "\n  after=";
  formatDouble(r, measured);
  r += " before=";
  formatDouble(r, entry.measured);
  r += " target=";
  formatDouble(r, entry.target);
  r += "\n  ";
  r += formatStatsLine(s);
  r += "\n  context: workload=";
  r += (ctx.workload != nullptr ? ctx.workload : "");
  r += " build=";
  r += (ctx.build != nullptr ? ctx.build : "");
  r += " machine=";
  r += (ctx.machine != nullptr ? ctx.machine : "");
  r += " warmup=";
  r += std::to_string(ctx.warmup);
  r += "\n";
  return r;
}

// --- budgets.json schema v1 validation -------------------------------------

// Stable id charset for budget names (LOG-001-style machine-greppable id).
bool isSnakeCase(std::string_view s) {
  if (s.empty()) return false;
  for (const char c : s) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '_';
    if (!ok) return false;
  }
  return true;
}

// Unit charset: an identifier (ms, draw_calls, allocs_per_frame, ...).
bool isUnitIdentifier(std::string_view s) {
  if (s.empty()) return false;
  for (const char c : s) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_';
    if (!ok) return false;
  }
  return true;
}

bool metricFromText(std::string_view text, BudgetMetric& out) {
  if (text == "mean") {
    out = BudgetMetric::Mean;
    return true;
  }
  if (text == "min") {
    out = BudgetMetric::Min;
    return true;
  }
  if (text == "max") {
    out = BudgetMetric::Max;
    return true;
  }
  if (text == "p50") {
    out = BudgetMetric::P50;
    return true;
  }
  if (text == "p95") {
    out = BudgetMetric::P95;
    return true;
  }
  if (text == "p99") {
    out = BudgetMetric::P99;
    return true;
  }
  return false;
}

// target/measured contract: a JSON number that is finite and >= 0.
// (The parser stores a well-formed overflow token such as 1e999 as +inf —
// a valid parse result that the schema rejects, ADR 0003.)
bool isNonNegativeFinite(const JsonValue& v) {
  return v.isNumber() && std::isfinite(v.asNumber()) && v.asNumber() >= 0.0;
}

Status parseBudgetEntry(const JsonValue& obj,
                        const std::vector<BudgetEntry>& existing,
                        BudgetEntry& out) {
  if (!obj.isObject() || obj.asObject().size() != kBudgetEntryFieldCount)
    return Status(ErrorCode::MalformedInput);

  const JsonValue *name = nullptr, *metric = nullptr, *unit = nullptr,
      *target = nullptr, *measured = nullptr, *workload = nullptr;
  for (const auto& member : obj.asObject()) {
    if (member.first == "name")
      name = &member.second;
    else if (member.first == "metric")
      metric = &member.second;
    else if (member.first == "unit")
      unit = &member.second;
    else if (member.first == "target")
      target = &member.second;
    else if (member.first == "measured")
      measured = &member.second;
    else if (member.first == "workload")
      workload = &member.second;
    else
      return Status(ErrorCode::MalformedInput);  // unknown field
  }
  // A size-6 object whose six distinct keys all routed above holds
  // exactly the v1 field set; this guard documents that and catches a
  // future edit that routes a seventh name without bumping the count.
  if (name == nullptr || metric == nullptr || unit == nullptr ||
      target == nullptr || measured == nullptr || workload == nullptr)
    return Status(ErrorCode::MalformedInput);

  if (!name->isString() || !isSnakeCase(name->asString()))
    return Status(ErrorCode::MalformedInput);
  out.name = std::string(name->asString());

  for (const BudgetEntry& e : existing)
    if (e.name == out.name)
      return Status(ErrorCode::MalformedInput);  // duplicate name

  if (!metric->isString() || !metricFromText(metric->asString(), out.metric))
    return Status(ErrorCode::MalformedInput);

  if (!unit->isString() || !isUnitIdentifier(unit->asString()))
    return Status(ErrorCode::MalformedInput);
  out.unit = std::string(unit->asString());

  if (!isNonNegativeFinite(*target))
    return Status(ErrorCode::MalformedInput);
  out.target = target->asNumber();

  if (!isNonNegativeFinite(*measured))
    return Status(ErrorCode::MalformedInput);
  out.measured = measured->asNumber();

  if (!workload->isString() || workload->asString().empty())
    return Status(ErrorCode::MalformedInput);
  out.workload = std::string(workload->asString());

  return Status();
}

Status parseBudgetsTable(const JsonValue& root,
                         std::vector<BudgetEntry>& out) {
  if (!root.isObject()) return Status(ErrorCode::MalformedInput);

  for (const auto& member : root.asObject()) {
    if (member.first != "version" && member.first != "description" &&
        member.first != "budgets")
      return Status(ErrorCode::MalformedInput);  // unknown top-level field
  }

  const JsonValue* version = root.findMember("version");
  if (version == nullptr || !version->isNumber() ||
      version->asNumber() != static_cast<double>(kBudgetsSchemaVersion))
    return Status(ErrorCode::MalformedInput);  // unsupported version

  if (const JsonValue* description = root.findMember("description"))
    if (!description->isString())
      return Status(ErrorCode::MalformedInput);

  const JsonValue* budgets = root.findMember("budgets");
  if (budgets == nullptr || !budgets->isArray())
    return Status(ErrorCode::MalformedInput);

  out.reserve(budgets->asArray().size());
  for (const JsonValue& element : budgets->asArray()) {
    BudgetEntry entry;
    const Status s = parseBudgetEntry(element, out, entry);
    if (s.isError()) return s;
    out.push_back(std::move(entry));
  }
  return Status();
}

std::string readWholeFile(const std::string& path, Status& ioStatus) {
  // MSVC's plain fopen is deprecated (C4996, fatal under the engine /WX
  // policy); _fsopen with _SH_DENYNO matches the logging file sink's
  // platform boundary (CPP-009).
#if defined(_MSC_VER)
  std::FILE* f = ::_fsopen(path.c_str(), "rb", _SH_DENYNO);
#else
  std::FILE* f = std::fopen(path.c_str(), "rb");
#endif
  if (f == nullptr) {
    ioStatus = Status(ErrorCode::IoError);
    return {};
  }
  std::string text;
  char buf[8192];
  while (true) {
    const std::size_t r = std::fread(buf, 1, sizeof(buf), f);
    text.append(buf, r);
    if (r < sizeof(buf)) break;  // EOF (or error — checked below)
    if (text.size() > kBudgetsMaxDocumentBytes) {
      std::fclose(f);
      ioStatus = Status(ErrorCode::MalformedInput);  // over the size bound
      return {};
    }
  }
  const bool errored = std::ferror(f) != 0;
  std::fclose(f);
  if (errored) {
    ioStatus = Status(ErrorCode::IoError);
    return {};
  }
  return text;
}

}  // namespace

std::string formatStatsLine(const HistogramStats& s) {
  std::string r = "stats: n=";
  r += std::to_string(s.n);
  r += " min=";
  formatDouble(r, s.min);
  r += " mean=";
  formatDouble(r, s.mean);
  r += " p50=";
  formatDouble(r, s.p50);
  r += " p95=";
  formatDouble(r, s.p95);
  r += " p99=";
  formatDouble(r, s.p99);
  r += " max=";
  formatDouble(r, s.max);
  return r;
}

BudgetCheckResult budgetCheck(const BudgetEntry& entry,
                              const Histogram& histogram,
                              const BudgetReportContext& context) {
  BudgetCheckResult out;
  out.target = entry.target;
  out.before = entry.measured;

  const HistogramStats s = histogram.stats();
  if (s.n == 0) {
    // A workload that recorded nothing is a broken harness: fail loudly
    // (CORE-008), never read NaN statistics as "passed".
    out.passed = false;
    out.measured = std::nan("");
    out.report = buildReport(entry, "NO_SAMPLES", out.measured, s, context);
    return out;
  }

  const double measured = metricValue(entry.metric, s);
  out.measured = measured;
  // Every PRD 8.1 budget is an at-most upper bound. target == 0 is the
  // hard-zero budget (e.g. sim heap allocations per frame), not "not
  // set" — the "not yet measured" marker lives in entry.measured.
  out.passed = (entry.target > 0.0) ? (measured <= entry.target)
                                    : (measured == 0.0);
  out.report =
      buildReport(entry, out.passed ? "PASS" : "FAIL", measured, s, context);
  return out;
}

Result<BudgetTable, ErrorCode> loadBudgets(std::string_view path) {
  const std::string pathString(path);  // fopen needs a NUL-terminated path

  Status ioStatus;
  const std::string text = readWholeFile(pathString, ioStatus);
  if (ioStatus.isError())
    return Result<BudgetTable, ErrorCode>(ioStatus.error());

  const Result<JsonValue> parsed = parseJson(text);
  if (parsed.isError())
    return Result<BudgetTable, ErrorCode>(parsed.error());

  BudgetTable table;
  const Status schema = parseBudgetsTable(parsed.value(), table.entries_);
  if (schema.isError())
    return Result<BudgetTable, ErrorCode>(schema.error());

  return Result<BudgetTable, ErrorCode>(std::move(table));
}

}  // namespace laige
