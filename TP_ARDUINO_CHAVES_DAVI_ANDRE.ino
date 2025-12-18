/**********************************************************************
  SISTEMA DE CONTROLE DE ACESSO INTELIGENTE (MEGA + WIFI)
  ---------------------------------------------------------------------
  Funcionalidades:
  1. Acesso Híbrido: Biometria (Dedo) e RFID (Cartão/Tag).
  2. Monitoramento de Chaves: Detecta retirada/devolução.
  3. Conectividade: Envia logs para Google Firebase via ESP8266.
  4. Segurança: Alarme local e proteção contra travamento do LCD.
  
  Correções Aplicadas: 
  - Verificação biométrica robusta.
  - Cadastro com dupla checagem (evita "dedo fantasma").
 **********************************************************************/

#include <Wire.h>
#include <rgb_lcd.h>
#include <Adafruit_Fingerprint.h>
#include <SPI.h>
#include <MFRC522.h>
#include <EEPROM.h>

// ====================================================================
// 1. CREDENCIAIS DE REDE E BANCO DE DADOS
// ====================================================================
#define SerialWifi Serial3 

const String WIFI_SSID     = "DCOMP_UFSJ"; 
const String WIFI_PASS     = ""; 
const String FIREBASE_HOST = "seguranca-de-chaves-default-rtdb.firebaseio.com"; 

// ====================================================================
// 2. MAPEAMENTO DE HARDWARE E OBJETOS
// ====================================================================

// --- Display LCD RGB ---
rgb_lcd lcd;
// Paleta de cores para feedback visual rápido
const int COR_PADRAO   = 0; // Branco
const int COR_SUCESSO  = 1; // Verde
const int COR_ERRO     = 2; // Vermelho
const int COR_AGUARDE  = 3; // Azul
const int COR_ALERTA   = 4; // Laranja

// --- Sensor Biométrico (Serial 2) ---
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&Serial2);

// --- Leitor RFID (SPI) ---
#define SS_PIN  53
#define RST_PIN 5
MFRC522 mfrc522(SS_PIN, RST_PIN);

// --- Pinos de Controle e Sensores ---
const int PINO_ALARME         = 7;  // Sirene ou Buzzer
const int PINO_BOTAO_CADASTRO = 48; // Botão físico para adicionar usuários
const int PINO_CHAVE_INICIO   = 25; // Primeiro pino do painel de chaves
const int PINO_CHAVE_FIM      = 26; // Último pino do painel de chaves

// ====================================================================
// 3. VARIÁVEIS DE CONTROLE DO SISTEMA
// ====================================================================

// Temporizadores para multitarefa (evita uso excessivo de delay)
unsigned long timerCheck = 0;
const int INTERVALO_CHECK = 50; // Verifica sensores 20 vezes por segundo

// Controle do Botão de Cadastro (com Debounce para evitar falso clique)
int estadoBotaoCad;              
int ultimoEstadoBotaoCad = HIGH; 
unsigned long ultimoTempoDebounce = 0;  
const unsigned long DELAY_DEBOUNCE = 50;
bool modoCadastroAtivo = false;

// Controle da Sessão de Acesso (Janela de tempo para pegar a chave)
bool sessaoAberta = false;             
unsigned long timerSessao = 0;         
const int TEMPO_LIMITE_SESSAO = 10000; // 10 segundos para retirar a chave
String idUsuarioAtual = "";            
String tipoUsuarioAtual = "";          

// Controle Visual do LCD
unsigned long timerMsgLCD = 0;
bool msgLCDTemporaria = false;

// Proteção contra Ruído Elétrico no LCD
unsigned long ultimoResetLCD = 0;
const unsigned long INTERVALO_RESET_LCD = 60000; // Reinicia o controlador do LCD a cada 60s

// Memória de Estado das Chaves (para detectar mudança)
bool estadoAnteriorChaves[PINO_CHAVE_FIM - PINO_CHAVE_INICIO + 1];

