#!/usr/bin/env node
'use strict';

// No browser, packages, network, learned detector, or device measurements are used.
// All placement and visibility geometry comes from the companion interactive HTML.
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const crypto = require('node:crypto');

const DEFAULT_SEED = 20261006;
const ROADS = ['normal', 'urban', 'underpass', 'elevated'];
const ROAD_LABELS = {normal: '일반도로', urban: '평면 교차로', underpass: '지하차도', elevated: '고가도로'};
const CASES_PER_ROAD = 25;
const LANES = 6;
const UINT32_RANGE = 4294967296;

function loadModel(file = path.join(__dirname, 'simulator.html')) {
  // Normalize only line endings so Windows checkout settings do not change provenance.
  const html = fs.readFileSync(file, 'utf8').replace(/\r\n/g, '\n');
  const scripts = [...html.matchAll(/<script\b[^>]*>([\s\S]*?)<\/script>/gi)];
  if (scripts.length !== 1) throw new Error('Expected exactly one inline model script');
  const context = vm.createContext({ACE_HEADLESS: true});
  vm.runInContext(scripts[0][1], context, {filename: file, timeout: 10000});
  if (typeof context.ACE_MODEL?.runScenario !== 'function') throw new Error('Missing model entry point');
  return {model: context.ACE_MODEL,
    snapshotGeometry: () => vm.runInContext('JSON.parse(JSON.stringify({roads, structures, blocks}))', context),
    source_sha256_lf_utf8: crypto.createHash('sha256').update(html, 'utf8').digest('hex')};
}

const ratio = (numerator, denominator) => denominator ? numerator / denominator : null;
const mean = values => values.length ? values.reduce((sum, value) => sum + value, 0) / values.length : null;

function countsFor(scene) {
  const vehicles = scene.vehicles;
  const N = vehicles.length;
  const N1 = vehicles.filter(v => v.first).length;
  const N2 = vehicles.filter(v => v.final).length;
  const visible_independent = vehicles.filter(v => v.visible).length;
  const visible_but_first_rejected = vehicles.filter(v => v.visible && !v.first).length;
  if (N2 > N1 || N1 > N || N2 + visible_but_first_rejected !== visible_independent) {
    throw new Error('Invalid selection counts');
  }
  return {N, N1, N2, visible_independent, visible_but_first_rejected,
    first_reduction: ratio(N - N1, N),
    second_conditional_reduction: ratio(N1 - N2, N1),
    total_reduction: ratio(N - N2, N),
    independent_visible_retention: ratio(N2, visible_independent)};
}

function summarize(rows) {
  const totals = {N: 0, N1: 0, N2: 0, visible_independent: 0, visible_but_first_rejected: 0};
  for (const row of rows) for (const key of Object.keys(totals)) totals[key] += row[key];
  const {N, N1, N2, visible_independent} = totals;
  return {cases: rows.length, ...totals,
    first_reduction: ratio(N - N1, N),
    second_conditional_reduction: ratio(N1 - N2, N1),
    total_reduction: ratio(N - N2, N),
    independent_visible_retention: ratio(N2, visible_independent)};
}

function percentiles(values) {
  const sorted = values.filter(value => value !== null).sort((a, b) => a - b);
  if (!sorted.length) return {count: 0, min: null, p10: null, p25: null, median: null, p75: null, p90: null, max: null, mean: null};
  const at = p => {
    const index = (sorted.length - 1) * p;
    const low = Math.floor(index), high = Math.ceil(index);
    return sorted[low] + (sorted[high] - sorted[low]) * (index - low);
  };
  return {count: sorted.length, min: sorted[0], p10: at(.1), p25: at(.25), median: at(.5),
    p75: at(.75), p90: at(.9), max: sorted.at(-1), mean: mean(sorted)};
}

function casePercentiles(rows) {
  const fields = ['N', 'N1', 'N2', 'visible_independent', 'visible_but_first_rejected',
    'first_reduction', 'second_conditional_reduction', 'total_reduction', 'independent_visible_retention'];
  return Object.fromEntries(fields.map(key => [key, percentiles(rows.map(row => row[key]))]));
}

