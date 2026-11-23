#!/usr/bin/env node
// Licence and dependency manifest gate.
//
// native/licenses.json is the source of truth for every third-party component.
// This script fails the build when:
//   - a component is used by a source file but has no manifest entry,
//   - a manifest entry is missing a required field,
//   - a licence is on the deny list (GPL/AGPL/SSPL/non-commercial/proprietary),
//   - an active component that must be used has no detected usage.
//
// It scans C++ include directives. Quoted includes are project-local and are
// ignored; angle includes are standard library, system, or third-party. The
// standard-library and system allow-lists live here so that a new platform
// header is a reviewed change, not a silent dependency.
//
// Usage:
//   node scripts/check-licenses.mjs [--root DIR] [--manifest FILE]
//                                   [--scan DIR ...] [--json] [--quiet]
//                                   [--self-test]
//
// Exit codes: 0 pass, 1 policy failure, 2 usage or I/O error.

import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const defaultRoot = path.resolve(scriptDir, '..');

const SOURCE_EXTENSIONS = new Set([
    '.h', '.hh', '.hpp', '.hxx', '.ipp', '.inl', '.c', '.cc', '.cpp', '.cxx',
]);

// C and C++ standard headers. A header without a slash is matched here.
const STDLIB_HEADERS = new Set([
    'algorithm', 'any', 'array', 'atomic', 'barrier', 'bit', 'bitset', 'cassert',
    'cctype', 'cerrno', 'cfenv', 'cfloat', 'charconv', 'chrono', 'cinttypes',
    'climits', 'clocale', 'cmath', 'codecvt', 'compare', 'complex', 'concepts',
    'condition_variable', 'coroutine', 'csetjmp', 'csignal', 'cstdarg', 'cstddef',
    'cstdint', 'cstdio', 'cstdlib', 'cstring', 'ctime', 'cuchar', 'cwchar', 'cwctype',
    'deque', 'exception', 'execution', 'expected', 'filesystem', 'flat_map',
    'flat_set', 'format', 'forward_list', 'fstream', 'functional', 'future',
    'generator', 'initializer_list', 'inplace_vector', 'iomanip', 'ios', 'iosfwd',
    'iostream', 'istream', 'iterator', 'latch', 'limits', 'list', 'locale', 'map',
    'mdspan', 'memory', 'memory_resource', 'mutex', 'new', 'numbers', 'numeric',
    'optional', 'ostream', 'print', 'queue', 'random', 'ranges', 'ratio', 'regex',
    'scoped_allocator', 'semaphore', 'set', 'shared_mutex', 'source_location',
    'span', 'sstream', 'stack', 'stdexcept', 'stdfloat', 'stop_token', 'streambuf',
    'string', 'string_view', 'syncstream', 'system_error', 'thread', 'tuple',
    'type_traits', 'typeindex', 'typeinfo', 'unordered_map', 'unordered_set',
    'utility', 'valarray', 'variant', 'vector', 'version',
    // C headers, including the .h spellings that appear in C++ code.
    'assert.h', 'ctype.h', 'errno.h', 'fenv.h', 'float.h', 'inttypes.h', 'limits.h',
    'locale.h', 'math.h', 'setjmp.h', 'signal.h', 'stdarg.h', 'stddef.h', 'stdint.h',
    'stdio.h', 'stdlib.h', 'string.h', 'time.h', 'uchar.h', 'wchar.h', 'wctype.h',
]);

