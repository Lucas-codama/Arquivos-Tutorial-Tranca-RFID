#include <WiFi.h>
#include <HTTPClient.h>
#include <PN532_HSU.h>
#include <PN532.h>
#include <Keypad.h>
#include <Preferences.h>
#include <time.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

const char* NOME_WIFI = "DIGITE_O_NOME_DA_REDE";
const char* SENHA_WIFI = "DIGITE_A_SENHA_DA_REDE";

const char* URL_SCRIPT ="COLE_AQUI_A_URL_DO_APP_SCRIPT";

const char* TOKEN_API = "RFID-ESCANINHOS-2026-SEGURANCA-123456789";

const char* ID_ESCANINHO = "ESC-01";
const char* ID_DISPOSITIVO = "ESP32-ESC-01";
const char* NOME_DISPOSITIVO = "Controlador Escaninho 01";
const char* VERSAO_FIRMWARE = "9.0.0-STABLE-NVS";

const uint32_t VERSAO_CACHE = 3;
const uint32_t EPOCH_MINIMO_VALIDO = 1700000000UL;

const uint32_t VALIDADE_CACHE_SEGUNDOS = 24UL * 60UL * 60UL * 10UL;
const unsigned long INTERVALO_REVALIDACAO_MS = 60UL * 60UL * 1000UL;
const unsigned long RETENTATIVA_REVALIDACAO_MS = 60UL * 1000UL;
const unsigned long INTERVALO_HEARTBEAT = 60000UL;
const unsigned long INTERVALO_DIAGNOSTICO = 60000UL;
const unsigned long INTERVALO_SAUDE_PN532 = 60000UL;
const unsigned long INTERVALO_RECUPERACAO_PN532 = 5000UL;
const unsigned long TIMEOUT_CONEXAO_HTTP = 5000UL;
const unsigned long TIMEOUT_HTTP = 10000UL;

// ----- OLED -----
const int PINO_SDA_OLED = 4;
const int PINO_SCL_OLED = 5;
const int LARGURA_OLED = 128;
const int ALTURA_OLED = 64;
const uint8_t ENDERECO_OLED = 0x3C;

Adafruit_SSD1306 oled(LARGURA_OLED, ALTURA_OLED, &Wire, -1);

bool oledFuncionando = false;
bool mensagemTemporariaOLED = false;
unsigned long horarioRetornoOLED = 0;
// ----------------

// ----- PN532 -----
HardwareSerial serialPN532(1);
PN532_HSU pn532HSU(serialPN532);
PN532 nfc(pn532HSU);

const int PINO_RX_PN532 = 17;
const int PINO_TX_PN532 = 18;
const uint16_t TIMEOUT_RFID = 20;

bool pn532Funcionando = false;
unsigned long horarioUltimaVerificacaoPN532 = 0;
// -----------------

// ----- RELE -----
const int PINO_RELE = 16;
const unsigned long TEMPO_RELE_LIGADO = 2500UL;
bool releLigado = false;
unsigned long horarioReleLigado = 0;
// ----------------

// ----- TECLADO -----
const byte LINHAS = 4;
const byte COLUNAS = 3;

char teclas[LINHAS][COLUNAS] = {
  {'1', '2', '3'}, 
  {'4', '5', '6'}, 
  {'7', '8', '9'}, 
  {'*', '0', '#'}
};

byte pinosLinhas[LINHAS] = {12, 11, 10, 9};
byte pinosColunas[COLUNAS] = {46, 3, 8};
Keypad teclado = Keypad(makeKeymap(teclas), pinosLinhas, pinosColunas, LINHAS, COLUNAS);

const char ORDEM_TECLAS[] = "123456789*0#";

bool teclaPressionada[12] = {false};
char senhaDigitada[7] = {'\0'};
uint8_t tamanhoSenha = 0;
// -------------------

// ----- RFID -----
char ultimoUIDLido[24] = {'\0'};
unsigned long horarioUltimaLeitura = 0;
const unsigned long INTERVALO_MESMA_CARTEIRINHA = 3000UL;
// ----------------

// ----- CACHE -----
const uint8_t MAX_RFID_CACHE = 64;
const uint8_t MAX_PIN_CACHE = 32;

enum TipoCredencial : uint8_t { CRED_RFID = 1, CRED_PIN = 2 };

enum TipoRequisicao : uint8_t { REQ_RFID_INICIAL = 1, REQ_RFID_REVALIDACAO = 2, REQ_PIN_INICIAL = 3, REQ_PIN_REVALIDACAO = 4 };

struct EntradaCache {
  uint8_t usada;
  char valor[24];
  uint32_t validadoEpoch;
  uint32_t ultimaRevalidacaoEpoch;
};

struct CredencialPendente {
  bool usada;
  TipoCredencial tipo;
  char valor[24];
};

struct TemposHTTP {
  uint32_t postMs;
  uint32_t redirectMs;
  uint32_t totalMs;
  int codigoPost;
  int codigoFinal;
};

struct RequisicaoRede {
  uint32_t id;
  TipoRequisicao tipo;
  char valor[24];
  bool abrirSeAutorizado;
  uint32_t inicioAcessoMs;
};

