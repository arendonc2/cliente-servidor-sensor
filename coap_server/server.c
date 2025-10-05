// server.c - Servidor CoAP mínimo (C puro) con sockets UDP.
// Endpoints soportados:
//   POST /echo    -> responde "echo: <payload>"
//   PUT  /sensor  -> guarda estado (en memoria)
//   GET  /sensor  -> devuelve estado almacenado
//
// Compilar:   gcc -O2 -Wall -Wextra -o server server.c
// Ejecutar:   ./server            (escucha en 0.0.0.0:5683)

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define COAP_PORT 5683

/* ---- CoAP defs mínimos ---- */
#define COAP_VER 1
enum { COAP_CON=0, COAP_NON=1, COAP_ACK=2, COAP_RST=3 };
/* Códigos: class<<5 | detail */
#define COAP_GET   0x01
#define COAP_POST  0x02
#define COAP_PUT   0x03

#define COAP_201_CREATED  0x41
#define COAP_204_CHANGED  0x44
#define COAP_205_CONTENT  0x45
#define COAP_404_NOTFOUND 0x84
#define COAP_405_METHODNA 0x85

/* Opciones que usaremos */
#define OPT_URI_PATH        11
#define OPT_CONTENT_FORMAT  12
/* Content-Format: text/plain;charset=utf-8 == 0 */
#define CF_TEXT_PLAIN       0

static volatile sig_atomic_t stop_flag = 0;
static void on_sigint(int sig){ (void)sig; stop_flag = 1; }

/* ---- Estado en memoria (simple) ---- */
static char g_state[1024] = "NO_DATA";

/* ---- Parseo mínimo del request CoAP ---- */
typedef struct {
  uint8_t type;       // CON/NON/ACK/RST
  uint8_t tkl;        // token length (0..8)
  uint8_t code;       // 8 bits (class.detail)
  uint16_t mid;       // message id
  uint8_t token[8];   // hasta 8 bytes
  char uri_path[128]; // path decodificado, ej: "sensor" o "echo"
  const uint8_t* payload; // puntero a payload dentro del datagrama
  size_t payload_len;
} coap_req_t;

/* Lee delta/len "corto" o con extensión (13/14) */
static int read_nibble_ext(uint8_t v, const uint8_t **p, const uint8_t *end) {
  if (v < 13) return v;
  if (v == 13) { if (*p >= end) return -1; return 13 + *(*p)++; }
  if (v == 14) {
    if (*p + 1 >= end) return -1;
    int val = (int)(((*p)[0] << 8) | (*p)[1]);
    *p += 2;
    return 269 + val;
  }
  return -1; // 15 es reservado
}

/* Decodifica header, token, Uri-Path (concatenado con '/'), payload */
static int coap_parse(const uint8_t *buf, size_t len, coap_req_t *r) {
  if (len < 4) return -1;
  uint8_t ver = (buf[0] >> 6) & 0x03;
  if (ver != COAP_VER) return -1;
  r->type = (buf[0] >> 4) & 0x03;
  r->tkl  =  buf[0]       & 0x0F;
  r->code =  buf[1];
  r->mid  = (uint16_t)buf[2] << 8 | buf[3];
  if (4 + r->tkl > len || r->tkl > 8) return -1;
  memcpy(r->token, buf + 4, r->tkl);

  const uint8_t *p = buf + 4 + r->tkl;
  const uint8_t *end = buf + len;
  r->uri_path[0] = '\0';
  int last_opt = 0;

  while (p < end && *p != 0xFF) {
    uint8_t byte = *p++;
    int delta4 = (byte >> 4) & 0x0F;
    int len4   =  byte       & 0x0F;
    int delta  = read_nibble_ext(delta4, &p, end);
    int optlen = read_nibble_ext(len4, &p, end);
    if (delta < 0 || optlen < 0) return -1;
    int optnum = last_opt + delta;
    if (p + optlen > end) return -1;

    if (optnum == OPT_URI_PATH) {
      size_t seglen = (size_t)optlen;
      if (seglen > 0) {
        if (r->uri_path[0] && strlen(r->uri_path) < sizeof(r->uri_path)-1)
          strncat(r->uri_path, "/", sizeof(r->uri_path)-1);
        strncat(r->uri_path, (const char*)p, seglen);
      }
    }
    /* ignoramos otras opciones de la petición */
    p += optlen;
    last_opt = optnum;
  }

  if (p < end && *p == 0xFF) { // payload marker
    p++;
    r->payload = p;
    r->payload_len = (size_t)(end - p);
  } else {
    r->payload = NULL;
    r->payload_len = 0;
  }
  return 0;
}

/* ---- Construcción de respuesta ---- */
/* añade una opción (con manejo de delta corto/extendido) */
static int add_option(uint8_t *out, size_t cap, int *last,
                      int number, const uint8_t *val, size_t vlen) {
  if (cap < 1) return -1;
  int delta = number - *last;
  uint8_t *p = out;

  uint8_t dl, ll;
  uint8_t dext[2], lext[2];
  size_t dextn = 0, lextn = 0;

  if (delta < 13) dl = delta;
  else if (delta < 269) { dl = 13; dext[dextn++] = (uint8_t)(delta - 13); }
  else { dl = 14; int D = delta - 269; dext[dextn++] = (D >> 8) & 0xFF; dext[dextn++] = D & 0xFF; }

  if (vlen < 13) ll = (uint8_t)vlen;
  else if (vlen < 269) { ll = 13; lext[lextn++] = (uint8_t)(vlen - 13); }
  else { ll = 14; int L = (int)vlen - 269; lext[lextn++] = (L >> 8) & 0xFF; lext[lextn++] = L & 0xFF; }

  size_t need = 1 + dextn + lextn + vlen;
  if (cap < need) return -1;

  *p++ = (uint8_t)((dl << 4) | ll);
  for (size_t i = 0; i < dextn; i++) *p++ = dext[i];
  for (size_t i = 0; i < lextn; i++) *p++ = lext[i];
  if (vlen && val) { memcpy(p, val, vlen); p += vlen; }

  *last = number;
  return (int)need;
}

