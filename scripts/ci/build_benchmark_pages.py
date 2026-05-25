#!/usr/bin/env python3
"""Build static benchmark pages in the gh-pages checkout."""

from __future__ import annotations

import argparse
import json
import shutil
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

MAX_RUNS = 10


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--site-dir", required=True, type=Path)
    parser.add_argument("--results-dir", required=True, type=Path)
    parser.add_argument("--history-url", default="")
    return parser.parse_args()


def read_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, data: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


def fetch_json(url: str) -> dict | None:
    if not url:
        return None
    try:
        with urllib.request.urlopen(url, timeout=10) as response:
            return json.loads(response.read().decode("utf-8"))
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError):
        return None


def slug_timestamp(started_at: str) -> str:
    normalized = started_at.replace("Z", "+00:00")
    parsed = datetime.fromisoformat(normalized)
    return parsed.astimezone(timezone.utc).strftime("%Y%m%dT%H%M%SZ")


def result_slug(result: dict) -> str:
    slug = str(result.get("run_slug", ""))
    if slug:
        return slug
    commit = str(result.get("commit", ""))
    started_at = str(result.get("started_at", ""))
    return f"{slug_timestamp(started_at)}-{commit[:12]}"


def manifest_entry(result: dict) -> dict:
    slug = result_slug(result)
    commit = str(result.get("commit", ""))
    return {
        "run_slug": slug,
        "title": slug,
        "commit": commit,
        "short_commit": str(result.get("short_commit") or commit[:12]),
        "ref": result.get("ref", ""),
        "run_id": result.get("run_id", ""),
        "run_attempt": result.get("run_attempt", ""),
        "started_at": result.get("started_at", ""),
        "finished_at": result.get("finished_at", ""),
        "subset": result.get("subset", ""),
        "benchmark_filter": result.get("benchmark_filter", ""),
        "result_path": f"{slug}/result.json",
    }


def load_manifest(path: Path) -> list[dict]:
    if not path.exists():
        return []
    data = read_json(path)
    runs = data.get("runs", [])
    return runs if isinstance(runs, list) else []


def import_legacy_history(history_url: str) -> list[dict]:
    data = fetch_json(history_url)
    if not isinstance(data, dict):
        return []
    imported = []
    for item in data.get("runs", []):
        if not isinstance(item, dict) or not item.get("commit") or not item.get("started_at"):
            continue
        result = {
            "schema_version": 1,
            "commit": item.get("commit", ""),
            "short_commit": item.get("short_commit", str(item.get("commit", ""))[:12]),
            "ref": item.get("ref", ""),
            "run_id": item.get("run_id", ""),
            "run_attempt": item.get("run_attempt", ""),
            "started_at": item.get("started_at", ""),
            "finished_at": item.get("finished_at", ""),
            "subset": item.get("subset", ""),
            "benchmark_filter": item.get("benchmark_filter", ""),
            "cpu": item.get("cpu", []),
        }
        result["run_slug"] = result_slug(result)
        imported.append(result)
    return imported


def merge_runs(existing: list[dict], current: dict) -> list[dict]:
    merged: dict[str, dict] = {}
    for item in existing:
        slug = str(item.get("run_slug", ""))
        if slug:
            merged[slug] = item
    current_entry = manifest_entry(current)
    merged[current_entry["run_slug"]] = current_entry
    return sorted(merged.values(), key=lambda item: str(item.get("run_slug", "")), reverse=True)[:MAX_RUNS]


def copy_results(results_dir: Path, target_dir: Path) -> None:
    if target_dir.exists():
        shutil.rmtree(target_dir)
    target_dir.mkdir(parents=True, exist_ok=True)
    for child in results_dir.iterdir():
        target = target_dir / child.name
        if child.is_dir():
            shutil.copytree(child, target)
        else:
            shutil.copy2(child, target)


def write_run_page(path: Path, result: dict) -> None:
    title = result_slug(result)
    short_commit = result.get("short_commit", "")
    started_at = result.get("started_at", "")
    path.write_text(
        f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title}</title>
