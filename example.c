#define CB_IMPLEMENTATION

#include "cb.h"
int main(int argc, char **argv) {

    command hello1 = {0};
    command hello2 = {0};
    CB_REBUILD_YOURSELF();
    cb_rebuild_yourself(argc, argv);
}
