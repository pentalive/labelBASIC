#include <stdio.h>
#include <string.h>

int main(void)
{
    char line[4096];

    while (fgets(line, sizeof(line), stdin) != NULL) {
        char *colon = strchr(line, ':');

        if (colon) {
            *colon = '\0';

            printf("%s:\n", line);

            /* Skip the colon we replaced */
            printf("%s", colon + 1);

            /* Ensure remainder ends with a newline */
            if (colon[1] != '\0' && colon[strlen(colon + 1)] != '\n')
                putchar('\n');
        } else {
            fputs(line, stdout);
            if (line[strlen(line) - 1] != '\n')
                putchar('\n');
        }

        putchar('\n');
    }

    return 0;
}

