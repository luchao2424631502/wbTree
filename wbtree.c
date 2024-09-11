#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <sys/mman.h>

#include <libpmem.h>
#include <libpmemobj.h>

#include "lclog.h"

// node
#define NODE_SIZE	63
#define SLOT_SIZE	(NODE_SIZE + 1)
#define BITMAP_SIZE	(NODE_SIZE + 1)

// bits
#define BITS_PER_LONG	64
#define BITOP_WORD(nr)	((nr) / BITS_PER_LONG)
#define MIN_LIVE_ENTRIES (NODE_SIZE / 2)

static inline unsigned long ffz(unsigned long word)
{
	asm ("bsfq %1, %0":"=r"(word):"r"(~word):"cc");
	return word;
}

unsigned long find_next_zero_bit(unsigned long *addr, unsigned long size, unsigned long offset)
{
	unsigned long *p = addr + BITOP_WORD(offset);
	unsigned long result = offset & ~(BITS_PER_LONG - 1);
	unsigned long tmp;

	if (offset >= size)
		return size; // BITMAPSIZE

	size -= result;
	offset %= BITS_PER_LONG;
	if (offset) {
		tmp = *(p++);
		tmp |= ~0UL >> (BITS_PER_LONG - offset); // 偏移前的都补上
		if (size < BITS_PER_LONG)
			goto found_first;
		if (~tmp)
			goto found_middle;

		size -= BITS_PER_LONG;
		result += BITS_PER_LONG;
	}
found_first:
	tmp |= ~0UL << size;
	if (tmp == ~0UL)
		return result + size;
found_middle:
	return result + ffz(tmp);
}

struct entry {
	unsigned long key;
	void *ptr;
};

struct node {
	char slot[SLOT_SIZE];
	unsigned long bitmap; 

	struct entry entries[NODE_SIZE];
	struct node *leftmostPtr;

	struct node *parent;
	int isleaf;
	char dummy[48];
};

struct tree {
	struct node *root;
	// struct log_area *start_log;
};

typedef struct node node;
typedef struct tree tree;
typedef struct entry entry;

node *allocNode()
{
	node *n = (node *)malloc(sizeof(node));
	memset(n, 0, sizeof(node));

	n->bitmap = 1;
	n->isleaf = 1;
	return n;
}

tree *initTree()
{
	tree *t = (tree *)malloc(sizeof(tree));
	t->root = allocNode();
	return t;
}

/*
   返回的mid满足属性 key 属于 (entries[slot[mid - 1]].key, entries[slot[mid]].key] 区间
 */
int Search(node *curr, char *slot, unsigned long key)
{
	int low = 1, mid = 1;
	int high = slot[0];

	while (low <= high) {
		mid = (low + high) >> 1;
		if (curr->entries[slot[mid]].key > key) 
			high = mid - 1;
		else if (curr->entries[slot[mid]].key < key)
			low = mid + 1;
		else
			break;
	}

	if (low > mid)
		mid = low;

	return mid; 
}

node *find_leaf_node(struct node *curr, unsigned long key)
{
	int loc;

	if (curr->isleaf)
		return curr;

	loc = Search(curr, curr->slot, key);

	if (loc > curr->slot[0]) // 这里loc - 1是取最右节点
		return find_leaf_node(curr->entries[curr->slot[loc - 1]].ptr, key);
		// return find_leaf_node(curr->entries[slot[slot[0]]].ptr, key); // slot[slot[0]]等价于slot[loc-1]
	else if (curr->entries[curr->slot[loc]].key <=  key) // 应该只有 == 情况
		return find_leaf_node(curr->entries[curr->slot[loc]].ptr, key);
	else if (loc == 1) // 最左情况
		return find_leaf_node(curr->leftmostPtr, key);
	else  // 这里loc - 1是取一个偏移位的值
		return find_leaf_node(curr->entries[curr->slot[loc - 1]].ptr, key); 
}

