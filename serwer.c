// server.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <errno.h>

#define SERVER_PORT 4000
#define MAX_PLOTTERS 32
#define MAX_UDP_BUF 1500

#define ALP_MAGIC 0x414C 

// Typy komunikatów ALP
#define MSG_TYPE_LOGIN_REQ      0x01
#define MSG_TYPE_LOGIN_ACK      0x02
#define MSG_TYPE_CONFIG_SEND    0x03
#define MSG_TYPE_CONFIG_ACK     0x04
#define MSG_TYPE_WORK_START     0x05
#define MSG_TYPE_HANDOVER_REQ   0x06
#define MSG_TYPE_HANDOVER_RESP  0x07
#define MSG_TYPE_IMAGE_DATA     0x08
#define MSG_TYPE_CHUNK_PROCESSED 0x09
#define MSG_TYPE_ACK            0x0A  

#pragma pack(push,1)
// Nagłówek komunikatu ALP
typedef struct {
    uint16_t magic;
    uint8_t  type;
    uint16_t payload_len;
    uint32_t seq_id;
    uint16_t sender_id;
    uint16_t checksum;           
} ALPHeader;

// Raport stanu żółwia
typedef struct {
    int16_t x;
    int16_t y;
    uint8_t angle;
    uint8_t processed_count;
} StatusPayload;
#pragma pack(pop)

// Informacje o zarejestrowanym plotterze
typedef struct {
    int active;
    uint16_t id;
    struct sockaddr_in addr;
} Plotter;
// Tablica plotterów
static Plotter plotters[MAX_PLOTTERS];
static uint16_t next_plotter_id = 1;

// Struktury L-systemu
typedef struct {
    char predecessor;
    char *successor;
} Rule;
typedef struct {
    char *alphabet;
    char *axiom;
    int depth;
    Rule *rules;
    int rule_count;
} LSystem;

//obszar rysowania
#define CANVAS_W 100
#define CANVAS_H 30
static char canvas[CANVAS_H][CANVAS_W][5];

// Rozmiar fragmentu programu
#define CHUNK_SIZE 5

uint16_t alp_checksum(const uint8_t *data, uint16_t len) {
    uint16_t sum = 0;
    for (uint16_t i = 0; i < len; i++) sum += data[i];
    return sum;
}

// Funkcje pomocnicze
static void trim(char *s) {
    char *end;
    while (isspace((unsigned char)*s)) s++;
    end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
}
// Wyszukiwanie zarejestrowanego plottera
static int find_plotter(struct sockaddr_in *addr) {
    for (int i = 0; i < MAX_PLOTTERS; i++) {
        if (plotters[i].active &&
            plotters[i].addr.sin_addr.s_addr == addr->sin_addr.s_addr &&
            plotters[i].addr.sin_port == addr->sin_port)
            return i;
    }
    return -1;
}
// Rejestracja nowego plottera
static int register_plotter(struct sockaddr_in *addr) {
    for (int i = 0; i < MAX_PLOTTERS; i++) {
        if (!plotters[i].active) {
            plotters[i].active = 1;
            plotters[i].id = next_plotter_id++;
            plotters[i].addr = *addr;
            return i;
        }
    }
    return -1;
}

