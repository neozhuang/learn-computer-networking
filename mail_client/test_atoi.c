#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[])
{
    const char * str = "250-mail.example.com HELO";
    int val = atoi(str);
    printf("val = %d\n", val);

    return EXIT_SUCCESS;
}
