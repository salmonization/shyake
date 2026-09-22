// add ./web/* to mldsa65-wasm exports map; idempotent, cross-platform
const fs = require('fs');
const f = 'node_modules/mldsa65-wasm/package.json';
if (fs.existsSync(f)) {
  const s = fs.readFileSync(f, 'utf8');
  if (!s.includes('"./web/*"')) {
    fs.writeFileSync(
      f,
      s.replace('"exports": {', '"exports": { "./web/*": "./web/*",'),
    );
  }
}
