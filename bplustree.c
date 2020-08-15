#include<stdio.h>
#include<stdlib.h>
#include<assert.h>
#include<string.h>
#include<fcntl.h>
#include<ctype.h>
#include<unistd.h>
#include<sys/types.h>
#include<sys/stat.h>

#include"bplustree.h"

/*
偏移量的枚举
INVALID_OFFSET非法偏移量
*/
enum {
        INVALID_OFFSET = 0xdeadbeef,
};

/*
是否为叶子节点的枚举
叶子节点
非叶子节点
*/
enum {
        BPLUS_TREE_LEAF,
        BPLUS_TREE_NON_LEAF = 1,
};

/*
兄弟节点的枚举
左兄弟
右兄弟
*/
enum {
        LEFT_SIBLING,
        RIGHT_SIBLING = 1,
};

/*
内存结构
 ---------------------------------------------------------------------------------------------------
|			|		|		|		|		|		|		|		|		|		|		|		|
|叶子节点	|  node | key	| key 	| key 	| key 	| key 	| data 	| data 	| data 	| data 	| data 	|
|			|		|		|		|		|		|		|		|		|		|		|		|
|---------------------------------------------------------------------------------------------------
|			|		|		|		|		|		|		|		|		|		|		|		|
|非叶子节点	|  node | key 	| key 	| key 	| key 	|  ptr  |  ptr  |  ptr  |  ptr  |	ptr	|	ptr	|
|			|		|		|		|		|		|		|		|		|		|		|		|
 ---------------------------------------------------------------------------------------------------
key和data的个数由_max_entries决定：_max_entries = (_block_size - sizeof(node)) / (sizeof(key_t) + sizeof(long));
一个节点的大小由_block_size决定，容量要包含1个node结构体和3个及以上的key，data
*/

/*16位数据宽度*/
#define ADDR_STR_WIDTH 16

/*B+树节点node末尾的偏移地址，即key的首地址*/
#define offset_ptr(node) ((char *) (node) + sizeof(*node))

/*返回B+树节点末尾地址，强制转换为key_t*，即key的指针*/
#define key(node) ((key_t *)offset_ptr(node))

/*返回B+树节点和key末尾地址，强制转换为long*，即data指针*/
#define data(node) ((long *)(offset_ptr(node) + _max_entries * sizeof(key_t)))

/*返回最后一个key的指针，用于非叶子节点的指向，即第一个ptr*/
#define sub(node) ((off_t *)(offset_ptr(node) + (_max_order - 1) * sizeof(key_t)))

/*
全局静态变量
_block_size--------------------每个节点的大小(容量要包含1个node和3个及以上的key，data)
_max_entries-------------------叶子节点内包含个数最大值
_max_order---------------------非叶子节点内最大关键字个数
*/
static int _block_size;
static int _max_entries;
static int _max_order;

/*
判断是否为叶子节点
*/
static inline int is_leaf(struct bplus_node *node)
{
        return node->type == BPLUS_TREE_LEAF;
}

/*
键值二分查找
*/
static int key_binary_search(struct bplus_node *node, key_t target)
{
        key_t *arr = key(node);
		/*叶子节点：len；非叶子节点：len-1;非叶子节点的key少一个，用于放ptr*/
        int len = is_leaf(node) ? node->children : node->children - 1;
        int low = -1;
        int high = len;

        while (low + 1 < high) {
                int mid = low + (high - low) / 2;
                if (target > arr[mid]) {
                        low = mid;
                } else {
                        high = mid;
                }
        }

        if (high >= len || arr[high] != target) {
                return -high - 1;
        } else {
                return high;
        }
}

/*
查找键值在父节点的第几位
*/
static inline int parent_key_index(struct bplus_node *parent, key_t key)
{
        int index = key_binary_search(parent, key);
        return index >= 0 ? index : -index - 2;
}

/*
占用缓存区，与cache_defer对应
占用内存，以供使用
缓存不足，assert(0)直接终止程序
*/
static inline struct bplus_node *cache_refer(struct bplus_tree *tree)
{
        int i;
        for (i = 0; i < MIN_CACHE_NUM; i++) {
                if (!tree->used[i]) {
                        tree->used[i] = 1;
                        char *buf = tree->caches + _block_size * i;
                        return (struct bplus_node *) buf;
                }
        }
        assert(0);
}

/*
释放缓冲区，与cache_refer对应
将used重置，能够存放接下来的数据
*/
static inline void cache_defer(struct bplus_tree *tree, struct bplus_node *node)
{
        char *buf = (char *) node;
        int i = (buf - tree->caches) / _block_size;
        tree->used[i] = 0;
}

/*
创建新的节点
*/
static struct bplus_node *node_new(struct bplus_tree *tree)
{
        struct bplus_node *node = cache_refer(tree);
        node->self = INVALID_OFFSET;
        node->parent = INVALID_OFFSET;
        node->prev = INVALID_OFFSET;
        node->next = INVALID_OFFSET;
        node->children = 0;
        return node;
}

/*
创建新的非叶子节点
*/
static inline struct bplus_node *non_leaf_new(struct bplus_tree *tree)
{
        struct bplus_node *node = node_new(tree);
        node->type = BPLUS_TREE_NON_LEAF;
        return node;
}

/*
创建新的叶子节点
*/
static inline struct bplus_node *leaf_new(struct bplus_tree *tree)
{
        struct bplus_node *node = node_new(tree);
        node->type = BPLUS_TREE_LEAF;
        return node;
}

/*
根据偏移量从.index获取节点的全部信息，加载到缓冲区
偏移量非法则返回NULL
*/
static struct bplus_node *node_fetch(struct bplus_tree *tree, off_t offset)
{
        if (offset == INVALID_OFFSET) {
                return NULL;
        }

        struct bplus_node *node = cache_refer(tree);
        int len = pread(tree->fd, node, _block_size, offset);
        assert(len == _block_size);
        return node;
}

/*
通过节点的偏移量从.index中获取节点的全部信息
*/
static struct bplus_node *node_seek(struct bplus_tree *tree, off_t offset)
{
		/*偏移量不合法*/
        if (offset == INVALID_OFFSET) {
                return NULL;
        }
		
		/*偏移量合法*/
        int i;
        for (i = 0; i < MIN_CACHE_NUM; i++) {
                if (!tree->used[i]) {
                        char *buf = tree->caches + _block_size * i;
                        int len = pread(tree->fd, buf, _block_size, offset);
                        assert(len == _block_size);
                        return (struct bplus_node *) buf;
                }
        }
        assert(0);
}

/*
B+树节点保存
将其保存到index
并将内存内的缓冲区释放
往tree->fd的文件描述符写入
node指向的节点信息和其后面跟随的节点内容
长度为_block_size
偏移量为node->self
*/
static inline void node_flush(struct bplus_tree *tree, struct bplus_node *node)
{
        if (node != NULL) {
                int len = pwrite(tree->fd, node, _block_size, node->self);
                assert(len == _block_size);
                cache_defer(tree, node);
        }
}

