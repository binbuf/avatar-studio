#!/usr/bin/env node
// scripts/check-design-links.mjs
//
// Verifies the tracked design set in docs/design/:
//
//   1. Every relative link resolves (file or directory), and every `#anchor` matches a heading in
//      the target document. Fenced code blocks and inline code spans are ignored.
//   2. Every ADR file is indexed in docs/design/adr/README.md, the index status matches the ADR's
//      own Status line, and every `ADR-NNNN` mention names an ADR that exists.
//   3. No design document names a task id (T07, T42a, ...) that has no file in docs/tasks/.
//      The task workspace is gitignored, so a missing docs/tasks/ is a warning unless
//      --require-tasks is passed.
//   4. Every design document is covered by docs/design/implementation-map.md.
//
// Usage: node scripts/check-design-links.mjs [--require-tasks] [--quiet]
// Exit code: 0 when clean, 1 when any error is found.

import { existsSync, readFileSync, readdirSync, statSync } from 'node:fs';
import { dirname, join, relative, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

const scriptDir = dirname(fileURLToPath(import.meta.url));
const root = resolve(scriptDir, '..');
const designDir = join(root, 'docs', 'design');
const adrDir = join(designDir, 'adr');
const adrIndexPath = join(adrDir, 'README.md');
const mapPath = join(designDir, 'implementation-map.md');
const tasksDir = join(root, 'docs', 'tasks');

const argv = process.argv.slice(2);
const options = {
    quiet: argv.includes('--quiet'),
    requireTasks: argv.includes('--require-tasks'),
    help: argv.includes('--help') || argv.includes('-h'),
};

if (options.help) {
    console.log('Usage: node scripts/check-design-links.mjs [--require-tasks] [--quiet]');
    process.exit(0);
}

const errors = [];
const warnings = [];

function error(where, message) {
    errors.push({ where, message });
}

function warn(where, message) {
    warnings.push({ where, message });
}

function displayPath(path) {
    return relative(root, path).split(sep).join('/');
}

function walkMarkdown(dir) {
    const found = [];
    for (const entry of readdirSync(dir, { withFileTypes: true })) {
        const full = join(dir, entry.name);
        if (entry.isDirectory()) {
            found.push(...walkMarkdown(full));
        } else if (entry.isFile() && entry.name.endsWith('.md')) {
            found.push(full);
        }
    }
    return found;
}

// GitHub heading slug: lowercase, drop punctuation, spaces become hyphens. Duplicate headings get
// -1, -2 suffixes in document order.
function slugify(text) {
    return text
        .trim()
        .toLowerCase()
        .replace(/[`*_~[\]()<>]/g, '')
        .replace(/[^\p{L}\p{N}\s_-]/gu, '')
        .replace(/\s/g, '-');
}

function scanMarkdown(text) {
    const lines = text.split(/\r?\n/);
    const headings = [];
    const seen = new Map();
    const links = [];
    const taskIds = [];
    const adrMentions = [];
    let fence = null;

    lines.forEach((line, index) => {
        const fenceOpen = line.match(/^\s*(`{3,}|~{3,})/);
        if (fenceOpen) {
            const marker = fenceOpen[1];
            if (fence === null) {
                fence = marker[0];
            } else if (marker[0] === fence) {
                fence = null;
            }
            return;
        }
        if (fence !== null) {
            return;
        }

        const heading = line.match(/^(#{1,6})\s+(.*?)\s*$/);
        if (heading) {
            const raw = heading[2].replace(/`/g, '');
            let slug = slugify(raw);
            const count = seen.get(slug) ?? 0;
            seen.set(slug, count + 1);
            if (count > 0) {
                slug = `${slug}-${count}`;
            }
            headings.push({ slug, line: index + 1, text: raw });
        }

        const prose = line.replace(/`[^`]*`/g, (span) => ' '.repeat(span.length));
        for (const match of prose.matchAll(/\[[^\]]*\]\(([^)\s]+)(?:\s+"[^"]*")?\)/g)) {
            links.push({ target: match[1], line: index + 1 });
        }

        for (const match of line.matchAll(/(?<![0-9A-Za-z_:])T(\d{2}[a-z]?)(?![0-9A-Za-z:_])/g)) {
            taskIds.push({ id: `T${match[1]}`, line: index + 1 });
        }

        for (const match of line.matchAll(/ADR-(\d{4})/g)) {
            adrMentions.push({ id: `ADR-${match[1]}`, line: index + 1 });
        }
    });

    return { headings, links, taskIds, adrMentions };
}

function isExternal(target) {
    return /^(?:[a-z][a-z0-9+.-]*:|\/\/)/i.test(target);
}

const files = walkMarkdown(designDir).sort();
const parsed = new Map();
for (const file of files) {
    parsed.set(file, scanMarkdown(readFileSync(file, 'utf8')));
}

const adrFiles = readdirSync(adrDir)
    .filter((name) => /^\d{4}-.*\.md$/.test(name) && name !== '0000-template.md')
    .sort();
const adrByNumber = new Map(adrFiles.map((name) => [name.slice(0, 4), join(adrDir, name)]));

// --- 1. relative links and anchors ---------------------------------------------------------------

let linkCount = 0;
for (const [file, doc] of parsed) {
    const where = displayPath(file);
    for (const link of doc.links) {
        if (isExternal(link.target)) {
            continue;
        }
        linkCount += 1;
        const at = `${where}:${link.line}`;
        const hashIndex = link.target.indexOf('#');
        const rawPath = hashIndex >= 0 ? link.target.slice(0, hashIndex) : link.target;
        const anchor = hashIndex >= 0 ? decodeURIComponent(link.target.slice(hashIndex + 1)) : '';

        let targetFile = file;
        if (rawPath.length > 0) {
            let decoded;
            try {
                decoded = decodeURIComponent(rawPath);
            } catch {
                error(at, `malformed percent-encoding in link: ${link.target}`);
                continue;
            }
            targetFile = resolve(dirname(file), decoded);
            if (!existsSync(targetFile)) {
                error(at, `broken relative link: ${link.target}`);
                continue;
            }
            if (anchor.length === 0) {
                continue;
            }
            if (statSync(targetFile).isDirectory() || !targetFile.endsWith('.md')) {
                continue;
            }
        }

        if (anchor.length === 0) {
            continue;
        }
        const targetDoc = parsed.get(targetFile);
        if (!targetDoc) {
            continue;
        }
        const wanted = decodeURIComponent(anchor).toLowerCase();
        if (!targetDoc.headings.some((heading) => heading.slug === wanted)) {
            error(at, `missing anchor #${anchor} in ${displayPath(targetFile)}`);
        }
    }
}

// --- 2. ADR index, index status, and ADR mentions ------------------------------------------------

const indexedAdrs = new Map();
if (!existsSync(adrIndexPath)) {
    error(displayPath(adrIndexPath), 'ADR index is missing');
} else {
    const indexText = readFileSync(adrIndexPath, 'utf8');
    for (const line of indexText.split(/\r?\n/)) {
        const row = line.match(/^\|\s*\[(\d{4})\]\(([^)]+)\)\s*\|\s*([^|]*?)\s*\|\s*([^|]*?)\s*\|/);
        if (row) {
            indexedAdrs.set(row[1], { file: row[2], status: row[4].trim() });
        }
    }
}

for (const name of adrFiles) {
    const number = name.slice(0, 4);
    const file = join(adrDir, name);
    const row = indexedAdrs.get(number);
    if (!row) {
        error(displayPath(file), `ADR-${number} is not indexed in adr/README.md`);
        continue;
    }
    if (row.file !== name) {
        error(displayPath(adrIndexPath), `ADR-${number} is indexed as ${row.file}, but the file is ${name}`);
    }
    const statusLine = readFileSync(file, 'utf8').match(/^-\s*\*\*Status:\*\*\s*(.+)$/m);
    if (!statusLine) {
        error(displayPath(file), 'ADR has no "- **Status:**" line');
        continue;
    }
    const status = statusLine[1].trim().replace(/\s+/g, ' ');
    if (status !== row.status) {
        error(displayPath(file), `status "${status}" does not match the index ("${row.status}")`);
    }
}

for (const [file, doc] of parsed) {
    for (const mention of doc.adrMentions) {
        if (!adrByNumber.has(mention.id.slice(4))) {
            error(`${displayPath(file)}:${mention.line}`, `references ${mention.id}, which does not exist`);
        }
    }
}

// --- 3. task ids --------------------------------------------------------------------------------

let taskCount = 0;
const knownTasks = new Set();
let taskWorkspace = false;
if (existsSync(tasksDir) && statSync(tasksDir).isDirectory()) {
    for (const name of readdirSync(tasksDir)) {
        const match = name.match(/^(\d{2}[a-z]?)-.+\.md$/);
        if (match) {
            knownTasks.add(`T${match[1]}`);
        }
    }
    taskWorkspace = knownTasks.size > 0;
}

if (!taskWorkspace) {
    const message =
        'docs/tasks/ is absent (gitignored planning workspace); task-id references were not checked';
    if (options.requireTasks) {
        error('docs/tasks', message);
    } else {
        warn('docs/tasks', message);
    }
} else {
    for (const [file, doc] of parsed) {
        for (const task of doc.taskIds) {
            taskCount += 1;
            if (!knownTasks.has(task.id)) {
                error(`${displayPath(file)}:${task.line}`, `references ${task.id}, which has no task file`);
            }
        }
    }
}

// --- 4. implementation map coverage -------------------------------------------------------------

if (!existsSync(mapPath)) {
    error(displayPath(mapPath), 'implementation map is missing');
} else {
    const mapDoc = parsed.get(mapPath) ?? scanMarkdown(readFileSync(mapPath, 'utf8'));
    const covered = new Set(
        mapDoc.links
            .filter((link) => !isExternal(link.target))
            .map((link) => resolve(dirname(mapPath), link.target.split('#')[0]))
            .filter((target) => target !== mapPath),
    );
    for (const file of files) {
        if (file === mapPath || file === join(adrDir, '0000-template.md')) {
            continue;
        }
        if (!covered.has(file)) {
            error(displayPath(mapPath), `does not cover design document ${displayPath(file)}`);
        }
    }
}

// --- report -------------------------------------------------------------------------------------

for (const warning of warnings) {
    console.warn(`WARN  ${warning.where}  ${warning.message}`);
}
for (const failure of errors) {
    console.error(`FAIL  ${failure.where}  ${failure.message}`);
}

if (!options.quiet) {
    console.log(
        `design-links: ${files.length} design files, ${linkCount} relative links, ${adrFiles.length} ADRs, ` +
            `${taskCount} task references — ${errors.length} error(s), ${warnings.length} warning(s)`,
    );
}

process.exit(errors.length === 0 ? 0 : 1);