// Platform SDK headers. These are OS contracts, not bundled third-party
// components. Add a prefix here when a new platform API is used.
const SYSTEM_PREFIXES = [
    'asm/', 'linux/', 'mach/', 'mach-o/', 'sys/', 'unistd.h', 'pthread.h', 'dlfcn.h',
    'intrin.h', 'x86intrin.h', 'immintrin.h', 'arm_neon.h', 'windows.h',
    'winbase.h', 'windef.h', 'winerror.h', 'winsock2.h', 'ws2tcpip.h',
    'objbase.h', 'unknwn.h', 'combaseapi.h', 'comdef.h', 'strmif.h', 'dshow.h',
    'ks.h', 'ksmedia.h', 'mfapi.h', 'mferror.h', 'mfidl.h', 'mfreadwrite.h',
    'mfsharingengine.h', 'mftransform.h', 'mfvirtualcamera.h',
    'shlobj.h', 'knownfolders.h', 'sddl.h', 'accctrl.h', 'aclapi.h',
    'wincrypt.h', 'bcrypt.h', 'ncrypt.h', 'setupapi.h', 'cfgmgr32.h', 'devpkey.h',
    'initguid.h', 'roapi.h', 'psapi.h', 'pdh.h', 'pdhmsg.h', 'timeapi.h',
    'tlhelp32.h', 'dirent.h', 'fcntl.h', 'poll.h',
    'mmsystem.h', 'avrt.h', 'd3d11.h', 'd3d12.h', 'dxgi.h', 'dxgi1_6.h',
    'wil/', 'wrl/', 'CoreFoundation/', 'CoreMedia/', 'CoreVideo/', 'CoreAudio/',
    'IOSurface/', 'VideoToolbox/', 'Foundation/', 'AppKit/', 'AVFoundation/',
    'pipewire/', 'spa/', 'vulkan/', 'volk.h', 'vk_mem_alloc.h',
];

