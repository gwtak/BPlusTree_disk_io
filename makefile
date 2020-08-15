main.out:bplustree.o bplustree_demo.o
	gcc  *.o -o main.out
	
bplustree.o:bplustree.c
	gcc -c bplustree.c -o bplustree.o 
	
bplustree_demo.o:bplustree_demo.c
	gcc -c bplustree_demo.c -o bplustree_demo.o
	
.PHONY:clean
clean:
	rm -rf *.o 