// Draws the SideDroid icon: a rounded tile with a phone beside a display.
// Writes host/resources/sidedroid.ico and ui/console resources without any image library.
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

const TILE = [0x4F, 0x37, 0x8B];
const SHAPE = [0xEA, 0xDD, 0xFF];
const ACCENT = [0x21, 0x00, 0x5D];

function inRounded(x, y, l, t, r, b, rad) {
  if (x < l || x > r || y < t || y > b) return false;
  const cx = Math.max(l + rad, Math.min(x, r - rad));
  const cy = Math.max(t + rad, Math.min(y, b - rad));
  return (x - cx) ** 2 + (y - cy) ** 2 <= rad * rad;
}

// Coverage by 4x4 supersampling, for smooth edges at small sizes.
function draw(size) {
  const px = Buffer.alloc(size * size * 4);
  const s = (v) => (v * size) / 64;
  for (let y = 0; y < size; y++) {
    for (let x = 0; x < size; x++) {
      let r = 0, g = 0, b = 0, a = 0;
      for (let sy = 0; sy < 4; sy++) {
        for (let sx = 0; sx < 4; sx++) {
          const fx = x + (sx + 0.5) / 4, fy = y + (sy + 0.5) / 4;
          let c = null;
          if (inRounded(fx, fy, 0, 0, size, size, s(18))) {
            c = TILE;
            if (inRounded(fx, fy, s(10), s(17), s(40), s(43), s(4))) c = SHAPE;       // display
            if (inRounded(fx, fy, s(13), s(20), s(37), s(40), s(2))) c = ACCENT;
            if (inRounded(fx, fy, s(35), s(23), s(55), s(53), s(5))) c = TILE;        // gap
            if (inRounded(fx, fy, s(37), s(25), s(53), s(51), s(4))) c = SHAPE;       // phone
            if (inRounded(fx, fy, s(39.5), s(28), s(50.5), s(48), s(1.5))) c = ACCENT;
          }
          if (c) { r += c[0]; g += c[1]; b += c[2]; a++; }
        }
      }
      const i = (y * size + x) * 4;
      if (a) {
        px[i] = Math.round(r / a); px[i + 1] = Math.round(g / a); px[i + 2] = Math.round(b / a);
        px[i + 3] = Math.round((a / 16) * 255);
      }
    }
  }
  return px; // RGBA, top-down
}

const crcTable = Array.from({ length: 256 }, (_, n) => {
  let c = n;
  for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
  return c >>> 0;
});
function crc32(buf) {
  let c = 0xffffffff;
  for (const b of buf) c = crcTable[(c ^ b) & 255] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}
function chunk(type, data) {
  const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
  const td = Buffer.concat([Buffer.from(type), data]);
  const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(td));
  return Buffer.concat([len, td, crc]);
}
function png(size) {
  const rgba = draw(size);
  const raw = Buffer.alloc((size * 4 + 1) * size);
  for (let y = 0; y < size; y++) {
    raw[y * (size * 4 + 1)] = 0;
    rgba.copy(raw, y * (size * 4 + 1) + 1, y * size * 4, (y + 1) * size * 4);
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(size, 0); ihdr.writeUInt32BE(size, 4);
  ihdr[8] = 8; ihdr[9] = 6;
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw, { level: 9 })), chunk('IEND', Buffer.alloc(0)),
  ]);
}

function ico(sizes) {
  const images = sizes.map(png);
  const header = Buffer.alloc(6 + 16 * sizes.length);
  header.writeUInt16LE(1, 2); header.writeUInt16LE(sizes.length, 4);
  let offset = header.length;
  sizes.forEach((s, i) => {
    const e = 6 + i * 16;
    header[e] = s >= 256 ? 0 : s; header[e + 1] = s >= 256 ? 0 : s;
    header.writeUInt16LE(1, e + 4); header.writeUInt16LE(32, e + 6);
    header.writeUInt32LE(images[i].length, e + 8); header.writeUInt32LE(offset, e + 12);
    offset += images[i].length;
  });
  return Buffer.concat([header, ...images]);
}

const root = path.join(__dirname, '..');
const icoData = ico([16, 20, 24, 32, 48, 64, 256]);
function put(rel, data) {
  const p = path.join(root, rel);
  fs.mkdirSync(path.dirname(p), { recursive: true });
  fs.writeFileSync(p, data);
  console.log(rel, data.length);
}
put('host/resources/sidedroid.ico', icoData);
put('ui/console/icons/sidedroid.ico', icoData);
put('ui/console/src/main/resources/sidedroid.png', png(256));
