#!/usr/bin/env node
// Compare an avatar_bench result to a baseline.
//
// The baseline is a result-shaped JSON file plus an explicit `checks` array:
//
//   { "metric": "metrics.app_latency_ms.p95", "operator": "<=",
//     "target": 100.0, "allowance": 0.10 }
//
// `target` is the reference value. The effective threshold is derived by the
// operator and allowance: for "<=" it is target * (1 + allowance), for ">=" it
// is target * (1 - allowance). A per-check allowance overrides the baseline
// default. The failing metric and both values are always printed.
//
// Modes:
//   node check.mjs --baseline <json> --result <json>
//   node check.mjs --baseline <json>                 # use baseline.result
//   node check.mjs --validate <json>                 # shape check only
//
// Exit codes: 0 pass, 1 failure, 2 usage/IO error.

import { readFileSync } from 'node:fs';

const USAGE = `check.mjs - compare avatar_bench results to a baseline

Usage:
  node tests/performance/check.mjs --baseline <baseline.json> --result <result.json>
  node tests/performance/check.mjs --baseline <baseline.json>
  node tests/performance/check.mjs --validate <result.json>

Options:
  --allowance <fraction>   override the baseline regression allowance (default 0.10)
  --require-hardware       fail when the result hardware fingerprint differs
  --quiet                  print only failures and the summary
  --help                   this text

Exit codes: 0 pass, 1 failure, 2 usage or I/O error.`;

function parseArgs(argv) {
    const args = {
        baseline: 'tests/performance/baselines/foundation-local.json',
        result: '',
        validate: '',
        allowance: null,
        requireHardware: false,
        quiet: false,
    };
    for (let index = 0; index < argv.length; index += 1) {
        const argument = argv[index];
        if (argument === '--help' || argument === '-h') {
            console.log(USAGE);
            process.exit(0);
        }
        if (!argument.startsWith('--')) {
            throw new Error(`unexpected argument '${argument}'`);
        }
        const equals = argument.indexOf('=');
        const key = equals === -1 ? argument : argument.slice(0, equals);
        const inline = equals === -1 ? null : argument.slice(equals + 1);
        const next = () => {
            if (inline !== null) {
                return inline;
            }
            if (index + 1 >= argv.length) {
                throw new Error(`missing value for ${key}`);
            }
            index += 1;
            return argv[index];
        };
        switch (key) {
            case '--baseline':
                args.baseline = next();
                break;
            case '--result':
                args.result = next();
                break;
            case '--validate':
                args.validate = next();
                break;
            case '--allowance': {
                const value = Number(next());
                if (!Number.isFinite(value) || value < 0) {
                    throw new Error('--allowance must be a non-negative number');
                }
                args.allowance = value;
                break;
            }
            case '--require-hardware':
                args.requireHardware = true;
                break;
            case '--quiet':
                args.quiet = true;
                break;
            default:
                throw new Error(`unknown option '${key}'`);
        }
    }
    return args;
}

function readJson(path) {
    // PowerShell 5.1 writes a UTF-8 BOM with Set-Content; tolerate it.
    const text = readFileSync(path, 'utf8').replace(/^\uFEFF/, '');
    return JSON.parse(text);
}

function getPath(object, path) {
    return path
        .split('.')
        .reduce((value, key) => (value !== null && value !== undefined ? value[key] : undefined), object);
}

function format(value) {
    if (typeof value === 'number') {
        return Number.isInteger(value) ? String(value) : value.toFixed(4);
    }
    return String(value);
}

function allowedValue(target, operator, allowance) {
    if (typeof target !== 'number') {
        return target;
    }
    switch (operator) {
        case '<=':
        case '<':
            return target * (1 + allowance);
        case '>=':
        case '>':
            return target * (1 - allowance);
        default:
            return target;
    }
}

function compare(value, operator, allowed) {
    switch (operator) {
        case '<=':
            return value <= allowed;
        case '<':
            return value < allowed;
        case '>=':
            return value >= allowed;
        case '>':
            return value > allowed;
        case '==':
            return value === allowed;
        case '!=':
            return value !== allowed;
        default:
            throw new Error(`unsupported operator '${operator}'`);
    }
}

