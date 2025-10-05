// server_persist_clean.c - Servidor CoAP simple (C, UDP) con persistencia en TXT (sin warnings)
// Endpoints:
//   GET  /sensor  -> lee estado actual desde CURRENTFILE; si no, última línea de DATAFILE; si nada, NO_DATA
//   POST /echo    -> actualiza estado (overwrite CURRENTFILE) + apendea historial (DATAFILE); responde "echo: <payload>"
//   PUT  /sensor  -> idem POST, responde "UPDATED"
//
// Compilar:  gcc -std=c99 -O2 -Wall -Wextra -o server_persist server_persist_clean.c
// Ejecutar:  ./server_persist
//
// Variables de entorno (opcional):
//   COAP_DATAFILE=/home/ec2-user/coap_data/history.log
//   COAP_CURRENTFILE=/home/ec2-user/coap_data/current.state
//
// Recomendado en EC2 (evita permisos /opt):
//   mkdir -p ~/coap_data
//   export COAP_DATAFILE=~/coap_data/history.log
//   export COAP_CURRENTFILE=~/coap_data/current.state
//   ./server_persist &

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define COAP_PORT 5683

/* ---- CoAP básicos ---- */
#define COAP_VER 1
enum { COAP_CON=0, COAP_NON=1, COAP_ACK=2, COAP_RST=3 };
#define COAP_GET   0x01
#define COAP_POST  0x02
#define COAP_PUT   0x03
#define COAP_204_CHANGED  0x44
#define COAP_205_CONTENT  0x45
#define COAP_404_NOTFOUND 0x84
#define COAP_500_INTERR   0xA0 /* 5.00 */

#define OPT_URI_PATH        11
#define OPT_CONTENT_FORMAT  12
#define CF_TEXT_PLAIN        0

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s){ (void)s; g_stop = 1; }

/* ---- Rutas ---- */
static const char* datafile_path(void){
    const char* p = getenv("COAP_DATAFILE");
    return (p && *p) ? p : "/opt/coap/data.txt";
}
static const char* currentfile_path(void){
    const char* p = getenv("COAP_CURRENTFILE");
    return (p && *p) ? p : "/opt/coap/current.txt";
}

/* ---- gmtime portable (sin warnings) ---- */
static void gmtime_compat(const time_t* t, struct tm* out) {
#ifdef _WIN32
    gmtime_s(out, t);
#else
    struct tm* tmp = gmtime(t);
    if (tmp) *out = *tmp; else memset(out, 0, sizeof(*out));
#endif
}

/* ---- Util: quitar \r\n ---- */
static void rstrip(char* s){
    size_t n = strlen(s);
    while (n && (s[n-1]=='\n' || s[n-1]=='\r')) s[--n] = '\0';
}

/* ---- Lee última línea NO vacía ---- */
static int read_last_line(const char* path, char* out, size_t outsz){
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    char line[2048]; char last[2048] = {0};
    while (fgets(line, sizeof(line), f)){
        rstrip(line);
        if (line[0]){
            snprintf(last, sizeof(last), "%s", line);
        }
    }
    fclose(f);
    if (!last[0]) return 0;
    snprintf(out, outsz, "%s", last);
    return 1;
}

/* ---- Si contiene "payload=", devolver lo que sigue ---- */
static void extract_state(const char* line, char* out, size_t outsz){
    if (outsz == 0) return;
    const char* tag = "payload=";
    const char* p = strstr(line, tag);
    if (p) snprintf(out, outsz, "%s", p + strlen(tag));
    else   snprintf(out, outsz, "%s", line);
}

/* ---- Escribe estado actual (overwrite) + apendea historial ---- */
static int write_current_and_history(const char* current, const char* history, const char* value){
    /* current: overwrite */
    FILE* fc = fopen(current, "w");
    if (!fc) return -1;
    if (fprintf(fc, "%s\n", value) < 0){ fclose(fc); return -1; }
    fclose(fc);
    /* history: append con timestamp UTC */
    time_t now = time(NULL);
    struct tm tm; gmtime_compat(&now, &tm);
    char iso[32]; strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tm);
    FILE* fh = fopen(history, "a");
    if (!fh) return -1;
    if (fprintf(fh, "%s payload=%s\n", iso, value) < 0){ fclose(fh); return -1; }
    fclose(fh);
    return 0;
}

