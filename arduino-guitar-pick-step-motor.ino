/*
  ==============================================================================
  PROJETO: Palhetador Automático de Guitarra com Motor de Passo
  HARDWARE:
    - Arduino Uno Rev3
    - DFRobot LCD Keypad Shield (16x2 + Teclado Analógico em A0)
    - Motor de Passo: 28BYJ-48 5V (Redução ~1:64)
    - Driver: Módulo ULN2003 alimentado em 5V
  ==============================================================================
*/

#include <LiquidCrystal.h>

// ==============================================================================
// DEFINIÇÕES DE PINOS
// ==============================================================================
LiquidCrystal lcd(8, 9, 4, 5, 6, 7);

const uint8_t PIN_KEYPAD     = A0; // Botões analógicos do shield
const uint8_t PIN_BACKLIGHT  = 10; // Backlight LCD

// Pinos de acionamento do ULN2003
const uint8_t PIN_IN1        = A1;
const uint8_t PIN_IN2        = A2;
const uint8_t PIN_IN3        = A3;
const uint8_t PIN_IN4        = A4;

// ==============================================================================
// PARÂMETROS DO MOTOR 28BYJ-48 (SEQUÊNCIA EM MEIO-PASSO / 8 FASES)
// O 28BYJ-48 foi desenhado para meio-passo: elimina travamentos, evita engasgos
// e garante movimento 100% simétrico nos dois sentidos com rotação perfeita.
// 4096 meio-passos / volta de 360° (~0.08789° por meio-passo)
// ==============================================================================
const float STEPS_PER_REV    = 4096.0f;
const float DEG_PER_STEP     = 360.0f / STEPS_PER_REV;

const uint8_t STEP_SEQ[8][4] = {
  {HIGH, LOW,  LOW,  LOW }, // 1: A
  {HIGH, HIGH, LOW,  LOW }, // 2: A+B
  {LOW,  HIGH, LOW,  LOW }, // 3: B
  {LOW,  HIGH, HIGH, LOW }, // 4: B+C
  {LOW,  LOW,  HIGH, LOW }, // 5: C
  {LOW,  LOW,  HIGH, HIGH}, // 6: C+D
  {LOW,  LOW,  LOW,  HIGH}, // 7: D
  {HIGH, LOW,  LOW,  HIGH}  // 8: D+A
};

// ==============================================================================
// ESTADOS DA MÁQUINA
// ==============================================================================
enum SystemState {
  STATE_STOP,   // Parado no centro, bobinas desenergizadas
  STATE_RUN,    // Strumming contínuo alternado up/down
  STATE_ALIGN   // Ajuste/calibração rápida do centro
};

SystemState currentState = STATE_STOP;

// ==============================================================================
// VARIÁVEIS DE CONTROLE DO MOVIMENTO
// ==============================================================================
// Em meio-passos: 100 passos ≈ ±8.8° (curso total de ~17.6°, ideal para corda)
int amplitudeSteps = 100;    
int minAmplitude   = 12;     // ~1.0°
int maxAmplitude   = 700;    // ~±61°

int speedLevel     = 5;      // Nível de velocidade (1 a 10)
const int MIN_SPEED = 1;
const int MAX_SPEED = 10;

// Delays calibrados para meio-passo suave, sem vibração e com excelente torque
const unsigned int SPEED_DELAY_US[] = {
  0,     // índice 0 não usado
  2800,  // Vel 1 (Muito forte e suave, ~54 SPM)
  2400,  // Vel 2 (~63 SPM)
  2100,  // Vel 3 (~71 SPM)
  1800,  // Vel 4 (~83 SPM)
  1500,  // Vel 5 (Padrão: ~100 SPM - Strumming rítmico perfeito)
  1350,  // Vel 6 (~111 SPM)
  1200,  // Vel 7 (~125 SPM)
  1050,  // Vel 8 (~143 SPM)
  950,   // Vel 9 (~158 SPM)
  850    // Vel 10 (~176 SPM)
};

// Posição angular atual (em meio-passos relativos ao centro zero)
long currentStepPos = 0;
int  motionDirection = 1;    // +1 = avançando; -1 = retornando
uint8_t stepPhaseIndex = 0;  // 0 a 7

// Base de tempo do motor
unsigned long lastStepMicros = 0;

// ==============================================================================
// FILTRO ANTI-RUÍDO E DEBOUNCE PARA OS BOTÕES
// ==============================================================================
enum Button {
  BTN_NONE,
  BTN_RIGHT,
  BTN_UP,
  BTN_DOWN,
  BTN_LEFT,
  BTN_SELECT
};