function validateDistribution(distribution, name, errors) {
    if (!distribution || typeof distribution !== 'object') {
        errors.push(`${name} is missing or not an object`);
        return;
    }
    for (const key of ['p50', 'p95', 'p99', 'mean', 'max']) {
        if (typeof distribution[key] !== 'number' || !Number.isFinite(distribution[key])) {
            errors.push(`${name}.${key} is missing or not a number`);
        } else if (distribution[key] < 0) {
            errors.push(`${name}.${key} is negative`);
        }
    }
    if (!Number.isInteger(distribution.samples) || distribution.samples < 0) {
        errors.push(`${name}.samples is missing or not a non-negative integer`);
    }
    if (
        typeof distribution.p50 === 'number' &&
        typeof distribution.p95 === 'number' &&
        typeof distribution.p99 === 'number' &&
        !(distribution.p50 <= distribution.p95 && distribution.p95 <= distribution.p99)
    ) {
        errors.push(`${name} percentiles are not ordered p50 <= p95 <= p99`);
    }
}

function validateResult(result) {
    const errors = [];
    if (!result || typeof result !== 'object') {
        return ['result is not a JSON object'];
    }
    if (result.schema !== 'avatar_bench.result.v1') {
        errors.push('schema is not avatar_bench.result.v1');
    }
    if (!['capture', 'ring', 'rig', 'render'].includes(result.scenario)) {
        errors.push(`unknown scenario '${result.scenario}'`);
    }
    if (!result.hardware || typeof result.hardware.fingerprint !== 'string' || result.hardware.fingerprint.length === 0) {
        errors.push('hardware.fingerprint is missing');
    }
    if (!result.build || typeof result.build.type !== 'string' || result.build.type.length === 0) {
        errors.push('build.type is missing');
    }
    if (!result.build || typeof result.build.git_commit !== 'string' || result.build.git_commit.length === 0) {
        errors.push('build.git_commit is missing');
    }
    if (!result.config || typeof result.config !== 'object') {
        errors.push('config is missing');
    }
    const metrics = result.metrics;
    if (!metrics || typeof metrics !== 'object') {
        return errors.concat('metrics is missing');
    }
    for (const key of [
        'frames',
        'dropped_frames',
        'dropped_tracking_frames',
        'frame_time_ms',
        'frame_interval_ms',
        'app_latency_ms',
        'rss_bytes',
        'cpu_percent_one_core',
        'gpu_percent',
        'stage_ms',
    ]) {
        if (!(key in metrics)) {
            errors.push(`metrics.${key} is missing`);
        }
    }
    if (!metrics.frames || !Number.isInteger(metrics.frames.measured) || metrics.frames.measured < 1) {
        errors.push('metrics.frames.measured is missing or zero');
    }
    for (const name of ['frame_time_ms', 'frame_interval_ms', 'app_latency_ms']) {
        validateDistribution(metrics[name], `metrics.${name}`, errors);
    }
    if (!metrics.stage_ms || typeof metrics.stage_ms !== 'object') {
        errors.push('metrics.stage_ms is missing');
    } else {
        for (const stage of ['capture', 'ring', 'tracker', 'rig', 'render']) {
            if (!(stage in metrics.stage_ms)) {
                errors.push(`metrics.stage_ms.${stage} is missing`);
            } else if (metrics.stage_ms[stage] !== null) {
                validateDistribution(metrics.stage_ms[stage], `metrics.stage_ms.${stage}`, errors);
            }
        }
    }
    for (const key of ['dropped_frames', 'dropped_tracking_frames']) {
        if (!Number.isInteger(metrics[key]) || metrics[key] < 0) {
            errors.push(`metrics.${key} is missing or negative`);
        }
    }
    if (typeof metrics.rss_bytes !== 'number' || metrics.rss_bytes < 0) {
        errors.push('metrics.rss_bytes is missing or negative');
    }
    if (typeof metrics.cpu_percent_one_core !== 'number' || metrics.cpu_percent_one_core < 0) {
        errors.push('metrics.cpu_percent_one_core is missing or negative');
    }
    if (metrics.gpu_percent !== null && (typeof metrics.gpu_percent !== 'number' || metrics.gpu_percent < 0 || metrics.gpu_percent > 100)) {
        errors.push('metrics.gpu_percent is not null and not in 0..100');
    }
    return errors;
}