/*
节点加入到树，为新节点分配新的偏移量，即文件大小
判断链表是否为空，判断是否有空闲区块
空闲区块首地址保存在.boot
*/
static off_t new_node_append(struct bplus_tree *tree, struct bplus_node *node)
{
        /*.index无空闲区块*/
        if (list_empty(&tree->free_blocks)) {
                node->self = tree->file_size;
                tree->file_size += _block_size;
		/*.inedx有空闲区块*/
        } else {
                struct free_block *block;
                block = list_first_entry(&tree->free_blocks, struct free_block, link);
                list_del(&block->link);
                node->self = block->offset;
                free(block);
        }
        return node->self;
}

/*
从.index删除整个节点，多出一块空闲区块，添加到B+树信息结构体
struct bplus_tree *tree-------------------B+树信息结构体
struct bplus_node *node-------------------要被删除的节点
struct bplus_node *left-------------------左孩子
struct bplus_node *right------------------右孩子
*/
static void node_delete(struct bplus_tree *tree, struct bplus_node *node, struct bplus_node *left, struct bplus_node *right)
{
        if (left != NULL) {
				/*左右孩子均存在*/
                if (right != NULL) {
                        left->next = right->self;
                        right->prev = left->self;
                        node_flush(tree, right);
				/*左孩子存在，右孩子不存在*/
                } else {
                        left->next = INVALID_OFFSET;
                }
                node_flush(tree, left);
        } else {
				/*没有孩子节点*/
                if (right != NULL) {
                        right->prev = INVALID_OFFSET;
                        node_flush(tree, right);
                }
        }

        assert(node->self != INVALID_OFFSET);
        struct free_block *block = malloc(sizeof(*block));
        assert(block != NULL);
        /*空闲区块指向被删除节点在.index中的偏移量*/
        block->offset = node->self;
		/*添加空闲区块*/
        list_add_tail(&block->link, &tree->free_blocks);
        /*释放缓冲区*/
        cache_defer(tree, node);
}

/*
更新非叶子节点的指向
struct bplus_tree *tree----------------B+树信息结构体
struct bplus_node *parent--------------父节点
int index------------------------------插入位置
struct bplus_node *sub_node------------要插入的分支
*/
static inline void sub_node_update(struct bplus_tree *tree, struct bplus_node *parent,
                		   int index, struct bplus_node *sub_node)
{
        assert(sub_node->self != INVALID_OFFSET);
        sub(parent)[index] = sub_node->self;
        sub_node->parent = parent->self;
        node_flush(tree, sub_node);
}

/*
将分裂的非叶子节点的孩子重定向，并写入.index
struct bplus_tree *tree----------------B+树信息结构体
struct bplus_node *parent--------------分裂的新的非叶子节点
off_t sub_offset-----------------------偏移量，即指向子节点的指针
*/
static inline void sub_node_flush(struct bplus_tree *tree, struct bplus_node *parent, off_t sub_offset)
{
        struct bplus_node *sub_node = node_fetch(tree, sub_offset);
        assert(sub_node != NULL);
        sub_node->parent = parent->self;
        node_flush(tree, sub_node);
}

/*
B+树查找
*/
static long bplus_tree_search(struct bplus_tree *tree, key_t key)
{
        int ret = -1;
		/*返回根节点的结构体*/
        struct bplus_node *node = node_seek(tree, tree->root);
        while (node != NULL) {
                int i = key_binary_search(node, key);
				/*到达叶子节点*/
                if (is_leaf(node)) {
                        ret = i >= 0 ? data(node)[i] : -1;
                        break;
				/*未到达叶子节点，循环递归*/
                } else {
                        if (i >= 0) {
                                node = node_seek(tree, sub(node)[i + 1]);
                        } else {
                                i = -i - 1;
                                node = node_seek(tree, sub(node)[i]);
                        }
                }
        }

        return ret;
}

/*
左节点添加
设置左右兄弟叶子节点的指向，不存在就设置为非法
struct bplus_tree *tree------------B+树信息结构体
struct bplus_node *node------------B+树要分裂的节点
struct bplus_node *left------------B+树左边的新节点
*/
static void left_node_add(struct bplus_tree *tree, struct bplus_node *node, struct bplus_node *left)
{
        new_node_append(tree, left);

        struct bplus_node *prev = node_fetch(tree, node->prev);
        if (prev != NULL) {
                prev->next = left->self;
                left->prev = prev->self;
                node_flush(tree, prev);
        } else {
                left->prev = INVALID_OFFSET;
        }
        left->next = node->self;
        node->prev = left->self;
}

/*
右节点添加
设置左右兄弟叶子节点的指向，不存在就设置为非法
struct bplus_tree *tree------------B+树信息结构体
struct bplus_node *node------------B+树要分裂的节点
struct bplus_node *right-----------B+树右边的新节点
*/
static void right_node_add(struct bplus_tree *tree, struct bplus_node *node, struct bplus_node *right)
{
        new_node_append(tree, right);

        struct bplus_node *next = node_fetch(tree, node->next);
        if (next != NULL) {
                next->prev = right->self;
                right->next = next->self;
                node_flush(tree, next);
        } else {
                right->next = INVALID_OFFSET;
        }
        right->prev = node->self;
        node->next = right->self;
}

/*非叶子节点插入，声明*/
static key_t non_leaf_insert(struct bplus_tree *tree, struct bplus_node *node, struct bplus_node *l_ch, struct bplus_node *r_ch, key_t key);

/*
下一层节点满后分裂，建立新的父节点，添加键值
struct bplus_tree *tree-------------B+树信息结构体
struct bplus_node *l_ch-------------B+树左孩子节点
struct bplus_node *r_ch-------------B+树右孩子节点
key_t key---------------------------后继节点的键值
*/
static int parent_node_build(struct bplus_tree *tree, struct bplus_node *l_ch, struct bplus_node *r_ch, key_t key)
{
		/*左右节点均没有父节点*/
        if (l_ch->parent == INVALID_OFFSET && r_ch->parent == INVALID_OFFSET) {
                /*左右节点均没有父节点，建立新的父节点*/
                struct bplus_node *parent = non_leaf_new(tree);
                key(parent)[0] = key;
                sub(parent)[0] = l_ch->self;
                sub(parent)[1] = r_ch->self;
                parent->children = 2;
				
                /*写入新的父节点，升级B+树信息结构体内的root根节点*/
                tree->root = new_node_append(tree, parent);
                l_ch->parent = parent->self;
                r_ch->parent = parent->self;
                tree->level++;
				
                /*操作完成，将父节点和子节点记入index*/
                node_flush(tree, l_ch);
                node_flush(tree, r_ch);
                node_flush(tree, parent);
                return 0;
		/*右节点没有父节点*/
        } else if (r_ch->parent == INVALID_OFFSET) {
				/*node_fetch(tree, l_ch->parent):从.index文件获取*/
                return non_leaf_insert(tree, node_fetch(tree, l_ch->parent), l_ch, r_ch, key);
        /*左节点没有父节点*/
		} else {
				/*node_fetch(tree, r_ch->parent):从.index文件获取*/
                return non_leaf_insert(tree, node_fetch(tree, r_ch->parent), l_ch, r_ch, key);
        }
}