/* return: entries[]中插入的位置 */
int Append(node *curr, unsigned long key, void *value)
{
	int errval = -1;
	unsigned long index;

	index = find_next_zero_bit(&curr->bitmap, BITMAP_SIZE, 1) - 1;  // 去掉slot有效位
	// lclog_info("index=%lu\n", index);
	if (index >= BITMAP_SIZE) { // Undefined Values returned by BSF instruction  
		printf("%s index error\n", __func__);
		return errval;
	}

	if (index == BITMAP_SIZE - 1)
		return errval;

	curr->entries[index].key = key;
	curr->entries[index].ptr = value;
	return index;
}

/* 
   在未满的leaf node中插入KV对
   对应 算法10
 */
int insert_in_leaf(node *curr, unsigned long key, void *value)
{
	int loc, mid, j;

	curr->bitmap = curr->bitmap - 1;

	// 根据bitmap进行追加
	loc = Append(curr, key, value);

	// 维护slot数组有序
	mid = Search(curr, curr->slot, key);
	
	// [ISSUE] 插入相同的key, 这里如何处理key和entries[].key相同?
	for (j = curr->slot[0]; j >= mid; j--)
		curr->slot[j + 1] = curr->slot[j];

	curr->slot[mid] = loc;

	curr->slot[0] = curr->slot[0] + 1;

	curr->bitmap = curr->bitmap + 1 + (0x1UL << (loc + 1));

	return loc;
}

int insert_in_inner(node *curr, unsigned long key, void *value)
{
	int loc, mid, j;

	curr->bitmap = curr->bitmap - 1;

	loc = Append(curr, key, value);

	mid = Search(curr, curr->slot, key);

	for (j = curr->slot[0]; j >= mid; j--)
		curr->slot[j + 1] = curr->slot[j];
	curr->slot[mid] = loc;

	curr->slot[0] = curr->slot[0] + 1;

	curr->bitmap = curr->bitmap + 1 + (0x1UL << (loc + 1));

	return loc;
}

/*
desc: leaf node分裂, 导致inner node需要更新 
 */
void insert_in_parent(tree *t, node *curr, unsigned long key, node *splitNode)
{
	if (t->root == curr) { // 当前分裂的是root节点
		node *root = allocNode();
		root->isleaf = 0;

		root->leftmostPtr = curr; // 写入左指针

		root->bitmap |= (0x1UL << (0 + 1)); // 写入右指针
		root->entries[0].key = key;
		root->entries[0].ptr = splitNode;

		root->slot[1] = 0;
		root->slot[0] = 1;

		splitNode->parent = root;
		curr->parent = root;

		t->root = root; // 更新tree的根节点
		return ;
	}

	// 当前分裂节点不是根节点
	node *parent = curr->parent;

	if (parent->slot[0] < NODE_SIZE) { // 在内部节点插入
		int mid, j, loc;

		parent->bitmap = parent->bitmap - 1;

		loc = Append(parent, key, splitNode);

		splitNode->parent = parent;

		mid = Search(parent, parent->slot, key);

		for (j = parent->slot[0]; j >= mid; j--)
			parent->slot[j + 1] = parent->slot[j];

		parent->slot[mid] = loc;

		parent->slot[0]++;

		parent->bitmap = parent->bitmap + 1 + (0x1UL << (loc + 1));
	} else { // 父节点已满, 分裂父节点来插入
		int j, loc, cp = parent->slot[0];

		node *splitParent = allocNode();
		splitParent->isleaf = 0;

		for (j = MIN_LIVE_ENTRIES; j > 0; j--) {
			loc = Append(splitParent, parent->entries[parent->slot[cp]].key,
					parent->entries[parent->slot[cp]].ptr);
			node *child = (node *)(parent->entries[parent->slot[cp]].ptr);
			child->parent = splitParent;

			splitParent->slot[j] = loc;
			splitParent->slot[0]++;
			splitParent->bitmap |= (0x1UL << (loc + 1));

			parent->bitmap &= ~(0x1UL << (parent->slot[cp] + 1));

			cp--;
		}

		parent->slot[0] -= MIN_LIVE_ENTRIES;

		// 判断KP pair插入到哪一边
		if (splitParent->entries[splitParent->slot[1]].key > key) {
			insert_in_inner(parent, key, splitNode);
			splitNode->parent = parent;
		} else {
			insert_in_inner(splitParent, key, splitNode);
			splitNode->parent = splitParent;
		}

		// 传递parent节点这一层的影响
		insert_in_parent(t, parent, splitParent->entries[splitParent->slot[1]].key, splitParent);
	}
}