function parseArgs(argv) {
    const options = {
        root: defaultRoot,
        manifest: '',
        scans: [],
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
            case '--manifest':
                options.manifest = path.resolve(requireValue(argv, ++index, argument));
                break;
            case '--scan':
                options.scans.push(path.resolve(requireValue(argv, ++index, argument)));
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
    if (options.manifest === '') {
        options.manifest = path.join(options.root, 'native', 'licenses.json');
    }
    if (options.scans.length === 0) {
        options.scans = ['native', 'tests', 'app']
            .map((name) => path.join(options.root, name))
            .filter((directory) => fs.existsSync(directory));
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
        'usage: node scripts/check-licenses.mjs [--root DIR] [--manifest FILE]\n' +
        '                                       [--scan DIR ...] [--json] [--quiet]\n' +
        '                                       [--self-test]\n',
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

function normaliseInclude(include) {
    return include.replaceAll('\\', '/');
}

function isStandardHeader(include) {
    return STDLIB_HEADERS.has(include);
}

function isSystemHeader(include) {
    return SYSTEM_PREFIXES.some((prefix) =>
        prefix.endsWith('/') ? include.startsWith(prefix) : include === prefix);
}

function matchesPrefix(include, prefixes) {
    return (prefixes || []).some((prefix) =>
        prefix.endsWith('/') ? include.startsWith(prefix) : include === prefix);
}

function validateManifest(manifest, errors) {
    if (!manifest || typeof manifest !== 'object') {
        errors.push('manifest is not a JSON object');
        return;
    }
    if (manifest.schema !== 'avatar.licenses.v1') {
        errors.push(`manifest schema is '${manifest.schema}', expected 'avatar.licenses.v1'`);
    }
    if (!Array.isArray(manifest.components) || manifest.components.length === 0) {
        errors.push('manifest has no components');
    }
    if (!manifest.policy || typeof manifest.policy !== 'object') {
        errors.push('manifest has no policy block');
    }
    const ids = new Set();
    for (const [index, component] of (manifest.components || []).entries()) {
        const label = component && component.id ? component.id : `component[${index}]`;
        for (const field of ['id', 'name', 'version', 'license', 'url']) {
            if (typeof component?.[field] !== 'string' || component[field].trim() === '') {
                errors.push(`${label} is missing required field '${field}'`);
            }
        }
        if (typeof component?.modified !== 'boolean') {
            errors.push(`${label} is missing required boolean field 'modified'`);
        }
        if (component?.status && !['active', 'planned', 'deprecated'].includes(component.status)) {
            errors.push(`${label} has unknown status '${component.status}'`);
        }
        if (component?.id) {
            if (ids.has(component.id)) {
                errors.push(`duplicate component id '${component.id}'`);
            }
            ids.add(component.id);
        }
    }
    for (const exception of (manifest.policy?.exceptions || [])) {
        if (!exception.id || !ids.has(exception.id)) {
            errors.push(`deny-list exception names unknown component '${exception.id}'`);
        }
        if (!exception.reason || !exception.adr) {
            errors.push(`deny-list exception for '${exception.id}' needs reason and adr`);
        }
    }
}

function licenceDenied(license, policy) {
    const normalised = String(license || '').trim().toUpperCase();
    const exact = (policy?.deny_spdx || []).map((entry) => entry.toUpperCase());
    if (exact.includes(normalised)) {
        return true;
    }
    return (policy?.deny_patterns || []).some((pattern) => {
        try {
            return new RegExp(pattern, 'i').test(license);
        } catch {
            return false;
        }
    });
}

function scanUsages(files, manifest, errors) {
    const components = manifest.components || [];
    const usages = new Map();
    const includePattern = /^\s*#\s*include\s*([<"])([^>"]+)[>"]/;

    for (const file of files) {
        const lines = fs.readFileSync(file, 'utf8').split(/\r?\n/);
        for (let index = 0; index < lines.length; index += 1) {
            const match = includePattern.exec(lines[index]);
            if (!match || match[1] !== '<') {
                continue;
            }
            const include = normaliseInclude(match[2].trim());
            if (isStandardHeader(include) || isSystemHeader(include)) {
                continue;
            }
            const component = components.find((candidate) =>
                matchesPrefix(include, candidate.include_prefixes));
            if (!component) {
                errors.push(
                    `third-party include without a manifest entry: <${include}> at ` +
                    `${path.relative(process.cwd(), file)}:${index + 1}`);
                continue;
            }
            if (!usages.has(component.id)) {
                usages.set(component.id, []);
            }
            usages.get(component.id).push(`${file}:${index + 1}`);
        }
    }
    return usages;
}

function runCheck(options) {
    const errors = [];
    let manifest;
    try {
        const text = fs.readFileSync(options.manifest, 'utf8').replace(/^\uFEFF/, '');
        manifest = JSON.parse(text);
    } catch (error) {
        return {
            manifest: null,
            usages: new Map(),
            errors: [`cannot read manifest ${options.manifest}: ${error.message}`],
            files: [],
        };
    }

    validateManifest(manifest, errors);

    const files = [];
    for (const directory of options.scans) {
        walkSources(directory, files);
    }
    const usages = scanUsages(files, manifest, errors);

    for (const component of (manifest.components || [])) {
        if (licenceDenied(component.license, manifest.policy)) {
            const exception = (manifest.policy?.exceptions || [])
                .find((entry) => entry.id === component.id);
            if (!exception) {
                errors.push(
                    `licence '${component.license}' for '${component.id}' is on the deny list ` +
                    '(add a reviewed exception to native/licenses.json if it is a separate process)');
            }
        }
        if (component.require_usage === true && !usages.has(component.id)) {
            errors.push(
                `'${component.id}' is marked require_usage but no source includes it`);
        }
    }

    return { manifest, usages, errors, files };
}

function report(options, result) {
    const components = result.manifest?.components?.length ?? 0;
    const active = (result.manifest?.components || [])
        .filter((component) => component.status === 'active').length;
    const includes = [...result.usages.values()].reduce((sum, list) => sum + list.length, 0);
    if (!options.json) {
        process.stdout.write(
            `licence check: ${components} components (${active} active), ` +
            `${includes} third-party includes in ${result.files.length} files\n`);
    }
    if (result.errors.length === 0) {
        if (!options.quiet) {
            process.stdout.write('licence check: PASS\n');
        }
        return 0;
    }
    if (options.json) {
        process.stdout.write(`${JSON.stringify({ ok: false, errors: result.errors }, null, 2)}\n`);
    } else {
        process.stdout.write('licence check: FAIL\n');
        for (const error of result.errors) {
            process.stdout.write(`  - ${error}\n`);
        }
    }
    return 1;
}

function writeFixture(root, relative, content) {
    const file = path.join(root, relative);
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(file, content, 'utf8');
    return file;
}

function runSelfTest(options) {
    const tempRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'avatar-licence-'));
    const failures = [];
    const check = (label, condition) => {
        if (!condition) {
            failures.push(label);
        }
    };
    try {
        // 1. The real tree passes.
        const real = runCheck(options);
        check('real tree has no licence or manifest violations',
            real.errors.length === 0);
        if (real.errors.length > 0) {
            failures.push(...real.errors.map((error) => `real tree: ${error}`));
        }

        // 2. A deliberately unlisted test dependency fails.
        const unlistedRoot = path.join(tempRoot, 'unlisted');
        writeFixture(unlistedRoot, 'tests/unit/bad_dependency.cpp',
            '#include <mysterylib/secret.h>\nint main() { return 0; }\n');
        const unlisted = runCheck({
            ...options,
            manifest: options.manifest,
            scans: [path.join(unlistedRoot, 'tests')],
        });
        check('unlisted dependency is rejected',
            unlisted.errors.some((error) => error.includes('mysterylib/secret.h')));

        // 3. A denied licence fails.
        const manifest = JSON.parse(
            fs.readFileSync(options.manifest, 'utf8').replace(/^\uFEFF/, ''));
        const deniedRoot = path.join(tempRoot, 'denied');
        fs.mkdirSync(deniedRoot, { recursive: true });
        const deniedManifest = path.join(tempRoot, 'denied-licenses.json');
        const badManifest = {
            ...manifest,
            components: manifest.components.concat([{
                id: 'bad-gpl-component',
                name: 'Bad GPL Component',
                version: '1.0.0',
                license: 'GPL-3.0-only',
                url: 'https://example.invalid/bad',
                modified: false,
                status: 'planned',
                include_prefixes: ['bad-gpl/'],
            }]),
        };
        fs.writeFileSync(deniedManifest, JSON.stringify(badManifest, null, 2), 'utf8');
        const denied = runCheck({
            ...options,
            manifest: deniedManifest,
            scans: [deniedRoot],
        });
        check('denied licence is rejected',
            denied.errors.some((error) => error.includes('GPL-3.0-only')));

        // 4. A missing required field fails.
        const missingManifest = path.join(tempRoot, 'missing-field.json');
        const missing = {
            ...manifest,
            components: manifest.components.concat([{
                id: 'missing-version',
                name: 'Missing Version',
                license: 'MIT',
                url: 'https://example.invalid/missing',
                modified: false,
                status: 'planned',
            }]),
        };
        fs.writeFileSync(missingManifest, JSON.stringify(missing, null, 2), 'utf8');
        const missingResult = runCheck({
            ...options,
            manifest: missingManifest,
            scans: [deniedRoot],
        });
        check('missing required field is rejected',
            missingResult.errors.some((error) => error.includes("'version'")));

        // 5. A denied licence with a reviewed exception is allowed.
        const exceptionManifest = path.join(tempRoot, 'exception.json');
        const exception = {
            ...badManifest,
            policy: {
                ...badManifest.policy,
                exceptions: [{
                    id: 'bad-gpl-component',
                    reason: 'separate process, never linked',
                    adr: 'ADR-9999',
                }],
            },
        };
        fs.writeFileSync(exceptionManifest, JSON.stringify(exception, null, 2), 'utf8');
        const exceptionResult = runCheck({
            ...options,
            manifest: exceptionManifest,
            scans: [deniedRoot],
        });
        check('reviewed deny-list exception is allowed',
            !exceptionResult.errors.some((error) => error.includes('deny list')));
    } finally {
        fs.rmSync(tempRoot, { recursive: true, force: true });
    }

    if (failures.length > 0) {
        process.stdout.write('licence check self-test: FAIL\n');
        for (const failure of failures) {
            process.stdout.write(`  - ${failure}\n`);
        }
        return 1;
    }
    process.stdout.write('licence check self-test: PASS (real tree, unlisted include, deny list, ' +
        'required field, reviewed exception)\n');
    return 0;
}

function main() {
    let options;
    try {
        options = parseArgs(process.argv.slice(2));
    } catch (error) {
        if (error instanceof UsageError) {
            process.stderr.write(`check-licenses: ${error.message}\n`);
            printUsage();
            return 2;
        }
        throw error;
    }
    if (options.selfTest) {
        return runSelfTest(options);
    }
    return report(options, runCheck(options));
}

process.exitCode = main();