function buildBatch(masterSeed = DEFAULT_SEED, loaded = loadModel()) {
  if (!Number.isInteger(masterSeed) || masterSeed < 0 || masterSeed >= UINT32_RANGE) {
    throw new Error('master seed must be an unsigned 32-bit integer');
  }
  const {model, source_sha256_lf_utf8} = loaded;
  const master = model.random(masterSeed);
  const order = ROADS.flatMap(road => Array(CASES_PER_ROAD).fill(road));
  for (let i = order.length - 1; i > 0; --i) {
    const j = Math.floor(master() * (i + 1));
    [order[i], order[j]] = [order[j], order[i]];
  }
  const usedSeeds = new Set(), scenarios = [], vehicles = [];
  for (const [index, road] of order.entries()) {
    let scenario_seed;
    do scenario_seed = Math.floor(master() * UINT32_RANGE); while (usedSeeds.has(scenario_seed));
    usedSeeds.add(scenario_seed);
    const rng = model.random(scenario_seed);
    const count = 20 + 5 * Math.floor(rng() * 17);
    const layout_seed = Math.floor(rng() * UINT32_RANGE);
    const scenario_id = `case-${String(index + 1).padStart(3, '0')}`;
    let scene;
    try {
      scene = model.runScenario({road, lanes: LANES, count, seed: layout_seed});
    } catch (error) {
      // Do not silently replace failed placements and bias the intended sample.
      throw new Error(`${scenario_id} (${road}, count=${count}, seed=${layout_seed}): ${error.message}`);
    }
    const finalFractions = scene.vehicles.filter(v => v.final).map(v => v.visibleFraction);
    scenarios.push({scenario_id, road, lanes: LANES, scenario_seed, layout_seed,
      accident_x: scene.accident.x, accident_y: scene.accident.y, accident_z: scene.accident.z,
      ...countsFor(scene), final_visible_fraction_mean: mean(finalFractions),
      final_visible_fraction_min: finalFractions.length ? Math.min(...finalFractions) : null});
    for (const v of scene.vehicles) vehicles.push({scenario_id, vehicle_id: v.id,
      x: v.x, y: v.y, z: v.z || 0, heading_radians: v.h, truck: Number(v.truck),
      first: Number(v.first), visible_independent: Number(v.visible), final: Number(v.final),
      visible_but_first_rejected: Number(v.visible && !v.first),
      total_body_samples: v.sampleCount, in_view_samples: v.inViewCount, visible_samples: v.visibleCount,
      visible_fraction: v.visibleFraction, visible_body_fraction: v.visibleBodyFraction});
  }
  const by_road = Object.fromEntries(ROADS.map(road => {
    const rows = scenarios.filter(row => row.road === road);
    return [road, {label: ROAD_LABELS[road], ...summarize(rows), case_percentiles: casePercentiles(rows)}];
  }));
  const summary = {
    schema_version: 1,
    title: '100개 합성 도로 배치의 기하 가시성 기반 선별 결과',
    model_type: 'geometric_visibility',
    source: {file: 'simulator.html', sha256_lf_utf8: source_sha256_lf_utf8,
      encoding: 'UTF-8 with CRLF normalized to LF before SHA-256'},
    design: {
      master_seed: masterSeed, cases: scenarios.length, cases_per_road: CASES_PER_ROAD,
      road_order: 'balanced four-type sample, Fisher-Yates shuffled by the master PRNG',
      rng: 'simulator.html random(seed), unsigned 32-bit seeded generator',
      seed_draws: 'after road-order shuffle: unique scenario seed; its first draw selects count and second draw selects layout seed',
      vehicle_count_values: Array.from({length: 17}, (_, i) => 20 + 5 * i),
      vehicle_count_sampling: 'discrete uniform, independently for each case',
      lanes: LANES,
      accident: {x: 500, y: 340, heading_degrees: -18, elevation_by_road: {normal: 0, urban: 0, underpass: -50, elevated: 80}},
      geometry: 'road shape and accident XY are fixed for each road type; observer count and non-overlapping placement vary',
      coordinates: 'original simulator scene units; geometric scale is not calibrated from a road survey',
      camera_horizontal_fov_degrees: model.cameraFov,
      first_stage: 'accident body bounding-circle angular span intersects inclusive +/-32-degree horizontal camera FOV; distance <= radius always passes',
      accident_body_bounding_circle: {length: 30, width: 16, radius: model.accidentBodyRadius, units: 'scene units'},
      second_stage: 'one or more accident body sample points inside horizontal FOV and unoccluded',
      body_samples: {flat: 117, grade_separated: 351},
      occlusion: '2D oriented boxes for flat roads; 3D volumes for underpass/elevated roads',
      visible_fraction_denominator: 'body sample points inside the observer horizontal FOV; zero if none',
      visible_body_fraction_denominator: 'all 117 or 351 body sample points',
      independent_visibility: 'same point visibility test without the stage-1 body-circle angular-overlap gate',
      real_ai_executed: false,
      resource_measurement_performed: false
    },
    aggregation: {ratios: 'ratios of summed vehicle counts, not unweighted averages of scenario percentages',
      percentiles: 'per-case unweighted distribution, linear interpolation at (n-1)*p; null denominators excluded',
      null_ratio: 'denominator equals zero',
      interpretation: 'generated-scenario distribution, not an estimate of the frequency of real traffic situations'},
    overall: summarize(scenarios), by_road, case_percentiles: casePercentiles(scenarios),
    outputs: {scenarios: 'scenarios.csv', vehicles: 'vehicles.csv'},
    baseline_counts: {
      all_upload: 'N uploads, no implied AI execution',
      first_only_upload: 'N1 uploads after body-circle angular-overlap gate',
      two_stage: 'N1 visibility checks, N2 uploads under the geometric proxy',
      all_visibility: 'N visibility checks, visible_independent uploads under the geometric proxy',
      first_gate_loss: 'visible_but_first_rejected counts geometrically visible accident bodies excluded by the body-circle angular-overlap gate'
    }
  };
  return {summary, scenarios, vehicles};
}

