#!/usr/bin/env node

const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawnSync } = require('child_process');

if (process.argv.length < 3) {
  console.error('Usage: node verify_hsat_assets.js <hsat-inspect-binary>');
  process.exit(2);
}

const binary = process.argv[2];

const HSAT_MAGIC = 0x48534154;
const HSAT_VERSION = 1;

const assets = [
  {
    logical: 'images/example.png',
    mime: 'image/png',
    bytes: [0x00, 0x11, 0x22, 0x00, 0xff, 0x80]
  },
  {
    logical: 'fonts/display.woff2',
    mime: 'font/woff2',
    bytes: [0x01, 0x03, 0x03, 0x07]
  },
  {
    logical: 'images/reference.png',
    mime: 'application/vnd.pathspace.image+ref',
    bytes: []
  }
];

function encodeHsatPayload(items) {
  const header = Buffer.alloc(10);
  header.writeUInt32LE(HSAT_MAGIC, 0);
  header.writeUInt16LE(HSAT_VERSION, 4);
  header.writeUInt32LE(items.length, 6);

  const chunks = [header];
  for (const item of items) {
    const logical = Buffer.from(item.logical, 'utf8');
    const mime = Buffer.from(item.mime, 'utf8');
    const bytes = Buffer.from(item.bytes);
    const lengths = Buffer.alloc(12);
    lengths.writeUInt32LE(logical.length, 0);
    lengths.writeUInt32LE(mime.length, 4);
    lengths.writeUInt32LE(bytes.length, 8);
    chunks.push(lengths, logical, mime, bytes);
  }
  return Buffer.concat(chunks);
}

const payload = encodeHsatPayload(assets);

const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), 'hsat-'));
const payloadPath = path.join(tempDir, 'payload.hsat');
fs.writeFileSync(payloadPath, payload);

function runInspect(args, inputBuffer) {
  const options = {
    encoding: 'utf8',
    maxBuffer: 10 * 1024 * 1024
  };
  if (inputBuffer) {
    options.input = inputBuffer;
  }
  const result = spawnSync(binary, args, options);
  if (result.error) {
    console.error(`Failed to execute ${binary}: ${result.error.message}`);
    process.exit(1);
  }
  if (result.status !== 0) {
    if (result.stderr && result.stderr.length) {
      console.error(result.stderr.trim());
    } else {
      console.error(`${binary} exited with status ${result.status}`);
    }
    process.exit(result.status || 1);
  }
  const stdout = (result.stdout || '').trim();
  if (!stdout.length) {
    console.error(`Invocation '${binary} ${args.join(' ')}' produced no output`);
    process.exit(1);
  }
  try {
    return JSON.parse(stdout);
  } catch (err) {
    console.error(`Invocation '${binary} ${args.join(' ')}' produced invalid JSON: ${err.message}`);
    process.exit(1);
  }
}