// ====================================================================
// 4. PROTÓTIPOS DAS FUNÇÕES (Índice)
// ====================================================================
void conectarWifiBlindado();
void enviarDadosFirebase(String jsonDados);
void registrarRetirada(int pino, String idUser, String tipoUser);
void registrarDevolucao(int pino);
void dispararAlarme(int pino);
void resetarLCDParaPadrao();
void listarInfosIniciais(); 

// ====================================================================
// 5. CONFIGURAÇÃO INICIAL (SETUP)
// ====================================================================
void setup() {
  Serial.begin(9600);      
  SerialWifi.begin(115200); 

  // Inicializa LCD
  lcd.begin(16, 2);
  atualizarLCD("Iniciando...", COR_AGUARDE);

  // Configura portas das chaves
  int index = 0;
  for(int i = PINO_CHAVE_INICIO; i <= PINO_CHAVE_FIM; i++){
    pinMode(i, INPUT_PULLUP);
    estadoAnteriorChaves[index] = digitalRead(i); 
    index++;
  }
  Serial.println(F("[SETUP] Pinos das chaves configurados."));

  // Configura atuadores e botões
  pinMode(PINO_ALARME, OUTPUT);
  pinMode(PINO_BOTAO_CADASTRO, INPUT_PULLUP);

  Serial.println(F("\n--- INICIANDO SISTEMA ---"));

  // Inicializa Biometria
  finger.begin(57600);
  if (!finger.verifyPassword()) Serial.println(F("[FALHA] Bio ausente"));
  else Serial.println(F("[SETUP] Sensor Biometrico OK."));

  // Inicializa RFID
  SPI.begin();
  mfrc522.PCD_Init();
  mfrc522.PCD_SetAntennaGain(mfrc522.RxGain_max); 
  Serial.println(F("[SETUP] Modulo RFID Iniciado."));

  // Mostra relatório de memória no Serial
  listarInfosIniciais(); 

  // Conecta na Rede
  conectarWifiBlindado();

  Serial.println(F("--- Sistema Pronto ---"));
  atualizarLCD("Aguardando...", COR_PADRAO);
}

// ====================================================================
// 6. LOOP PRINCIPAL (CORE)
// ====================================================================
void loop() {
  // 1. Verifica se alguém apertou o botão de cadastro
  lerBotaoCadastro();

  // 2. Manutenção Preventiva do LCD (Evita bugs visuais por ruído elétrico)
  if (millis() - ultimoResetLCD > INTERVALO_RESET_LCD) {
      // Só reseta se o sistema estiver ocioso para não atrapalhar o usuário
      if (!sessaoAberta && !modoCadastroAtivo) {
          Serial.println(F("[MANUTENCAO] Executando reset preventivo do LCD...")); 
          lcd.begin(16, 2); 
          resetarLCDParaPadrao();
          ultimoResetLCD = millis();
      }
  }

  // 3. Gerenciamento de Modos
  if (modoCadastroAtivo) {
    executarRotinaCadastro();
    modoCadastroAtivo = false; 
    resetarLCDParaPadrao(); 
  } 
  else {
    // Limpeza de mensagens temporárias na tela (ex: "Chave Devolvida")
    if (msgLCDTemporaria && millis() - timerMsgLCD > 3000) {
       if(!sessaoAberta) resetarLCDParaPadrao();
       msgLCDTemporaria = false;
    }

    // Loop rápido de verificação (Sensores e Timeout)
    if (millis() - timerCheck > INTERVALO_CHECK) {
      verificarEntradaUsuario();
      monitorarPainelDeChaves();
      verificarTimeoutSessao();
      timerCheck = millis();
    }
  }
}

// ====================================================================
// 7. LÓGICA DE ACESSO E AUTENTICAÇÃO
// ====================================================================

