@ECHO "https://libuv.org/"

git clone https://github.com/alanxz/rabbitmq-c.git rabbitmq-c\rabbitmq-c

curl -o openssl-1.0.X.zip https://raw.githubusercontent.com/KernelHelper/openssl/master/src/openssl.zip

@ECHO "如果openssl-1.0.X.zip下载完成，解压openssl-1.0.X.zip到当前目录openssl。目录结构为：openssl\lib"