/* arma respuesta ACK/NON eco de token, CF=text/plain, payload opcional */
static size_t build_response(uint8_t *out, size_t cap,
                             uint8_t req_type, uint8_t tkl, const uint8_t *tok,
                             uint16_t mid, uint8_t code,
                             const uint8_t *payload, size_t plen) {
  if (cap < 4 + tkl) return 0;
  uint8_t type = (req_type == COAP_CON) ? COAP_ACK : COAP_NON;
  out[0] = (uint8_t)((COAP_VER << 6) | (type << 4) | (tkl & 0x0F));
  out[1] = code;
  out[2] = (uint8_t)(mid >> 8);
  out[3] = (uint8_t)(mid & 0xFF);
  memcpy(out + 4, tok, tkl);
  size_t pos = 4 + tkl;

  /* Opción: Content-Format: text/plain (número=12, valor=0) */
  int last = 0;
  uint8_t cf = CF_TEXT_PLAIN;
  int n = add_option(out + pos, cap - pos, &last, OPT_CONTENT_FORMAT, &cf, 1);
  if (n < 0) return 0;
  pos += (size_t)n;

  if (payload && plen > 0) {
    if (pos + 1 + plen > cap) return 0;
    out[pos++] = 0xFF;
    memcpy(out + pos, payload, plen);
    pos += plen;
  }
  return pos;
}

/* ---- Main loop ---- */
int main(void) {
  signal(SIGINT, on_sigint);

  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) { perror("socket"); return 1; }

  struct sockaddr_in srv = {0};
  srv.sin_family = AF_INET;
  srv.sin_addr.s_addr = htonl(INADDR_ANY);
  srv.sin_port = htons(COAP_PORT);

  if (bind(fd, (struct sockaddr*)&srv, sizeof(srv)) < 0) {
    perror("bind"); close(fd); return 1;
  }

  printf("CoAP server escuchando en 0.0.0.0:%d\n", COAP_PORT);
  uint8_t inbuf[1500], outbuf[1500];

  while (!stop_flag) {
    struct sockaddr_in cli;
    socklen_t clen = sizeof(cli);
    ssize_t n = recvfrom(fd, inbuf, sizeof(inbuf), 0,
                         (struct sockaddr*)&cli, &clen);
    if (n <= 0) continue;

    coap_req_t req;
    if (coap_parse(inbuf, (size_t)n, &req) != 0) {
      /* paquete inválido, ignora */
      continue;
    }

    /* Extrae método (código bajo): 0.01 GET, 0.02 POST, 0.03 PUT */
    uint8_t method = req.code;
    const char *path = req.uri_path[0] ? req.uri_path : "";
    /* Convierte payload a texto (truncado) */
    char body[1024]; size_t blen = 0;
    if (req.payload_len > 0) {
      blen = req.payload_len < sizeof(body)-1 ? req.payload_len : sizeof(body)-1;
      memcpy(body, req.payload, blen);
      body[blen] = '\0';
    } else {
      body[0] = '\0';
    }

    uint8_t code = COAP_404_NOTFOUND;
    char resp[1200]; size_t rlen = 0;

    if (strcmp(path, "echo") == 0 && method == COAP_POST) {
      /* POST /echo -> "echo: <payload>" (2.05) */
      snprintf(resp, sizeof(resp), "echo: %s", body);
      rlen = strlen(resp);
      code = COAP_205_CONTENT;

    } else if (strcmp(path, "sensor") == 0) {
      if (method == COAP_PUT) {
        /* PUT /sensor -> guarda estado y responde 2.04 */
        snprintf(g_state, sizeof(g_state), "%s", body);
        snprintf(resp, sizeof(resp), "UPDATED");
        rlen = strlen(resp);
        code = COAP_204_CHANGED;
      } else if (method == COAP_GET) {
        /* GET /sensor -> devuelve estado actual (2.05) */
        snprintf(resp, sizeof(resp), "%s", g_state);
        rlen = strlen(resp);
        code = COAP_205_CONTENT;
      } else {
        code = COAP_405_METHODNA; // otro método no permitido
        snprintf(resp, sizeof(resp), "METHOD_NOT_ALLOWED");
        rlen = strlen(resp);
      }
    } else {
      /* cualquier otra ruta -> 4.04 */
      snprintf(resp, sizeof(resp), "NOT_FOUND");
      rlen = strlen(resp);
      code = COAP_404_NOTFOUND;
    }

    size_t outlen = build_response(outbuf, sizeof(outbuf),
                                   req.type, req.tkl, req.token,
                                   req.mid, code,
                                   (const uint8_t*)resp, rlen);
    if (outlen > 0) {
      sendto(fd, outbuf, outlen, 0, (struct sockaddr*)&cli, clen);
    }
  }

  close(fd);
  puts("bye");
  return 0;
}