struct ResultadoRede {
  uint32_t id;
  TipoRequisicao tipo;
  char valor[24];
  bool abrirSeAutorizado;
  bool comunicacaoOk;
  bool autorizado;
  bool cadastrado;
  uint32_t inicioAcessoMs;
  TemposHTTP tempos;
};

EntradaCache cacheRFID[MAX_RFID_CACHE];
EntradaCache cachePIN[MAX_PIN_CACHE];

const uint8_t MAX_PENDENTES = 32;

CredencialPendente pendentes[MAX_PENDENTES];

Preferences preferences;

QueueHandle_t filaPrioritaria = nullptr;
QueueHandle_t filaBackground = nullptr;
QueueHandle_t filaResultados = nullptr;

uint32_t proximoIdRequisicao = 1;

unsigned long horarioUltimoDiagnostico = 0;

// -----------------

void mostrarTelaInicialOLED() {
  if (!oledFuncionando) return;

  mensagemTemporariaOLED = false;
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(13, 8);
  oled.println("CONTROLE DE ACESSO");

  oled.setCursor(25, 29);
  oled.println("Digite o PIN");

  oled.setCursor(10, 46);
  oled.println("ou aproxime o RFID");

  oled.display();
}

void mostrarSenhaOLED() {
  if (!oledFuncionando) return;

  mensagemTemporariaOLED = false;
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(49, 5);
  oled.println("SENHA");

  oled.setTextSize(2);
  oled.setCursor(28, 23);

  if (tamanhoSenha == 0) oled.print("------");
  else oled.print(senhaDigitada);

  oled.setTextSize(1);
  oled.setCursor(12, 52);
  oled.print("* limpar   # OK");

  oled.display();
}

void mostrarMensagemOLED(const char* linha1, const char* linha2, unsigned long duracao) {
  if (!oledFuncionando) return;

  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(20, 18);
  oled.println(linha1);

  oled.setCursor(20, 38);
  oled.println(linha2);

  oled.display();
  mensagemTemporariaOLED = true;
  horarioRetornoOLED = millis() + duracao;
}

void mostrarAcessoOLED(bool autorizado) {
  if (!oledFuncionando) return;

  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(2);
  oled.setCursor(28, 10);
  oled.println("ACESSO");

  oled.setCursor(16, 37);
  if (autorizado) oled.println("LIBERADO");
  else oled.println("RECUSADO");

  oled.display();
  mensagemTemporariaOLED = true;
  horarioRetornoOLED = millis() + 2500UL;
}

void atualizarOLED() {
  if (!oledFuncionando || !mensagemTemporariaOLED) return;

  if ((int32_t)(millis() - horarioRetornoOLED) >= 0) {
    mensagemTemporariaOLED = false;
    mostrarTelaInicialOLED();
  }
}

void inicializarOLED() {
  Wire.begin(PINO_SDA_OLED, PINO_SCL_OLED);

  if (!oled.begin(SSD1306_SWITCHCAPVCC, ENDERECO_OLED)) {
    Serial.println("OLED nao encontrada.");
    oledFuncionando = false;
    return;
  }

  oledFuncionando = true;

  oled.clearDisplay();
  oled.display();
  mostrarTelaInicialOLED();
}

void copiarTexto(const char* origem, char* destino, size_t tamanho) {
  if (tamanho == 0) return;

  strncpy(destino, origem, tamanho - 1);
  destino[tamanho - 1] = '\0';
}

uint32_t obterEpochAtual() {
  time_t agora = time(nullptr);

  if (agora < EPOCH_MINIMO_VALIDO) return 0;

  return (uint32_t)agora;
}

void imprimirMemoria(const char* origem) {
  Serial.println();
  Serial.print("[MEMORIA] ");
  Serial.println(origem);
  Serial.print("Heap livre: ");
  Serial.print(ESP.getFreeHeap());
  Serial.println(" bytes");
  Serial.print("Menor heap livre: ");
  Serial.print(ESP.getMinFreeHeap());
  Serial.println(" bytes");
  Serial.print("Maior bloco livre: ");
  Serial.print(ESP.getMaxAllocHeap());
  Serial.println(" bytes");
  Serial.println();
}

bool conectarWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;

  Serial.println();
  Serial.print("Conectando ao Wi-Fi");

  WiFi.mode(WIFI_STA);
  WiFi.begin(NOME_WIFI, SENHA_WIFI);
  unsigned long inicioTentativa = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - inicioTentativa < 15000UL) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Wi-Fi conectado!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("RSSI: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");

    return true;
  }

  Serial.println("Wi-Fi não conectado.");

  return false;
}

void inicializarRele() {
  pinMode(PINO_RELE, OUTPUT_OPEN_DRAIN);
  digitalWrite(PINO_RELE, HIGH);
  releLigado = false;

  Serial.println();
  Serial.println("Relé inicializado.");
  Serial.print("GPIO 16 inicial: ");
  Serial.println(digitalRead(PINO_RELE));
}

void acionarRele() {
  Serial.println();
  Serial.println("==============================");
  Serial.println("ACIONANDO RELÉ");
  digitalWrite(PINO_RELE, LOW);

  releLigado = true;
  horarioReleLigado = millis();
  Serial.println("CADEADO LIBERADO");
  Serial.println("==============================");

  Serial.println();
}