/* ---- CoAP parsing/building (mínimo) ---- */
typedef struct {
    uint8_t type, tkl, code;
    uint16_t mid;
    uint8_t token[8];
    char uri_path[128];
    const uint8_t* payload; size_t payload_len;
} coap_req_t;

static int read_ext(uint8_t v, const uint8_t** p, const uint8_t* end){
    if (v < 13) return v;
    if (v == 13){ if (*p >= end) return -1; return 13 + *(*p)++; }
    if (v == 14){ if (*p+1 >= end) return -1; int val = (((*p)[0]<<8) | (*p)[1]); *p+=2; return 269 + val; }
    return -1;
}

static void append_uri_segment(char* dst, size_t dstsz, const char* seg, size_t seglen){
    size_t cur = strlen(dst);
    if (seglen == 0 || dstsz == 0) return;
    if (dst[0] && cur < dstsz-1) dst[cur++] = '/';
    size_t copy = seglen;
    if (cur + copy >= dstsz) copy = dstsz - 1 - cur;
    memcpy(dst + cur, seg, copy);
    dst[cur + copy] = '\0';
}

static int coap_parse(const uint8_t* buf, size_t len, coap_req_t* r){
    if (len < 4u) return -1;
    uint8_t ver = (buf[0]>>6) & 0x03;
    if (ver != COAP_VER) return -1;
    r->type = (buf[0]>>4) & 0x03;
    r->tkl  =  buf[0]     & 0x0F;
    r->code =  buf[1];
    r->mid  = (uint16_t)((buf[2]<<8) | buf[3]);
    if ((size_t)4 + (size_t)r->tkl > len || r->tkl > 8) return -1;
    memcpy(r->token, buf+4, r->tkl);
    const uint8_t* p = buf + 4 + r->tkl;
    const uint8_t* end = buf + len;
    r->uri_path[0] = '\0';
    int last = 0;
    while (p < end && *p != 0xFF){
        uint8_t b = *p++;
        int d = read_ext((b>>4)&0x0F, &p, end);
        int l = read_ext( b     &0x0F, &p, end);
        if (d < 0 || l < 0) return -1;
        int num = last + d;
        if ((size_t)(p + l) > (size_t)end) return -1;
        if (num == OPT_URI_PATH){
            append_uri_segment(r->uri_path, sizeof(r->uri_path), (const char*)p, (size_t)l);
        }
        p += l; last = num;
    }
    if (p < end && *p == 0xFF){
        p++;
        r->payload = p;
        r->payload_len = (size_t)(end - p);
    } else { r->payload = NULL; r->payload_len = 0; }
    return 0;
}

static int add_option(uint8_t* out, size_t cap, int* last, int number, const uint8_t* val, size_t vlen){
    if (cap < 1u) return -1;
    int delta = number - *last;
    uint8_t dl, ll, dext[2], lext[2]; size_t dextn=0, lextn=0;
    if (delta < 13){ dl=(uint8_t)delta; }
    else if (delta < 269){ dl=13; dext[dextn++]=(uint8_t)(delta-13); }
    else { dl=14; int D=delta-269; dext[dextn++]=(uint8_t)((D>>8)&0xFF); dext[dextn++]=(uint8_t)(D&0xFF); }
    if (vlen < 13u){ ll=(uint8_t)vlen; }
    else if (vlen < 269u){ ll=13; lext[lextn++]=(uint8_t)(vlen-13u); }
    else { ll=14; size_t K=vlen-269u; lext[lextn++]=(uint8_t)((K>>8)&0xFF); lext[lextn++] = (uint8_t)(K & 0xFF); }
    size_t need = 1u + dextn + lextn + vlen;
    if (cap < need) return -1;
    uint8_t* q = out;
    *q++ = (uint8_t)((dl<<4)|ll);
    for (size_t i=0;i<dextn;i++) *q++ = dext[i];
    for (size_t i=0;i<lextn;i++) *q++ = lext[i];
    if (vlen && val){ memcpy(q, val, vlen); q += vlen; }
    *last = number;
    return (int)need;
}

