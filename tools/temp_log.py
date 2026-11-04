"""Record and analyse the station's temperature logs (examples/Test_temp_log).

Written 2026-09-25 for the report's heat tests: T2 warm-up, T3 hold, T4 hysteresis switch points,
T5 slot uniformity and T7 plate vs air (extra probes). Tested on simulated data only so far.

    python temp_log.py record COM3 warmup_45C.csv     # save the port to a file (Ctrl+C to stop)
    python temp_log.py plot warmup_45C.csv            # -> warmup_45C.png + warmup_45C_summary.txt

`record` needs pyserial (pip install pyserial) and the Serial Monitor must be CLOSED (one program
per port). Opening the port may reset the ESP32 — that is fine, the log then starts from boot.
`plot` needs matplotlib. Lines starting with '#' are the sketch's comments and are kept in the
recording but skipped by the analysis.

The analysis assumes one setpoint for the whole log. For a clean result: start from room
temperature, do not change the setpoint mid-run, and log at least ~30 min after the first time
the heater switches off (the hold phase).
"""

import argparse
import math
import os
import statistics
import sys
import time


# ---------------------------------------------------------------- record

def record(port, out_path, baud):
    try:
        import serial  # pyserial
    except ImportError:
        sys.exit("pyserial is not installed: pip install pyserial")
    print(f"Recording {port} @ {baud} to {out_path} - Ctrl+C to stop.")
    with serial.Serial(port, baud, timeout=1) as ser, open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(f"# recorded {time.strftime('%Y-%m-%d %H:%M:%S')} from {port}\n")
        try:
            while True:
                raw = ser.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
                print(line)
                f.write(line + "\n")
                f.flush()  # a crash or unplug still leaves everything up to here on disk
        except KeyboardInterrupt:
            print("\nStopped.")


# ---------------------------------------------------------------- load

def load(path):
    """Returns (columns, rows). rows are dicts of floats; 'nan' becomes math.nan."""
    columns, rows = None, []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("time_s"):
                columns = line.split(",")  # the sketch re-prints it after every reset; last wins
                continue
            if columns is None:
                continue
            parts = line.split(",")
            if len(parts) != len(columns):
                continue  # a line cut in half by a reset or unplug
            try:
                rows.append({c: float(v) for c, v in zip(columns, parts)})
            except ValueError:
                continue
    if not rows:
        sys.exit(f"No data lines found in {path}. Is it a Test_temp_log recording?")
    return columns, rows


# ---------------------------------------------------------------- analyse

# Department rule for the chamber air (BME mentor, 2026-10-02): at or above 45 C and not above
# 50 C, from the first time the air reaches 45 C (the "cure-ready point") to the end of the cure.
RULE_LOW_C, RULE_HIGH_C = 45.0, 50.0

def fmt_min(seconds):
    return "n/a" if seconds is None else f"{seconds / 60:.1f} min"


