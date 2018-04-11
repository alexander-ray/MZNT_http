CC=g++ # Setting compiler
LDFLAGS= -lcurl -lpthread
CFLAGS= -Wall -Wextra -Werror# Specified by writeup
HEADERS= http-client.h

# Ensures make normally builds, doesn't clean unless specified
default: http-driver

#sds.o: sds.c
#	$(CC) -c $(CFLAGS) sds.c $(HEADERS) $(LDFLAGS)

http-client.o: http-client.c $(HEADERS)
	$(CC) -c $(CFLAGS) http-client.c $(HEADERS) $(LDFLAGS)

http-driver.o: http-driver.c $(HEADERS)
	$(CC) -c $(CFLAGS) http-driver.c $(HEADERS) $(LDFLAGS)

http-driver: http-client.o http-driver.o
	$(CC) -o http-driver $(CFLAGS) http-client.o http-driver.o $(LDFLAGS)

clean:
	rm -rf http-driver
	rm -rf http-driver.o
	rm -rf http-client.o
	rm -rf http-client.h.gch
	rm -rf http-client-helper.h.gch
	rm -rf sds.o
	rm -rf sds.h.gch
	rm -rf sdsalloc.h.gch
