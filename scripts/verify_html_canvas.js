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

function parseCssColor(value) {
  const match = /^rgba\((\d+),(\d+),(\d+),([\d.]+)\)$/i.exec(value.trim());
  if (!match) {
    throw new Error(`Unsupported color format '${value}'`);
  }
  return {
    r: Number(match[1]),
    g: Number(match[2]),
    b: Number(match[3]),
    a: Number(match[4]),
  };
}

function toChannels(color) {
  return {
    r: Math.round(color[0] * 255),
    g: Math.round(color[1] * 255),
    b: Math.round(color[2] * 255),
    a: Number(color[3].toFixed(3)),
  };
}

function assertChannels(actual, expected, label) {
  if (actual.r !== expected.r || actual.g !== expected.g || actual.b !== expected.b) {
    throw new Error(`${label}: expected rgb(${expected.r},${expected.g},${expected.b}) got rgb(${actual.r},${actual.g},${actual.b})`);
  }
  if (Math.abs(actual.a - expected.a) > 0.01) {
    throw new Error(`${label}: expected alpha ${expected.a} got ${actual.a}`);
  }
}

function rgbaString(channels) {
  return `rgba(${channels.r},${channels.g},${channels.b},${channels.a})`;
}

const defaultTheme = {
  buttonBackground: [0.176, 0.353, 0.914, 1.0],
  toggleTrack: [0.176, 0.353, 0.914, 1.0],
  toggleThumb: [1.0, 1.0, 1.0, 1.0],
  sliderTrack: [0.75, 0.75, 0.78, 1.0],
  sliderFill: [0.176, 0.353, 0.914, 1.0],
  sliderThumb: [1.0, 1.0, 1.0, 1.0],
  listBackground: [0.121, 0.129, 0.145, 1.0],
  listHover: [0.247, 0.278, 0.349, 1.0],
  listSelected: [0.176, 0.353, 0.914, 1.0],
  listItem: [0.176, 0.184, 0.204, 1.0],
  listSeparator: [0.224, 0.231, 0.247, 1.0],
};

const sunsetTheme = {
  buttonBackground: [0.882, 0.424, 0.310, 1.0],
  toggleTrack: [0.882, 0.424, 0.310, 1.0],
  toggleThumb: [0.996, 0.949, 0.902, 1.0],
  sliderTrack: [0.75, 0.75, 0.78, 1.0],
  sliderFill: [0.882, 0.424, 0.310, 1.0],
  sliderThumb: [0.996, 0.949, 0.902, 1.0],
  listBackground: [0.215, 0.128, 0.102, 1.0],
  listHover: [0.422, 0.248, 0.198, 1.0],
  listSelected: [0.882, 0.424, 0.310, 1.0],
  listItem: [0.266, 0.166, 0.138, 1.0],
  listSeparator: [0.365, 0.231, 0.201, 1.0],
};

function verifyWidgetCanvas(canvasCommands, theme, label) {
  const expectedSequence = [
    { index: 0, type: 'rounded_rect', color: theme.buttonBackground, desc: 'button background' },
    { index: 1, type: 'rounded_rect', color: theme.toggleTrack, desc: 'toggle track' },
    { index: 2, type: 'rounded_rect', color: theme.toggleThumb, desc: 'toggle thumb' },
    { index: 3, type: 'rect', color: theme.sliderTrack, desc: 'slider track' },
    { index: 4, type: 'rect', color: theme.sliderFill, desc: 'slider fill' },
    { index: 5, type: 'rounded_rect', color: theme.sliderThumb, desc: 'slider thumb' },
    { index: 6, type: 'rounded_rect', color: theme.listBackground, desc: 'list background' },
  ];
  expectedSequence.forEach(({ index, type, color, desc }) => {
    const cmd = canvasCommands[index];
    if (!cmd || cmd.type !== type) {
      throw new Error(`${label}: expected ${type} at index ${index}`);
    }
    const actual = parseCssColor(cmd.color);
    const expected = toChannels(color);
    assertChannels(actual, expected, `${label} ${desc}`);
  });

  const listItems = [
    { index: 7, color: theme.listHover, desc: 'list hover item' },
    { index: 9, color: theme.listSelected, desc: 'list selected item' },
    { index: 11, color: theme.listItem, desc: 'list default item' },
  ];
  listItems.forEach(({ index, color, desc }) => {
    const cmd = canvasCommands[index];
    if (!cmd || cmd.type !== 'rect') {
      throw new Error(`${label}: expected rect at index ${index}`);
    }
    const actual = parseCssColor(cmd.color);
    const expected = toChannels(color);
    assertChannels(actual, expected, `${label} ${desc}`);
  });

  const separators = [8, 10];
  separators.forEach((index) => {
    const cmd = canvasCommands[index];
    if (!cmd || cmd.type !== 'rect') {
      throw new Error(`${label}: expected separator rect at index ${index}`);
    }
    const actual = parseCssColor(cmd.color);
    const expected = toChannels(theme.listSeparator);
    assertChannels(actual, expected, `${label} list separator`);
  });
}

