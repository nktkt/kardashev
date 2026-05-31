#include <stdio.h>
int main(){ long acc=0; for(long i=0;i<200000000;i++) acc += i*3 - (i/2); printf("%ld\n", acc); return 0; }
