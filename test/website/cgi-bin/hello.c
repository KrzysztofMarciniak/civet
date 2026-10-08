#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    const char* method;
    const char* query_string;
    const char* script_name;

    method = getenv("REQUEST_METHOD");
    query_string = getenv("QUERY_STRING");
    script_name = getenv("SCRIPT_NAME");

    printf("Content-Type: text/html\r\n\r\n");
    printf("<!DOCTYPE html>\n");
    printf("<html>\n");
    printf("<head><title>CGI Test</title></head>\n");
    printf("<body>\n");
    printf("<h1>Hello from Civet CGI!</h1>\n");
    printf("<p>Script: %s</p>\n", script_name ? script_name : "(none)");
    printf("<p>Method: %s</p>\n", method ? method : "(none)");

    if (query_string && query_string[0] != '\0') {
        printf("<p>Query String: %s</p>\n", query_string);
    }

    printf("<hr>\n");
    printf("<nav><a href=\"/\">Back Home</a></nav>\n");
    printf("</body>\n");
    printf("</html>\n");

    return 0;
}
