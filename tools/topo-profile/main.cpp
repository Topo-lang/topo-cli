// topo-profile — spans + sampling trace CLI.
//
// Two trace modes ship today:
//
//   spans   (default) — spawn a host binary linked against libtopo-observe,
//                       drain its NDJSON span output and re-emit under the
//                       trace schema:
//
//                         { "spans": [ { "name":..., "stage":...,
//                                        "pipeline":..., "start_ns":...,
//                                        "end_ns":..., "tid":...,
//                                        "backend":..., "annotations":{} } ] }
//
//                       Stage / pipeline are parsed out of the span name
//                       (the runtime encodes them in the convention
//                       `pipeline::<name>::stage<N>`).
//
//   sampling           — convert a checked-in or recorded sampling profile
//                       (stackcollapse-folded text, JFR-NDJSON, speedscope
//                       JSON, or V8 `.cpuprofile`) into the trace
//                       sampling sub-schema. Requires exactly one of
//                       --sampling-input / --jfr-input /
//                       --speedscope-input / --cpuprofile-input; no host
//                       binary is spawned. This keeps CTest reproducible
//                       without `perf` / `xctrace` / a JVM / `node
//                       --cpu-prof` at test time. When the real recorders
//                       land, they will pipe stdout into the same parsers.
//
//   hybrid             — spans + sampling in ONE trace
//                       document. Spawns <binary> for exact span
//                       boundaries (identical to spans mode) AND ingests
//                       one sampling input file for statistical
//                       in-span hotspots (identical to sampling mode).
//                       Requires both a <binary> positional and exactly
//                       one sampling-input flag. No perf/xctrace spawn
//                       at test time — checked-in fixtures only, same as
//                       sampling mode.
//
// Exit codes:
//   0  success
//   1  CLI / IO / parse error
//   2  unsupported mode in this build
//   3  target binary exited non-zero (still produces output if spans were
//      captured before the failure)

#include "topo/Platform/Process.h"
#include "topo/Profile/AsyncProfilerConverter.h"
#include "topo/Profile/CProfileNdjsonConverter.h"
#include "topo/Profile/SysMonitoringNdjsonConverter.h"
#include "topo/Profile/CpuProfileConverter.h"
#include "topo/Profile/JfrNdjsonConverter.h"
#include "topo/Profile/SamplingConverter.h"
#include "topo/Profile/SpeedscopeConverter.h"

#include "SourceMapResolver.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using nlohmann::json;

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;
constexpr int kExitUnsupportedMode = 2;
constexpr int kExitTargetFailed = 3;

void printUsage(std::FILE* out) {
    std::fprintf(out,
                 "Usage:\n"
                 "  topo-profile trace <binary> [--mode spans] [--backend llvm]\n"
                 "                              [-- <target-args>...]\n"
                 "  topo-profile trace --mode sampling --sampling-input <file>\n"
                 "                              [--backend cpp]\n"
                 "  topo-profile trace --mode sampling --jfr-input <file>\n"
                 "                              [--backend java]\n"
                 "  topo-profile trace --mode sampling --speedscope-input <file>\n"
                 "                              [--backend python]\n"
                 "  topo-profile trace --mode sampling --cpuprofile-input <file>\n"
                 "                              [--backend typescript]\n"
                 "  topo-profile trace --mode sampling --cprofile-input <file>\n"
                 "                              [--backend python]\n"
                 "\n"
                 "spans (default): spawn the target (must link libtopo-observe),\n"
                 "drain NDJSON span output, transform to the unified trace schema.\n"
                 "\n"
                 "sampling: convert a sampling profile to the trace\n"
                 "sampling sub-schema. Source-format selected by which input\n"
                 "flag is given (mutually exclusive):\n"
                 "  --sampling-input <file>     stackcollapse-folded text\n"
                 "                              (perf-script / xctrace export)\n"
                 "  --jfr-input <file>          NDJSON-shaped JFR events (JVM\n"
                 "                              backend; one event per line)\n"
                 "  --speedscope-input <file>   speedscope JSON document\n"
                 "                              (py-spy default; Chromium /\n"
                 "                              Firefox profiler exports)\n"
                 "  --cpuprofile-input <file>   V8 `.cpuprofile` JSON\n"
                 "                              (Chrome DevTools / `node\n"
                 "                              --cpu-prof` output)\n"
                 "  --cprofile-input <file>     cProfile NDJSON (Python stdlib\n"
                 "                              deterministic profiler; emitted\n"
                 "                              by topo_profile_python.cprofile_harness)\n"
                 "  --async-profiler-input <f>  async-profiler `collapsed`\n"
                 "                              text (JVM off-CPU / wall /\n"
                 "                              alloc; --mode=async-profiler)\n"
                 "  --sys-monitoring-input <f>  PY_START/PY_RETURN NDJSON\n"
                 "                              (PEP 669 sys.monitoring,\n"
                 "                              Python 3.12+;\n"
                 "                              --mode=sys-monitoring)\n"
                 "  --jfr-binary-input <file>   `.jfr` recording; converted to\n"
                 "                              NDJSON via the topo-profile-\n"
                 "                              jvm-jfr bridge jar, then routed\n"
                 "                              through the --jfr-input path\n"
                 "  --jfr-bridge-jar <path>     override bridge jar location\n"
                 "                              (also honours\n"
                 "                              $TOPO_JFR_BRIDGE_JAR)\n"
                 "\n"
                 "async-profiler: convert async-profiler `collapsed` text\n"
                 "to the trace sampling sub-schema (JVM off-CPU /\n"
                 "wall-clock / allocation profiling, the cases JFR sampling\n"
                 "does not cover). Requires --async-profiler-input <file>\n"
                 "and selects what was sampled via --type:\n"
                 "  --type wall   off-CPU + on-CPU wall-clock (default)\n"
                 "  --type alloc  allocation-site sampling\n"
                 "  --type cpu    on-CPU cycles\n"
                 "  topo-profile trace --mode async-profiler\n"
                 "                     --async-profiler-input <file>\n"
                 "                     --type wall [--backend java]\n"
                 "\n"
                 "sys-monitoring: convert a PEP 669 sys.monitoring\n"
                 "PY_START/PY_RETURN NDJSON stream (Python 3.12+) to the\n"
                 "the trace sampling sub-schema. The harness gates on\n"
                 "the interpreter version; a checked-in NDJSON fixture\n"
                 "keeps tests reproducible without a live 3.12+ runtime.\n"
                 "  python -m topo_profile_python.sys_monitoring_harness\n"
                 "         <target.py> > events.ndjson\n"
                 "  topo-profile trace --mode sys-monitoring\n"
                 "                     --sys-monitoring-input events.ndjson\n"
                 "                     [--backend python]\n"
                 "\n"
                 "hybrid: spawn <binary> for exact span boundaries AND\n"
                 "ingest one sampling-input file for in-span hotspots; emit\n"
                 "both segments in a single unified trace document.\n"
                 "Requires <binary> and exactly one sampling-input flag.\n"
                 "  topo-profile trace <binary> --mode hybrid\n"
                 "                              --sampling-input <file>\n"
                 "                              [--backend cpp] [-- <args>...]\n");
}

