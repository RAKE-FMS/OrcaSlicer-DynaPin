# DynaPin最小G-codeの作成手順

## 概要

`models/`内の3MFを埋め込み設定のままスライスし、生成されたG-codeからDynaPinのピン引き出しと、その後4レイヤー分だけを抜き出した最小G-codeを作成する手順です。

WindowsとmacOSで同じPythonコマンドを使用できます。

## 前提

- リポジトリのルートディレクトリで作業する。
- `uv`がインストールされている。
- 入力3MFは`models/`以下に置く。例えば`models/part/Bambu PLA Basic.3mf`。
- 各3MFに使用するフィラメント・プリンター・工程設定を保存しておく。
- 最小G-codeの抽出には`models/min/min.gcode`が必要。

## 1. 3MFを埋め込み設定でスライスする

ターミナルまたはPowerShellで、リポジトリのルートへ移動します。

```text
cd /path/to/OrcaSlicer
```

Windowsの場合は、例えば次のように移動します。

```powershell
cd C:\path\to\OrcaSlicer
```

次のコマンドを実行します。

```text
uv run python scripts/slice_models_by_filament.py
```

このスクリプトは`models/`以下の3MFを再帰的に検索し、各ファイルを1回だけスライスします。`old-models/`は対象外です。3MFと同じ相対パスで、リポジトリ直下の`outputs/`にG-codeとスライス統計JSONを作成します。スクリプト名には`by_filament`が残っていますが、フィラメントプロファイルは別途設定せず、3MFに保存された設定を使用します。

3MFでDynaPinサポートと配置最適化が有効な場合、CLIはGUIと同じ配置探索APIを同期実行します。改善候補が見つかった場合だけ実行中のモデルへ回転・Y移動を適用し、再検証後にG-codeを生成します。入力3MFは変更しません。成功時には各入力の`OK`の下に、例えば次の結果が表示されます。

```text
dynapin_placement improved plate=1 rotation_deg=90 delta_y=4 support_volume_mm3=123
```

`skipped`は対象が複数あるなど探索条件を満たさない場合、`unchanged`は有意な改善がなかった場合、`fallback`は候補を安全に適用できなかった場合を表します。どの場合も元配置で通常のスライスを続けます。

```text
models/part/Bambu PLA Basic.3mf → outputs/part/Bambu PLA Basic.gcode
models/part/Bambu ABS.3mf       → outputs/part/Bambu ABS.gcode
models/top.3mf                  → outputs/top.gcode
```

各G-codeと同じディレクトリには、印刷時間、フィラメント使用量、重量、コスト、造形種別ごとの内訳を含むsidecar JSONも生成されます。

```text
outputs/part/Bambu PLA Basic.gcode.slice_statistics.json
outputs/part/Bambu ABS.gcode.slice_statistics.json
outputs/top.gcode.slice_statistics.json
```

出力できるのは単一プレートの3MFです。OrcaSlicerが複数プレートのG-codeを生成した場合や統計JSONが欠落・破損している場合は、その入力を失敗として報告し、同名の既存G-codeと統計JSONは置き換えません。ほかの3MFの処理は続行します。

## 2. DynaPin用の最小G-codeを作成する

スライスが完了したら、同じリポジトリルートで次のコマンドを実行します。

```text
uv run python scripts/extract_dynapin_min_gcode.py
```

`outputs/`以下を再帰的に検索し、各G-codeと同じディレクトリに次のファイルを作成します。

```text
outputs/<モデル名>/min_<材料名>.gcode
```

例：

```text
outputs/part/min_Bambu PLA Basic.gcode
outputs/part/min_Bambu ABS.gcode
```

## 抽出内容

生成される`min_*.gcode`は、次の順番で構成されます。

1. `models/min/min.gcode`の内容をそのままコピー
2. DynaPinのあるレイヤー内の全ピン引き出し
3. そのレイヤーの次にある4レイヤー分のG-code

DynaPinのマーカーは次の形式です。

```gcode
; BEGIN_DYNAPIN_PULL ROW=5 COL=6
...
; END_DYNAPIN_PULL
```

同一レイヤーに複数のピン引き出しがある場合も、すべて元の順番で1回だけ転記します。複数の抽出範囲が重なる場合も、レイヤーを重複出力しません。

元G-codeの行順・改行形式は保持されます。マーカーが存在しないG-codeはスキップされます。

## OrcaSlicerのRelease版自動選択

スライススクリプトは、`--slicer`を指定しない場合、リポジトリ内のReleaseビルドを優先して探します。

macOSでは、次のようなパスが優先されます。

```text
build/**/src/Release/OrcaSlicer.app/Contents/MacOS/OrcaSlicer
```

Windowsでは、次のようなパスが優先されます。

```text
build/**/src/Release/OrcaSlicer.exe
```

複数のReleaseビルドがある場合は、実行ファイルの更新日時が新しいものを使用します。`build/arm64/OrcaSlicer/`のようなCMakeインストール先のコピーは、古いResourcesを含む可能性があるため自動選択の対象外です。

Releaseビルドを明示する場合は、次のように指定できます。

```text
uv run python scripts/slice_models_by_filament.py --slicer /path/to/Release/OrcaSlicer
```

自動選択された実行ファイルは、スクリプト開始時に`Using OrcaSlicer:`として表示されます。

## 再実行について

同じコマンドを再実行すると、既存の`min_*.gcode`を元G-codeとして扱わず、元のG-codeから最小G-codeを再生成します。生成先は安全に置き換えられます。

## 別のmodelsディレクトリを指定する場合

リポジトリ標準の`models/`以外を使用する場合は、次のように指定します。

```text
uv run python scripts/extract_dynapin_min_gcode.py --models-dir /path/to/models
```

この場合、抽出元は`/path/to/outputs/`、ベースG-codeは`/path/to/models/min/min.gcode`です。

ベースG-codeを変更する場合は`--base-file`を使用します。

```text
uv run python scripts/extract_dynapin_min_gcode.py --models-dir /path/to/models --base-file /path/to/min.gcode
```

## エラー時の確認

- `outputs/`がない場合：先にスライススクリプトを実行する。
- G-codeにDynaPinマーカーがない場合：`skipped`として処理される。
- `BEGIN`と`END`の組み合わせが不正な場合：エラーとして処理される。
- DynaPinレイヤーの後に4レイヤー未満しかない場合：エラーとして処理される。

処理結果は最後に次の形式で表示されます。

```text
summary: created=4, skipped=0, errors=0
```