def analyse(columns, rows, settle_min):
    out = []
    t = [r["time_s"] for r in rows]
    temp = [r["control_C"] for r in rows]
    # ssr is nan on a logger-only board (LOG_ONLY): no switching events, the hold window then
    # starts from the first time the setpoint is reached.
    ssr_logged = not all(math.isnan(r["ssr"]) for r in rows)
    ssr = [0 if math.isnan(r["ssr"]) else int(r["ssr"]) for r in rows]
    sps = [r["setpoint_C"] for r in rows]
    sp = statistics.mode(sps)
    extra = [c for c in columns if c.startswith("probe")]
    hyst = 0.5

    out.append(f"Samples: {len(rows)} over {fmt_min(t[-1] - t[0])}")
    if len(set(sps)) > 1:
        out.append(f"WARNING: the setpoint changed during the log ({sorted(set(sps))}); analysed against {sp} C.")
    nan_n = sum(1 for v in temp if math.isnan(v))
    if nan_n:
        out.append(f"WARNING: {nan_n} samples with no control reading (probe fault/stale) - heater was OFF for those.")
    out.append(f"Setpoint: {sp:.1f} C (heater ON at or below {sp - hyst:.1f}, OFF at or above {sp + hyst:.1f})")

    good = [(ti, v) for ti, v in zip(t, temp) if not math.isnan(v)]
    out.append(f"Start temperature: {good[0][1]:.2f} C")

    # T2 warm-up
    def first_time(pred):
        for ti, v in good:
            if pred(v):
                return ti - t[0]
        return None
    t_band = first_time(lambda v: v >= sp - hyst)
    t_sp = first_time(lambda v: v >= sp)
    out.append("")
    out.append("T2 WARM-UP")
    out.append(f"  time to reach {sp - hyst:.1f} C (inside the band): {fmt_min(t_band)}")
    out.append(f"  time to reach the setpoint {sp:.1f} C:          {fmt_min(t_sp)}")

    # switching events (T4)
    offs = [(t[i], temp[i]) for i in range(1, len(rows)) if ssr[i - 1] == 1 and ssr[i] == 0 and not math.isnan(temp[i])]
    ons = [(t[i], temp[i]) for i in range(1, len(rows)) if ssr[i - 1] == 0 and ssr[i] == 1 and not math.isnan(temp[i])]
    if t_sp is not None and not ssr_logged:
        # Logger-only: the first peak is the highest reading before the air first falls back
        # below the band after reaching the setpoint.
        after = [(ti, v) for ti, v in good if ti >= t[0] + t_sp]
        end = next((ti for ti, v in after if v <= sp - hyst), after[-1][0])
        peak = max(v for ti, v in after if ti <= end)
        out.append(f"  first overshoot above the setpoint: {peak - sp:+.2f} C (peak {peak:.2f} C; "
                   f"heater state not logged on this board)")
    if t_sp is not None and offs:
        # first overshoot: the peak of the first heating cycle, from reaching the setpoint until
        # the heater comes back ON (or the end of the log). Heat already in the plate keeps the
        # reading rising after the SSR switches off, so the peak comes after the OFF event.
        first_off = offs[0][0]
        next_on = next((ti for ti, _ in ons if ti > first_off), t[-1])
        window = [v for ti, v in good if t[0] + t_sp <= ti <= next_on]
        peak = max(window)
        out.append(f"  first overshoot above the setpoint: {peak - sp:+.2f} C (peak {peak:.2f} C; "
                   f"the heater switched OFF at {sp + hyst:.1f})")

    out.append("")
    out.append("T4 HYSTERESIS SWITCH POINTS (temperature on the line where the SSR changed)")
    if not ssr_logged:
        out.append("  not available: logger-only board, the heater state was not recorded")
    else:
        for name, ev, target in (("OFF", offs, sp + hyst), ("ON ", ons, sp - hyst)):
            if ev:
                vals = [v for _, v in ev]
                out.append(f"  {name}: {len(vals)} events, mean {statistics.mean(vals):.2f} C "
                           f"(min {min(vals):.2f}, max {max(vals):.2f}), designed {target:.1f} C")
            else:
                out.append(f"  {name}: no events")
    out.append("  Note: the probe updates about once a second, so a fast-changing temperature is caught a little past the threshold.")

    # T3 hold, from the first OFF event + settle time
    out.append("")
    out.append("T3 HOLD (steady state)")
    if ssr_logged and not offs:
        out.append("  The heater never switched off - no hold phase in this log.")
        return out, sp, None
    if not ssr_logged and t_sp is None:
        out.append("  The setpoint was never reached - no hold phase in this log.")
        return out, sp, None
    hold_ref = offs[0][0] if ssr_logged else t[0] + t_sp
    hold_start = hold_ref + settle_min * 60
    hold = [(ti, v, s) for ti, v, s in zip(t, temp, ssr) if ti >= hold_start and not math.isnan(v)]
    if len(hold) < 60:
        out.append(f"  Less than a minute of data after the first OFF + {settle_min} min settle - log longer.")
        return out, sp, None
    hv = [v for _, v, _ in hold]
    out.append(f"  window: {fmt_min(hold_start - t[0])} to {fmt_min(hold[-1][0] - t[0])} "
               f"({'first OFF' if ssr_logged else 'setpoint first reached'} + {settle_min} min settle)")
    out.append(f"  mean {statistics.mean(hv):.2f} C, min {min(hv):.2f}, max {max(hv):.2f}, "
               f"std dev {statistics.pstdev(hv):.2f}, spread {max(hv) - min(hv):.2f} C")
    within = sum(1 for v in hv if abs(v - sp) <= 1.0) / len(hv) * 100
    out.append(f"  {within:.1f} % of samples within setpoint +/- 1.0 C")
    if ssr_logged:
        duty = sum(s for _, _, s in hold) / len(hold) * 100
        out.append(f"  heater duty cycle: {duty:.1f} % ON")
    hold_offs = [ti for ti, _ in offs if ti >= hold_start]
    if len(hold_offs) >= 2:
        periods = [b - a for a, b in zip(hold_offs, hold_offs[1:])]
        out.append(f"  switching cycle: mean {fmt_min(statistics.mean(periods))} between OFF events ({len(periods)} cycles)")

    # T5 / T7 extra probes
    if extra:
        out.append("")
        out.append("T5 / T7 EXTRA PROBES (hold window; label them in your notes: plate, air, slot 1..7)")
        means = {"control": statistics.mean(hv)}
        for c in extra:
            vals = [r[c] for r in rows if r["time_s"] >= hold_start and not math.isnan(r[c])]
            if not vals:
                out.append(f"  {c}: no readings")
                continue
            m = statistics.mean(vals)
            means[c] = m
            out.append(f"  {c}: mean {m:.2f} C ({m - means['control']:+.2f} vs control), "
                       f"min {min(vals):.2f}, max {max(vals):.2f}")
        if len(means) > 1:
            out.append(f"  spread of the means across all probes: {max(means.values()) - min(means.values()):.2f} C")
    return out, sp, hold_start


