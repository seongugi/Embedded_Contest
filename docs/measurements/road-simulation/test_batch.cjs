'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const {loadModel, countsFor, summarize, buildBatch, artifactContents} = require('./run_batch.cjs');
const loaded = loadModel();
const model = loaded.model;
const center = {x: 500, y: 340};
const observer = {id: 1, x: 400, y: 340, h: 0};
const evaluate = (v, points = [center], obstacles = [], grade = false) =>
  model.evaluateObserver(v, points, obstacles, grade, 0);

test('point visibility keeps the camera horizontal field of view at inclusive +/-32 degrees', () => {
  for (const sign of [-1, 1]) {
    assert.equal(evaluate({...observer, h: sign * 32 * Math.PI / 180}).visible, true);
    assert.equal(evaluate({...observer, h: sign * 32.0001 * Math.PI / 180}).visible, false);
  }
  assert.equal(evaluate({...observer, h: 2 * Math.PI}).visible, true);
  assert.equal(evaluate({...observer, h: Math.PI}).visible, false);
});

test('body-circle candidate includes tangency on both FOV edges and excludes just beyond it', () => {
  const halfSpan = Math.asin(17 / 100) * 180 / Math.PI;
  assert.equal(model.accidentBodyRadius, 17);
  for (const sign of [-1, 1]) {
    const tangent = sign * (32 + halfSpan) * Math.PI / 180;
    assert.equal(evaluate({...observer, h: tangent}).first, true);
    assert.equal(evaluate({...observer, h: tangent + sign * 1e-6}).first, false);
    assert.equal(evaluate({...observer, h: tangent - sign * 1e-6}).first, true);
  }
});

test('observers on or inside the body circle pass at every heading, including coincident centers', () => {
  for (const distance of [0, 1, 16.999, 17]) {
    for (const h of [0, Math.PI / 2, Math.PI, -Math.PI, 2 * Math.PI]) {
      assert.equal(evaluate({...observer, x: center.x - distance, h}).first, true);
    }
  }
  assert.equal(evaluate({...observer, x: center.x - 17.0001, h: Math.PI}).first, false);
});

test('body-circle candidate rejects vehicles looking away and wraps headings across +/-180 degrees', () => {
  assert.equal(evaluate({...observer, h: Math.PI}).first, false);
  assert.equal(evaluate({...observer, h: -Math.PI}).first, false);
  assert.equal(evaluate({...observer, h: 2 * Math.PI}).first, true);
  const opposite = {...observer, x: 600};
  assert.equal(evaluate({...opposite, h: 0}).first, false);
  assert.equal(evaluate({...opposite, h: Math.PI - .01}).first, true);
  assert.equal(evaluate({...opposite, h: -Math.PI + .01}).first, true);
});

test('occlusion can fully hide or partly reveal the body; observer does not block itself', () => {
  const points = [center, {x: 500, y: 330}, {x: 500, y: 350}];
  const wide = {x: 450, y: 340, w: 10, h: 30, id: 2};
  const narrow = {...wide, h: 3};
  assert.equal(evaluate(observer, points, [wide]).visibleCount, 0);
  const partial = evaluate(observer, points, [narrow]);
  assert.equal(partial.visibleCount, 2);
  assert.equal(partial.visibleFraction, 2 / 3);
  assert.equal(partial.final, true);
  assert.equal(evaluate(observer, points, [{...wide, id: 1}]).visibleCount, 3);
  assert.equal(evaluate(observer, points, [{...wide, x: 350}]).visibleCount, 3);
});

test('grade-separated visibility uses obstacle height and oriented deck volume', () => {
  const point = {...center, z: 10};
  const low = {x: 450, y: 340, w: 10, h: 30, zMin: 0, zMax: 5};
  assert.equal(evaluate(observer, [point], [low], true).visible, true);
  assert.equal(evaluate(observer, [point], [{...low, zMax: 20}], true).visible, false);
  const deck = {center: [450, 340, 11], axes: [[1, 0, 0], [0, 1, 0], [0, 0, 1]], size: [10, 30, 3]};
  assert.equal(evaluate(observer, [point], [deck], true).visible, false);
});