// Wczytywanie definicji L-systemu z pliku
int parse_lsystem_file(const char *filename, LSystem *lsys) {
    FILE *f = fopen(filename, "r");
    if (!f) { perror("Blad otwarcia pliku"); return 0; }

    lsys->alphabet = NULL; lsys->axiom = NULL;
    lsys->depth = 0; lsys->rules = NULL; lsys->rule_count = 0;
    char line[256];

    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        trim(line);
        if (line[0] == '\0') continue;
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = '\0';
        char *key = line; char *value = colon + 1;
        trim(key); trim(value);

        if (strcmp(key, "ALPHABET") == 0) {
            char tmp[128]; int idx = 0;
            for (int i = 0; value[i]; i++) {
                if (value[i] != ',' && !isspace((unsigned char)value[i]))
                    tmp[idx++] = value[i];
            }
            tmp[idx] = '\0';
            lsys->alphabet = strdup(tmp);
        } else if (strcmp(key, "AXIOM") == 0) {
            lsys->axiom = strdup(value);
        } else if (strcmp(key, "DEPTH") == 0) {
            lsys->depth = atoi(value);
        } else if (strcmp(key, "RULE") == 0) {
            char lhs, rhs[256];
            if (sscanf(value, " %c -> %255s", &lhs, rhs) == 2) {
                lsys->rules = realloc(lsys->rules, (lsys->rule_count + 1) * sizeof(Rule));
                lsys->rules[lsys->rule_count].predecessor = lhs;
                lsys->rules[lsys->rule_count].successor = strdup(rhs);
                lsys->rule_count++;
            }
        }
    }
    fclose(f);
    return 1;
}
// Rozwijanie L-systemu
char *expand_lsystem(const LSystem *lsys) {
    char *cur = strdup(lsys->axiom);
    for (int d = 0; d < lsys->depth; d++) {
        size_t newlen = 1;
        for (char *p = cur; *p; p++) {
            int replaced = 0;
            for (int i = 0; i < lsys->rule_count; i++) {
                if (lsys->rules[i].predecessor == *p) {
                    newlen += strlen(lsys->rules[i].successor);
                    replaced = 1; break;
                }
            }
            if (!replaced) newlen++;
        }
        char *next = malloc(newlen);
        size_t pos = 0;
        for (char *p = cur; *p; p++) {
            int done = 0;
            for (int i = 0; i < lsys->rule_count; i++) {
                if (lsys->rules[i].predecessor == *p) {
                    size_t l = strlen(lsys->rules[i].successor);
                    memcpy(next + pos, lsys->rules[i].successor, l);
                    pos += l; done = 1; break;
                }
            }
            if (!done) next[pos++] = *p;
        }
        next[pos] = 0;
        free(cur);
        cur = next;
    }
    return cur;
}

// Czyszczenie bufora obrazu
static void canvas_clear(void) {
    for (int y = 0; y < CANVAS_H; y++)
        for (int x = 0; x < CANVAS_W; x++)
            strcpy(canvas[y][x], " ");
}
// Zapis bufora obrazu do pliku
static void canvas_save(const char *fname) {
    FILE *f = fopen(fname, "w");
    if (!f) { perror("Blad zapisu do pliku out"); return; }
    for (int y = 0; y < CANVAS_H; y++) {
        for (int x = 0; x < CANVAS_W; x++) {
            fputs(canvas[y][x], f);
        }
        fputc('\n', f);
    }
    fclose(f);
}

// Wysłanie potwierdzenia rejestracji plottera
static void send_login_ack(int sock, struct sockaddr_in *cli,
                           socklen_t clen, uint16_t assigned_id, uint32_t seq_id) {
    uint8_t out[sizeof(ALPHeader) + 2];
    ALPHeader *resp = (ALPHeader*)out;
    resp->magic = htons(ALP_MAGIC);
    resp->type = MSG_TYPE_LOGIN_ACK;
    resp->payload_len = htons(2);
    resp->seq_id = seq_id;
    resp->sender_id = htons(0);

    uint16_t pid = htons(assigned_id);
    memcpy(out + sizeof(ALPHeader), &pid, sizeof(pid));

    resp->checksum = htons(alp_checksum(
        out + sizeof(ALPHeader), 2));   

    sendto(sock, out, sizeof(out), 0, (struct sockaddr*)cli, clen);
}

// Stan żółwia po stronie serwera
typedef struct { int x,y,angle; } Turtle;

