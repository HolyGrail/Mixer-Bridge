# Mixer Bridge 通信仕様（プロトコル 1、草案）

Mixer Bridge は OBS Studio の中で動き、音声ミキサーの計測値を外部プロセスに送り、外部プロセスからの操作を受け付けます。この文書だけを見れば、誰でもクライアントを書けることを目指しています。

草案の間は、互換性のない変更が入ることがあります。

## 接続

- Windows の名前付きパイプ `\\.\pipe\mixer-bridge` に接続します。
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

## メソッド

### `hello`

接続したら最初に送ります。これより前の要求には `hello_required` を返します。

| params | 型 | 説明 |
| --- | --- | --- |
| `protocol` | 数値 | クライアントが話すプロトコルのバージョン（今は `1`） |
| `client` | 文字列 | クライアントの名前とバージョン（ログ用） |

結果: `{"protocol": 1, "plugin": "0.1.0", "obs": "32.2.2"}`

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

結果: `{}`

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

## バージョン

- `protocol` は、互換性のない変更をしたときだけ上げます。
- メソッド、`params`、結果、イベントにキーが増えることがあります。クライアントは知らないキーを無視してください。
