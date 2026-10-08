#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* const methods[] = {"GET",   "HEAD",  "POST", "PUT",
                                      "PATCH", "DELETE"

};

/* One string per line: avoids overlength string literals. */
static const char* const script[] = {
    "<script>",
    "function run(m) {",
    "  var o = document.getElementById('out');",
    "  var opt = { method: m };",
    "  var ct = document.getElementById('ct').value;",
    "  if (m !== 'GET' && m !== 'HEAD') {",
    "    opt.body = document.getElementById('body').value;",
    "    if (ct) opt.headers = { 'Content-Type': ct };",
    "  }",
    "  fetch(location.pathname + '?via=' + m, opt).then(function (r) {",
    "    return r.text().then(function (t) {",
    "      o.textContent = r.status + ' ' + r.statusText + '\\n' +",
    "        'content-length: ' + r.headers.get('content-length') +",
    "        '\\n\\n' + t;",
    "    });",
    "  }).catch(function (e) {",
    "    o.textContent = 'error: ' + e;",
    "  });",
    "}",
    "</script>"};

static const char* env_or(const char* name, const char* fallback) {
        const char* v;

        v = getenv(name);

        return v != NULL ? v : fallback;
}

static void html_escape(const char* s) {
        if (s == NULL) return;

        for (; *s != '\0'; s++) {
                switch (*s) {
                        case '&':
                                fputs("&amp;", stdout);
                                break;
                        case '<':
                                fputs("&lt;", stdout);
                                break;
                        case '>':
                                fputs("&gt;", stdout);
                                break;
                        case '"':
                                fputs("&quot;", stdout);
                                break;
                        case '\'':
                                fputs("&#39;", stdout);
                                break;
                        default:
                                putchar(*s);
                }
        }
}

/* Copies up to `want` bytes of the request body from stdin to stdout. */
static void echo_body(unsigned long want) {
        char buf[4096];
        size_t n;

        while (want > 0) {
                size_t request;

                request = want < sizeof(buf) ? (size_t)want : sizeof(buf);

                n = fread(buf, 1, request, stdin);

                if (n == 0) break;

                fwrite(buf, 1, n, stdout);

                want -= (unsigned long)n;
        }
}

/*
 * Print one environment variable.
 *
 * getenv() cannot distinguish a missing variable from an empty variable,
 * which is fine for this diagnostic CGI: both are useful to see.
 */
static void print_env(const char* name) {
        const char* value;

        value = getenv(name);

        printf("%-20s: %s\n", name, value != NULL ? value : "(not set)");
}

/*
 * Print all HTTP_* variables.
 *
 * The server deliberately constructs the CGI environment, so environ
 * contains only the environment supplied to this CGI process. We use
 * environ here specifically to discover dynamically generated HTTP_*
 * variables.
 */
extern char** environ;

static void print_http_env(void) {
        char** p;
        int found;

        found = 0;

        for (p = environ; p != NULL && *p != NULL; p++) {
                if (strncmp(*p, "HTTP_", 5) != 0) continue;

                printf("  %s\n", *p + 5);
                found = 1;
        }

        if (!found) printf("  (none)\n");
}

static void print_environment(void) {
        printf("CGI environment\n");
        printf("================\n\n");

        printf("[CGI]\n");
        print_env("GATEWAY_INTERFACE");
        print_env("SERVER_SOFTWARE");
        print_env("SERVER_PROTOCOL");

        printf("\n[Server]\n");
        print_env("SERVER_NAME");
        print_env("SERVER_PORT");

        printf("\n[Request]\n");
        print_env("REQUEST_METHOD");
        print_env("SCRIPT_NAME");
        print_env("SCRIPT_FILENAME");
        print_env("QUERY_STRING");

        printf("\n[Entity]\n");
        print_env("CONTENT_TYPE");
        print_env("CONTENT_LENGTH");

        printf("\n[Client]\n");
        print_env("REMOTE_ADDR");

        printf("\n[Process]\n");
        print_env("PATH");

        printf("\n[HTTP headers]\n");
        print_http_env();

        printf("\n");
}

