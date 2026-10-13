#!/usr/bin/env python3
"""Percentile summary and plots for user-to-user latency logs (format: tools/log_format.md).

Python 3.11+ standard library; matplotlib is optional (plots are skipped without it).

    python3 -I tools/plot_percentiles.py results/*.csv --out results/summary
    python3 -I tools/plot_percentiles.py tools/example/u2u_example.csv --out /tmp/u2u --no-plot

Writes into --out:
    summary.md      markdown tables (paste into report.md)
    summary.csv     the same numbers, machine-readable
    percentiles.png p50/p95/p99 per profile (and per src->dst pair)   [matplotlib]
    cdf.png         empirical CDF of u2u_ms per profile               [matplotlib]
    components.png  median of latency components per profile          [matplotlib, if columns exist]

Percentiles use linear interpolation between closest ranks (same as numpy.percentile default).
Rows with empty u2u_ms are counted as "unseen" and excluded from percentiles.
"""

from __future__ import annotations

import argparse
import csv
import math
import os
import sys
from collections import defaultdict

REQUIRED = ("profile", "src_client", "dst_client", "input_seq", "u2u_ms")
COMPONENTS = ("uplink_ms", "tick_wait_ms", "server_ms", "downlink_ms")
QS = (50, 95, 99)


def percentile(sorted_vals: list[float], q: float) -> float:
    if not sorted_vals:
        return math.nan
    if len(sorted_vals) == 1:
        return sorted_vals[0]
    pos = (len(sorted_vals) - 1) * q / 100.0
    lo = math.floor(pos)
    hi = min(lo + 1, len(sorted_vals) - 1)
    return sorted_vals[lo] + (sorted_vals[hi] - sorted_vals[lo]) * (pos - lo)


def fnum(x: str) -> float | None:
    x = x.strip()
    if not x:
        return None
    v = float(x)
    return v if math.isfinite(v) else None


def load(paths: list[str]):
    rows = []
    for path in paths:
        with open(path, newline="", encoding="utf-8") as f:
            rd = csv.DictReader(f)
            missing = [c for c in REQUIRED if c not in (rd.fieldnames or [])]
            if missing:
                sys.exit("%s: missing required column(s): %s" % (path, ", ".join(missing)))
            for i, r in enumerate(rd, start=2):
                try:
                    row = {
                        "profile": r["profile"].strip(),
                        "src": int(r["src_client"]),
                        "dst": int(r["dst_client"]),
                        "seq": int(r["input_seq"]),
                        "u2u": fnum(r["u2u_ms"]),
                    }
                    for c in COMPONENTS:
                        row[c] = fnum(r.get(c) or "")
                except ValueError as e:
                    sys.exit("%s:%d: bad value (%s)" % (path, i, e))
                if row["src"] == row["dst"]:
                    sys.exit("%s:%d: src_client == dst_client" % (path, i))
                rows.append(row)
    return rows


def stats(vals: list[float], total: int) -> dict:
    s = sorted(vals)
    out = {"n": total, "seen": len(s), "unseen_pct": 100.0 * (total - len(s)) / total if total else math.nan,
           "mean": sum(s) / len(s) if s else math.nan, "max": s[-1] if s else math.nan}
    for q in QS:
        out["p%d" % q] = percentile(s, q)
    return out


def summarize(rows):
    by_profile = defaultdict(list)
    by_pair = defaultdict(list)
    for r in rows:
        by_profile[r["profile"]].append(r)
        by_pair[(r["profile"], r["src"], r["dst"])].append(r)
    prof = {p: stats([r["u2u"] for r in rs if r["u2u"] is not None], len(rs)) for p, rs in by_profile.items()}
    pair = {k: stats([r["u2u"] for r in rs if r["u2u"] is not None], len(rs)) for k, rs in by_pair.items()}
    comp = {}
    warnings = []
    for p, rs in by_profile.items():
        full = [r for r in rs if r["u2u"] is not None and all(r[c] is not None for c in COMPONENTS)]
        if not full:
            continue
        comp[p] = {c: percentile(sorted(r[c] for r in full), 50) for c in COMPONENTS}
        comp[p]["n"] = len(full)
        resid = sorted(abs(sum(r[c] for c in COMPONENTS) - r["u2u"]) for r in full)
        comp[p]["resid_p99"] = percentile(resid, 99)
        if comp[p]["resid_p99"] > 1.0:
            warnings.append("profile %s: sum of components differs from u2u_ms by > 1 ms (p99 = %.2f ms) — check clocks"
                            % (p, comp[p]["resid_p99"]))
        neg = sum(1 for r in full for c in COMPONENTS if r[c] < 0)
        if neg:
            warnings.append("profile %s: %d negative component values — clock offset not compensated?" % (p, neg))
    neg_u2u = sum(1 for r in rows if r["u2u"] is not None and r["u2u"] < 0)
    if neg_u2u:
        warnings.append("%d rows with negative u2u_ms — the measurement method is broken" % neg_u2u)
    return prof, pair, comp, warnings


def fmt(x, digits=1):
    return "—" if x is None or (isinstance(x, float) and math.isnan(x)) else ("%.*f" % (digits, x))