struct CliConfig {
    std::string subcommand;
    // For mode=spans, target is the binary to spawn. For mode=sampling,
    // target is unused (the input file path lives in samplingInput).
    std::string target;
    std::string mode = "spans";
    std::string backend = "llvm";
    std::vector<std::string> targetArgs;
    // Path to a stackcollapse-folded file consumed when
    // --mode=sampling. One of samplingInput / jfrInput / speedscopeInput is
    // required in that mode; the three are mutually exclusive.
    std::optional<std::string> samplingInput;
    // Path to a JFR-NDJSON file. Mutually exclusive with
    // samplingInput / speedscopeInput.
    std::optional<std::string> jfrInput;
    // Path to a speedscope JSON document. Mutually exclusive
    // with samplingInput / jfrInput.
    std::optional<std::string> speedscopeInput;
    // Path to a V8 `.cpuprofile` JSON document. Mutually
    // exclusive with the other sampling-input flags.
    std::optional<std::string> cpuprofileInput;
    // Path to a cProfile NDJSON document
    // (emitted by `topo_profile_python.cprofile_harness`). Mutually
    // exclusive with the other sampling-input flags.
    std::optional<std::string> cprofileInput;
    // Path to an async-profiler `collapsed` text file. Consumed
    // by --mode=async-profiler. Mutually exclusive with all other input
    // flags. async-profiler is a native agent the user installs per
    // platform; like the JFR `.jfr` path, the checked-in fixture keeps
    // CTest reproducible without spawning the agent at test time.
    std::optional<std::string> asyncProfilerInput;
    // async-profiler sampling event type: wall | alloc | cpu.
    // Recorded for diagnostics + stamped into the output; the collapsed
    // wire shape is uniform across types so a single converter handles all.
    std::string asyncProfilerType = "wall";
    // Path to a `sys.monitoring` (PEP 669, Py≥3.12) PY_START/
    // PY_RETURN NDJSON file, produced by the harness
    // `topo_profile_python.sys_monitoring_harness`. Consumed by
    // --mode=sys-monitoring. Mutually exclusive with all other input
    // flags; like the cProfile path, a checked-in NDJSON fixture keeps
    // CTest reproducible without requiring a live Python 3.12+ at test
    // time (the harness itself gates on the interpreter version).
    std::optional<std::string> sysMonitoringInput;
    // Path to a `.jfr` binary file. Distinct from --jfr-input
    // (which already expects the NDJSON intermediate produced by the bridge).
    // Mutually exclusive with all other input flags.
    std::optional<std::string> jfrBinaryInput;
    // Optional override for the bridge jar location; falls back to
    // $TOPO_JFR_BRIDGE_JAR, then the build-tree baked-in path.
    std::optional<std::string> jfrBridgeJar;
    // Directories to search for `.js.map` source maps when
    // resolving V8 frames back to original sources. Repeatable; each
    // occurrence appends. Meaningful only with --cpuprofile-input.
    std::vector<std::string> sourcemapSearchRoots;
};

