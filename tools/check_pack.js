// Checks the browser-side packer in main/index.html against the plane format. Run: node tools/check_pack.js
const fs = require('fs'), assert = require('assert');
const html = fs.readFileSync(__dirname + '/../main/index.html', 'utf8');
const pack = new Function(html.match(/const W=.*\n/)[0] + html.match(/function pack[\s\S]*?\n}\n/)[0] + 'return pack')();

const W = 960, H = 672, P = W * H / 8;
const px = new Uint8ClampedArray(W * H * 4).fill(255);
const set = (x, y, rgb) => px.set(rgb, 4 * (y * W + x));
set(0, 0, [0, 0, 0]);             // black, top left: bit 7 of byte 0
set(9, 0, [250, 10, 10]);         // red: bit 6 of red byte 1
set(W - 1, H - 1, [30, 30, 30]);  // dark grey -> black, bottom right: bit 0 of the last byte
const out = pack(px, false);
assert.strictEqual(out.length, 2 * P);
assert(out[0] === 0x80 && out[P + 1] === 0x40 && out[P - 1] === 0x01, 'bit packing');
assert.strictEqual(out.reduce((a, b) => a + b, 0), 0x80 + 0x40 + 0x01, 'stray ink');
console.log('pack ok');
