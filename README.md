# RDMAプログラミング入門

web記事[RDMAプログラミング入門](https://valinux.hatenablog.com/entry/20200806)の参照用コードです。

プログラムの内容については、web記事を参照ください。

## プログラムのビルド

以下のパッケージが必要です。
```
$ sudo apt update
$ sudo apt install gcc make
```
各ディレクトリの下で、`make`とやれば、バイナリができます。

## プログラムの使用
```
usage: rpp {-s|-c} [-d] server-ip-address
```
まずは、passive側を起動。(下記IPアドレスは一例)
```
$ rpp -s 192.168.0.11
```
次に、active側を起動。
```
$ rpp -c 192.168.0.11
```
どちら側もpassive側のipアドレスを指定します。-dを指定すると、librdmacmライブラリの使用が分かります。

rpp_h のpassive側は、起動し続けるので、終了するには、通信が行われていないときに、Ctrl-C で止めます。