def analyse_rule(rows):
    """45-50 C rule, judged on the control probe from the cure-ready point to the end of the log."""
    out = ["", f"45-50 C RULE (department, 2026-10-02): from the first {RULE_LOW_C:.0f} C to the end"]
    t0 = rows[0]["time_s"]
    good = [(r["time_s"], r["control_C"]) for r in rows if not math.isnan(r["control_C"])]
    ready = next((ti for ti, v in good if v >= RULE_LOW_C), None)
    if ready is None:
        out.append(f"  never reached {RULE_LOW_C:.0f} C - no verdict")
        return out, None
    win = [(ti, v) for ti, v in good if ti >= ready]
    vals = [v for _, v in win]
    below = sum(1 for v in vals if v < RULE_LOW_C)
    above = sum(1 for v in vals if v > RULE_HIGH_C)
    out.append(f"  cure-ready (first {RULE_LOW_C:.1f} C) at {fmt_min(ready - t0)}; window {fmt_min(win[-1][0] - ready)} long")
    out.append(f"  min {min(vals):.2f} C, max {max(vals):.2f} C; samples below {RULE_LOW_C:.0f}: {below}, "
               f"above {RULE_HIGH_C:.0f}: {above}")
    out.append(f"  margins: {min(vals) - RULE_LOW_C:+.2f} C above the floor, {RULE_HIGH_C - max(vals):+.2f} C below the ceiling")
    out.append(f"  VERDICT: {'PASS' if not below and not above else 'FAIL'}")
    return out, ready


def analyse_mi(rows, hold_start):
    """T40: the Xiaomi Mi reference sensor (mi_C column, added 2026-10-01) against the control probe."""
    out = ["", "T40 REFERENCE CHECK (Xiaomi Mi - control probe; the Mi updates every few seconds and reacts slower)"]
    pairs = [(r["time_s"], r["control_C"], r["mi_C"]) for r in rows
             if not math.isnan(r["control_C"]) and not math.isnan(r["mi_C"])]
    if not pairs:
        out.append("  no Mi readings in this log")
        return out
    gaps = [m - c for _, c, m in pairs]
    out.append(f"  whole log: {len(pairs)} paired samples, mean {statistics.mean(gaps):+.2f} C, "
               f"largest {max(gaps, key=abs):+.2f} C (expect the largest while warming up: Mi lag)")
    if hold_start is not None:
        hg = [m - c for ti, c, m in pairs if ti >= hold_start]
        if hg:
            out.append(f"  hold window: mean {statistics.mean(hg):+.2f} C, min {min(hg):+.2f}, max {max(hg):+.2f}")
    mi = [m for _, _, m in pairs]
    out.append(f"  Mi start {mi[0]:.2f} C, Mi peak {max(mi):.2f} C")
    return out