void verificarEntradaUsuario() {
  if (sessaoAberta) return; // Se já tem alguém logado, ignora novas tentativas

  // --- TENTATIVA VIA BIOMETRIA ---
  int idBio = lerSensorBiometrico(); 
  
  if (idBio >= 0) { 
    Serial.println("[ACESSO] Biometria reconhecida: ID #" + String(idBio)); 
    autorizarSessao("Biometria", String(idBio)); 
    return; 
  }
  else if (idBio == -2) {
    // -2 Significa erro de leitura ou dedo não cadastrado
    negacaoAcesso("Biometria", "Desconhecido");
    return;
  }
  
  // --- TENTATIVA VIA RFID ---
  String idTag = lerSensorRFID();
  if (idTag != "") {
    Serial.println("[ACESSO] Tag RFID lida: " + idTag); 
    if (verificarTagNaEEPROM(idTag)) {
      autorizarSessao("RFID", idTag);
    }
    else {
      negacaoAcesso("RFID", idTag);
    }
  }
}

void autorizarSessao(String tipo, String id) {
  sessaoAberta = true; 
  timerSessao = millis();
  idUsuarioAtual = id; 
  tipoUsuarioAtual = tipo;
  
  Serial.println(">>> SESSAO ABERTA | Tipo: " + tipo + " | ID: " + id);
  atualizarLCD("Pode retirar...", COR_SUCESSO);
}

void negacaoAcesso(String tipo, String id) {
  Serial.println("!!! ACESSO NEGADO | Tipo: " + tipo + " | ID Lido: " + id);
  atualizarLCD("Nao Cadastrado", COR_ERRO); 
  delay(2000); 
  resetarLCDParaPadrao();
}

void verificarTimeoutSessao() {
  if (sessaoAberta) {
    if (millis() - timerSessao > TEMPO_LIMITE_SESSAO) {
      sessaoAberta = false;
      Serial.println(F("--- Sessao Expirou (Timeout) ---")); 
      resetarLCDParaPadrao();
      idUsuarioAtual = "";
    }
  }
}

// ====================================================================
// 8. MONITORAMENTO FÍSICO DAS CHAVES
// ====================================================================

void monitorarPainelDeChaves() {
  int index = 0;
  for (int pino = PINO_CHAVE_INICIO; pino <= PINO_CHAVE_FIM; pino++) {
    bool leituraAtual = digitalRead(pino); 
    bool leituraAnterior = estadoAnteriorChaves[index];

    if (leituraAtual != leituraAnterior) {
      // HOUVE MUDANÇA NO ESTADO DA CHAVE
      if (leituraAtual == HIGH) { 
        // CHAVE SAIU (Foi retirada)
        if (sessaoAberta) {
          registrarRetirada(pino, idUsuarioAtual, tipoUsuarioAtual);
          
          // Fecha sessão para evitar que retirem outra chave com o mesmo login
          sessaoAberta = false;
          idUsuarioAtual = "";
          
          msgLCDTemporaria = true;
          timerMsgLCD = millis();
        } else {
          // Retirada sem login = ROUBO
          dispararAlarme(pino);
        }
      }
      else { 
        // CHAVE VOLTOU (Foi devolvida)
        registrarDevolucao(pino);
        msgLCDTemporaria = true;
        timerMsgLCD = millis();
      }
      estadoAnteriorChaves[index] = leituraAtual;
    }
    index++;
  }
}

// ====================================================================
// 9. LOGS E FIREBASE
// ====================================================================

void registrarRetirada(int pino, String idUser, String tipoUser) {
  Serial.println(">>> LOG: Retirada Autorizada! Pino: " + String(pino) + " | User: " + idUser + " (" + tipoUser + ")");
  atualizarLCD("Chave " + String(pino) + " Saiu", COR_SUCESSO);
  
  String json = "{\"evento\":\"RETIRADA_AUTORIZADA\",";
  json += "\"pino\":" + String(pino) + ",";
  json += "\"id_usuario\":\"" + idUser + "\",";
  json += "\"tipo_auth\":\"" + tipoUser + "\"}";
  
  enviarDadosFirebase(json);
}

void registrarDevolucao(int pino) {
  Serial.println("<<< LOG: Devolucao detectada no Pino: " + String(pino));
  atualizarLCD("Chave " + String(pino) + " OK", COR_PADRAO);

  String json = "{\"evento\":\"DEVOLUCAO\",";
  json += "\"pino\":" + String(pino) + "}";

  enviarDadosFirebase(json);
}

