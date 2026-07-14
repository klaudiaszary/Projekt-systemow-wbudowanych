// plotter1.ino

// Funkcje konwersji kolejności bajtów
#if defined(ARDUINO)
static inline uint16_t htons(uint16_t x) { return (x << 8) | (x >> 8); }
static inline uint16_t ntohs(uint16_t x) { return (x << 8) | (x >> 8); }
static inline uint32_t htonl(uint32_t x) {
  return ((x & 0x000000FFUL) << 24) | ((x & 0x0000FF00UL) << 8) |
         ((x & 0x00FF0000UL) >> 8) | ((x & 0xFF000000UL) >> 24);
}
static inline uint32_t ntohl(uint32_t x) { return htonl(x); }
#endif

#include <SPI.h>
#include <ZsutEthernet.h>
#include <ZsutEthernetUdp.h>

// Konfiguracja sieci plottera
byte MY_MAC[6] = { 0xDE,0xAD,0xBE,0xEF,0xFE,0x01 };
ZsutIPAddress MY_IP(192,168,56,105);
ZsutIPAddress SERVER_IP(192,168,56,104);
const unsigned int SERVER_PORT = 4000;
const unsigned int LOCAL_PORT = 50000;

ZsutEthernetUDP Udp;
// Stałe protokołu ALP
#define ALP_MAGIC 0x414C
// Typy komunikatów ALP
#define MSG_TYPE_LOGIN_REQ      0x01
#define MSG_TYPE_LOGIN_ACK      0x02
#define MSG_TYPE_CONFIG_SEND    0x03
#define MSG_TYPE_IMAGE_DATA     0x08
#define MSG_TYPE_HANDOVER_REQ   0x06
#define MSG_TYPE_HANDOVER_RESP  0x07
#define MSG_TYPE_CHUNK_PROCESSED 0x09 // NOWY TYP: Raport o stanie

#pragma pack(push,1)
// Nagłówek komunikatu ALP
typedef struct {
    uint16_t magic;
    uint8_t type;
    uint16_t payload_len;
    uint32_t seq_id;
    uint16_t sender_id;
    uint16_t checksum; //zmiana
} ALPHeader;
// Struktura piksela obrazu
typedef struct {
  int16_t x;
  int16_t y;
  uint8_t utf[4]; 
} PixelWire;

// Raport stanu żółwia
typedef struct {
    int16_t x;
    int16_t y;
    uint8_t angle;
    uint8_t processed_count;
} StatusPayload;
#pragma pack(pop)

// Obliczanie sumy kontrolnej payloadu
uint16_t alp_checksum(const uint8_t *data, uint16_t len) {
    uint16_t sum = 0;
    for (uint16_t i = 0; i < len; i++) {
        sum += data[i];
    }
    return sum;
}

// Identyfikator przydzielony przez serwer
int assigned_id = 0;

// Lokalny stan żółwia
struct LocalTurtle { int x; int y; int angle; } turtle;

// Bufor pikseli obrazu
PixelWire pixels[128]; 
int pix_count = 0;

// Granice przypisanego obszaru
int region_left = 0, region_right = 99;


// Linie
const uint8_t CHAR_H_LINE[] = {0xE2, 0x94, 0x81, 0x00}; // ━ (U+2501)
const uint8_t CHAR_V_LINE[] = {0xE2, 0x94, 0x83, 0x00}; // ┃ (U+2503)

// Narożniki 
const uint8_t CHAR_TL[]     = {0xE2, 0x94, 0x8F, 0x00}; // ┏ (U+250F)
const uint8_t CHAR_TR[]     = {0xE2, 0x94, 0x93, 0x00}; // ┓ (U+2513)
const uint8_t CHAR_BL[]     = {0xE2, 0x94, 0x97, 0x00}; // ┗ (U+2517)
const uint8_t CHAR_BR[]     = {0xE2, 0x94, 0x9B, 0x00}; // ┛ (U+251B)

// Skrzyżowanie
const uint8_t CHAR_CROSS[]  = {0xE2, 0x95, 0x8B, 0x00}; // ╋ (U+254B)

// f pomocnicze
bool is_corner(const uint8_t* c) {
    return (memcmp(c, CHAR_TL, 3)==0 || memcmp(c, CHAR_TR, 3)==0 || 
            memcmp(c, CHAR_BL, 3)==0 || memcmp(c, CHAR_BR, 3)==0 ||
            memcmp(c, CHAR_CROSS, 3)==0);
}
bool is_line(const uint8_t* c) {
    return (memcmp(c, CHAR_H_LINE, 3)==0 || memcmp(c, CHAR_V_LINE, 3)==0);
}