def plot(path, settle_min):
    columns, rows = load(path)
    summary, sp, hold_start = analyse(columns, rows, settle_min)
    rule, ready = analyse_rule(rows)
    summary += rule
    if "mi_C" in columns:
        summary += analyse_mi(rows, hold_start)
    base = os.path.splitext(path)[0]
    with open(base + "_summary.txt", "w", encoding="utf-8") as f:
        f.write(f"Summary of {os.path.basename(path)}\n\n" + "\n".join(summary) + "\n")
    print("\n".join(summary))

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("\nmatplotlib is not installed (pip install matplotlib) - summary written, no graph.")
        return

    t0 = rows[0]["time_s"]
    tm = [(r["time_s"] - t0) / 60 for r in rows]
    fig, ax = plt.subplots(figsize=(10, 5.5), dpi=150)
    # SSR ON as light shading behind everything
    on_start = None
    for i, r in enumerate(rows):
        if r["ssr"] == 1 and on_start is None:
            on_start = tm[i]
        if (r["ssr"] == 0 or i == len(rows) - 1) and on_start is not None:
            ax.axvspan(on_start, tm[i], color="#f4a582", alpha=0.25, lw=0)
            on_start = None
    ax.axhspan(sp - 0.5, sp + 0.5, color="#92c5de", alpha=0.3, lw=0, label="hysteresis band (SP ± 0.5 °C)")
    ax.axhline(sp, color="#2166ac", lw=1, ls="--", label=f"setpoint {sp:.1f} °C")
    ax.axhline(RULE_HIGH_C, color="#d73027", lw=1, ls="-.", label=f"rule ceiling {RULE_HIGH_C:.0f} °C")
    ax.axhline(RULE_LOW_C, color="#4d9221", lw=1, ls="-.", label=f"rule floor {RULE_LOW_C:.0f} °C")
    if ready is not None:
        ax.axvline((ready - rows[0]["time_s"]) / 60, color="#4d9221", lw=0.8, ls=":", label="cure-ready (first 45 °C)")
    ax.plot(tm, [r["control_C"] for r in rows], color="#b2182b", lw=1.4, label="control probe")
    for c in [c for c in columns if c.startswith("probe")]:
        ax.plot(tm, [r[c] for r in rows], lw=1, label=c.replace("_C", ""))
    if "mi_C" in columns:
        ax.plot(tm, [r["mi_C"] for r in rows], color="#1b7837", lw=1.2, label="Xiaomi Mi (reference)")
    if hold_start is not None:
        ax.axvline((hold_start - t0) / 60, color="grey", lw=0.8, ls=":", label="start of hold window")
    ax.plot([], [], color="#f4a582", alpha=0.5, lw=6, label="heater ON")
    ax.set_xlabel("Time (min)")
    ax.set_ylabel("Temperature (°C)")
    ax.set_title(os.path.basename(path))
    ax.grid(True, alpha=0.3)
    ax.legend(loc="lower right", fontsize=8)
    fig.tight_layout()
    fig.savefig(base + ".png")
    print(f"\nWrote {base}.png and {base}_summary.txt")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("record", help="save the serial port to a CSV file")
    r.add_argument("port")
    r.add_argument("out")
    r.add_argument("--baud", type=int, default=115200)
    p = sub.add_parser("plot", help="graph + summary numbers from a recorded log")
    p.add_argument("log")
    p.add_argument("--settle", type=float, default=5.0,
                   help="minutes after the first OFF event before the hold window starts (default 5)")
    a = ap.parse_args()
    if a.cmd == "record":
        record(a.port, a.out, a.baud)
    else:
        plot(a.log, a.settle)


if __name__ == "__main__":
    main()
