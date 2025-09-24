#include "types.h"
#include "stat.h"
#include "user.h"

int main(void)
{
    printf(1,"Total number of current active processes are  %d\n",getNumProc());
	exit();
}