/*
非叶子节点的分裂插入
insert在spilit左边,insert<spilit
struct bplus_tree *tree------------------B+树信息结构体
struct bplus_node *node------------------原节点
struct bplus_node *left------------------新分裂的节点
struct bplus_node *l_ch------------------左孩子
struct bplus_node *r_ch------------------右孩子
key_t key--------------------------------键值
int insert-------------------------------插入位置
*/
static key_t non_leaf_split_left(struct bplus_tree *tree, struct bplus_node *node, struct bplus_node *left, struct bplus_node *l_ch, struct bplus_node *r_ch, key_t key, int insert)
{
        int i;
        key_t split_key;

        /*分裂边界spilit=(len+1)/2*/
        int split = (_max_order + 1) / 2;

        /*左节点添加到树*/
        left_node_add(tree, node, left);

        /*重新计算左右兄弟节点的孩子*/
        int pivot = insert;
        left->children = split;
        node->children = _max_order - split + 1;

        /*将原来的insert~spilit的key和data复制到分裂的左兄弟*/
        memmove(&key(left)[0], &key(node)[0], pivot * sizeof(key_t));
        memmove(&sub(left)[0], &sub(node)[0], pivot * sizeof(off_t));

        /*将原来的insert+1~end的key和data后移1位，方便插入*/
        memmove(&key(left)[pivot + 1], &key(node)[pivot], (split - pivot - 1) * sizeof(key_t));
        memmove(&sub(left)[pivot + 1], &sub(node)[pivot], (split - pivot - 1) * sizeof(off_t));

        /*将分裂的左节点的孩子重定向，写入.index*/
        for (i = 0; i < left->children; i++) {
                if (i != pivot && i != pivot + 1) {
                        sub_node_flush(tree, left, sub(left)[i]);
                }
        }

        /*插入新键和子节点，并找到拆分键*/
        key(left)[pivot] = key;
		/*
		插入的非叶子节点有左右两孩子
		判断他们在分裂边界的那一边
		pivot == split - 1：孩子节点在分裂边界的两边
		else：孩子节点均在分裂节点左边
		*/
        if (pivot == split - 1) {
                /*
				孩子节点在分裂边界的两边
				更新索引，l_ch放到新分裂的非叶子节点
				r_ch放到原非叶子节点
				*/
                sub_node_update(tree, left, pivot, l_ch);
                sub_node_update(tree, node, 0, r_ch);
                split_key = key;
        } else {
                /*
				两个新的子节点在分裂左节点
				更新索引，l_ch和r_ch均放到新分裂的非叶子节点
				*/
                sub_node_update(tree, left, pivot, l_ch);
                sub_node_update(tree, left, pivot + 1, r_ch);
                sub(node)[0] = sub(node)[split - 1];
                split_key = key(node)[split - 2];
        }

        /*将原节点分裂边界右边的key和ptr左移*/
        memmove(&key(node)[0], &key(node)[split - 1], (node->children - 1) * sizeof(key_t));
        memmove(&sub(node)[1], &sub(node)[split], (node->children - 1) * sizeof(off_t));

		/*返回前继节点，作为上一层键值*/
        return split_key;
}

/*
非叶子节点的分裂插入
insert与spilit重叠，insert==spilit
直接分裂，移动操作减少
struct bplus_tree *tree------------------B+树信息结构体
struct bplus_node *node------------------原节点
struct bplus_node *right-----------------新分裂的节点
struct bplus_node *l_ch------------------左孩子
struct bplus_node *r_ch------------------右孩子
key_t key--------------------------------键值
int insert-------------------------------插入位置
*/
static key_t non_leaf_split_right1(struct bplus_tree *tree, struct bplus_node *node, struct bplus_node *right, struct bplus_node *l_ch, struct bplus_node *r_ch, key_t key, int insert)
{
        int i;

        /*分裂边界spilit=(len+1)/2*/
        int split = (_max_order + 1) / 2;

        /*新分裂的节点添加到树*/
        right_node_add(tree, node, right);

        /*上一层的键值*/
        key_t split_key = key(node)[split - 1];

        /*重新计算孩子个数*/
        int pivot = 0;
        node->children = split;
        right->children = _max_order - split + 1;

        /*插入key和ptr*/
        key(right)[0] = key;
        sub_node_update(tree, right, pivot, l_ch);
        sub_node_update(tree, right, pivot + 1, r_ch);

         /*复制数据到新的分裂节点*/
        memmove(&key(right)[pivot + 1], &key(node)[split], (right->children - 2) * sizeof(key_t));
        memmove(&sub(right)[pivot + 2], &sub(node)[split + 1], (right->children - 2) * sizeof(off_t));

        /*重定向父子结点，写入.index*/
        for (i = pivot + 2; i < right->children; i++) {
                sub_node_flush(tree, right, sub(right)[i]);
        }

		/*返回上一层键值*/
        return split_key;
}

/*
非叶子节点的分裂插入
insert在spilit右边,insert>spilit
struct bplus_tree *tree------------------B+树信息结构体
struct bplus_node *node------------------原节点
struct bplus_node *right-----------------新分裂的节点
struct bplus_node *l_ch------------------左孩子
struct bplus_node *r_ch------------------右孩子
key_t key--------------------------------键值
int insert-------------------------------插入位置
*/
static key_t non_leaf_split_right2(struct bplus_tree *tree, struct bplus_node *node, struct bplus_node *right, struct bplus_node *l_ch, struct bplus_node *r_ch, key_t key, int insert)
{
        int i;

        /*分裂边界spilit=(len+1)/2*/
        int split = (_max_order + 1) / 2;

        /*右节点添加到树*/
        right_node_add(tree, node, right);

        /*上一层的键值*/
        key_t split_key = key(node)[split];

        /*重新计算孩子个数*/
        int pivot = insert - split - 1;
        node->children = split + 1;
        right->children = _max_order - split;

        /*复制数据到新的分裂节点*/
        memmove(&key(right)[0], &key(node)[split + 1], pivot * sizeof(key_t));
        memmove(&sub(right)[0], &sub(node)[split + 1], pivot * sizeof(off_t));

        /*插入key和ptr，更新索引*/
        key(right)[pivot] = key;
        sub_node_update(tree, right, pivot, l_ch);
        sub_node_update(tree, right, pivot + 1, r_ch);

        /*将原节点insert+1~end的数据移动到新分裂的非叶子节点*/
        memmove(&key(right)[pivot + 1], &key(node)[insert], (_max_order - insert - 1) * sizeof(key_t));
        memmove(&sub(right)[pivot + 2], &sub(node)[insert + 1], (_max_order - insert - 1) * sizeof(off_t));

        /*重定向父子结点，写入.index*/
        for (i = 0; i < right->children; i++) {
                if (i != pivot && i != pivot + 1) {
                        sub_node_flush(tree, right, sub(right)[i]);
                }
        }

		/*返回上一层键值*/
        return split_key;
}

