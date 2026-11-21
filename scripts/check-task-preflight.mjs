#!/usr/bin/env node
// Check that tasks which need a live camera and a person say so consistently.
//
// The harness has no per-task start hook, so the requirement is carried in three
// places it can see:
//   1. the ROADMAP title suffix "[camera+person]" (printed by status and the
//      "=== TNN title" line at the start of every task session),
//   2. `requires: camera+person` front matter (machine-readable),
//   3. a "## Preflight" section at the top of the task body (read by the agent).
// This script keeps them in sync. It no-ops when the gitignored planning
// workspace is absent, so CI and clean checkouts stay green.

import { existsSync, readdirSync, readFileSync } from 'node:fs';
import { join, resolve } from 'node:path';

const root = resolve(import.meta.dirname, '..');
const roadmapPath = join(root, 'docs', 'ROADMAP.md');
const tasksDir = join(root, 'docs', 'tasks');
const TAG = '[camera+person]';

if (!existsSync(roadmapPath) || !existsSync(tasksDir)) {
    console.log('task-preflight: no local planning workspace, skipped');
    process.exit(0);
}

const roadmap = readFileSync(roadmapPath, 'utf8').split(/\r?\n/);
const bullets = [];
for (const line of roadmap) {
    const match = /^\s*-\s+\[[ xX~]\]\s+(T\d{1,3}[a-z]?\d*)\s+[—–-]\s+(.+)$/.exec(line);
    if (!match) continue;
    const id = match[1];
    let title = match[2];
    const arrow = title.search(/\s+(?:→|->)\s+/);
    if (arrow >= 0) title = title.slice(0, arrow);
    bullets.push({ id, key: id.toUpperCase(), title: title.trim() });
}

const taskFiles = new Map();
for (const name of readdirSync(tasksDir)) {
    const match = /^(\d{1,3})([a-z]\d*)?[-_. ].*\.md$/i.exec(name);
    if (!match) continue;
    const id = `T${match[1]}${match[2] ?? ''}`.toUpperCase();
    taskFiles.set(id, join(tasksDir, name));
}

const errors = [];
const tagged = [];

for (const bullet of bullets) {
    const file = taskFiles.get(bullet.key);
    const titleTagged = bullet.title.includes(TAG);
    let metaTags = new Set();
    let body = '';
    if (file) {
        const text = readFileSync(file, 'utf8');
        const fm = /^---\r?\n([\s\S]*?)\r?\n---\r?\n?/.exec(text);
        body = fm ? text.slice(fm[0].length) : text;
        if (fm) {
            for (const line of fm[1].split(/\r?\n/)) {
                const kv = /^\s*([A-Za-z][\w-]*)\s*:\s*(.*)$/.exec(line);
                if (!kv || kv[1].toLowerCase() !== 'requires') continue;
                for (const token of kv[2].split(/[,+]/).map((v) => v.trim().toLowerCase())) {
                    if (token) metaTags.add(token);
                }
            }
        }
    }

    const metaLive = metaTags.has('camera') && metaTags.has('person');
    if (metaTags.size && !metaLive) {
        errors.push(`${bullet.id}: requires must name "camera+person" (got "${[...metaTags].join('+')}")`);
    }
    if (titleTagged !== metaLive) {
        errors.push(`${bullet.id}: ROADMAP ${titleTagged ? 'has' : 'lacks'} ${TAG} but front matter ${metaLive ? 'has' : 'lacks'} camera+person`);
    }
    if (metaLive) {
        if (!/^##\s+preflight\b/im.test(body)) {
            errors.push(`${bullet.id}: missing "## Preflight" section in ${file ? file.slice(root.length + 1) : 'the task file'}`);
        } else if (!/camera/i.test(body) || !/person/i.test(body)) {
            errors.push(`${bullet.id}: the "## Preflight" section must name the camera and the person`);
        }
        tagged.push(bullet.id);
    }
}

if (tagged.length && !errors.length) {
    console.log(`task-preflight: ok (${tagged.length} tasks require a live camera and a person: ${tagged.join(', ')})`);
}
for (const error of errors) console.error(`task-preflight: ${error}`);
if (errors.length) process.exit(1);
if (!tagged.length) console.log('task-preflight: ok (no live-camera tasks tagged)');