bool physicalButtonsEnabled = true;
Button stableButton = BTN_NONE;
Button lastCandidateButton = BTN_NONE;
unsigned long candidateStartTime = 0;
unsigned long buttonPressStartTime = 0;
unsigned long lastRepeatTime = 0;
bool selectLongPressHandled = false;

// Leitura com filtro de múltiplas amostras (elimina ruído induzido pelo motor)
Button getFilteredButton() {
  if (!physicalButtonsEnabled) return BTN_NONE;

  int a0 = 0;
  for (int i = 0; i < 4; i++) {
    a0 += analogRead(PIN_KEYPAD);
  }
  a0 /= 4;

  if (a0 > 920) return BTN_NONE;
  if (a0 < 70)  return BTN_RIGHT;
  if (a0 < 240) return BTN_UP;
  if (a0 < 420) return BTN_DOWN;
  if (a0 < 630) return BTN_LEFT;
  if (a0 < 860) return BTN_SELECT;
  return BTN_NONE;
}

// ==============================================================================
// ACIONAMENTO DO MOTOR
// ==============================================================================
void applyStepPhase(uint8_t phase) {
  digitalWrite(PIN_IN1, STEP_SEQ[phase][0]);
  digitalWrite(PIN_IN2, STEP_SEQ[phase][1]);
  digitalWrite(PIN_IN3, STEP_SEQ[phase][2]);
  digitalWrite(PIN_IN4, STEP_SEQ[phase][3]);
}

void releaseMotor() {
  digitalWrite(PIN_IN1, LOW);
  digitalWrite(PIN_IN2, LOW);
  digitalWrite(PIN_IN3, LOW);
  digitalWrite(PIN_IN4, LOW);
}

void stepMotor(int dir) {
  if (dir > 0) {
    stepPhaseIndex = (stepPhaseIndex + 1) % 8;
    currentStepPos++;
  } else if (dir < 0) {
    stepPhaseIndex = (stepPhaseIndex + 7) % 8;
    currentStepPos--;
  }
  applyStepPhase(stepPhaseIndex);
}

void returnToCenter() {
  while (currentStepPos != 0) {
    int dir = (currentStepPos > 0) ? -1 : 1;
    stepMotor(dir);
    delayMicroseconds(2000);
  }
  releaseMotor();
}

// ==============================================================================
// ATUALIZAÇÃO DO DISPLAY LCD
// Chamada APENAS sob demanda para não engasgar o motor durante o RUN
// ==============================================================================
void updateDisplay() {
  unsigned long delayUs = SPEED_DELAY_US[speedLevel];
  float cycleSeconds = (4.0f * (float)amplitudeSteps * (float)delayUs) / 1000000.0f;
  int spm = (cycleSeconds > 0) ? (int)(60.0f / cycleSeconds) : 0;

  int degX10 = (int)(amplitudeSteps * DEG_PER_STEP * 10.0f + 0.5f);
  int degInt = degX10 / 10;
  int degDec = degX10 % 10;

  char line0[17];
  char line1[17];

  switch (currentState) {
    case STATE_RUN:
      snprintf(line0, sizeof(line0), "[RUN] V:%-2d %3dSPM", speedLevel, spm);
      snprintf(line1, sizeof(line1), "A:+-%-3dp %2d.%1d%c", amplitudeSteps, degInt, degDec, 0xDF);
      break;

    case STATE_STOP:
      snprintf(line0, sizeof(line0), "[STOP] V:%-2d%3dSPM", speedLevel, spm);
      snprintf(line1, sizeof(line1), "A:+-%-3dp %2d.%1d%c", amplitudeSteps, degInt, degDec, 0xDF);
      break;

    case STATE_ALIGN:
      snprintf(line0, sizeof(line0), "[AJUSTE CENTRO] ");
      snprintf(line1, sizeof(line1), "Pos:%+4ld  UP/DWN", currentStepPos);
      break;
  }

  lcd.setCursor(0, 0);
  lcd.print(line0);
  for (int i = strlen(line0); i < 16; i++) lcd.print(' ');

  lcd.setCursor(0, 1);
  lcd.print(line1);
  for (int i = strlen(line1); i < 16; i++) lcd.print(' ');
}

