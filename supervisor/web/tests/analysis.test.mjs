// Unit tests of the schedulability analysis: node supervisor/web/tests/analysis.test.mjs
import assert from 'node:assert/strict';
import { responseTimes, utilisation, llBound, rmViolations } from '../analysis-core.js';

let failures = 0;
function test(name, fn) {
  try { fn(); console.log('ok  ', name); } catch (e) { failures++; console.log('FAIL', name, '\n    ', e.message); }
}

// Classic textbook set (Burns & Wellings): T = 7, 12, 20; C = 3, 3, 5; rate-monotonic priorities.
const set = [
  { id: 'a', prio: 3, T: 7,  D: 7,  C: 3 },
  { id: 'b', prio: 2, T: 12, D: 12, C: 3 },
  { id: 'c', prio: 1, T: 20, D: 20, C: 5 },
];

test('textbook response times are 3, 6 and 20', () => {
  const r = responseTimes(set);
  assert.equal(r.get('a').R, 3);
  assert.equal(r.get('b').R, 6);
  assert.equal(r.get('c').R, 20);
  assert.equal(r.get('c').ok, true);
});

test('the iteration steps of the lowest task are 5, 11, 14, 17, 20, 20', () => {
  assert.deepEqual(responseTimes(set).get('c').steps, [5, 11, 14, 17, 20, 20]);
});

test('one more unit of C makes the lowest task miss its deadline', () => {
  const r = responseTimes(set.map(t => t.id === 'c' ? { ...t, C: 6 } : t));
  assert.equal(r.get('c').ok, false);
});

test('a finite response time can still miss the deadline', () => {
  // first job of y: 3 jobs of x (9) + its own 3 = done at 12, after its deadline 8
  const r = responseTimes([{ id: 'x', prio: 2, T: 4, D: 4, C: 3 }, { id: 'y', prio: 1, T: 8, D: 8, C: 3 }]);
  assert.equal(r.get('y').R, 12);
  assert.equal(r.get('y').ok, false);
});

test('with 75 % taken above it, a long job still ends (first job at 24)', () => {
  // R = 6 + 3 ceil(R / 4): 6, 12, 15, 18, 21, 24, 24
  const r = responseTimes([{ id: 'x', prio: 2, T: 4, D: 4, C: 3 }, { id: 'y', prio: 1, T: 8, D: 8, C: 6 }]);
  assert.equal(r.get('y').R, 24);
});

test('with 100 % taken above it, a task never runs: infinity', () => {
  // R = 6 + 4 ceil(R / 4) grows by 4 at least at each step
  const r = responseTimes([{ id: 'x', prio: 2, T: 4, D: 4, C: 4 }, { id: 'y', prio: 1, T: 8, D: 8, C: 6 }]);
  assert.equal(r.get('y').R, Infinity);
  assert.equal(r.get('y').ok, false);
});

test('context-switch cost and interrupts add interference', () => {
  const base = responseTimes(set).get('b').R;
  const withCtx = responseTimes(set, { ctx: 0.5 }).get('b').R;
  const withIrq = responseTimes(set, { interrupts: [{ T: 5, C: 1 }] }).get('b').R;
  assert.ok(withCtx > base);
  assert.ok(withIrq > base);
});

test('equal priorities interfere with each other (time slicing, conservative)', () => {
  const r = responseTimes([{ id: 'p', prio: 1, T: 10, D: 10, C: 2 }, { id: 'q', prio: 1, T: 10, D: 10, C: 2 }]);
  assert.equal(r.get('p').R, 4);
});

test('utilisation and Liu-Layland bound', () => {
  assert.ok(Math.abs(utilisation(set) - (3 / 7 + 3 / 12 + 5 / 20)) < 1e-12);
  assert.ok(Math.abs(llBound(3) - 0.7797631) < 1e-6);
  assert.equal(llBound(0), 1);
});

test('rate-monotonic violations are found', () => {
  const v = rmViolations([{ id: 's', prio: 1, T: 5 }, { id: 'l', prio: 2, T: 25 }]);
  assert.equal(v.length, 1);
  assert.equal(v[0][0].id, 's');
});

if (failures) { console.log(`${failures} failure(s)`); process.exit(1); }
console.log('all analysis tests passed');