/*
父节点未满时，非叶子节点的简单插入
struct bplus_tree *tree----------------B+树信息结构体
struct bplus_node *node----------------父节点
struct bplus_node *l_ch----------------左孩子
struct bplus_node *r_ch----------------右孩子
key_t key------------------------------键值
int insert-----------------------------插入位置
*/
static void non_leaf_simple_insert(struct bplus_tree *tree, struct bplus_node *node, struct bplus_node *l_ch, struct bplus_node *r_ch, key_t key, int insert)
{
		/*将insert处原来的值后移*/
        memmove(&key(node)[insert + 1], &key(node)[insert], (node->children - 1 - insert) * sizeof(key_t));
        memmove(&sub(node)[insert + 2], &sub(node)[insert + 1], (node->children - 1 - insert) * sizeof(off_t));
        
		/*在insert处插入键值，并更新索引*/
        key(node)[insert] = key;
        sub_node_update(tree, node, insert, l_ch);
        sub_node_update(tree, node, insert + 1, r_ch);
        node->children++;
}

/*
非叶子节点插入，定义
即生成新的父节点
struct bplus_tree *tree-------------B+树信息结构体
struct bplus_node *node-------------要接入的新的B+树兄弟叶子节点
struct bplus_node *l_ch-------------B+树左孩子节点
struct bplus_node *r_ch-------------B+树右孩子节点
key_t key
*/
static int non_leaf_insert(struct bplus_tree *tree, struct bplus_node *node, struct bplus_node *l_ch, struct bplus_node *r_ch, key_t key)
{
        /*键值二分查找*/
        int insert = key_binary_search(node, key);
        assert(insert < 0);
        insert = -insert - 1;

        /*父节点满，进行分裂*/
        if (node->children == _max_order) {
                key_t split_key;
                /*分裂边界spilit=(len+1)/2*/
                int split = (node->children + 1) / 2;
				/*生成一个新的分裂的非叶子节点*/
                struct bplus_node *sibling = non_leaf_new(tree);
                if (insert < split) {
                        split_key = non_leaf_split_left(tree, node, sibling, l_ch, r_ch, key, insert);
                } else if (insert == split) {
                        split_key = non_leaf_split_right1(tree, node, sibling, l_ch, r_ch, key, insert);
                } else {
                        split_key = non_leaf_split_right2(tree, node, sibling, l_ch, r_ch, key, insert);
                }

                /*再次建立新的父节点*/
                if (insert < split) {
                        return parent_node_build(tree, sibling, node, split_key);
                } else {
                        return parent_node_build(tree, node, sibling, split_key);
                }
		/*父节点未满，进行简单的非叶子节点插入，并保存*/
        } else {
                non_leaf_simple_insert(tree, node, l_ch, r_ch, key, insert);
                node_flush(tree, node);
        }
        return 0;
}

/*
节点分裂插入，插入位置在分裂位置的左边
与leaf_split_right类似
struct bplus_tree *tree-----------B+树信息结构体
struct bplus_node *leaf-----------B+树叶子节点
struct bplus_node *left-----------新的叶子节点
key_t key-------------------------键值
long data-------------------------数据
int insert------------------------插入位置
*/
static key_t leaf_split_left(struct bplus_tree *tree, struct bplus_node *leaf, struct bplus_node *left, key_t key, long data, int insert)
{
        /*分裂边界split=(len+1)/2*/
        int split = (leaf->children + 1) / 2;

        /*节点分裂，设置左右兄弟叶子节点的指向*/
        left_node_add(tree, leaf, left);

		/*重新设置children的数值*/
        int pivot = insert;
        left->children = split;
        leaf->children = _max_entries - split + 1;

        /*
		将原叶子节点key[0]-key[insert]的数值复制到左边分裂出的新的叶子节点
		将原叶子节点data[0]-data[insert]的数值复制到左边分裂出的新的叶子节点
		*/
        memmove(&key(left)[0], &key(leaf)[0], pivot * sizeof(key_t));
        memmove(&data(left)[0], &data(leaf)[0], pivot * sizeof(long));

        /*在insert处插入新的key和data*/
        key(left)[pivot] = key;
        data(left)[pivot] = data;

        /*从原叶子节点将insert到split的值放到新的叶子节点insert+1处*/
        memmove(&key(left)[pivot + 1], &key(leaf)[pivot], (split - pivot - 1) * sizeof(key_t));
        memmove(&data(left)[pivot + 1], &data(leaf)[pivot], (split - pivot - 1) * sizeof(long));

        /*将原叶子节点insert+1~end的key和data复制到原叶子节点key[0]*/
        memmove(&key(leaf)[0], &key(leaf)[split - 1], leaf->children * sizeof(key_t));
        memmove(&data(leaf)[0], &data(leaf)[split - 1], leaf->children * sizeof(long));
		
		/*返回后继节点的key，即原叶子节点现在的key[0]*/
        return key(leaf)[0];
}

/*
节点分裂插入，插入位置在分裂位置的右边
与leaf_split_left类似
struct bplus_tree *tree-----------B+树信息结构体
struct bplus_node *leaf-----------B+树叶子节点
struct bplus_node *right----------新的叶子节点
key_t key-------------------------键值
long data-------------------------数据
int insert------------------------插入位置
*/
static key_t leaf_split_right(struct bplus_tree *tree, struct bplus_node *leaf, struct bplus_node *right, key_t key, long data, int insert)
{
        /*分裂边界split=(len+1)/2*/
        int split = (leaf->children + 1) / 2;

        /*节点分裂，设置左右兄弟叶子节点的指向*/
        right_node_add(tree, leaf, right);

        /*重新设置children的数值*/
        int pivot = insert - split;
        leaf->children = split;
        right->children = _max_entries - split + 1;

        /*将原叶子节点spilt~insert的key和data复制到右边分裂出的新的叶子节点*/
        memmove(&key(right)[0], &key(leaf)[split], pivot * sizeof(key_t));
        memmove(&data(right)[0], &data(leaf)[split], pivot * sizeof(long));

        /*在insert处插入新的key和data*/
        key(right)[pivot] = key;
        data(right)[pivot] = data;

        /*移动剩余的数据*/
        memmove(&key(right)[pivot + 1], &key(leaf)[insert], (_max_entries - insert) * sizeof(key_t));
        memmove(&data(right)[pivot + 1], &data(leaf)[insert], (_max_entries - insert) * sizeof(long));

		/*返回后继节点的key，即分裂的叶子节点的key[0]*/
        return key(right)[0];
}

/*
叶子节点在未满时的简单插入
struct bplus_tree *tree--------------------B+树信息结构体
struct bplus_node *leaf--------------------B+树节点结构体，要插入的位置的上一个节点
key_t key----------------------------------键值
long data----------------------------------数据
int intsert--------------------------------要插入的节点位序
两个memmove是将在insert之前的数据往后存放，使得数据能够插入
*/
static void leaf_simple_insert(struct bplus_tree *tree, struct bplus_node *leaf, key_t key, long data, int insert)
{
        memmove(&key(leaf)[insert + 1], &key(leaf)[insert], (leaf->children - insert) * sizeof(key_t));
        memmove(&data(leaf)[insert + 1], &data(leaf)[insert], (leaf->children - insert) * sizeof(long));
        key(leaf)[insert] = key;
        data(leaf)[insert] = data;
        leaf->children++;
}