test('visible rotated body corners remain candidates when the center is outside the camera FOV', () => {
  const rotation = -18 * Math.PI / 180;
  const dx = 15 * Math.cos(rotation) - 8 * Math.sin(rotation);
  const dy = 15 * Math.sin(rotation) + 8 * Math.cos(rotation);
  for (const sign of [-1, 1]) {
    const v = {...observer, h: sign * 32.5 * Math.PI / 180};
    const corner = {x: center.x + sign * dx, y: center.y + sign * dy};
    assert.ok(model.angleDifference(v, center) > 32);
    const result = evaluate(v, [center, corner]);
    assert.equal(result.first, true);
    assert.equal(result.visible, true);
    assert.equal(result.final, true);
    assert.equal(result.inViewCount, 1);
    assert.equal(result.visibleFraction, 1);
    assert.equal(result.visibleBodyFraction, .5);
    const counts = countsFor({vehicles: [result]});
    assert.equal(counts.visible_independent, 1);
    assert.equal(counts.visible_but_first_rejected, 0);
    assert.equal(counts.independent_visible_retention, 1);
  }
});

test('aggregate reduction weights vehicles when scenario sizes differ', () => {
  const aggregate = summarize([
    {N: 20, N1: 10, N2: 5, visible_independent: 5, visible_but_first_rejected: 0},
    {N: 100, N1: 25, N2: 10, visible_independent: 10, visible_but_first_rejected: 0}
  ]);
  assert.equal(aggregate.first_reduction, 85 / 120);
  assert.equal(aggregate.second_conditional_reduction, 20 / 35);
  assert.equal(aggregate.total_reduction, 105 / 120);
  assert.equal(aggregate.independent_visible_retention, 1);
});

test('100 saved cases reproduce, have non-overlapping placements, and retain every visible candidate', () => {
  const batch = buildBatch(20261006, loaded);
  assert.equal(batch.scenarios.length, 100);
  assert.equal(new Set(batch.scenarios.map(row => row.scenario_seed)).size, 100);
  for (const road of ['normal', 'urban', 'underpass', 'elevated']) {
    assert.equal(batch.scenarios.filter(row => row.road === road).length, 25);
  }
  assert.equal(batch.summary.overall.N, batch.vehicles.length);
  assert.equal(batch.summary.overall.N, 5990);
  assert.equal(batch.summary.overall.N1, 2839);
  assert.equal(batch.summary.overall.N2, 925);
  assert.equal(batch.summary.overall.visible_but_first_rejected, 0);
  assert.equal(batch.summary.overall.independent_visible_retention, 1);
  for (const [name, content] of Object.entries(artifactContents(batch))) {
    assert.equal(fs.readFileSync(path.join(__dirname, name), 'utf8').replace(/\r\n/g, '\n'), content, name);
  }
  for (const row of batch.scenarios) {
    assert.equal(row.lanes, 6);
    assert.ok(row.N >= 20 && row.N <= 100 && row.N % 5 === 0);
    assert.equal(row.N2, row.visible_independent, `${row.scenario_id} visible candidates retained`);
    assert.equal(row.visible_but_first_rejected, 0, `${row.scenario_id} no visible candidates excluded`);
    const vehicles = batch.vehicles.filter(v => v.scenario_id === row.scenario_id)
      .map(v => ({...v, h: v.heading_radians}));
    assert.equal(vehicles.length, row.N);
    const incident = {x: 500, y: 340, h: -18 * Math.PI / 180, accident: true};
    for (let i = 0; i < vehicles.length; ++i) {
      const v = vehicles[i];
      assert.equal(v.visible_but_first_rejected, 0, `${row.scenario_id} vehicle ${v.vehicle_id}`);
      if (v.visible_independent) assert.equal(v.first, 1, `${row.scenario_id} vehicle ${v.vehicle_id} passes`);
      if (Math.abs(v.z - row.accident_z) < 24) {
        assert.equal(model.overlaps(v, incident), false, `${row.scenario_id} accident overlap`);
      }
      for (let j = i + 1; j < vehicles.length; ++j) {
        if (Math.abs(v.z - vehicles[j].z) < 24) {
          assert.equal(model.overlaps(v, vehicles[j]), false, `${row.scenario_id} vehicle overlap`);
        }
      }
    }
  }
  // Independent re-entry also detects leaked scene state between different road types.
  for (const index of [0, 33, 66, 99]) {
    const row = batch.scenarios[index];
    const scene = model.runScenario({road: row.road, lanes: row.lanes, count: row.N, seed: row.layout_seed});
    assert.equal(JSON.stringify(countsFor(scene)), JSON.stringify(Object.fromEntries(
      Object.keys(countsFor(scene)).map(key => [key, row[key]]))));
  }
});
