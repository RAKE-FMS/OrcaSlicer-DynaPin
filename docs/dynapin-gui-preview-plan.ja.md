# DynaPin GUIプレビュー 実装計画

## 概要

物理ピン向けに、プレビュー専用のDynaPin可視化を追加する。物理ピンの3Dモデルはユーザー側で通常の3Dモデルとして用意・配置する。OrcaSlicer側では、そのピンモデルを3Dビュー上で手動選択し、G-codeプレビュー時にDynaPinコメントと一致したタイミングで、選択ピンが引き出される様子を表示する。

この計画の対象はGUI選択とプレビュー表示のみとする。ピンモデル生成、ピン配列の自動配置、実際の出力G-code動作の変更は含めない。G-code内に既に存在するDynaPinコメントを読み取り、表示に使う。

## G-codeコメント仕様

- pull blockの開始:
  - `; BEGIN_DYNAPIN_PULL ROW=<row> COL=<col>`
- 実際にピンが引き出される1行:
  - `; DYNAPIN_PULL_MOVE`
- pull blockの終了:
  - `; END_DYNAPIN_PULL`
- `ROW` / `COL` は物理ピン配列上の位置を表す。下の角を `0,0` として扱う。
- ピンを動かして表示するのは `DYNAPIN_PULL_MOVE` が付いた1行のみとする。block内の他の移動はヘッドの準備・後処理移動として扱い、ピンモデルは動かさない。

## GUI挙動

- 3D/プレビューUIにDynaPin選択モードを追加する。
- 選択モード中に `dynapin_r<row>_c<col>` 形式の名前を持つ既存モデルをクリックすると、その物理ピンを選択する。
- 選択中のDynaPinはハイライト表示する。
- 名前がDynaPin命名規則に一致しないモデルは、DynaPin選択モードでは無視する。
- 最小限の操作を追加する。
  - DynaPin選択モードのON/OFF
  - 選択中DynaPinの解除
  - DynaPinプレビューオーバーレイの表示/非表示

## プレビュー挙動

- プレビュー読み込み時に `GCodeProcessorResult::filename` を行単位で読み、コメントからDynaPin pull eventを抽出する。
- `GCodeProcessorResult::lines_ends` と各 `MoveVertex::gcode_id` を使い、コメント行をプレビュー上のmove IDへ対応付ける。
- 有効なeventごとに以下を保持する。
  - `row`, `col`
  - begin行 / move範囲
  - pull move ID
  - end行 / move範囲
  - pull moveの開始位置と終了位置
- プレビュースライダーがpull moveより前にある場合、選択ピンは元のモデル位置に表示する。
- スライダーが `DYNAPIN_PULL_MOVE` 上にある間、pull moveの開始位置から終了位置へ線形補間してピンを動かす。
- pull move後は、プレビュー時系列の残り区間で選択ピンを引き出し後位置に保持する。
- 選択中ピンの `row,col` がeventの `ROW,COL` と一致しない場合、そのeventではピンを動かさない。

## 実装メモ

- 実装範囲はプレビュー経路に閉じ、主に `GCodeViewer` と `GUI_Preview` 周辺に置く。
- event解析と描画処理をスライダー処理へ直接混ぜず、小さなDynaPin preview stateオブジェクトに分離する。
- 内部データの候補:
  - `DynaPinEvent`: `row`, `col`, `begin_gcode_id`, `pull_gcode_id`, `end_gcode_id`, `start_pos`, `end_pos`
  - `DynaPinSelection`: 選択中の `row`, `col` と選択GL/model参照
  - `DynaPinPreviewState`: 現在のpreview moveから表示用ピン変換を解決する
- v1では選択モデルのメッシュ複製は行わない。選択モデルの一時的なプレビュー変換、または移動後位置を示す軽量オーバーレイで表現する。
- 不正または不完全なDynaPinコメントは致命エラーにせず、警告ログを出して無視する。

## テスト計画

- パーサーテスト:
  - `BEGIN_DYNAPIN_PULL ROW=2 COL=5` を解析できる。
  - `DYNAPIN_PULL_MOVE` はbegin/end block内にある場合だけ検出される。
  - 不完全なblock、ROW/COL欠落、pull moveなしのblockは無視される。
- preview stateテスト:
  - 選択中の `row,col` が一致する場合だけ、指定pull move中に移動する。
  - 選択中の `row,col` が一致しない場合は動かない。
  - pull move前、pull move中、pull move後の位置が安定して決定的である。
- 手動GUI確認:
  - `dynapin_r0_c0` という名前のモデルをクリックして選択できる。
  - 選択中ピンがハイライトされる。
  - preview sliderを動かすと、指定moveで選択ピンがビルドボリューム側面から引き出されるように見える。
  - 選択していないピンは動かない。
  - DynaPin overlay非表示時に、ハイライトと移動表示が隠れる。

## 前提

- 物理ピンモデルと配列配置は、この機能の外で用意済みとする。
- 選択対象の各ピンモデルには `dynapin_r<row>_c<col>` 形式の名前を付けられる。
- 初期実装では同時に選択できるDynaPinは1本とする。
- pull表示は、マークされたG-code moveに沿った直線補間として扱う。
- プロジェクト保存、ピン配列生成、row/col手入力、自動ピン割当はv1の対象外とする。
