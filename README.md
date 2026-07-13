# PlayerEditor

軽量な音声/動画プレイヤー + 簡易エディタ（C++ / Dear ImGui）。
再生・波形表示・トリミング・ラウドネス測定 (EBU R128) と **-14 LUFS 正規化**、動画の同期再生までを、即起動・軽量で行うことを目指しています。

A lightweight audio/video player & editor built with C++ and Dear ImGui.

## 主な機能
- 音声の読み込み（WAV / MP3 / FLAC / M4A / OGG / AAC ほか、miniaudio）
- 波形表示 / 再生 / 一時停止 / 停止 / ループ再生 / 音量スライダー
- ドラッグで区間選択、クリックでシーク
- **タブ**で複数ファイルを同時に開く（再生は排他、切替で即再生）。複数同時ドロップで別タブ
- **ラウドネス測定**（Integrated LUFS / True Peak / LRA、ffmpeg の loudnorm）
- **-14 LUFS 正規化**（書き出し）／**再生時 -14 LUFS**（ファイルは無改変、デフォルトON）
- **書き出し**: WAV / MP3 / M4A / FLAC / OGG / AIF、媒体別プリセット（ストリーミング/CD/放送等）、
  サンプルレート・ビット深度変換、範囲(全体/選択)、タグ埋め込み（入力ファイルのタグを自動流用）
- **加工**: ゲイン / フェードイン・アウト / リバース（メモリ上、選択範囲対応）
- **タブ連結**: 等パワークロスフェードで2タブを連結し新タブに
- **解析サイドパネル**: LUFS/TP/LRA/ピーク/RMS + リアルタイムスペクトラム（連続曲線）+ 曲全体平均。JSON/TXT保存
- **動画対応**: MP4/MOV等を開くと映像を表示（波形はトグル）。音声をマスタークロックに映像を同期再生、
  シーク・ループ追従。YouTube風コントロールバー（シーク/トリムハンドル/全画面）、
  映像ごとの切り出し(再エンコードなし)も可能
- **モニターシミュレーション**: スマホ(新旧)/ノートPC/イヤホン/車内/TV/電話の聴こえ方を再生時に再現
- ファイル関連付け・ドラッグ&ドロップ対応、単一インスタンス（2つ目の起動は既存ウィンドウの新タブに）

## 依存
- ビルド時に **CMake の FetchContent** で自動取得：GLFW 3.4 / Dear ImGui (docking) / miniaudio / portable-file-dialogs
- 実行時に **ffmpeg / ffprobe**（測定・エンコード・動画デコードに使用）。PATH か `winget install Gyan.FFmpeg`、または `ffmpeg/ffmpeg.exe` を隣に配置

## ビルド（Windows / MSVC）
```sh
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target PlayerEditor
```
生成物: `build/Release/PlayerEditor.exe`

テスト（ヘッドレス検証）:
```sh
cmake --build build --config Release --target PlayerEditorTests
./build/Release/PlayerEditorTests.exe
```

## 操作
- **Space**: 再生 / 停止　**F**: 全画面(動画)　**Esc**: 全画面解除
- 動画クリック: 再生/一時停止　ダブルクリック: 全画面
- ファイルをウィンドウにドラッグ&ドロップ（複数可）
- 波形をドラッグ: 区間選択 / クリック: シーク

---

# アーキテクチャ

## 設計方針
1. **UIと処理の分離** — DSP・解析・I/Oは「純粋な」ヘッダ/クラス（ImGui非依存）として切り出し、
   ヘッドレステスト(`tests/`)で数値検証する。UI(`main.cpp`)はそれらを呼ぶだけ。
2. **重い処理はffmpegサブプロセス** — 測定/エンコード/動画デコードは常駐させず必要時のみ、
   `CREATE_NO_WINDOW` で窓なし起動。バイナリ出力(PCM/映像)を受けるパイプにはstderrを混ぜない。
3. **起動時に何もしない** — デバイスopen・ffmpeg探索・測定はすべて遅延実行。即起動を維持。
4. **音声がマスタークロック** — 再生位置は常に音声(`Player`)が決め、映像はそれに追従する。
5. **バックグラウンド処理は「世代番号 + 共有状態」** — 測定/平均スペクトラムはdetachスレッドで実行し、
   `shared_ptr`の受け渡し領域に書き戻す。タブ切替/加工で世代が進むと古い結果は捨てる。

