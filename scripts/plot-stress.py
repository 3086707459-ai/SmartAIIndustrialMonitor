"""Render a completed stress report; matplotlib is only a reporting dependency."""
import argparse
import json
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path, help="Directory with report.json and timeline.jsonl")
    parser.add_argument("output", type=Path, help="Output PNG path")
    args = parser.parse_args()
    report = json.loads((args.run / "report.json").read_text(encoding="utf-8-sig"))
    points = [json.loads(line) for line in (args.run / "timeline.jsonl").read_text(encoding="utf-8-sig").splitlines() if line.strip()]
    if len(points) < 2:
        parser.error("At least two timeline samples are required")
    minutes = [p["elapsed_ms"] / 60000 for p in points]
    rates = [
        (b["received"] - a["received"]) * 1000 / (b["elapsed_ms"] - a["elapsed_ms"])
        for a, b in zip(points, points[1:])
    ]
    plt.rcParams.update({"font.size": 10, "axes.spines.top": False, "axes.spines.right": False})
    fig, axes = plt.subplots(2, 2, figsize=(12, 7), layout="constrained")
    status = "PASS" if report["passed"] else "FAIL"
    fig.suptitle(f"SAIM sustained TCP / SQLite acceptance | {status}\n"
                 f"{report['device_count']} devices | {report['actual_samples_per_second']:.1f} samples/s | "
                 f"{report['workload_ms'] / 60000:.1f} minutes", fontsize=15, weight="bold")

    ax = axes[0, 0]
    ax.plot(minutes[1:], rates, color="#007c83", linewidth=1.8)
    ax.axhline(report["requested_samples_per_second"], color="#9c6135", linestyle="--", label="Target")
    ax.set(title="Received throughput (10 s intervals)", ylabel="Samples / second", ylim=(0, max(rates) * 1.2))
    ax.legend(loc="lower right")

    ax = axes[0, 1]
    ax.plot(minutes, [p["private_bytes"] / 2**20 for p in points], label="Private memory", color="#007c83")
    ax.plot(minutes, [p["working_bytes"] / 2**20 for p in points], label="Working set", color="#5b53a4")
    ax.axvline(0.5, linestyle=":", color="#999999", label="Warmup baseline")
    ax.set(title="Process memory", ylabel="MiB", ylim=(0, None))
    ax.legend(loc="lower right")

    ax = axes[1, 0]
    for key, color in [("persistence", "#007c83"), ("ai", "#5b53a4"), ("ui", "#9c6135")]:
        ax.plot(minutes, [p[key]["high_water"] for p in points],
                label=f"{key} (capacity {report[key]['capacity']})", color=color)
    ax.set(title="Mailbox high-water marks", ylabel="Pending entries", ylim=(0, None))
    ax.legend(loc="lower right")

    ax = axes[1, 1]
    ax.axis("off")
    storage_loss = report["storage_dropped"] + report["persistence"]["rejected"]
    lines = [
        ("Sent / received / committed", f"{report['sent']:,} each" if report['sent'] == report['received'] == report['written'] else "MISMATCH (see JSON)"),
        ("Storage loss", str(storage_loss)),
        ("AI overflow / cancellation", f"{report['ai']['evicted']} / {report['ai']['cancelled']}"),
        ("Storage queue P95 upper bound", f"{report['persistence']['p95_latency_upper_ms']:g} ms"),
        ("Graceful stop", f"{report['stop_ms']} ms"),
        ("SQLite integrity / FK violations", f"{report['database']['integrity_check']} / {report['database']['foreign_key_violations']}"),
        ("Controller stalls", f"{report['main_thread_stalls']} x {report['stall_ms']} ms"),
    ]
    table = ax.table(cellText=lines, colWidths=[0.56, 0.44], cellLoc="left", loc="center")
    table.auto_set_font_size(False)
    table.set_fontsize(9)
    table.scale(1, 1.75)
    for (row, col), cell in table.get_celld().items():
        cell.set_edgecolor("#dddddd")
        cell.set_facecolor("#eef6f6" if col == 0 else "#ffffff")
    ax.set_title("Final checks after draining")
    for ax in [axes[0, 0], axes[0, 1], axes[1, 0]]:
        ax.set_xlabel("Elapsed minutes")
        ax.grid(alpha=0.18)
    fig.supxlabel("Synthetic loopback load; no widgets or neural backend. Queue latency is not end-to-end latency.", fontsize=9)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.output, dpi=160)
    plt.close(fig)


if __name__ == "__main__":
    main()