static void print_page(const char* script_name, const char* query) {
        size_t i;

        printf("Content-Type: text/html\r\n\r\n");

        printf("<!DOCTYPE html>\n");
        printf("<html>\n");

        printf("<head>\n");
        printf("<meta charset=\"utf-8\">\n");
        printf("<title>CGI Test</title>\n");
        printf("</head>\n");

        printf("<body>\n");

        printf("<h1>Hello from Civet CGI!</h1>\n");

        printf("<p>Script: ");
        html_escape(script_name);
        printf("</p>\n");

        if (query != NULL && query[0] != '\0') {
                printf("<p>Query String: ");
                html_escape(query);
                printf("</p>\n");
        }

        printf("<hr>\n");

        printf("<h2>Request body</h2>\n");

        printf(
            "<p>"
            "Content-Type: "
            "<input id=\"ct\" value=\"text/plain\">"
            "</p>\n");

        printf(
            "<p>"
            "<textarea id=\"body\" rows=\"4\" cols=\"50\">"
            "hello body"
            "</textarea>"
            "</p>\n");

        printf("<p>\n");

        for (i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
                printf("<button onclick=\"run('%s')\">%s</button>\n",
                       methods[i], methods[i]);
        }

        printf("</p>\n");

        printf("<pre id=\"out\">press a button</pre>\n");

        for (i = 0; i < sizeof(script) / sizeof(script[0]); i++)
                printf("%s\n", script[i]);

        printf("<hr>\n");

        printf("<h2>What this tests</h2>\n");
        printf("<ul>\n");
        printf("<li>CGI standard variables</li>\n");
        printf("<li>SERVER_NAME / SERVER_PORT</li>\n");
        printf("<li>SCRIPT_NAME / SCRIPT_FILENAME</li>\n");
        printf("<li>QUERY_STRING</li>\n");
        printf("<li>CONTENT_TYPE / CONTENT_LENGTH</li>\n");
        printf("<li>REMOTE_ADDR</li>\n");
        printf("<li>PATH</li>\n");
        printf("<li>HTTP_* request headers</li>\n");
        printf("<li>POST/PUT/PATCH request bodies</li>\n");
        printf("</ul>\n");

        printf("<nav><a href=\"/\">Back Home</a></nav>\n");

        printf("</body>\n");
        printf("</html>\n");
}

static unsigned long content_length(void) {
        const char* s;
        char* end;
        unsigned long value;

        s = getenv("CONTENT_LENGTH");

        if (s == NULL || *s == '\0') return 0;

        value = strtoul(s, &end, 10);

        if (end == s || *end != '\0') return 0;

        return value;
}

static int is_initial_get(void) {
        const char* method;
        const char* query;

        method = env_or("REQUEST_METHOD", "");
        query  = env_or("QUERY_STRING", "");

        return strcmp(method, "GET") == 0 && strstr(query, "via=") == NULL;
}

int main(void) {
        const char* method;
        const char* script_name;
        const char* query;
        const char* ctype;
        const char* clen;
        unsigned long want;

        method      = env_or("REQUEST_METHOD", "(none)");
        script_name = env_or("SCRIPT_NAME", "(none)");
        query       = env_or("QUERY_STRING", "");
        ctype       = env_or("CONTENT_TYPE", "");
        clen        = env_or("CONTENT_LENGTH", "0");

        want = content_length();

        /*
         * A normal browser load gets the interactive HTML page.
         *
         * Everything else prints the complete CGI environment and echoes
         * the request body. This makes curl and the browser buttons useful
         * for testing the same CGI execution path.
         */
        if (is_initial_get()) {
                print_page(script_name, query);
                return 0;
        }

        printf("Content-Type: text/plain\r\n\r\n");

        printf("method: %s\n", method);
        printf("script: %s\n", script_name);
        printf("query: %s\n", query);
        printf("content-type: %s\n", ctype);
        printf("content-length: %s\n\n", clen);

        print_environment();

        printf("body:\n");

        echo_body(want);

        printf("\n");

        return 0;
}
