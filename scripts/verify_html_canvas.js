#!/usr/bin/env node

const { spawnSync } = require('child_process');

if (process.argv.length < 3) {
  console.error('Usage: node verify_html_canvas.js <html-canvas-dump>');
  process.exit(2);
}

const binary = process.argv[2];

function runDump(args) {
  const result = spawnSync(binary, args, { encoding: 'utf8' });
  if (result.error) {
    console.error(`Failed to execute ${binary}: ${result.error.message}`);
    process.exit(1);
  }
  if (result.status !== 0) {
    if (result.stderr) {
      console.error(result.stderr.trim());
    }
    process.exit(result.status || 1);
  }
  const output = (result.stdout || '').trim();
  if (!output.length) {
    console.error(`Invocation '${binary} ${args.join(' ')}' produced no output`);
    process.exit(1);
  }
  try {
    return JSON.parse(output);
  } catch (err) {
    console.error(`Invocation '${binary} ${args.join(' ')}' produced invalid JSON: ${err.message}`);
    process.exit(1);
  }
}

const canvasDump = runDump([]);
if (canvasDump.preferDom !== false) {
  console.error('Expected preferDom=false for default canvas path');
  process.exit(1);
}
if (canvasDump.usedCanvasFallback !== true) {
  console.error('Canvas dump should exercise canvas fallback path (usedCanvasFallback=true)');
  process.exit(1);
}
if (!Array.isArray(canvasDump.canvas) || canvasDump.canvas.length === 0) {
  console.error('Canvas command array is empty');
  process.exit(1);
}

let sawRect = false;
let sawRounded = false;
canvasDump.canvas.forEach((cmd, index) => {
  if (typeof cmd !== 'object' || cmd === null) {
    console.error(`Canvas command ${index} is not an object`);
    process.exit(1);
  }
  ['type', 'x', 'y', 'width', 'height'].forEach((key) => {
    if (!(key in cmd)) {
      console.error(`Canvas command ${index} missing field '${key}'`);
      process.exit(1);
    }
  });
  if (typeof cmd.width !== 'number' || typeof cmd.height !== 'number') {
    console.error(`Canvas command ${index} width/height must be numeric`);
    process.exit(1);
  }
  if (cmd.type === 'rect') {
    sawRect = true;
  } else if (cmd.type === 'rounded_rect') {
    sawRounded = true;
    if (!Array.isArray(cmd.radii) || cmd.radii.length !== 4) {
      console.error(`Rounded rect command ${index} must carry four radii values`);
      process.exit(1);
    }
  }
});

if (!sawRect || !sawRounded) {
  console.error('Canvas commands should include both rect and rounded_rect entries');
  process.exit(1);
}

const domDump = runDump(['--prefer-dom']);
if (domDump.preferDom !== true) {
  console.error('Expected preferDom=true when running with --prefer-dom');
  process.exit(1);
}
if (domDump.usedCanvasFallback) {
  console.error('DOM path should not report canvas fallback when preferDom=true');
  process.exit(1);
}
const domContent = typeof domDump.dom === 'string' ? domDump.dom.trim() : '';
if (!domContent.length) {
  console.error('DOM output was empty when preferDom=true');
  process.exit(1);
}
if (!domContent.includes("<")) {
  console.error('DOM output does not look like HTML');
  process.exit(1);
}

console.log('HTML canvas/DOM outputs verified');
process.exit(0);