static size_t build_resp(uint8_t* out, size_t cap,
                         uint8_t req_type, uint8_t tkl, const uint8_t* tok,
                         uint16_t mid, uint8_t code,
                         const uint8_t* payload, size_t plen){
    if (cap < 4u + (size_t)tkl) return 0;
    uint8_t type = (req_type == COAP_CON) ? COAP_ACK : COAP_NON;
    out[0] = (uint8_t)((COAP_VER<<6) | (type<<4) | (tkl & 0x0F));
    out[1] = code;
    out[2] = (uint8_t)(mid>>8);
    out[3] = (uint8_t)(mid & 0xFF);
    memcpy(out+4, tok, tkl);
    size_t pos = 4u + (size_t)tkl;
    int last = 0;
    uint8_t cf = CF_TEXT_PLAIN;
    int n = add_option(out+pos, cap-pos, &last, OPT_CONTENT_FORMAT, &cf, 1u);
    if (n < 0) return 0;
    pos += (size_t)n;
    if (payload && plen > 0){
        if (pos + 1u + plen > cap) return 0;
        out[pos++] = 0xFF;
        memcpy(out+pos, payload, plen);
        pos += plen;
    }
    return pos;
}

int main(void){
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    const char* DATA = datafile_path();
    const char* CURR = currentfile_path();
    printf("CoAP persist server on 0.0.0.0:%d\n", COAP_PORT);
    printf("datafile=%s\ncurrent=%s\n", DATA, CURR);
    fflush(stdout);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0){ perror("socket"); return 1; }
    struct sockaddr_in a; memset(&a,0,sizeof(a));
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons(COAP_PORT);
    if (bind(fd, (struct sockaddr*)&a, sizeof(a)) != 0){ perror("bind"); close(fd); return 1; }

    uint8_t inbuf[1500], outbuf[1500];

    while (!g_stop){
        struct sockaddr_in cli; socklen_t clen = sizeof(cli);
        ssize_t n = recvfrom(fd, inbuf, sizeof(inbuf), 0, (struct sockaddr*)&cli, &clen);
        if (n <= 0) continue;

        coap_req_t req;
        if (coap_parse(inbuf, (size_t)n, &req) != 0) continue;

        /* cuerpo como texto */
        char body[1024] = {0};
        if (req.payload_len > 0){
            size_t L = req.payload_len < sizeof(body)-1 ? req.payload_len : sizeof(body)-1;
            memcpy(body, req.payload, L); body[L] = '\0';
        }

        char resp[1200] = {0};
        size_t rlen = 0;
        uint8_t rcode = COAP_404_NOTFOUND;

        if (req.code == COAP_GET && strcmp(req.uri_path, "sensor") == 0){
            char line[2048];
            if (read_last_line(CURR, line, sizeof(line))){
                snprintf(resp, sizeof(resp), "%s", line); rlen = strlen(resp); rcode = COAP_205_CONTENT;
            } else if (read_last_line(DATA, line, sizeof(line))){
                char st[2048]; extract_state(line, st, sizeof(st));
                snprintf(resp, sizeof(resp), "%s", st);   rlen = strlen(resp); rcode = COAP_205_CONTENT;
            } else {
                snprintf(resp, sizeof(resp), "NO_DATA");  rlen = strlen(resp); rcode = COAP_205_CONTENT;
            }
        }
        else if (req.code == COAP_POST && strcmp(req.uri_path, "echo") == 0){
            if (write_current_and_history(CURR, DATA, body) == 0){
                snprintf(resp, sizeof(resp), "echo: %s", body);
                rlen = strlen(resp); rcode = COAP_205_CONTENT;
            } else {
                snprintf(resp, sizeof(resp), "WRITE_FAIL");
                rlen = strlen(resp); rcode = COAP_500_INTERR;
            }
        }
        else if (req.code == COAP_PUT && strcmp(req.uri_path, "sensor") == 0){
            if (write_current_and_history(CURR, DATA, body) == 0){
                snprintf(resp, sizeof(resp), "UPDATED");
                rlen = strlen(resp); rcode = COAP_204_CHANGED;
            } else {
                snprintf(resp, sizeof(resp), "WRITE_FAIL");
                rlen = strlen(resp); rcode = COAP_500_INTERR;
            }
        }
        else {
            snprintf(resp, sizeof(resp), "NOT_FOUND"); rlen = strlen(resp); rcode = COAP_404_NOTFOUND;
        }

        size_t outlen = build_resp(outbuf, sizeof(outbuf),
                                   req.type, req.tkl, req.token, req.mid,
                                   rcode, (const uint8_t*)resp, rlen);
        if (outlen > 0){
            sendto(fd, outbuf, outlen, 0, (struct sockaddr*)&cli, clen);
        }
    }

    close(fd);
    puts("bye");
    return 0;
}
