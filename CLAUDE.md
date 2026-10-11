# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 概要

OBS Studio 用プラグイン（C、GPL-2.0-or-later）。音声ミキサーのラウドネスを EBU R 128 で計測して外部プロセスへ送り、音量とフィルタ設定の取得・変更を受け付ける。UI は持たない。通信は Windows の名前付きパイプ `\.\pipe\mixer-bridge` で、1 行 1 JSON。構成は [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate) をそのまま使っている。

ソースのコメント、README、docs は日本語で書く。英語にしない。

## ビルド

依存（OBS 31.1.1 のヘッダー、obs-deps）は `buildspec.json` に基づき、configure 時に `.deps/` へ自動ダウンロードされる。

```bash
cmake --preset windows-x64 -G "Visual Studio 18 2026"
cmake --build build_x64 --config RelWithDebInfo
```

- プリセットの既定ジェネレーターは VS 2022。この開発機には VS 2026 しか無いので `-G "Visual Studio 18 2026"` を付ける（既存の `build_x64` はそれで生成済み）
- Git Bash の PATH に cmake は無い。VS 同梱のものを使う:
  `"/c/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"`
- CI は `windows-ci-x64` プリセットで `CMAKE_COMPILE_WARNING_AS_ERROR=ON`。警告が 1 つでもあると CI は落ちるので、PR 前はこのプリセットで configure して確認する
- インストール先の形（OBS に置くフォルダー構成）を得るには:
  `cmake --install build_x64 --prefix release/RelWithDebInfo --config RelWithDebInfo`
  → `release/RelWithDebInfo/mixer-bridge/{bin/64bit/mixer-bridge.dll, data/}`。これを `C:\ProgramData\obs-studio\plugins\mixer-bridge` に置く
- macOS / Linux はビルドだけ通る（`server-stub.c`）。通信は未実装

## テスト

自動テストは無い。動作確認は OBS を起動した状態で付属クライアントを実行する:

```bash
node examples/minimal-client.mjs "マイク"
```

## フォーマット（CI で検査される）

- C ソース: clang-format（`.clang-format`、幅 120、タブ幅 8）。対象は `src/**/*.{c,h}` のみ。`deps/` は対象外で、編集もしない
- CMake: gersemi（`.gersemirc`、幅 120、インデント 2）。対象は `CMakeLists.txt` と `cmake/**`
- CI は clang-format **19.1.1 固定**で、Linux 上の zsh スクリプト `build-aux/run-clang-format --check` を走らせる。Windows では VS 同梱の clang-format を直接使う（バージョン差で結果がずれることがあるので、CI の結果を正とする）:

```bash
"/c/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/Llvm/x64/bin/clang-format.exe" -style=file -i src/*.c src/*.h
```

## アーキテクチャ

データの流れ（`src/` の 5 モジュール）:

```
外部プロセス ⇄ server-win.c ⇄ protocol.c ⇄ tap.c ⇄ meter.c（libebur128）
   名前付きパイプ    行の分割      JSON-RPC 風      OBS 音声コールバック
```

- **server-win.c**: listen スレッドが `CreateNamedPipe` でクライアントごとにパイプを作り、1 クライアントに 1 スレッドを割り当てる。読み込みは overlapped I/O で、`session_meters_due_ms()` の残り時間を `WaitForMultipleObjects` のタイムアウトに使い、読み込み待ちと meters イベント送信を同じループで回す。SDDL で「同じユーザーと SYSTEM のみ」に制限する。`server.h` の 2 関数だけが公開 API で、他プラットフォームは `server-stub.c` で差し替える
- **protocol.c**: `struct session` が 1 クライアントの状態（`hello` 済みか、購読中の `tap`、送信間隔）。`session_handle()` が 1 行を受けて応答 JSON を返す。`hello` 以外は `hello` 前に拒否する。OBS API はここから直接呼ぶ（OBS の API はスレッドセーフなので別スレッドから呼んで良い）
- **tap.c**: 購読 1 件分。ソースには `obs_source_add_audio_capture_callback`（フィルタ後・フェーダー前の音なので、ここでフェーダーとミュートを掛ける）、音声トラックには `obs_add_raw_audio_callback` を取り付け、それぞれに `meter` を 1 つ持つ。`subscribe_meters` を再送すると tap を作り直す
- **meter.c**: libebur128 の薄いラッパー。planar → interleaved に並べ替えて投入する。**音声スレッドから書き、通信スレッドから読む**ので内部に mutex を持つ。無音・未計測は `-INFINITY` で返し、protocol.c の `add_db()` が JSON の `null` に変える

スレッド: OBS 音声スレッド（meter への追加のみ）、listen スレッド、クライアントごとのスレッド。`session` と `tap` はそのクライアントのスレッドだけが触る前提。

### 通信仕様を変えるとき

- [docs/protocol.md](docs/protocol.md) が仕様の正。`protocol.c` のメソッド・params・結果・エラーコードを変えたら必ず同じ PR で更新する
- キーの追加は互換変更として扱い、`MIXER_BRIDGE_PROTOCOL`（`protocol.h`）は互換性のない変更のときだけ上げる
- `examples/minimal-client.mjs` は「protocol.md だけを見て書いた」例。仕様変更で動かなくなったら直す

## 設定ファイルの約束

- `buildspec.json` がプラグイン名・バージョン・OBS 依存バージョンの唯一の出所。`PLUGIN_VERSION` と `hello` の `plugin` もここから入る。リリース時はここだけ上げる
- `.gitignore` はホワイトリスト方式（`/*` を除外して `!/dir` で許可）。新しいトップレベルのディレクトリやファイルを追加したら `!` エントリも足す
- `data/locale/en-US.ini` は `OBS_MODULE_USE_DEFAULT_LOCALE` が要求するので空にしない
- 同梱ライブラリ（`deps/libebur128` 1.2.6、`deps/cjson` 1.7.19）は `vendored-deps` 静的ライブラリとして警告を切ってビルドする。更新は上流のバージョンごと差し替える
- `.gitattributes` で LF 固定。CRLF を混入させない
