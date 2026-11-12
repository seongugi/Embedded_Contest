// Run from the repository root: node scripts/docs/render-vision-assets.cjs
// Dependencies: sharp, @napi-rs/canvas. SVG and PNG outputs use the same geometry.
const fs = require('node:fs');
const path = require('node:path');
const sharp = require('sharp');
const { createCanvas } = require('@napi-rs/canvas');
const output = path.resolve(__dirname, '../../docs/assets');
const measure = createCanvas(1, 1).getContext('2d');
const C = { ink: '#153247', blue: '#236eac', teal: '#138174', pale: '#eaf3f8', green: '#e8f5f1', line: '#cbdbe5', muted: '#526d80' };
const xml = value => String(value).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
let p;
function start(w, h) {
  p = [`<svg xmlns="http://www.w3.org/2000/svg" width="${w}" height="${h}" viewBox="0 0 ${w} ${h}">`, `<rect width="${w}" height="${h}" fill="white"/>`, `<defs><marker id="arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M0 0L10 5L0 10Z" fill="${C.blue}"/></marker></defs>`];
}
function rect(x, y, w, h, fill, stroke = C.line, r = 8) {
  p.push(`<rect x="${x}" y="${y}" width="${w}" height="${h}" rx="${r}" fill="${fill}" stroke="${stroke}" stroke-width="1.5"/>`);
}
function text(value, x, y, size = 25, weight = 500, fill = C.ink, anchor = 'middle', maxWidth = 1240) {
  measure.font = `${weight} ${size}px "Malgun Gothic"`;
  if (measure.measureText(value).width > maxWidth) throw new Error(`Label overflow: ${value}`);
  p.push(`<text x="${x}" y="${y}" font-family="Malgun Gothic, sans-serif" font-size="${size}" font-weight="${weight}" fill="${fill}" text-anchor="${anchor}">${xml(value)}</text>`);
}
function arrow(d) { p.push(`<path d="${d}" fill="none" stroke="${C.blue}" stroke-width="3" marker-end="url(#arrow)"/>`); }
function box(x, y, w, h, lines, fill = C.pale) {
  rect(x, y, w, h, fill);
  const gap = 31, top = y + h / 2 - (lines.length - 1) * gap / 2 + 9;
  lines.forEach((s, i) => text(s, x + w / 2, top + i * gap, i ? 23 : 27, i ? 500 : 700, C.ink, 'middle', w - 28));
}
async function save(name) {
  const svg = p.join('\n') + '\n</svg>\n';
  fs.writeFileSync(path.join(output, name + '.svg'), svg);
  await sharp(Buffer.from(svg), { density: 144 }).png().toFile(path.join(output, name + '.png'));
}
async function main() {
  start(1280, 422);
  box(24, 24, 308, 104, ['사고차 · IMU 감지', '즉시 자체 영상 요청']);
  box(24, 160, 308, 104, ['관측차 · 화각 통과', '즉시 영상 분석 요청'], C.green);
  box(416, 80, 348, 144, ['카메라는 계속 촬영', '사고 전 8초 · 후 2초', '같은 10초 구간 확보']);
  box(856, 24, 400, 104, ['사고차', '확보한 구간을 MP4로 저장']);
  box(856, 160, 400, 104, ['관측차', 'YOLOX 검출 → 같은 구간 저장'], C.green);
  arrow('M332 76 H372 V130 H416'); arrow('M332 212 H372 V177 H416');
  arrow('M764 130 H805 V76 H856'); arrow('M764 177 H805 V212 H856');
  rect(24, 308, 1232, 88, C.ink, 'none');
  text('MP4 완성 → 인계 기록 저장 → Uploader 영속성 큐', 640, 345, 29, 700, 'white');
  text('영상 인계·전송과 다음 프레임 촬영을 병행', 640, 379, 23, 500, 'white');
  await save('vision-event-flow');

  start(1280, 516);
  text('미리 확보한 960개 프레임 버퍼를 순환 사용', 640, 44, 31, 700);
  rect(24, 72, 624, 416, '#f6f9fb');
  const cx = 200, cy = 266;
  for (let i = 0; i < 12; i++) {
    const a = (-90 + i * 30) * Math.PI / 180;
    const x = cx + 112 * Math.cos(a), y = cy + 112 * Math.sin(a);
    p.push(`<rect x="${x - 26}" y="${y - 18}" width="52" height="36" rx="5" transform="rotate(${i * 30} ${x} ${y})" fill="${i < 8 ? C.blue : C.teal}"/>`);
  }
  text('RAM Ring', cx, 253, 28, 700);
  text('최근 20초', cx, 287, 25, 700, C.blue);
  text('최대 640개', 459, 174, 30, 700, C.blue);
  text('최근 프레임 유지', 459, 212, 24);
  text('여유 320개', 459, 294, 30, 700, C.teal);
  text('참조 중인 슬롯 교체', 459, 332, 24);
  text('새 프레임 촬영은 계속', 336, 450, 28, 700, C.blue);
  box(710, 82, 546, 108, ['① 사고 구간 참조', '프레임 본체 복사 없이 참조·시각 확보']);
  box(710, 226, 546, 108, ['② 분석·저장 중 보호', '참조 중인 프레임은 덮어쓰기 방지'], C.green);
  box(710, 370, 546, 108, ['③ 처리 종료 후 반환', '참조를 반환하고 버퍼 재사용']);
  arrow('M983 190 V226'); arrow('M983 334 V370');
  await save('vision-frame-pool');

  start(1280, 388);
  box(24, 28, 360, 142, ['① 위치·화각', '사고차 차체 범위가', '카메라에 들어오는 관측차']);
  box(460, 28, 360, 142, ['② 실제 영상', 'YOLOX-Tiny가', '대상 차종 검출'], C.green);
  box(896, 28, 360, 142, ['증거 수집', '통과한 관측 영상', '사고 ID로 저장·전송'], C.green);
  arrow('M384 99 H460'); arrow('M820 99 H896');
  rect(24, 222, 1232, 136, '#f5f8fa');
  text('방향이 맞아도 앞차·지하차도·고가도로가 시야를 가릴 수 있습니다.', 640, 270, 27, 700);
  text('먼저 위치로 분석 대상을 줄이고, 영상에서 대상 차량을 한 번 더 찾습니다.', 640, 318, 26);
  await save('vision-two-stage');

  start(1280, 208);
  const steps = [['촬영·추출', '시점·거리·가림 수집'], ['박스 검수', '차종·차량 지정'], ['데이터셋 분리', '촬영 묶음별 배정'], ['학습·배포', 'YOLOX-Tiny → ONNX']];
  steps.forEach((s, i) => { box(24 + 320 * i, 24, 272, 118, s, i === 3 ? C.green : C.pale); if (i < 3) arrow(`M${296 + 320 * i} 83 H${344 + 320 * i}`); });
  text('같은 촬영 묶음의 프레임은 학습·검증·시험 중 한 세트에 배정', 640, 184, 24, 600, C.muted);
  await save('vision-training-flow');
  console.log('Rendered four vision diagrams; all labels fit their boxes.');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
