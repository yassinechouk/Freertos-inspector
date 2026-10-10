// 3D view of the motor (Three.js). The rotor turns at the MEASURED speed, slowed down for display.
import * as THREE from '/three.module.min.js';

// A 60 Hz screen samples the rotor 60 times per second. Above 30 rev/s (half the frame rate, the
// Nyquist limit) the wheel seems to stop or turn backwards, and beyond a few rev/s it is a blur.
// 3000 rpm = 50 rev/s, so the view is slowed down: same sampling limit as the encoder itself.
const SLOW = 25;

function diskTexture() {
  const c = document.createElement('canvas');
  c.width = c.height = 512;
  const g = c.getContext('2d');
  g.fillStyle = '#2b2f36';
  g.beginPath(); g.arc(256, 256, 256, 0, 2 * Math.PI); g.fill();
  g.fillStyle = '#d9dde3';
  const slits = 60;                                   // the real disk has 500 lines: 60 keep it readable
  for (let i = 0; i < slits; i++) {
    const a = (i / slits) * 2 * Math.PI;
    g.save(); g.translate(256, 256); g.rotate(a);
    g.fillRect(190, -6, 52, 12);
    g.restore();
  }
  g.fillStyle = '#f0883e';                            // index mark: makes the rotation visible
  g.beginPath(); g.arc(256, 70, 22, 0, 2 * Math.PI); g.fill();
  g.fillStyle = '#9aa3ad';
  g.beginPath(); g.arc(256, 256, 40, 0, 2 * Math.PI); g.fill();
  const t = new THREE.CanvasTexture(c);
  t.colorSpace = THREE.SRGBColorSpace;
  return t;
}

export function createMotor(container) {
  const renderer = new THREE.WebGLRenderer({ antialias: true, alpha: true });
  renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
  container.appendChild(renderer.domElement);

  const scene = new THREE.Scene();
  const camera = new THREE.PerspectiveCamera(32, 1, 0.1, 100);
  camera.position.set(3.4, 1.7, 4.6);
  camera.lookAt(0.55, 0, 0);
  scene.add(new THREE.HemisphereLight(0xffffff, 0x445566, 1.1));
  const sun = new THREE.DirectionalLight(0xffffff, 1.8);
  sun.position.set(3, 4, 5);
  scene.add(sun);

  // Housing along the X axis, with cooling fins and a dark end cap.
  const housingMat = new THREE.MeshStandardMaterial({ color: 0x8a94a6, metalness: 0.6, roughness: 0.35 });
  const housing = new THREE.Mesh(new THREE.CylinderGeometry(0.8, 0.8, 2.0, 48), housingMat);
  housing.rotation.z = Math.PI / 2;
  scene.add(housing);
  for (let x = -0.8; x <= 0.81; x += 0.2) {
    const fin = new THREE.Mesh(new THREE.TorusGeometry(0.81, 0.025, 8, 48), housingMat);
    fin.rotation.y = Math.PI / 2;
    fin.position.x = x;
    scene.add(fin);
  }
  const capMat = new THREE.MeshStandardMaterial({ color: 0x3b4252, metalness: 0.5, roughness: 0.5 });
  const cap = new THREE.Mesh(new THREE.CylinderGeometry(0.82, 0.82, 0.12, 48), capMat);
  cap.rotation.z = Math.PI / 2;
  cap.position.x = 1.04;
  scene.add(cap);

  // Rotating part: shaft with a key, and the encoder disk.
  const rotor = new THREE.Group();
  rotor.position.x = 1.1;
  scene.add(rotor);
  const steel = new THREE.MeshStandardMaterial({ color: 0xd0d4da, metalness: 0.9, roughness: 0.2 });
  const shaft = new THREE.Mesh(new THREE.CylinderGeometry(0.09, 0.09, 1.0, 24), steel);
  shaft.rotation.z = Math.PI / 2;
  shaft.position.x = 0.5;
  rotor.add(shaft);
  const key = new THREE.Mesh(new THREE.BoxGeometry(0.5, 0.05, 0.05), new THREE.MeshStandardMaterial({ color: 0xf0883e }));
  key.position.set(0.7, 0.1, 0);
  rotor.add(key);
  const face = new THREE.MeshStandardMaterial({ map: diskTexture(), metalness: 0.2, roughness: 0.6 });
  const edge = new THREE.MeshStandardMaterial({ color: 0x2b2f36 });
  const disk = new THREE.Mesh(new THREE.CylinderGeometry(0.62, 0.62, 0.03, 72), [edge, face, face]);
  disk.rotation.z = -Math.PI / 2;                    // caps face +X / -X
  disk.position.x = 0.35;
  rotor.add(disk);

  const base = new THREE.Color(0x8a94a6), hot = new THREE.Color(0xd9733a);
  let rpmTarget = 0, rpmShown = 0, duty = 0, fault = 0, angle = 0, last = performance.now();

  function resize() {
    const w = container.clientWidth, h = container.clientHeight;
    if (!w || !h) return;
    renderer.setSize(w, h, false);
    camera.aspect = w / h;
    camera.updateProjectionMatrix();
  }
  new ResizeObserver(resize).observe(container);
  resize();

  function frame(now) {
    const dt = Math.min((now - last) / 1000, 0.1);
    last = now;
    rpmShown += (rpmTarget - rpmShown) * Math.min(dt / 0.05, 1);   // 50 ms smoothing between 10 Hz updates
    angle += 2 * Math.PI * (rpmShown / 60 / SLOW) * dt;
    rotor.rotation.x = angle;
    housingMat.color.copy(base).lerp(hot, Math.min(Math.max(duty, 0), 1) * 0.6);
    housingMat.emissive.setHex(fault ? 0x550000 : 0x000000);
    renderer.render(scene, camera);
    requestAnimationFrame(frame);
  }
  requestAnimationFrame(frame);

  return {
    note: `Shown ${SLOW}x slower than real: a 60 Hz screen cannot show up to 50 rev/s (above half the frame rate a wheel ` +
          `seems to stop or turn backwards: the same sampling limit as the encoder). Colour: duty cycle; red glow: fault.`,
    set(rpm, dutyFraction, faultCode) { rpmTarget = rpm; duty = dutyFraction; fault = faultCode; },
  };
}
