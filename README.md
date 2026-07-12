# PlayerEditor

軽量な音声プレイヤー / 簡易エディタ（C++ / Dear ImGui）。
再生・波形表示・トリミング・ラウドネス測定 (EBU R128) と **-14 LUFS 正規化** を、即起動・軽量で行うことを目指しています。将来的に動画・タブ連結（クロスフェード）まで拡張予定。

A lightweight cross-platform audio player/editor built with C++ and Dear ImGui.

## 主な機能
- 音声の読み込み（WAV / MP3 / FLAC / M4A / OGG / AAC ほか、miniaudio）
- 波形表示 / 再生 / 一時停止 / 停止 / ループ再生 / 音量スライダー
- ドラッグで区間選択、クリックでシーク
- **タブ**で複数ファイルを同時に開く（再生は排他、切替で即再生）。複数同時ドロップで別タブ
- **ラウドネス測定**（Integrated LUFS / True Peak / LRA、ffmpeg の loudnorm）
- **-14 LUFS 正規化**（書き出し）／**再生時 -14 LUFS**（ファイルは無改変、デフォルトON）
- **書き出し**: WAV / MP3 / M4A / FLAC / OGG / AIF、範囲(全体/選択)、タグ埋め込み（入力ファイルのタグを自動流用）
- **加工**: ゲイン / フェードイン・アウト / リバース（メモリ上、選択範囲対応）
- **タブ連結**: 等パワークロスフェードで2タブを連結し新タブに
- **解析レポート出力**: LUFS/TP/LRA/ピーク/RMS などを JSON / TXT に保存
- ファイル関連付け・ドラッグ&ドロップ対応、単一インスタンス

## 依存
- ビルド時に **CMake の FetchContent** で自動取得：GLFW 3.4 / Dear ImGui (docking) / miniaudio / portable-file-dialogs
- 実行時に **ffmpeg**（測定・正規化・非WAVデコードに使用）。PATH か `winget install Gyan.FFmpeg`、または `ffmpeg/ffmpeg.exe` を隣に配置

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
- **Space**: 再生 / 停止
- ファイルをウィンドウにドラッグ&ドロップ（複数可）
- 波形をドラッグ: 区間選択 / クリック: シーク

## ライセンス
未定（TODO）。