bool parseCli(int argc, char** argv, CliConfig& cfg, std::string& err) {
    if (argc < 2) { err = "missing subcommand"; return false; }
    cfg.subcommand = argv[1];
    if (cfg.subcommand != "trace") {
        err = "only the 'trace' subcommand is implemented (got '" +
              cfg.subcommand + "')";
        return false;
    }
    // The binary positional is optional: spans-mode requires it (validated
    // after parsing), sampling-mode ignores it. We parse argv[2..] as flags
    // and accept the first non-flag arg as the target.
    bool inTargetArgs = false;
    bool sawTarget = false;
    auto eat = [&](int& i, const std::string& a, const std::string& flag,
                   std::string& dest) -> bool {
        if (a == flag && i + 1 < argc) { dest = argv[++i]; return true; }
        if (a.rfind(flag + "=", 0) == 0) {
            dest = a.substr(flag.size() + 1);
            return true;
        }
        return false;
    };
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (inTargetArgs) { cfg.targetArgs.push_back(std::move(a)); continue; }
        if (a == "--") { inTargetArgs = true; continue; }
        if (eat(i, a, "--mode", cfg.mode)) continue;
        if (eat(i, a, "--backend", cfg.backend)) continue;
        std::string samplingInputBuf;
        if (eat(i, a, "--sampling-input", samplingInputBuf)) {
            cfg.samplingInput = samplingInputBuf;
            continue;
        }
        std::string jfrInputBuf;
        if (eat(i, a, "--jfr-input", jfrInputBuf)) {
            cfg.jfrInput = jfrInputBuf;
            continue;
        }
        std::string speedscopeInputBuf;
        if (eat(i, a, "--speedscope-input", speedscopeInputBuf)) {
            cfg.speedscopeInput = speedscopeInputBuf;
            continue;
        }
        std::string cpuprofileInputBuf;
        if (eat(i, a, "--cpuprofile-input", cpuprofileInputBuf)) {
            cfg.cpuprofileInput = cpuprofileInputBuf;
            continue;
        }
        std::string cprofileInputBuf;
        if (eat(i, a, "--cprofile-input", cprofileInputBuf)) {
            cfg.cprofileInput = cprofileInputBuf;
            continue;
        }
        std::string sysMonitoringInputBuf;
        if (eat(i, a, "--sys-monitoring-input", sysMonitoringInputBuf)) {
            cfg.sysMonitoringInput = sysMonitoringInputBuf;
            continue;
        }
        std::string asyncProfilerInputBuf;
        if (eat(i, a, "--async-profiler-input", asyncProfilerInputBuf)) {
            cfg.asyncProfilerInput = asyncProfilerInputBuf;
            continue;
        }
        if (eat(i, a, "--type", cfg.asyncProfilerType)) continue;
        std::string jfrBinaryInputBuf;
        if (eat(i, a, "--jfr-binary-input", jfrBinaryInputBuf)) {
            cfg.jfrBinaryInput = jfrBinaryInputBuf;
            continue;
        }
        std::string jfrBridgeJarBuf;
        if (eat(i, a, "--jfr-bridge-jar", jfrBridgeJarBuf)) {
            cfg.jfrBridgeJar = jfrBridgeJarBuf;
            continue;
        }
        std::string sourcemapRootBuf;
        if (eat(i, a, "--sourcemap-search-root", sourcemapRootBuf)) {
            cfg.sourcemapSearchRoots.push_back(std::move(sourcemapRootBuf));
            continue;
        }
        if (a == "-h" || a == "--help") return false;
        if (a.empty() || a[0] == '-') {
            err = "unknown argument: " + a;
            return false;
        }
        if (sawTarget) {
            err = "unexpected positional argument '" + a +
                  "' (only one <binary> allowed before `--`)";
            return false;
        }
        cfg.target = std::move(a);
        sawTarget = true;
    }
    return true;
}

// Decode `pipeline::<pipename>::stage<N>` into (pipename, stageN). Leaves
// both fields empty if the convention doesn't match — the caller then
// omits them from the JSON output, which is preferable to inventing values.
struct StagePipe {
    std::optional<std::string> pipeline;
    std::optional<int64_t> stage;
};

StagePipe parseStagePipeline(const std::string& name) {
    StagePipe out;
    const std::string prefix = "pipeline::";
    if (name.rfind(prefix, 0) != 0) return out;
    size_t restStart = prefix.size();
    size_t nameEnd = name.find("::", restStart);
    if (nameEnd == std::string::npos) return out;
    std::string pipe = name.substr(restStart, nameEnd - restStart);
    std::string tail = name.substr(nameEnd + 2);
    const std::string stagePrefix = "stage";
    if (tail.rfind(stagePrefix, 0) != 0) return out;
    std::string stageStr = tail.substr(stagePrefix.size());
    // stage<N>; tolerate trailing characters by stopping at the first non-digit.
    size_t digitEnd = 0;
    while (digitEnd < stageStr.size() &&
           std::isdigit(static_cast<unsigned char>(stageStr[digitEnd]))) {
        ++digitEnd;
    }
    if (digitEnd == 0) return out;
    try {
        out.stage = std::stoll(stageStr.substr(0, digitEnd));
    } catch (const std::exception&) {
        return out;
    }
    out.pipeline = std::move(pipe);
    return out;
}

// Read a single line (terminated by '\n') from a PipedProcess. Returns
// true if a line was read; false on EOF/error. The line excludes the '\n'.
bool readLine(topo::platform::PipedProcess& p, std::string& line) {
    line.clear();
    while (true) {
        int c = p.readByte();
        if (c < 0) return !line.empty();
        char ch = static_cast<char>(c);
        if (ch == '\n') return true;
        line.push_back(ch);
    }
}

// Read an optional ns timestamp/id field that may legitimately arrive as
// either a JSON integer or a JSON float (some emitters serialize ns counts
// through a double). A float must NOT be silently dropped to the `0` default
// (that loses real data); convert it deterministically by rounding to the
// nearest int64, clamping at the int64 bounds so an out-of-range double cannot
// invoke the UB of a double→int64 cast. A non-numeric or absent field returns
// `fallback`.
int64_t readOptionalNsField(const json& src, const char* key, int64_t fallback) {
    auto it = src.find(key);
    if (it == src.end()) return fallback;
    if (it->is_number_integer()) return it->get<int64_t>();
    if (it->is_number_float()) {
        double v = it->get<double>();
        if (std::isnan(v)) return fallback;  // NaN has no deterministic int64
        // std::llround on a value past the int64 range is UB; clamp first.
        constexpr double kMax =
            static_cast<double>(std::numeric_limits<int64_t>::max());
        constexpr double kMin =
            static_cast<double>(std::numeric_limits<int64_t>::min());
        if (v >= kMax) return std::numeric_limits<int64_t>::max();
        if (v <= kMin) return std::numeric_limits<int64_t>::min();
        return static_cast<int64_t>(std::llround(v));
    }
    // Wrong-typed (string / bool / object) field: fall back rather than throw.
    return fallback;
}

