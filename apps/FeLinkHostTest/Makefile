CROSS_COMPILE	?= arm-linux-gnueabihf-
TARGET			?= program

SFTP_USERNAME	:=	root
SFTP_HOST		:=	192.168.43.51
SFTP_DIR		:=	/root/projects/FeLinkHostTest

GCC			:=	$(CROSS_COMPILE)gcc
LD			:=	$(CROSS_COMPILE)ld

SRCS	:=	*.c							\
			FeLinkBase/*.c				\
			FeLinkBase/micro-ecc/*.c	\
			cJSON/*.c					\

INCS	:=	-I/home/fjj/projects/linux/t113/T113-IoT-Station/libs/openssl/out/include	\

LIBS	:=	-L/home/fjj/projects/linux/t113/T113-IoT-Station/libs/openssl/out/lib		\
			-lpthread	\
			-lssl		\
			-lcrypto	\

debug:
	$(GCC) -g -Wall $(INCS) $(SRCS) -o $(TARGET) $(LIBS)

release:
	$(GCC) -O2 $(INCS) $(SRCS) -o $(TARGET) $(LIBS)

upload:
	./sftp-download.sh $(SFTP_USERNAME) $(SFTP_HOST) $(TARGET) $(SFTP_DIR)

clean:
	rm $(TARGET)