void atualizarRele() {
  if (!releLigado) return;

  if (millis() - horarioReleLigado >= TEMPO_RELE_LIGADO) {
    digitalWrite(PINO_RELE, HIGH);
    releLigado = false;
    Serial.println();
    Serial.println("CADEADO BLOQUEADO");
    Serial.println();
  }
}

bool iniciarPN532() {
  Serial.println();
  Serial.println("Inicializando PN532...");

  serialPN532.begin(115200, SERIAL_8N1, PINO_RX_PN532, PINO_TX_PN532);
  nfc.begin();
  uint32_t versaoPN532 = nfc.getFirmwareVersion();

  if (!versaoPN532) {
    Serial.println("ERRO: PN532 não encontrado.");
    pn532Funcionando = false;
    horarioUltimaVerificacaoPN532 = millis();
    return false;
  }

  Serial.print("PN532 encontrado: 0x");
  Serial.println(versaoPN532, HEX);

  nfc.SAMConfig();
  pn532Funcionando = true;
  horarioUltimaVerificacaoPN532 = millis();

  Serial.println("RFID inicializado.");
  return true;
}

void inicializarLeitorRFID() {
  while (!iniciarPN532()) {
    Serial.println("Nova tentativa do PN532 em 2 segundos...");
    delay(2000);
  }
}

bool recuperarPN532() {
  Serial.println();
  Serial.println("[PN532] Tentando recuperar leitor...");
  while (serialPN532.available()) serialPN532.read();

  delay(100);

  bool resultado = iniciarPN532();

  if (resultado) {
    Serial.println("[PN532] Leitor recuperado.");
    return true;
  }

  Serial.println("[PN532] Recuperação falhou.");
  return false;
}

void verificarSaudePN532() {
  unsigned long intervalo = pn532Funcionando ? INTERVALO_SAUDE_PN532 : INTERVALO_RECUPERACAO_PN532;

  if (millis() - horarioUltimaVerificacaoPN532 < intervalo) return;

  horarioUltimaVerificacaoPN532 = millis();

  if (!pn532Funcionando) {
    recuperarPN532();
    return;
  }

  uint32_t versao = nfc.getFirmwareVersion();

  if (!versao) {
    Serial.println();
    Serial.println("[PN532] Leitor deixou de responder.");
    pn532Funcionando = false;
    recuperarPN532();
    return;
  }

  nfc.SAMConfig();
  Serial.println("[PN532] Comunicação OK.");
}

String converterUIDParaTexto(uint8_t uid[], uint8_t tamanhoUID) {
  String uidTexto = "";
  uidTexto.reserve(tamanhoUID * 2);
  for (uint8_t indice = 0; indice < tamanhoUID; indice++) {
    if (uid[indice] < 0x10) uidTexto += "0";
    uidTexto += String(uid[indice], HEX);
  }

  uidTexto.toUpperCase();
  return uidTexto;
}

EntradaCache* obterCache(TipoCredencial tipo) {
  if (tipo == CRED_RFID) return cacheRFID;
  return cachePIN;
}

uint8_t tamanhoCache(TipoCredencial tipo) {
  if (tipo == CRED_RFID) return MAX_RFID_CACHE;
  return MAX_PIN_CACHE;
}

void salvarCache(TipoCredencial tipo) {
  if (tipo == CRED_RFID) preferences.putBytes("rfidcache", cacheRFID, sizeof(cacheRFID));
  else preferences.putBytes("pincache", cachePIN, sizeof(cachePIN));
}

uint8_t contarCache(TipoCredencial tipo) {
  EntradaCache* cache = obterCache(tipo);
  uint8_t limite = tamanhoCache(tipo);
  uint8_t quantidade = 0;
  for (uint8_t i = 0; i < limite; i++)
    if (cache[i].usada) quantidade++;
  return quantidade;
}

void carregarCaches() {
  memset(cacheRFID, 0, sizeof(cacheRFID));
  memset(cachePIN, 0, sizeof(cachePIN));
  preferences.begin("acesso", false);
  uint32_t versao = preferences.getUInt("cachever", 0);
  if (versao != VERSAO_CACHE) {
    preferences.clear();
    preferences.putUInt("cachever", VERSAO_CACHE);
    Serial.println("Novo cache NVS criado.");
    return;
  }
  size_t tamanhoRFID = preferences.getBytesLength("rfidcache");
  if (tamanhoRFID == sizeof(cacheRFID)) preferences.getBytes("rfidcache", cacheRFID, sizeof(cacheRFID));
  size_t tamanhoPIN = preferences.getBytesLength("pincache");
  if (tamanhoPIN == sizeof(cachePIN)) preferences.getBytes("pincache", cachePIN, sizeof(cachePIN));
  for (uint8_t i = 0; i < MAX_RFID_CACHE; i++) cacheRFID[i].valor[sizeof(cacheRFID[i].valor) - 1] = '\0';
  for (uint8_t i = 0; i < MAX_PIN_CACHE; i++) cachePIN[i].valor[sizeof(cachePIN[i].valor) - 1] = '\0';
  Serial.print("RFIDs carregados da NVS: ");
  Serial.println(contarCache(CRED_RFID));
  Serial.print("PINs carregados da NVS: ");
  Serial.println(contarCache(CRED_PIN));
}