void Insert(tree *t, unsigned long key, void *value)
{
	int numEntries;
	node *curr = t->root;

	curr = find_leaf_node(curr, key); // 找到key所在的leaf node

	numEntries = curr->slot[0];
	if (numEntries == NODE_SIZE) {
#ifdef SPLIT_COUNT
		lclog_info("%s %d curr->bitmap=0x%lx", __func__, __LINE__, curr->bitmap);
#endif

		node *splitNode = allocNode();
		int j, loc, cp = curr->slot[0]; 

		splitNode->leftmostPtr = curr->leftmostPtr;
		// 拷贝entry到新节点中
		for (j = MIN_LIVE_ENTRIES; j > 0; j--) {
			loc = Append(splitNode, curr->entries[curr->slot[cp]].key, curr->entries[curr->slot[cp]].ptr);
			splitNode->slot[j] = loc;
			splitNode->slot[0]++; // 这个可以优化到循环外

			splitNode->bitmap |= (0x1UL << (loc + 1)); // 添加bitmap标记
			curr->bitmap &= ~(0x1UL << (curr->slot[cp] + 1)); // 删除bitmap标记

			cp--;
		}

		curr->slot[0] -= MIN_LIVE_ENTRIES;

		// 判断kv pair 插入到old还是new节点 遵循[)规则
		if (splitNode->entries[splitNode->slot[1]].key > key) {
			loc = insert_in_leaf(curr, key, value);
		} else {
			loc = insert_in_leaf(splitNode, key, value);
		}

		// leaf node分裂后, 需要维护内部索引节点
		insert_in_parent(t, curr, splitNode->entries[splitNode->slot[1]].key, splitNode);

		curr->leftmostPtr = splitNode;
	} else { // 插入leaf node无需分裂
		insert_in_leaf(curr, key, value);
	}
}

/*
   前提: 保证key存在
 */
int delete_in_leaf(node *curr, unsigned long key)
{
	int mid, j;

	mid = Search(curr, curr->slot, key);

	curr->bitmap = curr->bitmap - 1; // 禁用slot

	int old_index = curr->slot[mid]; // 被删除的key所在下标
	// [FIXED] 应该从前往后移动
	for (j = mid; j < curr->slot[0]; j++)
		curr->slot[j] = curr->slot[j + 1];

	curr->slot[0] = curr->slot[0] - 1;

	curr->bitmap = curr->bitmap + 1;
	// [FIXED] entries[]项是没有移动的,但是slot槽移动了,失效mid代表的entries就行
	curr->bitmap &= ~(0x1UL << (old_index + 1));

	return old_index;
}

/*
   源代码错误的地方
 */
int wrong_delete_in_leaf(node *curr, unsigned long key)
{
	int mid, j;

	curr->bitmap = curr->bitmap - 1;  

	mid = Search(curr, curr->slot, key);

	// 问题1
	for (j = curr->slot[0]; j > mid; j--)
		curr->slot[j - 1] = curr->slot[j];

	curr->slot[0] = curr->slot[0] - 1;

	// 问题2
	curr->bitmap = curr->bitmap + 1 - (0x1UL << (mid + 1));
	return 0;
}

int Delete(tree *t, unsigned long key)
{
	node *curr = t->root;

	curr = find_leaf_node(curr, key);
	return delete_in_leaf(curr, key);
	// return wrong_delete_in_leaf(curr, key); 
}

/*
   更新<K, V_new> 对
 */