// ==============================================================================
// TRATAMENTO DOS BOTÕES FÍSICOS
// ==============================================================================
void handleButtonAction(Button btn) {
  if (currentState == STATE_ALIGN) {
    int dir = (btn == BTN_UP) ? 1 : (btn == BTN_DOWN ? -1 : 0);
    if (dir != 0) {
      // Salto inicial ágil de 16 meio-passos (~1.4°)
      for (int i = 0; i < 16; i++) {
        stepMotor(dir);
        delayMicroseconds(1600);
      }
      updateDisplay();
    }
    return;
  }

  // Ajuste de Amplitude
  if (btn == BTN_UP) {
    if (amplitudeSteps < maxAmplitude) amplitudeSteps += 8;
    updateDisplay();
  } else if (btn == BTN_DOWN) {
    if (amplitudeSteps > minAmplitude) amplitudeSteps -= 8;
    updateDisplay();
  }

  // Ajuste de Velocidade
  if (btn == BTN_RIGHT) {
    if (speedLevel < MAX_SPEED) speedLevel++;
    updateDisplay();
  } else if (btn == BTN_LEFT) {
    if (speedLevel > MIN_SPEED) speedLevel--;
    updateDisplay();
  }
}

void processPhysicalButtons() {
  Button rawBtn = getFilteredButton();
  unsigned long now = millis();

  if (rawBtn != lastCandidateButton) {
    lastCandidateButton = rawBtn;
    candidateStartTime = now;
    return;
  }

  if (now - candidateStartTime < 70) {
    return;
  }

  if (rawBtn != stableButton) {
    Button previousStable = stableButton;
    stableButton = rawBtn;
    buttonPressStartTime = now;
    lastRepeatTime = now;
    selectLongPressHandled = false;

    if (stableButton == BTN_NONE) {
      if (previousStable == BTN_SELECT && !selectLongPressHandled) {
        if (currentState == STATE_RUN) {
          currentState = STATE_STOP;
          Serial.println(F("[COMANDO] Estado -> STOP"));
          returnToCenter();
        } else if (currentState == STATE_STOP) {
          currentState = STATE_RUN;
          motionDirection = 1;
          Serial.println(F("[COMANDO] Estado -> RUN"));
        }
        updateDisplay();
      }
      return;
    }

    handleButtonAction(stableButton);
    return;
  }

  if (stableButton != BTN_NONE) {
    if (stableButton == BTN_SELECT && !selectLongPressHandled) {
      if (now - buttonPressStartTime >= 1500) {
        selectLongPressHandled = true;
        if (currentState == STATE_STOP) {
          currentState = STATE_ALIGN;
          currentStepPos = 0;
          Serial.println(F("[COMANDO] Modo AJUSTE CENTRO ativado"));
        } else if (currentState == STATE_ALIGN) {
          currentStepPos = 0;
          currentState = STATE_STOP;
          releaseMotor();
          Serial.println(F("[COMANDO] Novo centro zero calibrado!"));
        }
        updateDisplay();
      }
      return;
    }

    if (stableButton != BTN_SELECT) {
      if (currentState == STATE_ALIGN) {
        if (now - buttonPressStartTime > 200) {
          int dir = (stableButton == BTN_UP) ? 1 : (stableButton == BTN_DOWN ? -1 : 0);
          if (dir != 0) {
            stepMotor(dir);
            delayMicroseconds(1600);
            static unsigned long lastAlignLcd = 0;
            if (now - lastAlignLcd > 100) {
              lastAlignLcd = now;
              updateDisplay();
            }
          }
        }
      } else {
        if (now - buttonPressStartTime > 400 && now - lastRepeatTime > 80) {
          lastRepeatTime = now;
          handleButtonAction(stableButton);
        }
      }
    }
  }
}

