#!/usr/bin/env node
'use strict';

// Export model data only; this never starts a browser or renders HTML.
const fs = require('node:fs');
const path = require('node:path');
const {loadModel, countsFor} = require('./run_batch.cjs');

function buildExample() {
  const loaded = loadModel();
  const scene = loaded.model.runScenario({road: 'elevated', lanes: 8, count: 80, seed: 42});
  const geometry = loaded.snapshotGeometry();
  const fields = ['id', 'x', 'y', 'z', 'h', 'pitch', 'truck', 'road', 'first', 'visible', 'final'];
  return {schema_version: 1, model_type: 'geometric_visibility',
    source: {file: 'simulator.html', sha256_lf_utf8: loaded.source_sha256_lf_utf8},
    design: {road: scene.road, lanes: scene.lanes, count: scene.vehicles.length, seed: scene.seed,
      units: 'scene units', camera_horizontal_fov_degrees: loaded.model.cameraFov,
      accident_body_radius: loaded.model.accidentBodyRadius},
    counts: countsFor(scene), accident: scene.accident,
    vehicles: scene.vehicles.map(v => Object.fromEntries(fields.map(k => [k, v[k]]))),
    roads: geometry.roads, structures: geometry.structures,
    buildings: geometry.blocks.filter(block => block.decor)};
}

function main(args) {
  if (args.some(arg => !['--check', '--stdout'].includes(arg))) {
    throw new Error('Usage: node export_example.cjs [--check | --stdout]');
  }
  const data = buildExample();
  const content = JSON.stringify(data, null, 2) + '\n';
  const target = path.join(__dirname, 'example.json');
  if (args.includes('--stdout')) process.stdout.write(content);
  else if (args.includes('--check')) {
    if (fs.readFileSync(target, 'utf8').replace(/\r\n/g, '\n') !== content) throw new Error('Saved example.json differs');
    console.log(JSON.stringify({mode: 'verified', ...data.counts}));
  } else {
    fs.writeFileSync(target, content, 'utf8');
    console.log(JSON.stringify({mode: 'generated', file: target, ...data.counts}));
  }
}

module.exports = {buildExample};
if (require.main === module) {
  try { main(process.argv.slice(2)); }
  catch (error) { console.error(error.message); process.exitCode = 1; }
}
