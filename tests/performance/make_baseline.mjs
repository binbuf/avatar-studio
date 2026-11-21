#!/usr/bin/env node
// Wrap an avatar_bench result in a baseline file with explicit checks.
//
//   node tests/performance/make_baseline.mjs <result.json> <baseline.json>
//
// The generated baseline is marked `kind: "local"` and `release_gate: false`.
// It records the hardware fingerprint of the machine that produced the result,
// so a run on different hardware is a warning (or a failure with
// check.mjs --require-hardware), never a silent comparison.
//
// Targets are generous local sanity ceilings derived from the measured run:
// latency and frame time get 2x headroom, RSS 1.5x, and CPU 2x. check.mjs
// applies the per-check allowance (10% by default) on top. Nightly and release
// gates are T06/T26 work and will tighten these deliberately.

import { readFileSync, writeFileSync } from 'node:fs';

const [resultPath, baselinePath] = process.argv.slice(2);
if (!resultPath || !baselinePath) {
    console.error('usage: node tests/performance/make_baseline.mjs <result.json> <baseline.json>');
    process.exit(2);
}

const result = JSON.parse(readFileSync(resultPath, 'utf8'));
const metrics = result.metrics;
if (!metrics || result.schema !== 'avatar_bench.result.v1') {
    console.error(`${resultPath} is not an avatar_bench result`);
    process.exit(2);
}

const ceil1 = (value) => Math.ceil(value * 10) / 10;
const measured = result.config.frames;

const checks = [
    { metric: 'metrics.frames.measured', operator: '>=', target: measured, allowance: 0 },
    { metric: 'metrics.dropped_frames', operator: '<=', target: 0, allowance: 0 },
    { metric: 'metrics.dropped_tracking_frames', operator: '<=', target: 0, allowance: 0 },
    {
        metric: 'metrics.app_latency_ms.p95',
        operator: '<=',
        target: Math.max(100, ceil1(metrics.app_latency_ms.p95 * 2)),
        allowance: 0.1,
    },
    {
        metric: 'metrics.frame_time_ms.p99',
        operator: '<=',
        target: Math.max(33.4, ceil1(metrics.frame_time_ms.p99 * 2)),
        allowance: 0.1,
    },
    {
        metric: 'metrics.rss_bytes',
        operator: '<=',
        target: Math.max(512 * 1024 * 1024, Math.ceil(metrics.rss_bytes * 1.5)),
        allowance: 0.1,
    },
    {
        metric: 'metrics.cpu_percent_one_core',
        operator: '<=',
        target: Math.max(25, ceil1(metrics.cpu_percent_one_core * 2)),
        allowance: 0.1,
    },
];

const baseline = {
    schema: 'avatar_bench.baseline.v1',
    kind: 'local',
    release_gate: false,
    note:
        'Local foundation baseline generated on the machine identified by hardware.fingerprint. ' +
        'Not a release gate: T06/T26 own nightly and release gates. Targets are generous local ' +
        'sanity ceilings; check.mjs applies each check allowance on top.',
    created_utc: result.generated_utc,
    allowance: 0.1,
    scenario: result.scenario,
    config: result.config,
    hardware: result.hardware,
    checks,
    result,
};

writeFileSync(baselinePath, `${JSON.stringify(baseline, null, 2)}\n`);
console.log(`wrote ${baselinePath} (${checks.length} checks, fingerprint ${result.hardware.fingerprint})`);
