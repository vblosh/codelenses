#include <stddef.h>

struct Node {
    int value;
    struct Node* next;
    struct Node* prev;
};

int x = 42;
int* ptr = &x;
int** pptr = &ptr;
const char* msg = "hello";
char* names[4];

int (*comparator)(int, int);

struct Node* create_node(int val, struct Node* next) {
    struct Node node;
    node.value = val;
    node.next = next;
    node.prev = NULL;
    return ptr ? &node : NULL;
}

int process_nodes(struct Node* head) {
    if (head == NULL) {
        return 0;
    }
    head->value = *ptr;
    head->next = NULL;
    int res = comparator(10, 20);
    int res2 = (*comparator)(30, 40);
    return res + res2 + head->value;
}
