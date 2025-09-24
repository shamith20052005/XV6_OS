#include "types.h"
#include "stat.h"
#include "user.h"

int main(int argc, char* argv[]){
    if(argc != 1){
        printf(1, "Usage: get_burst_time...\n");
		exit();
    }

	printf(1, "Burst time = %d\n", get_burst_time());

	exit();
}