// ==============================================================================
// CONTROLE VIA SERIAL (USB)
// ==============================================================================
void processSerialCommands() {
  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\r' || c == '\n') continue;

    switch (c) {
      case ' ':
      case 'r':
      case 'R':
        if (currentState == STATE_RUN) {
          currentState = STATE_STOP;
          Serial.println(F("[SERIAL] STOP"));
          returnToCenter();
        } else {
          currentState = STATE_RUN;
          motionDirection = 1;
          Serial.println(F("[SERIAL] RUN"));
        }
        updateDisplay();
        break;

      case '+':
      case '=':
        if (amplitudeSteps < maxAmplitude) amplitudeSteps += 8;
        Serial.print(F("[SERIAL] Amplitude: ")); Serial.println(amplitudeSteps);
        updateDisplay();
        break;

      case '-':
      case '_':
        if (amplitudeSteps > minAmplitude) amplitudeSteps -= 8;
        Serial.print(F("[SERIAL] Amplitude: ")); Serial.println(amplitudeSteps);
        updateDisplay();
        break;

      case '1': case '2': case '3': case '4': case '5':
      case '6': case '7': case '8': case '9':
        speedLevel = c - '0';
        Serial.print(F("[SERIAL] Velocidade: ")); Serial.println(speedLevel);
        updateDisplay();
        break;

      case 'a':
      case 'A':
        if (currentState != STATE_ALIGN) {
          currentState = STATE_ALIGN;
          currentStepPos = 0;
          Serial.println(F("[SERIAL] Modo ALINHAMENTO. Teclas 'u' / 'd' movem, 'z' salva!"));
        } else {
          currentState = STATE_STOP;
          releaseMotor();
          Serial.println(F("[SERIAL] Saiu do alinhamento"));
        }
        updateDisplay();
        break;

      case 'u':
      case 'U':
        if (currentState == STATE_ALIGN) {
          for (int i = 0; i < 32; i++) { stepMotor(1); delayMicroseconds(1600); }
          Serial.print(F("[SERIAL] Posicao: ")); Serial.println(currentStepPos);
          updateDisplay();
        }
        break;

      case 'd':
      case 'D':
        if (currentState == STATE_ALIGN) {
          for (int i = 0; i < 32; i++) { stepMotor(-1); delayMicroseconds(1600); }
          Serial.print(F("[SERIAL] Posicao: ")); Serial.println(currentStepPos);
          updateDisplay();
        }
        break;

      case 'z':
      case 'Z':
        if (currentState == STATE_ALIGN) {
          currentStepPos = 0;
          currentState = STATE_STOP;
          releaseMotor();
          Serial.println(F("[SERIAL] CENTRO ZERO SALVO!"));
          updateDisplay();
        }
        break;

      case 'b':
      case 'B':
        physicalButtonsEnabled = !physicalButtonsEnabled;
        Serial.print(F("[SERIAL] Botoes fisicos: "));
        Serial.println(physicalButtonsEnabled ? F("ATIVADOS") : F("DESATIVADOS"));
        break;

      case '?':
      case 'h':
        Serial.println(F("\nCOMANDOS: [r] Run/Stop | [+] [-] Amplitude | [1-9] Velocidade | [a] Alinhar | [u]/[d] Mover | [z] Salvar Zero | [b] Ligar/Desligar Botoes"));
        break;
    }
  }
}

// ==============================================================================
// SETUP
// ==============================================================================
void setup() {
  Serial.begin(115200);
  Serial.println(F("\n=================================================="));
  Serial.println(F("   PALHETADOR AUTOMATICO DE GUITARRA COM ARDUINO  "));
  Serial.println(F("=================================================="));
  Serial.println(F("Sistema pronto em modo STOP."));
  Serial.println(F("Pressione SELECT ou envie 'r' para iniciar."));

  pinMode(PIN_IN1, OUTPUT);
  pinMode(PIN_IN2, OUTPUT);
  pinMode(PIN_IN3, OUTPUT);
  pinMode(PIN_IN4, OUTPUT);
  releaseMotor();

  pinMode(PIN_BACKLIGHT, OUTPUT);
  digitalWrite(PIN_BACKLIGHT, HIGH);

  lcd.begin(16, 2);
  lcd.print("Palhetador v3.1");
  lcd.setCursor(0, 1);
  lcd.print("Pronto (STOP)...");
  delay(1000);

  lcd.clear();
  updateDisplay();
}

// ==============================================================================
// LOOP PRINCIPAL (100% SUAVE E SIMÉTRICO)
// ==============================================================================
void loop() {
  // 1. Processa botões físicos com filtro pesado
  processPhysicalButtons();

  // 2. Processa comandos Serial (USB)
  processSerialCommands();

  // 3. Movimento de palhetada alternada contínua (Strumming Up/Down)
  if (currentState == STATE_RUN) {
    unsigned long nowMicros = micros();
    unsigned long stepInterval = SPEED_DELAY_US[speedLevel];

    if (nowMicros - lastStepMicros >= stepInterval) {
      lastStepMicros = nowMicros;

      // Executa o meio-passo
      stepMotor(motionDirection);

      // Inversão perfeitamente simétrica nos limites de amplitude
      if (currentStepPos >= amplitudeSteps) {
        motionDirection = -1;
      } else if (currentStepPos <= -amplitudeSteps) {
        motionDirection = 1;
      }
    }
  }
}