<style>
:root {{
  color-scheme: light dark;
  --bg: #f6f7f5;
  --fg: #17191b;
  --muted: #626970;
  --panel: #ffffff;
  --line: #d7dce0;
  --accent: #006b5f;
}}
@media (prefers-color-scheme: dark) {{
  :root {{
    --bg: #101214;
    --fg: #f1f3f4;
    --muted: #a5adb3;
    --panel: #181b1f;
    --line: #31363b;
    --accent: #2bb5a5;
  }}
}}
* {{ box-sizing: border-box; }}
body {{
  margin: 0;
  background: var(--bg);
  color: var(--fg);
  font: 14px/1.45 system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
}}
main {{ max-width: 1180px; margin: 0 auto; padding: 28px 20px 56px; }}
h1 {{ font-size: 26px; margin: 0 0 4px; letter-spacing: 0; }}
p {{ margin: 0 0 18px; color: var(--muted); }}
table {{
  width: 100%;
  border-collapse: collapse;
  background: var(--panel);
  border: 1px solid var(--line);
}}
th, td {{
  padding: 8px 10px;
  border-bottom: 1px solid var(--line);
  text-align: left;
  vertical-align: top;
}}
th {{ font-size: 12px; color: var(--muted); font-weight: 650; }}
td.num {{ text-align: right; font-variant-numeric: tabular-nums; }}
a {{ color: var(--accent); }}
</style>
</head>
<body>
<main>
<p><a href="../..">Benchmarks</a></p>
<h1>{title}</h1>
<p>{short_commit} &middot; {started_at}</p>
<div id="cpu"></div>
</main>
<script>
function html(value) {{
  return String(value).replace(/[&<>"']/g, c => ({{'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'}}[c]));
}}
function fmt(value) {{
  if (value === null || value === undefined || Number.isNaN(Number(value))) return '';
  const number = Number(value);
  if (Math.abs(number) >= 1000000) return number.toExponential(3);
  if (Math.abs(number) >= 1000) return number.toFixed(0);
  if (Math.abs(number) >= 10) return number.toFixed(2);
  return number.toFixed(4);
}}
fetch('result.json')
  .then(response => response.json())
  .then(result => {{
    const rows = (result.cpu || []).map(bench =>
      `<tr><td>${{html(bench.name)}}</td><td class="num">${{fmt(bench.cpu_time)}}</td><td>${{html(bench.time_unit || '')}}</td><td class="num">${{fmt(bench.items_per_second)}}</td><td class="num">${{fmt(bench.iterations)}}</td></tr>`
    );
    document.getElementById('cpu').innerHTML = '<table><thead><tr><th>Benchmark</th><th>CPU time</th><th>Unit</th><th>Items/sec</th><th>Iterations</th></tr></thead><tbody>' + rows.join('') + '</tbody></table>';
  }});
</script>
</body>
</html>
""",
        encoding="utf-8",
    )


def write_index_html(path: Path) -> None:
    path.write_text(
        """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Ashiato ECS Benchmarks</title>
<style>
:root {
  color-scheme: light dark;
  --bg: #f6f7f5;
  --fg: #17191b;
  --muted: #626970;
  --panel: #ffffff;
  --line: #d7dce0;
  --accent: #006b5f;
}
@media (prefers-color-scheme: dark) {
  :root {
    --bg: #101214;
    --fg: #f1f3f4;
    --muted: #a5adb3;
    --panel: #181b1f;
    --line: #31363b;
    --accent: #2bb5a5;
  }
}
* { box-sizing: border-box; }
body {
  margin: 0;
  background: var(--bg);
  color: var(--fg);
  font: 14px/1.45 system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
}
main { max-width: 1180px; margin: 0 auto; padding: 28px 20px 56px; }
h1 { font-size: 28px; margin: 0 0 4px; letter-spacing: 0; }
h2 { font-size: 18px; margin: 28px 0 10px; }
h3 { font-size: 14px; margin: 18px 0 6px; }
p { margin: 0 0 18px; color: var(--muted); }
.toolbar { display: flex; flex-wrap: wrap; gap: 10px; align-items: center; margin: 18px 0; }
input {
  min-height: 34px;
  border: 1px solid var(--line);
  border-radius: 6px;
  background: var(--panel);
  color: var(--fg);
  padding: 6px 10px;
}
table {
  width: 100%;
  border-collapse: collapse;
  background: var(--panel);
  border: 1px solid var(--line);
}
th, td {
  padding: 8px 10px;
  border-bottom: 1px solid var(--line);
  text-align: left;
  vertical-align: top;
}
th { font-size: 12px; color: var(--muted); font-weight: 650; }
td.num { text-align: right; font-variant-numeric: tabular-nums; }
a { color: var(--accent); }
.empty { padding: 18px; border: 1px solid var(--line); background: var(--panel); color: var(--muted); }
.chart {
  border: 1px solid var(--line);
  background: var(--panel);
  margin-bottom: 12px;
}
.chart svg { display: block; width: 100%; height: auto; min-height: 220px; }
.axis, .gridline { stroke: var(--line); stroke-width: 1; }
.axis-label { fill: var(--muted); font-size: 11px; }
.chart-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(460px, 1fr)); gap: 14px; align-items: start; }
@media (max-width: 720px) {
  .chart-grid { grid-template-columns: 1fr; }
}
</style>
</head>
<body>
<main>
<h1>Ashiato ECS Benchmarks</h1>
<p>Latest benchmark runs are stored as individual timestamped pages.</p>
<div class="toolbar">
  <label>Filter <input id="filter" type="search" placeholder="benchmark name"></label>
