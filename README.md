A toy implementation of the [LISP2][] [mark-compact][] garbage collection algorithm.

It contains two versions. `lisp2.c` is the simpler of the two and is well-documented. It implements the garbage collector using a single fixed-size heap. `lisp2-reallocate.c` extends that by growing and shrinking the heap as needed.

[lisp2]: http://en.wikipedia.org/wiki/Mark-compact_algorithm#LISP2_Algorithm
[mark-compact]: http://en.wikipedia.org/wiki/Mark-compact_algorithm