// Dodawanie piksela do bufora obrazu
void add_pixel(int x, int y, const uint8_t* new_char) {
    // Sprawdzenie kolizji piksela
    for(int i=0; i<pix_count; i++) {
        if(ntohs(pixels[i].x) == x && ntohs(pixels[i].y) == y) {
            uint8_t* old_char = pixels[i].utf;
            
            // Narożnik nadpisuje istniejący znak
            if(is_corner(new_char)) {
                memcpy(pixels[i].utf, new_char, 4);
                return;
            }
            // Linia nie nadpisuje narożnika
            if(is_corner(old_char) && is_line(new_char)) {
                return; 
            }
            // Przecięcie linii pionowej i poziomej
            if(is_line(old_char) && is_line(new_char)) {
                if(memcmp(old_char, new_char, 3) != 0) {
                   memcpy(pixels[i].utf, CHAR_CROSS, 4); 
                }
                return;
            }
            // Nadpisanie znaku
            memcpy(pixels[i].utf, new_char, 4);
            return;
        }
    }

     // Dodanie nowego piksela
    if (pix_count >= (int)(sizeof(pixels)/sizeof(pixels[0]))) return;
    pixels[pix_count].x = htons((int16_t)x);
    pixels[pix_count].y = htons((int16_t)y);
    memcpy(pixels[pix_count].utf, new_char, 4); 
    pix_count++;
}

// Logika Narożników
const uint8_t* get_corner(int old_a, int new_a) {
    // 0=góra, 1=prawo, 2=dół, 3=lewo
    if (old_a == 0) { 
        if(new_a == 1) return CHAR_TL; 
        if(new_a == 3) return CHAR_TR; 
    } 
    if (old_a == 1) { 
        if(new_a == 2) return CHAR_TR; 
        if(new_a == 0) return CHAR_BR; 
    } 
    if (old_a == 2) { 
        if(new_a == 3) return CHAR_BR; 
        if(new_a == 1) return CHAR_BL; 
    } 
    if (old_a == 3) { 
        if(new_a == 0) return CHAR_BL; 
        if(new_a == 2) return CHAR_TL; 
    } 
    return CHAR_CROSS; 
}

// Wysłanie żądania rejestracji
void send_login_req() {
  uint8_t pkt[sizeof(ALPHeader)];
  ALPHeader *h = (ALPHeader*)pkt;
  h->magic = htons(ALP_MAGIC);
  h->type = MSG_TYPE_LOGIN_REQ;
  h->payload_len = htons(0);
  h->seq_id = htonl(1);
  h->sender_id = htons(0);
  h->checksum = htons(0);//zmiana
  Udp.beginPacket(SERVER_IP, SERVER_PORT);
  Udp.write(pkt, sizeof(pkt));
  Udp.endPacket();
  Serial.println("LOGIN_REQ wyslany");
}
// Wysłanie danych obrazu do serwera
void send_image_data() {
  if (pix_count == 0) return; // Nie wysyłaj pustych
  ALPHeader h;
  int payload_len = pix_count * sizeof(PixelWire);
  h.magic = htons(ALP_MAGIC);
  h.type = MSG_TYPE_IMAGE_DATA;
  h.payload_len = htons(payload_len);
  h.seq_id = htonl(0);
  h.sender_id = htons((uint16_t)assigned_id);
  h.checksum = htons(alp_checksum((uint8_t*)pixels, payload_len)); 

  Udp.beginPacket(SERVER_IP, SERVER_PORT);
  Udp.write((uint8_t*)&h, sizeof(ALPHeader));
  Udp.write((uint8_t*)pixels, payload_len);
  Udp.endPacket();
  Serial.print("Wyslano IMAGE_DATA, piksele: ");
  Serial.println(pix_count);
  pix_count = 0;
}

// Wysłanie raportu stanu żółwia
void send_status(uint8_t type, int processed_count) {
  uint8_t out[sizeof(ALPHeader) + sizeof(StatusPayload)];
  ALPHeader *h = (ALPHeader*)out;
  h->magic = htons(ALP_MAGIC);
  h->type = type;
  h->payload_len = htons(sizeof(StatusPayload));
  h->seq_id = htonl(0);
  h->sender_id = htons((uint16_t)assigned_id);
  
  StatusPayload *pl = (StatusPayload*)(out + sizeof(ALPHeader));
  pl->x = htons((int16_t)turtle.x);
  pl->y = htons((int16_t)turtle.y);
  pl->angle = (uint8_t)(turtle.angle & 0xFF);
  pl->processed_count = (uint8_t)processed_count;

  h->checksum = htons(
        alp_checksum(out + sizeof(ALPHeader),
                     sizeof(StatusPayload))
    );                                     // ZMIANA POD UDP

  Udp.beginPacket(SERVER_IP, SERVER_PORT);
  Udp.write(out, sizeof(out));
  Udp.endPacket();
  Serial.print("Wyslano STATUS typ="); Serial.println(type);
}

