#include <stdio.h>
int main(){ long steps=0; for(long n=1;n<3000000;n++){ long x=n; while(x>1){ if(x%2==0)x/=2; else x=3*x+1; steps++; } } printf("%ld\n", steps); return 0; }