int encontrarNoCache(TipoCredencial tipo, const char* valor) {
  EntradaCache* cache = obterCache(tipo);
  uint8_t limite = tamanhoCache(tipo);
  for (uint8_t i = 0; i < limite; i++)
    if (cache[i].usada && strcmp(cache[i].valor, valor) == 0) return i;
  return -1;
}

void removerDoCache(TipoCredencial tipo, const char* valor) {
  int indice = encontrarNoCache(tipo, valor);
  if (indice < 0) return;
  EntradaCache* cache = obterCache(tipo);
  memset(&cache[indice], 0, sizeof(EntradaCache));
  salvarCache(tipo);
}

int encontrarPosicaoCache(TipoCredencial tipo) {
  EntradaCache* cache = obterCache(tipo);
  uint8_t limite = tamanhoCache(tipo);
  for (uint8_t i = 0; i < limite; i++)
    if (!cache[i].usada) return i;
  uint8_t indiceMaisAntigo = 0;
  uint32_t menorTempo = cache[0].validadoEpoch;
  for (uint8_t i = 1; i < limite; i++) {
    if (cache[i].validadoEpoch < menorTempo) {
      menorTempo = cache[i].validadoEpoch;
      indiceMaisAntigo = i;
    }
  }
  return indiceMaisAntigo;
}

void autorizarNoCache(TipoCredencial tipo, const char* valor) {
  int indice = encontrarNoCache(tipo, valor);
  if (indice < 0) indice = encontrarPosicaoCache(tipo);
  EntradaCache* cache = obterCache(tipo);
  cache[indice].usada = 1;
  copiarTexto(valor, cache[indice].valor, sizeof(cache[indice].valor));
  uint32_t agora = obterEpochAtual();
  cache[indice].validadoEpoch = agora;
  cache[indice].ultimaRevalidacaoEpoch = agora;
  salvarCache(tipo);
  Serial.print(tipo == CRED_RFID ? "[CACHE RFID] Total armazenado: " : "[CACHE PIN] Total armazenado: ");
  Serial.println(contarCache(tipo));
}

bool cacheValido(TipoCredencial tipo, const char* valor) {
  int indice = encontrarNoCache(tipo, valor);
  if (indice < 0) return false;
  uint32_t agora = obterEpochAtual();
  if (agora == 0) return false;
  EntradaCache* cache = obterCache(tipo);
  if (cache[indice].validadoEpoch == 0) return false;
  uint32_t idade = agora - cache[indice].validadoEpoch;
  if (idade > VALIDADE_CACHE_SEGUNDOS) {
    Serial.println(tipo == CRED_RFID ? "[CACHE RFID] Entrada expirou." : "[CACHE PIN] Entrada expirou.");
    removerDoCache(tipo, valor);
    return false;
  }
  return true;
}

bool precisaRevalidar(TipoCredencial tipo, const char* valor) {
  int indice = encontrarNoCache(tipo, valor);
  if (indice < 0) return false;
  uint32_t agora = obterEpochAtual();
  if (agora == 0) return false;
  EntradaCache* cache = obterCache(tipo);
  if (cache[indice].ultimaRevalidacaoEpoch == 0) return true;
  uint32_t intervalo = agora - cache[indice].ultimaRevalidacaoEpoch;
  return (intervalo >= 3600UL);
}

void marcarRevalidacao(TipoCredencial tipo, const char* valor) {
  int indice = encontrarNoCache(tipo, valor);
  if (indice < 0) return;
  uint32_t agora = obterEpochAtual();
  if (agora == 0) return;
  EntradaCache* cache = obterCache(tipo);
  cache[indice].ultimaRevalidacaoEpoch = agora;
}

void marcarRetentativaRevalidacao(TipoCredencial tipo, const char* valor) {
  int indice = encontrarNoCache(tipo, valor);
  if (indice < 0) return;
  uint32_t agora = obterEpochAtual();
  if (agora == 0) return;
  EntradaCache* cache = obterCache(tipo);
  if (agora > 3540UL) cache[indice].ultimaRevalidacaoEpoch = agora - 3540UL;
}


bool existePendente(TipoCredencial tipo, const char* valor) {
  for (uint8_t i = 0; i < MAX_PENDENTES; i++)
    if (pendentes[i].usada && pendentes[i].tipo == tipo && strcmp(pendentes[i].valor, valor) == 0) return true;
  return false;
}

bool adicionarPendente(TipoCredencial tipo, const char* valor) {
  if (existePendente(tipo, valor)) return true;
  for (uint8_t i = 0; i < MAX_PENDENTES; i++) {
    if (!pendentes[i].usada) {
      pendentes[i].usada = true;
      pendentes[i].tipo = tipo;
      copiarTexto(valor, pendentes[i].valor, sizeof(pendentes[i].valor));
      return true;
    }
  }
  return false;
}