void dispararAlarme(int pino) {
  Serial.println("!!! ALARME: Retirada nao autorizada no Pino " + String(pino) + " !!!");
  atualizarLCD("ALERTA! CHAVE " + String(pino), COR_ERRO); 
  digitalWrite(PINO_ALARME, HIGH);

  String json = "{\"evento\":\"ALARME_ROUBO\",";
  json += "\"pino\":" + String(pino) + ",";
  json += "\"mensagem\":\"Retirada Ilegal\"}";

  enviarDadosFirebase(json);
  
  delay(3000); 
  digitalWrite(PINO_ALARME, LOW);
  resetarLCDParaPadrao();
}

void enviarDadosFirebase(String jsonDados) {
  Serial.println(F("[FIREBASE] Preparando envio de dados..."));
  
  // Limpa buffer serial
  while(SerialWifi.available()) SerialWifi.read();
  
  // Fecha conexões anteriores por segurança
  SerialWifi.println("AT+CIPCLOSE"); delay(100); 

  // Configura SSL e Conecta
  SerialWifi.println("AT+CIPSSLSIZE=4096"); delay(50);
  String cmd = "AT+CIPSTART=\"SSL\",\"" + FIREBASE_HOST + "\",443";
  SerialWifi.println(cmd);

  long t = millis();
  bool conectado = false;
  while(millis() - t < 5000) {
    String resp = SerialWifi.readStringUntil('\n');
    if(resp.indexOf("CONNECT") != -1) { conectado = true; break; }
  }

  if(conectado) {
    String headers = "POST /logs.json HTTP/1.1\r\n"; 
    headers += "Host: " + String(FIREBASE_HOST) + "\r\n";
    headers += "Content-Type: application/json\r\n";
    headers += "Content-Length: " + String(jsonDados.length()) + "\r\n\r\n"; 
    
    SerialWifi.print("AT+CIPSEND=");
    SerialWifi.println(headers.length() + jsonDados.length());
    delay(200); 
    
    SerialWifi.print(headers);
    SerialWifi.print(jsonDados);
    
    Serial.println(F("[FIREBASE] Dados enviados com sucesso."));
  } else {
    Serial.println(F("[FIREBASE] Erro ao conectar com o servidor."));
  }
}

// ====================================================================
// 10. FUNÇÕES DE HARDWARE (DRIVERS)
// ====================================================================

// --- BIOMETRIA (LEITURA CORRIGIDA) ---
int lerSensorBiometrico() {
  uint8_t p = finger.getImage();
  
  // Se não tem dedo, retorna -1 silenciosamente
  if (p != FINGERPRINT_OK) return -1; 

  Serial.println(F("[BIO CHECK] Imagem capturada. Convertendo..."));

  // [CORREÇÃO CRÍTICA]: Forçamos a conversão para o Slot 1
  // Isso garante que a verificação use o buffer correto, evitando erros pós-cadastro
  p = finger.image2Tz(1); 
  
  if (p != FINGERPRINT_OK) {
    Serial.println(F("[ERRO BIO] Imagem muito suja ou erro de comunicação."));
    return -2; 
  }

  // Busca no banco de dados interno do sensor
  p = finger.fingerSearch();
  
  if (p == FINGERPRINT_OK) {
    Serial.print(F("[SUCESSO BIO] ID Encontrado: #")); Serial.print(finger.fingerID);
    Serial.print(F(" | Confianca: ")); Serial.println(finger.confidence);
    return finger.fingerID;
  } 
  else if (p == FINGERPRINT_NOTFOUND) {
    Serial.println(F("[FALHA BIO] Digital lida com sucesso, mas NAO cadastrada."));
    return -2;
  } 
  else {
    Serial.println(F("[ERRO BIO] Erro desconhecido na busca."));
    return -2;
  }
}

// --- RFID ---
String lerSensorRFID() {
  if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) return "";
  String tagID = "";
  for (byte i = 0; i < mfrc522.uid.size; i++) {
     tagID.concat(String(mfrc522.uid.uidByte[i] < 0x10 ? "0" : ""));
     tagID.concat(String(mfrc522.uid.uidByte[i], HEX));
  }
  tagID.toUpperCase(); mfrc522.PICC_HaltA(); return tagID;
}