function verifyWidgetDom(dom, theme, label) {
  const requiredColors = [
    theme.buttonBackground,
    theme.toggleTrack,
    theme.toggleThumb,
    theme.sliderTrack,
    theme.sliderFill,
    theme.sliderThumb,
    theme.listBackground,
    theme.listHover,
    theme.listSelected,
    theme.listItem,
    theme.listSeparator,
  ].map((color) => rgbaString(toChannels(color)));

  for (const cssColor of requiredColors) {
    if (!dom.includes(cssColor)) {
      throw new Error(`${label}: DOM output missing color ${cssColor}`);
    }
  }
}

function verifyBasicCanvas(canvasCommands) {
  if (!Array.isArray(canvasCommands) || canvasCommands.length === 0) {
    throw new Error('Canvas command array is empty');
  }
  let sawRect = false;
  let sawRounded = false;
  canvasCommands.forEach((cmd, index) => {
    if (typeof cmd !== 'object' || cmd === null) {
      throw new Error(`Canvas command ${index} is not an object`);
    }
    ['type', 'x', 'y', 'width', 'height'].forEach((key) => {
      if (!(key in cmd)) {
        throw new Error(`Canvas command ${index} missing field '${key}'`);
      }
    });
    if (typeof cmd.width !== 'number' || typeof cmd.height !== 'number') {
      throw new Error(`Canvas command ${index} width/height must be numeric`);
    }
    if (cmd.type === 'rect') {
      sawRect = true;
    } else if (cmd.type === 'rounded_rect') {
      sawRounded = true;
      if (!Array.isArray(cmd.radii) || cmd.radii.length !== 4) {
        throw new Error(`Rounded rect command ${index} must carry four radii values`);
      }
    }
  });
  if (!sawRect || !sawRounded) {
    throw new Error('Canvas commands should include both rect and rounded_rect entries');
  }
}

function verifyScenario({ name, args, theme }) {
  const canvasDump = runDump(args);
  if (canvasDump.scenario !== name) {
    throw new Error(`Scenario '${name}' reported as '${canvasDump.scenario}'`);
  }
  if (canvasDump.preferDom !== false) {
    throw new Error(`Expected preferDom=false for scenario '${name}'`);
  }
  if (canvasDump.usedCanvasFallback !== true) {
    throw new Error(`Scenario '${name}' should exercise canvas fallback (usedCanvasFallback=true)`);
  }
  if (!Array.isArray(canvasDump.canvas)) {
    throw new Error(`Scenario '${name}' returned invalid canvas data`);
  }
  if (!canvasDump.baselineDigest || !canvasDump.replayDigest) {
    throw new Error(`Scenario '${name}' missing render digests`);
  }
  if (canvasDump.baselineDigest !== canvasDump.replayDigest) {
    throw new Error(`Scenario '${name}' render digests differ`);
  }

  if (name === 'basic') {
    verifyBasicCanvas(canvasDump.canvas);
  } else if (theme) {
    verifyWidgetCanvas(canvasDump.canvas, theme, name);
  }

  const domArgs = [...args, '--prefer-dom'];
  const domDump = runDump(domArgs);
  if (domDump.scenario !== name) {
    throw new Error(`Scenario '${name}' DOM pass reported as '${domDump.scenario}'`);
  }
  if (domDump.preferDom !== true) {
    throw new Error(`Scenario '${name}' prefer-dom run should set preferDom=true`);
  }
  if (domDump.usedCanvasFallback) {
    throw new Error(`Scenario '${name}' DOM path should not report canvas fallback`);
  }
  const domContent = typeof domDump.dom === 'string' ? domDump.dom.trim() : '';
  if (!domContent.length || !domContent.includes('<')) {
    throw new Error(`Scenario '${name}' produced invalid DOM output`);
  }
  if (theme) {
    verifyWidgetDom(domContent, theme, `${name} dom`);
  }
}

const scenarios = [
  { name: 'basic', args: [] },
  { name: 'widgets-default', args: ['--scenario', 'widgets-default'], theme: defaultTheme },
  { name: 'widgets-sunset', args: ['--scenario', 'widgets-sunset'], theme: sunsetTheme },
];

try {
  scenarios.forEach(verifyScenario);
} catch (err) {
  console.error(err.message);
  process.exit(1);
}

console.log('HTML canvas/DOM outputs verified');
process.exit(0);
