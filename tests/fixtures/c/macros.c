#define MAX_SIZE 1024
#define PI 3.14159
#define DEFAULT_NAME "codelenses"

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define SQUARE(x) ((x) * (x))

int buffer[MAX_SIZE];
int larger = MAX(10, 20);
int squared = SQUARE(5);

int calculate(int val) {
    int m = MAX(val, MAX_SIZE);
    return SQUARE(m) + larger;
}
