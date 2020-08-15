#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<time.h>
#include<math.h>

#include"bplustree.h"

/*
B+树设置结构体
char filename[1024]----文件名字
int block_size---------文件大小
*/
struct bplus_tree_config {
        char filename[1024];
        int block_size;
};

/*计时器结构体，定义在<time.h>*/
struct timespec t1,t2; 

/*
刷新stdin缓冲区
*/
static void stdin_flush(void)
{
        int c;
        while ((c = getchar()) != '\n' && c != EOF) {
                continue;
        }
}

/*
B+树设置
设置索引文件的地址和名字
设置索引文件大小
struct bplus_tree_config *config------设置结构体
返回----------------------------------正确返回0，错误返回-1
*/
static int bplus_tree_setting(struct bplus_tree_config *config)
{
        int i, size, ret = 0, again = 1;

        printf("\n-- B+tree setting...\n");
		/*设置.index文件名和路径*/
        while (again) {
                printf("Set data index file name (e.g. /tmp/data.index): ");
				/*获取一个字符，从stdin流中删除*/
                switch (i = getchar()) {
				/*获取失败*/
                case EOF:
                        printf("\n");
				/*'q'退出*/
                case 'q':
                        return -1;
				/*换行符，文件名默认*/
                case '\n':
                        strcpy(config->filename, "/tmp/data.index");
                        again = 0;
                        break;
				/*默认：用户输入文件名*/
                default:
						/*将getchar拿走的字符放回去*/
                        ungetc(i, stdin);
						/*输入文件名和路径*/
                        ret = fscanf(stdin, "%s", config->filename);
						/*出错，再来一遍循环*/
                        if (!ret || getchar() != '\n') {
								/*刷新缓冲区*/
                                stdin_flush();
                                again = 1;
                        } else {
                                again = 0;
                        }
                        break;
                }
        }

        again = 1;
		/*设置区块大小，区块存放node、key、data、ptr*/
        while (again) {
                printf("Set index file block size (bytes, power of 2, e.g. 256): ");
                switch (i = getchar()) {
                case EOF:
                        printf("\n");
                case 'q':
                        return -1;
				/*换行符，默认区块大小256*/
                case '\n':
                        config->block_size = 256;
                        again = 0;
                        break;
				/*输入区块大小*/
                default:
                        ungetc(i, stdin);
                        ret = fscanf(stdin, "%d", &size);
						/*出错，刷新缓冲区，再来一遍循环*/
                        if (!ret || getchar() != '\n') {
                                stdin_flush();
                                again = 1;
						/*等于小小于0，或大小不是2的倍数*/
                        } else if (size <= 0 || (size & (size - 1)) != 0) {
                		fprintf(stderr, "Block size must be positive and pow of 2!\n");
                                again = 1;
                        } else if (size <= 0 || (size & (size - 1)) != 0) {
                                again = 1;
						/*最小容量包括：B+树节点，3个及以上键值和偏移量*/
                        } else {
                                int order = (size - sizeof(struct bplus_node)) / (sizeof(key_t) + sizeof(off_t));
                                if (size < (int) sizeof(struct bplus_node) || order <= 2) {
                                        fprintf(stderr, "block size is too small for one node!\n");
                                        again = 1;
                                } else {
                                        config->block_size = size;
                                        again = 0;
                                }
                        }
                        break;
                }
        }

        return ret;
}

/*
执行选定的操作
插入
删除
查找
*/
static void _proc(struct bplus_tree *tree, char op, int n)
{
        switch (op) {
                case 'i':
                        bplus_tree_put(tree, n, n);
                        break;
                case 'r':
                        bplus_tree_put(tree, n, 0);
                        break;
                case 's':
                        printf("key:%d data_index:%ld\n", n, bplus_tree_get(tree, n));
                        break;
                default:
                        break;
        }       
}