// --- WI-FI ---
void conectarWifiBlindado() {
  atualizarLCD("Conectando WiFi", COR_AGUARDE);
  Serial.println("\n--- CONFIGURANDO WIFI ---");
  
  SerialWifi.println("AT+RST"); delay(3000); 
  while(SerialWifi.available()) SerialWifi.read();

  SerialWifi.println("AT+CWMODE=1");    delay(500);
  SerialWifi.println("AT+CIPMUX=0");    delay(500);
  SerialWifi.println("AT+CIPRECVMODE=0"); delay(500);
  
  String cmd = "AT+CWJAP=\"" + WIFI_SSID + "\",\"" + WIFI_PASS + "\"";
  SerialWifi.println(cmd);
  
  bool conectado = false;
  for(int i=0; i<30; i++) {
    if (SerialWifi.find((char*)"OK")) { conectado = true; break; }
    delay(500);
  }

  if (conectado) {
    Serial.println("[OK] WiFi Conectado!");
    atualizarLCD("WiFi OK!", COR_SUCESSO);
  } else {
    Serial.println("[ERRO] Falha WiFi. Verifique SSID/Senha.");
    atualizarLCD("Erro WiFi", COR_ERRO);
  }
  delay(1000);
}

// ====================================================================
// 11. GERENCIAMENTO DE MEMÓRIA (EEPROM/FLASH)
// ====================================================================

bool verificarTagNaEEPROM(String tagLida) {
  int qtdTags = EEPROM.read(0);
  if (qtdTags == 0xFF) qtdTags = 0;
  for (int i = 0; i < qtdTags; i++) {
    int enderecoBase = 1 + (i * 4);
    String tagArmazenada = "";
    for (int j = 0; j < 4; j++) {
      byte valor = EEPROM.read(enderecoBase + j);
      tagArmazenada.concat(String(valor < 0x10 ? "0" : ""));
      tagArmazenada.concat(String(valor, HEX));
    }
    tagArmazenada.toUpperCase();
    if (tagLida == tagArmazenada) return true;
  }
  return false;
}

void salvarTagEEPROM(String tagHex) {
  int qtdTags = EEPROM.read(0); if (qtdTags == 0xFF) qtdTags = 0;
  int enderecoBase = 1 + (qtdTags * 4);
  for (int i = 0; i < 4; i++) {
    String byteString = tagHex.substring(i*2, (i*2)+2);
    byte valor = (byte) strtol(byteString.c_str(), NULL, 16);
    EEPROM.write(enderecoBase + i, valor);
  }
  EEPROM.write(0, qtdTags + 1);
  Serial.println("Tag Salva na EEPROM: " + tagHex);
}

void listarInfosIniciais() {
  Serial.println(F("\n=================================="));
  Serial.println(F("       RELATÓRIO DE CADASTROS      "));
  Serial.println(F("=================================="));
  
  // 1. Tags RFID
  int qtdTags = EEPROM.read(0);
  if (qtdTags == 0xFF) qtdTags = 0;
  Serial.print(F("TOTAL TAGS RFID: ")); Serial.println(qtdTags);
  for (int i = 0; i < qtdTags; i++) {
    // ... código de leitura da EEPROM ...
    int enderecoBase = 1 + (i * 4);
    String tagArmazenada = "";
    for (int j = 0; j < 4; j++) {
      byte valor = EEPROM.read(enderecoBase + j);
      tagArmazenada.concat(String(valor < 0x10 ? "0" : ""));
      tagArmazenada.concat(String(valor, HEX));
    }
    tagArmazenada.toUpperCase();
    Serial.print(F("  [TAG ")); Serial.print(i); Serial.print(F("]: "));
    Serial.println(tagArmazenada);
  }
  
  Serial.println(F("----------------------------------"));

  // 2. Biometria
  finger.getTemplateCount();
  Serial.print(F("TOTAL BIOMETRIAS: ")); Serial.println(finger.templateCount);
  if (finger.templateCount > 0) {
    Serial.println(F("  IDs Ocupados:"));
    for (int id = 1; id <= 127; id++) {
      if (finger.loadModel(id) == FINGERPRINT_OK) {
        Serial.print(F("   - ID #")); Serial.println(id);
      }
    }
  } else {
    Serial.println(F("  (Nenhuma digital cadastrada)"));
  }
  Serial.println(F("==================================\n"));
}