// Inicjalizacja plottera
void setup() {
  Serial.begin(115200);
  while (!Serial) { ; }
  Serial.println("Plotter startuje ...");
  ZsutEthernet.begin(MY_MAC, MY_IP);
  delay(500);
  if (Udp.begin(LOCAL_PORT) == 0) {
    Serial.println("Blad startu UDP");
    while (1) delay(1000);
  }
  send_login_req();

  unsigned long t0 = millis();
  bool got = false;

  // Oczekiwanie na potwierdzenie rejestracji
  while (millis() - t0 < 3000) {
    int plen = Udp.parsePacket();
    if (plen > 0) {
      uint8_t inbuf[1500];
      int n = Udp.read(inbuf, sizeof(inbuf));
      if (n < (int)sizeof(ALPHeader)) continue;

      ALPHeader *rh = (ALPHeader*)inbuf;
      if (ntohs(rh->magic) != ALP_MAGIC) continue;


      uint8_t *payload = inbuf + sizeof(ALPHeader);
      uint16_t len = ntohs(rh->payload_len);

      if (ntohs(rh->checksum) != alp_checksum(payload, len)) continue; 

      if (rh->type == MSG_TYPE_LOGIN_ACK && ntohs(rh->payload_len) >= 2) {
        int16_t pid;
        memcpy(&pid, inbuf + sizeof(ALPHeader), 2);
        assigned_id = ntohs((uint16_t)pid);
        Serial.print("Przypisane ID = "); Serial.println(assigned_id);
        got = true;
        break;
      }
    }
    delay(50);
  }
  if (!got) Serial.println("Brak LOGIN_ACK - kontunuuje");
  turtle.x = 50; turtle.y = 15; turtle.angle = 0;
}

// Główna pętla plottera
void loop() {
  int plen = Udp.parsePacket();
  if (plen <= 0) { delay(20); return; }

  uint8_t buf[1500];
  int n = Udp.read(buf, sizeof(buf));
  if (n < (int)sizeof(ALPHeader)) return;
  ALPHeader header;
  memcpy(&header, buf, sizeof(header));
  if (ntohs(header.magic) != ALP_MAGIC) return;
  
  uint8_t *payload = buf + sizeof(ALPHeader);
  uint16_t payload_len = ntohs(header.payload_len);

// Sprawdzenie sumy kontrolnej
  uint16_t calc_sum = alp_checksum(payload, payload_len);
  uint16_t recv_sum = ntohs(header.checksum);

  if (recv_sum != calc_sum) {
      Serial.print("ZLY CHECKSUM Typ: "); 
      Serial.print(header.type);
      Serial.print(" | Len: "); 
      Serial.print(payload_len);
      Serial.print(" | Recv: "); 
      Serial.print(recv_sum);
      Serial.print(" | Calc: "); 
      Serial.println(calc_sum);
      return; // Odrzucamy pakiet
  }

  if (header.type == MSG_TYPE_CONFIG_SEND) {
    if (payload_len < 9) return;
    int16_t sx, sy, rl, rr;
    memcpy(&sx, payload + 0, 2);
    memcpy(&sy, payload + 2, 2);
    uint8_t sangle = *(payload + 4);
    memcpy(&rl, payload + 5, 2);
    memcpy(&rr, payload + 7, 2);
    sx = ntohs((uint16_t)sx); sy = ntohs((uint16_t)sy);
    rl = ntohs((uint16_t)rl); rr = ntohs((uint16_t)rr);
    region_left = rl; region_right = rr;
    turtle.x = sx; turtle.y = sy; turtle.angle = (int)sangle;
    int cmd_len = payload_len - 9;
    char *cmds = (char*)(payload + 9);

    pix_count = 0;
    int processed = 0;

    // Przetwarzanie poleceń grafiki żółwia
    for (int i = 0; i < cmd_len; ++i) {
      char c = cmds[i];
      if (c == '+' || c == '-') {
          // TURN
          int old_angle = turtle.angle;
          if (c == '+') turtle.angle = (turtle.angle + 1) & 3;
          else turtle.angle = (turtle.angle + 3) & 3;
          
          const uint8_t* corner = get_corner(old_angle, turtle.angle);
          add_pixel(turtle.x, turtle.y, corner);
          processed++;
      }
      else if (c == 'F') {
          const uint8_t* line_char = (turtle.angle % 2 == 0) ? CHAR_V_LINE : CHAR_H_LINE;

          int nx = turtle.x;
          int ny = turtle.y;

          if (turtle.angle == 0) ny--;
          else if (turtle.angle == 1) nx++;
          else if (turtle.angle == 2) ny++;
          else nx--;

          // granice świata
          if (nx < 0 || nx >= 100 || ny < 0 || ny >= 30) {
              turtle.x = nx;
              turtle.y = ny;
              processed++;
              break;
          }

          // region handover
          if (nx < region_left || nx >= region_right) {
              turtle.x = nx;
              turtle.y = ny;
              processed++;
              send_image_data();
              send_status(MSG_TYPE_HANDOVER_REQ, processed);
              return;
          }

          turtle.x = nx;
          turtle.y = ny;
          add_pixel(turtle.x, turtle.y, line_char);
          
          processed++;
      }
      else{   
          processed++;     
      }
    }
    send_image_data();
    send_status(MSG_TYPE_CHUNK_PROCESSED, processed);
  }
}
