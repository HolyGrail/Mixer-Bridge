// Mixer Bridge の最小クライアント（docs/protocol.md だけを見て書いた例）。
// Node.js 18 以降で、追加のライブラリなしに動きます。
//
//   node examples/minimal-client.mjs            ソース一覧と、トラック1 のラウドネスを 5 秒表示
//   node examples/minimal-client.mjs "マイク"   そのソースのラウドネスも表示

import net from "node:net";
import readline from "node:readline";

const sourceName = process.argv[2];
const socket = net.connect("\\\\.\\pipe\\mixer-bridge");
const lines = readline.createInterface({ input: socket });
const pending = new Map();
let nextId = 1;

function call(method, params = {}) {
  const id = nextId++;
  socket.write(JSON.stringify({ id, method, params }) + "\n");
  return new Promise((resolve, reject) => pending.set(id, { resolve, reject }));
}

const fmt = (v) => (v === null ? "  —  " : v.toFixed(1).padStart(5));

lines.on("line", (line) => {
  const msg = JSON.parse(line);
  if (msg.event === "meters") {
    const mix = msg.data.mixes[0];
    const src = msg.data.sources[0];
    console.log(
      `${msg.data.time.toFixed(1).padStart(5)}s  トラック1 ${fmt(mix.momentary)} LUFS  ピーク ${fmt(mix.true_peak)} dBTP` +
        (src ? `  /  ${sourceName} ${fmt(src.momentary)} LUFS` : ""),
    );
    return;
  }
  const waiter = pending.get(msg.id);
  if (!waiter) return;
  pending.delete(msg.id);
  if (msg.error) waiter.reject(new Error(`${msg.error.code}: ${msg.error.message}`));
  else waiter.resolve(msg.result);
});

socket.on("error", (e) => {
  console.error(`Mixer Bridge に接続できません（OBS は起動していますか？）: ${e.message}`);
  process.exit(1);
});

const hello = await call("hello", { protocol: 1, client: "minimal-client/0.1.0" });
console.log(`接続しました: Mixer Bridge ${hello.plugin} / OBS ${hello.obs}`);

const { sources } = await call("list_sources");
for (const s of sources) {
  console.log(`  ${s.name}  音量 ${fmt(s.volume_db)} dB  ${s.muted ? "ミュート" : ""}  トラック ${s.mixes.join(",")}`);
}

const target = sources.find((s) => s.name === sourceName);
await call("subscribe_meters", { interval_ms: 500, mixes: [1], sources: target ? [target.uuid] : [] });
setTimeout(() => socket.end(), 5000);