void removerPendente(TipoCredencial tipo, const char* valor) {
  for (uint8_t i = 0; i < MAX_PENDENTES; i++) {
    if (pendentes[i].usada && pendentes[i].tipo == tipo && strcmp(pendentes[i].valor, valor) == 0) {
      memset(&pendentes[i], 0, sizeof(CredencialPendente));
      return;
    }
  }
}

TipoCredencial credencialDaRequisicao(TipoRequisicao tipo) {
  if (tipo == REQ_PIN_INICIAL || tipo == REQ_PIN_REVALIDACAO) return CRED_PIN;
  return CRED_RFID;
}

bool requisicaoEhRevalidacao(TipoRequisicao tipo) {
  return (tipo == REQ_RFID_REVALIDACAO || tipo == REQ_PIN_REVALIDACAO);
}


String criarDadosComuns() {
  String corpo = "";
  corpo.reserve(256);
  corpo += "\"token\":\"" + String(TOKEN_API) + "\",";
  corpo += "\"escaninho\":\"" + String(ID_ESCANINHO) + "\",";
  corpo += "\"dispositivo\":\"" + String(ID_DISPOSITIVO) + "\",";
  corpo += "\"nomeDispositivo\":\"" + String(NOME_DISPOSITIVO) + "\",";
  corpo += "\"firmware\":\"" + String(VERSAO_FIRMWARE) + "\",";
  corpo += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  corpo += "\"rssi\":" + String(WiFi.RSSI());
  return corpo;
}

bool respostaAutorizada(const String& resposta) {
  if (resposta.indexOf("\"autorizado\":true") >= 0) return true;
  if (resposta.indexOf("\"autorizado\": true") >= 0) return true;
  if (resposta.indexOf("\"status\":\"AUTORIZADO\"") >= 0) return true;
  if (resposta.indexOf("\"status\": \"AUTORIZADO\"") >= 0) return true;
  return false;
}

bool respostaCadastro(const String& resposta) {
  if (resposta.indexOf("\"cadastrado\":true") >= 0) return true;
  if (resposta.indexOf("\"cadastrado\": true") >= 0) return true;
  return false;
}

bool buscarRespostaRedirecionada(const String& url, String& resposta, TemposHTTP& tempos) {
  HTTPClient httpResposta;
  httpResposta.setConnectTimeout(TIMEOUT_CONEXAO_HTTP);
  httpResposta.setTimeout(TIMEOUT_HTTP);
  if (!httpResposta.begin(url)) return false;
  unsigned long inicio = millis();
  int codigo = httpResposta.GET();
  tempos.redirectMs = millis() - inicio;
  tempos.codigoFinal = codigo;
  Serial.print("[REDE] HTTP GET: ");
  Serial.print(codigo);
  Serial.print(" | ");
  Serial.print(tempos.redirectMs);
  Serial.println(" ms");
  if (codigo <= 0) {
    httpResposta.end();
    return false;
  }
  resposta = httpResposta.getString();
  bool sucesso = codigo >= 200 && codigo < 300;
  httpResposta.end();
  return sucesso;
}