## ファイル構成
```
PlayerEditor/
├── CMakeLists.txt          # FetchContentで依存取得。PlayerEditor / PlayerEditorTests の2ターゲット
├── src/
│   ├── main.cpp            # UI層: App/Doc状態、全パネル描画、イベント配線 (ImGui)
│   │
│   │   # ― コアクラス (UI非依存・.cpp持ち) ―
│   ├── audio_clip.h/.cpp   # AudioClip: デコード済み音声のメモリ表現 (float32インターリーブ)
│   ├── player.h/.cpp       # Player: miniaudio再生 + ゲイン/ループ/モニターチェーン (音声クロック)
│   ├── video_reader.h/.cpp # VideoReader: ffmpeg常駐デコーダ(rawvideoパイプ) + フレームキュー
│   ├── ffmpeg.h/.cpp       # Ffmpeg名前空間: 測定/エンコード/probe/音声抽出/動画カット/タグ読取
│   ├── wav_io.h/.cpp       # WavIo: float32 WAV書き出し (依存なし)
│   ├── miniaudio_impl.cpp  # miniaudio実装の単一翻訳単位
│   │
│   │   # ― 純粋DSP/ユーティリティ (header-only・全てテスト対象) ―
│   ├── effects.h           # Fx: ゲイン/フェード/リバース (破壊的、範囲指定)
│   ├── join.h              # Join: 等パワークロスフェード連結
│   ├── spectrum.h          # Spec: radix-2 FFT / 瞬時・平均スペクトラム / 対数バンド集計(補間つき)
│   ├── biquad.h            # Bq: RBJバイクワッド (HP/LP/Peaking/Shelf) + 周波数応答 → モニターシム
│   ├── analysis.h          # Analysis: 解析レポート生成 (JSON/TXT)
│   └── platform_utf8.h     # plat: UTF-8 ⇔ UTF-16 変換 (Windowsの日本語パス対応)
└── tests/
    └── test_main.cpp       # ヘッドレステスト: 全モジュールの数値検証 (ALL PASS必須)
```

## クラス / モジュール設計

### UI層 (`main.cpp`)
| 型 | 役割 |
|---|---|
| `App` | アプリ全体の状態。タブ配列 `docs`、単一の `Player`（排他再生）、再生設定(-14/ループ/音量/モニター)、書き出し・加工・連結・全画面などのUI状態 |
| `Doc` | **1タブ分の状態**。`AudioClip`、再生位置/選択範囲、波形ピークキャッシュ、ラウドネス測定結果、タグ、スペクトラム表示キャッシュ、映像状態(`VideoReader`/GLテクスチャ) |
| `MeasureState` / `SpecAvgState` | バックグラウンド処理の受け渡し領域（mutex + 世代番号。スレッド寿命に依存しない） |

主なUI関数: `drawUI`(全体) → `drawTabs` / `drawWaveform` / `drawVideoPane`(+`drawVideoControls`) /
`drawAnalysisPanel` / `drawExportPopup` / `drawFxPopup` / `drawJoinPopup`。
再生系は `startPlayback` / `togglePlay` に集約（Space・ボタン・タブ切替・クリックすべて同経路）。

### コアクラス
| クラス | 概要 |
|---|---|
| `AudioClip` | float32インターリーブのサンプル + ch/sr。`load()` は miniaudio → 失敗時 ffmpeg にフォールバック（動画コンテナ対応）。ピーク/RMS計算つき |
| `Player` | miniaudioデバイスを1つ保持。コールバックで区間再生・ループ(戻り先指定)・ゲイン・**モニターチェーン**(biquad列+モノ化)を適用。位置は`atomic`で公開＝アプリ全体のクロック |
| `VideoReader` | `ffmpeg -f rawvideo` を子プロセスとして起動し、読み取りスレッドがRGB24フレームを**有界キュー(4)**に積む（満杯時はパイプ詰まりで自然なバックプレッシャ）。`popUpTo(pts)`で音声時刻以下の最新フレームを取得。シークは stop→start の再スポーン |
| `Ffmpeg`(ns) | サブプロセスラッパー。`measure`(loudnorm JSON) / `transcode`(コーデック+タグ+SR/bit) / `probe`(ストリーム情報) / `decodeAudio`(f32パイプ) / `cutVideoCopy`(-c copy) / `readTags`(ffprobe)。ffmpeg/ffprobeはwinget/PATH等から自動探索しキャッシュ |

### データフロー
```
ファイル → AudioClip::load ─→ Doc(タブ) ─→ Player(音声クロック) ─→ スピーカー
              │                  │              │ position
              │ (動画なら)       │              ↓
              │ Ffmpeg::probe    │        updateVideo: pts<=位置 のフレームを
              └→ VideoReader ────┼──────→ GLテクスチャへアップロード → 映像ペイン
                                 │
   バックグラウンド: Ffmpeg::measure(loudnorm) / Spec::averageSpectrumDb
                                 │  (世代番号つき shared_ptr 経由で書き戻し)
                                 ↓
   書き出し: Doc.samples → WavIo(一時float WAV) → Ffmpeg::transcode → 出力ファイル
```

### スレッドモデル
| スレッド | 内容 |
|---|---|
| メイン(UI) | ImGui描画60fps。毎フレーム: `player.update()` → 測定/平均スペクトラムのポーリング → `updateVideo` |
| 音声コールバック | miniaudioが駆動。サンプル供給 + ゲイン/ループ/biquadチェーン（モニター差し替えのみmutex） |
| 測定スレッド | `Ffmpeg::measure` を実行し `MeasureState` に書き戻す(使い捨て・detach) |
| 平均スペクトラム | サンプルのコピーを受けてFFT集計、`SpecAvgState` に書き戻す(同上) |
| 映像読み取り | `VideoReader` 内。子プロセスのパイプからフレームを読みキューへ |

## ブランチ運用
- `main` — 安定版
- `Develop` — 開発中の統合先
- `feature/*` / `fix/*` — 機能追加 / 修正。完了後 `Develop` に `--no-ff` マージ

## ライセンス
未定（TODO）。