</div>
<section>
  <h2>CPU Time Over Time</h2>
  <div id="cpu-charts" class="chart-grid"></div>
  <div id="cpu"></div>
</section>
<section>
  <h2>Runs</h2>
  <div id="runs"></div>
</section>
</main>
<script>
let history = {schema_version: 1, runs: []};
const filterInput = document.getElementById('filter');
function fmt(value) {
  if (value === null || value === undefined || Number.isNaN(Number(value))) return '';
  const number = Number(value);
  if (Math.abs(number) >= 1000000) return number.toExponential(3);
  if (Math.abs(number) >= 1000) return number.toFixed(0);
  if (Math.abs(number) >= 10) return number.toFixed(2);
  return number.toFixed(4);
}
function html(value) {
  return String(value).replace(/[&<>"']/g, c => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'}[c]));
}
function table(headers, rows) {
  if (!rows.length) return '<div class="empty">No matching data.</div>';
  return '<table><thead><tr>' + headers.map(h => `<th>${h}</th>`).join('') +
    '</tr></thead><tbody>' + rows.join('') + '</tbody></table>';
}
function seriesFrom(metric, filter) {
  const grouped = new Map();
  const runs = [...history.runs].reverse();
  for (let runIndex = 0; runIndex < runs.length; ++runIndex) {
    const run = runs[runIndex];
    for (const item of run.cpu || []) {
      const name = item.name;
      if (filter && !String(name).toLowerCase().includes(filter)) continue;
      const value = Number(item[metric]);
      if (!Number.isFinite(value)) continue;
      if (!grouped.has(name)) grouped.set(name, []);
      grouped.get(name).push({x: runIndex, value, commit: run.short_commit, started: run.started_at});
    }
  }
  return [...grouped.entries()]
    .map(([name, points]) => ({name, points}))
    .filter(series => series.points.length > 0)
    .slice(0, 16);
}
function chartMarkup(series, ariaLabel, valueLabel) {
  if (!series.length) return '<div class="empty">No chartable data.</div>';
  const colors = ['#006b5f', '#b33c22', '#3157a4', '#8a5a00', '#6f4ab0', '#26733d', '#a9336b', '#4b6f7c'];
  const width = 640, height = 230, left = 68, right = 18, top = 24, bottom = 42;
  const plotWidth = width - left - right;
  const plotHeight = height - top - bottom;
  const maxRunIndex = Math.max(1, history.runs.length - 1);
  const values = series.flatMap(item => item.points.map(point => point.value));
  let minValue = Math.min(...values);
  let maxValue = Math.max(...values);
  if (minValue === maxValue) {
    minValue = minValue * 0.95;
    maxValue = maxValue * 1.05 + 1;
  }
  const yFor = value => top + plotHeight - ((value - minValue) / (maxValue - minValue)) * plotHeight;
  const xFor = index => left + (index / maxRunIndex) * plotWidth;
  const lines = [];
  for (let i = 0; i <= 4; ++i) {
    const y = top + (plotHeight / 4) * i;
    const value = maxValue - ((maxValue - minValue) / 4) * i;
    lines.push(`<line class="gridline" x1="${left}" y1="${y}" x2="${width - right}" y2="${y}"></line>`);
    lines.push(`<text class="axis-label" x="8" y="${y + 4}">${html(fmt(value))}</text>`);
  }
  const paths = series.map((item, index) => {
    const color = colors[index % colors.length];
    const points = item.points.map(point => `${xFor(point.x).toFixed(1)},${yFor(point.value).toFixed(1)}`).join(' ');
    const dots = item.points.map(point => `<circle cx="${xFor(point.x).toFixed(1)}" cy="${yFor(point.value).toFixed(1)}" r="3" fill="${color}"><title>${html(item.name)}\\n${html(point.commit)}\\n${fmt(point.value)}</title></circle>`).join('');
    return `<polyline points="${points}" fill="none" stroke="${color}" stroke-width="2"></polyline>${dots}`;
  }).join('');
  const runs = [...history.runs].reverse();
  const first = runs[0]?.short_commit || '';
  const last = runs[runs.length - 1]?.short_commit || '';
  return `<svg viewBox="0 0 ${width} ${height}" role="img" aria-label="${html(ariaLabel)}">
    ${lines.join('')}
    <text class="axis-label" x="${left}" y="${top - 8}">${html(valueLabel)}</text>
    <line class="axis" x1="${left}" y1="${height - bottom}" x2="${width - right}" y2="${height - bottom}"></line>
    <line class="axis" x1="${left}" y1="${top}" x2="${left}" y2="${height - bottom}"></line>
    <text class="axis-label" x="${left}" y="${height - 16}">${html(first)}</text>
    <text class="axis-label" x="${width - right - 86}" y="${height - 16}">${html(last)}</text>
    <text class="axis-label" x="${Math.floor(width / 2) - 42}" y="${height - 16}">run order</text>
    ${paths}
  </svg>`;
}
function renderCpu() {
  const filter = filterInput.value.toLowerCase();
  const series = seriesFrom('cpu_time', filter);
  const charts = series.map(item => `<div><h3>${html(item.name)}</h3><div class="chart">${chartMarkup([item], item.name + ' CPU time chart', 'CPU time')}</div></div>`);
  document.getElementById('cpu-charts').innerHTML = charts.length ? charts.join('') : '<div class="empty">No matching CPU benchmarks.</div>';
  const rows = [];
  for (const run of history.runs) {
    for (const bench of run.cpu || []) {
      if (filter && !String(bench.name).toLowerCase().includes(filter)) continue;
      rows.push(`<tr><td><a href="runs/${html(run.run_slug)}/">${html(run.short_commit)}</a></td><td>${html(run.started_at)}</td><td>${html(bench.name)}</td><td class="num">${fmt(bench.cpu_time)}</td><td>${html(bench.time_unit || '')}</td><td class="num">${fmt(bench.items_per_second)}</td></tr>`);
    }
  }
  document.getElementById('cpu').innerHTML = table(['Commit', 'Started', 'Benchmark', 'CPU time', 'Unit', 'Items/sec'], rows);
}
function renderRuns() {
  const rows = history.runs.map(run => `<tr><td><a href="runs/${html(run.run_slug)}/">${html(run.title)}</a></td><td>${html(run.ref)}</td><td>${html(run.started_at)}</td><td>${html(run.subset)}</td></tr>`);
  document.getElementById('runs').innerHTML = table(['Run', 'Ref', 'Started', 'Subset'], rows);
}
function render() { renderCpu(); renderRuns(); }
async function loadHistory() {
  const manifest = await fetch('runs/index.json').then(response => response.json());
  const entries = (manifest.runs || []).slice(0, 10);
  const runs = await Promise.all(entries.map(async entry => {
    const result = await fetch('runs/' + entry.result_path).then(response => response.json());
    return {...entry, cpu: result.cpu || []};
  }));
  history = {...manifest, runs};
  render();
}
filterInput.addEventListener('input', render);
loadHistory().catch(() => {
  document.getElementById('cpu-charts').innerHTML = '<div class="empty">Unable to load benchmark history.</div>';
  render();
});
</script>
</body>
</html>
""",
        encoding="utf-8",
    )


def prune_run_dirs(runs_dir: Path, retained: list[dict]) -> None:
    retained_slugs = {str(run.get("run_slug", "")) for run in retained}
    for child in runs_dir.iterdir():
        if child.is_dir() and child.name not in retained_slugs:
            shutil.rmtree(child)


def main() -> int:
    args = parse_args()
    result = read_json(args.results_dir / "result.json")
    slug = result_slug(result)
    result["run_slug"] = slug
    result["short_commit"] = result.get("short_commit") or str(result.get("commit", ""))[:12]

    benchmark_dir = args.site_dir / "benchmarks"
    runs_dir = benchmark_dir / "runs"
    manifest_path = runs_dir / "index.json"
    runs_dir.mkdir(parents=True, exist_ok=True)

    existing = load_manifest(manifest_path)
    if not existing:
        for legacy_result in import_legacy_history(args.history_url):
            legacy_slug = result_slug(legacy_result)
            legacy_dir = runs_dir / legacy_slug
            legacy_dir.mkdir(parents=True, exist_ok=True)
            write_json(legacy_dir / "result.json", legacy_result)
            write_run_page(legacy_dir / "index.html", legacy_result)
            existing.append(manifest_entry(legacy_result))

    run_dir = runs_dir / slug
    copy_results(args.results_dir, run_dir)
    write_json(run_dir / "result.json", result)
    write_run_page(run_dir / "index.html", result)

    retained = merge_runs(existing, result)
    prune_run_dirs(runs_dir, retained)
    manifest = {
        "schema_version": 2,
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "runs": retained,
    }
    write_json(manifest_path, manifest)
    write_json(benchmark_dir / "history" / "index.json", manifest)
    write_index_html(benchmark_dir / "index.html")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
