// Schedulability analysis for fixed-priority preemptive scheduling (FreeRTOS).
// Pure functions, no DOM: unit-tested in tests/analysis.test.mjs.

/* Worst-case response time of each task (Joseph & Pandya / Audsley iteration):
     R(n+1) = C_i + B_i + sum over j in hp(i) of ceil(R(n) / T_j) * (C_j + ctx)
   hp(i): tasks with a priority higher than or equal to i (equal priorities share the CPU by
   time slicing in FreeRTOS, so they are counted as interference: conservative).
   Each preemption also costs one context switch (ctx). Interrupts are passed as tasks with
   a priority above every task. Stops when R stabilises, or when R exceeds the limit.
   Returns, for each task, { R, steps, ok } where steps lists the successive values. */
export function responseTimes(tasks, { ctx = 0, interrupts = [], limitFactor = 20 } = {}) {
  const out = new Map();
  for (const i of tasks) {
    const hp = tasks.filter(j => j !== i && j.prio >= i.prio);
    const limit = Math.max(i.D || 0, i.T || 0, i.C) * limitFactor;
    let R = i.C + (i.B || 0), prev = -1;
    const steps = [R];
    while (R !== prev && R <= limit && steps.length < 500) {
      prev = R;
      R = i.C + (i.B || 0)
        + hp.reduce((s, j) => s + Math.ceil(prev / j.T) * (j.C + ctx), 0)
        + interrupts.reduce((s, q) => s + Math.ceil(prev / q.T) * q.C, 0);
      steps.push(R);
    }
    const R_ = R > limit ? Infinity : R;
    out.set(i.id, { R: R_, steps, ok: i.D ? R_ <= i.D : null });
  }
  return out;
}

/* Total utilisation U = sum C / T (tasks and interrupts). */
export function utilisation(tasks, interrupts = []) {
  return [...tasks, ...interrupts].reduce((s, x) => s + (x.T ? x.C / x.T : 0), 0);
}

/* Liu & Layland bound for rate-monotonic priorities: n (2^(1/n) - 1). Sufficient, not necessary. */
export function llBound(n) {
  return n > 0 ? n * (Math.pow(2, 1 / n) - 1) : 1;
}

/* Pairs (a, b) where a has a shorter period but a lower priority than b: not rate-monotonic. */
export function rmViolations(tasks) {
  const v = [];
  for (const a of tasks) for (const b of tasks) {
    if (a !== b && a.T < b.T && a.prio < b.prio) v.push([a, b]);
  }
  return v;
}