def to_markdown(prof, pair, comp, warnings) -> str:
    L = ["## User-to-user latency, мс", "",
         "| профиль | n | не увидено, % | p50 | p95 | p99 | max | mean |", "|---|---|---|---|---|---|---|---|"]
    for p, s in prof.items():
        L.append("| %s | %d | %s | %s | %s | %s | %s | %s |" % (p, s["n"], fmt(s["unseen_pct"], 2), fmt(s["p50"]), fmt(s["p95"]),
                                                             fmt(s["p99"]), fmt(s["max"]), fmt(s["mean"])))
    L += ["", "### По парам A → B", "", "| профиль | A → B | n | не увидено, % | p50 | p95 | p99 |",
          "|---|---|---|---|---|---|---|"]
    for (p, a, b), s in sorted(pair.items()):
        L.append("| %s | %d → %d | %d | %s | %s | %s | %s |" % (p, a, b, s["n"], fmt(s["unseen_pct"], 2), fmt(s["p50"]),
                                                             fmt(s["p95"]), fmt(s["p99"])))
    if comp:
        L += ["", "### Медианы компонент (плюс)", "",
              "| профиль | n | uplink | tick_wait | server | downlink | p99 невязки (сумма − u2u) |", "|---|---|---|---|---|---|---|"]
        for p, c in comp.items():
            L.append("| %s | %d | %s | %s | %s | %s | %s |" % (p, c["n"], fmt(c["uplink_ms"]), fmt(c["tick_wait_ms"]),
                                                             fmt(c["server_ms"]), fmt(c["downlink_ms"]), fmt(c["resid_p99"], 2)))
    if warnings:
        L += ["", "### Предупреждения", ""] + ["- " + w for w in warnings]
    return "\n".join(L) + "\n"


def write_csv(path, prof, pair):
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["profile", "src_client", "dst_client", "n", "seen", "unseen_pct", "p50", "p95", "p99", "max", "mean"])
        for p, s in prof.items():
            w.writerow([p, "*", "*", s["n"], s["seen"]] + ["%.3f" % s[k] for k in ("unseen_pct", "p50", "p95", "p99", "max", "mean")])
        for (p, a, b), s in sorted(pair.items()):
            w.writerow([p, a, b, s["n"], s["seen"]] + ["%.3f" % s[k] for k in ("unseen_pct", "p50", "p95", "p99", "max", "mean")])


def plot(out_dir, rows, prof, pair, comp):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not found: plots skipped (tables are still written)")
        return []
    files = []
    profiles = list(prof)
    # 1. grouped bars p50/p95/p99 per profile
    fig, ax = plt.subplots(figsize=(max(6, 1.8 * len(profiles)), 4))
    width = 0.25
    for i, q in enumerate(QS):
        ax.bar([x + (i - 1) * width for x in range(len(profiles))], [prof[p]["p%d" % q] for p in profiles],
               width, label="p%d" % q)
    ax.set_xticks(range(len(profiles)), profiles, rotation=15)
    ax.set_ylabel("user-to-user latency, ms")
    ax.set_title("u2u latency percentiles per network profile")
    ax.legend()
    ax.grid(axis="y", alpha=0.3)
    fig.tight_layout()
    files.append(os.path.join(out_dir, "percentiles.png"))
    fig.savefig(files[-1], dpi=150)
    plt.close(fig)
    # 2. CDF per profile
    fig, ax = plt.subplots(figsize=(7, 4))
    for p in profiles:
        v = sorted(r["u2u"] for r in rows if r["profile"] == p and r["u2u"] is not None)
        if v:
            ax.plot(v, [(i + 1) / len(v) for i in range(len(v))], label=p)
    ax.set_xlabel("user-to-user latency, ms")
    ax.set_ylabel("CDF")
    ax.grid(alpha=0.3)
    ax.legend()
    fig.tight_layout()
    files.append(os.path.join(out_dir, "cdf.png"))
    fig.savefig(files[-1], dpi=150)
    plt.close(fig)
    # 3. stacked median components
    if comp:
        fig, ax = plt.subplots(figsize=(max(6, 1.8 * len(comp)), 4))
        bottom = [0.0] * len(comp)
        names = list(comp)
        for c in COMPONENTS:
            vals = [comp[p][c] for p in names]
            ax.bar(names, vals, bottom=bottom, label=c)
            bottom = [b + v for b, v in zip(bottom, vals)]
        ax.set_ylabel("median, ms")
        ax.set_title("u2u latency decomposition (medians)")
        ax.legend()
        fig.tight_layout()
        files.append(os.path.join(out_dir, "components.png"))
        fig.savefig(files[-1], dpi=150)
        plt.close(fig)
    return files


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", nargs="+", help="u2u CSV file(s), format: tools/log_format.md")
    ap.add_argument("--out", default="summary", help="output directory")
    ap.add_argument("--no-plot", action="store_true")
    args = ap.parse_args()
    rows = load(args.csv)
    if not rows:
        sys.exit("no rows")
    prof, pair, comp, warnings = summarize(rows)
    os.makedirs(args.out, exist_ok=True)
    md = to_markdown(prof, pair, comp, warnings)
    with open(os.path.join(args.out, "summary.md"), "w", encoding="utf-8") as f:
        f.write(md)
    write_csv(os.path.join(args.out, "summary.csv"), prof, pair)
    print(md)
    if not args.no_plot:
        for p in plot(args.out, rows, prof, pair, comp):
            print("wrote", p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