// ====================================================================
// 12. ROTINA DE CADASTRO (COM DUPLA CHECAGEM)
// ====================================================================

void lerBotaoCadastro() {
  int leitura = digitalRead(PINO_BOTAO_CADASTRO);
  if (leitura != ultimoEstadoBotaoCad) ultimoTempoDebounce = millis();
  
  if ((millis() - ultimoTempoDebounce) > DELAY_DEBOUNCE) {
    if (leitura != estadoBotaoCad) {
      estadoBotaoCad = leitura;
      if (estadoBotaoCad == LOW) {
          modoCadastroAtivo = true;
          Serial.println(F("\n[SISTEMA] Botao Pressionado: Entrando em MODO CADASTRO...")); 
      }
    }
  }
  ultimoEstadoBotaoCad = leitura;
}

void executarRotinaCadastro() {
  atualizarLCD("MODO CADASTRO", COR_ALERTA);
  delay(1000);
  atualizarLCD("1-BIO  2-RFID", COR_PADRAO);
  
  unsigned long t = millis();
  
  // Janela de 10 segundos para apresentar um cartão ou dedo
  while(millis() - t < 10000) { 
    
    // --- OPÇÃO A: CADASTRO RFID ---
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
       Serial.println(F("[CADASTRO] Cartao RFID Detectado."));
       String tag = "";
       for (byte i = 0; i < mfrc522.uid.size; i++) {
          tag.concat(String(mfrc522.uid.uidByte[i] < 0x10 ? "0" : ""));
          tag.concat(String(mfrc522.uid.uidByte[i], HEX));
       }
       tag.toUpperCase(); mfrc522.PICC_HaltA();
       salvarTagEEPROM(tag);
       atualizarLCD("RFID Cadastrado!", COR_SUCESSO);
       delay(2000); return;
    }
    
    // --- OPÇÃO B: CADASTRO BIOMETRIA (LÓGICA CORRIGIDA) ---
    if (finger.getImage() == FINGERPRINT_OK) {
       Serial.println(F("[CADASTRO BIO] Dedo detectado."));
       
       // >>> CORREÇÃO CRÍTICA: BUSCAR ID LIVRE PRIMEIRO <<<
       // Se buscarmos depois de criar o modelo, o 'loadModel' apaga a digital da memória RAM do sensor
       Serial.println(F("[CADASTRO BIO] Buscando slot de memoria livre..."));
       int idNovo = -1;
       for (int i = 1; i <= 127; i++) {
         if (finger.loadModel(i) != FINGERPRINT_OK) {
           idNovo = i; // Achamos um buraco na memória
           break;
         }
       }
       
       if (idNovo == -1) {
         Serial.println(F("[ERRO BIO] Memoria cheia (127 IDs ocupados)."));
         atualizarLCD("Memoria Cheia", COR_ERRO); delay(2000); return;
       }
       Serial.print(F("[CADASTRO BIO] Slot encontrado: ID #")); Serial.println(idNovo);

       // -----------------------------------------------------------
       // AGORA SIM, COM O ID GARANTIDO, INICIAMOS A CAPTURA
       // -----------------------------------------------------------

       // >>> PASSO 1: Primeira Leitura (Buffer 1) <<<
       Serial.println(F("[CADASTRO BIO] Iniciando captura 1..."));
       atualizarLCD("Imagem 1 OK", COR_AGUARDE);
       if (finger.image2Tz(1) != FINGERPRINT_OK) {
          Serial.println(F("[ERRO BIO] Falha na conversao Imagem 1."));
          atualizarLCD("Erro Leitura 1", COR_ERRO); delay(2000); return;
       }
       
       // >>> PASSO 2: Solicitar retirada do dedo <<<
       atualizarLCD("Tire o dedo", COR_ALERTA);
       Serial.println(F("[CADASTRO BIO] Aguardando retirar o dedo..."));
       delay(1000);
       while (finger.getImage() != FINGERPRINT_NOFINGER) {} 
       Serial.println(F("[CADASTRO BIO] Dedo removido."));

       // >>> PASSO 3: Solicitar confirmação <<<
       atualizarLCD("Encoste de novo", COR_PADRAO);
       Serial.println(F("[CADASTRO BIO] Aguardando confirmacao..."));
       
       bool dedoColocadoNovamente = false;
       unsigned long timerBio2 = millis();
       while (millis() - timerBio2 < 5000) {
         if (finger.getImage() == FINGERPRINT_OK) {
           dedoColocadoNovamente = true;
           break;
         }
       }
       
       if (!dedoColocadoNovamente) {
         Serial.println(F("[ERRO BIO] Tempo esgotado."));
         atualizarLCD("Tempo Esgotado", COR_ERRO); delay(2000); return;
       }

       // >>> PASSO 4: Segunda Leitura (Buffer 2) <<<
       Serial.println(F("[CADASTRO BIO] Captura 2..."));
       atualizarLCD("Imagem 2 OK", COR_AGUARDE);
       if (finger.image2Tz(2) != FINGERPRINT_OK) {
          Serial.println(F("[ERRO BIO] Falha na conversao Imagem 2."));
          atualizarLCD("Erro Leitura 2", COR_ERRO); delay(2000); return;
       }
       
       // >>> PASSO 5: Criação do Modelo (Combina Buffer 1 e 2) <<<
       // O resultado fica no Buffer 1 e 2. NÃO PODEMOS CHAMAR loadModel DEPOIS DISSO.
       Serial.println(F("[CADASTRO BIO] Gerando modelo..."));
       if (finger.createModel() != FINGERPRINT_OK) {
         Serial.println(F("[ERRO BIO] Digitais nao batem."));
         atualizarLCD("Dedo Diferente!", COR_ERRO); 
         delay(2000); return;
       }

       // >>> PASSO 6: Salvar na Flash (Usando o ID que achamos lá no começo) <<<
       Serial.print(F("[CADASTRO BIO] Gravando no ID: ")); Serial.println(idNovo);
       if (finger.storeModel(idNovo) == FINGERPRINT_OK) {
         atualizarLCD("Salvo ID " + String(idNovo), COR_SUCESSO); 
         Serial.println(F("[SUCESSO] Biometria gravada!"));
         delay(1000); 
       } else {
         Serial.println(F("[ERRO BIO] Erro de escrita na Flash."));
         atualizarLCD("Erro ao Salvar", COR_ERRO);
       }
       delay(2000); return;
    }
  }
  Serial.println(F("[CADASTRO] Tempo limite encerrado."));
  atualizarLCD("Tempo Esgotado", COR_ERRO);
  delay(1000);
}

// ====================================================================
// 13. UTILITÁRIOS DE DISPLAY
// ====================================================================

void atualizarLCD(String texto, int corTipo) {
  switch (corTipo) {
    case COR_SUCESSO: lcd.setRGB(0, 255, 0);   break; 
    case COR_ERRO:    lcd.setRGB(255, 0, 0);   break; 
    case COR_AGUARDE: lcd.setRGB(0, 0, 255);   break; 
    case COR_ALERTA:  lcd.setRGB(255, 140, 0); break;
    default:          lcd.setRGB(255, 255, 255); break; 
  }
  lcd.clear();
  lcd.setCursor(0, 0);
  if (texto.length() <= 16) { lcd.print(texto); } 
  else { lcd.print(texto.substring(0, 16)); lcd.setCursor(0, 1); lcd.print(texto.substring(16, 32)); }
}

void resetarLCDParaPadrao() {
    // Força cor branca e limpa tela
    lcd.setRGB(255, 255, 255); 
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("Aguardando...");
}
