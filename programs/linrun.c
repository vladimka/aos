#include "libaos.h"

// Spawn lin/hello as a real (pid>0) Linux task with sink 0 so its stdout
// routes straight to the kernel terminal. Exercises the private-window path.
int main(void) {
    int rc = spawn("lin/hello", "", 0);
    if (rc < 0) {
        print("spawn failed rc=");
        print_dec((unsigned int)rc);
        print("\n");
        return 1;
    }
    print("linux task ");
    print_dec((unsigned int)rc);
    print(" spawned\n");
    return 0;
}
