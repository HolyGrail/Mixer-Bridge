# Mixer Bridge

OBS Studio の音声ミキサーの計測値を外部プロセスに送り、外部プロセスからの操作を受け付けるプラグインです（サードパーティ製）。画面は持ちません。

## できること

- マスター（音声トラック 1〜6）とソースごとのラウドネスを、EBU R 128 で計測して送る（momentary、short-term、integrated、トゥルーピーク）
- ソースごとの値は、フィルタ、フェーダー、ミュートを通った後で測る
- ソースの音量（フェーダー）の取得と変更
- 既存フィルタの設定の取得と変更（フィルタの追加や削除はしない）

通信は Windows では名前付きパイプ `\\.\pipe\mixer-bridge`、macOS では Unix ドメインソケット `$TMPDIR/mixer-bridge.sock` で、1 行に 1 つの JSON を送ります。同じユーザーのプロセスだけが接続できます。OBS WebSocket の設定は要りません。

## 対応環境

| OS | 状態 |
| --- | --- |
| Windows 10 / 11（x64） | 対応。OBS Studio 32.2.2 で確認 |
| macOS 12 以降（Apple シリコン / Intel） | 対応。OBS Studio 33.0.0 で確認 |
| Linux | ビルドのみ。通信部分が未実装のため、読み込んでも何もしない |

ビルドは OBS Studio 31.1.1 のヘッダーを使うため、OBS 31 以降で読み込めます。

## インストール（Windows）

[リリース](https://github.com/HolyGrail/Mixer-Bridge/releases)から `mixer-bridge-<バージョン>-windows-x64.zip` を取得します。中の `mixer-bridge` フォルダーを `C:\ProgramData\obs-studio\plugins\` に置き、OBS を再起動してください。

## インストール（macOS）

リリースの配布物はまだ Windows 向けだけです。後述の手順でビルドし、できた `mixer-bridge.plugin` を `~/Library/Application Support/obs-studio/plugins/` に置いて、OBS を再起動してください。

## 通信仕様とクライアントの例

- 通信仕様: [docs/protocol.md](docs/protocol.md)（プロトコル 1、草案）
- 最小クライアント: [examples/minimal-client.mjs](examples/minimal-client.mjs)。Node.js 18 以降で、追加のライブラリなしに動きます

```bash
node examples/minimal-client.mjs "マイク"
```

## ビルド（Windows）

[OBS の公式プラグインテンプレート](https://github.com/obsproject/obs-plugintemplate)の構成をそのまま使っています。Visual Studio 2022 以降（C++ によるデスクトップ開発）と CMake 3.28 以降が必要です。

```bash
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo
```

Visual Studio 2026 の場合は、生成時に `-G "Visual Studio 18 2026"` を付けます。

## ビルド（macOS）

Xcode 16 以降と CMake 3.28 以降が必要です。arm64 と x86_64 のユニバーサルバイナリができます。

```bash
cmake --preset macos
cmake --build --preset macos
```

`build_macos/RelWithDebInfo/mixer-bridge.plugin` ができます。

## 同梱しているライブラリ

| ライブラリ | 用途 | ライセンス |
| --- | --- | --- |
| [libebur128](https://github.com/jiixyj/libebur128) 1.2.6 | ラウドネスの計算 | MIT（[deps/libebur128/COPYING](deps/libebur128/COPYING)） |
| [cJSON](https://github.com/DaveGamble/cJSON) 1.7.19 | JSON の読み書き | MIT（[deps/cjson/LICENSE](deps/cjson/LICENSE)） |

## ライセンス

GPL-2.0-or-later。全文は [LICENSE](LICENSE) を参照してください。

## AI ツールの利用について

このプラグインの開発には AI コーディングツール（Claude Code）を使用しています。設計の判断と動作の確認は作者が行っています。