void *Update(tree *t, unsigned long key, void *value)
{
	node *curr = t->root;
	curr = find_leaf_node(curr, key);
	int mid = Search(curr, curr->slot, key);

	if (curr->entries[curr->slot[mid]].key != key || mid > curr->slot[0]) {
		lclog_debug("key is not in leaf node\n");
		return NULL;
	}

	// 直接更新KV
	curr->entries[curr->slot[mid]].ptr = value;

	return curr->entries[curr->slot[mid]].ptr;
}

/* 查询单个key 的value */
void *Lookup(tree *t, unsigned long key)
{
	node *curr = t->root; 
	curr = find_leaf_node(curr, key);
	int mid = Search(curr, curr->slot, key); 

	if (curr->entries[curr->slot[mid]].key != key || mid > curr->slot[0])
		return NULL;

	return curr->entries[curr->slot[mid]].ptr;
}

void Range_Lookup(tree *t, unsigned long start_key, unsigned int num, unsigned long buf[])
{
	int mid, i;
	unsigned long search_count = 0;
	node *curr = t->root;

	curr = find_leaf_node(curr, start_key);
	mid = Search(curr, curr->slot, start_key);

	while (search_count < num) {
		for (i = mid; i <= curr->slot[0]; i++) {
			buf[search_count] = *(unsigned long *)(curr->entries[curr->slot[i]].ptr);
			search_count++;
			if (search_count == num) 
				break;
		}

		if (search_count == num)
			break;
		curr = curr->leftmostPtr;
		if (curr == NULL) {
			lclog_debug("range_lookup end");
			return ;
		}

		mid = 1;  // 从下一个兄弟节点的最小Key开始
	}
}

#define OP_NUM 10000000
uint64_t keys[OP_NUM];

node *gethead(node *curr);
void scan(node *curr);

int main()
{
	tree *t = initTree();	
	for (int i = 0; i < OP_NUM; i++)
		keys[i] = i;

	for (int i = 0; i < OP_NUM; i++) {
		Insert(t, keys[i], &keys[i]);
	}

	node *head = gethead(t->root);
	if (!head)
		lclog_debug("head is null\n");

	unsigned long new_value =  0xFFFFFFFF00000000;
	for (int i = 0x989668; i < 0x989668 + 0x8; i++) {
		Update(t, i, &new_value);
	}

	unsigned long start_key = 0x98964f; 
	unsigned long ans[32];
	Range_Lookup(t, start_key, 32, ans);

	for (int i = 0; i < 32; i++) {
		printf("Range_Lookup <0x%lx, %lx>\n", start_key + i, ans[i]);
	}
	
	scan(head);
	lclog_info("Insert END\n");

	return 0;
}

node *gethead(node *curr) 
{
	if (curr->isleaf == 0) {
		node *child = curr->leftmostPtr;
		return gethead(child);
	} else {
		return curr;
	}
}

void scan(node *curr)
{
	int leaf_node = 1;
	while (curr) {
		printf("leaf node nr = %d, slot[0]=%d addr = 0x%lx\n", leaf_node, curr->slot[0], (uint64_t)curr);
		for (int i = 1; i <= curr->slot[0]; i++) {
			// 还没有判断bitmap有效位
			if (curr->bitmap & (0x1UL << (curr->slot[i] + 1)))
				printf("\t valid slot[%d]=%d <0x%lx, 0x%lx>\n", i, curr->slot[i], curr->entries[curr->slot[i]].key,
						*(uint64_t *)(curr->entries[curr->slot[i]].ptr));
			else
				printf("\t INvalid slot[%d]=%d <0x%lx, 0x%lx>\n", i, curr->slot[i], curr->entries[curr->slot[i]].key,
						*(uint64_t *)(curr->entries[curr->slot[i]].ptr));
		}
		printf("\t sibling_ptr = 0x%lx\n", (uint64_t)curr->leftmostPtr);

		leaf_node++;
		curr = curr->leftmostPtr;
	}
}