int main(int argc, char **argv) {
    if (argc < 2) { printf("Uzycie: %s <lsystem_file>\n", argv[0]); return 1; }
    // Wczytanie definicji L-systemu
    LSystem lsys;
    if (!parse_lsystem_file(argv[1], &lsys)) return 1;
    // Utworzenie gniazda UDP
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("Blad socketa"); return 1; }

    struct sockaddr_in srv = {0};
    srv.sin_family = AF_INET;
    srv.sin_addr.s_addr = INADDR_ANY;
    srv.sin_port = htons(SERVER_PORT);
    if (bind(sock, (struct sockaddr*)&srv, sizeof(srv)) < 0) { perror("Blad bindowania"); close(sock); return 1; }

    printf("Serwer ALP nasluchuje na porcie %d (No-Sim Mode)\n", SERVER_PORT);
    memset(plotters, 0, sizeof(plotters));

    // Rejestracja plotterów
    printf("Oczekiwanie na rejestracje 3 plotterow...\n");
    while (1) {
        uint8_t buf[MAX_UDP_BUF];
        struct sockaddr_in cli;
        socklen_t clen = sizeof(cli);
        ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr*)&cli, &clen);
        if (n < (ssize_t)sizeof(ALPHeader)) continue;
        ALPHeader hdr_net;
        memcpy(&hdr_net, buf, sizeof(hdr_net));
        if (ntohs(hdr_net.magic) != ALP_MAGIC) continue;

        // Sprawdzenie sumy kontrolnej
        uint16_t plen = ntohs(hdr_net.payload_len);
        uint16_t recv_sum = ntohs(hdr_net.checksum);
        uint16_t calc_sum = alp_checksum(
            buf + sizeof(ALPHeader), plen);

        if (recv_sum != calc_sum) continue;


        // Obsługa żądania rejestracji
        if (hdr_net.type == MSG_TYPE_LOGIN_REQ) {
            int idx = find_plotter(&cli);
            if (idx < 0) {
                idx = register_plotter(&cli);
                if (idx >= 0) printf("[ALP] Nowy plotter ID=%u\n", plotters[idx].id);
            }
            uint32_t seq = ntohl(hdr_net.seq_id);
            send_login_ack(sock, &cli, clen, plotters[idx].id, htonl(seq));
        }
        int cnt = 0;
        for (int i = 0; i < MAX_PLOTTERS; ++i) if (plotters[i].active) cnt++;
        if (cnt >= 3) break;
    }
    // Przypisanie plotterów do regionów
    int mapping[3] = {-1,-1,-1};
    int found = 0;
    for (int i = 0; i < MAX_PLOTTERS && found < 3; ++i) {
        if (plotters[i].active) mapping[found++] = i;
    }
    printf("Plotters przypisane.\n");
    // Przygotowanie bufora obrazu i programu
    canvas_clear();
    char *program = expand_lsystem(&lsys);
    size_t prog_len = strlen(program);
    printf("Dlugosc programu: %zu\n", prog_len);

    Turtle turtle = { CANVAS_W/2, CANVAS_H/2, 0 };
    int region_lefts[3] = {0, 34, 67};
    int region_rights[3] = {33, 66, 99};
    int active_map_index = 1; 

    size_t pc = 0;
    
    
    while (pc < prog_len) {
        int to_send = (int)((prog_len - pc) >= CHUNK_SIZE ? CHUNK_SIZE : (prog_len - pc));
        int payload_len = 9 + to_send; // 2+2+1+2+2 + commands

        uint8_t *pkt = malloc(sizeof(ALPHeader) + payload_len);
        ALPHeader *h = (ALPHeader*)pkt;
        h->magic = htons(ALP_MAGIC);
        h->type = MSG_TYPE_CONFIG_SEND;
        h->payload_len = htons(payload_len);
        h->seq_id = htonl((uint32_t)pc);
        h->sender_id = htons(0);

        // Przygotowanie danych sterujących
        uint8_t *p = pkt + sizeof(ALPHeader);
        int16_t sx = htons((int16_t)turtle.x);
        int16_t sy = htons((int16_t)turtle.y);
        memcpy(p, &sx, 2); p += 2;
        memcpy(p, &sy, 2); p += 2;
        *p = (uint8_t)turtle.angle; p += 1;
        int16_t rl = htons((int16_t)region_lefts[active_map_index]);
        int16_t rr = htons((int16_t)region_rights[active_map_index]);
        memcpy(p, &rl, 2); p += 2;
        memcpy(p, &rr, 2); p += 2;
        memcpy(p, program + pc, to_send);

        
        h->checksum = htons(alp_checksum(pkt + sizeof(ALPHeader), payload_len));

        // Wysłanie danych do aktywnego plottera
        int active_slot = mapping[active_map_index];
        struct sockaddr_in *dest = &plotters[active_slot].addr;
        sendto(sock, pkt, sizeof(ALPHeader) + payload_len, 0, (struct sockaddr*)dest, sizeof(*dest));
        free(pkt);

        int chunk_finished = 0;
        
        // Oczekiwanie na odpowiedź plottera
        while (!chunk_finished) {
            uint8_t in[MAX_UDP_BUF];
            struct sockaddr_in cli;
            socklen_t clen = sizeof(cli);
            ssize_t n = recvfrom(sock, in, sizeof(in), 0, (struct sockaddr*)&cli, &clen);
            if (n < (ssize_t)sizeof(ALPHeader)) continue;
            
            ALPHeader rh;
            memcpy(&rh, in, sizeof(rh));
            if (ntohs(rh.magic) != ALP_MAGIC) continue;
            
            uint8_t *payload_ptr = in + sizeof(ALPHeader);

            // Odbiór danych obrazu
            if (rh.type == MSG_TYPE_IMAGE_DATA) {
                uint16_t rplen = ntohs(rh.payload_len);
                if (rplen % 8 == 0) {
                    int count = rplen / 8;
                    for (int i = 0; i < count; ++i) {
                        int16_t nx, ny;
                        memcpy(&nx, payload_ptr + i*8, 2);
                        memcpy(&ny, payload_ptr + i*8 + 2, 2);
                        int x = ntohs(nx), y = ntohs(ny);
                        if (x >= 0 && x < CANVAS_W && y >= 0 && y < CANVAS_H) {
                            memcpy(canvas[y][x], payload_ptr + i*8 + 4, 4);
                            canvas[y][x][4] = '\0';
                        }
                    }
                }
            }
            // Przekazanie sterowania do innego plottera
            else if (rh.type == MSG_TYPE_CHUNK_PROCESSED) {
                // Plotter skończył chunk bez błędów
                StatusPayload st;
                memcpy(&st, payload_ptr, sizeof(StatusPayload));
                
                // AKTUALIZACJA STANU SERWERA DANYMI Z PLOTTERA
                turtle.x = ntohs(st.x);
                turtle.y = ntohs(st.y);
                turtle.angle = st.angle;
                
                // Przesuwamy wskaźnik o tyle, ile plotter przetworzył
                pc += st.processed_count;
                chunk_finished = 1;
            }
            else if (rh.type == MSG_TYPE_HANDOVER_REQ) {
                // HANDOVER: Plotter dotarł do granicy
                StatusPayload st;
                memcpy(&st, payload_ptr, sizeof(StatusPayload));

                // Aktualizacja stanu
                turtle.x = ntohs(st.x);
                turtle.y = ntohs(st.y);
                turtle.angle = st.angle;
                
                // Zmieniamy aktywny region na podstawie zgłoszonej pozycji X
                if (turtle.x < region_lefts[active_map_index]) active_map_index--;
                else if (turtle.x > region_rights[active_map_index]) active_map_index++;
                
                // Zabezpieczenie indeksu tablicy
                if (active_map_index < 0) active_map_index = 0;
                if (active_map_index > 2) active_map_index = 2;

                // Przesuwamy wskaźnik
                pc += st.processed_count;

                // Odsyłamy ACK
                uint8_t outresp[sizeof(ALPHeader)+2];
                ALPHeader *hr = (ALPHeader*)outresp;
                hr->magic = htons(ALP_MAGIC);
                hr->type = MSG_TYPE_HANDOVER_RESP;
                hr->payload_len = htons(2);
                hr->seq_id = rh.seq_id;
                hr->sender_id = htons(0);
                uint16_t npid = htons(plotters[mapping[active_map_index]].id);
                memcpy(outresp+sizeof(ALPHeader), &npid, 2);

                hr->checksum = htons(alp_checksum(outresp + sizeof(ALPHeader), 2));
                
                sendto(sock, outresp, sizeof(outresp), 0, (struct sockaddr*)&cli, clen);
                
                chunk_finished = 1;
            }
        }
    }

    canvas_save("out.txt");
    printf("Zakonczono. Wynik zapisano do out.txt\n");
    free(program);
    free(lsys.axiom);
    free(lsys.alphabet);
    for(int i=0; i<lsys.rule_count; ++i) free(lsys.rules[i].successor);
    free(lsys.rules);
    close(sock);
    return 0;
}
