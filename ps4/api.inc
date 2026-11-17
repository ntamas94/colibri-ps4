/* Small, single-request OpenAI-compatible API for the PS4 OLMoE build.
 * Included by olmoe.c so requests call its inference functions in-process. */
#include <orbis/Net.h>
#include <orbis/Sysmodule.h>
#include <orbis/libkernel.h>
#include <stdint.h>
#include <strings.h>
#include <sys/time.h>

#define PS4_API_PORT 8080
#define PS4_API_MAX_BODY (1u << 20)
#define PS4_API_SOL_SOCKET 0xffff
#define PS4_API_SO_REUSEADDR 0x0004
#define PS4_API_SO_RCVTIMEO 0x1006
#define PS4_API_SO_SNDTIMEO 0x1005
#define PS4_API_IPPROTO_TCP 6

typedef struct { uint8_t len, family; uint16_t port; uint32_t addr; char zero[8]; } Ps4SockaddrIn;
_Static_assert(sizeof(Ps4SockaddrIn) == sizeof(OrbisNetSockaddr), "sockaddr size");

typedef struct { char *p; size_t n, cap; } Ps4Buf;

static int ps4_api_requested(void) {
    const char *value = getenv("PS4_API");
    return !value || strcmp(value, "0") != 0;
}

static int ps4_buf_add(Ps4Buf *b, const char *s, size_t n) {
    if (n > SIZE_MAX - b->n - 1) return 0;
    size_t need = b->n + n + 1;
    if (need > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < need) { if (cap > SIZE_MAX / 2) return 0; cap *= 2; }
        char *p = realloc(b->p, cap); if (!p) return 0; b->p = p; b->cap = cap;
    }
    memcpy(b->p + b->n, s, n); b->n += n; b->p[b->n] = 0; return 1;
}

static int ps4_buf_text(Ps4Buf *b, const char *s) { return ps4_buf_add(b, s, strlen(s)); }

static int ps4_buf_json(Ps4Buf *b, const char *s, size_t n) {
    if (!ps4_buf_text(b, "\"")) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i]; char esc[7];
        if (c == '"' || c == '\\') { esc[0] = '\\'; esc[1] = (char)c; if (!ps4_buf_add(b, esc, 2)) return 0; }
        else if (c == '\n') { if (!ps4_buf_text(b, "\\n")) return 0; }
        else if (c == '\r') { if (!ps4_buf_text(b, "\\r")) return 0; }
        else if (c == '\t') { if (!ps4_buf_text(b, "\\t")) return 0; }
        else if (c < 0x20) { int m = snprintf(esc, sizeof esc, "\\u%04x", c); if (!ps4_buf_add(b, esc, (size_t)m)) return 0; }
        else if (!ps4_buf_add(b, (const char *)&s[i], 1)) return 0;
    }
    return ps4_buf_text(b, "\"");
}

static int ps4_send_all(OrbisNetId fd, const void *data, size_t n) {
    const char *p = data;
    while (n) { int sent = sceNetSend(fd, p, n > INT32_MAX ? INT32_MAX : (int)n, 0); if (sent <= 0) return 0; p += sent; n -= (size_t)sent; }
    return 1;
}