/*
插入叶子节点
struct bplus_tree *tree--------------------B+树信息结构体
struct bplus_node *leaf--------------------B+树节点结构体，要插入的位置的上一个节点
key_t key----------------------------------键值
long data----------------------------------数据
*/
static int leaf_insert(struct bplus_tree *tree, struct bplus_node *leaf, key_t key, long data)
{
        /*键值二分查找*/
        int insert = key_binary_search(leaf, key);
		/*已存在键值*/
        if (insert >= 0) {
                return -1;
        }
        insert = -insert - 1;

        /*从空闲节点缓存中获取*/
        int i = ((char *) leaf - tree->caches) / _block_size;
        tree->used[i] = 1;

        /*叶子节点满*/
        if (leaf->children == _max_entries) {
                key_t split_key;
				
                /*节点分裂边界split=(len+1)/2*/
                int split = (_max_entries + 1) / 2;
                struct bplus_node *sibling = leaf_new(tree);

                /*
				由插入位置决定的兄弟叶复制
				insert < split：插入位置在分裂位置的左边
				insert >= split：插入位置在分裂位置的右边
				返回后继节点，以放入父节点作为键值
				*/
                if (insert < split) {
                        split_key = leaf_split_left(tree, leaf, sibling, key, data, insert);
                } else {
                        split_key = leaf_split_right(tree, leaf, sibling, key, data, insert);
                }

                /*建立新的父节点*/
                if (insert < split) {
                        return parent_node_build(tree, sibling, leaf, split_key);
                } else {
                        return parent_node_build(tree, leaf, sibling, split_key);
                }
		/*叶子节点未满*/
        } else {
                leaf_simple_insert(tree, leaf, key, data, insert);
                node_flush(tree, leaf);
        }

        return 0;
}

/*
插入节点
*/
static int bplus_tree_insert(struct bplus_tree *tree, key_t key, long data)
{
        struct bplus_node *node = node_seek(tree, tree->root);
        while (node != NULL) {
				/*到达叶子节点*/
                if (is_leaf(node)) {
                        return leaf_insert(tree, node, key, data);
				/*还未到达叶子节点，继续循环递归查找*/
                } else {
                        int i = key_binary_search(node, key);
                        if (i >= 0) {
                                node = node_seek(tree, sub(node)[i + 1]);
                        } else {
                                i = -i - 1;
                                node = node_seek(tree, sub(node)[i]);
                        }
                }
        }

        /*
		创建新的叶子节点
		在B+树后面跟随赋值key和data
		添加key：key(root)[0] = key;
		添加data：data(root)[0] = data;
		插入树：tree->root = new_node_append(tree, root);
		刷新缓冲区：node_flush(tree, root);
		*/
        struct bplus_node *root = leaf_new(tree);
        key(root)[0] = key;
        data(root)[0] = data;
        root->children = 1;
        tree->root = new_node_append(tree, root);
        tree->level = 1;
        node_flush(tree, root);
        return 0;
}

/*
struct bplus_node *l_sib------------------左兄弟
struct bplus_node *r_sib------------------右兄弟
struct bplus_node *parent-----------------父节点
int i-------------------------------------键值在父节点中的位置
*/
static inline int sibling_select(struct bplus_node *l_sib, struct bplus_node *r_sib, struct bplus_node *parent, int i)
{
        if (i == -1) {
                /*没有左兄弟，选择右兄弟合并*/
                return RIGHT_SIBLING;
        } else if (i == parent->children - 2) {
                /*没有右兄弟，选择左兄弟*/
                return LEFT_SIBLING;
        } else {
                /*有左右兄弟，选择孩子更多的节点*/
                return l_sib->children >= r_sib->children ? LEFT_SIBLING : RIGHT_SIBLING;
        }
}

/*
非叶子节点从左兄弟拿一个值
*/
static void non_leaf_shift_from_left(struct bplus_tree *tree, struct bplus_node *node, struct bplus_node *left, struct bplus_node *parent, int parent_key_index, int remove)
{
        memmove(&key(node)[1], &key(node)[0], remove * sizeof(key_t));
        memmove(&sub(node)[1], &sub(node)[0], (remove + 1) * sizeof(off_t));

        key(node)[0] = key(parent)[parent_key_index];
        key(parent)[parent_key_index] = key(left)[left->children - 2];

        sub(node)[0] = sub(left)[left->children - 1];
        sub_node_flush(tree, node, sub(node)[0]);

        left->children--;
}

/*
非叶子节点合并到左兄弟
*/
static void non_leaf_merge_into_left(struct bplus_tree *tree, struct bplus_node *node, struct bplus_node *left, struct bplus_node *parent, int parent_key_index, int remove)
{
        /*键值下移*/
        key(left)[left->children - 1] = key(parent)[parent_key_index];

        memmove(&key(left)[left->children], &key(node)[0], remove * sizeof(key_t));
        memmove(&sub(left)[left->children], &sub(node)[0], (remove + 1) * sizeof(off_t));

        memmove(&key(left)[left->children + remove], &key(node)[remove + 1], (node->children - remove - 2) * sizeof(key_t));
        memmove(&sub(left)[left->children + remove + 1], &sub(node)[remove + 2], (node->children - remove - 2) * sizeof(off_t));

        int i, j;
        for (i = left->children, j = 0; j < node->children - 1; i++, j++) {
                sub_node_flush(tree, left, sub(left)[i]);
        }

        left->children += node->children - 1;
}

/*
非叶子节点从右兄弟拿一个值
*/
static void non_leaf_shift_from_right(struct bplus_tree *tree, struct bplus_node *node, struct bplus_node *right, struct bplus_node *parent, int parent_key_index)
{
        key(node)[node->children - 1] = key(parent)[parent_key_index];
        key(parent)[parent_key_index] = key(right)[0];

        sub(node)[node->children] = sub(right)[0];
        sub_node_flush(tree, node, sub(node)[node->children]);
        node->children++;

        memmove(&key(right)[0], &key(right)[1], (right->children - 2) * sizeof(key_t));
        memmove(&sub(right)[0], &sub(right)[1], (right->children - 1) * sizeof(off_t));

        right->children--;
}

/*
非叶子节点合并到右兄弟
*/
static void non_leaf_merge_from_right(struct bplus_tree *tree, struct bplus_node *node, struct bplus_node *right, struct bplus_node *parent, int parent_key_index)
{
        key(node)[node->children - 1] = key(parent)[parent_key_index];
        node->children++;

        memmove(&key(node)[node->children - 1], &key(right)[0], (right->children - 1) * sizeof(key_t));
        memmove(&sub(node)[node->children - 1], &sub(right)[0], right->children * sizeof(off_t));

        int i, j;
        for (i = node->children - 1, j = 0; j < right->children; i++, j++) {
                sub_node_flush(tree, node, sub(node)[i]);
        }

        node->children += right->children - 1;
}

/*
非叶子节点的简单删除
*/
static inline void non_leaf_simple_remove(struct bplus_tree *tree, struct bplus_node *node, int remove)
{
        assert(node->children >= 2);
        memmove(&key(node)[remove], &key(node)[remove + 1], (node->children - remove - 2) * sizeof(key_t));
        memmove(&sub(node)[remove + 1], &sub(node)[remove + 2], (node->children - remove - 2) * sizeof(off_t));
        node->children--;
}