function validateSummary(summary) {
  const expectedBytes = assets.reduce((acc, item) => acc + item.bytes.length, 0);
  const expectedEmpty = assets.filter((item) => item.bytes.length === 0).length;
  if (summary.assetCount !== assets.length) {
    console.error(`Expected assetCount=${assets.length}, got ${summary.assetCount}`);
    process.exit(1);
  }
  if (summary.totalBytes !== expectedBytes) {
    console.error(`Expected totalBytes=${expectedBytes}, got ${summary.totalBytes}`);
    process.exit(1);
  }
  if (typeof summary.bytesConsumed !== 'number' || summary.bytesConsumed !== payload.length) {
    console.error(`bytesConsumed should equal payload size (${payload.length})`);
    process.exit(1);
  }
  if (summary.trailingBytes !== 0) {
    console.error(`trailingBytes should be 0, got ${summary.trailingBytes}`);
    process.exit(1);
  }
  if (!Array.isArray(summary.assets) || summary.assets.length !== assets.length) {
    console.error('assets array length mismatch');
    process.exit(1);
  }
  if (summary.emptyAssetCount !== expectedEmpty) {
    console.error(`Expected emptyAssetCount=${expectedEmpty}, got ${summary.emptyAssetCount}`);
    process.exit(1);
  }
  if (!Array.isArray(summary.duplicateLogicalPaths)) {
    console.error('duplicateLogicalPaths missing or not an array');
    process.exit(1);
  }
  if (summary.duplicateLogicalPaths.length !== 0) {
    console.error(`Expected duplicateLogicalPaths to be empty, got ${summary.duplicateLogicalPaths.length} entries`);
    process.exit(1);
  }

  const classifyKind = (mime, logical) => {
    if (mime === 'application/vnd.pathspace.image+ref') {
      return 'image-reference';
    }
    if (mime === 'application/vnd.pathspace.font+ref') {
      return 'font-reference';
    }
    if (mime.startsWith('image/')) {
      return 'image';
    }
    if (mime.startsWith('font/') || mime.startsWith('application/font')) {
      return 'font';
    }
    if (mime.startsWith('text/')) {
      return 'text';
    }
    if (mime.startsWith('application/json')) {
      return 'json';
    }
    if (logical.startsWith('images/')) {
      return 'image';
    }
    if (logical.startsWith('fonts/')) {
      return 'font';
    }
    return 'binary';
  };

  const expectSummaryArray = (fieldName, keyField, entries, expectedMap) => {
    if (!Array.isArray(entries)) {
      console.error(`${fieldName} missing or not an array`);
      process.exit(1);
    }
    if (entries.length !== Object.keys(expectedMap).length) {
      console.error(`${fieldName} length mismatch (expected ${Object.keys(expectedMap).length}, got ${entries.length})`);
      process.exit(1);
    }
    const seen = new Set();
    for (const entry of entries) {
      if (typeof entry !== 'object' || entry === null) {
        console.error(`${fieldName} entries must be objects`);
        process.exit(1);
      }
      if (!(keyField in entry)) {
        console.error(`${fieldName} entry missing ${keyField}`);
        process.exit(1);
      }
      const key = entry[keyField];
      if (!Object.prototype.hasOwnProperty.call(expectedMap, key)) {
        console.error(`${fieldName} unexpected key '${key}'`);
        process.exit(1);
      }
      if (seen.has(key)) {
        console.error(`${fieldName} duplicate key '${key}'`);
        process.exit(1);
      }
      seen.add(key);
      const expected = expectedMap[key];
      if (entry.count !== expected.count) {
        console.error(`${fieldName} count mismatch for '${key}'`);
        process.exit(1);
      }
      if (entry.totalBytes !== expected.totalBytes) {
        console.error(`${fieldName} totalBytes mismatch for '${key}'`);
        process.exit(1);
      }
    }
  };

  const expectedKindSummary = {};
  assets.forEach((asset) => {
    const kind = classifyKind(asset.mime, asset.logical);
    if (!expectedKindSummary[kind]) {
      expectedKindSummary[kind] = { count: 0, totalBytes: 0 };
    }
    expectedKindSummary[kind].count += 1;
    expectedKindSummary[kind].totalBytes += asset.bytes.length;
  });
  const expectedMimeSummary = {};
  assets.forEach((asset) => {
    if (!expectedMimeSummary[asset.mime]) {
      expectedMimeSummary[asset.mime] = { count: 0, totalBytes: 0 };
    }
    expectedMimeSummary[asset.mime].count += 1;
    expectedMimeSummary[asset.mime].totalBytes += asset.bytes.length;
  });

  expectSummaryArray('kindSummary', 'kind', summary.kindSummary, expectedKindSummary);
  expectSummaryArray('mimeSummary', 'mimeType', summary.mimeSummary, expectedMimeSummary);

  summary.assets.forEach((asset, index) => {
    const expected = assets[index];
    if (asset.logicalPath !== expected.logical) {
      console.error(`Asset ${index} logicalPath mismatch`);
      process.exit(1);
    }
    if (asset.mimeType !== expected.mime) {
      console.error(`Asset ${index} mimeType mismatch`);
      process.exit(1);
    }
    if (asset.byteLength !== expected.bytes.length) {
      console.error(`Asset ${index} byteLength mismatch`);
      process.exit(1);
    }
    const expectingReference = expected.mime === 'application/vnd.pathspace.image+ref';
    if (!!asset.reference !== expectingReference) {
      console.error(`Asset ${index} reference flag mismatch`);
      process.exit(1);
    }
    let expectedKind = 'binary';
    if (expectingReference) {
      expectedKind = 'image-reference';
    } else if (expected.mime.startsWith('image/')) {
      expectedKind = 'image';
    } else if (expected.mime.startsWith('font/')) {
      expectedKind = 'font';
    }
    if (asset.kind !== expectedKind) {
      console.error(`Asset ${index} kind mismatch (expected ${expectedKind}, got ${asset.kind})`);
      process.exit(1);
    }
    if (expected.bytes.length) {
      if (!asset.bytePreviewHex) {
        console.error(`Asset ${index} should include preview hex`);
        process.exit(1);
      }
    } else if (asset.bytePreviewHex) {
      console.error(`Asset ${index} should not include preview hex`);
      process.exit(1);
    }
  });
}

const summaryDefault = runInspect(['--input', payloadPath]);
validateSummary(summaryDefault);

const summaryPretty = runInspect(['--input', payloadPath, '--pretty']);
validateSummary(summaryPretty);

const summaryStdin = runInspect(['--stdin'], payload);
validateSummary(summaryStdin);

console.log('HSAT inspection verified');
process.exit(0);
