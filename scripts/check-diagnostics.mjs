#!/usr/bin/env node
// Diagnostics source scan.
//
// Two rules from docs/design/diagnostics.md:
//   1. A literal AREA-NNN code must exist in the DiagnosticCode enum.
//   2. A log call must carry a code; raw printf/cout/cerr and GDScript print()
//      are rejected unless the line also names a diagnostic code.
//
// The scan is advisory until T41 wires the generated string table: violations
// print a warning and exit 0. Pass --strict (or set AVATAR_DIAGNOSTICS_STRICT=1)
// to make them fail, which is what the T06 gate and the self-test exercise.
//
// Usage:
//   node scripts/check-diagnostics.mjs [--root DIR] [--header FILE]
//                                      [--strict] [--json] [--quiet] [--self-test]
//
// Exit codes: 0 pass or advisory warning, 1 strict failure, 2 usage or I/O error.

import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const defaultRoot = path.resolve(scriptDir, '..');

const SOURCE_EXTENSIONS = new Set([
    '.h', '.hh', '.hpp', '.hxx', '.ipp', '.inl', '.c', '.cc', '.cpp', '.cxx', '.gd',
]);

const CODE_PATTERN = /\b([A-Z]{2,5})-(\d{3})\b/g;
const RAW_LOG_PATTERN = /\b(?:std::)?(printf|fprintf|cout|cerr|clog|puts|perror)\b/;
const GDSCRIPT_LOG_PATTERN = /\b(print|printerr|push_error|push_warning)\s*\(/;

function parseArgs(argv) {
    const options = {
        root: defaultRoot,
        header: '',
        strict: process.env.AVATAR_DIAGNOSTICS_STRICT === '1',
        json: false,
        quiet: false,
        selfTest: false,
    };
    for (let index = 0; index < argv.length; index += 1) {
        const argument = argv[index];
        switch (argument) {
            case '--root':
                options.root = path.resolve(requireValue(argv, ++index, argument));
                break;
            case '--header':
                options.header = path.resolve(requireValue(argv, ++index, argument));
                break;
            case '--strict':
                options.strict = true;
                break;
            case '--json':
                options.json = true;
                break;
            case '--quiet':
                options.quiet = true;
                break;
            case '--self-test':
                options.selfTest = true;
                break;
            case '-h':
            case '--help':
                printUsage();
                process.exit(0);
                break;
            default:
                throw new UsageError(`unknown argument: ${argument}`);
        }
    }
    if (options.header === '') {
        options.header = path.join(options.root, 'native', 'core', 'include', 'avatar', 'core',
            'diagnostics.h');
    }
    return options;
}

function requireValue(argv, index, argument) {
    if (index >= argv.length) {
        throw new UsageError(`${argument} needs a value`);
    }
    return argv[index];
}

class UsageError extends Error {}

function printUsage() {
    process.stdout.write(
        'usage: node scripts/check-diagnostics.mjs [--root DIR] [--header FILE]\n' +
        '                                          [--strict] [--json] [--quiet]\n' +
        '                                          [--self-test]\n',
    );
}

function walkSources(directory, results) {
    if (!fs.existsSync(directory)) {
        return;
    }
    for (const entry of fs.readdirSync(directory, { withFileTypes: true })) {
        const full = path.join(directory, entry.name);
        if (entry.isDirectory()) {
            if (['.git', '.tools', 'build', 'node_modules'].includes(entry.name) ||
                entry.name.startsWith('build-')) {
                continue;
            }
            walkSources(full, results);
        } else if (entry.isFile() && SOURCE_EXTENSIONS.has(path.extname(entry.name))) {
            results.push(full);
        }
    }
}

function parseRegistry(headerPath) {
    const text = fs.readFileSync(headerPath, 'utf8');
    const codes = new Set();
    const enumPattern = /^\s*([a-z]{2,4})_(\d{3})\s*=\s*\d+/gm;
    let match;
    while ((match = enumPattern.exec(text)) !== null) {
        codes.add(`${match[1].toUpperCase()}-${match[2]}`);
    }
    return codes;
}

function lineAllowed(lines, index) {
    if (lines[index].includes('diagnostics-scan: allow')) {
        return true;
    }
    return index > 0 && lines[index - 1].includes('diagnostics-scan: allow');
}

function scanFile(file, knownCodes, violations) {
    const lines = fs.readFileSync(file, 'utf8').split(/\r?\n/);
    const isGdScript = path.extname(file) === '.gd';

    for (let index = 0; index < lines.length; index += 1) {
        const line = lines[index];
        if (lineAllowed(lines, index)) {
            continue;
        }
        const lineNumber = index + 1;
        let hasCode = false;

        CODE_PATTERN.lastIndex = 0;
        let codeMatch;
        while ((codeMatch = CODE_PATTERN.exec(line)) !== null) {
            const code = `${codeMatch[1]}-${codeMatch[2]}`;
            hasCode = true;
            if (!knownCodes.has(code)) {
                violations.push({
                    rule: 'unknown-code',
                    file,
                    line: lineNumber,
                    text: line.trim(),
                    message: `literal '${code}' is not in the DiagnosticCode enum`,
                });
            }
        }

        if (isGdScript) {
            const logMatch = GDSCRIPT_LOG_PATTERN.exec(line);
            if (logMatch && !hasCode) {
                violations.push({
                    rule: 'log-without-code',
                    file,
                    line: lineNumber,
                    text: line.trim(),
                    message: `GDScript ${logMatch[1]}() call has no diagnostic code`,
                });
            }
        } else {
            const logMatch = RAW_LOG_PATTERN.exec(line);
            if (logMatch && !hasCode) {
                violations.push({
                    rule: 'log-without-code',
                    file,
                    line: lineNumber,
                    text: line.trim(),
                    message: `raw ${logMatch[0]} output has no diagnostic code; use Logger`,
                });
            }
        }
    }
}

function runScan(options) {
    const errors = [];
    let knownCodes;
    try {
        knownCodes = parseRegistry(options.header);
    } catch (error) {
        return { knownCodes: new Set(), files: [], violations: [], errors: [
            `cannot read diagnostics header ${options.header}: ${error.message}`,
        ] };
    }
    if (knownCodes.size === 0) {
        errors.push(`no DiagnosticCode values found in ${options.header}`);
    }

    const files = [];
    walkSources(path.join(options.root, 'native'), files);
    walkSources(path.join(options.root, 'app'), files);
    const filtered = files.filter((file) =>
        !path.relative(options.root, file).replaceAll('\\', '/').startsWith('app/tests/') &&
        path.resolve(file) !== path.resolve(options.header));

    const violations = [];
    for (const file of filtered) {
        scanFile(file, knownCodes, violations);
    }
    return { knownCodes, files: filtered, violations, errors };
}

function report(options, result) {
    if (!options.json) {
        process.stdout.write(
            `diagnostics scan: ${result.knownCodes.size} registered codes, ` +
            `${result.files.length} files\n`);
    }
    if (result.errors.length > 0) {
        if (!options.json) {
            process.stdout.write('diagnostics scan: FAIL (cannot scan)\n');
            for (const error of result.errors) {
                process.stdout.write(`  - ${error}\n`);
            }
        } else {
            process.stdout.write(`${JSON.stringify({ ok: false, errors: result.errors }, null, 2)}\n`);
        }
        return 1;
    }
    if (result.violations.length === 0) {
        if (!options.quiet) {
            process.stdout.write('diagnostics scan: PASS\n');
        }
        return 0;
    }

    const strict = options.strict;
    if (options.json) {
        process.stdout.write(`${JSON.stringify({
            ok: !strict,
            advisory: !strict,
            violations: result.violations,
        }, null, 2)}\n`);
    } else {
        process.stdout.write(
            `diagnostics scan: ${strict ? 'FAIL' : 'ADVISORY'} ` +
            `(${result.violations.length} finding(s))\n`);
        for (const violation of result.violations) {
            process.stdout.write(
                `  - ${path.relative(process.cwd(), violation.file)}:${violation.line}: ` +
                `${violation.message}\n`);
        }
        if (!strict) {
            process.stdout.write(
                '  advisory until T41 wires the string table; run with --strict to fail\n');
        }
    }
    return strict ? 1 : 0;
}

function writeFixture(root, relative, content) {
    const file = path.join(root, relative);
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(file, content, 'utf8');
    return file;
}

function scanFixture(root, header) {
    return runScan({ root, header, strict: true, json: false, quiet: true, selfTest: false });
}

function runSelfTest(options) {
    const tempRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'avatar-diagnostics-'));
    const failures = [];
    const check = (label, condition) => {
        if (!condition) {
            failures.push(label);
        }
    };
    try {
        // 1. The real tree passes strict mode.
        const real = runScan(options);
        check('real tree has no diagnostics findings', real.violations.length === 0);
        for (const violation of real.violations) {
            failures.push(`real tree: ${violation.message} at ${violation.file}:${violation.line}`);
        }

        // 2. An unknown literal code is rejected.
        const unknownRoot = path.join(tempRoot, 'unknown');
        writeFixture(unknownRoot, 'native/src/bad.cpp',
            'void f() { /* TRK-999 */ }\n');
        const unknown = scanFixture(unknownRoot, options.header);
        check('unknown literal code is rejected',
            unknown.violations.some((violation) =>
                violation.rule === 'unknown-code' && violation.message.includes('TRK-999')));

        // 3. A raw log call without a code is rejected.
        const rawRoot = path.join(tempRoot, 'raw');
        writeFixture(rawRoot, 'native/src/raw.cpp',
            '#include <cstdio>\nvoid f() { std::printf("no code here\\n"); }\n');
        const raw = scanFixture(rawRoot, options.header);
        check('raw log call without a code is rejected',
            raw.violations.some((violation) => violation.rule === 'log-without-code'));

        // 4. A known code and an explicit suppression are accepted.
        const goodRoot = path.join(tempRoot, 'good');
        writeFixture(goodRoot, 'native/src/good.cpp',
            '// TRK-002 is registered.\n' +
            'void f() { /* CAP-001 */ }\n' +
            '// diagnostics-scan: allow\n' +
            'void g() { std::printf("intentional CLI output\\n"); }\n');
        const good = scanFixture(goodRoot, options.header);
        check('registered code and suppression are accepted', good.violations.length === 0);
        for (const violation of good.violations) {
            failures.push(`suppression: ${violation.message}`);
        }
    } finally {
        fs.rmSync(tempRoot, { recursive: true, force: true });
    }

    if (failures.length > 0) {
        process.stdout.write('diagnostics scan self-test: FAIL\n');
        for (const failure of failures) {
            process.stdout.write(`  - ${failure}\n`);
        }
        return 1;
    }
    process.stdout.write('diagnostics scan self-test: PASS (real tree, unknown code, raw log, ' +
        'suppression)\n');
    return 0;
}

function main() {
    let options;
    try {
        options = parseArgs(process.argv.slice(2));
    } catch (error) {
        if (error instanceof UsageError) {
            process.stderr.write(`check-diagnostics: ${error.message}\n`);
            printUsage();
            return 2;
        }
        throw error;
    }
    if (options.selfTest) {
        return runSelfTest(options);
    }
    return report(options, runScan(options));
}

process.exitCode = main();
