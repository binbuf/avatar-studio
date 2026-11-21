#!/usr/bin/env node
// Verify the shipped model bytes and the actual PE export table, not source text.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import assert from 'node:assert/strict';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const expected = ['avatar_tracker_configure', 'avatar_tracker_create', 'avatar_tracker_destroy',
  'avatar_tracker_poll_result', 'avatar_tracker_push_frame'];

function exportsOf(bytes) {
  if (bytes.toString('ascii', 0, 2) !== 'MZ') throw Error('not a PE image');
  const pe = bytes.readUInt32LE(60);
  if (bytes.readUInt32LE(pe) !== 0x4550) throw Error('bad PE signature');
  const sections = bytes.readUInt16LE(pe + 6);
  const optionalSize = bytes.readUInt16LE(pe + 20);
  const optional = pe + 24;
  const magic = bytes.readUInt16LE(optional);
  if (![0x20b, 0x10b].includes(magic)) throw Error('unknown PE optional header');
  const directory = optional + (magic === 0x20b ? 112 : 96);
  const rvaToOffset = (rva) => {
    for (let i = 0; i < sections; ++i) {
      const section = optional + optionalSize + i * 40;
      const address = bytes.readUInt32LE(section + 12);
      const rawSize = bytes.readUInt32LE(section + 16);
      if (rva >= address && rva - address < rawSize) {
        const offset = bytes.readUInt32LE(section + 20) + rva - address;
        if (offset >= bytes.length) throw Error('PE RVA outside file');
        return offset;
      }
    }
    throw Error('unmapped PE RVA');
  };
  const table = rvaToOffset(bytes.readUInt32LE(directory));
  const count = bytes.readUInt32LE(table + 20);
  const namesCount = bytes.readUInt32LE(table + 24);
  if (count !== 5 || namesCount !== 5) throw Error(`tracker exports ${count} functions/${namesCount} names; expected five`);
  const namesTable = rvaToOffset(bytes.readUInt32LE(table + 32));
  const names = [];
  for (let i = 0; i < namesCount; ++i) {
    const offset = rvaToOffset(bytes.readUInt32LE(namesTable + i * 4));
    const end = bytes.indexOf(0, offset);
    if (end < offset || end - offset > 128) throw Error('invalid PE export name');
    names.push(bytes.toString('ascii', offset, end));
  }
  assert.deepEqual(names.sort(), expected, 'unexpected vendor export or missing ABI function');
  return names;
}

function fixture(names = expected) {
  const b = Buffer.alloc(2048);
  b.write('MZ'); b.writeUInt32LE(128, 60); b.writeUInt32LE(0x4550, 128);
  b.writeUInt16LE(1, 134); b.writeUInt16LE(240, 148); b.writeUInt16LE(0x20b, 152);
  b.writeUInt32LE(0x1000, 264);
  const section = 392;
  b.writeUInt32LE(0x1000, section + 12); b.writeUInt32LE(1536, section + 16);
  b.writeUInt32LE(512, section + 20);
  b.writeUInt32LE(names.length, 532); b.writeUInt32LE(names.length, 536);
  b.writeUInt32LE(0x1040, 544);
  names.forEach((name, i) => {
    b.writeUInt32LE(0x1100 + i * 128, 576 + i * 4);
    b.write(name, 768 + i * 128);
  });
  return b;
}

try {
  const args = process.argv.slice(2);
  let sdk = path.join(root, '.tools/tracker-sdk');
  for (let i = 0; i < args.length; ++i) {
    if (args[i] === '--sdk') sdk = path.resolve(args[++i]);
    else if (args[i] === '--self-test') {
      assert.deepEqual(exportsOf(fixture()), expected);
      assert.throws(() => exportsOf(fixture([...expected, 'vendor_symbol'])));
      assert.throws(() => exportsOf(fixture([...expected.slice(0, 4), 'vendor_symbol'])));
      assert.throws(() => exportsOf(Buffer.alloc(64)));
    } else throw Error(`unknown argument ${args[i]}`);
  }
  const manifest = JSON.parse(fs.readFileSync(path.join(root, 'native/licenses.json'), 'utf8'));
  const model = manifest.components.find((component) => component.id === 'face_landmarker_bundle');
  assert.equal(model.license, 'Apache-2.0');
  assert.equal(model.status, 'active');
  assert.equal(model.shipped, true);
  const actual = crypto.createHash('sha256').update(fs.readFileSync(path.join(sdk, model.model_file))).digest('hex');
  assert.equal(actual, model.sha256.toLowerCase(), 'tracker model SHA256 mismatch');
  const names = exportsOf(fs.readFileSync(path.join(sdk, 'libavatartracker.dll')));
  console.log(`tracker SDK: PASS (model SHA256; exactly ${names.length} C exports)`);
} catch (error) {
  console.error(`tracker SDK: FAIL - ${error.message}`);
  process.exitCode = 1;
}