/*
非叶子节点的删除操作
叶子节点删除操作后，更新非叶子节点，非叶子节点的键值也可能被删除，非叶子节点也可能合并
struct bplus_tree *tree---------------------B+树信息结构体
struct bplus_node *node---------------------要执行删除操作的节点
int remove----------------------------------要删除的键值位置
*/
static void non_leaf_remove(struct bplus_tree *tree, struct bplus_node *node, int remove)
{
		/*不存在父节点，要执行删除操作的节点是根节点*/
        if (node->parent == INVALID_OFFSET) {
                /*只有两个键值*/
                if (node->children == 2) {
                        /*用第一个子节点替换旧根节点*/
                        struct bplus_node *root = node_fetch(tree, sub(node)[0]);
                        root->parent = INVALID_OFFSET;
                        tree->root = root->self;
                        tree->level--;
                        node_delete(tree, node, NULL, NULL);
                        node_flush(tree, root);
				/*键值大于2，将remove后的数据前移*/
                } else {
                        non_leaf_simple_remove(tree, node, remove);
                        node_flush(tree, node);
                }
		/*存在父节点，且非叶子节点内含数据小于一半，也要进行合并操作*/
        } else if (node->children <= (_max_order + 1) / 2) {
                struct bplus_node *l_sib = node_fetch(tree, node->prev);
                struct bplus_node *r_sib = node_fetch(tree, node->next);
                struct bplus_node *parent = node_fetch(tree, node->parent);

                int i = parent_key_index(parent, key(node)[0]);

                /*选择左兄弟合并*/
                if (sibling_select(l_sib, r_sib, parent, i)  == LEFT_SIBLING) {
						/*左兄弟节点内数据过半，无法合并，就拿一个数据过来*/
                        if (l_sib->children > (_max_order + 1) / 2) {
								/*左兄弟数据未过半，两两合并*/
                                non_leaf_shift_from_left(tree, node, l_sib, parent, i, remove);
                                node_flush(tree, node);
                                node_flush(tree, l_sib);
                                node_flush(tree, r_sib);
                                node_flush(tree, parent);
						/*左兄弟数据未过半，两两合并*/
                        } else {
                                non_leaf_merge_into_left(tree, node, l_sib, parent, i, remove);
                                node_delete(tree, node, l_sib, r_sib);
                                non_leaf_remove(tree, parent, i);
                        }
				/*选择右兄弟合并*/
                } else {
                        /*在与兄弟节点合并时首先删除，以防溢出*/
                        non_leaf_simple_remove(tree, node, remove);
						
						/*右兄弟节点内数据过半，无法合并，就拿一个数据过来*/
                        if (r_sib->children > (_max_order + 1) / 2) {
                                non_leaf_shift_from_right(tree, node, r_sib, parent, i + 1);
                                node_flush(tree, node);
                                node_flush(tree, l_sib);
                                node_flush(tree, r_sib);
                                node_flush(tree, parent);
						/*右兄弟数据未过半，两两合并*/
                        } else {
                                non_leaf_merge_from_right(tree, node, r_sib, parent, i + 1);
                                struct bplus_node *rr_sib = node_fetch(tree, r_sib->next);
                                node_delete(tree, r_sib, node, rr_sib);
                                node_flush(tree, l_sib);
                                non_leaf_remove(tree, parent, i + 1);
                        }
                }
		/*存在父节点，且非叶子节点内含数据大于一半，不需要进行合并操作*/
        } else {
                non_leaf_simple_remove(tree, node, remove);
                node_flush(tree, node);
        }
}

/*
从左兄弟拿一个数据，来保持平衡
struct bplus_tree *tree------------------B+树信息结构体
struct bplus_node *leaf------------------要执行删除操作和合并操作的叶子节点
struct bplus_node *left------------------左兄弟
struct bplus_node *parent----------------父节点
int parent_key_index---------------------leaf在父节点的位置
int remove-------------------------------删除的数据在leaf的位置
*/
static void leaf_shift_from_left(struct bplus_tree *tree, struct bplus_node *leaf, struct bplus_node *left, struct bplus_node *parent, int parent_key_index, int remove)
{
        /*腾出第一个位置*/
        memmove(&key(leaf)[1], &key(leaf)[0], remove * sizeof(key_t));
        memmove(&data(leaf)[1], &data(leaf)[0], remove * sizeof(off_t));

        /*从左兄弟拿一个数据*/
        key(leaf)[0] = key(left)[left->children - 1];
        data(leaf)[0] = data(left)[left->children - 1];
        left->children--;

        /*更新父节点的键值*/
        key(parent)[parent_key_index] = key(leaf)[0];
}

/*
左兄弟数据未过半，两两合并
*/
static void leaf_merge_into_left(struct bplus_tree *tree, struct bplus_node *leaf, struct bplus_node *left, int parent_key_index, int remove)
{
        /*将key和data从leaf复制到left，不包括被删除的数据*/
        memmove(&key(left)[left->children], &key(leaf)[0], remove * sizeof(key_t));
        memmove(&data(left)[left->children], &data(leaf)[0], remove * sizeof(off_t));
        memmove(&key(left)[left->children + remove], &key(leaf)[remove + 1], (leaf->children - remove - 1) * sizeof(key_t));
        memmove(&data(left)[left->children + remove], &data(leaf)[remove + 1], (leaf->children - remove - 1) * sizeof(off_t));
        left->children += leaf->children - 1;
}

/*
从左兄弟拿一个数据，来保持平衡
struct bplus_tree *tree------------------B+树信息结构体
struct bplus_node *leaf------------------要执行删除操作和合并操作的叶子节点
struct bplus_node *right-----------------右兄弟
struct bplus_node *parent----------------父节点
int parent_key_index---------------------leaf在父节点的位置
int remove-------------------------------删除的数据在leaf的位置
*/
static void leaf_shift_from_right(struct bplus_tree *tree, struct bplus_node *leaf, struct bplus_node *right, struct bplus_node *parent, int parent_key_index)
{
        /*leaf最后一个位置放right第一个数据*/
        key(leaf)[leaf->children] = key(right)[0];
        data(leaf)[leaf->children] = data(right)[0];
        leaf->children++;

        /*right左移*/
        memmove(&key(right)[0], &key(right)[1], (right->children - 1) * sizeof(key_t));
        memmove(&data(right)[0], &data(right)[1], (right->children - 1) * sizeof(off_t));
        right->children--;

        /*更新父节点的键值*/
        key(parent)[parent_key_index] = key(right)[0];
}

/*
左兄弟数据未过半，两两合并
*/
static inline void leaf_merge_from_right(struct bplus_tree *tree, struct bplus_node *leaf, struct bplus_node *right)
{
        memmove(&key(leaf)[leaf->children], &key(right)[0], right->children * sizeof(key_t));
        memmove(&data(leaf)[leaf->children], &data(right)[0], right->children * sizeof(off_t));
        leaf->children += right->children;
}

/*
叶子节点的简单删除操作
*/
static inline void leaf_simple_remove(struct bplus_tree *tree, struct bplus_node *leaf, int remove)
{
		/*key和data左移覆盖被删除的key和data*/
        memmove(&key(leaf)[remove], &key(leaf)[remove + 1], (leaf->children - remove - 1) * sizeof(key_t));
        memmove(&data(leaf)[remove], &data(leaf)[remove + 1], (leaf->children - remove - 1) * sizeof(off_t));
        leaf->children--;
}