/*
读取输入的数字，判断要执行的操作
*/
static int number_process(struct bplus_tree *tree, char op)
{
        int c, n = 0;
        int start = 0, end = 0;

        while ((c = getchar()) != EOF) {
				/*空格||tab||换行*/
                if (c == ' ' || c == '\t' || c == '\n') {
                        if (start != 0) {
                                if (n >= 0) {
                                        end = n;
                                } else {
                                        n = 0;
                                }
                        }
						
						/*范围操作*/
                        if (start != 0 && end != 0) {
								/*从小到大*/
                                if (start <= end) {
                                        for (n = start; n <= end; n++) {
                                                _proc(tree, op, n);
                                        }
								/*从大到小*/
                                } else {
                                        for (n = start; n >= end; n--) {
                                                _proc(tree, op, n);
                                        }
                                }
						/*单个数据操作*/
                        } else {
                                if (n != 0) {
                                        _proc(tree, op, n);
                                }
                        }

                        n = 0;
                        start = 0;
                        end = 0;

                        if (c == '\n') {
                                return 0;
                        } else {
                                continue;
                        }
                }
				
				/*得到单个数字*/
                if (c >= '0' && c <= '9') {
                        n = n * 10 + c - '0';
				/*得到范围*/
                } else if (c == '-' && n != 0) {
                        start = n;
                        n = 0;
				/*删除字母*/
                } else {
                        n = 0;
                        start = 0;
                        end = 0;
                        while ((c = getchar()) != ' ' && c != '\t' && c != '\n') {
                                continue;
                        }
                        ungetc(c, stdin);
                }
        }

        printf("\n");
        return -1;
}

/*
显示帮助文档
*/
static void command_tips(void)
{
        printf("i: Insert key. e.g. i 1 4-7 9\n");
        printf("r: Remove key. e.g. r 1-100\n");
        printf("s: Search by key. e.g. s 41-60\n");
        printf("d: Dump the tree structure.\n");
        printf("q: quit.\n");
}

/*
命令进程
q---------------------退出
h---------------------显示帮助文档
d---------------------绘图
i---------------------插入节点
r---------------------删除节点
s---------------------查找节点
\n--------------------提示输入下一次命令
*/
static void command_process(struct bplus_tree *tree)
{
        int c;
        printf("Please input command (Type 'h' for help): ");
        for (; ;) {
                switch (c = getchar()) {
                case EOF:
                        printf("\n");
                case 'q':
                        return;
                case 'h':
                        command_tips();
                        break;
                case 'd':
						/*计时器启动*/
						clock_gettime(CLOCK_MONOTONIC,&t1);
                        bplus_tree_dump(tree);
						/*计时器结束*/
						clock_gettime(CLOCK_MONOTONIC,&t2);
						/*输出耗时*/
                        printf("This operation takes time:%lf second\n",((t2.tv_sec-t1.tv_sec)*pow(10,9)+t2.tv_nsec-t1.tv_nsec)/pow(10,9));
                        break;
                case 'i':
                case 'r':
                case 's':
						/*计时器启动*/
						clock_gettime(CLOCK_MONOTONIC,&t1);
                        if (number_process(tree, c) < 0) {
                                return;
                        }
						/*计时器结束*/
						clock_gettime(CLOCK_MONOTONIC,&t2);
						/*输出耗时*/
						printf("This operation takes time:%lf second\n",((t2.tv_sec-t1.tv_sec)*pow(10,9)+t2.tv_nsec-t1.tv_nsec)/pow(10,9));
                case '\n':
                        printf("Please input command (Type 'h' for help): ");
                default:
                        break;
                }
        }
}

int main(void)
{
		/*声明B+树设置*/
        struct bplus_tree_config config;
		/*定义一个B+树信息结构体*/
        struct bplus_tree *tree = NULL;
        while (tree == NULL) {
				/*设置B+树*/
                if (bplus_tree_setting(&config) < 0) {
                        return 0;
                }
				/*
				初始化索引，将config的值赋值给tree
				首次运行创建.index文件
				再次运行会将.boot内保存的free_blocks赋值给tree
				*/
                tree = bplus_tree_init(config.filename, config.block_size);
        }
        command_process(tree);
        bplus_tree_deinit(tree);

        return 0;
}
