
output: jxserver.o
	clang jxserver.c -lpthread -o jxserver -fsanitize=address -g

jxserver.o: jxserver.c
	clang -c jxserver.c


clean:
	rm *.o output