bool enviarRequisicaoServidor(const String& corpo, String& resposta, TemposHTTP& tempos) {
  resposta = "";
  memset(&tempos, 0, sizeof(tempos));
  unsigned long inicioTotal = millis();
  if (WiFi.status() != WL_CONNECTED) {
    if (!conectarWiFi()) {
      tempos.totalMs = millis() - inicioTotal;
      return false;
    }
  }
  HTTPClient http;
  http.setConnectTimeout(TIMEOUT_CONEXAO_HTTP);
  http.setTimeout(TIMEOUT_HTTP);
  const char* headers[] = {"Location"};
  http.collectHeaders(headers, 1);
  if (!http.begin(URL_SCRIPT)) {
    tempos.totalMs = millis() - inicioTotal;
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  unsigned long inicioPost = millis();
  int codigoResposta = http.POST(corpo);
  tempos.postMs = millis() - inicioPost;
  tempos.codigoPost = codigoResposta;
  Serial.print("[REDE] HTTP POST: ");
  Serial.print(codigoResposta);
  Serial.print(" | ");
  Serial.print(tempos.postMs);
  Serial.println(" ms");
  if (codigoResposta <= 0) {
    http.end();
    tempos.totalMs = millis() - inicioTotal;
    return false;
  }
  if (codigoResposta == 301 || codigoResposta == 302 || codigoResposta == 303 || codigoResposta == 307 || codigoResposta == 308) {
    String localizacao = http.header("Location");
    http.end();
    if (localizacao.length() == 0) {
      tempos.totalMs = millis() - inicioTotal;
      return false;
    }
    bool sucesso = buscarRespostaRedirecionada(localizacao, resposta, tempos);
    tempos.totalMs = millis() - inicioTotal;
    return sucesso;
  }
  resposta = http.getString();
  tempos.codigoFinal = codigoResposta;
  bool sucesso = codigoResposta >= 200 && codigoResposta < 300;
  http.end();
  tempos.totalMs = millis() - inicioTotal;
  return sucesso;
}

bool enfileirarRequisicao(TipoRequisicao tipo, const char* valor, bool abrirSeAutorizado, uint32_t inicioAcessoMs, bool background) {
  RequisicaoRede requisicao;
  memset(&requisicao, 0, sizeof(requisicao));
  requisicao.id = proximoIdRequisicao++;
  requisicao.tipo = tipo;
  requisicao.abrirSeAutorizado = abrirSeAutorizado;
  requisicao.inicioAcessoMs = inicioAcessoMs;
  copiarTexto(valor, requisicao.valor, sizeof(requisicao.valor));
  QueueHandle_t fila = background ? filaBackground : filaPrioritaria;
  return (xQueueSend(fila, &requisicao, 0) == pdTRUE);
}

void executarRequisicaoRede(const RequisicaoRede& requisicao) {
  ResultadoRede resultado;
  memset(&resultado, 0, sizeof(resultado));
  resultado.id = requisicao.id;
  resultado.tipo = requisicao.tipo;
  resultado.abrirSeAutorizado = requisicao.abrirSeAutorizado;
  resultado.inicioAcessoMs = requisicao.inicioAcessoMs;
  copiarTexto(requisicao.valor, resultado.valor, sizeof(resultado.valor));
  TipoCredencial tipo = credencialDaRequisicao(requisicao.tipo);
  String corpo = "{";
  corpo.reserve(512);
  if (tipo == CRED_RFID) {
    corpo += "\"acao\":\"rfid\",";
    corpo += "\"uid\":\"";
    corpo += requisicao.valor;
    corpo += "\",";
  } else {
    corpo += "\"acao\":\"pin\",";
    corpo += "\"pin\":\"";
    corpo += requisicao.valor;
    corpo += "\",";
  }
  corpo += criarDadosComuns();
  corpo += "}";
  String resposta;
  resposta.reserve(512);
  resultado.comunicacaoOk = enviarRequisicaoServidor(corpo, resposta, resultado.tempos);
  if (resultado.comunicacaoOk) {
    resultado.autorizado = respostaAutorizada(resposta);
    resultado.cadastrado = respostaCadastro(resposta);
    if (tipo == CRED_RFID) resultado.autorizado = resultado.autorizado || resultado.cadastrado;
  }
  if (xQueueSend(filaResultados, &resultado, pdMS_TO_TICKS(1000)) != pdTRUE) Serial.println("[REDE] Fila de resultados cheia.");
  imprimirMemoria("Após requisição HTTP");
}

void enviarHeartbeatRede() {
  if (WiFi.status() != WL_CONNECTED) {
    if (!conectarWiFi()) return;
  }
  String corpo = "{";
  corpo += "\"acao\":\"heartbeat\",";
  corpo += criarDadosComuns();
  corpo += "}";
  String resposta;
  TemposHTTP tempos;
  bool sucesso = enviarRequisicaoServidor(corpo, resposta, tempos);
  Serial.print("[HEARTBEAT] ");
  Serial.print(sucesso ? "OK" : "FALHOU");
  Serial.print(" | ");
  Serial.print(tempos.totalMs);
  Serial.println(" ms");
}

void tarefaRede(void* parametro) {
  unsigned long ultimoHeartbeat = millis();
  for (;;) {
    RequisicaoRede requisicao;
    if (xQueueReceive(filaPrioritaria, &requisicao, pdMS_TO_TICKS(20)) == pdTRUE) {
      executarRequisicaoRede(requisicao);
      continue;
    }
    if (xQueueReceive(filaBackground, &requisicao, 0) == pdTRUE) {
      executarRequisicaoRede(requisicao);
      continue;
    }
    if (millis() - ultimoHeartbeat >= INTERVALO_HEARTBEAT) {
      ultimoHeartbeat = millis();
      enviarHeartbeatRede();
      continue;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}


void processarResultadosRede() {
  ResultadoRede resultado;
  while (xQueueReceive(filaResultados, &resultado, 0) == pdTRUE) {
    TipoCredencial tipo = credencialDaRequisicao(resultado.tipo);
    bool revalidacao = requisicaoEhRevalidacao(resultado.tipo);
    removerPendente(tipo, resultado.valor);
    Serial.print("[TEMPO] POST: ");
    Serial.print(resultado.tempos.postMs);
    Serial.print(" ms | REDIRECT: ");
    Serial.print(resultado.tempos.redirectMs);
    Serial.print(" ms | TOTAL: ");
    Serial.print(resultado.tempos.totalMs);
    Serial.println(" ms");
    if (!resultado.comunicacaoOk) {
      if (resultado.abrirSeAutorizado) mostrarMensagemOLED("ERRO DE REDE", "Tente novamente", 2500UL);
      if (revalidacao) {
        marcarRetentativaRevalidacao(tipo, resultado.valor);
        Serial.println(tipo == CRED_RFID ? "[CACHE RFID] Falha na revalidação. Cache mantido."
                                         : "[CACHE PIN] Falha na revalidação. Cache mantido.");
      } else {
        Serial.println("FALHA NA COMUNICAÇÃO");
      }
      continue;
    }
    if (resultado.autorizado) {
      autorizarNoCache(tipo, resultado.valor);
      if (resultado.abrirSeAutorizado) {
        Serial.println(tipo == CRED_RFID ? "RFID AUTORIZADO PELO SERVIDOR" : "PIN AUTORIZADO PELO SERVIDOR");
        acionarRele();
        mostrarAcessoOLED(true);
        Serial.print(tipo == CRED_RFID ? "[TEMPO] RFID -> RELÉ: " : "[TEMPO] PIN -> RELÉ: ");
        Serial.print(millis() - resultado.inicioAcessoMs);
        Serial.println(" ms");
      } else {
        Serial.println(tipo == CRED_RFID ? "[CACHE RFID] Revalidação OK." : "[CACHE PIN] Revalidação OK.");
      }
    } else {
      removerDoCache(tipo, resultado.valor);
      if (resultado.abrirSeAutorizado) mostrarAcessoOLED(false);
      if (revalidacao)
        Serial.println(tipo == CRED_RFID ? "[CACHE RFID] Cartão removido pelo servidor." : "[CACHE PIN] PIN removido pelo servidor.");
      else Serial.println(tipo == CRED_RFID ? "RFID NÃO AUTORIZADO" : "PIN NÃO AUTORIZADO");
    }
  }
}


void solicitarRevalidacao(TipoCredencial tipo, const char* valor) {
  if (existePendente(tipo, valor)) return;
  if (!precisaRevalidar(tipo, valor)) return;
  if (!adicionarPendente(tipo, valor)) return;
  marcarRevalidacao(tipo, valor);
  TipoRequisicao requisicao = tipo == CRED_RFID ? REQ_RFID_REVALIDACAO : REQ_PIN_REVALIDACAO;
  if (!enfileirarRequisicao(requisicao, valor, false, millis(), true)) {
    removerPendente(tipo, valor);
    marcarRetentativaRevalidacao(tipo, valor);
  }
}


void processarCarteirinha(const String& uidTexto, uint32_t inicioMs) {
  Serial.println();
  Serial.println("---------------------------");
  Serial.println("Carteirinha detectada");
  Serial.print("UID: ");
  Serial.println(uidTexto);
  char uid[24];
  copiarTexto(uidTexto.c_str(), uid, sizeof(uid));
  if (cacheValido(CRED_RFID, uid)) {
    Serial.println("[CACHE RFID] HIT");
    acionarRele();
    mostrarAcessoOLED(true);
    Serial.print("[TEMPO] RFID -> RELÉ CACHE: ");
    Serial.print(millis() - inicioMs);
    Serial.println(" ms");
    solicitarRevalidacao(CRED_RFID, uid);
    return;
  }
  Serial.println("[CACHE RFID] MISS");
  if (existePendente(CRED_RFID, uid)) {
    Serial.println("RFID já está sendo verificado.");
    return;
  }
  if (!adicionarPendente(CRED_RFID, uid)) {
    Serial.println("Fila de pendências cheia.");
    return;
  }
  if (!enfileirarRequisicao(REQ_RFID_INICIAL, uid, true, inicioMs, false)) {
    removerPendente(CRED_RFID, uid);
    Serial.println("Fila de autenticação RFID cheia.");
  }
}

void processarRFID() {
  if (!pn532Funcionando) return;
  uint8_t uid[7] = {0};
  uint8_t tamanhoUID = 0;
  unsigned long inicioUs = micros();
  bool cartaoDetectado = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &tamanhoUID, TIMEOUT_RFID);
  unsigned long tempoUs = micros() - inicioUs;
  if (!cartaoDetectado) return;
  uint32_t inicioMs = millis();
  String uidTexto = converterUIDParaTexto(uid, tamanhoUID);
  Serial.print("[TEMPO] PN532 detectou/leu em ");
  Serial.print(tempoUs / 1000.0, 2);
  Serial.println(" ms");
  unsigned long horarioAtual = millis();
  bool mesmaCarteirinha = strcmp(uidTexto.c_str(), ultimoUIDLido) == 0 && horarioAtual - horarioUltimaLeitura < INTERVALO_MESMA_CARTEIRINHA;
  if (mesmaCarteirinha) return;
  copiarTexto(uidTexto.c_str(), ultimoUIDLido, sizeof(ultimoUIDLido));
  horarioUltimaLeitura = horarioAtual;
  processarCarteirinha(uidTexto, inicioMs);
}


void limparSenha() {
  memset(senhaDigitada, 0, sizeof(senhaDigitada));
  tamanhoSenha = 0;
}

void processarPIN(const char* pin, uint32_t inicioMs) {
  if (cacheValido(CRED_PIN, pin)) {
    Serial.println("[CACHE PIN] HIT");
    acionarRele();
    mostrarAcessoOLED(true);
    Serial.print("[TEMPO] PIN -> RELÉ CACHE: ");
    Serial.print(millis() - inicioMs);
    Serial.println(" ms");
    solicitarRevalidacao(CRED_PIN, pin);
    return;
  }
  Serial.println("[CACHE PIN] MISS");
  if (existePendente(CRED_PIN, pin)) {
    Serial.println("PIN já está sendo verificado.");
    return;
  }
  if (!adicionarPendente(CRED_PIN, pin)) {
    Serial.println("Fila de pendências cheia.");
    return;
  }
  if (!enfileirarRequisicao(REQ_PIN_INICIAL, pin, true, inicioMs, false)) {
    removerPendente(CRED_PIN, pin);
    Serial.println("Fila de autenticação PIN cheia.");
  }
}


int indiceDaTecla(char tecla) {
  for (int i = 0; i < 12; i++)
    if (ORDEM_TECLAS[i] == tecla) return i;
  return -1;
}


void processarCliqueTecla(char tecla) {
  Serial.print("Tecla pressionada: ");
  Serial.println(tecla);
  if (tecla == '*') {
    limparSenha();
    mostrarSenhaOLED();
    Serial.println("Senha apagada.");
    return;
  }
  if (tecla == '#') {
    if (tamanhoSenha != 6) {
      Serial.println("PIN precisa ter exatamente 6 dígitos.");
      limparSenha();
      mostrarMensagemOLED("PIN INVALIDO", "Digite 6 numeros", 2000UL);
      return;
    }
    char pin[7];
    copiarTexto(senhaDigitada, pin, sizeof(pin));
    limparSenha();
    mostrarMensagemOLED("VERIFICANDO", "Aguarde...", 10000UL);
    processarPIN(pin, millis());
    return;
  }
  if (tecla < '0' || tecla > '9') return;
  if (tamanhoSenha >= 6) {
    Serial.println("PIN completo. Pressione #.");
    return;
  }
  senhaDigitada[tamanhoSenha] = tecla;
  tamanhoSenha++;
  senhaDigitada[tamanhoSenha] = '\0';
  mostrarSenhaOLED();
  Serial.print("Senha: ");
  for (uint8_t i = 0; i < tamanhoSenha; i++) Serial.print("*");
  Serial.println();
}


void processarTeclado() {
  teclado.getKeys();
  for (byte i = 0; i < LIST_MAX; i++) {
    if (!teclado.key[i].stateChanged) continue;
    char tecla = teclado.key[i].kchar;
    int indice = indiceDaTecla(tecla);
    if (indice < 0) continue;
    if (teclado.key[i].kstate == PRESSED) {
      if (!teclaPressionada[indice]) {
        teclaPressionada[indice] = true;
        processarCliqueTecla(tecla);
      }
      continue;
    }
    if (teclado.key[i].kstate == RELEASED) teclaPressionada[indice] = false;
  }
}


void diagnosticoPeriodico() {
  if (millis() - horarioUltimoDiagnostico < INTERVALO_DIAGNOSTICO) return;
  horarioUltimoDiagnostico = millis();
  imprimirMemoria("Diagnóstico periódico");
  Serial.print("[CACHE] RFID: ");
  Serial.print(contarCache(CRED_RFID));
  Serial.print("/");
  Serial.println(MAX_RFID_CACHE);
  Serial.print("[CACHE] PIN: ");
  Serial.print(contarCache(CRED_PIN));
  Serial.print("/");
  Serial.println(MAX_PIN_CACHE);
  Serial.print("[PN532] Estado: ");
  Serial.println(pn532Funcionando ? "OK" : "FALHA");
  Serial.print("[HORÁRIO] NTP: ");
  Serial.println(obterEpochAtual() != 0 ? "OK" : "AINDA NÃO SINCRONIZADO");
}


void setup() {
  Serial.begin(115200);
  inicializarOLED();
  delay(1500);
  Serial.println();
  Serial.println("========================================");
  Serial.println("SISTEMA RFID + PIN + CACHE NVS");
  Serial.print("Firmware: ");
  Serial.println(VERSAO_FIRMWARE);
  Serial.println("========================================");
  inicializarRele();
  carregarCaches();
  teclado.setDebounceTime(70);
  teclado.setHoldTime(500);
  WiFi.setSleep(false);
  conectarWiFi();
  if (WiFi.status() == WL_CONNECTED) configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  inicializarLeitorRFID();
  filaPrioritaria = xQueueCreate(16, sizeof(RequisicaoRede));
  filaBackground = xQueueCreate(16, sizeof(RequisicaoRede));
  filaResultados = xQueueCreate(24, sizeof(ResultadoRede));
  if (filaPrioritaria == nullptr || filaBackground == nullptr || filaResultados == nullptr) {
    Serial.println("ERRO CRÍTICO AO CRIAR FILAS.");
    while (true) delay(1000);
  }
  BaseType_t criada = xTaskCreate(tarefaRede, "TarefaRede", 16384, nullptr, 1, nullptr);
  if (criada != pdPASS) {
    Serial.println("ERRO CRÍTICO AO CRIAR TAREFA DE REDE.");
    while (true) delay(1000);
  }
  horarioUltimoDiagnostico = millis();
  Serial.println();
  Serial.println("Sistema pronto.");
  Serial.println("Passe a carteirinha ou digite a senha.");
  Serial.println("* = apagar");
  Serial.println("# = confirmar");
  Serial.println();
  imprimirMemoria("Inicialização concluída");
}


void loop() {
  atualizarRele();
  processarResultadosRede();
  processarTeclado();
  processarRFID();
  verificarSaudePN532();
  diagnosticoPeriodico();
  atualizarOLED();
  delay(1);
}
