all: install

install:
	gcc shellfyre.c -o shellfyre
	./shellfyre

test:
	./shellfyre

clean:
	rm shellfyre