// Transform a single libtopo-observe NDJSON record into a trace
// span object. Returns std::nullopt for lines that don't parse as the
// expected shape (so the caller skips non-span output like assertions
// or user prints).
std::optional<json> transformSpan(const std::string& raw,
                                  const std::string& backend) {
    json src;
    try {
        src = json::parse(raw);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (!src.is_object()) return std::nullopt;
    if (!src.contains("name") || !src["name"].is_string()) return std::nullopt;
    if (!src.contains("duration_ns") || !src["duration_ns"].is_number_integer()) {
        return std::nullopt;
    }

    std::string name = src["name"].get<std::string>();
    int64_t durationNs = src["duration_ns"].get<int64_t>();
    // ts_ns / thread_id are optional. value() forwards to get<int64_t>() when
    // the key is PRESENT with a non-integer type, which throws json::type_error
    // (no exception barrier up to main). readOptionalNsField type-checks before
    // reading — mirroring the duration_ns guard above — so a wrong-typed field
    // defaults to 0; a FLOAT field is converted deterministically (rounded,
    // clamped) rather than silently truncated to 0.
    int64_t tsNs = readOptionalNsField(src, "ts_ns", 0);
    int64_t tid = readOptionalNsField(src, "thread_id", 0);

    json out = json::object();
    out["name"] = name;
    StagePipe sp = parseStagePipeline(name);
    if (sp.stage) out["stage"] = *sp.stage;
    if (sp.pipeline) out["pipeline"] = *sp.pipeline;
    // libtopo-observe currently records the end of each span; recover the
    // start as end - duration. Both are absolute ns timestamps from the
    // runtime's steady clock, so they are comparable within a single run.
    out["start_ns"] = tsNs;
    // tsNs + durationNs on two int64 values can overflow — signed overflow is
    // UB, not a wraparound. Use a checked add and saturate to INT64_MAX/MIN on
    // overflow so an extreme (or crafted) duration_ns cannot trip UB.
    int64_t endNs = 0;
    if (__builtin_add_overflow(tsNs, durationNs, &endNs)) {
        endNs = durationNs >= 0 ? std::numeric_limits<int64_t>::max()
                                : std::numeric_limits<int64_t>::min();
    }
    out["end_ns"] = endNs;
    out["tid"] = tid;
    out["backend"] = backend;
    out["annotations"] = json::object();
    return out;
}

// Recognize a reusable pass-event NDJSON record and route
// it into the top-level `pass_events` object. The wire is defined in
// topo-llvm/runtime/include/topo/rt/pass_event_rt.h:
//
//   {"kind":"pass_event","pass":"<Pass>","from":"<v>","to":"<v>",
//    "subject":"<name>"?,"bytes":<int64>?,"ts_ns":<int64>}
//
// Records are distinguished from libtopo-observe spans purely by the
// "kind":"pass_event" discriminator (spans never carry "kind"), so the
// two record types can interleave on the same stdout stream in any
// order. On a match the routed event {ts_ns,from,to,subject?,bytes?} is
// appended to passEvents["<Pass>"]; non-matching lines return false so
// the caller can still try transformSpan().
bool routePassEvent(const std::string& raw, json& passEvents) {
    json src;
    try {
        src = json::parse(raw);
    } catch (const std::exception&) {
        return false;
    }
    if (!src.is_object()) return false;
    if (!src.contains("kind") || !src["kind"].is_string()) return false;
    if (src["kind"].get<std::string>() != "pass_event") return false;
    if (!src.contains("pass") || !src["pass"].is_string()) return false;

    std::string pass = src["pass"].get<std::string>();
    json ev = json::object();
    // ts_ns / from / to are optional. value() throws json::type_error when the
    // key is present with a non-matching type (e.g. "ts_ns":"oops" or
    // "from":123); a crafted pass-event line on the target's stdout must be
    // skipped, not abort the process. readOptionalNsField type-checks before
    // reading and converts a float ts_ns deterministically (same contract as
    // the span path) instead of silently dropping it to 0.
    ev["ts_ns"] = readOptionalNsField(src, "ts_ns", 0);
    ev["from"] = (src.contains("from") && src["from"].is_string())
                     ? src["from"].get<std::string>()
                     : std::string{};
    ev["to"] = (src.contains("to") && src["to"].is_string())
                   ? src["to"].get<std::string>()
                   : std::string{};
    // `subject` is optional on the wire; preserve "omit rather than
    // invent" — only surface it when the producer set it.
    if (src.contains("subject") && src["subject"].is_string() &&
        !src["subject"].get<std::string>().empty()) {
        ev["subject"] = src["subject"].get<std::string>();
    }
    // `bytes` (LifetimeArenaPass arena size) is
    // OPTIONAL on the wire and only set by topo_pass_event_emit_sized().
    // Surface it only when the producer emitted a numeric value, so
    // AdaptiveDispatch events (no "bytes" key) are unaffected.
    if (src.contains("bytes") && src["bytes"].is_number_integer()) {
        ev["bytes"] = src["bytes"].get<int64_t>();
    }

    if (!passEvents.contains(pass)) passEvents[pass] = json::array();
    passEvents[pass].push_back(std::move(ev));
    return true;
}

// Resolve the JFR bridge jar location. Search order:
//   1. explicit --jfr-bridge-jar CLI override
//   2. $TOPO_JFR_BRIDGE_JAR env var
//   3. baked-in build path (TOPO_JFR_BRIDGE_JAR_DEFAULT) — only present on
//      builds where Gradle/Java were available at configure time
//
// `searched` is populated with the absolute paths we tried so a diagnostic
// can list them when none exist.
std::optional<std::string> resolveJfrBridgeJar(
    const CliConfig& cfg, std::vector<std::string>& searched) {
    auto exists = [](const std::string& p) {
        return !p.empty() && std::filesystem::exists(p);
    };
    if (cfg.jfrBridgeJar) {
        searched.push_back("--jfr-bridge-jar=" + *cfg.jfrBridgeJar);
        if (exists(*cfg.jfrBridgeJar)) return *cfg.jfrBridgeJar;
        return std::nullopt;
    }
    if (const char* env = std::getenv("TOPO_JFR_BRIDGE_JAR"); env && *env) {
        std::string p = env;
        searched.push_back("$TOPO_JFR_BRIDGE_JAR=" + p);
        if (exists(p)) return p;
    } else {
        searched.push_back("$TOPO_JFR_BRIDGE_JAR (unset)");
    }
#ifdef TOPO_JFR_BRIDGE_JAR_DEFAULT
    {
        std::string p = TOPO_JFR_BRIDGE_JAR_DEFAULT;
        searched.push_back("baked-in: " + p);
        if (exists(p)) return p;
    }
#else
    searched.push_back("baked-in: (not configured at build time — JDK absent)");
#endif
    return std::nullopt;
}

// Resolve a `java` executable. We prefer $JAVA_HOME/bin/java because the
// surrounding fixtures (CMake-driven) bake one in; fall back to `java` on
// PATH so casual invocation outside of a Topo build still works.
std::string resolveJavaExecutable() {
    if (const char* jh = std::getenv("JAVA_HOME"); jh && *jh) {
        std::filesystem::path candidate =
            std::filesystem::path(jh) / "bin" / "java";
        if (std::filesystem::exists(candidate)) return candidate.string();
    }
    return "java";
}

// Spawn the bridge jar with the given .jfr path. On success, returns true and
// fills `ndjson` with the bridge's stdout. On failure, returns false and
// fills `err` with a diagnostic that includes the bridge's stderr.
bool runJfrBridge(const std::string& javaExe,
                  const std::string& jar,
                  const std::string& jfrInput,
                  std::string& ndjson,
                  std::string& err) {
    std::vector<std::string> args = {"-jar", jar, jfrInput};
    auto res = topo::platform::runProcessCaptureWithTimeout(
        javaExe, args, /*timeoutMs=*/120000, /*verbose=*/false);
    if (res.exitCode != 0) {
        std::ostringstream o;
        o << "JFR bridge exited with code " << res.exitCode;
        if (!res.stderrOutput.empty()) {
            o << "\nstderr: " << res.stderrOutput;
        }
        err = o.str();
        return false;
    }
    ndjson = std::move(res.stdoutOutput);
    return true;
}

// ---------------------------------------------------------------------------
// Reusable building blocks. spans-mode, sampling-mode and hybrid-mode all
// compose these so the three paths stay byte-for-byte consistent.
// ---------------------------------------------------------------------------

// Spawn `cfg.target`, drain its NDJSON stdout, and collect the transformed
// span objects into `spans`. Returns the same exit code the
// single-mode spans path returns: kExitUsage on spawn failure (with an
// fprintf diagnostic), kExitTargetFailed if the child exited non-zero
// (spans collected so far are still returned), kExitOk otherwise. The
// pre-spawn existence/empty-target validation is the caller's job (spans
// mode and hybrid mode word their diagnostics differently).
//
// Pass-event records share the same stdout stream as spans.
// They are routed into `passEvents` (a per-Pass map) here so BOTH spans
// mode and hybrid mode capture them from the single drain loop; the caller
// only surfaces `pass_events` in the output when it is non-empty, keeping a
// span-only run byte-for-byte identical to pre-T4 builds.
int collectSpans(const CliConfig& cfg, json& spans, json& passEvents) {
    spans = json::array();
    passEvents = json::object();

    topo::platform::PipedProcess child;
    if (!child.start(cfg.target, cfg.targetArgs)) {
        std::fprintf(stderr, "topo-profile: failed to spawn '%s'\n",
                     cfg.target.c_str());
        return kExitUsage;
    }
    // Target reads no input; close our write end so it doesn't block on stdin.
    child.closeStdin();

    std::string line;
    while (readLine(child, line)) {
        if (line.empty()) continue;
        // Quick filter: lines must start with '{' to be candidate JSON.
        // libtopo-observe's stdout exporter emits one record per line; the
        // benchmark also prints non-JSON status lines, which we skip.
        if (line[0] != '{') continue;
        // Pass-event records carry a "kind":"pass_event" discriminator; try
        // that route first. Spans never carry "kind", so a span line falls
        // through to transformSpan() unchanged.
        if (routePassEvent(line, passEvents)) continue;
        auto span = transformSpan(line, cfg.backend);
        if (span) spans.push_back(std::move(*span));
    }

    child.stop(5000);
    int ec = child.exitCode();
    if (ec != 0) {
        std::fprintf(stderr,
                     "topo-profile: target '%s' exited with code %d; "
                     "trace above may be partial.\n",
                     cfg.target.c_str(), ec);
        return kExitTargetFailed;
    }
    return kExitOk;
}

// Run the sampling-input conversion exactly as single-mode sampling does,
// writing the `sampling{}` segment into `out` (the converters populate
// out["sampling"]; the backend tag is stamped at the segment root here).
// `out` must already contain whatever `spans` the caller wants preserved —
// the converters only touch out["sampling"]. Returns kExitOk on success or
// the same non-zero codes single-mode sampling returns (kExitUsage on
// IO/parse error, kExitUnsupportedMode on the evented-speedscope and
// JFR-bridge feature gaps). Mutual-exclusion / required-input validation is
// the caller's responsibility (worded differently per mode).
int runSamplingConversion(const CliConfig& cfg, json& out) {
    const bool hasFolded = cfg.samplingInput.has_value();
    const bool hasJfr = cfg.jfrInput.has_value();
    const bool hasSpeedscope = cfg.speedscopeInput.has_value();
    const bool hasCpuProfile = cfg.cpuprofileInput.has_value();
    // cProfile NDJSON is the final fallback branch below — selected by
    // elimination once the other inputs are ruled out, so no named bool.
    const bool hasJfrBinary = cfg.jfrBinaryInput.has_value();

    // --jfr-binary-input: spawn the bridge jar, feed its stdout NDJSON into
    // the same JfrNdjsonConverter used by --jfr-input.
    if (hasJfrBinary) {
        if (!std::filesystem::exists(*cfg.jfrBinaryInput)) {
            std::fprintf(stderr,
                         "topo-profile: cannot read JFR binary '%s'\n",
                         cfg.jfrBinaryInput->c_str());
            return kExitUsage;
        }
        std::vector<std::string> searched;
        auto jarOpt = resolveJfrBridgeJar(cfg, searched);
        if (!jarOpt) {
            std::fprintf(stderr,
                         "topo-profile: --jfr-binary-input requires the "
                         "topo-profile-jvm-jfr bridge jar, but none was "
                         "found. Search order tried:\n");
            for (const auto& s : searched) {
                std::fprintf(stderr, "  - %s\n", s.c_str());
            }
            std::fprintf(stderr,
                         "Set --jfr-bridge-jar=<path>, export "
                         "TOPO_JFR_BRIDGE_JAR, or rebuild with a JDK "
                         "available (TOPO_GRADLE_JAVA_HOME set).\n");
            return kExitUnsupportedMode;
        }
        std::string javaExe = resolveJavaExecutable();
        std::string ndjson;
        std::string bridgeErr;
        if (!runJfrBridge(javaExe, *jarOpt, *cfg.jfrBinaryInput, ndjson,
                          bridgeErr)) {
            std::fprintf(stderr,
                         "topo-profile: JFR bridge failed for '%s': %s\n",
                         cfg.jfrBinaryInput->c_str(), bridgeErr.c_str());
            return kExitUnsupportedMode;
        }
        std::istringstream bridgeStream(ndjson);
        std::string sErr;
        if (!topo::profile::convertJfrNdjsonStream(bridgeStream, out, sErr)) {
            std::fprintf(stderr,
                         "topo-profile: failed to parse JFR bridge "
                         "output: %s\n",
                         sErr.c_str());
            return kExitUsage;
        }
        out["sampling"]["backend"] = cfg.backend;
        return kExitOk;
    }

    const bool hasAsyncProfiler = cfg.asyncProfilerInput.has_value();
    const bool hasSysMonitoring = cfg.sysMonitoringInput.has_value();

    const std::string& inputPath = hasFolded         ? *cfg.samplingInput
                                   : hasJfr          ? *cfg.jfrInput
                                   : hasSpeedscope   ? *cfg.speedscopeInput
                                   : hasCpuProfile   ? *cfg.cpuprofileInput
                                   : hasAsyncProfiler ? *cfg.asyncProfilerInput
                                   : hasSysMonitoring ? *cfg.sysMonitoringInput
                                                     : *cfg.cprofileInput;
    std::ifstream in(inputPath);
    if (!in.is_open()) {
        std::fprintf(stderr,
                     "topo-profile: cannot read sampling input '%s'\n",
                     inputPath.c_str());
        return kExitUsage;
    }
    std::string sErr;
    bool ok = false;
    if (hasFolded) {
        ok = topo::profile::convertFoldedStream(in, out, sErr);
    } else if (hasJfr) {
        ok = topo::profile::convertJfrNdjsonStream(in, out, sErr);
    } else if (hasSpeedscope) {
        topo::profile::SpeedscopeError sk =
            topo::profile::SpeedscopeError::None;
        ok = topo::profile::convertSpeedscopeStream(in, out, sErr, sk);
        if (!ok && sk == topo::profile::SpeedscopeError::EventedNotSupported) {
            std::fprintf(stderr, "topo-profile: %s\n", sErr.c_str());
            return kExitUnsupportedMode;
        }
    } else if (hasCpuProfile) {
        std::unique_ptr<topo::v8::debug::SourceMapResolver> resolver;
        if (!cfg.sourcemapSearchRoots.empty()) {
            resolver = std::make_unique<topo::v8::debug::SourceMapResolver>();
            for (const auto& root : cfg.sourcemapSearchRoots) {
                resolver->addSearchRoot(root);
            }
        }
        ok = topo::profile::convertCpuProfileStream(in, out, sErr,
                                                    resolver.get());
    } else if (hasAsyncProfiler) {
        ok = topo::profile::convertAsyncProfilerCollapsedStream(in, out, sErr);
        if (ok) {
            // Record the requested async-profiler sampling type. The
            // collapsed wire is uniform across wall/alloc/cpu, so the
            // converter cannot infer it; the CLI carries the user's
            // intent into the trace for downstream display.
            out["sampling"]["profile_type"] = cfg.asyncProfilerType;
        }
    } else if (hasSysMonitoring) {
        ok = topo::profile::convertSysMonitoringNdjsonStream(in, out, sErr);
    } else {
        ok = topo::profile::convertCProfileNdjsonStream(in, out, sErr);
    }
    if (!ok) {
        std::fprintf(stderr,
                     "topo-profile: failed to parse sampling input: %s\n",
                     sErr.c_str());
        return kExitUsage;
    }
    out["sampling"]["backend"] = cfg.backend;
    return kExitOk;
}

// Count how many of the six mutually-exclusive sampling-input flags are set.
int samplingInputCount(const CliConfig& cfg) {
    return (cfg.samplingInput.has_value() ? 1 : 0) +
           (cfg.jfrInput.has_value() ? 1 : 0) +
           (cfg.speedscopeInput.has_value() ? 1 : 0) +
           (cfg.cpuprofileInput.has_value() ? 1 : 0) +
           (cfg.cprofileInput.has_value() ? 1 : 0) +
           (cfg.asyncProfilerInput.has_value() ? 1 : 0) +
           (cfg.sysMonitoringInput.has_value() ? 1 : 0) +
           (cfg.jfrBinaryInput.has_value() ? 1 : 0);
}

} // namespace