/*
叶子节点的删除操作
struct bplus_tree *tree-----------------B+树信息结构体
struct bplus_node *leaf-----------------要执行删除操作的叶子节点
key_t key-------------------------------要删除的键值
*/
static int leaf_remove(struct bplus_tree *tree, struct bplus_node *leaf, key_t key)
{
        int remove = key_binary_search(leaf, key);
		/*要删除的键值不存在*/
        if (remove < 0) {
                return -1;
        }

        /*节点所在的缓存位置*/
        int i = ((char *) leaf - tree->caches) / _block_size;
        tree->used[i] = 1;
		
		/*父节点非法，即不存在父节点，要进行删除操作的叶子节点是根节点*/
        if (leaf->parent == INVALID_OFFSET) {
                /*节点内只有1个数据*/
                if (leaf->children == 1) {
                        /* delete the only last node */
                        assert(key == key(leaf)[0]);
                        tree->root = INVALID_OFFSET;
                        tree->level = 0;
						/*删除节点*/
                        node_delete(tree, leaf, NULL, NULL);
				/*节点内有多个数据*/
                } else {
                        leaf_simple_remove(tree, leaf, remove);
                        node_flush(tree, leaf);
                }
		/*有父节点，删除后节点内数据过少，要进行合并操作*/
        } else if (leaf->children <= (_max_entries + 1) / 2) {
                struct bplus_node *l_sib = node_fetch(tree, leaf->prev);
                struct bplus_node *r_sib = node_fetch(tree, leaf->next);
                struct bplus_node *parent = node_fetch(tree, leaf->parent);

                i = parent_key_index(parent, key(leaf)[0]);

                /*选择左兄弟合并*/
                if (sibling_select(l_sib, r_sib, parent, i) == LEFT_SIBLING) {
						/*左兄弟节点内数据过半，无法合并，就拿一个数据过来*/
                        if (l_sib->children > (_max_entries + 1) / 2) {
                                leaf_shift_from_left(tree, leaf, l_sib, parent, i, remove);
                                node_flush(tree, leaf);
                                node_flush(tree, l_sib);
                                node_flush(tree, r_sib);
                                node_flush(tree, parent);
						/*左兄弟数据未过半，合并*/
                        } else {
                                leaf_merge_into_left(tree, leaf, l_sib, i, remove);
                                /*删除无意义的leaf*/
                                node_delete(tree, leaf, l_sib, r_sib);
                                /*更新父节点*/
                                non_leaf_remove(tree, parent, i);
                        }
				/*选择右兄弟合并*/
                } else {
                        leaf_simple_remove(tree, leaf, remove);
						
						/*右兄弟节点内数据过半，无法合并，就拿一个数据过来*/
                        if (r_sib->children > (_max_entries + 1) / 2) {
                                leaf_shift_from_right(tree, leaf, r_sib, parent, i + 1);
                                /* flush leaves */
                                node_flush(tree, leaf);
                                node_flush(tree, l_sib);
                                node_flush(tree, r_sib);
                                node_flush(tree, parent);
						/*右兄弟数据未过半，合并*/
                        } else {
                                leaf_merge_from_right(tree, leaf, r_sib);
                                /*删除无意义的leaf*/
                                struct bplus_node *rr_sib = node_fetch(tree, r_sib->next);
                                node_delete(tree, r_sib, leaf, rr_sib);
                                node_flush(tree, l_sib);
                                /*更新父节点*/
                                non_leaf_remove(tree, parent, i + 1);
                        }
                }
		/*有父节点，但删除后，节点内数据大于一半，不进行合并*/
        } else {
                leaf_simple_remove(tree, leaf, remove);
                node_flush(tree, leaf);
        }

        return 0;
}

/*
删除节点
*/
static int bplus_tree_delete(struct bplus_tree *tree, key_t key)
{
        struct bplus_node *node = node_seek(tree, tree->root);
        while (node != NULL) {
				/*叶子节点，直接进行删除操作*/
                if (is_leaf(node)) {
                        return leaf_remove(tree, node, key);
				/*非叶子节点，继续循环递归查找*/
                } else {
                        int i = key_binary_search(node, key);
                        if (i >= 0) {
                                node = node_seek(tree, sub(node)[i + 1]);
                        } else {
                                i = -i - 1;
                                node = node_seek(tree, sub(node)[i]);
                        }
                }
        }
        return -1;
}

/*
查找结点的入口
*/
long bplus_tree_get(struct bplus_tree *tree, key_t key)
{
        return bplus_tree_search(tree, key);
}

/*
处理节点入口
插入节点
删除节点
*/
int bplus_tree_put(struct bplus_tree *tree, key_t key, long data)
{
        if (data) {
                return bplus_tree_insert(tree, key, data);
        } else {
                return bplus_tree_delete(tree, key);
        }
}

/*
获取范围
*/
long bplus_tree_get_range(struct bplus_tree *tree, key_t key1, key_t key2)
{
        long start = -1;
        key_t min = key1 <= key2 ? key1 : key2;
        key_t max = min == key1 ? key2 : key1;

        struct bplus_node *node = node_seek(tree, tree->root);
        while (node != NULL) {
                int i = key_binary_search(node, min);
                if (is_leaf(node)) {
                        if (i < 0) {
                                i = -i - 1;
                                if (i >= node->children) {
                                        node = node_seek(tree, node->next);
                                }
                        }
                        while (node != NULL && key(node)[i] <= max) {
                                start = data(node)[i];
                                if (++i >= node->children) {
                                        node = node_seek(tree, node->next);
                                        i = 0;
                                }
                        }
                        break;
                } else {
                        if (i >= 0) {
                                node = node_seek(tree, sub(node)[i + 1]);
                        } else  {
                                i = -i - 1;
                                node = node_seek(tree, sub(node)[i]);
                        }
                }
        }

        return start;
}

/*
打开B+树
返回fd
*/
int bplus_open(char *filename)
{
        return open(filename, O_CREAT | O_RDWR, 0644);
}

/*
关闭B+树
*/
void bplus_close(int fd)
{
        close(fd);
}

/*
字符串转16进制
*/
static off_t str_to_hex(char *c, int len)
{
        off_t offset = 0;
        while (len-- > 0) {
                if (isdigit(*c)) {
                        offset = offset * 16 + *c - '0';
                } else if (isxdigit(*c)) {
                        if (islower(*c)) {
                                offset = offset * 16 + *c - 'a' + 10;
                        } else {
                                offset = offset * 16 + *c - 'A' + 10;
                        }
                }
                c++;
        }
        return offset;
}

/*
16进制转字符串
*/
static inline void hex_to_str(off_t offset, char *buf, int len)
{
        const static char *hex = "0123456789ABCDEF";
        while (len-- > 0) {
                buf[len] = hex[offset & 0xf];
                offset >>= 4;
        }
}

