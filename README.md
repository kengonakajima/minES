# minES
minimal echo supperssor

最小のスイッチオフ式エコーサプレッサをC++で実装する。

実装するサプレッサは、基本的に音声起動式のスイッチであり、
信号経路上の信号がエコーであると判定機構が判断したとき、
その帰還経路を切断する（あるいは非常に大きな伝送損失を導入する）。

これは教育用であるため、実用性は求めない。ただしエコー削除が実際に動く必要はある。

- libportaudio.a を用いる
- idea.cc の実装を参考にする
- echoback (echoback.cc) で、実際にportaudioを用いてエコーバックキャンセルの実体験ができる
- cancel_file (cancel_file.cc) で、テスト用の音声ファイルを用いてキャンセルのテストができる
- counting16kLong.wav が遠端信号
- playRecCounting16kLong.wav が近端信号
- macOSで動けばOK
- エラー処理は最小限に。

echoback.cc, cancel_file.cc は、別のレポジトリ(AECM用のもの)からもってきましたが、
エコー削除部分は、本レポジトリで実装する最小のスイッチオフ式のものを使うようにしてください。

## ビルド

macOS で [PortAudio](http://www.portaudio.com/) をローカルビルドし、
`pa/libportaudio.a` と `pa/include`（ヘッダ一式）を用意してください。
異なる場所に配置した場合は `PORTAUDIO_HOME=/path/to/portaudio` を指定します。

```
make              # echoback / cancel_file をビルド
PORTAUDIO_HOME=/opt/portaudio make echoback
make clean
```

`echoback` は PortAudio を利用したリアルタイム実験用、`cancel_file` は WAV ファイルを
入力としたオフライン検証用です。

## 実行オプション

両プログラム共通で以下のパラメータを指定できます（未指定時はデフォルト値）。

- `--atten-db <dB>` ミュート時ゲイン[dB]（初期値 `-80 dB` でほぼ完全ミュート）
- `--rho <val>` エコー判定の相関しきい値（初期値 `0.6`）
- `--ratio <val>` マイク/遠端パワー比の上限（初期値 `1.3`）
- `--hang <frames>` エコー判定後に抑圧を継続するフレーム数（初期値 `20` フレーム ≒200 ms）
- `--attack <0-1>` ゲイン減衰の追従係数（初期値 `1.0` で即時抑圧）
- `--release <0-1>` ゲイン回復の追従係数（初期値 `0.05` でゆっくり復帰）

`echoback` は各ブロック （10 ms） ごとに適用ゲインを標準エラー出力へ
`mute=XX.X% (gain=Y.YYY)` として表示します。`cancel_file` でも同様に処理ログが出力されます。