static void ps4_http_reply(OrbisNetId fd, int status, const char *reason, const char *type, const char *body, size_t n) {
    char head[384]; int h = snprintf(head, sizeof head,
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", status, reason, type, n);
    if (h > 0 && (size_t)h < sizeof head && ps4_send_all(fd, head, (size_t)h)) ps4_send_all(fd, body, n);
}

static void ps4_json_error(OrbisNetId fd, int status, const char *message, const char *code) {
    Ps4Buf b = {0}; ps4_buf_text(&b, "{\"error\":{\"message\":");
    ps4_buf_json(&b, message, strlen(message)); ps4_buf_text(&b, ",\"type\":\"invalid_request_error\",\"code\":");
    ps4_buf_json(&b, code, strlen(code)); ps4_buf_text(&b, "}}");
    ps4_http_reply(fd, status, status == 400 ? "Bad Request" : status == 401 ? "Unauthorized" :
                   status == 404 ? "Not Found" : "Internal Server Error",
                   "application/json", b.p ? b.p : "{}", b.n); free(b.p);
}

static int ps4_recv_more(OrbisNetId fd, char *p, size_t cap, size_t *n) {
    if (*n >= cap) return 0;
    int got = sceNetRecv(fd, p + *n, cap - *n, 0);
    if (got <= 0) return 0; *n += (size_t)got; return 1;
}

static const char *ps4_find_header(const char *headers, const char *name) {
    size_t n = strlen(name); const char *p = headers;
    while ((p = strstr(p, "\r\n")) != NULL) {
        p += 2; if (!strncasecmp(p, name, n) && p[n] == ':') { p += n + 1; while (*p == ' ' || *p == '\t') p++; return p; }
    }
    return NULL;
}

static int ps4_read_http(OrbisNetId fd, char *method, size_t mcap, char *path, size_t pcap,
                         char **body_out, size_t *body_n, const char **auth_out) {
    char head[16384]; size_t n = 0, sep = SIZE_MAX;
    while (sep == SIZE_MAX) {
        if (!ps4_recv_more(fd, head, sizeof head - 1, &n)) return 0;
        head[n] = 0;
        char *p = strstr(head, "\r\n\r\n"); if (p) sep = (size_t)(p - head) + 4;
    }
    char *line_end = strstr(head, "\r\n"); if (!line_end) return 0; *line_end = 0;
    char version[16]; if (sscanf(head, "%15s %511s %15s", method, path, version) != 3 ||
        strlen(method) >= mcap || strlen(path) >= pcap || strncmp(version, "HTTP/1.", 7)) return 0;
    const char *cl = ps4_find_header(line_end + 2, "Content-Length");
    const char *te = ps4_find_header(line_end + 2, "Transfer-Encoding");
    const char *auth = ps4_find_header(line_end + 2, "Authorization");
    if (auth) { static char saved[512]; size_t k = strcspn(auth, "\r\n"); if (k >= sizeof saved) return 0; memcpy(saved, auth, k); saved[k] = 0; *auth_out = saved; }
    else *auth_out = NULL;
    size_t len = 0;
    if (te && strncasecmp(te, "identity", 8)) return 0;
    if (cl) { char *end; unsigned long v = strtoul(cl, &end, 10); if (end == cl || v > PS4_API_MAX_BODY || (*end != '\r' && *end != '\n')) return 0; len = (size_t)v; }
    size_t have = n - sep; if (have > len) return 0;
    char *body = malloc(len + 1); if (!body) return 0;
    if (have) memcpy(body, head + sep, have);
    while (have < len) { int got = sceNetRecv(fd, body + have, len - have, 0); if (got <= 0) { free(body); return 0; } have += (size_t)got; }
    body[len] = 0; *body_out = body; *body_n = len; return 1;
}

static const char *ps4_json_string(jval *v) { return v && v->t == J_STR ? v->str : NULL; }

static int ps4_make_prompt(jval *root, Ps4Buf *prompt) {
    jval *messages = json_get(root, "messages");
    if (!messages || messages->t != J_ARR || messages->len < 1 || messages->len > 64) return 0;
    jval *tools = json_get(root, "tools");
    if (tools && tools->t != J_NULL) return 0;
    if (!ps4_buf_text(prompt, "|||IP_ADDRESS|||")) return 0;
    for (int i = 0; i < messages->len; i++) {
        jval *msg = messages->kids[i], *rv = json_get(msg, "role"), *cv = json_get(msg, "content");
        const char *role = ps4_json_string(rv), *content = ps4_json_string(cv);
        if (!role || !content) return 0;
        if (!strcmp(role, "system") || !strcmp(role, "developer")) {
            if (!ps4_buf_text(prompt, "<|system|>\n") || !ps4_buf_text(prompt, content) || !ps4_buf_text(prompt, "\n")) return 0;
        } else if (!strcmp(role, "user")) {
            if (!ps4_buf_text(prompt, "<|user|>\n") || !ps4_buf_text(prompt, content) || !ps4_buf_text(prompt, "\n")) return 0;
        } else if (!strcmp(role, "assistant")) {
            if (i == messages->len - 1) return 0;
            if (!ps4_buf_text(prompt, "<|assistant|>\n") || !ps4_buf_text(prompt, content) ||
                !ps4_buf_text(prompt, "|||IP_ADDRESS|||\n")) return 0;
        } else return 0;
    }
    return ps4_buf_text(prompt, "<|assistant|>\n");
}

static int ps4_api_chat(OrbisNetId fd, Model *m, Tok *T, int ctx_cap, char *body, size_t body_n) {
    (void)body_n;
    jval *root = json_parse_checked(body); if (!root || root->t != J_OBJ) { json_free(root); ps4_json_error(fd, 400, "Malformed JSON request.", "invalid_json"); return 0; }
    Ps4Buf prompt = {0};
    if (!ps4_make_prompt(root, &prompt)) { json_free(root); free(prompt.p); ps4_json_error(fd, 400, "Expected text messages with system, developer, user, or assistant roles; tools and multimodal content are not supported.", "unsupported_request"); return 0; }
    int max_tok = 128; float temp = 0.7f, top_p = 0.95f;
    jval *v = json_get(root, "max_tokens"); if (!v) v = json_get(root, "max_completion_tokens");
    if (v) { if (v->t != J_NUM || floor(v->num) != v->num || v->num < 1 || v->num > 512) goto bad_params; max_tok = (int)v->num; }
    v = json_get(root, "temperature"); if (v) { if (v->t != J_NUM || !isfinite(v->num) || v->num < 0 || v->num > 2) goto bad_params; temp = (float)v->num; }
    v = json_get(root, "top_p"); if (v) { if (v->t != J_NUM || !isfinite(v->num) || v->num <= 0 || v->num > 1) goto bad_params; top_p = (float)v->num; }
    v = json_get(root, "stream");
    if (v && v->t != J_BOOL) goto bad_params;
    if (v && v->boolean) { json_free(root); free(prompt.p); ps4_json_error(fd, 400, "Streaming is not supported by this PS4 API build.", "unsupported_parameter"); return 0; }
    json_free(root);
    int *probe = malloc((size_t)ctx_cap * sizeof(int));
    int np = probe ? tok_encode(T, prompt.p, (int)prompt.n, probe, ctx_cap) : 0;
    free(probe);
    if (np < 1 || coli_serve_budget(np, max_tok, ctx_cap, 0) < 0) { free(prompt.p); ps4_json_error(fd, 400, "Prompt exceeds the model context. Shorten the messages or lower max_tokens.", "context_length_exceeded"); return 0; }
    SReq q; memset(&q, 0, sizeof q); strcpy(q.id, "ps4"); q.max_tok = max_tok; q.temp = temp; q.top_p = top_p; q.payload = prompt.p; q.plen = (int)prompt.n;
    int *ids = NULL, prompt_tokens = 0; float *logit = NULL; OlmReqClock clk;
    /* The HTTP request has no stdin control channel for the serve loop's cancellation poll. */
    if (!olm_serve_start(m, T, &q, ctx_cap, &ids, &prompt_tokens, &logit, &clk, 0, NULL)) {
        free(prompt.p); ps4_json_error(fd, 500, "The model could not start this request.", "engine_error"); return 0;
    }
    Ps4Buf answer = {0}; int generated = 0, limited = 1, hist_len = prompt_tokens, oom = 0;
    for (; generated < q.max_tok && hist_len < ctx_cap; ) {
        int nt = pick_tok(logit, m->c.vocab, -1); free(logit); logit = NULL;
        if (is_stop(nt)) { limited = 0; break; }
        char token[512]; int bytes = tok_decode(T, &nt, 1, token, sizeof token - 1);
        if (bytes < 0 || !ps4_buf_add(&answer, token, (size_t)bytes)) { oom = 1; break; }
        generated++; hist_len++;
        if (generated >= q.max_tok || hist_len >= ctx_cap) break;
        logit = step(m, &nt, 1, hist_len - 1);
    }
    free(logit); free(ids); free(prompt.p); olm_unpark(m);
    if (oom) { free(answer.p); ps4_json_error(fd, 500, "Not enough memory to build the response.", "out_of_memory"); return 0; }
    if (!answer.p) answer.p = calloc(1, 1);
    if (!answer.p) { ps4_json_error(fd, 500, "Not enough memory to build the response.", "out_of_memory"); return 0; }
    Ps4Buf out = {0};
    char head[256]; int hn = snprintf(head, sizeof head, "{\"id\":\"chatcmpl-ps4\",\"object\":\"chat.completion\",\"created\":%lld,\"model\":\"olmoe\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":", (long long)time(NULL));
    if (hn < 0 || (size_t)hn >= sizeof head || !ps4_buf_add(&out, head, (size_t)hn) || !ps4_buf_json(&out, answer.p, answer.n) ||
        !ps4_buf_text(&out, limited ? "},\"finish_reason\":\"length\"}],\"usage\":{" : "},\"finish_reason\":\"stop\"}],\"usage\":{")) {
        free(answer.p); free(out.p); ps4_json_error(fd, 500, "Not enough memory to build the response.", "out_of_memory"); return 0;
    }
    snprintf(head, sizeof head, "\"prompt_tokens\":%d,\"completion_tokens\":%d,\"total_tokens\":%d}}", prompt_tokens, generated, prompt_tokens + generated);
    if (!ps4_buf_text(&out, head)) { free(answer.p); free(out.p); ps4_json_error(fd, 500, "Not enough memory to build the response.", "out_of_memory"); return 0; }
    ps4_http_reply(fd, 200, "OK", "application/json", out.p, out.n);
    free(answer.p); free(out.p); return 1;
bad_params:
    json_free(root); free(prompt.p); ps4_json_error(fd, 400, "Invalid max_tokens, temperature, or top_p.", "invalid_parameter"); return 0;
}

static void ps4_api_request(OrbisNetId fd, Model *m, Tok *T, int ctx_cap, const char *key) {
    char method[16] = {0}, path[512] = {0}; char *body = NULL; size_t body_n = 0; const char *auth = NULL;
    if (!ps4_read_http(fd, method, sizeof method, path, sizeof path, &body, &body_n, &auth)) { ps4_json_error(fd, 400, "Invalid or incomplete HTTP request.", "invalid_http_request"); return; }
    if (key && *key) { char expected[600]; snprintf(expected, sizeof expected, "Bearer %s", key); if (!auth || strcmp(auth, expected)) { free(body); ps4_json_error(fd, 401, "A valid bearer API key is required.", "unauthorized"); return; } }
    if (!strcmp(method, "GET") && !strcmp(path, "/health")) ps4_http_reply(fd, 200, "OK", "application/json", "{\"ok\":true}", 11);
    else if (!strcmp(method, "GET") && !strcmp(path, "/v1/models")) {
        static const char models[] = "{\"object\":\"list\",\"data\":[{\"id\":\"olmoe\",\"object\":\"model\",\"owned_by\":\"colibri-ps4\"}]}";
        ps4_http_reply(fd, 200, "OK", "application/json", models, sizeof models - 1);
    }
    else if (!strcmp(method, "POST") && !strcmp(path, "/v1/chat/completions")) ps4_api_chat(fd, m, T, ctx_cap, body, body_n);
    else ps4_json_error(fd, 404, "Unknown API route.", "not_found");
    free(body);
}

static void ps4_api_loop(Model *m, Tok *T, int ctx_cap) {
    int r = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET);
    if (r < 0 || sceNetInit() < 0) { fprintf(stderr, "[PS4 API] network initialization failed: 0x%08X\n", r); return; }
    char key[256] = {0}; FILE *kf = fopen("/data/colibri_api_key.txt", "rb");
    if (kf) { size_t n = fread(key, 1, sizeof key, kf); int extra = fgetc(kf); fclose(kf);
        if (n >= sizeof key || extra != EOF) { fprintf(stderr, "[PS4 API] API key file must be shorter than 256 bytes\n"); return; }
        while (n && (key[n-1] == '\n' || key[n-1] == '\r' || key[n-1] == ' ')) key[--n] = 0;
    }
    if (!key[0]) { fprintf(stderr, "[PS4 API] create /data/colibri_api_key.txt before enabling the network API\n"); return; }
    OrbisNetId listener = sceNetSocket("colibri_api", ORBIS_NET_AF_INET, ORBIS_NET_SOCK_STREAM, PS4_API_IPPROTO_TCP);
    if (listener < 0) { fprintf(stderr, "[PS4 API] socket creation failed: %d\n", listener); return; }
    int one = 1; sceNetSetsockopt(listener, PS4_API_SOL_SOCKET, PS4_API_SO_REUSEADDR, &one, sizeof one);
    Ps4SockaddrIn addr; memset(&addr, 0, sizeof addr); addr.len = sizeof addr; addr.family = ORBIS_NET_AF_INET;
    addr.port = sceNetHtons(PS4_API_PORT); addr.addr = 0;
    if (sceNetBind(listener, (const OrbisNetSockaddr *)&addr, sizeof addr) || sceNetListen(listener, 2)) {
        fprintf(stderr, "[PS4 API] bind/listen on %d failed\n", PS4_API_PORT); sceNetSocketClose(listener); return;
    }
    fprintf(stderr, "[PS4 API] listening on port %d%s\n", PS4_API_PORT, key[0] ? " (API key required)" : " (no API key configured)");
    for (;;) { Ps4SockaddrIn peer; OrbisNetSocklen_t plen = sizeof peer; OrbisNetId fd = sceNetAccept(listener, (OrbisNetSockaddr *)&peer, &plen);
        if (fd >= 0) {
            struct timeval timeout = { .tv_sec = 20, .tv_usec = 0 };
            sceNetSetsockopt(fd, PS4_API_SOL_SOCKET, PS4_API_SO_RCVTIMEO, &timeout, sizeof timeout);
            sceNetSetsockopt(fd, PS4_API_SOL_SOCKET, PS4_API_SO_SNDTIMEO, &timeout, sizeof timeout);
            ps4_api_request(fd, m, T, ctx_cap, key); sceNetSocketClose(fd);
        }
    }
}
