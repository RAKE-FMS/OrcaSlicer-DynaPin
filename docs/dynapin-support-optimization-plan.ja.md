# DynaPinサポート生成最適化 実装計画

## 概要

OrcaSlicer本体を改造し、G-code後処理ではなくサポート生成前に選択ピン領域のサポート除外を適用する。プリンタごとにピン位置が変わるため、DynaPinの固定Configは`machine`プリセット単位で紐づけ、実体はプリンタプロファイルと同じ場所の別JSONとして管理する。

主経路は本体改造とする。OrcaSlicerに汎用のサポート生成介入プラグインAPIは見当たらず、通常サポートとツリーサポートの生成前制御には`PrintObject::generate_support_material()`配下へ入る必要がある。

## 主な変更

- プリンタ別DynaPin Configを追加する。
  - 例: `resources/profiles/<vendor>/dynapin/<machine-name>.json`
  - machine presetには`dynapin_config_path`だけを追加し、相対パスで参照する。
  - ユーザーmachine presetでも同じキーを使い、ユーザープロファイル配下のJSONを優先して読めるようにする。
- DynaPin Configの内容はPyDynaPinの構造をベースにする。
  - `grid`: `pull_origin`、`support_origin`、row/column counts、row pitch Y、col pitch Z。row/colは常に0始まり
  - `support_exclusion`: ピン中心からのY/X幅、対象Zまたはレイヤー範囲
  - `pull_gcode`: `x_hook`, `x_latch`, `x_front`, offsets, feed rates
- スライス時の操作設定はプロジェクトまたはオブジェクト側に置く。
  - `enable_dynapin_support_optimization`
  - 選択ピンリスト `(row, col)`
  - 使用するDynaPin Configは現在のmachine presetから自動解決する。
- サポート生成前に、選択ピンからレイヤー別2Dブロッカー領域を生成する。
  - 通常サポートは`SupportMaterial.cpp`の既存`SupportAnnotations::blockers_layers`へ合流させる。
  - ツリーサポートも既存blocker layers経路へ合流させる。
  - G-code行を削るのではなく、support contact/intermediate/interface生成前のポリゴン差分で不要領域を消す。
- 引き出しG-codeはスライス時に挿入する。
  - PyDynaPinの`generate_pull_lines()`相当をC++へ移植する。
  - 初期版は`pin_z`に最も近いレイヤー開始直後へ挿入する。
  - `; BEGIN_DYNAPIN_PULL row=... col=...` / `; END_DYNAPIN_PULL ...`で囲む。

## テスト計画

- Config解決:
  - machine presetの`dynapin_config_path`から正しいJSONを読む。
  - Config未設定、ファイルなし、不正JSONでは明確なエラーまたは機能無効化になる。
- ピン計算:
  - PyDynaPin互換の`row,col -> Y,Z`計算を検証する。
  - 重複ピン、不正ピンを検出する。
- サポート生成:
  - 通常サポートとツリーサポートで対象領域のsupport polygons/extrusionsが消える。
  - 既存のsupport blocker/support paintingと併用しても既存挙動を壊さない。
- G-code:
  - 選択ピン数と同数のpull blockが挿入される。
  - 挿入Zと動作座標がConfigどおりになる。

## 前提

- 初期版はFFFのみ対象。SLAサポートは対象外。
- Configの紐づけ単位はmachine preset単位。
- ピン選択UIは初期版では`row,col`リスト入力でよい。グリッドクリック選択は次フェーズ。
- DynaPin Configは3MFに埋め込まず、3MFには選択ピンと有効/無効状態を保存する。再現には同じmachine presetとConfigが必要。
