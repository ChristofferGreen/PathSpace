#!/usr/bin/env node

const { spawnSync } = require('child_process');

if (process.argv.length < 3) {
  console.error('Usage: node verify_html_canvas.js <canvas-dump-binary>');
  process.exit(2);
}

const binary = process.argv[2];
const run = spawnSync(binary, [], { encoding: 'utf8' });

if (run.error) {
  console.error(`Failed to execute ${binary}: ${run.error.message}`);
  process.exit(1);
}

if (run.status !== 0) {
  console.error(`Canvas dump exited with status ${run.status}`);
  if (run.stderr) {
    console.error(run.stderr.trim());
  }
  process.exit(run.status || 1);
}

const output = run.stdout.trim();
if (!output.length) {
  console.error('Canvas dump produced no output');
  process.exit(1);
}

let commands;
try {
  commands = JSON.parse(output);
} catch (err) {
  console.error('Canvas commands were not valid JSON:', err.message);
  process.exit(1);
}

if (!Array.isArray(commands) || commands.length === 0) {
  console.error('Canvas command list is empty');
  process.exit(1);
}

let sawRect = false;
let sawRounded = false;
for (let i = 0; i < commands.length; ++i) {
  const cmd = commands[i];
  if (typeof cmd !== 'object' || cmd === null) {
    console.error(`Command ${i} is not an object`);
    process.exit(1);
  }
  const required = ['type', 'x', 'y', 'width', 'height'];
  for (const key of required) {
    if (!(key in cmd)) {
      console.error(`Command ${i} missing field '${key}'`);
      process.exit(1);
    }
  }
  if (typeof cmd.width !== 'number' || typeof cmd.height !== 'number') {
    console.error(`Command ${i} width/height must be numeric`);
    process.exit(1);
  }
  if (cmd.type === 'rect') {
    sawRect = true;
  } else if (cmd.type === 'rounded_rect') {
    sawRounded = true;
    if (!Array.isArray(cmd.radii) || cmd.radii.length !== 4) {
      console.error(`Rounded rect command ${i} must carry four radii entries`);
      process.exit(1);
    }
  }
}

if (!sawRect || !sawRounded) {
  console.error('Expected both rect and rounded_rect commands to be present');
  process.exit(1);
}

console.log('HTML canvas command stream verified');
process.exit(0);
