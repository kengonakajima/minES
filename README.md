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

`echoback` で指定できるオプションは `--passthrough`（抑圧無効化）と `--input-delay-ms <ms>` のみです。サプレッサ内部のしきい値やゲインは固定値で動作します。

`echoback` は各ブロック （10 ms） ごとに適用ゲインと遅延推定を標準エラー出力へ
`mute=XX.X% (gain=Y.YYY *** , lag=NNN samples)` のように表示します（遅延が検出されない場合は `lag=--`）。星印はゲインの段階表示で、`    ` → `*   ` → `**  ` → `*** ` → `****` の順に大きくなります。`cancel_file` でも同様に処理ログが出力されます。

`echoback` の入力遅延バッファは既定で `0 ms`（遅延なし）であり、`--input-delay-ms` で任意に遅延を付与できます。

サプレッサの主なパラメータは以下の固定値です。
- エコー検出スコアしきい値 `ρ_thresh = 0.6`
- マイク/遠端パワー比上限 `ratio = 1.3`
- ミュート時ゲイン `atten = -80 dB`（線形値 `0.0001`）
- ハングオーバ継続 `20` ブロック（≒200 ms）
- ゲイン追従係数 `attack = 0.1`, `release = 0.01`