int main(int argc, char** argv) {
    CliConfig cfg;
    std::string err;
    if (!parseCli(argc, argv, cfg, err)) {
        if (!err.empty()) std::fprintf(stderr, "topo-profile: %s\n", err.c_str());
        printUsage(stderr);
        return kExitUsage;
    }

    // The cProfile input flag is sampling-only. Reject it here before any
    // other dispatch (e.g. `--mode=spans --cprofile-input ...`) so the user
    // sees a clear diagnostic rather than the input being silently ignored.
    if (cfg.cprofileInput.has_value() && cfg.mode != "sampling") {
        std::fprintf(stderr,
                     "topo-profile: --cprofile-input requires --mode=sampling "
                     "(got --mode=%s)\n",
                     cfg.mode.c_str());
        return kExitUnsupportedMode;
    }

    // The async-profiler input flag is async-profiler-mode-only. Reject it
    // up front (e.g. `--mode=sampling --async-profiler-input ...`) so the
    // user gets a clear diagnostic instead of a silently-ignored flag —
    // same contract as the cProfile guard above.
    if (cfg.asyncProfilerInput.has_value() &&
        cfg.mode != "async-profiler") {
        std::fprintf(stderr,
                     "topo-profile: --async-profiler-input requires "
                     "--mode=async-profiler (got --mode=%s)\n",
                     cfg.mode.c_str());
        return kExitUnsupportedMode;
    }

    // The sys-monitoring input flag is sys-monitoring-mode-only. Same
    // up-front contract as the cProfile / async-profiler guards.
    if (cfg.sysMonitoringInput.has_value() &&
        cfg.mode != "sys-monitoring") {
        std::fprintf(stderr,
                     "topo-profile: --sys-monitoring-input requires "
                     "--mode=sys-monitoring (got --mode=%s)\n",
                     cfg.mode.c_str());
        return kExitUnsupportedMode;
    }

    if (cfg.mode == "sampling") {
        // Folded text path.
        // JFR-NDJSON path.
        // JFR binary path (this CLI invokes the
        //   topo-profile-jvm-jfr bridge, then routes the
        //   produced NDJSON through the same JfrNdjsonConverter
        //   used by --jfr-input).
        // speedscope JSON path.
        // V8 `.cpuprofile` JSON path.
        // cProfile NDJSON path.
        // Exactly one of the six input flags must be present.
        const int inputCount = samplingInputCount(cfg);
        if (inputCount > 1) {
            // Mutually-exclusive guard: silently picking one would make the
            // CLI surprising. Exit 2 (unsupported-mode bucket) keeps the
            // error class adjacent to other CLI-shape mistakes.
            std::fprintf(stderr,
                         "topo-profile: --sampling-input, --jfr-input, "
                         "--jfr-binary-input, --speedscope-input, "
                         "--cpuprofile-input, and --cprofile-input are "
                         "mutually exclusive; use only one input flag\n");
            return kExitUnsupportedMode;
        }
        if (inputCount == 0) {
            std::fprintf(stderr,
                         "topo-profile: --mode=sampling requires one of "
                         "--sampling-input <file> | --jfr-input <file> | "
                         "--jfr-binary-input <file> | "
                         "--speedscope-input <file> | "
                         "--cpuprofile-input <file> | "
                         "--cprofile-input <file>\n");
            return kExitUnsupportedMode;
        }
        // --sourcemap-search-root is V8-cpuprofile-only.
        // Reject it pre-conversion so users learn the constraint rather
        // than silently see the flag ignored.
        if (!cfg.sourcemapSearchRoots.empty() &&
            !cfg.cpuprofileInput.has_value()) {
            std::fprintf(stderr,
                         "topo-profile: --sourcemap-search-root only "
                         "applies to --cpuprofile-input (the V8 backend); "
                         "drop the flag or switch to --cpuprofile-input\n");
            return kExitUnsupportedMode;
        }

        // Sampling-only document: empty spans[] + the converted sampling{}
        // segment. runSamplingConversion writes out["sampling"]; the empty
        // spans[] keeps the trace shape stable for consumers.
        json out = json::object();
        out["spans"] = json::array();
        int rc = runSamplingConversion(cfg, out);
        if (rc != kExitOk) return rc;
        std::fputs((out.dump(2) + "\n").c_str(), stdout);
        return kExitOk;
    }

    if (cfg.mode == "async-profiler") {
        // async-profiler (off-CPU / wall-clock / alloc) sampling
        // source. async-profiler is a native agent the user installs per
        // platform; `topo profile trace --mode=async-profiler` consumes the
        // agent's `collapsed` text. Like the `.jfr` path, a checked-in
        // collapsed fixture keeps CTest reproducible without spawning the
        // agent at test time — feed it with --async-profiler-input. Mirrors
        // the --mode=sampling branch structure: exactly one input flag, the
        // async-profiler one, plus the wall|alloc|cpu --type selector.
        const std::string& t = cfg.asyncProfilerType;
        if (t != "wall" && t != "alloc" && t != "cpu") {
            std::fprintf(stderr,
                         "topo-profile: --mode=async-profiler --type must be "
                         "one of wall | alloc | cpu (got '%s')\n",
                         t.c_str());
            return kExitUnsupportedMode;
        }
        const int inputCount = samplingInputCount(cfg);
        if (inputCount > 1) {
            std::fprintf(stderr,
                         "topo-profile: --async-profiler-input is mutually "
                         "exclusive with the other sampling-input flags; use "
                         "only one input flag\n");
            return kExitUnsupportedMode;
        }
        if (!cfg.asyncProfilerInput.has_value()) {
            std::fprintf(stderr,
                         "topo-profile: --mode=async-profiler requires "
                         "--async-profiler-input <file> (async-profiler "
                         "`collapsed` text; install async-profiler per "
                         "platform and record with "
                         "`-o collapsed`)\n");
            return kExitUnsupportedMode;
        }
        json out = json::object();
        out["spans"] = json::array();
        int rc = runSamplingConversion(cfg, out);
        if (rc != kExitOk) return rc;
        std::fputs((out.dump(2) + "\n").c_str(), stdout);
        return kExitOk;
    }

    if (cfg.mode == "sys-monitoring") {
        // PEP 669 `sys.monitoring` (Python 3.12+) PY_START /
        // PY_RETURN event sequence. The harness
        // `topo_profile_python.sys_monitoring_harness` registers the
        // monitoring callbacks and emits NDJSON; it gates on the
        // interpreter version itself (Python < 3.12 → clear error, exit
        // 2). A checked-in NDJSON fixture keeps CTest reproducible
        // without a live 3.12+ interpreter — feed it with
        // --sys-monitoring-input. Mirrors the async-profiler branch:
        // exactly one input flag, the sys-monitoring one.
        const int inputCount = samplingInputCount(cfg);
        if (inputCount > 1) {
            std::fprintf(stderr,
                         "topo-profile: --sys-monitoring-input is mutually "
                         "exclusive with the other sampling-input flags; use "
                         "only one input flag\n");
            return kExitUnsupportedMode;
        }
        if (!cfg.sysMonitoringInput.has_value()) {
            std::fprintf(stderr,
                         "topo-profile: --mode=sys-monitoring requires "
                         "--sys-monitoring-input <file> (PY_START/PY_RETURN "
                         "NDJSON from `python -m "
                         "topo_profile_python.sys_monitoring_harness`, "
                         "Python 3.12+)\n");
            return kExitUnsupportedMode;
        }
        json out = json::object();
        out["spans"] = json::array();
        int rc = runSamplingConversion(cfg, out);
        if (rc != kExitOk) return rc;
        std::fputs((out.dump(2) + "\n").c_str(), stdout);
        return kExitOk;
    }

    if (cfg.mode == "hybrid") {
        // ObservabilityPass span boundaries (exact)
        // coexisting with sampling (statistical) in a single trace
        // document. We compose the *exact* spans-mode and sampling-mode
        // code paths (collectSpans + runSamplingConversion) so both
        // segments are byte-for-byte identical to their single-mode
        // output. Hybrid REQUIRES both a <binary> positional and exactly
        // one sampling-input flag.
        const int inputCount = samplingInputCount(cfg);
        if (cfg.target.empty() && inputCount == 0) {
            std::fprintf(stderr,
                         "topo-profile: --mode=hybrid requires both a "
                         "<binary> positional (for span boundaries) and "
                         "exactly one sampling-input flag (for in-span "
                         "hotspots)\n");
            return kExitUsage;
        }
        if (cfg.target.empty()) {
            std::fprintf(stderr,
                         "topo-profile: --mode=hybrid requires a <binary> "
                         "positional for span boundaries (got a sampling "
                         "input but no binary)\n");
            return kExitUsage;
        }
        if (inputCount == 0) {
            std::fprintf(stderr,
                         "topo-profile: --mode=hybrid requires one "
                         "sampling-input flag for in-span hotspots: "
                         "--sampling-input <file> | --jfr-input <file> | "
                         "--jfr-binary-input <file> | "
                         "--speedscope-input <file> | "
                         "--cpuprofile-input <file> | "
                         "--cprofile-input <file>\n");
            return kExitUsage;
        }
        if (inputCount > 1) {
            std::fprintf(stderr,
                         "topo-profile: --sampling-input, --jfr-input, "
                         "--jfr-binary-input, --speedscope-input, "
                         "--cpuprofile-input, and --cprofile-input are "
                         "mutually exclusive; use only one input flag\n");
            return kExitUsage;
        }
        // Same guard as sampling mode: --sourcemap-search-root
        // is V8-cpuprofile-only.
        if (!cfg.sourcemapSearchRoots.empty() &&
            !cfg.cpuprofileInput.has_value()) {
            std::fprintf(stderr,
                         "topo-profile: --sourcemap-search-root only "
                         "applies to --cpuprofile-input (the V8 backend); "
                         "drop the flag or switch to --cpuprofile-input\n");
            return kExitUsage;
        }
        if (!std::filesystem::exists(cfg.target)) {
            std::fprintf(stderr,
                         "topo-profile: target binary not found: '%s'\n",
                         cfg.target.c_str());
            return kExitUsage;
        }

        // 1. Span boundaries from the spawned binary (exactly spans-mode).
        // Pass-event records on the same stdout stream are routed through
        // here too so hybrid traces carry `pass_events` alongside spans.
        json out = json::object();
        json spans;
        json passEvents;
        int spansRc = collectSpans(cfg, spans, passEvents);
        out["spans"] = std::move(spans);
        if (!passEvents.empty()) out["pass_events"] = std::move(passEvents);
        // collectSpans only fails hard on spawn failure (kExitUsage). A
        // non-zero child exit (kExitTargetFailed) still yields whatever
        // spans were captured — mirror single-mode spans behaviour and
        // bail before the sampling merge so the partial-trace contract
        // (exit 3) is preserved.
        if (spansRc == kExitUsage) return kExitUsage;

        // 2. In-span hotspots from the sampling input (exactly
        // sampling-mode). Writes out["sampling"] without touching
        // out["spans"].
        int samplingRc = runSamplingConversion(cfg, out);
        if (samplingRc != kExitOk) return samplingRc;

        std::fputs((out.dump(2) + "\n").c_str(), stdout);
        // Preserve the spans-mode partial-trace contract: if the spawned
        // binary exited non-zero, surface exit 3 even though the merged
        // document was emitted.
        return spansRc == kExitTargetFailed ? kExitTargetFailed : kExitOk;
    }

    if (cfg.mode != "spans") {
        std::fprintf(stderr,
                     "topo-profile: unknown --mode=%s (expected spans, "
                     "sampling, async-profiler, sys-monitoring, or hybrid)\n",
                     cfg.mode.c_str());
        return kExitUsage;
    }

    if (cfg.target.empty()) {
        std::fprintf(stderr,
                     "topo-profile: --mode=spans requires <binary>\n");
        return kExitUsage;
    }
    if (!std::filesystem::exists(cfg.target)) {
        std::fprintf(stderr, "topo-profile: target binary not found: '%s'\n",
                     cfg.target.c_str());
        return kExitUsage;
    }

    json spans;
    json passEvents;
    int spansRc = collectSpans(cfg, spans, passEvents);
    if (spansRc == kExitUsage) return kExitUsage;  // spawn failed, no output

    json out = json::object();
    out["spans"] = std::move(spans);
    // Only emit `pass_events` when non-empty: this keeps the span-only
    // output identical to pre-T4 builds (acceptance criterion 2 — chosen
    // convention is "absent", not "empty object").
    if (!passEvents.empty()) out["pass_events"] = std::move(passEvents);
    std::fputs((out.dump(2) + "\n").c_str(), stdout);

    // collectSpans already emitted the partial-trace diagnostic to stderr
    // for a non-zero child; surface exit 3 here to keep the contract.
    return spansRc == kExitTargetFailed ? kExitTargetFailed : kExitOk;
}
