# Mixer Bridge 通信仕様（プロトコル 1、草案）

Mixer Bridge は OBS Studio の中で動き、音声ミキサーの計測値を外部プロセスに送り、外部プロセスからの操作を受け付けます。この文書だけを見れば、誰でもクライアントを書けることを目指しています。

草案の間は、互換性のない変更が入ることがあります。

## 接続

- 接続先は OS で変わります。
  - Windows: 名前付きパイプ `\\.\pipe\mixer-bridge`
  - macOS: Unix ドメインソケット `$TMPDIR/mixer-bridge.sock`。`$TMPDIR` はユーザーごとのディレクトリ（`/var/folders/…/T/`）で、OBS とクライアントが同じユーザーで動いていれば同じ値になります（Node.js なら `os.tmpdir()`）。`TMPDIR` が無い環境では `/tmp/mixer-bridge.sock` です
- 同じユーザーで動いているプロセスだけが接続できます。
- 複数のクライアントが同時に接続できます。計測の購読はクライアントごとに独立しています。
- OBS WebSocket の設定やパスワードは不要です。

## メッセージ

1 行に 1 つの JSON オブジェクトを、UTF-8 で送ります。行の区切りは LF（`\n`）です。

### 要求（クライアント → プラグイン）

```json
{"id": 1, "method": "set_volume", "params": {"source": "…", "volume_db": -6.0}}
```

- `id`：数値または文字列。応答に同じ値が返ります。
- `params`：省略できます（空オブジェクトと同じ扱い）。

### 応答（プラグイン → クライアント）

成功:

```json
{"id": 1, "result": {"volume_db": -6.0}}
```

失敗:

```json
{"id": 1, "error": {"code": "not_found", "message": "source not found"}}
```

| code | 意味 |
| --- | --- |
| `invalid_request` | JSON として読めない、または形が違う |
| `hello_required` | `hello` より前に別の要求を送った |
| `unsupported_protocol` | プロトコルのバージョンに対応していない |
| `unknown_method` | 知らない `method` |
| `invalid_params` | `params` の値が不正 |
| `not_found` | 指定したソースやフィルタがない |

### イベント（プラグイン → クライアント）

`id` を持たず、`event` と `data` を持ちます。

```json
{"event": "meters", "data": {…}}
```

## 値の約束

- ソースは `uuid` で指定します（名前を変えても変わらないため）。応答には `name` も付けます。
- 音量は dB で表します。無音や計測できない値は `null` です（JSON に `-Infinity` がないため）。
- ラウドネスは EBU R 128（ITU-R BS.1770）に従い、単位は LUFS、トゥルーピークは dBTP です。
- 品質統計のレベルは dBFS です（サンプル値の絶対値 1.0 を 0 dB とします）。

## メソッド

### `hello`

接続したら最初に送ります。これより前の要求には `hello_required` を返します。

| params | 型 | 説明 |
| --- | --- | --- |
| `protocol` | 数値 | クライアントが話すプロトコルのバージョン（今は `1`） |
| `client` | 文字列 | クライアントの名前とバージョン（ログ用） |

結果: `{"protocol": 1, "plugin": "0.2.0", "obs": "32.2.2"}`

### `list_sources`

音声を持つソースの一覧を返します。

結果:

```json
{"sources": [
  {"uuid": "…", "name": "マイク", "kind": "wasapi_input_capture",
   "volume_db": -5.0, "muted": false, "mixes": [1, 2, 3, 4, 5]}
]}
```

`mixes` は、そのソースを出力している音声トラックの番号（1〜6）です。

### `get_volume` / `set_volume`

| params | 型 | 説明 |
| --- | --- | --- |
| `source` | 文字列 | ソースの `uuid` |
| `volume_db` | 数値 | `set_volume` のみ。-100〜+26 に丸めます |

結果: `{"volume_db": -6.0, "muted": false}`

### `list_filters`

| params | 型 | 説明 |
| --- | --- | --- |
| `source` | 文字列 | ソースの `uuid` |

結果: `{"filters": [{"name": "ゲイン", "kind": "gain_filter", "enabled": true, "settings": {"db": 1.0}}]}`

### `set_filter`

既存のフィルタの設定を変えます。フィルタの追加や削除はしません。

| params | 型 | 説明 |
| --- | --- | --- |
| `source` | 文字列 | ソースの `uuid` |
| `filter` | 文字列 | フィルタの名前 |
| `enabled` | 真偽値 | 省略可 |
| `settings` | オブジェクト | 省略可。指定したキーだけを上書きします |

結果: `list_filters` の 1 要素と同じ形

### `subscribe_meters`

計測値の `meters` イベントを、一定間隔で送るよう頼みます。もう一度呼ぶと、前の購読を置き換えます。積算値（`integrated` と `true_peak`）は、購読を始めた時点から数えます。