/*
加载文件数据，每16位记录一个信息
如果读取到数据，即len>0，返回数据
如果没有读到数据，即len<=0，返回INVALID_OFFSET
*/
static inline off_t offset_load(int fd)
{
        char buf[ADDR_STR_WIDTH];
        ssize_t len = read(fd, buf, sizeof(buf));
        return len > 0 ? str_to_hex(buf, sizeof(buf)) : INVALID_OFFSET;
}

/*
存储B+相关数据
*/
static inline ssize_t offset_store(int fd, off_t offset)
{
        char buf[ADDR_STR_WIDTH];
        hex_to_str(offset, buf, sizeof(buf));
        return write(fd, buf, sizeof(buf));
}

/*
B+树初始化
char *filename----------文件名
int block_size----------文件大小
返回--------------------B+树头节点结构体指针
*/
struct bplus_tree *bplus_tree_init(char *filename, int block_size)
{
        int i;
        struct bplus_node node;
		
		/*文件名过长*/
        if (strlen(filename) >= 1024) {
                fprintf(stderr, "Index file name too long!\n");
                return NULL;
        }
		
		/*文件大小不是2的平方*/
        if ((block_size & (block_size - 1)) != 0) {
                fprintf(stderr, "Block size must be pow of 2!\n");
                return NULL;
        }
		
		/*文件容量太小*/
        if (block_size < (int) sizeof(node)) {
                fprintf(stderr, "block size is too small for one node!\n");
                return NULL;
        }

        _block_size = block_size;
        _max_order = (block_size - sizeof(node)) / (sizeof(key_t) + sizeof(off_t));
        _max_entries = (block_size - sizeof(node)) / (sizeof(key_t) + sizeof(long));
        
		/*文件容量太小*/
		if (_max_order <= 2) {
                fprintf(stderr, "block size is too small for one node!\n");
                return NULL;
        }
		
		/*为B+树信息节点分配内存*/
        struct bplus_tree *tree = calloc(1, sizeof(*tree));
        assert(tree != NULL);
        list_init(&tree->free_blocks);
        strcpy(tree->filename, filename);

        /*
		加载boot文件，可读可写
		tree->filename变为.boot
		首次运行不存在
		得到信息节点的信息，每16位记录一个信息
		root----------------B+树根节点在.index中的偏移量
		block_size----------分配的空间大小
		file_size-----------实际空间大小
		*/
        int fd = open(strcat(tree->filename, ".boot"), O_RDWR, 0644);
        if (fd >= 0) {
                tree->root = offset_load(fd);
                _block_size = offset_load(fd);
                tree->file_size = offset_load(fd);
				
                /*加载freeblocks空闲数据块*/
                while ((i = offset_load(fd)) != INVALID_OFFSET) {
                        struct free_block *block = malloc(sizeof(*block));
                        assert(block != NULL);
                        block->offset = i;
                        list_add(&block->link, &tree->free_blocks);
                }
                close(fd);
        } else {
                tree->root = INVALID_OFFSET;
                _block_size = block_size;
                tree->file_size = 0;
        }

        /*设置节点内关键字和数据最大个数*/
        _max_order = (_block_size - sizeof(node)) / (sizeof(key_t) + sizeof(off_t));
        _max_entries = (_block_size - sizeof(node)) / (sizeof(key_t) + sizeof(long));
        printf("config node order:%d and leaf entries:%d and _block_size:%d\n", _max_order, _max_entries,_block_size);

        /*申请和初始化节点缓存*/
        tree->caches = malloc(_block_size * MIN_CACHE_NUM);

        /*打开index文件，首次运行不存在，创建index文件=*/
        tree->fd = bplus_open(filename);
        assert(tree->fd >= 0);
        return tree;
}

/*
B+树的关闭操作
打开.boot文件
*/
void bplus_tree_deinit(struct bplus_tree *tree)
{
		/*向.boot写入B+树的3个配置数据*/
        int fd = open(tree->filename, O_CREAT | O_RDWR, 0644);
        assert(fd >= 0);
        assert(offset_store(fd, tree->root) == ADDR_STR_WIDTH);
        assert(offset_store(fd, _block_size) == ADDR_STR_WIDTH);
        assert(offset_store(fd, tree->file_size) == ADDR_STR_WIDTH);

        /*将空闲块存储在文件中以备将来重用*/
        struct list_head *pos, *n;
        list_for_each_safe(pos, n, &tree->free_blocks) {
                list_del(pos);
                struct free_block *block = list_entry(pos, struct free_block, link);
                assert(offset_store(fd, block->offset) == ADDR_STR_WIDTH);
                free(block);
        }

        bplus_close(tree->fd);
        free(tree->caches);
        free(tree);
}


#define MAX_LEVEL 10

struct node_backlog {
        /* Node backlogged */
        off_t offset;
        /* The index next to the backtrack point, must be >= 1 */
        int next_sub_idx;
};

/*
返回节点的children个数
*/
static inline int children(struct bplus_node *node)
{
        assert(!is_leaf(node));
        return node->children;
}

static void node_key_dump(struct bplus_node *node)
{
        int i;
        if (is_leaf(node)) {
                printf("leaf:");
                for (i = 0; i < node->children; i++) {
                        printf(" %d", key(node)[i]);
                }
        } else {
                printf("node:");
                for (i = 0; i < node->children - 1; i++) {
                        printf(" %d", key(node)[i]);
                }
        }
        printf("\n");
}

/*
绘图
*/
static void draw(struct bplus_tree *tree, struct bplus_node *node, struct node_backlog *stack, int level)
{
        int i;
        for (i = 1; i < level; i++) {
                if (i == level - 1) {
                        printf("%-8s", "+-------");
                } else {
                        if (stack[i - 1].offset != INVALID_OFFSET) {
                                printf("%-8s", "|");
                        } else {
                                printf("%-8s", " ");
                        }
                }
        }
        node_key_dump(node);
}

/*
绘图入口
*/
void bplus_tree_dump(struct bplus_tree *tree)
{
        int level = 0;
        struct bplus_node *node = node_seek(tree, tree->root);
        struct node_backlog *p_nbl = NULL;
        struct node_backlog nbl_stack[MAX_LEVEL];
        struct node_backlog *top = nbl_stack;

        for (; ;) {
                if (node != NULL) {
                        /*非零需要向后，零不需要*/
                        int sub_idx = p_nbl != NULL ? p_nbl->next_sub_idx : 0;
                        /*重置每个循环*/
                        p_nbl = NULL;

                        /*积压节点*/
                        if (is_leaf(node) || sub_idx + 1 >= children(node)) {
                                top->offset = INVALID_OFFSET;
                                top->next_sub_idx = 0;
                        } else {
                                top->offset = node->self;
                                top->next_sub_idx = sub_idx + 1;
                        }
                        top++;
                        level++;

                        /* Draw the node when first passed through */
                        if (sub_idx == 0) {
                                draw(tree, node, nbl_stack, level);
                        }

                        /* Move deep down */
                        node = is_leaf(node) ? NULL : node_seek(tree, sub(node)[sub_idx]);
                } else {
                        p_nbl = top == nbl_stack ? NULL : --top;
                        if (p_nbl == NULL) {
                                /* End of traversal */
                                break;
                        }
                        node = node_seek(tree, p_nbl->offset);
                        level--;
                }
        }
}
