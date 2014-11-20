.PHONY : clean

both : lisp2 lisp2-reallocate

lisp2 : lisp2.c
	$(CC) -ggdb -std=gnu99  lisp2.c -o lisp2

lisp2-reallocate : lisp2-reallocate.c
	$(CC) -ggdb -std=gnu99  lisp2-reallocate.c -o lisp2-reallocate

clean :
	rm -f lisp2 *~
	rm -f lisp2-reallocate *~

run : lisp2
	valgrind  --leak-check=yes lisp2