| params | 型 | 説明 |
| --- | --- | --- |
| `interval_ms` | 数値 | 100〜1000。省略時は 100 |
| `sources` | 文字列の配列 | 測るソースの `uuid` |
| `mixes` | 数値の配列 | 測る音声トラックの番号（1〜6）。省略時は `[1]` |
| `quality` | 真偽値 | `true` にすると、`meters` イベントの `sources` の各要素に品質統計 `quality` を付けます。省略時は `false` |

結果: `{}`

`quality` が `true` のときは、帯域別レベルの境界を返します。

```json
{"quality": {"band_edges_hz": [20, 150, 500, 2000, 5000, 10000]}}
```

`band_edges_hz` は昇順の Hz の配列です。`meters` イベントの `bands` の i 番目は、`band_edges_hz[i]` から `band_edges_hz[i + 1]` までの帯域です。境界は将来変わることがあるので、クライアントはこの配列で `bands` を解釈してください。

### `unsubscribe_meters`

`meters` イベントを止めます。結果: `{}`

### `reset_meters`

購読中の積算値をリセットします。結果: `{}`

## イベント

### `meters`

```json
{"event": "meters", "data": {
  "time": 12.3,
  "mixes": [
    {"mix": 1, "momentary": -15.2, "short_term": -15.8, "integrated": -16.1, "true_peak": -2.0}
  ],
  "sources": [
    {"uuid": "…", "momentary": -17.0, "short_term": -17.4, "integrated": -17.2, "true_peak": -4.1}
  ]
}}
```

- `time`：購読を始めてからの秒数
- `mixes`：OBS がエンコーダーに渡す、その音声トラックの最終出力を測った値
- `sources`：そのソースのフィルタ、フェーダー、ミュートを通った後の値。ミキサーで合成される直前の音です
- `momentary` は 400ms、`short_term` は 3 秒の窓で測ります

#### 品質統計 `quality`

`subscribe_meters` に `quality: true` を渡したときだけ、`sources` の各要素に `quality` が付きます。`mixes` には付きません。

```json
{"uuid": "…", "momentary": -17.0, "short_term": -17.4, "integrated": -17.2, "true_peak": -4.1,
 "quality": {"lra": 6.2, "sample_peak": -3.0, "clip_ratio": 0.0, "bands": [-40.1, -28.3, -22.0, -30.5, -41.2]}}
```

| キー | 単位 | 意味 |
| --- | --- | --- |
| `lra` | LU | EBU R 128 のラウドネスレンジ。`integrated` と同じく、購読開始（または `reset_meters`）から積算します。3 秒のブロックが揃う開始後 3 秒間と、-70 LUFS 以上の 3 秒ブロックがまだ無いとき（無音）は `null` |
| `sample_peak` | dBFS | 直前の送信間隔のサンプルピーク。無音は `null` |
| `clip_ratio` | 0〜1 | 直前の送信間隔で、絶対値が 0.999 以上だったサンプルの割合（全チャンネル分）。サンプルがなければ `0` |
| `bands` | dBFS の配列 | 直前の送信間隔の帯域別レベル（帯域ごとの RMS）。帯域は `subscribe_meters` の結果の `band_edges_hz` で決まります。無音（区間のサンプルがすべて 0）は `null` |

- `sample_peak`、`clip_ratio`、`bands` は、フィルタを通った後、フェーダーとミュートを掛ける前の音で測ります。`momentary` などはフェーダー後なので、ここだけ違います。フェーダーを下げても ADC で起きたクリップは消えないので、クリップの検出にはフェーダー前の波形が要るためです。
- `lra` は `integrated` と同じ音（フェーダー後）から求めます。ラウドネスの範囲なので、計測中にフェーダーを動かさない限り、フェーダー前と同じ値になります。
- クリップの閾値を 1.0 でなく 0.999 にしているのは、整数 PCM の正の最大値（16 bit なら 32767/32768）が float では 1.0 に届かないためです。計測点がフィルタ後なので、ゲインフィルタで下げると見逃し、上げると誤検出します。
- `bands` は、全チャンネルを平均したモノラルを帯域ごとに 2 次のバンドパスに通し、区間の RMS を取った値です。K 特性などの重み付けは掛けません。帯域の間の差で、声の状態を見る前提です。
- 音がある区間でも、帯域のエネルギーがちょうど 0 になると、その要素だけが `null` になります。実際にはほぼ起きません。
- 帯域の目安: 20〜150 Hz は近接効果、机の振動、空調（ハイパスで切る帯域）。150〜500 Hz はこもり。500〜2000 Hz は声の基本の帯域で、ほかの帯域はこれとの差で見ます。2000〜5000 Hz は子音の明瞭度。5000〜10000 Hz は歯擦音。

## バージョン

- `protocol` は、互換性のない変更をしたときだけ上げます。
- メソッド、`params`、結果、イベントにキーが増えることがあります。クライアントは知らないキーを無視してください。
