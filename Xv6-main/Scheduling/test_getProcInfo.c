#include "types.h"
#include "stat.h"
#include "user.h"

int main(int argc, char *argv[])
{
    if(argc < 2){
        printf(2, "Usage: getProcInfo\n");
        exit();
    }

    for(int i = 1; i < argc; i++){
        struct processInfo* pinfo;

        pinfo = (struct processInfo*) malloc(sizeof(struct processInfo));

        if(getProcInfo(atoi(argv[i]), pinfo) < 0){
            printf(1, "getProcInfo: %s failed.\n", argv[i]);
            break;
        }

        printf(1, "PPID: %d\n", pinfo->ppid);
        printf(1, "PSize: %d\n", pinfo->psize);
        printf(1, "Context Switches: %d\n", pinfo->numberContextSwitches);
    }

    exit();
}