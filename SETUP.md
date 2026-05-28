# OrcaSlicer - セットアップ & ビルド手順

このドキュメントは開発者向けにローカルでの環境構築、ビルド、テスト、実行手順をまとめたものです。OSごとの補足と、リポジトリに含まれるビルドスクリプトも併記します。

## 前提条件
- Git でリポジトリを取得できること
- CMake (推奨: 3.13 以上)
- ビルドツール: Ninja または Xcode / Visual Studio（プラットフォーム依存）
- Python 3（ビルド/スクリプト実行で使用される場合あり）
- macOS では Homebrew があると依存パッケージが入れやすい

## リポジトリ取得
```bash
git clone https://github.com/<your-org>/OrcaSlicer.git
cd OrcaSlicer
```

## 依存関係の準備（macOS 例）
Homebrew がある場合の例:
```bash
brew update
brew install cmake ninja pkg-config python3 gettext git clang-format
```

## ./build_release_macos.sh でエラーが出た時
```bash
brew install autoreconf
brew install texinfo
```

リポジトリ付属の macOS 向けラッパースクリプトを使うと手順が簡単です:
```bash
./build_release_macos.sh
```
（スクリプトは環境に応じた追加手順を実行します。必要に応じて実行権限を付与してください。）

## 推奨ビルド（アウトオブソース）
汎用的な手順（Linux/macOS/Windows 共通）:
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target OrcaSlicer --config Release --parallel
```
- Windows (Visual Studio) では `--config Release` が必要な場合があります。
- ビルドターゲット名は環境によって `OrcaSlicer` となります。

## テストの実行
ビルド済みテストを走らせる手順:
```bash
cmake --build build --target tests
ctest --test-dir build --output-on-failure
```

## 実行方法（ローカル）
ビルド後に生成される実行ファイルの場所はプラットフォーム/設定によって異なります。一般的な実行例:

- macOS (アプリバンドルが生成される場合):
```bash
open build/Release/OrcaSlicer.app || open build/OrcaSlicer.app
```
- 実行ファイルを直接起動する例（ビルドディレクトリ直下に生成される場合）:
```bash
./build/OrcaSlicer
# または（Release サブディレクトリがある場合）
./build/Release/OrcaSlicer
```
- Windows の場合は Visual Studio の出力フォルダ（`build/Release/OrcaSlicer.exe` 等）を実行します。

必要に応じてコマンドライン引数でプロファイルやリソースのパスを指定できます（プロジェクトの仕様に従ってください）。

## プラットフォーム固有の補足
- macOS: `build_release_macos.sh` を利用することで Xcode/Ninja 用のラッパー処理が実行されます。
- Linux: `cmake -G Ninja` を使うと高速です。環境に応じて `deps` フォルダ内の依存をビルドする必要があります（`deps/` と `deps_src/` を参照）。
- Windows: Visual Studio ソリューションでのビルドか、Ninja + MSVC の組み合わせを使用します。

## 便利なスクリプトと参考ファイル
- ビルドスクリプト（リポジトリルート）: `build_release_macos.sh`, `build_linux.sh`, `build_release_vs2022.bat` など
- 参考ドキュメント: [AGENTS.md](AGENTS.md), [CLAUDE.md](CLAUDE.md)

## トラブルシューティング
- CMake エラー: キャッシュをクリアして再設定します。
```bash
rm -rf build CMakeCache.txt
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```
- ライブラリ依存で失敗する場合: 必要なネイティブ依存（TBB, OpenVDB など）をインストールするか、`deps` 内のビルド手順を実行してください。
- テスト失敗: `ctest --output-on-failure` の出力を確認し、該当テストのログ/入力データを調査してください。

## 開発時のヒント
- コード整形: リポジトリは `.clang-format` を使用しています。編集後は `clang-format -i <files>` を実行してください。
- 大きな変更を行う前に小さなビルド（例: `--target OrcaSlicer` のみ）で増分ビルドを活用してください。

---
この `setup.md` を起点に、必要なら CI 用の手順や Docker/Nix ベースの再現手順を追記できます。問題があればどの環境で何を実行したか（OS、CMake バージョン、出力ログ）を教えてください。
