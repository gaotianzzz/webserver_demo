SrcFiles=webserver_libevent.c pub.c
all:webserver_libevent
webserver_libevent:$(SrcFiles)
	gcc -o $@ $^ -levent

clean:
	rm -f webserver_libevent

.PHONY:clean all
