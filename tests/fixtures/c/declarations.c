int add(int a, int b);
static void log_internal(const char* msg);

extern int external_count;
int external_count = 0;
static const int MAX_RETRIES = 3;

struct ForwardDecl;

struct ForwardDecl {
    int id;
    const char* name;
};

int alpha = 1, beta = 2, *gamma = 0;

int add(int a, int b) {
    return a + b;
}

static void log_internal(const char* msg) {
    if (msg) {
        external_count += 1;
    }
}