function serializeCsv(rows) {
  if (!rows.length) return '';
  const keys = Object.keys(rows[0]);
  const cell = value => {
    if (value === null || value === undefined) return '';
    const text = String(value);
    return /[",\r\n]/.test(text) ? `"${text.replaceAll('"', '""')}"` : text;
  };
  return [keys.join(','), ...rows.map(row => keys.map(key => cell(row[key])).join(','))].join('\n') + '\n';
}

function artifactContents(batch) {
  return {'scenarios.csv': serializeCsv(batch.scenarios), 'vehicles.csv': serializeCsv(batch.vehicles),
    'summary.json': JSON.stringify(batch.summary, null, 2) + '\n'};
}

function main(argv) {
  let seed = DEFAULT_SEED, out = __dirname, check = false;
  for (let i = 0; i < argv.length; ++i) {
    const arg = argv[i];
    if (arg === '--seed' && argv[i + 1] !== undefined) seed = Number(argv[++i]);
    else if (arg === '--out' && argv[i + 1] !== undefined) out = path.resolve(argv[++i]);
    else if (arg === '--check') check = true;
    else if (arg === '--help') {
      console.log('Usage: node run_batch.cjs [--seed UINT32] [--out DIRECTORY] [--check]\nDefault: seed 20261006, 100 cases, 6 lanes. --check compares saved artifacts without writing.');
      return;
    } else throw new Error(`Unknown or incomplete argument: ${arg}`);
  }
  const batch = buildBatch(seed);
  const artifacts = artifactContents(batch);
  if (!check) fs.mkdirSync(out, {recursive: true});
  for (const [name, content] of Object.entries(artifacts)) {
    const file = path.join(out, name);
    if (check) {
      if (fs.readFileSync(file, 'utf8').replace(/\r\n/g, '\n') !== content) throw new Error(`Saved artifact differs: ${name}`);
    } else fs.writeFileSync(file, content, 'utf8');
  }
  console.log(JSON.stringify({mode: check ? 'verified' : 'generated', master_seed: seed, ...batch.summary.overall}, null, 2));
}

module.exports = {loadModel, countsFor, summarize, percentiles, buildBatch, artifactContents};
if (require.main === module) {
  try { main(process.argv.slice(2)); }
  catch (error) { console.error(error.message); process.exitCode = 1; }
}
