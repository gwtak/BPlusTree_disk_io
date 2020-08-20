# 基于B+树的磁盘索引

支持查找、插入、删除、可视化，支持范围操作



两个存储文件：index存储数据，boot存储B+树的信息



设置.index位置和命名，用于存放数据

```
Set data index file name (e.g. /tmp/data.index):
```

设置_block_size，用于存放B+树节点在内存中的缓存

```
Set index file block size (bytes, power of 2, e.g. 4096): 
```

输入命令

```
Please input command (Type 'h' for help):
```

帮助文档，插入、删除、查找、可视化、退出

```
i: Insert key. e.g. i 1 4-7 9
r: Remove key. e.g. r 1-100
s: Search by key. e.g. s 41-60
d: Dump the tree structure.
q: quit.
```

编译

```
make
make clean
```

运行

```
./bplustree_demo.out
```

库文件

```
lib
|----bplustree.h
|----bplustree.c
```

demo

```
bplustree_demo.c
```