function runValidate(path, quiet) {
    const result = readJson(path);
    const errors = validateResult(result);
    if (errors.length > 0) {
        console.error(`check.mjs: ${path} is not a valid avatar_bench result`);
        for (const error of errors) {
            console.error(`  - ${error}`);
        }
        return 1;
    }
    if (!quiet) {
        console.log(
            `PASS validate ${path} (scenario=${result.scenario}, measured=${result.metrics.frames.measured})`,
        );
    }
    return 0;
}

function runCompare(args) {
    const baseline = readJson(args.baseline);
    if (!baseline || baseline.schema !== 'avatar_bench.baseline.v1') {
        throw new Error(`${args.baseline} is not an avatar_bench baseline (schema mismatch)`);
    }
    if (!Array.isArray(baseline.checks) || baseline.checks.length === 0) {
        throw new Error(`${args.baseline} has no checks`);
    }
    const result = args.result ? readJson(args.result) : baseline.result;
    if (!result) {
        throw new Error('no result supplied and the baseline has no embedded result');
    }
    const shapeErrors = validateResult(result);
    if (shapeErrors.length > 0) {
        console.error('check.mjs: result is not a valid avatar_bench result');
        for (const error of shapeErrors) {
            console.error(`  - ${error}`);
        }
        return 1;
    }
    if (baseline.scenario && result.scenario !== baseline.scenario) {
        console.error(
            `check.mjs: scenario mismatch: baseline=${baseline.scenario} result=${result.scenario}`,
        );
        return 1;
    }

    const defaultAllowance =
        args.allowance !== null ? args.allowance : Number.isFinite(baseline.allowance) ? baseline.allowance : 0.1;
    let failures = 0;

    if (baseline.hardware && baseline.hardware.fingerprint && result.hardware && result.hardware.fingerprint) {
        if (baseline.hardware.fingerprint !== result.hardware.fingerprint) {
            const message =
                `hardware fingerprint differs: baseline=${baseline.hardware.fingerprint} ` +
                `result=${result.hardware.fingerprint} (${result.hardware.os ?? 'unknown'}, ${result.hardware.cpu ?? 'unknown'})`;
            if (args.requireHardware) {
                console.error(`FAIL hardware: ${message}`);
                failures += 1;
            } else {
                console.warn(`WARN hardware: ${message}; baseline is local, numbers are not comparable`);
            }
        }
    }

    for (const check of baseline.checks) {
        const metric = check.metric;
        const operator = check.operator ?? '<=';
        const target = check.target;
        const allowance = Number.isFinite(check.allowance) ? check.allowance : defaultAllowance;
        const value = getPath(result, metric);
        if (typeof target !== 'number') {
            console.error(`FAIL ${metric}: baseline check has no numeric target`);
            failures += 1;
            continue;
        }
        if (typeof value !== 'number') {
            console.error(`FAIL ${metric}: result value is missing or not a number (target=${format(target)})`);
            failures += 1;
            continue;
        }
        const allowed = allowedValue(target, operator, allowance);
        const passed = compare(value, operator, allowed);
        if (passed) {
            if (!args.quiet) {
                console.log(
                    `PASS ${metric} ${operator} ${format(target)} -> allowed ${format(allowed)} (result ${format(value)}, allowance ${(allowance * 100).toFixed(0)}%)`,
                );
            }
        } else {
            console.error(
                `FAIL ${metric}: result=${format(value)} target=${format(target)} allowed=${format(allowed)} operator=${operator} allowance=${(allowance * 100).toFixed(0)}%`,
            );
            failures += 1;
        }
    }

    if (failures > 0) {
        console.error(`check.mjs: FAIL (${failures} metric${failures === 1 ? '' : 's'})`);
        return 1;
    }
    if (!args.quiet) {
        console.log(`check.mjs: PASS (${baseline.checks.length} checks, allowance ${(defaultAllowance * 100).toFixed(0)}%)`);
    }
    return 0;
}

let args;
try {
    args = parseArgs(process.argv.slice(2));
} catch (error) {
    console.error(`check.mjs: ${error.message}`);
    console.error(USAGE);
    process.exit(2);
}

try {
    if (args.validate) {
        process.exit(runValidate(args.validate, args.quiet));
    }
    process.exit(runCompare(args));
} catch (error) {
    console.error(`check.mjs: ${error.message}`);
    process.exit(2);
}
